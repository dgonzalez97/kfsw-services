#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <kfsw/services/boot.h>
#include <kfsw/services/parameter.h>

#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
#include <zephyr/dfu/mcuboot.h>
#endif

#define KFSW_BOOT_IMAGE_SIZE 40U
/* Seven repositories, each a label, eight hex digits and a dirty mark. */
#define KFSW_BOOT_REVISIONS_SIZE KFSW_PARAM_STRING_MAX

BUILD_ASSERT(sizeof(KFSW_BUILD_REVISIONS) <= KFSW_BOOT_REVISIONS_SIZE,
	     "the revisions of every repository must fit the parameter");

static char boot_image[KFSW_BOOT_IMAGE_SIZE];
static char boot_revisions[KFSW_BOOT_REVISIONS_SIZE];
#if CONFIG_KFSW_LASTWORDS
/* Retained reset note. All zero means no valid note was found. */
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

static void copy_text(void *value, const char *source, size_t capacity)
{
	size_t length = 0U;
	char *text = value;

	while ((length + 1U < capacity) && (source[length] != '\0')) {
		text[length] = source[length];
		length++;
	}
	text[length] = '\0';
}

static void sample_image(void *value)
{
	copy_text(value, kfsw_boot_get_image_version(), KFSW_BOOT_IMAGE_SIZE);
}

static void sample_revisions(void *value)
{
	copy_text(value, kfsw_boot_get_revisions(), KFSW_BOOT_REVISIONS_SIZE);
}

static void sample_reset_cause(void *value)
{
	/* From the boot service; reading the platform register again would clear it. */
	*(uint32_t *)value = kfsw_boot_get_reset_cause();
}

static void sample_confirmed(void *value)
{
#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
	*(uint8_t *)value = boot_is_img_confirmed() ? 1U : 0U;
#else
	*(uint8_t *)value = 0U;
#endif
}

/*
 * Writing 1 confirms the running image; writing 0 does nothing. 0 is accepted
 * because it is the default, and the sample callback reads it back as 0.
 */
static int validate_confirmed(const union kfsw_param_scalar *value)
{
	if (value->u8 > 1U) {
		return -EINVAL;
	}
#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
	return 0;
#else
	/* Without a bootloader there is nothing to confirm, so a write of 1 is refused. */
	return (value->u8 == 0U) ? 0 : -ENOTSUP;
#endif
}

static void apply_confirmed(const union kfsw_param_scalar *value)
{
#if CONFIG_BOOTLOADER_MCUBOOT && CONFIG_MCUBOOT_IMG_MANAGER
	if (value->u8 == 1U) {
		(void)kfsw_boot_confirm_image();
	}
#else
	ARG_UNUSED(value);
#endif
}

static uint32_t boot_attempts;
static uint8_t boot_revert_reason;
static uint8_t boot_trial_valid;

static void sample_attempts(void *value)
{
	struct kfsw_boot_diagnostics diagnostic;

	kfsw_boot_get_diagnostics(&diagnostic);
	*(uint32_t *)value = diagnostic.attempts;
}

static void sample_revert_reason(void *value)
{
	struct kfsw_boot_diagnostics diagnostic;

	kfsw_boot_get_diagnostics(&diagnostic);
	*(uint8_t *)value = diagnostic.revert_reason;
}

static void sample_trial_valid(void *value)
{
	struct kfsw_boot_diagnostics diagnostic;

	kfsw_boot_get_diagnostics(&diagnostic);
	*(uint8_t *)value = diagnostic.valid ? 1U : 0U;
}

static const struct kfsw_param_definition boot_param_definitions[] = {
	{
		.offset = 0x2cU,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "boot_attempts",
		.description = "Persisted unconfirmed boots of this image; UINT32_MAX invalid",
		.value = &boot_attempts,
		.sample = sample_attempts,
	},
	{
		.offset = 0x29U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "boot_revert_reason",
		.description = "Service evidence: 0 none, 1 missed confirmation, 2 replaced "
			       "unknown, 255 invalid",
		.value = &boot_revert_reason,
		.sample = sample_revert_reason,
	},
	{
		.offset = 0x2aU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "boot_trial_valid",
		.description = "Trial diagnostics were recovered and saved successfully",
		.value = &boot_trial_valid,
		.sample = sample_trial_valid,
	},
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
		.offset = 0x40U,
		.type = KFSW_PARAM_STRING,
		.capacity = KFSW_BOOT_REVISIONS_SIZE,
		.flags = KFSW_PARAM_FLAG_READ_ONLY | KFSW_PARAM_FLAG_SYSTEM_INFO,
		.name = "boot_revisions",
		.description = "Short revision of each repository compiled into this image",
		.value = boot_revisions,
		.sample = sample_revisions,
	},
	{
		.offset = 0x20U,
		.type = KFSW_PARAM_U32,
		/* Read-only and persistent: saved in the snapshot but not writable. */
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
	.description = "Running image, revisions, restarts",
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
