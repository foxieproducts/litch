#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <PCA95x5.h>
#include <Wire.h>

#include "esp_attr.h"
#include "foxie_embedded/millis.hpp"
#include "logger.hpp"

// Compile the shipping Arduino scanner against the same Wire boundary used
// by the vendor transport checks. Rename only to avoid the linked native owner.
#define ARDUINO 1
#define Pca9555InputScan ArduinoPca9555InputScan
#include "../../src/pca9555_input_scan.cpp"
#undef Pca9555InputScan
#undef ARDUINO

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void CheckConfigurationRecovery(const uint8_t failedRegister,
                                const uint8_t failedExpander) {
    TwoWire wire;
    wire.modelExpanders = true;
    wire.failingRegister = failedRegister;
    wire.failingExpander = failedExpander;
    wire.configurationFailures = UINT32_MAX;
    wire.electricalInput[3U] = 0xFFFEU;
    hardware::ArduinoPca9555InputScan scan(wire);
    scan.ConfigureHardwareProfile(
        hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3));
    scan.Begin();
    Check(!scan.LastReadOk() && scan.GetUpdateCounter() == 0U && scan.GetOcc64() == 0ULL,
          "failed boot configuration cannot publish a successful initial scan");
    for (uint8_t index = 0U; index < 4U; ++index) {
        Check(wire.directionWrites[index] != 0U && wire.polarityWrites[index] != 0U,
              "configuration attempts every expander after an earlier failure");
    }

    // Reads themselves work: a successful bus transaction must not establish
    // the polarity/configuration evidence that the scanner is missing.
    PCA9555 device;
    device.attach(wire, static_cast<uint8_t>(0x20U + failedExpander));
    uint16_t untrusted = 0U;
    Check(device.read(untrusted), "reads succeed despite failed configuration writes");
    const auto writesBefore = wire.polarityWrites;
    const auto failed = scan.AttemptScan();
    Check(failed.status == modules::BoardScanAttemptStatus::Failed &&
              failed.rawOccupiedMask == 0ULL && !scan.LastReadOk() &&
              scan.GetUpdateCounter() == 0U && scan.GetOcc64() == 0ULL,
          "successful reads cannot make unconfigured occupancy authoritative");
    for (uint8_t index = 0U; index < 4U; ++index) {
        Check(wire.polarityWrites[index] - writesBefore[index] <= 1U,
              "one scan attempt performs at most one complete configuration pass");
    }

    wire.configurationFailures = 0U;
    const auto recovered = scan.AttemptScan();
    const auto expected = hardware::DecodePca9555Occupancy64(0U, 0U, 0U, 1U,
        hardware::GetExpanderPinToSquareMap(hardware::BoardRevision::V3_3S3));
    Check(recovered.status == modules::BoardScanAttemptStatus::Succeeded &&
              recovered.rawOccupiedMask == expected && scan.GetOcc64() == expected &&
              scan.LastReadOk() && scan.LastRespondingExpanderMask() == 0x0FU &&
              scan.GetUpdateCounter() == 1U,
          "verified reconfiguration recovers one fresh correctly normalized scan");
}

void CheckBusReinitializationFailurePreservesOccupancy() {
    TwoWire wire;
    wire.modelExpanders = true;
    wire.electricalInput[0U] = 0xFFFEU;
    hardware::ArduinoPca9555InputScan scan(wire);
    scan.ConfigureHardwareProfile(
        hardware::BoardHardwareProfileFor(hardware::BoardRevision::V3_3S3));
    scan.Begin();
    Check(scan.LastReadOk(), "runtime recovery fixture starts with valid configuration");
    const auto retained = scan.GetOcc64();
    const auto updates = scan.GetUpdateCounter();
    wire.electricalInput[0U] = 0xFFFFU;
    wire.electricalInput[3U] = 0xFFFEU;
    wire.readFailures[0U] = 2U;
    wire.failingExpander = 1U;
    wire.configurationFailures = UINT32_MAX;
    wire.failOnlyAfterBusReset = true;
    const auto failed = scan.AttemptScan();
    Check(wire.busResetCount == 1U, "bounded read retries use the production bus reset");
    Check(failed.status == modules::BoardScanAttemptStatus::Failed &&
              !scan.LastReadOk() && scan.GetOcc64() == retained &&
              scan.GetUpdateCounter() == updates,
          "failed bus reinitialization retains the last validated occupancy");
    Check(scan.AttemptScan().status == modules::BoardScanAttemptStatus::Failed &&
              scan.GetOcc64() == retained,
          "configuration failure remains failed after ordinary reads recover");
    wire.configurationFailures = 0U;
    const auto recovered = scan.AttemptScan();
    Check(recovered.status == modules::BoardScanAttemptStatus::Succeeded &&
              scan.GetOcc64() != retained && scan.GetUpdateCounter() == updates + 1U,
          "successful reconfiguration resumes fresh observations without stale publication");
}

} // namespace

int main() {
    std::puts("[PCA95X5_TRANSPORT] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    TwoWire wire;
    PCA9555 device;
    device.attach(wire, 0x20U);

    uint16_t value = 0U;
    Check(device.read(value), "complete two-byte transaction succeeds");
    Check(value == 0x1234U, "complete transaction decodes both bytes");
    Check(device.i2c_error() == 0U, "successful read clears status");

    wire.txStatus = 4U;
    value = 0xA55AU;
    Check(!device.read(value), "register-address transmission failure is reported");
    Check(value == 0xA55AU, "failed transmission does not mutate caller value");
    Check(device.i2c_error() == 4U, "transmission status is retained");

    wire.txStatus = 0U;
    wire.requestedBytes = 1U;
    wire.availableBytes = 1U;
    value = 0x5AA5U;
    Check(!device.read(value), "short request is reported");
    Check(value == 0x5AA5U, "short request does not publish partial input");
    Check(device.i2c_error() != 0U, "short request sets an error status");

    for (const uint8_t reg : {uint8_t{4U}, uint8_t{6U}}) {
        for (uint8_t expander = 0U; expander < 4U; ++expander)
            CheckConfigurationRecovery(reg, expander);
    }
    CheckBusReinitializationFailurePreservesOccupancy();

    if (g_failures == 0) {
        std::puts("[PCA95X5_TRANSPORT] ok");
        return 0;
    }
    std::printf("[PCA95X5_TRANSPORT] failures=%d\n", g_failures);
    return 1;
}
