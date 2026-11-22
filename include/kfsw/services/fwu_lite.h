#ifndef KFSW_SERVICES_FWU_LITE_H
#define KFSW_SERVICES_FWU_LITE_H

#include <kfsw/services/fwu.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup kfsw_services_fwu_lite Lightweight firmware upload
 * @ingroup kfsw_services
 * Powered-receiver resume with explicit validation and activation. Transport
 * supplies the principal; the wire never supplies an owner. No legacy format.
 * @{ */
#define KFSW_FWU_LITE_VERSION 1U
#define KFSW_FWU_LITE_REQUEST 0U
#define KFSW_FWU_LITE_REPLY 1U
/** Fixed big-endian header: version/direction/op/status (one byte each),
 * request/transfer/generation/offset/CRC (uint32), payload length (uint16),
 * reserved zero (uint16). Offsets are artifact byte offsets.
 */
#define KFSW_FWU_LITE_HEADER_SIZE 28U
#define KFSW_FWU_LITE_DESCRIPTOR_SIZE 42U
#define KFSW_FWU_LITE_RECEIPT_SIZE 56U
#define KFSW_FWU_LITE_MAX_BLOCK_SIZE CONFIG_KFSW_FWU_LITE_BLOCK_SIZE
#define KFSW_FWU_LITE_MAX_PAYLOAD_SIZE                                                             \
	((KFSW_FWU_LITE_MAX_BLOCK_SIZE > KFSW_FWU_LITE_RECEIPT_SIZE)                               \
		 ? KFSW_FWU_LITE_MAX_BLOCK_SIZE                                                    \
		 : KFSW_FWU_LITE_RECEIPT_SIZE)
#define KFSW_FWU_LITE_WIRE_SIZE (KFSW_FWU_LITE_HEADER_SIZE + KFSW_FWU_LITE_MAX_PAYLOAD_SIZE)

/** Explicit operations; upload never implicitly verifies or activates. */
enum kfsw_fwu_lite_opcode {
	KFSW_FWU_LITE_OP_BEGIN = 1,
	KFSW_FWU_LITE_OP_BLOCK = 2,
	KFSW_FWU_LITE_OP_STATUS = 3,
	KFSW_FWU_LITE_OP_VERIFY = 4,
	KFSW_FWU_LITE_OP_START_FLASHING = 5,
	KFSW_FWU_LITE_OP_ABORT = 6,
};

/** Stable wire errors, independent of errno. */
enum kfsw_fwu_lite_status {
	KFSW_FWU_LITE_STATUS_OK = 0,
	KFSW_FWU_LITE_STATUS_INVALID = 1,
	KFSW_FWU_LITE_STATUS_OUT_OF_ORDER = 2,
	KFSW_FWU_LITE_STATUS_BAD_BLOCK = 3,
	KFSW_FWU_LITE_STATUS_BAD_IMAGE = 4,
	KFSW_FWU_LITE_STATUS_TOO_LARGE = 5,
	KFSW_FWU_LITE_STATUS_BUSY = 6,
	KFSW_FWU_LITE_STATUS_FAILED = 7,
	KFSW_FWU_LITE_STATUS_OWNER = 8,
	KFSW_FWU_LITE_STATUS_STALE = 9,
	KFSW_FWU_LITE_STATUS_NOT_FOUND = 10,
};

/** Decoded message. Payload points into the input buffer; no allocation. */
struct kfsw_fwu_lite_message {
	uint8_t version;
	uint8_t direction;
	uint8_t opcode;
	uint8_t status;
	uint32_t request_id;
	uint32_t transfer_id;
	uint32_t generation;
	uint32_t offset;
	uint32_t crc32;
	size_t data_size;
	const uint8_t *data;
};

/** Encode/decode with exact lengths and shape validation before narrowing.
 * Returns -EINVAL for NULL, -EMSGSIZE for excess size, -EBADMSG for illegal
 * wire fields or request shape. Decode payload remains owned by the caller.
 */
int kfsw_fwu_lite_encode(const struct kfsw_fwu_lite_message *message, uint8_t *buffer,
			 size_t buffer_size, size_t *encoded_size);
int kfsw_fwu_lite_decode(const uint8_t *buffer, size_t size, struct kfsw_fwu_lite_message *message);

/** Encode the mandatory digest-bearing BEGIN descriptor into 42 bytes. */
void kfsw_fwu_lite_pack_image(const struct kfsw_fwu_image *image, uint8_t *data);
/** Decode an exact BEGIN descriptor, rejecting missing size or block mismatch. */
int kfsw_fwu_lite_unpack_image(const uint8_t *data, size_t size, struct kfsw_fwu_image *image);
/** Decode a reply receipt, including descriptor and exact identity. */
int kfsw_fwu_lite_receipt(const struct kfsw_fwu_lite_message *reply,
			  struct kfsw_fwu_status *status);
/** Compare artifact descriptors, including mandatory digest and block size. */
bool kfsw_fwu_lite_same_image(const struct kfsw_fwu_image *a, const struct kfsw_fwu_image *b);
/** Apply a request using transport-supplied stable peer identity. Reply payload
 * is written to caller-owned receipt_data (at least 56 bytes). Direct calls
 * enforce the same shape checks as the decoder. No session state lives here.
 */
int kfsw_fwu_lite_handle_peer(uint16_t peer, const struct kfsw_fwu_lite_message *request,
			      struct kfsw_fwu_lite_message *reply, uint8_t *receipt_data);
/** Stable lowercase status name, or "unknown". */
const char *kfsw_fwu_lite_status_name(uint8_t status);

#if CONFIG_KFSW_FWU_LITE_CSP
#define KFSW_FWU_LITE_TIMEOUT_MIN_MS 100U
#define KFSW_FWU_LITE_TIMEOUT_MAX_MS 60000U
/** Start the existing server thread; repeated start returns -EALREADY. */
int kfsw_fwu_lite_server_start(void);
/** Read or set bounded per-attempt reply timeout. */
uint32_t kfsw_fwu_lite_get_timeout_ms(void);
int kfsw_fwu_lite_check_timeout_ms(uint32_t timeout_ms);
int kfsw_fwu_lite_set_timeout_ms(uint32_t timeout_ms);
/** Read or set additional attempts; zero is invalid. */
uint8_t kfsw_fwu_lite_get_retries(void);
int kfsw_fwu_lite_set_retries(uint8_t retries);
/** Upload receipt only. Later invocation reconciles the same owned descriptor
 * and local prefix CRC via STATUS. Errors retain receiver progress; never abort,
 * verify, activate or reset implicitly. Linux host paths must be absolute.
 * Optional receipt receives the last correlated snapshot, including on failure;
 * zero identity means acquisition was not acknowledged. Query STATUS to reconcile.
 */
int kfsw_fwu_lite_send_file(uint16_t node, const char *path, struct kfsw_fwu_status *receipt,
			    uint32_t *blocks_resent);
/** Query owned current session when handle is NULL; otherwise query exact handle.
 * Owner fields are not transmitted; remote operations use the IDs and the
 * transport supplies the principal.
 */
int kfsw_fwu_lite_remote_status(uint16_t node, const struct kfsw_fwu_handle *handle,
				struct kfsw_fwu_status *status);
/** Explicit operations on an exact receipt handle; NULL is invalid. */
int kfsw_fwu_lite_remote_verify(uint16_t node, const struct kfsw_fwu_handle *handle,
				struct kfsw_fwu_status *status);
int kfsw_fwu_lite_remote_activate(uint16_t node, const struct kfsw_fwu_handle *handle,
				  struct kfsw_fwu_status *status);
int kfsw_fwu_lite_remote_abort(uint16_t node, const struct kfsw_fwu_handle *handle,
			       struct kfsw_fwu_status *status);
/** Convenience caller: resolve once with STATUS then activate that exact handle. */
int kfsw_fwu_lite_start_flashing(uint16_t node);
#endif
/** @} */
#ifdef __cplusplus
}
#endif
#endif
