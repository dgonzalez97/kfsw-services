#ifndef KFSW_FBO_INTERNAL_H
#define KFSW_FBO_INTERNAL_H

#include <stdint.h>

/** Record one line's outcome against the running procedure. */
void kfsw_fbo_count_line(uint16_t line, int outcome);

/** Record one line a guard decided was not for this run. */
void kfsw_fbo_count_skipped(uint16_t line);

#endif
