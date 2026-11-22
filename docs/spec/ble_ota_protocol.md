# BLE OTA Protocol

The companion app and firmware use one current BLE session contract: capability
contract **3**, shared wire envelope **4**, and the typed OTA transaction below.
These numbers identify different layers. If discovery returns missing, malformed
or unsupported capabilities, reconnect and read them again. If the current
contract still cannot be established, install the current S3 firmware over USB using the
[firmware guide](../release/ci_foundation.md#usb-installation-and-recovery).

Hardware revision and storage layout are separate from the communication
contract. V2 and V3.0 ESP32-S3 boards can have the earlier 2 MiB A/B partition
map. Ordinary USB updates preserve the installed partition table, settings,
saved games and recovery image when the image fits.
Changing to the larger map requires deliberate full-erase factory provisioning
and discards saved data. The V1 ESP32-C3 does not advertise app OTA.

## Current endpoints

All endpoints below are registered under base service
`6f2a20d1-89df-4ac6-a9f2-53f6f7db0100`:

| Endpoint suffix | Current purpose |
| --- | --- |
| `0104` | Control Write/Write Without Response + Notify; mobile `info` bootstrap before the shared session; current developer maintenance control |
| `0108` | Shared session ingress Write/Write Without Response |
| `0109` | Shared session egress Notify; retained State, results and reverse Objects |
| `0105` | Raw image data for the current developer maintenance updater before session activation; never the companion app's image route |

The app registers Control notifications, reads current `info`, activates the
shared session and retires that direct notification subscription. App OTA
controls then use Object opcode `0x32`, image chunks use `0x31`, and exact
transaction status returns through reverse Object `0x32`. See the
[shared wire reference](connection_session_wire.md).

Recovery retains Control and session endpoints while withholding ordinary
product domains. Its mode flag, persistence rules and post-boot proof are
mandatory; missing ordinary Settings endpoints are valid in recovery.

## Phone update sequence

```mermaid
sequenceDiagram
  Phone->>Board: Read info (capability contract 3)
  Phone->>Board: Activate wire 4 session; establish owner readiness
  Phone->>Board: Validate source proof; typed Begin; validate admission proof
  Phone->>Board: Image chunks; await Progress and final receive acknowledgement
  Phone->>Board: Persist target, then End; board flushes required saves and reboots
  Phone->>Board: Reconnect; prove selected/running slot, attempt, version and digest
  Board-->>Phone: Correlated VALID image and runtime acceptance: Verified
```

A disconnect after End leaves verification pending until the fresh connection
proves the exact update attempt. Transfer completion alone cannot verify it.

## Transaction commands and developer maintenance interface

The same transaction owner handles the typed app route and the existing
pre-session raw developer maintenance interface used by `./litch flash s3` with a selected BLE target.
The maintenance writer must verify the current advertised capabilities and
negotiated transfer profile. After reboot it verifies the running image. See the
[developer tool guide](../../tools/README_ble_update.md).

### Host -> board

1. Optional version probe:

```json
{"cmd":"fw_version"}
```

2. Start transfer:

```json
{"cmd":"ota_begin","size":842256}
```

3. Stream raw firmware bytes over OTA data.

4. Finalize:

```json
{"cmd":"ota_end","size":842256,"crc32":"89abcdef"}
```

5. Optional abort:

```json
{"cmd":"ota_abort"}
```

On firmware with durable Litchcraft artwork, a valid `ota_end` waits for the
current artwork, mode and prepared game/settings snapshot to reach storage
before finalizing the image or reporting `done`. Pending completion remains
owned by the initiating connection and transfer; disconnect or abort cancels
it. A temporarily unpreparable game snapshot (for example, promotion or undo)
is retried during the ten-second persistence window instead of being reported
as a storage failure. Confirmed storage failure aborts with `persist_failed`;
an admitted write timeout uses `persist_timeout`, and a snapshot that never
becomes ready uses `persist_not_ready`. None finalizes the image. A completed
save is observed on the next service pass, including at the
deadline. The timeout only rejects a still-unresolved finalization. An exact
duplicate end request shares the pending completion. Simulation does not require
a persistence flush. A valid end request pauses Store artwork edits while its
frame, mode, replacement root and settings saves finish. New bulk artwork
objects are rejected while the image is active or awaiting its reboot;
abort or failure releases the pause and reopens bulk ingress.
After successful finalization, the pause remains through the existing restart
grace period so newly accepted paint cannot be lost at the reset.

### Board -> host

- Version response:

```json
{"v":"0.63-beta"}
```

- Begin accepted:

```json
{"st":"begin","pct":0,"rx":0,"total":842256}
```

- Progress heartbeat while data is being written/flashed:

```json
{"st":"progress","pct":37,"rx":314572,"total":842256}
```

Low-MTU fallback may collapse this to compact progress only:

```json
{"p":37,"r":314572}
{"r":1000000}
```

The receive-only form is used when percentage plus a seven-digit receive count no longer fits the minimum 20-byte ATT payload. Hosts must treat either `rx` or `r` as a receive acknowledgement even when no percentage is present.

Low-MTU error fallback keeps the same `{"e":"..."}` shape but uses stable
compact aliases whenever the canonical token would overflow the 20-byte ATT
payload, for example:

```json
{"e":"inv_begin"}
{"e":"overshoot"}
{"e":"wrong_size"}
{"e":"crc_mismatch"}
```

Current low-MTU OTA error aliases are `invalid_begin -> inv_begin`,
`invalid_test_args -> test_args`, `notify_test_failed -> notify_fail`,
`reboot_pending -> reboot_wait`, `image_too_large -> image_large`,
`not_in_progress -> idle_ota`, `size_overshoot -> overshoot`,
`size_mismatch -> wrong_size`, `checksum_mismatch -> crc_mismatch`,
`invalid_chunk -> bad_chunk`, and `chunk_too_large -> large_chunk`.

- Terminal states:

```json
{"st":"done","pct":100,"rx":842256,"total":842256}
{"e":"size_mismatch"}
{"e":"checksum_mismatch"}
{"st":"aborted","pct":0,"rx":0,"total":0}
```

### Shared-session data path

On wire envelope 4 with capability contract 3, the app begins and finalizes the
existing transaction through shared-session Object opcode `0x32`, while raw
image bytes use Object opcode `0x31`. Control JSON uses bounded version/Begin/End
fragments and becomes the existing OTA-control event only after complete
reassembly. Each data Object body is one exact image
chunk bounded by the negotiated Object body maximum. The app batches a negotiated
burst behind Object credit and always flushes a final partial burst. Its batch
completion waits for native submission, allowing the next burst to use remaining
grants while the shared session retains every peer admission and repair
obligation. The existing `sent - rx` window and final receive acknowledgement
remain mandatory; native completion never authorizes End. Firmware
admits a sequence only after the existing packed OTA/artwork FIFO copies it.
Object Progress is transport admission, not flash durability or image validity.
Before Begin, the app drains previously queued and issued Objects on the same
connection generation through their existing admission futures. While the upload
owns that connection, including its unresolved End proof on that same upload
generation, the shared sender refuses new non-OTA Objects before
allocating a sequence. Once the active upload has returned, cached public Support
`0x45` remains available while verification is unresolved; private Support and
product Objects remain fenced. OTA control and image Objects remain available.
This keeps background artwork inspection from reaching the firmware's OTA bulk-work
refusal and blocking later image data on the same ordered Object stream.
Control, State and Ordered recovery/cancellation remain live. The drain reuses
existing submission cancellation, recovery deadlines and retirement; it adds no
queue, polling loop or separate timeout. Admission never establishes domain
completion, flash durability or image validity.
With a shared session active, exact status JSON uses reverse Object code `0x32` on the
Object token. Version-1 Begin/End fragments fit the negotiated Object body;
the first declares the total `u16` length and continuations carry payload after
their two-byte version/flags header. One retained latest status may coalesce
obsolete progress while terminal/error JSON remains exact. The app reassembles
the record into its existing typed OTA control channel and retires that
channel's direct GATT subscription. Active-session firmware does not duplicate
status on the direct Control characteristic.
Artwork and firmware data use the same calibrated shared-session packet bound:
the production app selects up to 495 bytes from the actual native write-value
capacity, and OTA image chunks use the selected Object body (up to 485 bytes).
Firmware supports full 512-byte packets and 502-byte bodies when selected.
The receiver grants at most 26 records within its unchanged FIFO/byte budgets;
larger packets do not imply more reserved RAM or an extra transport. Transfer
startup does not renegotiate MTU after activation. See the
[wire contract](connection_session_wire.md#activation-and-lifetime).
The cooperative receiver collects credits from released FIFO slots both on
entry and after its bounded drain. A data-only drain submits the resulting
current-stream Progress through its existing notification flush in the same
pass. Held slots retain their reservations; failed notification admission keeps
Progress pending for the normal retry. Credit return does not establish flash
durability or authorize finalization.
The existing cooperative BLE owner makes at most three fast-interval requests
per physical connection. Native callbacks publish timing and completion evidence;
the owner issues requests from its normal Update. The first two retain Android's
7.5 ms interval and 2 s supervision timeout. The final request waits for shared
session/owner readiness and completion or synchronous refusal of the existing
PHY preference. An already-observed 2M PHY needs no preference request. Startup
collisions therefore cannot exhaust the final request before bootstrap. Its
fallback requests 15/15 ms, zero latency and a 6 s supervision timeout, matching
[Apple's current peripheral-request guidelines](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf)
section 57.6. The central owns the actual interval and may retain 30 ms; a
requested tuple alone never establishes an accepted faster link.
The transaction rules below govern both current entry points. Before session
activation, the developer maintenance interface retains Control notifications
and raw image data. After activation, direct OTA control and raw firmware-image
ingress are rejected; one transfer cannot mix shared Object order with native
callback order. Litchcraft artwork/document packets use shared Object code
`0x40`; the app never selects a raw characteristic after failed negotiation.

- `ota_begin` means the board has accepted the transfer size and, for real flashing, `Update.begin(size)` succeeded.
- Maintenance data writes carry raw bytes only; wire-4 Object envelopes supply
  transport sequence. Board progress notifications still provide cumulative
  transaction receive acknowledgements.
- Schema-3 control and data Objects receive one shared monotonic ingress order.
  Complete controls preserve their final Object ordinal when reclassified into
  the existing OTA-control event queue; the main loop merges that queue with the
  packed chunk queue, so `ota_begin`, its chunks, and `ota_end` cannot be
  reordered by their storage paths. Starting a transfer must never clear
  already-accepted same-session chunks. Pre-session direct writes retain their
  established callback ordering.
- OTA chunk events are drained from the firmware main loop as often as the loop runs; they are not intentionally rate-limited to the old IO cadence.
- Real flashing may stage multiple OTA chunks into a larger fixed-size firmware buffer before calling `Update.write()`. This does not change the wire protocol or `ota_end` checksum rules; it only reduces flash-write call frequency.
- Progress is board-originated only. The board emits shared status records (or
  pre-session OTA-control notifications) at most every
  `MIN_OTA_STATUS_INTERVAL_MS` and whenever the expected size is fully received.
- `ota_end` is the only hard transfer boundary. If the board's received byte count or CRC does not match, the board aborts and reports an error.
- Reliable status records remain retained through complete shared-session
  fragmentation; producer state advances only after bounded retention succeeds.
  Latest progress can coalesce, while read replies and terminal results cannot
  interrupt a reliable record. Credit/repair control remains serviceable behind
  a blocked reply, and safety abort retains one bounded terminal intent.
- After the final terminal `done` fragment is successfully submitted to NimBLE,
  firmware keeps pumping the shared session for a wrap-safe 500 ms delivery
  grace before rebooting. Retention in the reverse window alone does not arm
  that grace. If the peer disconnects or retires first, finalized firmware still
  reboots after the same grace from retirement. At the end of delivery grace,
  firmware requests one native GAP disconnect for a surviving link before
  restarting, so the phone need not infer a silent reset from supervision
  timeout. Restart follows the native disconnect callback, with a wrap-safe
  250 ms fallback if the SDK rejects the request or no callback arrives.
  Already disconnected firmware keeps the original 500 ms boundary. This does not weaken the host's
  obligation to verify the installed version or digest after reconnecting.
- Raw OTA data ingress uses a dedicated OTA chunk ring buffer rather than the generic BLE event queue so firmware can drain upload traffic without copying every packet through the larger mixed-event path.
- Connection availability is a fixed-size latest-state projection rather than
  ordinary command ingress. A full command queue cannot discard the only
  disconnect or replacement callback: firmware reconciles the newest lifecycle
  state before dispatching any current-generation control or data item.
- Settings, Litchcraft, console, and OTA subscription state belongs to the
  connection epoch that produced it. A subscription received before lifecycle
  activation remains effective, including one concurrent with activation;
  replacement connections never inherit an older epoch's subscription.
- If the BLE link drops during OTA, firmware aborts the in-flight transfer, clears OTA overlay state, and returns to the prior render state before resuming advertising.
- Disconnect retirement occurs at that same lifecycle reconciliation boundary.
  A replacement OTA admitted later in the drain cannot be aborted by the prior
  connection's deferred cleanup on the next loop.
- On error/abort, firmware must clear OTA overlay state and return to the prior render state.

### Firmware transaction boundary

- Every accepted begin declares one immutable positive image size. Older
  firmware builds retain the prior compiled `2 MiB` ceiling. New firmware
  running on the historical smaller partition table passes its configured S3 `3264 KiB`
  ceiling first, but its OTA platform adapter reads the actual `2 MiB`
  next-update partition and rejects an oversized declaration before calling
  `Update.begin`.
- The existing `ota_probe` response is the strict-profile gate. After a client
  probes on a connection, its subsequent v2 transfers must include the same
  declared size at `ota_end` and an exactly eight-digit hexadecimal CRC-32.
  The internal unprobed transaction handler tolerates omitted end claims for the
  deployed early-V2 updater, but any claim it does send must be well formed and
  match. Both profiles always require the received count to equal the immutable
  begin size.
- Raw chunks must contain 1–512 bytes and must not exceed the negotiated chunk
  size or the remaining declared image bytes. Firmware rejects an invalid or
  overshooting chunk before copying it into the write stage, updating CRC, or
  calling `Update.write`.
- CRC-32 uses the deployed IEEE convention (initial and final XOR
  `0xffffffff`, reflected polynomial `0xedb88320`). Finalization flushes the
  fixed write stage and calls `Update.end(false)` only after size and CRC checks
  pass. Firmware never asks the update library to force-finalize missing bytes.
- A transfer with no accepted chunk for 30 seconds is aborted using wrap-safe
  unsigned-millis arithmetic. Disconnect, BLE shutdown, explicit abort,
  backend failure, and validation failure use the same idempotent cleanup path
  to abort `Update`, discard staging, clear the OTA overlay/session, and
  invalidate queued bytes.
- Connection/rebegin epochs label queued chunks. Bytes owned by an older epoch
  are discarded after reconnect or replacement begin and cannot mutate the new
  transaction. If the bounded active-session chunk queue fills, firmware
  reports `queue_full` and aborts instead of silently continuing with missing
  bytes.
- A control packet classified as a begin candidate owns its new epoch even if
  full JSON parsing, size bounds, or command resolution later rejects it.
  Malformed,
  duplicate/nested-command, busy, or otherwise rejected candidates immediately
  terminate the prior active transfer and invalidate candidate-epoch data; they
  never leave the prior transaction active under an obsolete epoch. Malformed
  JSON reports `invalid_json`; scanner/parser command disagreement reports
  `invalid_begin` (compact low-MTU alias `inv_begin`); an oversized
  recognizable begin that cannot be queued also reports `invalid_begin`; a
  begin candidate rejected because the shared control queue is full reports
  `queue_full`; other rejected begins retain their specific error token or
  documented compact low-MTU alias.
- Raw data before begin is ignored. A begin received during an active transfer
  explicitly aborts and replaces that transfer. A repeated end after successful
  finalization is idempotently acknowledged as `done`; an end with no active or
  completed transaction reports `not_in_progress`.

### Image boot-policy descriptor

Firmware images contain a 16-byte descriptor immediately after the first segment's complete
256-byte SDK application descriptor. Its bytes are covered by the existing
image checksum, appended image SHA-256 and whole-file artifact SHA-256.
The descriptor uses explicit little-endian fields:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `FXBT` |
| 4 | 2 | Descriptor schema `1` |
| 6 | 2 | Descriptor size `16` |
| 8 | 4 | Boot-policy epoch: `1` record-dependent, `2` SDK-only |
| 12 | 4 | Reserved, zero |

The current producer declares epoch **2**: SDK OTA metadata is the sole boot
selection and rollback authority. Epoch 1 identifies the retired record-dependent
policy and cannot be activated as a new upload. Generic integrity readers still
verify an older already-VALID fallback. This descriptor is separate from the
shared-session wire epoch.

Integrity verification finishes before exposing descriptor evidence. Parsers
read only this position within the first segment; they never scan other
segments or join bytes across a segment boundary. A verified SDK descriptor
without exact `FXBT` magic reports Absent. Recognized magic with fewer than
16 bytes, wrong schema-1 size or nonzero reserved bytes reports Malformed.
A complete unknown schema reports Unsupported without interpreting its epoch;
schema 1 with an unknown epoch also reports Unsupported. Known epochs 1 and 2
report Known with that epoch. If a generic tool cannot establish the complete
SDK descriptor and its magic, boot-policy evidence is Unavailable; its existing
generic image-integrity rules remain unchanged.

Absent, Unavailable, Malformed and Unsupported are not permission to activate
an image or proof that it uses the old record policy. Capability inspection is
separate from NEW-trial admission, and a generic parser must still be able to
verify an older already-VALID fallback. Firmware upload finalization requires
Known epoch 2 before SDK activation. The app also requires it before Begin: a
complete unsupported image in a fresh download, immutable cache, verified
`.part` or `.previous` location is refused without promotion or transfer.
The upload service revalidates the actual file against the selected manifest
and policy, rather than trusting preparation metadata or caller-supplied hash
strings.
The descriptor does not change the release-manifest schema; host/app artifact
inspection remains separate from their activation-admission owners. The shared
full-image vectors are in
[`test/fixtures/firmware_boot_policy.json`](../../test/fixtures/firmware_boot_policy.json).

The descriptor symbol is explicitly retained against linker garbage
collection. The canonical S3 build verifies its unique producer, 16-byte
section, placement after the first SDK descriptor, complete binary integrity
and current epoch. A source declaration or compiler `used` attribute alone
does not establish a retained, correctly placed descriptor.

### Post-boot verification transaction

ESP-IDF OTA metadata is the sole selection and rollback authority. The two
32-byte selector records, actual supported OTA0/OTA1 partitions, actual running
partition and SDK-configured boot partition form one owned snapshot. The
adapter verifies the exact two-slot geometry, nonoverlapping partition extents,
raw record CRCs and SDK active-selector agreement. A state query for a partition
alone is insufficient: the pinned SDK returns the first same-slot record,
while boot mutation acts on the active selector. Sequence zero, unknown states,
missing/corrupt observations and selected/running disagreement cause no boot
mutation. Raw INVALID/ABORTED records remain failure evidence; healthy fallback
runtime does not prove that a requested update succeeded.

The existing Core-0 worker initializes before user settings/storage and normal
presentation. Read-only boot preflight never mounts LittleFS, loads a record,
hashes an image, confirms an image or requests a restart. The old
`/ota_post_boot.bin`, `.tmp` and `.bak` files remain inert on user storage; this
transition does not erase them or any game, settings, artwork or NVS data.
The USB-specific `LUSB` confirmation exception is removed.

Customer BLE updates preserve the currently SDK-confirmed VALID image and
write the distinct inactive slot. Before any target write, one request on that
same worker verifies the complete source image through the production software
`OtaImageIdentityCollector` and rechecks its exact raw selection. Pending,
unknown, unconfirmed, corrupt and near-wrap sources are rejected. The adapter
rechecks the source and target under the begin lease, validates the full
ordered incoming image, and requires Known boot-policy epoch 2 before
`Update.end(false)`. A fresh source/target check shares the final activation
lease. Finalization is successful only after raw NEW-selector readback identifies
the captured target and preserves the prior VALID selector. Accepted game,
settings and artwork saves still drain through their existing owners before
finalization; a separate filesystem boot-authorization record is not written.
When flashing over USB, the CLI writes OTA0 and marks it VALID using esptool.
An ordinary flash preserves the partition table and saved data. It can replace
the rollback image. The host uses esptool’s transfer verification and does
not verify the running image after reboot. If the transfer fails, the CLI
reports the error and how to retry on that board. Factory provisioning erases
the flash. See the [USB installation guide](../release/ci_foundation.md#usb-installation-and-recovery).

The S3 `version` diagnostic also prints the existing session `boot_nonce`, a
cached `ota_boot` sample number/age/availability, `ota_sdk` running and selected
addresses/selector/sequence, and both raw 32-byte selector entries. These are
bounded RAM copies of observations already taken by the SDK worker or adapter;
these evidence getters do not initiate flash reads or hashing. The existing
image-digest request retains its asynchronous digest owner. `version` itself uses
only the existing cached digest getter and never starts an image scan; an explicit
`fw_digest` retains its worker behavior. Unavailable or contended
evidence is explicit. The cache is observation, not fresh mutation authority,
and SDK state is separate from the existing `ota_accepted` runtime-health bit.
`ota_admission` reports the current boot's latest adapter attempt, phase,
source/target addresses and source/expected sequence. Receiving is published
only after successful platform Begin; End must read back the exact expected
sequence. Aborted or failed attempts cannot remain Receiving. These diagnostics
support exact-attempt hardware experiments. The current typed app attempt-proof
and durable pre-End target extend these same owners as specified below; the
compact acceptance response alone is insufficient.

### Correlated update-attempt proof

The current app extends the existing typed OTA control/status adapter. On an
activated shared session, `{"cmd":"ota_state","proof":1,"q":N}` requests
proof version 1 with a positive uint32 request ID. No new characteristic,
subscription or delivery mechanism is introduced. The response is one bounded
JSON object on the existing OTA Object/State status stream:

```json
{"op":1,"q":1,"n":"0123456789abcdef","v":1,"s":[1,0,0,0,0,23,2,23,2,1,24,4,1],"a":[0,0,255,255,0,0]}
```

- `op=1` identifies this exact schema; `q` echoes the request. `n` is the
  nonzero 16-digit lowercase hexadecimal session Hello boot nonce; `v` is
  normal-runtime acceptance, exactly 0 or 1.
- `s` has exactly 13 uint32 scalars: SDK cache sample, unsigned age in ms,
  running slot, selected slot, selected selector sector, selected sequence,
  running-state enum (0 unknown, 1 pending, 2 VALID), then sequence/state/CRC-valid
  for selector 0 and selector 1. Slots and selector sectors are 0 or 1;
  CRC-valid is 0 or 1. Raw SDK states are NEW=0, PENDING_VERIFY=1, VALID=2,
  INVALID=3, ABORTED=4, UNDEFINED=4294967295. The supported S3 partition
  identities must be corroborated before encoding slots.
- `a` has exactly six uint32 scalars: this boot's latest adapter attempt,
  admission phase (None=0, Receiving=1, Activated=2, Aborted=3, Failed=4),
  source slot, target slot, source sequence, expected target sequence. None
  uses attempt/sequence zero and slots 255. A Receiving admission has a
  positive attempt/sequence and distinct supported source/target slots. The
  source selector sector is `s[4]`; the target sector is `1 - s[4]`. Persist
  those sectors, rather than deriving them from partition-slot parity. The
  admitted target sequence is authority; clients must not replace it with an
  inferred source-sequence-plus-one.
- A contended, unavailable or incoherent copy returns only
  `{"op":1,"q":N,"e":"unavailable"}`. Unsupported/malformed requests never
  fall back to a weaker success proof. The response is at most 223 bytes plus
  its terminator within the existing 224-byte status owner; the full scalar
  maxima above encode in 196 bytes. Minimum-MTU fragmentation and reliable
  reply retention remain owned by the existing shared session.

The producer copies existing RAM-only SDK and admission evidence, checks
admission stability across the copy, and reports unavailable on disagreement.
It initiates no flash read, hash, confirmation or mutation. Cache age is
diagnostic, not a fabricated freshness deadline: a stable VALID observation
may legitimately age. A usable response must instead belong to the current
request, current native/session generation and corroborated Hello nonce, with
coherent running/selected/CRC/sequence evidence. Receiving proof must match
the adapter's current admitted source and immutable target. Post-reboot proof
must report the admitted target as running and selected, its exact expected
selector/sequence VALID, a changed nonce, exact selected artifact identity and
normal-runtime acceptance. Both success and failure also require the preserved
source at its exact stored sector/sequence with VALID state and valid CRC. A
newer unrelated sequence cannot satisfy it. Every `op`-bearing envelope is
reserved for strict proof decoding before generic version/error fields; an
unavailable proof never becomes a terminal OTA error.

Before Begin, the current app requires current correlated proof with a
coherent running/selected VALID source. Normal peers must also report normal
runtime acceptance; a not-yet-accepted normal source remains refused with a
keep-powered/try-again action. An explicitly reported recovery-only peer on the
current connection may have acceptance false, since recovery intentionally
never publishes normal acceptance. Its firmware adapter still requires complete
source integrity and unchanged coherent SDK VALID state. Missing, unavailable
or contradictory proof prevents Begin and image transfer in either mode.
After reboot, verification requires accepted normal runtime. A runtime accepted flag alone cannot prove selector coherence.

After final receive acknowledgement, the app obtains Receiving proof and
durably stores one versioned unresolved target before invoking End. The target
contains exact board/artifact version, image and file hashes, preboot nonce,
admitted attempt, source/target slots and selector sectors, source and expected
target sequences. Native connection tokens remain transient fences and are
not restored as reboot authority. Atomic temporary-file write, flush and
rename failure prevents End. Corrupt/unknown stored intent fails closed and
blocks another transfer rather than being discarded or replayed. Once End may
have been submitted, uncertain send/timeout/disconnect/retirement retains the
target and returns pending verification without automatic Abort or End replay.
A terminal error after End can follow selector publication and also remains
pending; only a positive correlated failure may resolve it; unknown evidence stays pending.
Recreated services restore the same owner before a new upload or screen setup.
A sole valid flushed temporary intent is restored read-only as unresolved; it
is never promoted to End replay. A valid primary can still be verified with a
retained unknown temporary file. Exact resolution clears only validated matching
records; corrupt, unknown or conflicting files remain evidence and block a new
transfer. On the original boot, exact matching Aborted/Failed admission plus
the preserved running/selected VALID source can resolve an interrupted
pre-activation attempt, including process death between intent publication and
End invocation. Missing correlation stays pending.
An accepted preserved source plus the exact admitted target's INVALID/ABORTED
selector resolves a failed attempt even when the image bytes are identical.

Normal acceptance follows the first healthy normal-runtime sample after the
full startup presentation has a confirmed physical completion and configured
BLE transport is working. Completion targets 5000 ms after that presentation,
including the final SDK proof and publication, without visible animation slowdown
or an added hold. The startup owner includes the selected
boot display and piece introduction, including skip/cancellation, and retains
the first confirmed completion timestamp. Recovery never publishes acceptance.

The existing Core-0 OTA worker starts normal full running-image verification only
after essential saved mode is resolved and completed nonzero LED output is
recorded. Confirmed startup completion is the valid dark/Off fallback and also
requires resolved mode. This is software output evidence, not proof of optical
Fox visibility. Read-only preflight and failed-boot recovery retain their existing
authority. Preparation reports no health and changes no SDK state. One
retained ticket owns the snapshot and complete image proof on that worker's
stack while it awaits the runtime decision. Essential Store root, saved-mode
and first-frame recovery is admitted promptly; an existing bound summary cache
can load, while only a missing-summary rebuild waits for normal acceptance
and inactive playback. Artwork with missing summaries retains its
observational LIVE-read unavailable result until that idle repair or an explicit
edit prepares it; LIVE reads never trigger repair or pause playback. Optional
reconstruction checks queued settings/lifecycle, ingress, mode, playback and edit
demand at each frame boundary. It returns Incomplete before publishing any
partial cache, lets the existing worker serve foreground work and retries only
when idle. Sleep/reboot readiness disables maintenance until the transition
finishes or cancels, including after its save completes.
Animation completion must not be delayed to conceal verification latency.
Short physical flash
leases preserve animation access; software hashing stays outside the leases.
No second hash worker or image cache is created.

Confirmation requires the complete running-image proof followed by a fresh
exact snapshot; that guard, SDK mark-valid and raw VALID readback share one
flash/RMT lease. Only the selected PENDING_VERIFY record may change its state
bytes to VALID. The exact completed healthy ticket carries the verified SHA
to the normal-runtime owner. Normal startup reserves the existing digest
provider before BLE admission, so early digest requests join its Pending
result. After the transport-readiness recheck the successful ticket completes
that owned reservation; an unrelated Pending or Ready provider retains its
own publication. Recovery retires the reservation after the native job, and
cannot reuse a normal proof as fallback integrity or normal acceptance.
Failed, stale and pending work never publishes identity or acceptance.
An already-VALID image needs full integrity and fresh coherent VALID proof but
no SDK mutation. Same-image fallback still requires separate attempt correlation
in the host/app verifier; content identity alone is not update success.

Failed health may request SDK invalidation only for a matching selected/running
PENDING_VERIFY image with a distinct supported fallback whose raw record is
VALID and whose complete image integrity has been verified. Recheck the exact
snapshot immediately before the SDK invalidation under one lease. The public
SDK invalidation API restarts internally, so a returning success is named
RollbackRequested, not completed/read-back rollback; the next boot must prove
the outcome. Unknown or changing evidence, failed hashing, missing fallback,
SDK errors and failed confirmation readback retain recovery and never cause a
bare restart, automatic retry reset or false acceptance.

Metadata-only startup preflight retains priority 2 before rail enable.
Confirmation and upload-source integrity run on the same Core-0 OTA worker
at priority 3, ahead of optional priority-2 analysis/storage, returning to
priority 2 before publishing completion. Its bounded
reader yields after each 4 KiB and while a flash lease is unavailable; physical
flash/RMT exclusion and configured transport service retain their owners. Runtime health submits one retained ticket and polls it without waiting,
so main-loop input, rendering, BLE and recovery continue during verification.
Pending is neither acceptance nor a retry. The exact ticket consumes its result
once; competing state/source reads return busy even after completion is posted.
A recovery transition retains the native job until completion and cannot reuse
its prior normal-health result as acceptance. Configured transport readiness is
checked again before publishing normal acceptance. There is no running-job
cancel or second worker. An abandoned synchronous caller retains its existing
separate completion-reaping recovery; it cannot consume a live health ticket.

Software hashing runs outside short flash leases, with the existing 18-second
absolute image-read deadline. Initial/final lease waits are each bounded at
five seconds; synchronous source requests retain their 30-second response
budget. There is no extra metadata lookup lease or filesystem authorization I/O.
The existing progress watchdog supervises the operation.

Crash-loop and fatal startup recovery initialize and pump the same BLE/session/
OTA updater without loading settings or starting game/engine services. Recovery
uses fixed name `Litch Recovery`, independent of unloaded or later-loaded BLE
preferences. Device info is `{"i":[3,hardware,24,1]}`: the optional fourth scalar
1 means recovery-only; normal info retains its three scalars. Mask 24 advertises
only FirmwareDigest and FirmwareUpdate when the OTA adapter is ready. An
unavailable adapter exposes only FirmwareDigest (mask 8) and rejects OTA Begin
with `unavailable`. Session negotiation, repair and OTA
Objects 31/32 retain their existing owners. Settings, Console, Identify, Home,
game history and artwork capabilities, ingress and publications are unavailable.
The current app discovers this mode before automatic domain reads and retains
only updater/session lifetime. The existing base Command/Console/Litchcraft
attribute skeleton remains in its normal registration order so bonded-phone
cached session/OTA handles stay stable across normal/recovery startup. These
skeletal attributes grant no domain capability/access: input and subscription
callbacks reject recovery use and publish no domain bytes. Recovery retains
public read-only support `0x45` and safe State 2 beside the existing updater;
private support `0x44` remains closed at ingress and final output. The public
capture binds in the early updater before the boot-guard recovery branch and
reports bounded boot/recovery/update/storage-load facts and available image
identity without normal settings or native owner initialization. Missing fields
remain unavailable; disabled-before-load settings are not called corrupt.
The app opens recovery in Setup with Board health: Recovery first, showing the
cause and safe next action. Play, Craft and ordinary board settings are hidden.
Contact Support opens an editable partial report independent of owner readiness;
Board software opens the existing updater and returns to Setup. A pending update
retains its verification screen; Back to Setup activates the same public health
read when it was deferred at cold entry.
No automatic reconnect, owner clear, factory reset or SDK/cache bypass is added.
Exit uses the existing restart; there is no
in-place promotion to normal mode. Normal healthy LED fade timing is unchanged.
A permanently rejected LED startup can be recovered through the worker or Console.

The additive read-only BLE OTA-control command `{"cmd":"ota_state"}` (19
bytes) exposes acceptance for BLE clients. Its response is `{"ov":"p"}` until
the normal runtime health owner has crossed the existing acceptance boundary
and the OTA worker has read back the running image as `ESP_OTA_IMG_VALID`; only
then does it return `{"ov":"v"}`. The accepted flag is monotonic for that boot
and survives BLE reconnects. The query does not confirm an image, recheck
health, touch flash, or alter rollback authority. Unknown or unavailable state
never publishes acceptance.

CLI USB flashing and factory provisioning complete when standard esptool
completes their write/reset; they do not perform a serial runtime-provenance
probe or acquire BLE locks. Exact USB target binding remains required. The
BLE OTA path retains its explicit target and the full protocol described here.

### Flutter sender contract

- On Android, discover the GATT table once for each live connection, retain the
  exact service/characteristic instances for that connection only, and renew
  them on reconnect. All connected OTA, console, settings, capability, and
  keepalive operations share that connection-scoped table; they must not
  independently repeat UUID-only characteristic resolution. The established
  iOS characteristic path remains unchanged.
- Before shared-session activation, use one OTA-control notification
  subscription per live connection generation; upload, version, and digest
  consumers share its typed event stream rather than opening competing GATT
  subscriptions. Activation retires the direct subscription while preserving
  the same typed event owner for Object status. Renew the direct subscription
  only for a new pre-session connection or notification-stream termination.
- Capability discovery shares one in-flight operation, including refresh callers.
  Native notification activation may finish after the first successful `info`
  write, so discovery retries missing or malformed replies on that same channel:
  at most three read-only exchanges, each with a 1.2-second reply window and
  180 ms between attempts. Only capability contract 3 grants feature support.
  Refusal, silence, malformed data or unsupported versions grant no update
  permission; incomplete current discovery retires the native connection so
  the existing reconnect/cache-refresh owner can recover. Current app admission requires an activated shared
  session and coherent correlated source proof before Begin; finalization also
  requires correlated Receiving admission proof. Litchcraft's
  six-second preparation deadline still bounds its UI when native work stalls,
  without releasing that work's in-flight ownership.
- Do not send firmware bytes until a begin acknowledgement is received.
- Clamp chunks to the negotiated Object body and bound outstanding data by `sent - rx`. At minimum MTU, compact transaction status uses a conservative receive window; this is a current packet-size variant, not a different peer protocol.
- Do not send `ota_end` until the board has acknowledged the final byte. Include the original size and CRC-32 in `ota_end`.
- Once `ota_begin` has been attempted, a failure before End may be submitted
  makes a best-effort `ota_abort`. After final receive acknowledgement, obtain
  coherent correlated Receiving proof and await durable target publication
  before submitting End exactly once. Persistence failure prevents End. Once
  End may have been submitted, ambiguous send, timeout, disconnect, background
  retirement or cleanup failure retains pending verification without Abort or
  End replay. Even a terminal error after End requires correlated SDK proof
  before clearing intent.
- An error, timeout, or pre-finalization disconnect ends the attempt promptly and safely; never infer success from bytes merely having been handed to the phone's BLE stack. A disconnect after complete receive acknowledgement and a sent `ota_end` is reported as pending post-reboot verification, not as proof of either success or failure.
- Before Begin acknowledgement, an explicit board error is reported as a
  refused update with its reason retained in diagnostic detail; it must not be
  presented as a missing acknowledgement. A disconnected transport is reported
  as a lost connection. The missing-acknowledgement message is reserved for
  actual silence through the existing Begin deadline. None of these failures
  permits image data to be sent, and each retains the same best-effort abort.
- After finalization, reconnect only the original device through the ordinary
  bounded connection owner. The app allows up to twenty seconds for the End
  result, covering the save window and flash acquisition. Acknowledged End
  starts verification on the original board's replacement connection. The
  pending owner retains that handoff through screen recreation and retry.
  Uncertain End may query the existing connection to prove a same-boot abort.
  Success requires exact selected version/digest,
  normal-runtime acceptance, a changed Hello boot nonce and the durable admitted
  target's running/selected slot, selector and sequence VALID as specified above.
  Same-image fallback resolves as a failed attempt when its exact failed target
  and preserved source are proved. Version/digest/acceptance alone never verify
  an update. Identity reads and proof share the same current native/session
  generation and Hello nonce, rechecked after awaits. Unknown evidence remains
  pending; another live board is never replaced automatically.
- Post-reboot verification obtains the exact accepted attempt before asking for
  its digest, so the common path reuses the health owner's full integrity proof
  instead of starting a second image scan. Source integrity before Begin still
  performs a fresh full scan. A retired shared session closes its typed OTA
  status owner promptly; a subsequent upload renews that session through the
  existing connection owner before capturing the new attempt's generation.
- One service-owned durable target survives notification retirement, screen
  recreation and process death. The recreated connected shell restores the
  Board update task before ordinary metadata setup. Native tokens are transient
  callback fences, excluded from durable storage; they are not reboot evidence.
  Missing/corrupt stored authority blocks a new transfer. Clear the exact intent
  only after a positively resolved outcome and successful durable removal.
  Retain expected/observed attempt identities in failure diagnostics and restore
  the observed running version through the existing result surface.
- Missing post-reboot version or digest responses receive bounded automatic
  retries on the same fresh connection. An accepted command write alone is
  not a response. Retries must stop if that connection is replaced, and must
  never turn missing information into a successful verification.
- After the verification attempt, reload the selected channel's release notes
  without overwriting its verification result. This also applies when the
  connected screen is recreated with a pending update.
- An idle board already running the selected release offers an explicit
  **Reinstall** action using the same verified release snapshot and OTA flow.
  This does not advertise a new update. An active transfer or unresolved
  post-reboot verification prevents starting another transfer.

## OTA V2 Flow

OTA v2 keeps the same data characteristic but adds an explicit negotiation/probe step so host tooling can use measured transport parameters instead of blind guesses.

### Host -> board

1. Probe OTA profile before transfer:

```json
{"cmd":"ota_probe","mode":"flash","size":842256}
```

Use `mode:"sim"` for simulated upload-path benchmarking.

2. Start transfer with optional requested tuning values:

```json
{"cmd":"ota_begin","size":842256,"chunk":244,"burst":8,"window":32768}
```

For simulated upload benchmarking:

```json
{"cmd":"ota_sim_begin","size":524288,"chunk":512,"burst":8,"window":32768}
```

3. Send raw OTA data bytes in windows selected by the host.

4. Finalize:

```json
{"cmd":"ota_end","size":842256,"crc32":"89abcdef"}
```

### Board -> host

1. Probe response with protocol and transport recommendations:

```json
{
  "st":"probe",
  "proto":2,
  "mode":"flash",
  "size":842256,
  "max_chunk":512,
  "chunk":512,
  "burst":48,
  "window":15616,
  "status_ms":100,
  "supports_sim":1,
  "supports_notify_test":1
}
```

2. Begin accepted with negotiated values echoed back:

```json
{
  "st":"begin",
  "proto":2,
  "sim":0,
  "rx":0,
  "total":842256,
  "chunk":512,
  "burst":48,
  "window":15616
}
```

3. Progress notifications during receive/flash:

```json
{"st":"progress","pct":52,"rx":438272,"total":842256}
```

4. Terminal state:

```json
{"st":"done","pct":100,"rx":842256,"total":842256}
```

### Current negotiated transfer profile

- Share the existing verified flash transaction owner between typed app Objects and the current developer maintenance interface.
- Expose enough transport information for host tools to test chunk, burst, and window sizing with real data.
- Make host auto-tuning deterministic: probe first, transfer second.
- Require current capability contract 3; failed discovery never selects another app update transport.
- Current firmware requests ATT MTU 517 and allows OTA/link-test payload sizing up to 512 bytes when the negotiated transport supports it.
- Requested burst count is clamped to 1–48 chunks. Requested window is clamped
  to at least 1024 bytes and at most the smaller of `negotiated chunk × 48` or
  the queue's 15616-byte fragmentation-safe payload budget, so echoed tuning
  values never promise more buffering than firmware owns.

### Production-app transfer diagnostics

The app logs the returned MTU when available and emits one `OTA upload metrics`
summary after upload cleanup. It records the effective begin profile, sent and
received bytes, terminal disposition, and monotonic phase durations. Reliable
and no-response chunk spans include retries inside the sender; their counts are
logical chunk calls, not individual native attempts. Credit-wait counters include
only calls that actually block for receive-window capacity.

`begin_control_ms` measures the Begin send/admission span separately from
`begin_status_ms`, the remaining wait for Begin status; together they form
`begin_ms`. The shared Object adapter also emits one aggregate of native batch,
grant-wait and post-native admission spans. These admission spans may overlap
after pipelining and are not additional elapsed time. The historical reliable
chunk label means a batch flush on the shared path, not an ATT response write.
Before waiting for receive-window capacity, the shared adapter flushes any
already-buffered partial batch through the same submitted-Object owner. Those
bytes remain inside the unchanged receive window and must receive cumulative
acknowledgement before End. This avoids idling with window-counted bytes still
waiting in the app's batch buffer; it adds no queue or retry timer.

A real-flash `done` status may include optional unsigned 32-bit `wb` (bytes
flushed) and `wm` (cumulative write-path milliseconds), using counters already
owned by the receiving firmware. Missing or malformed values stay unavailable.
The app reports these only as `fw_write_bytes` and `fw_write_ms` diagnostics;
they never authorize success, End or boot acceptance. Timing includes SDK,
hashing and flash/RMT arbitration within each staged write, so it is not raw
SPI flash bandwidth. Minimum-MTU compact status can omit these fields;
shared-session status retains the existing bounded fragmentation.

`stream_ms` includes file reads, CRC, progress callbacks, credit waits and
`final_rx_ms`; these fields overlap and must not be summed. Download, MTU
calibration and post-reboot version/digest verification are outside uploader
timing. Correlate with the app's phase logs and firmware flash/path metrics.
The receiving firmware owns transfer pacing, so uploading a changed receiver
image does not measure its new behavior until a subsequent transfer.

### OTA-time board progress

- Real OTA owns a central 4×4 circulating amber pulse and purple perimeter progress bar,
  independent of game/menu rendering and the Minimal animations setting.
  The bar fills all 32 fixed physical perimeter positions in order, including
  the two beside USB. V3.3's inserted button LEDs do not participate.
- After validating an `ota_begin` or `ota_sim_begin` request, firmware revokes
  above-default CC authorization before aborting a prior
  transfer or calling `Update.begin`. Real OTA then renders progress using the
  conservative LED power budget; simulation does not render progress.
- Accepted begin yields before draining prequeued data so initial progress can
  latch before a data write starts erasing flash. Each ingress pass processes at
  most 64 items and yields after the current operation once 20 ms has elapsed;
  one synchronous SDK operation can exceed that soft budget.
- Real transfer progress targets a 200 ms refresh cadence. At 100% received,
  finalization uses a 20 ms render cadence so the existing settings worker's
  render handoffs can complete its durable snapshot inside the OTA persistence
  gate. LED transmission and flash
  operations retain their shared exclusion: an in-flight flash operation leaves
  the previous LED frame latched until rendering can resume safely.
- A temporary minimum brightness scale of 32/256 keeps update progress visible
  without changing the saved brightness. The conservative power limiter still
  applies to the expanded physical frame.
- The perimeter fills only when all bytes have arrived. The board has no numeric percentage; its central pulse continues through
  firmware finalization and pre-reboot grace. Normal startup replaces the OTA cue
  after reboot. The app displays numeric sent-byte progress capped
  at 99 until verification, then reports completion after verifying the running image. Abort restores normal rendering.

## Related Commands

- `notify_test` exercises structured board-to-host notify throughput.
- `fw_version` remains the simple version probe.
- `ota_abort` is handled by the common transaction owner on both current entry points.

### Notification throughput diagnostic

The opt-in mobile RAM reference uses the existing shared Object session in
either direction. Forward uses `ota_sim_begin`; reverse uses
`object_bench_begin` and reverse Object code `0x33`. Both are bounded to
300,000 bytes and verify the deterministic byte pattern and final CRC32 without
writing firmware or artwork. Session owns ordering, credits, repair and
connection retirement. Reverse records have a tag and little-endian run `u16`:
tag `0x11` carries data after that three-byte header; tag `0x12` is exactly ten
bytes, with total `u24` at offset 3 and CRC32 `u32` at offset 6. The reverse
reported rate includes Begin through receipt of the final data record; its
terminal result requires the matching End proof. It measures received bytes,
not native queue admission, and does not establish the physical radio ceiling.
The reverse reference bounds the whole receive phase to two minutes and retains
the partial received-byte count on failure; a timeout is not a measured ceiling.

`notify_test` takes unsigned 16-bit `size` and `count` values (both nonzero),
plus optional unsigned 16-bit `delay_ms` (default 0). The payload remains
clamped to the negotiated ATT payload and the existing firmware packet limit.
The normal host request of 200 packets with zero delay remains supported.
Each packet submits its exact payload to the original native connection handle.
The returned byte and packet counters describe successful SDK admission, excluding
refused submissions; they do not establish host reception. A host throughput
measurement must separately count and validate the received payloads.

An optional nonzero unsigned 32-bit `run` selects tagged diagnostic packets.
Tagged requests require `size >= 16`; omitted `run` retains the existing all-01
payload. Every tagged packet begins `01 1e 1f 01`, followed by the run's eight
little-endian nibbles and the zero-based packet ordinal's four little-endian
nibbles. Each nibble is encoded as `10 | nibble`; remaining bytes are `01`.
These bytes remain below `20` and do not produce ordinary Console text.
For tagged runs, `count` is the number of successful native admissions. A
refused submission retains its ordinal through the existing scheduler pause
and retries within the same batch deadline. SDK `fail` counts these refused
attempts and may exceed `count`; it is not a count of lost accepted packets.
Untagged runs retain their existing fixed-attempt behavior.
The existing `bench_notify` detail echoes `run` and requested `count` alongside
effective size and SDK admission totals. It is control evidence, not receipt.
The phone benchmark validates exact payload, unique sequence coverage, byte
count and current connection generation through the existing Console subscription.
It measures both command-to-last-receipt and first-to-last-receipt time and
rejects missing, duplicate or corrupt packets. A partial result is not a ceiling.
Its whole observation deadline is two minutes; the firmware's bounded blocking
batch retains the admission and ten-second limits below. Raw results still
include this diagnostic's at-least-one-tick yield and must not be presented as
an established radio ceiling until generator/host pacing has been excluded.

A request must satisfy `count * max(delay_ms, 1) <= 10000`. Firmware checks this
without multiplication overflow and rejects invalid or oversized requests with
`invalid_test_args` (compact `test_args`) before entering the diagnostic or
invoking its power-safety callback. Zero delay still yields to the scheduler.

Each accepted batch also has a 10-second elapsed-time budget, including its
pre-diagnostic safety callback. The loop checks its original connection
generation and deadline before sending and during deliberate pauses, split
into at most 10 ms requested slices. A disconnect, replacement connection or
expired budget ends the batch; a fresh explicit request may run after recovery.
An SDK operation already in progress must return before cancellation takes
effect. The watchdog continues to supervise a stuck SDK call; the diagnostic
feeds it only after work or a deliberate sleep returns.

An interrupted batch is unsuccessful even if some notifications were sent.
Partial counts and elapsed time remain available to the caller. A still-owned
connection receives `notify_test_failed` (compact `notify_fail`); a disconnected
or replacement session receives no terminal response from the old request.
Completed batches retain the existing `bench_notify` response.

The developer command `ble test [duration_s] [delay_ms]` uses the same admission
and per-batch limits. It stops the enclosing benchmark after a failed batch,
including partial cancellation, instead of continuing into another packet-size
bucket. Its explicit duration still controls how long successful batches repeat.

## Device capability registry and runtime-health commands

The compact device-info response contains the contract version, hardware
revision ID and 16-bit capability mask. Current firmware reports contract
version 3 and provides the shared-session endpoints. The app accepts version 3
only. Unsupported versions, malformed replies and missing discovery leave
board capabilities unavailable.
Clients ignore unknown bits within the supported 16-bit mask. Use the
advertised mask to decide which features to offer. The [firmware encoder](../../include/modules/ble_device_capabilities.hpp)
keeps the response within the 20-byte minimum ATT payload.

Current stable assignments are:

| Bit | Mask | Capability | Advertisement state |
| ---: | ---: | --- | --- |
| 0 | `0x001` | Console | Current |
| 1 | `0x002` | Settings | Current |
| 2 | `0x004` | Identify | Current |
| 3 | `0x008` | Firmware digest | Current |
| 4 | `0x010` | App firmware update | Hardware-dependent |
| 5 | `0x020` | Runtime health | Current |
| 6 | `0x040` | Runtime-health recheck | S3 callback-dependent |
| 7 | `0x080` | Game History | Advertised when the game history receive, read and acknowledgement callbacks are installed |
| 8 | `0x100` | Coach Insight | Reserved. Current firmware leaves this bit clear |
| 9 | `0x200` | Owner Pairing | Advertised when board pairing is enabled. Recovery-only and factory-only modes leave it clear |
| 10 | `0x400` | Litchcraft | Explicit light-painting session; [controls](../controls_and_menu.md#litchcraft-face-controls) |
| 11 | `0x800` | Litchcraft bulk artwork | High-MTU full-canvas replacement; requires bit 10 and retains the minimum-MTU fallback |
| 12 | `0x1000` | Litchcraft efficient inspection | Requires bit 10; one-State inspection and negotiated snapshot chunks |
| 13 | `0x2000` | Litchcraft favorite palette | Requires bit 10; atomic eight-color synchronization |
| 14 | `0x4000` | Litchcraft animated artwork | Requires bits 10 and 12 plus a started schema-4 Store worker with ingress and reverse response owners |
| 15 | `0x8000` | Litchcraft shake erase | V3 motion-hardware dependent; requires bit 10 and clears only the selected frame as one Undo operation |

Bits 14 and 15 fit in the 16-bit mask. A client should check each bit before
using animation or shake erase. Hardware revision, bulk transfer and a successful
static artwork snapshot cannot establish support for either feature.

Capability bit 15 enables the existing `4c` Litchcraft notification event
`89 Gesture`. Its payload is `action:u8 token:u16`; action `1` means that a
sustained V3 shake erased the selected frame. The request identifier is zero,
the token is nonzero and changes per accepted gesture, and the existing header
revision is the resulting canvas/document authority. Older apps ignore this
additive event and recover the changed authority through their normal snapshot
path.
Litchcraft request op `14` lets an active foreground app ask firmware to
run that same selected-frame erase transaction after one second of continuous
host-phone shake. Firmware remains the mutation/history authority and emits the
same Gesture event; an empty displayed frame is an acknowledged no-op.

### Litchcraft animated-artwork protocol v2

Discover bit 14 before using animated artwork messages.

Reuse Litchcraft characteristic `...0106` and its one typed connection-scoped
subscription. Keep every existing `4c` command/event and UUID meaning unchanged.
After explicit bit-14 discovery, protocol v2 uses a distinct envelope:

- Request: `4d 02 op session:u32 request:u16` (9 bytes).
- Notification: `4d 02 event session:u32 revision:u32 request:u16` (13 bytes).
- All multi-byte integers are little-endian; request IDs are nonzero, shared
  with the connection's existing Litchcraft request allocator. Physical events
  use request zero. Unknown versions, sizes, enums, and reserved bits fail closed.
- Session admission/departure retains existing Enter/Exit and their durable
  ownership rules. Only v2 Inspect and Snapshot permit session zero and neither
  admits a session. Cancel may use session zero only to retire the exact outgoing
  Snapshot that this connection admitted with session zero; it does not admit a
  session or authorize another mutation. All edits require the current active
  session. No console
  text, settings key, or OTA begin command is a document mutation channel.

The response header's revision is the complete document revision. An accepted
RGB/effect/frame/favorite/preference mutation advances it once per accepted
command, while an identical retry, no-op, playback tick, pause, or frame seek
does not. Stroke boundaries group Undo without manufacturing extra revisions.
The existing static compatibility revision may still reflect Tool changes;
clients must never substitute that counter for v2 document authority. On schema
upgrade/downgrade, retire the old schema's cached canvas and inspect the current
document before accepting further deltas. Session continues to identify the
same physical Litchcraft owner across schema changes.

| Op | Payload after the 9-byte request header |
| --- | --- |
| `01` Inspect | Empty. Return State and one frozen Timeline sample; never transfer document bytes. |
| `02` Timeline | `action:u8` (`0 pause`, `1 play`, `2 select-frame`), `frame:u8`. Frame must exist. Pause atomically samples and freezes the board's current frame; its frame byte is advisory so crossing a playback boundary cannot reject Pause. Play requires the current selected index, and select-frame always pauses before seeking. Runtime only. |
| `03` Paint | `frame:u8 stroke:u16 flags:u8 mask:u8 R G B`, then 0–3 square indices. Flags retain begin/end bits; every square/mask/frame is validated before this packet changes anything. |
| `04` Effect | `frame:u8 effect:u8 durationMs:u16 mask:u8 stroke:u16 flags:u8`, then 0–3 square indices. Effects Off require duration zero; other ranges come from the document contract. |
| `05` Frame edit | `baseRevision:u32 action:u8 source:u8 destination:u8 durationMs:u16`; actions `0 add-copy`, `1 delete`, `2 move`, `3 set-duration`. Unused indices/duration must be zero. Frame limits and resulting selection are checked before mutation. |
| `06` Undo / `07` Redo | Empty; operate on the shared document history and return a new revision or No history. |
| `08` Snapshot | Empty; State then one frozen canonical document, including its length/CRC and matching End. |
| `09` Replace begin | `baseRevision:u32 length:u16 crc32:u32` (10 bytes); reserve one staging body. The request ID becomes the transfer ID; malformed length cannot allocate memory. |
| `0a` Replace chunk | `transfer:u16 offset:u16`, then 1–7 canonical bytes. Contiguous offsets only; duplicate identical requests use receipt replay. |
| `0b` Replace commit | `transfer:u16`; validate complete coverage, CRC, schema, all bounds, and unchanged base revision, then swap once and create one Undo operation. |
| `0c` Cancel transfer | `transfer:u16`; cancel only the caller's current connection/session transfer; repeat is harmless. |
| `0d` Canvas edit | Empty is a read-only capability probe for the current active session. Edit payload is `baseRevision:u32 frame:u8 mask:u8 R G B flags:u8` (10 bytes). Flags `0` fills every square on the masked emitters, preserving effects; flags `1` erases all 256 colors and effects on that frame and requires mask `15`, RGB zero. |

Canvas edit discovers support inside v2: the global 16-bit bitmap is full and
older clients reject a wider mask. After Inspect/admission, probe once per
connection/session. Success enables the 19-byte command; Unsupported retains
complete-document Replace. Older clients send neither probe nor edit and receive
unchanged State envelopes. Probe has no edit/history effect. Edits validate
session, base revision, selected frame, payload, active stroke, and staging
ownership before pausing or mutating. A frame changed by playback or physical
selection rejects with Revision conflict; it never redirects the edit. Accepted
edits pause playback and commit once through the existing document/history/
persistence/render owner. Exact receipt replay does not add history. One State
ACK confirms the result without a snapshot download on success. The app displays
the intent before the write without a loading indicator. Rejection retires only
that preview before coherent recovery; uncertain ACK/reconnect uses the retained
common document and normal snapshot reconciliation.

Add-copy requires the selected existing source, inserts its complete frame
immediately after it, selects the new copy, and requires destination and duration
zero. Delete and set-duration use source and require destination zero; Move
removes source then inserts at destination in the remaining list, with duration
zero. Delete also uses duration zero. Reject operations that would produce zero
or more than eight frames. RGB/effect stroke packets latch a frame at Begin; a
frame mismatch before End is rejected. Frame selection and every accepted edit
pause the authoritative playback clock before applying, including physical edits
received during a staged replacement. The existing bounded physical release and
app cancellation rules close interrupted strokes and preserve their accepted
work. There is no global edit lock against ordinary physical painting while a
replacement is staged.

| Event | Payload after the 13-byte notification header |
| --- | --- |
| `81` State / ACK | `status:u8 flags:u8 frameCount:u8 selectedFrame:u8 schema:u8`. Flags: bit0 active, bit1 playing, bit2 Undo available, bit3 Redo available; bits4–7 zero. |
| `84` Snapshot begin | `schema:u8 length:u16 crc32:u32` (7 bytes), frozen with the header's session/revision/request. |
| `85` Snapshot chunk | `offset:u16`, then 1–5 canonical bytes at minimum MTU; negotiated ATT payload may carry more, never past declared length. |
| `86` Snapshot end | Empty; must match Begin's session/revision/request, with complete coverage and matching CRC. |
| `87` Document changed / resync | Empty; announces a later document revision. It is a watermark, not proof that cached bytes match it. |
| `88` Timeline low | `sample:u16 flags:u8 elapsedLow:u32`; only flag bit0 playing is defined. |
| `89` Timeline high | `sample:u16 selectedFrame:u8 elapsedHigh:u32`; pair with Low for the same sample/session/revision. |

State describes the current advanced document. When no advanced document exists,
its only valid document tuple is `schema=0`, `frameCount=0`, and
`selectedFrame=0`; this truthful tuple is also used by replacement admission and
refusal ACKs before an advanced document exists. A State that names schema 3 must
have one through eight frames and a selected frame in range. Snapshot Begin and
every adopted document remain strictly schema 3 with one through eight frames;
the no-document tuple never represents a transferable empty artwork. Its Undo
and Redo flags are both clear. History flags describe authoritative board
history at the exact acknowledged revision; an exact request replay therefore
replays the original flags even if later accepted work changed current history.

Statuses retain static numeric meanings 0–8. V2 adds `9 revision conflict`,
`10 resource limit`, and `11 integrity failure`; unsupported version/schema or
effect uses existing Unsupported (`4`). A refusal never partly applies a packet
or claims a new document. Accepted requests replay the same ACK, not the action,
using the existing 32-receipt bound. Playback replies freeze both halves under
one nonzero wrapping sample ID. The app retains only one pending pair, drops
stale/mismatched halves, and asks Inspect again after a missing half; it never
combines two samples. The app extrapolates elapsed time only from a complete
sample while playing, using its monotonic clock. Transport latency is phase
uncertainty, not permission to alter the board clock; exact deterministic tests
supply the same elapsed value to both renderers.

ACK confirms semantic acceptance, not flash durability. Snapshot ACK must arrive
before Begin; bytes remain invisible until complete End validation. A frozen
snapshot may be followed by a later Document changed watermark; the app retains
its latest local edits, records the newer revision, and performs one bounded
resync before declaring live editing coherent. A local RGB/effect edit may be
optimistically shown, but only its ACK advances confirmed authority. V2's first
implementation uses a whole-document snapshot for an unseen physical document
change; it does not invent a second partial-state reducer. Coalesce watermarks
while a snapshot is active, cap recovery to the existing three attempts, and
expose Retry if a continuously changing document prevents confirmation.

All control packets fit ATT payload 20. Optional throughput improvement changes
only Replace chunk and Snapshot chunk byte counts after MTU negotiation; their
headers/offset units remain bytes. Do not assume bit 11 or reuse its `4c 40` RGB
format for schema 3. At payload 20 a largest document requires at most 1,763
seven-byte writes plus Begin/Commit, and 2,468 five-byte notifications plus
State/Begin/End. At payload 244 these become at most 54 writes and 54
notifications. Keep one acknowledged control/write request outstanding and no
more than the existing 96 bounded notification descriptors. Generate snapshot
chunks from the frozen body, rather than queueing document copies.

Request ACK windows remain 250 ms, 750 ms, then six seconds with identical bytes
and ID. An entire document transfer has a 180-second absolute deadline and a
six-second inactivity deadline, measured from actual accepted chunk progress;
duplicates and malformed packets do not extend it. Admission remains a small
round trip and usable local painting remains visible while a large transfer
progresses. Report confirmed bytes/total, not optimistic writes. Timeout, CRC
failure, invalid schema, base-revision conflict, disconnect, or session end
retires staging and preserves the complete live artwork. Do not switch packet
formats or send a flattened fallback after any advanced bytes were accepted.

Transfer IDs, staged bytes, receipt replay, subscription state, and pending
Timeline halves belong to one physical connection epoch and session. Reconnect
starts a fresh subscription and request/transfer space, never resumes old
staging, and never lets a prior cleanup disable its replacement. First Inspect
establishes schema/session/revision. Equality with the last coherent document
allows a metadata-only resume; otherwise fetch one coherent document and apply
the existing board-specific common-ancestor reconciliation to the **whole**
document, including frames/effects/favorites. Genuine divergent edits require
the normal board/app choice. Automatic reconnect never silently flattens,
admits an inactive board, or exits a newer session on behalf of an old departure.
Explicitly selected offline entry and restart recovery retain their existing
admission rules. Playback continues on the board while the app is disconnected.

Before bit 14 ships in a published release, `.77` must demonstrate minimum/high-MTU byte
fixtures, exactly-once retry, stale epoch/session/base revision rejection,
interrupted snapshot/replacement recovery, bounded resource usage, and old/new
app/firmware compatibility. In particular, an old app must neither corrupt nor
receive a misleading flattened copy of an active advanced artwork. Static
operations remain unchanged for static documents, and advanced capability is
not a grant to run a firmware update or mutate an unrequested physical target.

Bit 5 (`0x20`)
advertises the read-only runtime-health snapshot. Bit 6 (`0x40`) independently
advertises the explicit S3 power-health recheck. Existing V1/C3 and older S3
masks keep bit 6 clear; an app must not infer it from the hardware revision,
OTA support, or bit 5. A current S3 compact mask with bits 0 through 6 set is
decimal `127`, and its device-info envelope still fits the 20-byte minimum ATT
payload. Firmware that does not advertise Game History or Coach Insight keeps
the v1 envelope unchanged.

After the OTA-control notification channel is ready, bit 5 permits exactly:

```json
{"cmd":"health"}
```

The fixed 17-byte response is `{"h":"RRRRSCCCC"}`, where `RRRR` is the
uppercase hexadecimal health revision, `S` is the state digit, and `CCCC` is
the uppercase hexadecimal cause mask.

Bit 6 permits the exact 20-byte request:

```json
{"cmd":"health_chk"}
```

The only successful and failed acknowledgements are `{"c":"ok"}` and
`{"c":"fail"}`. They acknowledge the firmware power-recheck action; they are
not health snapshots. The app must fetch `health` afterward. Requests and
notifications are scoped to the active connection generation, and the board
must not invoke queued stale work after replacement or disconnect. Extra keys,
malformed frames, timeouts, unknown commands, and disconnects never imply
success. Exact `c` frames are classified independently of OTA state and digest
fragments. The [runtime health protocol](../../include/modules/ble_runtime_health_protocol.hpp) defines the health fields.

## Firmware digest and provenance

`fw_digest` is an additive command on OTA control; it uses the current Control/Object adapter without changing UUIDs:

```json
{"cmd":"fw_digest"}
```

The board responds on OTA control with `{"h":"p"}` while pending, eight ordered four-byte SHA-256 fragments (`{"h0":"01234567"}` through `{"h7":"..."}`), then `{"h":"r"}`. `h:"e"` means calculation failed and `h:"u"` means unavailable. Unsupported digest evidence maps to Unverified, never Custom.

The app digest reader requires all eight valid fragments followed by ready within
its original eight-second deadline. A ready marker observed before that reader
has all eight fragments is nonterminal: overlapping reads on the same status
channel can receive an earlier read's delayed ready before their own fragments.
It neither completes Unverified prematurely nor restarts the deadline, issues
another request or adopts a partial SHA. Error/unavailable/unknown-command,
send failure, stream end/error and cancellation retain their terminal behavior.
This tolerance is limited to the current channel and immutable running image;
connection replacement disposes the old channel, and the BLE service's final
board/connection-generation/channel-identity fence rejects a retired result.


On ESP32-S3 and ESP32-C3, the digest is the appended SHA-256 of the running
app image, accepted only after the complete image is recomputed and validated
by `OtaImageIdentityCollector`. The background reader shares this bounded
verifier with post-boot correlation and caches successful evidence in RAM. It
starts no earlier than one second after boot. Each bounded partition read
acquires the existing flash/RMT lease for at most five seconds and releases it
before hashing; the reader yields periodically. The complete image read has an
18-second absolute deadline. Timeout, read failure or integrity failure
publishes an error. Each caller owns separate fixed scratch state; no shared
bootloader mapping is used. The running partition is checked again before
publication. Firmware injects the arbiter when constructing BLE, including
recovery startup; an unbound platform cannot compute the digest.

The physical S3 backend uses the pinned IDF 4.4.7 mbedTLS streaming SHA engine;
native and other targets retain the portable backend. Its 116-byte value-owned
SDK context has no resource pointer and retains no engine lease between calls.
Project code adds no heap owner. The SDK lazily retains one shared recursive
crypto mutex, one TX and one RX GDMA channel, at most one group and two pair
objects until shared-GDMA teardown or reset. Channel discovery may temporarily
allocate one extra group and pair, freeing unused candidates. No DMA interrupt
callback is registered. A non-DMA-capable buffer may require at most 64 bytes of
internal DMA staging, freed before the SDK call returns. SDK channel retries
are bounded to about one second per channel; initial mutex OOM can abort in
the SDK. Hash API errors remain failed until `Reset()` and return a zero digest,
which integrity consumers must reject. Allow for these SDK allocations when measuring memory use and digest latency on an ESP32-S3.

Startup provenance joins the current connection's capability discovery and
applicable owner readiness before requesting a digest. Unknown capabilities
must not be interpreted as absence of owner pairing. After asynchronous
preparation, the current generation and applicable owner readiness must still
hold at shared OTA Object admission, including read-only provenance requests;
only bootstrap `info` may precede that owner gate.

Ordinary provenance reads on a positively identified normal updatable S3 with
an active shared session then read runtime acceptance on that same fenced
channel. Pending or unavailable readiness or acceptance stays Unverified without
starting `fw_digest`. Recovery and explicit digest diagnostics retain their current typed path;
unavailable capabilities never authorize a pre-session product digest read.

The official app looks up the resulting 64-character lowercase digest at
`https://updates.foxieproducts.com/fw/provenance_<digest>.json`. The protected
publication origin serves individual recognition records; it exposes no
enumeration endpoint and the app ships no release allowlist. A schema-1 record
contains `schema_version: 1`, the exact requested `image_sha256`, and `status`:
`official`, `custom`, or `unknown`. Only an explicit matching `official` result
means Official; only an explicit `custom` result means Custom. Missing records,
unknown status/schema, a different digest, HTTP errors, redirects, malformed or
oversized responses, timeouts and unavailable board evidence remain Unverified.
The app bounds records to 1 KiB and reuses the firmware HTTP deadlines and
no-redirect policy. Requests revalidate origin data; no app-persisted recognition
or selected release catalog substitutes for the current lookup. Connection and
channel identity are rechecked after the lookup before returning its result.

Recognition authority stays at the protected update origin. Registration reads
the selected channel's fixed server manifest, validates its canonical immutable
artifact URL, downloads the server image, and verifies both whole-file and
appended-image digests before conditionally creating and reading back the
recognition record. Registration cannot overwrite conflicting records and is
idempotent across channels. Current release registration follows successful
publication readback; a merely uploaded or locally prepared candidate is not
recognition authority.

Historical recognition remains available when the current catalog moves on.
Historical registration selects a channel and version, then obtains proof from
the protected server's versioned copies of that channel's exact manifest key.
The search is bounded to 256 versions and 32 KiB per manifest. Missing,
conflicting or incomplete server evidence leaves registration pending; it must
not be replaced by a local image, receipt, source commit, digest, URL, embedded
proof table or a prior tag. The protected workflow accepts channel/version
selection, not actual release checksums. Real release checksum records and
historical proof remain server-side; application source, tests, documentation
and workflow inputs must not embed real release digest examples or allowlists.

The runtime recognition uses the release manifest's `artifact.image_sha256`,
verified against the SHA appended inside the exact admitted binary.
`artifact.sha256` is the separate whole-file transport/admission digest and must
not be compared directly with the running-partition response. Image integrity,
host publication receipts, board update-attempt/boot acceptance and official
source recognition remain distinct evidence; none replaces another.

Outside an active update, the retained Firmware page refreshes installed version
when the board reconnects. It reads live provenance only while visible in the
foreground and reports provenance offline on disconnect. Hidden pages may prewarm
version/capabilities for update availability without reading a firmware digest. Returning to the page or resuming it in the foreground refreshes
provenance once. Explicit pending runtime acceptance permits at most ten further
reads, spaced one second after each completed read, while the page remains visible
and foregrounded. Missing/error acceptance and a complete digest with unknown
origin recognition end that automatic cycle. Leaving the page, changing the
connection or target, or starting update verification cancels queued refreshes.
An already-issued bounded read may finish, but its retired view cannot apply the
result. One in-flight read serves the current page, with at most one replacement
queued for a new view/connection. Ordinary refresh preserves pending and resolved
OTA verification outcomes.

This is friendly firmware provenance, not hardware-backed attestation: custom firmware can report another digest. It must not be used as an anti-cheat guarantee. A verified non-release digest gets empowering, non-punitive guidance and firmware update remains available.

Each complete image-reader invocation emits one `ota_image` result, including
task, partition, SDK error, reason, elapsed time, read attempts, last offset/size,
and cumulative/maximum flash-grant wait. Successful reads use Info; failures use
Warn. Reasons distinguish the absolute image deadline, individual grant timeout,
SDK read failure and integrity failure. Grant waits are sampled in milliseconds;
elapsed time also includes parsing, hashing, SDK work and scheduling. These
observations do not alter deadlines, admission or image acceptance.

## Release admission and manifests

Stable and beta releases use schema-1 manifests at `/fw/manifest.json` and `/fw/manifest_beta.json`. A release tool admits only an exact, complete ESP32-S3 image (chip ID 9) with valid segment bounds, checksum, appended ESP image SHA-256, full-file SHA-256, and size no greater than `2 MiB`. The configured build slot is `3264 KiB`, but official publication remains at the conservative admission ceiling until the app can prove the target's actual slot capacity before download or activation.

Each manifest names same-origin HTTPS, SHA-addressed immutable firmware and notes assets. The firmware object carries exact byte size, full-file SHA-256, and appended-image SHA-256; the notes object carries its own exact byte size and SHA-256. Channel, version, target, and chip ID are part of the same release description.

The app fetches one channel-tagged manifest and its verified notes as a release snapshot. The version and text shown for consent and the binary admitted for upload must come from that exact snapshot; a late stable response cannot replace a selected beta snapshot (or vice versa). Manifest, notes, and artifact requests do not follow redirects and have bounded connection, body-inactivity, and overall deadlines. Admitted binaries use channel-and-SHA cache names. A complete verified `.part` file or matching `.previous` file is recovered after interruption, while a failed new admission cannot overwrite an older valid cache.

Website publishers upload immutable assets and named aliases first, then publish the manifest last. The GitHub publisher creates or resumes a draft, uploads firmware and notes before the manifest, downloads and compares all three assets byte-for-byte, and only then makes the release public. `firmware.bin`, `firmware_beta.bin`, `version.md`, and `version_beta.md` are named ESP32-S3 asset aliases; they are not evidence of a separate supported app protocol.
