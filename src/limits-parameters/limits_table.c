#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/limits.h>
#include <kfsw/services/parameter.h>

static uint8_t limits_rules;
static uint8_t limits_running;
static uint8_t limits_last_index;
static uint16_t limits_error_line;
static uint32_t limits_sweeps;
static uint32_t limits_fires;
static uint32_t limits_failures;
static uint32_t limits_parse_errors;
static uint32_t limits_last_ms;

static void sample_state(void)
{
	struct kfsw_limits_status status;

	kfsw_limits_get_status(&status);
	limits_rules = status.rules;
	limits_running = status.running ? 1U : 0U;
	limits_last_index = status.last_fired_index;
	limits_error_line = status.error_line;
	limits_sweeps = status.sweeps;
	limits_fires = status.fires;
	limits_failures = status.failures;
	limits_parse_errors = status.parse_errors;
	limits_last_ms = status.last_fired_ms;
}

static void sample_rules(void *value)
{
	sample_state();
	*(uint8_t *)value = limits_rules;
}

static void sample_running(void *value)
{
	sample_state();
	*(uint8_t *)value = limits_running;
}

static void sample_last_index(void *value)
{
	sample_state();
	*(uint8_t *)value = limits_last_index;
}

static void sample_error_line(void *value)
{
	sample_state();
	*(uint16_t *)value = limits_error_line;
}

static void sample_sweeps(void *value)
{
	sample_state();
	*(uint32_t *)value = limits_sweeps;
}

static void sample_fires(void *value)
{
	sample_state();
	*(uint32_t *)value = limits_fires;
}

static void sample_failures(void *value)
{
	sample_state();
	*(uint32_t *)value = limits_failures;
}

static void sample_parse_errors(void *value)
{
	sample_state();
	*(uint32_t *)value = limits_parse_errors;
}

static void sample_last_ms(void *value)
{
	sample_state();
	*(uint32_t *)value = limits_last_ms;
}

static int validate_enabled(const union kfsw_param_scalar *value)
{
	return (value->u8 > 1U) ? -ERANGE : 0;
}

static int validate_interval(const union kfsw_param_scalar *value)
{
	if ((value->u32 != 0U) && ((value->u32 < KFSW_LIMITS_INTERVAL_MIN_MS) ||
				   (value->u32 > KFSW_LIMITS_INTERVAL_MAX_MS))) {
		return -ERANGE;
	}
	return 0;
}

static void apply_interval(const union kfsw_param_scalar *value)
{
	(void)kfsw_limits_set_interval_ms(value->u32);
}

static const struct kfsw_param_definition limits_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_rules",
		.description = "Rules in force",
		.value = &limits_rules,
		.default_value = {.u8 = 0U},
		.sample = sample_rules,
	},
	{
		.offset = 0x01U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_running",
		.description = "Whether the periodic sweep is scheduled",
		.value = &limits_running,
		.default_value = {.u8 = 0U},
		.sample = sample_running,
	},
	{
		.offset = 0x02U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_PERSISTENT | KFSW_PARAM_FLAG_LIVE,
		.name = "limits_enabled",
		.description = "0 stops the sweeps and keeps the loaded rules",
		.value = &kfsw_limits_enabled,
		.default_value = {.u8 = 1U},
		.validate = validate_enabled,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.unit = "ms",
		.flags = KFSW_PARAM_FLAG_PERSISTENT | KFSW_PARAM_FLAG_LIVE,
		.name = "limits_interval_ms",
		.description = "Between periodic sweeps; 0 or 100 to 3600000",
		.value = &kfsw_limits_interval_ms,
		.default_value = {.u32 = CONFIG_KFSW_LIMITS_INTERVAL_MS},
		.validate = validate_interval,
		.changed = apply_interval,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_sweeps",
		.description = "Sweeps completed while enabled",
		.value = &limits_sweeps,
		.default_value = {.u32 = 0U},
		.sample = sample_sweeps,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_fires",
		.description = "Actions released",
		.value = &limits_fires,
		.default_value = {.u32 = 0U},
		.sample = sample_fires,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_failures",
		.description = "Parameter reads and releases that failed",
		.value = &limits_failures,
		.default_value = {.u32 = 0U},
		.sample = sample_failures,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_parse_errors",
		.description = "Rule files refused for a bad line",
		.value = &limits_parse_errors,
		.default_value = {.u32 = 0U},
		.sample = sample_parse_errors,
	},
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U32,
		.unit = "ms",
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_last_fired_ms",
		.description = "Uptime of the last release; 0 before any",
		.value = &limits_last_ms,
		.default_value = {.u32 = 0U},
		.sample = sample_last_ms,
	},
	{
		.offset = 0x1cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_last_fired",
		.description = "Index of the rule that fired last; 255 before any",
		.value = &limits_last_index,
		.default_value = {.u8 = KFSW_LIMITS_NO_RULE},
		.sample = sample_last_index,
	},
	{
		.offset = 0x1eU,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "limits_error_line",
		.description = "Line that got the last file refused; 0 for none",
		.value = &limits_error_line,
		.default_value = {.u16 = 0U},
		.sample = sample_error_line,
	},
};

KFSW_PARAM_TABLE_FITS_THE_LINK(limits_param_definitions);

const struct kfsw_param_definition_set kfsw_limits_param_definitions = {
	.table = KFSW_LIMITS_PARAM_TABLE_ID,
	.name = KFSW_LIMITS_PARAM_TABLE_NAME,
	.description = "Parameter limits",
	.definitions = limits_param_definitions,
	.count = ARRAY_SIZE(limits_param_definitions),
};
