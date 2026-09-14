# K-FSW Services

Reusable services for K-FSW nodes: logging, parameters, persistence, files,
events, commands, health, firmware update and housekeeping.

| Service | Kconfig | What it does |
| --- | --- | --- |
| Boot | always | Startup markers, reset cause, image version and the note from the previous run |
| Log | always | Console output, with a level per module |
| Parameters | `KFSW_PARAM` | Named settings in tables, local or from the ground |
| Persistence | `KFSW_PARAM_PERSISTENCE` | A saved snapshot of the persistent settings |
| Files | `KFSW_FTP` | File transfer in both directions, checked before commit |
| Events | `KFSW_EVENT` | A RAM record of events, separate from the log |
| Commands | `KFSW_COMMAND` | Typed commands with typed results, local or remote |
| Health | `KFSW_HEALTH` | Component deadlines and the watchdog |
| Firmware update | `KFSW_FWU` | Receives and checks an image and hands it to the bootloader |
| Housekeeping | `KFSW_HK` | Collects a set of values together and keeps the samples |
| File based operations | `KFSW_FBO` | Runs a list of commands from a file |

Each service can be enabled on its own. One only pulls in another when it needs
it, and none of them need CSP.

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

On the wire the pair becomes one ID that is unique on the node and decodes back
to the table and offset.

Scalars, strings and byte arrays are supported. An array is always written and
validated as a whole.

### From the ground

`CONFIG_KFSW_PARAM_CSP` adds remote access with the MIT-licensed
[Space Inventor libparam](https://github.com/spaceinventor/libparam) wire
codec, pinned as a separate west project. CSP itself is in `kfsw-comms`.

A write from a ground node to two flight nodes, read back afterwards:

![Parameters read and written across a link](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/param-over-a-link.gif)

A named read asks the node for that one descriptor. Listing a node downloads
its descriptors one at a time and checks them with a table CRC, so a lost
packet fails the download instead of leaving a parameter out.

### Persistence

One snapshot with a CRC. With `param_autosave` on, the default, an accepted
change to a persistent value is saved; `kfsw_param_persist_save()` saves on
request.

Read-only values can still be saved: the boot counter is read-only and
persistent.

## Boot

Prints the startup markers, keeps the reset cause and reports the image
version.

It also reads the last words note the previous run left in `kfsw-platform`,
logs it and records it as an event so the ground can see it. No note means the
node lost power.

## Log

One console stream with a global level and a level per module, so raising one
module to debug doesn't flood the console. A file sets its module with a define
before the include.

## Events

A RAM ring of numeric records: ID, timestamp, sequence number, severity and a
small payload.

Events are separate from the log: small numeric records that are cheap to
downlink, with a sequence number that shows when records were missed.

The ring does not survive a reset, and it counts the records it overwrites.

## Commands

A command has a name, up to four typed arguments and a typed result. Handlers
run to completion, and commands that change something are marked as mutating.

Each request is one packet and each result another, with no retransmission. The
server matches the reply to the request ID but doesn't deduplicate, so a resent
`reboot` would run twice. A lost packet shows up as a timeout, and the operator
decides whether to send it again.

There is no authentication yet. The request has a source node and an
authentication flag that is always false.

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

Receiving and flashing are separate steps. A verified image stays in the slot
until `fwu flash` is sent.

MCUboot checks the signature and reverts an image that isn't confirmed. K-FSW
adds no authentication of its own.

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

Every path is inside `/kfsw/ftp`. Path traversal, empty components,
backslashes, control characters and embedded NULs are rejected.

A receiver writes a `.part` file, syncs it, checks it and then renames it over
the final name. A failed transfer removes the partial file and leaves any
existing file alone. The server handles one request at a time and answers
`busy` to the rest.

Version 1 has no resume, recursion, globbing, compression or encryption. The
protocol is K-FSW's own and is not compatible with other FTP or TFTP
implementations.

## Housekeeping

Reading a node one value at a time costs one round trip per value. A report
names a set of values once, and then one request returns the whole set.

A report stores parameter IDs, the same table and offset pair the wire uses,
instead of names. It is checked when it is defined: every parameter must exist
and the values must fit in one packet.

Widths come from the declarations, so every sample of a report has the same
layout. A value that can't be read is zero-filled and flagged instead of left
out.

```text
  one sample = one CSP packet
  a lost packet costs one sample, and the sequence number shows the gap
```

The timestamp is when the collection started. Local values are read in one
loop, and remote values arrive over the link.

Report definitions survive a reset. Samples are kept in RAM unless `hk store`
writes them to a file. A report can also be sent periodically as a beacon with
`hk beacon`, with a minimum interval.

## Not implemented yet

- a persistent event log, rate limiting and coalescing
- authentication of commands
- a flight planner

## License

Licensed under [Apache 2.0](LICENSE). Third-party dependencies retain their
own licences.
