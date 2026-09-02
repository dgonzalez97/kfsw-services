# K-FSW Services

Reusable flight-software services.

Bootstrap:

- boot/readiness service
- basic console logging
- deterministic local parameter service with optional CSP integration

## Parameters

`CONFIG_KFSW_PARAM` enables the deterministic local index and public local API.
It has no dependency on CSP or libcsp. Each semantic component owns a plain
`const struct kfsw_param_definition` table, its backing values, validation, and
change callbacks. The executable passes its enabled definition sets to
`kfsw_param_init()`, which validates duplicate IDs/names, supported scalar
shapes, and defaults before building a bounded sorted index. The PARAM core
does not include owner headers or encode owner-specific policy.

`CONFIG_KFSW_PARAM_PERSISTENCE` adds explicit local snapshots and depends only
on the storage capability.

`CONFIG_KFSW_PARAM_CSP` is the optional remote adapter. It depends on both
`CONFIG_KFSW_PARAM` and `CONFIG_KFSW_CSP` and integrates the MIT-licensed
[Space Inventor libparam](https://github.com/spaceinventor/libparam) wire codec.
The independent west project is checked out at `third_party/libparam` and
pinned to `c296dfb6055a3c360f44dcbbd6ad108e98c76640`.

The adapter bridges the local table to libparam's server codec and uses a
16-entry preallocated remote list pool. Dynamic parameter creation, timestamps,
upstream file and FRAM VMEM, the collector, upstream shell code, RDP, and MPack
file/allocation helpers are disabled. Parameter transactions use configurable
CSP port 10 and list descriptions use configurable CSP port 12, matching
current upstream libparam. CSP initialization, interfaces, routes, and the
router remain owned by `kfsw-comms`.

Applications should use `include/kfsw/services/parameter.h`; raw libparam APIs
are an integration detail.

### Persistence

When `CONFIG_KFSW_PARAM_PERSISTENCE` is enabled, K-FSW stores one bounded,
CRC-protected snapshot at `/kfsw/params/parameters.dat`. Persistence is
explicit: parameter writes change RAM, while `kfsw_param_persist_save()` writes
the selected values. A valid snapshot is restored after parameter-table
initialization and before the CSP parameter server starts.

The persistent set is selected by a K-FSW-owned parameter flag. The logging
service owns the production `log_level` persistent value; test compositions
may add separately owned compatibility fixtures. The application-owned,
read-only `node_id` is excluded. Loading matches by name and type, ignores
unknown or incompatible entries, and rejects invalid headers, versions,
lengths, CRCs, or owner validation without changing that entry. Saves use a
synced temporary file followed by LittleFS rename replacement; compiled
defaults and snapshot deletion remain separate operations. The KPAR v1 wire
format is unchanged by definition aggregation.

## File transfer

`CONFIG_KFSW_FTP` enables the K-FSW-owned file-transfer client and server. The
current transport is CSP on configurable port 9 with libcsp RDP plus CSP CRC32
on every connection. RDP supplies flow control, retransmission, and ordered
delivery; the application protocol does not add a second ACK/retry layer.

### Layering rule

The service is split into an operation layer, a transfer engine, and a
reliable-transport interface:

```text
ftp_client.c  ftp_server.c  ftp_transfer.c  ftp_protocol.c  ftp_store.c
                              |
                       only the ftp_link API
                              v
                          ftp_link.h
                              |
                    +---------+----------+
                    |                    |
              ftp_link_csp.c        future backend
               CSP + RDP           or test double
```

One rule keeps that boundary checkable rather than a matter of judgement:

> No `#include <csp/...>` and no `kfsw_csp_*()` outside `ftp_link_csp.c`.

Everything the core needs to know about the transport is reached through
`ftp_link.h`, including the local endpoint identifier, the highest addressable
node, the largest payload one message can carry, and whether the transport is
ready to route. Swapping the backend therefore does not touch the protocol,
the transfer engine, or the storage rules.

The interface also carries the packet-ownership contract. A received message
arrives as a `kfsw_ftp_link_frame` whose `path` and `data` borrow the
transport's buffer; `kfsw_ftp_link_release()` is what ends that borrow, so the
lifetime is expressed by the API rather than by comment.

This is deliberately not TFTP. TFTP carries DATA/ACK, timeouts, retries, and
duplicate handling because it usually runs over UDP. Here RDP already provides
those, so adding them again would duplicate the transport without buying
anything.

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
