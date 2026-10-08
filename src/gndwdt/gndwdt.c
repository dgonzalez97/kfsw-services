#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#if CONFIG_KFSW_COMMAND_CSP
#include <csp/csp_iflist.h>
#endif

#include <kfsw/platform/time.h>
#include <kfsw/services/gndwdt.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_HEALTH
#include <kfsw/services/log.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

/* A composition that cannot be fed, or cannot reset, would arm a timer that
 * runs out once and then does nothing useful for the rest of the mission.
 * Tests call the handler directly and supply the source themselves.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_KFSW_COMMAND_CSP) || IS_ENABLED(CONFIG_ZTEST),
	     "KFSW_GNDWDT needs KFSW_COMMAND_CSP: only a feed over CSP counts");
BUILD_ASSERT(IS_ENABLED(CONFIG_REBOOT), "KFSW_GNDWDT needs REBOOT to act on an expiry");

/* The command worker and timeout worker share this state. */
static uint64_t last_contact_ms;
static uint32_t contacts;
static uint16_t last_node;

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
	last_contact_ms = kfsw_time_monotonic_ms();
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
	/* Taking the service out of service is the one thing that calls off a
	 * reset already decided. A late feed does not.
	 */
	resetting = false;
	k_mutex_unlock(&gndwdt_lock);

	(void)k_work_cancel_delayable(&gndwdt_reset_work);
	(void)k_work_cancel_delayable(&gndwdt_work);
	kfsw_log_info("Ground watchdog stopped");
	return 0;
}

static void ground_wtd_reply(struct kfsw_command_result *result)
{
	struct kfsw_gndwdt_status status;

	/* Parameter samples use this same snapshot. */
	kfsw_gndwdt_get_status(&status);
	result->status = KFSW_COMMAND_OK;
	(void)snprintf(result->detail, sizeof(result->detail),
		       "ground_wtd_cnt=%u ground_wtd_timeout=%u", status.remaining_s,
		       status.timeout_s);
}

static int ground_wtd(const struct kfsw_command_arg *args, size_t arg_count,
		      const struct kfsw_command_source *source, struct kfsw_command_result *result)
{
	ARG_UNUSED(arg_count);

	if (strcmp(args[0].value.text, "get") == 0) {
		ground_wtd_reply(result);
		return 0;
	}

	if (!source->via_csp || strcmp(args[0].value.text, KFSW_GNDWDT_FEED_WORD) != 0) {
		result->status = KFSW_COMMAND_DENIED;
		return -EACCES;
	}

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	if (!running || resetting) {
		k_mutex_unlock(&gndwdt_lock);
		result->status = KFSW_COMMAND_UNAVAILABLE;
		return -EAGAIN;
	}
	restart_countdown();
	last_node = source->node;
	if (contacts < UINT32_MAX) {
		contacts++;
	}
	k_mutex_unlock(&gndwdt_lock);
	ground_wtd_reply(result);
	return 0;
}

static const enum kfsw_command_type ground_wtd_args[] = {KFSW_COMMAND_TYPE_TEXT};
static const struct kfsw_command_definition ground_wtd_commands[] = {
	{
		.id = KFSW_COMMAND_ID_GROUND_WTD,
		.name = "ground_wtd",
		.help = "Ground watchdog: get, or KFSWWSFK to feed over CSP.",
		.flags = KFSW_COMMAND_FLAG_MUTATING,
		.arg_count = 1U,
		.arg_types = ground_wtd_args,
		.handler = ground_wtd,
	},
};

const struct kfsw_command_definition_set kfsw_gndwdt_command_definitions = {
	.commands = ground_wtd_commands,
	.count = ARRAY_SIZE(ground_wtd_commands),
};

#if CONFIG_KFSW_COMMAND_CSP
int kfsw_gndwdt_remote(uint16_t node, bool feed, struct kfsw_command_result *result)
{
	struct kfsw_command_arg arg = {
		.type = KFSW_COMMAND_TYPE_TEXT,
		.value.text = feed ? KFSW_GNDWDT_FEED_WORD : "get",
	};

	if ((node == 0U) || (result == NULL) || (csp_iflist_get_by_addr(node) != NULL)) {
		return -EINVAL;
	}
	return kfsw_command_invoke_remote(node, "ground_wtd", &arg, 1U, result);
}
#endif

int kfsw_gndwdt_evaluate(void)
{
	uint64_t elapsed_ms;
	uint32_t allowed_s;

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	/* A decided reset is reported once, not again on every later check. */
	if (!running || !enabled || resetting) {
		k_mutex_unlock(&gndwdt_lock);
		return 0;
	}
	allowed_s = timeout_s;
	elapsed_ms = kfsw_time_monotonic_ms() - last_contact_ms;

	if ((elapsed_ms / MSEC_PER_SEC) < allowed_s) {
		k_mutex_unlock(&gndwdt_lock);
		return 0;
	}

	if (expiries < UINT32_MAX) {
		expiries++;
	}
	/* So a disarm and re-arm starts from now rather than from the expiry. */
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
	uint64_t elapsed_ms;

	if (status == NULL) {
		return;
	}

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	elapsed_ms = kfsw_time_monotonic_ms() - last_contact_ms;
	status->timeout_s = timeout_s;
	status->expiries = expiries;
	status->enabled = enabled;
	status->running = running;
	status->since_contact_s = MIN(elapsed_ms / MSEC_PER_SEC, UINT32_MAX);
	status->remaining_s = (!running || resetting || status->since_contact_s >= timeout_s)
				      ? 0U
				      : timeout_s - status->since_contact_s;
	status->contacts = contacts;
	status->last_node = last_node;
	k_mutex_unlock(&gndwdt_lock);
}

int kfsw_gndwdt_set_timeout_s(uint32_t value)
{
	if ((value < CONFIG_KFSW_GNDWDT_TIMEOUT_MIN_S) ||
	    (value > CONFIG_KFSW_GNDWDT_TIMEOUT_MAX_S)) {
		return -ERANGE;
	}

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	timeout_s = value;
	k_mutex_unlock(&gndwdt_lock);
	return 0;
}

void kfsw_gndwdt_set_enabled(bool value)
{
	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	enabled = value;
	k_mutex_unlock(&gndwdt_lock);
}

static void gndwdt_reset_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	sys_reboot(SYS_REBOOT_COLD);
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

	k_mutex_lock(&gndwdt_lock, K_FOREVER);
	if (running) {
		(void)k_work_reschedule(&gndwdt_work, K_MSEC(CONFIG_KFSW_GNDWDT_CHECK_MS));
	}
	k_mutex_unlock(&gndwdt_lock);
}
