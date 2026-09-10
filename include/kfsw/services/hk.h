#ifndef KFSW_SERVICES_HK_H
#define KFSW_SERVICES_HK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/parameter.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Parameter table this service publishes itself in. */
#define KFSW_HK_PARAM_TABLE_ID 33U
#define KFSW_HK_PARAM_TABLE_NAME "hk"

/** Wire format version, rejected rather than guessed at when it does not match. */
#define KFSW_HK_PROTOCOL_VERSION 1U

/** Bytes before the first value in a serialised sample. */
#define KFSW_HK_HEADER_SIZE 10U

/** Set when at least one entry could not be sampled and was zero-filled. */
#define KFSW_HK_FLAG_INCOMPLETE 0x01U

/**
 * Set when the node's clock was not set, so the timestamp is zero.
 *
 * The values are still what they say they are; only the time is missing. The
 * periodic collector waits for a clock rather than filling the ring with
 * samples that cannot be placed in order, so in practice this appears only on
 * a sample collected by hand before the clock arrives.
 */
#define KFSW_HK_FLAG_CLOCK_UNSET 0x02U

/** Node number meaning "this one", so a definition need not know its own address. */
#define KFSW_HK_NODE_LOCAL 0U

/**
 * One value a report collects.
 *
 * Parameters are named by the identifier the wire already uses rather than by
 * name: a report holding sixteen names would cost 512 bytes of definition, and
 * ground has the names anyway.
 */
struct kfsw_hk_entry {
	/** Node to read from, or KFSW_HK_NODE_LOCAL. */
	uint16_t node;
	/** KFSW_PARAM_ID(table, offset). */
	uint16_t param_id;
};

/** What a report collected, and when it started collecting it. */
struct kfsw_hk_sample {
	uint32_t seconds;
	uint16_t sequence;
	uint8_t entry_count;
	uint8_t flags;
	uint16_t length;
	uint8_t data[CONFIG_KFSW_HK_SAMPLE_BYTES];
};

/** Counters this service publishes, and what it is doing now. */
struct kfsw_hk_stats {
	uint32_t collections;
	uint32_t failures;
	uint32_t entries_failed;
	uint32_t overwritten;
	uint32_t last_seconds;
	uint8_t reports;
	/** Whether the node has a wall clock, and so whether samples are timed. */
	bool clock_valid;
	/** Whether periodic collection is enabled. */
	bool enabled;
};

/** Prepare the service. Safe to call before CSP exists. */
int kfsw_hk_init(void);

/** Start collecting reports that have been given a period. */
int kfsw_hk_start(void);

#if CONFIG_KFSW_HK_CSP
/** Serve housekeeping requests from other nodes. Call after the router starts. */
int kfsw_hk_server_start(void);
#endif

/**
 * @brief Name the values a report collects, replacing whatever it held.
 *
 * Validated here rather than at collection time: every parameter must exist
 * and the values must fit one sample. A report that cannot be collected is
 * refused now, not discovered mid-pass, and the previous definition is left
 * intact when it is.
 */
int kfsw_hk_define(uint8_t report, const struct kfsw_hk_entry *entries, size_t count);

/** Forget a report's definition and everything it collected. */
int kfsw_hk_clear(uint8_t report);

/** Copy a report's definition. @p count is in and out. */
int kfsw_hk_get_definition(uint8_t report, struct kfsw_hk_entry *entries, size_t *count);

/**
 * @brief Collect a report now.
 *
 * Blocks: a remote entry waits for its node. Never call this from a parameter
 * sample callback, which runs under the table lock this needs.
 */
int kfsw_hk_collect(uint8_t report);

/**
 * @brief Read back a collected sample, counting backwards from the newest.
 *
 * @p age of 0 is the most recent. Returns -ENOENT past what the ring holds.
 */
int kfsw_hk_get(uint8_t report, uint16_t age, struct kfsw_hk_sample *sample);

/** How many samples a report is currently holding. */
int kfsw_hk_depth(uint8_t report, uint16_t *depth);

/** Collect this report every @p period_ms, or stop when it is zero. */
int kfsw_hk_set_period(uint8_t report, uint32_t period_ms);

/** Read the configured period. */
int kfsw_hk_get_period(uint8_t report, uint32_t *period_ms);

/** Copy the service counters. */
void kfsw_hk_get_stats(struct kfsw_hk_stats *stats);

/**
 * @brief Start or stop collecting on a period.
 *
 * Definitions and everything already collected are kept, so an operator who
 * quietens housekeeping during a firmware upload gets the history back by
 * turning it on again. Collecting by hand still works: disabling is a decision
 * about the schedule, not a lock on the service.
 *
 * @param enabled True to collect on each report's period.
 */
void kfsw_hk_set_enabled(bool enabled);

/**
 * @brief Whether periodic collection is enabled.
 *
 * @return True when reports with a period are being collected.
 */
bool kfsw_hk_enabled(void);

/** Definitions this service publishes as parameter table 33. */
extern const struct kfsw_param_definition_set kfsw_hk_param_definitions;

#ifdef __cplusplus
}
#endif

#endif /* KFSW_SERVICES_HK_H */
