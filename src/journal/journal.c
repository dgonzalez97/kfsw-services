#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <kfsw/platform/storage.h>
#include <kfsw/platform/wallclock.h>
#include <kfsw/services/boot.h>
#if CONFIG_KFSW_CSP
#include <kfsw/comms/csp.h>
#endif
#include "journal_store.h"

K_MSGQ_DEFINE(events, sizeof(struct kfsw_event_record), CONFIG_KFSW_JOURNAL_QUEUE_DEPTH, 4);
K_MUTEX_DEFINE(journal_lock);
static struct kfsw_journal_store store;
static struct kfsw_journal_record pending;
static bool have_pending;
static bool started;
static uint64_t boot;
static atomic_t dropped;
static atomic_t boot_seen;
static uint32_t errors;
static int last_error;

static int open_store(void)
{
	int result;

	if (store.ready && kfsw_storage_is_ready()) {
		return 0;
	}
	result = kfsw_journal_store_open(&store);
	if ((result == 0) && (boot == 0U)) {
		if (store.highest_boot >= UINT64_MAX - 1U) {
			store.ready = false;
			return -EOVERFLOW;
		}
		boot = store.highest_boot + 1U;
	}
	return result;
}

void kfsw_journal_submit(const struct kfsw_event_record *event)
{
	int boot_bit = -1;

	if (event == NULL) {
		return;
	}
	if ((event->payload_size > KFSW_EVENT_MAX_PAYLOAD_SIZE) ||
	    (event->severity > KFSW_EVENT_CRITICAL) || (event->source < KFSW_EVENT_SOURCE_BOOT) ||
	    (event->source > KFSW_EVENT_SOURCE_RESMON)) {
		atomic_inc(&dropped);
		return;
	}
	if (event->source == KFSW_EVENT_SOURCE_BOOT) {
		if ((event->id == KFSW_EVENT_BOOT_READY) ||
		    (event->id == KFSW_EVENT_BOOT_LASTWORDS)) {
			boot_bit = (int)event->id - 1;
			if (atomic_test_and_set_bit(&boot_seen, boot_bit)) {
				return;
			}
		}
	} else if (event->severity < CONFIG_KFSW_JOURNAL_MIN_SEVERITY) {
		return;
	}
	if (k_msgq_put(&events, event, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
		if (boot_bit >= 0) {
			atomic_clear_bit(&boot_seen, boot_bit);
		}
	}
}

static void timestamp(struct kfsw_journal_record *record)
{
#if CONFIG_KFSW_CSP
	struct kfsw_csp_clock clock;

	kfsw_csp_clock_get(&clock);
	record->utc_valid = kfsw_csp_clock_is_set(&clock);
	record->utc_seconds = record->utc_valid ? clock.seconds : 0;
#else
	int64_t seconds;

	record->utc_valid = (kfsw_wallclock_get(&seconds) == 0) && (seconds > 0);
	record->utc_seconds = record->utc_valid ? seconds : 0;
#endif
}

int kfsw_journal_flush(void)
{
	int result;

	k_mutex_lock(&journal_lock, K_FOREVER);
	if (!started) {
		k_mutex_unlock(&journal_lock);
		return -EACCES;
	}
	result = open_store();
	for (size_t i = 0; (result == 0) && (i < CONFIG_KFSW_JOURNAL_QUEUE_DEPTH); i++) {
		if (!have_pending) {
			memset(&pending, 0, sizeof(pending));
			if (k_msgq_get(&events, &pending.event, K_NO_WAIT) != 0) {
				break;
			}
			pending.boot = boot;
			timestamp(&pending);
			have_pending = true;
		}
		result = kfsw_journal_store_append(&store, &pending);
		if (result == 0) {
			have_pending = false;
		}
	}
	last_error = result;
	if (result != 0) {
		errors++;
	}
	k_mutex_unlock(&journal_lock);
	return result;
}

static void writer(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sleep(K_MSEC(CONFIG_KFSW_JOURNAL_FLUSH_MS));
		(void)kfsw_journal_flush();
	}
}

K_THREAD_DEFINE(journal_thread, CONFIG_KFSW_JOURNAL_STACK_SIZE, writer, NULL, NULL, NULL, 6, 0,
		SYS_FOREVER_MS);

int kfsw_journal_start(void)
{
	int result;

	k_mutex_lock(&journal_lock, K_FOREVER);
	if (started) {
		result = last_error;
	} else {
		started = true;
		result = open_store();
		last_error = result;
		if (result != 0) {
			errors++;
		}
		k_thread_start(journal_thread);
	}
	k_mutex_unlock(&journal_lock);
	return result;
}

int kfsw_journal_get(uint16_t age, struct kfsw_journal_record *record)
{
	int result;

	if (record == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&journal_lock, K_FOREVER);
	result = kfsw_journal_store_get(&store, age, record);
	k_mutex_unlock(&journal_lock);
	return result;
}

void kfsw_journal_get_stats(struct kfsw_journal_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	k_mutex_lock(&journal_lock, K_FOREVER);
	stats->held = store.held;
	stats->queued = k_msgq_num_used_get(&events) + (have_pending ? 1U : 0U);
	stats->dropped = (uint32_t)atomic_get(&dropped);
	stats->errors = errors;
	stats->corrupt = store.corrupt;
	stats->last_error = last_error;
	stats->ready = store.ready;
	k_mutex_unlock(&journal_lock);
}
