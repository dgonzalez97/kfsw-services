#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/health.h>
#include <kfsw/services/parameter.h>

static char health_faulted_by[KFSW_HEALTH_NAME_SIZE];
static uint32_t health_faults;
static uint32_t health_feeds;
static uint16_t health_interval;
static uint8_t health_state;
static uint8_t health_feeding;
static uint8_t health_count;

static void sample_status(void)
{
	struct kfsw_health_status status;

	if (kfsw_health_get_status(&status) != 0) {
		return;
	}
	health_faults = status.faults;
	health_feeds = status.feeds;
	health_state = status.state;
	health_feeding = status.feeding ? 1U : 0U;
	health_count = status.count;
	for (size_t index = 0U; index < sizeof(health_faulted_by); index++) {
		health_faulted_by[index] = status.faulted_by[index];
	}
	health_faulted_by[sizeof(health_faulted_by) - 1U] = '\0';
}

#define HEALTH_SAMPLE(field, type)                                                                 \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_status();                                                                   \
		*(type *)value = health_##field;                                                   \
	}

HEALTH_SAMPLE(faults, uint32_t)
HEALTH_SAMPLE(feeds, uint32_t)
HEALTH_SAMPLE(state, uint8_t)
HEALTH_SAMPLE(feeding, uint8_t)
HEALTH_SAMPLE(count, uint8_t)

static void sample_faulted_by(void *value)
{
	sample_status();
	((char *)value)[0] = health_faulted_by[0];
	for (size_t index = 0U; index < KFSW_HEALTH_NAME_SIZE; index++) {
		((char *)value)[index] = health_faulted_by[index];
	}
}

static void sample_interval(void *value)
{
	*(uint16_t *)value = (uint16_t)kfsw_health_get_interval_ms();
}

/* The service owns the decision, not this table: it is the one that knows what
 * the watchdog was armed with. Refusing happens here, before the value is
 * stored, so a rejected interval is reported as rejected rather than written
 * and quietly undone.
 */
static int validate_interval(const union kfsw_param_scalar *value)
{
	return kfsw_health_check_interval_ms(value->u16);
}

static void apply_interval(const union kfsw_param_scalar *value)
{
	(void)kfsw_health_set_interval_ms(value->u16);
}

static const struct kfsw_param_definition health_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_HEALTH_NAME_SIZE,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_faulted_by",
		.description = "Component holding the system faulted, or empty",
		.value = health_faulted_by,
		.sample = sample_faulted_by,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_faults",
		.description = "Times the system was judged unwell since boot",
		.value = &health_faults,
		.sample = sample_faults,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_feeds",
		.description = "Watchdog feeds this service has issued",
		.value = &health_feeds,
		.sample = sample_feeds,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "health_interval_ms",
		.unit = "ms",
		.description = "How often deadlines are checked; refused above the feed interval",
		.value = &health_interval,
		.default_value = {.u16 = CONFIG_KFSW_HEALTH_INTERVAL_MS},
		.validate = validate_interval,
		.changed = apply_interval,
		.sample = sample_interval,
	},
	{
		.offset = 0x1aU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_state",
		.description = "ok, faulted or stopped",
		.value = &health_state,
		.sample = sample_state,
	},
	{
		.offset = 0x1bU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_feeding",
		.description = "Whether health is the one feeding the watchdog",
		.value = &health_feeding,
		.sample = sample_feeding,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "health_components",
		.description = "Components currently watched",
		.value = &health_count,
		.sample = sample_count,
	},
};

const struct kfsw_param_definition_set kfsw_health_param_definitions = {
	.table = KFSW_HEALTH_PARAM_TABLE_ID,
	.name = KFSW_HEALTH_PARAM_TABLE_NAME,
	.definitions = health_param_definitions,
	.count = ARRAY_SIZE(health_param_definitions),
};
