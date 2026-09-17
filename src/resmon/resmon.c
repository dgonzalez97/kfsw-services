#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/resmon.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_APP
#include <kfsw/services/log.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

/* Low thresholds are allowed: on a target whose threads run on host stacks the
 * figures are small, and a test has to be able to cross one.
 */
#define KFSW_RESMON_PERCENT_MIN 1U

/* The busiest thread of one walk of the thread list. */
struct sweep {
	const struct k_thread *thread;
	uint32_t used_percent;
	uint32_t unused_bytes;
	uint32_t stack_bytes;
	uint16_t threads;
};

static K_MUTEX_DEFINE(resmon_lock);
static struct kfsw_resmon_status resmon_state = {
	.alert_percent = CONFIG_KFSW_RESMON_ALERT_PERCENT,
};
/* True while the last sweep was at the alert percentage, so one crossing is
 * one event however long a node stays there.
 */
static bool alerting;

static void resmon_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(resmon_work, resmon_work_handler);

static void visit_thread(const struct k_thread *thread, void *user_data)
{
	struct sweep *sweep = user_data;
	size_t stack_bytes = thread->stack_info.size;
	size_t unused = 0U;
	uint32_t used_percent;

	/* A thread whose stack cannot be measured is skipped, not counted. */
	if ((stack_bytes == 0U) || (k_thread_stack_space_get(thread, &unused) != 0)) {
		return;
	}

	used_percent = (uint32_t)(((stack_bytes - MIN(unused, stack_bytes)) * 100U) / stack_bytes);

	if ((sweep->threads == 0U) || (used_percent > sweep->used_percent)) {
		sweep->used_percent = used_percent;
		sweep->unused_bytes = (uint32_t)MIN(unused, (size_t)UINT32_MAX);
		sweep->stack_bytes = (uint32_t)MIN(stack_bytes, (size_t)UINT32_MAX);
		sweep->thread = thread;
	}
	sweep->threads++;
}

static void copy_thread_name(char *destination, const struct k_thread *thread)
{
	const char *name = NULL;

	if (thread != NULL) {
		name = k_thread_name_get((k_tid_t)thread);
	}
	if ((name == NULL) || (name[0] == '\0')) {
		name = "unnamed";
	}

	(void)strncpy(destination, name, KFSW_RESMON_NAME_SIZE - 1U);
	destination[KFSW_RESMON_NAME_SIZE - 1U] = '\0';
}

int kfsw_resmon_sample(void)
{
	struct sweep sweep = {0};
	char name[KFSW_RESMON_NAME_SIZE];
	bool crossed = false;

	k_thread_foreach_unlocked(visit_thread, &sweep);

	if (sweep.threads == 0U) {
		return -ENODATA;
	}

	copy_thread_name(name, sweep.thread);

	k_mutex_lock(&resmon_lock, K_FOREVER);
	if (resmon_state.sweeps < UINT32_MAX) {
		resmon_state.sweeps++;
	}
	resmon_state.threads = sweep.threads;
	resmon_state.last_used_percent = sweep.used_percent;

	if (sweep.used_percent >= resmon_state.worst_used_percent) {
		resmon_state.worst_used_percent = sweep.used_percent;
		resmon_state.worst_unused_bytes = sweep.unused_bytes;
		resmon_state.worst_stack_bytes = sweep.stack_bytes;
		(void)strncpy(resmon_state.worst_thread, name, KFSW_RESMON_NAME_SIZE - 1U);
		resmon_state.worst_thread[KFSW_RESMON_NAME_SIZE - 1U] = '\0';
	}

	if (sweep.used_percent >= resmon_state.alert_percent) {
		if (!alerting) {
			alerting = true;
			crossed = true;
			if (resmon_state.alerts < UINT32_MAX) {
				resmon_state.alerts++;
			}
		}
	} else {
		alerting = false;
	}
	k_mutex_unlock(&resmon_lock);

	if (crossed) {
		kfsw_log_warning("Resource monitor: %s is at %u%% of its stack, %u bytes left",
				 name, sweep.used_percent, sweep.unused_bytes);
#if CONFIG_KFSW_EVENT
		{
			uint8_t payload[5];

			payload[0] = (uint8_t)MIN(sweep.used_percent, 100U);
			sys_put_be32(sweep.unused_bytes, &payload[1]);
			kfsw_event_emit(KFSW_EVENT_SOURCE_RESMON, KFSW_EVENT_RESMON_LOW_STACK,
					KFSW_EVENT_WARNING, payload, sizeof(payload));
		}
#endif
	}

	return (int)sweep.threads;
}

static void resmon_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)kfsw_resmon_sample();

	k_mutex_lock(&resmon_lock, K_FOREVER);
	if (resmon_state.running) {
		(void)k_work_reschedule(&resmon_work, K_MSEC(CONFIG_KFSW_RESMON_PERIOD_MS));
	}
	k_mutex_unlock(&resmon_lock);
}

int kfsw_resmon_start(void)
{
	k_mutex_lock(&resmon_lock, K_FOREVER);
	if (resmon_state.running) {
		k_mutex_unlock(&resmon_lock);
		return -EALREADY;
	}
	resmon_state.running = true;
	k_mutex_unlock(&resmon_lock);

	/* Sweep once now, so the numbers are readable without waiting a period. */
	(void)kfsw_resmon_sample();
	(void)k_work_reschedule(&resmon_work, K_MSEC(CONFIG_KFSW_RESMON_PERIOD_MS));
	kfsw_log_info("Resource monitor started, every %u ms, alert at %u%%",
		      CONFIG_KFSW_RESMON_PERIOD_MS, resmon_state.alert_percent);
	return 0;
}

int kfsw_resmon_stop(void)
{
	k_mutex_lock(&resmon_lock, K_FOREVER);
	if (!resmon_state.running) {
		k_mutex_unlock(&resmon_lock);
		return -EALREADY;
	}
	resmon_state.running = false;
	k_mutex_unlock(&resmon_lock);

	(void)k_work_cancel_delayable(&resmon_work);
	return 0;
}

void kfsw_resmon_get_status(struct kfsw_resmon_status *status)
{
	if (status == NULL) {
		return;
	}

	k_mutex_lock(&resmon_lock, K_FOREVER);
	*status = resmon_state;
	k_mutex_unlock(&resmon_lock);
}

int kfsw_resmon_set_alert_percent(uint32_t percent)
{
	if ((percent < KFSW_RESMON_PERCENT_MIN) || (percent > 100U)) {
		return -ERANGE;
	}

	k_mutex_lock(&resmon_lock, K_FOREVER);
	resmon_state.alert_percent = percent;
	/* A new threshold decides its own crossing on the next sweep. */
	alerting = false;
	k_mutex_unlock(&resmon_lock);
	return 0;
}
