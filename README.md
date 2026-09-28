# K-FSW Services

Reusable services for K-FSW nodes: logging, parameters, persistence, files,
events, commands, health, firmware update and housekeeping.

| Service | Kconfig | What it does |
| --- | --- | --- |
| Boot | always | Startup markers, reset cause, image version and the note from the previous run |
| Log | always | Console output, with a level per module |
| Log history | `KFSW_LOG_HISTORY` | Recent text messages in RAM, with optional remote reads |
| Parameters | `KFSW_PARAM` | Named settings in tables, local or from the ground |
| Persistence | `KFSW_PARAM_PERSISTENCE` | A saved snapshot of the persistent settings |
| Files | `KFSW_FTP` | File transfer in both directions, checked before commit |
| Events | `KFSW_EVENT` | A RAM record of events, separate from the log |
| Journal | `KFSW_JOURNAL` | Boot reports and selected events saved to storage |
| Commands | `KFSW_COMMAND` | Typed commands with typed results, local or remote |
| Health | `KFSW_HEALTH` | Component deadlines and the watchdog |
| Ground watchdog | `KFSW_GNDWDT` | CSP feed with `KFSWWSFK`; `ground_wtd get` reads the countdown |
| Resource monitor | `KFSW_RESMON` | Thread stack use and alert events |
| Firmware update | `KFSW_FWU` | Receives and checks an image and hands it to the bootloader |
| Housekeeping | `KFSW_HK` | Collects a set of values together and keeps the samples |
| File based operations | `KFSW_FBO` | Runs a list of commands from a file |

Kconfig selects each service and checks its dependencies. Local parameters,
logging, events and health can run without CSP. Remote adapters and FTP need
the communications layer; FTP also needs storage.

Full documentation is on the [K-FSW site](https://dgonzalez97.github.io/k-fsw/).

## Parameters

Parameters are defined by the code that uses them; the parameter service
handles lookup, addressing and type checks.

Settings are addressed by table and offset, and the table number shows what
kind of component defines it:

```text
  table   0 ---- reserved, so an uninitialised field addresses nothing
        1-24 ---- core: identity, platform, links
       25-49 ---- services
       50-99 ---- modules
```

The wire ID uses the table as its high byte and the offset as its low byte.

Scalars, strings and byte arrays are supported. An array is always written and
validated as a whole.

### From the ground

`CONFIG_KFSW_PARAM_CSP` adds remote access with the MIT-licensed
[Space Inventor libparam](https://github.com/spaceinventor/libparam) wire
codec. The composition pins the K-FSW fork as a separate west project. CSP
itself is in `kfsw-comms`.

A write from a ground node to two flight nodes, read back afterwards:

![Parameters read and written across a link](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/param-over-a-link.gif)

A named read asks the node for that one descriptor. Listing a node downloads
its descriptors one at a time and checks them with a table CRC, so a lost
packet fails the download instead of leaving a parameter out.

### Persistence

Persistent values share one CRC-checked snapshot. `param_autosave` saves
accepted changes by default; `kfsw_param_persist_save()` saves on request.

Read-only values can still be saved: the boot counter is read-only and
persistent.

## Boot

Prints the startup markers, keeps the reset cause and reports the image
version.

It also reads the last words note the previous run left in `kfsw-platform`,
logs it and records it as an event. A missing note means no valid record was
retained; use the reset-cause flags to distinguish power loss from other resets.

## Log

One console stream with a global level and a level per module, so raising one
module to debug doesn't flood the console. A file sets its module with a define
before the include.

## Events

A RAM ring of numeric records: ID, timestamp, sequence number, severity and a
small payload.

Sequence gaps show missed records. The ring counts overwritten records and
is cleared at reset.

The optional journal saves boot reports and events at or above a configured
severity. Its worker drains a bounded queue to a checksummed file. Queued
records can be lost on power failure; full queues and write failures are
counted. `journal_stats`, `journal_tail` and `journal_time` expose the stored
records through the command service.

## Commands

A command has a name, up to four typed arguments and a typed result. Handlers
run to completion, and commands that change something are marked as mutating.

Legacy calls send one request and wait for one result. Resending is a new
operation, so inspect the node after a timeout before trying again.

`KFSW_COMMAND_RETRY` adds ticket reservations and cached results. `cmd retry`
reuses one ticket within an invocation; a new invocation is a new operation.
Tickets expire and are lost on reset. A lost reply can still leave the outcome
unknown.

Tickets do not authenticate commands. The source authentication flag remains
false; optional radio encryption protects that link separately.

## Health

Components register with the health service and report periodically. The
service feeds the watchdog, so if a component stops reporting, the board resets.

When the service stops feeding it leaves a last words note, and removes it if
the component recovers before the reset.

## Firmware update

An image is written into the secondary slot, checked against a whole-image
CRC32 and handed to the bootloader. It can arrive through the file transfer
service, or through FWU lite, which sends blocks with their own checksum and
resends failed ones for poor links.

FWU lite separates upload from activation: a verified image waits for
`fwu flash`. A successful FTP upload to the reserved firmware path schedules
the trial boot immediately. Both paths expose the slots for readback.

MCUboot checks the signature and reverts an image that isn't confirmed.
Signing an image and authorizing its upload are separate checks.

## File transfer

Client and server over CSP, with RDP and CRC32 on every connection. RDP handles
flow control, retransmission and ordering, so the protocol has no retry layer
of its own. It is not TFTP.

A file uploaded, checked on the node, downloaded again and compared, with the
same CRC32 at every step:

![A file sent and fetched back](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/file-transfer.gif)

### Code layout

The service has request handling, a transfer engine and a transport interface:

```text
  ftp_client   ftp_server   ftp_transfer   ftp_protocol   ftp_store
                              |
                       only the ftp_link API
                              v
                        ftp_link_csp        CSP + RDP
```

> No `#include <csp/...>` and no `kfsw_csp_*()` outside `ftp_link_csp.c`.

The transport is only used through `ftp_link.h`, so a different backend doesn't
change the protocol, the engine or the storage code. A received message points
into the transport's buffer until it is released.

### Transfer rules

Ordinary paths resolve inside `/kfsw/ftp`. Read-only `/hk` and `/boot` paths
expose sample files and firmware slots when enabled. Path traversal, empty
components, backslashes, control characters and embedded NULs are rejected.

A receiver writes a `.part` file, syncs it, checks it and then renames it over
the final name. A failed transfer removes the partial file and leaves any
existing file alone. The server handles one request at a time and answers
`busy` to the rest.

Version 1 has no resume, recursion, globbing, compression or encryption. The
protocol is K-FSW's own and is not compatible with other FTP or TFTP
implementations.

## Housekeeping

A housekeeping report collects a set of parameters and returns them in one
request.

A report stores parameter IDs, the same table and offset pair the wire uses,
instead of names. It is checked when it is defined: every parameter must exist
and the values must fit in one packet.

Widths come from the declarations, so every sample of a report has the same
layout. A value that can't be read is zero-filled and flagged instead of left
out.

Each sample fits one CSP packet. Sequence gaps show missed samples.

The timestamp is when the collection started. Local values are read in one
loop, and remote values arrive over the link.

Report definitions survive a reset. Samples are kept in RAM unless `hk store`
writes them to a file. A report can also be sent periodically as a beacon with
`hk beacon`, with a minimum interval.

## Limits

- No per-node command authorization.
- No general event rate limiting or coalescing.
- File procedures have bounded waits, but no persistent flight planner.

## License

Licensed under [Apache 2.0](LICENSE). Third-party dependencies retain their
own licences.
