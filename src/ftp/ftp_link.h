#ifndef KFSW_SERVICES_FTP_LINK_H
#define KFSW_SERVICES_FTP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "ftp_internal.h"

/**
 * @file
 * @brief Reliable message transport for the K-FSW file-transfer service.
 *
 * The transport supplies ordered, retransmitted, integrity-checked delivery of
 * whole protocol messages between two nodes. The file-transfer client, server
 * and transfer engine speak only this interface, so the backend is the only
 * place that knows which transport carries them.
 *
 * The current backend is CSP with RDP and CRC32 (`ftp_link_csp.c`).
 */

/** One transport connection. The handle field belongs to the backend. */
struct kfsw_ftp_link {
	/** Backend-private connection handle. Only the backend dereferences it. */
	void *connection;
};

/** One listening endpoint. The handle field belongs to the backend. */
struct kfsw_ftp_listener {
	/** Backend-private socket handle. Only the backend dereferences it. */
	void *socket;
};

/**
 * One received message together with the buffer that carries it.
 *
 * `message.path` and `message.data` point into that buffer, so every received
 * frame must be released with kfsw_ftp_link_release() exactly once, and only
 * after the caller has copied everything it still needs.
 */
struct kfsw_ftp_link_frame {
	struct kfsw_ftp_message message;
	/** Backend-private buffer handle. */
	void *buffer;
};

/** Largest file-data payload one message can carry on this transport. */
uint16_t kfsw_ftp_link_max_payload(void);

/** Highest node address this transport can address. */
uint16_t kfsw_ftp_link_max_node(void);

/**
 * Local endpoint identifier.
 *
 * A request addressed to this identifier targets this node itself and is
 * served from local storage instead of opening a connection.
 */
uint16_t kfsw_ftp_link_local_node(void);

/**
 * Report whether the transport can currently carry traffic.
 *
 * This is transport lifecycle state, not link state: it says the backend is
 * initialized and able to route, not that any particular peer is reachable.
 */
bool kfsw_ftp_link_is_ready(void);

/** Open a connection to the file-transfer port on one remote node. */
int kfsw_ftp_link_connect(struct kfsw_ftp_link *link, uint16_t node);

/** Encode and transmit one protocol message. */
int kfsw_ftp_link_send(struct kfsw_ftp_link *link, const struct kfsw_ftp_message *message);

/**
 * Wait for one protocol message.
 *
 * On success the frame owns a buffer that the caller must release. On failure
 * the frame owns nothing.
 */
int kfsw_ftp_link_receive(struct kfsw_ftp_link *link, struct kfsw_ftp_link_frame *frame);

/** Release one received frame and invalidate its borrowed message pointers. */
void kfsw_ftp_link_release(struct kfsw_ftp_link_frame *frame);

/** Close one connection. Safe to call on a link that was never opened. */
void kfsw_ftp_link_close(struct kfsw_ftp_link *link);

/** Return whether the link currently holds a connection. */
bool kfsw_ftp_link_is_open(const struct kfsw_ftp_link *link);

/** Bind and listen on the file-transfer port with a one-connection backlog. */
int kfsw_ftp_link_listen(struct kfsw_ftp_listener *listener);

/**
 * Wait up to @p timeout_ms for one inbound connection.
 *
 * Returns 0 and an open link on success, -EAGAIN when the wait expired.
 */
int kfsw_ftp_link_accept(struct kfsw_ftp_listener *listener, struct kfsw_ftp_link *link,
			 uint32_t timeout_ms);

/** Stop listening and release the endpoint. */
void kfsw_ftp_link_listener_close(struct kfsw_ftp_listener *listener);

#endif
