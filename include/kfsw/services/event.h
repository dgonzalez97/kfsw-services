#ifndef KFSW_SERVICES_EVENT_H
#define KFSW_SERVICES_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Bounded record of what a node did, separate from the console log.
 *
 * Logging is a human-readable stream that only exists while someone is
 * watching it. An event is a numeric record: a stable identifier, a monotonic
 * timestamp, a sequence number and a small opaque payload. It survives a
 * console nobody is attached to, it can be read back over a link, and a gap in
 * the sequence is detectable.
 *
 * Events do not replace logging. A message that only helps a developer reading
 * a terminal stays a log call. A fact an operator may need to establish after
 * the moment has passed becomes an event.
 *
 * The record lives in a fixed-size RAM ring, so it does not survive a reset.
 * Persisting it is separate work and deliberately not part of this service.
 */

/** Owning component of an event identifier. Identifiers are unique per source. */
enum kfsw_event_source {
	KFSW_EVENT_SOURCE_BOOT = 1,
	KFSW_EVENT_SOURCE_COMMAND = 2,
	KFSW_EVENT_SOURCE_FTP = 3,
	KFSW_EVENT_SOURCE_APP = 4,
};

/** How much attention the event deserves. */
enum kfsw_event_severity {
	KFSW_EVENT_INFO = 0,
	KFSW_EVENT_WARNING = 1,
	KFSW_EVENT_ERROR = 2,
	KFSW_EVENT_CRITICAL = 3,
};

/**
 * Payload bytes are opaque to this service and are stored exactly as given.
 * A producer whose payload will cross a link is responsible for writing it in
 * network byte order.
 */
#define KFSW_EVENT_MAX_PAYLOAD_SIZE 16U

/** One recorded event. */
struct kfsw_event_record {
	uint64_t monotonic_us;
	/** Monotonic across the whole service; a gap means records were lost. */
	uint32_t sequence;
	uint16_t source;
	uint16_t id;
	uint8_t severity;
	uint8_t payload_size;
	uint8_t payload[KFSW_EVENT_MAX_PAYLOAD_SIZE];
};

/** Service counters, for checking that nothing was silently dropped. */
struct kfsw_event_stats {
	/** Events accepted since start. */
	uint32_t recorded;
	/** Events rejected for an invalid payload size or source. */
	uint32_t rejected;
	/** Events overwritten because the ring wrapped before they were read. */
	uint32_t overwritten;
	/** Records currently held. */
	uint16_t held;
	/** Capacity of the ring. */
	uint16_t capacity;
};

typedef bool (*kfsw_event_visitor_t)(const struct kfsw_event_record *record, void *context);

/**
 * Record one event.
 *
 * Safe to call from any thread. Never blocks on a link or a filesystem. When
 * the ring is full the oldest record is overwritten and the overwritten
 * counter increases, so losing history is visible rather than silent.
 */
void kfsw_event_emit(enum kfsw_event_source source, uint16_t id, enum kfsw_event_severity severity,
		     const void *payload, size_t payload_size);

/** Visit held records, oldest first. */
void kfsw_event_visit(kfsw_event_visitor_t visitor, void *context);

/**
 * Copy one held record by age, where 0 is the most recent.
 *
 * Returns -ENOENT when fewer records are held than requested.
 */
int kfsw_event_get(uint16_t age, struct kfsw_event_record *record);

/** Copy the current counters. */
void kfsw_event_get_stats(struct kfsw_event_stats *stats);

/** Discard held records. Counters are preserved. */
void kfsw_event_clear(void);

/** Short name for a severity, for shell output. */
const char *kfsw_event_severity_name(enum kfsw_event_severity severity);

/** Short name for a source, for shell output. */
const char *kfsw_event_source_name(enum kfsw_event_source source);

#if CONFIG_KFSW_PARAM
/** Parameter table owned by this service, in the service band. */
#define KFSW_EVENT_PARAM_TABLE_ID 27U
/** Stable logical name paired with KFSW_EVENT_PARAM_TABLE_ID. */
#define KFSW_EVENT_PARAM_TABLE_NAME "event"

/** Event record counters. */
extern const struct kfsw_param_definition_set kfsw_event_param_definitions;
#endif

#ifdef __cplusplus
}
#endif

#endif
