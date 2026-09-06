/* The redirect is a command-line define for this whole library. It must not
 * apply here: this file provides the replacement, and leaving it in place would
 * rewrite the printf-format attribute in the logging header too.
 */
#undef printf

#include <stdarg.h>

#include <zephyr/sys/printk.h>

#define KFSW_LOG_MODULE KFSW_LOG_MODULE_PARAM
#include <kfsw/services/log.h>

/*
 * libparam prints its diagnostics with printf, one line per parameter, so
 * asking a sixty-six parameter node for one value buried the answer.
 *
 * The library is vendored at a pinned revision and not edited here; its printf
 * is redirected at the build instead, on the kfsw_libparam target. The messages
 * become debug log lines in the parameter module, off at the default level.
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
