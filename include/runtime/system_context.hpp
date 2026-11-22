#pragma once

namespace fxe {
class SettingsStore;
class Console;
class PerformanceTelemetry;
} // namespace fxe

namespace runtime {

// SystemContext is an explicit ownership/DI container for top-level runtime services.
//
// Ownership model:
// - The context does not own objects; it only stores pointers to long-lived services.
// - On device, these services are typically owned by the firmware root (e.g. main.cpp).
// - Modules should take dependencies explicitly (constructor args or function params) and
//   never reach for hidden globals.
struct SystemContext {
    fxe::SettingsStore* settings = nullptr;
    fxe::Console* console = nullptr;
    fxe::PerformanceTelemetry* telemetry = nullptr;
};

} // namespace runtime
