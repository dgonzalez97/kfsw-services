#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include <kfsw/platform/reset.h>
#include <kfsw/platform/time.h>
#include <kfsw/services/boot.h>
#include <kfsw/services/log.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

/*
 * Latched once, because reading the cause clears it. The platform call wipes
 * the hardware flags so the next boot reports a new event, and this service is
 * the first reader; anything that called it again would be told the register is
 * empty and report that as the cause of the reset.
 */
static uint32_t boot_reset_cause;
static int boot_reset_rc = -EAGAIN;

uint32_t kfsw_boot_get_reset_cause(void)
{
	return boot_reset_cause;
}

int kfsw_boot_get_reset_result(void)
{
	return boot_reset_rc;
}

const char *kfsw_boot_get_image_version(void)
{
	return KFSW_IMAGE_VERSION;
}

void kfsw_boot_service_start(void)
{
	uint32_t reset_cause = 0U;
	int reset_rc;

	reset_rc = kfsw_platform_get_reset_cause(&reset_cause);
	boot_reset_cause = reset_cause;
	boot_reset_rc = reset_rc;

	if (reset_rc != 0) {
		kfsw_log_warning("Reset cause unavailable: %d", reset_rc);
	}

	/* These markers are consumed by CI and HIL tests. The decoded name is
	 * appended rather than replacing the raw mask: the mask can latch
	 * several causes at once and the name reports only the first.
	 */
	printk("@BOOT sw=%s board=%s reset=0x%08x reset_rc=%d reset_cause=%s\n", KFSW_IMAGE_VERSION,
	       CONFIG_BOARD_TARGET, (unsigned int)reset_cause, reset_rc,
	       kfsw_platform_reset_cause_name(reset_cause));

	if (kfsw_platform_reset_cause_is_watchdog(reset_cause)) {
		kfsw_log_warning("Previous run was ended by the watchdog");
	}

	kfsw_log_info("Boot checks complete at %llu us",
		      (unsigned long long)kfsw_time_monotonic_us());

#if CONFIG_KFSW_EVENT
	/* The reset cause is the first thing an operator wants after an
	 * unattended restart, so it is recorded rather than only printed. */
	{
		uint8_t payload[5];

		sys_put_be32(reset_cause, &payload[0]);
		payload[4] = (reset_rc == 0) ? 0U : 1U;
		/* A watchdog reset is not routine. It is raised above INFO so
		 * that it survives a severity filter on the way to ground.
		 */
		kfsw_event_emit(
			KFSW_EVENT_SOURCE_BOOT, KFSW_EVENT_BOOT_READY,
			((reset_rc != 0) || kfsw_platform_reset_cause_is_watchdog(reset_cause))
				? KFSW_EVENT_WARNING
				: KFSW_EVENT_INFO,
			payload, sizeof(payload));
	}
#endif

	printk("@READY uptime_ms=%lld\n", (long long)kfsw_time_monotonic_ms());
}
