#ifndef KFSW_LOG_HISTORY_INTERNAL_H
#define KFSW_LOG_HISTORY_INTERNAL_H

#include <kfsw/services/log_history.h>

void kfsw_log_history_append(uint8_t module, uint8_t severity, const char *text, bool truncated);

#endif
