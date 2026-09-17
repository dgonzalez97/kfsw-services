#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#if CONFIG_REBOOT
#include <zephyr/sys/reboot.h>
#endif

#include <kfsw/platform/time.h>
#include <kfsw/services/gndwdt.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HEALTH
#include <kfsw/services/log.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

/* Contact is recorded from the router thread and read by the work handler, so
 * the timestamp is atomic and no lock is taken on the receive path.
 */
static atomic_t last_contact_ms;
static atomic_t contacts;
static atomic_t last_node;

static K_MUTEX_DEFINE(gndwdt_lock);
static uint32_t timeout_s = CONFIG_KFSW_GNDWDT_TIMEOUT_S;
static uint32_t expiries;
static bool enabled = IS_ENABLED(CONFIG_KFSW_GNDWDT_ENABLED_AT_START);
static bool running;
static bool resetting;

static void gndwdt_work_handler(struct k_work *work);
static void gndwdt_reset_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(gndwdt_work, gndwdt_work_handler);
K_WORK_DELAYABLE_DEFINE(gndwdt_reset_work, gndwdt_reset_handler);

static void restart_countdown(void)
{
	atomic_set(&last_contact_ms, (atomic_val_t)kfsw_time_monotonic_ms());
}

int kfsw_gndwdt_start(void)
{
	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	if (running) {
		k_mutex_unlock(&gndwdt_lock);
		return -EALREADY;
	}
	running = true;
	restart_countdown();
	k_mutex_unlock(&gndwdt_lock);

	(void)k_work_reschedule(&gndwdt_work, K_MSEC(CONFIG_KFSW_GNDWDT_CHECK_MS));
	kfsw_log_info("Ground watchdog started, timeout %u s, %s", timeout_s,
		      enabled ? "armed" : "disarmed");
	return 0;
}

int kfsw_gndwdt_stop(void)
{
	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	if (!running) {
		k_mutex_unlock(&gndwdt_lock);
		return -EALREADY;
	}
	running = false;
	k_mutex_unlock(&gndwdt_lock);

	(void)k_work_cancel_delayable(&gndwdt_work);
	kfsw_log_info("Ground watchdog stopped");
	return 0;
}

void kfsw_gndwdt_contact(uint16_t node)
{
	/* The caller decides what counts as contact; this node's own address is
	 * filtered where the address is known.
	 */
	if (!running) {
		return;
	}

	restart_countdown();
	atomic_set(&last_node, (atomic_val_t)node);
	if (atomic_get(&contacts) < (atomic_val_t)UINT32_MAX) {
		atomic_inc(&contacts);
	}
}

int kfsw_gndwdt_evaluate(void)
{
	uint32_t elapsed_ms;
	uint32_t allowed_s;

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	if (!running || !enabled) {
		k_mutex_unlock(&gndwdt_lock);
		return 0;
	}
	allowed_s = timeout_s;
	elapsed_ms = (uint32_t)(kfsw_time_monotonic_ms() - (uint64_t)atomic_get(&last_contact_ms));

	if ((elapsed_ms / MSEC_PER_SEC) < allowed_s) {
		k_mutex_unlock(&gndwdt_lock);
		return 0;
	}

	if (expiries < UINT32_MAX) {
		expiries++;
	}
	/* The countdown restarts so a node that cannot reset keeps reporting
	 * whole timeouts rather than one growing number.
	 */
	restart_countdown();
	k_mutex_unlock(&gndwdt_lock);

	kfsw_log_error("Ground watchdog: no contact for %u s; resetting", allowed_s);
#if CONFIG_KFSW_EVENT
	{
		uint8_t payload[4];

		sys_put_be32(allowed_s, payload);
		kfsw_event_emit(KFSW_EVENT_SOURCE_GNDWDT, KFSW_EVENT_GNDWDT_EXPIRED,
				KFSW_EVENT_CRITICAL, payload, sizeof(payload));
	}
#endif
	return -ETIMEDOUT;
}

void kfsw_gndwdt_get_status(struct kfsw_gndwdt_status *status)
{
	uint32_t elapsed_ms;

	if (status == NULL) {
		return;
	}

	elapsed_ms = (uint32_t)(kfsw_time_monotonic_ms() - (uint64_t)atomic_get(&last_contact_ms));

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	status->timeout_s = timeout_s;
	status->expiries = expiries;
	status->enabled = enabled;
	status->running = running;
	k_mutex_unlock(&gndwdt_lock);

	status->since_contact_s = elapsed_ms / MSEC_PER_SEC;
	status->contacts = (uint32_t)atomic_get(&contacts);
	status->last_node = (uint16_t)atomic_get(&last_node);
}

int kfsw_gndwdt_set_timeout_s(uint32_t value)
{
	if ((value < CONFIG_KFSW_GNDWDT_TIMEOUT_MIN_S) || (value > CONFIG_KFSW_GNDWDT_TIMEOUT_MAX_S)) {
		return -ERANGE;
	}

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	timeout_s = value;
	restart_countdown();
	k_mutex_unlock(&gndwdt_lock);
	return 0;
}

void kfsw_gndwdt_set_enabled(bool value)
{
	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	enabled = value;
	if (value) {
		restart_countdown();
	}
	k_mutex_unlock(&gndwdt_lock);
}

static void gndwdt_reset_handler(struct k_work *work)
{
	ARG_UNUSED(work);

#if CONFIG_REBOOT
	sys_reboot(SYS_REBOOT_COLD);
#else
	kfsw_log_error("Ground watchdog: this build cannot reset the node");
#endif
}

static void gndwdt_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (kfsw_gndwdt_evaluate() == -ETIMEDOUT) {
		k_mutex_lock(&gndwdt_lock, K_FOREVER);
		if (!resetting) {
			resetting = true;
			k_mutex_unlock(&gndwdt_lock);
			/* Delayed so the log line and the event record leave first. */
			(void)k_work_reschedule(&gndwdt_reset_work,
						K_MSEC(CONFIG_KFSW_GNDWDT_RESET_DELAY_MS));
		} else {
			k_mutex_unlock(&gndwdt_lock);
		}
	}

	if (running) {
		(void)k_work_reschedule(&gndwdt_work, K_MSEC(CONFIG_KFSW_GNDWDT_CHECK_MS));
	}
}
