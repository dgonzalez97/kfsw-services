#ifndef KFSW_SERVICES_COMMAND_INTERNAL_H
#define KFSW_SERVICES_COMMAND_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/command.h>

/*
 * Command wire format, version 1. Explicit big-endian, fixed header, bounded
 * payload. A C structure is never transmitted directly.
 *
 *   offset size field
 *   0      1    version
 *   1      1    opcode
 *   2      1    status        request: zero. result: kfsw_command_status
 *   3      1    argument count
 *   4      2    command identifier
 *   6      2    request identifier, echoed in the result
 *   8      2    payload size
 *   10     2    reserved, zero
 *
 * A request payload holds argument_count entries of [type:1][size:2][bytes].
 * A result payload holds the detail text, without a terminator.
 */

#define KFSW_COMMAND_PROTOCOL_VERSION 1U
#define KFSW_COMMAND_HEADER_SIZE 12U
#define KFSW_COMMAND_MAX_PAYLOAD_SIZE 192U

enum kfsw_command_opcode {
	KFSW_COMMAND_OP_REQUEST = 1,
	KFSW_COMMAND_OP_RESULT = 2,
};

struct kfsw_command_message {
	uint8_t opcode;
	uint8_t status;
	uint8_t arg_count;
	uint16_t command_id;
	uint16_t request_id;
	uint16_t payload_size;
	const uint8_t *payload;
};

int kfsw_command_protocol_encode(uint8_t *buffer, size_t capacity,
				 const struct kfsw_command_message *message, size_t *encoded_size);
int kfsw_command_protocol_decode(const uint8_t *buffer, size_t size,
				 struct kfsw_command_message *message);

/**
 * Decode a request payload into validated arguments.
 *
 * Text arguments are copied into @p text_storage so they are terminated and
 * outlive the receive buffer. Returns the argument count, or a negative errno.
 */
int kfsw_command_decode_args(const struct kfsw_command_message *message,
			     struct kfsw_command_arg *args, size_t max_args,
			     char text_storage[][KFSW_COMMAND_MAX_TEXT_SIZE + 1U]);

/** Encode validated arguments into a request payload. */
int kfsw_command_encode_args(const struct kfsw_command_arg *args, size_t arg_count,
			     uint8_t *payload, size_t capacity, uint16_t *payload_size);

/** Resolve a registered command name to its wire identifier. */
int kfsw_command_lookup_id(const char *name, uint16_t *id);

#endif
