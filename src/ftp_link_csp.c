#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <csp/csp.h>
#include <csp/csp_buffer.h>
#include <csp/csp_crc32.h>
#include <csp/csp_id.h>

#include <kfsw/comms/csp.h>

#include "ftp_link.h"

/*
 * CSP backing for the file-transfer transport. This is the only translation
 * unit in the service that includes libcsp, so every packet-ownership rule
 * libcsp imposes is enforced here rather than spread across the client and
 * the server.
 */

#define KFSW_FTP_LINK_OVERHEAD (CSP_RDP_HEADER_SIZE + sizeof(csp_crc32_t))

/*
 * libcsp appends the RDP header and the CRC32 after the application has filled
 * the packet, so the usable space is the buffer minus both, not the buffer
 * alone.
 */
BUILD_ASSERT(KFSW_FTP_PROTOCOL_HEADER_SIZE + KFSW_FTP_CHUNK_SIZE + KFSW_FTP_LINK_OVERHEAD <=
		     CSP_BUFFER_SIZE,
	     "One FTP message plus the RDP header and CRC32 must fit in one CSP packet");

uint16_t kfsw_ftp_link_max_payload(void)
{
	return (uint16_t)(CSP_BUFFER_SIZE - KFSW_FTP_LINK_OVERHEAD - KFSW_FTP_PROTOCOL_HEADER_SIZE);
}

uint16_t kfsw_ftp_link_max_node(void)
{
	return (uint16_t)((1UL << csp_id_get_host_bits()) - 1UL);
}

uint16_t kfsw_ftp_link_local_node(void)
{
	struct kfsw_csp_info csp_info;

	kfsw_csp_get_info(&csp_info);
	return csp_info.address;
}

bool kfsw_ftp_link_is_ready(void)
{
	struct kfsw_csp_info csp_info;

	kfsw_csp_get_info(&csp_info);
	return csp_info.initialized && csp_info.router_running;
}

int kfsw_ftp_link_connect(struct kfsw_ftp_link *link, uint16_t node)
{
	if (link == NULL) {
		return -EINVAL;
	}
	link->connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_FTP_CSP_PORT,
				       CONFIG_KFSW_FTP_TIMEOUT_MS, CSP_O_RDP | CSP_O_CRC32);
	return (link->connection != NULL) ? 0 : -ECONNREFUSED;
}

int kfsw_ftp_link_send(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *message)
{
	csp_packet_t *packet;
	size_t encoded_size;
	int result;

	if ((link == NULL) || (link->connection == NULL) || (message == NULL)) {
		return -EINVAL;
	}
	packet = csp_buffer_get(KFSW_FTP_PROTOCOL_HEADER_SIZE + message->path_size +
				message->data_size);
	if (packet == NULL) {
		return -ENOMEM;
	}
	result = kfsw_ftp_protocol_encode(packet->data, CSP_BUFFER_SIZE, message, &encoded_size);
	if (result != 0) {
		csp_buffer_free(packet);
		return result;
	}
	packet->length = encoded_size;
	/* csp_send() takes ownership of the packet, including on transmit failure. */
	csp_send(link->connection, packet);
	return 0;
}

int kfsw_ftp_link_receive(struct kfsw_ftp_link *link, struct kfsw_ftp_link_frame *frame)
{
	csp_packet_t *packet;
	int result;

	if ((link == NULL) || (link->connection == NULL) || (frame == NULL)) {
		return -EINVAL;
	}
	frame->buffer = NULL;
	packet = csp_read(link->connection, CONFIG_KFSW_FTP_TIMEOUT_MS);
	if (packet == NULL) {
		return -ETIMEDOUT;
	}
	result = kfsw_ftp_protocol_decode(packet->data, packet->length, &frame->message);
	if (result != 0) {
		csp_buffer_free(packet);
		return result;
	}
	frame->buffer = packet;
	return 0;
}

void kfsw_ftp_link_release(struct kfsw_ftp_link_frame *frame)
{
	if ((frame == NULL) || (frame->buffer == NULL)) {
		return;
	}
	csp_buffer_free(frame->buffer);
	frame->buffer = NULL;
	/* The decoded pointers borrowed that buffer; do not leave them dangling. */
	frame->message.path = NULL;
	frame->message.data = NULL;
}

void kfsw_ftp_link_close(struct kfsw_ftp_link *link)
{
	if ((link == NULL) || (link->connection == NULL)) {
		return;
	}
	(void)csp_close(link->connection);
	link->connection = NULL;
}

bool kfsw_ftp_link_is_open(const struct kfsw_ftp_link *link)
{
	return (link != NULL) && (link->connection != NULL);
}

/*
 * One listening endpoint is enough: the service accepts one request at a time
 * and rejects overlapping connections as busy.
 */
static csp_socket_t link_server_socket;

int kfsw_ftp_link_listen(struct kfsw_ftp_listener *listener)
{
	int result;

	if (listener == NULL) {
		return -EINVAL;
	}
	memset(&link_server_socket, 0, sizeof(link_server_socket));
	link_server_socket.opts = CSP_SO_RDPREQ | CSP_SO_CRC32REQ;
	result = csp_listen(&link_server_socket, 1U);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&link_server_socket);
		return -EIO;
	}
	result = csp_bind(&link_server_socket, CONFIG_KFSW_FTP_CSP_PORT);
	if (result != CSP_ERR_NONE) {
		(void)csp_socket_close(&link_server_socket);
		return -EADDRINUSE;
	}
	listener->socket = &link_server_socket;
	return 0;
}

int kfsw_ftp_link_accept(struct kfsw_ftp_listener *listener, struct kfsw_ftp_link *link,
			 uint32_t timeout_ms)
{
	if ((listener == NULL) || (listener->socket == NULL) || (link == NULL)) {
		return -EINVAL;
	}
	link->connection = csp_accept(listener->socket, timeout_ms);
	return (link->connection != NULL) ? 0 : -EAGAIN;
}

void kfsw_ftp_link_listener_close(struct kfsw_ftp_listener *listener)
{
	if ((listener == NULL) || (listener->socket == NULL)) {
		return;
	}
	(void)csp_socket_close(listener->socket);
	/* csp_socket_close() leaves the queue pointer set; clear it before reuse. */
	link_server_socket.rx_queue = NULL;
	listener->socket = NULL;
}
