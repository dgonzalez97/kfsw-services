#include <errno.h>
#include <psa/crypto.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/storage/stream_flash.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#if CONFIG_KFSW_FWU_MCUBOOT
#include <zephyr/dfu/mcuboot.h>
#endif

#include <kfsw/services/fwu.h>
#if CONFIG_KFSW_BOOT_STORE
#include <kfsw/services/boot_budget.h>
#endif
#include "fwu_internal.h"
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_FWU
#include <kfsw/services/log.h>

#define KFSW_FWU_PARTITION_NODE DT_CHOSEN(kfsw_fwu_partition)
#define KFSW_FWU_PARTITION_PRESENT DT_NODE_EXISTS(KFSW_FWU_PARTITION_NODE)

#if KFSW_FWU_PARTITION_PRESENT
#define KFSW_FWU_PARTITION_ID DT_FIXED_PARTITION_ID(KFSW_FWU_PARTITION_NODE)
#define KFSW_FWU_PARTITION_SIZE DT_REG_SIZE(KFSW_FWU_PARTITION_NODE)
#endif

#if CONFIG_KFSW_FWU_RECOVERY
BUILD_ASSERT(KFSW_FWU_PARTITION_PRESENT, "Recovery requires a target partition");
BUILD_ASSERT(DT_SAME_NODE(KFSW_FWU_PARTITION_NODE, DT_NODELABEL(slot0_partition)),
	     "Recovery must target primary A");
BUILD_ASSERT(DT_SAME_NODE(DT_CHOSEN(zephyr_code_partition), DT_NODELABEL(slot1_partition)),
	     "Recovery must execute from permanent Golden");
BUILD_ASSERT(DT_SAME_NODE(DT_MTD_FROM_FIXED_PARTITION(KFSW_FWU_PARTITION_NODE),
			  DT_MTD_FROM_FIXED_PARTITION(DT_CHOSEN(zephyr_code_partition))),
	     "Recovery partitions must share flash");
BUILD_ASSERT(DT_REG_ADDR(KFSW_FWU_PARTITION_NODE) + DT_REG_SIZE(KFSW_FWU_PARTITION_NODE) <=
			     DT_REG_ADDR(DT_CHOSEN(zephyr_code_partition)) ||
		     DT_REG_ADDR(DT_CHOSEN(zephyr_code_partition)) +
				     DT_REG_SIZE(DT_CHOSEN(zephyr_code_partition)) <=
			     DT_REG_ADDR(KFSW_FWU_PARTITION_NODE),
	     "Recovery code and target must not overlap");
BUILD_ASSERT(CONFIG_KFSW_FWU_SLOT_OFFSET_SECTORS == 0, "Recovery must not reserve a swap offset");
BUILD_ASSERT(!IS_ENABLED(CONFIG_KFSW_FWU_MCUBOOT) && !IS_ENABLED(CONFIG_KFSW_GOLDEN) &&
		     !IS_ENABLED(CONFIG_KFSW_SCRUB) && !IS_ENABLED(CONFIG_KFSW_FWU_FILES),
	     "Recovery cannot use secondary-slot services");
BUILD_ASSERT(IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT) ||
		     (IS_ENABLED(CONFIG_ZTEST) && IS_ENABLED(CONFIG_ARCH_POSIX)),
	     "Recovery requires MCUboot outside native tests");
#if CONFIG_BOOTLOADER_MCUBOOT
BUILD_ASSERT(IS_ENABLED(CONFIG_MCUBOOT_BOOTLOADER_MODE_FIRMWARE_UPDATER) &&
		     IS_ENABLED(CONFIG_MCUBOOT_APPLICATION_FIRMWARE_UPDATER),
	     "Recovery requires MCUboot firmware updater mode");
#endif
#endif

/* Swap mode reserves the last sector for the bootloader trailer. */
#define KFSW_FWU_TRAILER_SECTORS (IS_ENABLED(CONFIG_KFSW_FWU_RECOVERY) ? 0U : 1U)

/* Serializes session lifetime with flash I/O so no owner can replace an active writer. */
K_MUTEX_DEFINE(fwu_lock);
static struct kfsw_fwu_status fwu_state = {
	.state = KFSW_FWU_IDLE,
	.target_bound = KFSW_FWU_PARTITION_PRESENT,
};
#if CONFIG_KFSW_FWU_LITE
/* Protected by fwu_lock with the canonical writer and generation. */
static uint8_t last_block[CONFIG_KFSW_FWU_LITE_BLOCK_SIZE];
static uint32_t last_offset;
static size_t last_size;
#endif
static unsigned int secondary_readers;
static uint32_t generation;
static struct kfsw_fwu_handle local_handle;
static const struct kfsw_fwu_owner local_owner = {.kind = KFSW_FWU_OWNER_LOCAL};

static bool secondary_needed_for_revert(void)
{
#if CONFIG_KFSW_FWU_MCUBOOT
	return mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT;
#else
	return false;
#endif
}

int kfsw_fwu_secondary_acquire(void)
{
	k_mutex_lock(&fwu_lock, K_FOREVER);
	if ((fwu_state.state == KFSW_FWU_RECEIVING) || (fwu_state.state == KFSW_FWU_FAILED)) {
		k_mutex_unlock(&fwu_lock);
		return -EBUSY;
	}
	secondary_readers++;
	k_mutex_unlock(&fwu_lock);
	return 0;
}

void kfsw_fwu_secondary_release(void)
{
	k_mutex_lock(&fwu_lock, K_FOREVER);
	__ASSERT_NO_MSG(secondary_readers > 0U);
	secondary_readers--;
	k_mutex_unlock(&fwu_lock);
}

#if KFSW_FWU_PARTITION_PRESENT
static uint8_t fwu_stream_buffer[CONFIG_KFSW_FWU_STREAM_BUFFER_SIZE];
static struct stream_flash_ctx fwu_stream;
static const struct flash_area *fwu_area;

/* Uses its own flash area handle instead of the shared one. */
static int fwu_sector_size(void)
{
	const struct flash_area *area;
	const struct device *device;
	struct flash_pages_info page;
	int result;

	result = flash_area_open(KFSW_FWU_PARTITION_ID, &area);
	if (result != 0) {
		return result;
	}

	device = flash_area_get_device(area);
	if (device == NULL) {
		flash_area_close(area);
		return -ENODEV;
	}

	result = flash_get_page_info_by_offs(device, area->fa_off, &page);
	flash_area_close(area);
	if (result != 0) {
		return result;
	}

	return (int)page.size;
}
#endif /* KFSW_FWU_PARTITION_PRESENT */

uint32_t kfsw_fwu_slot_write_offset(void)
{
#if KFSW_FWU_PARTITION_PRESENT && CONFIG_KFSW_FWU_SLOT_OFFSET_SECTORS > 0
	int sector = fwu_sector_size();

	if (sector <= 0) {
		return 0U;
	}

	return (uint32_t)sector * CONFIG_KFSW_FWU_SLOT_OFFSET_SECTORS;
#else
	return 0U;
#endif
}

uint32_t kfsw_fwu_max_image_size(void)
{
#if KFSW_FWU_PARTITION_PRESENT
	uint32_t offset = kfsw_fwu_slot_write_offset();
	uint32_t trailer;
	int sector = fwu_sector_size();

	if (sector <= 0) {
		return 0U;
	}

	trailer = (uint32_t)sector * KFSW_FWU_TRAILER_SECTORS;
	if ((offset + trailer) >= KFSW_FWU_PARTITION_SIZE) {
		return 0U;
	}

	return KFSW_FWU_PARTITION_SIZE - offset - trailer;
#else
	return 0U;
#endif
}

#if KFSW_FWU_PARTITION_PRESENT
/* Caller holds fwu_lock. */
static void fwu_close_stream(void)
{
	if (fwu_area != NULL) {
		flash_area_close(fwu_area);
		fwu_area = NULL;
	}
}

/* Caller holds fwu_lock. Leaves the slot erased. */
static int fwu_erase_slot(void)
{
	int result;

	result = flash_area_open(KFSW_FWU_PARTITION_ID, &fwu_area);
	if (result != 0) {
		return result;
	}

	result = flash_area_flatten(fwu_area, 0, KFSW_FWU_PARTITION_SIZE);
	if (result == 0) {
		fwu_state.committed = 0U;
	}
	flash_area_close(fwu_area);
	fwu_area = NULL;

	return result;
}
#endif /* KFSW_FWU_PARTITION_PRESENT */

static int fwu_begin_locked(uint32_t total_size, uint32_t expected_crc32)
{
#if KFSW_FWU_PARTITION_PRESENT
	uint32_t maximum;
	uint32_t write_offset;
	int result;

	maximum = kfsw_fwu_max_image_size();

	result = fwu_erase_slot();
	if (result != 0) {
		/* Log why the image was refused. */
		kfsw_log_error("Firmware update: could not erase the slot (%d)", result);
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return result;
	}

	write_offset = kfsw_fwu_slot_write_offset();

	result = flash_area_open(KFSW_FWU_PARTITION_ID, &fwu_area);
	if (result != 0) {
		kfsw_log_error("Firmware update: could not open the slot (%d)", result);
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return result;
	}

	/* The stream writer gets the whole region, which is a multiple of the write
	 * block size; the image size limit is checked here instead.
	 */
	result = stream_flash_init(&fwu_stream, flash_area_get_device(fwu_area), fwu_stream_buffer,
				   sizeof(fwu_stream_buffer), fwu_area->fa_off + write_offset,
				   maximum, NULL);
	if (result != 0) {
		kfsw_log_error("Firmware update: could not prepare the slot for writing (%d)",
			       result);
		fwu_close_stream();
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return result;
	}

	kfsw_log_debug("Firmware update slot ready: offset %u, %u bytes usable", write_offset,
		       maximum);
	fwu_state.state = KFSW_FWU_RECEIVING;
	fwu_state.started++;

	kfsw_log_info("Firmware update started: %u bytes, crc32=%08x", total_size, expected_crc32);
	return 0;
#else
	ARG_UNUSED(total_size);
	ARG_UNUSED(expected_crc32);
	return -ENODEV;
#endif
}

static int fwu_write_locked(uint32_t offset, const void *data, size_t size)
{
#if KFSW_FWU_PARTITION_PRESENT
	int result;

	if ((data == NULL) || (size == 0U)) {
		return -EINVAL;
	}

	if (fwu_state.state != KFSW_FWU_RECEIVING) {
		return -EINVAL;
	}
	if (offset != fwu_state.received) {
		return -ESPIPE;
	}
	if (size > fwu_state.total_size - offset) {
		return -EFBIG;
	}

	result = stream_flash_buffered_write(&fwu_stream, data, size, false);
	fwu_state.committed = MIN(fwu_stream.bytes_written, fwu_state.total_size);
	if (result != 0) {
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return result;
	}

	fwu_state.actual_crc32 = crc32_ieee_update(fwu_state.actual_crc32, data, size);
	fwu_state.received += size;

	return 0;
#else
	ARG_UNUSED(offset);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -ENODEV;
#endif
}

#if KFSW_FWU_PARTITION_PRESENT
/* Caller holds fwu_lock. Reads only image bytes, excluding stream padding. */
static int fwu_check_stored_image(void)
{
	uint8_t chunk[128];
	uint8_t digest[KFSW_FWU_SHA256_SIZE];
	size_t digest_size;
	psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
	uint32_t write_offset = kfsw_fwu_slot_write_offset();
	uint32_t crc = 0U;
	uint32_t offset = 0U;
	int result;

	if (fwu_state.image.digest_present &&
	    (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS)) {
		return -EIO;
	}
	while (offset < fwu_state.total_size) {
		size_t size = MIN(sizeof(chunk), fwu_state.total_size - offset);

		result = flash_area_read(fwu_area, write_offset + offset, chunk, size);
		if (result != 0) {
			(void)psa_hash_abort(&hash);
			return result;
		}
		if (fwu_state.image.digest_present &&
		    (psa_hash_update(&hash, chunk, size) != PSA_SUCCESS)) {
			(void)psa_hash_abort(&hash);
			return -EIO;
		}
		crc = crc32_ieee_update(crc, chunk, size);
		offset += size;
	}
	if (fwu_state.image.digest_present) {
		psa_status_t result = psa_hash_finish(&hash, digest, sizeof(digest), &digest_size);

		(void)psa_hash_abort(&hash);
		if (result != PSA_SUCCESS) {
			return -EIO;
		}
		if ((digest_size != sizeof(digest)) ||
		    (memcmp(digest, fwu_state.image.sha256, sizeof(digest)) != 0)) {
			return -EILSEQ;
		}
	}
	return crc == fwu_state.expected_crc32 ? 0 : -EILSEQ;
}

/* Caller holds fwu_lock. Verification makes buffered bytes readable without
 * scheduling an upgrade; both upload protocols use this boundary.
 */
static int fwu_verify_locked(void)
{
	int result;

	if ((fwu_state.state == KFSW_FWU_VERIFIED) || (fwu_state.state == KFSW_FWU_READY)) {
		return 0;
	}
	if (fwu_state.state != KFSW_FWU_RECEIVING) {
		return -EINVAL;
	}

	if (fwu_state.received != fwu_state.total_size) {
		return -EAGAIN;
	}

	if (fwu_state.actual_crc32 != fwu_state.expected_crc32) {
		kfsw_log_error("Firmware update rejected: crc32 %08x, expected %08x",
			       fwu_state.actual_crc32, fwu_state.expected_crc32);
		fwu_close_stream();
		result = fwu_erase_slot();
		if (result != 0) {
			kfsw_log_error("Firmware update cleanup failed (%d)", result);
		}
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return -EILSEQ;
	}

	/* Flush whatever is still buffered before anything reads the slot. */
	result = stream_flash_buffered_write(&fwu_stream, NULL, 0, true);
	fwu_state.committed = MIN(fwu_stream.bytes_written, fwu_state.total_size);
	if (result == 0) {
		result = fwu_check_stored_image();
	}
	if (result != 0) {
		fwu_close_stream();
		if (result == -EILSEQ) {
			int cleanup = fwu_erase_slot();

			if (cleanup != 0) {
				kfsw_log_error("Firmware update cleanup failed (%d)", cleanup);
			}
		}
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return result;
	}
	fwu_close_stream();
	fwu_state.committed = fwu_state.total_size;
	fwu_state.state = KFSW_FWU_VERIFIED;
	return 0;
}
#else
static int fwu_verify_locked(void)
{
	return -ENODEV;
}
#endif

static int fwu_finish_locked(void)
{
#if KFSW_FWU_PARTITION_PRESENT
	int result;

	if (fwu_state.state == KFSW_FWU_READY) {
		return 0;
	}
	if ((secondary_readers != 0U) || secondary_needed_for_revert()) {
		return -EBUSY;
	}
	result = fwu_verify_locked();
	if (result != 0) {
		return result;
	}

#if CONFIG_KFSW_FWU_MCUBOOT
	result = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (result != 0) {
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return result;
	}

	/* Check that the bootloader scheduled the swap; otherwise it boots the old image. */
	if (mcuboot_swap_type() != BOOT_SWAP_TYPE_TEST) {
		kfsw_log_error("Firmware update: no swap was scheduled");
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		return -EIO;
	}
	fwu_state.swap_scheduled = true;
#endif

	fwu_state.state = KFSW_FWU_READY;
	fwu_state.completed++;

#if CONFIG_KFSW_BOOT_STORE
	result = kfsw_boot_budget_refill();
	if (result != 0) {
		/* The installed image remains usable even if the budget store fails. */
		kfsw_log_error("Firmware installed but boot budget refill failed (%d)", result);
	}
#endif

	kfsw_log_info("Firmware update ready: %u bytes accepted", fwu_state.total_size);
	return 0;
#else
	return -ENODEV;
#endif
}

static int fwu_abort_locked(void)
{
	if ((secondary_readers != 0U) || secondary_needed_for_revert()) {
		return -EBUSY;
	}

#if KFSW_FWU_PARTITION_PRESENT
	int result;

	if (fwu_state.state == KFSW_FWU_RECEIVING) {
		fwu_state.failed++;
	}
	fwu_close_stream();
	result = fwu_erase_slot();
	if (result != 0) {
		fwu_state.state = KFSW_FWU_FAILED;
		return result;
	}
#endif

	fwu_state.aborted = true;
	fwu_state.committed = 0U;
	fwu_state.total_size = 0U;
	fwu_state.received = 0U;
	fwu_state.expected_crc32 = 0U;
	fwu_state.actual_crc32 = 0U;
	fwu_state.swap_scheduled = false;
	fwu_state.state = KFSW_FWU_IDLE;

	return 0;
}

static bool owner_equal(const struct kfsw_fwu_owner *a, const struct kfsw_fwu_owner *b)
{
	return (a->kind == b->kind) && (a->peer == b->peer);
}

static bool image_equal(const struct kfsw_fwu_image *a, const struct kfsw_fwu_image *b)
{
	return a->digest_present && b->digest_present && (a->size == b->size) &&
	       (a->crc32 == b->crc32) && (a->block_size == b->block_size) &&
	       (memcmp(a->sha256, b->sha256, sizeof(a->sha256)) == 0);
}

/* Identity checks and the operation share fwu_lock, including local adapters. */
static int fwu_check_handle(const struct kfsw_fwu_handle *handle)
{
	if (!owner_equal(&handle->owner, &fwu_state.handle.owner)) {
		return -EACCES;
	}
	if ((handle->generation == 0U) || (handle->generation != fwu_state.handle.generation) ||
	    (handle->transfer_id != fwu_state.handle.transfer_id)) {
		return -ESTALE;
	}
	return 0;
}

/* Snapshot is copied before dropping the lock, including failed operations. */
static int fwu_snapshot_unlock(int result, struct kfsw_fwu_status *status)
{
	*status = fwu_state;
	k_mutex_unlock(&fwu_lock);
	return result;
}

static int fwu_session_begin_locked(const struct kfsw_fwu_owner *owner,
				    const struct kfsw_fwu_image *image, uint32_t transfer_id,
				    struct kfsw_fwu_handle *handle)
{
	if ((owner->kind < KFSW_FWU_OWNER_LOCAL) || (owner->kind > KFSW_FWU_OWNER_LITE) ||
	    (image->size == 0U) ||
	    (image->digest_present && ((owner->kind != KFSW_FWU_OWNER_LITE) ||
				       (transfer_id == 0U) || (image->block_size == 0U)))) {
		return -EINVAL;
	}
	if (!fwu_state.target_bound) {
		return -ENODEV;
	}
	if ((fwu_state.state != KFSW_FWU_IDLE) &&
	    !((fwu_state.state == KFSW_FWU_READY) && !fwu_state.image.digest_present &&
	      !image->digest_present && owner_equal(owner, &fwu_state.handle.owner))) {
		if ((fwu_state.state != KFSW_FWU_FAILED) &&
		    owner_equal(owner, &fwu_state.handle.owner) &&
		    (transfer_id == fwu_state.handle.transfer_id) &&
		    image_equal(image, &fwu_state.image)) {
			*handle = fwu_state.handle;
			return 0;
		}
		return -EBUSY;
	}
	if (fwu_state.aborted && (transfer_id != 0U) &&
	    owner_equal(owner, &fwu_state.handle.owner) &&
	    (transfer_id == fwu_state.handle.transfer_id)) {
		return -ESTALE;
	}
	if (image->size > kfsw_fwu_max_image_size()) {
		return -EFBIG;
	}
	if ((secondary_readers != 0U) || secondary_needed_for_revert()) {
		return -EBUSY;
	}
	if (generation == UINT32_MAX) {
		return -EOVERFLOW;
	}
	generation++;
#if CONFIG_KFSW_FWU_LITE
	last_size = 0U;
#endif
	fwu_state.handle.owner = *owner;
	fwu_state.handle.transfer_id = transfer_id != 0U ? transfer_id : generation;
	fwu_state.handle.generation = generation;
	fwu_state.image = *image;
	fwu_state.aborted = false;
	fwu_state.committed = 0U;
	fwu_state.total_size = image->size;
	fwu_state.expected_crc32 = image->crc32;
	fwu_state.received = 0U;
	fwu_state.actual_crc32 = 0U;
	fwu_state.swap_scheduled = false;
	*handle = fwu_state.handle;
	return fwu_begin_locked(image->size, image->crc32);
}

int kfsw_fwu_session_begin(const struct kfsw_fwu_owner *owner, const struct kfsw_fwu_image *image,
			   uint32_t transfer_id, struct kfsw_fwu_handle *handle,
			   struct kfsw_fwu_status *status)
{
	int result;

	if ((owner == NULL) || (image == NULL) || (handle == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_session_begin_locked(owner, image, transfer_id, handle);
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_session_write(const struct kfsw_fwu_handle *handle, uint32_t offset, const void *data,
			   size_t size, struct kfsw_fwu_status *status)
{
	int result;

	if ((handle == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_handle(handle);
#if CONFIG_KFSW_FWU_LITE
	if ((result == 0) && fwu_state.image.digest_present) {
		if (fwu_state.state != KFSW_FWU_RECEIVING || fwu_state.aborted) {
			return fwu_snapshot_unlock(-EINVAL, status);
		}
		if ((data == NULL) || (size == 0U) || (size > sizeof(last_block)) ||
		    (size > fwu_state.image.size) || (offset > fwu_state.image.size - size) ||
		    ((size != fwu_state.image.block_size) &&
		     (offset + size != fwu_state.image.size))) {
			return fwu_snapshot_unlock(-EINVAL, status);
		}
		if ((last_size != 0U) && (offset == last_offset)) {
			result = ((size == last_size) && (memcmp(data, last_block, size) == 0))
					 ? 0
					 : -EILSEQ;
			return fwu_snapshot_unlock(result, status);
		}
	}
#endif
	if (result == 0) {
		result = fwu_write_locked(offset, data, size);
#if CONFIG_KFSW_FWU_LITE
		if ((result == 0) && fwu_state.image.digest_present) {
			memcpy(last_block, data, size);
			last_size = size;
			last_offset = offset;
		}
#endif
	}
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_session_verify(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status)
{
	int result;

	if ((handle == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_handle(handle);
	if (result == 0) {
		result = fwu_verify_locked();
	}
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_session_activate(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status)
{
	int result;

	if ((handle == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_handle(handle);
	if (result == 0) {
		if ((fwu_state.state != KFSW_FWU_VERIFIED) && (fwu_state.state != KFSW_FWU_READY)) {
			result = -EINVAL;
		} else {
			result = fwu_finish_locked();
		}
	}
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_session_abort(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status)
{
	int result;

	if ((handle == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_handle(handle);
	if ((result == 0) && !fwu_state.aborted) {
		result = fwu_abort_locked();
	}
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_session_lookup(const struct kfsw_fwu_owner *owner, struct kfsw_fwu_status *status)
{
	int result;

	if ((owner == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_state.handle.generation == 0U || fwu_state.aborted ? -ENOENT
		 : owner_equal(owner, &fwu_state.handle.owner)          ? 0
									: -EACCES;
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_session_status(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status)
{
	int result;

	if ((handle == NULL) || (status == NULL)) {
		return -EINVAL;
	}
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_handle(handle);
	return fwu_snapshot_unlock(result, status);
}

int kfsw_fwu_begin(uint32_t total_size, uint32_t expected_crc32)
{
	const struct kfsw_fwu_image image = {.size = total_size, .crc32 = expected_crc32};
	struct kfsw_fwu_handle handle = {0};
	int result;

	/* The local handle must be installed before a concurrent local operation. */
	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_session_begin_locked(&local_owner, &image, 0U, &handle);
	if (handle.generation != 0U) {
		local_handle = handle;
	}
	k_mutex_unlock(&fwu_lock);
	return result;
}

static int fwu_check_local(void)
{
	if (!owner_equal(&local_owner, &fwu_state.handle.owner)) {
		return -EACCES;
	}
	if (fwu_state.state == KFSW_FWU_IDLE) {
		return -EINVAL;
	}
	return fwu_check_handle(&local_handle);
}

int kfsw_fwu_write(uint32_t offset, const void *data, size_t size)
{
	int result;

	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_local();
	if (result == 0) {
		result = fwu_write_locked(offset, data, size);
	}
	k_mutex_unlock(&fwu_lock);
	return result;
}

int kfsw_fwu_verify(void)
{
	int result;

	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_local();
	if (result == 0) {
		result = fwu_verify_locked();
	}
	k_mutex_unlock(&fwu_lock);
	return result;
}

int kfsw_fwu_finish(void)
{
	int result;

	k_mutex_lock(&fwu_lock, K_FOREVER);
	result = fwu_check_local();
	if (result == 0) {
		result = fwu_finish_locked();
	}
	k_mutex_unlock(&fwu_lock);
	return result;
}

int kfsw_fwu_abort(void)
{
	int result;

	k_mutex_lock(&fwu_lock, K_FOREVER);
	if (!owner_equal(&local_owner, &fwu_state.handle.owner)) {
		result = -EACCES;
	} else if (fwu_state.aborted) {
		result = 0;
	} else {
		result = fwu_state.handle.generation == 0U ? 0 : fwu_check_handle(&local_handle);
		if (result == 0) {
			result = fwu_abort_locked();
		}
	}
	k_mutex_unlock(&fwu_lock);
	return result;
}

int kfsw_fwu_get_status(struct kfsw_fwu_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&fwu_lock, K_FOREVER);
	*status = fwu_state;
	k_mutex_unlock(&fwu_lock);

	return 0;
}

const char *kfsw_fwu_state_name(enum kfsw_fwu_state state)
{
	switch (state) {
	case KFSW_FWU_IDLE:
		return "idle";
	case KFSW_FWU_RECEIVING:
		return "receiving";
	case KFSW_FWU_READY:
		return "ready";
	case KFSW_FWU_FAILED:
		return "failed";
	case KFSW_FWU_VERIFIED:
		return "verified";
	default:
		return "unknown";
	}
}
