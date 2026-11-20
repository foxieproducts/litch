#pragma once

#include <array>
#include <cstdint>

#include "hardware/board_hardware_profile.hpp"

namespace firmware {

struct SleepWakePinSet {
    static constexpr uint8_t CAPACITY = 15U;

    constexpr bool Add(const uint8_t pin) {
        if (pin == hardware::NO_GPIO || Contains(pin)) {
            return true;
        }
        if (count >= pins.size()) {
            return false;
        }
        pins[count++] = pin;
        return true;
    }

    constexpr bool Contains(const uint8_t pin) const {
        for (uint8_t index = 0U; index < count; ++index) {
            if (pins[index] == pin) {
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t Mask() const {
        uint64_t mask = 0U;
        for (uint8_t index = 0U; index < count; ++index) {
            if (pins[index] < 64U) {
                mask |= (1ULL << pins[index]);
            }
        }
        return mask;
    }

    std::array<uint8_t, CAPACITY> pins{};
    uint8_t count = 0U;
};

constexpr void AddDpadPins(SleepWakePinSet& pins,
                           const hardware::PlayerControlPinProfile& player) {
    (void)pins.Add(player.up);
    (void)pins.Add(player.down);
    (void)pins.Add(player.left);
    (void)pins.Add(player.right);
}

constexpr SleepWakePinSet BuildRuntimeButtonPinSet(
    const hardware::BoardHardwareProfile& profile) {
    SleepWakePinSet pins;
    AddDpadPins(pins, profile.p1);
    (void)pins.Add(profile.p1.x);
    (void)pins.Add(profile.p1.o);
    if (profile.hasP2Controls) {
        AddDpadPins(pins, profile.p2);
        (void)pins.Add(profile.p2.x);
        (void)pins.Add(profile.p2.o);
    }
    return pins;
}

/**
 * \brief Builds the active-low sleep wake set for a resolved board profile.
 *
 * By default V1 and V2 retain their four-direction-only sets. V3 enables all
 * controls except active-low P1-X on GPIO0. The includeP1X flag is used for
 * runtime pad restoration only: S3 V2/V3 wake P1-X through the release-gated
 * ULP path, never through EXT1. C3 always ignores it.
 */
constexpr SleepWakePinSet BuildDeepSleepWakePinSet(
    const hardware::BoardHardwareProfile& profile,
    const bool includeMotion,
    const bool includeP1X = false) {
    SleepWakePinSet pins;
    AddDpadPins(pins, profile.p1);
    if (profile.hasP2Controls) {
        AddDpadPins(pins, profile.p2);
    }

    if (profile.isS3 && includeP1X) {
        (void)pins.Add(profile.p1.x);
    }
    if (hardware::HasFourLedSquares(profile.revision)) {
        (void)pins.Add(profile.p1.o);
        (void)pins.Add(profile.p2.x);
        (void)pins.Add(profile.p2.o);
        if (includeMotion) {
            (void)pins.Add(profile.accelerometerInterrupt);
        }
    }
    return pins;
}

} // namespace firmware
