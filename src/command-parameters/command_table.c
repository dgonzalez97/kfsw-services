#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/command.h>
#include <kfsw/services/parameter.h>

static uint32_t command_invoked;
static uint32_t command_failed;
static uint32_t command_unknown;
static uint32_t command_rejected;
#if CONFIG_KFSW_COMMAND_CSP
static uint32_t command_timeout_ms = CONFIG_KFSW_COMMAND_TIMEOUT_MS;
#endif
static uint16_t command_registered;
static uint8_t command_echo_enabled;

static void sample_stats(void)
{
	struct kfsw_command_stats stats;

	if (kfsw_command_get_stats(&stats) != 0) {
		return;
	}
	command_invoked = stats.invoked;
	command_failed = stats.failed;
	command_unknown = stats.unknown;
	command_rejected = stats.rejected;
	command_registered = stats.registered;
}

#define COMMAND_SAMPLE(field, type)                                                                \
	static void sample_##field(void *value)                                                    \
	{                                                                                          \
		sample_stats();                                                                    \
		*(type *)value = command_##field;                                                  \
	}

COMMAND_SAMPLE(invoked, uint32_t)
COMMAND_SAMPLE(failed, uint32_t)
COMMAND_SAMPLE(unknown, uint32_t)
COMMAND_SAMPLE(rejected, uint32_t)
COMMAND_SAMPLE(registered, uint16_t)

static void sample_echo(void *value)
{
	*(uint8_t *)value = kfsw_command_echo_enabled() ? 1U : 0U;
}

static int validate_echo(const union kfsw_param_scalar *value)
{
	return (value->u8 > 1U) ? -ERANGE : 0;
}

static void apply_echo(const union kfsw_param_scalar *value)
{
	kfsw_command_set_echo(value->u8 != 0U);
}

/* Only with the remote path: without it there is no invocation to time out. */
#if CONFIG_KFSW_COMMAND_CSP
static void sample_timeout(void *value)
{
	*(uint32_t *)value = kfsw_command_get_timeout_ms();
}

static int validate_timeout(const union kfsw_param_scalar *value)
{
	return kfsw_command_check_timeout_ms(value->u32);
}

static void apply_timeout(const union kfsw_param_scalar *value)
{
	(void)kfsw_command_set_timeout_ms(value->u32);
}
#endif

static const struct kfsw_param_definition command_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "cmd_invoked",
		.description = "Invocations that reached a handler or were refused before one",
		.value = &command_invoked,
		.sample = sample_invoked,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "cmd_failed",
		.description = "Invocations whose handler reported a failure",
		.value = &command_failed,
		.sample = sample_failed,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "cmd_unknown",
		.description = "Invocations naming a command that is not registered",
		.value = &command_unknown,
		.sample = sample_unknown,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "cmd_rejected",
		.description = "Invocations refused before the handler ran",
		.value = &command_rejected,
		.sample = sample_rejected,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_U16,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "cmd_registered",
		.description = "Commands in the frozen registry",
		.value = &command_registered,
		.sample = sample_registered,
	},
#if CONFIG_KFSW_COMMAND_CSP
	{
		.offset = 0x14U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "cmd_timeout_ms",
		.unit = "ms",
		.description = "Timeout for new remote invocations",
		.value = &command_timeout_ms,
		.default_value = {.u32 = CONFIG_KFSW_COMMAND_TIMEOUT_MS},
		.validate = validate_timeout,
		.changed = apply_timeout,
		.sample = sample_timeout,
	},
#endif
	{
		.offset = 0x18U,
		.type = KFSW_PARAM_U8,
		/* Off by default: the shell repeats every input byte, so a
		 * session driven by a script shows each command twice. Live,
		 * because it is worth toggling while watching the console. */
		.flags = KFSW_PARAM_FLAG_CONFIGURATION,
		.name = "echo_enabled",
		.description = "Console repeats what is typed at it",
		.value = &command_echo_enabled,
		.default_value = {.u8 = 0U},
		.validate = validate_echo,
		.changed = apply_echo,
		.sample = sample_echo,
	},
};

const struct kfsw_param_definition_set kfsw_command_param_definitions = {
	.table = KFSW_COMMAND_PARAM_TABLE_ID,
	.name = KFSW_COMMAND_PARAM_TABLE_NAME,
	.definitions = command_param_definitions,
	.count = ARRAY_SIZE(command_param_definitions),
};
