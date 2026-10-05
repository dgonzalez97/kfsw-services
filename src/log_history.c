#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/cbprintf.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "log_history_internal.h"

#if CONFIG_KFSW_LOG_HISTORY_RETAINED
/* Out of .bss, so start-up does not clear it. */
#define KFSW_LOG_RETAINED __noinit
#else
#define KFSW_LOG_RETAINED
#endif

BUILD_ASSERT(CBPRINTF_PACKAGE_ALIGNMENT <= 8, "a held package must be aligned for cbprintf");

static KFSW_LOG_RETAINED struct kfsw_log_encoded records[CONFIG_KFSW_LOG_HISTORY_DEPTH];
static KFSW_LOG_RETAINED struct kfsw_log_retained_header header;
static struct k_spinlock history_lock;
/* In .bss either way, so it is false once per boot. */
static bool restored;

static uint32_t header_crc(const struct kfsw_log_retained_header *value)
{
	/* Over everything except the CRC itself. */
	return crc32_ieee((const uint8_t *)value, offsetof(struct kfsw_log_retained_header, crc));
}

/*
 * Changes with the revisions and with where the image keeps its read-only
 * data, which is where package format strings point.
 */
static uint32_t image_identity(void)
{
	static const char revisions[] = KFSW_BUILD_REVISIONS;
	const uintptr_t anchor = (uintptr_t)revisions;

	return crc32_ieee_update(crc32_ieee((const uint8_t *)revisions, sizeof(revisions)),
				 (const uint8_t *)&anchor, sizeof(anchor));
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
		(header.record_size == sizeof(records[0])) && (header.image == image_identity()) &&
		(header.next_sequence > 0U) && (header.crc == header_crc(&header));

	if (!valid) {
		header = (struct kfsw_log_retained_header){
			.magic = KFSW_LOG_RETAINED_MAGIC,
			.version = KFSW_LOG_RETAINED_VERSION,
			.depth = ARRAY_SIZE(records),
			.record_size = sizeof(records[0]),
			.image = image_identity(),
			.next_sequence = 1U,
		};
		publish_header();
	}
}

void kfsw_log_history_append(uint8_t module, uint8_t severity, const char *format, va_list args,
			     const char *text, bool truncated)
{
	k_spinlock_key_t key = k_spin_lock(&history_lock);
	struct kfsw_log_encoded *record;
	va_list copy;
	int size;

	restore_once();
	record = &records[(header.next_sequence - 1U) % ARRAY_SIZE(records)];
	*record = (struct kfsw_log_encoded){
		.sequence = header.next_sequence++,
		.uptime_ms = (uint64_t)k_uptime_get(),
		.module = module,
		.severity = severity,
		.truncated = truncated,
	};

	/* Packaged in place, so the record costs no stack in the caller. */
	va_copy(copy, args);
	size = cbvprintf_package(record->data, sizeof(record->data), 0U, format, copy);
	va_end(copy);
	if (size > 0) {
		record->package = true;
		record->size = (uint16_t)size;
	} else {
		size_t length = strnlen(text, sizeof(record->data));

		if (length == sizeof(record->data)) {
			length--;
			record->truncated = true;
		}
		memcpy(record->data, text, length);
		record->size = (uint16_t)length;
	}
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

int kfsw_log_history_get_encoded(uint64_t sequence, struct kfsw_log_encoded *record)
{
	k_spinlock_key_t key;
	const struct kfsw_log_encoded *held;

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
	if ((held->sequence != sequence) || (held->size > sizeof(held->data))) {
		k_spin_unlock(&history_lock, key);
		return -ENOENT;
	}
	*record = *held;
	k_spin_unlock(&history_lock, key);
	return 0;
}

struct text_sink {
	char *text;
	size_t length;
	bool full;
};

static int put_char(int c, void *context)
{
	struct text_sink *sink = context;

	if (sink->length + 1U >= KFSW_LOG_TEXT_SIZE) {
		sink->full = true;
		return c;
	}
	sink->text[sink->length++] = ((c == '\n') || (c == '\r')) ? ' ' : (char)c;
	return c;
}

void kfsw_log_history_format(const struct kfsw_log_encoded *encoded, struct kfsw_log_record *record)
{
	struct text_sink sink = {0};

	*record = (struct kfsw_log_record){
		.sequence = encoded->sequence,
		.uptime_ms = encoded->uptime_ms,
		.module = encoded->module,
		.severity = encoded->severity,
		.truncated = encoded->truncated,
	};
	sink.text = record->text;
	if (encoded->package) {
		(void)cbpprintf(put_char, &sink, (void *)encoded->data);
	} else {
		sink.length = MIN(encoded->size, sizeof(record->text) - 1U);
		memcpy(record->text, encoded->data, sink.length);
	}
	record->text[sink.length] = '\0';
	record->truncated = record->truncated || sink.full;
}

int kfsw_log_history_get(uint64_t sequence, struct kfsw_log_record *record)
{
	struct kfsw_log_encoded encoded;
	int result;

	if (record == NULL) {
		return -EINVAL;
	}
	result = kfsw_log_history_get_encoded(sequence, &encoded);
	if (result == 0) {
		kfsw_log_history_format(&encoded, record);
	}
	return result;
}

#if CONFIG_ZTEST
uint32_t kfsw_log_history_image(void)
{
	return image_identity();
}

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
