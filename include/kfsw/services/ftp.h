#ifndef KFSW_SERVICES_FTP_H
#define KFSW_SERVICES_FTP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFSW_FTP_MAX_PATH_SIZE 96U
#define KFSW_FTP_CHUNK_SIZE 192U
#define KFSW_FTP_STORAGE_ROOT "/kfsw/ftp"

enum kfsw_ftp_entry_type {
	KFSW_FTP_ENTRY_FILE = 1,
	KFSW_FTP_ENTRY_DIRECTORY = 2,
};

struct kfsw_ftp_entry {
	const char *name;
	enum kfsw_ftp_entry_type type;
	uint32_t size;
};

struct kfsw_ftp_stat {
	enum kfsw_ftp_entry_type type;
	uint32_t size;
	uint32_t crc32;
};

struct kfsw_ftp_transfer_result {
	uint32_t bytes;
	uint32_t crc32;
	uint32_t duration_ms;
};

typedef bool (*kfsw_ftp_list_visitor_t)(const struct kfsw_ftp_entry *entry, void *context);

/** Prepare the sandboxed local file-transfer root after storage is mounted. */
int kfsw_ftp_init(void);

/** Bind the RDP/CRC32 CSP endpoint and start the static server threads. */
int kfsw_ftp_start(void);

/** Stop accepting new transfers and close the server endpoint. */
int kfsw_ftp_stop(void);

/** Return whether the local service is initialized and accepting connections. */
bool kfsw_ftp_is_started(void);

/** Validate a virtual path using the same rules as the client and server. */
int kfsw_ftp_validate_path(const char *path, bool allow_root);

/**
 * List one directory. Paths are relative to the FTP root.
 *
 * A request addressed to this node's own CSP address is served directly from
 * local storage, without a connection or a route.
 */
int kfsw_ftp_list(uint16_t node, const char *path, kfsw_ftp_list_visitor_t visitor, void *context);

/**
 * Return file/directory metadata and a file CRC32.
 *
 * A request addressed to this node's own CSP address is served locally.
 */
int kfsw_ftp_stat(uint16_t node, const char *path, struct kfsw_ftp_stat *info);

/**
 * Create one directory. Parent directories must already exist.
 *
 * A request addressed to this node's own CSP address is served locally.
 */
int kfsw_ftp_mkdir(uint16_t node, const char *path);

/**
 * Stream a local FTP-root file to a remote FTP-root path.
 *
 * Transfers are between two nodes; this node's own CSP address returns
 * -ENOTSUP.
 */
int kfsw_ftp_put(uint16_t node, const char *local_path, const char *remote_path,
		 struct kfsw_ftp_transfer_result *result);

/**
 * Stream a remote FTP-root file to a local FTP-root path.
 *
 * Transfers are between two nodes; this node's own CSP address returns
 * -ENOTSUP.
 */
int kfsw_ftp_get(uint16_t node, const char *remote_path, const char *local_path,
		 struct kfsw_ftp_transfer_result *result);

#ifdef __cplusplus
}
#endif

#endif
