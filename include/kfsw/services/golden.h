/**
 * @file
 * @brief Install the recovery image when an image will not stay up.
 *
 * The golden partition holds a signed image that is known to boot and to be
 * reachable. Nothing boots it from there: MCUboot boots the slots, so recovery
 * means copying it into the secondary slot and asking for a swap.
 *
 * The trigger is the boot attempt count the boot service already keeps against
 * the running image. Once it passes the retry budget, the image has had its
 * chances and the recovery image goes in.
 */

#ifndef KFSW_SERVICES_GOLDEN_H
#define KFSW_SERVICES_GOLDEN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * Checks the bootloader's magic at the start of the partition. It does not
 * verify the signature; the bootloader does that when it swaps.
 */
bool kfsw_golden_present(void);

/** Size in bytes of the golden partition, or zero when there is none. */
uint32_t kfsw_golden_size(void);

/**
 * @brief Copy the recovery image into the secondary slot and schedule a swap.
 *
 * The caller restarts the node. Refused with -EBUSY while the bootloader still
 * needs the secondary slot to revert, because that revert is the cheaper
 * recovery and it should happen first.
 *
 * @retval 0 on success.
 * @retval -ENOENT when the partition holds no image.
 * @retval -ENODEV when this build has no golden partition.
 */
int kfsw_golden_install(void);

/**
 * @brief Decide whether the running image has run out of attempts, and act.
 *
 * Reports what it did rather than acting silently, so the composition decides
 * whether to restart.
 *
 * @param outcome Filled with the decision. Must not be NULL.
 * @retval 0 on success, a negative errno when the decision could not be made.
 */
int kfsw_golden_check(enum kfsw_golden_outcome *outcome);

#ifdef __cplusplus
}
#endif

#endif
