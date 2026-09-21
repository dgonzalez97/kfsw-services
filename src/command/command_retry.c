#include <errno.h>
#include <string.h>

#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include "command_retry.h"

int kfsw_command_retry_random(uint64_t *value)
{
#if CONFIG_CSPRNG_ENABLED
	int result = sys_csrand_get(value, sizeof(*value));

	if (result != 0) {
		*value = 0U;
		return result;
	}
	return (*value == 0U) ? -EIO : 0;
#else
	*value = 0U;
	return -ENOTSUP;
#endif
}

static bool matches(const struct kfsw_command_ticket *entry,
		    const struct kfsw_command_message *request)
{
	return (entry->command == request->command_id) && (entry->request == request->request_id) &&
	       (entry->size == request->payload_size) && (entry->count == request->arg_count) &&
	       (memcmp(entry->payload, request->payload, entry->size) == 0);
}

void kfsw_command_retry_dispatch(struct kfsw_command_retry_cache *cache, uint16_t source,
				 const struct kfsw_command_message *request, uint64_t new_ticket,
				 int64_t now, uint64_t *ticket, struct kfsw_command_result *result)
{
	struct kfsw_command_ticket *entry = NULL;
	struct kfsw_command_ticket *free_entry = NULL;
	struct kfsw_command_arg args[KFSW_COMMAND_MAX_ARGS];
	char text[KFSW_COMMAND_MAX_ARGS][KFSW_COMMAND_MAX_TEXT_SIZE + 1U];
	struct kfsw_command_source origin = {.node = source};
	bool collision = false;
	int count;

	memset(result, 0, sizeof(*result));
	result->status = KFSW_COMMAND_UNAVAILABLE;
	*ticket = request->token;
	if ((request->version != 2U) || (request->token == 0U) || (request->status != 0U) ||
	    (request->payload_size > KFSW_COMMAND_MAX_PAYLOAD_SIZE) ||
	    ((request->opcode != KFSW_COMMAND_OP_PREPARE) &&
	     (request->opcode != KFSW_COMMAND_OP_EXECUTE))) {
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(cache->entries); i++) {
		struct kfsw_command_ticket *candidate = &cache->entries[i];

		if ((candidate->ticket == 0U) || (now >= candidate->expires)) {
			if (free_entry == NULL) {
				free_entry = candidate;
			}
			continue;
		}
		collision |= candidate->ticket == new_ticket;
		if ((candidate->source == source) &&
		    (((request->opcode == KFSW_COMMAND_OP_PREPARE) &&
		      (candidate->nonce == request->token)) ||
		     ((request->opcode == KFSW_COMMAND_OP_EXECUTE) &&
		      (candidate->ticket == request->token)))) {
			entry = candidate;
		}
	}
	if (entry != NULL) {
		if (!matches(entry, request)) {
			result->status = KFSW_COMMAND_INVALID_ARGUMENT;
			return;
		}
		*ticket = entry->ticket;
		if (request->opcode == KFSW_COMMAND_OP_PREPARE) {
			result->status = KFSW_COMMAND_OK;
			return;
		}
		if (entry->complete) {
			*result = entry->result;
			return;
		}
	} else if (request->opcode == KFSW_COMMAND_OP_PREPARE) {
		if (free_entry == NULL) {
			result->status = KFSW_COMMAND_BUSY;
			return;
		}
		if ((new_ticket == 0U) || collision) {
			return;
		}
		count = kfsw_command_decode_args(request, args, ARRAY_SIZE(args), text);
		if (count < 0) {
			result->status = KFSW_COMMAND_INVALID_ARGUMENT;
			return;
		}
		entry = free_entry;
		memset(entry, 0, sizeof(*entry));
		entry->nonce = request->token;
		entry->ticket = new_ticket;
		entry->expires = now + CONFIG_KFSW_COMMAND_RETRY_WINDOW_MS;
		entry->source = source;
		entry->command = request->command_id;
		entry->request = request->request_id;
		entry->size = request->payload_size;
		entry->count = request->arg_count;
		memcpy(entry->payload, request->payload, entry->size);
		*ticket = entry->ticket;
		result->status = KFSW_COMMAND_OK;
		return;
	} else {
		strcpy(result->detail, "ticket expired or unknown; outcome may be unknown");
		return;
	}
	count = kfsw_command_decode_args(request, args, ARRAY_SIZE(args), text);
	if (count < 0) {
		result->status = KFSW_COMMAND_INVALID_ARGUMENT;
	} else {
		(void)kfsw_command_invoke_id(request->command_id, args, (size_t)count, &origin,
					     result);
	}
	entry->result = *result;
	entry->complete = true;
}
