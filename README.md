# K-FSW Services

Reusable flight-software services.

Bootstrap:

- boot/readiness service
- basic console logging
- deterministic local and CSP parameter service

## Parameters

K-FSW integrates the MIT-licensed
[Space Inventor libparam](https://github.com/spaceinventor/libparam) through a
project-owned Zephyr/CMake adapter. The independent west project is checked out
at `third_party/libparam` and pinned to
`c296dfb6055a3c360f44dcbbd6ad108e98c76640`.

The first configuration uses a static local table and a 16-entry preallocated
remote list pool. Dynamic parameter creation, timestamps, upstream file and
FRAM VMEM, the collector, upstream shell code, RDP, and MPack file/allocation
helpers are disabled. Parameter transactions use configurable CSP port 10 and
list descriptions use configurable CSP port 12, matching current upstream
libparam. CSP initialization, interfaces, routes, and the router remain owned
by `kfsw-comms`.

Applications should use `include/kfsw/services/parameter.h`; raw libparam APIs
are an integration detail.

### Persistence

When `CONFIG_KFSW_PARAM_PERSISTENCE` is enabled, K-FSW stores one bounded,
CRC-protected snapshot at `/kfsw/params/parameters.dat`. Persistence is
explicit: parameter writes change RAM, while `kfsw_param_persist_save()` writes
the selected values. A valid snapshot is restored after parameter-table
initialization and before the CSP parameter server starts.

The persistent set is marked with a K-FSW-owned libparam user flag and currently
contains `log_level`, `test_u32`, `test_i32`, and `test_float`. The read-only
`node_id` is excluded. Loading matches by name and type, ignores unknown or
incompatible entries, and rejects invalid headers, versions, lengths, or CRCs
without changing the live table. Saves use a synced temporary file followed by
LittleFS rename replacement; compiled defaults and snapshot deletion remain
separate operations.

Roadmap:

- structured events and persistent event backends
- command service
- housekeeping
- health/FDIR
- storage
- flight planner
- firmware update
