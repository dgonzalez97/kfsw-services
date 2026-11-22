#include <errno.h>
#include <string.h>
#include <psa/crypto.h>
#include <csp/csp.h>
#include <kfsw/comms/csp.h>
#include <kfsw/platform/storage.h>
#include <kfsw/services/fwu_lite.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
#include <nsi_host_trampolines.h>
#endif

#define CONNECTION_OPTIONS                                                                         \
	((CSP_O_CRC32 | KFSW_CSP_AUTHENTICATED_CONNECTION) |                                       \
	 (IS_ENABLED(CONFIG_KFSW_FWU_LITE_RDP) ? CSP_O_RDP : 0))
#define STALE_LIMIT 16U
static atomic_t request_sequence;

static int next_request(struct kfsw_fwu_lite_message *request, uint8_t opcode,
			const struct kfsw_fwu_handle *handle)
{
	atomic_val_t previous;

	/* Saturate rather than reusing a request identity during this boot. */
	do {
		previous = atomic_get(&request_sequence);
		if ((uint32_t)previous == UINT32_MAX) {
			return -EOVERFLOW;
		}
	} while (!atomic_cas(&request_sequence, previous, (atomic_val_t)((uint32_t)previous + 1U)));
	*request = (struct kfsw_fwu_lite_message){.version = KFSW_FWU_LITE_VERSION,
						  .direction = KFSW_FWU_LITE_REQUEST,
						  .opcode = opcode,
						  .request_id = (uint32_t)previous + 1U};
	if (handle != NULL) {
		request->transfer_id = handle->transfer_id;
		request->generation = handle->generation;
	}
	return 0;
}

static int remote_error(uint8_t status)
{
	switch (status) {
	case KFSW_FWU_LITE_STATUS_OK:
		return 0;
	case KFSW_FWU_LITE_STATUS_NOT_FOUND:
		return -ENOENT;
	case KFSW_FWU_LITE_STATUS_OWNER:
		return -EACCES;
	case KFSW_FWU_LITE_STATUS_STALE:
		return -ESTALE;
	case KFSW_FWU_LITE_STATUS_BUSY:
		return -EBUSY;
	case KFSW_FWU_LITE_STATUS_BAD_BLOCK:
	case KFSW_FWU_LITE_STATUS_BAD_IMAGE:
		return -EILSEQ;
	case KFSW_FWU_LITE_STATUS_OUT_OF_ORDER:
		return -ESPIPE;
	case KFSW_FWU_LITE_STATUS_TOO_LARGE:
		return -EFBIG;
	case KFSW_FWU_LITE_STATUS_INVALID:
		return -EINVAL;
	default:
		return -EIO;
	}
}

static bool correlated(const struct kfsw_fwu_lite_message *request,
		       const struct kfsw_fwu_lite_message *reply, struct kfsw_fwu_status *status,
		       const struct kfsw_fwu_image *image, uint32_t accepted, uint32_t crc)
{
	if (reply->direction != KFSW_FWU_LITE_REPLY || reply->opcode != request->opcode ||
	    reply->request_id != request->request_id || kfsw_fwu_lite_receipt(reply, status) != 0) {
		return false;
	}
	if ((request->transfer_id != 0U && reply->transfer_id != request->transfer_id) ||
	    (request->generation != 0U && reply->generation != request->generation)) {
		return false;
	}
	if (reply->status != KFSW_FWU_LITE_STATUS_OK) {
		return true;
	}
	if (reply->transfer_id == 0U || reply->generation == 0U || !status->image.digest_present ||
	    status->image.block_size != KFSW_FWU_LITE_MAX_BLOCK_SIZE || status->image.size == 0U ||
	    status->aborted != (request->opcode == KFSW_FWU_LITE_OP_ABORT)) {
		return false;
	}
	if (((status->state == KFSW_FWU_VERIFIED || status->state == KFSW_FWU_READY) &&
	     (status->received != status->image.size || status->committed != status->image.size ||
	      status->actual_crc32 != status->image.crc32)) ||
	    (status->swap_scheduled && status->state != KFSW_FWU_READY) ||
	    (request->opcode == KFSW_FWU_LITE_OP_BLOCK && status->state != KFSW_FWU_RECEIVING) ||
	    (request->opcode == KFSW_FWU_LITE_OP_ABORT &&
	     (status->state != KFSW_FWU_IDLE || status->received != 0U ||
	      status->committed != 0U))) {
		return false;
	}
	if (image != NULL && !kfsw_fwu_lite_same_image(image, &status->image)) {
		return false;
	}
	if (accepted != UINT32_MAX &&
	    (status->received != accepted || status->actual_crc32 != crc)) {
		return false;
	}
	if (request->opcode == KFSW_FWU_LITE_OP_VERIFY && status->state != KFSW_FWU_VERIFIED &&
	    status->state != KFSW_FWU_READY) {
		return false;
	}
	if (request->opcode == KFSW_FWU_LITE_OP_START_FLASHING && status->state != KFSW_FWU_READY) {
		return false;
	}
	return true;
}

static csp_packet_t *read_before_deadline(csp_conn_t *connection, int64_t deadline)
{
	for (;;) {
		int64_t remaining = deadline - k_uptime_get();
		csp_packet_t *packet;

		if (remaining <= 0) {
			return NULL;
		}
		/* RDP clamps nonzero reads to its connection timeout. Poll instead. */
		packet = csp_read(connection,
				  IS_ENABLED(CONFIG_KFSW_FWU_LITE_RDP) ? 0U : (uint32_t)remaining);
		if (packet != NULL || !IS_ENABLED(CONFIG_KFSW_FWU_LITE_RDP)) {
			return packet;
		}
		k_sleep(K_MSEC(1));
	}
}

static int exchange(csp_conn_t *connection, const struct kfsw_fwu_lite_message *request,
		    struct kfsw_fwu_status *status, const struct kfsw_fwu_image *image,
		    uint32_t accepted, uint32_t crc)
{
	csp_packet_t *packet = csp_buffer_get(0);
	int64_t deadline = k_uptime_get() + kfsw_fwu_lite_get_timeout_ms();
	size_t encoded;
	bool incompatible = false;
	int result;

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
	for (unsigned int dropped = 0U; dropped < STALE_LIMIT; dropped++) {
		struct kfsw_fwu_lite_message reply;
		struct kfsw_fwu_status candidate = {0};
		bool match = false;

		packet = read_before_deadline(connection, deadline);
		if (packet == NULL) {
			break;
		}
		if (k_uptime_get() >= deadline) {
			csp_buffer_free(packet);
			break;
		}
		if (packet->length >= KFSW_FWU_LITE_HEADER_SIZE &&
		    packet->length <= CSP_BUFFER_SIZE && packet->data[0] != KFSW_FWU_LITE_VERSION &&
		    packet->data[1] == KFSW_FWU_LITE_REPLY && packet->data[2] == request->opcode &&
		    sys_get_be32(packet->data + 4) == request->request_id) {
			incompatible = true;
		}
		/* Decode in the owned packet after checking its physical capacity. */
		if (packet->length <= CSP_BUFFER_SIZE &&
		    kfsw_fwu_lite_decode(packet->data, packet->length, &reply) == 0) {
			match = correlated(request, &reply, &candidate, image, accepted, crc);
			result = remote_error(reply.status);
		}
		csp_buffer_free(packet);
		if (k_uptime_get() >= deadline) {
			break;
		}
		if (match) {
			*status = candidate;
			return result;
		}
	}
	return incompatible ? -EPROTONOSUPPORT : -ETIMEDOUT;
}

static int retry(csp_conn_t *connection, const struct kfsw_fwu_lite_message *request,
		 struct kfsw_fwu_status *status, const struct kfsw_fwu_image *image,
		 uint32_t accepted, uint32_t crc, uint32_t *resent)
{
	for (unsigned int attempt = 0U;; attempt++) {
		int result = exchange(connection, request, status, image, accepted, crc);
		if (result != -ETIMEDOUT || attempt >= kfsw_fwu_lite_get_retries()) {
			return result;
		}
		if (resent != NULL) {
			(*resent)++;
		}
	}
}

static csp_conn_t *connect_node(uint16_t node)
{
	return csp_connect(CSP_PRIO_NORM, node, CONFIG_KFSW_FWU_LITE_CSP_PORT,
			   kfsw_fwu_lite_get_timeout_ms(), CONNECTION_OPTIONS);
}

static int control(uint16_t node, uint8_t opcode, const struct kfsw_fwu_handle *handle,
		   struct kfsw_fwu_status *status)
{
	struct kfsw_fwu_lite_message request;
	csp_conn_t *connection;
	int result;

	if (node == 0U || node >= KFSW_CSP_BROADCAST_ADDRESS || status == NULL ||
	    (handle == NULL && opcode != KFSW_FWU_LITE_OP_STATUS)) {
		return -EINVAL;
	}
	result = next_request(&request, opcode, handle);
	if (result != 0) {
		return result;
	}
	connection = connect_node(node);
	if (connection == NULL) {
		return -ENOTCONN;
	}
	result = retry(connection, &request, status, NULL, UINT32_MAX, 0U, NULL);
	csp_close(connection);
	return result;
}

int kfsw_fwu_lite_remote_status(uint16_t node, const struct kfsw_fwu_handle *handle,
				struct kfsw_fwu_status *status)
{
	return control(node, KFSW_FWU_LITE_OP_STATUS, handle, status);
}

int kfsw_fwu_lite_remote_verify(uint16_t node, const struct kfsw_fwu_handle *handle,
				struct kfsw_fwu_status *status)
{
	return control(node, KFSW_FWU_LITE_OP_VERIFY, handle, status);
}

int kfsw_fwu_lite_remote_activate(uint16_t node, const struct kfsw_fwu_handle *handle,
				  struct kfsw_fwu_status *status)
{
	return control(node, KFSW_FWU_LITE_OP_START_FLASHING, handle, status);
}

int kfsw_fwu_lite_remote_abort(uint16_t node, const struct kfsw_fwu_handle *handle,
			       struct kfsw_fwu_status *status)
{
	return control(node, KFSW_FWU_LITE_OP_ABORT, handle, status);
}

int kfsw_fwu_lite_start_flashing(uint16_t node)
{
	struct kfsw_fwu_status status = {0};
	int result = kfsw_fwu_lite_remote_status(node, NULL, &status);
	struct kfsw_fwu_handle handle = status.handle;

	return result == 0 ? kfsw_fwu_lite_remote_activate(node, &handle, &status) : result;
}
/*
 * The image can be a node file or, on a host process, a host file. Paths under
 * the node's mount point are node files; boards don't build the host branch.
 */
struct image_source {
	bool host;
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	int host_fd;
#endif

#if CONFIG_FILE_SYSTEM
	struct fs_file_t file;
#endif
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

#if CONFIG_FILE_SYSTEM
	fs_file_t_init(&source->file);
	return fs_open(&source->file, path, FS_O_READ);
#else
	return -ENOTSUP;
#endif
}

static ssize_t source_read(struct image_source *source, void *buffer, size_t size)
{
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	if (source->host) {
		long bytes = nsi_host_read(source->host_fd, buffer, size);

		return (bytes < 0) ? -EIO : (ssize_t)bytes;
	}
#endif
#if CONFIG_FILE_SYSTEM
	return fs_read(&source->file, buffer, size);
#else
	ARG_UNUSED(source);
	ARG_UNUSED(buffer);
	ARG_UNUSED(size);
	return -ENOTSUP;
#endif
}

static void source_close(struct image_source *source)
{
#if CONFIG_KFSW_FWU_LITE_HOST_FILES
	if (source->host) {
		(void)nsi_host_close(source->host_fd);
		return;
	}
#endif
#if CONFIG_FILE_SYSTEM
	(void)fs_close(&source->file);
#else
	ARG_UNUSED(source);
#endif
}

static int describe_file(const char *path, struct kfsw_fwu_image *image)
{
	struct image_source source;
	uint8_t block[KFSW_FWU_LITE_MAX_BLOCK_SIZE];
	psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
	size_t digest_size;
	int result = source_open(&source, path);

	if (result != 0) {
		return result;
	}
	*image = (struct kfsw_fwu_image){.block_size = KFSW_FWU_LITE_MAX_BLOCK_SIZE,
					 .digest_present = true};
	if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		result = -EIO;
		goto out;
	}
	while (true) {
		ssize_t bytes = source_read(&source, block, sizeof(block));
		if (bytes < 0) {
			result = (int)bytes;
			break;
		}
		if (bytes == 0) {
			break;
		}
		if ((size_t)bytes > sizeof(block) || (uint32_t)bytes > UINT32_MAX - image->size) {
			result = -EFBIG;
			break;
		}
		image->size += (uint32_t)bytes;
		image->crc32 = crc32_ieee_update(image->crc32, block, (size_t)bytes);
		if (psa_hash_update(&hash, block, (size_t)bytes) != PSA_SUCCESS) {
			result = -EIO;
			break;
		}
	}
	if (result == 0 && psa_hash_finish(&hash, image->sha256, sizeof(image->sha256),
					   &digest_size) != PSA_SUCCESS) {
		result = -EIO;
	}
	if (result == 0 && image->size == 0U) {
		result = -EINVAL;
	}
out:
	(void)psa_hash_abort(&hash);
	source_close(&source);
	return result;
}

static int upload(csp_conn_t *connection, const char *path, const struct kfsw_fwu_image *image,
		  struct kfsw_fwu_status *status, uint32_t *resent)
{
	struct kfsw_fwu_handle handle = status->handle;
	struct kfsw_fwu_lite_message request;
	struct image_source source;
	psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
	uint8_t block[KFSW_FWU_LITE_MAX_BLOCK_SIZE];
	uint8_t digest[KFSW_FWU_SHA256_SIZE];
	uint32_t offset = 0U;
	uint32_t crc = 0U;
	uint32_t resume = status->received;
	size_t digest_size;
	int result = source_open(&source, path);

	if (result != 0) {
		return result;
	}
	if (resume > image->size || (resume != image->size && resume % image->block_size != 0U)) {
		result = -EBADMSG;
		goto out;
	}
	if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		result = -EIO;
		goto out;
	}
	if (resume == 0U && status->actual_crc32 != 0U) {
		result = -EILSEQ;
		goto out;
	}
	while (offset < image->size) {
		size_t count = MIN(sizeof(block), image->size - offset);
		ssize_t bytes = source_read(&source, block, count);
		if (bytes != (ssize_t)count) {
			result = bytes < 0 ? (int)bytes : -ESTALE;
			goto out;
		}
		crc = crc32_ieee_update(crc, block, count);
		if (psa_hash_update(&hash, block, count) != PSA_SUCCESS) {
			result = -EIO;
			goto out;
		}
		if (offset < resume) {
			if (offset + count == resume && crc != status->actual_crc32) {
				result = -EILSEQ;
				goto out;
			}
		} else {
			result = next_request(&request, KFSW_FWU_LITE_OP_BLOCK, &handle);
			if (result != 0) {
				goto out;
			}
			request.offset = offset;
			request.crc32 = crc32_ieee(block, count);
			request.data = block;
			request.data_size = count;
			result = retry(connection, &request, status, image,
				       offset + (uint32_t)count, crc, resent);
			if (result != 0) {
				goto out;
			}
		}
		offset += (uint32_t)count;
	}
	if (source_read(&source, block, 1U) != 0 || crc != image->crc32 ||
	    psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) != PSA_SUCCESS ||
	    memcmp(digest, image->sha256, sizeof(digest)) != 0) {
		result = -ESTALE;
	}
out:
	(void)psa_hash_abort(&hash);
	source_close(&source);
	return result;
}

int kfsw_fwu_lite_send_file(uint16_t node, const char *path, struct kfsw_fwu_status *receipt,
			    uint32_t *blocks_resent)
{
	struct kfsw_fwu_image image;
	struct kfsw_fwu_status status = {0};
	struct kfsw_fwu_lite_message request;
	uint8_t descriptor[KFSW_FWU_LITE_DESCRIPTOR_SIZE];
	csp_conn_t *connection;
	int result;

	if (receipt != NULL) {
		*receipt = status;
	}
	if (blocks_resent != NULL) {
		*blocks_resent = 0U;
	}
	if (path == NULL || path[0] != '/' || node == 0U || node >= KFSW_CSP_BROADCAST_ADDRESS) {
		return -EINVAL;
	}
	result = describe_file(path, &image);
	if (result != 0) {
		return result;
	}
	connection = connect_node(node);
	if (connection == NULL) {
		return -ENOTCONN;
	}
	result = next_request(&request, KFSW_FWU_LITE_OP_STATUS, NULL);
	if (result == 0) {
		result = retry(connection, &request, &status, NULL, UINT32_MAX, 0U, NULL);
	}
	if (result == -ENOENT) {
		result = next_request(&request, KFSW_FWU_LITE_OP_BEGIN, NULL);
		if (result == 0) {
			request.transfer_id = request.request_id;
			kfsw_fwu_lite_pack_image(&image, descriptor);
			request.data = descriptor;
			request.data_size = sizeof(descriptor);
			result = retry(connection, &request, &status, &image, UINT32_MAX, 0U, NULL);
		}
	}
	if (result == 0 && !kfsw_fwu_lite_same_image(&image, &status.image)) {
		result = -ESTALE;
	}
	if (result == 0 && status.state != KFSW_FWU_RECEIVING &&
	    status.state != KFSW_FWU_VERIFIED && status.state != KFSW_FWU_READY) {
		result = -EINVAL;
	}
	if (result == 0) {
		result = upload(connection, path, &image, &status, blocks_resent);
	}
	csp_close(connection);
	if (receipt != NULL) {
		*receipt = status;
	}
	return result;
}
