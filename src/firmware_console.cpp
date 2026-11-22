#ifdef ARDUINO

#include <Arduino.h>
#include <esp_timer.h>

#include <atomic>
#include <cstring>

#include "firmware/firmware_console_internal.hpp"
#include "firmware/firmware_console_logger_adapter.hpp"
#include "firmware/boot_platform_timing.hpp"
#include "firmware/hybrid_console_transport.hpp"
#include "firmware/s3_usb_console_device.hpp"
#include "firmware/startup_console_policy.hpp"
#include "firmware/usb_cdc_console_endpoint.hpp"
#include "firmware/usb_boot_probe.hpp"

#include "firmware/firmware_internal.hpp"
#include "firmware/board_storage_recovery.hpp"

#include "logger.hpp"
#include "modules/board_owner_persistence.hpp"
#include "modules/trace_console_commands.hpp"
#include "modules/sdk_att_witness.hpp"

namespace {

std::atomic<bool> g_consoleOutputEnabled{false};
bool g_consoleWelcomed = false;
firmware_internal::FirmwareConsoleLoggerAdapter g_consoleLogger;

bool IsConsoleOutputEnabled() {
    return g_consoleOutputEnabled.load(std::memory_order_acquire);
}

void MarkConsoleOutputEnabled() {
    g_consoleOutputEnabled.store(true, std::memory_order_release);
}

void ConsoleLineToTrace(const char* line) {
#ifdef LITCH_TRACE
    if (line) {
        firmware_internal::GetTraceRecorder().RecordCmd(line);
    }
#else
    (void)line;
#endif
}

bool ConsoleEnteredLine(const char* line) {
    auto& ctx = firmware_internal::GetSystemContext();
    if (!ctx.console || !line) {
        return false;
    }

    auto& sys = firmware_internal::GetSystem();
    // Recovery can take over after Console initialization. Its existing local
    // storage-reset authority must not inherit the normal owner-only callback.
    if (sys.safeModeActive) ctx.console->SetSettingsClearCallback({});
    if (sys.settingsClearPending) {
        ctx.console->PrintLn("settings clear: pending; wait for reset or restart-required feedback");
        return true;
    }
    if (!sys.safeModeActive && !sys.startupLeds.IsReady() &&
        firmware::HandleStartupConsoleLine(
            *ctx.console, line,
            firmware_internal::IsLedBrownoutLimitLatched(sys.settings) &&
                sys.settings.IsPersistenceGuardArmed())) {
        return true;
    }

    if (sys.factoryBootOnly) {
        const std::string_view entered(line);
        const auto end = entered.find_first_of(" \t");
        const auto command = entered.substr(0U, end);
        constexpr std::string_view allowed[] = {"help", "version", "ver", "heap", "perf",
            "hwtest", "factory", "reboot", "safemode", "ledpower", "cc", "5ven",
            "setled", "input", "ambient", "comptest", "ble", "i2c"};
        for (const auto candidate : allowed) if (command == candidate) return false;
        ctx.console->PrintLn("factory mode: ordinary game/settings commands unavailable; finish hwtest or reboot");
        return true;
    }
    if (sys.amptest.HandleEnteredLine(*ctx.console, std::string_view(line))) {
        return true;
    }

    return sys.amptestVerify.HandleEnteredLine(*ctx.console, std::string_view(line));
}

void LoggerLineToConsole(const char* line) {
    if (!IsConsoleOutputEnabled()) {
        return;
    }

    if (!line) {
        return;
    }

#ifdef LITCH_TRACE
    if (std::strncmp(line, "{\"type\":\"tick\"", 14) == 0) {
        return;
    }
#endif

    (void)g_consoleLogger.Enqueue(line);
}

void PrintBufferedLoggerLine(const char* line) {
    auto* const console = firmware_internal::GetSystemContext().console;
    if (console != nullptr && line != nullptr) {
        console->PrintLn("%s", line);
    }
}

void DrainQueuedLoggerLines(fxe::Console& console) {
    (void)g_consoleLogger.DrainTo(console);
}

} // namespace

namespace firmware_internal {

void ConfigureConsoleLogger() {
#ifdef LITCH_RELEASE_PROFILE
    logger::SetMinLevel(logger::Level::INFO);
#else
    logger::SetMinLevel(logger::Level::DEBUG);
#endif
    logger::SetOutputLineCallback(&LoggerLineToConsole);
}

void EnsureSettingsLoaded() {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    auto& sys = GetSystem();
    auto& settings = sys.settings;
    settings.SetVerifiedPrimaryLoadCallback(
        [](void* context, const JsonDocument& document) {
            static_cast<FirmwareSystem*>(context)->boardIdentity.ObserveVerifiedPrimaryLoad(document);
        }, &sys);
#ifdef LITCH_DISABLE_SETTINGS_LOAD
    settings.DisableForBoot();
    sys.settingsLoadAttempted = false;
    sys.settingsLoadSucceeded = true;
#else
    if (sys.safeModeActive) {
        settings.DisableForBoot();
        sys.settingsLoadAttempted = false;
        sys.settingsLoadSucceeded = false;
    } else {
        sys.settingsLoadAttempted = true;
        sys.settingsLoadSucceeded = sys.gamePersistence.LoadSettingsForRecovery();
    }
#endif

    loaded = true;
}

void EnsureConsoleInitialized() {
    EnsureSettingsLoaded();
    static bool initialized = false;
    if (initialized) {
        return;
    }

    auto& sys = GetSystem();
    auto& ctx = GetSystemContext();
    auto& settings = *ctx.settings;

#if defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
    static S3UsbConsoleDevice usbDevice;
    static UsbCdcConsoleEndpoint serialEndpoint(Serial, usbDevice);
    static HybridConsoleTransport transport(serialEndpoint, sys.ble);
#else
    static HybridConsoleTransport transport(Serial, sys.ble);
#endif
    static Console console(transport);
    Console* consolePtr = &console;
    ctx.console = &console;

    console.SetSettingsStore(&settings);
    console.SetSettingsLoadCallback([]() {
        return GetSystem().gamePersistence.LoadSettingsForRecovery();
    });
    console.SetStorageReformatCallback([](const Console::StorageReformatAction action) {
        return action == Console::StorageReformatAction::Confirm
            ? ConfirmBoardStorageRecovery(GetSystem())
            : BoardStorageRecoveryStatus(GetSystem());
    });
    console.SetSettingsKeyEnumCallback(&SettingsKeyEnumCallback);
    console.SetSettingsWritePolicyCallback(&SettingsWritePolicyCallback);
    console.SetSettingsDiagnosticAccessCallback([](std::string_view key) {
        return !modules::IsBoardIdentitySettingKey(key) &&
               !modules::IsBoardOwnerSettingKey(key);
    });
    console.SetSettingsMutationCallback(&BeforeSettingsMutationCallback);
    console.SetSettingsStorageAdmissionCallback(
        [](Console::SettingsStorageOperation operation) {
            // Only clear/erase has an authenticated asynchronous reset owner.
            // Other whole-store replacement retains physical recovery admission.
            return !modules::OWNER_PAIRING_ENABLED ||
                operation == Console::SettingsStorageOperation::Erase ||
                GetSystem().safeModeActive || GetSystem().factoryBootOnly;
        });
    if (modules::OWNER_PAIRING_ENABLED && !sys.safeModeActive && !sys.factoryBootOnly) {
        console.SetSettingsClearCallback([consolePtr]() {
            return RequestSettingsClear(*consolePtr);
        });
    }
    console.SetSettingsDescriptorLookupCallback(&SettingsDescriptorLookupCallback);
    console.SetSettingsValidateAndSetCallback(&SettingsValidateAndSetCallback);

    if (sys.factoryBootOnly) {
        // Register only owners initialized by the factory branch. Built-in raw
        // settings/storage commands are rejected by ConsoleEnteredLine above.
        struct Command {
            const char* name;
            const char* help;
            bool (*run)(Console&, const Console::Args&);
        };
        const Command commands[] = {
            {"perf", "Performance telemetry: perf [reset] (service-gap window)", &console_commands::PerfCommand},
            {"heap", "Heap diagnostics", &console_commands::HeapCommand},
            {"version", "Firmware version and image digest", &console_commands::VersionCommand},
            {"ver", "Alias: version", &console_commands::VersionCommand},
            {"reboot", "Save and reboot", &console_commands::RebootCommand},
            {"safemode", "Recovery: safemode [status|clear|reboot]", &console_commands::SafeModeCommand},
            {"ble", "BLE: on|off|status|test [duration_s] [delay_ms]", &console_commands::BleCommand},
#if defined(CONFIG_IDF_TARGET_ESP32S3)
            {"input", "Physical controls: input scan | input capture [status|poll|irq]", &console_commands::InputCommand},
#else
            {"input", "Physical GPIO diagnostics: input scan", &console_commands::InputCommand},
#endif
            {"ambient", "Light sensor diagnostics: ambient [status|on|off]", &console_commands::AmbientCommand},
            {"comptest", "Governed compliance diagnostics", &console_commands::ComptestCommand},
            {"ledpower", "LED current protection: ledpower [status|clear|latch]", &console_commands::LedPowerCommand},
            {"cc", "USB-C source: cc [read|show|clear|set <raw500> <raw1500> <raw3000>]", &console_commands::UsbCcCommand},
            {"i2c", "I2C clock: i2c status | i2c set <100k|400k|1m>", &console_commands::I2cCommand},
            {"5ven", "LED power rail: 5ven <on|off>", &console_commands::FiveVEnCommand},
            {"setled", "LED diagnostic: setled board <index> <RRGGBB> | setled clear", &console_commands::SetLedCommand},
        };
        for (const auto& command : commands) {
            console.RegisterCommand(command.name, command.help,
                [consolePtr, run = command.run](const Console::Args& args) {
                    return run(*consolePtr, args);
                });
        }
        RegisterFeatureConsoleCommands(sys, console);
        console.SetEnteredLineCallback(&ConsoleEnteredLine);
        initialized = true;
        return;
    }
    console.RegisterCommand("support", "Local support snapshot: support [settingsread] (read-only primary settings timing)", [consolePtr](const Console::Args& args) {
        auto& sys = GetSystem();
#if defined(LITCH_PANIC_DIAGNOSTIC) && defined(LITCH_ESP32S3) && defined(LITCH_BLE) && defined(LITCH_BLE_OTA)
        // Sample before printing the longer support inventory. Each owner is
        // independently sampled; unavailable worker values are not evidence.
        const auto liveIngress = sys.ble.GetLiveIngressDiagnostics();
        const auto liveWorker = sys.settingsPersistence.GetLiveIngressDiagnostics();
        const auto ingressPerformance = sys.settingsPersistence.GetIngressPerformanceDiagnostics();
        // e=logical/native epoch; obj=accepted/grant/pending;
        // rx=current-native-epoch available/sequence/enqueued;
        // raw=ordinary/priority/type/kind/sequence; fifo=count/bytes/claimed/ordinal/op.
        consolePtr->PrintLn("ble_ingress_live: e=%lu/%lu obj=%lu/%lu/%lu rev=%lu/%lu rx=%u/%lu/%u tx=%lu raw=%u/%u/%u/%u/%lu fifo=%u/%u/%u/%lu/%u",
            static_cast<unsigned long>(liveIngress.session.generation),
            static_cast<unsigned long>(liveIngress.nativeEpoch),
            static_cast<unsigned long>(liveIngress.session.accepted),
            static_cast<unsigned long>(liveIngress.session.grant),
            static_cast<unsigned long>(liveIngress.session.pending),
            static_cast<unsigned long>(liveIngress.session.reverseConfirmed),
            static_cast<unsigned long>(liveIngress.session.reverseGrant),
            unsigned(liveIngress.nativeAvailable),
            static_cast<unsigned long>(liveIngress.nativeSequence),
            unsigned(liveIngress.nativeEnqueued),
            static_cast<unsigned long>(liveIngress.progressSubmitted),
            unsigned(liveIngress.raw.ordinaryCount), unsigned(liveIngress.raw.priorityCount),
            unsigned(liveIngress.raw.headType), unsigned(liveIngress.raw.headKind),
            static_cast<unsigned long>(liveIngress.raw.headSequence),
            unsigned(liveIngress.store.count), unsigned(liveIngress.store.bytes),
            unsigned(liveIngress.store.readClaimed),
            static_cast<unsigned long>(liveIngress.store.headOrdinal), unsigned(liveIngress.store.headOperation));
        consolePtr->PrintLn("store_ingress_live: available=%u owner_epoch=%lu response_bytes=%u prefetch=%u ingress_pending=%u mode_save_pending=%u storage_active=%u handle=%u/%u age_ms=%lu",
            unsigned(liveWorker.available), static_cast<unsigned long>(liveWorker.ownerEpoch),
            unsigned(liveWorker.responseSize), unsigned(liveWorker.prefetchCount),
            unsigned(liveWorker.ingressPending), unsigned(liveWorker.modeSavePending),
            unsigned(liveWorker.storageActive), unsigned(liveWorker.handlePhase), unsigned(liveWorker.handleOperation),
            static_cast<unsigned long>(liveWorker.handleAgeMs));
        // All values are cached and copied together under the worker mutex.
        // Slots are first/part-transition/ordinary calls,total_ms,max_ms;
        // flash tuples are calls,bytes,driver_us,max_driver_us (kind 0 body,1 commit).
        const auto& perf = ingressPerformance;
        consolePtr->PrintLn("store_perf: available=%u physical=%u epoch=%lu request=%lu skipped=%lu calls=%lu/%lu slots=%lu/%lu/%lu,%lu/%lu/%lu,%lu/%lu/%lu",
            unsigned(perf.available), unsigned(perf.physicalAvailable),
            static_cast<unsigned long>(perf.ownerEpoch), static_cast<unsigned long>(perf.request),
            static_cast<unsigned long>(perf.skipped), static_cast<unsigned long>(perf.calls[0U]), static_cast<unsigned long>(perf.calls[1U]),
            static_cast<unsigned long>(perf.slots[0U].calls), static_cast<unsigned long>(perf.slots[0U].totalMs), static_cast<unsigned long>(perf.slots[0U].maximumMs),
            static_cast<unsigned long>(perf.slots[1U].calls), static_cast<unsigned long>(perf.slots[1U].totalMs), static_cast<unsigned long>(perf.slots[1U].maximumMs),
            static_cast<unsigned long>(perf.slots[2U].calls), static_cast<unsigned long>(perf.slots[2U].totalMs), static_cast<unsigned long>(perf.slots[2U].maximumMs));
        for (size_t kind = 0U; kind < perf.flash.size(); ++kind) {
            const auto& flash = perf.flash[kind];
            consolePtr->PrintLn("store_flash: kind=%u read=%lu/%lu/%lu/%lu prog=%lu/%lu/%lu/%lu erase=%lu/%lu/%lu/%lu",
                unsigned(kind),
                static_cast<unsigned long>(flash.physical[0U].calls), static_cast<unsigned long>(flash.physical[0U].bytes), static_cast<unsigned long>(flash.physical[0U].totalUs), static_cast<unsigned long>(flash.physical[0U].maxUs),
                static_cast<unsigned long>(flash.physical[1U].calls), static_cast<unsigned long>(flash.physical[1U].bytes), static_cast<unsigned long>(flash.physical[1U].totalUs), static_cast<unsigned long>(flash.physical[1U].maxUs),
                static_cast<unsigned long>(flash.physical[2U].calls), static_cast<unsigned long>(flash.physical[2U].bytes), static_cast<unsigned long>(flash.physical[2U].totalUs), static_cast<unsigned long>(flash.physical[2U].maxUs));
            consolePtr->PrintLn("store_wait: kind=%u outer=%lu/%lu handoff=%lu/%lu",
                unsigned(kind), static_cast<unsigned long>(flash.outerCalls), static_cast<unsigned long>(flash.outerWaitMs),
                static_cast<unsigned long>(flash.handoffs), static_cast<unsigned long>(flash.handoffWaitMs));
        }
#endif
        const bool requestSettingsRead =
            args.Has(1U) && args.Get(1U) == "settingsread";
#if defined(LITCH_ESP32S3)
        if (requestSettingsRead) {
            consolePtr->PrintLn(
                "settings_read_request=%s",
                sys.settingsPersistence.RequestSettingsReadDiagnostic()
                    ? "accepted" : "busy");
        }
#else
        if (requestSettingsRead) {
            consolePtr->PrintLn("settings_read_request=unsupported");
        }
#endif
        const auto line = modules::FormatSupportIncident(
            runtime::GetProgressSnapshot(), sys.settings.GetSaveCount(),
            sys.settings.GetErrorCount(), sys.bootStorageError);
        modules::WriteSupportIncident(*consolePtr, line);
        const auto& boot = firmware::PlatformBootTiming();
        consolePtr->PrintLn(
            "boot_platform: clock=firmware_ms unobserved=%lu app_main=%lu usb_begin=%lu usb_end=%lu arduino_begin=%lu arduino_end=%lu setup=%lu watchdog_end=%lu",
            static_cast<unsigned long>(firmware::BootPlatformTiming::UNOBSERVED_MS),
            static_cast<unsigned long>(boot.appMainBeginMs),
            static_cast<unsigned long>(boot.usbBeginMs),
            static_cast<unsigned long>(boot.usbEndMs),
            static_cast<unsigned long>(boot.arduinoBeginMs),
            static_cast<unsigned long>(boot.arduinoEndMs),
            static_cast<unsigned long>(boot.setupBeginMs),
            static_cast<unsigned long>(boot.watchdogEndMs));
        consolePtr->PrintLn(
            "boot_startup: power_to_light=unmeasured entry=%lu settings_attempted=%u settings_ms=%lu fade_started=%u fade_start=%lu fade_complete=%lu services_returned=%u presentation_complete=%lu",
            static_cast<unsigned long>(sys.startupEntryMs),
            static_cast<unsigned>(sys.settingsLoadAttempted),
            static_cast<unsigned long>(sys.settingsLoadAttempted
                ? sys.startupSettingsMs : firmware::BootPlatformTiming::UNOBSERVED_MS),
            static_cast<unsigned>(sys.startupLeds.Fade().IsStarted()),
            static_cast<unsigned long>(sys.startupLeds.Fade().IsStarted()
                ? sys.startupLeds.FadeStartedMs() : firmware::BootPlatformTiming::UNOBSERVED_MS),
            static_cast<unsigned long>(sys.startupLeds.IsReady()
                ? sys.startupLeds.FadeCompletedMs() : firmware::BootPlatformTiming::UNOBSERVED_MS),
            static_cast<unsigned>(sys.startupLeds.IsReady()),
            static_cast<unsigned long>(sys.runtimeBootHealth.startupCompletedAtMs));
        const auto& artwork = sys.gamePersistence.ArtPersistence();
        consolePtr->PrintLn(
            "litchcraft: active=%u dirty=%u saving=%u failed=%u recovered=%u flush_pending=%u session=%lu mode_gen=%lu",
            static_cast<unsigned>(sys.gamePersistence.Litchcraft().IsActive()),
            static_cast<unsigned>(artwork.IsDirty()),
            static_cast<unsigned>(artwork.IsSaving()),
            static_cast<unsigned>(artwork.HasFailure()),
            static_cast<unsigned>(sys.litchcraftStorageRecovered),
            static_cast<unsigned>(artwork.FlushState() == modules::LitchcraftPersistence::FlushStatus::Pending),
            static_cast<unsigned long>(sys.gamePersistence.Litchcraft().SessionId()),
            static_cast<unsigned long>(sys.gamePersistence.Litchcraft().ModeGeneration()));
        const auto io = sys.settingsPersistence.GetDiagnostics();
        const auto& mutation = sys.gamePersistence.Litchcraft().LastAdvancedMutation();
        consolePtr->PrintLn(
            "litchcraft_v2: mutations=%lu last_op=%u detail=%u revision=%lu frames=%u",
            static_cast<unsigned long>(mutation.count), static_cast<unsigned>(mutation.op),
            static_cast<unsigned>(mutation.detail), static_cast<unsigned long>(mutation.revision),
            static_cast<unsigned>(mutation.frames));
        const auto art = artwork.GetAdvancedDiagnostics();
        consolePtr->PrintLn(
            "litchcraft_art: revision=%lu frames=%u submitted_receipt=%lu submitted_rev=%lu submitted_crc=%08lx submitted_frames=%u saved_receipt=%lu saved_rev=%lu saved_crc=%08lx saved_frames=%u",
            static_cast<unsigned long>(art.revision), static_cast<unsigned>(art.frames),
            static_cast<unsigned long>(art.submittedReceipt), static_cast<unsigned long>(art.submittedRevision),
            static_cast<unsigned long>(art.submittedCrc), static_cast<unsigned>(art.submittedFrames),
            static_cast<unsigned long>(art.savedReceipt), static_cast<unsigned long>(art.savedRevision),
            static_cast<unsigned long>(art.savedCrc), static_cast<unsigned>(art.savedFrames));
#if defined(LITCH_ESP32S3)
        const auto& stream = sys.settingsPersistence.LitchcraftFrameStream();
        consolePtr->PrintLn(
            "litchcraft_store: generation=%lu frames=%u requested=%u displayed=%u presentations=%lu fingerprint=%08lx playing=%u",
            static_cast<unsigned long>(stream.Generation()),
            static_cast<unsigned>(stream.FrameCount()),
            static_cast<unsigned>(stream.RequestedIndex()),
            static_cast<unsigned>(stream.DisplayedIndex()),
            static_cast<unsigned long>(stream.PresentationCount()),
            static_cast<unsigned long>(stream.DisplayedFingerprint()),
            static_cast<unsigned>(
                sys.gamePersistence.Litchcraft().IsAnimationPlaying()));
#endif
        consolePtr->PrintLn(
            "settings_io: requested=%lu completed=%lu receipt=%lu outcome=%u settings_ms=%lu artwork_ms=%lu flash_accesses=%lu stack_free=%lu",
            static_cast<unsigned long>(io.requested), static_cast<unsigned long>(io.completed),
            static_cast<unsigned long>(io.receipt), static_cast<unsigned>(io.outcome),
            static_cast<unsigned long>(io.settingsMs), static_cast<unsigned long>(io.artworkMs),
            static_cast<unsigned long>(io.flashAccesses),
            static_cast<unsigned long>(io.stackFreeBytes));
        if (io.settingsOperationsAvailable) {
            const auto printOperation = [&](const char* label, const fxe::SettingsSaveOperation& operation) {
                consolePtr->PrintLn(
                    "settings_op: sample=%s worker=%lu save=%lu kind=%s path=%s to=%s bytes=%lu op_ms=%lu ok=%u errno=%d",
                    label, static_cast<unsigned long>(io.settingsOperationWorkerGeneration),
                    static_cast<unsigned long>(operation.saveGeneration),
                    fxe::SettingsSaveOperationKindToString(operation.kind),
                    fxe::SettingsSavePathRoleToString(operation.path),
                    fxe::SettingsSavePathRoleToString(operation.destination),
                    static_cast<unsigned long>(operation.requestedBytes),
                    static_cast<unsigned long>(operation.durationMs),
                    static_cast<unsigned>(operation.succeeded), operation.operationError);
            };
            printOperation("last", io.settingsLastOperation);
            printOperation("worst", io.settingsWorstOperation);
#if defined(LITCH_ESP32S3)
            const auto printPhysical = [&](const char* label, const fxe::SettingsSaveOperation& operation,
                                           const firmware::SettingsPhysicalAccessSample& sample) {
                const auto& read = sample.access[0U];
                const auto& write = sample.access[1U];
                const auto& erase = sample.access[2U];
                consolePtr->PrintLn(
                    "settings_physical: sample=%s worker=%lu save=%lu available=%u read=%lu/%lu/%lu/%lu write=%lu/%lu/%lu/%lu erase=%lu/%lu/%lu/%lu",
                    label, static_cast<unsigned long>(io.settingsOperationWorkerGeneration),
                    static_cast<unsigned long>(operation.saveGeneration), static_cast<unsigned>(sample.available),
                    static_cast<unsigned long>(read.calls), static_cast<unsigned long>(read.bytes),
                    static_cast<unsigned long>(read.totalMs), static_cast<unsigned long>(read.maxMs),
                    static_cast<unsigned long>(write.calls), static_cast<unsigned long>(write.bytes),
                    static_cast<unsigned long>(write.totalMs), static_cast<unsigned long>(write.maxMs),
                    static_cast<unsigned long>(erase.calls), static_cast<unsigned long>(erase.bytes),
                    static_cast<unsigned long>(erase.totalMs), static_cast<unsigned long>(erase.maxMs));
            };
            printPhysical("last", io.settingsLastOperation, io.settingsLastPhysicalAccess);
            printPhysical("worst", io.settingsWorstOperation, io.settingsWorstPhysicalAccess);
#endif
        } else {
            consolePtr->PrintLn("settings_op: unavailable=busy");
        }
        if (io.settingsOperationsAvailable) {
            const auto printTiming = [&](const char* kind,
                    const firmware::SettingsPersistenceWorker::PersistenceTiming& timing) {
                const auto& work = timing.flash.work;
                const auto& paging = timing.flash.paging;
                consolePtr->PrintLn(
                    "persist_timing: kind=%s available=%u seq=%lu gen=%lu ok=%u wall_ms=%lu fs=%lu/%lu paging=%lu/%lu outer=%lu/%lu page_outer=%lu/%lu inner=%lu/%lu page_inner=%lu/%lu",
                    kind, unsigned(timing.available), static_cast<unsigned long>(timing.sequence),
                    static_cast<unsigned long>(timing.generation), unsigned(timing.succeeded),
                    static_cast<unsigned long>(timing.elapsedMs),
                    static_cast<unsigned long>(timing.fileCalls), static_cast<unsigned long>(timing.fileMs),
                    static_cast<unsigned long>(timing.pagingCalls), static_cast<unsigned long>(timing.pagingMs),
                    static_cast<unsigned long>(work.outerCalls), static_cast<unsigned long>(work.outerWaitMs),
                    static_cast<unsigned long>(paging.outerCalls), static_cast<unsigned long>(paging.outerWaitMs),
                    static_cast<unsigned long>(work.handoffs), static_cast<unsigned long>(work.handoffWaitMs),
                    static_cast<unsigned long>(paging.handoffs), static_cast<unsigned long>(paging.handoffWaitMs));
                const auto printDriver = [&](const char* scope, const firmware::SettingsFlashTiming& flash) {
                    const auto& read = flash.physical[0U];
                    const auto& write = flash.physical[1U];
                    const auto& erase = flash.physical[2U];
                    consolePtr->PrintLn(
                        "persist_driver_us: kind=%s seq=%lu scope=%s available=%u read=%lu/%lu/%lu/%lu write=%lu/%lu/%lu/%lu erase=%lu/%lu/%lu/%lu",
                        kind, static_cast<unsigned long>(timing.sequence), scope, unsigned(timing.flash.available),
                        static_cast<unsigned long>(read.calls), static_cast<unsigned long>(read.bytes),
                        static_cast<unsigned long>(read.totalUs), static_cast<unsigned long>(read.maxUs),
                        static_cast<unsigned long>(write.calls), static_cast<unsigned long>(write.bytes),
                        static_cast<unsigned long>(write.totalUs), static_cast<unsigned long>(write.maxUs),
                        static_cast<unsigned long>(erase.calls), static_cast<unsigned long>(erase.bytes),
                        static_cast<unsigned long>(erase.totalUs), static_cast<unsigned long>(erase.maxUs));
                };
                if (timing.available) {
                    printDriver("work", work);
                    if (timing.pagingCalls != 0U) printDriver("paging", paging);
                }
            };
            printTiming("settings", io.settingsTiming);
        } else {
            consolePtr->PrintLn("persist_timing: unavailable=busy");
        }
        const auto flash = sys.rmtFlashArbiter.GetFlashDiagnostics();
        consolePtr->PrintLn(
            "flash_window: accesses=%lu last_hold_ms=%lu max_hold_ms=%lu",
            static_cast<unsigned long>(flash.accesses),
            static_cast<unsigned long>(flash.lastHoldMs),
            static_cast<unsigned long>(flash.maxHoldMs));
        const auto link = sys.ble.GetLinkObservation();
        const auto session = sys.ble.GetSessionDiagnostics();
        consolePtr->PrintLn(
            "ble_link: mtu=%u interval_units=%u latency=%u timeout_units=%u tx_phy=%u rx_phy=%u phy_known=%u dle_known=%u console_overflow_writes_total=%lu",
            link.mtu, link.intervalUnits, link.latency, link.timeoutUnits,
            link.txPhy, link.rxPhy, static_cast<unsigned>(link.phyKnown),
            static_cast<unsigned>(link.dataLengthKnown),
            static_cast<unsigned long>(session.consoleOutputOverflows));
        consolePtr->PrintLn(
            "ble_session: active=%u subscribed=%u state_token=%lu ingress_total=%lu received=%lu controls=%04x result=%u attempts=%lu failed=%lu last_fail_ms=%lu fail_kind=%u fail_code=%u sent_states=%08lx%08lx",
            static_cast<unsigned>(session.activated),
            static_cast<unsigned>(session.subscribed),
            static_cast<unsigned long>(session.stateStreamToken),
            static_cast<unsigned long>(sys.ble.SessionIngressCount()),
            static_cast<unsigned long>(session.received),
            static_cast<unsigned>(session.receivedControls),
            static_cast<unsigned>(session.lastIngressResult),
            static_cast<unsigned long>(session.notifyAttempts),
            static_cast<unsigned long>(session.notifyFailures),
            static_cast<unsigned long>(session.lastFailureMs),
            static_cast<unsigned>(session.lastFailureKind),
            static_cast<unsigned>(session.lastFailureCode),
            static_cast<unsigned long>(session.sentStateCodes >> 32U),
            static_cast<unsigned long>(session.sentStateCodes));
#if defined(LITCH_ESP32S3)
        modules::PrintSdkAttDiagnostics(*consolePtr);
        const char* const readKind = io.storageReadState >= 1U && io.storageReadState <= 4U
            ? "settings_primary" : io.storageReadState >= 9U ? "saved_frame"
            : io.storageReadState >= 5U ? "saved_body" : "none";
        const auto readState = io.storageReadState;
        const char* const readOutcome = !io.settingsOperationsAvailable ? "unavailable"
            : readState == 0U ? "idle" : readState == 1U ? "queued"
            : readState == 2U || readState == 6U || readState == 12U ? "reading"
            : readState == 3U || readState == 7U || readState == 9U ? "complete"
            : readState == 4U || readState == 8U || readState == 10U ? "failed"
            : readState == 5U || readState == 11U ? "pending" : "unknown";
        const bool readFileMetricsAvailable = io.settingsOperationsAvailable &&
            (readState == 7U || readState == 8U || readState == 9U || readState == 10U);
        consolePtr->PrintLn(
            "storage_read_sample: snapshot_available=%u kind=%s state=%u outcome=%s file_bytes=%lu local_ms=%lu file_metrics=%s file_ms=%lu hash=%08lx file_calls=%lu",
            unsigned(io.settingsOperationsAvailable), readKind,
            static_cast<unsigned>(io.storageReadState),
            readOutcome,
            static_cast<unsigned long>(io.storageReadBytes),
            static_cast<unsigned long>(io.storageReadWorkerMs),
            readFileMetricsAvailable ? "measured" : "unmeasured",
            static_cast<unsigned long>(io.storageReadFileMs),
            static_cast<unsigned long>(io.storageReadHash),
            static_cast<unsigned long>(io.storageReadCalls));
        consolePtr->PrintLn(
            "frame_store_timing: transfer_ms=%lu body_write_ms=%lu body_prepare_ms=%lu root_publish_ms=%lu body_bytes=%lu ingress_packets=%lu",
            static_cast<unsigned long>(io.frameStoreTransferMs),
            static_cast<unsigned long>(io.frameStoreBodyWriteMs),
            static_cast<unsigned long>(io.frameStoreBodyPrepareMs),
            static_cast<unsigned long>(io.frameStoreRootPublishMs),
            static_cast<unsigned long>(io.frameStoreBodyBytes),
            static_cast<unsigned long>(io.frameStoreIngressPackets));
#if defined(LITCH_HW_VARIANT_S3) && defined(ARDUINO_USB_MODE) && !(ARDUINO_USB_MODE)
        const auto usb = serialEndpoint.GetDiagnostics();
        consolePtr->PrintLn(
            "usb_console: ready=%u line=%u rx_bytes=%lu tx_accepted=%lu rx_queue_max=%lu last_rx_ms=%lu last_tx_ms=%lu",
            static_cast<unsigned>(usb.ready), static_cast<unsigned>(usb.lineState),
            static_cast<unsigned long>(usb.rxBytes),
            static_cast<unsigned long>(usb.txAcceptedBytes),
            static_cast<unsigned long>(usb.rxQueuedMax),
            static_cast<unsigned long>(usb.lastRxMs),
            static_cast<unsigned long>(usb.lastTxMs));
        consolePtr->PrintLn(
            "usb_console_wait: not_ready_samples=%lu not_ready_max_ms=%lu tx_zero_samples=%lu tx_zero_max_ms=%lu",
            static_cast<unsigned long>(usb.notReadySamples),
            static_cast<unsigned long>(usb.notReadyMaxMs),
            static_cast<unsigned long>(usb.txZeroSamples),
            static_cast<unsigned long>(usb.txZeroMaxMs));
        firmware::PrintUsbBootProbe(*consolePtr);
#endif
        consolePtr->PrintLn(
            "frame_store_recovery: status=%u runtime_ready=%u storage_active=%u playback_pending=%u mode_pending=%u mode_failed=%u ready_ms=%lu begin_ms=%lu mode_resolved_ms=%lu",
            static_cast<unsigned>(io.frameStoreRecovery),
            static_cast<unsigned>(io.frameStoreRuntimeReady),
            static_cast<unsigned>(io.frameStoreStorageActive),
            static_cast<unsigned>(io.frameStorePlaybackPending),
            static_cast<unsigned>(io.frameStoreModeSavePending),
            static_cast<unsigned>(io.frameStoreModeSaveFailed),
            static_cast<unsigned long>(io.frameStoreStartupReadyMs),
            static_cast<unsigned long>(io.frameStoreStartupBeginMs),
            static_cast<unsigned long>(io.frameStoreStartupModeResolvedMs));
#endif
        return true;
    });
    console.RegisterCommand("perf", "Show performance telemetry", [consolePtr](const Console::Args& args) {
        return console_commands::PerfCommand(*consolePtr, args);
    });
    console.RegisterCommand("heap", "Show heap usage", [consolePtr](const Console::Args& args) {
        return console_commands::HeapCommand(*consolePtr, args);
    });
    console.RegisterCommand(
        "engine",
        "Engine: engine fen | engine reset",
        [consolePtr](const Console::Args& args) { return console_commands::EngineCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::EngineComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });

    RegisterFeatureConsoleCommands(sys, console);

    static modules::TraceConsoleCommandContext traceCmdCtx;
    const char* deviceName =
#ifdef LITCH_ESP32S3
        "esp32s3";
#else
        "esp32c3";
#endif

#ifdef LITCH_TRACE
    modules::RegisterTraceConsoleCommands(console,
                                          sys.settings,
                                          sys.stateMachine,
                                          &sys.gamePersistence,
                                          &GetTraceRecorder(),
                                          "litch",
                                          deviceName,
                                          sys.rngSeed,
                                          traceCmdCtx);
#else
    modules::RegisterTraceConsoleCommands(console,
                                          sys.settings,
                                          sys.stateMachine,
                                          &sys.gamePersistence,
                                          nullptr,
                                          "litch",
                                          deviceName,
                                          sys.rngSeed,
                                          traceCmdCtx);
#endif

    console.RegisterCommand(
        "board",
        "Board: board status | resync | stress [seconds] [delay_ms] [ambient_0_or_1] | hallmap <start|stop|status|reset>",
        [consolePtr](const Console::Args& args) { return console_commands::BoardCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::BoardComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "ble",
        "BLE: ble <on|off|status> | ble test [duration_s] [delay_ms]",
        [consolePtr](const Console::Args& args) { return console_commands::BleCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::BleComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "input",
        "Input: scan (dump raw GPIO levels)",
        [consolePtr](const Console::Args& args) { return console_commands::InputCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::InputComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "comptest", "Compliance load: comptest <start|core|rail|led|ble|combined|status|stop> [value]",
        [consolePtr](const Console::Args& args) { return console_commands::ComptestCommand(*consolePtr, args); });
    console.RegisterCommand(
        "amptest",
        "Current test: amptest [start|status|stop|restart] | amptest verify [start|status|stop|restart]",
        [consolePtr](const Console::Args& args) { return console_commands::AmptestCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::AmptestComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "ledpower",
        "LED power: status|clear|latch",
        [consolePtr](const Console::Args& args) { return console_commands::LedPowerCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::LedPowerComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "cc",
        "USB-C source: cc <read|show|clear> | cc set <raw500> <raw1500> <raw3000>",
        [consolePtr](const Console::Args& args) { return console_commands::UsbCcCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::UsbCcComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "i2c",
        "I2C clock: i2c status | i2c set <100k|400k|1m|100000|400000|1000000> (1m experimental; saved)",
        [consolePtr](const Console::Args& args) { return console_commands::I2cCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::I2cComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "5ven",
        "Power rail: on|off (S3 only)",
        [consolePtr](const Console::Args& args) { return console_commands::FiveVEnCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::FiveVEnComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "setled",
        "LED diagnostic: setled board <index> <RRGGBB> | setled clear",
        [consolePtr](const Console::Args& args) { return console_commands::SetLedCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::SetLedComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand("rotate",
                            "Rotate the physical board mapping 180 degrees (starting position only)",
                            [consolePtr](const Console::Args& args) { return console_commands::FlipCommand(*consolePtr, args); });
    console.RegisterCommand("firstplayer",
                            "Player end: toggle P1 between physical ends (starting position only)",
                            [consolePtr](const Console::Args& args) { return console_commands::FirstPlayerCommand(*consolePtr, args); });
    console.RegisterCommand("flip",
                            "Alias: toggle P1 physical end (starting position only)",
                            [consolePtr](const Console::Args& args) { return console_commands::FirstPlayerCommand(*consolePtr, args); });
    console.RegisterCommand("p1black",
                            "Legacy alias: toggle P1 physical end (starting position only)",
                            [consolePtr](const Console::Args& args) { return console_commands::FirstPlayerCommand(*consolePtr, args); });
    console.RegisterCommand(
        "reboot",
#if defined(LITCH_ESP32S3)
        "Save and reboot (optional: nosave|bootloader)",
#else
        "Reboot device (optional: nosave)",
#endif
        [consolePtr](const Console::Args& args) { return console_commands::RebootCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::RebootComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "safemode",
        "Safe mode: status|clear (arm next reboot)|reboot",
        [consolePtr](const Console::Args& args) { return console_commands::SafeModeCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::SafeModeComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand("uptime", "Show elapsed time since boot", [consolePtr](const Console::Args& args) {
        if (args.argc != 1) {
            consolePtr->PrintLn("Usage: uptime");
            return false;
        }
        // Boot diagnostics need the full timer width beyond the millis rollover.
        const uint64_t uptimeMs = modules::SupportUptimeMs();
        const uint64_t uptimeSeconds = uptimeMs / 1000U;
        consolePtr->PrintLn(
            "uptime: %llu ms (%llu days %02u:%02u:%02u)",
            static_cast<unsigned long long>(uptimeMs),
            static_cast<unsigned long long>(uptimeSeconds / 86400U),
            static_cast<unsigned>((uptimeSeconds / 3600U) % 24U),
            static_cast<unsigned>((uptimeSeconds / 60U) % 60U),
            static_cast<unsigned>(uptimeSeconds % 60U));
        return true;
    });
    console.RegisterCommand("version",
                            "Show firmware version",
                            [consolePtr](const Console::Args& args) { return console_commands::VersionCommand(*consolePtr, args); });
    console.RegisterCommand("ver",
                            "Alias: version",
                            [consolePtr](const Console::Args& args) { return console_commands::VersionCommand(*consolePtr, args); });
    console.RegisterCommand(
        "sleep",
        "Sleep: enter [wake_ms]|status (S3 timer: 100..60000 ms; 0 disables)",
        [consolePtr](const Console::Args& args) { return console_commands::SleepCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::SleepComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });
    console.RegisterCommand(
        "ambient",
        "Ambient light sensors: ambient [status|on|off]",
        [consolePtr](const Console::Args& args) { return console_commands::AmbientCommand(*consolePtr, args); },
        [](const Console::Args& args,
           const size_t argIndex,
           const char* prefix,
           const char** outMatches,
           const size_t outMatchesCap) {
            return console_commands::AmbientComplete(args, argIndex, prefix, outMatches, outMatchesCap);
        });

#ifdef LITCH_TRACE
    console.SetCommandLineCallback(&ConsoleLineToTrace);
#endif
    console.SetEnteredLineCallback(&ConsoleEnteredLine);

    initialized = true;
}

void MaybeEnableConsoleOutput() {
    EnsureConsoleInitialized();

    if (IsConsoleOutputEnabled()) {
        auto& ctx = GetSystemContext();
        if (!ctx.console) {
            return;
        }
        DrainQueuedLoggerLines(*ctx.console);
        PrintLitchcraftClearResult(GetSystem(), *ctx.console);
        return;
    }

    auto& sys = GetSystem();
    const bool bleRequestsOutput = sys.ble.IsConnected();

#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT)
    if (!bleRequestsOutput && !static_cast<bool>(Serial) &&
        (Serial.available() <= 0)) {
        return;
    }
#else
    if (!bleRequestsOutput && Serial.available() <= 0) {
        return;
    }
#endif

    auto& ctx = GetSystemContext();
    if (!ctx.console) {
        return;
    }

    if (!g_consoleWelcomed) {
        ctx.console->PrintWelcome();

        auto& settings = *ctx.settings;
        if (!settings.IsDisabled() && settings.GetLastError() != modules::SettingsStoreError::None) {
            ctx.console->PrintLn("settings: load failed: %s (disabled=%u)",
                                 modules::SettingsStoreErrorToString(settings.GetLastError()),
                                 static_cast<unsigned>(settings.IsDisabled() ? 1U : 0U));
            ctx.console->PrintLn("settings: try 'settings status' or 'settings erase'/'settings clear' or 'settings format'");
        }

        logger::DumpBufferedLines(&PrintBufferedLoggerLine);
        g_consoleWelcomed = true;
    }
    // Publish the live callback handoff only after the retained boot dump.
    // Logger's mutex excludes task producers throughout that dump, avoiding
    // duplicate live and retained copies of the same line.
    MarkConsoleOutputEnabled();
    DrainQueuedLoggerLines(*ctx.console);
    PrintLitchcraftClearResult(sys, *ctx.console);
}

} // namespace firmware_internal

#endif
