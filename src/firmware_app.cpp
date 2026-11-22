#ifdef ARDUINO

#include "firmware/firmware_app.hpp"
#include "firmware/firmware_app_internal.hpp"
#include "runtime/progress_watchdog.hpp"
#include "firmware/boot_platform_timing.hpp"
#include "foxie_embedded/millis.hpp"
#if defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
#include "runtime/panic_diagnostic.hpp"
#endif

namespace firmware {

volatile uint32_t g_expanderIntCount[4] = {0, 0, 0, 0};

void Setup() {
#if defined(LITCH_BOOT_ENTRY_DIAGNOSTIC)
    runtime::panic_diagnostic::NoteSetupEntry();
#endif
    PlatformBootTiming().setupBeginMs = fxe::Millis();
    runtime::StartProgressWatchdog();
    PlatformBootTiming().watchdogEndMs = fxe::Millis();
    firmware_internal::SetupApp();
    runtime::FeedProgressWatchdog();
}

void Loop() {
    runtime::NoteProgress(0U, runtime::ProgressStage::Loop);
    firmware_internal::LoopApp();
    runtime::FeedProgressWatchdog();
}

} // namespace firmware

#endif
