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

static void sample_state(void)
{
	struct kfsw_remexec_stats stats;

	kfsw_remexec_get_stats(&stats);
	remexec_accepted = stats.accepted;
	remexec_refused = stats.refused;
	remexec_truncated = stats.truncated;
	remexec_entries = stats.entries;
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
};

const struct kfsw_param_definition_set kfsw_remexec_param_definitions = {
	.table = KFSW_REMEXEC_PARAM_TABLE_ID,
	.name = KFSW_REMEXEC_PARAM_TABLE_NAME,
	.description = "Remote shell execution and its refusals",
	.definitions = remexec_param_definitions,
	.count = ARRAY_SIZE(remexec_param_definitions),
};
