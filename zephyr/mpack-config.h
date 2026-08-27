#ifndef MPACK_CONFIG_H
#define MPACK_CONFIG_H 1

#include <libparam.h>

#define MPACK_READER 1
#define MPACK_EXPECT 1
#define MPACK_NODE 0
#define MPACK_WRITER 1

/* Fixed-buffer reader/writer paths only: no file helpers or allocation. */
#define MPACK_STDLIB 1
#define MPACK_STDIO 0
#define MPACK_DEBUG 0
#define MPACK_STRINGS 0
#define MPACK_CUSTOM_ASSERT 0
#define MPACK_READ_TRACKING 0
#define MPACK_WRITE_TRACKING 0

#endif
