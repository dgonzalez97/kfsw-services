# K-FSW Services

The reusable services a node needs before it does anything mission-specific:
what it says, what it remembers, what it can be told to do, and how it reports
that it is still alive.

| Service | Kconfig | What it is |
| --- | --- | --- |
| Boot | always | Startup markers, latched reset cause, image version |
| Log | always | Console output, per-module levels |
| Parameters | `KFSW_PARAM` | Named values in tables, local, with optional remote access |
| Persistence | `KFSW_PARAM_PERSISTENCE` | One snapshot of the values worth keeping |
| Files | `KFSW_FTP` | Transfers in either direction, verified before commit |
| Events | `KFSW_EVENT` | A bounded record of what happened, separate from the log |
| Commands | `KFSW_COMMAND` | Typed calls with typed results, local or remote |
| Health | `KFSW_HEALTH` | Component supervision, and the watchdog behind it |
| Firmware update | `KFSW_FWU` | An image received, verified, and handed to the bootloader |

Each is independent. Enabling one never pulls in another unless it genuinely
needs it, and none of them require CSP.

## Parameters

A parameter is a named value with an owner. The owner is the code the value
describes: it holds the storage, validates a write before it lands, and decides
what a change means. The parameter service owns lookup, addressing, type
checking and enumeration, and knows nothing about what any particular value is
for.

Values are addressed by **table and offset**, and the table number says who
owns it:

| Band | Owner |
| --- | --- |
| 0 | reserved, so an uninitialised field cannot address a real table |
| 1–24 | core: identity, platform, links |
| 25–49 | services |
| 50–99 | modules |

On the wire the pair becomes a single identifier, `(table << 8) | offset`,
which is unique across a node and decodes back to the pair it came from.

Scalars, strings and byte arrays all work. An array is written whole or not at
all — a short or over-long write is refused rather than partly applied — and
its owner judges it as one value rather than element by element.

`kfsw_param_init()` takes the definition sets a composition enabled, checks for
duplicate names and addresses, checks that every default fits its type, and
builds a sorted index. It includes no owner header and encodes no owner policy.
A module contributing a table never edits the service.

### Reading and writing

Two rules are worth stating because they are easy to get backwards.

**A validator refuses; a change callback cannot.** Validation runs before the
value reaches storage, which is the only point where refusing it still leaves
the old value intact. By the time a change callback runs, the write has already
happened.

**Sampling and writing must not race.** A value that samples hardware refreshes
on read. A write that arrives over the link lands in storage and is then handed
back for the owner to apply — and sampling at that moment would overwrite the
value that just arrived, so the write would report success and change nothing.
Reads that exist to serve a write therefore skip the sample.

### Remote access

`CONFIG_KFSW_PARAM_CSP` adds the remote adapter. It needs `CONFIG_KFSW_PARAM`
and `CONFIG_KFSW_CSP`, and uses the MIT-licensed
[Space Inventor libparam](https://github.com/spaceinventor/libparam) wire
codec, pinned at `c296dfb6055a3c360f44dcbbd6ad108e98c76640` as an independent
west project under `third_party/libparam`.

The adapter bridges the local tables to libparam's server codec with a
16-entry preallocated remote pool. Dynamic creation, timestamps, file and FRAM
VMEM, the collector, upstream shell code and the MPack helpers are all off.
Transactions use CSP port 10 and list descriptions port 12. CSP itself —
initialisation, interfaces, routes, the router — stays in `kfsw-comms`.

Descriptor listing runs over RDP by default. Without it a listing crossing a
radio arrives short: the packets are independent, and a lost one silently
removes a parameter from the list rather than failing the read.

Applications use `include/kfsw/services/parameter.h`. Raw libparam calls are an
integration detail.

### Persistence

One bounded, CRC-protected snapshot at `/kfsw/params/parameters.dat`. Saving is
explicit: a write changes RAM, and `kfsw_param_persist_save()` is what makes it
survive a reset.

Read-only means an operator cannot write it, not that it cannot be kept — a
boot counter is read-only and still has to persist, or it would restart from
zero every time. Loading matches by name and type, ignores what it does not
recognise, and rejects a bad header, version, length, CRC or owner validation
without disturbing the entry it failed on. Saves write a synced temporary file
and rename it into place.

## Log

One console stream, with a level per module rather than one global level, so
raising CSP to debug does not drown the output in everything else. Modules are
selected by a `KFSW_LOG_MODULE` define before the include, so the call sites
themselves are unchanged. The levels are readable and writable as one array
parameter.

`CONFIG_KFSW_LOG_MIN_LEVEL` still compiles messages out below a threshold, so
the runtime level can only filter within what was built.

## Events

A bounded ring of numeric records: a stable identifier, a monotonic timestamp,
a sequence number, a severity, and up to 16 opaque payload bytes. Identifiers
belong to the component that emits them and are never reused.

This is not the log. The log is for a human reading a console; the event record
is for a ground station reconstructing what a node did while nobody was
listening. It is RAM only, so it does not survive a reset, and it counts what
it overwrites rather than quietly losing it.

## Commands

A command has a name, up to four typed arguments, and a typed result carrying a
status and a detail string. Handlers run to completion — there is no
accepted-plus-identifier form for a long operation — and a command that changes
something is marked as mutating.

Delivery is deliberately at-most-once. One packet carries the request and one
carries the result, and no transport-level retransmission sits underneath: the
server correlates the reply by request identifier but does not deduplicate on
it, so a silently resent `reboot` would run twice. A lost packet is a clean
timeout, and repeating it is the operator's decision.

There is no authentication. The request context reserves a source and an
authentication result, and the flag is always false.

## Health

Components register, report that they are still running, and the service
decides. It supervises up to a configured number of them by name, and it is
what feeds the watchdog — so a component that stops reporting stops the feeding
and the board resets. Feeding a watchdog from a timer proves only that the
timer runs.

## Firmware update

An image arrives, is written into the secondary slot at the swap offset, is
checked against a whole-image CRC32, and is handed to the bootloader. Two
routes exist: one over the file-transfer service, and a direct block protocol
with per-block checksums and repeat for links where a long stream will not
survive.

Committing is a separate step from receiving. A transfer stops at a verified
image and stays there until told to flash, because an image that arrived intact
is not the same thing as an image you want to boot.

MCUboot verifies the signature and reverts an image that never confirms itself.
K-FSW adds no authenticity of its own beyond that.

## File transfer

`CONFIG_KFSW_FTP` enables the client and the server. The transport is CSP on
port 9, with RDP and CRC32 on every connection. RDP already provides flow
control, retransmission and ordering, so the application protocol does not add
a second retry layer on top.

### Layering rule

The service splits into an operation layer, a transfer engine, and a
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

Everything the core needs from the transport comes through `ftp_link.h`: the
local endpoint, the highest addressable node, the largest payload one message
carries, and whether the transport can route yet. Changing the backend
therefore does not touch the protocol, the engine, or the storage rules.

That interface also carries the ownership contract. A received message arrives
as a `kfsw_ftp_link_frame` whose `path` and `data` borrow the transport's
buffer, and `kfsw_ftp_link_release()` ends the borrow — so the lifetime is in
the API rather than in a comment.

This is deliberately not TFTP. TFTP carries DATA/ACK, timeouts, retries and
duplicate handling because it usually runs over UDP. Here RDP already does
that, and doing it twice buys nothing.

### Wire format and rules

Version 1 is encoded in network byte order. A 24-byte header carries version,
opcode, flags and status, request identifier, offset, total size, file CRC32,
path length and data length. Data streams in 192-byte chunks. Each transfer
workspace is 448 bytes — two 128-byte paths and one chunk — while packet
storage and the server thread stacks are shared.

Every path is sandboxed below `/kfsw/ftp`. A leading slash is accepted for
convenience, so `/flash/sample.txt` resolves to `/kfsw/ftp/flash/sample.txt`.
Traversal, empty components, backslashes, control characters, embedded NULs and
paths over 96 bytes are rejected.

PUT and GET both validate the total size and a CRC32. A receiver writes to a
`.part` file, syncs and closes it, validates it, and only then renames it over
the final name — so a failed transfer removes the partial file and leaves any
existing file alone. The server handles one request at a time on one static
worker; anything overlapping gets `busy`.

Version 1 has no resume, recursion, globbing, compression or encryption. The
protocol is K-FSW's own. It claims no compatibility with anything else that
happens to use the FTP or TFTP name, and no external implementation or source
is included.

## Not here yet

- housekeeping collection, so a pass does not read values one round trip at a
  time
- a persistent event journal, rate limiting and coalescing
- authentication on the command path
- a flight planner
