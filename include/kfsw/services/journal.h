#ifndef KFSW_SERVICES_JOURNAL_H
#define KFSW_SERVICES_JOURNAL_H

#include <kfsw/services/event.h>

/** A durable event. UTC is sampled when the worker writes, not in the emitter. */
struct kfsw_journal_record {
	uint64_t sequence;
	uint64_t boot;
	int64_t utc_seconds;
	bool utc_valid;
	struct kfsw_event_record event;
};

/** Journal status; counters describe this process and may wrap. */
struct kfsw_journal_stats {
	uint16_t held;
	uint16_t queued;
	uint32_t dropped;
	uint32_t errors;
	uint32_t corrupt;
	int last_error;
	bool ready;
};

/** Initialize once and start the writer. Failed storage initialization is retried. */
int kfsw_journal_start(void);

/** Queue a selected event without blocking. Safe in interrupt context. */
void kfsw_journal_submit(const struct kfsw_event_record *event);

/** Flush one bounded batch on the calling thread. Never call from interrupt context. */
int kfsw_journal_flush(void);

/** Read a committed record by age, newest first. No records are consumed. */
int kfsw_journal_get(uint16_t age, struct kfsw_journal_record *record);

/** Copy the current status. */
void kfsw_journal_get_stats(struct kfsw_journal_stats *stats);

#endif
