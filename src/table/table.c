#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>
#include <kfsw/services/table.h>

#include "../parameter/param_wire.h"
#include "../parameter/parameter_internal.h"
#include "../snapshot_file.h"

/*
 * File layout, the same shape as the parameter snapshot so one decoder reads
 * both. All fields are big-endian.
 *
 *   0  magic "KTBL"
 *   4  format version
 *   6  header size
 *   8  payload bytes after the header
 *  12  entry count
 *  14  table identifier, then one reserved byte
 *  16  CRC32 over the whole file with these four bytes zero
 *
 * Each entry is an offset within the table, a wire type code, the value size,
 * and the value.
 */
#define KFSW_TABLE_MAGIC "KTBL"
#define KFSW_TABLE_MAGIC_SIZE 4U
#define KFSW_TABLE_VERSION 1U
#define KFSW_TABLE_HEADER_SIZE 20U
#define KFSW_TABLE_CRC_OFFSET 16U
#define KFSW_TABLE_ENTRY_HEADER_SIZE 4U

#define KFSW_TABLE_MAX_BYTES                                                                       \
	(KFSW_TABLE_HEADER_SIZE + (KFSW_TABLE_MAX_ENTRIES * (KFSW_TABLE_ENTRY_HEADER_SIZE +        \
							     KFSW_PARAM_WIRE_MAX_VALUE_SIZE)))

#define KFSW_TABLE_TEMPORARY_SUFFIX ".part"

/** One entry as it sits in the file. */
struct file_entry {
	const uint8_t *value;
	uint16_t value_size;
	uint8_t offset;
	uint8_t type;
};

/** One value a load replaced, kept so the load can be undone. */
struct saved_value {
	uint16_t id;
	struct kfsw_param_value value;
};

static K_MUTEX_DEFINE(table_lock);
static uint8_t image[KFSW_TABLE_MAX_BYTES];
static struct saved_value saved[KFSW_TABLE_MAX_ENTRIES];
static uint16_t saved_count;
/* Filled by a load's check pass and applied by the pass after it, so nothing is
 * written until every entry has been accepted. */
static struct kfsw_param_value decoded[KFSW_TABLE_MAX_ENTRIES];
static struct kfsw_table_status status = {
	.state = KFSW_TABLE_EMPTY,
};

static void count_up(uint32_t *counter)
{
	if (*counter < UINT32_MAX) {
		(*counter)++;
	}
}

static void report_event(enum kfsw_table_event id, uint8_t table, uint16_t entries)
{
#if CONFIG_KFSW_EVENT
	uint8_t payload[3];

	payload[0] = table;
	sys_put_be16(entries, &payload[1]);
	kfsw_event_emit(KFSW_EVENT_SOURCE_TABLE, (uint16_t)id,
			(id == KFSW_EVENT_TABLE_LOADED) ? KFSW_EVENT_INFO : KFSW_EVENT_WARNING,
			payload, sizeof(payload));
#else
	ARG_UNUSED(id);
	ARG_UNUSED(table);
	ARG_UNUSED(entries);
#endif
}

/* fs_read is allowed to return less than it was asked for. */
static int read_all(struct fs_file_t *file, uint8_t *data, size_t size)
{
	size_t offset = 0U;

	while (offset < size) {
		ssize_t read = fs_read(file, &data[offset], size - offset);

		if (read < 0) {
			return (int)read;
		}
		if (read == 0) {
			return -EBADMSG;
		}
		offset += (size_t)read;
	}
	return 0;
}

static int read_image(const char *path, size_t *size)
{
	struct fs_dirent entry;
	struct fs_file_t file;
	int result;
	int close_result;

	result = fs_stat(path, &entry);
	if (result != 0) {
		return result;
	}
	if (entry.type != FS_DIR_ENTRY_FILE) {
		return -EBADMSG;
	}
	if (entry.size < KFSW_TABLE_HEADER_SIZE) {
		return -EBADMSG;
	}
	if (entry.size > (size_t)sizeof(image)) {
		return -EFBIG;
	}

	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result != 0) {
		return result;
	}
	result = read_all(&file, image, (size_t)entry.size);
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		*size = (size_t)entry.size;
	}
	return result;
}

static int validate_header(size_t size, uint16_t *entry_count, uint8_t *table)
{
	uint32_t expected_crc;
	uint32_t actual_crc;
	uint32_t payload_size;

	if (memcmp(image, KFSW_TABLE_MAGIC, KFSW_TABLE_MAGIC_SIZE) != 0) {
		return -EBADMSG;
	}
	if (sys_get_be16(&image[4]) != KFSW_TABLE_VERSION) {
		return -ENOTSUP;
	}
	if (sys_get_be16(&image[6]) != KFSW_TABLE_HEADER_SIZE) {
		return -EBADMSG;
	}
	payload_size = sys_get_be32(&image[8]);
	if (payload_size != (uint32_t)(size - KFSW_TABLE_HEADER_SIZE)) {
		return -EBADMSG;
	}
	*entry_count = sys_get_be16(&image[12]);
	if ((*entry_count == 0U) || (*entry_count > KFSW_TABLE_MAX_ENTRIES)) {
		return -EBADMSG;
	}
	*table = image[14];
	if ((*table < KFSW_PARAM_TABLE_CORE_FIRST) || (*table > KFSW_PARAM_TABLE_MODULE_LAST)) {
		return -EBADMSG;
	}

	expected_crc = sys_get_be32(&image[KFSW_TABLE_CRC_OFFSET]);
	sys_put_be32(0U, &image[KFSW_TABLE_CRC_OFFSET]);
	actual_crc = crc32_ieee(image, size);
	sys_put_be32(expected_crc, &image[KFSW_TABLE_CRC_OFFSET]);
	if (actual_crc != expected_crc) {
		return -EBADMSG;
	}
	return 0;
}

static int next_entry(size_t size, size_t *offset, struct file_entry *entry)
{
	if ((*offset + KFSW_TABLE_ENTRY_HEADER_SIZE) > size) {
		return -EBADMSG;
	}
	entry->offset = image[*offset];
	entry->type = image[*offset + 1U];
	entry->value_size = sys_get_be16(&image[*offset + 2U]);
	*offset += KFSW_TABLE_ENTRY_HEADER_SIZE;

	if ((entry->value_size == 0U) || (entry->value_size > KFSW_PARAM_WIRE_MAX_VALUE_SIZE) ||
	    ((*offset + entry->value_size) > size)) {
		return -EBADMSG;
	}
	entry->value = &image[*offset];
	*offset += entry->value_size;
	return 0;
}

/*
 * Walks the whole file. A load keeps each accepted value for the pass that
 * applies them; validating alone keeps nothing.
 */
static int check_entries(size_t size, uint16_t entry_count, uint8_t table,
			 const struct kfsw_param_entry *entries[], bool keep_decoded,
			 struct kfsw_table_report *report)
{
	size_t offset = KFSW_TABLE_HEADER_SIZE;
	uint8_t seen[KFSW_TABLE_MAX_ENTRIES];
	uint16_t seen_count = 0U;

	for (uint16_t index = 0U; index < entry_count; index++) {
		const struct kfsw_param_entry *param_entry;
		struct kfsw_param_value value;
		struct file_entry entry;
		int result = next_entry(size, &offset, &entry);

		if (result != 0) {
			report->failed_entry = index + 1U;
			report->reason = result;
			return -EBADMSG;
		}

		report->failed_entry = index + 1U;
		report->failed_offset = entry.offset;

		/* The same offset twice would make the file's effect depend on
		 * the order it is applied in. */
		for (uint16_t seen_index = 0U; seen_index < seen_count; seen_index++) {
			if (seen[seen_index] == entry.offset) {
				report->reason = -EEXIST;
				return -EBADMSG;
			}
		}
		seen[seen_count] = entry.offset;
		seen_count++;

		param_entry = kfsw_param_find_id(KFSW_PARAM_ID(table, entry.offset));
		if (param_entry == NULL) {
			report->reason = -ENXIO;
			return -EBADMSG;
		}
		if (param_entry->info.read_only) {
			report->reason = -EROFS;
			return -EBADMSG;
		}

		/* The parameter's own range check runs here, before anything is
		 * written, which is the whole point of validating a file. */
		result = kfsw_param_wire_decode(param_entry, entry.type, entry.value,
						entry.value_size, &value);
		if (result != 0) {
			report->reason = result;
			return -EBADMSG;
		}

		if (entries != NULL) {
			entries[index] = param_entry;
		}
		if (keep_decoded) {
			decoded[index] = value;
		}
		report->accepted++;
	}

	if (offset != size) {
		report->reason = -EBADMSG;
		return -EBADMSG;
	}

	report->failed_entry = 0U;
	report->failed_offset = 0U;
	report->reason = 0;
	return 0;
}

static int prepare(const char *path, size_t *size, uint16_t *entry_count, uint8_t *table)
{
	int result;

	if ((path == NULL) || (path[0] == '\0') || (strlen(path) >= (size_t)KFSW_TABLE_PATH_SIZE)) {
		return -EINVAL;
	}
	if (!kfsw_param_is_initialized()) {
		return -EACCES;
	}
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}

	result = read_image(path, size);
	if (result != 0) {
		return result;
	}
	return validate_header(*size, entry_count, table);
}

static int examine(const char *path, struct kfsw_table_report *report,
		   const struct kfsw_param_entry *entries[], bool keep_decoded)
{
	uint16_t entry_count = 0U;
	uint8_t table = 0U;
	size_t size = 0U;
	int result;

	*report = (struct kfsw_table_report){0};

	result = prepare(path, &size, &entry_count, &table);
	if (result != 0) {
		report->reason = result;
		return result;
	}
	report->table = table;
	report->entries = entry_count;

	return check_entries(size, entry_count, table, entries, keep_decoded, report);
}

int kfsw_table_validate(const char *path, struct kfsw_table_report *report)
{
	struct kfsw_table_report local;
	int result;

	k_mutex_lock(&table_lock, K_FOREVER);
	result = examine(path, &local, NULL, false);
	k_mutex_unlock(&table_lock);

	if (report != NULL) {
		*report = local;
	}
	return result;
}

int kfsw_table_load(const char *path, struct kfsw_table_report *report)
{
	const struct kfsw_param_entry *entries[KFSW_TABLE_MAX_ENTRIES];
	struct kfsw_table_report local;
	int result;

	k_mutex_lock(&table_lock, K_FOREVER);
	result = examine(path, &local, entries, true);
	if (result != 0) {
		count_up(&status.rejections);
		k_mutex_unlock(&table_lock);
		if (local.failed_entry == 0U) {
			kfsw_log_warning("Table file '%s' refused (%d)", path, local.reason);
		} else {
			kfsw_log_warning("Table file '%s' refused at entry %u (%d)", path,
					 local.failed_entry, local.reason);
		}
		report_event(KFSW_EVENT_TABLE_REJECTED, local.table, local.entries);
		if (report != NULL) {
			*report = local;
		}
		return result;
	}

	/* Every entry passed, so the whole file goes in under one lock and the
	 * values it replaces are kept in the same pass. */
	kfsw_param_table_lock();
	saved_count = 0U;
	for (uint16_t index = 0U; index < local.entries; index++) {
		struct kfsw_param_value previous;

		if (kfsw_param_read_stored_entry(entries[index], &previous) == 0) {
			saved[saved_count].id = entries[index]->info.id;
			saved[saved_count].value = previous;
			saved_count++;
		}
		kfsw_param_wire_apply(entries[index], &decoded[index]);
	}
	kfsw_param_table_unlock();

	for (uint16_t index = 0U; index < local.entries; index++) {
		kfsw_param_value_changed(entries[index]->info.id);
	}

	status.state = KFSW_TABLE_LOADED;
	status.table = local.table;
	status.entries = local.entries;
	count_up(&status.loads);
	(void)strncpy(status.path, path, sizeof(status.path) - 1U);
	status.path[sizeof(status.path) - 1U] = '\0';
	k_mutex_unlock(&table_lock);

	kfsw_log_info("Table %u loaded from '%s', %u entries", local.table, path, local.entries);
	report_event(KFSW_EVENT_TABLE_LOADED, local.table, local.entries);

	if (report != NULL) {
		*report = local;
	}
	return 0;
}

int kfsw_table_revert(void)
{
	uint16_t restored = 0U;
	uint8_t table;

	if (!kfsw_param_is_initialized()) {
		return -EACCES;
	}

	k_mutex_lock(&table_lock, K_FOREVER);
	if ((status.state != KFSW_TABLE_LOADED) || (saved_count == 0U)) {
		k_mutex_unlock(&table_lock);
		return -ENOENT;
	}
	table = status.table;

	kfsw_param_table_lock();
	for (uint16_t index = 0U; index < saved_count; index++) {
		const struct kfsw_param_entry *entry = kfsw_param_find_id(saved[index].id);

		if (entry != NULL) {
			kfsw_param_wire_apply(entry, &saved[index].value);
			restored++;
		}
	}
	kfsw_param_table_unlock();

	for (uint16_t index = 0U; index < saved_count; index++) {
		kfsw_param_value_changed(saved[index].id);
	}

	saved_count = 0U;
	status.state = KFSW_TABLE_REVERTED;
	count_up(&status.reverts);
	k_mutex_unlock(&table_lock);

	kfsw_log_info("Table %u reverted, %u values put back", table, restored);
	report_event(KFSW_EVENT_TABLE_REVERTED, table, restored);
	return 0;
}

int kfsw_table_dump(uint8_t table, const char *path, uint16_t *entries)
{
	char temporary_path[KFSW_TABLE_PATH_SIZE + sizeof(KFSW_TABLE_TEMPORARY_SUFFIX)];
	size_t offset = KFSW_TABLE_HEADER_SIZE;
	uint16_t written_entries = 0U;
	int result = 0;

	if ((path == NULL) || (path[0] == '\0') || (strlen(path) >= (size_t)KFSW_TABLE_PATH_SIZE)) {
		return -EINVAL;
	}
	if ((table < KFSW_PARAM_TABLE_CORE_FIRST) || (table > KFSW_PARAM_TABLE_MODULE_LAST)) {
		return -EINVAL;
	}
	if (!kfsw_param_is_initialized()) {
		return -EACCES;
	}
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}

	k_mutex_lock(&table_lock, K_FOREVER);
	kfsw_param_table_lock();
	for (size_t index = 0U; index < kfsw_param_entry_count(); index++) {
		const struct kfsw_param_entry *entry = kfsw_param_entry_at(index);
		uint16_t value_size;
		uint16_t value_bytes;
		uint8_t type;

		if ((entry->info.table != table) || entry->info.read_only) {
			continue;
		}
		if (kfsw_param_wire_type_of(entry, &type, &value_size) != 0) {
			/* A type no file carries is left out rather than
			 * making the whole table undumpable. */
			continue;
		}
		if (written_entries == KFSW_TABLE_MAX_ENTRIES) {
			result = -E2BIG;
			break;
		}

		if (kfsw_param_wire_encode(entry, &image[offset + KFSW_TABLE_ENTRY_HEADER_SIZE],
					   sizeof(image) - offset - KFSW_TABLE_ENTRY_HEADER_SIZE,
					   &value_bytes) != 0) {
			result = -E2BIG;
			break;
		}
		image[offset] = entry->info.offset;
		image[offset + 1U] = type;
		sys_put_be16(value_bytes, &image[offset + 2U]);
		offset += KFSW_TABLE_ENTRY_HEADER_SIZE + value_bytes;
		written_entries++;
	}
	kfsw_param_table_unlock();

	if ((result == 0) && (written_entries == 0U)) {
		result = -ENOENT;
	}
	if (result != 0) {
		k_mutex_unlock(&table_lock);
		return result;
	}

	memcpy(image, KFSW_TABLE_MAGIC, KFSW_TABLE_MAGIC_SIZE);
	sys_put_be16(KFSW_TABLE_VERSION, &image[4]);
	sys_put_be16(KFSW_TABLE_HEADER_SIZE, &image[6]);
	sys_put_be32((uint32_t)(offset - KFSW_TABLE_HEADER_SIZE), &image[8]);
	sys_put_be16(written_entries, &image[12]);
	image[14] = table;
	image[15] = 0U;
	sys_put_be32(0U, &image[KFSW_TABLE_CRC_OFFSET]);
	sys_put_be32(crc32_ieee(image, offset), &image[KFSW_TABLE_CRC_OFFSET]);

	result = fs_mkdir(KFSW_TABLE_DIRECTORY);
	if ((result != 0) && (result != -EEXIST)) {
		k_mutex_unlock(&table_lock);
		return result;
	}

	(void)snprintf(temporary_path, sizeof(temporary_path), "%s%s", path,
		       KFSW_TABLE_TEMPORARY_SUFFIX);
	result = kfsw_snapshot_write(path, temporary_path, image, offset);
	k_mutex_unlock(&table_lock);

	if ((result == 0) && (entries != NULL)) {
		*entries = written_entries;
	}
	return result;
}

int kfsw_table_get_status(struct kfsw_table_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&table_lock, K_FOREVER);
	*out = status;
	k_mutex_unlock(&table_lock);
	return 0;
}

const char *kfsw_table_state_name(enum kfsw_table_state state)
{
	switch (state) {
	case KFSW_TABLE_EMPTY:
		return "empty";
	case KFSW_TABLE_LOADED:
		return "loaded";
	case KFSW_TABLE_REVERTED:
		return "reverted";
	default:
		return "unknown";
	}
}
