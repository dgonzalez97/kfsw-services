#ifndef KFSW_SERVICES_FBO_H
#define KFSW_SERVICES_FBO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kfsw_param_definition_set;

/** Parameter table reserved for file based operations. */
#define KFSW_FBO_PARAM_TABLE_ID 34U
/** Stable logical name paired with KFSW_FBO_PARAM_TABLE_ID. */
#define KFSW_FBO_PARAM_TABLE_NAME "fbo"

/** Longest procedure name, including the terminator. */
#define KFSW_FBO_NAME_MAX 32U

/** @defgroup kfsw_services_fbo File based operations
 *  @ingroup kfsw_services
 *  Carry out a sequence of commands written in a file.
 *
 *  A node is handed a file and does what is in it — a pass plan, a recovery
 *  sequence, a reconfiguration — without an operator on the link for every
 *  step. Every line is a command the command service already validates, so
 *  nothing can be written into a file that could not be sent over the radio.
 *
 *  Deliberately not a language. There are no loops and no jumps, so a
 *  procedure terminates by construction, which is what makes it safe to run
 *  with nobody watching.
 *
 *  @{
 */

/** What a procedure run has done so far. */
struct kfsw_fbo_status {
	/** Procedure currently running, or the last one that ran. */
	char name[KFSW_FBO_NAME_MAX];
	/** Whether a procedure is running now. */
	bool running;
	/** Lines carried out since boot, across all runs. */
	uint32_t lines_run;
	/** Lines that reported a failure. */
	uint32_t lines_failed;
	/** Lines skipped by a guard. */
	uint32_t lines_skipped;
	/** Procedures started since boot. */
	uint32_t runs;
	/** Line the current or last run stopped on, 1-based, 0 before any. */
	uint16_t line;
};

/**
 * @brief Initialize the service.
 *
 * @retval 0 Ready, or already initialized.
 */
int kfsw_fbo_init(void);

/**
 * @brief Start a procedure.
 *
 * Returns as soon as the run is accepted; the procedure itself runs on the
 * service's own thread, because a `wait` line blocks and must not hold the
 * caller. Watch it through kfsw_fbo_get_status() or the event record.
 *
 * @param name File under the procedure directory, without a path.
 * @retval 0 The run was accepted.
 * @retval -EINVAL @p name is NULL, empty, too long, or contains a path.
 * @retval -EACCES The service is not initialized.
 * @retval -EBUSY A procedure is already running.
 * @retval -ENOENT No such procedure.
 * @retval -ENODEV There is no storage.
 */
int kfsw_fbo_run(const char *name);

/**
 * @brief Ask a running procedure to stop.
 *
 * Takes effect before the next line, so a `wait` already under way finishes
 * first. A procedure that is not running is not an error.
 *
 * @retval 0 A stop was requested, or nothing was running.
 */
int kfsw_fbo_stop(void);

/**
 * @brief Read what the service has done.
 *
 * @param[out] status Destination.
 * @retval 0 Written.
 * @retval -EINVAL @p status is NULL.
 */
int kfsw_fbo_get_status(struct kfsw_fbo_status *status);

/** Read-only live PARAM definitions owned by file based operations. */
extern const struct kfsw_param_definition_set kfsw_fbo_param_definitions;

/** @} */

#ifdef __cplusplus
}
#endif

#endif
