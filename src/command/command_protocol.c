#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "command_internal.h"

/* Wire codec. Every length is checked against the buffer before use. */

int kfsw_command_protocol_encode(uint8_t *buffer, size_t capacity,
				 const struct kfsw_command_message *message, size_t *encoded_size)
{
	size_t size;
	size_t header;
	uint8_t version;

	if ((buffer == NULL) || (message == NULL) || (encoded_size == NULL) ||
	    (message->payload_size > KFSW_COMMAND_MAX_PAYLOAD_SIZE) ||
	    ((message->payload_size != 0U) && (message->payload == NULL)) ||
	    (message->arg_count > KFSW_COMMAND_MAX_ARGS)) {
		return -EINVAL;
	}
	version = message->version ? message->version : KFSW_COMMAND_PROTOCOL_VERSION;
	if ((version != 1U) && (version != 2U)) {
		return -EPROTONOSUPPORT;
	}
	header = (version == 2U) ? KFSW_COMMAND_RETRY_HEADER_SIZE : KFSW_COMMAND_HEADER_SIZE;
	size = header + message->payload_size;
	if (size > capacity) {
		return -EMSGSIZE;
	}

	memset(buffer, 0, header);
	buffer[0] = version;
	if (version == 2U) {
		sys_put_be64(message->token, &buffer[12]);
	}
	buffer[1] = message->opcode;
	buffer[2] = message->status;
	buffer[3] = message->arg_count;
	sys_put_be16(message->command_id, &buffer[4]);
	sys_put_be16(message->request_id, &buffer[6]);
	sys_put_be16(message->payload_size, &buffer[8]);
	if (message->payload_size != 0U) {
		memcpy(&buffer[header], message->payload, message->payload_size);
	}
	*encoded_size = size;
	return 0;
}

int kfsw_command_protocol_decode(const uint8_t *buffer, size_t size,
				 struct kfsw_command_message *message)
{
	size_t header;

	if ((buffer == NULL) || (message == NULL) || (size < KFSW_COMMAND_HEADER_SIZE)) {
		return -EMSGSIZE;
	}
	if ((buffer[0] != 1U) && (buffer[0] != 2U)) {
		return -EPROTONOSUPPORT;
	}
	header = (buffer[0] == 2U) ? KFSW_COMMAND_RETRY_HEADER_SIZE : KFSW_COMMAND_HEADER_SIZE;
	if (size < header) {
		return -EMSGSIZE;
	}
	if ((buffer[10] != 0U) || (buffer[11] != 0U)) {
		return -EBADMSG;
	}
	if ((buffer[1] < KFSW_COMMAND_OP_REQUEST) ||
	    (buffer[1] > ((buffer[0] == 2U) ? KFSW_COMMAND_OP_EXECUTE : KFSW_COMMAND_OP_RESULT))) {
		return -ENOTSUP;
	}

	memset(message, 0, sizeof(*message));
	message->version = buffer[0];
	message->token = (buffer[0] == 2U) ? sys_get_be64(&buffer[12]) : 0U;
	message->opcode = buffer[1];
	message->status = buffer[2];
	message->arg_count = buffer[3];
	message->command_id = sys_get_be16(&buffer[4]);
	message->request_id = sys_get_be16(&buffer[6]);
	message->payload_size = sys_get_be16(&buffer[8]);

	if (message->arg_count > KFSW_COMMAND_MAX_ARGS) {
		return -E2BIG;
	}
	if (message->payload_size > KFSW_COMMAND_MAX_PAYLOAD_SIZE) {
		return -EMSGSIZE;
	}
	/* The declared payload size must match what arrived. */
	if ((size_t)(header + message->payload_size) != size) {
		return -EMSGSIZE;
	}
	message->payload = &buffer[header];
	return 0;
}

int kfsw_command_encode_args(const struct kfsw_command_arg *args, size_t arg_count,
			     uint8_t *payload, size_t capacity, uint16_t *payload_size)
{
	size_t offset = 0U;

	if ((payload == NULL) || (payload_size == NULL) || ((arg_count != 0U) && (args == NULL))) {
		return -EINVAL;
	}
	if (arg_count > KFSW_COMMAND_MAX_ARGS) {
		return -E2BIG;
	}
	for (size_t index = 0U; index < arg_count; index++) {
		size_t value_size;

		switch (args[index].type) {
		case KFSW_COMMAND_TYPE_U32:
		case KFSW_COMMAND_TYPE_I32:
			value_size = sizeof(uint32_t);
			break;
		case KFSW_COMMAND_TYPE_TEXT:
			if (args[index].value.text == NULL) {
				return -EINVAL;
			}
			value_size = 0U;
			while ((value_size <= KFSW_COMMAND_MAX_TEXT_SIZE) &&
			       (args[index].value.text[value_size] != '\0')) {
				value_size++;
			}
			if (value_size > KFSW_COMMAND_MAX_TEXT_SIZE) {
				return -ENAMETOOLONG;
			}
			break;
		default:
			return -ENOTSUP;
		}
		if ((offset + 3U + value_size) > capacity) {
			return -EMSGSIZE;
		}
		payload[offset] = (uint8_t)args[index].type;
		sys_put_be16((uint16_t)value_size, &payload[offset + 1U]);
		offset += 3U;
		if (args[index].type == KFSW_COMMAND_TYPE_TEXT) {
			memcpy(&payload[offset], args[index].value.text, value_size);
		} else if (args[index].type == KFSW_COMMAND_TYPE_U32) {
			sys_put_be32(args[index].value.u32, &payload[offset]);
		} else {
			sys_put_be32((uint32_t)args[index].value.i32, &payload[offset]);
		}
		offset += value_size;
	}
	*payload_size = (uint16_t)offset;
	return 0;
}

int kfsw_command_decode_args(const struct kfsw_command_message *message,
			     struct kfsw_command_arg *args, size_t max_args,
			     char text_storage[][KFSW_COMMAND_MAX_TEXT_SIZE + 1U])
{
	size_t offset = 0U;
	size_t text_index = 0U;

	if ((message == NULL) || (args == NULL) || (text_storage == NULL)) {
		return -EINVAL;
	}
	if (message->arg_count > max_args) {
		return -E2BIG;
	}
	for (size_t index = 0U; index < message->arg_count; index++) {
		uint8_t type;
		uint16_t value_size;

		/* Each entry needs its type byte and length before its value. */
		if ((offset + 3U) > message->payload_size) {
			return -EMSGSIZE;
		}
		type = message->payload[offset];
		value_size = sys_get_be16(&message->payload[offset + 1U]);
		offset += 3U;
		if ((offset + value_size) > message->payload_size) {
			return -EMSGSIZE;
		}

		switch (type) {
		case KFSW_COMMAND_TYPE_U32:
			if (value_size != sizeof(uint32_t)) {
				return -EBADMSG;
			}
			args[index].type = KFSW_COMMAND_TYPE_U32;
			args[index].value.u32 = sys_get_be32(&message->payload[offset]);
			break;
		case KFSW_COMMAND_TYPE_I32:
			if (value_size != sizeof(int32_t)) {
				return -EBADMSG;
			}
			args[index].type = KFSW_COMMAND_TYPE_I32;
			args[index].value.i32 = (int32_t)sys_get_be32(&message->payload[offset]);
			break;
		case KFSW_COMMAND_TYPE_TEXT:
			if (value_size > KFSW_COMMAND_MAX_TEXT_SIZE) {
				return -EMSGSIZE;
			}
			if (memchr(&message->payload[offset], '\0', value_size) != NULL) {
				return -EBADMSG;
			}
			memcpy(text_storage[text_index], &message->payload[offset], value_size);
			text_storage[text_index][value_size] = '\0';
			args[index].type = KFSW_COMMAND_TYPE_TEXT;
			args[index].value.text = text_storage[text_index];
			text_index++;
			break;
		default:
			return -ENOTSUP;
		}
		offset += value_size;
	}
	if (offset != message->payload_size) {
		return -EMSGSIZE;
	}
	return (int)message->arg_count;
}
