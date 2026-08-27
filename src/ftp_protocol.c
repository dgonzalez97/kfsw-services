#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#include "ftp_internal.h"

int kfsw_ftp_protocol_encode(uint8_t *buffer, size_t capacity,
			     const struct kfsw_ftp_message *message, size_t *encoded_size)
{
	size_t size;

	if ((buffer == NULL) || (message == NULL) || (encoded_size == NULL) ||
	    (message->opcode < KFSW_FTP_OP_MKDIR_REQUEST) ||
	    (message->opcode > KFSW_FTP_OP_GET_RESULT) ||
	    (message->path_size > KFSW_FTP_MAX_PATH_SIZE) ||
	    (message->data_size > KFSW_FTP_CHUNK_SIZE) ||
	    ((message->path_size != 0U) && (message->path == NULL)) ||
	    ((message->data_size != 0U) && (message->data == NULL))) {
		return -EINVAL;
	}

	size = KFSW_FTP_PROTOCOL_HEADER_SIZE + message->path_size + message->data_size;
	if (size > capacity) {
		return -EMSGSIZE;
	}

	buffer[0] = KFSW_FTP_PROTOCOL_VERSION;
	buffer[1] = message->opcode;
	buffer[2] = message->flags;
	buffer[3] = message->status;
	sys_put_be32(message->request_id, &buffer[4]);
	sys_put_be32(message->offset, &buffer[8]);
	sys_put_be32(message->total_size, &buffer[12]);
	sys_put_be32(message->crc32, &buffer[16]);
	sys_put_be16(message->path_size, &buffer[20]);
	sys_put_be16(message->data_size, &buffer[22]);
	if (message->path_size != 0U) {
		memcpy(&buffer[KFSW_FTP_PROTOCOL_HEADER_SIZE], message->path, message->path_size);
	}
	if (message->data_size != 0U) {
		memcpy(&buffer[KFSW_FTP_PROTOCOL_HEADER_SIZE + message->path_size], message->data,
		       message->data_size);
	}
	*encoded_size = size;
	return 0;
}

int kfsw_ftp_protocol_decode(const uint8_t *buffer, size_t size, struct kfsw_ftp_message *message)
{
	size_t expected_size;

	if ((buffer == NULL) || (message == NULL) || (size < KFSW_FTP_PROTOCOL_HEADER_SIZE)) {
		return -EMSGSIZE;
	}
	if (buffer[0] != KFSW_FTP_PROTOCOL_VERSION) {
		return -EPROTONOSUPPORT;
	}
	if ((buffer[1] < KFSW_FTP_OP_MKDIR_REQUEST) || (buffer[1] > KFSW_FTP_OP_GET_RESULT)) {
		return -ENOTSUP;
	}

	memset(message, 0, sizeof(*message));
	message->opcode = buffer[1];
	message->flags = buffer[2];
	message->status = buffer[3];
	message->request_id = sys_get_be32(&buffer[4]);
	message->offset = sys_get_be32(&buffer[8]);
	message->total_size = sys_get_be32(&buffer[12]);
	message->crc32 = sys_get_be32(&buffer[16]);
	message->path_size = sys_get_be16(&buffer[20]);
	message->data_size = sys_get_be16(&buffer[22]);
	if ((message->path_size > KFSW_FTP_MAX_PATH_SIZE) ||
	    (message->data_size > KFSW_FTP_CHUNK_SIZE)) {
		return -EMSGSIZE;
	}

	expected_size = KFSW_FTP_PROTOCOL_HEADER_SIZE + message->path_size + message->data_size;
	if (expected_size != size) {
		return -EMSGSIZE;
	}
	message->path = &buffer[KFSW_FTP_PROTOCOL_HEADER_SIZE];
	message->data = &buffer[KFSW_FTP_PROTOCOL_HEADER_SIZE + message->path_size];
	if ((message->path_size != 0U) &&
	    (memchr(message->path, '\0', message->path_size) != NULL)) {
		return -EBADMSG;
	}
	return 0;
}

int kfsw_ftp_resolve_path(const char *virtual_path, bool allow_root, char *resolved,
			  size_t resolved_size)
{
	const char *relative;
	size_t component_size = 0U;
	size_t relative_size;
	size_t root_size = sizeof(KFSW_FTP_ROOT_PATH) - 1U;

	if ((virtual_path == NULL) || (resolved == NULL)) {
		return -EINVAL;
	}
	relative_size = strnlen(virtual_path, KFSW_FTP_MAX_PATH_SIZE + 1U);
	if (relative_size > KFSW_FTP_MAX_PATH_SIZE) {
		return -ENAMETOOLONG;
	}
	relative = virtual_path;
	if ((relative_size != 0U) && (relative[0] == '/')) {
		relative++;
		relative_size--;
	}
	if (relative_size == 0U) {
		if (!allow_root || (resolved_size <= root_size)) {
			return -EINVAL;
		}
		memcpy(resolved, KFSW_FTP_ROOT_PATH, root_size + 1U);
		return 0;
	}
	if (resolved_size <= root_size + 1U + relative_size) {
		return -ENAMETOOLONG;
	}

	for (size_t index = 0U; index <= relative_size; index++) {
		const bool at_end = index == relative_size;
		const unsigned char character = at_end ? '/' : (unsigned char)relative[index];

		if (character == '/') {
			const size_t component_start = index - component_size;

			if ((component_size == 0U) ||
			    ((component_size == 1U) && (relative[component_start] == '.')) ||
			    ((component_size == 2U) && (relative[component_start] == '.') &&
			     (relative[component_start + 1U] == '.'))) {
				return -EINVAL;
			}
			component_size = 0U;
			continue;
		}
		if ((character == '\\') || (character < 0x20U) || (character == 0x7fU)) {
			return -EINVAL;
		}
		component_size++;
	}

	memcpy(resolved, KFSW_FTP_ROOT_PATH, root_size);
	resolved[root_size] = '/';
	memcpy(&resolved[root_size + 1U], relative, relative_size);
	resolved[root_size + 1U + relative_size] = '\0';
	return 0;
}

int kfsw_ftp_validate_path(const char *path, bool allow_root)
{
	char resolved[KFSW_FTP_FULL_PATH_SIZE];

	return kfsw_ftp_resolve_path(path, allow_root, resolved, sizeof(resolved));
}

int kfsw_ftp_wire_status_to_errno(uint8_t status)
{
	switch (status) {
	case KFSW_FTP_STATUS_OK:
		return 0;
	case KFSW_FTP_STATUS_INVALID_REQUEST:
		return -EBADMSG;
	case KFSW_FTP_STATUS_INVALID_PATH:
		return -EINVAL;
	case KFSW_FTP_STATUS_NOT_FOUND:
		return -ENOENT;
	case KFSW_FTP_STATUS_ALREADY_EXISTS:
		return -EEXIST;
	case KFSW_FTP_STATUS_NO_SPACE:
		return -ENOSPC;
	case KFSW_FTP_STATUS_INTEGRITY_ERROR:
		return -EILSEQ;
	case KFSW_FTP_STATUS_BUSY:
		return -EBUSY;
	case KFSW_FTP_STATUS_UNSUPPORTED:
		return -ENOTSUP;
	case KFSW_FTP_STATUS_TIMEOUT:
		return -ETIMEDOUT;
	case KFSW_FTP_STATUS_CONNECTION_ERROR:
		return -ECONNREFUSED;
	case KFSW_FTP_STATUS_NOT_DIRECTORY:
		return -ENOTDIR;
	case KFSW_FTP_STATUS_IO_ERROR:
	default:
		return -EIO;
	}
}

uint8_t kfsw_ftp_errno_to_wire_status(int error)
{
	const int positive_error = (error < 0) ? -error : error;

	switch (positive_error) {
	case 0:
		return KFSW_FTP_STATUS_OK;
	case EINVAL:
	case ENAMETOOLONG:
		return KFSW_FTP_STATUS_INVALID_PATH;
	case ENOENT:
		return KFSW_FTP_STATUS_NOT_FOUND;
	case EEXIST:
		return KFSW_FTP_STATUS_ALREADY_EXISTS;
	case ENOSPC:
		return KFSW_FTP_STATUS_NO_SPACE;
	case EILSEQ:
	case EBADMSG:
		return KFSW_FTP_STATUS_INTEGRITY_ERROR;
	case EBUSY:
		return KFSW_FTP_STATUS_BUSY;
	case ENOTSUP:
	case EPROTONOSUPPORT:
		return KFSW_FTP_STATUS_UNSUPPORTED;
	case ETIMEDOUT:
		return KFSW_FTP_STATUS_TIMEOUT;
	case ECONNREFUSED:
	case ECONNRESET:
		return KFSW_FTP_STATUS_CONNECTION_ERROR;
	case EISDIR:
	case ENOTDIR:
		return KFSW_FTP_STATUS_NOT_DIRECTORY;
	default:
		return KFSW_FTP_STATUS_IO_ERROR;
	}
}
