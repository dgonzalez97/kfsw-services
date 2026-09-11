#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/storage.h>
#include <kfsw/services/command.h>
#include <kfsw/services/event.h>
#include <kfsw/services/ftp.h>
#include <kfsw/services/fbo.h>
/* Attributes this file's messages to procedures, so a pass can be made quiet
 * or loud without touching the command service it drives. */
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_FBO
#include <kfsw/services/log.h>
#include <kfsw/services/parameter.h>

#include "fbo_internal.h"

/*
 * A procedure is a list of commands, and nothing else.
 *
 * Every line is something the command service already validates, so a file
 * cannot ask a node to do anything the radio could not. Three guards let a
 * sequence react to what happened — stop on a failure, wait for a pass, skip a
 * line unless an event was recorded — and there are no loops and no jumps, so
 * the file terminates by construction. That is the property that makes it safe
 * to run with nobody listening.
 */
#define KFSW_FBO_DIRECTORY KFSW_FTP_STORAGE_ROOT "/procedures"

/* Events this service records, so a pass can be reconstructed from the ring
 * rather than from a console nobody was watching.
 */
#define KFSW_FBO_EVENT_STARTED 1U
#define KFSW_FBO_EVENT_LINE_FAILED 2U
#define KFSW_FBO_EVENT_FINISHED 3U

struct line {
	char text[CONFIG_KFSW_FBO_LINE_MAX];
	size_t length;
};

static struct kfsw_fbo_status status;
static struct k_mutex lock;
static bool initialized;
static atomic_t stop_requested;
static atomic_t running;
/* Given once the name is in place, so the thread cannot start on a name that
 * has not been written yet. Waiting on it rather than polling a flag is also
 * what removes the tick: there is nothing to look for between procedures.
 */
static K_SEM_DEFINE(start, 0, 1);
static char requested[KFSW_FBO_NAME_MAX];

/* One at a time, so the buffers below are the service's rather than a stack's:
 * a line plus its arguments is more than a 2 kB thread should carry.
 */
static struct line current;
static char argument_text[KFSW_COMMAND_MAX_ARGS][KFSW_COMMAND_MAX_TEXT_SIZE + 1U];

static void note(uint16_t id, enum kfsw_event_severity severity, uint16_t line)
{
	uint8_t payload[2];

	payload[0] = (uint8_t)(line >> 8);
	payload[1] = (uint8_t)(line & 0xFFU);
	kfsw_event_emit(KFSW_EVENT_SOURCE_FBO, id, severity, payload, sizeof(payload));
}

/*
 * Split a line into a name and its arguments, in place.
 *
 * Whitespace-separated, no quoting: an argument with a space in it cannot be
 * written, which is a limit worth having while the alternative is a parser
 * with an escape syntax nobody asked for.
 */
static size_t tokenise(char *text, char **tokens, size_t max)
{
	size_t count = 0U;
	char *cursor = text;

	while ((count < max) && (*cursor != '\0')) {
		while ((*cursor == ' ') || (*cursor == '\t')) {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}
		tokens[count++] = cursor;
		while ((*cursor != '\0') && (*cursor != ' ') && (*cursor != '\t')) {
			cursor++;
		}
		if (*cursor != '\0') {
			*cursor = '\0';
			cursor++;
		}
	}
	return count;
}

/* Whether an event of this source and id is in the ring. */
static bool event_seen(uint16_t source, uint16_t id)
{
	struct kfsw_event_record record;

	for (uint16_t age = 0U; age < CONFIG_KFSW_FBO_EVENT_LOOKBACK; age++) {
		if (kfsw_event_get(age, &record) != 0) {
			break;
		}
		if ((record.source == source) && (record.id == id)) {
			return true;
		}
	}
	return false;
}

static int run_command_line(char **tokens, size_t count)
{
	struct kfsw_command_arg args[KFSW_COMMAND_MAX_ARGS];
	struct kfsw_command_result result;
	struct kfsw_command_info info;
	size_t arg_count = count - 1U;
	int outcome;

	outcome = kfsw_command_find(tokens[0], &info);
	if (outcome != 0) {
		return outcome;
	}
	if (arg_count != info.arg_count) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < arg_count; index++) {
		/* Copied, because the text a command keeps must outlive the
		 * line buffer the next read overwrites.
		 */
		if (strlen(tokens[index + 1U]) > KFSW_COMMAND_MAX_TEXT_SIZE) {
			return -ENAMETOOLONG;
		}
		strcpy(argument_text[index], tokens[index + 1U]);
		outcome = kfsw_command_parse_arg(argument_text[index], info.arg_types[index],
						 &args[index]);
		if (outcome != 0) {
			return outcome;
		}
	}

	outcome = kfsw_command_invoke(tokens[0], args, arg_count, &result);
	if (outcome != 0) {
		return outcome;
	}
	return (result.status == KFSW_COMMAND_OK) ? 0 : -EIO;
}

/*
 * One line.
 *
 * Returns 0 to carry on, a negative errno when the line failed, and leaves
 * *skip_next set when a guard says the following line is not for this run.
 */
static int run_line(char *text, bool *skip_next, bool *stop_on_error)
{
	char *tokens[KFSW_COMMAND_MAX_ARGS + 2U];
	size_t count = tokenise(text, tokens, ARRAY_SIZE(tokens));

	if (count == 0U) {
		return 0;
	}
	/* One more slot than any command can use, so a line with too many
	 * words is refused instead of quietly losing its tail.
	 */
	if (count > (KFSW_COMMAND_MAX_ARGS + 1U)) {
		return -E2BIG;
	}

	if (strcmp(tokens[0], "on-error") == 0) {
		if ((count != 2U) ||
		    ((strcmp(tokens[1], "stop") != 0) && (strcmp(tokens[1], "continue") != 0))) {
			return -EINVAL;
		}
		*stop_on_error = (strcmp(tokens[1], "stop") == 0);
		return 0;
	}

	if (strcmp(tokens[0], "wait") == 0) {
		unsigned long seconds;
		char *end;

		if (count != 2U) {
			return -EINVAL;
		}
		seconds = strtoul(tokens[1], &end, 0);
		if ((end == tokens[1]) || (*end != '\0') ||
		    (seconds > CONFIG_KFSW_FBO_WAIT_MAX_S)) {
			return -EINVAL;
		}
		/* Bounded by construction: a procedure that could wait for ever
		 * would be a procedure that can hang the node.
		 */
		k_sleep(K_SECONDS(seconds));
		return 0;
	}

	if (strcmp(tokens[0], "if-event") == 0) {
		unsigned long source;
		unsigned long id;
		char *end;

		if ((count != 4U) || (strcmp(tokens[3], "skip") != 0)) {
			return -EINVAL;
		}
		source = strtoul(tokens[1], &end, 0);
		if ((end == tokens[1]) || (*end != '\0')) {
			return -EINVAL;
		}
		id = strtoul(tokens[2], &end, 0);
		if ((end == tokens[2]) || (*end != '\0')) {
			return -EINVAL;
		}
		*skip_next = !event_seen((uint16_t)source, (uint16_t)id);
		return 0;
	}

	return run_command_line(tokens, count);
}

static void run_procedure(const char *name)
{
	char path[128];
	struct fs_file_t file;
	bool stop_on_error = true;
	bool skip_next = false;
	uint16_t line_number = 0U;
	size_t used = 0U;
	bool at_end = false;
	bool overflowed = false;
	int result;

	(void)snprintf(path, sizeof(path), "%s/%s", KFSW_FBO_DIRECTORY, name);
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result != 0) {
		kfsw_log_error("FBO: cannot open %s (%d)", name, result);
		return;
	}

	kfsw_log_info("FBO: %s started", name);
	note(KFSW_FBO_EVENT_STARTED, KFSW_EVENT_INFO, 0U);

	while (!at_end && (line_number < CONFIG_KFSW_FBO_LINES_MAX)) {
		ssize_t read;
		size_t index = 0U;

		if (atomic_get(&stop_requested) != 0) {
			kfsw_log_warning("FBO: %s stopped at line %u", name, line_number);
			break;
		}

		/* One byte at a time because a line is short and a procedure
		 * runs at human speed; buffering a file this size would be
		 * state for no gain.
		 */
		used = 0U;
		overflowed = false;
		while (true) {
			char character;

			read = fs_read(&file, &character, 1U);
			if (read <= 0) {
				at_end = true;
				break;
			}
			if (character == '\n') {
				break;
			}
			if (character == '\r') {
				continue;
			}
			if (used + 1U >= sizeof(current.text)) {
				/* Read on to the newline rather than stopping
				 * here: leaving the tail in the file would turn
				 * one over-long line into two, and the second
				 * half might parse as something.
				 */
				overflowed = true;
				continue;
			}
			current.text[used++] = character;
		}
		current.text[used] = '\0';

		while ((current.text[index] == ' ') || (current.text[index] == '\t')) {
			index++;
		}
		if ((current.text[index] == '\0') || (current.text[index] == '#')) {
			continue;
		}

		line_number++;
		if (overflowed) {
			kfsw_log_error("FBO: %s line %u is longer than %u bytes", name, line_number,
				       (unsigned int)(sizeof(current.text) - 1U));
			kfsw_fbo_count_line(line_number, -ENAMETOOLONG);
			note(KFSW_FBO_EVENT_LINE_FAILED, KFSW_EVENT_ERROR, line_number);
			if (stop_on_error) {
				break;
			}
			continue;
		}
		if (skip_next) {
			skip_next = false;
			kfsw_fbo_count_skipped(line_number);
			continue;
		}

		result = run_line(&current.text[index], &skip_next, &stop_on_error);
		kfsw_fbo_count_line(line_number, result);
		if (result != 0) {
			kfsw_log_error("FBO: %s line %u failed (%d)", name, line_number, result);
			note(KFSW_FBO_EVENT_LINE_FAILED, KFSW_EVENT_ERROR, line_number);
			if (stop_on_error) {
				break;
			}
		}
	}

	(void)fs_close(&file);
	kfsw_log_info("FBO: %s finished at line %u", name, line_number);
	note(KFSW_FBO_EVENT_FINISHED, KFSW_EVENT_INFO, line_number);
}

static void fbo_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_sem_take(&start, K_FOREVER);
		run_procedure(requested);
		k_mutex_lock(&lock, K_FOREVER);
		status.running = false;
		k_mutex_unlock(&lock);
		atomic_set(&stop_requested, 0);
		atomic_set(&running, 0);
	}
}

K_THREAD_DEFINE(kfsw_fbo_thread, CONFIG_KFSW_FBO_STACK_SIZE, fbo_thread, NULL, NULL, NULL,
		CONFIG_KFSW_FBO_PRIORITY, 0, SYS_FOREVER_MS);

void kfsw_fbo_count_line(uint16_t line, int outcome)
{
	k_mutex_lock(&lock, K_FOREVER);
	status.line = line;
	status.lines_run++;
	if (outcome != 0) {
		status.lines_failed++;
	}
	k_mutex_unlock(&lock);
}

void kfsw_fbo_count_skipped(uint16_t line)
{
	k_mutex_lock(&lock, K_FOREVER);
	status.line = line;
	status.lines_skipped++;
	k_mutex_unlock(&lock);
}

int kfsw_fbo_init(void)
{
	if (initialized) {
		return 0;
	}
	(void)k_mutex_init(&lock);
	initialized = true;
	k_thread_start(kfsw_fbo_thread);
	kfsw_log_info("FBO: ready, up to %u lines a procedure", CONFIG_KFSW_FBO_LINES_MAX);
	return 0;
}

int kfsw_fbo_run(const char *name)
{
	struct fs_dirent info;
	char path[128];
	size_t length;
	int result;

	if (!initialized) {
		return -EACCES;
	}
	if (name == NULL) {
		return -EINVAL;
	}
	length = strnlen(name, KFSW_FBO_NAME_MAX);
	if ((length == 0U) || (length >= KFSW_FBO_NAME_MAX)) {
		return -EINVAL;
	}
	/* A name, not a path: a procedure lives in one directory and cannot
	 * reach out of it.
	 */
	if ((strchr(name, '/') != NULL) || (strstr(name, "..") != NULL)) {
		return -EINVAL;
	}
	if (!kfsw_storage_is_ready()) {
		return -ENODEV;
	}
	if (atomic_cas(&running, 0, 1) == false) {
		return -EBUSY;
	}

	(void)snprintf(path, sizeof(path), "%s/%s", KFSW_FBO_DIRECTORY, name);
	result = fs_stat(path, &info);
	if (result != 0) {
		atomic_set(&running, 0);
		return result;
	}

	k_mutex_lock(&lock, K_FOREVER);
	strcpy(requested, name);
	strcpy(status.name, name);
	status.running = true;
	status.line = 0U;
	status.runs++;
	k_mutex_unlock(&lock);

	/* Last, and only once the name is in place. */
	k_sem_give(&start);
	return 0;
}

int kfsw_fbo_stop(void)
{
	atomic_set(&stop_requested, 1);
	return 0;
}

int kfsw_fbo_get_status(struct kfsw_fbo_status *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	*out = status;
	k_mutex_unlock(&lock);
	return 0;
}
