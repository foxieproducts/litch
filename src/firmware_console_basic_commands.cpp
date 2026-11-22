#ifdef ARDUINO

#include <Arduino.h>

#if defined(LITCH_ESP32)
#include <esp_heap_caps.h>
#endif

#include "firmware/firmware_console_internal.hpp"

#include "firmware/firmware_internal.hpp"
#include "firmware/firmware_version.hpp"
#include "firmware/ota_platform_adapter.hpp"
#include "modules/game_console_commands.hpp"

#include "foxie_embedded/fixed_string.hpp"
#include "foxie_embedded/heap_stats.hpp"
#include "foxie_embedded/millis.hpp"

namespace firmware_internal::console_commands {

namespace {

#if defined(LITCH_ESP32S3)
void PrintOtaAttemptEvidence(Console& console) {
    const auto& ble = GetSystem().ble;
    const uint64_t nonce = ble.ConnectionSessionBootNonce();
    console.PrintLn("boot_nonce: %08lx%08lx",
                    static_cast<unsigned long>(nonce >> 32U),
                    static_cast<unsigned long>(nonce & 0xFFFFFFFFU));

    firmware::OtaBootEvidence evidence{};
    const bool copied = firmware::CopyEsp32OtaBootEvidence(evidence);
    console.PrintLn("ota_boot: available=%u sample=%lu age_ms=%lu",
                    copied && evidence.available ? 1U : 0U,
                    static_cast<unsigned long>(evidence.sample),
                    static_cast<unsigned long>(copied
                        ? fxe::Millis() - evidence.capturedAtMs : 0U));
    if (copied && evidence.available) {
        const auto& snapshot = evidence.snapshot;
        console.PrintLn(
            "ota_sdk: running=%08lx selected=%08lx sector=%u sequence=%lu running_state=%u",
            static_cast<unsigned long>(snapshot.running.address),
            static_cast<unsigned long>(snapshot.selected.address),
            static_cast<unsigned>(snapshot.selectedEntry),
            static_cast<unsigned long>(snapshot.selectedSequence),
            static_cast<unsigned>(evidence.runningState));
        constexpr char SELECTOR_HEX_DIGITS[] = "0123456789abcdef";
        for (size_t index = 0U; index < snapshot.selectors.size(); ++index) {
            fxe::FixedString<2U * modules::OTA_POST_BOOT_SELECTOR_BYTES + 1U> raw;
            for (const uint8_t byte : snapshot.selectors[index]) {
                (void)raw.PushBack(SELECTOR_HEX_DIGITS[byte >> 4U]);
                (void)raw.PushBack(SELECTOR_HEX_DIGITS[byte & 0x0FU]);
            }
            console.PrintLn("ota_selector%u: %s",
                            static_cast<unsigned>(index), raw.CStr());
        }
    }

    modules::OtaAdmissionEvidence admission{};
    if (!ble.ReadOtaAdmissionEvidence(admission)) {
        console.PrintLn("ota_admission: available=0");
        return;
    }
    console.PrintLn(
        "ota_admission: available=1 attempt=%lu phase=%u source=%08lx target=%08lx source_sequence=%lu expected_sequence=%lu",
        static_cast<unsigned long>(admission.attempt),
        static_cast<unsigned>(admission.phase),
        static_cast<unsigned long>(admission.source.address),
        static_cast<unsigned long>(admission.target.address),
        static_cast<unsigned long>(admission.sourceSequence),
        static_cast<unsigned long>(admission.expectedSequence));
}
#endif

} // namespace

bool StartsWithStr(const char* s, const char* prefix) {
    if (!s || !prefix) {
        return false;
    }

    while (*prefix != '\0') {
        if (*s == '\0' || *s != *prefix) {
            return false;
        }
        ++s;
        ++prefix;
    }

    return true;
}

size_t AddMatchIfPrefix(const char* candidate,
                        const char* prefix,
                        const char** outMatches,
                        size_t matchCount,
                        const size_t outMatchesCap) {
    if (!candidate || candidate[0] == '\0') {
        return matchCount;
    }

    if (prefix && prefix[0] != '\0' && !StartsWithStr(candidate, prefix)) {
        return matchCount;
    }

    if (!outMatches || matchCount >= outMatchesCap) {
        return matchCount;
    }

    outMatches[matchCount] = candidate;
    return matchCount + 1;
}

size_t EngineComplete(const Console::Args&,
                      const size_t argIndex,
                      const char* prefix,
                      const char** outMatches,
                      const size_t outMatchesCap) {
    if (argIndex != 1) {
        return 0;
    }

    size_t matchCount = 0;
    matchCount = AddMatchIfPrefix("fen", prefix, outMatches, matchCount, outMatchesCap);
    matchCount = AddMatchIfPrefix("reset", prefix, outMatches, matchCount, outMatchesCap);
    return matchCount;
}

bool PerfCommand(Console& console, const Console::Args& args) {
    if (args.Has(1U) && args.Get(1U) == "reset") {
        GetSystemContext().telemetry->ResetServiceGaps();
        console.PrintLn("cadence_window_start_ms=%lu first_interval=excluded",
                        static_cast<unsigned long>(fxe::Millis()));
    }
    const auto snapshot = GetSystemContext().telemetry->GetSnapshot();
    console.PrintLn("loops=%lu last_dt_ms=%lu min_dt_ms=%lu max_dt_ms=%lu",
                    static_cast<unsigned long>(snapshot.loopCount),
                    static_cast<unsigned long>(snapshot.lastLoopDeltaMs),
                    static_cast<unsigned long>(snapshot.minLoopDeltaMs),
                    static_cast<unsigned long>(snapshot.maxLoopDeltaMs));
    console.PrintLn("dropped_frames=%lu i2c_errors=%lu min_free_heap=%lu",
                    static_cast<unsigned long>(snapshot.droppedFrames),
                    static_cast<unsigned long>(snapshot.i2cErrors),
                    static_cast<unsigned long>(snapshot.minFreeHeapBytes));
    console.PrintLn("service_gaps: at_ms=%lu input_available=%u input_completion_max_ms=%lu ble_available=%u ble_completion_max_ms=%lu",
                    static_cast<unsigned long>(fxe::Millis()),
                    unsigned(snapshot.inputCompletionGapMs != UINT32_MAX),
                    static_cast<unsigned long>(snapshot.inputCompletionGapMs == UINT32_MAX ? 0U : snapshot.inputCompletionGapMs),
                    unsigned(snapshot.bleCompletionGapMs != UINT32_MAX),
                    static_cast<unsigned long>(snapshot.bleCompletionGapMs == UINT32_MAX ? 0U : snapshot.bleCompletionGapMs));
    const auto& sys = GetSystem();
    const auto animation = sys.ledAnimation.Receipt();
    console.PrintLn("animation_task: started=%u sequence=%lu context_epoch=%lu stack_available=%u stack_min_free_bytes=%lu main_stack_min_free_bytes=%lu",
        unsigned(sys.ledAnimation.IsStarted()), static_cast<unsigned long>(animation.sequence),
        static_cast<unsigned long>(animation.contextEpoch),
        unsigned(sys.ledAnimation.IsStarted() && animation.stackFreeBytes != 0U),
        static_cast<unsigned long>(animation.stackFreeBytes),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    const auto leds = sys.ledAnimation.IsStarted()
        ? animation.delivery : sys.Leds().GetDeliverySnapshot();
    console.PrintLn("led_delivery: at_ms=%lu attempts=%lu submitted=%lu confirmed=%lu",
                    static_cast<unsigned long>(fxe::Millis()),
                    static_cast<unsigned long>(leds.attempts),
                    static_cast<unsigned long>(leds.submissions),
                    static_cast<unsigned long>(leds.confirmations));
    console.PrintLn("led_outcomes: flash_denied=%lu bus_timeout=%lu unchanged=%lu unconfirmed=%lu confirmed=%lu",
                    static_cast<unsigned long>(leds.outcomes[0]),
                    static_cast<unsigned long>(leds.outcomes[1]),
                    static_cast<unsigned long>(leds.outcomes[2]),
                    static_cast<unsigned long>(leds.outcomes[3]),
                    static_cast<unsigned long>(leds.outcomes[4]));
    console.PrintLn("led_timing: last_submit_ms=%lu last_confirm_ms=%lu max_submit_gap_ms=%lu max_confirm_gap_ms=%lu max_pre_submit_ms=%lu max_show_ms=%lu",
                    static_cast<unsigned long>(leds.lastSubmissionMs),
                    static_cast<unsigned long>(leds.lastConfirmationMs),
                    static_cast<unsigned long>(leds.maxSubmissionGapMs),
                    static_cast<unsigned long>(leds.maxConfirmationGapMs),
                    static_cast<unsigned long>(leds.maxPreSubmitMs),
                    static_cast<unsigned long>(leds.maxShowMs));
    for (unsigned confirmation = 0U; confirmation < 2U; ++confirmation) {
        const auto& gaps = confirmation ? leds.confirmationGaps : leds.submissionGaps;
        console.PrintLn("led_gaps: kind=%s le40=%lu le60=%lu le100=%lu le250=%lu gt250=%lu",
                        confirmation ? "confirmed" : "submitted",
                        static_cast<unsigned long>(gaps[0]),
                        static_cast<unsigned long>(gaps[1]),
                        static_cast<unsigned long>(gaps[2]),
                        static_cast<unsigned long>(gaps[3]),
                        static_cast<unsigned long>(gaps[4]));
    }
    for (unsigned i = 0U; i < leds.recentLongGapCount; ++i) {
        const auto& gap = leds.recentLongGaps[i];
        console.PrintLn("led_gap: kind=%s at_ms=%lu gap_ms=%lu",
                        gap.confirmation ? "confirmed" : "submitted",
                        static_cast<unsigned long>(gap.atMs),
                        static_cast<unsigned long>(gap.gapMs));
    }
    return true;
}

bool HeapCommand(Console& console, const Console::Args&) {
    const fxe::HeapStats stats = fxe::GetHeapStats();
    if (!stats.supported) {
        console.PrintLn("heap: unsupported");
        return false;
    }

    console.PrintLn("heap: total=%lu used=%lu free=%lu min_free=%lu",
                    static_cast<unsigned long>(stats.totalBytes),
                    static_cast<unsigned long>(stats.usedBytes),
                    static_cast<unsigned long>(stats.freeBytes),
                    static_cast<unsigned long>(stats.minFreeBytes));

#if defined(LITCH_ESP32)
    console.PrintLn("hw: model=%s rev=%u cores=%u cpu_mhz=%u",
                    ESP.getChipModel(),
                    static_cast<unsigned>(ESP.getChipRevision()),
                    static_cast<unsigned>(ESP.getChipCores()),
                    static_cast<unsigned>(ESP.getCpuFreqMHz()));
    console.PrintLn("hw: sdk=%s", ESP.getSdkVersion());
    console.PrintLn("flash: size=%lu speed_hz=%lu mode=%u",
                    static_cast<unsigned long>(ESP.getFlashChipSize()),
                    static_cast<unsigned long>(ESP.getFlashChipSpeed()),
                    static_cast<unsigned>(ESP.getFlashChipMode()));
    // Memory observation must not invoke getSketchSize: the Arduino getter
    // verifies the entire image on this cooperative loop, adding allocation,
    // stack and flash work to the operation being measured.
    console.PrintLn("psram: size=%lu free=%lu",
                    static_cast<unsigned long>(ESP.getPsramSize()),
                    static_cast<unsigned long>(ESP.getFreePsram()));

    {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
        console.PrintLn("heap_caps_internal: free=%lu alloc=%lu min_free=%lu largest=%lu",
                        static_cast<unsigned long>(info.total_free_bytes),
                        static_cast<unsigned long>(info.total_allocated_bytes),
                        static_cast<unsigned long>(info.minimum_free_bytes),
                        static_cast<unsigned long>(info.largest_free_block));
        console.PrintLn("heap_caps_internal: free_blocks=%lu alloc_blocks=%lu total_blocks=%lu",
                        static_cast<unsigned long>(info.free_blocks),
                        static_cast<unsigned long>(info.allocated_blocks),
                        static_cast<unsigned long>(info.total_blocks));
    }

    if (ESP.getPsramSize() > 0) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
        console.PrintLn("heap_caps_spiram: free=%lu alloc=%lu min_free=%lu largest=%lu",
                        static_cast<unsigned long>(info.total_free_bytes),
                        static_cast<unsigned long>(info.total_allocated_bytes),
                        static_cast<unsigned long>(info.minimum_free_bytes),
                        static_cast<unsigned long>(info.largest_free_block));
        console.PrintLn("heap_caps_spiram: free_blocks=%lu alloc_blocks=%lu total_blocks=%lu",
                        static_cast<unsigned long>(info.free_blocks),
                        static_cast<unsigned long>(info.allocated_blocks),
                        static_cast<unsigned long>(info.total_blocks));
    }
#endif

    return true;
}

bool EngineCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();

    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view();
    if (sub.empty()) {
        console.PrintLn("usage: engine <fen|reset> ...");
        return false;
    }

    if (sub == "fen") {
        modules::GameConsoleCommandContext context;
        context.console = &console;
        context.gamePersistence = &sys.gamePersistence;
        return modules::FenCommand(context, args, 2U);
    }

    if (sub == "reset") {
        const bool ok = sys.gamePersistence.ResetEngine();
        console.PrintLn(ok ? "OK" : "FAIL");
        return ok;
    }

    console.PrintLn("usage: engine <fen|reset> ...");
    return false;
}

bool VersionCommand(Console& console, const Console::Args& args) {
    if (args.argc != 1) {
        console.PrintLn("Usage: version");
        return false;
    }

    console.PrintLn("firmware: %s", firmware::FIRMWARE_VERSION);
#if defined(LITCH_ESP32S3)
    uint8_t digest[modules::FirmwareDigestProvider::DIGEST_BYTES] = {};
    const auto digestState = GetSystem().ble.CachedRunningFirmwareDigest(
        digest, sizeof(digest));
    if (digestState == modules::FirmwareDigestState::Ready) {
        char hex[2U * sizeof(digest) + 1U] = {};
        constexpr char HEX_DIGITS[] = "0123456789abcdef";
        for (size_t index = 0U; index < sizeof(digest); ++index) {
            hex[2U * index] = HEX_DIGITS[digest[index] >> 4U];
            hex[2U * index + 1U] = HEX_DIGITS[digest[index] & 0x0FU];
        }
        console.PrintLn("image_sha256: %s", hex);
    } else {
        console.PrintLn("image_sha256: pending");
    }
    console.PrintLn("ota_accepted: %u",
                    GetSystem().ble.IsOtaRuntimeAccepted() ? 1U : 0U);
    PrintOtaAttemptEvidence(console);
#endif
    return true;
}

} // namespace firmware_internal::console_commands

#endif
