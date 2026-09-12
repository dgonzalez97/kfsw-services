#ifndef KFSW_FWU_INTERNAL_H
#define KFSW_FWU_INTERNAL_H

/* A secondary-slot reader excludes begin, abort, and upgrade scheduling. */
int kfsw_fwu_secondary_acquire(void);
void kfsw_fwu_secondary_release(void);

#endif
