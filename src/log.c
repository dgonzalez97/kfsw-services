#include <stdarg.h>
#include <stddef.h>

#include <zephyr/sys/printk.h>

#include <kfsw/services/log.h>

#define KFSW_LOG_MESSAGE_SIZE 192U

#if CONFIG_KFSW_LOG_MIN_LEVEL < 4
static void kfsw_log_vwrite(const char *level, const char *format, va_list args)
{
    char message[KFSW_LOG_MESSAGE_SIZE];
    size_t i;

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
    kfsw_log_vwrite("ERROR", format, args);
    va_end(args);
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 2
void kfsw_log_warning(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    kfsw_log_vwrite("WARNING", format, args);
    va_end(args);
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 1
void kfsw_log_info(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    kfsw_log_vwrite("INFO", format, args);
    va_end(args);
}
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 0
void kfsw_log_debug(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    kfsw_log_vwrite("DEBUG", format, args);
    va_end(args);
}
#endif
