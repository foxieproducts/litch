# foxie_embedded version history

## 0.1

Initial release of the unified foxie_embedded library package.

Contents:

- Button handling: debounced input with repeat and long-hold support.
- Byte-order helpers: little/big-endian read/write helpers for 1/2/4/8 bytes.
- Console core: line editing, history, completion, and command dispatch.
- Console transports: stream and in-memory implementations.
- Fixed-capacity utilities: `FixedFunction` and `FixedString`.
- Logger: lightweight logging frontend with pluggable output sink.
- Runtime helpers: millis provider, elapsed time, timers, telemetry, heap stats.
- Settings store: JSON-backed persistence with deferred saves.
- RGB helpers and frame hashing utilities.
