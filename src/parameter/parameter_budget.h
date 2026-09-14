#ifndef KFSW_PARAMETER_BUDGET_H
#define KFSW_PARAMETER_BUDGET_H

/*
 * Size limit of the parameter snapshot, which is also the size of the static
 * buffer it is built in. The reference composition saves 10 values in about
 * 210 bytes. Free space is still checked when saving.
 */
#define KFSW_PARAM_PERSIST_MAX_BYTES 2048U

/* Maximum entries in one snapshot. */
#define KFSW_PARAM_PERSIST_MAX_ENTRY_COUNT 64U

#endif /* KFSW_PARAMETER_BUDGET_H */
