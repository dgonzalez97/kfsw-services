#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <kfsw/platform/reset.h>
#include <kfsw/services/boot.h>

void kfsw_boot_service_start(void)
{
    uint32_t reset_cause = 0U;
    int reset_rc;

    reset_rc = kfsw_platform_get_reset_cause(&reset_cause);

    /* These markers are consumed by CI and HIL tests. */
    printk("@BOOT sw=kfsw-dev board=%s reset=0x%08x reset_rc=%d\n",
           CONFIG_BOARD_TARGET,
           (unsigned int)reset_cause,
           reset_rc);

    printk("@READY uptime_ms=%lld\n",
           (long long)k_uptime_get());
}
