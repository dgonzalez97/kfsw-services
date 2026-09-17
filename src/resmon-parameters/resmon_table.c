#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/parameter.h>
#include <kfsw/services/resmon.h>

static uint32_t resmon_sweeps;
static uint32_t resmon_alerts;
static uint32_t resmon_worst_percent;
static uint32_t resmon_last_percent;
static uint32_t resmon_worst_free;
static uint32_t resmon_worst_size;
static uint32_t resmon_alert_percent = CONFIG_KFSW_RESMON_ALERT_PERCENT;
static uint16_t resmon_threads;
static uint8_t resmon_running;
static char resmon_worst_thread[KFSW_RESMON_NAME_SIZE];

static void sample_state(void)
{
	struct kfsw_resmon_status status;

	kfsw_resmon_get_status(&status);
	resmon_sweeps = status.sweeps;
	resmon_alerts = status.alerts;
	resmon_worst_percent = status.worst_used_percent;
	resmon_last_percent = status.last_used_percent;
	resmon_worst_free = status.worst_unused_bytes;
	resmon_worst_size = status.worst_stack_bytes;
	resmon_alert_percent = status.alert_percent;
	resmon_threads = status.threads;
	resmon_running = status.running ? 1U : 0U;
	(void)strncpy(resmon_worst_thread, status.worst_thread, KFSW_RESMON_NAME_SIZE - 1U);
	resmon_worst_thread[KFSW_RESMON_NAME_SIZE - 1U] = '\0';
}

static void sample_sweeps(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_sweeps;
}

static void sample_alerts(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_alerts;
}

static void sample_worst_percent(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_worst_percent;
}

static void sample_last_percent(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_last_percent;
}

static void sample_worst_free(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_worst_free;
}

static void sample_worst_size(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_worst_size;
}

static void sample_alert_percent(void *value)
{
	sample_state();
	*(uint32_t *)value = resmon_alert_percent;
}

static void sample_threads(void *value)
{
	sample_state();
	*(uint16_t *)value = resmon_threads;
}

static void sample_running(void *value)
{
	sample_state();
	*(uint8_t *)value = resmon_running;
}

static void sample_worst_thread(void *value)
{
	sample_state();
	(void)strncpy(value, resmon_worst_thread, KFSW_RESMON_NAME_SIZE - 1U);
	((char *)value)[KFSW_RESMON_NAME_SIZE - 1U] = '\0';
}

static int validate_alert_percent(const union kfsw_param_scalar *value)
{
	if ((value->u32 == 0U) || (value->u32 > 100U)) {
		return -ERANGE;
	}
	return 0;
}

static void apply_alert_percent(const union kfsw_param_scalar *value)
{
	(void)kfsw_resmon_set_alert_percent(value->u32);
}

static const struct kfsw_param_definition resmon_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U32,
		.unit = "%",
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_worst_used",
		.description = "Highest stack use seen on any thread since start",
		.value = &resmon_worst_percent,
		.default_value = {.u32 = 0U},
		.sample = sample_worst_percent,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.unit = "%",
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_last_used",
		.description = "Highest stack use in the most recent sweep",
		.value = &resmon_last_percent,
		.default_value = {.u32 = 0U},
		.sample = sample_last_percent,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.unit = "%",
		.name = "stack_alert_used",
		.description = "A thread at or above this raises an event",
		.value = &resmon_alert_percent,
		.default_value = {.u32 = CONFIG_KFSW_RESMON_ALERT_PERCENT},
		.validate = validate_alert_percent,
		.changed = apply_alert_percent,
		.sample = sample_alert_percent,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.unit = "B",
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_worst_free",
		.description = "Unused bytes on the busiest thread",
		.value = &resmon_worst_free,
		.default_value = {.u32 = 0U},
		.sample = sample_worst_free,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U32,
		.unit = "B",
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_worst_size",
		.description = "Stack size of the busiest thread",
		.value = &resmon_worst_size,
		.default_value = {.u32 = 0U},
		.sample = sample_worst_size,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_sweeps",
		.description = "Sweeps completed since start",
		.value = &resmon_sweeps,
		.default_value = {.u32 = 0U},
		.sample = sample_sweeps,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_alerts",
		.description = "Times a sweep first found a thread at the alert level",
		.value = &resmon_alerts,
		.default_value = {.u32 = 0U},
		.sample = sample_alerts,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_threads",
		.description = "Threads the last sweep could read",
		.value = &resmon_threads,
		.default_value = {.u16 = 0U},
		.sample = sample_threads,
	},
	{
		.offset = 0x1eU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_running",
		.description = "Whether the periodic sweep is running",
		.value = &resmon_running,
		.default_value = {.u8 = 0U},
		.sample = sample_running,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_RESMON_NAME_SIZE,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "stack_worst_thread",
		.description = "Thread holding the highest stack use",
		.value = resmon_worst_thread,
		.sample = sample_worst_thread,
	},
};

const struct kfsw_param_definition_set kfsw_resmon_param_definitions = {
	.table = KFSW_RESMON_PARAM_TABLE_ID,
	.name = KFSW_RESMON_PARAM_TABLE_NAME,
	.definitions = resmon_param_definitions,
	.count = ARRAY_SIZE(resmon_param_definitions),
};
