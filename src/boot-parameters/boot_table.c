#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/boot.h>
#include <kfsw/services/parameter.h>

#if CONFIG_KFSW_FWU_MCUBOOT
#include <zephyr/dfu/mcuboot.h>
#endif

#define KFSW_BOOT_IMAGE_SIZE 40U

static char boot_image[KFSW_BOOT_IMAGE_SIZE];
#if CONFIG_KFSW_LASTWORDS
/* What the previous run said, published so a pass can read it without holding a
 * console open. Zero for every one of them means the node lost power outright,
 * which is itself an answer.
 */
static uint8_t boot_last_reason;
static uint32_t boot_last_detail;
static uint32_t boot_last_uptime_ms;

static void sample_last_reason(void *value)
{
	*(uint8_t *)value = (uint8_t)kfsw_boot_get_lastwords()->reason;
}

static void sample_last_detail(void *value)
{
	*(uint32_t *)value = kfsw_boot_get_lastwords()->detail;
}

static void sample_last_uptime_ms(void *value)
{
	*(uint32_t *)value = kfsw_boot_get_lastwords()->uptime_ms;
}
#endif
static uint32_t boot_count;
static uint32_t boot_reset_cause;
static uint8_t boot_confirmed;

static void sample_image(void *value)
{
	const char *version = kfsw_boot_get_image_version();
	size_t length = 0U;
	char *text = value;

	while ((length + 1U < KFSW_BOOT_IMAGE_SIZE) && (version[length] != '\0')) {
		text[length] = version[length];
		length++;
	}
	text[length] = '\0';
}

static void sample_reset_cause(void *value)
{
	/* Taken from the latch the boot service holds, not from the platform
	 * again: reading the cause clears it, and boot is the first reader.
	 * Calling the platform here would report an empty register as the
	 * cause of the reset.
	 */
	*(uint32_t *)value = kfsw_boot_get_reset_cause();
}

static void sample_confirmed(void *value)
{
#if CONFIG_KFSW_FWU_MCUBOOT
	*(uint8_t *)value = boot_is_img_confirmed() ? 1U : 0U;
#else
	*(uint8_t *)value = 0U;
#endif
}

/*
 * A request to confirm, not a mirror of the flag. Writing 1 marks the running
 * image good; writing 0 asks for nothing and changes nothing, because
 * un-confirming an image that has already proven itself has no honest meaning
 * and would arm a revert nobody asked for.
 *
 * Zero has to be accepted rather than refused: it is the compiled default, and
 * a parameter that refuses its own default cannot be registered at all. What
 * keeps this honest is the sample callback, which reports what the bootloader
 * actually holds -- so a write of 0 is visible as a no-op on the very next
 * read rather than being mistaken for a cleared flag.
 */
static int validate_confirmed(const union kfsw_param_scalar *value)
{
	if (value->u8 > 1U) {
		return -EINVAL;
	}
#if CONFIG_KFSW_FWU_MCUBOOT
	return 0;
#else
	/* Without a bootloader there is nothing to confirm. Accepting the
	 * default lets the table register; asking to confirm is refused,
	 * because reporting success would suggest an image had been marked
	 * good when nothing records that. */
	return (value->u8 == 0U) ? 0 : -ENOTSUP;
#endif
}

static void apply_confirmed(const union kfsw_param_scalar *value)
{
#if CONFIG_KFSW_FWU_MCUBOOT
	if (value->u8 == 1U) {
		(void)boot_write_img_confirmed();
	}
#else
	ARG_UNUSED(value);
#endif
}

static const struct kfsw_param_definition boot_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_BOOT_IMAGE_SIZE,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_SYSTEM_INFO,
		.name = "boot_image",
		.description = "Version of the running image, from the build",
		.value = boot_image,
		.sample = sample_image,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_U32,
		/* Persistent and read-only together: the service writes it, a
		 * snapshot carries it, and an operator cannot rewrite the
		 * history of how many times the node has restarted. */
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_PERSISTENT,
		.name = "boot_count",
		.description = "Restarts recorded across the life of the node",
		.value = &boot_count,
		.default_value = {.u32 = 0U},
	},
	{
		.offset = 0x24U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "boot_reset_cause",
		.description = "Cause latched at boot, before the flags were cleared",
		.value = &boot_reset_cause,
		.sample = sample_reset_cause,
	},
#if CONFIG_KFSW_LASTWORDS
	{
		.offset = 0x30U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "last_reason",
		.description = "Why the previous run ended: 0 said nothing, 1 commanded, "
			       "2 brownout, 3 fatal, 4 starved, 5 unknown",
		.value = &boot_last_reason,
		.sample = sample_last_reason,
	},
	{
		.offset = 0x34U,
		.type = KFSW_PARAM_X32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "last_detail",
		.description = "Meaningful with the reason: the faulting address for a fatal",
		.value = &boot_last_detail,
		.sample = sample_last_detail,
	},
	{
		.offset = 0x38U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "last_uptime_ms",
		.unit = "ms",
		.description = "How long the previous run had been up when it went away",
		.value = &boot_last_uptime_ms,
		.sample = sample_last_uptime_ms,
	},
#endif
	{
		.offset = 0x28U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_LIVE,
		.name = "boot_confirmed",
		.description = "Mark the running image good; can only be set, never cleared",
		.value = &boot_confirmed,
		.default_value = {.u8 = 0U},
		.validate = validate_confirmed,
		.changed = apply_confirmed,
		.sample = sample_confirmed,
	},
};

const struct kfsw_param_definition_set kfsw_boot_param_definitions = {
	.table = KFSW_BOOT_PARAM_TABLE_ID,
	.name = KFSW_BOOT_PARAM_TABLE_NAME,
	.definitions = boot_param_definitions,
	.count = ARRAY_SIZE(boot_param_definitions),
};

void kfsw_boot_count_restart(void)
{
	if (boot_count < UINT32_MAX) {
		boot_count++;
	}
}

uint32_t kfsw_boot_get_count(void)
{
	return boot_count;
}
