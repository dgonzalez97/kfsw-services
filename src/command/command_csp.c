#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>

#include <kfsw/comms/csp.h>

#include "command_internal.h"
#if CONFIG_KFSW_COMMAND_RETRY
#include "command_retry.h"
static struct kfsw_command_retry_cache retry_cache;
#endif

/* Remote commands: one request per connection, handlers run on this thread. */

#define KFSW_COMMAND_POLL_MS 100U

BUILD_ASSERT(KFSW_COMMAND_RETRY_HEADER_SIZE + KFSW_COMMAND_MAX_PAYLOAD_SIZE <= CSP_BUFFER_SIZE,
	     "One command message must fit in one CSP packet");

static csp_socket_t command_socket;
static atomic_t server_started;
static bool thread_started;

static void send_result(csp_conn_t *connection, const struct kfsw_command_message *request,
			uint64_t token, const struct kfsw_command_result *result)
{
	uint8_t buffer[KFSW_COMMAND_RETRY_HEADER_SIZE + KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	struct kfsw_command_message message = {
		.version = request->version,
		.token = token,
		.opcode = (request->opcode == KFSW_COMMAND_OP_PREPARE) ? KFSW_COMMAND_OP_TICKET
								       : KFSW_COMMAND_OP_RESULT,
		.status = (uint8_t)result->status,
		.command_id = request->command_id,
		.request_id = request->request_id,
	};
	csp_packet_t *packet;
	size_t detail_size;
	size_t encoded_size;

	detail_size = strnlen(result->detail, sizeof(result->detail));
	message.payload_size = (uint16_t)detail_size;
	message.payload = (const uint8_t *)result->detail;

	if (kfsw_command_protocol_encode(buffer, sizeof(buffer), &message, &encoded_size) != 0) {
		return;
	}
	packet = csp_buffer_get(encoded_size);
	if (packet == NULL) {
		return;
	}
	memcpy(packet->data, buffer, encoded_size);
	packet->length = encoded_size;
	/* csp_send() frees the packet, even when sending fails. */
	csp_send(connection, packet);
}

static void serve_request(csp_conn_t *connection, uint16_t source_node)
{
	char text_storage[KFSW_COMMAND_MAX_ARGS][KFSW_COMMAND_MAX_TEXT_SIZE + 1U];
	struct kfsw_command_arg args[KFSW_COMMAND_MAX_ARGS];
	struct kfsw_command_message request = {0};
	struct kfsw_command_result result;
	struct kfsw_command_source source = {
		.node = source_node,
		/* Link protection does not provide a command-level identity here. */
		.authenticated = false,
	};
	csp_packet_t *packet;
	int decoded;

	packet = csp_read(connection, kfsw_command_get_timeout_ms());
	if (packet == NULL) {
		return;
	}

	memset(&result, 0, sizeof(result));
	if (kfsw_command_protocol_decode(packet->data, packet->length, &request) != 0) {
		result.status = KFSW_COMMAND_INVALID_ARGUMENT;
		csp_buffer_free(packet);
		memset(&request, 0, sizeof(request));
		send_result(connection, &request, 0U, &result);
		return;
	}
	if (request.version == 2U) {
		uint64_t token = request.token;
#if CONFIG_KFSW_COMMAND_RETRY
		uint64_t fresh = 0U;

		if (request.opcode == KFSW_COMMAND_OP_PREPARE) {
			(void)kfsw_command_retry_random(&fresh);
		}
		kfsw_command_retry_dispatch(&retry_cache, source_node, &request, fresh,
					    k_uptime_get(), &token, &result);
#else
		result.status = KFSW_COMMAND_UNAVAILABLE;
#endif
		csp_buffer_free(packet);
		send_result(connection, &request, token, &result);
		return;
	}
	if ((request.opcode != KFSW_COMMAND_OP_REQUEST) || (request.status != 0U)) {
		result.status = KFSW_COMMAND_INVALID_ARGUMENT;
		csp_buffer_free(packet);
		send_result(connection, &request, request.token, &result);
		return;
	}

	decoded = kfsw_command_decode_args(&request, args, ARRAY_SIZE(args), text_storage);
	if (decoded < 0) {
		result.status = KFSW_COMMAND_INVALID_ARGUMENT;
		csp_buffer_free(packet);
		send_result(connection, &request, request.token, &result);
		return;
	}

	(void)kfsw_command_invoke_id(request.command_id, args, (size_t)decoded, &source, &result);

	/* Arguments were copied to text_storage, so the packet can be freed now. */
	csp_buffer_free(packet);
	send_result(connection, &request, request.token, &result);
}

static void command_server(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection;

		if (atomic_get(&server_started) == 0) {
			k_sleep(K_MSEC(KFSW_COMMAND_POLL_MS));
			continue;
		}
		connection = csp_accept(&command_socket, KFSW_COMMAND_POLL_MS);
		if (connection == NULL) {
			continue;
		}
		serve_request(connection, csp_conn_src(connection));
		(void)csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_command_server_thread, CONFIG_KFSW_COMMAND_SERVER_STACK_SIZE, command_server,
		NULL, NULL, NULL, CONFIG_KFSW_COMMAND_SERVER_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_command_server_start(void)
{
	struct kfsw_csp_info csp_info;
	int result;

	if (!kfsw_command_is_initialized()) {
		return -EACCES;
	}
	if (atomic_get(&server_started) != 0) {
		return 0;
	}
	kfsw_csp_get_info(&csp_info);
	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}

	memset(&command_socket, 0, sizeof(command_socket));
	command_socket.opts = CSP_SO_CRC32REQ;
	result = csp_listen(&command_socket, 1U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&command_socket);
		return -EIO;
	}
	result = csp_bind(&command_socket, CONFIG_KFSW_COMMAND_CSP_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&command_socket);
		return -EADDRINUSE;
	}
	atomic_set(&server_started, 1);
	if (!thread_started) {
		k_thread_start(kfsw_command_server_thread);
		thread_started = true;
	}
	return 0;
}

bool kfsw_command_server_is_started(void)
{
	return atomic_get(&server_started) != 0;
}

/* Serializes the single client workspace. */
K_MUTEX_DEFINE(command_client_lock);

static int receive_result(csp_conn_t *connection, const struct kfsw_command_message *request,
			  uint64_t *token, struct kfsw_command_result *result)
{
	struct kfsw_command_message response;
	csp_packet_t *packet;
	size_t detail_size;

	packet = csp_read(connection, kfsw_command_get_timeout_ms());
	if (packet == NULL) {
		return -ETIMEDOUT;
	}
	if (kfsw_command_protocol_decode(packet->data, packet->length, &response) != 0) {
		csp_buffer_free(packet);
		return -EBADMSG;
	}
	uint8_t opcode = (request->opcode == KFSW_COMMAND_OP_PREPARE) ? KFSW_COMMAND_OP_TICKET
								      : KFSW_COMMAND_OP_RESULT;
	uint8_t version = request->version ? request->version : 1U;

	if ((response.version != version) || (response.opcode != opcode) ||
	    (response.command_id != request->command_id) ||
	    (response.request_id != request->request_id) || (response.arg_count != 0U) ||
	    (response.status > KFSW_COMMAND_UNAVAILABLE) ||
	    ((version == 2U) &&
	     ((response.token == 0U) || ((request->opcode == KFSW_COMMAND_OP_EXECUTE) &&
					 (response.token != request->token))))) {
		csp_buffer_free(packet);
		return -EBADMSG;
	}

	*token = response.token;
	result->status = (enum kfsw_command_status)response.status;
	detail_size = MIN((size_t)response.payload_size, sizeof(result->detail) - 1U);
	memcpy(result->detail, response.payload, detail_size);
	result->detail[detail_size] = '\0';
	csp_buffer_free(packet);
	return 0;
}

static int exchange(uint16_t node, const struct kfsw_command_message *request,
		    const uint8_t *buffer, size_t size, uint64_t *token,
		    struct kfsw_command_result *result)
{
	csp_conn_t *connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_COMMAND_CSP_PORT,
					     kfsw_command_get_timeout_ms(), CSP_O_CRC32);
	csp_packet_t *packet;
	int outcome;

	if (connection == NULL) {
		return -ECONNREFUSED;
	}
	packet = csp_buffer_get(size);
	if (packet == NULL) {
		(void)csp_close(connection);
		return -ENOMEM;
	}
	memcpy(packet->data, buffer, size);
	packet->length = size;
	csp_send(connection, packet);
	outcome = receive_result(connection, request, token, result);
	(void)csp_close(connection);
	return outcome;
}

static int invoke_remote(uint16_t node, const char *name, const struct kfsw_command_arg *args,
			 size_t arg_count, bool retry, struct kfsw_command_result *result)
{
	static uint16_t next_request_id;
	static uint8_t payload[KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	uint8_t buffer[KFSW_COMMAND_RETRY_HEADER_SIZE + KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	struct kfsw_command_message request = {.opcode = KFSW_COMMAND_OP_REQUEST};
	struct kfsw_csp_info csp_info;
	uint64_t token = 0U;
	size_t encoded_size;
	int outcome;

	if ((name == NULL) || (result == NULL) || (node >= 16383U)) {
		return -EINVAL;
	}
	kfsw_csp_get_info(&csp_info);
	if (node == csp_info.address) {
		return kfsw_command_invoke(name, args, arg_count, result);
	}
	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}
	memset(result, 0, sizeof(*result));
	result->status = KFSW_COMMAND_UNAVAILABLE;
	k_mutex_lock(&command_client_lock, K_FOREVER);
	outcome = kfsw_command_lookup_id(name, &request.command_id);
	if (outcome == 0) {
		outcome = kfsw_command_encode_args(args, arg_count, payload, sizeof(payload),
						   &request.payload_size);
	}
	if (outcome != 0) {
		goto done;
	}
	request.request_id = ++next_request_id;
	request.arg_count = (uint8_t)arg_count;
	request.payload = payload;
	if (retry) {
#if CONFIG_KFSW_COMMAND_RETRY
		outcome = kfsw_command_retry_random(&request.token);
#else
		outcome = -ENOTSUP;
#endif
		if (outcome != 0) {
			goto done;
		}
		request.version = 2U;
		request.opcode = KFSW_COMMAND_OP_PREPARE;
	}
	for (unsigned int phase = 0U; phase < (retry ? 2U : 1U); phase++) {
		outcome = kfsw_command_protocol_encode(buffer, sizeof(buffer), &request,
						       &encoded_size);
		if (outcome != 0) {
			break;
		}
		for (unsigned int attempt = 0U; attempt < (retry ? 3U : 1U); attempt++) {
			outcome = exchange(node, &request, buffer, encoded_size, &token, result);
			if ((outcome != -ETIMEDOUT) && (outcome != -ECONNREFUSED)) {
				break;
			}
		}
		if ((outcome != 0) || (result->status != KFSW_COMMAND_OK)) {
			break;
		}
		request.opcode = KFSW_COMMAND_OP_EXECUTE;
		request.token = token;
	}
done:
	k_mutex_unlock(&command_client_lock);
	return outcome;
}

int kfsw_command_invoke_remote(uint16_t node, const char *name, const struct kfsw_command_arg *args,
			       size_t arg_count, struct kfsw_command_result *result)
{
	return invoke_remote(node, name, args, arg_count, false, result);
}

int kfsw_command_invoke_remote_retry(uint16_t node, const char *name,
				     const struct kfsw_command_arg *args, size_t arg_count,
				     struct kfsw_command_result *result)
{
	return invoke_remote(node, name, args, arg_count, true, result);
}
