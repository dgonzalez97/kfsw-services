#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#define KFSW_LOG_MODULE KFSW_LOG_MODULE_COMMAND
#include <kfsw/services/log.h>
#include <kfsw/services/remexec.h>

#include "remexec_internal.h"

/*
 * The allowlist, the output cap and the counters. Execution goes through a
 * shell instance of this service's own, whose transport counts every byte a
 * command prints and keeps the first KFSW_REMEXEC_OUTPUT_MAX of them. Zephyr's
 * dummy backend would be the obvious fixture, but it discards what overflows
 * without saying so, which is the one thing a reply here must never do.
 */

BUILD_ASSERT(KFSW_REMEXEC_COMMAND_MAX < CONFIG_SHELL_CMD_BUFF_SIZE,
	     "A command line must fit the shell command buffer");

/** Printable ASCII only: a control byte in a command line is a mistake. */
#define REMEXEC_FIRST_PRINTABLE 0x20
#define REMEXEC_LAST_PRINTABLE 0x7e

static struct {
	const struct kfsw_remexec_entry *entries;
	size_t count;
	bool initialized;
	struct kfsw_remexec_stats stats;
	char last_refusal[KFSW_REMEXEC_REFUSAL_MAX];
} service;

/* One execution at a time; a second request is refused, never queued. */
K_MUTEX_DEFINE(remexec_executor);

static struct {
	/** Bytes the running command printed, whether or not they were kept. */
	size_t written;
	/** Bytes kept in buf. */
	size_t held;
	char buf[KFSW_REMEXEC_OUTPUT_MAX + 1U];
} capture;

static int capture_init(const struct shell_transport *transport, const void *config,
			shell_transport_handler_t handler, void *context)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(config);
	ARG_UNUSED(handler);
	ARG_UNUSED(context);

	return 0;
}

static int capture_uninit(const struct shell_transport *transport)
{
	ARG_UNUSED(transport);

	return 0;
}

static int capture_enable(const struct shell_transport *transport, bool blocking)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(blocking);

	return 0;
}

static int capture_write(const struct shell_transport *transport, const void *data, size_t length,
			 size_t *cnt)
{
	size_t room = KFSW_REMEXEC_OUTPUT_MAX - capture.held;
	size_t kept = MIN(length, room);

	ARG_UNUSED(transport);

	memcpy(&capture.buf[capture.held], data, kept);
	capture.held += kept;
	capture.buf[capture.held] = '\0';
	/* Counted whole, so the reply can say how much went missing. */
	capture.written += length;
	*cnt = length;
	return 0;
}

static int capture_read(const struct shell_transport *transport, void *data, size_t length,
			size_t *cnt)
{
	ARG_UNUSED(transport);
	ARG_UNUSED(data);
	ARG_UNUSED(length);

	/* Nothing ever types at this shell; it exists to be executed against. */
	*cnt = 0;
	return 0;
}

static const struct shell_transport_api capture_transport_api = {
	.init = capture_init,
	.uninit = capture_uninit,
	.enable = capture_enable,
	.write = capture_write,
	.read = capture_read,
};

static struct shell_transport capture_transport = {
	.api = &capture_transport_api,
	.ctx = &capture,
};

/* CRLF_DEFAULT: newlines travel as the command wrote them, so the byte count
 * the reply reports is the byte count the command produced.
 */
SHELL_DEFINE(remexec_shell, "", &capture_transport, 0, 0, SHELL_FLAG_CRLF_DEFAULT);

static void saturating_increment(uint32_t *counter)
{
	if (*counter != UINT32_MAX) {
		(*counter)++;
	}
}

static void reset_reply(struct kfsw_remexec_reply *reply)
{
	reply->status = KFSW_REMEXEC_OK;
	reply->command_result = 0;
	reply->produced = 0U;
	reply->dropped = 0U;
	reply->truncated = false;
	reply->size = 0U;
	reply->text[0] = '\0';
}

/** Append @p text, keeping what fits and counting what does not. */
static void append(struct kfsw_remexec_reply *reply, const char *text)
{
	size_t length = strlen(text);
	size_t room = KFSW_REMEXEC_OUTPUT_MAX - reply->size;
	size_t kept = MIN(length, room);

	memcpy(&reply->text[reply->size], text, kept);
	reply->size = (uint16_t)(reply->size + kept);
	reply->text[reply->size] = '\0';
	reply->produced += (uint32_t)length;
}

static void finish(struct kfsw_remexec_reply *reply)
{
	reply->dropped = reply->produced - (uint32_t)reply->size;
	reply->truncated = (reply->dropped != 0U);
	if (reply->truncated) {
		saturating_increment(&service.stats.truncated);
	}
}

/** Refuse, and leave a reason the operator and the parameter table can read. */
static void refuse(struct kfsw_remexec_reply *reply, enum kfsw_remexec_status status,
		   const char *reason)
{
	reset_reply(reply);
	reply->status = status;
	append(reply, reason);
	finish(reply);
	(void)strncpy(service.last_refusal, reason, sizeof(service.last_refusal) - 1U);
	service.last_refusal[sizeof(service.last_refusal) - 1U] = '\0';
	saturating_increment(&service.stats.refused);
	kfsw_log_warning("Remote execution refused: %s", service.last_refusal);
}

static void refuse_not_offered(struct kfsw_remexec_reply *reply, const char *command)
{
	char reason[KFSW_REMEXEC_REFUSAL_MAX];

	(void)snprintk(reason, sizeof(reason), "'%s' is not offered for remote execution", command);
	refuse(reply, KFSW_REMEXEC_NOT_OFFERED, reason);
}

static bool printable(const char *text)
{
	for (const char *cursor = text; *cursor != '\0'; cursor++) {
		if ((*cursor < REMEXEC_FIRST_PRINTABLE) || (*cursor > REMEXEC_LAST_PRINTABLE)) {
			return false;
		}
	}
	return true;
}

/** Whether @p line is @p command or @p command followed by arguments. */
static bool line_uses(const char *line, const char *command)
{
	size_t length = strlen(command);

	return (strncmp(line, command, length) == 0) &&
	       ((line[length] == '\0') || (line[length] == ' '));
}

static const struct kfsw_remexec_entry *offered(const char *line)
{
	for (size_t index = 0U; index < service.count; index++) {
		if (line_uses(line, service.entries[index].command)) {
			return &service.entries[index];
		}
	}
	return NULL;
}

static int check_entry(const struct kfsw_remexec_entry *entry, size_t index)
{
	if ((entry->command == NULL) || (entry->help == NULL) || (entry->command[0] == '\0') ||
	    (entry->help[0] == '\0') || !printable(entry->command)) {
		return -EINVAL;
	}
	if (strnlen(entry->command, KFSW_REMEXEC_COMMAND_MAX + 1U) > KFSW_REMEXEC_COMMAND_MAX) {
		return -ENAMETOOLONG;
	}
	for (size_t earlier = 0U; earlier < index; earlier++) {
		if (strcmp(service.entries[earlier].command, entry->command) == 0) {
			return -EINVAL;
		}
	}
	return 0;
}

int kfsw_remexec_init(const struct kfsw_remexec_allowlist *allowlist)
{
	static const struct shell_backend_config_flags flags = {
		.insert_mode = 0,
		.echo = 0,
		.obscure = 0,
		.mode_delete = 0,
		/* Escape sequences would be counted as output and confuse a reader. */
		.use_colors = 0,
		.use_vt100 = 0,
	};
	int result;

	if (service.initialized) {
		return -EALREADY;
	}
	service.entries = (allowlist != NULL) ? allowlist->entries : NULL;
	service.count = (allowlist != NULL) ? allowlist->count : 0U;
	if (service.count > (size_t)CONFIG_KFSW_REMEXEC_MAX_ENTRIES) {
		service.entries = NULL;
		service.count = 0U;
		return -E2BIG;
	}
	if ((service.count != 0U) && (service.entries == NULL)) {
		service.count = 0U;
		return -EINVAL;
	}
	for (size_t index = 0U; index < service.count; index++) {
		result = check_entry(&service.entries[index], index);
		if (result != 0) {
			service.entries = NULL;
			service.count = 0U;
			return result;
		}
	}

	result = shell_init(&remexec_shell, NULL, flags, false, 0U);
	if (result != 0) {
		service.entries = NULL;
		service.count = 0U;
		return result;
	}
	/* A shell that is only initialised discards everything a command
	 * prints, so start it here. CONFIG_SHELL_AUTOSTART makes its own
	 * thread race for the same call; -ENOTSUP means that thread won.
	 */
	result = shell_start(&remexec_shell);
	if ((result != 0) && (result != -ENOTSUP)) {
		service.entries = NULL;
		service.count = 0U;
		return result;
	}
	if (!shell_ready(&remexec_shell)) {
		service.entries = NULL;
		service.count = 0U;
		return -EAGAIN;
	}

	service.stats.entries = (uint16_t)service.count;
	service.initialized = true;
	kfsw_log_info("Remote execution offers %u command(s)", (unsigned int)service.count);
	return 0;
}

bool kfsw_remexec_is_initialized(void)
{
	return service.initialized;
}

void kfsw_remexec_get_stats(struct kfsw_remexec_stats *stats)
{
	if (stats != NULL) {
		*stats = service.stats;
	}
}

const char *kfsw_remexec_last_refusal(void)
{
	return service.last_refusal;
}

const char *kfsw_remexec_status_name(enum kfsw_remexec_status status)
{
	switch (status) {
	case KFSW_REMEXEC_OK:
		return "ok";
	case KFSW_REMEXEC_NOT_OFFERED:
		return "not offered";
	case KFSW_REMEXEC_TOO_LONG:
		return "too long";
	case KFSW_REMEXEC_INVALID:
		return "invalid";
	case KFSW_REMEXEC_BUSY:
		return "busy";
	case KFSW_REMEXEC_TIMEOUT:
		return "timeout";
	case KFSW_REMEXEC_FAILED:
		return "failed";
	case KFSW_REMEXEC_UNAVAILABLE:
		return "unavailable";
	default:
		return "unknown";
	}
}

/** Whether @p entry is @p prefix itself or one of its subcommands. */
static bool under_prefix(const struct kfsw_remexec_entry *entry, const char *prefix)
{
	size_t length = strlen(prefix);

	return (strncmp(entry->command, prefix, length) == 0) &&
	       ((entry->command[length] == '\0') || (entry->command[length] == ' '));
}

void kfsw_remexec_list_local(const char *prefix, struct kfsw_remexec_reply *reply)
{
	bool narrowed = (prefix != NULL) && (prefix[0] != '\0');
	size_t matches = 0U;

	reset_reply(reply);
	if (!service.initialized) {
		refuse(reply, KFSW_REMEXEC_UNAVAILABLE, "remote execution is not initialised");
		return;
	}
	if (narrowed && (!printable(prefix) ||
			 (strnlen(prefix, KFSW_REMEXEC_COMMAND_MAX + 1U) >
			  KFSW_REMEXEC_COMMAND_MAX))) {
		refuse(reply, KFSW_REMEXEC_INVALID, "the requested command is not a valid name");
		return;
	}

	for (size_t index = 0U; index < service.count; index++) {
		const struct kfsw_remexec_entry *entry = &service.entries[index];
		char line[KFSW_REMEXEC_COMMAND_MAX + KFSW_REMEXEC_REFUSAL_MAX];

		if (narrowed && !under_prefix(entry, prefix)) {
			continue;
		}
		matches++;
		if (narrowed) {
			(void)snprintk(line, sizeof(line), "%s\t%s\n", entry->command, entry->help);
		} else {
			(void)snprintk(line, sizeof(line), "%s\n", entry->command);
		}
		append(reply, line);
	}

	/* An empty allowlist answers with an empty listing: a node that never
	 * opted in is not an error, it offers nothing.
	 */
	if (narrowed && (matches == 0U)) {
		refuse_not_offered(reply, prefix);
		return;
	}
	finish(reply);
}

void kfsw_remexec_run_local(const char *line, struct kfsw_remexec_reply *reply)
{
	const struct kfsw_remexec_entry *entry;
	int64_t started;
	int64_t elapsed;
	int handler_result;

	reset_reply(reply);
	if (!service.initialized) {
		refuse(reply, KFSW_REMEXEC_UNAVAILABLE, "remote execution is not initialised");
		return;
	}
	if ((line == NULL) || (line[0] == '\0')) {
		refuse(reply, KFSW_REMEXEC_INVALID, "the request carried no command");
		return;
	}
	if (strnlen(line, KFSW_REMEXEC_COMMAND_MAX + 1U) > KFSW_REMEXEC_COMMAND_MAX) {
		char reason[KFSW_REMEXEC_REFUSAL_MAX];

		(void)snprintk(reason, sizeof(reason), "a command is at most %u bytes",
			       KFSW_REMEXEC_COMMAND_MAX);
		refuse(reply, KFSW_REMEXEC_TOO_LONG, reason);
		return;
	}
	if (!printable(line)) {
		refuse(reply, KFSW_REMEXEC_INVALID,
		       "the command holds a byte that is not printable");
		return;
	}
	entry = offered(line);
	if (entry == NULL) {
		refuse_not_offered(reply, line);
		return;
	}
	if (k_mutex_lock(&remexec_executor, K_NO_WAIT) != 0) {
		refuse(reply, KFSW_REMEXEC_BUSY, "another remote execution is running");
		return;
	}

	capture.written = 0U;
	capture.held = 0U;
	capture.buf[0] = '\0';
	started = k_uptime_get();
	handler_result = shell_execute_cmd(&remexec_shell, line);
	elapsed = k_uptime_get() - started;

	append(reply, capture.buf);
	/* append() counted what it kept; the transport counted the whole lot. */
	reply->produced = (uint32_t)capture.written;
	reply->command_result = (int32_t)handler_result;
	if (elapsed > (int64_t)CONFIG_KFSW_REMEXEC_TIMEOUT_MS) {
		reply->status = KFSW_REMEXEC_TIMEOUT;
	} else if (handler_result != 0) {
		reply->status = KFSW_REMEXEC_FAILED;
	} else {
		reply->status = KFSW_REMEXEC_OK;
	}
	finish(reply);
	saturating_increment(&service.stats.accepted);
	k_mutex_unlock(&remexec_executor);
	kfsw_log_info("Remote execution '%s': %s result=%d output=%u/%u", line,
		      kfsw_remexec_status_name(reply->status), handler_result,
		      (unsigned int)reply->size, (unsigned int)reply->produced);
}
