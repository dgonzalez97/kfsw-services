#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>

#include "ftp_internal.h"

BUILD_ASSERT(KFSW_FTP_PROTOCOL_HEADER_SIZE + KFSW_FTP_CHUNK_SIZE <= CSP_BUFFER_SIZE,
	     "FTP protocol messages must fit in one CSP packet");

int kfsw_ftp_send_message(csp_conn_t *connection, const struct kfsw_ftp_message *message)
{
	csp_packet_t *packet;
	size_t encoded_size;
	int result = 0;

	if ((connection == NULL) || (message == NULL)) {
		return -EINVAL;
	}
	packet = csp_buffer_get(KFSW_FTP_PROTOCOL_HEADER_SIZE + message->path_size +
				message->data_size);
	if (packet == NULL) {
		return -ENOMEM;
	}
	result = kfsw_ftp_protocol_encode(packet->data, CSP_BUFFER_SIZE, message, &encoded_size);
	if (result != 0) {
		csp_buffer_free(packet);
		return result;
	}
	packet->length = encoded_size;
	csp_send(connection, packet);
	return 0;
}

int kfsw_ftp_receive_message(csp_conn_t *connection, struct kfsw_ftp_message *message,
			     csp_packet_t **packet)
{
	int result;

	if ((connection == NULL) || (message == NULL) || (packet == NULL)) {
		return -EINVAL;
	}
	*packet = csp_read(connection, CONFIG_KFSW_FTP_TIMEOUT_MS);
	if (*packet == NULL) {
		return -ETIMEDOUT;
	}
	result = kfsw_ftp_protocol_decode((*packet)->data, (*packet)->length, message);
	if (result != 0) {
		csp_buffer_free(*packet);
		*packet = NULL;
	}
	return result;
}

int kfsw_ftp_copy_message_path(const struct kfsw_ftp_message *message, char *path, size_t path_size)
{
	if ((message == NULL) || (path == NULL) || (message->path_size == 0U) ||
	    (message->path_size >= path_size)) {
		return -EINVAL;
	}
	memcpy(path, message->path, message->path_size);
	path[message->path_size] = '\0';
	return 0;
}

int kfsw_ftp_file_crc(const char *path, struct kfsw_ftp_workspace *workspace, uint32_t *file_size,
		      uint32_t *crc32)
{
	struct fs_file_t file;
	uint32_t size = 0U;
	uint32_t crc = 0U;
	ssize_t bytes_read;
	int close_result;
	int result;

	if ((path == NULL) || (workspace == NULL) || (file_size == NULL) || (crc32 == NULL)) {
		return -EINVAL;
	}
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result != 0) {
		return result;
	}
	for (;;) {
		bytes_read = fs_read(&file, workspace->chunk, sizeof(workspace->chunk));
		if (bytes_read < 0) {
			result = (int)bytes_read;
			break;
		}
		if (bytes_read == 0) {
			break;
		}
		if ((uint32_t)bytes_read > UINT32_MAX - size) {
			result = -EFBIG;
			break;
		}
		size += (uint32_t)bytes_read;
		crc = crc32_ieee_update(crc, workspace->chunk, (size_t)bytes_read);
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		*file_size = size;
		*crc32 = crc;
	}
	return result;
}

int kfsw_ftp_make_temporary_path(const char *path, char *temporary_path, size_t temporary_path_size)
{
	static const char suffix[] = ".part";
	size_t path_size;

	if ((path == NULL) || (temporary_path == NULL)) {
		return -EINVAL;
	}
	path_size = strnlen(path, temporary_path_size);
	if ((path_size == temporary_path_size) ||
	    (path_size + sizeof(suffix) > temporary_path_size)) {
		return -ENAMETOOLONG;
	}
	memcpy(temporary_path, path, path_size);
	memcpy(&temporary_path[path_size], suffix, sizeof(suffix));
	return 0;
}

int kfsw_ftp_commit_temporary(const char *path, const char *temporary_path, uint32_t actual_size,
			      uint32_t actual_crc32, uint32_t expected_size,
			      uint32_t expected_crc32)
{
	int result;

	if ((path == NULL) || (temporary_path == NULL)) {
		return -EINVAL;
	}
	if ((actual_size != expected_size) || (actual_crc32 != expected_crc32)) {
		result = -EILSEQ;
	} else {
		result = fs_rename(temporary_path, path);
	}
	if (result != 0) {
		(void)fs_unlink(temporary_path);
	}
	return result;
}
