#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/command.h>
#include <kfsw/services/fbo.h>

#include "fbo_internal.h"

/*
 * The ground reaches procedures and the queue through the command registry,
 * the same path the shell uses, so there is one set of rules for both.
 */

/* `+N` is N seconds from now, `@N` is the UTC second N. A bare number is
 * refused: the two are not interchangeable, and guessing picks the dangerous
 * one.
 */
static int parse_when(const char *text, enum kfsw_fbo_time_kind *kind, int64_t *when,
		      struct kfsw_command_result *result)
{
	char *end;
	long long parsed;

	if ((text[0] != '+') && (text[0] != '@')) {
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
		(void)snprintf(result->detail, sizeof(result->detail),
			       "write +seconds from now or @utc second");
		return -EINVAL;
	}
	*kind = (text[0] == '+') ? KFSW_FBO_TIME_RELATIVE : KFSW_FBO_TIME_ABSOLUTE;
	parsed = strtoll(&text[1], &end, 10);
	if ((end == &text[1]) || (*end != '\0') || (parsed <= 0)) {
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
		return -EINVAL;
	}
	*when = (int64_t)parsed;
	return 0;
}

static void say(struct kfsw_command_result *result, int outcome, const char *reason)
{
	switch (outcome) {
	case -ENODATA:
		result->status = KFSW_COMMAND_UNAVAILABLE;
		(void)snprintf(result->detail, sizeof(result->detail),
			       "the clock is not set, so absolute scheduling is unavailable");
		break;
	case -ENOSPC:
		result->status = KFSW_COMMAND_BUSY;
		(void)snprintf(result->detail, sizeof(result->detail), "the queue is full");
		break;
	case -EACCES:
		result->status = KFSW_COMMAND_DENIED;
		(void)snprintf(result->detail, sizeof(result->detail), "%s", reason);
		break;
	case -EALREADY:
		result->status = KFSW_COMMAND_BUSY;
		(void)snprintf(result->detail, sizeof(result->detail),
			       "the entry has already run");
		break;
	case -ENOENT:
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
		(void)snprintf(result->detail, sizeof(result->detail), "%s", reason);
		break;
	default:
		result->status = KFSW_COMMAND_FAILED;
		(void)snprintf(result->detail, sizeof(result->detail), "%s (%d)", reason, outcome);
		break;
	}
}

static int command_fbo_run(const struct kfsw_command_arg *args, size_t count,
			   const struct kfsw_command_source *source,
			   struct kfsw_command_result *result)
{
	int outcome;

	ARG_UNUSED(count);
	ARG_UNUSED(source);

	outcome = kfsw_fbo_run(args[0].value.text);
	if (outcome == -EBUSY) {
		result->status = KFSW_COMMAND_BUSY;
		(void)snprintf(result->detail, sizeof(result->detail),
			       "a procedure is already running");
		return outcome;
	}
	if (outcome != 0) {
		say(result, outcome, "the procedure could not be started");
		return outcome;
	}
	result->status = KFSW_COMMAND_OK;
	(void)snprintf(result->detail, sizeof(result->detail), "started=%s",
		       args[0].value.text);
	return 0;
}

static int command_sched_add(const struct kfsw_command_arg *args, size_t count,
			     const struct kfsw_command_source *source,
			     struct kfsw_command_result *result)
{
	struct kfsw_fbo_schedule_status status;
	enum kfsw_fbo_time_kind kind;
	int64_t when;
	uint16_t index = 0U;
	int outcome;

	ARG_UNUSED(count);
	ARG_UNUSED(source);

	outcome = parse_when(args[0].value.text, &kind, &when, result);
	if (outcome != 0) {
		return outcome;
	}
	if (args[1].value.u32 > UINT16_MAX) {
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
		return -EINVAL;
	}
	outcome = kfsw_fbo_schedule_add(kind, when, (uint16_t)args[1].value.u32,
					args[2].value.text, &index);
	if (outcome == -EEXIST) {
		/* The same content is already queued, so the upload was a repeat
		 * and the entry it names is the one that will run.
		 */
		result->status = KFSW_COMMAND_OK;
		(void)snprintf(result->detail, sizeof(result->detail), "index=%u repeated=yes",
			       index);
		return 0;
	}
	if (outcome != 0) {
		say(result, outcome, "the entry was refused");
		return outcome;
	}
	(void)kfsw_fbo_schedule_get_status(&status);
	result->status = KFSW_COMMAND_OK;
	(void)snprintf(result->detail, sizeof(result->detail), "index=%u queue=0x%08x", index,
		       status.hash);
	return 0;
}

static int command_sched_cancel(const struct kfsw_command_arg *args, size_t count,
				const struct kfsw_command_source *source,
				struct kfsw_command_result *result)
{
	int outcome;

	ARG_UNUSED(count);
	ARG_UNUSED(source);

	if (args[0].value.u32 > UINT16_MAX) {
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
		return -EINVAL;
	}
	outcome = kfsw_fbo_schedule_cancel((uint16_t)args[0].value.u32);
	if (outcome != 0) {
		say(result, outcome, "no such entry");
		return outcome;
	}
	result->status = KFSW_COMMAND_OK;
	(void)snprintf(result->detail, sizeof(result->detail), "cancelled=%u",
		       args[0].value.u32);
	return 0;
}

static int command_sched_clear(const struct kfsw_command_arg *args, size_t count,
			       const struct kfsw_command_source *source,
			       struct kfsw_command_result *result)
{
	ARG_UNUSED(args);
	ARG_UNUSED(count);
	ARG_UNUSED(source);

	result->status = KFSW_COMMAND_OK;
	(void)snprintf(result->detail, sizeof(result->detail), "dropped=%d",
		       kfsw_fbo_schedule_clear());
	return 0;
}

static const enum kfsw_command_type run_args[] = {KFSW_COMMAND_TYPE_TEXT};
static const enum kfsw_command_type add_args[] = {KFSW_COMMAND_TYPE_TEXT, KFSW_COMMAND_TYPE_U32,
						  KFSW_COMMAND_TYPE_TEXT};
static const enum kfsw_command_type cancel_args[] = {KFSW_COMMAND_TYPE_U32};

static const struct kfsw_command_definition fbo_commands[] = {
	{
		.id = KFSW_COMMAND_ID_FBO_RUN,
		.name = "fbo_run",
		.help = "Carry out a procedure: fbo_run <name>.",
		.flags = KFSW_COMMAND_FLAG_MUTATING,
		.arg_count = 1U,
		.arg_types = run_args,
		.handler = command_fbo_run,
	},
	{
		.id = KFSW_COMMAND_ID_FBO_SCHED_ADD,
		.name = "fbo_sched_add",
		.help = "Release a command later: fbo_sched_add <+s|@utc> <node> \"<command>\".",
		.flags = KFSW_COMMAND_FLAG_MUTATING,
		.arg_count = 3U,
		.arg_types = add_args,
		.handler = command_sched_add,
	},
	{
		.id = KFSW_COMMAND_ID_FBO_SCHED_CANCEL,
		.name = "fbo_sched_cancel",
		.help = "Cancel a scheduled entry: fbo_sched_cancel <index>.",
		.flags = KFSW_COMMAND_FLAG_MUTATING,
		.arg_count = 1U,
		.arg_types = cancel_args,
		.handler = command_sched_cancel,
	},
	{
		.id = KFSW_COMMAND_ID_FBO_SCHED_CLEAR,
		.name = "fbo_sched_clear",
		.help = "Empty the queue; a release in flight finishes.",
		.flags = KFSW_COMMAND_FLAG_MUTATING,
		.handler = command_sched_clear,
	},
};

const struct kfsw_command_definition_set kfsw_fbo_command_definitions = {
	.commands = fbo_commands,
	.count = ARRAY_SIZE(fbo_commands),
};
