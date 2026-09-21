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

struct kfsw_log_record {
	uint64_t sequence;
	uint64_t uptime_ms;
	uint8_t module;
	uint8_t severity;
	bool truncated;
	char text[KFSW_LOG_TEXT_SIZE];
};

/** Sequence interval [first, end); an empty history has first == end. */
struct kfsw_log_history_window {
	uint64_t first;
	uint64_t end;
	uint64_t overwritten;
};

/** Snapshot the bounds of the newest count records, without consuming them. */
int kfsw_log_history_window(uint16_t count, struct kfsw_log_history_window *window);

/** Read one sequence. -ENOENT means it was overwritten or never recorded. */
int kfsw_log_history_get(uint64_t sequence, struct kfsw_log_record *record);

/** Start the optional CSP server after the router has started. */
int kfsw_log_history_server_start(void);

#ifdef __cplusplus
}
#endif

#endif
