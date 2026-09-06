#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <csp/csp.h>

#include <kfsw/comms/csp.h>
#include <kfsw/services/hk.h>
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>

#include "hk_internal.h"

#if CONFIG_KFSW_HK_CSP

/* Request: version, report, count, then the age to start from.
 * Reply: one packet per sample, each a complete frame.
 */
#define KFSW_HK_REQUEST_SIZE 5U
#define KFSW_HK_POLL_MS 100U

BUILD_ASSERT(CONFIG_KFSW_HK_SAMPLE_BYTES <= CSP_BUFFER_SIZE,
	     "a housekeeping sample must fit one CSP buffer");

static csp_socket_t hk_socket;
static bool running;

/*
 * One packet per sample, not one stream.
 *
 * A pass that loses a packet then loses one sample, and the sequence number in
 * each frame makes the gap visible, so ground knows it missed 41 to 47 rather
 * than believing it has everything. Streaming under RDP would turn a bad link
 * into no answer at all.
 */
static void serve_request(csp_conn_t *connection, csp_packet_t *request)
{
	static struct kfsw_hk_sample sample;
	uint8_t report;
	uint8_t wanted;
	uint16_t first_age;
	uint16_t depth = 0U;
	uint16_t sent = 0U;

	if (request->length < KFSW_HK_REQUEST_SIZE) {
		csp_buffer_free(request);
		return;
	}
	if (request->data[0] != KFSW_HK_PROTOCOL_VERSION) {
		kfsw_log_warning("HK: request version %u is not %u", request->data[0],
				 KFSW_HK_PROTOCOL_VERSION);
		csp_buffer_free(request);
		return;
	}

	report = request->data[1];
	wanted = request->data[2];
	first_age = sys_get_be16(&request->data[3]);
	csp_buffer_free(request);

	if (kfsw_hk_depth(report, &depth) != 0) {
		return;
	}

	for (uint16_t offset = 0U; (offset < wanted) && ((first_age + offset) < depth); offset++) {
		csp_packet_t *reply;

		if (kfsw_hk_get(report, (uint16_t)(first_age + offset), &sample) != 0) {
			break;
		}

		reply = csp_buffer_get(sample.length);
		if (reply == NULL) {
			/* Normal under load. Skipping costs one sample, which
			 * the sequence numbers will show; waiting would hold a
			 * connection open while the router needs the pool.
			 */
			kfsw_log_warning("HK: no buffer for report %u, %u sent", report, sent);
			break;
		}
		memcpy(reply->data, sample.data, sample.length);
		reply->length = sample.length;
		csp_send(connection, reply);
		sent++;
	}
}

static void hk_server(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		csp_conn_t *connection = csp_accept(&hk_socket, KFSW_HK_POLL_MS);
		csp_packet_t *request;

		if (connection == NULL) {
			continue;
		}
		request = csp_read(connection, 0);
		if (request != NULL) {
			serve_request(connection, request);
		}
		(void)csp_close(connection);
	}
}

K_THREAD_DEFINE(kfsw_hk_server_thread, CONFIG_KFSW_HK_SERVER_STACK_SIZE, hk_server, NULL, NULL,
		NULL, CONFIG_KFSW_HK_SERVER_PRIORITY, 0, SYS_FOREVER_MS);

/* Bound here rather than inside the thread, so a port that is taken is
 * reported to the caller instead of only logged from somewhere nobody is
 * watching.
 */
int kfsw_hk_server_start(void)
{
	struct kfsw_csp_info csp_info;
	int result;

	if (running) {
		return 0;
	}
	kfsw_csp_get_info(&csp_info);
	if (!csp_info.initialized || !csp_info.router_running) {
		return -EACCES;
	}

	memset(&hk_socket, 0, sizeof(hk_socket));
	hk_socket.opts = CSP_SO_CRC32REQ;
	result = csp_listen(&hk_socket, 1U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&hk_socket);
		return -EIO;
	}
	result = csp_bind(&hk_socket, CONFIG_KFSW_HK_CSP_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&hk_socket);
		return -EADDRINUSE;
	}

	running = true;
	k_thread_start(kfsw_hk_server_thread);
	kfsw_log_info("HK: serving on CSP port %d", CONFIG_KFSW_HK_CSP_PORT);
	return 0;
}

#endif /* CONFIG_KFSW_HK_CSP */
