#ifndef KFSW_SERVICES_GNDWDT_H
#define KFSW_SERVICES_GNDWDT_H

#include <stdbool.h>
#include <stdint.h>

#include <kfsw/services/command.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_gndwdt K-FSW ground watchdog
 * @ingroup kfsw_services
 *
 * Resets the node if no valid ground_wtd CSP feed arrives before the timeout.
 * The feed must contain KFSWWSFK; any subsystem may send it. The get command
 * reads the countdown without feeding.
 *
 * This timer is independent of @ref kfsw_services_health component deadlines.
 *
 * @{
 */

/** Table 35: ground watchdog configuration and counters. */
#define KFSW_GNDWDT_PARAM_TABLE_ID 35U
/** Stable logical name paired with KFSW_GNDWDT_PARAM_TABLE_ID. */
#define KFSW_GNDWDT_PARAM_TABLE_NAME "gndwdt"

/** Event IDs of this service. */
enum kfsw_gndwdt_event {
	/** The timeout passed with no contact; a reset follows. */
	KFSW_EVENT_GNDWDT_EXPIRED = 1,
};

/** What the ground watchdog is doing. */
struct kfsw_gndwdt_status {
	/** Silence allowed before a reset, in seconds. */
	uint32_t timeout_s;
	/** Seconds since the last contact, or since the service started. */
	uint32_t since_contact_s;
	/** Seconds left; zero if stopped, expired or waiting to reset. */
	uint32_t remaining_s;
	/** Contacts counted since start; saturates. */
	uint32_t contacts;
	/** Times the timeout passed since start; saturates. */
	uint32_t expiries;
	/** Node of the most recent contact, or zero. */
	uint16_t last_node;
	/** The timer runs only while this is set. */
	bool enabled;
	/** The service has been started. */
	bool running;
};

/** Start the timer. The countdown begins now, not at boot. */
int kfsw_gndwdt_start(void);

/** Stop the timer. Does not cancel a reset already queued. */
int kfsw_gndwdt_stop(void);

/** Stable wire ID for ground_wtd, with one text argument: KFSWWSFK or get. */
#define KFSW_COMMAND_ID_GROUND_WTD 16U

/** Register this set on both the flight node and its command clients. */
extern const struct kfsw_command_definition_set kfsw_gndwdt_command_definitions;

/**
 * @brief Check the timer once.
 *
 * @retval 0 Contact is recent enough, or the watchdog is stopped or disabled.
 * @retval -ETIMEDOUT The timeout has passed. The caller resets the node.
 */
int kfsw_gndwdt_evaluate(void);

/** Copy the current status. */
void kfsw_gndwdt_get_status(struct kfsw_gndwdt_status *status);

/**
 * @brief Set how long silence is allowed.
 *
 * Changing the timeout does not count as contact or restart the countdown.
 *
 * @retval 0 Applied.
 * @retval -ERANGE Outside the configured bounds.
 */
int kfsw_gndwdt_set_timeout_s(uint32_t timeout_s);

/** Turn the timer on or off. Changing this does not restart the countdown. */
void kfsw_gndwdt_set_enabled(bool enabled);

#if CONFIG_KFSW_PARAM
/** Parameter table 35. */
extern const struct kfsw_param_definition_set kfsw_gndwdt_param_definitions;
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif
