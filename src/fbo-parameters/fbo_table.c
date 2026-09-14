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
};

const struct kfsw_param_definition_set kfsw_fbo_param_definitions = {
	.table = KFSW_FBO_PARAM_TABLE_ID,
	.name = KFSW_FBO_PARAM_TABLE_NAME,
	.definitions = fbo_param_definitions,
	.count = ARRAY_SIZE(fbo_param_definitions),
};
