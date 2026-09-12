#ifndef KFSW_LIBPARAM_PRINT_H
#define KFSW_LIBPARAM_PRINT_H

#include <stdio.h>
#include <zephyr/toolchain.h>

int kfsw_libparam_printf(const char *format, ...) __printf_like(1, 2);

/* Redirect calls without rewriting GCC's format(printf, ...) attribute. */
#define printf(...) kfsw_libparam_printf(__VA_ARGS__)

#endif
