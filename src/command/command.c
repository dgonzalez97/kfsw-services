#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/command.h>
#if CONFIG_KFSW_EVENT
#include <kfsw/services/event.h>
#endif

#include "command_internal.h"

/*
 * The registry. Definitions are contributed by their owning component as
 * compile-time sets, validated once, and frozen. There is no runtime
 * registration: a command that exists after startup existed at build time.
 */

static const struct kfsw_command_definition *registry[CONFIG_KFSW_COMMAND_MAX_COMMANDS];
static size_t registry_count;
static bool registry_ready;

/* Serializes invocation so a handler never runs concurrently with itself. */
K_MUTEX_DEFINE(command_lock);

/* Lifetime totals. Outcomes were recorded as events and counted nowhere, which
 * answers "what happened" but not "how often"; a pass is too short to read a
 * ring when a number would do.
 */
static atomic_t command_invoked;
static atomic_t command_failed;
static atomic_t command_unknown;
static atomic_t command_rejected;

#if CONFIG_KFSW_COMMAND_CSP
/* Applied to the next remote invocation. A slow link is exactly when a caller
 * needs to raise it, and exactly when it cannot rebuild the image. Only exists
 * with the remote path: without it there is no invocation to time out. */
static uint32_t command_timeout_ms = CONFIG_KFSW_COMMAND_TIMEOUT_MS;
#endif

/*
 * Console echo, off by default. The shell prints every input byte back, so a
 * session driven by a script shows each command twice: once as the sender
 * typed it and once as the shell repeated it. That is noise on a console and
 * unreadable in a recording.
 *
 * The console belongs to the composition, not to this service, so applying the
 * change is handed back through a hook the composition fills in. Without that
 * the value could only take effect at the next boot, and a setting you want to
 * toggle while watching the console is exactly the wrong one to make a reboot
 * of.
 */
static uint8_t command_echo_enabled;
static kfsw_command_echo_handler_t command_echo_handler;

static const struct kfsw_command_definition *find_by_id(uint16_t id)
{
	for (size_t index = 0U; index < registry_count; index++) {
		if (registry[index]->id == id) {
			return registry[index];
		}
	}
	return NULL;
}

static const struct kfsw_command_definition *find_by_name(const char *name)
{
	for (size_t index = 0U; index < registry_count; index++) {
		if (strcmp(registry[index]->name, name) == 0) {
			return registry[index];
		}
	}
	return NULL;
}

static int validate_definition(const struct kfsw_command_definition *definition)
{
	if ((definition->name == NULL) || (definition->name[0] == '\0') ||
	    (definition->handler == NULL)) {
		return -EINVAL;
	}
	if (definition->arg_count > KFSW_COMMAND_MAX_ARGS) {
		return -E2BIG;
	}
	if ((definition->arg_count != 0U) && (definition->arg_types == NULL)) {
		return -EINVAL;
	}
	/* Bounded check without strnlen, which the minimal libc does not provide. */
	if (memchr(definition->name, '\0', KFSW_COMMAND_MAX_TEXT_SIZE) == NULL) {
		return -ENAMETOOLONG;
	}
	/* A duplicate id or name would make dispatch ambiguous between front ends. */
	if ((find_by_id(definition->id) != NULL) || (find_by_name(definition->name) != NULL)) {
		return -EEXIST;
	}
	return 0;
}

int kfsw_command_init(const struct kfsw_command_definition_set *const *sets, size_t set_count)
{
	if ((sets == NULL) && (set_count != 0U)) {
		return -EINVAL;
	}
	registry_count = 0U;
	registry_ready = false;

	for (size_t set_index = 0U; set_index < set_count; set_index++) {
		const struct kfsw_command_definition_set *set = sets[set_index];

		if (set == NULL) {
			return -EINVAL;
		}
		if ((set->count != 0U) && (set->commands == NULL)) {
			return -EINVAL;
		}
		for (size_t index = 0U; index < set->count; index++) {
			const struct kfsw_command_definition *definition = &set->commands[index];
			int result = validate_definition(definition);

			if (result != 0) {
				registry_count = 0U;
				return result;
			}
			if (registry_count == ARRAY_SIZE(registry)) {
				registry_count = 0U;
				return -ENOSPC;
			}
			registry[registry_count] = definition;
			registry_count++;
		}
	}
	registry_ready = true;
	return 0;
}

bool kfsw_command_is_initialized(void)
{
	return registry_ready;
}

static void describe(const struct kfsw_command_definition *definition,
		     struct kfsw_command_info *info)
{
	info->id = definition->id;
	info->name = definition->name;
	info->help = definition->help;
	info->flags = definition->flags;
	info->arg_count = definition->arg_count;
	info->arg_types = definition->arg_types;
}

void kfsw_command_visit(kfsw_command_visitor_t visitor, void *context)
{
	if ((visitor == NULL) || !registry_ready) {
		return;
	}
	for (size_t index = 0U; index < registry_count; index++) {
		struct kfsw_command_info info;

		describe(registry[index], &info);
		if (!visitor(&info, context)) {
			return;
		}
	}
}

int kfsw_command_find(const char *name, struct kfsw_command_info *info)
{
	const struct kfsw_command_definition *definition;

	if ((name == NULL) || (info == NULL)) {
		return -EINVAL;
	}
	if (!registry_ready) {
		return -EACCES;
	}
	definition = find_by_name(name);
	if (definition == NULL) {
		return -ENOENT;
	}
	describe(definition, info);
	return 0;
}

/*
 * Common validation runs before any handler sees a request, so a malformed
 * request cannot partially apply.
 */
static enum kfsw_command_status check_arguments(const struct kfsw_command_definition *definition,
						const struct kfsw_command_arg *args,
						size_t arg_count)
{
	if (arg_count != definition->arg_count) {
		return KFSW_COMMAND_INVALID_ARGUMENT;
	}
	for (size_t index = 0U; index < arg_count; index++) {
		if (args[index].type != definition->arg_types[index]) {
			return KFSW_COMMAND_INVALID_ARGUMENT;
		}
		if ((args[index].type == KFSW_COMMAND_TYPE_TEXT) &&
		    (args[index].value.text == NULL)) {
			return KFSW_COMMAND_INVALID_ARGUMENT;
		}
	}
	return KFSW_COMMAND_OK;
}

/*
 * Every dispatch outcome is recorded, so what a remote caller did remains
 * answerable after the console has scrolled away.
 */
static void record_outcome(uint16_t event_id, uint16_t command_id,
			   const struct kfsw_command_source *source,
			   enum kfsw_command_status status)
{
	/* Outside the event guard: a composition without the event record still
	 * needs to know how many commands have run and how many were refused.
	 * Every outcome passes through here, which is why it is the one place
	 * worth counting. */
	(void)atomic_inc(&command_invoked);
	if (status == KFSW_COMMAND_UNKNOWN) {
		(void)atomic_inc(&command_unknown);
	} else if (status == KFSW_COMMAND_DENIED) {
		(void)atomic_inc(&command_rejected);
	} else if (status != KFSW_COMMAND_OK) {
		(void)atomic_inc(&command_failed);
	}

#if CONFIG_KFSW_EVENT
	uint8_t payload[5];

	sys_put_be16(command_id, &payload[0]);
	sys_put_be16(source->node, &payload[2]);
	payload[4] = (uint8_t)status;
	kfsw_event_emit(KFSW_EVENT_SOURCE_COMMAND, event_id,
			(status == KFSW_COMMAND_OK) ? KFSW_EVENT_INFO : KFSW_EVENT_WARNING, payload,
			sizeof(payload));
#else
	ARG_UNUSED(event_id);
	ARG_UNUSED(command_id);
	ARG_UNUSED(source);
	ARG_UNUSED(status);
#endif
}

static int dispatch(const struct kfsw_command_definition *definition,
		    const struct kfsw_command_arg *args, size_t arg_count,
		    const struct kfsw_command_source *source, struct kfsw_command_result *result)
{
	int handler_result;

	memset(result, 0, sizeof(*result));
	if (!registry_ready) {
		result->status = KFSW_COMMAND_UNAVAILABLE;
		return -EACCES;
	}
	if (definition == NULL) {
		result->status = KFSW_COMMAND_UNKNOWN;
		record_outcome(KFSW_EVENT_COMMAND_UNKNOWN, 0U, source, result->status);
		return -ENOENT;
	}
	result->status = check_arguments(definition, args, arg_count);
	if (result->status != KFSW_COMMAND_OK) {
		record_outcome(KFSW_EVENT_COMMAND_REJECTED, definition->id, source, result->status);
		return -EINVAL;
	}

	k_mutex_lock(&command_lock, K_FOREVER);
	handler_result = definition->handler(args, arg_count, source, result);
	k_mutex_unlock(&command_lock);

	/* A handler that fails without saying how still reports a usable status. */
	if ((handler_result != 0) && (result->status == KFSW_COMMAND_OK)) {
		result->status = KFSW_COMMAND_FAILED;
	}
	record_outcome(KFSW_EVENT_COMMAND_INVOKED, definition->id, source, result->status);
	return handler_result;
}

int kfsw_command_invoke(const char *name, const struct kfsw_command_arg *args, size_t arg_count,
			struct kfsw_command_result *result)
{
	const struct kfsw_command_source source = {.node = 0U, .authenticated = false};

	if ((name == NULL) || (result == NULL)) {
		return -EINVAL;
	}
	return dispatch(find_by_name(name), args, arg_count, &source, result);
}

int kfsw_command_invoke_id(uint16_t id, const struct kfsw_command_arg *args, size_t arg_count,
			   const struct kfsw_command_source *source,
			   struct kfsw_command_result *result)
{
	const struct kfsw_command_source local = {.node = 0U, .authenticated = false};

	if (result == NULL) {
		return -EINVAL;
	}
	return dispatch(find_by_id(id), args, arg_count, (source != NULL) ? source : &local,
			result);
}

int kfsw_command_lookup_id(const char *name, uint16_t *id)
{
	const struct kfsw_command_definition *definition;

	if ((name == NULL) || (id == NULL)) {
		return -EINVAL;
	}
	if (!registry_ready) {
		return -EACCES;
	}
	definition = find_by_name(name);
	if (definition == NULL) {
		return -ENOENT;
	}
	*id = definition->id;
	return 0;
}

int kfsw_command_get_stats(struct kfsw_command_stats *stats)
{
	if (stats == NULL) {
		return -EINVAL;
	}

	stats->invoked = (uint32_t)atomic_get(&command_invoked);
	stats->failed = (uint32_t)atomic_get(&command_failed);
	stats->unknown = (uint32_t)atomic_get(&command_unknown);
	stats->rejected = (uint32_t)atomic_get(&command_rejected);
	stats->registered = (uint16_t)registry_count;
	return 0;
}

#if CONFIG_KFSW_COMMAND_CSP
uint32_t kfsw_command_get_timeout_ms(void)
{
	return command_timeout_ms;
}

int kfsw_command_check_timeout_ms(uint32_t timeout_ms)
{
	/* Matches the Kconfig range, so a value accepted here is one the
	 * composition could have been built with. Separate from applying it
	 * because a change callback cannot refuse. */
	if ((timeout_ms < KFSW_COMMAND_TIMEOUT_MIN_MS) ||
	    (timeout_ms > KFSW_COMMAND_TIMEOUT_MAX_MS)) {
		return -ERANGE;
	}
	return 0;
}

int kfsw_command_set_timeout_ms(uint32_t timeout_ms)
{
	int result = kfsw_command_check_timeout_ms(timeout_ms);

	if (result != 0) {
		return result;
	}
	command_timeout_ms = timeout_ms;
	return 0;
}
#endif

bool kfsw_command_echo_enabled(void)
{
	return command_echo_enabled != 0U;
}

void kfsw_command_set_echo_handler(kfsw_command_echo_handler_t handler)
{
	command_echo_handler = handler;

	/* Applied as soon as a console exists, so the default reaches the shell
	 * without waiting for somebody to write the parameter. */
	if (handler != NULL) {
		handler(command_echo_enabled != 0U);
	}
}

void kfsw_command_set_echo(bool enabled)
{
	command_echo_enabled = enabled ? 1U : 0U;
	if (command_echo_handler != NULL) {
		command_echo_handler(enabled);
	}
}

const char *kfsw_command_status_name(enum kfsw_command_status status)
{
	switch (status) {
	case KFSW_COMMAND_OK:
		return "ok";
	case KFSW_COMMAND_UNKNOWN:
		return "unknown command";
	case KFSW_COMMAND_INVALID_ARGUMENT:
		return "invalid argument";
	case KFSW_COMMAND_DENIED:
		return "denied";
	case KFSW_COMMAND_BUSY:
		return "busy";
	case KFSW_COMMAND_UNAVAILABLE:
		return "service unavailable";
	case KFSW_COMMAND_FAILED:
	default:
		return "failed";
	}
}
