#include <array>
#include <cstdio>

#include "hardware/hardware_test_boot_entry.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[HARDWARE_TEST_BOOT_ENTRY] start");

    Check(hardware::ShouldEnterHardwareTestFromBoot(
              hardware::BoardRevision::V3S3, true, true, false),
          "P1 Up enters production test on V3");
    Check(hardware::ShouldEnterHardwareTestFromBoot(
              hardware::BoardRevision::V3S3, true, false, true),
          "P2 Up enters production test on V3");
    Check(!hardware::ShouldEnterHardwareTestFromBoot(
              hardware::BoardRevision::V3S3, true, false, false),
          "neutral V3 controls keep normal boot");
    for (const auto unsupported : std::array{
             hardware::BoardRevision::Unknown,
             hardware::BoardRevision::V1C3,
         }) {
        Check(!hardware::SupportsProductionHardwareTest(unsupported) &&
                  !hardware::ShouldEnterHardwareTestFromBoot(
                      unsupported, true, true, true),
              "Unknown/V1 cannot enter production test from held controls");
    }
    Check(hardware::ShouldEnterHardwareTestFromBoot(hardware::BoardRevision::V2S3, true, true, false), "explicitly supported V2 Up admits factory software journey");
    Check(hardware::SupportsProductionHardwareTest(
              hardware::BoardRevision::V3S3),
          "defined V3 production profile is admitted");

    if (g_failures == 0) {
        std::puts("[HARDWARE_TEST_BOOT_ENTRY] ok");
    }
    return g_failures == 0 ? 0 : 1;
}
