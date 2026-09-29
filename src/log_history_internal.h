#ifndef KFSW_LOG_HISTORY_INTERNAL_H
#define KFSW_LOG_HISTORY_INTERNAL_H

#include <kfsw/services/log_history.h>

#define KFSW_LOG_RETAINED_MAGIC 0x4B464C48UL /* "KFLH" */
#define KFSW_LOG_RETAINED_VERSION 1U

/*
 * Where the reader picks up. Kept beside the records so a reset that preserves
 * RAM leaves a readable history instead of unvalidated bytes.
 */
struct kfsw_log_retained_header {
	uint32_t magic;
	uint32_t version;
	uint32_t depth;
	uint32_t record_size;
	uint64_t next_sequence;
	uint32_t crc;
};

void kfsw_log_history_append(uint8_t module, uint8_t severity, const char *text, bool truncated);

#if CONFIG_ZTEST
/** Install a retained image and re-run the once-per-boot decision on it. */
void kfsw_log_history_install_retained(const struct kfsw_log_retained_header *retained);

/** CRC a header the way the service does, so a test can build a valid one. */
uint32_t kfsw_log_history_header_crc(const struct kfsw_log_retained_header *retained);
#endif

#endif
