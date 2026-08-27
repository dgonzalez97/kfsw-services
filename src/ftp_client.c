#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_id.h>

#include <kfsw/comms/csp.h>
#include <kfsw/platform/storage.h>
#include <kfsw/services/ftp.h>

#include "ftp_internal.h"

K_MUTEX_DEFINE(kfsw_ftp_client_lock);

static atomic_t next_request_id;
static struct kfsw_ftp_workspace client_workspace;

static int validate_client(uint16_t node)
{
	struct kfsw_csp_info csp_info;
	const unsigned int host_bits = csp_id_get_host_bits();

	kfsw_csp_get_info(&csp_info);
	if (!kfsw_storage_is_ready() || !csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}
	return (node < (1UL << host_bits)) ? 0 : -EINVAL;
}

static uint32_t allocate_request_id(void)
{
	uint32_t request_id = (uint32_t)atomic_inc(&next_request_id) + 1U;

	return (request_id == 0U) ? (uint32_t)atomic_inc(&next_request_id) + 1U : request_id;
}

static csp_conn_t *connect_remote(uint16_t node)
{
	return csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_FTP_CSP_PORT,
			   CONFIG_KFSW_FTP_TIMEOUT_MS, CSP_O_RDP | CSP_O_CRC32);
}

static int validate_virtual_path(const char *path, bool allow_root, char *scratch,
				 size_t scratch_size, uint16_t *path_size)
{
	size_t size;
	int result;

	if ((path == NULL) || (path_size == NULL)) {
		return -EINVAL;
	}
	result = kfsw_ftp_resolve_path(path, allow_root, scratch, scratch_size);
	if (result != 0) {
		return result;
	}
	size = strnlen(path, KFSW_FTP_MAX_PATH_SIZE + 1U);
	if (size > KFSW_FTP_MAX_PATH_SIZE) {
		return -ENAMETOOLONG;
	}
	*path_size = (uint16_t)size;
	return 0;
}

static int receive_response(csp_conn_t *connection, uint8_t expected_opcode, uint32_t request_id,
			    struct kfsw_ftp_message *response, csp_packet_t **packet)
{
	int result = kfsw_ftp_receive_message(connection, response, packet);

	if (result != 0) {
		return result;
	}
	if ((response->opcode != expected_opcode) || (response->request_id != request_id)) {
		csp_buffer_free(*packet);
		*packet = NULL;
		return -EBADMSG;
	}
	result = kfsw_ftp_wire_status_to_errno(response->status);
	if (result != 0) {
		csp_buffer_free(*packet);
		*packet = NULL;
	}
	return result;
}

static int simple_path_request(uint16_t node, const char *path, bool allow_root,
			       uint8_t request_opcode, uint8_t response_opcode,
			       struct kfsw_ftp_message *response_out)
{
	struct kfsw_ftp_message response;
	struct kfsw_ftp_message request = {
		.opcode = request_opcode,
		.request_id = allocate_request_id(),
		.path = (const uint8_t *)path,
	};
	csp_packet_t *packet = NULL;
	csp_conn_t *connection = NULL;
	int result;

	result = validate_client(node);
	if (result != 0) {
		return result;
	}
	result = validate_virtual_path(path, allow_root, client_workspace.path,
				       sizeof(client_workspace.path), &request.path_size);
	if (result != 0) {
		return result;
	}
	connection = connect_remote(node);
	if (connection == NULL) {
		return -ECONNREFUSED;
	}
	result = kfsw_ftp_send_message(connection, &request);
	if (result == 0) {
		result = receive_response(connection, response_opcode, request.request_id,
					  &response, &packet);
	}
	if ((result == 0) && (response_out != NULL)) {
		*response_out = response;
	}
	if (packet != NULL) {
		csp_buffer_free(packet);
	}
	(void)csp_close(connection);
	return result;
}

int kfsw_ftp_mkdir(uint16_t node, const char *path)
{
	int result;

	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	result = simple_path_request(node, path, false, KFSW_FTP_OP_MKDIR_REQUEST,
				     KFSW_FTP_OP_MKDIR_RESPONSE, NULL);
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

int kfsw_ftp_stat(uint16_t node, const char *path, struct kfsw_ftp_stat *info)
{
	struct kfsw_ftp_message response;
	int result;

	if (info == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	result = simple_path_request(node, path, true, KFSW_FTP_OP_STAT_REQUEST,
				     KFSW_FTP_OP_STAT_RESPONSE, &response);
	if (result == 0) {
		if ((response.path_size != 0U) || (response.data_size != 0U) ||
		    ((response.flags != KFSW_FTP_ENTRY_FILE) &&
		     (response.flags != KFSW_FTP_ENTRY_DIRECTORY))) {
			result = -EBADMSG;
		} else {
			info->type = (enum kfsw_ftp_entry_type)response.flags;
			info->size = response.total_size;
			info->crc32 = response.crc32;
		}
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

int kfsw_ftp_list(uint16_t node, const char *path, kfsw_ftp_list_visitor_t visitor, void *context)
{
	struct kfsw_ftp_message request = {
		.opcode = KFSW_FTP_OP_LIST_REQUEST,
		.request_id = allocate_request_id(),
		.path = (const uint8_t *)path,
	};
	csp_conn_t *connection = NULL;
	bool keep_visiting = true;
	int result;

	if (visitor == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	result = validate_client(node);
	if (result == 0) {
		result = validate_virtual_path(path, true, client_workspace.path,
					       sizeof(client_workspace.path), &request.path_size);
	}
	if (result == 0) {
		connection = connect_remote(node);
		result = (connection != NULL) ? 0 : -ECONNREFUSED;
	}
	if (result == 0) {
		result = kfsw_ftp_send_message(connection, &request);
	}
	while (result == 0) {
		struct kfsw_ftp_message response;
		csp_packet_t *packet = NULL;

		result = kfsw_ftp_receive_message(connection, &response, &packet);
		if (result != 0) {
			break;
		}
		if (response.request_id != request.request_id) {
			result = -EBADMSG;
		} else if (response.opcode == KFSW_FTP_OP_LIST_END) {
			result = kfsw_ftp_wire_status_to_errno(response.status);
			csp_buffer_free(packet);
			break;
		} else if ((response.opcode != KFSW_FTP_OP_LIST_ENTRY) ||
			   (response.status != KFSW_FTP_STATUS_OK) || (response.path_size == 0U) ||
			   (response.data_size != 0U) ||
			   ((response.flags != KFSW_FTP_ENTRY_FILE) &&
			    (response.flags != KFSW_FTP_ENTRY_DIRECTORY))) {
			result = -EBADMSG;
		} else if (keep_visiting) {
			char name[KFSW_FTP_MAX_PATH_SIZE + 1U];
			const struct kfsw_ftp_entry entry = {
				.name = name,
				.type = (enum kfsw_ftp_entry_type)response.flags,
				.size = response.total_size,
			};

			memcpy(name, response.path, response.path_size);
			name[response.path_size] = '\0';
			keep_visiting = visitor(&entry, context);
		}
		csp_buffer_free(packet);
	}
	if (connection != NULL) {
		(void)csp_close(connection);
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

static int open_local_source(const char *local_path, const char *remote_path,
			     struct kfsw_ftp_message *request, uint32_t *file_size, uint32_t *crc32)
{
	uint16_t remote_path_size;
	int result;

	result = validate_virtual_path(local_path, false, client_workspace.path,
				       sizeof(client_workspace.path), &remote_path_size);
	if (result != 0) {
		return result;
	}
	result = validate_virtual_path(remote_path, false, client_workspace.temporary_path,
				       sizeof(client_workspace.temporary_path), &remote_path_size);
	if (result != 0) {
		return result;
	}
	result = kfsw_ftp_file_crc(client_workspace.path, &client_workspace, file_size, crc32);
	if (result != 0) {
		return result;
	}
	request->path = (const uint8_t *)remote_path;
	request->path_size = remote_path_size;
	request->total_size = *file_size;
	request->crc32 = *crc32;
	return 0;
}

int kfsw_ftp_put(uint16_t node, const char *local_path, const char *remote_path,
		 struct kfsw_ftp_transfer_result *transfer_result)
{
	struct kfsw_ftp_message request = {
		.opcode = KFSW_FTP_OP_PUT_REQUEST,
		.request_id = allocate_request_id(),
	};
	struct fs_file_t file;
	csp_conn_t *connection = NULL;
	uint32_t file_size = 0U;
	uint32_t crc32 = 0U;
	uint32_t offset = 0U;
	uint32_t started_ms;
	int close_result;
	int result;

	if (transfer_result == NULL) {
		return -EINVAL;
	}
	memset(transfer_result, 0, sizeof(*transfer_result));
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	started_ms = k_uptime_get_32();
	result = validate_client(node);
	if (result == 0) {
		result = open_local_source(local_path, remote_path, &request, &file_size, &crc32);
	}
	if (result == 0) {
		connection = connect_remote(node);
		result = (connection != NULL) ? 0 : -ECONNREFUSED;
	}
	if (result == 0) {
		result = kfsw_ftp_send_message(connection, &request);
	}
	if (result == 0) {
		struct kfsw_ftp_message response;
		csp_packet_t *packet = NULL;

		result = kfsw_ftp_receive_message(connection, &response, &packet);
		if (result == 0) {
			if ((response.request_id != request.request_id) ||
			    ((response.opcode != KFSW_FTP_OP_PUT_READY) &&
			     (response.opcode != KFSW_FTP_OP_PUT_RESULT))) {
				result = -EBADMSG;
			} else {
				result = kfsw_ftp_wire_status_to_errno(response.status);
				if ((result == 0) && (response.opcode != KFSW_FTP_OP_PUT_READY)) {
					result = -EBADMSG;
				}
			}
			csp_buffer_free(packet);
		}
	}
	fs_file_t_init(&file);
	if (result == 0) {
		result = fs_open(&file, client_workspace.path, FS_O_READ);
	}
	while ((result == 0) && (offset < file_size)) {
		ssize_t bytes_read =
			fs_read(&file, client_workspace.chunk, sizeof(client_workspace.chunk));
		struct kfsw_ftp_message data_message = {
			.opcode = KFSW_FTP_OP_PUT_DATA,
			.request_id = request.request_id,
			.offset = offset,
			.total_size = file_size,
			.crc32 = crc32,
			.data = client_workspace.chunk,
		};

		if (bytes_read <= 0) {
			result = (bytes_read < 0) ? (int)bytes_read : -EIO;
			break;
		}
		data_message.data_size = (uint16_t)bytes_read;
		result = kfsw_ftp_send_message(connection, &data_message);
		offset += (result == 0) ? (uint32_t)bytes_read : 0U;
	}
	if (file.mp != NULL) {
		close_result = fs_close(&file);
		if (result == 0) {
			result = close_result;
		}
	}
	if (result == 0) {
		struct kfsw_ftp_message response;
		csp_packet_t *packet = NULL;

		result = receive_response(connection, KFSW_FTP_OP_PUT_RESULT, request.request_id,
					  &response, &packet);
		if ((result == 0) &&
		    ((response.total_size != file_size) || (response.crc32 != crc32))) {
			result = -EILSEQ;
		}
		if (packet != NULL) {
			csp_buffer_free(packet);
		}
	}
	if (connection != NULL) {
		(void)csp_close(connection);
	}
	if (result == 0) {
		transfer_result->bytes = file_size;
		transfer_result->crc32 = crc32;
		transfer_result->duration_ms = k_uptime_get_32() - started_ms;
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

static int prepare_local_download(const char *remote_path, const char *local_path,
				  struct kfsw_ftp_message *request)
{
	struct fs_dirent entry;
	uint16_t local_path_size;
	uint16_t path_size;
	int result;

	result = validate_virtual_path(remote_path, false, client_workspace.temporary_path,
				       sizeof(client_workspace.temporary_path), &path_size);
	if (result != 0) {
		return result;
	}
	result = validate_virtual_path(local_path, false, client_workspace.path,
				       sizeof(client_workspace.path), &local_path_size);
	if (result != 0) {
		return result;
	}
	request->path = (const uint8_t *)remote_path;
	request->path_size = path_size;
	result = fs_stat(client_workspace.path, &entry);
	if ((result == 0) && (entry.type == FS_DIR_ENTRY_DIR)) {
		return -EISDIR;
	}
	if ((result != 0) && (result != -ENOENT)) {
		return result;
	}
	return kfsw_ftp_make_temporary_path(client_workspace.path, client_workspace.temporary_path,
					    sizeof(client_workspace.temporary_path));
}

static int write_all(struct fs_file_t *file, const uint8_t *data, size_t size)
{
	size_t offset = 0U;

	while (offset < size) {
		ssize_t written = fs_write(file, &data[offset], size - offset);

		if (written < 0) {
			return (int)written;
		}
		if (written == 0) {
			return -EIO;
		}
		offset += (size_t)written;
	}
	return 0;
}

int kfsw_ftp_get(uint16_t node, const char *remote_path, const char *local_path,
		 struct kfsw_ftp_transfer_result *transfer_result)
{
	struct kfsw_ftp_message request = {
		.opcode = KFSW_FTP_OP_GET_REQUEST,
		.request_id = allocate_request_id(),
	};
	struct fs_file_t file;
	csp_conn_t *connection = NULL;
	uint32_t expected_size = 0U;
	uint32_t expected_crc = 0U;
	uint32_t received = 0U;
	uint32_t crc32 = 0U;
	uint32_t started_ms;
	bool temporary_ready = false;
	int close_result;
	int result;

	if (transfer_result == NULL) {
		return -EINVAL;
	}
	memset(transfer_result, 0, sizeof(*transfer_result));
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	started_ms = k_uptime_get_32();
	result = validate_client(node);
	if (result == 0) {
		result = prepare_local_download(remote_path, local_path, &request);
		temporary_ready = result == 0;
	}
	if (result == 0) {
		connection = connect_remote(node);
		result = (connection != NULL) ? 0 : -ECONNREFUSED;
	}
	if (result == 0) {
		result = kfsw_ftp_send_message(connection, &request);
	}
	if (result == 0) {
		struct kfsw_ftp_message response;
		csp_packet_t *packet = NULL;

		result = kfsw_ftp_receive_message(connection, &response, &packet);
		if (result == 0) {
			if ((response.request_id != request.request_id) ||
			    ((response.opcode != KFSW_FTP_OP_GET_INFO) &&
			     (response.opcode != KFSW_FTP_OP_GET_RESULT))) {
				result = -EBADMSG;
			} else {
				result = kfsw_ftp_wire_status_to_errno(response.status);
				if ((result == 0) && (response.opcode == KFSW_FTP_OP_GET_INFO)) {
					expected_size = response.total_size;
					expected_crc = response.crc32;
				} else if (result == 0) {
					result = -EBADMSG;
				}
			}
			csp_buffer_free(packet);
		}
	}
	fs_file_t_init(&file);
	if (result == 0) {
		(void)fs_unlink(client_workspace.temporary_path);
		result = fs_open(&file, client_workspace.temporary_path,
				 FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	}
	while ((result == 0) && (received < expected_size)) {
		struct kfsw_ftp_message response;
		csp_packet_t *packet = NULL;

		result = kfsw_ftp_receive_message(connection, &response, &packet);
		if (result != 0) {
			break;
		}
		if ((response.opcode != KFSW_FTP_OP_GET_DATA) ||
		    (response.request_id != request.request_id) || (response.status != 0U) ||
		    (response.flags != 0U) || (response.path_size != 0U) ||
		    (response.data_size == 0U) || (response.offset != received) ||
		    (response.total_size != expected_size) || (response.crc32 != expected_crc) ||
		    (response.data_size > expected_size - received)) {
			result = -EBADMSG;
		} else {
			result = write_all(&file, response.data, response.data_size);
			if (result == 0) {
				crc32 = crc32_ieee_update(crc32, response.data, response.data_size);
				received += response.data_size;
			}
		}
		csp_buffer_free(packet);
	}
	if (result == 0) {
		struct kfsw_ftp_message response;
		csp_packet_t *packet = NULL;

		result = receive_response(connection, KFSW_FTP_OP_GET_RESULT, request.request_id,
					  &response, &packet);
		if ((result == 0) &&
		    ((response.total_size != expected_size) || (response.crc32 != expected_crc))) {
			result = -EILSEQ;
		}
		if (packet != NULL) {
			csp_buffer_free(packet);
		}
	}
	if (result == 0) {
		result = fs_sync(&file);
	}
	if (file.mp != NULL) {
		close_result = fs_close(&file);
		if (result == 0) {
			result = close_result;
		}
	}
	if (result == 0) {
		result = kfsw_ftp_commit_temporary(client_workspace.path,
						   client_workspace.temporary_path, received, crc32,
						   expected_size, expected_crc);
	}
	if ((result != 0) && temporary_ready) {
		(void)fs_unlink(client_workspace.temporary_path);
	}
	if (connection != NULL) {
		(void)csp_close(connection);
	}
	if (result == 0) {
		transfer_result->bytes = received;
		transfer_result->crc32 = crc32;
		transfer_result->duration_ms = k_uptime_get_32() - started_ms;
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}
