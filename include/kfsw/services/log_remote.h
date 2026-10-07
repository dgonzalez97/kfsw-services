#ifndef KFSW_SERVICES_LOG_REMOTE_H
#define KFSW_SERVICES_LOG_REMOTE_H

#include <stdbool.h>
#include <stdint.h>

#include <kfsw/services/journal.h>
#include <kfsw/services/log_history.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a remote read returns. */
enum kfsw_log_remote_stream {
	/** The recent log messages. */
	KFSW_LOG_REMOTE_LOG = 0,
	/** The newest journal records. */
	KFSW_LOG_REMOTE_JOURNAL = 1,
};

/** How the serving node sends log messages, set by its log_remote_format parameter. */
enum kfsw_log_remote_format {
	/** Formatted on the serving node. */
	KFSW_LOG_REMOTE_TEXT = 0,
	/** A cbprintf package: format string address, arguments and RAM strings. */
	KFSW_LOG_REMOTE_DICTIONARY = 1,
};

/** First reply of a read. Sequences describe the log; a journal read leaves them zero. */
struct kfsw_log_remote_start {
	uint8_t format;
	uint64_t first;
	uint64_t end;
	uint64_t overwritten;
};

/** One log message: text, or a package when the server sends dictionary. */
struct kfsw_log_remote_message {
	uint64_t sequence;
	uint64_t uptime_ms;
	uint8_t module;
	uint8_t severity;
	bool truncated;
	bool package;
	uint8_t size;
	uint8_t data[KFSW_LOG_TEXT_SIZE];
};

/** Callbacks of a read; return false from a record callback to stop early. */
struct kfsw_log_remote_visitor {
	void (*start)(const struct kfsw_log_remote_start *start, void *context);
	bool (*message)(const struct kfsw_log_remote_message *message, void *context);
	bool (*event)(const struct kfsw_journal_record *record, void *context);
};

/** Start the server after the CSP router has started. */
int kfsw_log_remote_server_start(void);

/** The format this node serves log messages in. */
uint8_t kfsw_log_remote_format(void);

/** Change it, as the log_remote_format parameter does. -ERANGE for an unknown format. */
int kfsw_log_remote_set_format(uint8_t format);

/**
 * @brief Read another node's newest log messages or journal records.
 *
 * Records arrive oldest first. Nothing is removed on the serving node.
 *
 * @param count Records to consider, 1 to KFSW_LOG_HISTORY_MAX_READ.
 * @param min_level Lowest severity sent for log messages, 0 to 3.
 *
 * @retval 0 Every record arrived, or a callback stopped the read.
 * @retval -EINVAL A bad argument, or @p node is 0 or broadcast or above.
 * @retval -ENOTCONN No connection to the node.
 * @retval -ETIMEDOUT The node did not answer.
 * @retval -ENOTSUP The node does not serve that stream.
 * @retval -EIO The read was cut short; the records received are valid.
 */
int kfsw_log_remote_read(uint16_t node, enum kfsw_log_remote_stream stream, uint16_t count,
			 uint8_t min_level, const struct kfsw_log_remote_visitor *visitor,
			 void *context);

#ifdef __cplusplus
}
#endif

#endif
