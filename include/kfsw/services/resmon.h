#ifndef KFSW_SERVICES_RESMON_H
#define KFSW_SERVICES_RESMON_H

#include <stdbool.h>
#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_resmon K-FSW resource monitor
 * @ingroup kfsw_services
 *
 * How much stack the threads of this node have left. A thread that runs out
 * fails in a way that is hard to read afterwards, and the numbers were only
 * reachable from a bench probe until now.
 *
 * Usage is a percentage of each thread's own stack. Bytes alone say nothing
 * across threads sized differently: the idle thread sits near its limit by
 * design, and a worker with a kilobyte spare may still be the one in trouble.
 *
 * Sampling walks the kernel's thread list, so nothing has to be instrumented
 * or registered.
 *
 * The figures are real on an MCU target. On the POSIX architecture a thread
 * runs on a host stack and the declared one is ignored, so what a Linux build
 * reports exercises the mechanism without measuring anything.
 *
 * @{
 */

/** Table 36: stack headroom and its counters. */
#define KFSW_RESMON_PARAM_TABLE_ID 36U
/** Stable logical name paired with KFSW_RESMON_PARAM_TABLE_ID. */
#define KFSW_RESMON_PARAM_TABLE_NAME "resmon"

/** Longest thread name kept, including the terminator. */
#define KFSW_RESMON_NAME_SIZE 20U

/** Event IDs of this service. */
enum kfsw_resmon_event {
	/** A thread reached the alert percentage. Payload: the percentage used,
	 *  then its unused bytes as a big-endian u32.
	 */
	KFSW_EVENT_RESMON_LOW_STACK = 1,
};

/** What the sweeps have found. */
struct kfsw_resmon_status {
	/** Sweeps completed since start; saturates. */
	uint32_t sweeps;
	/** Times a sweep first found a thread at the alert percentage. */
	uint32_t alerts;
	/** Highest stack use seen on any thread since start, as a percentage. */
	uint32_t worst_used_percent;
	/** Unused bytes on that thread when that percentage was recorded. */
	uint32_t worst_unused_bytes;
	/** Stack size of that thread. */
	uint32_t worst_stack_bytes;
	/** Highest stack use in the most recent sweep, as a percentage. */
	uint32_t last_used_percent;
	/** A thread at or above this percentage raises an event. */
	uint32_t alert_percent;
	/** Threads the last sweep could read. */
	uint16_t threads;
	/** Name of the thread holding worst_used_percent. */
	char worst_thread[KFSW_RESMON_NAME_SIZE];
	/** The periodic sweep is running. */
	bool running;
};

/** Start sweeping on the configured period. */
int kfsw_resmon_start(void);

/** Stop sweeping. The recorded numbers stay readable. */
int kfsw_resmon_stop(void);

/**
 * @brief Sweep once, whether or not the periodic sweep is running.
 *
 * @return Threads read, or a negative errno.
 */
int kfsw_resmon_sample(void);

/** Copy the current numbers. */
void kfsw_resmon_get_status(struct kfsw_resmon_status *status);

/**
 * @brief Set the stack use that raises an event, as a percentage.
 *
 * @retval 0 Applied.
 * @retval -ERANGE Zero or above 100.
 */
int kfsw_resmon_set_alert_percent(uint32_t percent);

#if CONFIG_KFSW_PARAM
/** Parameter table 36. */
extern const struct kfsw_param_definition_set kfsw_resmon_param_definitions;
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif
