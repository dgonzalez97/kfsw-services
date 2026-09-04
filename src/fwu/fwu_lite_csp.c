#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <csp/csp.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/fwu.h>
#include <kfsw/services/fwu_lite.h>
#include <kfsw/services/log.h>

#if CONFIG_KFSW_FWU_LITE_HOST_FILES
#include <nsi_host_trampolines.h>
#endif

/* The only unit here that speaks CSP. Everything above it works on decoded
 * messages, which is why the protocol can be tested without a link at all.
 */

#define CONNECTION_OPTIONS (CSP_O_CRC32 | (IS_ENABLED(CONFIG_KFSW_FWU_LITE_RDP) ? CSP_O_RDP : 0))
#define SOCKET_OPTIONS                                                                             \
	(CSP_SO_CRC32REQ | (IS_ENABLED(CONFIG_KFSW_FWU_LITE_RDP) ? CSP_SO_RDPREQ : 0))

#define WIRE_BUFFER_SIZE (KFSW_FWU_LITE_HEADER_SIZE + KFSW_FWU_LITE_MAX_BLOCK_SIZE)

BUILD_ASSERT(WIRE_BUFFER_SIZE <= CSP_BUFFER_SIZE,
	     "One block message must fit a CSP buffer; reduce KFSW_FWU_LITE_BLOCK_SIZE");

static K_THREAD_STACK_DEFINE(server_stack, CONFIG_KFSW_FWU_LITE_STACK_SIZE);
static struct k_thread server_thread;
static bool server_running;

static void serve_packet(csp_conn_t *connection, csp_packet_t *packet)
{
	struct kfsw_fwu_lite_message request;
	struct kfsw_fwu_lite_message reply;
	csp_packet_t *response;
	size_t encoded = 0U;

	if (kfsw_fwu_lite_decode(packet->data, packet->length, &request) != 0) {
		/* A packet that cannot be decoded has no request id to answer,
		 * so there is nothing to reply to. */
		csp_buffer_free(packet);
		return;
	}

	(void)kfsw_fwu_lite_handle(&request, &reply);

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
	/* csp_send() takes ownership, including on transmit failure. */
	csp_send(connection, response);
}

static void server_entry(void *first, void *second, void *third)
{
	static csp_socket_t socket;
	csp_conn_t *connection;

	ARG_UNUSED(first);
	ARG_UNUSED(second);
	ARG_UNUSED(third);

	socket.opts = SOCKET_OPTIONS;
	if (csp_bind(&socket, CONFIG_KFSW_FWU_LITE_CSP_PORT) != CSP_ERR_NONE) {
		kfsw_log_error("Firmware upload could not bind port %d",
			       CONFIG_KFSW_FWU_LITE_CSP_PORT);
		return;
	}
	if (csp_listen(&socket, 1) != CSP_ERR_NONE) {
		kfsw_log_error("Firmware upload could not listen");
		return;
	}

	kfsw_log_info("Firmware upload server on CSP port %d", CONFIG_KFSW_FWU_LITE_CSP_PORT);

	while (true) {
		connection = csp_accept(&socket, CSP_MAX_TIMEOUT);
		if (connection == NULL) {
			continue;
		}

		while (true) {
			/* The same timeout the sender waits for a reply with. A
			 * shorter one would drop a connection between blocks on a
			 * slow link, where one block and its turnaround can take
			 * longer than a second. */
			csp_packet_t *packet =
				csp_read(connection, CONFIG_KFSW_FWU_LITE_TIMEOUT_MS);

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
	if (server_running) {
		return -EALREADY;
	}

	(void)k_thread_create(&server_thread, server_stack, K_THREAD_STACK_SIZEOF(server_stack),
			      server_entry, NULL, NULL, NULL, CONFIG_KFSW_FWU_LITE_PRIORITY, 0,
			      K_NO_WAIT);
	k_thread_name_set(&server_thread, "kfsw_fwu_lite");
	server_running = true;
	return 0;
}

/* Send one message and wait for its reply. */
static int exchange(csp_conn_t *connection, const struct kfsw_fwu_lite_message *request,
		    struct kfsw_fwu_lite_message *reply, uint8_t *reply_wire)
{
	csp_packet_t *packet;
	csp_packet_t *response;
	size_t encoded = 0U;
	int result;

	packet = csp_buffer_get(0);
	if (packet == NULL) {
		return -ENOBUFS;
	}

	result = kfsw_fwu_lite_encode(request, packet->data, CSP_BUFFER_SIZE, &encoded);
	if (result != 0) {
		csp_buffer_free(packet);
		return result;
	}
	packet->length = (uint16_t)encoded;
	csp_send(connection, packet);

	response = csp_read(connection, CONFIG_KFSW_FWU_LITE_TIMEOUT_MS);
	if (response == NULL) {
		return -ETIMEDOUT;
	}

	/* The reply payload is copied out before the buffer is released, so the
	 * decoded message does not point into freed memory. */
	memcpy(reply_wire, response->data, MIN((size_t)response->length, WIRE_BUFFER_SIZE));
	result = kfsw_fwu_lite_decode(reply_wire, response->length, reply);
	csp_buffer_free(response);

	return result;
}

/*
 * An image can come from the node's own filesystem or, where the node is a
 * process on a host, straight from the host. A ground station has the image on
 * the machine it runs on; requiring it to be copied into a simulated flash
 * partition first adds a step and a size limit for no benefit.
 *
 * The rule is positional and not a guess: a path under the node's mount point
 * is a node file, anything else is a host path. Nothing on a real board can
 * take the host branch, because it is not compiled there.
 */
struct image_source {
	bool host;
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	int host_fd;
#endif
	struct fs_file_t file;
};

static bool path_is_host(const char *path)
{
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	return strncmp(path, KFSW_STORAGE_MOUNT_POINT "/", sizeof(KFSW_STORAGE_MOUNT_POINT)) != 0;
#else
	ARG_UNUSED(path);
	return false;
#endif
}

static int source_open(struct image_source *source, const char *path)
{
	source->host = path_is_host(path);

#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	if (source->host) {
		/* O_RDONLY is zero on every host this runs on. */
		source->host_fd = nsi_host_open(path, 0);
		return (source->host_fd < 0) ? -ENOENT : 0;
	}
#endif

	fs_file_t_init(&source->file);
	return fs_open(&source->file, path, FS_O_READ);
}

static ssize_t source_read(struct image_source *source, void *buffer, size_t size)
{
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	if (source->host) {
		long bytes = nsi_host_read(source->host_fd, buffer, size);

		return (bytes < 0) ? -EIO : (ssize_t)bytes;
	}
#endif
	return fs_read(&source->file, buffer, size);
}

static void source_close(struct image_source *source)
{
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	if (source->host) {
		(void)nsi_host_close(source->host_fd);
		return;
	}
#endif
	(void)fs_close(&source->file);
}

/* Whole-file checksum, computed by reading the file rather than holding it. */
static int file_size_and_crc(const char *path, uint32_t *size, uint32_t *crc)
{
	uint8_t chunk[KFSW_FWU_LITE_MAX_BLOCK_SIZE];
	struct image_source source;
	uint32_t total = 0U;
	uint32_t running = 0U;
	int result;

	result = source_open(&source, path);
	if (result != 0) {
		return result;
	}

	while (true) {
		ssize_t bytes = source_read(&source, chunk, sizeof(chunk));

		if (bytes < 0) {
			result = (int)bytes;
			break;
		}
		if (bytes == 0) {
			break;
		}
		running = crc32_ieee_update(running, chunk, (size_t)bytes);
		total += (uint32_t)bytes;
	}

	source_close(&source);
	if (result != 0) {
		return result;
	}

	*size = total;
	*crc = running;
	return 0;
}

int kfsw_fwu_lite_send_file(uint16_t node, const char *path, uint32_t *blocks_resent)
{
	struct kfsw_fwu_lite_message request = {0};
	struct kfsw_fwu_lite_message reply;
	uint8_t reply_wire[WIRE_BUFFER_SIZE];
	uint8_t block[KFSW_FWU_LITE_MAX_BLOCK_SIZE];
	struct image_source source;
	bool source_opened = false;
	csp_conn_t *connection;
	uint32_t resent = 0U;
	uint32_t size = 0U;
	uint32_t crc = 0U;
	uint16_t index = 0U;
	uint32_t sent = 0U;
	int result;

	if (path == NULL) {
		return -EINVAL;
	}

	result = file_size_and_crc(path, &size, &crc);
	if (result != 0) {
		return result;
	}
	if (size == 0U) {
		return -EINVAL;
	}

	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_FWU_LITE_CSP_PORT,
				 CONFIG_KFSW_FWU_LITE_TIMEOUT_MS, CONNECTION_OPTIONS);
	if (connection == NULL) {
		return -ENOTCONN;
	}

	request.opcode = KFSW_FWU_LITE_OP_BEGIN;
	request.argument = size;
	request.extra = crc;
	result = exchange(connection, &request, &reply, reply_wire);
	if ((result == 0) && (reply.status != KFSW_FWU_LITE_STATUS_OK)) {
		kfsw_log_error("Firmware upload refused: %s",
			       kfsw_fwu_lite_status_name(reply.status));
		result = -EIO;
	}

	/* Opened a second time rather than rewound: the host interface offers no
	 * seek, and reopening is the same cost at this size. */
	if (result == 0) {
		result = source_open(&source, path);
		source_opened = (result == 0);
	}

	while ((result == 0) && (sent < size)) {
		ssize_t bytes = source_read(&source, block, sizeof(block));
		uint32_t attempt = 0U;

		if (bytes <= 0) {
			result = (bytes < 0) ? (int)bytes : -EIO;
			break;
		}

		while (true) {
			request.opcode = KFSW_FWU_LITE_OP_BLOCK;
			request.block_index = index;
			request.argument = crc32_ieee(block, (size_t)bytes);
			request.extra = 0U;
			request.data = block;
			request.data_size = (uint16_t)bytes;

			result = exchange(connection, &request, &reply, reply_wire);

			/* Silence is the ordinary way a block is lost. The
			 * transport carries its own checksum, so a damaged
			 * packet is discarded before it is ever delivered: what
			 * the sender sees is not a bad block but no answer at
			 * all. Treating that as fatal would end a transfer on
			 * the first disturbance, which is the situation this
			 * path exists to survive.
			 */
			if (result == -ETIMEDOUT) {
				attempt++;
				resent++;
				if (attempt > CONFIG_KFSW_FWU_LITE_BLOCK_RETRIES) {
					kfsw_log_error("Firmware upload block %u: no answer after "
						       "%u tries",
						       index, attempt);
					break;
				}
				result = 0;
				continue;
			}
			if (result != 0) {
				break;
			}
			if (reply.status == KFSW_FWU_LITE_STATUS_OK) {
				break;
			}

			/* A block can be written and its reply still be lost. The
			 * resend then arrives for a block the node has moved past,
			 * and the node says which one it wants instead. If that is
			 * the block after this one, the write did happen and only
			 * the acknowledgement went missing, so the sender moves on
			 * rather than resending forever.
			 */
			if ((reply.status == KFSW_FWU_LITE_STATUS_OUT_OF_ORDER) &&
			    (reply.block_index == (uint16_t)(index + 1U))) {
				break;
			}

			/* Anything else is a disagreement that resending will
			 * not resolve.
			 */
			if ((reply.status != KFSW_FWU_LITE_STATUS_BAD_BLOCK) &&
			    (reply.status != KFSW_FWU_LITE_STATUS_OUT_OF_ORDER)) {
				kfsw_log_error("Firmware upload block %u: %s", index,
					       kfsw_fwu_lite_status_name(reply.status));
				result = -EIO;
				break;
			}

			attempt++;
			resent++;
			if (attempt > CONFIG_KFSW_FWU_LITE_BLOCK_RETRIES) {
				kfsw_log_error("Firmware upload block %u failed after %u tries",
					       index, attempt);
				result = -EIO;
				break;
			}
		}

		if (result == 0) {
			sent += (uint32_t)bytes;
			index++;
		}
	}
	/* Closing what was never opened reads a structure that was never filled
	 * in, and this is the path taken whenever the node refuses the transfer
	 * before a single block is sent.
	 */
	if (source_opened) {
		source_close(&source);
	}

	if (result == 0) {
		request.opcode = KFSW_FWU_LITE_OP_VERIFY;
		request.data = NULL;
		request.data_size = 0U;
		result = exchange(connection, &request, &reply, reply_wire);
		if ((result == 0) && (reply.status != KFSW_FWU_LITE_STATUS_OK)) {
			kfsw_log_error("Firmware upload verify: %s",
				       kfsw_fwu_lite_status_name(reply.status));
			result = -EILSEQ;
		}
	}

	csp_close(connection);

	if (blocks_resent != NULL) {
		*blocks_resent = resent;
	}
	return result;
}

int kfsw_fwu_lite_start_flashing(uint16_t node)
{
	struct kfsw_fwu_lite_message request = {.opcode = KFSW_FWU_LITE_OP_START_FLASHING};
	struct kfsw_fwu_lite_message reply;
	uint8_t reply_wire[WIRE_BUFFER_SIZE];
	csp_conn_t *connection;
	int result;

	connection = csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_FWU_LITE_CSP_PORT,
				 CONFIG_KFSW_FWU_LITE_TIMEOUT_MS, CONNECTION_OPTIONS);
	if (connection == NULL) {
		return -ENOTCONN;
	}

	result = exchange(connection, &request, &reply, reply_wire);
	if ((result == 0) && (reply.status != KFSW_FWU_LITE_STATUS_OK)) {
		kfsw_log_error("Start flashing refused: %s",
			       kfsw_fwu_lite_status_name(reply.status));
		result = -EIO;
	}

	csp_close(connection);
	return result;
}
