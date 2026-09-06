#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/hk.h>
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>

#include "hk_internal.h"

#if CONFIG_KFSW_HK_PERSISTENCE

/*
 * Definitions outlive a reset, the samples do not.
 *
 * The same shape as the parameter snapshot next door: a magic, a version, a
 * CRC, and a write through a temporary file that is renamed only once it is
 * whole. A reset landing mid-write leaves the previous set intact rather than
 * half of two.
 */
#define KFSW_HK_PERSIST_DIRECTORY KFSW_STORAGE_MOUNT_POINT "/hk"
#define KFSW_HK_PERSIST_PATH KFSW_HK_PERSIST_DIRECTORY "/reports.dat"
#define KFSW_HK_PERSIST_TEMP_PATH KFSW_HK_PERSIST_DIRECTORY "/reports.tmp"
#define KFSW_HK_PERSIST_MAGIC "KHKR"
#define KFSW_HK_PERSIST_MAGIC_SIZE 4U
#define KFSW_HK_PERSIST_VERSION 1U
#define KFSW_HK_PERSIST_HEADER_SIZE 12U
#define KFSW_HK_PERSIST_CRC_OFFSET 8U

/* magic 4, version 1, reserved 1, count 2, crc 4, then per report:
 * id 1, entry count 1, period 4, then entries of node 2 and id 2.
 */
#define KFSW_HK_PERSIST_REPORT_HEADER 6U
#define KFSW_HK_PERSIST_ENTRY_SIZE 4U
#define KFSW_HK_PERSIST_MAX_SIZE                                                                   \
	(KFSW_HK_PERSIST_HEADER_SIZE +                                                             \
	 (CONFIG_KFSW_HK_REPORTS * (KFSW_HK_PERSIST_REPORT_HEADER +                                \
				    (CONFIG_KFSW_HK_ENTRIES * KFSW_HK_PERSIST_ENTRY_SIZE))))

static uint8_t blob[KFSW_HK_PERSIST_MAX_SIZE];

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

int kfsw_hk_persist_save(void)
{
	struct fs_file_t file;
	size_t offset = KFSW_HK_PERSIST_HEADER_SIZE;
	uint16_t saved = 0U;
	int result;
	int close_result;

	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}

	kfsw_hk_lock();
	for (uint8_t index = 0U; index < CONFIG_KFSW_HK_REPORTS; index++) {
		const struct kfsw_hk_report *report = kfsw_hk_report_at(index);

		if ((report == NULL) || !report->defined) {
			continue;
		}
		blob[offset++] = index;
		blob[offset++] = report->entry_count;
		sys_put_be32(report->period_ms, &blob[offset]);
		offset += 4U;
		for (uint8_t entry = 0U; entry < report->entry_count; entry++) {
			sys_put_be16(report->entries[entry].node, &blob[offset]);
			sys_put_be16(report->entries[entry].param_id, &blob[offset + 2U]);
			offset += KFSW_HK_PERSIST_ENTRY_SIZE;
		}
		saved++;
	}
	kfsw_hk_unlock();

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

	fs_file_t_init(&file);
	result = fs_open(&file, KFSW_HK_PERSIST_TEMP_PATH, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (result != 0) {
		return result;
	}
	result = write_all(&file, blob, offset);
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result != 0) {
		(void)fs_unlink(KFSW_HK_PERSIST_TEMP_PATH);
		return result;
	}

	(void)fs_unlink(KFSW_HK_PERSIST_PATH);
	result = fs_rename(KFSW_HK_PERSIST_TEMP_PATH, KFSW_HK_PERSIST_PATH);
	if (result != 0) {
		(void)fs_unlink(KFSW_HK_PERSIST_TEMP_PATH);
		return result;
	}

	kfsw_log_info("HK: %u report definitions saved", saved);
	return 0;
}

int kfsw_hk_persist_load(void)
{
	struct fs_file_t file;
	struct fs_dirent info;
	size_t offset = KFSW_HK_PERSIST_HEADER_SIZE;
	uint16_t expected;
	uint32_t stored_crc;
	uint32_t actual_crc;
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
	if (blob[4] != KFSW_HK_PERSIST_VERSION) {
		/* Refused rather than guessed at: a layout this reader does not
		 * know would be decoded into the wrong parameters.
		 */
		kfsw_log_warning("HK: saved definitions are version %u, not %u", blob[4],
				 KFSW_HK_PERSIST_VERSION);
		return -EPROTONOSUPPORT;
	}

	stored_crc = sys_get_be32(&blob[KFSW_HK_PERSIST_CRC_OFFSET]);
	sys_put_be32(0U, &blob[KFSW_HK_PERSIST_CRC_OFFSET]);
	actual_crc = crc32_ieee(blob, (size_t)info.size);
	if (actual_crc != stored_crc) {
		kfsw_log_warning("HK: saved definitions failed their checksum");
		return -EBADMSG;
	}

	expected = sys_get_be16(&blob[6]);
	for (uint16_t index = 0U; index < expected; index++) {
		struct kfsw_hk_entry entries[CONFIG_KFSW_HK_ENTRIES];
		uint8_t report;
		uint8_t count;
		uint32_t period;

		if ((offset + KFSW_HK_PERSIST_REPORT_HEADER) > (size_t)info.size) {
			return -EBADMSG;
		}
		report = blob[offset];
		count = blob[offset + 1U];
		period = sys_get_be32(&blob[offset + 2U]);
		offset += KFSW_HK_PERSIST_REPORT_HEADER;

		if ((count == 0U) || (count > ARRAY_SIZE(entries)) ||
		    ((offset + ((size_t)count * KFSW_HK_PERSIST_ENTRY_SIZE)) > (size_t)info.size)) {
			return -EBADMSG;
		}
		for (uint8_t entry = 0U; entry < count; entry++) {
			entries[entry].node = sys_get_be16(&blob[offset]);
			entries[entry].param_id = sys_get_be16(&blob[offset + 2U]);
			offset += KFSW_HK_PERSIST_ENTRY_SIZE;
		}

		/* Re-validated on the way in, not trusted. A parameter named by
		 * a definition written before an update may not exist any more,
		 * and a report that cannot be collected should not come back
		 * looking defined.
		 */
		result = kfsw_hk_define(report, entries, count);
		if (result != 0) {
			kfsw_log_warning("HK: saved report %u no longer defines (%d)", report,
					 result);
			continue;
		}
		if (period != 0U) {
			(void)kfsw_hk_set_period(report, period);
		}
	}
	return 0;
}

#endif /* CONFIG_KFSW_HK_PERSISTENCE */
