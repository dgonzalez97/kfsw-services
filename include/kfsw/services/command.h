#ifndef KFSW_SERVICES_COMMAND_H
#define KFSW_SERVICES_COMMAND_H

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
 * @file
 * @brief Commands called by name from the shell and by ID over CSP.
 *
 * Both use the same definition, validation and handler. Definitions are
 * registered at build time, and the registry is fixed before startup ends.
 */

#define KFSW_COMMAND_MAX_ARGS 4U
#define KFSW_COMMAND_MAX_TEXT_SIZE 64U
#define KFSW_COMMAND_MAX_DETAIL_SIZE 96U

/** Argument and result value kinds carried on the wire. */
enum kfsw_command_type {
	KFSW_COMMAND_TYPE_U32 = 1,
	KFSW_COMMAND_TYPE_I32 = 2,
	KFSW_COMMAND_TYPE_TEXT = 3,
};

/* Matches the Kconfig range, so a value accepted at runtime is one the
 * composition could have been built with. */
#define KFSW_COMMAND_TIMEOUT_MIN_MS 1000U
#define KFSW_COMMAND_TIMEOUT_MAX_MS 120000U

/** Lifetime totals for the service. Counters saturate and are never reset. */
struct kfsw_command_stats {
	/** Invocations that reached a handler or were refused before one. */
	uint32_t invoked;
	/** Invocations whose handler reported a failure. */
	uint32_t failed;
	/** Invocations naming a command that is not registered. */
	uint32_t unknown;
	/** Invocations refused before the handler ran. */
	uint32_t rejected;
	/** Commands in the frozen registry. */
	uint16_t registered;
};

/** Outcome of one command invocation. */
enum kfsw_command_status {
	KFSW_COMMAND_OK = 0,
	KFSW_COMMAND_UNKNOWN = 1,
	KFSW_COMMAND_INVALID_ARGUMENT = 2,
	KFSW_COMMAND_DENIED = 3,
	KFSW_COMMAND_BUSY = 4,
	KFSW_COMMAND_FAILED = 5,
	KFSW_COMMAND_UNAVAILABLE = 6,
};

/**
 * Event IDs of the command service. IDs are never reused. Payload: command ID
 * and source node as big-endian u16, then the status byte.
 */
enum kfsw_event_command_id {
	/** A command ran, whatever its outcome. */
	KFSW_EVENT_COMMAND_INVOKED = 1,
	/** A request named a command this node does not implement. */
	KFSW_EVENT_COMMAND_UNKNOWN = 2,
	/** A request failed validation before any handler ran. */
	KFSW_EVENT_COMMAND_REJECTED = 3,
};

/** Command flags. */
#define KFSW_COMMAND_FLAG_MUTATING BIT(0)

/** Longest claim owner name, including the terminator. */
#define KFSW_COMMAND_CLAIM_NAME_MAX 32U

/**
 * How a claim holder shares the command path with everything else.
 *
 * A claim does not serialize anything by itself: handlers already run one at a
 * time. It decides what happens to an invocation from another thread while the
 * holder is part way through a sequence of them.
 */
enum kfsw_command_claim_mode {
	/** Another thread's invocation is served, and counted as an intrusion. */
	KFSW_COMMAND_CLAIM_SHARED = 0,
	/** Another thread's invocation is refused, naming the holder. */
	KFSW_COMMAND_CLAIM_EXCLUSIVE = 1,
};

/** The claim as a reader sees it. */
struct kfsw_command_claim_state {
	/** Whether a claim is held now. */
	bool held;
	enum kfsw_command_claim_mode mode;
	/** Who holds it, for the operator and for a refusal detail. */
	char owner[KFSW_COMMAND_CLAIM_NAME_MAX];
	/** Invocations from another thread since the claim was taken. */
	uint32_t intrusions;
	/** Invocations refused because the claim was exclusive. */
	uint32_t refusals;
};

/** One validated argument handed to a handler. */
struct kfsw_command_arg {
	enum kfsw_command_type type;
	union {
		uint32_t u32;
		int32_t i32;
		const char *text;
	} value;
};

/**
 * Where a request came from. There is no authentication yet, so handlers must
 * not trust any particular source.
 */
struct kfsw_command_source {
	/** CSP node that issued the request, or 0 for a local invocation. */
	uint16_t node;
	/** End-to-end command identity, separate from link protection. Currently false. */
	bool authenticated;
	/** Set by the CSP front end; false for shell and procedure calls. */
	bool via_csp;
};

/** Command result. */
struct kfsw_command_result {
	enum kfsw_command_status status;
	/** Optional short human-readable detail. May be left empty. */
	char detail[KFSW_COMMAND_MAX_DETAIL_SIZE];
};

/**
 * Command implementation.
 *
 * Runs in the caller's thread under the command mutex. Remote requests use the
 * command server thread, not the CSP router. Arguments have been checked for
 * count and type. Text is NUL-terminated and valid only during the call.
 */
typedef int (*kfsw_command_handler_t)(const struct kfsw_command_arg *args, size_t arg_count,
				      const struct kfsw_command_source *source,
				      struct kfsw_command_result *result);

/** One command. */
struct kfsw_command_definition {
	/** Stable numeric identifier used on the wire. Never reused. */
	uint16_t id;
	/** Stable short name used by the shell. */
	const char *name;
	/** Description shown by `cmd list`. */
	const char *help;
	uint32_t flags;
	uint8_t arg_count;
	/** Expected type of each argument, in order. */
	const enum kfsw_command_type *arg_types;
	kfsw_command_handler_t handler;
};

/** A compile-time group of command definitions. */
struct kfsw_command_definition_set {
	const struct kfsw_command_definition *commands;
	size_t count;
};

/** Description of one registered command, for enumeration and argument parsing. */
struct kfsw_command_info {
	uint16_t id;
	const char *name;
	const char *help;
	uint32_t flags;
	uint8_t arg_count;
	/** Expected type of each argument, so a front end can convert its input. */
	const enum kfsw_command_type *arg_types;
};

typedef bool (*kfsw_command_visitor_t)(const struct kfsw_command_info *info, void *context);

/**
 * Aggregate the supplied definition sets and freeze the registry.
 *
 * Rejects duplicate identifiers, duplicate names, missing handlers, and
 * argument counts above KFSW_COMMAND_MAX_ARGS.
 */
int kfsw_command_init(const struct kfsw_command_definition_set *const *sets, size_t set_count);

/** Return whether the registry was built successfully. */
bool kfsw_command_is_initialized(void);

/** Visit each registered command. */
void kfsw_command_visit(kfsw_command_visitor_t visitor, void *context);

/**
 * @brief Convert one text argument to the type a command declares.
 *
 * A text argument is not copied: @p text must outlive the invocation.
 *
 * @param text Argument as written.
 * @param type Type the command declares for that position.
 * @param[out] arg Destination.
 * @retval 0 Converted.
 * @retval -EINVAL @p text is not a number where one is required, or a NULL was
 *         given.
 * @retval -ENAMETOOLONG The text is longer than KFSW_COMMAND_MAX_TEXT_SIZE.
 * @retval -ENOTSUP The type is not one this service carries.
 */
int kfsw_command_parse_arg(const char *text, enum kfsw_command_type type,
			   struct kfsw_command_arg *arg);

/** Look up one registered command by name. Returns -ENOENT when absent. */
int kfsw_command_find(const char *name, struct kfsw_command_info *info);

/** Invoke a command on this node by name. */
int kfsw_command_invoke(const char *name, const struct kfsw_command_arg *args, size_t arg_count,
			struct kfsw_command_result *result);

/** Invoke a command on this node by wire identifier. */
int kfsw_command_invoke_id(uint16_t id, const struct kfsw_command_arg *args, size_t arg_count,
			   const struct kfsw_command_source *source,
			   struct kfsw_command_result *result);

/**
 * @brief Claim the command path for a sequence of invocations.
 *
 * The calling thread becomes the holder. Its own invocations are unaffected;
 * an invocation from any other thread is served and counted in @ref
 * kfsw_command_claim_state::intrusions when the mode is shared, and refused
 * with KFSW_COMMAND_BUSY naming @p owner when it is exclusive.
 *
 * Calling it again from the holder changes the mode and keeps the counters, so
 * a sequence can protect one part of itself.
 *
 * @param mode How to treat another thread's invocation.
 * @param owner Short name of the holder, shown in a refusal and in status.
 * @retval 0 Claimed.
 * @retval -EINVAL @p owner is NULL, empty or longer than the limit.
 * @retval -EBUSY Another thread holds the claim.
 */
int kfsw_command_claim_acquire(enum kfsw_command_claim_mode mode, const char *owner);

/**
 * @brief Release a claim held by the calling thread.
 *
 * @retval 0 Released, or nothing was held.
 * @retval -EPERM Another thread holds the claim.
 */
int kfsw_command_claim_release(void);

/** Read the claim. Returns -EINVAL for a NULL destination. */
int kfsw_command_claim_get(struct kfsw_command_claim_state *state);

/** Human-readable name for a status, for shell output and logs. */
const char *kfsw_command_status_name(enum kfsw_command_status status);

/** Read the lifetime totals. Returns -EINVAL for a NULL destination. */
int kfsw_command_get_stats(struct kfsw_command_stats *stats);

#if CONFIG_KFSW_COMMAND_CSP
/** Timeout used by the next remote invocation. */
uint32_t kfsw_command_get_timeout_ms(void);

/** Whether a timeout would be accepted, without applying it. */
int kfsw_command_check_timeout_ms(uint32_t timeout_ms);

/**
 * @brief Change the timeout used by new remote invocations.
 *
 * @retval 0 Applied to the next invocation.
 * @retval -ERANGE Outside the range the composition could have been built with.
 */
int kfsw_command_set_timeout_ms(uint32_t timeout_ms);
#endif

/** Applies console echo; provided by the application. */
typedef void (*kfsw_command_echo_handler_t)(bool enabled);

/**
 * @brief Register the function that applies console echo, and apply the current value.
 */
void kfsw_command_set_echo_handler(kfsw_command_echo_handler_t handler);

/** Whether console echo is enabled. Off by default. */
bool kfsw_command_echo_enabled(void);

/** Change console echo and apply it through the registered handler. */
void kfsw_command_set_echo(bool enabled);

#if defined(CONFIG_KFSW_COMMAND_CSP)

/** Bind the command port and start serving remote requests. */
int kfsw_command_server_start(void);

/** Return whether the remote front end is accepting requests. */
bool kfsw_command_server_is_started(void);

/**
 * Invoke a command on a remote node by name.
 *
 * The name is resolved against this node's registry to obtain the wire
 * identifier, so both nodes must agree on identifiers. A node that does not
 * implement the identifier answers with KFSW_COMMAND_UNKNOWN. Node 0, and the
 * broadcast address or above, return -EINVAL.
 */
int kfsw_command_invoke_remote(uint16_t node, const char *name, const struct kfsw_command_arg *args,
			       size_t arg_count, struct kfsw_command_result *result);

/**
 * Invoke using a reserved server ticket, with at most three attempts per phase.
 * Requires retry support and entropy on both peers; never falls back to legacy.
 * A timeout may leave the outcome unknown. Calling this again is a new operation.
 */
int kfsw_command_invoke_remote_retry(uint16_t node, const char *name,
				     const struct kfsw_command_arg *args, size_t arg_count,
				     struct kfsw_command_result *result);

#endif /* CONFIG_KFSW_COMMAND_CSP */

#if CONFIG_KFSW_PARAM
/** Parameter table of this service, in the service band. */
#define KFSW_COMMAND_PARAM_TABLE_ID 28U
/** Parameter table name. */
#define KFSW_COMMAND_PARAM_TABLE_NAME "command"

/** Command counters and reply timeout. */
extern const struct kfsw_param_definition_set kfsw_command_param_definitions;
#endif

#ifdef __cplusplus
}
#endif

#endif
