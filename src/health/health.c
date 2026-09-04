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
};

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
		if ((now - entry->last_report_ms) > entry->deadline_ms) {
			overdue = entry;
			break;
		}
	}

	if (overdue == NULL) {
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
	(void)k_work_reschedule(&health_work, K_MSEC(CONFIG_KFSW_HEALTH_INTERVAL_MS));
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

	(void)k_work_reschedule(&health_work, K_MSEC(CONFIG_KFSW_HEALTH_INTERVAL_MS));

	kfsw_log_info("Health supervising %u component(s) every %d ms", health_state.count,
		      CONFIG_KFSW_HEALTH_INTERVAL_MS);
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
