/* This file provides the replacement for the forced-include redirect. */
#undef printf

#include <stdarg.h>

#include <zephyr/sys/printk.h>

#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>

/*
 * libparam prints diagnostics with printf. The build redirects them here, and
 * they become debug log lines in the parameter module.
 */

int kfsw_libparam_printf(const char *format, ...)
{
	char message[128];
	va_list args;
	int written;

	va_start(args, format);
	written = vsnprintk(message, sizeof(message), format, args);
	va_end(args);

	/* libparam ends most of its lines itself; the log adds its own. */
	for (size_t index = 0U; message[index] != '\0'; index++) {
		if ((message[index] == '\n') || (message[index] == '\r')) {
			message[index] = '\0';
			break;
		}
	}

	kfsw_log_debug("%s", message);
	return written;
}
