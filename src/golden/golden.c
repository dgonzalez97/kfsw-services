#define KFSW_LOG_MODULE KFSW_LOG_MODULE_BOOT

#include <errno.h>

#include <zephyr/devicetree.h>
#include <bootutil/image.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <kfsw/services/boot.h>
#include <kfsw/services/fwu.h>
#include <kfsw/services/golden.h>
#include <kfsw/services/log.h>

#if CONFIG_KFSW_CSP_HMAC_REQUIRE
#warning "A recovery image that requires authentication cannot be reached without the key"
#endif

#define GOLDEN_NODE DT_CHOSEN(kfsw_golden_partition)
#define GOLDEN_PRESENT DT_NODE_EXISTS(GOLDEN_NODE)

#if GOLDEN_PRESENT
#define GOLDEN_ID DT_FIXED_PARTITION_ID(GOLDEN_NODE)
#define GOLDEN_SIZE DT_REG_SIZE(GOLDEN_NODE)

/* Read in pieces: the partition is larger than anything worth putting on a stack. */
#define GOLDEN_CHUNK 256U
#endif

bool kfsw_golden_present(void)
{
#if GOLDEN_PRESENT
	const struct flash_area *area;
	uint8_t header[4];
	bool found = false;

	if (flash_area_open(GOLDEN_ID, &area) != 0) {
		return false;
	}
	if (flash_area_read(area, 0U, header, sizeof(header)) == 0) {
		found = sys_get_le32(header) == IMAGE_MAGIC;
	}
	flash_area_close(area);
	return found;
#else
	return false;
#endif
}

int kfsw_golden_install(void)
{
#if GOLDEN_PRESENT
	const struct flash_area *area;
	uint8_t chunk[GOLDEN_CHUNK];
	uint32_t crc = 0U;
	uint32_t offset;
	int result;

	if (!kfsw_golden_present()) {
		return -ENOENT;
	}

	result = flash_area_open(GOLDEN_ID, &area);
	if (result != 0) {
		return result;
	}

	/* From the same bytes that will be written, so the update service checks
	   the copy and not the intention. */
	for (offset = 0U; offset < GOLDEN_SIZE; offset += GOLDEN_CHUNK) {
		result = flash_area_read(area, offset, chunk, GOLDEN_CHUNK);
		if (result != 0) {
			flash_area_close(area);
			return result;
		}
		crc = crc32_ieee_update(crc, chunk, GOLDEN_CHUNK);
	}

	result = kfsw_fwu_begin(GOLDEN_SIZE, crc);
	if (result != 0) {
		flash_area_close(area);
		return result;
	}

	for (offset = 0U; offset < GOLDEN_SIZE; offset += GOLDEN_CHUNK) {
		result = flash_area_read(area, offset, chunk, GOLDEN_CHUNK);
		if (result == 0) {
			result = kfsw_fwu_write(offset, chunk, GOLDEN_CHUNK);
		}
		if (result != 0) {
			flash_area_close(area);
			(void)kfsw_fwu_abort();
			return result;
		}
	}
	flash_area_close(area);

	result = kfsw_fwu_finish();
	if (result != 0) {
		(void)kfsw_fwu_abort();
		return result;
	}


	kfsw_log_warning("Recovery image installed; it boots at the next restart");
	return 0;
#else
	return -ENODEV;
#endif
}

int kfsw_golden_check(enum kfsw_golden_outcome *outcome)
{
	struct kfsw_boot_diagnostics diagnostics;
	uint8_t budget;
	int result;

	if (outcome == NULL) {
		return -EINVAL;
	}
	*outcome = KFSW_GOLDEN_NOT_NEEDED;

#if !CONFIG_KFSW_PARAM_PERSISTENCE
	/* Said once a boot, because the budget it counts against is not saved. */
	kfsw_log_warning("Recovery: boot_retries is not persisted, so the budget restarts at "
			 "every boot and this will not trigger");
#endif

#if CONFIG_KFSW_CSP_HMAC_REQUIRE
	/* The recovery image is what is left when everything else failed, and
	 * asking it for a key is how a recoverable node becomes a silent one.
	 */
	kfsw_log_warning("Recovery: this image refuses unsigned commanding, so a lost key "
			 "leaves it unreachable");
#endif

	kfsw_boot_get_diagnostics(&diagnostics);
	if (!diagnostics.valid) {
		/* Without a readable count there is no evidence to act on, and
		   installing the recovery image on no evidence is worse than
		   leaving a working image alone. */
		kfsw_log_warning("Recovery: no boot attempt count, so nothing is decided");
		return -ENODATA;
	}

	budget = kfsw_boot_get_retry_budget();
	if (diagnostics.attempts <= budget) {
		return 0;
	}

	if (!kfsw_golden_present()) {
		kfsw_log_error("Recovery: %u attempts against a budget of %u and the golden "
			       "partition is empty",
			       diagnostics.attempts, budget);
		return -ENOENT;
	}

	kfsw_log_warning("Recovery: %u unconfirmed boots against a budget of %u; installing the "
			 "recovery image",
			 diagnostics.attempts, budget);
	result = kfsw_golden_install();
	if (result != 0) {
		return result;
	}
	*outcome = KFSW_GOLDEN_INSTALLED;
	return 0;
}
