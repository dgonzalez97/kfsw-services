#ifndef KFSW_SERVICES_FWU_LITE_H
#define KFSW_SERVICES_FWU_LITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_fwu_lite K-FSW lightweight firmware upload
 * @ingroup kfsw_services
 *
 * A direct CSP path for putting a firmware image on a node, alongside the file
 * transfer route. Both feed the same update service, and both may be built in;
 * whichever starts a transfer first holds it, and the other is told the service
 * is busy.
 *
 * The difference is what each assumes about the link. The file transfer route
 * needs the image to exist as a file on the sending node and runs over a
 * reliable connection, which is convenient when there is somewhere to put a
 * file and the link is good. This route sends blocks straight from wherever
 * the sender has them, checks each block on arrival, and lets the sender repeat
 * a block that did not survive.
 *
 * Per-block checking is the point. A whole-image checksum tells you an eight
 * minute upload failed; a per-block one tells you which 192 bytes to send
 * again. A block that fails its check is not written and does not advance the
 * transfer, so repeating it is simply sending it once more.
 *
 * Reliable delivery is available but off by default. The per-block check and
 * repeat already recover losses, and layering a second retry mechanism
 * underneath adds connection state and timeouts that can stall a transfer on a
 * marginal link rather than reporting a block that needs resending.
 *
 * @{
 */

/** Wire opcodes. Requests carry the low value; replies echo it. */
enum kfsw_fwu_lite_opcode {
	/** Declare size and whole-image checksum; erases the slot. */
	KFSW_FWU_LITE_OP_BEGIN = 1,
	/** Carry one block of image bytes with its own checksum. */
	KFSW_FWU_LITE_OP_BLOCK = 2,
	/** Report transfer state and the block expected next. */
	KFSW_FWU_LITE_OP_STATUS = 3,
	/** Check the received image against the declared checksum. */
	KFSW_FWU_LITE_OP_VERIFY = 4,
	/** Offer the verified image to the bootloader for the next boot. */
	KFSW_FWU_LITE_OP_START_FLASHING = 5,
	/** Abandon the transfer and erase the slot. */
	KFSW_FWU_LITE_OP_ABORT = 6,
};

/** Reply status values. Distinct from errno so the wire meaning is stable. */
enum kfsw_fwu_lite_status {
	KFSW_FWU_LITE_STATUS_OK = 0,
	/** The request was malformed or arrived in the wrong state. */
	KFSW_FWU_LITE_STATUS_INVALID = 1,
	/** The block index was not the one expected next. */
	KFSW_FWU_LITE_STATUS_OUT_OF_ORDER = 2,
	/** The block failed its own checksum; send that block again. */
	KFSW_FWU_LITE_STATUS_BAD_BLOCK = 3,
	/** The image failed its whole-image checksum. */
	KFSW_FWU_LITE_STATUS_BAD_IMAGE = 4,
	/** The image does not fit the slot. */
	KFSW_FWU_LITE_STATUS_TOO_LARGE = 5,
	/** A transfer is already running. */
	KFSW_FWU_LITE_STATUS_BUSY = 6,
	/** The node could not write flash or reach its bootloader. */
	KFSW_FWU_LITE_STATUS_FAILED = 7,
};

/** Fixed header on every request and reply, big-endian on the wire. */
#define KFSW_FWU_LITE_HEADER_SIZE 12U

/** Largest block payload this build accepts. */
#define KFSW_FWU_LITE_MAX_BLOCK_SIZE CONFIG_KFSW_FWU_LITE_BLOCK_SIZE

/** Decoded form of a request or reply. */
struct kfsw_fwu_lite_message {
	uint8_t opcode;
	uint8_t status;
	uint16_t block_index;
	/** BEGIN: image size. BLOCK: checksum of this block. Replies: varies. */
	uint32_t argument;
	/** BEGIN: whole-image checksum. Replies: bytes received so far. */
	uint32_t extra;
	uint16_t data_size;
	const uint8_t *data;
};

/**
 * @brief Encode a message into a buffer.
 *
 * @param message Message to encode.
 * @param buffer Destination.
 * @param buffer_size Bytes available.
 * @param[out] encoded_size Bytes written.
 *
 * @retval 0 Encoded.
 * @retval -EINVAL A pointer is NULL.
 * @retval -EMSGSIZE The payload does not fit, or exceeds the block size.
 */
int kfsw_fwu_lite_encode(const struct kfsw_fwu_lite_message *message, uint8_t *buffer,
			 size_t buffer_size, size_t *encoded_size);

/**
 * @brief Decode a message from a buffer.
 *
 * The decoded @c data points into @p buffer and is valid only while it is.
 *
 * @param buffer Source bytes.
 * @param size Bytes available.
 * @param[out] message Decoded message.
 *
 * @retval 0 Decoded.
 * @retval -EINVAL A pointer is NULL.
 * @retval -EBADMSG The buffer is shorter than a header, or the opcode is not
 *                  one this build knows.
 * @retval -EMSGSIZE The payload is larger than the block size.
 */
int kfsw_fwu_lite_decode(const uint8_t *buffer, size_t size, struct kfsw_fwu_lite_message *message);

/**
 * @brief Apply a decoded request and produce the reply.
 *
 * Holds no transport state, so it is exercised directly by tests without a
 * link. The reply is always well formed: a rejected request produces a reply
 * saying why, never silence.
 *
 * @param request Decoded request.
 * @param[out] reply Reply to send back.
 *
 * @retval 0 A reply was produced, whatever its status.
 * @retval -EINVAL A pointer is NULL.
 */
int kfsw_fwu_lite_handle(const struct kfsw_fwu_lite_message *request,
			 struct kfsw_fwu_lite_message *reply);

/**
 * @brief Human-readable name for a reply status.
 *
 * @param status One of @ref kfsw_fwu_lite_status.
 *
 * @return A stable lowercase name; "unknown" for an unrecognised value.
 */
const char *kfsw_fwu_lite_status_name(uint8_t status);

#if CONFIG_KFSW_FWU_LITE_CSP
/**
 * @brief Start the upload server on the configured CSP port.
 *
 * @retval 0 The server is running.
 * @retval -EALREADY It was already running.
 * @return A negative errno value on failure.
 */
int kfsw_fwu_lite_server_start(void);

/**
 * @brief Send an image file to a node, block by block.
 *
 * The image is read a block at a time rather than held in memory: a firmware
 * image is larger than the RAM of the node it is destined for, and often of
 * the node sending it.
 *
 * Each block is repeated up to the configured retry count if the node reports
 * it did not arrive intact. A block that fails costs one block, not the
 * transfer around it.
 *
 * @param node Destination CSP address.
 * @param path Image file on the sending node.
 * @param[out] blocks_resent Blocks that needed repeating; may be NULL.
 *
 * @retval 0 The node accepted and verified the whole image.
 * @retval -EINVAL @p path is NULL.
 * @retval -ENOTCONN The node did not accept a connection.
 * @retval -EILSEQ The node received the image but it did not match.
 * @return A negative errno value on failure.
 */
int kfsw_fwu_lite_send_file(uint16_t node, const char *path, uint32_t *blocks_resent);

/**
 * @brief Ask a node to boot the image it has accepted.
 *
 * @param node Destination CSP address.
 *
 * @retval 0 The node scheduled a swap.
 * @return A negative errno value on failure.
 */
int kfsw_fwu_lite_start_flashing(uint16_t node);
#endif /* CONFIG_KFSW_FWU_LITE_CSP */

/** @} */

#ifdef __cplusplus
}
#endif

#endif
