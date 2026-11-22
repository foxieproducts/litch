#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "hardware/lis2dw12.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

struct FakeBus {
    std::array<uint8_t, 256> registers{};
    uint8_t failWriteRegister = 0xFFU;
    uint32_t delayedMs = 0U;
    uint32_t readCount = 0U;
    uint32_t writeCount = 0U;

    static bool Read(void* context,
                     const uint8_t address,
                     const uint8_t reg,
                     uint8_t* data,
                     const size_t length) {
        auto& self = *static_cast<FakeBus*>(context);
        if (address != hardware::Lis2dw12::I2C_ADDRESS || data == nullptr) {
            return false;
        }
        ++self.readCount;
        for (size_t i = 0; i < length; ++i) {
            data[i] = self.registers[static_cast<uint8_t>(reg + i)];
        }
        return true;
    }

    static bool Write(void* context,
                      const uint8_t address,
                      const uint8_t reg,
                      const uint8_t* data,
                      const size_t length) {
        auto& self = *static_cast<FakeBus*>(context);
        if (address != hardware::Lis2dw12::I2C_ADDRESS || data == nullptr ||
            reg == self.failWriteRegister) {
            return false;
        }
        ++self.writeCount;
        for (size_t i = 0; i < length; ++i) {
            self.registers[static_cast<uint8_t>(reg + i)] = data[i];
        }
        return true;
    }

    static void Delay(void* context, const uint32_t milliseconds) {
        static_cast<FakeBus*>(context)->delayedMs += milliseconds;
    }

    hardware::Lis2dw12Transport Transport() {
        return {this, &Read, &Write, &Delay};
    }
};

void CheckMotionConfiguration() {
    FakeBus bus;
    bus.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;

    hardware::Lis2dw12 sensor;
    hardware::Lis2dw12Config config;
    Check(sensor.Begin(bus.Transport(), config),
          "motion: WHO_AM_I device configures successfully");
    Check(sensor.IsAvailable(), "motion: successful begin marks device available");
    Check(bus.registers[hardware::lis2dw12_register::CTRL1] == 0x30U,
          "motion: default is 25 Hz low-power mode 1");
    Check(bus.registers[hardware::lis2dw12_register::CTRL2] == 0x0CU,
          "motion: BDU and auto-increment remain enabled after reset");
    Check(bus.registers[hardware::lis2dw12_register::CTRL3] == 0x18U,
          "motion: INT is push-pull active-low and latched");
    Check(bus.registers[hardware::lis2dw12_register::CTRL4_INT1] == 0x20U,
          "motion: wake-up is routed to INT1");
    Check(bus.registers[hardware::lis2dw12_register::WAKE_UP_THS] == 0x04U,
          "motion: 125 mg threshold quantizes to four FS/64 steps");
    Check(bus.registers[hardware::lis2dw12_register::WAKE_UP_DUR] == 0x40U,
          "motion: 80 ms duration quantizes to two 25 Hz samples");
    Check(bus.registers[hardware::lis2dw12_register::CTRL7] == 0x20U,
          "motion: embedded interrupts are enabled last");

    const auto& applied = sensor.GetAppliedConfig();
    Check(applied.odrTenthsHz == 250U &&
              applied.motionThresholdMg == 125U &&
              applied.motionDurationMs == 80U,
          "motion: applied diagnostics preserve 25 Hz and the requested 80 ms duration");
    Check(!applied.tapExperimental,
          "motion: default mode does not claim experimental tap support");
}

void CheckTapConfiguration() {
    FakeBus bus;
    bus.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;

    hardware::Lis2dw12Config config;
    config.mode = hardware::Lis2dw12WakeMode::MotionAndTap;
    config.tapThresholdMg = 563U;
    config.tapShockMs = 40U;

    hardware::Lis2dw12 sensor;
    Check(sensor.Begin(bus.Transport(), config),
          "tap: combined motion/tap configuration succeeds");
    Check(bus.registers[hardware::lis2dw12_register::CTRL1] == 0x74U,
          "tap: valid tap mode uses 400 Hz high-performance ODR");
    Check(bus.registers[hardware::lis2dw12_register::CTRL6] == 0x04U,
          "tap: low-noise stays enabled at plus/minus 2 g");
    Check(bus.registers[hardware::lis2dw12_register::TAP_THS_X] == 0x09U &&
              bus.registers[hardware::lis2dw12_register::TAP_THS_Y] == 0x09U &&
              bus.registers[hardware::lis2dw12_register::TAP_THS_Z] == 0xE9U,
          "tap: recommended 563 mg threshold applies to all three axes");
    Check(bus.registers[hardware::lis2dw12_register::INT_DUR] == 0x02U,
          "tap: 40 ms shock window quantizes to ST recommended value two");
    Check(bus.registers[hardware::lis2dw12_register::CTRL4_INT1] == 0x60U,
          "tap: motion and single tap both route to INT1");
    Check(sensor.GetAppliedConfig().tapExperimental,
          "tap: diagnostics identify the uncharacterized high-ODR mode");
}

void CheckEventAndSampleDiagnostics() {
    FakeBus bus;
    bus.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    hardware::Lis2dw12 sensor;
    Check(sensor.Begin(bus.Transport(), {}), "diagnostics: begin succeeds");

    bus.registers[hardware::lis2dw12_register::WAKE_UP_SRC] = 0x0DU;
    bus.registers[hardware::lis2dw12_register::TAP_SRC] = 0x65U;
    bus.registers[hardware::lis2dw12_register::ALL_INT_SRC] = 0x06U;
    hardware::Lis2dw12Event event;
    Check(sensor.ReadAndClearEvents(event), "diagnostics: event registers read");
    Check(event.wakeUp && event.wakeX && event.wakeZ,
          "diagnostics: wake event axes decode");
    Check(event.singleTap && event.tapX && event.tapZ,
          "diagnostics: tap event axes decode");
    Check(event.rawAll == 0x06U,
          "diagnostics: ALL_INT_SRC is retained for bring-up output");

    const int16_t rawX = 0x4000;
    const int16_t rawY = static_cast<int16_t>(0xC000);
    const int16_t rawZ = 0x0000;
    const int16_t rawValues[] = {rawX, rawY, rawZ};
    for (size_t axis = 0; axis < 3U; ++axis) {
        const uint16_t bits = static_cast<uint16_t>(rawValues[axis]);
        bus.registers[hardware::lis2dw12_register::OUT_X_L + axis * 2U] =
            static_cast<uint8_t>(bits & 0xFFU);
        bus.registers[hardware::lis2dw12_register::OUT_X_L + axis * 2U + 1U] =
            static_cast<uint8_t>(bits >> 8U);
    }

    hardware::Lis2dw12Sample sample;
    Check(sensor.ReadSample(sample), "diagnostics: coherent axis sample reads");
    Check(sample.rawX == rawX && sample.rawY == rawY && sample.rawZ == rawZ,
          "diagnostics: signed raw axes decode little-endian");
    Check(sample.milliGx == 999 && sample.milliGy == -999,
          "diagnostics: LP1 plus/minus 2 g conversion is deterministic");
}

void CheckFailuresStayFailSafe() {
    FakeBus wrongId;
    wrongId.registers[hardware::lis2dw12_register::WHO_AM_I] = 0x00U;
    hardware::Lis2dw12 sensor;
    Check(!sensor.Begin(wrongId.Transport(), {}),
          "failure: wrong WHO_AM_I rejects the device");
    Check(!sensor.IsAvailable() &&
              sensor.GetLastError() == hardware::Lis2dw12Error::WrongDeviceId,
          "failure: wrong device stays unavailable with diagnostic reason");

    FakeBus writeFailure;
    writeFailure.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    writeFailure.failWriteRegister = hardware::lis2dw12_register::WAKE_UP_THS;
    Check(!sensor.Begin(writeFailure.Transport(), {}),
          "failure: partial register programming is rejected");
    Check(!sensor.IsAvailable() &&
              sensor.GetLastError() == hardware::Lis2dw12Error::Transport,
          "failure: failed configuration never advertises wake readiness");

    FakeBus disabled;
    disabled.registers[hardware::lis2dw12_register::WHO_AM_I] =
        hardware::Lis2dw12::EXPECTED_DEVICE_ID;
    hardware::Lis2dw12Config off;
    off.enabled = false;
    Check(sensor.Begin(disabled.Transport(), off),
          "failure: explicitly disabled sensor is a valid powered-down state");
    Check(disabled.registers[hardware::lis2dw12_register::CTRL1] == 0U &&
              !sensor.IsWakeArmed(),
          "failure: disabled state powers down and never claims wake arming");
}

void CheckInteractiveSampling() {
    FakeBus bus;
    bus.registers[hardware::lis2dw12_register::WHO_AM_I] = 0x44U;
    hardware::Lis2dw12 sensor;
    Check(sensor.Begin(bus.Transport(), {}), "interactive fixture starts motion profile");
    const auto motionRegisters = bus.registers;
    const uint32_t delayBefore = bus.delayedMs;
    const uint32_t writesBefore = bus.writeCount;
    Check(sensor.SetInteractiveSampling(true) &&
              bus.registers[hardware::lis2dw12_register::CTRL1] == 0x30U &&
              sensor.GetAppliedConfig().odrTenthsHz == 250U &&
              sensor.GetAppliedConfig().motionDurationMs == 80U,
          "active water retains 25 Hz low-power acquisition and 80 ms wake duration");
    Check(bus.delayedMs == delayBefore && bus.writeCount == writesBefore &&
              bus.registers == motionRegisters,
          "entering water neither blocks nor rewrites the already matching motion profile");
    Check(sensor.SetInteractiveSampling(true) && bus.writeCount == writesBefore,
          "repeated active handling does not churn sensor registers");
    Check(sensor.SetInteractiveSampling(false) &&
              sensor.GetAppliedConfig().odrTenthsHz == 250U &&
              sensor.GetAppliedConfig().motionDurationMs == 80U &&
              bus.writeCount == writesBefore && bus.registers == motionRegisters,
          "exiting water preserves exact wake registers without a sensor write");
    hardware::Lis2dw12Config tap;
    tap.mode = hardware::Lis2dw12WakeMode::MotionAndTap;
    sensor.ApplyConfig(tap);
    const uint32_t tapWrites = bus.writeCount;
    Check(sensor.SetInteractiveSampling(true) && sensor.SetInteractiveSampling(false) &&
              sensor.GetAppliedConfig().odrTenthsHz == 4000U &&
              bus.writeCount == tapWrites,
          "interactive override cannot change the existing 400 Hz tap profile");
    sensor.ApplyConfig({});
    sensor.SetInteractiveSampling(true);
    const uint32_t sleepWrites = bus.writeCount;
    bus.registers[hardware::lis2dw12_register::CTRL1] = 0U;
    bus.registers[hardware::lis2dw12_register::WAKE_UP_THS] = 0U;
    Check(sensor.ArmForSleep({}) && sensor.IsWakeArmed() &&
              sensor.GetAppliedConfig().odrTenthsHz == 250U &&
              bus.writeCount > sleepWrites && bus.registers == motionRegisters,
          "sleep fully reapplies the persisted wake profile even after write-free water");
    bus.failWriteRegister = hardware::lis2dw12_register::CTRL1;
    Check(sensor.SetInteractiveSampling(true),
          "matching active rate needs no attempted register reconfiguration");
    Check(!sensor.ArmForSleep({}) && !sensor.IsAvailable() &&
              !sensor.IsWakeArmed(),
          "failed full sleep reconfiguration still fails closed without wake readiness");
}

} // namespace

int main() {
    std::puts("[LIS2DW12_DRIVER] start");
    CheckMotionConfiguration();
    CheckTapConfiguration();
    CheckEventAndSampleDiagnostics();
    CheckFailuresStayFailSafe();
    CheckInteractiveSampling();

    if (g_failures == 0) {
        std::puts("[LIS2DW12_DRIVER] ok");
        return 0;
    }
    std::printf("[LIS2DW12_DRIVER] failures=%d\n", g_failures);
    return 1;
}
