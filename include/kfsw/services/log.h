#ifndef KFSW_SERVICES_LOG_H
#define KFSW_SERVICES_LOG_H

#include <stdint.h>

#if CONFIG_KFSW_PARAM
#include <kfsw/services/parameter.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define KFSW_LOG_PRINTF_LIKE(format_index, first_arg)                                              \
	__attribute__((format(printf, format_index, first_arg)))
#else
#define KFSW_LOG_PRINTF_LIKE(format_index, first_arg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Modules a message can be attributed to.
 *
 * Every slot is allocated now, including for components that do not log yet,
 * because the number is a position in the level array and in the snapshot. A
 * renumbering would silently move every stored level onto a different module.
 */
enum kfsw_log_module {
	KFSW_LOG_MODULE_APP = 0,
	KFSW_LOG_MODULE_BOOT,
	KFSW_LOG_MODULE_LOG,
	KFSW_LOG_MODULE_PARAM,
	KFSW_LOG_MODULE_STORAGE,
	KFSW_LOG_MODULE_CSP,
	KFSW_LOG_MODULE_UART,
	KFSW_LOG_MODULE_FTP,
	KFSW_LOG_MODULE_FWU,
	KFSW_LOG_MODULE_HEALTH,
	KFSW_LOG_MODULE_COMMAND,
	KFSW_LOG_MODULE_EVENT,
	KFSW_LOG_MODULE_RADIO,
	KFSW_LOG_MODULE_COUNT,
};

/*
 * A file says which module it is by defining KFSW_LOG_MODULE before including
 * this header. Anything that does not is attributed to the application, which
 * is the honest default: an unattributed message is the composition's.
 */
#ifndef KFSW_LOG_MODULE
#define KFSW_LOG_MODULE KFSW_LOG_MODULE_APP
#endif

/** Short stable name for a module, as the shell and the docs print it. */
const char *kfsw_log_module_name(enum kfsw_log_module module);

/** Minimum level for one module. */
uint8_t kfsw_log_get_module_level(enum kfsw_log_module module);

/**
 * @brief Set the minimum level for one module.
 *
 * @retval 0 Applied.
 * @retval -EINVAL Unknown module.
 * @retval -ERANGE Level above 4.
 */
int kfsw_log_set_module_level(enum kfsw_log_module module, uint8_t level);

/** Write one message. Called through the kfsw_log_* macros, not directly. */
void kfsw_log_write(uint8_t module, uint8_t severity, const char *format, ...)
	KFSW_LOG_PRINTF_LIKE(3, 4);

/** Set the runtime minimum log level (0 DEBUG through 4 disabled). */
int kfsw_log_set_level(uint8_t level);

/** Return the active runtime minimum log level. */
uint8_t kfsw_log_get_level(void);

#if CONFIG_KFSW_PARAM
/** Parameter table owned by the log service, in the service band. */
#define KFSW_LOG_PARAM_TABLE_ID 25U
/** Stable logical name paired with KFSW_LOG_PARAM_TABLE_ID. */
#define KFSW_LOG_PARAM_TABLE_NAME "log"

/** Logging-owned runtime policy parameter definitions. */
extern const struct kfsw_param_definition_set kfsw_log_param_definitions;
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 3
/**
 * @brief Log an error message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
#if defined(__DOXYGEN__)
void kfsw_log_error(const char *format, ...) KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_error(...) kfsw_log_write(KFSW_LOG_MODULE, 3U, __VA_ARGS__)
#endif
#else
#define kfsw_log_error(...)                                                                        \
	do {                                                                                       \
	} while (0)
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 2
/**
 * @brief Log a warning message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
#if defined(__DOXYGEN__)
void kfsw_log_warning(const char *format, ...) KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_warning(...) kfsw_log_write(KFSW_LOG_MODULE, 2U, __VA_ARGS__)
#endif
#else
#define kfsw_log_warning(...)                                                                      \
	do {                                                                                       \
	} while (0)
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 1
/**
 * @brief Log an informational message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
#if defined(__DOXYGEN__)
void kfsw_log_info(const char *format, ...) KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_info(...) kfsw_log_write(KFSW_LOG_MODULE, 1U, __VA_ARGS__)
#endif
#else
#define kfsw_log_info(...)                                                                         \
	do {                                                                                       \
	} while (0)
#endif

#if CONFIG_KFSW_LOG_MIN_LEVEL <= 0
/**
 * @brief Log a debug message.
 *
 * @param format printf-style format string.
 * @param ... Format arguments.
 */
#if defined(__DOXYGEN__)
void kfsw_log_debug(const char *format, ...) KFSW_LOG_PRINTF_LIKE(1, 2);
#else
#define kfsw_log_debug(...) kfsw_log_write(KFSW_LOG_MODULE, 0U, __VA_ARGS__)
#endif
#else
#define kfsw_log_debug(...)                                                                        \
	do {                                                                                       \
	} while (0)
#endif

#ifdef __cplusplus
}
#endif

#undef KFSW_LOG_PRINTF_LIKE

#endif
