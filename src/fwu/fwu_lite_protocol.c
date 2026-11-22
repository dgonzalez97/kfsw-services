#include <errno.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <kfsw/services/fwu_lite.h>

static int message_shape(const struct kfsw_fwu_lite_message *m)
{
	if ((m->data_size > KFSW_FWU_LITE_MAX_PAYLOAD_SIZE) || (m->data_size > UINT16_MAX)) {
		return -EMSGSIZE;
	}
	if ((m->version != KFSW_FWU_LITE_VERSION) || (m->direction > KFSW_FWU_LITE_REPLY) ||
	    (m->opcode < KFSW_FWU_LITE_OP_BEGIN) || (m->opcode > KFSW_FWU_LITE_OP_ABORT) ||
	    (m->request_id == 0U) || ((m->data_size != 0U) && (m->data == NULL))) {
		return -EBADMSG;
	}
	if (m->direction == KFSW_FWU_LITE_REPLY) {
		return (m->data_size == KFSW_FWU_LITE_RECEIPT_SIZE &&
			m->status <= KFSW_FWU_LITE_STATUS_NOT_FOUND)
			       ? 0
			       : -EBADMSG;
	}
	if (m->status != 0U) {
		return -EBADMSG;
	}
	if (m->opcode == KFSW_FWU_LITE_OP_BEGIN) {
		return (m->transfer_id != 0U && m->generation == 0U && m->offset == 0U &&
			m->crc32 == 0U && m->data_size == KFSW_FWU_LITE_DESCRIPTOR_SIZE)
			       ? 0
			       : -EBADMSG;
	}
	if (m->opcode == KFSW_FWU_LITE_OP_STATUS && m->transfer_id == 0U && m->generation == 0U) {
		return (m->offset == 0U && m->crc32 == 0U && m->data_size == 0U) ? 0 : -EBADMSG;
	}
	if (m->transfer_id == 0U || m->generation == 0U) {
		return -EBADMSG;
	}
	if (m->opcode == KFSW_FWU_LITE_OP_BLOCK) {
		return m->data_size != 0U && m->data_size <= KFSW_FWU_LITE_MAX_BLOCK_SIZE
			       ? 0
			       : -EBADMSG;
	}
	return (m->offset == 0U && m->crc32 == 0U && m->data_size == 0U) ? 0 : -EBADMSG;
}

int kfsw_fwu_lite_encode(const struct kfsw_fwu_lite_message *m, uint8_t *buffer, size_t capacity,
			 size_t *encoded)
{
	int result;
	if (m == NULL || buffer == NULL || encoded == NULL) {
		return -EINVAL;
	}
	result = message_shape(m);
	if (result != 0) {
		return result;
	}
	if (capacity < KFSW_FWU_LITE_HEADER_SIZE + m->data_size) {
		return -EMSGSIZE;
	}
	buffer[0] = m->version;
	buffer[1] = m->direction;
	buffer[2] = m->opcode;
	buffer[3] = m->status;
	sys_put_be32(m->request_id, buffer + 4);
	sys_put_be32(m->transfer_id, buffer + 8);
	sys_put_be32(m->generation, buffer + 12);
	sys_put_be32(m->offset, buffer + 16);
	sys_put_be32(m->crc32, buffer + 20);
	sys_put_be16((uint16_t)m->data_size, buffer + 24);
	sys_put_be16(0U, buffer + 26);
	if (m->data_size != 0U) {
		memcpy(buffer + KFSW_FWU_LITE_HEADER_SIZE, m->data, m->data_size);
	}
	*encoded = KFSW_FWU_LITE_HEADER_SIZE + m->data_size;
	return 0;
}

int kfsw_fwu_lite_decode(const uint8_t *buffer, size_t size, struct kfsw_fwu_lite_message *m)
{
	if (buffer == NULL || m == NULL) {
		return -EINVAL;
	}
	if (size < KFSW_FWU_LITE_HEADER_SIZE) {
		return -EBADMSG;
	}
	if (size - KFSW_FWU_LITE_HEADER_SIZE > KFSW_FWU_LITE_MAX_PAYLOAD_SIZE) {
		return -EMSGSIZE;
	}
	if (sys_get_be16(buffer + 26) != 0U ||
	    sys_get_be16(buffer + 24) != size - KFSW_FWU_LITE_HEADER_SIZE) {
		return -EBADMSG;
	}
	*m = (struct kfsw_fwu_lite_message){
		.version = buffer[0],
		.direction = buffer[1],
		.opcode = buffer[2],
		.status = buffer[3],
		.request_id = sys_get_be32(buffer + 4),
		.transfer_id = sys_get_be32(buffer + 8),
		.generation = sys_get_be32(buffer + 12),
		.offset = sys_get_be32(buffer + 16),
		.crc32 = sys_get_be32(buffer + 20),
		.data_size = size - KFSW_FWU_LITE_HEADER_SIZE,
		.data = buffer + KFSW_FWU_LITE_HEADER_SIZE,
	};
	return message_shape(m);
}

void kfsw_fwu_lite_pack_image(const struct kfsw_fwu_image *image, uint8_t *data)
{
	sys_put_be32(image->size, data);
	sys_put_be32(image->crc32, data + 4);
	sys_put_be16(image->block_size, data + 8);
	memcpy(data + 10, image->sha256, KFSW_FWU_SHA256_SIZE);
}

int kfsw_fwu_lite_unpack_image(const uint8_t *data, size_t size, struct kfsw_fwu_image *image)
{
	if (data == NULL || image == NULL || size != KFSW_FWU_LITE_DESCRIPTOR_SIZE) {
		return -EBADMSG;
	}
	*image = (struct kfsw_fwu_image){.size = sys_get_be32(data),
					 .crc32 = sys_get_be32(data + 4),
					 .block_size = sys_get_be16(data + 8),
					 .digest_present = true};
	memcpy(image->sha256, data + 10, KFSW_FWU_SHA256_SIZE);
	return image->size != 0U && image->block_size == KFSW_FWU_LITE_MAX_BLOCK_SIZE ? 0
										      : -EBADMSG;
}

bool kfsw_fwu_lite_same_image(const struct kfsw_fwu_image *a, const struct kfsw_fwu_image *b)
{
	return a->digest_present && b->digest_present && a->size == b->size &&
	       a->crc32 == b->crc32 && a->block_size == b->block_size &&
	       memcmp(a->sha256, b->sha256, KFSW_FWU_SHA256_SIZE) == 0;
}

static void pack_receipt(const struct kfsw_fwu_status *s, uint8_t *data)
{
	data[0] = s->state;
	data[1] = (s->swap_scheduled ? 1U : 0U) | (s->aborted ? 2U : 0U) |
		  (s->image.digest_present ? 4U : 0U);
	sys_put_be32(s->received, data + 2);
	sys_put_be32(s->committed, data + 6);
	sys_put_be32(s->actual_crc32, data + 10);
	kfsw_fwu_lite_pack_image(&s->image, data + 14);
}

int kfsw_fwu_lite_receipt(const struct kfsw_fwu_lite_message *reply, struct kfsw_fwu_status *s)
{
	if (reply == NULL || s == NULL || message_shape(reply) != 0 ||
	    reply->direction != KFSW_FWU_LITE_REPLY || reply->data[0] > KFSW_FWU_VERIFIED ||
	    (reply->data[1] & ~7U) != 0U) {
		return -EBADMSG;
	}
	memset(s, 0, sizeof(*s));
	s->state = reply->data[0];
	s->swap_scheduled = (reply->data[1] & 1U) != 0U;
	s->aborted = (reply->data[1] & 2U) != 0U;
	s->received = sys_get_be32(reply->data + 2);
	s->committed = sys_get_be32(reply->data + 6);
	s->actual_crc32 = sys_get_be32(reply->data + 10);
	/* An empty/error snapshot need not carry a valid lite descriptor. */
	(void)kfsw_fwu_lite_unpack_image(reply->data + 14, KFSW_FWU_LITE_DESCRIPTOR_SIZE,
					 &s->image);
	s->image.digest_present = (reply->data[1] & 4U) != 0U;
	s->total_size = s->image.size;
	s->expected_crc32 = s->image.crc32;
	s->handle.transfer_id = reply->transfer_id;
	s->handle.generation = reply->generation;
	if (s->committed > s->total_size || s->received > s->total_size ||
	    (s->state != KFSW_FWU_FAILED && s->committed > s->received) ||
	    reply->offset != s->received || reply->crc32 != s->actual_crc32) {
		return -EBADMSG;
	}
	return 0;
}

static uint8_t status_for_errno(int result)
{
	switch (result) {
	case 0:
		return KFSW_FWU_LITE_STATUS_OK;
	case -EBUSY:
		return KFSW_FWU_LITE_STATUS_BUSY;
	case -EACCES:
		return KFSW_FWU_LITE_STATUS_OWNER;
	case -ESTALE:
		return KFSW_FWU_LITE_STATUS_STALE;
	case -ENOENT:
		return KFSW_FWU_LITE_STATUS_NOT_FOUND;
	case -EFBIG:
		return KFSW_FWU_LITE_STATUS_TOO_LARGE;
	case -ESPIPE:
		return KFSW_FWU_LITE_STATUS_OUT_OF_ORDER;
	case -EILSEQ:
		return KFSW_FWU_LITE_STATUS_BAD_IMAGE;
	case -EBADMSG:
	case -EINVAL:
	case -EAGAIN:
		return KFSW_FWU_LITE_STATUS_INVALID;
	default:
		return KFSW_FWU_LITE_STATUS_FAILED;
	}
}

int kfsw_fwu_lite_handle_peer(uint16_t peer, const struct kfsw_fwu_lite_message *request,
			      struct kfsw_fwu_lite_message *reply, uint8_t *receipt_data)
{
	struct kfsw_fwu_status snapshot = {0};
	struct kfsw_fwu_handle handle;
	struct kfsw_fwu_image image;
	int result;

	if (request == NULL || reply == NULL || receipt_data == NULL) {
		return -EINVAL;
	}
	*reply = *request;
	reply->direction = KFSW_FWU_LITE_REPLY;
	reply->data = receipt_data;
	reply->data_size = KFSW_FWU_LITE_RECEIPT_SIZE;
	handle = (struct kfsw_fwu_handle){.owner = {.kind = KFSW_FWU_OWNER_LITE, .peer = peer},
					  .transfer_id = request->transfer_id,
					  .generation = request->generation};
	result = message_shape(request);
	if (result != 0 || request->direction != KFSW_FWU_LITE_REQUEST) {
		reply->status = KFSW_FWU_LITE_STATUS_INVALID;
		goto done;
	}
	switch (request->opcode) {
	case KFSW_FWU_LITE_OP_BEGIN:
		result = kfsw_fwu_lite_unpack_image(request->data, request->data_size, &image);
		if (result == 0) {
			result = kfsw_fwu_session_begin(&handle.owner, &image, request->transfer_id,
							&handle, &snapshot);
		}
		break;
	case KFSW_FWU_LITE_OP_BLOCK:
		if (crc32_ieee(request->data, request->data_size) != request->crc32) {
			reply->status = KFSW_FWU_LITE_STATUS_BAD_BLOCK;
			goto done;
		}
		result = kfsw_fwu_session_write(&handle, request->offset, request->data,
						request->data_size, &snapshot);
		break;
	case KFSW_FWU_LITE_OP_STATUS:
		result = request->generation == 0U
				 ? kfsw_fwu_session_lookup(&handle.owner, &snapshot)
				 : kfsw_fwu_session_status(&handle, &snapshot);
		break;
	case KFSW_FWU_LITE_OP_VERIFY:
		result = kfsw_fwu_session_verify(&handle, &snapshot);
		break;
	case KFSW_FWU_LITE_OP_START_FLASHING:
		result = kfsw_fwu_session_activate(&handle, &snapshot);
		break;
	case KFSW_FWU_LITE_OP_ABORT:
		result = kfsw_fwu_session_abort(&handle, &snapshot);
		break;
	default:
		result = -EINVAL;
	}
	reply->status = status_for_errno(result);
	if (result == 0) {
		reply->transfer_id = snapshot.handle.transfer_id;
		reply->generation = snapshot.handle.generation;
	} else if (result == -EACCES || result == -ESTALE || result == -ENOENT) {
		memset(&snapshot, 0, sizeof(snapshot));
	}
done:
	reply->offset = snapshot.received;
	reply->crc32 = snapshot.actual_crc32;
	pack_receipt(&snapshot, receipt_data);
	return 0;
}

const char *kfsw_fwu_lite_status_name(uint8_t status)
{
	static const char *const names[] = {"ok",        "invalid",   "out-of-order", "bad-block",
					    "bad-image", "too-large", "busy",         "failed",
					    "owner",     "stale",     "not-found"};
	return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown";
}
