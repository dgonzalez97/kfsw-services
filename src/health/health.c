#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/time.h>
#include <kfsw/platform/watchdog.h>
#include <kfsw/services/health.h>
/* Attributes this file's messages, so its level can be raised alone. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HEALTH
#include <kfsw/services/log.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

struct health_entry {
	char name[KFSW_HEALTH_NAME_SIZE];
	uint32_t deadline_ms;
	uint64_t last_report_ms;
	uint32_t reports;
	bool used;
	/* True once this component has been warned about, so the warning is
	 * issued on crossing rather than on every check. */
	bool warned;
};

/* Live rather than compiled in, because the right check interval depends on
 * what the watchdog was armed with, and that is a runtime fact.
 */
static uint32_t health_interval_ms = CONFIG_KFSW_HEALTH_INTERVAL_MS;

static K_MUTEX_DEFINE(health_lock);
static struct health_entry health_entries[KFSW_HEALTH_MAX_COMPONENTS];
static struct kfsw_health_status health_state = {
	.state = KFSW_HEALTH_STOPPED,
};

static void health_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(health_work, health_work_handler);

/* Caller holds health_lock. */
static struct health_entry *find_by_name(const char *name)
{
	for (uint8_t index = 0U; index < KFSW_HEALTH_MAX_COMPONENTS; index++) {
		if (health_entries[index].used &&
		    (strncmp(health_entries[index].name, name, KFSW_HEALTH_NAME_SIZE) == 0)) {
			return &health_entries[index];
		}
	}
	return NULL;
}

int kfsw_health_register(const char *name, uint32_t deadline_ms, uint8_t *handle)
{
	if ((name == NULL) || (handle == NULL) || (name[0] == '\0') || (deadline_ms == 0U)) {
		return -EINVAL;
	}

	k_mutex_lock(&health_lock, K_FOREVER);

	if (find_by_name(name) != NULL) {
		k_mutex_unlock(&health_lock);
		return -EEXIST;
	}

	for (uint8_t index = 0U; index < KFSW_HEALTH_MAX_COMPONENTS; index++) {
		struct health_entry *entry = &health_entries[index];

		if (entry->used) {
			continue;
		}

		(void)strncpy(entry->name, name, KFSW_HEALTH_NAME_SIZE - 1U);
		entry->name[KFSW_HEALTH_NAME_SIZE - 1U] = '\0';
		entry->deadline_ms = deadline_ms;
		entry->last_report_ms = kfsw_time_monotonic_ms();
		entry->reports = 0U;
		entry->used = true;
		entry->warned = false;

		health_state.count++;
		*handle = index;

		k_mutex_unlock(&health_lock);
		kfsw_log_info("Health watching %s, deadline %u ms", entry->name, deadline_ms);
		return 0;
	}

	k_mutex_unlock(&health_lock);
	return -ENOSPC;
}

int kfsw_health_unregister(uint8_t handle)
{
	if (handle >= KFSW_HEALTH_MAX_COMPONENTS) {
		return -EINVAL;
	}

	k_mutex_lock(&health_lock, K_FOREVER);

	if (!health_entries[handle].used) {
		k_mutex_unlock(&health_lock);
		return -EINVAL;
	}

	/* If this component was the one holding the system faulted, the fault
	 * goes with it: what was overdue is no longer expected to report.
	 */
	if (strncmp(health_state.faulted_by, health_entries[handle].name, KFSW_HEALTH_NAME_SIZE) ==
	    0) {
		health_state.faulted_by[0] = '\0';
	}

	health_entries[handle].used = false;
	health_entries[handle].name[0] = '\0';
	health_state.count--;

	k_mutex_unlock(&health_lock);
	kfsw_log_info("Health stopped watching handle %u", handle);
	return 0;
}

int kfsw_health_report(uint8_t handle)
{
	if (handle >= KFSW_HEALTH_MAX_COMPONENTS) {
		return -EINVAL;
	}

	k_mutex_lock(&health_lock, K_FOREVER);

	if (!health_entries[handle].used) {
		k_mutex_unlock(&health_lock);
		return -EINVAL;
	}

	health_entries[handle].last_report_ms = kfsw_time_monotonic_ms();
	if (health_entries[handle].reports < UINT32_MAX) {
		health_entries[handle].reports++;
	}

	k_mutex_unlock(&health_lock);
	return 0;
}

int kfsw_health_evaluate(void)
{
	struct health_entry *overdue = NULL;
	uint64_t now;
	uint32_t elapsed;
	bool was_ok;

	k_mutex_lock(&health_lock, K_FOREVER);

	if (health_state.state == KFSW_HEALTH_STOPPED) {
		k_mutex_unlock(&health_lock);
		return -EINVAL;
	}

	now = kfsw_time_monotonic_ms();
	was_ok = (health_state.state == KFSW_HEALTH_OK);

	for (uint8_t index = 0U; index < KFSW_HEALTH_MAX_COMPONENTS; index++) {
		struct health_entry *entry = &health_entries[index];

		if (!entry->used) {
			continue;
		}
		elapsed = (uint32_t)(now - entry->last_report_ms);
		if (elapsed > entry->deadline_ms) {
			overdue = entry;
			break;
		}

		/* A component well into its deadline is not yet a fault, but it
		 * is the only warning anyone gets before one. Said once per
		 * crossing rather than on every check, so a component that sits
		 * near its limit does not fill the log.
		 */
		if ((elapsed > ((entry->deadline_ms * 3U) / 4U)) && !entry->warned) {
			entry->warned = true;
			kfsw_log_warning("Health: %s is %u ms into a %u ms deadline", entry->name,
					 elapsed, entry->deadline_ms);
		} else if (elapsed <= (entry->deadline_ms / 2U)) {
			entry->warned = false;
		}
	}

	if (overdue == NULL) {
		if (!was_ok) {
			kfsw_log_info("Health: every component is reporting again");
		}
		health_state.state = KFSW_HEALTH_OK;
		health_state.faulted_by[0] = '\0';
		k_mutex_unlock(&health_lock);

		if (kfsw_platform_watchdog_feed() == 0) {
			k_mutex_lock(&health_lock, K_FOREVER);
			if (health_state.feeds < UINT32_MAX) {
				health_state.feeds++;
			}
			health_state.feeding = true;
			k_mutex_unlock(&health_lock);
		}
		return 0;
	}

	health_state.state = KFSW_HEALTH_FAULTED;
	health_state.feeding = false;
	(void)strncpy(health_state.faulted_by, overdue->name, KFSW_HEALTH_NAME_SIZE - 1U);
	health_state.faulted_by[KFSW_HEALTH_NAME_SIZE - 1U] = '\0';
	if (was_ok) {
		health_state.faults++;
	}
	k_mutex_unlock(&health_lock);

	/* Said once per fault rather than once per check: a reset takes a whole
	 * timeout to arrive, and repeating the same line until it does buries
	 * the reason under the noise.
	 */
	if (was_ok) {
		kfsw_log_error("Health: %s is overdue; the watchdog will no longer be fed",
			       overdue->name);
#if CONFIG_KFSW_EVENT
		kfsw_event_emit(KFSW_EVENT_SOURCE_APP, KFSW_EVENT_HEALTH_FAULT, KFSW_EVENT_CRITICAL,
				(const uint8_t *)overdue->name,
				(uint8_t)strnlen(overdue->name, KFSW_HEALTH_NAME_SIZE - 1U));
#endif
	}

	/* The feed is withheld rather than the watchdog being starved outright.
	 * A component that recovers before the timeout expires is allowed to
	 * keep the system alive; only a fault that persists causes the reset.
	 */
	return -ETIMEDOUT;
}

static void health_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)kfsw_health_evaluate();
	/* Read every cycle so a change takes effect on the next one. */
	(void)k_work_reschedule(&health_work, K_MSEC(health_interval_ms));
}

int kfsw_health_start(void)
{
	uint64_t now;
	int result;

	k_mutex_lock(&health_lock, K_FOREVER);
	if (health_state.state != KFSW_HEALTH_STOPPED) {
		k_mutex_unlock(&health_lock);
		return -EALREADY;
	}
	k_mutex_unlock(&health_lock);

	/* Taking the watchdog over is what makes any of this consequential. If
	 * it cannot be taken over there is nothing to withhold, and pretending
	 * to supervise would be worse than not starting.
	 */
	result = kfsw_platform_watchdog_release();
	if (result != 0) {
		kfsw_log_error("Health could not take over the watchdog: %d", result);
		return result;
	}

	k_mutex_lock(&health_lock, K_FOREVER);
	now = kfsw_time_monotonic_ms();
	for (uint8_t index = 0U; index < KFSW_HEALTH_MAX_COMPONENTS; index++) {
		if (health_entries[index].used) {
			/* Everything counts as having just reported, so a slow
			 * start is not mistaken for a fault. */
			health_entries[index].last_report_ms = now;
		}
	}
	health_state.state = KFSW_HEALTH_OK;
	k_mutex_unlock(&health_lock);

	(void)k_work_reschedule(&health_work, K_MSEC(health_interval_ms));

	kfsw_log_info("Health supervising %u component(s) every %u ms", health_state.count,
		      health_interval_ms);
	kfsw_log_debug("Health took the watchdog over from the platform keep-alive");
	return 0;
}

int kfsw_health_get_status(struct kfsw_health_status *status)
{
	if (status == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&health_lock, K_FOREVER);
	*status = health_state;
	k_mutex_unlock(&health_lock);

	return 0;
}

int kfsw_health_get_component(uint8_t index, struct kfsw_health_component *component)
{
	uint64_t now;

	if (component == NULL) {
		return -EINVAL;
	}
	if (index >= KFSW_HEALTH_MAX_COMPONENTS) {
		return -ENOENT;
	}

	k_mutex_lock(&health_lock, K_FOREVER);

	if (!health_entries[index].used) {
		k_mutex_unlock(&health_lock);
		return -ENOENT;
	}

	now = kfsw_time_monotonic_ms();
	(void)strncpy(component->name, health_entries[index].name, KFSW_HEALTH_NAME_SIZE);
	component->name[KFSW_HEALTH_NAME_SIZE - 1U] = '\0';
	component->deadline_ms = health_entries[index].deadline_ms;
	component->since_report_ms = (uint32_t)(now - health_entries[index].last_report_ms);
	component->reports = health_entries[index].reports;
	component->overdue = component->since_report_ms > component->deadline_ms;

	k_mutex_unlock(&health_lock);
	return 0;
}

uint32_t kfsw_health_get_interval_ms(void)
{
	return health_interval_ms;
}

int kfsw_health_check_interval_ms(uint32_t interval_ms)
{
	struct kfsw_platform_watchdog_info watchdog;
	uint32_t feed_interval;

	if (interval_ms == 0U) {
		return -EINVAL;
	}

	/* Checked against the watchdog the system is actually running with,
	 * not a compiled constant. The watchdog is fed only by a check that
	 * finds every component healthy, so a check slower than the feed
	 * interval resets a board where nothing is wrong. This is the one value
	 * here that can do that by being set to a number that looks perfectly
	 * reasonable.
	 *
	 * Separate from applying it because a change callback cannot refuse:
	 * by the time one runs the value is already stored, and rolling back
	 * afterwards still reports success for a value that was rejected.
	 */
	if (kfsw_platform_watchdog_get_info(&watchdog) != 0) {
		return 0;
	}
	if (watchdog.timeout_ms == 0U) {
		/* No watchdog is armed, so there is nothing for a slow check to
		 * outlast. Refusing here would refuse on every target that has
		 * no watchdog hardware, which is most of the test matrix. */
		return 0;
	}

	feed_interval = kfsw_platform_watchdog_feed_interval_ms(watchdog.timeout_ms);
	if (interval_ms > feed_interval) {
		kfsw_log_warning("Health: a %u ms check is slower than the %u ms feed interval",
				 interval_ms, feed_interval);
		return -ERANGE;
	}
	return 0;
}

int kfsw_health_set_interval_ms(uint32_t interval_ms)
{
	int result = kfsw_health_check_interval_ms(interval_ms);

	if (result != 0) {
		return result;
	}

	k_mutex_lock(&health_lock, K_FOREVER);
	health_interval_ms = interval_ms;
	k_mutex_unlock(&health_lock);

	kfsw_log_info("Health: checking every %u ms", interval_ms);
	return 0;
}

const char *kfsw_health_state_name(enum kfsw_health_state state)
{
	switch (state) {
	case KFSW_HEALTH_OK:
		return "ok";
	case KFSW_HEALTH_FAULTED:
		return "faulted";
	case KFSW_HEALTH_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}
