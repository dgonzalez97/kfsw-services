#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/ftp.h>

#include "ftp_link.h"

#define KFSW_FTP_SERVER_POLL_MS 100U

static bool initialized;
static bool threads_started;
static struct kfsw_ftp_listener server_listener;
static atomic_t server_started;
static atomic_t server_busy;
static struct kfsw_ftp_workspace server_workspace;

K_MSGQ_DEFINE(kfsw_ftp_connection_queue, sizeof(struct kfsw_ftp_link), 1, sizeof(void *));

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

static uint8_t wire_status(int result)
{
	return (result == -EBADMSG) ? KFSW_FTP_STATUS_INVALID_REQUEST
				    : kfsw_ftp_errno_to_wire_status(result);
}

static int send_status(struct kfsw_ftp_link *link, uint8_t opcode, uint32_t request_id,
		       uint8_t status, uint32_t total_size, uint32_t crc32)
{
	const struct kfsw_ftp_message response = {
		.opcode = opcode,
		.status = status,
		.request_id = request_id,
		.total_size = total_size,
		.crc32 = crc32,
	};

	return kfsw_ftp_link_send(link, &response);
}

/** A path-only request carries a path and leaves every payload field zero. */
static bool path_request_is_valid(const struct kfsw_ftp_message *request)
{
	return (request->data_size == 0U) && (request->status == 0U) && (request->flags == 0U) &&
	       (request->offset == 0U) && (request->total_size == 0U) && (request->crc32 == 0U);
}

static int copy_request_path(const struct kfsw_ftp_message *request, char *virtual_path,
			     size_t virtual_path_size)
{
	if (!path_request_is_valid(request)) {
		return -EBADMSG;
	}
	return kfsw_ftp_copy_message_path(request, virtual_path, virtual_path_size);
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

static int serve_mkdir(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *request)
{
	char virtual_path[KFSW_FTP_MAX_PATH_SIZE + 1U];
	int result = copy_request_path(request, virtual_path, sizeof(virtual_path));

	if (result == 0) {
		result = kfsw_ftp_local_mkdir(virtual_path, &server_workspace);
	}
	return send_status(link, KFSW_FTP_OP_MKDIR_RESPONSE, request->request_id,
			   wire_status(result), 0U, 0U);
}

static int serve_stat(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *request)
{
	char virtual_path[KFSW_FTP_MAX_PATH_SIZE + 1U];
	struct kfsw_ftp_stat info = {0};
	int result = copy_request_path(request, virtual_path, sizeof(virtual_path));

	if (result == 0) {
		result = kfsw_ftp_local_stat(virtual_path, &server_workspace, &info);
	}

	const struct kfsw_ftp_message response = {
		.opcode = KFSW_FTP_OP_STAT_RESPONSE,
		.flags = (result == 0) ? (uint8_t)info.type : 0U,
		.status = wire_status(result),
		.request_id = request->request_id,
		.total_size = info.size,
		.crc32 = info.crc32,
	};
	return kfsw_ftp_link_send(link, &response);
}

struct list_entry_sender {
	struct kfsw_ftp_link *link;
	uint32_t request_id;
	int result;
};

static bool send_list_entry(const struct kfsw_ftp_entry *entry, void *context)
{
	struct list_entry_sender *sender = context;
	struct kfsw_ftp_message response;

	memset(&response, 0, sizeof(response));
	response.opcode = KFSW_FTP_OP_LIST_ENTRY;
	response.flags = (uint8_t)entry->type;
	response.request_id = sender->request_id;
	response.total_size = entry->size;
	response.path = (const uint8_t *)entry->name;
	response.path_size = (uint16_t)strnlen(entry->name, KFSW_FTP_MAX_PATH_SIZE);
	sender->result = kfsw_ftp_link_send(sender->link, &response);
	return sender->result == 0;
}

static int serve_list(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *request)
{
	struct list_entry_sender sender = {
		.link = link,
		.request_id = request->request_id,
	};
	char virtual_path[KFSW_FTP_MAX_PATH_SIZE + 1U];
	int result = copy_request_path(request, virtual_path, sizeof(virtual_path));

	if (result == 0) {
		result = kfsw_ftp_local_list(virtual_path, &server_workspace, send_list_entry,
					     &sender);
	}
	if (sender.result != 0) {
		/* A failed entry send stops the walk; report it over any close error. */
		result = sender.result;
	}
	return send_status(link, KFSW_FTP_OP_LIST_END, request->request_id, wire_status(result), 0U,
			   0U);
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
	return 0;
}

static int serve_put(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *request)
{
	struct kfsw_ftp_transfer transfer = {
		.link = link,
		.workspace = &server_workspace,
		.request_id = request->request_id,
		.total_size = request->total_size,
		.crc32 = request->crc32,
		.data_opcode = KFSW_FTP_OP_PUT_DATA,
	};
	int result;

	if ((request->data_size != 0U) || (request->status != 0U) || (request->flags != 0U) ||
	    (request->offset != 0U)) {
		result = -EBADMSG;
	} else {
		result = resolve_message_path(request, false);
	}
#if CONFIG_KFSW_FWU
	/* An ordinary put to the reserved path is a firmware upload. The client
	 * already sends the size and CRC32 the update service needs, so this
	 * needs no protocol change and works with the existing client on both
	 * ends.
	 */
	if ((result == 0) && kfsw_ftp_path_is_firmware(request->path)) {
		result = kfsw_ftp_transfer_open_firmware_sink(&transfer);
		if (result != 0) {
			return send_status(link, KFSW_FTP_OP_PUT_RESULT, request->request_id,
					   wire_status(result), 0U, 0U);
		}
	}
	if ((result == 0) && !transfer.firmware) {
#else
	if (result == 0) {
#endif
		result = prepare_upload_target();
		if (result == 0) {
			result = kfsw_ftp_transfer_open_sink(&transfer,
							     server_workspace.temporary_path);
		}
	}
	if (result != 0) {
		return send_status(link, KFSW_FTP_OP_PUT_RESULT, request->request_id,
				   wire_status(result), 0U, 0U);
	}

	/* The temporary file exists from here on, so the client may start sending. */
	result = send_status(link, KFSW_FTP_OP_PUT_READY, request->request_id, KFSW_FTP_STATUS_OK,
			     request->total_size, request->crc32);
	if (result == 0) {
		result = kfsw_ftp_transfer_receive(&transfer);
	}
	result = kfsw_ftp_transfer_finish(&transfer, server_workspace.path,
					  server_workspace.temporary_path, result);
	return send_status(link, KFSW_FTP_OP_PUT_RESULT, request->request_id, wire_status(result),
			   transfer.offset, transfer.actual_crc32);
}

static int serve_get(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *request)
{
	struct kfsw_ftp_transfer transfer = {
		.link = link,
		.workspace = &server_workspace,
		.request_id = request->request_id,
		.data_opcode = KFSW_FTP_OP_GET_DATA,
	};
	struct fs_dirent entry;
	int result;

	result = path_request_is_valid(request) ? resolve_message_path(request, false) : -EBADMSG;
	if (result == 0) {
		result = fs_stat(server_workspace.path, &entry);
	}
	if ((result == 0) && (entry.type != FS_DIR_ENTRY_FILE)) {
		result = -EISDIR;
	}
	if (result == 0) {
		result = kfsw_ftp_file_crc(server_workspace.path, &server_workspace,
					   &transfer.total_size, &transfer.crc32);
	}
	if (result == 0) {
		result = kfsw_ftp_transfer_open_source(&transfer, server_workspace.path);
	}
	if (result != 0) {
		return send_status(link, KFSW_FTP_OP_GET_RESULT, request->request_id,
				   wire_status(result), 0U, 0U);
	}

	result = send_status(link, KFSW_FTP_OP_GET_INFO, request->request_id, KFSW_FTP_STATUS_OK,
			     transfer.total_size, transfer.crc32);
	if (result == 0) {
		result = kfsw_ftp_transfer_send(&transfer);
	} else {
		(void)fs_close(&transfer.file);
	}
	return send_status(link, KFSW_FTP_OP_GET_RESULT, request->request_id,
			   kfsw_ftp_errno_to_wire_status(result), transfer.offset, transfer.crc32);
}

static void serve_connection(struct kfsw_ftp_link *link)
{
	struct kfsw_ftp_link_frame frame;
	int result = kfsw_ftp_link_receive(link, &frame);

	if (result != 0) {
		(void)send_status(link, KFSW_FTP_OP_GET_RESULT, 0U,
				  ((result == -EPROTONOSUPPORT) || (result == -ENOTSUP))
					  ? KFSW_FTP_STATUS_UNSUPPORTED
					  : KFSW_FTP_STATUS_INVALID_REQUEST,
				  0U, 0U);
		return;
	}
	switch (frame.message.opcode) {
	case KFSW_FTP_OP_MKDIR_REQUEST:
		(void)serve_mkdir(link, &frame.message);
		break;
	case KFSW_FTP_OP_LIST_REQUEST:
		(void)serve_list(link, &frame.message);
		break;
	case KFSW_FTP_OP_STAT_REQUEST:
		(void)serve_stat(link, &frame.message);
		break;
	case KFSW_FTP_OP_PUT_REQUEST:
		(void)serve_put(link, &frame.message);
		break;
	case KFSW_FTP_OP_GET_REQUEST:
		(void)serve_get(link, &frame.message);
		break;
	default:
		(void)send_status(link, response_opcode(frame.message.opcode),
				  frame.message.request_id, KFSW_FTP_STATUS_INVALID_REQUEST, 0U,
				  0U);
		break;
	}
	kfsw_ftp_link_release(&frame);
}

static void ftp_worker(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		struct kfsw_ftp_link link;

		if (k_msgq_get(&kfsw_ftp_connection_queue, &link,
			       K_MSEC(KFSW_FTP_SERVER_POLL_MS)) != 0) {
			continue;
		}
		if (kfsw_ftp_link_is_open(&link)) {
			serve_connection(&link);
			kfsw_ftp_link_close(&link);
		}
		atomic_clear(&server_busy);
	}
}

static void reject_busy_connection(struct kfsw_ftp_link *link)
{
	struct kfsw_ftp_link_frame frame;

	if (kfsw_ftp_link_receive(link, &frame) == 0) {
		(void)send_status(link, response_opcode(frame.message.opcode),
				  frame.message.request_id, KFSW_FTP_STATUS_BUSY, 0U, 0U);
		kfsw_ftp_link_release(&frame);
	}
	kfsw_ftp_link_close(link);
}

static void ftp_acceptor(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		struct kfsw_ftp_link link;

		if (atomic_get(&server_started) == 0) {
			k_sleep(K_MSEC(KFSW_FTP_SERVER_POLL_MS));
			continue;
		}
		if (kfsw_ftp_link_accept(&server_listener, &link, KFSW_FTP_SERVER_POLL_MS) != 0) {
			continue;
		}
		if (!atomic_cas(&server_busy, 0, 1)) {
			reject_busy_connection(&link);
			continue;
		}
		/* A successful put transfers the connection to the worker. */
		if (k_msgq_put(&kfsw_ftp_connection_queue, &link, K_NO_WAIT) != 0) {
			atomic_clear(&server_busy);
			reject_busy_connection(&link);
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
	int result;

	if (!initialized) {
		return -EACCES;
	}
	if (atomic_get(&server_started) != 0) {
		return 0;
	}
	if (!kfsw_ftp_link_is_ready()) {
		return -EACCES;
	}
	result = kfsw_ftp_link_listen(&server_listener);
	if (result != 0) {
		return result;
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
	/* Let the acceptor leave its accept window before the endpoint goes away. */
	k_sleep(K_MSEC(KFSW_FTP_SERVER_POLL_MS + 10U));
	kfsw_ftp_link_listener_close(&server_listener);
	return 0;
}

bool kfsw_ftp_is_started(void)
{
	return initialized && (atomic_get(&server_started) != 0);
}
