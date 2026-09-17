#ifndef KFSW_SERVICES_BOOT_H
#define KFSW_SERVICES_BOOT_H

#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Event IDs of the boot service. IDs are never reused.
 */
enum kfsw_event_boot_id {
	/** Startup finished. Payload: reset cause big-endian u32, then a byte
	 *  that is non-zero when the cause could not be read. */
	KFSW_EVENT_BOOT_READY = 1,
	/** The previous run left a note. Payload: reason byte, then detail, uptime
	 *  in milliseconds and boot count, each a big-endian u32. */
	KFSW_EVENT_BOOT_LASTWORDS = 2,
};

/**
 * @brief Emit the boot and readiness markers.
 */
void kfsw_boot_service_start(void);

/*
 * Reading the reset cause clears it, and this service reads it first. Get it
 * from here instead of the platform.
 */

/** Reset cause latched at boot. Zero before the service has run. */
uint32_t kfsw_boot_get_reset_cause(void);

/** Result of the latched read: 0 if the cause is trustworthy. */
int kfsw_boot_get_reset_result(void);

/** Version of the running image, from the build. */
const char *kfsw_boot_get_image_version(void);

/**
 * @brief Short revision of every repository compiled into this image.
 *
 * Formatted as `app:<sha> plat:<sha> svc:<sha> comms:<sha> mod:<sha>`, with a
 * trailing `+` on a repository that had uncommitted changes when it was built.
 * The image version alone comes from k-fsw, so it cannot tell two images apart
 * when only a dependency moved.
 */
const char *kfsw_boot_get_revisions(void);

/**
 * @brief The chip's unique ID, read at boot. Never NULL; empty when the SoC has none.
 */
const char *kfsw_boot_get_hardware_id(void);

#if CONFIG_KFSW_LASTWORDS
#include <kfsw/platform/lastwords.h>

/**
 * @brief The note the previous run left, read once at start-up.
 *
 * Reason is KFSW_LASTWORDS_NONE when there was no valid note.
 */
const struct kfsw_lastwords *kfsw_boot_get_lastwords(void);
#endif

#if CONFIG_KFSW_PARAM
/** Parameter table of this service, in the service band. */
#define KFSW_BOOT_PARAM_TABLE_ID 32U
/** Stable logical name paired with KFSW_BOOT_PARAM_TABLE_ID. */
#define KFSW_BOOT_PARAM_TABLE_NAME "boot"

/** Image identity and what the last restart was. */
extern const struct kfsw_param_definition_set kfsw_boot_param_definitions;

/**
 * @brief Count a restart.
 *
 * Called after the snapshot is restored, so the count continues from the saved
 * value. The count saturates.
 */
void kfsw_boot_count_restart(void);

/** Restarts recorded across the life of the node. */
uint32_t kfsw_boot_get_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
