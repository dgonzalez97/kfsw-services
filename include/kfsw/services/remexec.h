#ifndef KFSW_SERVICES_REMEXEC_H
#define KFSW_SERVICES_REMEXEC_H

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
 * @defgroup kfsw_services_remexec Remote shell execution
 * @ingroup kfsw_services
 *
 * Reads which shell commands a node offers for remote execution and runs one
 * of them, returning what it printed to the node that asked.
 *
 * The allowlist the composition hands to kfsw_remexec_init() is the security
 * boundary. A command is not reachable until it is marked there, the listing
 * reports exactly the marked commands, and anything else is refused with a
 * reason naming the command. There is no wildcard, no debug mode and no PIN:
 * discovery and permission are one list, so they cannot disagree. A node that
 * never calls kfsw_remexec_init(), or marks nothing, answers the listing with
 * an empty reply and refuses every execution.
 *
 * Output is bounded. A reply carries at most KFSW_REMEXEC_OUTPUT_MAX bytes and
 * says how many it dropped, so a chatty command cannot flood the link and an
 * operator is never left guessing whether the output was complete. Whether the
 * command succeeded is reported separately from whether its output fitted.
 *
 * @{
 */

/** Table 38: what remote execution accepted, refused and truncated. */
#define KFSW_REMEXEC_PARAM_TABLE_ID 38U
/** Parameter table name. */
#define KFSW_REMEXEC_PARAM_TABLE_NAME "remexec"

/** Longest command line accepted, excluding the terminator. */
#define KFSW_REMEXEC_COMMAND_MAX 64U

/** Largest output one reply carries, excluding the terminator. */
#define KFSW_REMEXEC_OUTPUT_MAX ((size_t)CONFIG_KFSW_REMEXEC_OUTPUT_MAX)

/** Longest refusal text kept and published, including the terminator. */
#define KFSW_REMEXEC_REFUSAL_MAX 96U

/** Outcome of one request. */
enum kfsw_remexec_status {
	/** The command ran and reported success. */
	KFSW_REMEXEC_OK = 0,
	/** The command is not marked in the serving node's allowlist. */
	KFSW_REMEXEC_NOT_OFFERED = 1,
	/** The command line is longer than KFSW_REMEXEC_COMMAND_MAX. */
	KFSW_REMEXEC_TOO_LONG = 2,
	/** The command line is empty or holds a byte that is not printable. */
	KFSW_REMEXEC_INVALID = 3,
	/** An execution is already running; requests are refused, never queued. */
	KFSW_REMEXEC_BUSY = 4,
	/** The command overran the configured budget. */
	KFSW_REMEXEC_TIMEOUT = 5,
	/** The command ran and reported a failure. */
	KFSW_REMEXEC_FAILED = 6,
	/** The service is not initialised on the serving node. */
	KFSW_REMEXEC_UNAVAILABLE = 7,
};

/** One command the composition offers for remote execution. */
struct kfsw_remexec_entry {
	/**
	 * Shell command as typed, with its subcommand but without arguments,
	 * for example "storage info". Marking it marks its arguments too, so
	 * mark only commands whose whole argument space is safe to run from
	 * the ground.
	 */
	const char *command;
	/** One line describing it, as the listing reports it. */
	const char *help;
};

/** The commands a composition offers. A count of zero offers nothing. */
struct kfsw_remexec_allowlist {
	const struct kfsw_remexec_entry *entries;
	size_t count;
};

/**
 * One reply. @p status says what happened to the command; @p truncated and
 * @p dropped say what happened to its output. The two are independent: a
 * command can succeed and still have its output cut.
 */
struct kfsw_remexec_reply {
	enum kfsw_remexec_status status;
	/** What the shell handler returned. Meaningful for OK and FAILED. */
	int32_t command_result;
	/** Bytes the command printed. */
	uint32_t produced;
	/** Bytes the reply did not carry. */
	uint32_t dropped;
	/** Whether @p text is short of what the command printed. */
	bool truncated;
	/** Bytes held in @p text. */
	uint16_t size;
	/**
	 * The listing, the captured output, or the refusal reason. Always
	 * NUL-terminated.
	 */
	char text[KFSW_REMEXEC_OUTPUT_MAX + 1U];
};

/** Lifetime totals. Counters saturate and are never reset. */
struct kfsw_remexec_stats {
	/** Requests whose command was allowed and run. */
	uint32_t accepted;
	/** Requests refused before any command ran. */
	uint32_t refused;
	/** Replies that could not carry all the output. */
	uint32_t truncated;
	/** Commands marked in the allowlist. */
	uint16_t entries;
};

/**
 * @brief Adopt the allowlist and prepare the capture shell.
 *
 * Call once, before the server starts. The entries are referenced, not
 * copied, so they must have static lifetime.
 *
 * @param allowlist Commands this node offers. NULL, or a count of zero,
 *        offers nothing and is a valid composition.
 * @retval 0 Adopted.
 * @retval -E2BIG More entries than CONFIG_KFSW_REMEXEC_MAX_ENTRIES.
 * @retval -EINVAL An entry has no command or no help text, or a command is
 *         empty, holds a byte that is not printable, or repeats another.
 * @retval -ENAMETOOLONG A command is longer than KFSW_REMEXEC_COMMAND_MAX.
 * @retval -EALREADY Already initialised.
 * @retval -EAGAIN The capture shell did not become ready, so a command's
 *         output could not be read back. Nothing is offered.
 */
int kfsw_remexec_init(const struct kfsw_remexec_allowlist *allowlist);

/** Whether an allowlist was adopted. */
bool kfsw_remexec_is_initialized(void);

/** Read the lifetime totals. */
void kfsw_remexec_get_stats(struct kfsw_remexec_stats *stats);

/** The most recent refusal, as the operator was told it. Empty before any. */
const char *kfsw_remexec_last_refusal(void);

/** Human-readable name for a status, for shell output and logs. */
const char *kfsw_remexec_status_name(enum kfsw_remexec_status status);

/**
 * @brief Fill @p reply with the commands this node offers.
 *
 * With an empty or NULL @p prefix the listing is one command name a line.
 * With a prefix it is the matching command and its subcommands, each as
 * `name<TAB>help`. A listing longer than the output cap is truncated and
 * reports the bytes it dropped.
 */
void kfsw_remexec_list_local(const char *prefix, struct kfsw_remexec_reply *reply);

/**
 * @brief Run @p line on this node if the allowlist offers it.
 *
 * Refuses rather than queues when an execution is already running. Must not
 * be called from a shell command handler: the capture shell cannot execute
 * from within an execution.
 */
void kfsw_remexec_run_local(const char *line, struct kfsw_remexec_reply *reply);

/** Bind the remote execution port and start serving requests. */
int kfsw_remexec_server_start(void);

/** Whether the server is accepting requests. */
bool kfsw_remexec_server_is_started(void);

/**
 * @brief Ask @p node which commands it offers.
 *
 * @retval 0 The node answered; the outcome is in @p reply.
 * @retval -EINVAL Node 0, the broadcast address or above, or the local node.
 * @retval -EACCES CSP is not running.
 * @retval -ETIMEDOUT No reply inside CONFIG_KFSW_REMEXEC_TIMEOUT_MS.
 * @retval -EBADMSG The reply did not match the request.
 */
int kfsw_remexec_list_remote(uint16_t node, const char *prefix,
			     struct kfsw_remexec_reply *reply);

/**
 * @brief Ask @p node to run @p line and return what it printed.
 *
 * Errors are those of kfsw_remexec_list_remote(). A timeout leaves the
 * outcome unknown: the serving node runs the command to completion whether or
 * not the requester is still there, and discards a reply it cannot deliver.
 */
int kfsw_remexec_run_remote(uint16_t node, const char *line, struct kfsw_remexec_reply *reply);

#if CONFIG_KFSW_PARAM
/** Parameter table 38. */
extern const struct kfsw_param_definition_set kfsw_remexec_param_definitions;
#endif

/** @} */

#ifdef __cplusplus
}
#endif

#endif
