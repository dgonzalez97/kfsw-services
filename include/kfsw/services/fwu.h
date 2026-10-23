#ifndef KFSW_SERVICES_FWU_H
#define KFSW_SERVICES_FWU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_fwu Firmware update
 * @ingroup kfsw_services
 *
 * Receives a firmware image into the secondary image slot and asks the
 * bootloader to try it on the next boot.
 *
 * Images are written directly to flash. With KFSW_FWU_FILES, both slots are
 * also available as read-only files, without using data filesystem space.
 * The service handles the MCUboot write offset and checks upgrade scheduling.
 *
 * With KFSW_FWU_RECOVERY, permanent Golden replaces primary A directly.
 * The whole partition is available, READY schedules no swap, and finish never
 * resets the board. Begin destroys previous A; there is no previous-A rollback.
 * MCUboot must reject invalid A and execute Golden after an interrupted upload.
 *
 * The CRC32 catches corruption in transit, not tampering. Authenticity is the
 * bootloader's signature check.
 *
 * @{
 */

/** Transfer state. */
enum kfsw_fwu_state {
	/** No transfer in progress. */
	KFSW_FWU_IDLE = 0,
	/** A transfer has begun and is accepting data. */
	KFSW_FWU_RECEIVING = 1,
	/** Fully received and verified; swap scheduled only with swap integration. */
	KFSW_FWU_READY = 2,
	/** Failed; use owned abort before retrying (local transfers may use local abort). */
	KFSW_FWU_FAILED = 3,
	/** Checked and flushed to flash; no upgrade requested yet. */
	KFSW_FWU_VERIFIED = 4,
};

/** Whole-artifact SHA-256 length in bytes. */
#define KFSW_FWU_SHA256_SIZE 32U

/** Backend transfer principal. Peer identity is supplied by the transport. */
enum kfsw_fwu_owner_kind {
	/** Ordinary local API (peer zero), or Golden installation (peer one). */
	KFSW_FWU_OWNER_LOCAL = 0,
	/** Reserved firmware upload through FTP; peer is the source node. */
	KFSW_FWU_OWNER_FTP = 1,
	/** Lightweight upload; peer is the source node across connections. */
	KFSW_FWU_OWNER_LITE = 2,
};

/** Stable principal across connections; local peer zero is the ordinary API. */
struct kfsw_fwu_owner {
	/** Entry point that acquired this transfer. */
	enum kfsw_fwu_owner_kind kind;
	/** Stable node address, or local principal described above. */
	uint16_t peer;
};

/** Exact artifact description. CRC-only callers cannot resume by matching it. */
struct kfsw_fwu_image {
	/** Exact artifact length, excluding flash padding and boot metadata. */
	uint32_t size;
	/** IEEE CRC32 of all artifact bytes. */
	uint32_t crc32;
	/** Negotiated block size; part of digest-bearing session identity. */
	uint16_t block_size;
	/** Whether sha256 identifies the complete artifact; false forbids resume. */
	bool digest_present;
	/** SHA-256 of exactly size bytes, valid only with digest_present. */
	uint8_t sha256[KFSW_FWU_SHA256_SIZE];
};

/** Capability bound to a principal and one backend incarnation since boot. */
struct kfsw_fwu_handle {
	/** Principal checked on every operation. */
	struct kfsw_fwu_owner owner;
	/** Sender identity, or a backend ID for CRC-only callers. */
	uint32_t transfer_id;
	/** Nonzero backend incarnation, invalidated by a successor. */
	uint32_t generation;
};

/** Consistent snapshot of an update in progress. */
struct kfsw_fwu_status {
	/** Current identity, retained after failure and successful abort. */
	struct kfsw_fwu_handle handle;
	/** Descriptor retained with the identity, including in the abort tombstone. */
	struct kfsw_fwu_image image;
	/** Image bytes physically written, excluding stream padding. A failed write
	 * may commit a prefix beyond the last fully accepted span.
	 */
	uint32_t committed;
	/** True after successful cleanup; repeated abort performs no flash operation. */
	bool aborted;
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
 * @brief Acquire or resume the canonical writer, returning an atomic snapshot.
 *
 * Only digest-bearing lite descriptors may resume an identical owner, descriptor
 * and transfer ID without erasing. CRC-only begin is non-resumable. Transfer ID
 * zero requests a backend ID for CRC-only callers; digest sessions require a
 * nonzero ID and block size. Generation never wraps during this boot.
 * A completed CRC-only session may be replaced by the same owner, preserving
 * ordinary FWU upload behavior. Failed sessions require owned abort. Successful
 * abort retains one tombstone.
 *
 * All session operations validate and act under the same lock, and fill status
 * even on operational errors. Once acquisition starts, handle is returned even
 * if flash preparation fails, allowing owner-scoped cleanup. Refused acquisition
 * leaves the output handle unchanged. Arguments and output pointers must be
 * non-NULL.
 * Owner mismatch returns -EACCES; superseded identity returns -ESTALE; conflicting
 * begin returns -EBUSY. Handles are RAM-only and do not survive receiver reset.
 * Digest verification establishes content integrity, not authentication.
 */
int kfsw_fwu_session_begin(const struct kfsw_fwu_owner *owner, const struct kfsw_fwu_image *image,
			   uint32_t transfer_id, struct kfsw_fwu_handle *handle,
			   struct kfsw_fwu_status *status);

/** Accept the next ordered span and return its atomic snapshot.
 * Digest-bearing lite sessions replay only the exact last block, including a
 * short tail, without another write. Changed duplicates and older offsets fail.
 */
int kfsw_fwu_session_write(const struct kfsw_fwu_handle *handle, uint32_t offset, const void *data,
			   size_t size, struct kfsw_fwu_status *status);

/** Flush and check exactly the stored image bytes against CRC and optional SHA-256. */
int kfsw_fwu_session_verify(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status);

/** Offer an explicitly VERIFIED image; READY replay does not schedule again. */
int kfsw_fwu_session_activate(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status);

/** Erase this owned transfer; retain identity on failure and a tombstone on success. */
int kfsw_fwu_session_abort(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status);

/** Resolve the current transfer owned by this principal, without mutating it.
 * Returns -ENOENT for no active identity, -EACCES for another owner.
 * Bind subsequent operations to the returned handle; never resolve again on retry.
 */
int kfsw_fwu_session_lookup(const struct kfsw_fwu_owner *owner, struct kfsw_fwu_status *status);

/** Query this handle with the same ownership and generation checks as mutations. */
int kfsw_fwu_session_status(const struct kfsw_fwu_handle *handle, struct kfsw_fwu_status *status);

/**
 * @brief Largest image the target slot can hold, in bytes.
 *
 * Smaller than the partition: the swap offset costs one sector at the start,
 * and the bootloader needs its trailer at the end. Recovery uses the full partition.
 *
 * @return The maximum image size, or zero when no target is bound.
 */
uint32_t kfsw_fwu_max_image_size(void);

/**
 * @brief Byte offset within the target partition where an image is written.
 *
 * Non-zero because of the bootloader's swap mode; zero in recovery mode.
 *
 * @return The offset in bytes.
 */
uint32_t kfsw_fwu_slot_write_offset(void);

/**
 * @brief Begin a CRC-only local transfer, erasing the available slot.
 *
 * Local mutation APIs refuse remote ownership (-EACCES). Begin returns -EBUSY
 * for a conflicting session.
 *
 * @param total_size Image size in bytes.
 * @param expected_crc32 CRC32 (IEEE, as produced by Zephyr's crc32_ieee) over
 *                       the whole image.
 *
 * @retval 0 The slot is ready to receive.
 * @retval -ENODEV No target partition is bound.
 * @retval -EINVAL @p total_size is zero.
 * @retval -EFBIG @p total_size exceeds @ref kfsw_fwu_max_image_size.
 * @retval -EBUSY An unfinished session or another owner holds the slot, a
 *                secondary reader is open, or MCUboot needs trial rollback.
 * @return A negative errno value from the flash layer on failure.
 */
int kfsw_fwu_begin(uint32_t total_size, uint32_t expected_crc32);

/**
 * @brief Accept the next span of image bytes.
 *
 * Spans must arrive in order and without gaps.
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
 * @brief Flush received bytes and independently check the stored image CRC.
 *
 * Reads exactly the declared image bytes, excluding flash padding. Does not
 * schedule a boot. Repeated verification of a verified or ready transfer
 * succeeds. Further writes are rejected until a new transfer begins.
 *
 * @retval 0 The complete image is available in flash.
 * @retval -EINVAL No transfer is receiving or verified.
 * @retval -EAGAIN The transfer is incomplete.
 * @retval -EILSEQ The transfer CRC does not match.
 * @return A negative errno from flash on failure.
 */
int kfsw_fwu_verify(void);

#if CONFIG_KFSW_FWU_FILES
/**
 * @brief Mount read-only MCUboot images at /kfsw/boot.
 *
 * firmware_1.bin reads slot0; firmware_2.bin reads slot1. Files contain the
 * image header, payload, and TLVs, excluding slot padding and swap metadata.
 * A secondary reader blocks erase and upgrade requests until it closes.
 * Image visibility checks structure, not the bootloader's signature policy.
 * Call once during service startup, before exposing file transfers.
 *
 * @return 0 on success, or a negative errno on failure.
 */
int kfsw_fwu_files_mount(void);
#endif

/**
 * @brief Verify the received image and offer it to the bootloader.
 *
 * Repeated finish of a ready transfer succeeds without another boot request
 * or counter increment. On success the image is marked to be tried once. It
 * becomes permanent only if it confirms itself after booting; otherwise the
 * bootloader restores the previous image.
 *
 * Recovery instead leaves A ready for signature verification on an explicitly
 * commanded reset. It neither schedules a swap nor resets the board.
 *
 * @retval 0 The image was accepted; swap_scheduled reports swap integration.
 * @retval -EINVAL No transfer is receiving or verified.
 * @retval -EBUSY A secondary-slot reader is open.
 * @retval -EAGAIN Fewer bytes were received than declared.
 * @retval -EILSEQ The CRC32 does not match what the sender declared.
 * @retval -EIO The bootloader did not schedule a swap despite being asked.
 * @return A negative errno value from the flash layer on failure.
 */
int kfsw_fwu_finish(void);

/**
 * @brief Abandon a transfer and return to idle.
 *
 * On success the secondary slot is erased. If cleanup fails, the service
 * retains its transfer details in the failed state; call again to retry.
 * Erasing the rollback image during a trial boot is refused.
 *
 * @retval 0 Cleanup completed, or no target partition is bound.
 * @retval -EBUSY A secondary-slot reader is open.
 * @return A negative errno from flash on cleanup failure.
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

#if CONFIG_KFSW_PARAM
/** Parameter table of this service, in the service band. */
#define KFSW_FWU_PARAM_TABLE_ID 30U
/** Parameter table name. */
#define KFSW_FWU_PARAM_TABLE_NAME "fwu"

/** Firmware update state and lifetime counters. */
extern const struct kfsw_param_definition_set kfsw_fwu_param_definitions;
#endif

#ifdef __cplusplus
}
#endif

#endif
