#ifndef KFSW_SERVICES_BOOT_H
#define KFSW_SERVICES_BOOT_H

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

#ifdef __cplusplus
}
#endif

#endif
