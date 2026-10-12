#ifndef KFSW_FBO_INTERNAL_H
#define KFSW_FBO_INTERNAL_H

#include <stdint.h>

#include <kfsw/services/event.h>

/* Events recorded by this service. IDs are never reused. */
#define KFSW_FBO_EVENT_STARTED 1U
#define KFSW_FBO_EVENT_LINE_FAILED 2U
#define KFSW_FBO_EVENT_FINISHED 3U
#define KFSW_FBO_EVENT_RELEASED 4U
#define KFSW_FBO_EVENT_OVERDUE 5U
#define KFSW_FBO_EVENT_CLOCK_STEP 6U
#define KFSW_FBO_EVENT_INTERLEAVED 7U

/** Read the same UTC clock used by local clock set/get. */
int kfsw_fbo_clock_seconds(int64_t *seconds);

/**
 * The queue's own reading of the same clock. Separate from a procedure's wait
 * so the two can be driven independently in a test.
 */
int kfsw_fbo_schedule_clock_seconds(int64_t *seconds);

/** Record one line's outcome against the running procedure. */
void kfsw_fbo_count_line(uint16_t line, int outcome);

/** Record one line a guard decided was not for this run. */
void kfsw_fbo_count_skipped(uint16_t line);

/** Emit one service event carrying a line number or an entry index. */
void kfsw_fbo_note(uint16_t id, enum kfsw_event_severity severity, uint16_t detail);

/** Start the release thread. Called once from kfsw_fbo_init(). */
void kfsw_fbo_schedule_init(void);

#endif
