#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/parameter.h>
#include <kfsw/services/table.h>

static uint32_t table_loads;
static uint32_t table_rejections;
static uint32_t table_reverts;
static uint16_t table_entries;
static uint8_t table_state;
static uint8_t table_table;
static char table_path[KFSW_TABLE_PATH_SIZE];

static void sample_state(void)
{
	struct kfsw_table_status status;

	(void)kfsw_table_get_status(&status);
	table_loads = status.loads;
	table_rejections = status.rejections;
	table_reverts = status.reverts;
	table_entries = status.entries;
	table_state = status.state;
	table_table = status.table;
	(void)strncpy(table_path, status.path, sizeof(table_path) - 1U);
	table_path[sizeof(table_path) - 1U] = '\0';
}

static void sample_loads(void *value)
{
	sample_state();
	*(uint32_t *)value = table_loads;
}

static void sample_rejections(void *value)
{
	sample_state();
	*(uint32_t *)value = table_rejections;
}

static void sample_reverts(void *value)
{
	sample_state();
	*(uint32_t *)value = table_reverts;
}

static void sample_entries(void *value)
{
	sample_state();
	*(uint16_t *)value = table_entries;
}

static void sample_table_state(void *value)
{
	sample_state();
	*(uint8_t *)value = table_state;
}

static void sample_table(void *value)
{
	sample_state();
	*(uint8_t *)value = table_table;
}

static void sample_path(void *value)
{
	sample_state();
	(void)strncpy((char *)value, table_path, sizeof(table_path) - 1U);
	((char *)value)[sizeof(table_path) - 1U] = '\0';
}

static const struct kfsw_param_definition table_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_state",
		.description = "Whether a file is loaded and can still be reverted",
		.value = &table_state,
		.default_value = {.u8 = (uint8_t)KFSW_TABLE_EMPTY},
		.sample = sample_table_state,
	},
	{
		.offset = 0x01U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_last",
		.description = "Parameter table the last loaded file addressed",
		.value = &table_table,
		.default_value = {.u8 = 0U},
		.sample = sample_table,
	},
	{
		.offset = 0x02U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_entries",
		.description = "Entries the last loaded file carried",
		.value = &table_entries,
		.default_value = {.u16 = 0U},
		.sample = sample_entries,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_loads",
		.description = "Files adopted since boot",
		.value = &table_loads,
		.default_value = {.u32 = 0U},
		.sample = sample_loads,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_rejections",
		.description = "Files refused since boot",
		.value = &table_rejections,
		.default_value = {.u32 = 0U},
		.sample = sample_rejections,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_reverts",
		.description = "Loads undone since boot",
		.value = &table_reverts,
		.default_value = {.u32 = 0U},
		.sample = sample_reverts,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_TABLE_PATH_SIZE,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "table_path",
		.description = "File of the last adopted load",
		.value = table_path,
		.sample = sample_path,
	},
};

const struct kfsw_param_definition_set kfsw_table_param_definitions = {
	.table = KFSW_TABLE_PARAM_TABLE_ID,
	.name = KFSW_TABLE_PARAM_TABLE_NAME,
	.definitions = table_param_definitions,
	.count = ARRAY_SIZE(table_param_definitions),
};
