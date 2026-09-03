#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/fwu.h>
#include <kfsw/services/fwu_lite.h>

/*
 * Wire layout, big-endian, twelve bytes then the payload:
 *
 *   0      opcode
 *   1      status          (zero in a request)
 *   2..3   block index
 *   4..7   argument        BEGIN: image size. BLOCK: this block's checksum.
 *   8..11  extra           BEGIN: whole-image checksum. Replies: bytes held.
 *   12..   payload         BLOCK only
 *
 * Fields are placed one at a time rather than by copying a structure, so the
 * layout does not depend on how a compiler chooses to pad or order it.
 */

#define OFFSET_OPCODE 0U
#define OFFSET_STATUS 1U
#define OFFSET_BLOCK_INDEX 2U
#define OFFSET_ARGUMENT 4U
#define OFFSET_EXTRA 8U

static bool opcode_is_known(uint8_t opcode)
{
	switch (opcode) {
	case KFSW_FWU_LITE_OP_BEGIN:
	case KFSW_FWU_LITE_OP_BLOCK:
	case KFSW_FWU_LITE_OP_STATUS:
	case KFSW_FWU_LITE_OP_VERIFY:
	case KFSW_FWU_LITE_OP_START_FLASHING:
	case KFSW_FWU_LITE_OP_ABORT:
		return true;
	default:
		return false;
	}
}

int kfsw_fwu_lite_encode(const struct kfsw_fwu_lite_message *message, uint8_t *buffer,
			 size_t buffer_size, size_t *encoded_size)
{
	size_t total;

	if ((message == NULL) || (buffer == NULL) || (encoded_size == NULL)) {
		return -EINVAL;
	}
	if ((message->data_size > 0U) && (message->data == NULL)) {
		return -EINVAL;
	}
	if (message->data_size > KFSW_FWU_LITE_MAX_BLOCK_SIZE) {
		return -EMSGSIZE;
	}

	total = KFSW_FWU_LITE_HEADER_SIZE + message->data_size;
	if (total > buffer_size) {
		return -EMSGSIZE;
	}

	buffer[OFFSET_OPCODE] = message->opcode;
	buffer[OFFSET_STATUS] = message->status;
	sys_put_be16(message->block_index, &buffer[OFFSET_BLOCK_INDEX]);
	sys_put_be32(message->argument, &buffer[OFFSET_ARGUMENT]);
	sys_put_be32(message->extra, &buffer[OFFSET_EXTRA]);

	if (message->data_size > 0U) {
		memcpy(&buffer[KFSW_FWU_LITE_HEADER_SIZE], message->data, message->data_size);
	}

	*encoded_size = total;
	return 0;
}

int kfsw_fwu_lite_decode(const uint8_t *buffer, size_t size, struct kfsw_fwu_lite_message *message)
{
	if ((buffer == NULL) || (message == NULL)) {
		return -EINVAL;
	}
	if (size < KFSW_FWU_LITE_HEADER_SIZE) {
		return -EBADMSG;
	}

	memset(message, 0, sizeof(*message));
	message->opcode = buffer[OFFSET_OPCODE];
	message->status = buffer[OFFSET_STATUS];
	message->block_index = sys_get_be16(&buffer[OFFSET_BLOCK_INDEX]);
	message->argument = sys_get_be32(&buffer[OFFSET_ARGUMENT]);
	message->extra = sys_get_be32(&buffer[OFFSET_EXTRA]);
	message->data_size = (uint16_t)(size - KFSW_FWU_LITE_HEADER_SIZE);

	if (!opcode_is_known(message->opcode)) {
		return -EBADMSG;
	}
	if (message->data_size > KFSW_FWU_LITE_MAX_BLOCK_SIZE) {
		return -EMSGSIZE;
	}

	message->data = (message->data_size > 0U) ? &buffer[KFSW_FWU_LITE_HEADER_SIZE] : NULL;
	return 0;
}

static uint8_t status_for_errno(int result)
{
	switch (result) {
	case 0:
		return KFSW_FWU_LITE_STATUS_OK;
	case -EBUSY:
		return KFSW_FWU_LITE_STATUS_BUSY;
	case -EFBIG:
		return KFSW_FWU_LITE_STATUS_TOO_LARGE;
	case -ESPIPE:
		return KFSW_FWU_LITE_STATUS_OUT_OF_ORDER;
	case -EILSEQ:
		return KFSW_FWU_LITE_STATUS_BAD_IMAGE;
	case -EINVAL:
	case -EAGAIN:
		return KFSW_FWU_LITE_STATUS_INVALID;
	default:
		return KFSW_FWU_LITE_STATUS_FAILED;
	}
}

/* Fill in the fields every reply carries, so no path can answer without them. */
static void describe_state(struct kfsw_fwu_lite_message *reply)
{
	struct kfsw_fwu_status status;

	if (kfsw_fwu_get_status(&status) != 0) {
		return;
	}

	reply->block_index = (uint16_t)(status.received / KFSW_FWU_LITE_MAX_BLOCK_SIZE);
	reply->argument = status.actual_crc32;
	reply->extra = status.received;
}

static void handle_begin(const struct kfsw_fwu_lite_message *request,
			 struct kfsw_fwu_lite_message *reply)
{
	reply->status = status_for_errno(kfsw_fwu_begin(request->argument, request->extra));
}

static void handle_block(const struct kfsw_fwu_lite_message *request,
			 struct kfsw_fwu_lite_message *reply)
{
	struct kfsw_fwu_status status;
	uint32_t expected_index;
	uint32_t computed;

	if ((request->data == NULL) || (request->data_size == 0U)) {
		reply->status = KFSW_FWU_LITE_STATUS_INVALID;
		return;
	}
	if (kfsw_fwu_get_status(&status) != 0) {
		reply->status = KFSW_FWU_LITE_STATUS_FAILED;
		return;
	}
	if (status.state != KFSW_FWU_RECEIVING) {
		reply->status = KFSW_FWU_LITE_STATUS_INVALID;
		return;
	}

	/* Blocks are fixed size except the last, so the index the node expects
	 * follows from how much it already holds. Saying which block is wanted
	 * lets a sender recover without restarting.
	 */
	expected_index = status.received / KFSW_FWU_LITE_MAX_BLOCK_SIZE;
	if (request->block_index != expected_index) {
		reply->status = KFSW_FWU_LITE_STATUS_OUT_OF_ORDER;
		return;
	}

	/* A block index is derived from how much the node holds, which only
	 * works if every block but the last is full. A short block in the middle
	 * would make the sender and the node disagree about which block comes
	 * next, and neither would notice.
	 */
	if (((status.received + request->data_size) < status.total_size) &&
	    (request->data_size != KFSW_FWU_LITE_MAX_BLOCK_SIZE)) {
		reply->status = KFSW_FWU_LITE_STATUS_INVALID;
		return;
	}

	/* Check the block before writing it. A block that fails is not written
	 * and does not advance the transfer, so resending it is just sending it
	 * again rather than restarting or seeking.
	 */
	computed = crc32_ieee(request->data, request->data_size);
	if (computed != request->argument) {
		reply->status = KFSW_FWU_LITE_STATUS_BAD_BLOCK;
		return;
	}

	reply->status = status_for_errno(
		kfsw_fwu_write(status.received, request->data, request->data_size));
}

static void handle_verify(struct kfsw_fwu_lite_message *reply)
{
	struct kfsw_fwu_status status;

	if (kfsw_fwu_get_status(&status) != 0) {
		reply->status = KFSW_FWU_LITE_STATUS_FAILED;
		return;
	}
	if (status.received != status.total_size) {
		reply->status = KFSW_FWU_LITE_STATUS_INVALID;
		return;
	}
	if (status.actual_crc32 != status.expected_crc32) {
		reply->status = KFSW_FWU_LITE_STATUS_BAD_IMAGE;
		return;
	}

	reply->status = KFSW_FWU_LITE_STATUS_OK;
}

static void handle_start_flashing(struct kfsw_fwu_lite_message *reply)
{
	/* Finishing verifies the image and asks the bootloader for a swap, and
	 * reports failure if no swap was actually scheduled.
	 */
	reply->status = status_for_errno(kfsw_fwu_finish());
}

int kfsw_fwu_lite_handle(const struct kfsw_fwu_lite_message *request,
			 struct kfsw_fwu_lite_message *reply)
{
	if ((request == NULL) || (reply == NULL)) {
		return -EINVAL;
	}

	memset(reply, 0, sizeof(*reply));
	reply->opcode = request->opcode;

	switch (request->opcode) {
	case KFSW_FWU_LITE_OP_BEGIN:
		handle_begin(request, reply);
		break;
	case KFSW_FWU_LITE_OP_BLOCK:
		handle_block(request, reply);
		break;
	case KFSW_FWU_LITE_OP_STATUS:
		reply->status = KFSW_FWU_LITE_STATUS_OK;
		break;
	case KFSW_FWU_LITE_OP_VERIFY:
		handle_verify(reply);
		break;
	case KFSW_FWU_LITE_OP_START_FLASHING:
		handle_start_flashing(reply);
		break;
	case KFSW_FWU_LITE_OP_ABORT:
		(void)kfsw_fwu_abort();
		reply->status = KFSW_FWU_LITE_STATUS_OK;
		break;
	default:
		reply->status = KFSW_FWU_LITE_STATUS_INVALID;
		break;
	}

	describe_state(reply);
	return 0;
}

const char *kfsw_fwu_lite_status_name(uint8_t status)
{
	switch (status) {
	case KFSW_FWU_LITE_STATUS_OK:
		return "ok";
	case KFSW_FWU_LITE_STATUS_INVALID:
		return "invalid";
	case KFSW_FWU_LITE_STATUS_OUT_OF_ORDER:
		return "out-of-order";
	case KFSW_FWU_LITE_STATUS_BAD_BLOCK:
		return "bad-block";
	case KFSW_FWU_LITE_STATUS_BAD_IMAGE:
		return "bad-image";
	case KFSW_FWU_LITE_STATUS_TOO_LARGE:
		return "too-large";
	case KFSW_FWU_LITE_STATUS_BUSY:
		return "busy";
	case KFSW_FWU_LITE_STATUS_FAILED:
		return "failed";
	default:
		return "unknown";
	}
}
