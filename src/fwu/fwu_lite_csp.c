#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <csp/csp.h>
#include <csp/crypto/csp_hmac.h>
#include <kfsw/comms/csp.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/fwu_lite.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_FWU
#include <kfsw/services/log.h>

#define SOCKET_OPTIONS                                                                             \
	(CSP_SO_CRC32REQ | KFSW_CSP_AUTHENTICATED_SOCKET |                                         \
	 (IS_ENABLED(CONFIG_KFSW_FWU_LITE_RDP) ? CSP_SO_RDPREQ : 0))

#define WIRE_BUFFER_SIZE KFSW_FWU_LITE_WIRE_SIZE

/* Pinned libcsp appends RDP(5), HMAC(4), CSP CRC(4), KISS CRC(4).
 * CSP IDs use packet headroom. KISS escapes on output, at most
 * 2 * (payload + these trailers + CSP2 ID(6)) + 3 serial bytes.
 */
BUILD_ASSERT(WIRE_BUFFER_SIZE + 5U + CSP_HMAC_LENGTH + 4U + 4U <= CSP_BUFFER_SIZE,
	     "One block message must fit a CSP buffer; reduce KFSW_FWU_LITE_BLOCK_SIZE");

static K_THREAD_STACK_DEFINE(server_stack, CONFIG_KFSW_FWU_LITE_STACK_SIZE);
static struct k_thread server_thread;
static bool server_running;
static csp_socket_t server_socket;
K_MUTEX_DEFINE(server_start_lock);

static void serve_packet(csp_conn_t *connection, csp_packet_t *packet)
{
	struct kfsw_fwu_lite_message request;
	struct kfsw_fwu_lite_message reply;
	uint8_t receipt_data[KFSW_FWU_LITE_RECEIPT_SIZE];
	csp_packet_t *response;
	size_t encoded = 0U;

	if (packet->length > CSP_BUFFER_SIZE ||
	    kfsw_fwu_lite_decode(packet->data, packet->length, &request) != 0) {
		/* Drop undecodable packets; no valid request ID is available. */
		csp_buffer_free(packet);
		return;
	}

	(void)kfsw_fwu_lite_handle_peer((uint16_t)csp_conn_src(connection), &request, &reply,
					receipt_data);

	response = csp_buffer_get(0);
	if (response == NULL) {
		csp_buffer_free(packet);
		return;
	}

	if (kfsw_fwu_lite_encode(&reply, response->data, CSP_BUFFER_SIZE, &encoded) != 0) {
		csp_buffer_free(response);
		csp_buffer_free(packet);
		return;
	}

	response->length = (uint16_t)encoded;
	csp_buffer_free(packet);
	/* csp_send() frees the packet, even when sending fails. */
	csp_send(connection, response);
}

/* Read on every use, so a change applies to the next block. */
static uint32_t fwu_lite_timeout_ms = CONFIG_KFSW_FWU_LITE_TIMEOUT_MS;
static uint8_t fwu_lite_retries = CONFIG_KFSW_FWU_LITE_BLOCK_RETRIES;

uint32_t kfsw_fwu_lite_get_timeout_ms(void)
{
	return fwu_lite_timeout_ms;
}

int kfsw_fwu_lite_check_timeout_ms(uint32_t timeout_ms)
{
	if ((timeout_ms < KFSW_FWU_LITE_TIMEOUT_MIN_MS) ||
	    (timeout_ms > KFSW_FWU_LITE_TIMEOUT_MAX_MS)) {
		return -ERANGE;
	}
	return 0;
}

int kfsw_fwu_lite_set_timeout_ms(uint32_t timeout_ms)
{
	int result = kfsw_fwu_lite_check_timeout_ms(timeout_ms);

	if (result != 0) {
		return result;
	}
	fwu_lite_timeout_ms = timeout_ms;
	return 0;
}

uint8_t kfsw_fwu_lite_get_retries(void)
{
	return fwu_lite_retries;
}

int kfsw_fwu_lite_set_retries(uint8_t retries)
{
	/* Zero retries would give up on the first lost block. */
	if (retries == 0U) {
		return -EINVAL;
	}
	fwu_lite_retries = retries;
	return 0;
}

static void server_entry(void *first, void *second, void *third)
{
	csp_conn_t *connection;

	ARG_UNUSED(first);
	ARG_UNUSED(second);
	ARG_UNUSED(third);

	kfsw_log_info("Firmware upload server on CSP port %d", CONFIG_KFSW_FWU_LITE_CSP_PORT);

	while (true) {
		connection = csp_accept(&server_socket, CSP_MAX_TIMEOUT);
		if (connection == NULL) {
			continue;
		}

		while (true) {
			/* Same timeout the sender uses; a block and its reply can take more than
			 * a second on a slow link.
			 */
			csp_packet_t *packet = csp_read(connection, kfsw_fwu_lite_get_timeout_ms());

			if (packet == NULL) {
				break;
			}
			serve_packet(connection, packet);
		}
		csp_close(connection);
	}
}

int kfsw_fwu_lite_server_start(void)
{
	struct kfsw_csp_info info;
	int result;

	k_mutex_lock(&server_start_lock, K_FOREVER);
	if (server_running) {
		result = -EALREADY;
		goto out;
	}
	kfsw_csp_get_info(&info);
	if (!info.initialized || !info.router_running) {
		result = -EACCES;
		goto out;
	}
	memset(&server_socket, 0, sizeof(server_socket));
	server_socket.opts = SOCKET_OPTIONS;
	if (csp_listen(&server_socket, 1U) != CSP_ERR_NONE) {
		(void)csp_socket_close(&server_socket);
		result = -EIO;
		goto out;
	}
	if (csp_bind(&server_socket, CONFIG_KFSW_FWU_LITE_CSP_PORT) != CSP_ERR_NONE) {
		(void)csp_socket_close(&server_socket);
		result = -EADDRINUSE;
		goto out;
	}

	(void)k_thread_create(&server_thread, server_stack, K_THREAD_STACK_SIZEOF(server_stack),
			      server_entry, NULL, NULL, NULL, CONFIG_KFSW_FWU_LITE_PRIORITY, 0,
			      K_FOREVER);
	k_thread_name_set(&server_thread, "kfsw_fwu_lite");
	server_running = true;
	k_thread_start(&server_thread);
	result = 0;
out:
	k_mutex_unlock(&server_start_lock);
	return result;
}
