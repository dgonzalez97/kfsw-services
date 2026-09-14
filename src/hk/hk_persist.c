#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/hk.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>

#include "../snapshot_file.h"
#include "hk_internal.h"

#if CONFIG_KFSW_HK_PERSISTENCE

/*
 * Report definitions survive a reset; samples don't. The file has a magic,
 * version and CRC, and is written through a temporary file.
 */
#define KFSW_HK_PERSIST_DIRECTORY KFSW_STORAGE_MOUNT_POINT "/hk"
#define KFSW_HK_PERSIST_PATH KFSW_HK_PERSIST_DIRECTORY "/reports.dat"
#define KFSW_HK_PERSIST_TEMP_PATH KFSW_HK_PERSIST_DIRECTORY "/reports.tmp"
#define KFSW_HK_PERSIST_MAGIC "KHKR"
#define KFSW_HK_PERSIST_MAGIC_SIZE 4U
#define KFSW_HK_PERSIST_VERSION 2U
#define KFSW_HK_PERSIST_HEADER_SIZE 12U
#define KFSW_HK_PERSIST_CRC_OFFSET 8U

/* magic 4, version 1, reserved 1, count 2, crc 4, then per report:
 * id 1, entry count 1, period 4, store interval 4, beacon node 2,
 * beacon interval 4, then entries of node 2 and id 2.
 *
 * Version 2 added the store interval and the beacon fields. Version 1 files
 * are still read.
 */
#define KFSW_HK_PERSIST_REPORT_HEADER_V1 6U
#define KFSW_HK_PERSIST_REPORT_HEADER 16U
#define KFSW_HK_PERSIST_ENTRY_SIZE 4U
#define KFSW_HK_PERSIST_MAX_SIZE                                                                   \
	(KFSW_HK_PERSIST_HEADER_SIZE +                                                             \
	 (CONFIG_KFSW_HK_REPORTS * (KFSW_HK_PERSIST_REPORT_HEADER +                                \
				    (CONFIG_KFSW_HK_ENTRIES * KFSW_HK_PERSIST_ENTRY_SIZE))))

static uint8_t blob[KFSW_HK_PERSIST_MAX_SIZE];
static K_MUTEX_DEFINE(file_lock);

struct restored_report {
	struct kfsw_hk_definition definition;
	uint32_t period_ms;
	uint32_t store_ms;
	uint32_t beacon_ms;
	uint16_t beacon_node;
	bool present;
};

static struct restored_report restored[CONFIG_KFSW_HK_REPORTS];

/* The store and beacon fields are always written, as zero when not built in. */
static uint32_t store_interval_of(uint8_t report)
{
#if CONFIG_KFSW_HK_STORE
	uint32_t interval = 0U;

	(void)kfsw_hk_get_store(report, &interval);
	return interval;
#else
	ARG_UNUSED(report);
	return 0U;
#endif
}

static uint16_t beacon_node_of(uint8_t report)
{
#if CONFIG_KFSW_HK_BEACON
	uint16_t node = 0U;
	uint32_t interval = 0U;

	(void)kfsw_hk_get_beacon(report, &node, &interval);
	return node;
#else
	ARG_UNUSED(report);
	return 0U;
#endif
}

static uint32_t beacon_interval_of(uint8_t report)
{
#if CONFIG_KFSW_HK_BEACON
	uint16_t node = 0U;
	uint32_t interval = 0U;

	(void)kfsw_hk_get_beacon(report, &node, &interval);
	return interval;
#else
	ARG_UNUSED(report);
	return 0U;
#endif
}

static int save_snapshot(uint64_t *revision)
{
	size_t offset = KFSW_HK_PERSIST_HEADER_SIZE;
	uint16_t saved = 0U;
	int result;

	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}

	kfsw_hk_storage_lock();
	kfsw_hk_lock();
	*revision = kfsw_hk_config_revision();
	if (kfsw_hk_save_blocked()) {
		kfsw_hk_unlock();
		kfsw_hk_storage_unlock();
		return -EROFS;
	}
	for (uint8_t index = 0U; index < CONFIG_KFSW_HK_REPORTS; index++) {
		const struct kfsw_hk_report *report = kfsw_hk_report_at(index);

		if ((report == NULL) || !report->defined) {
			continue;
		}
		blob[offset++] = index;
		blob[offset++] = report->entry_count;
		sys_put_be32(report->period_ms, &blob[offset]);
		offset += 4U;
		sys_put_be32(store_interval_of(index), &blob[offset]);
		offset += 4U;
		sys_put_be16(beacon_node_of(index), &blob[offset]);
		offset += 2U;
		sys_put_be32(beacon_interval_of(index), &blob[offset]);
		offset += 4U;
		for (uint8_t entry = 0U; entry < report->entry_count; entry++) {
			sys_put_be16(report->entries[entry].node, &blob[offset]);
			sys_put_be16(report->entries[entry].param_id, &blob[offset + 2U]);
			offset += KFSW_HK_PERSIST_ENTRY_SIZE;
		}
		saved++;
	}
	kfsw_hk_unlock();
	kfsw_hk_storage_unlock();

	memcpy(blob, KFSW_HK_PERSIST_MAGIC, KFSW_HK_PERSIST_MAGIC_SIZE);
	blob[4] = KFSW_HK_PERSIST_VERSION;
	blob[5] = 0U;
	sys_put_be16(saved, &blob[6]);
	sys_put_be32(0U, &blob[KFSW_HK_PERSIST_CRC_OFFSET]);
	sys_put_be32(crc32_ieee(blob, offset), &blob[KFSW_HK_PERSIST_CRC_OFFSET]);

	result = fs_mkdir(KFSW_HK_PERSIST_DIRECTORY);
	if ((result != 0) && (result != -EEXIST)) {
		return result;
	}

	result = kfsw_snapshot_write(KFSW_HK_PERSIST_PATH, KFSW_HK_PERSIST_TEMP_PATH, blob, offset);
	if (result != 0) {
		return result;
	}

	kfsw_log_info("HK: %u report definitions saved", saved);
	return 0;
}

static int load_snapshot(void)
{
	struct fs_file_t file;
	struct fs_dirent info;
	size_t offset = KFSW_HK_PERSIST_HEADER_SIZE;
	uint16_t expected;
	uint32_t stored_crc;
	uint32_t actual_crc;
	uint8_t version;
	size_t report_header;
	ssize_t read;
	int result;

	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}
	result = fs_stat(KFSW_HK_PERSIST_PATH, &info);
	if (result != 0) {
		return result;
	}
	if ((info.size < KFSW_HK_PERSIST_HEADER_SIZE) || (info.size > sizeof(blob))) {
		return -EFBIG;
	}

	fs_file_t_init(&file);
	result = fs_open(&file, KFSW_HK_PERSIST_PATH, FS_O_READ);
	if (result != 0) {
		return result;
	}
	read = fs_read(&file, blob, info.size);
	(void)fs_close(&file);
	if (read != (ssize_t)info.size) {
		return -EIO;
	}

	if (memcmp(blob, KFSW_HK_PERSIST_MAGIC, KFSW_HK_PERSIST_MAGIC_SIZE) != 0) {
		return -EBADMSG;
	}
	/* Older versions are read; newer ones are refused. */
	version = blob[4];
	if ((version != KFSW_HK_PERSIST_VERSION) && (version != 1U)) {
		kfsw_log_warning("HK: saved definitions are version %u, not %u", version,
				 KFSW_HK_PERSIST_VERSION);
		return -EPROTONOSUPPORT;
	}
	report_header =
		(version == 1U) ? KFSW_HK_PERSIST_REPORT_HEADER_V1 : KFSW_HK_PERSIST_REPORT_HEADER;

	stored_crc = sys_get_be32(&blob[KFSW_HK_PERSIST_CRC_OFFSET]);
	sys_put_be32(0U, &blob[KFSW_HK_PERSIST_CRC_OFFSET]);
	actual_crc = crc32_ieee(blob, (size_t)info.size);
	if (actual_crc != stored_crc) {
		kfsw_log_warning("HK: saved definitions failed their checksum");
		return -EBADMSG;
	}

	expected = sys_get_be16(&blob[6]);
	if (expected > ARRAY_SIZE(restored) || blob[5] != 0U) {
		return -EBADMSG;
	}
	memset(restored, 0, sizeof(restored));
	for (uint16_t index = 0; index < expected; index++) {
		struct kfsw_hk_entry entries[CONFIG_KFSW_HK_ENTRIES];
		uint8_t report;
		uint8_t count;

		if (offset + report_header > info.size) {
			return -EBADMSG;
		}
		report = blob[offset];
		count = blob[offset + 1];
		if (report >= ARRAY_SIZE(restored) || restored[report].present || count == 0U ||
		    count > ARRAY_SIZE(entries)) {
			return -EBADMSG;
		}
		struct restored_report *item = &restored[report];

		item->present = true;
		item->period_ms = sys_get_be32(&blob[offset + 2]);
		if (version == 2U) {
			item->store_ms = sys_get_be32(&blob[offset + 6]);
			item->beacon_node = sys_get_be16(&blob[offset + 10]);
			item->beacon_ms = sys_get_be32(&blob[offset + 12]);
		}
		if (item->period_ms != 0U && item->period_ms < CONFIG_KFSW_HK_PERIOD_FLOOR_MS) {
			return -ERANGE;
		}
#if CONFIG_KFSW_HK_STORE
		if (item->store_ms != 0U && item->store_ms < CONFIG_KFSW_HK_STORE_FLOOR_MS) {
			return -ERANGE;
		}
#endif
#if CONFIG_KFSW_HK_BEACON
		if (item->beacon_ms != 0U &&
		    (item->beacon_ms < CONFIG_KFSW_HK_BEACON_FLOOR_MS || item->beacon_node == 0U ||
		     item->beacon_node > 16383U)) {
			return -ERANGE;
		}
#endif
		offset += report_header;
		if (offset + count * KFSW_HK_PERSIST_ENTRY_SIZE > info.size) {
			return -EBADMSG;
		}
		for (uint8_t entry = 0; entry < count; entry++) {
			entries[entry].node = sys_get_be16(&blob[offset]);
			entries[entry].param_id = sys_get_be16(&blob[offset + 2]);
			offset += KFSW_HK_PERSIST_ENTRY_SIZE;
		}
		result = kfsw_hk_prepare_definition(report, entries, count, &item->definition);
		if (result != 0) {
			return result;
		}
	}
	if (offset != info.size) {
		return -EBADMSG;
	}

	/* Prepare every store without replacing an existing sample file. */
	kfsw_hk_storage_lock();
#if CONFIG_KFSW_HK_STORE
	for (uint8_t index = 0; index < ARRAY_SIZE(restored); index++) {
		const struct restored_report *item = &restored[index];

		result = kfsw_hk_store_restore_prepare(index, item->store_ms, item->period_ms,
						       KFSW_HK_HEADER_SIZE +
							       item->definition.payload_bytes);
		if (result != 0) {
			kfsw_hk_storage_unlock();
			return result;
		}
	}
#endif
	kfsw_hk_lock();
	for (uint8_t index = 0; index < ARRAY_SIZE(restored); index++) {
		const struct restored_report *item = &restored[index];

		kfsw_hk_restore_report(index, item->present ? &item->definition : NULL,
				       item->period_ms);
#if CONFIG_KFSW_HK_BEACON
		kfsw_hk_beacon_restore(index, item->beacon_node, item->beacon_ms);
#endif
	}
#if CONFIG_KFSW_HK_STORE
	kfsw_hk_store_restore_commit();
#endif
	kfsw_hk_unlock();
	kfsw_hk_storage_unlock();
	return 0;
}

int kfsw_hk_persist_save(void)
{
	uint64_t revision = 0;
	int result;

	k_mutex_lock(&file_lock, K_FOREVER);
	result = save_snapshot(&revision);
	kfsw_hk_save_result(revision, result);
	k_mutex_unlock(&file_lock);
	return result;
}

int kfsw_hk_persist_load(void)
{
	int result;

	/* Changes release the config lock before waiting for the file lock. */
	kfsw_hk_restore_begin();
	k_mutex_lock(&file_lock, K_FOREVER);
	result = load_snapshot();
	kfsw_hk_restore_end(result);
	k_mutex_unlock(&file_lock);
	return result;
}

#endif /* CONFIG_KFSW_HK_PERSISTENCE */
