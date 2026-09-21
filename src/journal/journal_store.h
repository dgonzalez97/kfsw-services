#ifndef KFSW_JOURNAL_STORE_H
#define KFSW_JOURNAL_STORE_H

#include <kfsw/services/journal.h>

#define KFSW_JOURNAL_PATH "/kfsw/journal.bin"
#define KFSW_JOURNAL_HEADER_SIZE 16U
#define KFSW_JOURNAL_RECORD_SIZE 72U

struct kfsw_journal_store {
	uint64_t sequences[CONFIG_KFSW_JOURNAL_CAPACITY];
	uint64_t next_sequence;
	uint64_t highest_boot;
	uint16_t held;
	uint32_t corrupt;
	bool ready;
};

int kfsw_journal_store_open(struct kfsw_journal_store *store);
int kfsw_journal_store_append(struct kfsw_journal_store *store, struct kfsw_journal_record *record);
int kfsw_journal_store_get(struct kfsw_journal_store *store, uint16_t age,
			   struct kfsw_journal_record *record);

#endif
