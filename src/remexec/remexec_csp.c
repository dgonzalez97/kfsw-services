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
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_COMMAND
#include <kfsw/services/log.h>

#include "remexec_internal.h"

/* One request per connection. The served command runs on this thread, not on
 * the CSP router, so a slow handler cannot stall routing.
 */

#define KFSW_REMEXEC_POLL_MS 100U

BUILD_ASSERT(KFSW_REMEXEC_REPLY_HEADER_SIZE + CONFIG_KFSW_REMEXEC_OUTPUT_MAX <=
		     KFSW_CSP_PAYLOAD_MAX,
	     "One reply must fit in one CSP packet on the narrowest link");

static csp_socket_t remexec_socket;
static atomic_t server_started;
static bool thread_started;

static void send_reply(csp_conn_t *connection, enum kfsw_remexec_opcode opcode,
		       const struct kfsw_remexec_reply *reply)
{
	static uint8_t buffer[KFSW_REMEXEC_REPLY_HEADER_SIZE + KFSW_REMEXEC_OUTPUT_MAX];
	csp_packet_t *packet;
	size_t size;

	if (kfsw_remexec_encode_reply(buffer, sizeof(buffer), opcode, reply, &size) != 0) {
		return;
	}
	packet = csp_buffer_get(size);
	if (packet == NULL) {
		return;
	}
	memcpy(packet->data, buffer, size);
	packet->length = size;
	/* csp_send() frees the packet, even when sending fails. A requester
	 * that has gone away simply never sees this; the command already ran.
	 */
	csp_send(connection, packet);
}

static void serve_request(csp_conn_t *connection)
{
	static struct kfsw_remexec_reply reply;
	char text[KFSW_REMEXEC_COMMAND_MAX + 1U];
	enum kfsw_remexec_opcode opcode;
	csp_packet_t *packet;
	int decoded;

	packet = csp_read(connection, CONFIG_KFSW_REMEXEC_TIMEOUT_MS);
	if (packet == NULL) {
		return;
	}
	decoded = kfsw_remexec_decode_request(packet->data, packet->length, &opcode, text,
					      sizeof(text));
	csp_buffer_free(packet);
	if (decoded != 0) {
		/* Nothing to answer: the opcode the reply must carry is exactly
		 * what could not be read.
		 */
		kfsw_log_warning("Remote execution request was not understood (%d)", decoded);
		return;
	}

	if (opcode == KFSW_REMEXEC_OP_LIST) {
		kfsw_remexec_list_local(text, &reply);
	} else {
		kfsw_remexec_run_local(text, &reply);
	}
	send_reply(connection, opcode, &reply);
}

static void remexec_server(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection;

		if (atomic_get(&server_started) == 0) {
			k_sleep(K_MSEC(KFSW_REMEXEC_POLL_MS));
			continue;
		}
		connection = csp_accept(&remexec_socket, KFSW_REMEXEC_POLL_MS);
		if (connection == NULL) {
			continue;
		}
		serve_request(connection);
		(void)csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_remexec_server_thread, CONFIG_KFSW_REMEXEC_SERVER_STACK_SIZE, remexec_server,
		NULL, NULL, NULL, CONFIG_KFSW_REMEXEC_SERVER_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_remexec_server_start(void)
{
	struct kfsw_csp_info csp_info;
	int result;

	if (!kfsw_remexec_is_initialized()) {
		return -EACCES;
	}
	if (atomic_get(&server_started) != 0) {
		return 0;
	}
	kfsw_csp_get_info(&csp_info);
	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}

	memset(&remexec_socket, 0, sizeof(remexec_socket));
	remexec_socket.opts = CSP_SO_CRC32REQ;
	result = csp_listen(&remexec_socket, 1U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&remexec_socket);
		return -EIO;
	}
	result = csp_bind(&remexec_socket, CONFIG_KFSW_REMEXEC_CSP_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&remexec_socket);
		return -EADDRINUSE;
	}
	atomic_set(&server_started, 1);
	if (!thread_started) {
		k_thread_start(kfsw_remexec_server_thread);
		thread_started = true;
	}
	return 0;
}

bool kfsw_remexec_server_is_started(void)
{
	return atomic_get(&server_started) != 0;
}

K_MUTEX_DEFINE(remexec_client_lock);

static int exchange(uint16_t node, enum kfsw_remexec_opcode opcode, const char *text,
		    struct kfsw_remexec_reply *reply)
{
	static uint8_t buffer[KFSW_REMEXEC_REQUEST_HEADER_SIZE + KFSW_REMEXEC_COMMAND_MAX];
	csp_conn_t *connection;
	csp_packet_t *packet;
	size_t size;
	int result;

	result = kfsw_remexec_encode_request(buffer, sizeof(buffer), opcode, text, &size);
	if (result != 0) {
		return result;
	}
	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_REMEXEC_CSP_PORT,
				 CONFIG_KFSW_REMEXEC_TIMEOUT_MS, CSP_O_CRC32);
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

	packet = csp_read(connection, CONFIG_KFSW_REMEXEC_TIMEOUT_MS);
	if (packet == NULL) {
		(void)csp_close(connection);
		return -ETIMEDOUT;
	}
	result = kfsw_remexec_decode_reply(packet->data, packet->length, opcode, reply);
	csp_buffer_free(packet);
	(void)csp_close(connection);
	return result;
}

static int request(uint16_t node, enum kfsw_remexec_opcode opcode, const char *text,
		   struct kfsw_remexec_reply *reply)
{
	struct kfsw_csp_info csp_info;
	int result;

	if ((reply == NULL) || (node == 0U) || (node >= KFSW_CSP_BROADCAST_ADDRESS)) {
		return -EINVAL;
	}
	kfsw_csp_get_info(&csp_info);
	/* The capture shell cannot be driven from the shell that asked, so a
	 * request addressed to this node is refused rather than deadlocked.
	 */
	if (node == csp_info.address) {
		return -EINVAL;
	}
	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}
	k_mutex_lock(&remexec_client_lock, K_FOREVER);
	result = exchange(node, opcode, text, reply);
	k_mutex_unlock(&remexec_client_lock);
	return result;
}

int kfsw_remexec_list_remote(uint16_t node, const char *prefix, struct kfsw_remexec_reply *reply)
{
	return request(node, KFSW_REMEXEC_OP_LIST, (prefix != NULL) ? prefix : "", reply);
}

int kfsw_remexec_run_remote(uint16_t node, const char *line, struct kfsw_remexec_reply *reply)
{
	if (line == NULL) {
		return -EINVAL;
	}
	return request(node, KFSW_REMEXEC_OP_RUN, line, reply);
}
