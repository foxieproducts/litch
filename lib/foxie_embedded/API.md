# foxie_embedded API

This is a concise reference for public headers in `include/foxie_embedded`.

## [button.hpp](include/foxie_embedded/button.hpp)

- `fxe::Button`: debounced button logic with repeat and long-hold support.

## [byte_order.hpp](include/foxie_embedded/byte_order.hpp)

- `fxe`: little/big-endian read/write helpers for 1/2/4/8 bytes.

## [console.hpp](include/foxie_embedded/console.hpp)

- `fxe::Console`: command registry, line editing, history, tab completion.
- Built-ins installed by default: help/logs/settings/set/debug.
- The registry holds 36 commands, including built-ins. A submitted command is
  bounded to 255 bytes and eight tokens, including the command name. Input
  containing NUL, exceeding the line bound, or exceeding the token bound is
  rejected in full before command dispatch; a valid prefix is never executed.
  `Args::overflow` exposes token overflow to direct parser users. Ctrl-U clears
  a rejected draft; submitting it also resets admission for the next line.
- `logs` replays the sixteen most recent buffered lines; live logger output is not delayed or limited.
- `Console::Config::installBuiltins` can disable built-ins.
- Interactive command history retains the five most recently submitted lines.
- `Console::SetSettingsDiagnosticAccessCallback()` optionally filters diagnostic
  settings reads, lists, exports and completion, and rejects raw individual
  deletion of denied keys. An unset callback allows all keys. Typed writes keep
  their existing validation; explicit whole-store clear/erase remains available.
  Filtered `settings dump` streams JSON through a bounded scratch buffer.
- `Console::SetSettingsStorageAdmissionCallback()` independently admits whole
  storage Load, Erase (clear/erase), Corrupt and confirmed Format before their
  filesystem or RAM mutation. An unset callback preserves existing behavior;
  refusal leaves the document/files untouched. Diagnostic format status and
  ordinary preference saving retain their existing owners. A key visibility
  filter alone does not set this storage policy.
- `Console::StartPasteMode()` streams mutable `PasteEvent::Line` callbacks and
  one final `PasteEvent::Finish` callback. Aggregate paste size is unbounded;
  lines longer than `INPUT_CAPACITY - 1`, or containing NUL, produce
  `PasteEvent::LineOverflow` without delivering a truncated `Line` callback;
  the next line continues normally.

## console_transport*.hpp

- `IConsoleTransport`: read/write abstraction.
- `StreamConsoleTransport`: adapter for Arduino `Stream`.
- `MemoryConsoleTransport`: deterministic transport for native/tests.

## [fixed_function.hpp](include/foxie_embedded/fixed_function.hpp)

- `fxe::FixedFunction<Signature, StorageBytes>`: heap-free callable wrapper.

## [fixed_string.hpp](include/foxie_embedded/fixed_string.hpp)

- `fxe::FixedString<N>`: fixed-capacity string with formatting helpers.

## [fixed_ring_buffer.hpp](include/foxie_embedded/fixed_ring_buffer.hpp)

- `fxe::FixedRingBuffer<T, N>`: heap-free fixed-capacity ring buffer.

## [ordered_receive_window.hpp](include/foxie_embedded/ordered_receive_window.hpp)

- `fxe::OrderedReceiveWindow<Capacity, MaxPayloadBytes>`: single-owner ordered
  semantic admission and terminal-outcome retention, with 1..64 fixed slots.
  No allocation, platform dependency, lock, clock, scheduling or radio credits.
- `Start(freshToken)` requires a nonzero token never reused by the caller across
  prior streams/boots; it rejects the current token. `Retire(token)` fences old
  callbacks. Starting a new stream discards local pending/retained state; the
  caller must reconcile unknown domain results rather than blindly replay them.
- `Ingress(token, sequence, bytes)` admits identities in the absolute lease
  `(confirmedThrough, grantThrough]`. Retain **every semantic field**, including
  kind/opcode and body (a complete validated packet is suitable). Exact retries
  reuse their slot; changed bytes/length fail, including after completion.
- `BeginNext(token, command)` exposes only the contiguous next command and
  grants application once. `ReadApplying` re-observes pending work without a
  second dispatch. Payload spans expire on confirmation, retirement or restart.
- `Complete(token, sequence, outcome)` records exactly one terminal result:
  Applied with reason zero, or Rejected/Cancelled with a nonzero 16-bit reason.
  A rejected invalid completion leaves pending work intact. `ReadOutcome`
  provides retained outcomes for bounded retransmission.
- `ConfirmThrough(token, sequence)` releases only the terminal prefix. The
  sender must have received its complete outcomes/reasons before confirming.
  Terminal completion alone grants no new capacity; duplicate receipts do not
  add capacity. Confirmed old ingress never applies again; its bytes are gone.
- `ReadProgress` reports terminal/confirmed prefixes, absolute grant, next
  expected sequence and received/missing masks. Bit zero denotes
  `confirmedThrough + 1`; received includes pending and terminal entries.
  These leases are independent of local BLE ingress/egress packet capacity.
- Sequence starts at one. Zero and `UINT32_MAX` are reserved; grants saturate at
  `LAST_SEQUENCE = UINT32_MAX - 1`. `nextExpected == UINT32_MAX` requires stream
  rotation, never counter wrap. `GrantThroughFor` exposes that bounded arithmetic.

## [millis.hpp](include/foxie_embedded/millis.hpp)

- `fxe::Millis()`, `SetMillisProviderForTests()`.
- `fxe::ManualMillis` and `fxe::ScopedMillisProviderOverride`.

## [deadline_timer.hpp](include/foxie_embedded/deadline_timer.hpp)

- `fxe::DeadlineTimer`: single-shot timer.

## [deadline_timer_manager.hpp](include/foxie_embedded/deadline_timer_manager.hpp)

- `fxe::DeadlineTimerManager`: manages multiple timers.

## [elapsed_time.hpp](include/foxie_embedded/elapsed_time.hpp)

- `fxe::ElapsedTime`: elapsed time helper.

## [performance_telemetry.hpp](include/foxie_embedded/performance_telemetry.hpp)

- `fxe::PerformanceTelemetry`: lightweight performance counters.

## [heap_stats.hpp](include/foxie_embedded/heap_stats.hpp)

- `fxe::HeapStats`: runtime heap usage snapshot.

## [hex_dump.hpp](include/foxie_embedded/hex_dump.hpp)

- `fxe::LogHexDump()` with `fxe::HexDumpOptions` for formatted hex + ASCII output.

## [rgb.hpp](include/foxie_embedded/rgb.hpp)

- `fxe::Rgb` and common constants.

## [frame_hash.hpp](include/foxie_embedded/frame_hash.hpp)

- `fxe::frame_hash`: FNV-1a hashing helpers for RGB frames.

## [logger.hpp](include/foxie_embedded/logger.hpp)

- `fxe::Trace/Debug/Info/Warn/Error`.
- `fxe::SetOutputLineCallback()` and buffered line helpers.

## [settings_store.hpp](include/foxie_embedded/settings_store.hpp)

- `fxe::SettingsStore`: JSON-backed settings store with deferred saves.
- Settings JSON is read and written incrementally through a 256-byte scratch buffer; the
  8,192-byte serialized-file limit is validation only and does not reserve RAM.
- Each store owns one 9,600-byte ArduinoJson document. The maximum firmware-schema
  [fixture](../../test/test_undo_history_persistence_roundtrip/test_main.cpp) includes
  normal settings, 128 undo entries and the current Litchcraft artwork: its
  5,229-byte serialized snapshot uses 7,507 bytes when reloaded, retaining
  2,093 bytes of arena headroom. The worker snapshot buffer remains 5,376 bytes.
  Top-level key canonicalization
  edits that document in place and reserves no second document. When selected-record
  sanitation removes a recoverable field and an alternate exists, one full-capacity
  heap document holds the compacted selected data and removed-key provenance while
  the resident document is reused to validate alternates. The pool is freed before
  `Load()` returns; clean, migration-only and no-alternate loads do not allocate it.
- Every parsed recovery candidate passes migration, project scrubbing and key
  normalization before it can replace a file. If a selected temporary or backup
  document needs repair, `Load()` retains the original files, preserves usable
  selected fields, and fills removed fields only from validated alternates.
  The repaired document is marked must-save. A deferred canonical rewrite or
  filename repair of a selected alternate remains load-repair-pending through
  queue acceptance; only the later normal verified save clears it and promotes
  the document. A restart before that save can still recover from the original
  files. This also applies when the primary file is missing or cannot parse.
  Migration sees original legacy spellings. Keys are then canonicalized before
  project scrubbing and again afterward to cover keys added by the scrub hook;
  each migration/scrub hook runs once per candidate.
- `fxe::SettingsStoreConfig::verifyWrites` (default on): after writing `*.tmp`, flush+close then read back and verify hash/size before renaming into place.
- `SettingsStore::ReadValue()`: typed accessors without extra `containsKey()` lookups.
- `SettingsStore::NotifyDocChanged()`: bump revision when mutating the doc without `MarkDirty()`.
- `SettingsStore::MarkDirty()`: schedule an ordinary save after the default
  2-second trailing debounce.
- `SettingsStore::MarkMustSaveDirty()`: schedule high-value state after the
  independent default 100 ms trailing debounce. A save from either tier
  persists the whole current document and clears both tiers.
- `SettingsStore::SetAsyncSaveRequestCallback()`: route `SaveNow()` and
  deferred `Update()` requests to a platform-owned asynchronous writer. The
  boolean callback returns true for acceptance and false for rejection. An
  accepting callback must copy the document before returning; `SaveNow()`
  reports queue acceptance rather than completed storage I/O.
- `SettingsStore::SetAsyncSaveAdmissionCallback()`: use the same asynchronous
  route with `AsyncSaveAdmission::Accepted`, `Deferred`, or `Rejected`.
  `Deferred` means no snapshot was accepted yet: `SaveNow()` returns false,
  retains dirty intent for a later `Update()`, and preserves existing errors
  without reporting a new write failure. `Rejected` reports a request failure
  and retries the latest document without disabling the shared writer.
  Installing either callback replaces the other.
  Acceptance does not clear a previous durability failure; the platform writer
  reports actual completion through `ReportAsyncSaveSuccess()` or
  `ReportAsyncSaveFailure()`.
- `SettingsStore::SaveNowBlocking()`: bypass the asynchronous route for a
  deliberate boot or shutdown boundary.
- `SettingsStore::HasTerminalSaveFailure()`: reports confirmed shared-storage
  failure or failed direct saving. Consumer refusal, load-repair warning and `Deferred`
  admission do not set this flag.
- `SettingsStore::GetConfirmedStorageFailureCount()`: monotonic evidence count.
  A writer compares before/after an attempt to avoid attributing an older sticky
  storage diagnostic to a newer consumer refusal.
- `SettingsStore::DisablePersistenceForSession()`: disables saving while keeping
  the live document, mounted-state evidence and error. A later accepted-save
  completion cannot re-enable it. This differs from boot default reseeding;
  the platform decides when to enter this RAM-only policy.
- `SettingsStore::ReformatFilesystemAndSaveNowBlocking()`: explicit destructive
  ESP32 recovery for the configured LittleFS `spiffs` partition. Force format,
  non-formatting remount, verified save and promoted-primary size/hash readback
  must all succeed. The caller provides its safe recovery document and storage
  access owner; use a private writer so the live RAM session is unchanged.
  Native returns `Unsupported`; this method never substitutes a normal mount.
- `SettingsStore::IsLoadRepairPending()`: a valid alternate document was loaded
  but filename repair failed, or a valid primary survived an unreadable alternate.
  Keep the document enabled and dirty; a verified
  synchronous save or asynchronous completion clears this state and its warning.
  Queue acceptance alone does not establish recovery.
- Settings reads distinguish absent files from open/read failures, malformed
  JSON, exhausted document capacity, and file-size limits. Open/read failures
  retry at most three times with closed handles; capacity/size failures do not.
  `SetStorageAccessCallbacks()` brackets each complete load attempt as well as
  the individual save accesses. Unreadable load candidates and verified pending
  files whose promotion failed remain available for recovery.
- If a pending `*.tmp` already exists, retries write and verify one separate
  `*.retry` scratch file so the selected pending document cannot be truncated.
  The previous pending file is removed only after the new primary is installed;
  failure to remove it remains a failed save. Boot never selects `*.retry`,
  which is uncommitted scratch, and explicit deletion removes it too.
- Deferred saves back off after failures at 100/200/400/800/1600/3200/5000 ms,
  capped at 5 s, while retaining the latest edits. Explicit `SaveNow()` remains
  an immediate request. A completed save resets the cooldown and the shared
  atomic current-error state; historical error counters remain diagnostic.
- `SettingsStore::SetStorageAccessCallbacks()`: bracket every persistence
  filesystem access, including mounts, verification reads, metadata queries,
  and mutations. LittleFS JSON I/O is emitted in bounded 512-byte chunks so a platform
  can yield time-critical peripherals between flash accesses.
- `SettingsStore::SetSaveOperationCallback()`: optional synchronous save-only
  witness after each access-release callback. Fixed scalar records identify the
  operation, source/destination path roles, requested bytes, writer save
  generation, result and operation errno (zero on success). Durations use
  `fxe::Millis()` from after acquisition through the filesystem call; acquisition
  wait, release work, paging and observer time are excluded. Original errno is
  restored before and after the observer. No filenames or file contents escape.
  Install before saving; observers must not reenter saving or change callbacks.
  Read-close on Arduino has no SDK result and records completion as success.
- `fxe::CachedSetting<T>`: cached settings value keyed by store revision for hot paths.

## [setting_descriptor.hpp](include/foxie_embedded/setting_descriptor.hpp)

- `fxe::SettingType`: schema value kind (`UInt32`, `Bool`, `String`, `Enum`).
- `fxe::SettingCategory`: metadata grouping (`Display`, `Animation`, `Behavior`, `Clock`, `Hardware`, `Internal`).
- `fxe::SettingDescriptor`: constexpr-friendly metadata record for key/type/category/default/range/allowed-values/description/flags.
- `fxe::SettingDescriptorFlags`: bitmask constants (`VISIBLE_TO_APP`, `PERSISTED`, `FORCE_SAVE`, `INTERNAL`). `FORCE_SAVE` selects the short must-save debounce tier; it does not perform storage I/O in the mutation call.

### Selected-record sanitation recovery

`SettingsStoreConfig::migrateFunc` canonicalizes the selected document and still
returns whether it changed. A migration, default, case normalization or retired
key is not authority to backfill an absent key from another snapshot.

`scrubFunc` receives a nullable `SettingsRemovedKeys*`. Before removing a corrupt
top-level field, report its exact key with `Record`; stop removing if it fails.
The selected load owns this synchronous context. Alternate validation and
pre-save sanitation receive null. Only reported absent fields can be recovered;
other selected fields and intentional omissions remain authoritative.

Recovery uses at most one existing 9,600-byte temporary JSON pool, allocated
lazily only for a recoverable selected removal with an available alternate.
The internal selected/eligibility wrapper never becomes saved data. Clean,
migration-only and no-alternate loads do not allocate this pool. Provenance or
selected-copy exhaustion fails before promotion or save and preserves all files.
Corrupt or overlong key names cannot authorize a truncated-name replacement.
Domain validation remains responsible for rejecting inconsistent records.
