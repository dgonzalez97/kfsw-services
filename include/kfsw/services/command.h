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
 * @brief One normalized path for invoking a K-FSW operation.
 *
 * A command is defined once and reachable two ways: by name from the shell,
 * and by numeric identifier over CSP. Both front ends resolve to the same
 * definition, the same argument validation and the same handler, so a local
 * operator and a ground station cannot diverge.
 *
 * Definitions are contributed as compile-time sets by their semantic owner,
 * the same way parameter definitions are, and the registry is frozen before
 * the application reports readiness.
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
 * Event identifiers owned by the command service.
 *
 * Numbers are stable and never reused. Payloads carry the command identifier
 * and the source node as big-endian u16, then the status as one byte.
 */
enum kfsw_event_command_id {
	/** A command ran, whatever its outcome. */
	KFSW_EVENT_COMMAND_INVOKED = 1,
	/** A request named a command this node does not implement. */
	KFSW_EVENT_COMMAND_UNKNOWN = 2,
	/** A request failed validation before any handler ran. */
	KFSW_EVENT_COMMAND_REJECTED = 3,
};

/** Behavioural flags a definition declares about itself. */
#define KFSW_COMMAND_FLAG_MUTATING BIT(0)

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
 * Where a request came from.
 *
 * Authentication and authorization are not implemented. The fields are
 * reserved now so that adding them later does not change this structure's
 * meaning: a handler must never assume a particular source is trusted.
 */
struct kfsw_command_source {
	/** CSP node that issued the request, or 0 for a local invocation. */
	uint16_t node;
	/** True once the request has been authenticated. Always false today. */
	bool authenticated;
};

/** What a handler reports back. */
struct kfsw_command_result {
	enum kfsw_command_status status;
	/** Optional short human-readable detail. May be left empty. */
	char detail[KFSW_COMMAND_MAX_DETAIL_SIZE];
};

/**
 * Command implementation.
 *
 * Runs on the command worker thread, never on a CSP receive context. Arguments
 * are already checked for count and type. Text arguments are NUL-terminated
 * and remain valid only for the duration of the call.
 */
typedef int (*kfsw_command_handler_t)(const struct kfsw_command_arg *args, size_t arg_count,
				      const struct kfsw_command_source *source,
				      struct kfsw_command_result *result);

/** One command, owned by the component that implements it. */
struct kfsw_command_definition {
	/** Stable numeric identifier used on the wire. Never reused. */
	uint16_t id;
	/** Stable short name used by the shell. */
	const char *name;
	/** One-line description shown by `command list`. */
	const char *help;
	uint32_t flags;
	uint8_t arg_count;
	/** Expected type of each argument, in order. */
	const enum kfsw_command_type *arg_types;
	kfsw_command_handler_t handler;
};

/** A compile-time group of command definitions from one semantic owner. */
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

/** Look up one registered command by name. Returns -ENOENT when absent. */
int kfsw_command_find(const char *name, struct kfsw_command_info *info);

/** Invoke a command on this node by name. */
int kfsw_command_invoke(const char *name, const struct kfsw_command_arg *args, size_t arg_count,
			struct kfsw_command_result *result);

/** Invoke a command on this node by wire identifier. */
int kfsw_command_invoke_id(uint16_t id, const struct kfsw_command_arg *args, size_t arg_count,
			   const struct kfsw_command_source *source,
			   struct kfsw_command_result *result);

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

/** Applies console echo. Provided by the composition, which owns the console. */
typedef void (*kfsw_command_echo_handler_t)(bool enabled);

/**
 * @brief Register what applies console echo.
 *
 * The console belongs to the composition rather than to this service, so the
 * service holds the setting and the composition applies it. Registering also
 * applies the current value, so the default reaches the shell without waiting
 * for anyone to write the parameter.
 */
void kfsw_command_set_echo_handler(kfsw_command_echo_handler_t handler);

/** Whether the console repeats what is typed at it. Off by default. */
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
 * implement the identifier answers with KFSW_COMMAND_UNKNOWN.
 */
int kfsw_command_invoke_remote(uint16_t node, const char *name, const struct kfsw_command_arg *args,
			       size_t arg_count, struct kfsw_command_result *result);

#endif /* CONFIG_KFSW_COMMAND_CSP */

#if CONFIG_KFSW_PARAM
/** Parameter table owned by this service, in the service band. */
#define KFSW_COMMAND_PARAM_TABLE_ID 28U
/** Stable logical name paired with KFSW_COMMAND_PARAM_TABLE_ID. */
#define KFSW_COMMAND_PARAM_TABLE_NAME "command"

/** Command counters and the timeout the service applies. */
extern const struct kfsw_param_definition_set kfsw_command_param_definitions;
#endif

#ifdef __cplusplus
}
#endif

#endif
