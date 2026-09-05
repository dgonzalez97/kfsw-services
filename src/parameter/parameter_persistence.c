#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "parameter_internal.h"

#define KFSW_PARAM_PERSIST_DIRECTORY KFSW_STORAGE_MOUNT_POINT "/params"
#define KFSW_PARAM_PERSIST_PATH KFSW_PARAM_PERSIST_DIRECTORY "/parameters.dat"
#define KFSW_PARAM_PERSIST_TEMP_PATH KFSW_PARAM_PERSIST_DIRECTORY "/parameters.tmp"
#define KFSW_PARAM_PERSIST_MAGIC "KPAR"
#define KFSW_PARAM_PERSIST_MAGIC_SIZE 4U
#define KFSW_PARAM_PERSIST_VERSION 1U
#define KFSW_PARAM_PERSIST_HEADER_SIZE 20U
#define KFSW_PARAM_PERSIST_CRC_OFFSET 16U
#define KFSW_PARAM_PERSIST_ENTRY_HEADER_SIZE 4U
#define KFSW_PARAM_PERSIST_MAX_NAME_SIZE KFSW_PARAM_NAME_MAX
#define KFSW_PARAM_PERSIST_MAX_VALUE_SIZE KFSW_PARAM_STRING_MAX
#define KFSW_PARAM_PERSIST_MAX_ENTRY_COUNT 32U
#define KFSW_PARAM_PERSIST_MAX_SNAPSHOT_SIZE 2048U

enum persist_type {
	PERSIST_TYPE_U8 = 1,
	PERSIST_TYPE_U32 = 2,
	PERSIST_TYPE_I32 = 3,
	PERSIST_TYPE_FLOAT = 4,
	/* Added after the first snapshots were written. A reader that does not
	 * know a type code refuses the entry rather than guessing at its width,
	 * so an older reader meeting one fails safe instead of misdecoding the
	 * rest of the snapshot.
	 */
	PERSIST_TYPE_U16 = 5,
	PERSIST_TYPE_I16 = 6,
	/* Stored with its terminator, so value_size carries the whole thing and
	 * a shorter string does not have to be padded. */
	PERSIST_TYPE_STRING = 7,
};

_Static_assert(sizeof(float) == sizeof(uint32_t),
	       "parameter snapshots require a 32-bit IEEE-compatible float");

struct persist_entry {
	const uint8_t *name;
	const uint8_t *value;
	uint16_t value_size;
	uint8_t name_size;
	uint8_t type;
};

K_MUTEX_DEFINE(kfsw_param_persist_lock);

static uint8_t snapshot[KFSW_PARAM_PERSIST_MAX_SNAPSHOT_SIZE];

static size_t bounded_string_length(const char *text, size_t maximum)
{
	size_t length = 0U;

	while ((length < maximum) && (text[length] != '\0')) {
		length++;
	}
	return length;
}

static int persistent_type(const struct kfsw_param_entry *entry, uint8_t *type,
			   uint16_t *value_size)
{
	switch (entry->info.type) {
	case KFSW_PARAM_U8:
		*type = PERSIST_TYPE_U8;
		*value_size = sizeof(uint8_t);
		return 0;
	case KFSW_PARAM_U16:
		*type = PERSIST_TYPE_U16;
		*value_size = sizeof(uint16_t);
		return 0;
	case KFSW_PARAM_I16:
		*type = PERSIST_TYPE_I16;
		*value_size = sizeof(int16_t);
		return 0;
	case KFSW_PARAM_U32:
		*type = PERSIST_TYPE_U32;
		*value_size = sizeof(uint32_t);
		return 0;
	case KFSW_PARAM_I32:
		*type = PERSIST_TYPE_I32;
		*value_size = sizeof(int32_t);
		return 0;
	case KFSW_PARAM_FLOAT:
		*type = PERSIST_TYPE_FLOAT;
		*value_size = sizeof(float);
		return 0;
	case KFSW_PARAM_STRING:
		*type = PERSIST_TYPE_STRING;
		/* The stored length is the current value's, not the capacity: a
		 * snapshot should not grow with storage a parameter is not
		 * using. Filled in by the encoder, which has read the value. */
		*value_size = 0U;
		return 0;
	default:
		return -ENOTSUP;
	}
}

/* Reports how many bytes it wrote, because a string's length is its own and is
 * only known once the value has been read.
 */
static int encode_value(const struct kfsw_param_entry *entry, uint8_t *output, size_t output_size,
			uint16_t *written)
{
	struct kfsw_param_value value = {0};
	uint16_t value_size;
	uint8_t type;
	uint32_t raw_value;
	int result;

	result = persistent_type(entry, &type, &value_size);
	if (result != 0) {
		return result;
	}

	result = kfsw_param_read_entry(entry, &value);
	if (result != 0) {
		return result;
	}
	if (type == PERSIST_TYPE_STRING) {
		value_size = (uint16_t)value.size;
	}
	if (output_size < value_size) {
		return -ENOSPC;
	}
	switch (type) {
	case PERSIST_TYPE_U8:
		output[0] = value.scalar.u8;
		break;
	case PERSIST_TYPE_U16:
		sys_put_be16(value.scalar.u16, output);
		break;
	case PERSIST_TYPE_I16:
		sys_put_be16((uint16_t)value.scalar.i16, output);
		break;
	case PERSIST_TYPE_U32:
		sys_put_be32(value.scalar.u32, output);
		break;
	case PERSIST_TYPE_I32:
		sys_put_be32((uint32_t)value.scalar.i32, output);
		break;
	case PERSIST_TYPE_FLOAT:
		memcpy(&raw_value, &value.scalar.f32, sizeof(raw_value));
		sys_put_be32(raw_value, output);
		break;
	case PERSIST_TYPE_STRING:
		memcpy(output, value.text, value_size);
		break;
	default:
		return -ENOTSUP;
	}

	*written = value_size;
	return 0;
}

static int build_snapshot(size_t *snapshot_size)
{
	size_t offset = KFSW_PARAM_PERSIST_HEADER_SIZE;
	uint16_t entry_count = 0U;
	int result = 0;

	memset(snapshot, 0, sizeof(snapshot));
	kfsw_param_table_lock();
	for (size_t index = 0U; index < kfsw_param_entry_count(); index++) {
		const struct kfsw_param_entry *entry = kfsw_param_entry_at(index);
		size_t name_size;
		size_t header_offset;
		uint16_t value_size;
		uint8_t type;

		/* Read-only means an operator cannot write it, not that it
		 * cannot be kept. A boot counter is exactly that: the service
		 * owns the value and nobody should be able to rewrite the
		 * history of how many times the node has restarted, but losing
		 * the count on every restart would defeat the point of it.
		 */
		if ((entry->info.node != 0U) ||
		    ((entry->info.flags & KFSW_PARAM_FLAG_PERSISTENT) == 0U)) {
			continue;
		}

		name_size = bounded_string_length(entry->info.name,
						  KFSW_PARAM_PERSIST_MAX_NAME_SIZE + 1U);
		if ((name_size == 0U) || (name_size > KFSW_PARAM_PERSIST_MAX_NAME_SIZE)) {
			result = -ENAMETOOLONG;
			break;
		}
		result = persistent_type(entry, &type, &value_size);
		if (result != 0) {
			break;
		}
		/* Checked against the capacity rather than the current length,
		 * because a later save of a longer string must not be the one
		 * that discovers there was never room for it. */
		if (entry->info.type == KFSW_PARAM_STRING) {
			value_size = entry->info.array_size;
		}
		if ((entry_count >= KFSW_PARAM_PERSIST_MAX_ENTRY_COUNT) ||
		    (offset + KFSW_PARAM_PERSIST_ENTRY_HEADER_SIZE + name_size + value_size >
		     sizeof(snapshot))) {
			result = -E2BIG;
			break;
		}

		snapshot[offset] = (uint8_t)name_size;
		snapshot[offset + 1U] = type;
		header_offset = offset;
		offset += KFSW_PARAM_PERSIST_ENTRY_HEADER_SIZE;
		memcpy(&snapshot[offset], entry->info.name, name_size);
		offset += name_size;
		result = encode_value(entry, &snapshot[offset], sizeof(snapshot) - offset,
				      &value_size);
		if (result != 0) {
			break;
		}
		/* Written once the value is encoded: only then is a string's
		 * length known. */
		sys_put_be16(value_size, &snapshot[header_offset + 2U]);
		offset += value_size;
		entry_count++;
	}
	kfsw_param_table_unlock();
	if (result != 0) {
		return result;
	}

	memcpy(snapshot, KFSW_PARAM_PERSIST_MAGIC, KFSW_PARAM_PERSIST_MAGIC_SIZE);
	sys_put_be16(KFSW_PARAM_PERSIST_VERSION, &snapshot[4]);
	sys_put_be16(KFSW_PARAM_PERSIST_HEADER_SIZE, &snapshot[6]);
	sys_put_be32((uint32_t)(offset - KFSW_PARAM_PERSIST_HEADER_SIZE), &snapshot[8]);
	sys_put_be16(entry_count, &snapshot[12]);
	sys_put_be16(0U, &snapshot[14]);
	sys_put_be32(0U, &snapshot[KFSW_PARAM_PERSIST_CRC_OFFSET]);
	sys_put_be32(crc32_ieee(snapshot, offset), &snapshot[KFSW_PARAM_PERSIST_CRC_OFFSET]);
	*snapshot_size = offset;
	return 0;
}

static int write_all(struct fs_file_t *file, const uint8_t *data, size_t size)
{
	size_t offset = 0U;

	while (offset < size) {
		ssize_t written = fs_write(file, &data[offset], size - offset);

		if (written < 0) {
			return (int)written;
		}
		if (written == 0) {
			return -EIO;
		}
		offset += (size_t)written;
	}
	return 0;
}

static int read_all(struct fs_file_t *file, uint8_t *data, size_t size)
{
	size_t offset = 0U;

	while (offset < size) {
		ssize_t bytes_read = fs_read(file, &data[offset], size - offset);

		if (bytes_read < 0) {
			return (int)bytes_read;
		}
		if (bytes_read == 0) {
			return -EMSGSIZE;
		}
		offset += (size_t)bytes_read;
	}
	return 0;
}

static int next_entry(const uint8_t *data, size_t size, size_t *offset, struct persist_entry *entry)
{
	size_t entry_size;

	if (*offset + KFSW_PARAM_PERSIST_ENTRY_HEADER_SIZE > size) {
		return -EBADMSG;
	}

	entry->name_size = data[*offset];
	entry->type = data[*offset + 1U];
	entry->value_size = sys_get_be16(&data[*offset + 2U]);
	*offset += KFSW_PARAM_PERSIST_ENTRY_HEADER_SIZE;
	entry_size = (size_t)entry->name_size + entry->value_size;
	if ((entry->name_size == 0U) || (entry->name_size > KFSW_PARAM_PERSIST_MAX_NAME_SIZE) ||
	    (entry->value_size == 0U) || (entry->value_size > KFSW_PARAM_PERSIST_MAX_VALUE_SIZE) ||
	    (*offset + entry_size > size)) {
		return -EBADMSG;
	}

	entry->name = &data[*offset];
	entry->value = &data[*offset + entry->name_size];
	*offset += entry_size;
	return 0;
}

static int validate_snapshot(size_t size, uint16_t *entry_count)
{
	uint32_t expected_crc;
	uint32_t actual_crc;
	uint32_t payload_size;
	size_t offset = KFSW_PARAM_PERSIST_HEADER_SIZE;

	if (size < KFSW_PARAM_PERSIST_HEADER_SIZE) {
		return -EMSGSIZE;
	}
	if (memcmp(snapshot, KFSW_PARAM_PERSIST_MAGIC, KFSW_PARAM_PERSIST_MAGIC_SIZE) != 0) {
		return -EBADMSG;
	}
	if (sys_get_be16(&snapshot[4]) != KFSW_PARAM_PERSIST_VERSION) {
		return -EPROTONOSUPPORT;
	}
	if ((sys_get_be16(&snapshot[6]) != KFSW_PARAM_PERSIST_HEADER_SIZE) ||
	    (sys_get_be16(&snapshot[14]) != 0U)) {
		return -EBADMSG;
	}

	payload_size = sys_get_be32(&snapshot[8]);
	*entry_count = sys_get_be16(&snapshot[12]);
	if ((payload_size != size - KFSW_PARAM_PERSIST_HEADER_SIZE) ||
	    (*entry_count > KFSW_PARAM_PERSIST_MAX_ENTRY_COUNT)) {
		return -EMSGSIZE;
	}

	expected_crc = sys_get_be32(&snapshot[KFSW_PARAM_PERSIST_CRC_OFFSET]);
	sys_put_be32(0U, &snapshot[KFSW_PARAM_PERSIST_CRC_OFFSET]);
	actual_crc = crc32_ieee(snapshot, size);
	if (actual_crc != expected_crc) {
		return -EBADMSG;
	}

	for (uint16_t index = 0U; index < *entry_count; index++) {
		struct persist_entry entry;
		int result = next_entry(snapshot, size, &offset, &entry);

		if (result != 0) {
			return result;
		}
	}
	return (offset == size) ? 0 : -EBADMSG;
}

static bool entry_matches(const struct kfsw_param_entry *param_entry,
			  const struct persist_entry *persist_entry)
{
	uint16_t value_size;
	uint8_t type;

	return (persistent_type(param_entry, &type, &value_size) == 0) &&
	       (type == persist_entry->type) && (value_size == persist_entry->value_size);
}

static int decode_and_set(const struct kfsw_param_entry *param_entry,
			  const struct persist_entry *persist_entry)
{
	struct kfsw_param_value value = {
		.type = param_entry->info.type,
		.size = persist_entry->value_size,
	};
	uint32_t raw_value;
	int result;

	switch (persist_entry->type) {
	case PERSIST_TYPE_U8:
		value.scalar.u8 = persist_entry->value[0];
		break;
	case PERSIST_TYPE_U16:
		value.scalar.u16 = sys_get_be16(persist_entry->value);
		break;
	case PERSIST_TYPE_I16:
		value.scalar.i16 = (int16_t)sys_get_be16(persist_entry->value);
		break;
	case PERSIST_TYPE_U32:
		value.scalar.u32 = sys_get_be32(persist_entry->value);
		break;
	case PERSIST_TYPE_I32:
		value.scalar.i32 = (int32_t)sys_get_be32(persist_entry->value);
		break;
	case PERSIST_TYPE_FLOAT:
		raw_value = sys_get_be32(persist_entry->value);
		memcpy(&value.scalar.f32, &raw_value, sizeof(value.scalar.f32));
		break;
	case PERSIST_TYPE_STRING:
		/* A stored string that lost its terminator is a corrupt entry,
		 * not one to repair by guessing where it ended. */
		if ((persist_entry->value_size == 0U) ||
		    (persist_entry->value_size > sizeof(value.text)) ||
		    (persist_entry->value[persist_entry->value_size - 1U] != '\0')) {
			return -EBADMSG;
		}
		memcpy(value.text, persist_entry->value, persist_entry->value_size);
		value.size = persist_entry->value_size;
		break;
	default:
		return -ENOTSUP;
	}
	result = kfsw_param_validate_entry(param_entry, &value);
	if (result == 0) {
		if (value.type == KFSW_PARAM_STRING) {
			kfsw_param_write_text_entry(param_entry, value.text);
		} else {
			kfsw_param_write_entry(param_entry, &value.scalar);
		}
	}
	return result;
}

static int apply_snapshot(size_t size, uint16_t entry_count)
{
	size_t offset = KFSW_PARAM_PERSIST_HEADER_SIZE;

	kfsw_param_table_lock();
	for (uint16_t index = 0U; index < entry_count; index++) {
		struct persist_entry entry;
		const struct kfsw_param_entry *param_entry;
		char name[KFSW_PARAM_PERSIST_MAX_NAME_SIZE + 1U];
		int result = next_entry(snapshot, size, &offset, &entry);

		if (result != 0) {
			kfsw_param_table_unlock();
			return result;
		}
		memcpy(name, entry.name, entry.name_size);
		name[entry.name_size] = '\0';
		param_entry = kfsw_param_find_name(name);
		if ((param_entry == NULL) ||
		    ((param_entry->info.flags & KFSW_PARAM_FLAG_PERSISTENT) == 0U)) {
			kfsw_log_warning("Ignoring unknown persistent parameter '%s'", name);
			continue;
		}
		if (!entry_matches(param_entry, &entry)) {
			kfsw_log_warning("Ignoring incompatible persistent parameter '%s'", name);
			continue;
		}
		if (decode_and_set(param_entry, &entry) != 0) {
			kfsw_log_warning("Ignoring invalid persistent parameter '%s'", name);
		}
	}
	kfsw_param_table_unlock();
	return 0;
}

static int persist_save(void)
{
	struct fs_file_t file;
	size_t snapshot_size;
	int close_result;
	int result;

	if (!kfsw_param_is_initialized() || !kfsw_storage_is_ready()) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_persist_lock, K_FOREVER);
	result = build_snapshot(&snapshot_size);
	if (result != 0) {
		goto out;
	}

	result = fs_mkdir(KFSW_PARAM_PERSIST_DIRECTORY);
	if ((result != 0) && (result != -EEXIST)) {
		goto out;
	}
	(void)fs_unlink(KFSW_PARAM_PERSIST_TEMP_PATH);
	fs_file_t_init(&file);
	result =
		fs_open(&file, KFSW_PARAM_PERSIST_TEMP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result != 0) {
		goto out;
	}

	result = write_all(&file, snapshot, snapshot_size);
	if (result == 0) {
		result = fs_sync(&file);
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		result = fs_rename(KFSW_PARAM_PERSIST_TEMP_PATH, KFSW_PARAM_PERSIST_PATH);
	}
	if (result != 0) {
		(void)fs_unlink(KFSW_PARAM_PERSIST_TEMP_PATH);
	}

out:
	k_mutex_unlock(&kfsw_param_persist_lock);
	return result;
}

static int persist_load(void)
{
	struct fs_dirent entry;
	struct fs_file_t file;
	uint16_t entry_count;
	int close_result;
	int result;

	if (!kfsw_param_is_initialized() || !kfsw_storage_is_ready()) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_persist_lock, K_FOREVER);
	(void)fs_unlink(KFSW_PARAM_PERSIST_TEMP_PATH);
	result = fs_stat(KFSW_PARAM_PERSIST_PATH, &entry);
	if (result != 0) {
		goto out;
	}
	if ((entry.type != FS_DIR_ENTRY_FILE) || (entry.size < KFSW_PARAM_PERSIST_HEADER_SIZE) ||
	    (entry.size > sizeof(snapshot))) {
		result = -EMSGSIZE;
		goto out;
	}

	fs_file_t_init(&file);
	result = fs_open(&file, KFSW_PARAM_PERSIST_PATH, FS_O_READ);
	if (result != 0) {
		goto out;
	}
	result = read_all(&file, snapshot, entry.size);
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		result = validate_snapshot(entry.size, &entry_count);
	}
	if (result == 0) {
		result = apply_snapshot(entry.size, entry_count);
	}

out:
	k_mutex_unlock(&kfsw_param_persist_lock);
	return result;
}

int kfsw_param_persist_clear(void)
{
	int active_result;
	int temp_result;

	if (!kfsw_param_is_initialized() || !kfsw_storage_is_ready()) {
		return -EACCES;
	}

	k_mutex_lock(&kfsw_param_persist_lock, K_FOREVER);
	active_result = fs_unlink(KFSW_PARAM_PERSIST_PATH);
	temp_result = fs_unlink(KFSW_PARAM_PERSIST_TEMP_PATH);
	k_mutex_unlock(&kfsw_param_persist_lock);

	if ((active_result != 0) && (active_result != -ENOENT)) {
		return active_result;
	}
	return ((temp_result == 0) || (temp_result == -ENOENT)) ? 0 : temp_result;
}

/*
 * Counted at one exit each rather than at every return inside. A snapshot that
 * fails to load is the difference between running on stored settings and
 * running on compiled defaults, and nothing else records which happened.
 */
int kfsw_param_persist_save(void)
{
	int result = persist_save();

	if (result == 0) {
		kfsw_param_count_save();
	}
	return result;
}

int kfsw_param_persist_load(void)
{
	int result = persist_load();

	/* A missing snapshot is the ordinary first boot, not a failure. */
	if ((result != 0) && (result != -ENOENT)) {
		kfsw_param_count_load_failure();
	}
	return result;
}
