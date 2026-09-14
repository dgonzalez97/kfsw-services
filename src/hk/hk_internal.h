#ifndef KFSW_HK_INTERNAL_H
#define KFSW_HK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <kfsw/services/hk.h>

/* Collection snapshot: no history ring or scheduling state. */
struct kfsw_hk_definition {
	struct kfsw_hk_entry entries[CONFIG_KFSW_HK_ENTRIES];
	uint16_t widths[CONFIG_KFSW_HK_ENTRIES];
	uint16_t offsets[CONFIG_KFSW_HK_ENTRIES];
	uint16_t payload_bytes;
	uint16_t sequence;
	uint8_t entry_count;
};

/** A report: what it collects, how often, and what it has collected. */
struct kfsw_hk_report {
	struct kfsw_hk_entry entries[CONFIG_KFSW_HK_ENTRIES];
	/* Resolved when the report is defined. */
	uint16_t widths[CONFIG_KFSW_HK_ENTRIES];
	uint16_t offsets[CONFIG_KFSW_HK_ENTRIES];
	uint8_t entry_count;
	bool defined;
	uint16_t payload_bytes;
	uint32_t period_ms;
	int64_t next_uptime_ms;

	struct kfsw_hk_sample ring[CONFIG_KFSW_HK_HISTORY];
	uint16_t held;
	uint16_t next_slot;
	uint16_t sequence;
};

struct kfsw_hk_report *kfsw_hk_report_at(uint8_t report);
void kfsw_hk_lock(void);
void kfsw_hk_unlock(void);
void kfsw_hk_count_overwritten(void);

struct kfsw_hk_due {
	uint64_t revision;
	int64_t uptime_ms;
	uint32_t period_ms;
};

int kfsw_hk_config_begin(void);
int kfsw_hk_config_end(int result);
int kfsw_hk_prepare_definition(uint8_t report, const struct kfsw_hk_entry *entries, size_t count,
			       struct kfsw_hk_definition *definition);
void kfsw_hk_wake(void);
bool kfsw_hk_schedule_take(uint8_t report, int64_t now, struct kfsw_hk_due *due);
void kfsw_hk_schedule_finish(uint8_t report, const struct kfsw_hk_due *due, int64_t now);
int64_t kfsw_hk_schedule_wait(int64_t now);
int64_t kfsw_hk_next_due(int64_t due, uint32_t period, int64_t now);

/** Serialise one collection into @p sample. Takes the clock and the values. */
int kfsw_hk_collect_report(const struct kfsw_hk_definition *entry, struct kfsw_hk_sample *sample,
			   uint32_t *failures);

/** Serialise one value at its fixed width, big-endian. */
bool kfsw_hk_clock_valid(void);

void kfsw_hk_write_value(uint8_t *out, size_t width, const struct kfsw_param_value *value);

/** Count one entry that could not be read. */
/* Lock order: collection, storage, HK. Never hold HK during I/O. */
void kfsw_hk_storage_lock(void);
void kfsw_hk_storage_unlock(void);

#if CONFIG_KFSW_PARAM_CSP
/**
 * Read every entry of one remote node in as few exchanges as possible.
 */
int kfsw_hk_collect_remote(const struct kfsw_hk_definition *entry, uint16_t node,
			   struct kfsw_hk_sample *sample, uint32_t *failures, int64_t deadline);
#endif

#if CONFIG_KFSW_HK_PERSISTENCE
int kfsw_hk_persist_save(void);
uint64_t kfsw_hk_config_revision(void);
bool kfsw_hk_save_blocked(void);
void kfsw_hk_save_result(uint64_t revision, int result);
void kfsw_hk_restore_begin(void);
void kfsw_hk_restore_end(int result);
void kfsw_hk_restore_report(uint8_t report, const struct kfsw_hk_definition *definition,
			    uint32_t period_ms);
int kfsw_hk_persist_load(void);
#endif

#if CONFIG_KFSW_HK_STORE
int kfsw_hk_store_configure(uint8_t report, uint32_t interval_ms, uint32_t period_ms,
			    uint16_t record_size);
uint32_t kfsw_hk_store_interval(uint8_t report);
void kfsw_hk_store_forget(uint8_t report);
int kfsw_hk_store_flush(uint8_t report, uint16_t next_sequence);
int kfsw_hk_store_bytes_needed(uint16_t record_size);
#if CONFIG_KFSW_HK_PERSISTENCE
int kfsw_hk_store_restore_prepare(uint8_t report, uint32_t interval_ms, uint32_t period_ms,
				  uint16_t record_size);
void kfsw_hk_store_restore_commit(void);
uint16_t kfsw_hk_store_restore_sequence(uint8_t report);
#endif
#endif

#if CONFIG_KFSW_HK_BEACON
void kfsw_hk_beacon_stats(uint32_t *sent, uint32_t *skipped);

/* Save a beacon setting, like a period. */
void kfsw_hk_beacon_restore(uint8_t report, uint16_t node, uint32_t interval_ms);
void kfsw_hk_beacon_tick(uint8_t report, int64_t now);
int64_t kfsw_hk_beacon_wait(uint8_t report, int64_t now);
#endif

#if CONFIG_KFSW_HK_CSP
int kfsw_hk_server_start(void);

/* Forward declaration, to keep the CSP headers out. */
struct csp_conn_s;
struct csp_packet_s;

/**
 * @brief Answer one request, sending a packet per sample on @p connection.
 *
 * Separate from the accept loop so tests can call it. Frees @p request.
 */
void kfsw_hk_serve_request(struct csp_conn_s *connection, struct csp_packet_s *request);
#endif

#endif /* KFSW_HK_INTERNAL_H */
