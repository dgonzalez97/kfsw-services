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

#include <kfsw/comms/csp.h>
#include <kfsw/platform/storage.h>
#include <kfsw/services/ftp.h>

#include "ftp_internal.h"

#define KFSW_FTP_SERVER_POLL_MS 100U

static bool initialized;
static bool threads_started;
static csp_socket_t server_socket;
static atomic_t server_started;
static atomic_t server_busy;
static struct kfsw_ftp_workspace server_workspace;

K_MSGQ_DEFINE(kfsw_ftp_connection_queue, sizeof(csp_conn_t *), 1, sizeof(void *));

static uint8_t response_opcode(uint8_t request_opcode)
{
	switch (request_opcode) {
	case KFSW_FTP_OP_MKDIR_REQUEST:
		return KFSW_FTP_OP_MKDIR_RESPONSE;
	case KFSW_FTP_OP_LIST_REQUEST:
		return KFSW_FTP_OP_LIST_END;
	case KFSW_FTP_OP_STAT_REQUEST:
		return KFSW_FTP_OP_STAT_RESPONSE;
	case KFSW_FTP_OP_PUT_REQUEST:
		return KFSW_FTP_OP_PUT_RESULT;
	case KFSW_FTP_OP_GET_REQUEST:
		return KFSW_FTP_OP_GET_RESULT;
	default:
		return KFSW_FTP_OP_GET_RESULT;
	}
}

static int send_status(csp_conn_t *connection, uint8_t opcode, uint32_t request_id, uint8_t status,
		       uint32_t total_size, uint32_t crc32)
{
	const struct kfsw_ftp_message response = {
		.opcode = opcode,
		.status = status,
		.request_id = request_id,
		.total_size = total_size,
		.crc32 = crc32,
	};

	return kfsw_ftp_send_message(connection, &response);
}

static int resolve_message_path(const struct kfsw_ftp_message *request, bool allow_root)
{
	char virtual_path[KFSW_FTP_MAX_PATH_SIZE + 1U];
	int result;

	result = kfsw_ftp_copy_message_path(request, virtual_path, sizeof(virtual_path));
	if (result != 0) {
		return result;
	}
	return kfsw_ftp_resolve_path(virtual_path, allow_root, server_workspace.path,
				     sizeof(server_workspace.path));
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

static int serve_mkdir(csp_conn_t *connection, const struct kfsw_ftp_message *request)
{
	int result;

	if ((request->data_size != 0U) || (request->status != 0U) || (request->flags != 0U) ||
	    (request->offset != 0U) || (request->total_size != 0U) || (request->crc32 != 0U)) {
		result = -EBADMSG;
	} else {
		result = resolve_message_path(request, false);
	}
	if (result == 0) {
		result = fs_mkdir(server_workspace.path);
	}
	return send_status(connection, KFSW_FTP_OP_MKDIR_RESPONSE, request->request_id,
			   (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
						: kfsw_ftp_errno_to_wire_status(result),
			   0U, 0U);
}

static int serve_stat(csp_conn_t *connection, const struct kfsw_ftp_message *request)
{
	struct fs_dirent entry;
	uint32_t crc32 = 0U;
	uint32_t size = 0U;
	uint8_t flags = 0U;
	int result;

	if ((request->data_size != 0U) || (request->status != 0U) || (request->flags != 0U) ||
	    (request->offset != 0U) || (request->total_size != 0U) || (request->crc32 != 0U)) {
		result = -EBADMSG;
	} else {
		result = resolve_message_path(request, true);
	}
	if (result == 0) {
		result = fs_stat(server_workspace.path, &entry);
	}
	if (result == 0) {
		if (entry.type == FS_DIR_ENTRY_DIR) {
			flags = KFSW_FTP_ENTRY_DIRECTORY;
		} else if (entry.type == FS_DIR_ENTRY_FILE) {
			flags = KFSW_FTP_ENTRY_FILE;
			result = kfsw_ftp_file_crc(server_workspace.path, &server_workspace, &size,
						   &crc32);
		} else {
			result = -ENOTSUP;
		}
	}

	const struct kfsw_ftp_message response = {
		.opcode = KFSW_FTP_OP_STAT_RESPONSE,
		.flags = flags,
		.status = (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
					       : kfsw_ftp_errno_to_wire_status(result),
		.request_id = request->request_id,
		.total_size = size,
		.crc32 = crc32,
	};
	return kfsw_ftp_send_message(connection, &response);
}

static int serve_list(csp_conn_t *connection, const struct kfsw_ftp_message *request)
{
	struct fs_dir_t directory;
	struct fs_dirent entry;
	int close_result;
	int result;

	if ((request->data_size != 0U) || (request->status != 0U) || (request->flags != 0U) ||
	    (request->offset != 0U) || (request->total_size != 0U) || (request->crc32 != 0U)) {
		result = -EBADMSG;
	} else {
		result = resolve_message_path(request, true);
	}
	fs_dir_t_init(&directory);
	if (result == 0) {
		result = fs_opendir(&directory, server_workspace.path);
	}
	while (result == 0) {
		size_t name_size;
		struct kfsw_ftp_message response;

		result = fs_readdir(&directory, &entry);
		if ((result != 0) || (entry.name[0] == '\0')) {
			break;
		}
		name_size = strnlen(entry.name, KFSW_FTP_MAX_PATH_SIZE + 1U);
		if (name_size > KFSW_FTP_MAX_PATH_SIZE) {
			result = -ENAMETOOLONG;
			break;
		}
		if ((entry.type == FS_DIR_ENTRY_FILE) && (entry.size > UINT32_MAX)) {
			result = -EFBIG;
			break;
		}
		memset(&response, 0, sizeof(response));
		response.opcode = KFSW_FTP_OP_LIST_ENTRY;
		response.flags = (entry.type == FS_DIR_ENTRY_DIR) ? KFSW_FTP_ENTRY_DIRECTORY
								  : KFSW_FTP_ENTRY_FILE;
		response.request_id = request->request_id;
		response.total_size = (entry.type == FS_DIR_ENTRY_FILE) ? (uint32_t)entry.size : 0U;
		response.path = (const uint8_t *)entry.name;
		response.path_size = (uint16_t)name_size;
		result = kfsw_ftp_send_message(connection, &response);
	}
	if (directory.mp != NULL) {
		close_result = fs_closedir(&directory);
		if (result == 0) {
			result = close_result;
		}
	}
	return send_status(connection, KFSW_FTP_OP_LIST_END, request->request_id,
			   (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
						: kfsw_ftp_errno_to_wire_status(result),
			   0U, 0U);
}

static int prepare_upload_target(void)
{
	struct fs_dirent entry;
	int result;

	result =
		kfsw_ftp_make_temporary_path(server_workspace.path, server_workspace.temporary_path,
					     sizeof(server_workspace.temporary_path));
	if (result != 0) {
		return result;
	}
	result = fs_stat(server_workspace.path, &entry);
	if ((result == 0) && (entry.type == FS_DIR_ENTRY_DIR)) {
		return -EISDIR;
	}
	if ((result != 0) && (result != -ENOENT)) {
		return result;
	}
	(void)fs_unlink(server_workspace.temporary_path);
	return 0;
}

static int serve_put(csp_conn_t *connection, const struct kfsw_ftp_message *request)
{
	struct fs_file_t file;
	uint32_t received = 0U;
	uint32_t crc32 = 0U;
	int close_result;
	int result;

	if ((request->data_size != 0U) || (request->status != 0U) || (request->flags != 0U) ||
	    (request->offset != 0U)) {
		result = -EBADMSG;
	} else {
		result = resolve_message_path(request, false);
	}
	if (result == 0) {
		result = prepare_upload_target();
	}
	fs_file_t_init(&file);
	if (result == 0) {
		result = fs_open(&file, server_workspace.temporary_path,
				 FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	}
	if (result != 0) {
		return send_status(connection, KFSW_FTP_OP_PUT_RESULT, request->request_id,
				   (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
							: kfsw_ftp_errno_to_wire_status(result),
				   0U, 0U);
	}

	result = send_status(connection, KFSW_FTP_OP_PUT_READY, request->request_id,
			     KFSW_FTP_STATUS_OK, request->total_size, request->crc32);
	while ((result == 0) && (received < request->total_size)) {
		struct kfsw_ftp_message data_message;
		csp_packet_t *packet = NULL;

		result = kfsw_ftp_receive_message(connection, &data_message, &packet);
		if (result != 0) {
			break;
		}
		if ((data_message.opcode != KFSW_FTP_OP_PUT_DATA) ||
		    (data_message.request_id != request->request_id) ||
		    (data_message.path_size != 0U) || (data_message.status != 0U) ||
		    (data_message.flags != 0U) || (data_message.data_size == 0U) ||
		    (data_message.offset != received) ||
		    (data_message.total_size != request->total_size) ||
		    (data_message.crc32 != request->crc32) ||
		    (data_message.data_size > request->total_size - received)) {
			result = -EBADMSG;
		} else {
			result = write_all(&file, data_message.data, data_message.data_size);
			if (result == 0) {
				crc32 = crc32_ieee_update(crc32, data_message.data,
							  data_message.data_size);
				received += data_message.data_size;
			}
		}
		csp_buffer_free(packet);
	}
	if (result == 0) {
		result = fs_sync(&file);
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	if (result == 0) {
		result = kfsw_ftp_commit_temporary(server_workspace.path,
						   server_workspace.temporary_path, received, crc32,
						   request->total_size, request->crc32);
	}
	if (result != 0) {
		(void)fs_unlink(server_workspace.temporary_path);
	}
	return send_status(connection, KFSW_FTP_OP_PUT_RESULT, request->request_id,
			   (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
						: kfsw_ftp_errno_to_wire_status(result),
			   received, crc32);
}

static int serve_get(csp_conn_t *connection, const struct kfsw_ftp_message *request)
{
	struct fs_dirent entry;
	struct fs_file_t file;
	uint32_t file_size = 0U;
	uint32_t crc32 = 0U;
	uint32_t offset = 0U;
	ssize_t bytes_read;
	int close_result;
	int result;

	if ((request->data_size != 0U) || (request->status != 0U) || (request->flags != 0U) ||
	    (request->offset != 0U) || (request->total_size != 0U) || (request->crc32 != 0U)) {
		result = -EBADMSG;
	} else {
		result = resolve_message_path(request, false);
	}
	if (result == 0) {
		result = fs_stat(server_workspace.path, &entry);
	}
	if ((result == 0) && (entry.type != FS_DIR_ENTRY_FILE)) {
		result = -EISDIR;
	}
	if (result == 0) {
		result = kfsw_ftp_file_crc(server_workspace.path, &server_workspace, &file_size,
					   &crc32);
	}
	fs_file_t_init(&file);
	if (result == 0) {
		result = fs_open(&file, server_workspace.path, FS_O_READ);
	}
	if (result != 0) {
		return send_status(connection, KFSW_FTP_OP_GET_RESULT, request->request_id,
				   (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
							: kfsw_ftp_errno_to_wire_status(result),
				   0U, 0U);
	}

	result = send_status(connection, KFSW_FTP_OP_GET_INFO, request->request_id,
			     KFSW_FTP_STATUS_OK, file_size, crc32);
	while ((result == 0) && (offset < file_size)) {
		struct kfsw_ftp_message data_message = {
			.opcode = KFSW_FTP_OP_GET_DATA,
			.request_id = request->request_id,
			.offset = offset,
			.total_size = file_size,
			.crc32 = crc32,
			.data = server_workspace.chunk,
		};

		bytes_read = fs_read(&file, server_workspace.chunk, sizeof(server_workspace.chunk));
		if (bytes_read <= 0) {
			result = (bytes_read < 0) ? (int)bytes_read : -EIO;
			break;
		}
		data_message.data_size = (uint16_t)bytes_read;
		result = kfsw_ftp_send_message(connection, &data_message);
		offset += (result == 0) ? (uint32_t)bytes_read : 0U;
	}
	close_result = fs_close(&file);
	if (result == 0) {
		result = close_result;
	}
	return send_status(connection, KFSW_FTP_OP_GET_RESULT, request->request_id,
			   kfsw_ftp_errno_to_wire_status(result), offset, crc32);
}

static void serve_connection(csp_conn_t *connection)
{
	struct kfsw_ftp_message request;
	csp_packet_t *packet = NULL;
	int result;

	result = kfsw_ftp_receive_message(connection, &request, &packet);
	if (result != 0) {
		(void)send_status(connection, KFSW_FTP_OP_GET_RESULT, 0U,
				  ((result == -EPROTONOSUPPORT) || (result == -ENOTSUP))
					  ? KFSW_FTP_STATUS_UNSUPPORTED
					  : KFSW_FTP_STATUS_INVALID_REQUEST,
				  0U, 0U);
		return;
	}
	switch (request.opcode) {
	case KFSW_FTP_OP_MKDIR_REQUEST:
		(void)serve_mkdir(connection, &request);
		break;
	case KFSW_FTP_OP_LIST_REQUEST:
		(void)serve_list(connection, &request);
		break;
	case KFSW_FTP_OP_STAT_REQUEST:
		(void)serve_stat(connection, &request);
		break;
	case KFSW_FTP_OP_PUT_REQUEST:
		(void)serve_put(connection, &request);
		break;
	case KFSW_FTP_OP_GET_REQUEST:
		(void)serve_get(connection, &request);
		break;
	default:
		(void)send_status(connection, response_opcode(request.opcode), request.request_id,
				  KFSW_FTP_STATUS_INVALID_REQUEST, 0U, 0U);
		break;
	}
	csp_buffer_free(packet);
}

static void ftp_worker(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection = NULL;

		if (k_msgq_get(&kfsw_ftp_connection_queue, &connection,
			       K_MSEC(KFSW_FTP_SERVER_POLL_MS)) != 0) {
			continue;
		}
		if (connection != NULL) {
			serve_connection(connection);
			(void)csp_close(connection);
		}
		atomic_clear(&server_busy);
	}
}

static void reject_busy_connection(csp_conn_t *connection)
{
	struct kfsw_ftp_message request;
	csp_packet_t *packet = NULL;

	if (kfsw_ftp_receive_message(connection, &request, &packet) == 0) {
		(void)send_status(connection, response_opcode(request.opcode), request.request_id,
				  KFSW_FTP_STATUS_BUSY, 0U, 0U);
		csp_buffer_free(packet);
	}
	(void)csp_close(connection);
}

static void ftp_acceptor(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection;

		if (atomic_get(&server_started) == 0) {
			k_sleep(K_MSEC(KFSW_FTP_SERVER_POLL_MS));
			continue;
		}
		connection = csp_accept(&server_socket, KFSW_FTP_SERVER_POLL_MS);
		if (connection == NULL) {
			continue;
		}
		if (!atomic_cas(&server_busy, 0, 1)) {
			reject_busy_connection(connection);
			continue;
		}
		if (k_msgq_put(&kfsw_ftp_connection_queue, &connection, K_NO_WAIT) != 0) {
			atomic_clear(&server_busy);
			reject_busy_connection(connection);
		}
	}
}

K_THREAD_DEFINE(kfsw_ftp_worker_thread, CONFIG_KFSW_FTP_WORKER_STACK_SIZE, ftp_worker, NULL, NULL,
		NULL, CONFIG_KFSW_FTP_WORKER_PRIORITY, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(kfsw_ftp_acceptor_thread, CONFIG_KFSW_FTP_ACCEPTOR_STACK_SIZE, ftp_acceptor, NULL,
		NULL, NULL, CONFIG_KFSW_FTP_ACCEPTOR_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_ftp_init(void)
{
	int result;

	if (initialized) {
		return 0;
	}
	if (!kfsw_storage_is_ready()) {
		return -EACCES;
	}
	result = fs_mkdir(KFSW_FTP_ROOT_PATH);
	if ((result != 0) && (result != -EEXIST)) {
		return result;
	}
	result = fs_mkdir(KFSW_FTP_EXCHANGE_PATH);
	if ((result != 0) && (result != -EEXIST)) {
		return result;
	}
	initialized = true;
	return 0;
}

int kfsw_ftp_start(void)
{
	struct kfsw_csp_info csp_info;
	int result;

	if (!initialized) {
		return -EACCES;
	}
	if (atomic_get(&server_started) != 0) {
		return 0;
	}
	kfsw_csp_get_info(&csp_info);
	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}
	memset(&server_socket, 0, sizeof(server_socket));
	server_socket.opts = CSP_SO_RDPREQ | CSP_SO_CRC32REQ;
	result = csp_listen(&server_socket, 1U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&server_socket);
		return -EIO;
	}
	result = csp_bind(&server_socket, CONFIG_KFSW_FTP_CSP_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&server_socket);
		return -EADDRINUSE;
	}
	atomic_set(&server_started, 1);
	if (!threads_started) {
		k_thread_start(kfsw_ftp_worker_thread);
		k_thread_start(kfsw_ftp_acceptor_thread);
		threads_started = true;
	}
	return 0;
}

int kfsw_ftp_stop(void)
{
	if (atomic_get(&server_started) == 0) {
		return 0;
	}
	if (atomic_get(&server_busy) != 0) {
		return -EBUSY;
	}
	atomic_clear(&server_started);
	k_sleep(K_MSEC(KFSW_FTP_SERVER_POLL_MS + 10U));
	(void)csp_socket_close(&server_socket);
	server_socket.rx_queue = NULL;
	return 0;
}

bool kfsw_ftp_is_started(void)
{
	return initialized && (atomic_get(&server_started) != 0);
}
