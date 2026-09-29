#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <csp/csp.h>

#include <kfsw/comms/csp.h>
#include <kfsw/services/log_history.h>

#define LOG_REQUEST_SIZE 12U
#define LOG_HEADER_SIZE 10U
#define LOG_RECORD_HEADER_SIZE (LOG_HEADER_SIZE + 20U)
/* A round number under what the encrypted link takes, leaving some margin. */
#define LOG_WIRE_TEXT_MAX 190U

BUILD_ASSERT(LOG_RECORD_HEADER_SIZE + LOG_WIRE_TEXT_MAX <= KFSW_CSP_PAYLOAD_MAX,
	     "a log record must fit the tightest transport, not just a CSP buffer");

static csp_socket_t log_socket;
static bool running;
static K_MUTEX_DEFINE(start_lock);

static csp_packet_t *new_reply(uint8_t type, const uint8_t *nonce, size_t size)
{
	csp_packet_t *packet = csp_buffer_get(size);

	if (packet != NULL) {
		packet->data[0] = 1U;
		packet->data[1] = type;
		memcpy(&packet->data[2], nonce, 8U);
		packet->length = size;
	}
	return packet;
}

static void serve(csp_conn_t *connection, csp_packet_t *request)
{
	struct kfsw_log_history_window window;
	struct kfsw_log_record record;
	uint8_t nonce[8];
	uint8_t minimum;
	uint8_t status = 0U;
	uint16_t count;
	uint16_t sent = 0U;
	csp_packet_t *reply;

	if ((request->length != LOG_REQUEST_SIZE) || (request->data[0] != 1U) ||
	    (request->data[1] > 3U)) {
		csp_buffer_free(request);
		return;
	}
	minimum = request->data[1];
	count = sys_get_be16(&request->data[2]);
	memcpy(nonce, &request->data[4], sizeof(nonce));
	csp_buffer_free(request);
	if (kfsw_log_history_window(count, &window) != 0) {
		return;
	}
	reply = new_reply(0U, nonce, LOG_HEADER_SIZE + 24U);
	if (reply == NULL) {
		return;
	}
	sys_put_be64(window.first, &reply->data[10]);
	sys_put_be64(window.end, &reply->data[18]);
	sys_put_be64(window.overwritten, &reply->data[26]);
	csp_send(connection, reply);

	for (uint64_t sequence = window.first; sequence < window.end; sequence++) {
		size_t length;

		if (kfsw_log_history_get(sequence, &record) != 0) {
			status = 1U;
			break;
		}
		if (record.severity < minimum) {
			continue;
		}
		length = strlen(record.text);
		if (length > LOG_WIRE_TEXT_MAX) {
			length = LOG_WIRE_TEXT_MAX;
			record.truncated = true;
		}
		reply = new_reply(1U, nonce, LOG_RECORD_HEADER_SIZE + length);
		if (reply == NULL) {
			status = 2U;
			break;
		}
		sys_put_be64(record.sequence, &reply->data[10]);
		sys_put_be64(record.uptime_ms, &reply->data[18]);
		reply->data[26] = record.module;
		reply->data[27] = record.severity;
		reply->data[28] = record.truncated ? 1U : 0U;
		reply->data[29] = length;
		memcpy(&reply->data[30], record.text, length);
		csp_send(connection, reply);
		sent++;
	}
	reply = new_reply(2U, nonce, LOG_HEADER_SIZE + 3U);
	if (reply != NULL) {
		reply->data[10] = status;
		sys_put_be16(sent, &reply->data[11]);
		csp_send(connection, reply);
	}
}

static void log_server(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		csp_conn_t *connection = csp_accept(&log_socket, 100U);
		csp_packet_t *request;

		if (connection == NULL) {
			continue;
		}
		request = csp_read(connection, 0U);
		if (request != NULL) {
			serve(connection, request);
		}
		(void)csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_log_server_thread, CONFIG_KFSW_LOG_HISTORY_STACK_SIZE, log_server, NULL, NULL,
		NULL, 8, 0, SYS_FOREVER_MS);

int kfsw_log_history_server_start(void)
{
	struct kfsw_csp_info info;
	int result = 0;

	k_mutex_lock(&start_lock, K_FOREVER);
	if (running) {
		goto done;
	}
	kfsw_csp_get_info(&info);
	if (!info.initialized || !info.router_running) {
		result = -ENETDOWN;
		goto done;
	}
	memset(&log_socket, 0, sizeof(log_socket));
	log_socket.opts = CSP_SO_CRC32REQ;
	if (csp_listen(&log_socket, 1U) != CSP_ERR_NONE) {
		(void)csp_socket_close(&log_socket);
		result = -EIO;
		goto done;
	}
	if (csp_bind(&log_socket, CONFIG_KFSW_LOG_HISTORY_PORT) != CSP_ERR_NONE) {
		(void)csp_socket_close(&log_socket);
		result = -EADDRINUSE;
		goto done;
	}
	running = true;
	k_thread_start(kfsw_log_server_thread);
done:
	k_mutex_unlock(&start_lock);
	return result;
}
