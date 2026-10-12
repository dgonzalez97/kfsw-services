#ifndef KFSW_SERVICES_GOLDEN_H
#define KFSW_SERVICES_GOLDEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_golden Recovery image
 * @ingroup kfsw_services
 *
 * Installs the image held in the golden partition once the running one has
 * booted unconfirmed more times than its retry budget allows. The bootloader
 * only boots the slots, so installing means copying the image into the
 * secondary slot and asking for a permanent swap.
 *
 * @{
 */

/** What a recovery decision concluded. */
enum kfsw_golden_outcome {
	/** The running image is within its budget, or there is nothing to install. */
	KFSW_GOLDEN_NOT_NEEDED = 0,
	/** The recovery image is in the secondary slot and a swap is scheduled. */
	KFSW_GOLDEN_INSTALLED,
};

/**
 * @brief Whether the golden partition holds something that looks like an image.
 *
 * Checks the bootloader's magic. The signature is checked by the bootloader
 * when it swaps, not here.
 */
bool kfsw_golden_present(void);

/**
 * @brief Copy the recovery image into the secondary slot and schedule a swap.
 *
 * The caller restarts the node. Refused with -EBUSY while the bootloader still
 * needs the secondary slot to revert, because that revert is the cheaper
 * recovery and it comes first.
 *
 * @retval 0 on success.
 * @retval -ENOENT when the partition holds no image.
 * @retval -ENODEV when this build has no golden partition.
 */
int kfsw_golden_install(void);

/**
 * @brief Decide whether the running image has run out of attempts, and act.
 *
 * @param outcome Filled with the decision. Must not be NULL.
 * @retval 0 on success, a negative errno when the decision could not be made.
 */
int kfsw_golden_check(enum kfsw_golden_outcome *outcome);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
