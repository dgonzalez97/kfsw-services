#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/hk.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>

#include "hk_internal.h"

#if CONFIG_KFSW_HK_STORE

/*
 * Samples stored in a ring file with fixed-size records. The file stops growing
 * once every slot is used. The newest record is found from the sequence numbers
 * when the file is opened, so no header is rewritten on each write.
 */
#define KFSW_HK_STORE_DIRECTORY KFSW_STORAGE_MOUNT_POINT "/hk"
#define KFSW_HK_STORE_MAGIC "KHKS"
#define KFSW_HK_STORE_MAGIC_SIZE 4U
#define KFSW_HK_STORE_VERSION 1U

/* magic 4, version 1, report 1, record size 2, capacity 4 */
#define KFSW_HK_STORE_HEADER_SIZE 12U

struct store_state {
	uint32_t interval_ms;
	uint16_t every_n;
	uint16_t pending;
	uint16_t record_size;
	bool open;
};

static struct store_state stores[CONFIG_KFSW_HK_REPORTS];

static void store_path(uint8_t report, char *out, size_t size)
{
	(void)snprintf(out, size, KFSW_HK_STORE_DIRECTORY "/report%u.bin", report);
}

/*
 * Rated erase cycles of the storage flash, used in the interval warning.
 */
#define KFSW_HK_STORE_PARTITION DT_CHOSEN(kfsw_storage_partition)
#define KFSW_HK_STORE_ERASE_BLOCK                                                                  \
	DT_PROP_OR(DT_GPARENT(KFSW_HK_STORE_PARTITION), erase_block_size, 4096)
#define KFSW_HK_STORE_PARTITION_BYTES DT_REG_SIZE(KFSW_HK_STORE_PARTITION)

#define KFSW_HK_STORE_ERASE_BUDGET                                                                 \
	((uint32_t)(KFSW_HK_STORE_PARTITION_BYTES / KFSW_HK_STORE_ERASE_BLOCK) *                   \
	 (uint32_t)CONFIG_KFSW_HK_STORE_ERASE_CYCLES)

static int write_at(struct fs_file_t *file, off_t offset, const uint8_t *data, size_t size)
{
	int result = fs_seek(file, offset, FS_SEEK_SET);

	if (result != 0) {
		return result;
	}
	while (size > 0U) {
		ssize_t written = fs_write(file, data, size);

		if (written < 0) {
			return (int)written;
		}
		if (written == 0) {
			return -EIO;
		}
		data += written;
		size -= (size_t)written;
	}
	return 0;
}

static int create_file(uint8_t report, uint16_t record_size)
{
	uint8_t header[KFSW_HK_STORE_HEADER_SIZE] = {0};
	char path[64];
	struct fs_file_t file;
	int result;
	int close_result;

	result = fs_mkdir(KFSW_HK_STORE_DIRECTORY);
	if ((result != 0) && (result != -EEXIST)) {
		return result;
	}

	memcpy(header, KFSW_HK_STORE_MAGIC, KFSW_HK_STORE_MAGIC_SIZE);
	header[4] = KFSW_HK_STORE_VERSION;
	header[5] = report;
	sys_put_be16(record_size, &header[6]);
	sys_put_be32(CONFIG_KFSW_HK_STORE_CAPACITY, &header[8]);

	store_path(report, path, sizeof(path));
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result != 0) {
		return result;
	}
	result = write_at(&file, 0, header, sizeof(header));
	close_result = fs_close(&file);
	return (result != 0) ? result : close_result;
}

int kfsw_hk_store_bytes_needed(uint16_t record_size)
{
	return (int)(KFSW_HK_STORE_HEADER_SIZE +
		     ((size_t)record_size * (size_t)CONFIG_KFSW_HK_STORE_CAPACITY));
}

int kfsw_hk_store_configure(uint8_t report, uint32_t interval_ms, uint32_t period_ms,
			    uint16_t record_size)
{
	struct kfsw_storage_info storage;
	uint32_t needed;
	uint32_t every;
	int result;

	if (report >= ARRAY_SIZE(stores)) {
		return -EINVAL;
	}
	if (interval_ms == 0U) {
		stores[report].interval_ms = 0U;
		stores[report].open = false;
		stores[report].pending = 0U;
		return 0;
	}
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}
	if (interval_ms < CONFIG_KFSW_HK_STORE_FLOOR_MS) {
		return -ERANGE;
	}

	/* Refuse a store that won't fit now, not when the filesystem fills. */
	needed = (uint32_t)kfsw_hk_store_bytes_needed(record_size);
	result = kfsw_storage_get_info(&storage);
	if (result != 0) {
		return result;
	}
	if ((uint64_t)needed > storage.free_bytes) {
		kfsw_log_warning("HK: report %u needs %u bytes and %u are free", report, needed,
				 (uint32_t)storage.free_bytes);
		return -ENOSPC;
	}

	result = create_file(report, record_size);
	if (result != 0) {
		return result;
	}

	/* Batch as many collections as fit in the interval, capped at the ring depth. */
	every = (period_ms == 0U) ? 1U : (((uint64_t)interval_ms + period_ms - 1U) / period_ms);
	if (every == 0U) {
		every = 1U;
	}
	if (every > CONFIG_KFSW_HK_HISTORY) {
		kfsw_log_warning("HK: report %u stores every %u samples, not %u: the ring holds %u",
				 report, (unsigned int)CONFIG_KFSW_HK_HISTORY, (unsigned int)every,
				 (unsigned int)CONFIG_KFSW_HK_HISTORY);
		every = CONFIG_KFSW_HK_HISTORY;
	}

	stores[report].interval_ms = interval_ms;
	stores[report].every_n = (uint16_t)every;
	stores[report].record_size = record_size;
	stores[report].pending = 0U;
	stores[report].open = true;

	if (interval_ms < CONFIG_KFSW_HK_STORE_ADVISED_MS) {
		uint32_t per_day = (uint32_t)(86400000UL / interval_ms);

		kfsw_log_warning("HK: report %u stores every %u ms, about %u writes a day "
				 "against this partition's %u erase budget",
				 report, interval_ms, per_day, KFSW_HK_STORE_ERASE_BUDGET);
	}
	kfsw_log_info("HK: report %u stores every %u ms, %u samples a write", report, interval_ms,
		      (unsigned int)every);
	return 0;
}

uint32_t kfsw_hk_store_interval(uint8_t report)
{
	if (report >= ARRAY_SIZE(stores)) {
		return 0U;
	}
	return stores[report].interval_ms;
}

void kfsw_hk_store_forget(uint8_t report)
{
	char path[64];

	if (report >= ARRAY_SIZE(stores)) {
		return;
	}
	stores[report].open = false;
	stores[report].interval_ms = 0U;
	stores[report].pending = 0U;
	store_path(report, path, sizeof(path));
	(void)fs_unlink(path);
}

int kfsw_hk_store_flush(uint8_t report, uint16_t next_sequence)
{
	struct fs_file_t file;
	char path[64];
	uint16_t first;
	int result;
	int close_result;

	if ((report >= ARRAY_SIZE(stores)) || !stores[report].open) {
		return -EACCES;
	}

	stores[report].pending = MIN(stores[report].pending + 1U, CONFIG_KFSW_HK_HISTORY);
	if (stores[report].pending < stores[report].every_n) {
		return 0;
	}

	store_path(report, path, sizeof(path));
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_WRITE);
	if (result != 0) {
		return result;
	}

	/* Write each sample to the slot its sequence number selects, oldest first. */
	first = (uint16_t)(next_sequence - stores[report].pending);
	for (uint16_t index = 0U; index < stores[report].pending; index++) {
		uint16_t sequence = (uint16_t)(first + index);
		uint16_t age = (uint16_t)(stores[report].pending - 1U - index);
		struct kfsw_hk_sample sample;
		off_t offset = (off_t)KFSW_HK_STORE_HEADER_SIZE +
			       ((off_t)(sequence % CONFIG_KFSW_HK_STORE_CAPACITY) *
				(off_t)stores[report].record_size);

		result = kfsw_hk_get(report, age, &sample);
		if (result == 0) {
			result = write_at(&file, offset, sample.data, stores[report].record_size);
		}
		if (result != 0) {
			break;
		}
	}

	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	stores[report].pending = 0U;
	return result;
}

#if CONFIG_KFSW_HK_PERSISTENCE
static struct store_state restored_stores[CONFIG_KFSW_HK_REPORTS];
static uint16_t restored_sequences[CONFIG_KFSW_HK_REPORTS];

int kfsw_hk_store_restore_prepare(uint8_t report, uint32_t interval_ms, uint32_t period_ms,
				  uint16_t record_size)
{
	struct fs_file_t file;
	uint8_t header[KFSW_HK_STORE_HEADER_SIZE];
	char path[64];
	int result;

	memset(&restored_stores[report], 0, sizeof(restored_stores[report]));
	restored_sequences[report] = 0;
	if (interval_ms == 0U) {
		return 0;
	}
	store_path(report, path, sizeof(path));
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result == -ENOENT) {
		result = create_file(report, record_size);
	} else if (result == 0) {
		ssize_t read = fs_read(&file, header, sizeof(header));
		bool have_sequence = false;
		uint16_t newest = 0;

		result = 0;
		if (read != sizeof(header) || memcmp(header, KFSW_HK_STORE_MAGIC, 4) != 0 ||
		    header[4] != KFSW_HK_STORE_VERSION || header[5] != report ||
		    sys_get_be16(&header[6]) != record_size ||
		    sys_get_be32(&header[8]) != CONFIG_KFSW_HK_STORE_CAPACITY) {
			result = -EBADMSG;
		}
		for (uint32_t slot = 0; result == 0 && slot <= CONFIG_KFSW_HK_STORE_CAPACITY;
		     slot++) {
			uint8_t sample[CONFIG_KFSW_HK_SAMPLE_BYTES];

			read = fs_read(&file, sample, record_size);
			if (read == 0) {
				break;
			}
			if (read != record_size || slot == CONFIG_KFSW_HK_STORE_CAPACITY) {
				result = read < 0 ? (int)read : -EBADMSG;
				break;
			}
			if (sample[0] == 0U) {
				continue;
			} /* Unwritten sparse slot. */
			uint16_t sequence = sys_get_be16(&sample[2]);

			if (sample[0] != KFSW_HK_PROTOCOL_VERSION || sample[1] != report ||
			    sequence % CONFIG_KFSW_HK_STORE_CAPACITY != slot) {
				result = -EBADMSG;
				break;
			}
			if (!have_sequence || (int16_t)(sequence - newest) > 0) {
				newest = sequence;
				have_sequence = true;
			}
		}
		int closed = fs_close(&file);

		if (result == 0) {
			result = closed;
		}
		if (have_sequence) {
			restored_sequences[report] = newest + 1U;
		}
	}
	if (result != 0) {
		return result;
	}
	uint64_t every =
		period_ms == 0U ? 1U : ((uint64_t)interval_ms + period_ms - 1U) / period_ms;

	restored_stores[report] = (struct store_state){
		.interval_ms = interval_ms,
		.every_n = MIN(every, CONFIG_KFSW_HK_HISTORY),
		.record_size = record_size,
		.open = true,
	};
	return 0;
}

uint16_t kfsw_hk_store_restore_sequence(uint8_t report)
{
	return restored_sequences[report];
}

void kfsw_hk_store_restore_commit(void)
{
	memcpy(stores, restored_stores, sizeof(stores));
}
#endif

#endif /* CONFIG_KFSW_HK_STORE */
