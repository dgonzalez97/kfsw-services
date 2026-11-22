#include <stdint.h>

#include <csp/csp.h>

/* Never called: two enabled services on one port fail here as a duplicate case
 * value instead of in csp_bind at start-up. The beacon port is in the set
 * because its help promises that nothing binds it.
 */
static inline void csp_ports_are_distinct(uint8_t port)
{
	switch (port) {
	case CSP_CMP:
	case CSP_PING:
#if CONFIG_KFSW_REMEXEC
	case CONFIG_KFSW_REMEXEC_CSP_PORT:
#endif
#if CONFIG_KFSW_FTP
	case CONFIG_KFSW_FTP_CSP_PORT:
#endif
#if CONFIG_KFSW_PARAM_CSP
	case CONFIG_KFSW_PARAM_PORT:
	case CONFIG_KFSW_PARAM_LIST_PORT:
#endif
#if CONFIG_KFSW_COMMAND_CSP
	case CONFIG_KFSW_COMMAND_CSP_PORT:
#endif
#if CONFIG_KFSW_FWU_LITE_CSP
	case CONFIG_KFSW_FWU_LITE_CSP_PORT:
#endif
#if CONFIG_KFSW_HK_CSP
	case CONFIG_KFSW_HK_CSP_PORT:
#endif
#if CONFIG_KFSW_HK_BEACON
	case CONFIG_KFSW_HK_BEACON_PORT:
#endif
#if CONFIG_KFSW_LOG_REMOTE
	case CONFIG_KFSW_LOG_REMOTE_PORT:
#endif
	default:
		break;
	}
}
