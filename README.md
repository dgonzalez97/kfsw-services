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
remote list pool. Dynamic parameter creation, timestamps, persistence, file and
FRAM VMEM, the collector, upstream shell code, RDP, and MPack file/allocation
helpers are disabled. Parameter transactions use configurable CSP port 10 and
list descriptions use configurable CSP port 12, matching current upstream
libparam. CSP initialization, interfaces, routes, and the router remain owned
by `kfsw-comms`.

Applications should use `include/kfsw/services/parameter.h`; raw libparam APIs
are an integration detail.

Roadmap:

- structured events and persistent event backends
- command service
- housekeeping
- health/FDIR
- storage
- flight planner
- firmware update
