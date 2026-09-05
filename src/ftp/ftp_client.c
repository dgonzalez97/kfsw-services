#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/ftp.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

#include "ftp_link.h"

K_MUTEX_DEFINE(kfsw_ftp_client_lock);

static atomic_t next_request_id;
static struct kfsw_ftp_workspace client_workspace;

static int validate_client(uint16_t node)
{
	if (!kfsw_storage_is_ready() || !kfsw_ftp_link_is_ready()) {
		return -EACCES;
	}
	return (node <= kfsw_ftp_link_max_node()) ? 0 : -EINVAL;
}

static bool is_local_node(uint16_t node)
{
	return node == kfsw_ftp_link_local_node();
}

/*
 * A request addressed to this node reads its own FTP root directly, so it
 * needs storage and a started service but no router, route or connection.
 */
static int validate_local(void)
{
	return (kfsw_storage_is_ready() && kfsw_ftp_is_started()) ? 0 : -EACCES;
}

static uint32_t allocate_request_id(void)
{
	uint32_t request_id = (uint32_t)atomic_inc(&next_request_id) + 1U;

	return (request_id == 0U) ? (uint32_t)atomic_inc(&next_request_id) + 1U : request_id;
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

/*
 * Receive one response and require that it is the expected reply to the
 * request that is in flight. The frame stays owned by the caller so a response
 * body can still be read; an error releases it here.
 */
static int receive_response(struct kfsw_ftp_link *link, uint8_t expected_opcode,
			    uint32_t request_id, struct kfsw_ftp_link_frame *frame)
{
	int result = kfsw_ftp_link_receive(link, frame);

	if (result != 0) {
		return result;
	}
	if ((frame->message.opcode != expected_opcode) ||
	    (frame->message.request_id != request_id)) {
		kfsw_ftp_link_release(frame);
		return -EBADMSG;
	}
	result = kfsw_ftp_wire_status_to_errno(frame->message.status);
	if (result != 0) {
		kfsw_ftp_link_release(frame);
	}
	return result;
}

static int simple_path_request(uint16_t node, const char *path, bool allow_root,
			       uint8_t request_opcode, uint8_t response_opcode,
			       struct kfsw_ftp_message *response_out)
{
	struct kfsw_ftp_link link = {0};
	struct kfsw_ftp_link_frame frame = {0};
	struct kfsw_ftp_message request = {
		.opcode = request_opcode,
		.request_id = allocate_request_id(),
		.path = (const uint8_t *)path,
	};
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
	result = kfsw_ftp_link_connect(&link, node);
	if (result != 0) {
		return result;
	}
	result = kfsw_ftp_link_send(&link, &request);
	if (result == 0) {
		result = receive_response(&link, response_opcode, request.request_id, &frame);
	}
	if ((result == 0) && (response_out != NULL)) {
		*response_out = frame.message;
	}
	kfsw_ftp_link_release(&frame);
	kfsw_ftp_link_close(&link);
	return result;
}

int kfsw_ftp_mkdir(uint16_t node, const char *path)
{
	int result;

	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	if (is_local_node(node)) {
		result = validate_local();
		if (result == 0) {
			result = kfsw_ftp_local_mkdir(path, &client_workspace);
		}
	} else {
		result = simple_path_request(node, path, false, KFSW_FTP_OP_MKDIR_REQUEST,
					     KFSW_FTP_OP_MKDIR_RESPONSE, NULL);
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

static int remote_stat(uint16_t node, const char *path, struct kfsw_ftp_stat *info)
{
	struct kfsw_ftp_message response;
	int result = simple_path_request(node, path, true, KFSW_FTP_OP_STAT_REQUEST,
					 KFSW_FTP_OP_STAT_RESPONSE, &response);

	if (result != 0) {
		return result;
	}
	if ((response.path_size != 0U) || (response.data_size != 0U) ||
	    ((response.flags != KFSW_FTP_ENTRY_FILE) &&
	     (response.flags != KFSW_FTP_ENTRY_DIRECTORY))) {
		return -EBADMSG;
	}
	info->type = (enum kfsw_ftp_entry_type)response.flags;
	info->size = response.total_size;
	info->crc32 = response.crc32;
	return 0;
}

int kfsw_ftp_stat(uint16_t node, const char *path, struct kfsw_ftp_stat *info)
{
	int result;

	if (info == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	if (is_local_node(node)) {
		result = validate_local();
		if (result == 0) {
			result = kfsw_ftp_local_stat(path, &client_workspace, info);
		}
	} else {
		result = remote_stat(node, path, info);
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

static int visit_list_entry(const struct kfsw_ftp_message *response,
			    kfsw_ftp_list_visitor_t visitor, void *context, bool *keep_visiting)
{
	char name[KFSW_FTP_MAX_PATH_SIZE + 1U];
	struct kfsw_ftp_entry entry = {
		.name = name,
		.type = (enum kfsw_ftp_entry_type)response->flags,
		.size = response->total_size,
	};

	if ((response->opcode != KFSW_FTP_OP_LIST_ENTRY) ||
	    (response->status != KFSW_FTP_STATUS_OK) || (response->path_size == 0U) ||
	    (response->data_size != 0U) ||
	    ((response->flags != KFSW_FTP_ENTRY_FILE) &&
	     (response->flags != KFSW_FTP_ENTRY_DIRECTORY))) {
		return -EBADMSG;
	}
	memcpy(name, response->path, response->path_size);
	name[response->path_size] = '\0';
	*keep_visiting = visitor(&entry, context);
	return 0;
}

static int remote_list(uint16_t node, const char *path, kfsw_ftp_list_visitor_t visitor,
		       void *context)
{
	struct kfsw_ftp_link link = {0};
	struct kfsw_ftp_message request = {
		.opcode = KFSW_FTP_OP_LIST_REQUEST,
		.request_id = allocate_request_id(),
		.path = (const uint8_t *)path,
	};
	bool keep_visiting = true;
	int result;

	result = validate_client(node);
	if (result == 0) {
		result = validate_virtual_path(path, true, client_workspace.path,
					       sizeof(client_workspace.path), &request.path_size);
	}
	if (result == 0) {
		result = kfsw_ftp_link_connect(&link, node);
	}
	if (result == 0) {
		result = kfsw_ftp_link_send(&link, &request);
	}
	while (result == 0) {
		struct kfsw_ftp_link_frame frame;

		result = kfsw_ftp_link_receive(&link, &frame);
		if (result != 0) {
			break;
		}
		if (frame.message.request_id != request.request_id) {
			result = -EBADMSG;
		} else if (frame.message.opcode == KFSW_FTP_OP_LIST_END) {
			result = kfsw_ftp_wire_status_to_errno(frame.message.status);
			kfsw_ftp_link_release(&frame);
			break;
		} else if (keep_visiting) {
			result = visit_list_entry(&frame.message, visitor, context, &keep_visiting);
		}
		kfsw_ftp_link_release(&frame);
	}
	kfsw_ftp_link_close(&link);
	return result;
}

int kfsw_ftp_list(uint16_t node, const char *path, kfsw_ftp_list_visitor_t visitor, void *context)
{
	int result;

	if (visitor == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	if (is_local_node(node)) {
		result = validate_local();
		if (result == 0) {
			result = kfsw_ftp_local_list(path, &client_workspace, visitor, context);
		}
	} else {
		result = remote_list(node, path, visitor, context);
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

/*
 * Resolve both ends of an upload and CRC the local file before the connection
 * opens, so the server learns the expected size and CRC in the request itself.
 */
static int prepare_upload(const char *local_path, const char *remote_path,
			  struct kfsw_ftp_message *request, struct kfsw_ftp_transfer *transfer)
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
	result = kfsw_ftp_file_crc(client_workspace.path, &client_workspace, &transfer->total_size,
				   &transfer->crc32);
	if (result != 0) {
		return result;
	}
	request->path = (const uint8_t *)remote_path;
	request->path_size = remote_path_size;
	request->total_size = transfer->total_size;
	request->crc32 = transfer->crc32;
	return 0;
}

/* PUT_READY means the server opened its temporary file; PUT_RESULT means it refused. */
static int await_put_ready(struct kfsw_ftp_link *link, uint32_t request_id)
{
	struct kfsw_ftp_link_frame frame;
	int result = kfsw_ftp_link_receive(link, &frame);

	if (result != 0) {
		return result;
	}
	if ((frame.message.request_id != request_id) ||
	    ((frame.message.opcode != KFSW_FTP_OP_PUT_READY) &&
	     (frame.message.opcode != KFSW_FTP_OP_PUT_RESULT))) {
		result = -EBADMSG;
	} else {
		result = kfsw_ftp_wire_status_to_errno(frame.message.status);
		if ((result == 0) && (frame.message.opcode != KFSW_FTP_OP_PUT_READY)) {
			result = -EBADMSG;
		}
	}
	kfsw_ftp_link_release(&frame);
	return result;
}

/* The peer echoes what it committed; a disagreement is an integrity failure. */
static int await_transfer_result(struct kfsw_ftp_link *link, uint8_t result_opcode,
				 const struct kfsw_ftp_transfer *transfer)
{
	struct kfsw_ftp_link_frame frame;
	int result = receive_response(link, result_opcode, transfer->request_id, &frame);

	if (result != 0) {
		return result;
	}
	if ((frame.message.total_size != transfer->total_size) ||
	    (frame.message.crc32 != transfer->crc32)) {
		result = -EILSEQ;
	}
	kfsw_ftp_link_release(&frame);
	return result;
}

static void report_transfer(struct kfsw_ftp_transfer_result *transfer_result, uint32_t bytes,
			    uint32_t crc32, uint32_t started_ms)
{
	transfer_result->bytes = bytes;
	transfer_result->crc32 = crc32;
	transfer_result->duration_ms = k_uptime_get_32() - started_ms;
}

/*
 * A completed or failed transfer is exactly the kind of fact an operator needs
 * to establish later, so it is recorded as well as returned.
 */
static void record_transfer_event(uint16_t event_id, uint16_t node, uint32_t bytes,
				  uint32_t crc32_or_errno, bool failed)
{
	/* Outside the event guard: a composition without the event record still
	 * needs to know how many transfers have run. */
	kfsw_ftp_count_transfer(bytes, failed);

#if CONFIG_KFSW_EVENT
	uint8_t payload[10];

	sys_put_be16(node, &payload[0]);
	sys_put_be32(bytes, &payload[2]);
	sys_put_be32(crc32_or_errno, &payload[6]);
	kfsw_event_emit(KFSW_EVENT_SOURCE_FTP, event_id,
			failed ? KFSW_EVENT_ERROR : KFSW_EVENT_INFO, payload, sizeof(payload));
#else
	ARG_UNUSED(event_id);
	ARG_UNUSED(node);
	ARG_UNUSED(bytes);
	ARG_UNUSED(crc32_or_errno);
	ARG_UNUSED(failed);
#endif
}

int kfsw_ftp_put(uint16_t node, const char *local_path, const char *remote_path,
		 struct kfsw_ftp_transfer_result *transfer_result)
{
	struct kfsw_ftp_link link = {0};
	struct kfsw_ftp_message request = {
		.opcode = KFSW_FTP_OP_PUT_REQUEST,
		.request_id = allocate_request_id(),
	};
	struct kfsw_ftp_transfer transfer = {
		.link = &link,
		.workspace = &client_workspace,
		.request_id = request.request_id,
		.data_opcode = KFSW_FTP_OP_PUT_DATA,
	};
	uint32_t started_ms;
	int result;

	if (transfer_result == NULL) {
		return -EINVAL;
	}
	if (is_local_node(node)) {
		return -ENOTSUP;
	}
	memset(transfer_result, 0, sizeof(*transfer_result));
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	started_ms = k_uptime_get_32();
	result = validate_client(node);
	if (result == 0) {
		result = prepare_upload(local_path, remote_path, &request, &transfer);
	}
	if (result == 0) {
		result = kfsw_ftp_link_connect(&link, node);
	}
	if (result == 0) {
		result = kfsw_ftp_link_send(&link, &request);
	}
	if (result == 0) {
		result = await_put_ready(&link, request.request_id);
	}
	if (result == 0) {
		result = kfsw_ftp_transfer_open_source(&transfer, client_workspace.path);
	}
	if (result == 0) {
		result = kfsw_ftp_transfer_send(&transfer);
	}
	if (result == 0) {
		result = await_transfer_result(&link, KFSW_FTP_OP_PUT_RESULT, &transfer);
	}
	kfsw_ftp_link_close(&link);
	if (result == 0) {
		report_transfer(transfer_result, transfer.total_size, transfer.crc32, started_ms);
		record_transfer_event(KFSW_EVENT_FTP_PUT_DONE, node, transfer.total_size,
				      transfer.crc32, false);
	} else {
		record_transfer_event(KFSW_EVENT_FTP_TRANSFER_FAILED, node, transfer.offset,
				      (uint32_t)(-result), true);
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}

/*
 * Resolve both ends of a download and derive the temporary path. An existing
 * local file stays untouched until the transfer commits.
 */
static int prepare_download(const char *remote_path, const char *local_path,
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

/* GET_INFO carries the expected size and CRC; GET_RESULT means the server refused. */
static int await_get_info(struct kfsw_ftp_link *link, struct kfsw_ftp_transfer *transfer)
{
	struct kfsw_ftp_link_frame frame;
	int result = kfsw_ftp_link_receive(link, &frame);

	if (result != 0) {
		return result;
	}
	if ((frame.message.request_id != transfer->request_id) ||
	    ((frame.message.opcode != KFSW_FTP_OP_GET_INFO) &&
	     (frame.message.opcode != KFSW_FTP_OP_GET_RESULT))) {
		result = -EBADMSG;
	} else {
		result = kfsw_ftp_wire_status_to_errno(frame.message.status);
		if ((result == 0) && (frame.message.opcode == KFSW_FTP_OP_GET_INFO)) {
			transfer->total_size = frame.message.total_size;
			transfer->crc32 = frame.message.crc32;
		} else if (result == 0) {
			result = -EBADMSG;
		}
	}
	kfsw_ftp_link_release(&frame);
	return result;
}

int kfsw_ftp_get(uint16_t node, const char *remote_path, const char *local_path,
		 struct kfsw_ftp_transfer_result *transfer_result)
{
	struct kfsw_ftp_link link = {0};
	struct kfsw_ftp_message request = {
		.opcode = KFSW_FTP_OP_GET_REQUEST,
		.request_id = allocate_request_id(),
	};
	struct kfsw_ftp_transfer transfer = {
		.link = &link,
		.workspace = &client_workspace,
		.request_id = request.request_id,
		.data_opcode = KFSW_FTP_OP_GET_DATA,
	};
	uint32_t started_ms;
	int result;

	if (transfer_result == NULL) {
		return -EINVAL;
	}
	if (is_local_node(node)) {
		return -ENOTSUP;
	}
	memset(transfer_result, 0, sizeof(*transfer_result));
	k_mutex_lock(&kfsw_ftp_client_lock, K_FOREVER);
	started_ms = k_uptime_get_32();
	result = validate_client(node);
	if (result == 0) {
		result = prepare_download(remote_path, local_path, &request);
	}
	if (result == 0) {
		result = kfsw_ftp_link_connect(&link, node);
	}
	if (result == 0) {
		result = kfsw_ftp_link_send(&link, &request);
	}
	if (result == 0) {
		result = await_get_info(&link, &transfer);
	}
	if (result == 0) {
		result = kfsw_ftp_transfer_open_sink(&transfer, client_workspace.temporary_path);
	}
	if (result == 0) {
		result = kfsw_ftp_transfer_receive(&transfer);
		if (result == 0) {
			/*
			 * The server's result arrives before the commit, so a
			 * server-side failure is seen while the received data is
			 * still only a temporary file.
			 */
			result = await_transfer_result(&link, KFSW_FTP_OP_GET_RESULT, &transfer);
		}
		result = kfsw_ftp_transfer_finish(&transfer, client_workspace.path,
						  client_workspace.temporary_path, result);
	}
	kfsw_ftp_link_close(&link);
	if (result == 0) {
		report_transfer(transfer_result, transfer.offset, transfer.actual_crc32,
				started_ms);
		record_transfer_event(KFSW_EVENT_FTP_GET_DONE, node, transfer.offset,
				      transfer.actual_crc32, false);
	} else {
		record_transfer_event(KFSW_EVENT_FTP_TRANSFER_FAILED, node, transfer.offset,
				      (uint32_t)(-result), true);
	}
	k_mutex_unlock(&kfsw_ftp_client_lock);
	return result;
}
