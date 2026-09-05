#include <errno.h>
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
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_FWU
#include <kfsw/services/log.h>

#define KFSW_FWU_PARTITION_NODE DT_CHOSEN(kfsw_fwu_partition)
#define KFSW_FWU_PARTITION_PRESENT DT_NODE_EXISTS(KFSW_FWU_PARTITION_NODE)

#if KFSW_FWU_PARTITION_PRESENT
#define KFSW_FWU_PARTITION_ID DT_FIXED_PARTITION_ID(KFSW_FWU_PARTITION_NODE)
#define KFSW_FWU_PARTITION_SIZE DT_REG_SIZE(KFSW_FWU_PARTITION_NODE)
#endif

/* The bootloader keeps its own trailer at the end of the slot. Reserving a
 * sector for it stops a maximum-sized image from overwriting the metadata that
 * says the image is there.
 */
#define KFSW_FWU_TRAILER_SECTORS 1U

K_MUTEX_DEFINE(fwu_lock);
static struct kfsw_fwu_status fwu_state = {
	.state = KFSW_FWU_IDLE,
	.target_bound = KFSW_FWU_PARTITION_PRESENT,
};

#if KFSW_FWU_PARTITION_PRESENT
static uint8_t fwu_stream_buffer[CONFIG_KFSW_FWU_STREAM_BUFFER_SIZE];
static struct stream_flash_ctx fwu_stream;
static const struct flash_area *fwu_area;
static bool fwu_stream_open;

/* Uses its own handle rather than the shared one. An earlier version borrowed
 * the transfer's area pointer and closed it, which set the shared pointer to
 * NULL underneath an expression that was still about to dereference it.
 */
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
	fwu_stream_open = false;
}

/* Caller holds fwu_lock. Leaves the slot erased so that a partial image can
 * never be mistaken for a complete one.
 */
static int fwu_erase_slot(void)
{
	int result;

	result = flash_area_open(KFSW_FWU_PARTITION_ID, &fwu_area);
	if (result != 0) {
		return result;
	}

	result = flash_area_flatten(fwu_area, 0, KFSW_FWU_PARTITION_SIZE);
	flash_area_close(fwu_area);
	fwu_area = NULL;

	return result;
}
#endif /* KFSW_FWU_PARTITION_PRESENT */

int kfsw_fwu_begin(uint32_t total_size, uint32_t expected_crc32)
{
#if KFSW_FWU_PARTITION_PRESENT
	uint32_t maximum;
	uint32_t write_offset;
	int result;

	if (total_size == 0U) {
		return -EINVAL;
	}

	maximum = kfsw_fwu_max_image_size();
	if (total_size > maximum) {
		kfsw_log_error("Firmware update: %u bytes exceeds the %u the slot holds",
			       total_size, maximum);
		return -EFBIG;
	}

	k_mutex_lock(&fwu_lock, K_FOREVER);

	if (fwu_state.state == KFSW_FWU_RECEIVING) {
		k_mutex_unlock(&fwu_lock);
		return -EBUSY;
	}

	result = fwu_erase_slot();
	if (result != 0) {
		/* A refusal that says nothing leaves an operator with a failed
		 * upload and no way to tell a full slot from a broken one. */
		kfsw_log_error("Firmware update: could not erase the slot (%d)", result);
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return result;
	}

	write_offset = kfsw_fwu_slot_write_offset();

	result = flash_area_open(KFSW_FWU_PARTITION_ID, &fwu_area);
	if (result != 0) {
		kfsw_log_error("Firmware update: could not open the slot (%d)", result);
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return result;
	}

	/* The region handed to the streaming writer is the space it may use, not
	 * the length of this image. It must be a whole number of write blocks,
	 * and an image is whatever length the linker produced: passing the image
	 * length rejects every image whose byte count is not a multiple of the
	 * write block, for a reason that has nothing to do with the image.
	 *
	 * How much of the region this image occupies is tracked here, and a
	 * write past its declared size is refused, so the bound does not depend
	 * on the region being tight.
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
		k_mutex_unlock(&fwu_lock);
		return result;
	}

	fwu_stream_open = true;
	kfsw_log_debug("Firmware update slot ready: offset %u, %u bytes usable", write_offset,
		       maximum);
	fwu_state.total_size = total_size;
	fwu_state.expected_crc32 = expected_crc32;
	fwu_state.received = 0U;
	fwu_state.actual_crc32 = 0U;
	fwu_state.swap_scheduled = false;
	fwu_state.state = KFSW_FWU_RECEIVING;
	fwu_state.started++;

	k_mutex_unlock(&fwu_lock);

	kfsw_log_info("Firmware update started: %u bytes, crc32=%08x", total_size, expected_crc32);
	return 0;
#else
	ARG_UNUSED(total_size);
	ARG_UNUSED(expected_crc32);
	return -ENODEV;
#endif
}

int kfsw_fwu_write(uint32_t offset, const void *data, size_t size)
{
#if KFSW_FWU_PARTITION_PRESENT
	int result;

	if ((data == NULL) || (size == 0U)) {
		return -EINVAL;
	}

	k_mutex_lock(&fwu_lock, K_FOREVER);

	if (fwu_state.state != KFSW_FWU_RECEIVING) {
		k_mutex_unlock(&fwu_lock);
		return -EINVAL;
	}
	if (offset != fwu_state.received) {
		k_mutex_unlock(&fwu_lock);
		return -ESPIPE;
	}
	if ((uint64_t)offset + size > fwu_state.total_size) {
		k_mutex_unlock(&fwu_lock);
		return -EFBIG;
	}

	result = stream_flash_buffered_write(&fwu_stream, data, size, false);
	if (result != 0) {
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return result;
	}

	fwu_state.actual_crc32 = crc32_ieee_update(fwu_state.actual_crc32, data, size);
	fwu_state.received += size;

	k_mutex_unlock(&fwu_lock);
	return 0;
#else
	ARG_UNUSED(offset);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -ENODEV;
#endif
}

int kfsw_fwu_finish(void)
{
#if KFSW_FWU_PARTITION_PRESENT
	int result;

	k_mutex_lock(&fwu_lock, K_FOREVER);

	if (fwu_state.state != KFSW_FWU_RECEIVING) {
		k_mutex_unlock(&fwu_lock);
		return -EINVAL;
	}

	if (fwu_state.received != fwu_state.total_size) {
		k_mutex_unlock(&fwu_lock);
		return -EAGAIN;
	}

	if (fwu_state.actual_crc32 != fwu_state.expected_crc32) {
		kfsw_log_error("Firmware update rejected: crc32 %08x, expected %08x",
			       fwu_state.actual_crc32, fwu_state.expected_crc32);
		(void)fwu_erase_slot();
		fwu_close_stream();
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return -EILSEQ;
	}

	/* Flush whatever is still buffered before anything reads the slot. */
	result = stream_flash_buffered_write(&fwu_stream, NULL, 0, true);
	if (result != 0) {
		fwu_close_stream();
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return result;
	}
	fwu_close_stream();

#if CONFIG_KFSW_FWU_MCUBOOT
	result = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (result != 0) {
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return result;
	}

	/* Asking is not the same as being heard. An image written to the wrong
	 * offset leaves the bootloader with nothing to swap, and it says so
	 * only by quietly running the old image on the next boot -- which from
	 * the ground looks exactly like a successful update.
	 */
	if (mcuboot_swap_type() != BOOT_SWAP_TYPE_TEST) {
		kfsw_log_error("Firmware update: no swap was scheduled");
		fwu_state.state = KFSW_FWU_FAILED;
		fwu_state.failed++;
		k_mutex_unlock(&fwu_lock);
		return -EIO;
	}
	fwu_state.swap_scheduled = true;
#endif

	fwu_state.state = KFSW_FWU_READY;
	fwu_state.completed++;
	k_mutex_unlock(&fwu_lock);

	kfsw_log_info("Firmware update ready: %u bytes accepted", fwu_state.total_size);
	return 0;
#else
	return -ENODEV;
#endif
}

int kfsw_fwu_abort(void)
{
	k_mutex_lock(&fwu_lock, K_FOREVER);

#if KFSW_FWU_PARTITION_PRESENT
	if (fwu_state.state == KFSW_FWU_RECEIVING) {
		fwu_state.failed++;
	}
	fwu_close_stream();
	(void)fwu_erase_slot();
#endif

	fwu_state.total_size = 0U;
	fwu_state.received = 0U;
	fwu_state.expected_crc32 = 0U;
	fwu_state.actual_crc32 = 0U;
	fwu_state.swap_scheduled = false;
	fwu_state.state = KFSW_FWU_IDLE;

	k_mutex_unlock(&fwu_lock);
	return 0;
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
	default:
		return "unknown";
	}
}
