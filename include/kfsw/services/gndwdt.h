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
 * @defgroup kfsw_services_gndwdt Ground watchdog
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
/** Parameter table name. */
#define KFSW_GNDWDT_PARAM_TABLE_NAME "gndwdt"

/** Event IDs of this service. */
enum kfsw_gndwdt_event {
	/** No valid feed arrived before the timeout; a reset follows. */
	KFSW_EVENT_GNDWDT_EXPIRED = 1,
};

/** Ground watchdog state and counters. */
struct kfsw_gndwdt_status {
	/** Maximum time between valid feeds, in seconds. */
	uint32_t timeout_s;
	/** Seconds since the last valid feed or service start. */
	uint32_t since_contact_s;
	/** Seconds left; zero if stopped, expired or waiting to reset. */
	uint32_t remaining_s;
	/** Valid feeds since start; saturates. */
	uint32_t contacts;
	/** Times the timeout passed since start; saturates. */
	uint32_t expiries;
	/** Source of the most recent valid feed, or zero. */
	uint16_t last_node;
	/** Whether expiry checks are enabled. */
	bool enabled;
	/** The service has been started. */
	bool running;
};

/** Start the timer. The countdown begins now, not at boot. */
int kfsw_gndwdt_start(void);

/** Stop the timer, and cancel a reset already queued. */
int kfsw_gndwdt_stop(void);

/** Stable wire ID for ground_wtd, with one text argument: KFSWWSFK or get. */
#define KFSW_COMMAND_ID_GROUND_WTD 16U

/** The ground_wtd argument that feeds; anything else except get is refused. */
#define KFSW_GNDWDT_FEED_WORD "KFSWWSFK"

/** Register this set on both the flight node and its command clients. */
extern const struct kfsw_command_definition_set kfsw_gndwdt_command_definitions;

/**
 * @brief Check the timer once.
 *
 * @retval 0 The feed is recent enough, or the watchdog is stopped or disabled.
 * @retval -ETIMEDOUT The timeout has passed. The caller resets the node.
 */
int kfsw_gndwdt_evaluate(void);

/** Copy the current status. */
void kfsw_gndwdt_get_status(struct kfsw_gndwdt_status *status);

/**
 * @brief Set the feed timeout.
 *
 * Changing the timeout does not feed or restart the countdown.
 *
 * @retval 0 Applied.
 * @retval -ERANGE Outside the configured bounds.
 */
int kfsw_gndwdt_set_timeout_s(uint32_t timeout_s);

/** Turn the timer on or off. Changing this does not restart the countdown. */
void kfsw_gndwdt_set_enabled(bool enabled);

#if CONFIG_KFSW_COMMAND_CSP
/**
 * @brief Feed another node's ground watchdog over CSP, or only read it.
 *
 * Sends ground_wtd to @p node. The reply detail carries the countdown and the
 * timeout. A node never feeds itself: the watchdog is there to prove that
 * somebody else still talks to it.
 *
 * @param node Node to reach.
 * @param feed true to feed, false to read without feeding.
 * @param result Filled with the node's reply.
 * @retval 0 The node replied; @p result->status says how.
 * @retval -EINVAL @p node is zero or one of this node's addresses, or @p result is NULL.
 * @retval other Transport error from the command service.
 */
int kfsw_gndwdt_remote(uint16_t node, bool feed, struct kfsw_command_result *result);
#endif

#if CONFIG_KFSW_PARAM
/** Parameter table 35. */
extern const struct kfsw_param_definition_set kfsw_gndwdt_param_definitions;
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif
