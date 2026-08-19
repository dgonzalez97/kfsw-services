#ifndef KFSW_SERVICES_LOG_H
#define KFSW_SERVICES_LOG_H

#if defined(__GNUC__) || defined(__clang__)
#define KFSW_LOG_PRINTF_LIKE(format_index, first_arg) \
    __attribute__((format(printf, format_index, first_arg)))
#else
#define KFSW_LOG_PRINTF_LIKE(format_index, first_arg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 3
/**
 * @brief Log an error message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
void kfsw_log_error(const char *format, ...)
    KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_error(...) do { } while (0)
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 2
/**
 * @brief Log a warning message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
void kfsw_log_warning(const char *format, ...)
    KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_warning(...) do { } while (0)
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 1
/**
 * @brief Log an informational message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
void kfsw_log_info(const char *format, ...)
    KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_info(...) do { } while (0)
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 0
/**
 * @brief Log a debug message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
void kfsw_log_debug(const char *format, ...)
    KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_debug(...) do { } while (0)
#endif

#ifdef __cplusplus
}
#endif

#undef KFSW_LOG_PRINTF_LIKE

#endif
