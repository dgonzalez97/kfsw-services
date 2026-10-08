#ifndef KFSW_SERVICES_LOG_HISTORY_H
#define KFSW_SERVICES_LOG_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFSW_LOG_TEXT_SIZE 192U
#define KFSW_LOG_HISTORY_MAX_READ 32U

/**
 * Bytes a held record keeps: the message as a cbprintf package (format string
 * address, arguments and any strings from RAM), or its text when the package
 * does not fit.
 */
#define KFSW_LOG_ENCODED_SIZE 128U

struct kfsw_log_record {
	uint64_t sequence;
	uint64_t uptime_ms;
	uint8_t module;
	uint8_t severity;
	bool truncated;
	char text[KFSW_LOG_TEXT_SIZE];
};

/** A held record as stored, for a reader that formats it elsewhere. */
struct kfsw_log_encoded {
	uint64_t sequence;
	uint64_t uptime_ms;
	uint8_t module;
	uint8_t severity;
	bool truncated;
	/** True for a cbprintf package, false for text that did not fit one. */
	bool package;
	uint16_t size;
	uint8_t data[KFSW_LOG_ENCODED_SIZE] __aligned(8);
};

/** Sequence interval [first, end); an empty history has first == end. */
struct kfsw_log_history_window {
	uint64_t first;
	uint64_t end;
	uint64_t overwritten;
};

/** Snapshot the bounds of the newest count records, without consuming them. */
int kfsw_log_history_window(uint16_t count, struct kfsw_log_history_window *window);

/**
 * Read one sequence as text. -ENOENT means it was overwritten or never
 * recorded.
 */
int kfsw_log_history_get(uint64_t sequence, struct kfsw_log_record *record);

/** Read one sequence as it is held, package or text. */
int kfsw_log_history_get_encoded(uint64_t sequence, struct kfsw_log_encoded *record);

/** Format a held record into text; newlines become spaces. */
void kfsw_log_history_format(const struct kfsw_log_encoded *encoded,
			     struct kfsw_log_record *record);

#ifdef __cplusplus
}
#endif

#endif
