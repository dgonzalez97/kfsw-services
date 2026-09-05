#ifndef KFSW_SERVICES_FTP_H
#define KFSW_SERVICES_FTP_H

#include <stdbool.h>
#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KFSW_FTP_MAX_PATH_SIZE 96U
#define KFSW_FTP_CHUNK_SIZE 192U
#define KFSW_FTP_STORAGE_ROOT "/kfsw/ftp"
/* Matches the Kconfig range, so a value accepted at runtime is one the
 * composition could have been built with. */
#define KFSW_FTP_TIMEOUT_MIN_MS 1000U
#define KFSW_FTP_TIMEOUT_MAX_MS 120000U

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

/** Lifetime totals for the service. Counters saturate and are never reset. */
struct kfsw_ftp_stats {
	/** Transfers that committed, in either direction. */
	uint32_t transfers;
	/** Transfers that failed or were abandoned. */
	uint32_t failures;
	/** Bytes moved by transfers that committed. */
	uint32_t bytes;
	/** True while the server is handling a transfer. */
	bool busy;
};

typedef bool (*kfsw_ftp_list_visitor_t)(const struct kfsw_ftp_entry *entry, void *context);

/**
 * Event identifiers owned by the file-transfer service.
 *
 * Numbers are stable and never reused. Transfer payloads are the peer node as
 * a big-endian u16, then the byte count and CRC32 as big-endian u32.
 */
enum kfsw_event_ftp_id {
	/** An upload committed on the peer. */
	KFSW_EVENT_FTP_PUT_DONE = 1,
	/** A download committed locally. */
	KFSW_EVENT_FTP_GET_DONE = 2,
	/** A transfer failed. Payload: peer node u16, then the errno as i32. */
	KFSW_EVENT_FTP_TRANSFER_FAILED = 3,
};

/** Read the lifetime totals. Returns -EINVAL for a NULL destination. */
int kfsw_ftp_get_stats(struct kfsw_ftp_stats *stats);

/** Reply timeout used by the next transfer; a running one keeps its value. */
uint32_t kfsw_ftp_get_timeout_ms(void);

/**
 * @brief Change the reply timeout used by new transfers.
 *
 * @retval 0 Applied to the next transfer.
 * @retval -ERANGE Outside the range the composition could have been built with.
 */
int kfsw_ftp_set_timeout_ms(uint32_t timeout_ms);

/** Whether a timeout would be accepted, without applying it. */
int kfsw_ftp_check_timeout_ms(uint32_t timeout_ms);

/** Bytes of file data carried per message. */
uint16_t kfsw_ftp_get_chunk_size(void);

/**
 * @brief Change how much file data each message carries.
 *
 * Shortening it is useful on a link that loses long frames. It can only be
 * shortened: the workspace buffer is sized at build time and the protocol codec
 * refuses anything larger.
 *
 * @retval 0 Applied to the next transfer.
 * @retval -ERANGE Zero, or larger than KFSW_FTP_CHUNK_SIZE.
 */
int kfsw_ftp_set_chunk_size(uint16_t chunk_size);

/** Whether a chunk size would be accepted, without applying it. */
int kfsw_ftp_check_chunk_size(uint16_t chunk_size);

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

#if CONFIG_KFSW_PARAM
/** Parameter table owned by this service, in the service band. */
#define KFSW_FTP_PARAM_TABLE_ID 29U
/** Stable logical name paired with KFSW_FTP_PARAM_TABLE_ID. */
#define KFSW_FTP_PARAM_TABLE_NAME "ftp"

/** File-transfer counters and the settings the service applies. */
extern const struct kfsw_param_definition_set kfsw_ftp_param_definitions;
#endif

#ifdef __cplusplus
}
#endif

#endif
