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

void kfsw_boot_service_start(void)
{
	uint32_t reset_cause = 0U;
	int reset_rc;

	reset_rc = kfsw_platform_get_reset_cause(&reset_cause);

	if (reset_rc != 0) {
		kfsw_log_warning("Reset cause unavailable: %d", reset_rc);
	}

	/* These markers are consumed by CI and HIL tests. */
	printk("@BOOT sw=kfsw-dev board=%s reset=0x%08x reset_rc=%d\n", CONFIG_BOARD_TARGET,
	       (unsigned int)reset_cause, reset_rc);

	kfsw_log_info("Boot checks complete at %llu us",
		      (unsigned long long)kfsw_time_monotonic_us());

#if CONFIG_KFSW_EVENT
	/* The reset cause is the first thing an operator wants after an
	 * unattended restart, so it is recorded rather than only printed. */
	{
		uint8_t payload[5];

		sys_put_be32(reset_cause, &payload[0]);
		payload[4] = (reset_rc == 0) ? 0U : 1U;
		kfsw_event_emit(KFSW_EVENT_SOURCE_BOOT, KFSW_EVENT_BOOT_READY,
				(reset_rc == 0) ? KFSW_EVENT_INFO : KFSW_EVENT_WARNING, payload,
				sizeof(payload));
	}
#endif

	printk("@READY uptime_ms=%lld\n", (long long)kfsw_time_monotonic_ms());
}
