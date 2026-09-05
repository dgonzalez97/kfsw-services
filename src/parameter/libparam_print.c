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
 * libparam writes its diagnostics straight to the console with printf. Most of
 * them are per-parameter: downloading a node's descriptor list prints one line
 * for every parameter it holds, so asking a sixty-six parameter node for one
 * value buried the answer under sixty-six lines of chatter.
 *
 * The library is vendored at an upstream revision that west manages, so it is
 * not edited here. Its printf is redirected at the build instead -- see the
 * compile definition on the kfsw_libparam target -- which leaves the vendored
 * tree byte-identical to upstream and keeps the redirection in one obvious
 * place.
 *
 * The messages become debug-level log lines attributed to the parameter
 * module, so they can be turned back on through log_levels like anything else,
 * and are compiled out entirely at the default log level.
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
