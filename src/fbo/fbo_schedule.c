#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <kfsw/platform/time.h>
#include <kfsw/services/command.h>
#include <kfsw/services/fbo.h>
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_FBO
#include <kfsw/services/log.h>

#include "fbo_internal.h"

/*
 * Time-tagged releases. The queue lives in RAM only: a reset clears it,
 * because a queue that outlives a reset while its clock may have jumped fires
 * at the wrong time with nobody watching.
 *
 * A relative entry is due against the monotonic clock, so it is right on a
 * node that was never told the time and a wall clock step does not move it. An
 * absolute entry is due against the wall clock and needs it to have been set.
 */

/** Longest a relative entry may be put off: a week. */
#define KFSW_FBO_SCHEDULE_DELAY_MAX_S 604800
/** Matches the Kconfig range, so a value accepted here is one it could be built with. */
#define KFSW_FBO_SCHEDULE_LATENCY_MAX_S 3600U

struct slot {
	struct kfsw_fbo_schedule_entry entry;
	/** Relative entry: monotonic millisecond it is due. */
	int64_t due_uptime_ms;
	bool used;
};

static struct slot slots[CONFIG_KFSW_FBO_SCHEDULE_ENTRIES];
static K_MUTEX_DEFINE(queue_lock);
static K_SEM_DEFINE(wake, 0, 1);

static uint32_t latency_s = CONFIG_KFSW_FBO_SCHEDULE_LATENCY_S;
static uint32_t releases;
static uint32_t refusals;
static uint32_t overdues;
static uint32_t clock_steps;
static int last_error;

/* One add at a time under the queue lock, and one release at a time on the
 * release thread, so each parse has its own storage instead of a stack copy.
 */
static char add_text[KFSW_COMMAND_MAX_ARGS][KFSW_COMMAND_MAX_TEXT_SIZE + 1U];
static char release_text[KFSW_COMMAND_MAX_ARGS][KFSW_COMMAND_MAX_TEXT_SIZE + 1U];

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

/*
 * Resolve an entry's text into a command and its arguments. Used at add, so a
 * bad entry is refused while an operator is listening, and again at release,
 * where the text is the record of what was asked for.
 */
static int build(char *text, char storage[][KFSW_COMMAND_MAX_TEXT_SIZE + 1U],
		 struct kfsw_command_info *info, struct kfsw_command_arg *args, size_t *count)
{
	char *tokens[KFSW_COMMAND_MAX_ARGS + 2U];
	size_t words = tokenise(text, tokens, ARRAY_SIZE(tokens));
	int outcome;

	if (words == 0U) {
		return -EINVAL;
	}
	if (words > (KFSW_COMMAND_MAX_ARGS + 1U)) {
		return -E2BIG;
	}
	outcome = kfsw_command_find(tokens[0], info);
	if (outcome != 0) {
		return outcome;
	}
	if ((words - 1U) != info->arg_count) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < info->arg_count; index++) {
		if (strlen(tokens[index + 1U]) > KFSW_COMMAND_MAX_TEXT_SIZE) {
			return -EINVAL;
		}
		strcpy(storage[index], tokens[index + 1U]);
		outcome = kfsw_command_parse_arg(storage[index], info->arg_types[index],
						 &args[index]);
		if (outcome != 0) {
			return outcome;
		}
	}
	*count = info->arg_count;
	return 0;
}

/*
 * CRC32 over what an upload asked for: the kind, the time, the node and the
 * text. Two adds of the same content give the same hash, which is what makes
 * an upload idempotent and a partial one visible.
 */
static uint32_t content_hash(uint8_t kind, int64_t when, uint16_t node, const char *line)
{
	uint8_t header[13];

	header[0] = kind;
	sys_put_be64((uint64_t)when, &header[1]);
	sys_put_be16(node, &header[9]);
	sys_put_be16(0U, &header[11]);
	return crc32_ieee_update(crc32_ieee(header, sizeof(header)), (const uint8_t *)line,
				 strlen(line));
}

/* Hash of the queue content, slot order, so the ground can address it. */
static uint32_t queue_hash(void)
{
	uint32_t hash = 0U;
	uint8_t word[4];

	for (size_t index = 0U; index < ARRAY_SIZE(slots); index++) {
		if (!slots[index].used) {
			continue;
		}
		sys_put_be32(slots[index].entry.content_hash, word);
		hash = crc32_ieee_update(hash, word, sizeof(word));
	}
	return hash;
}

static bool live(const struct slot *slot)
{
	return slot->used && ((slot->entry.status == KFSW_FBO_ENTRY_SCHEDULED) ||
			      (slot->entry.status == KFSW_FBO_ENTRY_RUNNING));
}

static uint32_t latency_floor_s(void)
{
#if CONFIG_KFSW_COMMAND_CSP
	/* A release waits its turn behind a command in flight; a buffer shorter
	 * than that reply timeout would report an overdue that never happened.
	 */
	return DIV_ROUND_UP(kfsw_command_get_timeout_ms(), 1000U);
#else
	return 1U;
#endif
}

uint32_t kfsw_fbo_schedule_get_latency_s(void)
{
	return latency_s;
}

int kfsw_fbo_schedule_set_latency_s(uint32_t seconds)
{
	if ((seconds < latency_floor_s()) || (seconds > KFSW_FBO_SCHEDULE_LATENCY_MAX_S)) {
		return -ERANGE;
	}
	latency_s = seconds;
	return 0;
}

int kfsw_fbo_schedule_add(enum kfsw_fbo_time_kind kind, int64_t when, uint16_t node,
			  const char *line, uint16_t *index)
{
	struct kfsw_command_arg args[KFSW_COMMAND_MAX_ARGS];
	struct kfsw_command_info info;
	char text[KFSW_FBO_SCHEDULE_LINE_MAX];
	size_t count = 0U;
	size_t free_slot = ARRAY_SIZE(slots);
	int64_t now_utc = 0;
	uint32_t hash;
	int outcome;

	if ((line == NULL) || (index == NULL) || (line[0] == '\0') ||
	    (memchr(line, '\0', sizeof(text)) == NULL)) {
		return -EINVAL;
	}
	if ((kind != KFSW_FBO_TIME_RELATIVE) && (kind != KFSW_FBO_TIME_ABSOLUTE)) {
		return -EINVAL;
	}
	if (kind == KFSW_FBO_TIME_RELATIVE) {
		if ((when <= 0) || (when > KFSW_FBO_SCHEDULE_DELAY_MAX_S)) {
			return -EINVAL;
		}
	} else {
		if (when <= 0) {
			return -EINVAL;
		}
		/* The gate: with nothing having set the clock this power cycle,
		 * an absolute entry has no time to be due against.
		 */
		if (kfsw_fbo_schedule_clock_seconds(&now_utc) != 0) {
			refusals++;
			kfsw_log_warning("FBO: absolute entry refused, the clock is not set");
			return -ENODATA;
		}
		if ((now_utc - when) > (int64_t)latency_s) {
			refusals++;
			return -ETIME;
		}
	}
#if !CONFIG_KFSW_COMMAND_CSP
	if (node != 0U) {
		return -ENOTSUP;
	}
#endif

	strcpy(text, line);
	k_mutex_lock(&queue_lock, K_FOREVER);
	outcome = build(text, add_text, &info, args, &count);
	if (outcome != 0) {
		refusals++;
		k_mutex_unlock(&queue_lock);
		return outcome;
	}
	hash = content_hash((uint8_t)kind, when, node, line);
	for (size_t slot = 0U; slot < ARRAY_SIZE(slots); slot++) {
		if (live(&slots[slot]) && (slots[slot].entry.content_hash == hash)) {
			*index = (uint16_t)slot;
			k_mutex_unlock(&queue_lock);
			return -EEXIST;
		}
		if (!slots[slot].used && (free_slot == ARRAY_SIZE(slots))) {
			free_slot = slot;
		}
	}
	if (free_slot == ARRAY_SIZE(slots)) {
		refusals++;
		k_mutex_unlock(&queue_lock);
		return -ENOSPC;
	}

	struct slot *target = &slots[free_slot];

	memset(target, 0, sizeof(*target));
	strcpy(target->entry.line, line);
	target->entry.index = (uint16_t)free_slot;
	target->entry.node = node;
	target->entry.command_id = info.id;
	target->entry.kind = (uint8_t)kind;
	target->entry.status = KFSW_FBO_ENTRY_SCHEDULED;
	target->entry.content_hash = hash;
	if (kind == KFSW_FBO_TIME_RELATIVE) {
		target->entry.delay_s = (uint32_t)when;
		target->due_uptime_ms = (int64_t)kfsw_time_monotonic_ms() + (when * 1000);
	} else {
		target->entry.due_utc = when;
	}
	target->used = true;
	*index = (uint16_t)free_slot;
	k_mutex_unlock(&queue_lock);

	k_sem_give(&wake);
	kfsw_log_info("FBO: entry %u scheduled, %s, queue 0x%08x", *index, line, queue_hash());
	return 0;
}

int kfsw_fbo_schedule_cancel(uint16_t index)
{
	int outcome = 0;

	if (index >= ARRAY_SIZE(slots)) {
		return -ENOENT;
	}
	k_mutex_lock(&queue_lock, K_FOREVER);
	if (!slots[index].used) {
		outcome = -ENOENT;
	} else if (slots[index].entry.status != KFSW_FBO_ENTRY_SCHEDULED) {
		/* Never claim to have stopped something that already fired. */
		outcome = -EALREADY;
	} else {
		slots[index].entry.status = KFSW_FBO_ENTRY_CANCELLED;
	}
	k_mutex_unlock(&queue_lock);
	return outcome;
}

int kfsw_fbo_schedule_clear(void)
{
	int dropped = 0;

	k_mutex_lock(&queue_lock, K_FOREVER);
	for (size_t index = 0U; index < ARRAY_SIZE(slots); index++) {
		if (!slots[index].used) {
			continue;
		}
		/* A release in flight keeps its slot until it reports. */
		if (slots[index].entry.status == KFSW_FBO_ENTRY_RUNNING) {
			continue;
		}
		memset(&slots[index], 0, sizeof(slots[index]));
		dropped++;
	}
	k_mutex_unlock(&queue_lock);
	return dropped;
}

/* Seconds still to wait, 0 once due or once the entry is no longer waiting. */
static uint32_t remaining(const struct slot *slot, int64_t mono_ms, bool utc_ok, int64_t utc)
{
	int64_t left;

	if (slot->entry.status != KFSW_FBO_ENTRY_SCHEDULED) {
		return 0U;
	}
	if (slot->entry.kind == KFSW_FBO_TIME_RELATIVE) {
		left = DIV_ROUND_UP(slot->due_uptime_ms - mono_ms, 1000);
	} else if (utc_ok) {
		left = slot->entry.due_utc - utc;
	} else {
		return 0U;
	}
	return (left > 0) ? (uint32_t)left : 0U;
}

int kfsw_fbo_schedule_get_entry(uint16_t index, struct kfsw_fbo_schedule_entry *entry)
{
	int64_t utc = 0;
	bool utc_ok;

	if (entry == NULL) {
		return -EINVAL;
	}
	if (index >= ARRAY_SIZE(slots)) {
		return -ENOENT;
	}
	utc_ok = kfsw_fbo_schedule_clock_seconds(&utc) == 0;
	k_mutex_lock(&queue_lock, K_FOREVER);
	if (!slots[index].used) {
		k_mutex_unlock(&queue_lock);
		return -ENOENT;
	}
	*entry = slots[index].entry;
	entry->remaining_s =
		remaining(&slots[index], (int64_t)kfsw_time_monotonic_ms(), utc_ok, utc);
	k_mutex_unlock(&queue_lock);
	return 0;
}

int kfsw_fbo_schedule_get_status(struct kfsw_fbo_schedule_status *status)
{
	int64_t utc = 0;
	int64_t mono;
	bool utc_ok;

	if (status == NULL) {
		return -EINVAL;
	}
	utc_ok = kfsw_fbo_schedule_clock_seconds(&utc) == 0;
	mono = (int64_t)kfsw_time_monotonic_ms();
	memset(status, 0, sizeof(*status));
	k_mutex_lock(&queue_lock, K_FOREVER);
	status->capacity = (uint16_t)ARRAY_SIZE(slots);
	status->latency_s = latency_s;
	status->releases = releases;
	status->refusals = refusals;
	status->overdues = overdues;
	status->clock_steps = clock_steps;
	status->last_error = last_error;
	status->clock_set = utc_ok;
	status->hash = queue_hash();
	for (size_t index = 0U; index < ARRAY_SIZE(slots); index++) {
		const struct slot *slot = &slots[index];

		if (!slot->used) {
			continue;
		}
		status->entries++;
		switch (slot->entry.status) {
		case KFSW_FBO_ENTRY_SCHEDULED:
			status->scheduled++;
			if (slot->entry.kind == KFSW_FBO_TIME_ABSOLUTE) {
				if ((status->next_due_utc == 0) ||
				    (slot->entry.due_utc < status->next_due_utc)) {
					status->next_due_utc = slot->entry.due_utc;
				}
			} else {
				uint32_t left = remaining(slot, mono, utc_ok, utc);

				if ((status->next_due_s == 0U) || (left < status->next_due_s)) {
					status->next_due_s = left;
				}
			}
			break;
		case KFSW_FBO_ENTRY_RUNNING:
			status->running++;
			break;
		case KFSW_FBO_ENTRY_COMPLETED:
			status->completed++;
			break;
		case KFSW_FBO_ENTRY_FAILED:
			status->failed++;
			break;
		case KFSW_FBO_ENTRY_OVERDUE:
			status->overdue++;
			break;
		default:
			status->cancelled++;
			break;
		}
	}
	k_mutex_unlock(&queue_lock);
	return 0;
}

/* Run one entry's command. The queue lock is not held: a handler may block. */
static void invoke(struct kfsw_fbo_schedule_entry *entry, int *outcome,
		   struct kfsw_command_result *result)
{
	struct kfsw_command_arg args[KFSW_COMMAND_MAX_ARGS];
	struct kfsw_command_info info;
	char text[KFSW_FBO_SCHEDULE_LINE_MAX];
	size_t count = 0U;

	strcpy(text, entry->line);
	*outcome = build(text, release_text, &info, args, &count);
	if (*outcome != 0) {
		return;
	}
	if (entry->node == 0U) {
		*outcome = kfsw_command_invoke(info.name, args, count, result);
		return;
	}
#if CONFIG_KFSW_COMMAND_RETRY && CONFIG_KFSW_COMMAND_CSP
	/* A ticket, so a link retry cannot execute the release twice. */
	*outcome = kfsw_command_invoke_remote_retry(entry->node, info.name, args, count, result);
#elif CONFIG_KFSW_COMMAND_CSP
	*outcome = kfsw_command_invoke_remote(entry->node, info.name, args, count, result);
#else
	*outcome = -ENOTSUP;
#endif
}

static void release(size_t index, bool utc_ok, int64_t utc)
{
	struct kfsw_command_result result = {0};
	struct kfsw_fbo_schedule_entry entry;
	bool failed;
	int outcome;

	k_mutex_lock(&queue_lock, K_FOREVER);
	slots[index].entry.status = KFSW_FBO_ENTRY_RUNNING;
	slots[index].entry.released_utc = utc_ok ? utc : 0;
	if (releases < UINT32_MAX) {
		releases++;
	}
	entry = slots[index].entry;
	k_mutex_unlock(&queue_lock);

	kfsw_fbo_note(KFSW_FBO_EVENT_RELEASED, KFSW_EVENT_INFO, (uint16_t)index);
	invoke(&entry, &outcome, &result);

	k_mutex_lock(&queue_lock, K_FOREVER);
	slots[index].entry.result = outcome;
	slots[index].entry.command_status = (uint8_t)result.status;
	failed = (outcome != 0) || (result.status != KFSW_COMMAND_OK);
	if (failed) {
		slots[index].entry.status = KFSW_FBO_ENTRY_FAILED;
		last_error = outcome;
	} else {
		slots[index].entry.status = KFSW_FBO_ENTRY_COMPLETED;
	}
	k_mutex_unlock(&queue_lock);
	if (failed) {
		kfsw_log_error("FBO: entry %u released %s and failed (%d)", (unsigned int)index,
			       entry.line, outcome);
	} else {
		kfsw_log_info("FBO: entry %u released %s", (unsigned int)index, entry.line);
	}
}

/* Mark anything later than the buffer overdue, and report the earliest due. */
static bool due_now(const struct slot *slot, int64_t mono_ms, bool utc_ok, int64_t utc,
		    int64_t *lateness_s)
{
	if (slot->entry.kind == KFSW_FBO_TIME_RELATIVE) {
		if (mono_ms < slot->due_uptime_ms) {
			return false;
		}
		*lateness_s = (mono_ms - slot->due_uptime_ms) / 1000;
		return true;
	}
	if (!utc_ok || (utc < slot->entry.due_utc)) {
		return false;
	}
	*lateness_s = utc - slot->entry.due_utc;
	return true;
}

static void sweep(bool utc_ok, int64_t utc)
{
	int64_t mono = (int64_t)kfsw_time_monotonic_ms();
	unsigned int released = 0U;

	while (released < CONFIG_KFSW_FBO_SCHEDULE_BURST) {
		size_t candidate = ARRAY_SIZE(slots);
		int64_t earliest = 0;

		k_mutex_lock(&queue_lock, K_FOREVER);
		for (size_t index = 0U; index < ARRAY_SIZE(slots); index++) {
			struct slot *slot = &slots[index];
			int64_t lateness = 0;

			if (!slot->used || (slot->entry.status != KFSW_FBO_ENTRY_SCHEDULED)) {
				continue;
			}
			if (!due_now(slot, mono, utc_ok, utc, &lateness)) {
				continue;
			}
			if (lateness > (int64_t)latency_s) {
				slot->entry.status = KFSW_FBO_ENTRY_OVERDUE;
				slot->entry.result = -ETIME;
				if (overdues < UINT32_MAX) {
					overdues++;
				}
				kfsw_log_warning("FBO: entry %u is %lld s late and will not run",
						 (unsigned int)index, (long long)lateness);
				kfsw_fbo_note(KFSW_FBO_EVENT_OVERDUE, KFSW_EVENT_ERROR,
					      (uint16_t)index);
				continue;
			}
			/* Earliest due first, so release order follows time order. */
			if ((candidate == ARRAY_SIZE(slots)) || (lateness > earliest)) {
				candidate = index;
				earliest = lateness;
			}
		}
		k_mutex_unlock(&queue_lock);
		if (candidate == ARRAY_SIZE(slots)) {
			return;
		}
		release(candidate, utc_ok, utc);
		released++;
	}
}

/*
 * A step is the wall clock disagreeing with the monotonic clock. It is counted
 * and reported; it changes no entry's status by itself, and relative entries
 * cannot be affected by one at all.
 */
static void check_step(bool utc_ok, int64_t utc, int64_t mono)
{
	static int64_t last_utc;
	static int64_t last_mono;
	static bool have_last;
	int64_t drift;

	if (!utc_ok) {
		have_last = false;
		return;
	}
	if (have_last) {
		drift = ((utc - last_utc) * 1000) - (mono - last_mono);
		if ((drift > CONFIG_KFSW_FBO_SCHEDULE_STEP_MS) ||
		    (drift < -CONFIG_KFSW_FBO_SCHEDULE_STEP_MS)) {
			if (clock_steps < UINT32_MAX) {
				clock_steps++;
			}
			kfsw_log_warning("FBO: wall clock stepped %lld ms", (long long)drift);
			kfsw_fbo_note(KFSW_FBO_EVENT_CLOCK_STEP, KFSW_EVENT_WARNING,
				      (uint16_t)clock_steps);
		}
	}
	last_utc = utc;
	last_mono = mono;
	have_last = true;
}

static void schedule_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		int64_t utc = 0;
		bool utc_ok;

		(void)k_sem_take(&wake, K_MSEC(CONFIG_KFSW_FBO_SCHEDULE_TICK_MS));
		utc_ok = kfsw_fbo_schedule_clock_seconds(&utc) == 0;
		check_step(utc_ok, utc, (int64_t)kfsw_time_monotonic_ms());
		sweep(utc_ok, utc);
	}
}

K_THREAD_DEFINE(kfsw_fbo_schedule_thread, CONFIG_KFSW_FBO_SCHEDULE_STACK_SIZE, schedule_thread,
		NULL, NULL, NULL, CONFIG_KFSW_FBO_SCHEDULE_PRIORITY, 0, SYS_FOREVER_MS);

void kfsw_fbo_schedule_init(void)
{
	k_thread_start(kfsw_fbo_schedule_thread);
}
