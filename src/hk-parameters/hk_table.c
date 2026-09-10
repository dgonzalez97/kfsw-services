#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/hk.h>
#include <kfsw/services/parameter.h>

/* What the service counts, published so an operator can see whether collection
 * is happening at all before wondering why a report reads oddly. `overwritten`
 * is the one worth watching: it says the ring wrapped, so the interesting
 * minutes may already be gone.
 */
static uint8_t hk_reports;
static uint8_t hk_enabled = 1U;
static uint32_t hk_collections;
static uint32_t hk_failures;
static uint32_t hk_entries_failed;
static uint32_t hk_overwritten;
static uint32_t hk_last_seconds;
static uint32_t hk_period_floor_ms = CONFIG_KFSW_HK_PERIOD_FLOOR_MS;
static uint8_t hk_history = CONFIG_KFSW_HK_HISTORY;
static uint8_t hk_max_entries = CONFIG_KFSW_HK_ENTRIES;
static uint8_t hk_clock_valid;

static void sample_stats(void)
{
	struct kfsw_hk_stats stats;

	kfsw_hk_get_stats(&stats);
	hk_reports = stats.reports;
	hk_collections = stats.collections;
	hk_failures = stats.failures;
	hk_entries_failed = stats.entries_failed;
	hk_overwritten = stats.overwritten;
	hk_last_seconds = stats.last_seconds;
	hk_clock_valid = stats.clock_valid ? 1U : 0U;
}

static void sample_reports(void *value)
{
	sample_stats();
	*(uint8_t *)value = hk_reports;
}

static void sample_collections(void *value)
{
	sample_stats();
	*(uint32_t *)value = hk_collections;
}

static void sample_failures(void *value)
{
	sample_stats();
	*(uint32_t *)value = hk_failures;
}

static void sample_entries_failed(void *value)
{
	sample_stats();
	*(uint32_t *)value = hk_entries_failed;
}

static void sample_overwritten(void *value)
{
	sample_stats();
	*(uint32_t *)value = hk_overwritten;
}

static void sample_last_seconds(void *value)
{
	sample_stats();
	*(uint32_t *)value = hk_last_seconds;
}

static void sample_clock_valid(void *value)
{
	sample_stats();
	*(uint8_t *)value = hk_clock_valid;
}

static const struct kfsw_param_definition hk_param_definitions[] = {
	{
		.offset = 0x00,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_reports",
		.description = "Reports currently defined",
		.value = &hk_reports,
		.sample = sample_reports,
	},
	{
		.offset = 0x01,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "hk_enabled",
		.description = "Collect periodic reports",
		.value = &hk_enabled,
		.default_value.u8 = 1U,
	},
	{
		.offset = 0x02,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_collections",
		.description = "Reports collected since boot",
		.value = &hk_collections,
		.sample = sample_collections,
	},
	{
		.offset = 0x06,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_failures",
		.description = "Collections that produced nothing",
		.value = &hk_failures,
		.sample = sample_failures,
	},
	{
		.offset = 0x0a,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_entries_failed",
		.description = "Values that could not be read and were left absent",
		.value = &hk_entries_failed,
		.sample = sample_entries_failed,
	},
	{
		.offset = 0x0e,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_overwritten",
		.description = "Samples the ring dropped to make room",
		.value = &hk_overwritten,
		.sample = sample_overwritten,
	},
	{
		.offset = 0x12,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.unit = "s",
		.name = "hk_last_seconds",
		.description = "When the last collection started, UTC",
		.value = &hk_last_seconds,
		.sample = sample_last_seconds,
	},
	{
		.offset = 0x16,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_LIVE,
		.name = "hk_clock_valid",
		/* The one to check when a report is defined and enabled and the
		 * ring is still empty: nothing is collected on a schedule until
		 * the node has been told the time.
		 */
		.description = "Whether the node has a clock, and so collects on its period",
		.value = &hk_clock_valid,
		.sample = sample_clock_valid,
	},
	{
		.offset = 0x1a,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.unit = "ms",
		.name = "hk_period_floor_ms",
		.description = "Shortest period a report may be given",
		.value = &hk_period_floor_ms,
		.default_value.u32 = CONFIG_KFSW_HK_PERIOD_FLOOR_MS,
	},
	{
		.offset = 0x1e,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_history",
		.description = "Samples each report keeps",
		.value = &hk_history,
		.default_value.u8 = CONFIG_KFSW_HK_HISTORY,
	},
	{
		.offset = 0x1f,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "hk_max_entries",
		.description = "Values one report may name",
		.value = &hk_max_entries,
		.default_value.u8 = CONFIG_KFSW_HK_ENTRIES,
	},
};

const struct kfsw_param_definition_set kfsw_hk_param_definitions = {
	.table = KFSW_HK_PARAM_TABLE_ID,
	.name = KFSW_HK_PARAM_TABLE_NAME,
	.definitions = hk_param_definitions,
	.count = ARRAY_SIZE(hk_param_definitions),
};
