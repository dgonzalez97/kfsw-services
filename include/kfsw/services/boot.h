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
 * Event identifiers owned by the boot service.
 *
 * Numbers are stable and never reused. Payload layouts are documented here
 * because ground tooling decodes them.
 */
enum kfsw_event_boot_id {
	/** Startup finished. Payload: reset cause big-endian u32, then a byte
	 *  that is non-zero when the cause could not be read. */
	KFSW_EVENT_BOOT_READY = 1,
};

/**
 * @brief Emit the boot and readiness markers.
 */
void kfsw_boot_service_start(void);

/*
 * Reading the reset cause clears the latched hardware flags, and this service
 * is the first reader. Anything else that wants the cause must take it from
 * here; calling the platform again reports an empty register.
 */

/** Reset cause latched at boot. Zero before the service has run. */
uint32_t kfsw_boot_get_reset_cause(void);

/** Result of the latched read: 0 if the cause is trustworthy. */
int kfsw_boot_get_reset_result(void);

/** Version of the running image, from the build. */
const char *kfsw_boot_get_image_version(void);

#if CONFIG_KFSW_PARAM
/** Parameter table owned by this service, in the service band. */
#define KFSW_BOOT_PARAM_TABLE_ID 32U
/** Stable logical name paired with KFSW_BOOT_PARAM_TABLE_ID. */
#define KFSW_BOOT_PARAM_TABLE_NAME "boot"

/** Image identity and what the last restart was. */
extern const struct kfsw_param_definition_set kfsw_boot_param_definitions;

/**
 * @brief Record one more restart.
 *
 * Called by the composition after the persistent snapshot has been restored,
 * so the count continues from what was stored rather than from zero. Saturates
 * rather than wrapping: a counter that wraps hides the thing it was counting.
 */
void kfsw_boot_count_restart(void);

/** Restarts recorded across the life of the node. */
uint32_t kfsw_boot_get_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
