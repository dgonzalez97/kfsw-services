#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/parameter.h>

/*
 * Parameter service counters and settings.
 */

static uint16_t param_count;
static uint16_t param_tables;
static uint16_t param_persistent;
static uint32_t param_saves;
static uint32_t param_load_failures;
static uint32_t param_requests_dropped;
static uint8_t param_autosave;
#if CONFIG_KFSW_PARAM_PERSISTENCE
static uint32_t param_persist_bytes;
static uint32_t param_persist_max_bytes;
#endif

static void sample_stats(void)
{
	struct kfsw_param_stats stats;

	if (kfsw_param_get_stats(&stats) != 0) {
		return;
	}
	param_count = stats.count;
	param_tables = stats.tables;
	param_persistent = stats.persistent;
#if CONFIG_KFSW_PARAM_PERSISTENCE
	param_persist_bytes = kfsw_param_persist_bytes();
	param_persist_max_bytes = kfsw_param_persist_max_bytes();
#endif
	param_saves = stats.saves;
	param_load_failures = stats.load_failures;
	param_requests_dropped = stats.requests_dropped;
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
PARAM_SAMPLE(requests_dropped, uint32_t)

static int validate_autosave(const union kfsw_param_scalar *value)
{
	return (value->u8 > 1U) ? -ERANGE : 0;
}

#if CONFIG_KFSW_PARAM_PERSISTENCE
static void sample_persist_bytes(void *value)
{
	sample_stats();
	*(uint32_t *)value = param_persist_bytes;
}

static void sample_persist_max_bytes(void *value)
{
	sample_stats();
	*(uint32_t *)value = param_persist_max_bytes;
}
#endif

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
		/* Stored and live. */
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_PERSISTENT |
			 KFSW_PARAM_FLAG_LIVE,
		.name = "param_autosave",
		.description = "Write a snapshot after every accepted change",
		.value = &param_autosave,
		/* On by default: an accepted change to a persistent value writes the snapshot. */
		.default_value = {.u8 = 1U},
		.validate = validate_autosave,
	},
#if CONFIG_KFSW_PARAM_PERSISTENCE
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_persist_bytes",
		.description = "Space the last snapshot occupied",
		.value = &param_persist_bytes,
		.sample = sample_persist_bytes,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		/* Snapshot size limit, also the size of its buffer. */
		.name = "param_persist_max_bytes",
		.description = "Most space a snapshot is allowed",
		.value = &param_persist_max_bytes,
		.sample = sample_persist_max_bytes,
	},
#endif
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "param_requests_dropped",
		.description = "Value requests dropped before worker admission",
		.value = &param_requests_dropped,
		.sample = sample_requests_dropped,
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
