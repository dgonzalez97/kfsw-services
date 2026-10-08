#ifndef KFSW_SERVICES_FTP_INTERNAL_H
#define KFSW_SERVICES_FTP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>

#include <kfsw/services/ftp.h>

#define KFSW_FTP_ROOT_PATH KFSW_FTP_STORAGE_ROOT
#define KFSW_FTP_EXCHANGE_PATH KFSW_FTP_ROOT_PATH "/build"
#define KFSW_FTP_PROTOCOL_VERSION 1U
/*
 * Set on a PUT request by a client that understands a non-zero start offset in
 * the PUT_READY reply. A client without it gets the original behaviour: any
 * partial is discarded and the upload starts from zero.
 */
#define KFSW_FTP_FLAG_RESUME 0x01U
#define KFSW_FTP_PROTOCOL_HEADER_SIZE 24U
#define KFSW_FTP_FULL_PATH_SIZE 128U

enum kfsw_ftp_opcode {
	KFSW_FTP_OP_MKDIR_REQUEST = 1,
	KFSW_FTP_OP_MKDIR_RESPONSE = 2,
	KFSW_FTP_OP_LIST_REQUEST = 3,
	KFSW_FTP_OP_LIST_ENTRY = 4,
	KFSW_FTP_OP_LIST_END = 5,
	KFSW_FTP_OP_STAT_REQUEST = 6,
	KFSW_FTP_OP_STAT_RESPONSE = 7,
	KFSW_FTP_OP_PUT_REQUEST = 8,
	KFSW_FTP_OP_PUT_READY = 9,
	KFSW_FTP_OP_PUT_DATA = 10,
	KFSW_FTP_OP_PUT_RESULT = 11,
	KFSW_FTP_OP_GET_REQUEST = 12,
	KFSW_FTP_OP_GET_INFO = 13,
	KFSW_FTP_OP_GET_DATA = 14,
	KFSW_FTP_OP_GET_RESULT = 15,
};

enum kfsw_ftp_wire_status {
	KFSW_FTP_STATUS_OK = 0,
	KFSW_FTP_STATUS_INVALID_REQUEST = 1,
	KFSW_FTP_STATUS_INVALID_PATH = 2,
	KFSW_FTP_STATUS_NOT_FOUND = 3,
	KFSW_FTP_STATUS_ALREADY_EXISTS = 4,
	KFSW_FTP_STATUS_NO_SPACE = 5,
	KFSW_FTP_STATUS_IO_ERROR = 6,
	KFSW_FTP_STATUS_INTEGRITY_ERROR = 7,
	KFSW_FTP_STATUS_BUSY = 8,
	KFSW_FTP_STATUS_UNSUPPORTED = 9,
	KFSW_FTP_STATUS_TIMEOUT = 10,
	KFSW_FTP_STATUS_CONNECTION_ERROR = 11,
	KFSW_FTP_STATUS_NOT_DIRECTORY = 12,
	KFSW_FTP_STATUS_READ_ONLY = 13,
};

/*
 * One decoded protocol message. After a decode, path and data point into the
 * buffer the message arrived in; both are borrowed until that buffer is
 * released.
 */
struct kfsw_ftp_message {
	uint8_t opcode;
	uint8_t flags;
	uint8_t status;
	uint32_t request_id;
	uint32_t offset;
	uint32_t total_size;
	uint32_t crc32;
	uint16_t path_size;
	uint16_t data_size;
	const uint8_t *path;
	const uint8_t *data;
};

struct kfsw_ftp_workspace {
	char path[KFSW_FTP_FULL_PATH_SIZE];
	char temporary_path[KFSW_FTP_FULL_PATH_SIZE];
	uint8_t chunk[KFSW_FTP_CHUNK_SIZE];
};

/* Counted where an outcome is already known, in ftp_client.c. */
void kfsw_ftp_count_transfer(uint32_t bytes, bool failed);

/* Written by the server, read by the stats getter in ftp_state.c. */
bool kfsw_ftp_server_is_busy(void);

/* Defined by the transport backend; see ftp_link.h. */
struct kfsw_ftp_link;

/*
 * One file transfer in progress. The engine keeps the file handle open between
 * an open and the matching send or receive call, and updates offset and
 * actual_crc32 as it goes.
 */
struct kfsw_ftp_transfer {
	struct kfsw_ftp_link *link;
	struct kfsw_ftp_workspace *workspace;
	struct fs_file_t file;
	uint32_t request_id;
	uint32_t total_size;
	uint32_t crc32;
	uint32_t offset;
	uint32_t actual_crc32;
	uint8_t data_opcode;
	/* True when this upload goes to the firmware update slot instead of a file.
	 */
	bool firmware;
};

/* Wire codec, path policy and status mapping. */
int kfsw_ftp_protocol_encode(uint8_t *buffer, size_t capacity,
			     const struct kfsw_ftp_message *message, size_t *encoded_size);
int kfsw_ftp_protocol_decode(const uint8_t *buffer, size_t size, struct kfsw_ftp_message *message);
bool kfsw_ftp_path_is_read_only(const char *virtual_path);

/**
 * Visit each extra root that exists as a directory, so a listing of the FTP
 * root shows them. Returns false when the visitor stopped.
 */
bool kfsw_ftp_list_roots(kfsw_ftp_list_visitor_t visitor, void *context);

/** Resolve a path a caller intends to write, refusing the read-only root. */
int kfsw_ftp_resolve_write_path(const char *virtual_path, char *resolved, size_t resolved_size);

int kfsw_ftp_resolve_path(const char *virtual_path, bool allow_root, char *resolved,
			  size_t resolved_size);
int kfsw_ftp_wire_status_to_errno(uint8_t status);
uint8_t kfsw_ftp_errno_to_wire_status(int error);
int kfsw_ftp_copy_message_path(const struct kfsw_ftp_message *message, char *path,
			       size_t path_size);

/* Local storage below the FTP root. */
/** -ENOSPC unless the volume holding path has bytes plus the margin free. */
int kfsw_ftp_check_space(const char *path, uint32_t bytes);
int kfsw_ftp_file_crc(const char *path, struct kfsw_ftp_workspace *workspace, uint32_t *file_size,
		      uint32_t *crc32);
int kfsw_ftp_make_temporary_path(const char *path, char *temporary_path,
				 size_t temporary_path_size);

/**
 * @brief Where an interrupted upload left off, if it can be continued.
 *
 * Reads the note beside the partial file and accepts it only when it describes
 * this same transfer. The offset comes from the partial's own size rather than
 * the note, because a power cut can leave the note ahead of what reached flash.
 *
 * @param path Resolved target path.
 * @param workspace Scratch buffer for the CRC pass.
 * @param total_size Size the caller intends to write.
 * @param crc32 CRC32 the caller intends to write.
 * @param offset Bytes already committed; zero when there is nothing to resume.
 * @param partial_crc32 CRC32 of those bytes.
 *
 * @retval 0 Always; a partial that does not match reports a zero offset.
 */
int kfsw_ftp_partial_resume_point(const char *path, struct kfsw_ftp_workspace *workspace,
				  uint32_t total_size, uint32_t crc32, uint32_t *offset,
				  uint32_t *partial_crc32);

/** Record what a fresh partial is going to become, so it can be continued. */
int kfsw_ftp_partial_note_write(const char *path, uint32_t total_size, uint32_t crc32);

/** Forget the note. Safe when there is none. */
void kfsw_ftp_partial_note_remove(const char *path);
int kfsw_ftp_commit_temporary(const char *path, const char *temporary_path, uint32_t actual_size,
			      uint32_t actual_crc32, uint32_t expected_size,
			      uint32_t expected_crc32);
int kfsw_ftp_local_mkdir(const char *virtual_path, struct kfsw_ftp_workspace *workspace);
int kfsw_ftp_local_stat(const char *virtual_path, struct kfsw_ftp_workspace *workspace,
			struct kfsw_ftp_stat *info);
int kfsw_ftp_local_list(const char *virtual_path, struct kfsw_ftp_workspace *workspace,
			kfsw_ftp_list_visitor_t visitor, void *context);

/* Transfer engine; see ftp_transfer.c. */
int kfsw_ftp_transfer_open_source(struct kfsw_ftp_transfer *transfer, const char *source_path);
int kfsw_ftp_transfer_send(struct kfsw_ftp_transfer *transfer);
int kfsw_ftp_transfer_open_sink(struct kfsw_ftp_transfer *transfer, const char *temporary_path);

#if CONFIG_KFSW_FWU
/**
 * Send this upload to the firmware update slot instead of a file. The PUT
 * request already has the size and CRC32 the update service needs.
 */
int kfsw_ftp_transfer_open_firmware_sink(struct kfsw_ftp_transfer *transfer);

/**
 * True when a wire path names the reserved firmware upload target.
 *
 * Takes the length because the path on the wire is not terminated.
 */
bool kfsw_ftp_path_is_firmware(const uint8_t *path, uint16_t path_size);
#endif
int kfsw_ftp_transfer_receive(struct kfsw_ftp_transfer *transfer);
int kfsw_ftp_transfer_finish(struct kfsw_ftp_transfer *transfer, const char *target_path,
			     const char *temporary_path, int result);

#endif
