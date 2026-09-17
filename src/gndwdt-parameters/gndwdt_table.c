#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/gndwdt.h>
#include <kfsw/services/parameter.h>

static uint8_t gndwdt_enabled = IS_ENABLED(CONFIG_KFSW_GNDWDT_ENABLED_AT_START);
static uint8_t gndwdt_running;
static uint32_t gndwdt_timeout_s = CONFIG_KFSW_GNDWDT_TIMEOUT_S;
static uint32_t gndwdt_since_s;
static uint32_t gndwdt_contacts;
static uint32_t gndwdt_expiries;
static uint16_t gndwdt_last_node;

static void sample_status(void)
{
	struct kfsw_gndwdt_status status;

	kfsw_gndwdt_get_status(&status);
	gndwdt_enabled = status.enabled ? 1U : 0U;
	gndwdt_running = status.running ? 1U : 0U;
	gndwdt_timeout_s = status.timeout_s;
	gndwdt_since_s = status.since_contact_s;
	gndwdt_contacts = status.contacts;
	gndwdt_expiries = status.expiries;
	gndwdt_last_node = status.last_node;
}

static void sample_enabled(void *value)
{
	sample_status();
	*(uint8_t *)value = gndwdt_enabled;
}

static void sample_running(void *value)
{
	sample_status();
	*(uint8_t *)value = gndwdt_running;
}

static void sample_timeout(void *value)
{
	sample_status();
	*(uint32_t *)value = gndwdt_timeout_s;
}

static void sample_since(void *value)
{
	sample_status();
	*(uint32_t *)value = gndwdt_since_s;
}

static void sample_contacts(void *value)
{
	sample_status();
	*(uint32_t *)value = gndwdt_contacts;
}

static void sample_expiries(void *value)
{
	sample_status();
	*(uint32_t *)value = gndwdt_expiries;
}

static void sample_last_node(void *value)
{
	sample_status();
	*(uint16_t *)value = gndwdt_last_node;
}

static int validate_enabled(const union kfsw_param_scalar *value)
{
	return (value->u8 <= 1U) ? 0 : -EINVAL;
}

static void apply_enabled(const union kfsw_param_scalar *value)
{
	kfsw_gndwdt_set_enabled(value->u8 != 0U);
}

static int validate_timeout(const union kfsw_param_scalar *value)
{
	if ((value->u32 < CONFIG_KFSW_GNDWDT_TIMEOUT_MIN_S) ||
	    (value->u32 > CONFIG_KFSW_GNDWDT_TIMEOUT_MAX_S)) {
		return -ERANGE;
	}
	return 0;
}

static void apply_timeout(const union kfsw_param_scalar *value)
{
	(void)kfsw_gndwdt_set_timeout_s(value->u32);
}

static const struct kfsw_param_definition gndwdt_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U8,
		.name = "gndwdt_enabled",
		.description = "Whether the countdown is armed",
		.value = &gndwdt_enabled,
		.default_value = {.u8 = IS_ENABLED(CONFIG_KFSW_GNDWDT_ENABLED_AT_START)},
		.validate = validate_enabled,
		.changed = apply_enabled,
		.sample = sample_enabled,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.unit = "s",
		.name = "gndwdt_timeout_s",
		.description = "Silence allowed before the node resets itself",
		.value = &gndwdt_timeout_s,
		.default_value = {.u32 = CONFIG_KFSW_GNDWDT_TIMEOUT_S},
		.validate = validate_timeout,
		.changed = apply_timeout,
		.sample = sample_timeout,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.unit = "s",
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "gndwdt_since_s",
		.description = "Seconds since the last contact",
		.value = &gndwdt_since_s,
		.default_value = {.u32 = 0U},
		.sample = sample_since,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "gndwdt_contacts",
		.description = "Packets counted as contact since start",
		.value = &gndwdt_contacts,
		.default_value = {.u32 = 0U},
		.sample = sample_contacts,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "gndwdt_expiries",
		.description = "Times the timeout passed since start",
		.value = &gndwdt_expiries,
		.default_value = {.u32 = 0U},
		.sample = sample_expiries,
	},
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "gndwdt_last_node",
		.description = "Node of the most recent contact",
		.value = &gndwdt_last_node,
		.default_value = {.u16 = 0U},
		.sample = sample_last_node,
	},
	{
		.offset = 0x16U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "gndwdt_running",
		.description = "Whether the service has been started",
		.value = &gndwdt_running,
		.default_value = {.u8 = 0U},
		.sample = sample_running,
	},
};

const struct kfsw_param_definition_set kfsw_gndwdt_param_definitions = {
	.table = KFSW_GNDWDT_PARAM_TABLE_ID,
	.name = KFSW_GNDWDT_PARAM_TABLE_NAME,
	.definitions = gndwdt_param_definitions,
	.count = ARRAY_SIZE(gndwdt_param_definitions),
};
