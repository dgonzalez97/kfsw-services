#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/hk.h>
/* Attributes this file's messages to housekeeping, so its level can be
 * raised without also raising the parameter service's. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HK
#include <kfsw/services/log.h>

#include "hk_internal.h"

/* How long the thread sleeps when nothing is due. Long enough not to spin,
 * short enough that a period set from the ground takes effect promptly.
 */
#define KFSW_HK_TICK_MS 200

static bool running;

/*
 * A thread rather than a work item on the system queue.
 *
 * A remote entry blocks on its node for up to a second, and the system
 * workqueue runs at Zephyr's default stack and is shared with health, the
 * watchdog and GPIO debounce. Collection waiting there would delay all three.
 */
static void hk_collector(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* Said once when the clock arrives, and again if it is ever lost, so a
	 * log shows when timed collection actually began rather than when the
	 * thread started.
	 */
	bool announced = false;

	while (true) {
		int64_t now = k_uptime_get();

		/* Nothing is collected on a schedule until the node knows what
		 * time it is. A ring full of samples stamped zero cannot be put
		 * in order, and it would overwrite the ones that can: the
		 * period keeps running while the clock is missing, so a node
		 * that came up without one would discard its own history
		 * before anybody could ask for it.
		 *
		 * `hk collect` still works, and marks what it produces. An
		 * operator debugging a node with no clock wants the values.
		 */
		if (!kfsw_hk_enabled()) {
			k_sleep(K_MSEC(KFSW_HK_TICK_MS));
			continue;
		}

		if (!kfsw_hk_clock_valid()) {
			announced = false;
			k_sleep(K_MSEC(KFSW_HK_TICK_MS));
			continue;
		}
		if (!announced) {
			announced = true;
			kfsw_log_info("HK: clock is set, collecting on schedule");
		}

		for (uint8_t index = 0U; index < CONFIG_KFSW_HK_REPORTS; index++) {
			struct kfsw_hk_report *report = kfsw_hk_report_at(index);
			bool due = false;

			kfsw_hk_lock();
			if ((report != NULL) && report->defined && (report->period_ms != 0U) &&
			    (now >= report->next_uptime_ms)) {
				/* Advanced before collecting, not after, so a
				 * collection that takes longer than the period
				 * does not immediately queue another.
				 */
				report->next_uptime_ms = now + (int64_t)report->period_ms;
				due = true;
			}
			kfsw_hk_unlock();

			if (due) {
				(void)kfsw_hk_collect(index);
			}
		}
		k_sleep(K_MSEC(KFSW_HK_TICK_MS));
	}
}

K_THREAD_DEFINE(kfsw_hk_thread, CONFIG_KFSW_HK_STACK_SIZE, hk_collector, NULL, NULL, NULL,
		CONFIG_KFSW_HK_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_hk_start(void)
{
	if (running) {
		return 0;
	}
	running = true;
	k_thread_start(kfsw_hk_thread);
	kfsw_log_info("HK: collector started");
	return 0;
}
