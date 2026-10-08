#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#include <kfsw/platform/storage.h>

#include "journal_store.h"

static int close_file(struct fs_file_t *file, int result)
{
	int closed = fs_close(file);

	return result ? result : closed;
}

static int write_all(struct fs_file_t *file, const uint8_t *bytes, size_t size)
{
	while (size != 0U) {
		ssize_t written = fs_write(file, bytes, size);

		if (written <= 0) {
			return written < 0 ? (int)written : -EIO;
		}
		bytes += written;
		size -= (size_t)written;
	}
	return 0;
}

static void encode(const struct kfsw_journal_record *record, uint8_t *bytes)
{
	memset(bytes, 0, KFSW_JOURNAL_RECORD_SIZE);
	/* codechecker_intentional [bugprone-not-null-terminated-result] a binary magic */
	memcpy(bytes, "KFE1", 4);
	bytes[4] = 1;
	bytes[5] = record->utc_valid;
	sys_put_be16(KFSW_JOURNAL_RECORD_SIZE, bytes + 6);
	sys_put_be64(record->sequence, bytes + 8);
	sys_put_be64(record->boot, bytes + 16);
	sys_put_be64(record->event.monotonic_us, bytes + 24);
	sys_put_be64((uint64_t)record->utc_seconds, bytes + 32);
	sys_put_be32(record->event.sequence, bytes + 40);
	sys_put_be16(record->event.source, bytes + 44);
	sys_put_be16(record->event.id, bytes + 46);
	bytes[48] = record->event.severity;
	bytes[49] = record->event.payload_size;
	memcpy(bytes + 52, record->event.payload, record->event.payload_size);
	sys_put_be32(crc32_ieee(bytes, 68), bytes + 68);
}

static int decode(const uint8_t *bytes, struct kfsw_journal_record *record)
{
	if ((memcmp(bytes, "KFE1", 4) != 0) || (bytes[4] != 1U) || (bytes[5] > 1U) ||
	    (sys_get_be16(bytes + 6) != KFSW_JOURNAL_RECORD_SIZE) ||
	    (sys_get_be32(bytes + 68) != crc32_ieee(bytes, 68)) ||
	    (sys_get_be16(bytes + 44) < KFSW_EVENT_SOURCE_BOOT) ||
	    (sys_get_be16(bytes + 44) > KFSW_EVENT_SOURCE_RESMON) ||
	    (bytes[48] > KFSW_EVENT_CRITICAL) || (bytes[49] > KFSW_EVENT_MAX_PAYLOAD_SIZE) ||
	    (bytes[50] != 0U) || (bytes[51] != 0U)) {
		return -EBADMSG;
	}
	memset(record, 0, sizeof(*record));
	record->sequence = sys_get_be64(bytes + 8);
	record->boot = sys_get_be64(bytes + 16);
	if ((record->sequence == 0U) || (record->sequence == UINT64_MAX) || (record->boot == 0U) ||
	    (record->boot == UINT64_MAX)) {
		return -EBADMSG;
	}
	record->utc_valid = bytes[5] != 0U;
	record->utc_seconds = (int64_t)sys_get_be64(bytes + 32);
	record->event.monotonic_us = sys_get_be64(bytes + 24);
	record->event.sequence = sys_get_be32(bytes + 40);
	record->event.source = sys_get_be16(bytes + 44);
	record->event.id = sys_get_be16(bytes + 46);
	record->event.severity = bytes[48];
	record->event.payload_size = bytes[49];
	memcpy(record->event.payload, bytes + 52, record->event.payload_size);
	return 0;
}

int kfsw_journal_store_open(struct kfsw_journal_store *store)
{
	uint8_t header[KFSW_JOURNAL_HEADER_SIZE] = {0};
	uint8_t bytes[KFSW_JOURNAL_RECORD_SIZE];
	struct kfsw_journal_record record;
	struct fs_dirent info;
	struct fs_file_t file;
	int result;

	memset(store, 0, sizeof(*store));
	store->next_sequence = 1U;
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}
	result = fs_stat(KFSW_JOURNAL_PATH, &info);
	if ((result != 0) && (result != -ENOENT)) {
		return result;
	}
	fs_file_t_init(&file);
	if (result == -ENOENT) {
		memcpy(header, "KFJ1", 4);
		header[4] = 1U;
		sys_put_be16(KFSW_JOURNAL_RECORD_SIZE, header + 6);
		sys_put_be32(CONFIG_KFSW_JOURNAL_CAPACITY, header + 8);
		sys_put_be32(crc32_ieee(header, 12), header + 12);
		result = fs_open(&file, KFSW_JOURNAL_PATH, FS_O_CREATE | FS_O_WRITE);
		if (result != 0) {
			return result;
		}
		result = write_all(&file, header, sizeof(header));
		if (result == 0) {
			result = fs_sync(&file);
		}
		result = close_file(&file, result);
		if (result != 0) {
			(void)fs_unlink(KFSW_JOURNAL_PATH);
		}
		store->ready = result == 0;
		return result;
	}
	if ((info.type != FS_DIR_ENTRY_FILE) ||
	    (info.size >
	     KFSW_JOURNAL_HEADER_SIZE + CONFIG_KFSW_JOURNAL_CAPACITY * KFSW_JOURNAL_RECORD_SIZE)) {
		return -EFBIG;
	}
	result = fs_open(&file, KFSW_JOURNAL_PATH, FS_O_READ);
	if (result != 0) {
		return result;
	}
	ssize_t read = fs_read(&file, header, sizeof(header));

	if ((read != sizeof(header)) || (memcmp(header, "KFJ1", 4) != 0) || (header[4] != 1U) ||
	    (header[5] != 0U) || (sys_get_be16(header + 6) != KFSW_JOURNAL_RECORD_SIZE) ||
	    (sys_get_be32(header + 8) != CONFIG_KFSW_JOURNAL_CAPACITY) ||
	    (sys_get_be32(header + 12) != crc32_ieee(header, 12))) {
		return close_file(&file, read < 0 ? (int)read : -EBADMSG);
	}
	for (size_t i = 0; i < ARRAY_SIZE(store->sequences); i++) {
		read = fs_read(&file, bytes, sizeof(bytes));
		if (read < 0) {
			return close_file(&file, (int)read);
		}
		if (read == 0) {
			break;
		}
		if (read != sizeof(bytes)) {
			store->corrupt++;
			break;
		}
		if ((decode(bytes, &record) != 0) ||
		    ((record.sequence - 1U) % ARRAY_SIZE(store->sequences) != i)) {
			store->corrupt++;
			continue;
		}
		store->sequences[i] = record.sequence;
		store->next_sequence = MAX(store->next_sequence, record.sequence + 1U);
		store->highest_boot = MAX(store->highest_boot, record.boot);
		store->held++;
	}
	result = close_file(&file, 0);
	store->ready = result == 0;
	return result;
}

int kfsw_journal_store_append(struct kfsw_journal_store *store, struct kfsw_journal_record *record)
{
	uint8_t bytes[KFSW_JOURNAL_RECORD_SIZE];
	struct fs_file_t file;
	size_t slot;
	int result;

	if (!store->ready || !kfsw_storage_is_ready()) {
		return -ENODEV;
	}
	if ((record->boot == 0U) || (record->boot == UINT64_MAX) ||
	    (record->event.payload_size > KFSW_EVENT_MAX_PAYLOAD_SIZE) ||
	    (record->event.severity > KFSW_EVENT_CRITICAL)) {
		return -EINVAL;
	}
	if (record->sequence == 0U) {
		record->sequence = store->next_sequence;
	}
	if ((record->sequence == UINT64_MAX) || (record->sequence > store->next_sequence)) {
		return -EOVERFLOW;
	}
	slot = (record->sequence - 1U) % ARRAY_SIZE(store->sequences);
	if (store->sequences[slot] > record->sequence) {
		return -ESTALE;
	}
	encode(record, bytes);
	fs_file_t_init(&file);
	result = fs_open(&file, KFSW_JOURNAL_PATH, FS_O_WRITE);
	if (result != 0) {
		store->ready = false;
		return result;
	}
	result = fs_seek(&file, KFSW_JOURNAL_HEADER_SIZE + slot * sizeof(bytes), FS_SEEK_SET);
	if (result == 0) {
		result = write_all(&file, bytes, sizeof(bytes));
	}
	if (result == 0) {
		result = fs_sync(&file);
	}
	result = close_file(&file, result);
	if (result != 0) {
		store->ready = false;
		return result;
	}
	if (store->sequences[slot] == 0U) {
		store->held++;
	}
	store->sequences[slot] = record->sequence;
	store->next_sequence = MAX(store->next_sequence, record->sequence + 1U);
	store->highest_boot = MAX(store->highest_boot, record->boot);
	return 0;
}

int kfsw_journal_store_get(struct kfsw_journal_store *store, uint16_t age,
			   struct kfsw_journal_record *record)
{
	uint64_t ceiling = UINT64_MAX;
	uint64_t sequence = 0;
	uint8_t bytes[KFSW_JOURNAL_RECORD_SIZE];
	struct fs_file_t file;
	size_t slot = 0;
	int result;

	if (!store->ready) {
		return -ENODEV;
	}
	if (age >= store->held) {
		return -ENOENT;
	}
	for (unsigned int rank = 0; rank <= age; rank++) {
		sequence = 0;
		for (size_t i = 0; i < ARRAY_SIZE(store->sequences); i++) {
			if ((store->sequences[i] > sequence) && (store->sequences[i] < ceiling)) {
				sequence = store->sequences[i];
				slot = i;
			}
		}
		ceiling = sequence;
	}
	fs_file_t_init(&file);
	result = fs_open(&file, KFSW_JOURNAL_PATH, FS_O_READ);
	if (result != 0) {
		return result;
	}
	result = fs_seek(&file, KFSW_JOURNAL_HEADER_SIZE + slot * sizeof(bytes), FS_SEEK_SET);
	if (result == 0) {
		ssize_t read = fs_read(&file, bytes, sizeof(bytes));

		result = (read < 0) ? (int)read
				    : ((read == sizeof(bytes)) ? decode(bytes, record) : -EBADMSG);
		if ((result == 0) && (record->sequence != sequence)) {
			result = -EBADMSG;
		}
	}
	return close_file(&file, result);
}
