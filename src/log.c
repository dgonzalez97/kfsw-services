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

static const struct kfsw_param_definition log_param_definitions[] = {
	{
		.id = 1U,
		.type = KFSW_PARAM_U8,
		.flags = KFSW_PARAM_FLAG_CONFIGURATION | KFSW_PARAM_FLAG_PERSISTENT,
		.name = "log_level",
		.description = "Runtime logging policy value",
		.value = &kfsw_log_param_value,
		.default_value = {.u8 = CONFIG_KFSW_LOG_MIN_LEVEL},
		.validate = validate_log_level,
		.changed = apply_log_level,
	},
};

const struct kfsw_param_definition_set kfsw_log_param_definitions = {
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

#if CONFIG_KFSW_LOG_MIN_LEVEL < 4
static void kfsw_log_vwrite(uint8_t severity, const char *level, const char *format, va_list args)
{
	char message[KFSW_LOG_MESSAGE_SIZE];
	size_t i;

	if (severity < kfsw_log_get_level()) {
		return;
	}

	(void)vsnprintk(message, sizeof(message), format, args);

	for (i = 0U; message[i] != '\0'; i++) {
		if ((message[i] == '\n') || (message[i] == '\r')) {
			message[i] = ' ';
		}
	}

	printk("[%s] %s\n", level, message);
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
