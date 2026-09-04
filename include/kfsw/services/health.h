#ifndef KFSW_SERVICES_HEALTH_H
#define KFSW_SERVICES_HEALTH_H

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
 * @defgroup kfsw_services_health K-FSW health monitoring
 * @ingroup kfsw_services
 *
 * Decides whether the system is still working, and stops feeding the watchdog
 * when it is not.
 *
 * The platform owns the watchdog mechanism and knows nothing about health. It
 * feeds on a timer, which keeps a board alive but proves only that a timer is
 * running. This service takes the feeding over and makes it conditional: parts
 * of the system say they are still running, and the watchdog is fed only while
 * all of them have said so recently enough.
 *
 * That is what closes the recovery chain. A component that stops checking in
 * stops the feed; the watchdog resets the part; the reset lands in the
 * bootloader; and an image still on trial is replaced by the one that worked.
 * Nothing in that sequence needs an operator, which is the point of it.
 *
 * A component is late, not dead, until its deadline passes. Deadlines are per
 * component because a radio worker and a storage worker do not run at the same
 * rate, and one deadline for both would either be too slow to catch the fast
 * one or too fast for the slow one.
 *
 * @{
 */

/** Largest number of components that can be watched. */
#define KFSW_HEALTH_MAX_COMPONENTS CONFIG_KFSW_HEALTH_MAX_COMPONENTS

/** Longest component name kept, including the terminator. */
#define KFSW_HEALTH_NAME_SIZE 16U

/** Event identifiers this service produces. Owned here, as every producer's are. */
enum kfsw_health_event {
	/** A watched component missed its deadline; the watchdog is withheld. */
	KFSW_EVENT_HEALTH_FAULT = 1,
};

/** What the service currently believes. */
enum kfsw_health_state {
	/** Registered components are all reporting within their deadlines. */
	KFSW_HEALTH_OK = 0,
	/** At least one component is overdue; the watchdog is no longer fed. */
	KFSW_HEALTH_FAULTED = 1,
	/** Supervision has not been started. */
	KFSW_HEALTH_STOPPED = 2,
};

/** What one watched component looks like from outside. */
struct kfsw_health_component {
	char name[KFSW_HEALTH_NAME_SIZE];
	/** How long this component may go without reporting. */
	uint32_t deadline_ms;
	/** Milliseconds since it last reported. */
	uint32_t since_report_ms;
	/** Reports received since registration; saturates. */
	uint32_t reports;
	/** True when it has missed its deadline. */
	bool overdue;
};

/** Consistent snapshot of what the service knows. */
struct kfsw_health_status {
	/** One of @ref kfsw_health_state. */
	uint8_t state;
	/** Components currently registered. */
	uint8_t count;
	/** True once the watchdog is being fed by this service. */
	bool feeding;
	/** Times the service has decided the system is unwell since boot. */
	uint32_t faults;
	/** Watchdog feeds this service has issued; saturates. */
	uint32_t feeds;
	/** Name of the component that caused the current fault, or empty. */
	char faulted_by[KFSW_HEALTH_NAME_SIZE];
};

/**
 * @brief Register a component to be watched.
 *
 * Registration is deliberately explicit rather than automatic. A component
 * that is watched without knowing it will eventually be the reason a working
 * satellite resets itself.
 *
 * @param name Short name, truncated to @ref KFSW_HEALTH_NAME_SIZE.
 * @param deadline_ms How long this component may go without reporting. Must be
 *                    long enough to cover its slowest normal cycle.
 * @param[out] handle Identifier to report with.
 *
 * @retval 0 Registered.
 * @retval -EINVAL @p name or @p handle is NULL, @p name is empty, or
 *                 @p deadline_ms is zero.
 * @retval -ENOSPC No room is left in the table.
 * @retval -EEXIST A component of that name is already registered.
 */
int kfsw_health_register(const char *name, uint32_t deadline_ms, uint8_t *handle);

/**
 * @brief Stop watching a component.
 *
 * Needed because a service can be stopped deliberately. A component that is no
 * longer running but is still watched will miss its deadline and reset a board
 * that is working exactly as intended.
 *
 * @param handle Identifier from @ref kfsw_health_register.
 *
 * @retval 0 No longer watched.
 * @retval -EINVAL The handle is not registered.
 */
int kfsw_health_unregister(uint8_t handle);

/**
 * @brief Report that a component is still running.
 *
 * Cheap on purpose: a component should be able to call this from its normal
 * loop without thinking about the cost.
 *
 * @param handle Identifier from @ref kfsw_health_register.
 *
 * @retval 0 Recorded.
 * @retval -EINVAL The handle is not registered.
 */
int kfsw_health_report(uint8_t handle);

/**
 * @brief Begin supervising, and take the watchdog over.
 *
 * The platform keep-alive is stopped and this service becomes responsible for
 * feeding. From here a component that stops reporting will reset the part.
 *
 * Every registered component is treated as having just reported, so a slow
 * start does not immediately look like a fault.
 *
 * @retval 0 Supervision is running.
 * @retval -EALREADY It was already running.
 * @retval -ENODEV There is no watchdog to take over.
 * @return A negative errno value from the platform on failure.
 */
int kfsw_health_start(void);

/**
 * @brief Evaluate every component once and feed or withhold accordingly.
 *
 * Called on a timer once supervision is running. Exposed so the decision can
 * be tested directly, without waiting out real deadlines.
 *
 * @retval 0 Everything is within its deadline and the watchdog was fed.
 * @retval -ETIMEDOUT At least one component is overdue; the feed was withheld.
 * @retval -EINVAL Supervision is not running.
 */
int kfsw_health_evaluate(void);

/**
 * @brief Read a consistent snapshot of the service.
 *
 * @param[out] status Destination snapshot.
 *
 * @retval 0 The snapshot was written.
 * @retval -EINVAL @p status is NULL.
 */
int kfsw_health_get_status(struct kfsw_health_status *status);

/**
 * @brief Read one component's entry.
 *
 * @param index Position in the table, below the registered count.
 * @param[out] component Destination entry.
 *
 * @retval 0 The entry was written.
 * @retval -EINVAL @p component is NULL.
 * @retval -ENOENT @p index is not registered.
 */
int kfsw_health_get_component(uint8_t index, struct kfsw_health_component *component);

/**
 * @brief Human-readable name for a state.
 *
 * @param state One of @ref kfsw_health_state.
 *
 * @return A stable lowercase name; "unknown" for an unrecognised value.
 */
const char *kfsw_health_state_name(enum kfsw_health_state state);

/** Milliseconds between deadline checks. */
uint32_t kfsw_health_get_interval_ms(void);

/**
 * @brief Whether an interval is safe to use, without applying it.
 *
 * Exposed so an owner can refuse the value before it is stored. A change
 * callback cannot refuse: by the time one runs the value is already written,
 * and rolling back afterwards still reports success for something rejected.
 *
 * @param interval_ms Milliseconds between checks.
 *
 * @retval 0 Safe, or no watchdog is armed for it to outlast.
 * @retval -EINVAL @p interval_ms is zero.
 * @retval -ERANGE The interval is slower than the watchdog's feed interval.
 */
int kfsw_health_check_interval_ms(uint32_t interval_ms);

/**
 * @brief Change how often deadlines are checked.
 *
 * Refused when the interval is slower than the watchdog's feed interval,
 * checked against the watchdog the system is actually running with rather
 * than a compiled constant. The watchdog is fed only by a check that finds
 * every component healthy, so a check slower than that resets a board where
 * nothing is wrong.
 *
 * @param interval_ms Milliseconds between checks.
 *
 * @retval 0 Applied from the next cycle.
 * @retval -EINVAL @p interval_ms is zero.
 * @retval -ERANGE The interval would outlast the watchdog.
 */
int kfsw_health_set_interval_ms(uint32_t interval_ms);

/** @} */

#if CONFIG_KFSW_PARAM
/** Parameter table owned by this service, in the service band. */
#define KFSW_HEALTH_PARAM_TABLE_ID 31U
/** Stable logical name paired with KFSW_HEALTH_PARAM_TABLE_ID. */
#define KFSW_HEALTH_PARAM_TABLE_NAME "health"

/** Health supervision state and the live check interval. */
extern const struct kfsw_param_definition_set kfsw_health_param_definitions;
#endif

#ifdef __cplusplus
}
#endif

#endif
