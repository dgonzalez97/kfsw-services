#ifndef KFSW_HK_INTERNAL_H
#define KFSW_HK_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include <kfsw/services/hk.h>

/** A report: what it collects, how often, and what it has collected. */
struct kfsw_hk_report {
	struct kfsw_hk_entry entries[CONFIG_KFSW_HK_ENTRIES];
	/* Resolved once when the report is defined. Looking a width up per
	 * entry per collection would walk the parameter list every time, which
	 * is the cost this service exists to avoid paying.
	 */
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

/** Serialise one collection into @p sample. Takes the clock and the values. */
int kfsw_hk_collect_report(struct kfsw_hk_report *entry, struct kfsw_hk_sample *sample);

/** Serialise one value at its fixed width, big-endian. */
bool kfsw_hk_clock_valid(void);

void kfsw_hk_write_value(uint8_t *out, size_t width, const struct kfsw_param_value *value);

/** Count one entry that could not be read. */
void kfsw_hk_count_entry_failure(void);

#if CONFIG_KFSW_PARAM_CSP
/**
 * Read every entry belonging to one remote node in as few exchanges as fit.
 *
 * Grouped by node because the descriptor cache holds one node at a time, so
 * alternating between two would re-download a list over the radio.
 */
int kfsw_hk_collect_remote(struct kfsw_hk_report *entry, uint16_t node,
			   struct kfsw_hk_sample *sample);
#endif

#if CONFIG_KFSW_HK_PERSISTENCE
int kfsw_hk_persist_save(void);
int kfsw_hk_persist_load(void);
#endif

#if CONFIG_KFSW_HK_STORE
int kfsw_hk_store_configure(uint8_t report, uint32_t interval_ms, uint32_t period_ms,
			    uint16_t record_size);
uint32_t kfsw_hk_store_interval(uint8_t report);
void kfsw_hk_store_forget(uint8_t report);
int kfsw_hk_store_flush(uint8_t report, const struct kfsw_hk_report *entry);
int kfsw_hk_store_bytes_needed(uint16_t record_size);
#endif

#if CONFIG_KFSW_HK_CSP
int kfsw_hk_server_start(void);
#endif

#endif /* KFSW_HK_INTERNAL_H */
