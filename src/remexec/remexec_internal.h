#ifndef KFSW_SERVICES_REMEXEC_INTERNAL_H
#define KFSW_SERVICES_REMEXEC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/remexec.h>

/** Wire version. A reply carrying another version is discarded. */
#define KFSW_REMEXEC_PROTOCOL_VERSION 1U

/** What a request asks for. */
enum kfsw_remexec_opcode {
	KFSW_REMEXEC_OP_LIST = 1,
	KFSW_REMEXEC_OP_RUN = 2,
};

/** version, opcode, text length. */
#define KFSW_REMEXEC_REQUEST_HEADER_SIZE 4U

/**
 * version, opcode, status, flags, command result, produced, dropped, text
 * length.
 */
#define KFSW_REMEXEC_REPLY_HEADER_SIZE 18U

/** The reply does not carry everything the command printed. */
#define KFSW_REMEXEC_FLAG_TRUNCATED 0x01U

/**
 * @brief Encode a request.
 *
 * @retval 0 Encoded; @p size holds the byte count.
 * @retval -ENOSPC @p capacity is too small.
 * @retval -EINVAL A NULL was given or the opcode is not known.
 * @retval -ENAMETOOLONG @p text is longer than KFSW_REMEXEC_COMMAND_MAX.
 */
int kfsw_remexec_encode_request(uint8_t *buffer, size_t capacity, enum kfsw_remexec_opcode opcode,
				const char *text, size_t *size);

/**
 * @brief Decode a request.
 *
 * @param[out] opcode What was asked for.
 * @param[out] text NUL-terminated request text, at most
 *             KFSW_REMEXEC_COMMAND_MAX bytes plus a terminator.
 * @retval 0 Decoded.
 * @retval -EBADMSG Short, wrong version, unknown opcode, or a declared length
 *         that does not match the packet.
 * @retval -ENAMETOOLONG The text does not fit @p text_capacity.
 */
int kfsw_remexec_decode_request(const uint8_t *data, size_t size, enum kfsw_remexec_opcode *opcode,
				char *text, size_t text_capacity);

/**
 * @brief Encode a reply.
 *
 * Text past KFSW_REMEXEC_OUTPUT_MAX is not encoded; the caller has already
 * accounted for it in @p reply.
 */
int kfsw_remexec_encode_reply(uint8_t *buffer, size_t capacity, enum kfsw_remexec_opcode opcode,
			      const struct kfsw_remexec_reply *reply, size_t *size);

/**
 * @brief Decode a reply and check it answers @p opcode.
 *
 * @retval 0 Decoded.
 * @retval -EBADMSG Short, wrong version, wrong opcode, unknown status, or a
 *         declared length that does not match the packet.
 */
int kfsw_remexec_decode_reply(const uint8_t *data, size_t size, enum kfsw_remexec_opcode opcode,
			      struct kfsw_remexec_reply *reply);

#endif
