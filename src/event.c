#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/event.h>

/*
 * A fixed ring of records with a monotonic sequence number. Emitting takes a
 * short spinlock rather than a mutex so any context can record, including one
 * that must not sleep. Nothing here touches a link or a filesystem.
 */

static struct kfsw_event_record ring[CONFIG_KFSW_EVENT_RING_DEPTH];
static struct k_spinlock ring_lock;

static uint32_t next_sequence;
static uint32_t recorded_count;
static uint32_t rejected_count;
static uint32_t overwritten_count;
static uint16_t held_count;
/* Index of the oldest held record. */
static uint16_t oldest_index;

static bool source_is_known(enum kfsw_event_source source)
{
	switch (source) {
	case KFSW_EVENT_SOURCE_BOOT:
	case KFSW_EVENT_SOURCE_COMMAND:
	case KFSW_EVENT_SOURCE_FTP:
	case KFSW_EVENT_SOURCE_APP:
		return true;
	default:
		return false;
	}
}

void kfsw_event_emit(enum kfsw_event_source source, uint16_t id, enum kfsw_event_severity severity,
		     const void *payload, size_t payload_size)
{
	struct kfsw_event_record *slot;
	k_spinlock_key_t key;
	uint16_t write_index;

	if (!source_is_known(source) || (payload_size > KFSW_EVENT_MAX_PAYLOAD_SIZE) ||
	    ((payload_size != 0U) && (payload == NULL))) {
		key = k_spin_lock(&ring_lock);
		rejected_count++;
		k_spin_unlock(&ring_lock, key);
		return;
	}

	key = k_spin_lock(&ring_lock);

	write_index = (uint16_t)((oldest_index + held_count) % ARRAY_SIZE(ring));
	if (held_count == ARRAY_SIZE(ring)) {
		/* Full: the oldest record is replaced and the loss is counted. */
		overwritten_count++;
		oldest_index = (uint16_t)((oldest_index + 1U) % ARRAY_SIZE(ring));
	} else {
		held_count++;
	}

	slot = &ring[write_index];
	slot->monotonic_us = (uint64_t)k_ticks_to_us_floor64(k_uptime_ticks());
	slot->sequence = next_sequence;
	slot->source = (uint16_t)source;
	slot->id = id;
	slot->severity = (uint8_t)severity;
	slot->payload_size = (uint8_t)payload_size;
	if (payload_size != 0U) {
		memcpy(slot->payload, payload, payload_size);
	}
	memset(&slot->payload[payload_size], 0, sizeof(slot->payload) - payload_size);

	next_sequence++;
	recorded_count++;

	k_spin_unlock(&ring_lock, key);
}

void kfsw_event_visit(kfsw_event_visitor_t visitor, void *context)
{
	struct kfsw_event_record copy;
	uint16_t index = 0U;

	if (visitor == NULL) {
		return;
	}
	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&ring_lock);

		if (index >= held_count) {
			k_spin_unlock(&ring_lock, key);
			return;
		}
		/*
		 * Copy under the lock and call the visitor outside it, so a
		 * visitor that prints cannot hold off a producer.
		 */
		copy = ring[(oldest_index + index) % ARRAY_SIZE(ring)];
		k_spin_unlock(&ring_lock, key);

		if (!visitor(&copy, context)) {
			return;
		}
		index++;
	}
}

int kfsw_event_get(uint16_t age, struct kfsw_event_record *record)
{
	k_spinlock_key_t key;
	int result = 0;

	if (record == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&ring_lock);
	if (age >= held_count) {
		result = -ENOENT;
	} else {
		/* Age 0 is the newest, which sits at the end of the ring. */
		*record = ring[(oldest_index + held_count - 1U - age) % ARRAY_SIZE(ring)];
	}
	k_spin_unlock(&ring_lock, key);
	return result;
}

void kfsw_event_get_stats(struct kfsw_event_stats *stats)
{
	k_spinlock_key_t key;

	if (stats == NULL) {
		return;
	}
	key = k_spin_lock(&ring_lock);
	stats->recorded = recorded_count;
	stats->rejected = rejected_count;
	stats->overwritten = overwritten_count;
	stats->held = held_count;
	stats->capacity = (uint16_t)ARRAY_SIZE(ring);
	k_spin_unlock(&ring_lock, key);
}

void kfsw_event_clear(void)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	held_count = 0U;
	oldest_index = 0U;
	k_spin_unlock(&ring_lock, key);
}

const char *kfsw_event_severity_name(enum kfsw_event_severity severity)
{
	switch (severity) {
	case KFSW_EVENT_INFO:
		return "info";
	case KFSW_EVENT_WARNING:
		return "warning";
	case KFSW_EVENT_ERROR:
		return "error";
	case KFSW_EVENT_CRITICAL:
		return "critical";
	default:
		return "unknown";
	}
}

const char *kfsw_event_source_name(enum kfsw_event_source source)
{
	switch (source) {
	case KFSW_EVENT_SOURCE_BOOT:
		return "boot";
	case KFSW_EVENT_SOURCE_COMMAND:
		return "command";
	case KFSW_EVENT_SOURCE_FTP:
		return "ftp";
	case KFSW_EVENT_SOURCE_APP:
		return "app";
	default:
		return "unknown";
	}
}
