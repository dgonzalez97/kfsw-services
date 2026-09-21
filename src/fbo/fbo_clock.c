#include <errno.h>
#include <kfsw/platform/wallclock.h>
#if CONFIG_KFSW_CSP
#include <kfsw/comms/csp.h>
#endif
#include "fbo_internal.h"

int kfsw_fbo_clock_seconds(int64_t *seconds)
{
#if CONFIG_KFSW_CSP
	struct kfsw_csp_clock clock;

	kfsw_csp_clock_get(&clock);
	if (!kfsw_csp_clock_is_set(&clock)) {
		return -ENODATA;
	}
	*seconds = clock.seconds;
	return 0;
#else
	return kfsw_wallclock_get(seconds);
#endif
}
