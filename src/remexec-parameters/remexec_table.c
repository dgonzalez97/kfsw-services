#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/parameter.h>
#include <kfsw/services/remexec.h>

static uint32_t remexec_accepted;
static uint32_t remexec_refused;
static uint32_t remexec_truncated;
static uint16_t remexec_entries;
static char remexec_last_refusal[KFSW_REMEXEC_REFUSAL_MAX];
static uint8_t remexec_running;
static int32_t remexec_last_result;
static uint32_t remexec_last_ms;
static char remexec_last_command[KFSW_REMEXEC_COMMAND_MAX + 1U];

static void sample_state(void)
{
	struct kfsw_remexec_stats stats;

	kfsw_remexec_get_stats(&stats);
	remexec_accepted = stats.accepted;
	remexec_refused = stats.refused;
	remexec_truncated = stats.truncated;
	remexec_entries = stats.entries;
	remexec_running = stats.running ? 1U : 0U;
	remexec_last_result = stats.last_result;
	remexec_last_ms = stats.last_duration_ms;
	(void)strncpy(remexec_last_command, kfsw_remexec_last_command(),
		      sizeof(remexec_last_command) - 1U);
	remexec_last_command[sizeof(remexec_last_command) - 1U] = '\0';
	(void)strncpy(remexec_last_refusal, kfsw_remexec_last_refusal(),
		      sizeof(remexec_last_refusal) - 1U);
	remexec_last_refusal[sizeof(remexec_last_refusal) - 1U] = '\0';
}

static void sample_accepted(void *value)
{
	sample_state();
	*(uint32_t *)value = remexec_accepted;
}

static void sample_refused(void *value)
{
	sample_state();
	*(uint32_t *)value = remexec_refused;
}

static void sample_truncated(void *value)
{
	sample_state();
	*(uint32_t *)value = remexec_truncated;
}

static void sample_entries(void *value)
{
	sample_state();
	*(uint16_t *)value = remexec_entries;
}

static void sample_running(void *value)
{
	sample_state();
	*(uint8_t *)value = remexec_running;
}

static void sample_last_result(void *value)
{
	sample_state();
	*(int32_t *)value = remexec_last_result;
}

static void sample_last_ms(void *value)
{
	sample_state();
	*(uint32_t *)value = remexec_last_ms;
}

static void sample_last_command(void *value)
{
	sample_state();
	(void)strncpy(value, remexec_last_command, sizeof(remexec_last_command) - 1U);
	((char *)value)[sizeof(remexec_last_command) - 1U] = '\0';
}

static void sample_last_refusal(void *value)
{
	sample_state();
	(void)strncpy(value, remexec_last_refusal, KFSW_REMEXEC_REFUSAL_MAX - 1U);
	((char *)value)[KFSW_REMEXEC_REFUSAL_MAX - 1U] = '\0';
}

static const struct kfsw_param_definition remexec_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_accepted",
		.description = "Requests whose command was allowed and run",
		.value = &remexec_accepted,
		.default_value = {.u32 = 0U},
		.sample = sample_accepted,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_refused",
		.description = "Requests refused before any command ran",
		.value = &remexec_refused,
		.default_value = {.u32 = 0U},
		.sample = sample_refused,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_truncated",
		.description = "Replies that could not carry all the output",
		.value = &remexec_truncated,
		.default_value = {.u32 = 0U},
		.sample = sample_truncated,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_offered",
		.description = "Commands marked for remote execution",
		.value = &remexec_entries,
		.default_value = {.u16 = 0U},
		.sample = sample_entries,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_REMEXEC_REFUSAL_MAX,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_refusal",
		.description = "Why the most recent request was refused",
		.value = remexec_last_refusal,
		.sample = sample_last_refusal,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_running",
		.description = "Whether a command is running on this node now",
		.value = &remexec_running,
		.sample = sample_running,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_I32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_last_result",
		.description = "What the last command returned; zero before any ran",
		.value = &remexec_last_result,
		.sample = sample_last_result,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_last_ms",
		.unit = "ms",
		.description = "How long the last command took",
		.value = &remexec_last_ms,
		.sample = sample_last_ms,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_REMEXEC_COMMAND_MAX + 1U,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "remexec_last_command",
		.description = "The last command line asked for, with its arguments",
		.value = remexec_last_command,
		.sample = sample_last_command,
	},
};

const struct kfsw_param_definition_set kfsw_remexec_param_definitions = {
	.table = KFSW_REMEXEC_PARAM_TABLE_ID,
	.name = KFSW_REMEXEC_PARAM_TABLE_NAME,
	.description = "Remote shell execution and its refusals",
	.definitions = remexec_param_definitions,
	.count = ARRAY_SIZE(remexec_param_definitions),
};
