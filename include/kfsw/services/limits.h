#ifndef KFSW_SERVICES_LIMITS_H
#define KFSW_SERVICES_LIMITS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kfsw/services/fbo.h>
#include <kfsw/services/parameter.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup kfsw_services_limits Parameter limits
 * @ingroup kfsw_services
 *
 * Watches local parameters against a bound and releases a command when one is
 * crossed. The rules live in one text file under the FTP root, uploaded as a
 * procedure is, and read at boot and on command:
 *
 * @code
 * # parameter       cmp  bound  dwell  node  action
 * stack_worst_used  >    90     3      0     resmon sample
 * @endcode
 *
 * The first five whitespace separated fields are the parameter, comparison,
 * bound, dwell and node. Everything after them is the action, a registered
 * command with its arguments, kept as written. A line starting with `#` is a
 * comment.
 *
 * A file with any bad line is refused whole and the previous rule set stays in
 * force, so a rule set is never half loaded.
 *
 * A rule fires once per crossing. It latches when its action is released and
 * re-arms on the first sweep that finds the comparison false. The action goes
 * through kfsw_fbo_schedule_add(), so the release is recorded, may target
 * another node, and has no execution path of its own.
 *
 * @{
 */

/** Table 40: the counters of this service and its two switches. */
#define KFSW_LIMITS_PARAM_TABLE_ID 40U
/** Parameter table name. */
#define KFSW_LIMITS_PARAM_TABLE_NAME "limits"

/** Rule files live in this directory of the FTP root. */
#define KFSW_LIMITS_FTP_PATH "limits"

/** Event source id of this service is KFSW_EVENT_SOURCE_LIMITS. */
enum kfsw_limits_event {
	/** A rule released its action. Payload: rule index, then the node as a
	 *  big-endian u16.
	 */
	KFSW_EVENT_LIMITS_FIRED = 1,
};

/** Longest bound as written in the file, including the terminator. */
#define KFSW_LIMITS_BOUND_TEXT_MAX 24U

/** Shortest periodic sweep. Zero turns the periodic sweep off. */
#define KFSW_LIMITS_INTERVAL_MIN_MS 100U
/** Longest periodic sweep. */
#define KFSW_LIMITS_INTERVAL_MAX_MS 3600000U

/** Longest dwell a rule may ask for. */
#define KFSW_LIMITS_DWELL_MAX 255U

/** Value of last_fired_index before any rule has fired. */
#define KFSW_LIMITS_NO_RULE 255U

/** How a parameter is compared with its bound. */
enum kfsw_limits_cmp {
	KFSW_LIMITS_GT = 0,
	KFSW_LIMITS_GE = 1,
	KFSW_LIMITS_LT = 2,
	KFSW_LIMITS_LE = 3,
	KFSW_LIMITS_EQ = 4,
	KFSW_LIMITS_NE = 5,
};

/** One rule of the loaded set. */
struct kfsw_limits_rule {
	/** Position in the file among the rules, from 0. */
	uint8_t index;
	/** Local parameter watched. */
	char parameter[KFSW_PARAM_NAME_MAX + 1U];
	/** Type of the parameter, which is also the type of the bound. */
	enum kfsw_param_type type;
	/** enum kfsw_limits_cmp */
	uint8_t cmp;
	/** The bound parsed against @ref type. */
	union kfsw_param_scalar bound;
	/** The bound as written in the file. */
	char bound_text[KFSW_LIMITS_BOUND_TEXT_MAX];
	/** Consecutive sweeps the comparison must hold before the rule fires. */
	uint8_t dwell;
	/** Consecutive sweeps the comparison has held now. */
	uint8_t streak;
	/** The rule has fired and the comparison has not yet gone false. */
	bool latched;
	/** Node the action is released on; 0 is this node. */
	uint16_t node;
	/** The action: a registered command and its arguments. */
	char action[KFSW_FBO_SCHEDULE_LINE_MAX];
};

/** Counters and the last load result. Counters are per boot and saturate. */
struct kfsw_limits_status {
	/** Rules in force. */
	uint8_t rules;
	/** Sweeps completed with the service enabled. */
	uint32_t sweeps;
	/** Actions released. */
	uint32_t fires;
	/** Parameter reads and releases that failed. */
	uint32_t failures;
	/** Files refused for a bad line. */
	uint32_t parse_errors;
	/** Rule that fired last, or @ref KFSW_LIMITS_NO_RULE. */
	uint8_t last_fired_index;
	/** Monotonic milliseconds of the last release; 0 before any. */
	uint32_t last_fired_ms;
	/** Line of the last refused file that was at fault; 0 when the last load
	 *  succeeded or failed for another reason.
	 */
	uint16_t error_line;
	/** What was wrong with that line. Empty when @ref error_line is 0. */
	const char *error;
	/** The periodic sweep is scheduled. */
	bool running;
};

/** Visit the rules in force. Returning false stops the walk. The rule set is
 *  locked during the walk, so a visitor must not call back into this service.
 */
typedef bool (*kfsw_limits_visitor_t)(const struct kfsw_limits_rule *rule, void *context);

/**
 * @brief Read the configured rule file and start the periodic sweep.
 *
 * A missing file or storage is not a failure: the node simply has no rules
 * until one is loaded.
 *
 * @retval 0 Started, with or without rules.
 * @retval negative The file exists and could not be loaded; see
 *         kfsw_limits_load(). The periodic sweep runs regardless.
 */
int kfsw_limits_init(void);

/**
 * @brief Replace the rule set with the rules in a file.
 *
 * @param name File under the rule directory, without a path.
 * @retval 0 The file was loaded; the streaks and latches of every rule restart.
 * @retval -EINVAL @p name is NULL, empty, too long or has a path, or a line of
 *         the file is bad. The previous set stays in force and the line is in
 *         kfsw_limits_get_status().
 * @retval -ENODEV There is no storage.
 * @retval -ENOENT No such file.
 * @retval -EFBIG The file is larger than the rule set could be.
 */
int kfsw_limits_load(const char *name);

/**
 * @brief Evaluate every rule once.
 *
 * @return Rules evaluated.
 * @retval -EACCES The service is disabled; streaks restart.
 */
int kfsw_limits_sweep(void);

/** Change the periodic sweep. Zero stops it. Returns -ERANGE out of bounds. */
int kfsw_limits_set_interval_ms(uint32_t interval_ms);

/** Read the counters. */
void kfsw_limits_get_status(struct kfsw_limits_status *status);

/** Visit the rules in force. */
void kfsw_limits_visit_rules(kfsw_limits_visitor_t visitor, void *context);

/** The symbol for a comparison, for listings. */
const char *kfsw_limits_cmp_text(enum kfsw_limits_cmp cmp);

/** Nonzero lets the sweeps run. Backs the `limits_enabled` parameter. */
extern uint8_t kfsw_limits_enabled;
/** Milliseconds between periodic sweeps. Backs `limits_interval_ms`. */
extern uint32_t kfsw_limits_interval_ms;

/** Parameter table of this service. */
extern const struct kfsw_param_definition_set kfsw_limits_param_definitions;

/** @} */

#ifdef __cplusplus
}
#endif

#endif
