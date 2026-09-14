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
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_FBO
#include <kfsw/services/log.h>

#include "fbo_internal.h"

#define KFSW_FBO_DIRECTORY KFSW_FTP_STORAGE_ROOT "/procedures"

/* Events recorded by this service. */
#define KFSW_FBO_EVENT_STARTED 1U
#define KFSW_FBO_EVENT_LINE_FAILED 2U
#define KFSW_FBO_EVENT_FINISHED 3U

struct line {
	char text[CONFIG_KFSW_FBO_LINE_MAX];
	size_t length;
};

static struct kfsw_fbo_status status;
static K_MUTEX_DEFINE(lock);
static bool initialized;
static atomic_t stop_requested;
static K_SEM_DEFINE(cancel, 0, 1);
/* Given once the name is set, so the thread never starts on an empty name. */
static K_SEM_DEFINE(start, 0, 1);
static char requested[KFSW_FBO_NAME_MAX];

/* One run at a time, so these buffers are static instead of on the stack. */
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
 * Split a line into a name and arguments, in place. Arguments are separated by
 * whitespace; there is no quoting.
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
		/* Copied: the next read overwrites the line buffer. */
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
	/* One extra slot so a line with too many words is refused. */
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
		struct kfsw_command_arg seconds;

		if (count != 2U) {
			return -EINVAL;
		}
		if ((kfsw_command_parse_arg(tokens[1], KFSW_COMMAND_TYPE_U32, &seconds) != 0) ||
		    (seconds.value.u32 > CONFIG_KFSW_FBO_WAIT_MAX_S)) {
			return -EINVAL;
		}
		(void)k_sem_take(&cancel, K_SECONDS(seconds.value.u32));
		return atomic_get(&stop_requested) ? -ECANCELED : 0;
	}

	if (strcmp(tokens[0], "if-event") == 0) {
		struct kfsw_command_arg source;
		struct kfsw_command_arg id;

		if ((count != 4U) || (strcmp(tokens[3], "skip") != 0)) {
			return -EINVAL;
		}
		if ((kfsw_command_parse_arg(tokens[1], KFSW_COMMAND_TYPE_U32, &source) != 0) ||
		    (kfsw_command_parse_arg(tokens[2], KFSW_COMMAND_TYPE_U32, &id) != 0) ||
		    (source.value.u32 > UINT16_MAX) || (id.value.u32 > UINT16_MAX)) {
			return -EINVAL;
		}
		*skip_next = !event_seen((uint16_t)source.value.u32, (uint16_t)id.value.u32);
		return 0;
	}

	return run_command_line(tokens, count);
}

/* Read errors and the byte limit discard the entire unfinished line. */
static int read_line(struct fs_file_t *file, size_t *bytes, bool *at_end)
{
	size_t used = 0U;
	bool overflowed = false;

	while (true) {
		char character;
		ssize_t count;

		if (atomic_get(&stop_requested)) {
			return -ECANCELED;
		}
		count = fs_read(file, &character, 1U);
		if (count < 0) {
			return (int)count;
		}
		if (count == 0) {
			*at_end = true;
			break;
		}
		if (*bytes >= CONFIG_KFSW_FBO_BYTES_MAX) {
			return -EFBIG;
		}
		(*bytes)++;
		if (character == '\n') {
			break;
		}
		if (character == '\r') {
			continue;
		}
		if (used + 1U >= sizeof(current.text)) {
			overflowed = true;
			continue;
		}
		current.text[used++] = character;
	}
	current.text[used] = '\0';
	return overflowed ? -ENAMETOOLONG : 0;
}

static int run_procedure(const char *name)
{
	char path[128];
	struct fs_file_t file;
	bool stop_on_error = true;
	bool skip_next = false;
	bool at_end = false;
	uint16_t line_number = 0U;
	size_t bytes = 0U;
	int outcome = 0;
	int result;

	(void)snprintf(path, sizeof(path), "%s/%s", KFSW_FBO_DIRECTORY, name);
	fs_file_t_init(&file);
	result = fs_open(&file, path, FS_O_READ);
	if (result != 0) {
		return result;
	}

	kfsw_log_info("FBO: %s started", name);
	note(KFSW_FBO_EVENT_STARTED, KFSW_EVENT_INFO, 0U);
	while (!at_end) {
		size_t index = 0U;

		result = read_line(&file, &bytes, &at_end);
		if ((result != 0) && (result != -ENAMETOOLONG)) {
			outcome = result;
			break;
		}
		while ((current.text[index] == ' ') || (current.text[index] == '\t')) {
			index++;
		}
		if ((result == 0) &&
		    ((current.text[index] == '\0') || (current.text[index] == '#'))) {
			continue;
		}
		if (line_number == CONFIG_KFSW_FBO_LINES_MAX) {
			outcome = -E2BIG;
			break;
		}
		line_number++;
		if ((result == 0) && skip_next) {
			skip_next = false;
			kfsw_fbo_count_skipped(line_number);
			continue;
		}
		if (result == 0) {
			result = run_line(&current.text[index], &skip_next, &stop_on_error);
		}
		kfsw_fbo_count_line(line_number, result);
		if (result != 0) {
			kfsw_log_error("FBO: %s line %u failed (%d)", name, line_number, result);
			note(KFSW_FBO_EVENT_LINE_FAILED, KFSW_EVENT_ERROR, line_number);
			if ((outcome == 0) || (result == -ECANCELED)) {
				outcome = result;
			}
			if (stop_on_error || (result == -ECANCELED)) {
				break;
			}
		}
	}
	result = fs_close(&file);
	if (outcome == 0) {
		outcome = result;
	}
	kfsw_log_info("FBO: %s finished at line %u (%d)", name, line_number, outcome);
	note(KFSW_FBO_EVENT_FINISHED, outcome ? KFSW_EVENT_ERROR : KFSW_EVENT_INFO, line_number);
	return outcome;
}

static void fbo_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		k_sem_take(&start, K_FOREVER);
		int result = run_procedure(requested);

		k_mutex_lock(&lock, K_FOREVER);
		status.last_result = result;
		status.running = false;
		atomic_set(&stop_requested, 0);
		k_sem_reset(&cancel);
		k_mutex_unlock(&lock);
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
	k_mutex_lock(&lock, K_FOREVER);
	if (!initialized) {
		initialized = true;
		k_thread_start(kfsw_fbo_thread);
	}
	k_mutex_unlock(&lock);
	kfsw_log_info("FBO: ready, up to %u lines a procedure", CONFIG_KFSW_FBO_LINES_MAX);
	return 0;
}

int kfsw_fbo_run(const char *name)
{
	struct fs_dirent info;
	char path[128];
	const char *terminator;
	int result;

	if (name == NULL) {
		return -EINVAL;
	}
	/* Bounded check without strnlen: no terminator within the limit means the
	 * name is too long, and one at the first byte means it is empty.
	 */
	terminator = memchr(name, '\0', KFSW_FBO_NAME_MAX);
	if ((terminator == NULL) || (terminator == name)) {
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
	(void)snprintf(path, sizeof(path), "%s/%s", KFSW_FBO_DIRECTORY, name);
	result = fs_stat(path, &info);
	if (result != 0) {
		return result;
	}

	if (info.type != FS_DIR_ENTRY_FILE) {
		return -EISDIR;
	}
	if (info.size > CONFIG_KFSW_FBO_BYTES_MAX) {
		return -EFBIG;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (!initialized || status.running) {
		result = initialized ? -EBUSY : -EACCES;
		k_mutex_unlock(&lock);
		return result;
	}
	k_sem_reset(&cancel);
	atomic_set(&stop_requested, 0);
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
	k_mutex_lock(&lock, K_FOREVER);
	if (status.running) {
		atomic_set(&stop_requested, 1);
		k_sem_give(&cancel);
	}
	k_mutex_unlock(&lock);
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
