#ifndef KFSW_COMMAND_RETRY_H
#define KFSW_COMMAND_RETRY_H

#include "command_internal.h"

struct kfsw_command_ticket {
	uint64_t nonce;
	uint64_t ticket;
	int64_t expires;
	uint16_t source;
	uint16_t command;
	uint16_t request;
	uint16_t size;
	uint8_t count;
	bool complete;
	uint8_t payload[KFSW_COMMAND_MAX_PAYLOAD_SIZE];
	struct kfsw_command_result result;
};

struct kfsw_command_retry_cache {
	struct kfsw_command_ticket entries[CONFIG_KFSW_COMMAND_RETRY_SLOTS];
};

/* Called by the single command server; callers serialize access. */
void kfsw_command_retry_dispatch(struct kfsw_command_retry_cache *cache, uint16_t source,
				 const struct kfsw_command_message *request, uint64_t new_ticket,
				 int64_t now, uint64_t *ticket, struct kfsw_command_result *result);
int kfsw_command_retry_random(uint64_t *value);

#endif
