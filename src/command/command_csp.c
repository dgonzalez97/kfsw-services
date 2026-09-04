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

/*
 * The remote front end. This is the only translation unit in the command
 * service that includes libcsp; the registry and the shell adapter know
 * nothing about a transport.
 *
 * One request per connection, one connection at a time. Handlers run on this
 * thread, never on a CSP receive context.
 */

#define KFSW_COMMAND_POLL_MS 100U

BUILD_ASSERT(KFSW_COMMAND_HEADER_SIZE + KFSW_COMMAND_MAX_PAYLOAD_SIZE <= CSP_BUFFER_SIZE,
	     "One command message must fit in one CSP packet");

static csp_socket_t command_socket;
static atomic_t server_started;
static bool thread_started;

static void send_result(csp_conn_t *connection, uint16_t command_id, uint16_t request_id,
			const struct kfsw_command_result *result)
{
	uint8_t buffer[KFSW_COMMAND_HEADER_SIZE + KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	struct kfsw_command_message message = {
		.opcode = KFSW_COMMAND_OP_RESULT,
		.status = (uint8_t)result->status,
		.command_id = command_id,
		.request_id = request_id,
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
	/* csp_send() takes ownership, including on transmit failure. */
	csp_send(connection, packet);
}

static void serve_request(csp_conn_t *connection, uint16_t source_node)
{
	char text_storage[KFSW_COMMAND_MAX_ARGS][KFSW_COMMAND_MAX_TEXT_SIZE + 1U];
	struct kfsw_command_arg args[KFSW_COMMAND_MAX_ARGS];
	struct kfsw_command_message request;
	struct kfsw_command_result result;
	struct kfsw_command_source source = {
		.node = source_node,
		/* Authentication is not implemented; never claim otherwise. */
		.authenticated = false,
	};
	csp_packet_t *packet;
	int decoded;

	packet = csp_read(connection, CONFIG_KFSW_COMMAND_TIMEOUT_MS);
	if (packet == NULL) {
		return;
	}

	memset(&result, 0, sizeof(result));
	if (kfsw_command_protocol_decode(packet->data, packet->length, &request) != 0) {
		result.status = KFSW_COMMAND_INVALID_ARGUMENT;
		csp_buffer_free(packet);
		send_result(connection, 0U, 0U, &result);
		return;
	}
	if (request.opcode != KFSW_COMMAND_OP_REQUEST) {
		result.status = KFSW_COMMAND_INVALID_ARGUMENT;
		csp_buffer_free(packet);
		send_result(connection, request.command_id, request.request_id, &result);
		return;
	}

	decoded = kfsw_command_decode_args(&request, args, ARRAY_SIZE(args), text_storage);
	if (decoded < 0) {
		result.status = KFSW_COMMAND_INVALID_ARGUMENT;
		csp_buffer_free(packet);
		send_result(connection, request.command_id, request.request_id, &result);
		return;
	}

	(void)kfsw_command_invoke_id(request.command_id, args, (size_t)decoded, &source, &result);

	/*
	 * The arguments borrow the packet only through text_storage, which is a
	 * copy, so the buffer is released before the reply is built.
	 */
	csp_buffer_free(packet);
	send_result(connection, request.command_id, request.request_id, &result);
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

static int receive_result(csp_conn_t *connection, uint16_t request_id,
			  struct kfsw_command_result *result)
{
	struct kfsw_command_message response;
	csp_packet_t *packet;
	size_t detail_size;

	packet = csp_read(connection, CONFIG_KFSW_COMMAND_TIMEOUT_MS);
	if (packet == NULL) {
		return -ETIMEDOUT;
	}
	if (kfsw_command_protocol_decode(packet->data, packet->length, &response) != 0) {
		csp_buffer_free(packet);
		return -EBADMSG;
	}
	if ((response.opcode != KFSW_COMMAND_OP_RESULT) || (response.request_id != request_id)) {
		csp_buffer_free(packet);
		return -EBADMSG;
	}

	result->status = (enum kfsw_command_status)response.status;
	detail_size = MIN((size_t)response.payload_size, sizeof(result->detail) - 1U);
	memcpy(result->detail, response.payload, detail_size);
	result->detail[detail_size] = '\0';
	csp_buffer_free(packet);
	return 0;
}

int kfsw_command_invoke_remote(uint16_t node, const char *name, const struct kfsw_command_arg *args,
			       size_t arg_count, struct kfsw_command_result *result)
{
	static uint16_t next_request_id;
	static uint8_t payload[KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	uint8_t buffer[KFSW_COMMAND_HEADER_SIZE + KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	struct kfsw_command_message request = {.opcode = KFSW_COMMAND_OP_REQUEST};
	struct kfsw_csp_info csp_info;
	csp_conn_t *connection;
	csp_packet_t *packet;
	size_t encoded_size;
	int outcome;

	if ((name == NULL) || (result == NULL)) {
		return -EINVAL;
	}
	kfsw_csp_get_info(&csp_info);

	/* A command addressed to this node is run here rather than sent into
	 * the network and back. It is the same command against the same
	 * registry, so the answer is identical, and not involving the link
	 * means it still works when every link is down.
	 *
	 * The file transfer service already does this for its own local node.
	 */
	if (node == csp_info.address) {
		return kfsw_command_invoke(name, args, arg_count, result);
	}

	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}
	memset(result, 0, sizeof(*result));

	k_mutex_lock(&command_client_lock, K_FOREVER);
	outcome = kfsw_command_lookup_id(name, &request.command_id);
	if (outcome == 0) {
		outcome = kfsw_command_encode_args(args, arg_count, payload, sizeof(payload),
						   &request.payload_size);
	}
	if (outcome == 0) {
		next_request_id++;
		request.request_id = next_request_id;
		request.arg_count = (uint8_t)arg_count;
		request.payload = payload;
		outcome = kfsw_command_protocol_encode(buffer, sizeof(buffer), &request,
						       &encoded_size);
	}
	if (outcome != 0) {
		k_mutex_unlock(&command_client_lock);
		return outcome;
	}

	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_COMMAND_CSP_PORT,
				 CONFIG_KFSW_COMMAND_TIMEOUT_MS, CSP_O_CRC32);
	if (connection == NULL) {
		k_mutex_unlock(&command_client_lock);
		return -ECONNREFUSED;
	}
	packet = csp_buffer_get(encoded_size);
	if (packet == NULL) {
		(void)csp_close(connection);
		k_mutex_unlock(&command_client_lock);
		return -ENOMEM;
	}
	memcpy(packet->data, buffer, encoded_size);
	packet->length = encoded_size;
	csp_send(connection, packet);

	outcome = receive_result(connection, request.request_id, result);
	(void)csp_close(connection);
	k_mutex_unlock(&command_client_lock);
	return outcome;
}
