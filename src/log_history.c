#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "log_history_internal.h"

#if CONFIG_KFSW_LOG_HISTORY_RETAINED
/* Out of .bss, so start-up does not clear it. */
#define KFSW_LOG_RETAINED __noinit
#else
#define KFSW_LOG_RETAINED
#endif

static KFSW_LOG_RETAINED struct kfsw_log_record records[CONFIG_KFSW_LOG_HISTORY_DEPTH];
static KFSW_LOG_RETAINED struct kfsw_log_retained_header header;
static struct k_spinlock history_lock;
/* In .bss either way, so it is false once per boot. */
static bool restored;

static uint32_t header_crc(const struct kfsw_log_retained_header *value)
{
	/* Over everything except the CRC itself. */
	return crc32_ieee((const uint8_t *)value, offsetof(struct kfsw_log_retained_header, crc));
}

/* Caller holds the lock. */
static void publish_header(void)
{
	header.crc = header_crc(&header);
}

/*
 * Decide once per boot whether the retained ring belongs to this image. A
 * mismatch starts clean; individual records are checked as they are read.
 * Caller holds the lock.
 */
static void restore_once(void)
{
	bool valid;

	if (restored) {
		return;
	}
	restored = true;

	valid = (header.magic == KFSW_LOG_RETAINED_MAGIC) &&
		(header.version == KFSW_LOG_RETAINED_VERSION) &&
		(header.depth == ARRAY_SIZE(records)) &&
		(header.record_size == sizeof(records[0])) && (header.next_sequence > 0U) &&
		(header.crc == header_crc(&header));

	if (!valid) {
		header = (struct kfsw_log_retained_header){
			.magic = KFSW_LOG_RETAINED_MAGIC,
			.version = KFSW_LOG_RETAINED_VERSION,
			.depth = ARRAY_SIZE(records),
			.record_size = sizeof(records[0]),
			.next_sequence = 1U,
		};
		publish_header();
	}
}

#if CONFIG_ZTEST
uint32_t kfsw_log_history_header_crc(const struct kfsw_log_retained_header *retained)
{
	return header_crc(retained);
}

void kfsw_log_history_install_retained(const struct kfsw_log_retained_header *retained)
{
	k_spinlock_key_t key = k_spin_lock(&history_lock);

	header = *retained;
	restored = false;
	k_spin_unlock(&history_lock, key);
}
#endif

void kfsw_log_history_append(uint8_t module, uint8_t severity, const char *text, bool truncated)
{
	k_spinlock_key_t key = k_spin_lock(&history_lock);
	struct kfsw_log_record *record;

	restore_once();
	record = &records[(header.next_sequence - 1U) % ARRAY_SIZE(records)];

	*record = (struct kfsw_log_record){
		.sequence = header.next_sequence++,
		.uptime_ms = (uint64_t)k_uptime_get(),
		.module = module,
		.severity = severity,
		.truncated = truncated,
	};
	memcpy(record->text, text, strnlen(text, sizeof(record->text) - 1U));
	publish_header();
	k_spin_unlock(&history_lock, key);
}

int kfsw_log_history_window(uint16_t count, struct kfsw_log_history_window *window)
{
	k_spinlock_key_t key;
	uint64_t held;

	if ((window == NULL) || (count == 0U) || (count > KFSW_LOG_HISTORY_MAX_READ)) {
		return -EINVAL;
	}
	key = k_spin_lock(&history_lock);
	restore_once();
	held = MIN(header.next_sequence - 1U, ARRAY_SIZE(records));
	window->end = header.next_sequence;
	window->first = header.next_sequence - MIN(held, count);
	window->overwritten = header.next_sequence - 1U - held;
	k_spin_unlock(&history_lock, key);
	return 0;
}

int kfsw_log_history_get(uint64_t sequence, struct kfsw_log_record *record)
{
	k_spinlock_key_t key;
	const struct kfsw_log_record *held;

	if (record == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&history_lock);
	restore_once();
	if ((sequence == 0U) || (sequence >= header.next_sequence) ||
	    ((header.next_sequence - sequence) > ARRAY_SIZE(records))) {
		k_spin_unlock(&history_lock, key);
		return -ENOENT;
	}
	held = &records[(sequence - 1U) % ARRAY_SIZE(records)];
	/* A retained slot that disagrees with its own index did not survive. */
	if (held->sequence != sequence) {
		k_spin_unlock(&history_lock, key);
		return -ENOENT;
	}
	*record = *held;
	k_spin_unlock(&history_lock, key);
	return 0;
}
