#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "log_history_internal.h"

static struct kfsw_log_record records[CONFIG_KFSW_LOG_HISTORY_DEPTH];
static struct k_spinlock history_lock;
static uint64_t next_sequence = 1U;

void kfsw_log_history_append(uint8_t module, uint8_t severity, const char *text, bool truncated)
{
	k_spinlock_key_t key = k_spin_lock(&history_lock);
	struct kfsw_log_record *record = &records[(next_sequence - 1U) % ARRAY_SIZE(records)];

	*record = (struct kfsw_log_record){
		.sequence = next_sequence++,
		.uptime_ms = (uint64_t)k_uptime_get(),
		.module = module,
		.severity = severity,
		.truncated = truncated,
	};
	memcpy(record->text, text, strnlen(text, sizeof(record->text) - 1U));
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
	held = MIN(next_sequence - 1U, ARRAY_SIZE(records));
	window->end = next_sequence;
	window->first = next_sequence - MIN(held, count);
	window->overwritten = next_sequence - 1U - held;
	k_spin_unlock(&history_lock, key);
	return 0;
}

int kfsw_log_history_get(uint64_t sequence, struct kfsw_log_record *record)
{
	k_spinlock_key_t key;

	if (record == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&history_lock);
	if ((sequence == 0U) || (sequence >= next_sequence) ||
	    ((next_sequence - sequence) > ARRAY_SIZE(records))) {
		k_spin_unlock(&history_lock, key);
		return -ENOENT;
	}
	*record = records[(sequence - 1U) % ARRAY_SIZE(records)];
	k_spin_unlock(&history_lock, key);
	return 0;
}
