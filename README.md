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

## File transfer

`CONFIG_KFSW_FTP` enables the K-FSW-owned CSP file-transfer client and server.
It uses configurable CSP port 9 and requires libcsp RDP plus CSP CRC32 on every
connection. RDP supplies flow control, retransmission, and ordered delivery;
the application protocol does not add a second ACK/retry layer.

The version 1 wire format is explicitly encoded in network byte order. A
24-byte header carries version, opcode, flags/status, request identifier,
offset, total size, file CRC32, path length, and data length. File data is
streamed in fixed 192-byte chunks. Each client or server transfer workspace is
448 bytes (two 128-byte paths and one chunk); libcsp packet/window storage and
the fixed server thread stacks are separate shared resources.

Every local and remote virtual path is sandboxed below `/kfsw/ftp`. A leading
slash is accepted for operator convenience, so `/flash/sample.txt` resolves to
`/kfsw/ftp/flash/sample.txt`; traversal, empty components, backslashes, control
characters, embedded NULs, and paths longer than 96 bytes are rejected. The
`/kfsw/ftp/build` exchange directory is created at initialization. It remains a
Zephyr filesystem directory on KFSW-Linux rather than exposing arbitrary host
POSIX paths.

PUT and GET validate the total size and an IEEE CRC32. Receivers write to a
`.part` path, sync and close it, validate it, then rename it over the final
file. A failed transfer removes the partial file and preserves an existing
final file. The server accepts one active request at a time using one static
worker; overlapping connections receive `busy`. Version 1 has no resume,
recursive traversal, globbing, compression, encryption, or firmware activation.

The protocol is not claimed to be compatible with GomSpace libftp, EnduroSat
es-tftp, or another protocol that happens to use the FTP/TFTP name. No external
FTP implementation or source code is included.

Roadmap:

- structured events and persistent event backends
- command service
- housekeeping
- health/FDIR
- storage
- flight planner
- firmware update
