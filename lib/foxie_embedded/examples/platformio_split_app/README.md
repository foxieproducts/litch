# foxie_embedded example: split App (PlatformIO + Arduino)

This is a copy-paste friendly example showing how to structure a small firmware project that uses `foxie_embedded` for:

- Console (`fxe::Console` + `fxe::StreamConsoleTransport`)
- Settings persistence (`fxe::SettingsStore`)
- Logger sink routing (`fxe::SetOutputLineCallback`)
- Debounced button input (`fxe::Button`)

The point of the example is *structure*: [main.cpp](src/main.cpp) stays small, and ownership/wiring is pushed into `App` and helper modules.

## How to use

1) Copy this folder into your project root (or just copy `src/` into your `src/`).

2) Ensure the library exists at `lib/foxie_embedded/`.

3) Update your [platformio.ini](platformio.ini) with ArduinoJson + LittleFS support for your platform.

## Layout

- [src/main.cpp](src/main.cpp): Arduino `setup()`/`loop()` only.
- `src/app.*`: top-level owner of subsystems.
- `src/app_console.*`: app commands + console wiring.
- `src/app_settings.*`: runtime settings struct + load/persist/migrate/scrub.
- `src/app_buttons.*`: button config + events.
- `src/app_logging.*`: logger sink routing.
- [src/app_pins.hpp](src/app_pins.hpp): central pin definitions.

## Notes

- This example is intentionally conservative about heap churn: no `String`, and it uses the fixed-capacity helpers already available in `foxie_embedded`.
- Settings changes made by the built-in console `set` command are detected by watching `Setti                   ngsStore::GetRevision()` and re-loading the cached runtime settings.
