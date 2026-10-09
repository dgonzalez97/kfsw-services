#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/fbo.h>
#include <kfsw/services/parameter.h>

/* Procedure counters and state. */

static char fbo_name[KFSW_FBO_NAME_MAX];
static uint8_t fbo_running;
static uint32_t fbo_runs;
static uint32_t fbo_lines_run;
static uint32_t fbo_lines_failed;
static uint32_t fbo_lines_skipped;
static uint16_t fbo_line;
static uint16_t fbo_lines_max = CONFIG_KFSW_FBO_LINES_MAX;

/* Queue state and counters. */

static uint16_t sched_entries;
static uint16_t sched_scheduled;
static uint16_t sched_overdue;
static uint8_t sched_clock_set;
static uint32_t sched_next_due_s;
static uint32_t sched_releases;
static uint32_t sched_refusals;
static uint32_t sched_overdues;
static uint32_t sched_hash;
static uint32_t sched_latency_s = CONFIG_KFSW_FBO_SCHEDULE_LATENCY_S;
static int64_t sched_next_due_utc;

static void sample_status(void)
{
	struct kfsw_fbo_status status;

	if (kfsw_fbo_get_status(&status) != 0) {
		return;
	}
	for (size_t index = 0U; index < sizeof(fbo_name); index++) {
		fbo_name[index] = status.name[index];
	}
	fbo_name[sizeof(fbo_name) - 1U] = '\0';
	fbo_running = status.running ? 1U : 0U;
	fbo_runs = status.runs;
	fbo_lines_run = status.lines_run;
	fbo_lines_failed = status.lines_failed;
	fbo_lines_skipped = status.lines_skipped;
	fbo_line = status.line;
}

static void sample_schedule(void)
{
	struct kfsw_fbo_schedule_status status;

	if (kfsw_fbo_schedule_get_status(&status) != 0) {
		return;
	}
	sched_entries = status.entries;
	sched_scheduled = status.scheduled;
	sched_overdue = status.overdue;
	sched_clock_set = status.clock_set ? 1U : 0U;
	sched_next_due_s = status.next_due_s;
	sched_releases = status.releases;
	sched_refusals = status.refusals;
	sched_overdues = status.overdues;
	sched_hash = status.hash;
	sched_latency_s = status.latency_s;
	sched_next_due_utc = status.next_due_utc;
}

#define SCHED_SAMPLE(field, type)                                                                  \
	static void sample_sched_##field(void *value)                                              \
	{                                                                                          \
		sample_schedule();                                                                 \
		*(type *)value = sched_##field;                                                    \
	}

SCHED_SAMPLE(entries, uint16_t)
SCHED_SAMPLE(scheduled, uint16_t)
SCHED_SAMPLE(overdue, uint16_t)
SCHED_SAMPLE(clock_set, uint8_t)
SCHED_SAMPLE(next_due_s, uint32_t)
SCHED_SAMPLE(releases, uint32_t)
SCHED_SAMPLE(refusals, uint32_t)
SCHED_SAMPLE(overdues, uint32_t)
SCHED_SAMPLE(hash, uint32_t)
SCHED_SAMPLE(latency_s, uint32_t)
SCHED_SAMPLE(next_due_utc, int64_t)

/*
 * The service owns the only rule about what a latency is allowed to be, so the
 * proposed value is handed to it here rather than range-checked a second time.
 * A table that kept its own copy of the rule would eventually disagree with it.
 */
static int apply_latency(const union kfsw_param_scalar *value)
{
	return kfsw_fbo_schedule_set_latency_s(value->u32);
}

static void sample_name(void *value)
{
	sample_status();
	for (size_t index = 0U; index < sizeof(fbo_name); index++) {
		((char *)value)[index] = fbo_name[index];
	}
}

#define FBO_SAMPLE(field, type)                                                                    \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_status();                                                                   \
		*(type *)value = fbo_##field;                                                      \
	}

FBO_SAMPLE(running, uint8_t)
FBO_SAMPLE(runs, uint32_t)
FBO_SAMPLE(lines_run, uint32_t)
FBO_SAMPLE(lines_failed, uint32_t)
FBO_SAMPLE(lines_skipped, uint32_t)
FBO_SAMPLE(line, uint16_t)

static const struct kfsw_param_definition fbo_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_running",
		.description = "Whether a procedure is running now",
		.value = &fbo_running,
		.sample = sample_running,
	},
	{
		.offset = 0x01U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_runs",
		.description = "Procedures started since boot",
		.value = &fbo_runs,
		.sample = sample_runs,
	},
	{
		.offset = 0x05U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_lines_run",
		.description = "Lines carried out since boot",
		.value = &fbo_lines_run,
		.sample = sample_lines_run,
	},
	{
		.offset = 0x09U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_lines_failed",
		.description = "Lines that reported a failure",
		.value = &fbo_lines_failed,
		.sample = sample_lines_failed,
	},
	{
		.offset = 0x0dU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_lines_skipped",
		.description = "Lines a guard decided were not for this run",
		.value = &fbo_lines_skipped,
		.sample = sample_lines_skipped,
	},
	{
		.offset = 0x11U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_line",
		.description = "Line the current or last run reached, 1-based",
		.value = &fbo_line,
		.sample = sample_line,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_FBO_NAME_MAX,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_procedure",
		.description = "Procedure running now, or the last one that ran",
		.value = fbo_name,
		.sample = sample_name,
	},
	{
		.offset = 0x40U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_lines_max",
		.description = "Lines one procedure may carry out",
		.value = &fbo_lines_max,
	},
	{
		.offset = 0x50U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_entries",
		.description = "Slots the queue is holding",
		.value = &sched_entries,
		.sample = sample_sched_entries,
	},
	{
		.offset = 0x54U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_scheduled",
		.description = "Entries still waiting for their time",
		.value = &sched_scheduled,
		.sample = sample_sched_scheduled,
	},
	{
		.offset = 0x56U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_overdue",
		.description = "Entries whose time passed beyond the buffer",
		.value = &sched_overdue,
		.sample = sample_sched_overdue,
	},
	{
		.offset = 0x58U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_clock_set",
		.description = "Whether an absolute entry can be accepted at all",
		.value = &sched_clock_set,
		.sample = sample_sched_clock_set,
	},
	{
		.offset = 0x59U,
		.type = KFSW_PARAM_U32,
		.unit = "s",
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_next_due_s",
		.description = "Seconds to the earliest relative entry",
		.value = &sched_next_due_s,
		.sample = sample_sched_next_due_s,
	},
	{
		.offset = 0x5dU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_sched_releases",
		.description = "Releases attempted since boot",
		.value = &sched_releases,
		.sample = sample_sched_releases,
	},
	{
		.offset = 0x61U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_sched_refusals",
		.description = "Entries refused when they were added",
		.value = &sched_refusals,
		.sample = sample_sched_refusals,
	},
	{
		.offset = 0x65U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "fbo_sched_overdues",
		.description = "Entries that went overdue since boot",
		.value = &sched_overdues,
		.sample = sample_sched_overdues,
	},
	{
		.offset = 0x69U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_hash",
		.description = "CRC32 over the queue, to compare against what was uploaded",
		.value = &sched_hash,
		.sample = sample_sched_hash,
	},
	{
		.offset = 0x6dU,
		.type = KFSW_PARAM_U32,
		.unit = "s",
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "fbo_sched_latency_s",
		.description = "An entry later than this reports overdue instead of running",
		.value = &sched_latency_s,
		.default_value = {.u32 = CONFIG_KFSW_FBO_SCHEDULE_LATENCY_S},
		.validate = apply_latency,
		.sample = sample_sched_latency_s,
	},
	{
		.offset = 0x71U,
		.type = KFSW_PARAM_I64,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "fbo_sched_next_due_utc",
		.description = "UTC second of the earliest absolute entry, 0 for none",
		.value = &sched_next_due_utc,
		.sample = sample_sched_next_due_utc,
	},
};

const struct kfsw_param_definition_set kfsw_fbo_param_definitions = {
	.table = KFSW_FBO_PARAM_TABLE_ID,
	.name = KFSW_FBO_PARAM_TABLE_NAME,
	.description = "Procedure runs, progress and the time-tagged queue",
	.definitions = fbo_param_definitions,
	.count = ARRAY_SIZE(fbo_param_definitions),
};
