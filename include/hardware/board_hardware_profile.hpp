#pragma once

#include <array>
#include <cstdint>

#include "hardware/board_led_output_mapping.hpp"
#include "hardware/board_revision.hpp"

namespace hardware {

constexpr uint8_t NO_GPIO = 0xFFU;

// The center contacts on V2/V3 are unused; V3.3+ has no center switch.
struct PlayerControlPinProfile {
    uint8_t up = NO_GPIO;
    uint8_t left = NO_GPIO;
    uint8_t down = NO_GPIO;
    uint8_t right = NO_GPIO;
    uint8_t x = NO_GPIO;
    uint8_t o = NO_GPIO;
};

struct S3RevisionPinProfile {
    uint8_t fiveVoltEnable = NO_GPIO;
    PlayerControlPinProfile p2{};
    std::array<uint8_t, 4> expanderInterrupts{
        NO_GPIO,
        NO_GPIO,
        NO_GPIO,
        NO_GPIO,
    };
    uint8_t accelerometerInterrupt = NO_GPIO;
};

struct BoardHardwareProfile {
    BoardRevision revision = BoardRevision::V2S3;
    bool isS3 = true;
    bool hasP2Controls = true;
    bool hasExpanderInterrupts = true;
    bool hasFiveVoltEnable = true;
    bool hasLightSensors = true;
    bool hasCustomI2cPins = true;
    bool supportsBleOta = true;
    uint8_t boardLedPin = 47U;
    uint8_t statusLedPin = 33U;
    uint8_t fiveVoltEnable = 13U;
    PlayerControlPinProfile p1{};
    PlayerControlPinProfile p2{};
    uint8_t i2cSda = 3U;
    uint8_t i2cScl = 4U;
    std::array<uint8_t, 4> expanderInterrupts{};
    uint8_t usbCc1 = 1U;
    uint8_t usbCc2 = 2U;
    uint8_t accelerometerInterrupt = NO_GPIO;
    BoardLedTopologyProfile leds{};
    // Provisional V3.3 emitter gain until its new LEDs are qualified.
    uint16_t ledOutputScale256 = 256U;
    // Resolved S3 profiles use the full 75 ms train; keep the short trial opt-in.
    bool usesShortFiveVoltPrecharge = false;
};

constexpr S3RevisionPinProfile S3RevisionPinProfileFor(
    const BoardRevision revision) {
    if (HasFourLedSquares(ResolveBoardRevision(revision))) {
        // V3.3 replaces V3's rotated P2 switch footprint with individually
        // placed contacts. Use their named schematic/PCB nets directly.
        const PlayerControlPinProfile p2 =
            ResolveBoardRevision(revision) == BoardRevision::V3_3S3
                ? PlayerControlPinProfile{15U, 13U, 14U, 17U, 12U, 18U}
                : PlayerControlPinProfile{14U, 17U, 15U, 13U, 12U, 18U};
        return S3RevisionPinProfile{
            11U,
            p2,
            std::array<uint8_t, 4>{34U, 38U, 48U, 39U},
            21U,
        };
    }

    return S3RevisionPinProfile{
        13U,
        PlayerControlPinProfile{17U, 15U, 16U, 21U, 14U, 26U},
        std::array<uint8_t, 4>{34U, 12U, 48U, 11U},
        NO_GPIO,
    };
}

constexpr bool HasAccelerometer(const BoardRevision revision) {
    return S3RevisionPinProfileFor(revision).accelerometerInterrupt != NO_GPIO;
}

constexpr BoardHardwareProfile BoardHardwareProfileFor(
    const BoardRevision requestedRevision) {
    const BoardRevision revision = ResolveBoardRevision(requestedRevision);
    if (revision == BoardRevision::V1C3) {
        return BoardHardwareProfile{
            BoardRevision::V1C3,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            2U,
            2U,
            NO_GPIO,
            PlayerControlPinProfile{10U, 4U, 7U, 9U, 21U, 20U},
            PlayerControlPinProfile{},
            NO_GPIO,
            NO_GPIO,
            std::array<uint8_t, 4>{NO_GPIO, NO_GPIO, NO_GPIO, NO_GPIO},
            NO_GPIO,
            NO_GPIO,
            NO_GPIO,
            BoardLedTopologyFor(BoardRevision::V1C3),
        };
    }

    const S3RevisionPinProfile revisionPins =
        S3RevisionPinProfileFor(revision);
    return BoardHardwareProfile{
        revision,
        true,
        true,
        true,
        true,
        true,
        true,
        true,
        47U,
        33U,
        revisionPins.fiveVoltEnable,
        PlayerControlPinProfile{7U, 5U, 6U, 9U, 0U, 10U},
        revisionPins.p2,
        3U,
        4U,
        revisionPins.expanderInterrupts,
        1U,
        2U,
        revisionPins.accelerometerInterrupt,
        BoardLedTopologyFor(revision),
        revision == BoardRevision::V3_3S3 ? uint16_t{128U} : uint16_t{256U},
        false,
    };
}

struct RuntimeBoardHardwareProfile {
    // Keep the raw decode for diagnostics even when behavior falls back to V2.
    BoardRevision detectedRevision = BoardRevision::Unknown;
    BoardHardwareProfile resolved =
        BoardHardwareProfileFor(BoardRevision::Unknown);
    bool revisionSampled = false;
};

constexpr RuntimeBoardHardwareProfile RuntimeProfileForC3() {
    return RuntimeBoardHardwareProfile{
        BoardRevision::V1C3,
        BoardHardwareProfileFor(BoardRevision::V1C3),
        true,
    };
}

constexpr RuntimeBoardHardwareProfile RuntimeProfileForS3Sample(
    const S3RevisionStrapSample sample) {
    const BoardRevision detected = DetectS3BoardRevision(sample);
    return RuntimeBoardHardwareProfile{
        detected,
        BoardHardwareProfileFor(detected),
        true,
    };
}

constexpr std::array<uint8_t, 3> S3_REVISION_STRAP_PINS{
    35U,
    36U,
    37U,
};

} // namespace hardware
