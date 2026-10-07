#include <errno.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <kfsw/services/boot.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_BOOT
#include <kfsw/services/log.h>
#if CONFIG_KFSW_STORAGE
#include <kfsw/platform/storage.h>
#include "snapshot_file.h"
#endif
#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
#include <bootutil/image.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#endif

#define RECORD_MAGIC_SIZE 4U
#define RECORD_IMAGE_OFFSET 4U
#define RECORD_ATTEMPTS_OFFSET 36U
#define RECORD_TRIAL_OFFSET 40U
#define RECORD_REASON_OFFSET 41U
#define RECORD_REVERT_OFFSET 42U
#define RECORD_PREVIOUS_IMAGE_OFFSET 44U
#define RECORD_SIZE 80U
#define RECORD_CRC_OFFSET 76U
#if CONFIG_KFSW_STORAGE
#define RECORD_PATH KFSW_STORAGE_MOUNT_POINT "/boot-trial.dat"
#define RECORD_TEMP_PATH KFSW_STORAGE_MOUNT_POINT "/boot-trial.tmp"
#endif

K_MUTEX_DEFINE(boot_trial_lock);
static struct kfsw_boot_diagnostics diagnostics = {
	.attempts = UINT32_MAX,
	.revert_reason = UINT8_MAX,
};

void kfsw_boot_get_diagnostics(struct kfsw_boot_diagnostics *value)
{
	k_mutex_lock(&boot_trial_lock, K_FOREVER);
	*value = diagnostics;
	k_mutex_unlock(&boot_trial_lock);
}

#if CONFIG_KFSW_STORAGE
static int load_record(uint8_t *record)
{
	struct fs_file_t file;
	uint8_t extra;
	size_t offset = 0U;
	int result;

	fs_file_t_init(&file);
	result = fs_open(&file, RECORD_PATH, FS_O_READ);
	if (result != 0) {
		return result;
	}
	while (offset < RECORD_SIZE) {
		ssize_t count = fs_read(&file, &record[offset], RECORD_SIZE - offset);

		if (count <= 0) {
			result = count < 0 ? (int)count : -EBADMSG;
			break;
		}
		offset += (size_t)count;
	}
	if ((result == 0) && (fs_read(&file, &extra, 1U) != 0)) {
		result = -EBADMSG;
	}
	int close_result = fs_close(&file);

	if (result == 0) {
		result = close_result;
	}
	if (result != 0) {
		return result;
	}
	if ((memcmp(record, "KBT1", RECORD_MAGIC_SIZE) != 0) ||
	    (record[RECORD_TRIAL_OFFSET] > 1U) ||
	    (record[RECORD_REASON_OFFSET] > KFSW_BOOT_REVERT_REPLACED_UNKNOWN) ||
	    (record[RECORD_REVERT_OFFSET] > 1U) ||
	    (sys_get_be32(&record[RECORD_ATTEMPTS_OFFSET]) == UINT32_MAX) ||
	    (sys_get_be32(&record[RECORD_CRC_OFFSET]) != crc32_ieee(record, RECORD_CRC_OFFSET))) {
		return -EBADMSG;
	}
	return 0;
}
#endif

int kfsw_boot_record_image(const uint8_t image[KFSW_BOOT_IMAGE_ID_SIZE], bool confirmed,
			   bool revert_pending)
{
	int result = -ENOTSUP;

	k_mutex_lock(&boot_trial_lock, K_FOREVER);
	diagnostics = (struct kfsw_boot_diagnostics){
		.attempts = UINT32_MAX,
		.revert_reason = UINT8_MAX,
	};
#if CONFIG_KFSW_STORAGE
	uint8_t record[RECORD_SIZE] = {0};

	if (image == NULL) {
		result = -EINVAL;
		goto out;
	}
	if (!kfsw_storage_is_ready()) {
		result = -ENODEV;
		goto out;
	}
	result = load_record(record);
	if (result == -ENOENT) {
		memset(record, 0, sizeof(record));
		memcpy(record, "KBT1", RECORD_MAGIC_SIZE);
	} else if (result != 0) {
		/* Keep rejected evidence rather than silently starting at one. */
		goto out;
	}
	if ((record[RECORD_TRIAL_OFFSET] != 0U) &&
	    (memcmp(&record[RECORD_IMAGE_OFFSET], image, KFSW_BOOT_IMAGE_ID_SIZE) != 0)) {
		record[RECORD_REASON_OFFSET] = record[RECORD_REVERT_OFFSET] != 0U
						       ? KFSW_BOOT_REVERT_UNCONFIRMED_REPLACED
						       : KFSW_BOOT_REVERT_REPLACED_UNKNOWN;
		memcpy(&record[RECORD_PREVIOUS_IMAGE_OFFSET], &record[RECORD_IMAGE_OFFSET],
		       KFSW_BOOT_IMAGE_ID_SIZE);
	}
	uint32_t attempts = sys_get_be32(&record[RECORD_ATTEMPTS_OFFSET]);

	if (confirmed ||
	    (memcmp(&record[RECORD_IMAGE_OFFSET], image, KFSW_BOOT_IMAGE_ID_SIZE) != 0)) {
		attempts = 0U;
	}
	if (!confirmed && (attempts < UINT32_MAX - 1U)) {
		attempts++;
	}
	memcpy(&record[RECORD_IMAGE_OFFSET], image, KFSW_BOOT_IMAGE_ID_SIZE);
	sys_put_be32(attempts, &record[RECORD_ATTEMPTS_OFFSET]);
	record[RECORD_TRIAL_OFFSET] = confirmed ? 0U : 1U;
	/* Revert pending is observed in the trial image, not retained by MCUboot. */
	record[RECORD_REVERT_OFFSET] = revert_pending ? 1U : 0U;
	sys_put_be32(crc32_ieee(record, RECORD_CRC_OFFSET), &record[RECORD_CRC_OFFSET]);
	result = kfsw_snapshot_write(RECORD_PATH, RECORD_TEMP_PATH, record, sizeof(record));
	if (result == 0) {
		diagnostics.attempts = attempts;
		diagnostics.revert_reason = record[RECORD_REASON_OFFSET];
		diagnostics.valid = true;
	}
out:
#else
	ARG_UNUSED(image);
	ARG_UNUSED(confirmed);
	ARG_UNUSED(revert_pending);
#endif
	k_mutex_unlock(&boot_trial_lock);
	return result;
}

#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
/* The signed image's SHA256 TLV identifies content even when versions repeat. */
static int running_image_id(uint8_t *image)
{
	const struct flash_area *area;
	struct image_header header;
	struct image_tlv_info info;
	uint32_t offset;
	uint32_t end;
	int result = flash_area_open(boot_fetch_active_slot(), &area);

	if (result != 0) {
		return result;
	}
	result = flash_area_read(area, 0U, &header, sizeof(header));
	if (result != 0) {
		goto out;
	}
	uint32_t header_size = sys_le16_to_cpu(header.ih_hdr_size);
	uint32_t body_size = sys_le32_to_cpu(header.ih_img_size);
	uint32_t protected_size = sys_le16_to_cpu(header.ih_protect_tlv_size);

	result = -EBADMSG;
	if ((sys_le32_to_cpu(header.ih_magic) != IMAGE_MAGIC) || (header_size > area->fa_size) ||
	    (body_size > area->fa_size - header_size) ||
	    (protected_size > area->fa_size - header_size - body_size)) {
		goto out;
	}
	offset = header_size + body_size + protected_size;
	if (area->fa_size - offset < sizeof(info)) {
		goto out;
	}
	result = flash_area_read(area, offset, &info, sizeof(info));
	if (result != 0) {
		goto out;
	}
	uint32_t length = sys_le16_to_cpu(info.it_tlv_tot);

	result = -EBADMSG;
	if ((sys_le16_to_cpu(info.it_magic) != IMAGE_TLV_INFO_MAGIC) || (length < sizeof(info)) ||
	    (length > area->fa_size - offset)) {
		goto out;
	}
	end = offset + length;
	offset += sizeof(info);
	while (end - offset >= sizeof(struct image_tlv)) {
		struct image_tlv tlv;

		result = flash_area_read(area, offset, &tlv, sizeof(tlv));
		if (result != 0) {
			goto out;
		}
		offset += sizeof(tlv);
		length = sys_le16_to_cpu(tlv.it_len);
		result = -EBADMSG;
		if (length > end - offset) {
			goto out;
		}
		if ((tlv.it_type == IMAGE_TLV_SHA256) && (length == KFSW_BOOT_IMAGE_ID_SIZE)) {
			result = flash_area_read(area, offset, image, length);
			goto out;
		}
		offset += length;
	}
	result = -ENOENT;
out:
	flash_area_close(area);
	return result;
}
#endif

int kfsw_boot_diagnostics_start(void)
{
#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
	uint8_t image[KFSW_BOOT_IMAGE_ID_SIZE];
	int result = running_image_id(image);

	if (result != 0) {
		k_mutex_lock(&boot_trial_lock, K_FOREVER);
		diagnostics = (struct kfsw_boot_diagnostics){
			.attempts = UINT32_MAX,
			.revert_reason = UINT8_MAX,
		};
		k_mutex_unlock(&boot_trial_lock);
		return result;
	}
	return kfsw_boot_record_image(image, boot_is_img_confirmed(),
				      mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT);
#else
	return -ENOTSUP;
#endif
}

int kfsw_boot_confirm_image(void)
{
#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
	int result = boot_write_img_confirmed();

	if (result != 0) {
		kfsw_log_error("Image confirmation failed: %d", result);
		return result;
	}
	/* Re-read confirmation; a failed snapshot is retried at the next startup. */
	result = kfsw_boot_diagnostics_start();
	if (result != 0) {
		kfsw_log_error("Confirmed image but trial diagnostics save failed: %d", result);
	}
	return result;
#else
	return -ENOTSUP;
#endif
}
