#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "hk_internal.h"

int kfsw_hk_get_class(uint8_t report, uint8_t *retrieval_class)
{
	struct kfsw_hk_report *target = kfsw_hk_report_at(report);
	int result = 0;

	if (target == NULL || retrieval_class == NULL) {
		return -EINVAL;
	}
	kfsw_hk_lock();
	if (!target->defined) {
		result = -ENOENT;
	} else {
		*retrieval_class = target->retrieval_class;
	}
	kfsw_hk_unlock();
	return result;
}

int kfsw_hk_get_selected(uint8_t class_mask, uint16_t index, struct kfsw_hk_sample *sample)
{
	int result = -ENOENT;

	if (sample == NULL) {
		return -EINVAL;
	}
	kfsw_hk_lock();
	for (uint8_t cls = 0; cls <= KFSW_HK_CLASS_MAX; cls++) {
		uint16_t ages[CONFIG_KFSW_HK_REPORTS] = {0};
		uint16_t available = 0;

		if ((class_mask & (1U << cls)) == 0U) {
			continue;
		}
		for (uint8_t report = 0; report < CONFIG_KFSW_HK_REPORTS; report++) {
			const struct kfsw_hk_report *target = kfsw_hk_report_at(report);

			if (target->defined && target->retrieval_class == cls) {
				available += target->held;
			}
		}
		if (index >= available) {
			index -= available;
			continue;
		}
		/* Merge newest-first report rings; inspect only each ring's next sample.
		 * Collection order remains valid even when UTC steps backwards.
		 */
		for (uint16_t position = 0; position <= index; position++) {
			uint64_t newest = 0;
			uint8_t selected = 0;
			const struct kfsw_hk_sample *chosen = NULL;

			for (uint8_t report = 0; report < CONFIG_KFSW_HK_REPORTS; report++) {
				const struct kfsw_hk_report *target = kfsw_hk_report_at(report);
				uint16_t slot;

				if (!target->defined || target->retrieval_class != cls ||
				    ages[report] >= target->held) {
					continue;
				}
				slot = (uint16_t)((target->next_slot + ARRAY_SIZE(target->ring) -
						   1U - ages[report]) %
						  ARRAY_SIZE(target->ring));
				if (target->collected_order[slot] > newest) {
					newest = target->collected_order[slot];
					chosen = &target->ring[slot];
					selected = report;
				}
			}
			if (position == index && chosen != NULL) {
				*sample = *chosen;
				result = 0;
			}
			ages[selected]++;
		}
		break;
	}
	kfsw_hk_unlock();
	return result;
}
