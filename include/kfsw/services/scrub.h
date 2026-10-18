#ifndef KFSW_SERVICES_SCRUB_H
#define KFSW_SERVICES_SCRUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_scrub Image scrub
 * @ingroup kfsw_services
 *
 * Re-reads the resident images and compares a CRC32 against a reference kept
 * across resets. FWU checks an image as it arrives and the bootloader checks
 * the slot it boots; without this, the slot that is not running and the
 * recovery image are never read again, so a bit flip in either is found when
 * the image is needed and not before.
 *
 * The reference is learned on command, never silently: a region whose
 * reference is unknown reports @ref KFSW_SCRUB_UNKNOWN and never
 * @ref KFSW_SCRUB_OK.
 *
 * The storage partition is deliberately not a region. It is a mutable
 * filesystem holding the parameter snapshots, the journal and the procedure
 * files, so a checksum over it would differ after every ordinary write.
 *
 * @{
 */

/** Table 39: what the last sweep found in each region. */
#define KFSW_SCRUB_PARAM_TABLE_ID 39U
/** Parameter table name. */
#define KFSW_SCRUB_PARAM_TABLE_NAME "scrub"

/** Longest region name kept, including the terminator. */
#define KFSW_SCRUB_NAME_SIZE 8U

/** The regions this build can sweep. */
enum kfsw_scrub_region {
	/** The primary slot, where the running image came from. */
	KFSW_SCRUB_SLOT0 = 0,
	/** The secondary slot, where an update waits for its swap. */
	KFSW_SCRUB_SLOT1 = 1,
	/** The recovery image. */
	KFSW_SCRUB_GOLDEN = 2,
	/** Regions defined, present or not. */
	KFSW_SCRUB_REGIONS = 3,
};

/** What a sweep concluded about one region. */
enum kfsw_scrub_state {
	/** No image header, so there is nothing to check. An erased secondary
	 *  slot reads this way and it is the normal case.
	 */
	KFSW_SCRUB_EMPTY = 0,
	/** An image is there and no reference has been learned for it. */
	KFSW_SCRUB_UNKNOWN = 1,
	/** The measured CRC32 matches the reference. */
	KFSW_SCRUB_OK = 2,
	/** It does not. The image on flash is not the one that was learned. */
	KFSW_SCRUB_MISMATCH = 3,
	/** The region could not be read. */
	KFSW_SCRUB_UNREADABLE = 4,
	/** The region does not exist in this build. */
	KFSW_SCRUB_ABSENT = 5,
};

/** Event IDs of this service. */
enum kfsw_scrub_event {
	/** A region's image no longer matches its reference. Payload: the
	 *  region id, then the measured CRC32 as a big-endian u32.
	 */
	KFSW_EVENT_SCRUB_MISMATCH = 1,
};

/** What the last sweep found in one region. */
struct kfsw_scrub_result {
	/** enum kfsw_scrub_region */
	uint8_t id;
	/** enum kfsw_scrub_state */
	uint8_t state;
	/** Stable lowercase name, as the shell and the parameters spell it. */
	const char *name;
	/** Bytes swept: the image length from its header, not the partition. */
	uint32_t bytes;
	/** The learned value, or zero when none is. */
	uint32_t reference;
	/** What the last sweep measured, or zero before the first one. */
	uint32_t measured;
};

/** Sweep counters. Per boot unless stated, and they saturate. */
struct kfsw_scrub_status {
	/** Regions present in this build. */
	uint8_t regions;
	/** Sweeps completed. */
	uint32_t sweeps;
	/** Regions found mismatched, counted once per crossing. */
	uint32_t mismatches;
	/** Monotonic milliseconds at the end of the last sweep; 0 before any. */
	uint32_t last_sweep_ms;
	/** The periodic sweep is running. */
	bool running;
};

/** A visitor over the regions. Returning false stops the walk. */
typedef bool (*kfsw_scrub_visitor_t)(const struct kfsw_scrub_result *result, void *context);

/** Start sweeping on the configured period. */
int kfsw_scrub_start(void);

/** Stop sweeping. The recorded numbers stay readable. */
int kfsw_scrub_stop(void);

/**
 * @brief Sweep every present region once.
 *
 * @return Regions swept, or a negative errno.
 */
int kfsw_scrub_sweep(void);

/**
 * @brief Sweep one region.
 *
 * @retval 0 Swept; read the result to see what it concluded.
 * @retval -ENODEV The region does not exist in this build.
 * @retval -EINVAL No such region.
 */
int kfsw_scrub_sweep_region(enum kfsw_scrub_region region);

/**
 * @brief Take the last measurement of a region as its reference.
 *
 * Sweeps the region first, so what is learned is what is on flash now. The
 * reference is persisted, because a reference held only in RAM would be
 * relearned at every boot and could never report a mismatch.
 *
 * @retval 0 Learned and persisted.
 * @retval -ENODEV The region does not exist in this build.
 * @retval -ENOENT The region holds no image.
 * @retval -EIO The region could not be read, or the snapshot not written.
 */
int kfsw_scrub_learn(enum kfsw_scrub_region region);

/** Copy the counters. */
void kfsw_scrub_get_status(struct kfsw_scrub_status *status);

/** Read one region's last result. Returns -EINVAL for an unknown region. */
int kfsw_scrub_get_result(enum kfsw_scrub_region region, struct kfsw_scrub_result *result);

/** Walk every region, present or not, in id order. */
void kfsw_scrub_visit_regions(kfsw_scrub_visitor_t visitor, void *context);

#if CONFIG_KFSW_PARAM
/** Parameter table 39. */
extern const struct kfsw_param_definition_set kfsw_scrub_param_definitions;
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif
