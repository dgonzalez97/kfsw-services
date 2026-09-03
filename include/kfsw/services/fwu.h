#ifndef KFSW_SERVICES_FWU_H
#define KFSW_SERVICES_FWU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_fwu K-FSW firmware update
 * @ingroup kfsw_services
 *
 * Receives a firmware image into the secondary image slot and asks the
 * bootloader to try it on the next boot.
 *
 * The image is streamed straight into raw flash. It is never a file: an
 * application image is larger than the filesystem partition on the first
 * target, so staging it as a file is not merely wasteful but impossible.
 *
 * Two properties are the reason this service exists rather than the caller
 * writing flash directly.
 *
 * The first is the write offset. MCUboot runs in swap-using-offset mode, where
 * an update must be written one sector into the secondary slot rather than at
 * its start. Writing at the start is not rejected: the bootloader simply finds
 * nothing to swap and carries on with the old image. Callers pass offsets from
 * zero and this service places them correctly, so the trap is expressible in
 * one place instead of in every caller.
 *
 * The second is that finishing does not merely request an upgrade, it checks
 * one was actually scheduled. A request that quietly achieves nothing looks
 * exactly like a successful update until the old image answers the next
 * telemetry poll.
 *
 * Integrity is a CRC32 over the whole image, which detects corruption in
 * transit. It is not authenticity: that is the bootloader's signature check,
 * performed before it will run anything.
 *
 * @{
 */

/** Transfer state. */
enum kfsw_fwu_state {
	/** No transfer in progress. */
	KFSW_FWU_IDLE = 0,
	/** A transfer has begun and is accepting data. */
	KFSW_FWU_RECEIVING = 1,
	/** Fully received, verified, and offered to the bootloader. */
	KFSW_FWU_READY = 2,
	/** The transfer failed; call @ref kfsw_fwu_abort before retrying. */
	KFSW_FWU_FAILED = 3,
};

/** Consistent snapshot of an update in progress. */
struct kfsw_fwu_status {
	/** Image size the sender declared, in bytes. */
	uint32_t total_size;
	/** Bytes accepted so far. */
	uint32_t received;
	/** CRC32 the sender declared. */
	uint32_t expected_crc32;
	/** CRC32 accumulated over what was accepted. */
	uint32_t actual_crc32;
	/** Transfers begun since boot. */
	uint32_t started;
	/** Transfers that reached @ref KFSW_FWU_READY since boot. */
	uint32_t completed;
	/** Transfers that failed or were aborted since boot. */
	uint32_t failed;
	/** One of @ref kfsw_fwu_state. */
	uint8_t state;
	/** True once the bootloader confirmed a swap is scheduled. */
	bool swap_scheduled;
	/** True when a target partition was bound at build time. */
	bool target_bound;
};

/**
 * @brief Largest image the target slot can hold, in bytes.
 *
 * Smaller than the partition: the swap offset costs one sector at the start,
 * and the bootloader needs its trailer at the end.
 *
 * @return The maximum image size, or zero when no target is bound.
 */
uint32_t kfsw_fwu_max_image_size(void);

/**
 * @brief Byte offset within the target partition where an image is written.
 *
 * Non-zero because of the bootloader's swap mode. Exposed so the reason is
 * testable rather than buried.
 *
 * @return The offset in bytes.
 */
uint32_t kfsw_fwu_slot_write_offset(void);

/**
 * @brief Begin receiving an image, erasing whatever the slot held.
 *
 * @param total_size Image size in bytes.
 * @param expected_crc32 CRC32 (IEEE, as produced by Zephyr's crc32_ieee) over
 *                       the whole image.
 *
 * @retval 0 The slot is ready to receive.
 * @retval -ENODEV No target partition is bound.
 * @retval -EINVAL @p total_size is zero.
 * @retval -EFBIG @p total_size exceeds @ref kfsw_fwu_max_image_size.
 * @retval -EBUSY A transfer is already in progress.
 * @return A negative errno value from the flash layer on failure.
 */
int kfsw_fwu_begin(uint32_t total_size, uint32_t expected_crc32);

/**
 * @brief Accept the next span of image bytes.
 *
 * Spans must arrive in order and without gaps. An out-of-order write is
 * rejected rather than seeked to: a hole in a firmware image that still passes
 * a whole-image CRC would have to be a deliberate collision, but a hole that
 * is never noticed until the bootloader jumps into it is not worth the risk.
 *
 * @param offset Offset of this span within the image, from zero.
 * @param data Bytes to write.
 * @param size Number of bytes.
 *
 * @retval 0 The span was accepted.
 * @retval -EINVAL @p data is NULL, @p size is zero, or no transfer is running.
 * @retval -ESPIPE @p offset is not where the transfer had reached.
 * @retval -EFBIG The span would run past the declared image size.
 * @return A negative errno value from the flash layer on failure.
 */
int kfsw_fwu_write(uint32_t offset, const void *data, size_t size);

/**
 * @brief Verify the received image and offer it to the bootloader.
 *
 * On success the image is marked to be tried once. It becomes permanent only
 * if it confirms itself after booting; otherwise the bootloader restores the
 * previous image.
 *
 * @retval 0 The image was accepted and a swap is scheduled.
 * @retval -EINVAL No transfer is running.
 * @retval -EAGAIN Fewer bytes were received than declared.
 * @retval -EILSEQ The CRC32 does not match what the sender declared.
 * @retval -EIO The bootloader did not schedule a swap despite being asked.
 * @return A negative errno value from the flash layer on failure.
 */
int kfsw_fwu_finish(void);

/**
 * @brief Abandon a transfer and return to idle.
 *
 * Safe to call in any state. The slot is left erased, so a partial image can
 * never be mistaken for a complete one.
 *
 * @retval 0 Always.
 */
int kfsw_fwu_abort(void);

/**
 * @brief Read a consistent snapshot of the update state.
 *
 * @param[out] status Destination snapshot.
 *
 * @retval 0 The snapshot was written.
 * @retval -EINVAL @p status is NULL.
 */
int kfsw_fwu_get_status(struct kfsw_fwu_status *status);

/**
 * @brief Human-readable name for a state, for shells and logs.
 *
 * @param state One of @ref kfsw_fwu_state.
 *
 * @return A stable lowercase name; "unknown" for an unrecognised value.
 */
const char *kfsw_fwu_state_name(enum kfsw_fwu_state state);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
