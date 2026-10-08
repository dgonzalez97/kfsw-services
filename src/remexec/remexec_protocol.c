#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "remexec_internal.h"

/* Layout: version, opcode, be16 length, text. */

static bool known_opcode(uint8_t opcode)
{
	return (opcode == (uint8_t)KFSW_REMEXEC_OP_LIST) ||
	       (opcode == (uint8_t)KFSW_REMEXEC_OP_RUN);
}

int kfsw_remexec_encode_request(uint8_t *buffer, size_t capacity, enum kfsw_remexec_opcode opcode,
				const char *text, size_t *size)
{
	size_t length = (text != NULL) ? strnlen(text, KFSW_REMEXEC_COMMAND_MAX + 1U) : 0U;

	if ((buffer == NULL) || (size == NULL) || !known_opcode((uint8_t)opcode)) {
		return -EINVAL;
	}
	if (length > KFSW_REMEXEC_COMMAND_MAX) {
		return -ENAMETOOLONG;
	}
	if (capacity < (KFSW_REMEXEC_REQUEST_HEADER_SIZE + length)) {
		return -ENOSPC;
	}
	buffer[0] = KFSW_REMEXEC_PROTOCOL_VERSION;
	buffer[1] = (uint8_t)opcode;
	sys_put_be16((uint16_t)length, &buffer[2]);
	if (length != 0U) {
		memcpy(&buffer[KFSW_REMEXEC_REQUEST_HEADER_SIZE], text, length);
	}
	*size = KFSW_REMEXEC_REQUEST_HEADER_SIZE + length;
	return 0;
}

int kfsw_remexec_decode_request(const uint8_t *data, size_t size, enum kfsw_remexec_opcode *opcode,
				char *text, size_t text_capacity)
{
	uint16_t length;

	if ((data == NULL) || (opcode == NULL) || (text == NULL) || (text_capacity == 0U)) {
		return -EINVAL;
	}
	if (size < KFSW_REMEXEC_REQUEST_HEADER_SIZE) {
		return -EBADMSG;
	}
	if ((data[0] != KFSW_REMEXEC_PROTOCOL_VERSION) || !known_opcode(data[1])) {
		return -EBADMSG;
	}
	length = sys_get_be16(&data[2]);
	if ((size_t)length != (size - KFSW_REMEXEC_REQUEST_HEADER_SIZE)) {
		return -EBADMSG;
	}
	if ((size_t)length >= text_capacity) {
		return -ENAMETOOLONG;
	}
	*opcode = (enum kfsw_remexec_opcode)data[1];
	memcpy(text, &data[KFSW_REMEXEC_REQUEST_HEADER_SIZE], length);
	text[length] = '\0';
	return 0;
}

int kfsw_remexec_encode_reply(uint8_t *buffer, size_t capacity, enum kfsw_remexec_opcode opcode,
			      const struct kfsw_remexec_reply *reply, size_t *size)
{
	size_t length;

	if ((buffer == NULL) || (reply == NULL) || (size == NULL) ||
	    !known_opcode((uint8_t)opcode)) {
		return -EINVAL;
	}
	length = MIN((size_t)reply->size, KFSW_REMEXEC_OUTPUT_MAX);
	if (capacity < (KFSW_REMEXEC_REPLY_HEADER_SIZE + length)) {
		return -ENOSPC;
	}
	buffer[0] = KFSW_REMEXEC_PROTOCOL_VERSION;
	buffer[1] = (uint8_t)opcode;
	buffer[2] = (uint8_t)reply->status;
	buffer[3] = reply->truncated ? KFSW_REMEXEC_FLAG_TRUNCATED : 0U;
	sys_put_be32((uint32_t)reply->command_result, &buffer[4]);
	sys_put_be32(reply->produced, &buffer[8]);
	sys_put_be32(reply->dropped, &buffer[12]);
	sys_put_be16((uint16_t)length, &buffer[16]);
	if (length != 0U) {
		memcpy(&buffer[KFSW_REMEXEC_REPLY_HEADER_SIZE], reply->text, length);
	}
	*size = KFSW_REMEXEC_REPLY_HEADER_SIZE + length;
	return 0;
}

int kfsw_remexec_decode_reply(const uint8_t *data, size_t size, enum kfsw_remexec_opcode opcode,
			      struct kfsw_remexec_reply *reply)
{
	uint16_t length;

	if ((data == NULL) || (reply == NULL)) {
		return -EINVAL;
	}
	if (size < KFSW_REMEXEC_REPLY_HEADER_SIZE) {
		return -EBADMSG;
	}
	if ((data[0] != KFSW_REMEXEC_PROTOCOL_VERSION) || (data[1] != (uint8_t)opcode) ||
	    (data[2] > (uint8_t)KFSW_REMEXEC_UNAVAILABLE)) {
		return -EBADMSG;
	}
	length = sys_get_be16(&data[16]);
	if (((size_t)length != (size - KFSW_REMEXEC_REPLY_HEADER_SIZE)) ||
	    ((size_t)length > KFSW_REMEXEC_OUTPUT_MAX)) {
		return -EBADMSG;
	}
	reply->status = (enum kfsw_remexec_status)data[2];
	reply->truncated = (data[3] & KFSW_REMEXEC_FLAG_TRUNCATED) != 0U;
	reply->command_result = (int32_t)sys_get_be32(&data[4]);
	reply->produced = sys_get_be32(&data[8]);
	reply->dropped = sys_get_be32(&data[12]);
	reply->size = length;
	memcpy(reply->text, &data[KFSW_REMEXEC_REPLY_HEADER_SIZE], length);
	reply->text[length] = '\0';
	return 0;
}
