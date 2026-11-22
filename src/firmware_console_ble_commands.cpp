#ifdef ARDUINO

#include <Arduino.h>
#include <ArduinoJson.h>

#if defined(LITCH_BLE)
#include <NimBLEDevice.h>
#endif

#include "firmware/firmware_console_internal.hpp"

#include "firmware/firmware_internal.hpp"

#include "foxie_embedded/elapsed_time.hpp"

#include "hardware/usb_cc_sense.hpp"

namespace {
bool IsAppliedOrNoop(const modules::ControlMutationOutcome& outcome) {
    return outcome.status == modules::ControlMutationStatus::Applied ||
           outcome.status == modules::ControlMutationStatus::Noop;
}

bool ReportMutationRejected(Console& console,
                            const char* command,
                            const modules::ControlMutationOutcome& outcome) {
    console.PrintLn("%s: blocked (%s)",
                    command,
                    modules::ControlMutationReasonToString(outcome.reason));
    return false;
}

} // namespace

namespace firmware_internal::console_commands {

bool BleCommand(Console& console, const Console::Args& args) {
    auto& sys = GetSystem();
    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view("status");

    if (sys.comptest.IsActive() && sub != "status") {
        console.PrintLn("ble: use comptest ble or stop comptest first"); return false;
    }
    if (!(sub == "on" || sub == "off" || sub == "status" || sub == "test")) {
        console.PrintLn("usage: ble [on|off|status|test]");
        return false;
    }

#if !defined(LITCH_BLE)
    (void)sys;
    console.PrintLn("ble: unsupported on this build");
    return false;
#else
    if (sub == "on") {
        StaticJsonDocument<16> value;
        value.set(true);
        const modules::ControlMutationOutcome outcome =
            sys.controlMutation.MutateSetting(
                "BLE_ENABLED", value.as<JsonVariantConst>());
        if (!IsAppliedOrNoop(outcome)) {
            return ReportMutationRejected(console, "ble", outcome);
        }

        sys.ble.Update(sys.settings);
        sys.ble.StartAdvertising();
        console.PrintLn("BLE advertising started");
        return true;
    }

    if (sub == "off") {
        StaticJsonDocument<16> value;
        value.set(false);
        const modules::ControlMutationOutcome outcome =
            sys.controlMutation.MutateSetting(
                "BLE_ENABLED", value.as<JsonVariantConst>());
        if (!IsAppliedOrNoop(outcome)) {
            return ReportMutationRejected(console, "ble", outcome);
        }

        sys.ble.Stop();
        console.PrintLn("BLE stopped");
        return true;
    }

    if (sub == "test") {
        if (!sys.ble.IsConnected()) {
            console.PrintLn("ble test: requires an active BLE connection");
            return false;
        }

        if (sys.ble.IsOtaInProgress()) {
            console.PrintLn("ble test: unavailable while OTA is in progress");
            return false;
        }

        uint16_t durationSeconds = 3;
        uint16_t interPacketDelayMs = 0;
        if (args.Has(2) && (!TryParseU16(args.Get(2), durationSeconds) || durationSeconds == 0U)) {
            console.PrintLn("usage: ble test [duration_s] [delay_ms]");
            return false;
        }
        if (args.Has(3) && !TryParseU16(args.Get(3), interPacketDelayMs)) {
            console.PrintLn("usage: ble test [duration_s] [delay_ms]");
            return false;
        }

        const uint16_t mtu = sys.ble.GetMtu();
        uint16_t maxPayload = 20U;
        if (mtu > 3U) {
            maxPayload = static_cast<uint16_t>(mtu - 3U);
        }
        if (maxPayload > 244U) {
            maxPayload = 244U;
        }

        static constexpr uint16_t SMALL_PACKET_SIZES[] = {1U, 2U, 4U, 8U, 12U, 16U, 20U};
        static constexpr uint16_t MEDIUM_PACKET_SIZES[] = {50U, 64U, 80U, 100U};
        static constexpr uint16_t TEST_BATCH_PACKETS = 200U;

        if (!modules::BleService::AreNotifyTestArgumentsValid(
                maxPayload, TEST_BATCH_PACKETS, interPacketDelayMs)) {
            console.PrintLn("ble test: delay exceeds the 10-second batch limit");
            return false;
        }

        console.PrintLn("ble test: mtu=%u max_payload=%u duration_s=%u delay_ms=%u",
                        static_cast<unsigned>(mtu),
                        static_cast<unsigned>(maxPayload),
                        static_cast<unsigned>(durationSeconds),
                        static_cast<unsigned>(interPacketDelayMs));

        struct BleTestGuard {
            explicit BleTestGuard(modules::BleService& bleService) : m_bleService(bleService) {
                m_bleService.SetBleLinkTestInProgress(true);
            }

            ~BleTestGuard() { m_bleService.SetBleLinkTestInProgress(false); }

          private:
            modules::BleService& m_bleService;
        };

        bool allPassed = true;
        bool interrupted = false;
        uint32_t totalBytesSent = 0U;
        uint32_t totalElapsedMs = 0U;

        struct Aggregate {
            uint16_t packetSize = 0;
            uint32_t elapsedMs = 0;
            uint32_t bytesSent = 0;
            uint32_t okCount = 0;
            uint32_t failCount = 0;
        };

        auto runOneSize = [&](const uint16_t requestedSize, const char* bucketLabel) {
            const uint16_t candidateSize = (requestedSize > maxPayload) ? maxPayload : requestedSize;
            Aggregate aggregate;
            aggregate.packetSize = candidateSize;

            fxe::ElapsedTime elapsed = fxe::ElapsedTime::StartNow();
            const uint32_t targetMs = static_cast<uint32_t>(durationSeconds) * 1000U;

            while (elapsed.ElapsedMs() < targetMs) {
                modules::BleLinkTestResult batch;
                const bool ran = sys.ble.RunConsoleNotifyTest(candidateSize,
                                                              TEST_BATCH_PACKETS,
                                                              interPacketDelayMs,
                                                              batch);
                aggregate.elapsedMs += batch.elapsedMs;
                aggregate.bytesSent += batch.bytesSent;
                aggregate.okCount += batch.okCount;
                aggregate.failCount += batch.failCount;

                if (!ran || !sys.ble.IsConnected()) {
                    interrupted = true;
                    allPassed = false;
                    break;
                }
            }

            const uint32_t elapsedMs = (aggregate.elapsedMs == 0U) ? 1U : aggregate.elapsedMs;
            const uint32_t bytesPerSec = static_cast<uint32_t>(
                (static_cast<uint64_t>(aggregate.bytesSent) * 1000U) / elapsedMs);
            const uint32_t bitsPerSec = bytesPerSec * 8U;
            const uint32_t avgPacketLatencyX100 =
                (aggregate.okCount == 0U) ? 0U : (elapsedMs * 100U) / aggregate.okCount;

            if (aggregate.okCount == 0U) {
                allPassed = false;
                console.PrintLn("ble test: bucket=%s size=%u no successful notifications",
                                bucketLabel,
                                static_cast<unsigned>(aggregate.packetSize));
                return;
            }

            if (aggregate.failCount > 0U) {
                allPassed = false;
            }

            totalBytesSent += aggregate.bytesSent;
            totalElapsedMs += elapsedMs;

            console.PrintLn("ble test: bucket=%s size=%u ok=%lu fail=%lu elapsed_ms=%lu avg_pkt_ms=%lu.%02lu rate_Bps=%lu rate_bps=%lu",
                            bucketLabel,
                            static_cast<unsigned>(aggregate.packetSize),
                            static_cast<unsigned long>(aggregate.okCount),
                            static_cast<unsigned long>(aggregate.failCount),
                            static_cast<unsigned long>(elapsedMs),
                            static_cast<unsigned long>(avgPacketLatencyX100 / 100U),
                            static_cast<unsigned long>(avgPacketLatencyX100 % 100U),
                            static_cast<unsigned long>(bytesPerSec),
                            static_cast<unsigned long>(bitsPerSec));
        };

        {
            // A notification batch can block the main loop for seconds.
            // Shed above-default LED load before normal CC polling pauses.
            ResetUsbCcSourceAuthorization(sys);
            BleTestGuard testGuard(sys.ble);

            for (const uint16_t size : SMALL_PACKET_SIZES) {
                runOneSize(size, "small");
                if (interrupted || !sys.ble.IsConnected()) {
                    interrupted = true;
                    allPassed = false;
                    break;
                }
            }

            if (!interrupted && sys.ble.IsConnected()) {
                for (const uint16_t size : MEDIUM_PACKET_SIZES) {
                    if (size > maxPayload) {
                        continue;
                    }
                    runOneSize(size, "medium");
                    if (interrupted || !sys.ble.IsConnected()) {
                        interrupted = true;
                        allPassed = false;
                        break;
                    }
                }
            }

            if (!interrupted && sys.ble.IsConnected()) {
                runOneSize(maxPayload, "large");
            }
        }

        if (interrupted) {
            console.PrintLn("ble test: stopped (connection changed or batch limit reached)");
        }

        if (totalElapsedMs > 0U) {
            const uint32_t totalBytesPerSec = static_cast<uint32_t>(
                (static_cast<uint64_t>(totalBytesSent) * 1000U) / totalElapsedMs);
            console.PrintLn("ble test: aggregate bytes=%lu elapsed_ms=%lu aggregate_Bps=%lu",
                            static_cast<unsigned long>(totalBytesSent),
                            static_cast<unsigned long>(totalElapsedMs),
                            static_cast<unsigned long>(totalBytesPerSec));
        }

        return allPassed;
    }

    const bool enabled = sys.ble.IsEnabled();
    const bool connected = sys.ble.IsConnected();
    const bool advertising = sys.ble.IsAdvertising();
    const char* state = connected ? "connected" : (advertising ? "advertising" : "idle");

    const uint16_t mtu = sys.ble.GetMtu();
    const uint8_t connectionCount = sys.ble.GetConnectionCount();
    const char* peerAddress = sys.ble.GetPeerAddress();

    console.PrintLn("ble: enabled=%u state=%s name=%s mtu=%u connections=%u",
                    static_cast<unsigned>(enabled ? 1U : 0U),
                    state,
                    sys.ble.GetDeviceName(),
                    static_cast<unsigned>(mtu),
                    static_cast<unsigned>(connectionCount));
    if (connected && peerAddress != nullptr && peerAddress[0] != '\0') {
        console.PrintLn("ble: peer=%s", peerAddress);
    } else {
        console.PrintLn("ble: peer=(none)");
    }
    const auto store = sys.ble.ReadOwnerStoreDiagnostics();
    if (store.available) {
        console.PrintLn("ble_store: available=1 saved_peers=%u rpas=%u orphan_rpas=%u",
                        unsigned(store.savedPeers), unsigned(store.rpas), unsigned(store.orphanRpas));
    } else {
        console.PrintLn("ble_store: available=0");
    }

    return true;
#endif
}

bool UsbCcCommand(Console& console, const Console::Args& args) {
    if (!hardware::HW_IS_S3) {
        console.PrintLn("cc: unsupported on this hardware");
        return false;
    }

    const std::string_view sub = args.Has(1) ? args.Get(1) : std::string_view();
    if (!sub.empty() && sub != "read" && sub != "show" && sub != "set" && sub != "clear") {
        console.PrintLn("usage: cc [read|show|set <500> <1500> <3000>|clear]");
        return false;
    }

    auto& sys = GetSystem();

    if (sub == "set") {
        if (args.argc != 5) {
            console.PrintLn("usage: cc set <raw500> <raw1500> <raw3000>");
            return false;
        }

        uint16_t t500 = 0;
        uint16_t t1500 = 0;
        uint16_t t3000 = 0;
        if (!TryParseU16(args.Get(2), t500) || !TryParseU16(args.Get(3), t1500) || !TryParseU16(args.Get(4), t3000)) {
            console.PrintLn("cc: invalid thresholds");
            return false;
        }

        const modules::ControlMutationOutcome outcome =
            sys.controlMutation.MutateUsbCcThresholds(t500, t1500, t3000);
        if (!IsAppliedOrNoop(outcome)) {
            return ReportMutationRejected(console, "cc", outcome);
        }
        if (outcome.status == modules::ControlMutationStatus::Noop) {
            console.PrintLn("cc: thresholds unchanged");
            return true;
        }

        console.PrintLn("cc: thresholds set; source confirmation reset");
        return true;
    }

    if (sub == "clear") {
        const modules::ControlMutationOutcome outcome =
            sys.controlMutation.ResetUsbCcThresholds();
        if (!IsAppliedOrNoop(outcome)) {
            return ReportMutationRejected(console, "cc", outcome);
        }
        if (outcome.status == modules::ControlMutationStatus::Noop) {
            console.PrintLn("cc: thresholds already default");
            return true;
        }

        console.PrintLn("cc: thresholds cleared; source confirmation reset");
        return true;
    }

    // This detailed diagnostic pauses the cooperative monitor while it reads
    // both ADC channels and prints. Keep the physical output conservative.
    ResetUsbCcSourceAuthorization(sys);
    const hardware::UsbCcReadings readings = hardware::ReadUsbCcPins();
    const hardware::UsbCcThresholds thresholds = LoadUsbCcThresholds(sys.settings);
    const hardware::UsbCcLevel advertised =
        hardware::ClassifyUsbCcReadings(readings.cc1, readings.cc2, thresholds);
    const bool entryCandidate =
        hardware::UsbCcReadingsMeetStrongConfirmationThreshold(
            readings.cc1, readings.cc2, thresholds);

    console.PrintLn("cc: raw1=%u range1=%u..%u prime1=%u "
                    "raw2=%u range2=%u..%u prime2=%u",
                    static_cast<unsigned>(readings.cc1),
                    static_cast<unsigned>(readings.cc1Minimum),
                    static_cast<unsigned>(readings.cc1Maximum),
                    static_cast<unsigned>(readings.cc1Priming),
                    static_cast<unsigned>(readings.cc2),
                    static_cast<unsigned>(readings.cc2Minimum),
                    static_cast<unsigned>(readings.cc2Maximum),
                    static_cast<unsigned>(readings.cc2Priming));
    console.PrintLn("cc: thresh_500=%u thresh_1500=%u thresh_3000=%u",
                    static_cast<unsigned>(thresholds.level500),
                    static_cast<unsigned>(thresholds.level1500),
                    static_cast<unsigned>(thresholds.level3000));
    console.PrintLn("cc: level1=%s level2=%s",
                    hardware::UsbCcLevelToString(hardware::ClassifyUsbCcRaw(readings.cc1, thresholds)),
                    hardware::UsbCcLevelToString(hardware::ClassifyUsbCcRaw(readings.cc2, thresholds)));
    console.PrintLn("cc: advertised=%s strong_enter_raw=%u entry_candidate=%u",
                    hardware::UsbCcLevelToString(advertised),
                    static_cast<unsigned>(
                        hardware::GetUsbCcStrongConfirmationRawThreshold(
                            thresholds)),
                    static_cast<unsigned>(entryCandidate ? 1U : 0U));
    return true;
}

} // namespace firmware_internal::console_commands

#endif
