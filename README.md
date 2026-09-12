# K-FSW Services

The reusable services a node needs before it does anything mission-specific:
what it says, what it remembers, what it can be told to do, and how it reports
that it is still alive.

| Service | Kconfig | What it is |
| --- | --- | --- |
| Boot | always | Startup markers, reset cause, image version, and what the previous run said before it went away |
| Log | always | Console output, with a level per module |
| Parameters | `KFSW_PARAM` | Named settings in tables, local, with optional access from the ground |
| Persistence | `KFSW_PARAM_PERSISTENCE` | One snapshot of the settings worth keeping |
| Files | `KFSW_FTP` | Transfers in either direction, verified before commit |
| Events | `KFSW_EVENT` | A bounded record of what happened, separate from the log |
| Commands | `KFSW_COMMAND` | Typed calls with typed results, local or remote |
| Health | `KFSW_HEALTH` | Component supervision, and the watchdog behind it |
| Firmware update | `KFSW_FWU` | An image received, verified, and handed to the bootloader |
| Housekeeping | `KFSW_HK` | A named set of values, collected together and kept for a while |

Each is independent. Enabling one never pulls in another unless it genuinely
needs it, and none of them require CSP.

Full documentation is on the
[K-FSW site](https://dgonzalez97.github.io/k-fsw/); what follows is the
reasoning behind the parts that are easy to get wrong.

## Parameters

A parameter is a named setting with an **owner** — the code it describes. The
owner holds the storage, checks a write before it lands, and decides what a
change means. The service itself owns lookup, addressing and type checking, and
knows nothing about what any particular value is for.

Settings are addressed by **table and offset**, and the table number says who
owns it:

```text
  table   0 ──── reserved, so an uninitialised field addresses nothing real
        1-24 ──── core: identity, platform, links
       25-49 ──── services
       50-99 ──── modules
```

On the wire that pair becomes one identifier, unique across the node and
decodable back to the pair it came from.

Scalars, strings and byte arrays all work. An array is written whole or not at
all, and its owner judges it as one value rather than element by element.

### Two rules that are easy to get backwards

**A validator refuses; a change callback cannot.** Validation runs before the
value reaches storage, which is the only moment where refusing it still leaves
the old value intact. By the time a change callback runs, the write has already
happened.

**Sampling and writing collide.** A value that samples hardware refreshes on
read. A write arriving over the link lands in storage and is then handed back
for the owner to apply — and sampling at that moment would overwrite the value
that just arrived, so the write would report success and change nothing. Reads
that exist to serve a write therefore skip the sample.

### From the ground

`CONFIG_KFSW_PARAM_CSP` adds the remote adapter, using the MIT-licensed
[Space Inventor libparam](https://github.com/spaceinventor/libparam) wire
codec, pinned as a separate west project. CSP itself stays in `kfsw-comms`.

A write from a ground node reaching two flight nodes, and staying there:

![Parameters read and written across a link](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/param-over-a-link.gif)

Descriptor listing runs over RDP by default. Without it a listing crossing a
radio arrives short: the packets are independent, so a lost one silently
removes a parameter from the list rather than failing the read.

### Persistence

One CRC-protected snapshot. Saving is explicit: a write changes RAM, and
`kfsw_param_persist_save()` is what makes it survive a reset.

Read-only means an operator cannot write it, not that it cannot be kept — a
boot counter is read-only and still has to persist, or it would restart from
zero every time.

## Boot

Startup markers, the latched reset cause, and the image version.

It also reads the note the previous run left behind. The event ring is RAM, so
what a node was doing in the moment before it went away is exactly the record a
reset destroys; `kfsw-platform` keeps one small record in memory that start-up
does not clear, and this service reads it, logs it, and emits it as an event so
it reaches the ground.

An **absent** note is informative too: it means the node lost power outright
rather than going down for a reason it could name.

## Log

One console stream with a level per module rather than one global level, so
raising CSP to debug does not drown everything else. Modules are selected by a
define before the include, so the call sites themselves are unchanged.

## Events

A bounded ring of numeric records: a stable identifier, a timestamp, a sequence
number, a severity, and a small opaque payload.

This is not the log. The log is for a human reading a console; the event record
is for a ground station reconstructing what a node did while nobody was
listening. Text is unaffordable to downlink at 57600 baud; numbers are not, and
a sequence number lets ground see that it missed records 41 to 47.

It is RAM only, so it does not survive a reset, and it counts what it
overwrites rather than quietly losing it.

## Commands

A command has a name, up to four typed arguments, and a typed result. Handlers
run to completion, and a command that changes something is marked as mutating.

Delivery is deliberately **at most once**. One packet carries the request and
one the result, with no transport-level retransmission underneath: the server
correlates the reply by request identifier but does not deduplicate, so a
silently resent `reboot` would run twice. A lost packet is a clean timeout, and
repeating it is the operator's decision.

There is no authentication. The request context reserves a source and an
authentication result, and the flag is always false.

## Health

Components register, report that they are still running, and the service
decides. It is what feeds the watchdog, so a component that stops reporting
stops the feeding and the board resets. Feeding a watchdog from a timer proves
only that the timer runs.

When it withholds the feed it leaves a note saying so, and withdraws it if the
component recovers — a reset arriving later for an unrelated reason should not
be blamed on a fault that had already cleared.

## Firmware update

An image arrives, is written into the secondary slot, checked against a
whole-image CRC32, and handed to the bootloader. Two routes exist: one over the
file-transfer service, and a direct block protocol with per-block checksums and
repeat, for links where a long stream will not survive.

Committing is separate from receiving. A transfer stops at a verified image and
stays there until told to flash, because an image that arrived intact is not
the same thing as an image you want to boot.

MCUboot verifies the signature and reverts an image that never confirms itself.
K-FSW adds no authenticity of its own beyond that.

## File transfer

Client and server over CSP, with RDP and CRC32 on every connection. RDP already
provides flow control, retransmission and ordering, so the application protocol
does not add a second retry layer on top. This is deliberately not TFTP, which
carries all of that because it usually runs over UDP.

Up, checked on the node, back down, and compared. The same CRC32 appears at
every step — generated, uploaded, read back off the node, downloaded — and that
repetition is what makes it a round trip rather than two separate transfers:

![A file sent and fetched back](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/file-transfer.gif)

### The layering rule

The service splits into an operation layer, a transfer engine, and a
reliable-transport interface:

```text
  ftp_client   ftp_server   ftp_transfer   ftp_protocol   ftp_store
                              │
                       only the ftp_link API
                              ▼
                        ftp_link_csp        CSP + RDP
```

One rule keeps that boundary checkable rather than a matter of judgement:

> No `#include <csp/...>` and no `kfsw_csp_*()` outside `ftp_link_csp.c`.

Everything the core needs from the transport comes through one header, so
changing the backend does not touch the protocol, the engine, or the storage
rules. That header also carries the ownership contract: a received message
borrows the transport's buffer, and releasing it ends the borrow — the lifetime
is in the API rather than in a comment.

### Rules that matter in flight

Every path is sandboxed below `/kfsw/ftp`. Traversal, empty components,
backslashes, control characters and embedded NULs are rejected.

A receiver writes to a `.part` file, syncs it, validates it, and only then
renames it over the final name — so a failed transfer removes the partial file
and leaves any existing file alone. The server handles one request at a time;
anything overlapping gets `busy`.

Version 1 has no resume, recursion, globbing, compression or encryption. The
protocol is K-FSW's own and claims no compatibility with anything else that
happens to use the FTP or TFTP name.

## Housekeeping

Reading a node one value at a time costs one round trip each. A **report** names
a set once, and afterwards a pass asks for the set.

A report holds identifiers rather than names — the `(table, offset)` pair the
wire already uses — because sixteen names would be 512 bytes of definition and
ground has the names anyway. It is **validated when it is defined**: every
parameter must exist and the values must fit one packet, so a report that
cannot be collected is refused there rather than discovered mid-pass.

Widths come from the declaration, not from what a value happens to hold, so
every sample of a report has the same layout and the tenth value can be read
without parsing the nine before it. An entry that cannot be sampled is
zero-filled and flagged rather than dropped: a short frame that silently
shifted everything after it would be worse than a marked absence.

```text
  one sample = one CSP packet
  a lost packet costs one sample, and the sequence number shows the gap
```

That is deliberate. Streaming under RDP would turn a bad pass into no answer
rather than most of one.

**The timestamp is when collection started**, and that is what the field is
called. Local values are read in a tight loop and are coherent to within it; a
remote value arrives over a radio and cannot be simultaneous with anything.
Claiming a snapshot would be a promise the protocol cannot keep.

Definitions survive a reset; samples do not. Periodic beacons — a node sending
telemetry unprompted — are deliberately absent: something that transmits by
design can flood a link, and that deserves its own floor and its own evidence.

## Not here yet

- periodic housekeeping beacons, sent without being asked
- a persistent event journal, rate limiting and coalescing
- authentication on the command path
- a flight planner

## License

Licensed under [Apache 2.0](LICENSE). Third-party dependencies retain their
own licences.
