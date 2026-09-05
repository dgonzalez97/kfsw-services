#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/parameter.h>

/*
 * The parameter service describing itself. Worth having because an empty table
 * and a table whose snapshot failed to load look identical from the ground
 * otherwise: both answer every read with a compiled default.
 */

static uint16_t param_count;
static uint16_t param_tables;
static uint16_t param_persistent;
static uint32_t param_saves;
static uint32_t param_load_failures;
static uint8_t param_autosave;

static void sample_stats(void)
{
	struct kfsw_param_stats stats;

	if (kfsw_param_get_stats(&stats) != 0) {
		return;
	}
	param_count = stats.count;
	param_tables = stats.tables;
	param_persistent = stats.persistent;
	param_saves = stats.saves;
	param_load_failures = stats.load_failures;
}

#define PARAM_SAMPLE(field, type)                                                                  \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_stats();                                                                    \
		*(type *)value = param_##field;                                                    \
	}

PARAM_SAMPLE(count, uint16_t)
PARAM_SAMPLE(tables, uint16_t)
PARAM_SAMPLE(persistent, uint16_t)
PARAM_SAMPLE(saves, uint32_t)
PARAM_SAMPLE(load_failures, uint32_t)

static int validate_autosave(const union kfsw_param_scalar *value)
{
	return (value->u8 > 1U) ? -ERANGE : 0;
}

static const struct kfsw_param_definition param_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_count",
		.description = "Parameters registered across every table",
		.value = &param_count,
		.sample = sample_count,
	},
	{
		.offset = 0x02U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_tables",
		.description = "Tables registered",
		.value = &param_tables,
		.sample = sample_tables,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_persistent",
		.description = "Of those, the ones a snapshot carries",
		.value = &param_persistent,
		.sample = sample_persistent,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_saves",
		.description = "Snapshots written since boot",
		.value = &param_saves,
		.sample = sample_saves,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_load_failures",
		.description = "Snapshot loads that failed for a reason other than absence",
		.value = &param_load_failures,
		.sample = sample_load_failures,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U8,
		/* Stored as well as live: an operator who turns this on wants it
		 * to survive the reboot they are about to cause. */
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_PERSISTENT |
			 KFSW_PARAM_FLAG_LIVE,
		.name = "param_autosave",
		.description = "Write a snapshot after every accepted change",
		.value = &param_autosave,
		.default_value = {.u8 = 0U},
		.validate = validate_autosave,
	},
};

const struct kfsw_param_definition_set kfsw_param_param_definitions = {
	.table = KFSW_PARAM_PARAM_TABLE_ID,
	.name = KFSW_PARAM_PARAM_TABLE_NAME,
	.definitions = param_param_definitions,
	.count = ARRAY_SIZE(param_param_definitions),
};

bool kfsw_param_autosave_enabled(void)
{
	return param_autosave != 0U;
}
