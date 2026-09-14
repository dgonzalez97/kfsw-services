#include <stdarg.h>
#include <errno.h>
#include <stddef.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <kfsw/services/log.h>
#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#define KFSW_LOG_MESSAGE_SIZE 192U

static atomic_t kfsw_log_level = ATOMIC_INIT(CONFIG_KFSW_LOG_MIN_LEVEL);

/*
 * One minimum level per module. A message has to pass both the global level
 * and its module's level.
 */
static uint8_t kfsw_log_module_levels[KFSW_LOG_MODULE_COUNT];

static const char *const kfsw_log_module_names[KFSW_LOG_MODULE_COUNT] = {
	"app", "boot",   "log",     "param", "storage", "csp", "uart", "ftp",
	"fwu", "health", "command", "event", "radio",   "hk",  "fbo",
};

const char *kfsw_log_module_name(enum kfsw_log_module module)
{
	if ((unsigned int)module >= KFSW_LOG_MODULE_COUNT) {
		return "unknown";
	}
	return kfsw_log_module_names[module];
}

uint8_t kfsw_log_get_module_level(enum kfsw_log_module module)
{
	if ((unsigned int)module >= KFSW_LOG_MODULE_COUNT) {
		return 0U;
	}
	return kfsw_log_module_levels[module];
}

int kfsw_log_set_module_level(enum kfsw_log_module module, uint8_t level)
{
	if ((unsigned int)module >= KFSW_LOG_MODULE_COUNT) {
		return -EINVAL;
	}
	if (level > 4U) {
		return -ERANGE;
	}
	kfsw_log_module_levels[module] = level;
	return 0;
}

/* Used by the write path, so not behind the parameter guard. */
static uint8_t kfsw_log_color_value = IS_ENABLED(CONFIG_KFSW_LOG_COLOR);
static atomic_t kfsw_log_emitted;
static atomic_t kfsw_log_dropped;

#if CONFIG_KFSW_PARAM
static uint8_t kfsw_log_param_value = CONFIG_KFSW_LOG_MIN_LEVEL;

static int validate_log_level(const union kfsw_param_scalar *value)
{
	return (value->u8 <= 4U) ? 0 : -ERANGE;
}

static void apply_log_level(const union kfsw_param_scalar *value)
{
	if (kfsw_log_set_level(value->u8) != 0) {
		kfsw_log_param_value = CONFIG_KFSW_LOG_MIN_LEVEL;
		(void)kfsw_log_set_level(kfsw_log_param_value);
	}
}

/* Sample the level so the table shows the level in use. */
static void sample_log_level(void *value)
{
	*(uint8_t *)value = kfsw_log_get_level();
}

static void sample_emitted(void *value)
{
	*(uint32_t *)value = (uint32_t)atomic_get(&kfsw_log_emitted);
}

static void sample_dropped(void *value)
{
	*(uint32_t *)value = (uint32_t)atomic_get(&kfsw_log_dropped);
}

static uint32_t kfsw_log_emitted_value;
static uint32_t kfsw_log_dropped_value;

static int validate_log_color(const union kfsw_param_scalar *value)
{
	return (value->u8 > 1U) ? -ERANGE : 0;
}

static uint8_t kfsw_log_levels_value[KFSW_LOG_MODULE_COUNT];

static void sample_module_levels(void *value)
{
	uint8_t *levels = value;

	for (unsigned int index = 0U; index < KFSW_LOG_MODULE_COUNT; index++) {
		levels[index] = kfsw_log_module_levels[index];
	}
}

static int validate_module_levels(const uint8_t *data, size_t size)
{
	/* Validate the whole array at once. */
	for (size_t index = 0U; index < size; index++) {
		if (data[index] > 4U) {
			return -ERANGE;
		}
	}
	return 0;
}

static void apply_module_levels(const uint8_t *data, size_t size)
{
	for (size_t index = 0U; index < size; index++) {
		(void)kfsw_log_set_module_level((enum kfsw_log_module)index, data[index]);
	}
}

static const struct kfsw_param_definition log_param_definitions[] = {
	{
		.offset = 0x00U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_PERSISTENT,
		.name = "log_level",
		.description = "Runtime logging policy value",
		.value = &kfsw_log_param_value,
		.default_value = {.u8 = CONFIG_KFSW_LOG_MIN_LEVEL},
		.validate = validate_log_level,
		.changed = apply_log_level,
		.sample = sample_log_level,
	},
	{
		.offset = 0x10U,
		.type = KFSW_PARAM_DATA,
		.capacity = KFSW_LOG_MODULE_COUNT,
		/* One level per module, in enum kfsw_log_module order. Persistent. */
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_PERSISTENT,
		.name = "log_levels",
		.description = "Minimum level per module, raising the global one for that module",
		.value = kfsw_log_levels_value,
		.validate_data = validate_module_levels,
		.changed_data = apply_module_levels,
		.sample = sample_module_levels,
	},
	{
		.offset = 0x04U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "log_emitted",
		.description = "Messages written since boot",
		.value = &kfsw_log_emitted_value,
		.sample = sample_emitted,
	},
	{
		.offset = 0x08U,
		.type = KFSW_PARAM_U32,
		.flags = KFSW_PARAM_FLAG_READ_ONLY,
		.name = "log_dropped",
		.description = "Messages below the active level, so not written",
		.value = &kfsw_log_dropped_value,
		.sample = sample_dropped,
	},
	{
		.offset = 0x0cU,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_LIVE,
		.name = "log_color",
		.description = "Colour log lines by severity; read every line",
		.value = &kfsw_log_color_value,
		.default_value = {.u8 = IS_ENABLED(CONFIG_KFSW_LOG_COLOR)},
		.validate = validate_log_color,
	},
};

const struct kfsw_param_definition_set kfsw_log_param_definitions = {
	.table = KFSW_LOG_PARAM_TABLE_ID,
	.name = KFSW_LOG_PARAM_TABLE_NAME,
	.definitions = log_param_definitions,
	.count = ARRAY_SIZE(log_param_definitions),
};
#endif

int kfsw_log_set_level(uint8_t level)
{
	if (level > 4U) {
		return -ERANGE;
	}

	atomic_set(&kfsw_log_level, level);
	return 0;
}

uint8_t kfsw_log_get_level(void)
{
	return (uint8_t)atomic_get(&kfsw_log_level);
}

#define KFSW_LOG_COLOR_RESET "\033[0m"
#define KFSW_LOG_COLOR_ERROR "\033[31m"
#define KFSW_LOG_COLOR_WARNING "\033[33m"
#define KFSW_LOG_COLOR_INFO "\033[37m"
#define KFSW_LOG_COLOR_DEBUG "\033[90m"

static const char *severity_color(uint8_t severity)
{
	if (!IS_ENABLED(CONFIG_KFSW_LOG_COLOR) || (kfsw_log_color_value == 0U)) {
		return "";
	}
	switch (severity) {
	case 3U:
		return KFSW_LOG_COLOR_ERROR;
	case 2U:
		return KFSW_LOG_COLOR_WARNING;
	case 1U:
		return KFSW_LOG_COLOR_INFO;
	default:
		return KFSW_LOG_COLOR_DEBUG;
	}
}

#if CONFIG_KFSW_LOG_MIN_LEVEL < 4
static void kfsw_log_vwrite(uint8_t module, uint8_t severity, const char *level, const char *format,
			    va_list args)
{
	char message[KFSW_LOG_MESSAGE_SIZE];
	size_t i;

	/* A message must pass the global level and its module's level. */
	if ((module < KFSW_LOG_MODULE_COUNT) && (severity < kfsw_log_module_levels[module])) {
		(void)atomic_inc(&kfsw_log_dropped);
		return;
	}
	if (severity < kfsw_log_get_level()) {
		/* Count messages dropped by the level filter. */
		(void)atomic_inc(&kfsw_log_dropped);
		return;
	}
	(void)atomic_inc(&kfsw_log_emitted);

	(void)vsnprintk(message, sizeof(message), format, args);

	for (i = 0U; message[i] != '\0'; i++) {
		if ((message[i] == '\n') || (message[i] == '\r')) {
			message[i] = ' ';
		}
	}

	/* The colour wraps the whole line so "[LEVEL] message" stays intact. */
	printk("%s[%s] %s%s\n", severity_color(severity), level, message,
	       (IS_ENABLED(CONFIG_KFSW_LOG_COLOR) && (kfsw_log_color_value != 0U))
		       ? KFSW_LOG_COLOR_RESET
		       : "");
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL < 4
/*
 * Called by the macros in the header.
 */
void kfsw_log_write(uint8_t module, uint8_t severity, const char *format, ...)
{
	static const char *const names[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
	va_list args;

	if (severity > 3U) {
		return;
	}
	va_start(args, format);
	kfsw_log_vwrite(module, severity, names[severity], format, args);
	va_end(args);
}
#endif
