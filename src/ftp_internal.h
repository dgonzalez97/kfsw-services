#ifndef KFSW_SERVICES_FTP_INTERNAL_H
#define KFSW_SERVICES_FTP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include <csp/csp.h>

#include <kfsw/services/ftp.h>

#define KFSW_FTP_ROOT_PATH KFSW_FTP_STORAGE_ROOT
#define KFSW_FTP_EXCHANGE_PATH KFSW_FTP_ROOT_PATH "/build"
#define KFSW_FTP_PROTOCOL_VERSION 1U
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
};

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

int kfsw_ftp_protocol_encode(uint8_t *buffer, size_t capacity,
			     const struct kfsw_ftp_message *message, size_t *encoded_size);
int kfsw_ftp_protocol_decode(const uint8_t *buffer, size_t size, struct kfsw_ftp_message *message);
int kfsw_ftp_resolve_path(const char *virtual_path, bool allow_root, char *resolved,
			  size_t resolved_size);
int kfsw_ftp_wire_status_to_errno(uint8_t status);
uint8_t kfsw_ftp_errno_to_wire_status(int error);
int kfsw_ftp_send_message(csp_conn_t *connection, const struct kfsw_ftp_message *message);
int kfsw_ftp_receive_message(csp_conn_t *connection, struct kfsw_ftp_message *message,
			     csp_packet_t **packet);
int kfsw_ftp_copy_message_path(const struct kfsw_ftp_message *message, char *path,
			       size_t path_size);
int kfsw_ftp_file_crc(const char *path, struct kfsw_ftp_workspace *workspace, uint32_t *file_size,
		      uint32_t *crc32);
int kfsw_ftp_make_temporary_path(const char *path, char *temporary_path,
				 size_t temporary_path_size);
int kfsw_ftp_commit_temporary(const char *path, const char *temporary_path, uint32_t actual_size,
			      uint32_t actual_crc32, uint32_t expected_size,
			      uint32_t expected_crc32);

#endif
