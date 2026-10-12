#ifndef KFSW_SERVICES_FBO_H
#define KFSW_SERVICES_FBO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kfsw_param_definition_set;
struct kfsw_command_definition_set;

/** Parameter table reserved for file based operations. */
#define KFSW_FBO_PARAM_TABLE_ID 34U
/** Parameter table name. */
#define KFSW_FBO_PARAM_TABLE_NAME "fbo"

/** Wire identifiers of this service's commands. Never reused. */
#define KFSW_COMMAND_ID_FBO_RUN 20U
#define KFSW_COMMAND_ID_FBO_SCHED_ADD 21U
#define KFSW_COMMAND_ID_FBO_SCHED_CANCEL 22U
#define KFSW_COMMAND_ID_FBO_SCHED_CLEAR 23U

/** Longest procedure name, including the terminator. */
#define KFSW_FBO_NAME_MAX 32U

/** Procedures live in this directory of the FTP root. */
#define KFSW_FBO_FTP_PATH "procedures"

/** @defgroup kfsw_services_fbo File based operations
 *  @ingroup kfsw_services
 *  Run a list of commands from a file. Every line is a normal command, and
 *  there are no loops or jumps.
 *
 *  A procedure does not own the command path. A command arriving from the
 *  shell or from another node while a procedure runs is served: it waits at
 *  most for the one handler in flight, never for the rest of the procedure,
 *  and the step it ran underneath is counted and recorded. A procedure that
 *  cannot survive that protects itself with a `concurrency exclusive` line,
 *  and from there a command from anywhere else is refused, naming the running
 *  procedure, until a `concurrency shared` line or the end of the run.
 *
 *  @{
 */

/** Procedure state and execution counters. */
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
	/**
	 * Lines a command from somewhere else ran underneath. A direct command
	 * takes precedence over a procedure step, so an operator needs to be
	 * able to tell afterwards that the two interleaved.
	 */
	uint32_t lines_interleaved;
	/** Commands refused while a protected part of a procedure was running. */
	uint32_t refused_direct;
	/** Line the current or last run stopped on, 1-based, 0 before any. */
	uint16_t line;
	/** Whether the running procedure is protecting itself against concurrency. */
	bool exclusive;
	/** Last completed run: 0 on success, negative errno on failure or cancellation. */
	int last_result;
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
 * Returns once the run is accepted; the procedure runs on the service thread.
 * Follow it with kfsw_fbo_get_status() or the event record.
 *
 * @param name File under the procedure directory, without a path.
 * @retval 0 The run was accepted.
 * @retval -EINVAL @p name is NULL, empty, too long, or contains a path.
 * @retval -EACCES The service is not initialized.
 * @retval -EBUSY A procedure is already running.
 * @retval -ENOENT No such procedure.
 * @retval -ENODEV There is no storage.
 * @retval -EISDIR The name refers to a directory.
 * @retval -EFBIG The file exceeds CONFIG_KFSW_FBO_BYTES_MAX.
 */
int kfsw_fbo_run(const char *name);

/**
 * @brief Ask a running procedure to stop.
 *
 * Wakes a `wait` immediately. Other command handlers finish before the stop
 * takes effect. Stopping while idle has no effect on the next run.
 *
 * @retval 0 A stop was requested, or nothing was running.
 */
int kfsw_fbo_stop(void);

/**
 * @brief Read procedure state and counters.
 *
 * @param[out] status Destination.
 * @retval 0 Written.
 * @retval -EINVAL @p status is NULL.
 */
int kfsw_fbo_get_status(struct kfsw_fbo_status *status);

/** @defgroup kfsw_services_fbo_schedule Time-tagged releases
 *  @ingroup kfsw_services_fbo
 *
 *  Release a registered command at a time, instead of now. An entry names a
 *  command exactly as a procedure line does, so a release is an ordinary
 *  invocation with the same validation, counters and events.
 *
 *  The queue is volatile. A reset clears it, deliberately: a queue that
 *  outlives a reset while its clock may have jumped fires at the wrong time
 *  with nobody watching.
 *
 *  Two kinds of time, and they are not equally safe.
 *
 *  **Relative** is the safer one. An entry due in N seconds needs only the
 *  monotonic clock, so it is right even on a node that was never told the
 *  time, and a clock step does not touch it because it never consults the wall
 *  clock.
 *
 *  **Absolute** needs the clock to have been set this power cycle. While
 *  nothing has set it, adding or releasing an absolute entry is refused and
 *  says so. Note what *set* claims: that something wrote the clock since this
 *  power cycle. It is a weaker claim than *correct*. An RTC set once and since
 *  drifted, or set from a wrong source, still reads as set. When a time
 *  quality service exists the gate tightens from was-set to a minimum quality,
 *  and that is the only change.
 *
 *  @{
 */

/** Longest entry text, a command name and its arguments, with the terminator. */
#define KFSW_FBO_SCHEDULE_LINE_MAX 80U

/** Which clock an entry is due against. */
enum kfsw_fbo_time_kind {
	/** Seconds from the moment the entry was added, on the monotonic clock. */
	KFSW_FBO_TIME_RELATIVE = 0,
	/** UTC seconds, and only with the clock set. */
	KFSW_FBO_TIME_ABSOLUTE = 1,
};

/** What has become of one entry. */
enum kfsw_fbo_entry_status {
	/** Waiting for its time. */
	KFSW_FBO_ENTRY_SCHEDULED = 0,
	/** Released; the handler has not returned yet. */
	KFSW_FBO_ENTRY_RUNNING = 1,
	/** Released and the command reported success. */
	KFSW_FBO_ENTRY_COMPLETED = 2,
	/** Released and the command reported a failure. */
	KFSW_FBO_ENTRY_FAILED = 3,
	/** Its time passed by more than the latency buffer; never released. */
	KFSW_FBO_ENTRY_OVERDUE = 4,
	/** Cancelled before it was released. */
	KFSW_FBO_ENTRY_CANCELLED = 5,
};

/** One queue entry. */
struct kfsw_fbo_schedule_entry {
	/** Command name and arguments, as given. */
	char line[KFSW_FBO_SCHEDULE_LINE_MAX];
	/** Absolute entry: UTC second it is due. Zero for a relative entry. */
	int64_t due_utc;
	/** UTC second it was released, or 0 when the clock was not set then. */
	int64_t released_utc;
	/** Relative entry: seconds asked for. Zero for an absolute entry. */
	uint32_t delay_s;
	/** Seconds still to wait, 0 once due. */
	uint32_t remaining_s;
	/** CRC32 of this entry's content: kind, time, node, command and arguments. */
	uint32_t content_hash;
	/** Handler result, or the error that stopped the release. */
	int result;
	/** Node the command is released on; 0 is this node. */
	uint16_t node;
	/** Slot, as cancel addresses it. */
	uint16_t index;
	/** Wire identifier resolved when the entry was added. */
	uint16_t command_id;
	/** enum kfsw_fbo_time_kind */
	uint8_t kind;
	/** enum kfsw_fbo_entry_status */
	uint8_t status;
	/** enum kfsw_command_status of the release. */
	uint8_t command_status;
};

/** Queue state and counters. Counters are per boot and saturate. */
struct kfsw_fbo_schedule_status {
	/** CRC32 over the content of every occupied slot, in slot order. */
	uint32_t hash;
	/** Releases attempted since boot. */
	uint32_t releases;
	/** Entries refused at add. */
	uint32_t refusals;
	/** Entries whose time passed beyond the buffer. */
	uint32_t overdues;
	/** Wall clock steps the scheduler noticed. */
	uint32_t clock_steps;
	/** Latency buffer in seconds: late inside it still runs. */
	uint32_t latency_s;
	/** UTC of the earliest scheduled absolute entry, or 0. */
	int64_t next_due_utc;
	/** Seconds to the earliest scheduled relative entry, 0 when there is none. */
	uint32_t next_due_s;
	uint16_t capacity;
	uint16_t entries;
	uint16_t scheduled;
	uint16_t running;
	uint16_t completed;
	uint16_t failed;
	uint16_t overdue;
	uint16_t cancelled;
	/** Whether the wall clock has been set, which is what absolute entries need. */
	bool clock_set;
	/** Last error a release reported. */
	int last_error;
};

/**
 * @brief Add one entry.
 *
 * The command is resolved and its arguments parsed now, so a bad entry is
 * refused while an operator is listening rather than at its release.
 *
 * @param kind Relative or absolute.
 * @param when Seconds from now, or the UTC second, by @p kind.
 * @param node Node to release on; 0 is this node.
 * @param line Command name and arguments, as a procedure line writes them.
 * @param[out] index Slot taken, or the slot already holding this content.
 * @retval 0 Scheduled.
 * @retval -EINVAL A NULL, an empty line, a bad kind, or a time out of range.
 * @retval -ENODATA An absolute entry while the clock has not been set.
 * @retval -ENOENT The command is not registered.
 * @retval -EACCES The registry is not frozen.
 * @retval -ENOSPC The queue is full; nothing changed.
 * @retval -EEXIST An entry with this content is already live; @p index names it.
 * @retval -ENOTSUP @p node is another node and this build has no remote
 *         commanding.
 */
int kfsw_fbo_schedule_add(enum kfsw_fbo_time_kind kind, int64_t when, uint16_t node,
			  const char *line, uint16_t *index);

/**
 * @brief Cancel a scheduled entry.
 *
 * @retval 0 Cancelled; it will not run.
 * @retval -ENOENT No such slot.
 * @retval -EALREADY The entry is running, or has already finished. Cancellation
 *         never claims to have stopped something that fired.
 */
int kfsw_fbo_schedule_cancel(uint16_t index);

/** Empty the queue. A release in flight finishes. Returns the slots dropped. */
int kfsw_fbo_schedule_clear(void);

/** Read one slot. Returns -ENOENT for an empty or out-of-range slot. */
int kfsw_fbo_schedule_get_entry(uint16_t index, struct kfsw_fbo_schedule_entry *entry);

/** Read the queue state. Returns -EINVAL for a NULL destination. */
int kfsw_fbo_schedule_get_status(struct kfsw_fbo_schedule_status *status);

/**
 * @brief Change the latency buffer.
 *
 * An entry due less than this long ago still runs; anything later reports
 * overdue instead of firing at the wrong time.
 *
 * @retval 0 Applied.
 * @retval -ERANGE Below the command reply timeout, which would make a release
 *         waiting its turn look overdue, or above the compiled maximum.
 */
int kfsw_fbo_schedule_set_latency_s(uint32_t seconds);

/** The latency buffer in seconds. */
uint32_t kfsw_fbo_schedule_get_latency_s(void);

/** @} */

/** Parameter table of file based operations. */
extern const struct kfsw_param_definition_set kfsw_fbo_param_definitions;

/**
 * Commands of this service: run a procedure, and add, cancel or clear a
 * time-tagged release. Registered by the application alongside its own set.
 */
extern const struct kfsw_command_definition_set kfsw_fbo_command_definitions;

/** @} */

#ifdef __cplusplus
}
#endif

#endif
