#include <stdarg.h>
#include <errno.h>
#include <stddef.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <kfsw/services/log.h>

#define KFSW_LOG_MESSAGE_SIZE 192U

static atomic_t kfsw_log_level = ATOMIC_INIT(CONFIG_KFSW_LOG_MIN_LEVEL);

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
