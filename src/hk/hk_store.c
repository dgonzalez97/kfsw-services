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
 * A ring on disk, mirroring the ring in RAM.
 *
 * Records are fixed size because a report's frame length is settled when it is
 * defined, which is the same fact that already fixes the widths. Fixed records
 * mean the oldest is overwritten in place, so rotation is arithmetic rather
 * than a rewrite and the file stops growing once every slot has been used —
 * capacity times the record size, and never more. A full filesystem cannot
 * creep up on a pass.
 *
 * The header is written once, at creation. Which record is newest is worked
 * out by reading sequence numbers when the file is opened, because deriving it
 * costs one pass over a small file and storing it would mean rewriting the
 * same block on every single write — the one block that would then wear out
 * first.
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
 * How many erases the backing partition is rated for in total.
 *
 * Only ever used to put a number beside a store interval in the warning. The
 * service cannot know the part, so the cycle count comes from the composition
 * and the geometry from the flash the storage partition sits on.
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

	/* Refused here rather than when the filesystem fills. A report that
	 * cannot be stored should say so while somebody is still listening.
	 */
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

	/* Collect often, write rarely: the batch is how many collections fit in
	 * the interval. Capped at the ring depth, because a batch larger than
	 * the ring would mean samples were overwritten before they were
	 * written out, and losing them quietly is worse than writing more often
	 * than asked.
	 */
	every = (period_ms == 0U) ? 1U : ((interval_ms + period_ms - 1U) / period_ms);
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

int kfsw_hk_store_flush(uint8_t report, const struct kfsw_hk_report *entry)
{
	struct fs_file_t file;
	char path[64];
	uint16_t first;
	int result;
	int close_result;

	if ((report >= ARRAY_SIZE(stores)) || (entry == NULL) || !stores[report].open) {
		return -EACCES;
	}

	stores[report].pending++;
	if (stores[report].pending < stores[report].every_n) {
		return 0;
	}

	store_path(report, path, sizeof(path));
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_WRITE);
	if (result != 0) {
		return result;
	}

	/* The batch is the newest `pending` samples, oldest first, each into
	 * the slot its own sequence number names. Writing by sequence rather
	 * than in order means a replayed or repeated flush lands in the same
	 * place instead of shifting the ring.
	 */
	first = (uint16_t)(entry->sequence - stores[report].pending);
	for (uint16_t index = 0U; index < stores[report].pending; index++) {
		uint16_t sequence = (uint16_t)(first + index);
		uint16_t age = (uint16_t)(stores[report].pending - 1U - index);
		uint16_t slot = (uint16_t)((entry->next_slot + CONFIG_KFSW_HK_HISTORY - 1U - age) %
					   CONFIG_KFSW_HK_HISTORY);
		off_t offset = (off_t)KFSW_HK_STORE_HEADER_SIZE +
			       ((off_t)(sequence % CONFIG_KFSW_HK_STORE_CAPACITY) *
				(off_t)stores[report].record_size);

		result =
			write_at(&file, offset, entry->ring[slot].data, stores[report].record_size);
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

#endif /* CONFIG_KFSW_HK_STORE */
