# foxie_embedded integration

This library is a single package under `lib/foxie_embedded` with public headers in `include/foxie_embedded` and sources in `src`.

## Include paths

Prefer the explicit prefix:

- `#include "foxie_embedded/core.hpp"` for an umbrella include
- Or include specific headers, e.g. `#include "foxie_embedded/console.hpp"`

For binary protocols, include `#include "foxie_embedded/byte_order.hpp"`.

## PlatformIO

Place the library at `lib/foxie_embedded`. PlatformIO will discover it automatically when located under the `lib/` folder.

## Console setup

1. Choose a transport.
   - `fxe::StreamConsoleTransport` for Arduino `Stream`.
   - `fxe::MemoryConsoleTransport` for native or tests.
2. Construct `fxe::Console` with the transport. Use `fxe::Console::Config` to disable built-ins if needed.
3. Register commands and call `Update()` regularly.

Built-in commands are installed automatically in the constructor.

## Settings store

1. Construct `fxe::SettingsStore` with a config path.
2. Call `Load()` once at startup.
3. Mutate `Doc()` and call `MarkDirty()` for ordinary settings or
   `MarkMustSaveDirty()` for high-value state, plus `Update()` periodically.
   The default trailing debounces are 2 seconds and 100 ms respectively, and
   their timers are independent.
4. Reserve `SaveNow()` for explicit user save commands and verified lifecycle
   drains. An asynchronous save callback reports admission, not durability.
   Use `SetAsyncSaveAdmissionCallback()` when the writer can temporarily defer
   requests: return `Accepted` only after copying the document, `Deferred`
   when no snapshot was accepted yet, and `Rejected` for a failed request.
   Deferral makes `SaveNow()` return false while retaining dirty intent and
   prior errors for a later `Update()`; it creates no new write-failure report.
   The legacy `SetAsyncSaveRequestCallback()` keeps true/false acceptance and
   rejection semantics. Installing either callback replaces the other.
   Unconfirmed consumer failures retain retry intent without disabling the
   shared writer. Report actual completion through its success/failure callbacks;
   use its verified flush boundary or `SaveNowBlocking()` when completed
   storage I/O is required before shutdown.
5. Platforms that coordinate flash with time-critical peripherals can install
   `SetStorageAccessCallbacks()`. Device reads and writes are chunked at 256
   bytes, and mounts, metadata queries, flushes, closes, removals, and renames
   are bracketed separately as well.
6. Set `allowSynchronousDeferredSaves=false` when deferred writes must never
   fall back to the caller if the asynchronous writer is unavailable. Explicit
   `SaveNow()`/`SaveNowBlocking()` lifecycle drains remain available.

After a confirmed shared-storage failure, a platform may choose
`DisablePersistenceForSession()` to preserve its live RAM document while
suspending further saves. Keep user controls and validation in their existing
owners and report session-only behavior. `Deferred` admission and a retryable
load-repair warning are not terminal failures. The library itself retains its
normal retry policy until the platform selects this session policy.

Destructive recovery is a separate explicit operation. Freeze only the safe
fields the product promises to preserve into a private writer, acquire the
existing storage owner, and call `ReformatFilesystemAndSaveNowBlocking()`.
Report completion from its verified result, keep the current live document
unchanged, and require a fresh boot before ordinary saving resumes.

### Settings performance guidance

- `JsonDocument` lookups are not constant time. `Doc()["KEY"]` performs a key lookup and type conversion.
- Avoid doing `Doc()["KEY"]` in tight loops. Read once, store in a local variable, and use that.
- Prefer a runtime settings struct for hot-path reads. Load from JSON at startup, then use plain variables during runtime.
- Only write when values actually change, then call `MarkDirty()` once per logical change.
- If you need faster reads, use `fxe::SettingsStore::ReadValue()` or `fxe::CachedSetting<T>`.
- A `JsonObjectConst` snapshot can further reduce repeated `Doc()` calls and key hashing.
- Use `fxe::ElapsedTime` to measure access costs if you are unsure.

## Logger

1. Register a sink with `fxe::SetOutputLineCallback()`.
2. Use `fxe::Info`, `fxe::Warn`, `fxe::Error` as needed.

## Deterministic time

Use `fxe::ManualMillis` and `fxe::ScopedMillisProviderOverride` from [foxie_embedded/millis.hpp](include/foxie_embedded/millis.hpp) in tests to control time deterministically.

## Settings runtime struct pattern

Use a small struct to cache values after `Load()`, then use the struct in hot paths.

```cpp
struct RuntimeSettings {
   uint8_t brightness = 0;
   bool diagnosticsEnabled = false;
   int32_t telemetryLevel = 0;
};

inline void LoadRuntimeSettings(fxe::SettingsStore& store, RuntimeSettings& out) {
   const JsonObjectConst root = store.Doc().as<JsonObjectConst>();
   out.brightness = static_cast<uint8_t>(root["BRIGHTNESS"] | 0);
   out.diagnosticsEnabled = (root["DIAGNOSTICS"] | 0) != 0;
   out.telemetryLevel = static_cast<int32_t>(root["TELEMETRY_LEVEL"] | 0);
}

inline void PersistRuntimeSettings(fxe::SettingsStore& store,
                           const RuntimeSettings& settings) {
   JsonDocument& doc = store.Doc();
   doc["BRIGHTNESS"] = settings.brightness;
   doc["DIAGNOSTICS"] = settings.diagnosticsEnabled ? 1 : 0;
   doc["TELEMETRY_LEVEL"] = settings.telemetryLevel;
   store.MarkDirty();
}
```

## Examples

### Button

```cpp
fxe::Button button;
fxe::Button::Config cfg;
cfg.debounceMs = 20;
cfg.onEvent = [](const fxe::Button::Event evt) {
   (void)evt;
};
button.SetConfig(cfg);

bool isPressed = false;
button.SetDetectionFn([&]() { return isPressed; });

// Call once per input scan.
button.Update();
```

### Console

```cpp
fxe::StreamConsoleTransport transport(Serial);
fxe::Console::Config cfg;
cfg.installBuiltins = false;
fxe::Console console(transport, cfg);

console.RegisterCommand("ping", "Respond with pong", [&](const fxe::Console::Args&) {
   console.PrintLn("pong");
   return true;
});

console.Update();
```

### Console transport (memory)

```cpp
fxe::MemoryConsoleTransport transport;
fxe::Console console(transport);

fxe::FixedString<256> output;
transport.SetOutputCallback([&output](const char* data, const size_t len) {
   if (!data || len == 0) {
      return;
   }
   (void)output.Append(std::string_view(data, len));
});

transport.PushBytes("help\n", 5);
console.Update();
const char* text = output.CStr();
```

### FixedFunction

```cpp
fxe::FixedFunction<void(int), 32> fn = [](int value) {
   (void)value;
};

fn(7);
```

### FixedString

```cpp
fxe::FixedString<64> buf;
buf.Format("Temp: %dC", temperatureC);
const char* text = buf.CStr();
```

### Logger

```cpp
fxe::SetOutputLineCallback([](const char* line) {
   (void)line;
   /* write line to transport */
});

fxe::Info("System ready");
```

### SettingsStore

```cpp
fxe::SettingsStoreConfig cfg;
cfg.path = "/settings.json";

fxe::SettingsStore store(cfg);
store.Load();

store.Doc()["BRIGHTNESS"] = 5;
store.MarkDirty();
store.Update();

// Typed read helper (avoids extra containsKey() lookup).
const uint32_t brightness = store.ReadValue("BRIGHTNESS", 5U);

// Cached value for hot paths (refreshes when settings revision changes).
fxe::CachedSetting<uint32_t> brightnessCache("BRIGHTNESS", 5U);
const uint32_t cachedBrightness = brightnessCache.Get(store);
```

### Millis override (tests)

```cpp
fxe::ScopedMillisProviderOverride overrideProvider(&fxe::ManualMillisProvider);
fxe::ManualMillis::SetNowMs(1000);
const uint32_t nowMs = fxe::Millis();
```

### DeadlineTimer

```cpp
fxe::DeadlineTimer timer;
timer.Start(250);
if (timer.IsExpired()) {
   /* expired */
}
```

### DeadlineTimerManager

```cpp
fxe::DeadlineTimerManager<8> manager;
const int handle = manager.Start(500, []() {});
manager.Update();
if (handle >= 0) {
   /* expired */
}
```

### ElapsedTime

```cpp
fxe::ElapsedTime elapsed;
elapsed.Reset();
if (elapsed.HasElapsed(100)) {
   /* 100 ms passed */
}
```

### PerformanceTelemetry

```cpp
fxe::PerformanceTelemetry telemetry;
telemetry.OnLoop();
```

### HeapStats

```cpp
fxe::HeapStats stats = fxe::HeapStats::Capture();
const size_t freeBytes = stats.freeBytes;
```

### RGB helpers

```cpp
fxe::Rgb color = fxe::Rgb(255, 128, 0);
const uint8_t r = color.r;
```

### Frame hash

```cpp
uint32_t hash = 0;
hash = fxe::frame_hash::Append(hash, color);
```

### Byte order

```cpp
uint8_t payload[8] = {};
fxe::WriteU32BE(0x11223344U, payload);
const uint32_t value = fxe::ReadU32BE(payload);
```
