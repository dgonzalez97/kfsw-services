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

/* Used by the write path, which is compiled whatever the parameter service is
 * doing, so these cannot live behind the parameter guard.
 */
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

/* The level lives in the atomic; this is only the parameter's view of it. A
 * direct kfsw_log_set_level() call moves the atomic without going through the
 * parameter path, so without sampling the table would report a level the system
 * is not using.
 */
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

/* Jade for the prompt is the shell's business; here severity is the only thing
 * worth colouring, because it is what a reader scans for.
 */
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
static void kfsw_log_vwrite(uint8_t severity, const char *level, const char *format, va_list args)
{
	char message[KFSW_LOG_MESSAGE_SIZE];
	size_t i;

	if (severity < kfsw_log_get_level()) {
		/* Counted rather than passed over silently: a console that has
		 * gone quiet because the level was raised looks exactly like one
		 * that has gone quiet because the system stopped. */
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

	/* The colour brackets the whole line rather than sitting inside it, so
	 * "[LEVEL] message" is still one contiguous run of text for anything
	 * matching on it.
	 */
	printk("%s[%s] %s%s\n", severity_color(severity), level, message,
	       (IS_ENABLED(CONFIG_KFSW_LOG_COLOR) && (kfsw_log_color_value != 0U))
		       ? KFSW_LOG_COLOR_RESET
		       : "");
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 3
void kfsw_log_error(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	kfsw_log_vwrite(3U, "ERROR", format, args);
	va_end(args);
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 2
void kfsw_log_warning(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	kfsw_log_vwrite(2U, "WARNING", format, args);
	va_end(args);
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 1
void kfsw_log_info(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	kfsw_log_vwrite(1U, "INFO", format, args);
	va_end(args);
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 0
void kfsw_log_debug(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	kfsw_log_vwrite(0U, "DEBUG", format, args);
	va_end(args);
}
#endif
