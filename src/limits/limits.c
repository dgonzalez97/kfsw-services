#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#if CONFIG_KFSW_PARAM_CSP
#include <csp/csp_iflist.h>
#include <kfsw/comms/csp.h>
#endif

#include <kfsw/platform/storage.h>
#include <kfsw/platform/time.h>
#include <kfsw/services/command.h>
#include <kfsw/services/event.h>
#include <kfsw/services/fbo.h>
#include <kfsw/services/ftp.h>
#include <kfsw/services/limits.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_LIMITS
#include <kfsw/services/log.h>

/* Parameter, comparison, bound, dwell, node and a 79 character action with its
 * separators come to about 150 bytes; a read-node prefix adds at most 6.
 */
#define LIMITS_LINE_BYTES 192U
#define LIMITS_FIELDS 5U
/* Room for a full rule set with comments between the rules. */
#define LIMITS_FILE_BYTES_MAX (CONFIG_KFSW_LIMITS_MAX_RULES * LIMITS_LINE_BYTES * 2U)
/* The scheduler refuses a release due now, so the shortest delay is one second. */
/*
 * The rules arrive as a file, so a composition without the file transfer
 * service can only ever run the rules it was compiled with, which is none.
 */
#if !IS_ENABLED(CONFIG_KFSW_FTP) && !IS_ENABLED(CONFIG_ZTEST)
#warning "KFSW_LIMITS without KFSW_FTP: nothing can upload a rule file to this node"
#endif

#define LIMITS_RELEASE_DELAY_S 1U

BUILD_ASSERT((CONFIG_KFSW_LIMITS_INTERVAL_MS == 0) ||
		     ((CONFIG_KFSW_LIMITS_INTERVAL_MS >= KFSW_LIMITS_INTERVAL_MIN_MS) &&
		      (CONFIG_KFSW_LIMITS_INTERVAL_MS <= KFSW_LIMITS_INTERVAL_MAX_MS)),
	     "KFSW_LIMITS_INTERVAL_MS must be zero or between 100 ms and an hour");
BUILD_ASSERT(CONFIG_KFSW_LIMITS_MAX_RULES < KFSW_LIMITS_NO_RULE,
	     "a rule index must not collide with the no-rule marker");

uint8_t kfsw_limits_enabled = 1U;
uint32_t kfsw_limits_interval_ms = CONFIG_KFSW_LIMITS_INTERVAL_MS;

/* The rule set, the staging copy and the line buffer. Held across the
 * parameter service, the command registry and the scheduler, so the counters
 * have their own lock: the parameter sampler takes the table lock first and
 * then state_lock, and nothing may hold state_lock while it waits for either.
 */
static K_MUTEX_DEFINE(rules_lock);
static struct kfsw_limits_rule rules[CONFIG_KFSW_LIMITS_MAX_RULES];
static struct kfsw_limits_rule staged[CONFIG_KFSW_LIMITS_MAX_RULES];
static size_t rule_count;
#if CONFIG_KFSW_PARAM_CSP
#define LIMITS_REMOTE_GROUP_MAX 16U
/* Remote I/O gets half the interval, capped at 5 s (also for manual sweeps).
 * Each group has its own deadline and at most 16 names. Name resolution can
 * exchange once per cache miss, retry reads or walk the list; the one-node
 * cache can be evicted between groups. The deadline bounds that work, not a
 * promise of one wire exchange per node. Defer when less than 10 ms remains.
 */
#define LIMITS_REMOTE_BUDGET_MS 5000U
#define LIMITS_REMOTE_MIN_MS 10U
static const char *names[LIMITS_REMOTE_GROUP_MAX];
static size_t members[LIMITS_REMOTE_GROUP_MAX];
static struct kfsw_param_value values[LIMITS_REMOTE_GROUP_MAX];
static bool done[CONFIG_KFSW_LIMITS_MAX_RULES];
static size_t remote_resume;
#endif
static char line[LIMITS_LINE_BYTES];

static K_MUTEX_DEFINE(state_lock);
static struct kfsw_limits_status state = {
	.last_fired_index = KFSW_LIMITS_NO_RULE,
	.error = "",
};
static bool initialized;

static void work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(limits_work, work_handler);
static struct k_work_q limits_queue;
static K_THREAD_STACK_DEFINE(limits_stack, CONFIG_KFSW_LIMITS_STACK_SIZE);

static const struct {
	const char *text;
	enum kfsw_limits_cmp cmp;
} comparisons[] = {
	{">", KFSW_LIMITS_GT},  {">=", KFSW_LIMITS_GE}, {"<", KFSW_LIMITS_LT},
	{"<=", KFSW_LIMITS_LE}, {"==", KFSW_LIMITS_EQ}, {"!=", KFSW_LIMITS_NE},
};

const char *kfsw_limits_cmp_text(enum kfsw_limits_cmp cmp)
{
	for (size_t index = 0U; index < ARRAY_SIZE(comparisons); index++) {
		if (comparisons[index].cmp == cmp) {
			return comparisons[index].text;
		}
	}
	return "?";
}

static void saturating_increment(uint32_t *counter)
{
	if (*counter < UINT32_MAX) {
		(*counter)++;
	}
}

static void note_failure(void)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	saturating_increment(&state.failures);
	k_mutex_unlock(&state_lock);
}

/* A parameter value widened to the one type its comparison is done in. */
struct wide {
	enum { WIDE_UNSIGNED, WIDE_SIGNED, WIDE_FLOAT } kind;
	union {
		uint64_t u;
		int64_t i;
		double f;
	};
};

/* Comparison of two values that cannot be ordered, a NaN. */
#define ORDER_NONE 2

static bool is_numeric(enum kfsw_param_type type)
{
	return (type <= KFSW_PARAM_DOUBLE);
}

static bool is_float(enum kfsw_param_type type)
{
	return (type == KFSW_PARAM_FLOAT) || (type == KFSW_PARAM_DOUBLE);
}

static void widen(enum kfsw_param_type type, const union kfsw_param_scalar *scalar,
		  struct wide *wide)
{
	wide->kind = WIDE_UNSIGNED;
	switch (type) {
	case KFSW_PARAM_U8:
	case KFSW_PARAM_X8:
		wide->u = scalar->u8;
		break;
	case KFSW_PARAM_U16:
	case KFSW_PARAM_X16:
		wide->u = scalar->u16;
		break;
	case KFSW_PARAM_U32:
	case KFSW_PARAM_X32:
		wide->u = scalar->u32;
		break;
	case KFSW_PARAM_U64:
	case KFSW_PARAM_X64:
		wide->u = scalar->u64;
		break;
	case KFSW_PARAM_I8:
		wide->kind = WIDE_SIGNED;
		/* Widened explicitly: a plain assignment from a signed char is
		 * the shape that usually hides a sign-extension mistake.
		 */
		wide->i = (int64_t)scalar->i8;
		break;
	case KFSW_PARAM_I16:
		wide->kind = WIDE_SIGNED;
		wide->i = scalar->i16;
		break;
	case KFSW_PARAM_I32:
		wide->kind = WIDE_SIGNED;
		wide->i = scalar->i32;
		break;
	case KFSW_PARAM_I64:
		wide->kind = WIDE_SIGNED;
		wide->i = scalar->i64;
		break;
	case KFSW_PARAM_FLOAT:
		wide->kind = WIDE_FLOAT;
		wide->f = scalar->f32;
		break;
	case KFSW_PARAM_DOUBLE:
		wide->kind = WIDE_FLOAT;
		wide->f = scalar->f64;
		break;
	default:
		wide->u = 0U;
		break;
	}
}

static int order(const struct wide *a, const struct wide *b)
{
	switch (a->kind) {
	case WIDE_SIGNED:
		return (a->i > b->i) - (a->i < b->i);
	case WIDE_FLOAT:
		if (isnan(a->f) || isnan(b->f)) {
			return ORDER_NONE;
		}
		return (a->f > b->f) - (a->f < b->f);
	default:
		return (a->u > b->u) - (a->u < b->u);
	}
}

static bool holds(uint8_t cmp, int ordering)
{
	if (ordering == ORDER_NONE) {
		return false;
	}
	switch (cmp) {
	case KFSW_LIMITS_GT:
		return ordering > 0;
	case KFSW_LIMITS_GE:
		return ordering >= 0;
	case KFSW_LIMITS_LT:
		return ordering < 0;
	case KFSW_LIMITS_LE:
		return ordering <= 0;
	case KFSW_LIMITS_EQ:
		return ordering == 0;
	default:
		return ordering != 0;
	}
}

/* Take the next whitespace separated field in place, or NULL at the end. */
static char *next_field(char **cursor)
{
	char *start = *cursor;
	char *end;

	while ((*start == ' ') || (*start == '\t')) {
		start++;
	}
	if (*start == '\0') {
		*cursor = start;
		return NULL;
	}
	end = start;
	while ((*end != '\0') && (*end != ' ') && (*end != '\t')) {
		end++;
	}
	if (*end != '\0') {
		*end++ = '\0';
	}
	*cursor = end;
	return start;
}

static bool parse_unsigned(const char *text, unsigned long maximum, unsigned long *value)
{
	char *end;

	if ((text[0] < '0') || (text[0] > '9')) {
		return false;
	}
	errno = 0;
	*value = strtoul(text, &end, 10);
	return (errno == 0) && (*end == '\0') && (*value <= maximum);
}

/* Refuse at load what would fail at the release: the scheduler resolves the
 * command and parses its arguments when the entry is added, and by then the
 * operator who wrote the rule is long gone.
 */
static const char *check_action(const char *action)
{
	char text[KFSW_FBO_SCHEDULE_LINE_MAX];
	struct kfsw_command_info info;
	struct kfsw_command_arg arg;
	char *cursor = text;
	char *token;

	if (strlen(action) >= sizeof(text)) {
		return "action is too long";
	}
	strcpy(text, action);
	token = next_field(&cursor);
	if ((token == NULL) || (kfsw_command_find(token, &info) != 0)) {
		return "action is not a registered command";
	}
	for (uint8_t index = 0U; index < info.arg_count; index++) {
		token = next_field(&cursor);
		if ((token == NULL) ||
		    (kfsw_command_parse_arg(token, info.arg_types[index], &arg) != 0)) {
			return "action argument is missing or of the wrong type";
		}
	}
	if (next_field(&cursor) != NULL) {
		return "action has more arguments than the command takes";
	}
	return NULL;
}

static const char *parse_comparison(const char *text, struct kfsw_limits_rule *rule)
{
	for (size_t index = 0U; index < ARRAY_SIZE(comparisons); index++) {
		if (strcmp(comparisons[index].text, text) == 0) {
			rule->cmp = comparisons[index].cmp;
			return NULL;
		}
	}
	return "unknown comparison";
}

static const char *parse_read_node(char **name, uint16_t *node)
{
	char *colon = strchr(*name, ':');
	unsigned long number;
	unsigned long maximum = UINT16_MAX;

	if (colon == NULL) {
		return NULL;
	}
	*colon = '\0';
#if CONFIG_KFSW_PARAM_CSP
	maximum = KFSW_CSP_BROADCAST_ADDRESS - 1U;
#endif
	if (!parse_unsigned(*name, maximum, &number) || (colon[1] == '\0') ||
	    (strchr(colon + 1, ':') != NULL)) {
		return "invalid parameter node prefix";
	}
	*name = colon + 1;
#if CONFIG_KFSW_PARAM_CSP
	if (csp_iflist_get_by_addr((uint16_t)number) != NULL) {
		number = 0U;
	}
#else
	if (number != 0U) {
		return "this build has no remote parameters";
	}
#endif
	*node = (uint16_t)number;
	return NULL;
}

static const char *parse_bound(const char *text, struct kfsw_limits_rule *rule)
{
	struct kfsw_param_value value = {.type = rule->type};

	if (is_float(rule->type) &&
	    ((rule->cmp == KFSW_LIMITS_EQ) || (rule->cmp == KFSW_LIMITS_NE))) {
		return "equality is refused for a float parameter";
	}
	if (strlen(text) >= sizeof(rule->bound_text)) {
		return "bound is too long";
	}
	if (rule->read_node != 0U) {
		const enum kfsw_param_type types[] = {KFSW_PARAM_I64, KFSW_PARAM_U64,
						      KFSW_PARAM_DOUBLE};
		for (size_t i = 0U; i < ARRAY_SIZE(types); i++) {
			value.type = types[i];
			if (kfsw_param_parse_text(text, &value) == 0) {
				break;
			}
		}
	}
	if (kfsw_param_parse_text(text, &value) != 0) {
		return "bound does not fit the type of the parameter";
	}
	if (is_float(value.type) &&
	    ((rule->cmp == KFSW_LIMITS_EQ) || (rule->cmp == KFSW_LIMITS_NE))) {
		return "equality is refused for a float parameter";
	}
	rule->bound = value.scalar;
	strcpy(rule->bound_text, text);
	return NULL;
}

/* Returns what is wrong with a line, or NULL once @p rule is filled in. */
static const char *parse_line(char *text, struct kfsw_limits_rule *rule)
{
	static const char *const missing =
		"expected parameter, comparison, bound, dwell, node and an action";
	struct kfsw_param_info info;
	unsigned long number;
	char *cursor = text;
	char *fields[LIMITS_FIELDS];
	char *action;
	char *end;
	const char *reason;

	memset(rule, 0, sizeof(*rule));
	for (size_t index = 0U; index < LIMITS_FIELDS; index++) {
		fields[index] = next_field(&cursor);
		if (fields[index] == NULL) {
			return missing;
		}
	}
	action = cursor;
	while ((*action == ' ') || (*action == '\t')) {
		action++;
	}
	end = action + strlen(action);
	while ((end > action) && ((end[-1] == ' ') || (end[-1] == '\t'))) {
		*--end = '\0';
	}
	if (*action == '\0') {
		return missing;
	}

	reason = parse_read_node(&fields[0], &rule->read_node);
	if (reason != NULL) {
		return reason;
	}
	if (strlen(fields[0]) > KFSW_PARAM_NAME_MAX) {
		return "parameter name is too long";
	}
	rule->type = KFSW_PARAM_INVALID;
	if (rule->read_node == 0U) {
		if (kfsw_param_get_info(fields[0], &info) != 0) {
			return "unknown parameter";
		}
		if (!is_numeric(info.type)) {
			return "parameter is not a number";
		}
		rule->type = info.type;
	}
	strcpy(rule->parameter, fields[0]);

	reason = parse_comparison(fields[1], rule);
	if (reason != NULL) {
		return reason;
	}
	reason = parse_bound(fields[2], rule);
	if (reason != NULL) {
		return reason;
	}

	if (!parse_unsigned(fields[3], KFSW_LIMITS_DWELL_MAX, &number) || (number == 0U)) {
		return "dwell must be 1 to 255";
	}
	rule->dwell = (uint8_t)number;

	if (!parse_unsigned(fields[4], UINT16_MAX, &number)) {
		return "node is not an address";
	}
	rule->node = (uint16_t)number;
#if !CONFIG_KFSW_COMMAND_CSP
	if (rule->node != 0U) {
		return "this build has no remote commanding";
	}
#endif

	reason = check_action(action);
	if (reason != NULL) {
		return reason;
	}
	strcpy(rule->action, action);
	return NULL;
}

/* Read one line into the shared buffer. A line longer than the buffer is
 * consumed whole and reported, never split into two rules.
 */
static int read_line(struct fs_file_t *file, size_t *bytes, bool *at_end, bool *too_long)
{
	size_t used = 0U;

	*too_long = false;
	for (;;) {
		char character;
		ssize_t count = fs_read(file, &character, 1U);

		if (count < 0) {
			return (int)count;
		}
		if (count == 0) {
			*at_end = true;
			break;
		}
		if (++(*bytes) > LIMITS_FILE_BYTES_MAX) {
			return -EFBIG;
		}
		if (character == '\n') {
			break;
		}
		if (character == '\r') {
			continue;
		}
		if (used + 1U >= sizeof(line)) {
			*too_long = true;
			continue;
		}
		line[used++] = character;
	}
	line[used] = '\0';
	return 0;
}

/* Parse the whole file into staged[]. Returns -EINVAL with a reason for a bad
 * line, or another negative errno for a read that failed.
 */
static int parse_file(struct fs_file_t *file, size_t *count, uint16_t *error_line,
		      const char **reason)
{
	size_t bytes = 0U;
	uint16_t number = 0U;
	bool at_end = false;

	*count = 0U;
	*error_line = 0U;
	*reason = NULL;
	while (!at_end) {
		bool too_long;
		char *text = line;
		int result = read_line(file, &bytes, &at_end, &too_long);

		if (result != 0) {
			return result;
		}
		number++;
		while ((*text == ' ') || (*text == '\t')) {
			text++;
		}
		if ((*text == '#') || ((*text == '\0') && !too_long)) {
			continue;
		}
		if (too_long) {
			*reason = "line is too long";
		} else if (*count == ARRAY_SIZE(staged)) {
			*reason = "more rules than this build holds";
		} else {
			*reason = parse_line(text, &staged[*count]);
		}
		if (*reason != NULL) {
			*error_line = number;
			return -EINVAL;
		}
		staged[*count].index = (uint8_t)*count;
		(*count)++;
	}
	return 0;
}

int kfsw_limits_load(const char *name)
{
	char path[96];
	struct fs_file_t file;
	const char *reason = NULL;
	uint16_t error_line = 0U;
	size_t count = 0U;
	int result;

	if ((name == NULL) || (memchr(name, '\0', KFSW_FBO_NAME_MAX) == NULL) ||
	    (name[0] == '\0') || (strchr(name, '/') != NULL) || (strstr(name, "..") != NULL)) {
		return -EINVAL;
	}
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}
	(void)snprintf(path, sizeof(path), "%s/%s/%s", KFSW_FTP_STORAGE_ROOT, KFSW_LIMITS_FTP_PATH,
		       name);

	k_mutex_lock(&rules_lock, K_FOREVER);
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result == 0) {
		result = parse_file(&file, &count, &error_line, &reason);
		(void)fs_close(&file);
	}
	if (result == 0) {
		memcpy(rules, staged, count * sizeof(rules[0]));
		rule_count = count;
#if CONFIG_KFSW_PARAM_CSP
		remote_resume = 0U;
#endif
		k_mutex_lock(&state_lock, K_FOREVER);
		state.failed_by = 0U;
		k_mutex_unlock(&state_lock);
	}
	k_mutex_unlock(&rules_lock);

	if (result == 0) {
		k_mutex_lock(&state_lock, K_FOREVER);
		state.rules = (uint8_t)count;
		state.error_line = 0U;
		state.error = "";
		k_mutex_unlock(&state_lock);
		kfsw_log_info("Limits: %u rules loaded from %s", (unsigned int)count, name);
	} else if (reason != NULL) {
		k_mutex_lock(&state_lock, K_FOREVER);
		saturating_increment(&state.parse_errors);
		state.error_line = error_line;
		state.error = reason;
		k_mutex_unlock(&state_lock);
		kfsw_log_error("Limits: %s refused, line %u: %s", name, error_line, reason);
	}
	return result;
}

static bool release(const struct kfsw_limits_rule *rule)
{
	uint8_t payload[3];
	uint16_t slot;
	int result = kfsw_fbo_schedule_add(KFSW_FBO_TIME_RELATIVE, LIMITS_RELEASE_DELAY_S,
					   rule->node, rule->action, &slot);

	/* An identical entry still waiting stands for this release. */
	if (result == -EEXIST) {
		return true;
	}
	if (result != 0) {
		note_failure();
		kfsw_log_warning("Limits: rule %u could not release '%s' (%d)", rule->index,
				 rule->action, result);
		return false;
	}

	k_mutex_lock(&state_lock, K_FOREVER);
	saturating_increment(&state.fires);
	state.last_fired_index = rule->index;
	state.last_fired_ms = (uint32_t)MIN(kfsw_time_monotonic_ms(), (uint64_t)UINT32_MAX);
	k_mutex_unlock(&state_lock);

	kfsw_log_warning("Limits: rule %u fired, %s %s %s, releasing '%s' on node %u", rule->index,
			 rule->parameter, kfsw_limits_cmp_text(rule->cmp), rule->bound_text,
			 rule->action, rule->node);
	payload[0] = rule->index;
	sys_put_be16(rule->node, &payload[1]);
	kfsw_event_emit(KFSW_EVENT_SOURCE_LIMITS, KFSW_EVENT_LIMITS_FIRED, KFSW_EVENT_WARNING,
			payload, sizeof(payload));
	return true;
}

static void apply_reading(struct kfsw_limits_rule *rule, const struct kfsw_param_value *value)
{
	struct wide current;
	struct wide bound;

	widen(rule->type, &value->scalar, &current);
	widen(rule->type, &rule->bound, &bound);
	if (!holds(rule->cmp, order(&current, &bound))) {
		rule->streak = 0U;
		rule->latched = false;
		return;
	}

	if (rule->streak < UINT8_MAX) {
		rule->streak++;
	}
	if (!rule->latched && (rule->streak >= rule->dwell)) {
		rule->latched = release(rule);
	}
}

static void evaluate(struct kfsw_limits_rule *rule)
{
	struct kfsw_param_value value;
	int result = kfsw_param_get(rule->parameter, &value);

	/* No local reading says nothing about the comparison, so streak and latch stay. */
	if (result != 0) {
		note_failure();
		kfsw_log_warning("Limits: rule %u cannot read %s (%d)", rule->index,
				 rule->parameter, result);
		return;
	}
	apply_reading(rule, &value);
}

#if CONFIG_KFSW_PARAM_CSP
static void apply_remote_reading(struct kfsw_limits_rule *rule,
				 const struct kfsw_param_value *value)
{
	struct kfsw_param_value bound = {.type = value->type};

	if ((rule->type != value->type) || !is_numeric(value->type)) {
		rule->type = KFSW_PARAM_INVALID;
		if (!is_numeric(value->type) ||
		    (is_float(value->type) &&
		     ((rule->cmp == KFSW_LIMITS_EQ) || (rule->cmp == KFSW_LIMITS_NE))) ||
		    (kfsw_param_parse_text(rule->bound_text, &bound) != 0)) {
			rule->streak = 0U;
			note_failure();
			kfsw_log_warning("Limits: rule %u cannot bind %u:%s to bound %s",
					 rule->index, rule->read_node, rule->parameter,
					 rule->bound_text);
			return;
		}
		rule->bound = bound.scalar;
		rule->type = value->type;
	}
	apply_reading(rule, value);
}

static size_t pending_groups(void)
{
	size_t groups = 0U;

	for (size_t i = 0U; i < rule_count; i++) {
		if (done[i]) {
			continue;
		}
		size_t preceding = 0U;
		for (size_t j = 0U; j < i; j++) {
			if (!done[j] && (rules[j].read_node == rules[i].read_node)) {
				preceding++;
			}
		}
		if ((preceding % LIMITS_REMOTE_GROUP_MAX) == 0U) {
			groups++;
		}
	}
	return groups;
}

static void read_group(uint16_t node, size_t count, int64_t deadline)
{
	int result = kfsw_param_remote_get_many_until(node, names, count, values, deadline);

	k_mutex_lock(&state_lock, K_FOREVER);
	if (result != 0) {
		state.failed_by = node;
		for (size_t i = 0U; i < count; i++) {
			saturating_increment(&state.failures);
			saturating_increment(&state.remote_failures);
		}
	} else if (state.failed_by == node) {
		state.failed_by = 0U;
	}
	k_mutex_unlock(&state_lock);
	if (result != 0) {
		kfsw_log_warning("Limits: cannot read node %u, %u rules (%d)", node,
				 (unsigned int)count, result);
	}
	for (size_t i = 0U; i < count; i++) {
		struct kfsw_limits_rule *rule = &rules[members[i]];

		if (result != 0) {
			rule->streak = 0U;
		} else {
			apply_remote_reading(rule, &values[i]);
		}
	}
}

/* rules_lock protects the scratch buffers and resume position across blocking reads. */
static size_t evaluate_remote(void)
{
	uint32_t interval = kfsw_limits_interval_ms;
	uint32_t budget = (interval == 0U) ? LIMITS_REMOTE_BUDGET_MS
					   : MIN(interval / 2U, LIMITS_REMOTE_BUDGET_MS);
	int64_t deadline = k_uptime_get() + budget;
	size_t evaluated = 0U;
	size_t groups;

	while ((groups = pending_groups()) != 0U) {
		int64_t now = k_uptime_get();
		int64_t left = deadline - now;
		size_t first = remote_resume;
		size_t count = 0U;

		while (done[first]) {
			first = (first + 1U) % rule_count;
		}
		remote_resume = first;
		if (left < LIMITS_REMOTE_MIN_MS) {
			kfsw_log_warning("Limits: remote budget exhausted, deferring rules");
			break;
		}
		uint16_t node = rules[first].read_node;
		for (size_t i = 0U; (i < rule_count) && (count < ARRAY_SIZE(names)); i++) {
			size_t index = (first + i) % rule_count;

			if (!done[index] && (rules[index].read_node == node)) {
				names[count] = rules[index].parameter;
				members[count++] = index;
				done[index] = true;
			}
		}
		int64_t share = MAX(left / (int64_t)groups, LIMITS_REMOTE_MIN_MS);
		read_group(node, count, now + share);
		evaluated += count;
	}
	return evaluated;
}
#endif

int kfsw_limits_sweep(void)
{
	size_t evaluated = 0U;

	k_mutex_lock(&rules_lock, K_FOREVER);
	if (!kfsw_limits_enabled) {
		/* Dwell counts consecutive sweeps, and a disabled one breaks the run. */
		for (size_t index = 0U; index < rule_count; index++) {
			rules[index].streak = 0U;
		}
		k_mutex_unlock(&rules_lock);
		return -EACCES;
	}
	for (size_t index = 0U; index < rule_count; index++) {
#if CONFIG_KFSW_PARAM_CSP
		done[index] = (rules[index].read_node == 0U);
#endif
		if (rules[index].read_node == 0U) {
			evaluate(&rules[index]);
			evaluated++;
		}
	}
#if CONFIG_KFSW_PARAM_CSP
	evaluated += evaluate_remote();
#endif
	k_mutex_unlock(&rules_lock);

	k_mutex_lock(&state_lock, K_FOREVER);
	saturating_increment(&state.sweeps);
	k_mutex_unlock(&state_lock);
	return (int)evaluated;
}

static void work_handler(struct k_work *work)
{
	bool again;

	ARG_UNUSED(work);

	(void)kfsw_limits_sweep();

	k_mutex_lock(&state_lock, K_FOREVER);
	again = state.running;
	k_mutex_unlock(&state_lock);
	if (again) {
		(void)k_work_reschedule_for_queue(&limits_queue, &limits_work,
						  K_MSEC(kfsw_limits_interval_ms));
	}
}

int kfsw_limits_set_interval_ms(uint32_t interval_ms)
{
	bool active;

	if ((interval_ms != 0U) && ((interval_ms < KFSW_LIMITS_INTERVAL_MIN_MS) ||
				    (interval_ms > KFSW_LIMITS_INTERVAL_MAX_MS))) {
		return -ERANGE;
	}

	kfsw_limits_interval_ms = interval_ms;
	k_mutex_lock(&state_lock, K_FOREVER);
	active = initialized;
	if (active) {
		state.running = (interval_ms != 0U);
	}
	k_mutex_unlock(&state_lock);

	if (!active) {
		return 0;
	}
	if (interval_ms != 0U) {
		(void)k_work_reschedule_for_queue(&limits_queue, &limits_work, K_MSEC(interval_ms));
	} else {
		(void)k_work_cancel_delayable(&limits_work);
	}
	return 0;
}

int kfsw_limits_init(void)
{
	bool first;
	int result;

	k_mutex_lock(&state_lock, K_FOREVER);
	first = !initialized;
	if (first) {
		k_work_queue_start(&limits_queue, limits_stack, K_THREAD_STACK_SIZEOF(limits_stack),
				   CONFIG_KFSW_LIMITS_PRIORITY, NULL);
		initialized = true;
	}
	k_mutex_unlock(&state_lock);
	if (!first) {
		return 0;
	}

	/* Same reason as the procedures directory: a rule file has to be able to
	 * arrive before anybody can load one.
	 */
	if (kfsw_storage_is_ready()) {
		char directory[64];

		(void)snprintf(directory, sizeof(directory), "%s/%s", KFSW_FTP_STORAGE_ROOT,
			       KFSW_LIMITS_FTP_PATH);
		result = fs_mkdir(KFSW_FTP_STORAGE_ROOT);
		if ((result == 0) || (result == -EEXIST)) {
			result = fs_mkdir(directory);
		}
		if ((result != 0) && (result != -EEXIST)) {
			kfsw_log_warning("Limits: %s is missing (%d); no rule file can be uploaded",
					 directory, result);
		}
	}

	/* A restored interval that is out of range gives way to the compiled one. */
	if (kfsw_limits_set_interval_ms(kfsw_limits_interval_ms) != 0) {
		(void)kfsw_limits_set_interval_ms(CONFIG_KFSW_LIMITS_INTERVAL_MS);
	}
	kfsw_log_info("Limits: ready, sweeping every %u ms", kfsw_limits_interval_ms);

	result = kfsw_limits_load(CONFIG_KFSW_LIMITS_FILE);
	if ((result == -ENOENT) || (result == -ENODEV)) {
		kfsw_log_info("Limits: no rule file %s, no rules in force",
			      CONFIG_KFSW_LIMITS_FILE);
		return 0;
	}
	return result;
}

void kfsw_limits_get_status(struct kfsw_limits_status *status)
{
	if (status == NULL) {
		return;
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	*status = state;
	k_mutex_unlock(&state_lock);
}

void kfsw_limits_visit_rules(kfsw_limits_visitor_t visitor, void *context)
{
	if (visitor == NULL) {
		return;
	}
	k_mutex_lock(&rules_lock, K_FOREVER);
	for (size_t index = 0U; index < rule_count; index++) {
		if (!visitor(&rules[index], context)) {
			break;
		}
	}
	k_mutex_unlock(&rules_lock);
}
