#include <errno.h>
#include <zephyr/kernel.h>
#include <kfsw/services/hk.h>
#include "hk_internal.h"

static K_MUTEX_DEFINE(start_lock);
static K_SEM_DEFINE(schedule_wake, 0, 1);
static bool running;

void kfsw_hk_wake(void)
{
	k_sem_give(&schedule_wake);
}

static void hk_collector(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		for (uint8_t index = 0; index < CONFIG_KFSW_HK_REPORTS; index++) {
			struct kfsw_hk_due due;

			if (!kfsw_hk_enabled() || !kfsw_hk_clock_valid()) {
				break;
			}
			if (kfsw_hk_schedule_take(index, k_uptime_get(), &due)) {
				(void)kfsw_hk_collect(index);
				kfsw_hk_schedule_finish(index, &due, k_uptime_get());
			}
#if CONFIG_KFSW_HK_BEACON
			if (kfsw_hk_enabled() && kfsw_hk_clock_valid()) {
				kfsw_hk_beacon_tick(index, k_uptime_get());
			}
#endif
		}
		/* Clock changes have no wake hook yet. Poll at most every 200 ms. */
		int64_t wait = (kfsw_hk_enabled() && kfsw_hk_clock_valid())
				       ? kfsw_hk_schedule_wait(k_uptime_get())
				       : 200;

		(void)k_sem_take(&schedule_wake, K_MSEC(wait));
	}
}

K_THREAD_DEFINE(kfsw_hk_thread, CONFIG_KFSW_HK_STACK_SIZE, hk_collector, NULL, NULL, NULL,
		CONFIG_KFSW_HK_PRIORITY, 0, SYS_FOREVER_MS);

int kfsw_hk_start(void)
{
	if (!kfsw_hk_is_ready()) {
		return -EACCES;
	}
	k_mutex_lock(&start_lock, K_FOREVER);
	if (!running) {
		running = true;
		k_thread_start(kfsw_hk_thread);
	}
	k_mutex_unlock(&start_lock);
	return 0;
}
