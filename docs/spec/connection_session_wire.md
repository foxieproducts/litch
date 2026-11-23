# Shared connection protocol

The app and board use the versioned shared BLE envelope below. Decode a frame only after validating its epoch, session and message type. See the [settings interface](ble_settings_interface.md) and [pairing guide](ble_owner_pairing.md) for their message semantics.

## Endpoints and envelope

Under base service `6f2a20d1-89df-4ac6-a9f2-53f6f7db0100`, reserve ingress
`6f2a20d1-89df-4ac6-a9f2-53f6f7db0108` for Write Without Response and egress
`6f2a20d1-89df-4ac6-a9f2-53f6f7db0109` for Notify. Capability discovery uses
Control `0104` before activation and reports contract version **3**. That
capability version is distinct from this wire envelope's version **4**.
Pairing uses Ordered opcode 27 and State 40–42; no pairing characteristic is
registered. Missing or unsupported capabilities are unavailable, never a
request to select an older peer protocol. Current app and firmware move together;
stored data and hardware recovery remain separate obligations.

One GATT value contains exactly one packet; no trailing padding or concatenated
packets. All multibyte integers are unsigned little-endian. Header size is ten
bytes; the first negotiated packet bound is 20 through 512 bytes, constrained
by actual sender and receiver transport limits, not the requested MTU.

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 1 | High nibble version `4`; low nibble kind |
| 1 | 1 | Control code or domain opcode |
| 2 | 4 | Connection-control token or unidirectional stream token |
| 6 | 4 | Semantic sequence, or control-specific cursor/request ID |
| 10 | remaining | Body; exact length for each control |

Kinds are `0 Control`, `1 State`, `2 Patch`, `3 Ordered`, `4 Object`.
Other versions/kinds are invalid. Kind is not permission to bypass an object's
ordering boundary. Compatible unsent patches may merge before receiving a
sequence; ordinary mutations of one object use its one ordered admission owner.
After sequence assignment, kind, opcode and body are immutable.

The decoder validates before returning a borrowed view; invalid input publishes
no new packet. Firmware decode/encode allocate no heap memory; Dart encoding
returns an owned packet buffer. A borrowed input is valid only
during its owner's documented lifetime. Ingress copies admitted record identity
once into its reserved slot before returning the input buffer.

## Exact control records

Every record below has kind Control. Sizes include the ten-byte header. `C`
means the active connection-control token; `S` a stream token. Unless specified,
header sequence is zero. Reserved fields must be zero.

| Code | Name | Header token / sequence | Body, in order | Bytes |
| --- | --- | --- | --- | ---: |
| `01` | Hello | `0 / 0` | client nonce `u64`, receive packet maximum `u16` | 20 |
| `02` | Welcome | `C / 0` | echoed client nonce `u64`, selected packet maximum `u16` | 20 |
| `03` | Identity | `C / 0` | board boot nonce `u64`, features `u16` | 20 |
| `04` | Ready | `C / 0` | transcript CRC32 `u32` | 14 |
| `05` | Activated | `C / 0` | same transcript CRC32 `u32` | 14 |
| `06` | Open | `C / request ID` | topic `u16`, lane `u8`, capacity `u8`, maximum body `u16`, expected feedback ms `u16` | 18 |
| `07` | Opened | `C / same request ID` | new stream token `u32`, maximum body `u16`, capacity `u8`, lane `u8`, status `u16` | 20 |
| `08` | Progress | `S / terminal through` | confirmed through `u32`, grant through `u32`, received ahead `u16` | 20 |
| `09` | Checkpoint | `S / terminal through` | domain revision `u32`, domain digest `u32`, exception bits `u16` | 20 |
| `0a` | Outcome | `S / exact record sequence` | disposition `u8`, reason `u16` | 13 |
| `0b` | Confirm | `S / received complete dispositions through` | empty | 10 |
| `0c` | Query | `S / 0 or requested outcome sequence` | empty | 10 |
| `0d` | Close | `S / 0` | empty | 10 |
| `0e` | Closed | `S / 0` | empty | 10 |
| `0f` | Heartbeat | `C / 0` | empty | 10 |

Data sequences, Outcome sequences and Open request IDs start at one.
`UINT32_MAX` is reserved and never accepted as a sequence, cursor or request ID.
Confirm/Checkpoint may use zero. Query zero on an Ordered stream requests current
Progress and Checkpoint; Query zero on the implemented State stream requests its
exact retained latest value. Tokens and both nonces are nonzero except Hello's token.
Heartbeat is admitted only for the active connection token, refreshes link
activity at ingress, and deliberately creates no response or domain work.

Open's expected-feedback field is a stream-level repair hint, not a pacing
command or delivery promise. Zero means unspecified; `1..60000` milliseconds is
valid. It describes the healthy interval in which the opener normally expects
some cumulative feedback while work is outstanding. The first implementation
advertises 40 ms and probes after `3 * 40 + 1 = 121 ms`. A peer may adapt from
measured RTT/variance, but must not query an idle stream merely because the hint
elapsed.

Known feature bits are `0001 Ordered`, `0002 State`, `0004 Patch`,
`0008 Object`, `0010 Reverse Object`, `0020 Store Guard epoch`, and
`0040 Ordered mode`;
other bits are invalid in this version. The implemented Litchcraft slice
advertises Ordered, State, Object, Reverse Object, Store Guard, and Ordered mode.
Recognition of Patch does not implement its delivery.
Selected packet maximum is `20..512`, no larger than Hello or actual local
transport capacity. A smaller post-activation transport bound requires orderly
renegotiation/retirement before further oversized submission; do not truncate.

Open asks the peer to reserve a receiver for one unidirectional topic stream.
The reverse direction opens its own stream. Topic zero is reserved. Lane
`0 interactive` permits body `0..10` bytes; lane `1 bulk` permits body up to
selected packet maximum minus ten. Open's maximum-body bound is `1..502`, even
when particular operations have an empty body. Capacity is `1..16` for
interactive lane `0` and `1..64` for reverse/bulk lane `1`; each domain still
requests its own smaller fixed bound. Opened
success is status zero, with nonzero stream token and bounds no greater than
requested. Refusal status is an opaque nonzero `u16`; token, maximum body and
capacity must all be zero. The returned lane must equal the requested lane,
including refusals. Do not reinterpret an
unknown refusal as success. One lifecycle request is outstanding per initiator;
same request ID and exact request bytes replay the frozen response, allocating
nothing twice. Request IDs increase without reuse; older requests outside the
one retained lifecycle response are ignored. A changed request under the same
ID is invalid.

## Home state

Home publishes one 100-byte snapshot in ten State fragments, codes 4–13.
All ten fragments must carry the same snapshot identity. In Classic play,
when byte 8 bit 6 is clear, byte 71 reports the board's result. Values 0, 1,
2 and 3 mean ongoing, checkmate, stalemate and another automatic draw,
respectively. At checkmate the winner is the opponent of the FEN side to
move. While the CPU is thinking, bytes 71–74 carry its elapsed half-seconds
as a little-endian 32-bit integer. Earlier app builds ignore byte 71 while
the CPU is idle, and earlier boards report zero there.

Duck Hint guidance follows the current physical action. A ready chess hint
contains the chess source and destination. A pickup can add the legal chess
destinations. The suggested Duck source and destination appear once the
board accepts the chess move and enters Duck relocation. Apps reading older
compound suggestions display the step for the current action.

## Activation and lifetime

The central registers the exact notification endpoint before Hello. Board
Welcome and Identity form a bounded response to that Hello. The central checks
the echoed nonce, token, boot identity and selected limits. Ready carries the
existing repository CRC32 of exact encoded `Hello || Welcome || Identity`, in
that order, including headers. The board becomes ready after validating Ready;
the central submits domain work only after matching Activated. CRC32 detects
transcript mismatch; it is not authentication and does not replace pairing.

A lost Ready or Activated is recovered by repeating identical Ready. A lost
Open or Opened is recovered by repeating the exact same Open request; the
receiver replays its frozen response without allocating another stream. The
central uses the advertised repair timing for these lifecycle probes. If the
board-initiated State Open is missing, the central repeats identical Ready so
the board replays Activated and its retained reverse Open. Repeating Hello during
negotiation produces the same coalesced response. A different Hello cannot
replace a live session or erase its outcomes. Negotiation has one bounded slot
and a timeout; failure retires it rather than accumulating candidate states.

Connection generation belongs to the actual native link instance. All callbacks,
notification registrations and asynchronous completions carry that generation.
A disconnect retires its connection and stream tokens before subsequent work.
Tokens are allocated monotonically without reuse within a connection: board
allocations use bit 31 clear, central allocations use bit 31 set; the remaining
31-bit ordinal starts at one. The board-issued connection token shares its
allocator with board-issued stream tokens. Retire the connection before token
allocation wraps. Boot nonce and client nonce distinguish negotiated lifetimes;
an old callback must be rejected before interpreting its packet token.

A same-boot resumable object transaction, if implemented, has a separate domain
identity and lease. Reconnect reconciliation issues fresh connection-bound tokens
only after querying actual authority; it never replays an unknown ordered action
blindly. The transport defines no persistent format for partially received objects.

## Admission leases, not attempted-packet credits

**A receiver grant reserves semantic identities, not transmission attempts.**
For a stream, the receiver maintains `confirmedThrough <= terminalThrough <=
grantThrough`. Its capacity slots represent the interval
`(confirmedThrough, grantThrough]`; initially these are `1..capacity`.
The complete interval is reserved even when some packets have not arrived.
An exact retransmission uses its existing reservation and consumes no new remote
capacity. Native admission/completion is not receiver or domain progress.

If an issued Ordered native write fails, the app retires the shared session.
Queued actions and repair replays recheck lifetime and retained-record ownership
before handing work to the transport. Work already handed down may complete after
retirement. Without a previously received terminal outcome, the failed write's
domain result remains unknown until authority is reconciled in a renewed session.
Late notifications
cannot settle the failed caller again. Local validation or admission failure
before issue does not by itself retire the session.

The app Object producer may await native submission to pipeline another bounded
batch, or await cumulative peer admission. The shared session retains issued
bytes, admission/repair obligations and retirement in either case; submitted
completion does not complete a verified object. Queue bounds, negotiated grants,
fairness and cancellation boundaries are identical for both completion choices.

This deliberately replaces attempt-count credits: if native submission reports
success but the packet never reaches the peer, a packet-attempt counter would
leak credit forever. Here the missing semantic slot remains reserved. A Query
reveals the hole and the sender resends the same immutable sequence under that
same lease. No extra paint header, transmission ordinal or per-edit ACK is needed.

Receiver ingress checks lifetime, frame length, stream bounds and sequence
before copying. One slot retains exact `[kind, opcode, body]` bytes. Identical
retry is Duplicate without new storage or repeated domain execution; a different
kind/opcode/body at that sequence is a protocol conflict and cannot overwrite
the first record. Out-of-window, retired or malformed records are dropped with
bounded diagnostics/coalesced progress, not one queued reply each. Already
confirmed sequences never execute again and need no retained payload comparison.
An admitted envelope whose domain body/opcode is invalid instead receives one
retained Rejected outcome; domain validation cannot leave an unfillable hole.

Only the next contiguous sequence may enter domain application, once. The domain
may finish asynchronously; the next dependent operation waits for its terminal
result, not for a phone receipt. Physical input remains board-owned and independent.
Rejected or explicitly cancelled records also finish the ordered position.
The core's `terminalThrough` field is therefore called **terminal through** on the
wire: it must never imply that every disposition was Applied.

The initial implementation shares one reserved record slot across reception,
application and outcome retention. It retains bytes and outcome until Confirm,
then frees that terminal prefix and extends grant through, saturating at
`UINT32_MAX-1`. These are distinct progress facts with a deliberately conservative
shared allocation; there is no second full payload queue. Empty/outcome-complete
windows reopen before sequence exhaustion; identities never wrap in place.

## Progress and complete outcomes

Progress's received-ahead bit `j` means sequence `terminalThrough+1+j` is retained
(including an in-flight domain operation). Bit zero clear while later bits are
set identifies a hole. It proves admission only. The codec rejects confirmed
greater than terminal, terminal greater than grant, grant-minus-confirmed greater
than 64, and bits whose named sequence exceeds grant. The bitmap still names
only the next 16 positions. Stateful admission additionally enforces each
stream's selected capacity (Ordered wire maximum 16, production maximum eight).
Initial/final saturated windows are valid. Stateful checks reject regression of authority;
old control frames cannot decrease a cursor or revoke a newer grant.
The portable window's raw received bitmap is anchored at
`confirmedThrough+1`, including retained terminal records. The wire adapter
shifts it by `terminalThrough-confirmedThrough` before encoding received ahead;
it must not copy those differently anchored masks unchanged.

The sequential Object ingress uses the same Progress shape with
`confirmedThrough == terminalThrough == acceptedThrough` and `receivedAhead ==
0`. The initial grant is the negotiated capacity. It advances monotonically
when an admitted envelope releases its reserved raw-ingress slot, saturating at
`UINT32_MAX-1`; `acceptedThrough <= grantThrough <= acceptedThrough + negotiatedCapacity`.
Copying a completed packet into the bulk domain FIFO keeps its reservation
until that slot is released; a completed control reinserted into the ordinary
FIFO likewise keeps its reservation until the derived event is dequeued.
The shared Litchcraft adapter admits completed bulk logical packets up to 512
bytes, including reassembled packets. The receiver selects at most 26 Object
reservations, bounded by the callback FIFO and the unchanged shared byte arena;
26 maximum records plus one raw 512-byte record fit independently of ring wrap.
The callback FIFO holds 26 528-byte events (13,728 bytes), within its former
54 260-byte event budget (14,040 bytes). Nonbulk artwork events retain their
244-byte domain bound. Its existing bounded reassembly prefix may return fragment credit
immediately; only the completed packet retains a slot reservation. Direct diagnostic
bulk queue records can still use up to 512 bytes and consume proportional arena
space. Here “confirmed” means copied into the
domain-owned FIFO, not durably committed. Query only replays these cursors and
never creates credit. The sender retains at most the granted exact envelopes. A
zero Query on the Object token replays this watermark; if it is unchanged, the
sender retries only the exact retained envelope at `acceptedThrough + 1`.
Further unchanged valid replies repeat that earliest repair with bounded probe
backoff until progress resumes or the finite failure deadline retires the
session. Later retained envelopes are not burst into the sequential receiver
while an earlier hole prevents them from advancing its watermark.

A nonzero Query on the forward Object token requests the latest retained Store
receipt whose domain `request:u16` equals that sequence. Firmware republishes
the exact receipt bytes as one or more Litchcraft `0x40` fragments on the
existing reverse Object stream, with fresh reverse-stream sequences, without
rerunning the Store barrier. The serialized
Store owner reuses its one latest-response buffer; a different or unavailable
request produces no guessed response. Replay is fenced by the current connection
epoch. This receipt is domain evidence, not the Object admission watermark or a
generic durable Object checkpoint.

The central may report reverse Object packets received ahead in Progress, but
delivers them to domain consumers only after every earlier reverse Object
sequence is present. This keeps fragmented domain responses in order while a
missing packet is selectively repaired.

Object opcode `30` is the Current Game adapter. Its first fragment body is
`version:u8 = 1`, flags `u8`, complete-frame length `u16`, then exact frame
bytes; continuation fragments contain `version`, flags, then more exact bytes.
Flag bit zero is Begin and bit one is End; all other bits are zero. The complete
length is `8..20`, Begin appears only on the first fragment, and End appears
only when that fragment completes the declared length. Malformed or interrupted
assemblies are discarded without exposing partial input. One 20-byte firmware
slot admits a complete frame atomically to the existing settings worker, so
Object Progress advances only after that owner accepts it.

The reverse path uses the same fragment body on State code `30`. One retained
20-byte output slot and transfer-owned delivery token keep the exact reply until
the final fragment is accepted by BLE. Disconnect changes the connection epoch,
drops partial fragments, and fences stale worker completions. This admission
receipt is not a durable segment commit: the existing Current Game protocol's
commit and archive-ack frames remain the durable authority.

Checkpoint captures one coherent domain revision/digest at its terminal boundary.
Exception bit `j` names `terminalThrough-j`. Zero means Applied; one requires
that sequence's exact retained Outcome before confirmation. Bits referring to
sequence zero or below must be zero. All outstanding records fit this 16-record
range because grants remain anchored to confirmed through. Later domain changes
publish through their watch owner; an older checkpoint cannot overwrite them.

Outcome dispositions are `0 Applied`, `1 Rejected`, `2 Cancelled`. Applied has
reason zero; the other dispositions require a nonzero domain-owned reason.
Unknown nonzero reasons remain explicit rejection/cancellation. Query with a
nonzero sequence requests that retained complete outcome. A pending or already
confirmed sequence yields coalesced Progress instead; impossible future queries
do not allocate replies. Duplicate progress/outcomes are idempotent.

The sender issues Confirm only through a contiguous prefix for which it has
received a Checkpoint proving Applied or the complete exceptional Outcome,
including reason. Merely observing a Progress terminal cursor is insufficient.
The receiver accepts Confirm only through its actual terminal prefix. Repeated
or older confirmation does not free additional capacity. Lost Confirm is resent;
lost updated grant is recovered by Query. Cumulative Checkpoint and Confirm can
retire many records at once; nothing requires stop-and-wait per edit.

Application revision/digest is not durable adoption. Persistence and verified
object Commit retain separate domain-owned evidence. A CRC is not a permission
to replace unseen physical edits or to regenerate the domain's authoritative state.

## Scheduling, memory and failure bounds

Window reservations must fit actual total memory before Opened succeeds. Budget
record identity bytes, terminal outcome/reason, slot bookkeeping, stream metadata,
local raw ingress, native egress and existing transfer staging together. A
ten-byte interactive body needs twelve identity bytes when kind and opcode are
retained; no on-wire enlargement follows. Bulk cannot reserve the interactive
pool. Retaining bulk bytes in a slot and another staging buffer counts both
copies; prefer a bounded ownership transfer when practical, not fictitious reuse.

Local raw ingress and native submission queues remain separately bounded.
Reservation never permits unlimited duplicate packets, queued copies or service
work. Reserve ingress and egress control capacity outside bulk admission, so a
full data window cannot block Confirm, Query or lifetime recovery. Native adapters
admit only when actual platform readiness allows; firmware
notification enqueue failure retains the same bounded pending control/data item.
A cumulative reverse Object receipt that retires a selected retry also releases
that output selection, so its freed slot cannot block subsequent control replies.

Cancellation of a local Object producer retires only its unissued suffix. The
session owner preserves every issued sequence, admission obligation and pending
native write, and wakes a producer waiting for grants. Logical-message fragment
boundaries remain intact: cancellation cannot strand a partial domain packet or
let a later producer inherit its prefix. Cancellation is an intent result,
not a send failure that retires unrelated producers or the connection. The Store
adapter then sends its existing Ordered Cancel without waiting for new grants
to issue bytes that have already been abandoned. Worker-owned discard and actual
slot release remain responsible for returning held receiver credit.
Each service call and each stream has bounded work. Eligible bulk gets a turn
after a bounded interactive burst, without reordering same-object mutations.

Control output uses fixed pending state per active stream: latest Progress,
latest Checkpoint, a bounded exceptional-outcome request bitmap, and one lifecycle
reply. The Object stream has its own coalesced latest-Progress slot. Repeated
malformed/stale/duplicate traffic updates these slots at most;
it cannot create a reply list. Errors and completed drain slices request progress
immediately. Under continuous traffic, publish after eight new terminal records
or five milliseconds from the first unpublished terminal result, whichever
occurs first; this is a publication deadline, not packet pacing. Platform
unavailability remains separately observable. Quiet links have no polling timer.

Recovery timers exist only while negotiation/work/confirmation is outstanding.
The first Query uses the negotiated/measured probe interval; valid unchanged
Progress clears the pending probe and backs later probes off to avoid a repair
storm. Explicit holes still trigger exact selective retransmission. Probe delay
is separate from the finite no-progress failure deadline (initially two seconds),
so a slow or variable link degrades in responsiveness before it is retired.
For Ordered work, `receivedAhead` bit zero proves retention of the current
nonterminal head (`terminalThrough + 1`), not domain acceptance, durability or
success. That head receives a separate fixed completion allowance, configured
by the runtime adapter (initially twenty seconds). Only a valid unchanged Query Progress
snapshot refreshes the two-second reply-silence bound while this head remains
retained. The wire has no per-Query correlation; an advanced snapshot must be
followed by stable evidence within that short bound. Unsolicited tail bitmap, State and unrelated traffic must not refresh
reply silence or restart completion. A successor earns its allowance only when
it becomes the retained head. Missing positions, Object recovery, lost terminal
Checkpoint/outcome and Confirm repair retain ordinary no-progress recovery;
terminal delivery remains the only completion authority. Callers must allow the
shared completion budget plus their admission and authority work, excluding
predecessor waiting through existing lifetime cancellation.

Concrete tuning belongs to the measured runtime adapter. These timers must not
pace healthy transmission or poll an idle stream.

Close is graceful stream retirement, allowed only when the sender has no
unconfirmed issued work and the receiver has no unconfirmed, retained or in-flight
work. Otherwise the receiver keeps the token and requests Progress. Closed is a
bounded replayable lifecycle response; retain the last closed-token response in
the receiver's lifecycle slot so a lost Closed can be retried without reviving
the token. Close is not cancellation: cancelling an
object/action requires its explicit domain operation and terminal disposition;
disconnect may leave an unknown result requiring authority reconciliation.


### Diagnostic Console

The diagnostic Console streams newline-delimited records on its own subscribed
characteristic. Each notification carries at most `min(ATT_MTU - 3, 244)`
bytes for the current connection. The board waits at least five firmware
millisecond ticks after each submission returns before trying the next one. A
rejected submission retains its unsent bytes. The client assembles records
across notifications and negotiated MTU changes. The `support` command reports
the number of writes that encountered a full Console buffer since boot as
`console_overflow_writes_total`.

### Identify binding

Ordered opcode **28** uses the existing epoch-4 Ordered stream. Its exact
8-byte v1 body is `[1 action number red green blue durationLE16]`. Actions are
Probe=0, Start=1 and Stop=2. Probe and Stop require all remaining bytes zero;
Start accepts number 0..99 and duration 250..60000 ms. Unknown versions, actions,
lengths and out-of-range values reject without changing presentation.

An accepted Start feeds the existing transient presentation owner: its deadline
starts when the renderer consumes it, and the numeric cue refreshes every 1000 ms (150 ms for the
existing OTA two-digit presentation), Quick Setup retains its existing priority,
and expiry or Stop restores the underlying mode. The renderer and VirtualLitch
reuse this same timing owner. A receipt replay cannot restart its deadline.

Probe is an applied no-op: its ordinary domain Result announces support.
A rejected Probe makes Identify unavailable; the app sends no direct command. This adds
no common feature bit, State key, characteristic, capability-mask bit or envelope
version. Existing Identify capability bit 2 remains unchanged. Private peers
must never fall back to direct Console Identify before ownership. State 43 is
reserved for the separate assistance binding and is not used by Identify.

Identify alone is admitted before product ownership; it invokes the existing
bounded transient Identify presentation. Settings, Console, artwork, history and
OTA keep their current guards. Recovery-only mode remains unsupported. Stop and
Probe are free. Only a new Ordered Start consumes allowance; duplicate delivery
uses the common receipt and cannot restart presentation or consume allowance.

Eight fixed RAM buckets use the SDK's resolved peer identity when available,
otherwise the typed native address. They survive reconnect, allocate no heap or
flash, and never evict active accounting. An unresolved RPA may change across
links and cannot establish persistent peer identity. Three new Starts are
allowed; the fourth starts a fixed 300000 ms ignore window. Ignored requests
never extend that window. Idle partial buckets expire after 300000 ms. A full
active table denies only Identify, never connection or pairing. Rejected domain
Result reasons are `0x1c01` cooldown, `0x1c02` capacity and `0x1c03` unavailable
native peer identity. Common malformed/unsupported results retain their values.

### Owner pairing

ESP32-S3 builds with `LITCH_OWNER_PAIRING=1` use Ordered opcode 27 and State
keys 40, 41 and 42 (decimal). See the [pairing protocol](ble_owner_pairing.md#protocol).
They use the existing epoch-4 streams; there is no pairing characteristic.
Pairing shares the current 33-key reverse State capacity with the other domains.
A Query
for key 40 rearms status and both identity halves through the same retained
State owner. Before verified ownership, State replay admits only those three
keys and suppresses product reverse Objects, including an output retained after
a failed notify. Ordered completion remains available for Begin/Cancel, with
protected domain revision/digest fields zeroed in Checkpoints. Domain ingress
admission remains with the BLE adapter, independently of generic envelope credit.

The reverse stream uses a central-issued token and adds no firmware task, timer,
or domain queue.

Domain registration and all field validation belong to the Litchcraft adapter.
Tool changes with resting pieces remain ordered actions, never replaceable
passive state; the State stream is their authoritative readback, not their
mutation path. Store encoding is unchanged; only its initial transaction
packets and exact receipts gain the connection-session envelope.

To test interoperability, drop the first, middle or last packet or its receipt.
Exercise duplicate and out-of-order packets, including duplicates with different
opcodes and empty bodies. Drop Checkpoint, Outcome, Confirm and grant messages
in turn. Try delayed completions, a rejection at the window edge, a stalled
stream, floods of stale or malformed packets, sequence or token exhaustion,
a transcript mismatch and a disconnect at each transaction boundary. Check
that memory and work stay bounded, duplicates never repeat a domain action,
an unrecognized rejection cannot retire a request, and each stream recovers
when delivery resumes.
