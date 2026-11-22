# foxie_embedded

This directory contains a small set of reusable embedded-oriented libraries.

The original library software is copyright Foxie Products, LLC and licensed
under [MPL 2.0](../../LICENSE), including commercial reuse. Separately licensed
dependencies retain their own terms. Include the root license and preserve
applicable notices when extracting or redistributing this library.

Design goals:

- Deterministic behavior (avoid unpredictable heap allocation in hot paths)
- Small, testable modules (usable from both firmware and native tools)
- Clear layering (console I/O stays in the console/transport layer)

## Library contents

The library is organized as a single package:

- Public headers live in `include/foxie_embedded/`.
- Sources live in `src/`.

Modules provided:

- Button handling: debounced input with lambda-friendly callbacks.
- Byte-order helpers: little/big-endian read/write helpers for binary payloads.
- Console core: line editing, history, completion, and command dispatch.
  Register lifetime-long help and optional usage text with each command;
  `help <words>` searches command names, help and usage case-insensitively,
  requiring every word to match. It never invokes command/completion handlers.
  Put nested subcommands and their arguments in that same registered metadata.
  An optional `SetUnknownCommandHandler` raw callback runs only after registered
  command/alias lookup fails, preserving the same admission and print path.
  It receives parsed bounded arguments and a caller-owned lifetime-long context;
  returning false retains the ordinary unknown-command response. This fallback
  has no command-table entry and is absent from help and completion.
  Transports may identify each received byte with `LastInputSource()`; the
  submitted-line callback's and command handler's `CommandInputSource()` is Local or Remote only when the
  entire submitted line (including edits and Enter) has that source. Unknown
  and mixed input fail closed for callers requiring a local terminal.
  History/scratch recall has unknown provenance, so privileged commands require
  a fresh complete line rather than replaying shared terminal history.
- Fixed-capacity utilities: `FixedFunction` and `FixedString`.
- `OrderedReceiveWindow`: fixed storage with an optional smaller capacity at
  `Start`; admission, Progress and replenishment honor that negotiated capacity.
- Logger: lightweight logging frontend with pluggable output sink.
- Runtime helpers: clocks, timers, and runtime primitives.
- Settings store: ArduinoJson-backed persistence (LittleFS on device, filesystem on native).
- RGB + frame hashing helpers.

## Documentation

- API overview: see [API.md](API.md)
- Integration guide: see [INTEGRATION.md](INTEGRATION.md)
- Version history: see [version.md](version.md)
- Example projects: see `examples/`

## PlatformIO integration

Place this library under `lib/foxie_embedded`. No extra configuration is required.

Include public headers with the `foxie_embedded/` prefix, for example:

- `#include "foxie_embedded/core.hpp"`
- `#include "foxie_embedded/console.hpp"`
- `#include "foxie_embedded/settings_store.hpp"`

## Tests

Native unit tests live under `lib/foxie_embedded/tests` and can be run with:

```bash
pio test -e native -f test_all_native_foxie
```

## Notes

- Keep direct serial I/O scoped to console/transport code. Other code should use logger and/or an injected console output function.
- The Arduino logger owns static storage for its recursive FreeRTOS mutex.
  Concurrent first use initializes it once without allocating; callbacks may
  reenter logger APIs while a line is being delivered.
- Avoid Arduino `String` in firmware code; prefer fixed-capacity utilities.

[checksum.hpp](include/foxie_embedded/checksum.hpp) provides CRC-32 calculation and `CombineCrc32` for concatenated
records without rereading their bytes. Pass finalized CRCs and the exact second
record length; an empty sequence has finalized CRC zero. Combination uses fixed
read-only shift powers (4 KiB on a 32-bit target), with no heap or mutable cache.

SettingsStore keeps terminal saving policy separate from confirmed storage
failure evidence. Async owners pass `StorageFailureEvidence::Confirmed` only
for a witnessed write/sync/close/readback or underlying I/O failure. Admission,
preparation, timeout and generic path/owner refusals remain errors without that
evidence. Consumer rejection and unconfirmed asynchronous failures retain retry
intent without marking the shared writer terminal. Queue acceptance does not
clear evidence; verified save success does. Only the owning Update consumes
retry intent, so an older completion cannot erase a newer failed request.

`SettingsStore::Load()` reads without promoting, renaming, deleting or creating
settings files. Defaults, migrations and selected pending/backup recovery stay
in RAM without scheduling a save; a later user mutation persists them through
the ordinary verified save owner. Existing recovery sources remain intact until
that save. Proven erased storage is initialized only by an authorized save.
`SetVerifiedPrimaryLoadCallback` observes a successfully read canonical primary
before migrations or other RAM changes. A successful `Load()` or an alternate
source alone does not provide that provenance.

The armed before-persistence callback guards synchronous storage operations.
Asynchronous snapshot admission only prepares RAM and queues work, so it does
not invoke that callback; the asynchronous owner guards actual storage access.
Deferred admission retains armed intent. Accepted admission clears intent while
retaining any earlier durability failure until verified save success.

`Console::SetSettingsClearCallback` delegates the entire `settings clear|erase`
request before deletion, mutation notification or reload. Its Boolean result is
request admission, including asynchronous admission, rather than proof of durable
completion. An installed owner must report its eventual result. Without a
callback, the built-in synchronous clear remains available under storage admission.

Remote editor fragments retain `IConsoleTransport::RemoteInputGeneration()` from
existing admission/session authority. A changed or revoked generation discards
remote and mixed partial input before the next byte is handled; local-only input
survives. `CommandRemoteInputGeneration()` is valid only during dispatch and is
zero when no identified remote lease exists. Privileged remote owners must
reject zero and recheck their current authority before mutation.
