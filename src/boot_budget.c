#include <errno.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include <kfsw/platform/boot_store.h>
#include <kfsw/services/boot.h>
#include <kfsw/services/boot_budget.h>
#if CONFIG_KFSW_GOLDEN
#include <kfsw/services/golden.h>
#endif
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_BOOT
#include <kfsw/services/log.h>

BUILD_ASSERT(CONFIG_KFSW_BOOT_ATTEMPTS_MAX == 3 || CONFIG_KFSW_BOOT_ATTEMPTS_MAX == 5 ||
		     CONFIG_KFSW_BOOT_ATTEMPTS_MAX == 10,
	     "boot attempt limit must be 3, 5 or 10");

static uint8_t attempts_left = UINT8_MAX;
static uint8_t attempts_max = UINT8_MAX;
static bool attempts_spent;
/* Parameter writes and FWU installation share the same durable record. */
K_MUTEX_DEFINE(budget_lock);

static bool valid_max(uint8_t maximum)
{
	return maximum == 3U || maximum == 5U || maximum == 10U;
}

static int load_budget(void)
{
	int result = kfsw_boot_store_open();
	uint8_t left;
	uint8_t maximum;

	if (result != 0) {
		return result;
	}
	result = kfsw_boot_store_read(&left, &maximum);
	if (result == -ENOENT) {
		left = CONFIG_KFSW_BOOT_ATTEMPTS_MAX;
		maximum = CONFIG_KFSW_BOOT_ATTEMPTS_MAX;
	} else if (result != 0) {
		return result;
	}
	if (!valid_max(maximum) || left > maximum) {
		return -EBADMSG;
	}
	attempts_left = left;
	attempts_max = maximum;
	return 0;
}

static int save_budget(uint8_t left, uint8_t maximum)
{
	int result = kfsw_boot_store_write(left, maximum);

	if (result == 0) {
		attempts_left = left;
		attempts_max = maximum;
	}
	return result;
}

bool kfsw_boot_budget_commanded(uint32_t cause, bool valid_commanded_note)
{
	/* PIN can accompany a software reset on STM32. Any stronger cause wins. */
	return valid_commanded_note && (cause & RESET_SOFTWARE) != 0U &&
	       (cause & ~(RESET_SOFTWARE | RESET_PIN)) == 0U;
}

int kfsw_boot_budget_start(bool commanded)
{
	int result;

	k_mutex_lock(&budget_lock, K_FOREVER);
	attempts_left = attempts_max = UINT8_MAX;
	result = load_budget();
	/* A budget of three is three startups, so the startup that spends the
	   last one still runs the application; the next one finds nothing left. */
	attempts_spent = result == 0 && attempts_left == 0U;
	if (result == 0 && !commanded && attempts_left > 0U) {
		/* Keep the in-memory decision even when the flash write fails. */
		attempts_left--;
		result = kfsw_boot_store_write(attempts_left, attempts_max);
	}
	k_mutex_unlock(&budget_lock);
	return result;
}

bool kfsw_boot_budget_spent(void)
{
	return attempts_spent;
}

void kfsw_boot_budget_get(uint8_t *left, uint8_t *maximum)
{
	k_mutex_lock(&budget_lock, K_FOREVER);
	if (attempts_max == UINT8_MAX) {
		/* Recovery never consumes, so this is the first read of the
		   record in that image, and the ground is asking for it. */
		(void)load_budget();
	}
	*left = attempts_left;
	*maximum = attempts_max;
	k_mutex_unlock(&budget_lock);
}

int kfsw_boot_budget_set_left(uint8_t left)
{
	int result;

	k_mutex_lock(&budget_lock, K_FOREVER);
	result = attempts_max == UINT8_MAX ? load_budget() : 0;
	if (result == 0) {
		result = left <= attempts_max ? save_budget(left, attempts_max) : -ERANGE;
	}
	k_mutex_unlock(&budget_lock);
	return result;
}

int kfsw_boot_budget_set_max(uint8_t maximum)
{
	int result;

	if (!valid_max(maximum)) {
		return -EINVAL;
	}
	k_mutex_lock(&budget_lock, K_FOREVER);
	result = attempts_max == UINT8_MAX ? load_budget() : 0;
	if (result == 0) {
		result = save_budget(MIN(attempts_left, maximum), maximum);
	}
	k_mutex_unlock(&budget_lock);
	return result;
}

int kfsw_boot_budget_refill(void)
{
	int result;

	k_mutex_lock(&budget_lock, K_FOREVER);
	result = attempts_max == UINT8_MAX ? load_budget() : 0;
	if (result == 0) {
		result = save_budget(attempts_max, attempts_max);
	}
	k_mutex_unlock(&budget_lock);
	return result;
}

#if CONFIG_KFSW_BOOT_BUDGET
static int early_budget(void)
{
	bool commanded = false;
	int result;

#if CONFIG_KFSW_LASTWORDS && !CONFIG_ARCH_POSIX
	commanded = kfsw_boot_budget_commanded(kfsw_boot_get_reset_cause(),
					       kfsw_boot_get_lastwords()->reason ==
						       KFSW_LASTWORDS_COMMANDED);
#endif
	result = kfsw_boot_budget_start(commanded);
	if (result != 0) {
		kfsw_log_warning("Boot budget store unavailable: %d; startup continues", result);
	}
	kfsw_log_info("Boot budget: left=%u max=%u commanded=%u", attempts_left, attempts_max,
		      commanded);
	if (kfsw_boot_budget_spent()) {
#if CONFIG_KFSW_GOLDEN
		enum kfsw_golden_outcome outcome;

		result = kfsw_golden_check(&outcome);
		if (result == -EBUSY || (result == 0 && outcome == KFSW_GOLDEN_INSTALLED)) {
			sys_reboot(SYS_REBOOT_COLD);
		}
		if (result != 0) {
			kfsw_log_warning("Golden unavailable: %d; startup continues", result);
		}
#else
		kfsw_log_warning("Boot budget exhausted; no Golden support; startup continues");
#endif
	}
	return 0;
}

SYS_INIT(early_budget, POST_KERNEL, 52);
#endif
