#include <cstdio>

#include "hardware/board_hardware_profile.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/board_revision_runtime.hpp"
#include "hardware/pca9555_occupancy_decode.hpp"
#include "hardware/status_led_chain_profile.hpp"
#include "modules/orientation.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

bool SameRgb(const modules::Rgb a, const modules::Rgb b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

void CheckFullFrameMapping() {
    std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
        board[index] = {static_cast<uint8_t>(index + 1U),
                        static_cast<uint8_t>(255U - index),
                        static_cast<uint8_t>(index ^ 0x55U)};
    }
    for (uint8_t index = 0U; index < status.size(); ++index) {
        status[index] = {static_cast<uint8_t>(index * 7U), 93U, index};
        board[hardware::BOARD_LED_COUNT + index] = status[index];
    }
    for (const bool squareRotate : {false, true}) {
        for (const bool boardRotate : {false, true}) {
            const auto v2 = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V2S3, squareRotate, boardRotate}, board, status);
            std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> previous{};
            for (uint16_t source = 0U; source < hardware::BOARD_LED_COUNT; ++source) {
                previous[hardware::MapLedIndexForSquareAndEdgeRotate(
                    source, squareRotate, boardRotate)] = board[source];
            }
            modules::status_led_frame_adapter::ApplyToDedicatedChain(status, boardRotate, previous);
            for (uint16_t index = 0U; index < previous.size(); ++index) {
                Check(SameRgb(v2[index], previous[index]),
                      "full mapper preserves prior production/Virtual V2 bytes in all orientations");
            }
            const auto unknown = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::Unknown, squareRotate, boardRotate}, board, status);
            const auto legacy = hardware::MapLegacyLedFrame(
                {hardware::BoardRevision::V2S3, squareRotate, boardRotate}, board);
            for (uint16_t index = 0U; index < v2.size(); ++index) {
                Check(SameRgb(v2[index], unknown[index]) && SameRgb(v2[index], legacy[index]),
                      "unknown fallback and legacy status-tail adapter retain complete V2 frame");
            }
            const auto v3 = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V3S3, squareRotate, boardRotate}, board, status);
            for (uint8_t square = 0U; square < 64U; ++square) {
                const uint16_t physical = (squareRotate ? 63U - square : square) * 4U;
                const uint16_t source = square * 3U;
                Check(SameRgb(v3[physical], board[source + (squareRotate ? 2U : 0U)]) &&
                          SameRgb(v3[physical + 2U], board[source + (squareRotate ? 0U : 2U)]) &&
                          SameRgb(v3[physical + 1U], board[source + 1U]) &&
                          SameRgb(v3[physical + 3U], board[source + 1U]),
                      "V3 expands all64 squares into left/top/right/bottom with equal piece pair");
            }
            for (uint8_t index = 0U; index < status.size(); ++index) {
                const uint8_t mapped = static_cast<uint8_t>((index + (boardRotate ? 16U : 0U)) % 32U);
                Check(SameRgb(v3[256U + mapped], status[index]),
                      "V3 status begins at256 and follows board rotation only");
            }
            // Shared perimeter output must still yield to the composer's mask.
            const uint64_t priority = UINT64_C(0x8100000000000081);
            const auto perimeter = hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V1C3, squareRotate, boardRotate, priority}, board, status);
            auto previousPerimeter = previous;
            modules::status_led_frame_adapter::ApplyToVirtualPerimeter(
                status, boardRotate, previousPerimeter, priority, squareRotate);
            for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
                Check(SameRgb(perimeter[index], previousPerimeter[index]),
                      "shared perimeter preserves byte-exact blend and protected-square priority");
            }
            const auto retainedPerimeter = hardware::MapLegacyLedFrame(
                {hardware::BoardRevision::V1C3}, previousPerimeter);
            for (uint16_t index = 0U; index < hardware::BOARD_LED_COUNT; ++index) {
                Check(SameRgb(retainedPerimeter[index], previousPerimeter[index]),
                      "legacy power adapter never blends an already-composed perimeter again");
            }

            // Sleep used to rotate the three-slot canvas, then expand it in
            // the power adapter. Direct physical output must commute exactly,
            // including the duplicated V3 piece pair and status-tail offset.
            std::array<modules::Rgb, hardware::TOTAL_LED_COUNT> rotatedLegacy{};
            for (uint16_t source = 0U; source < board.size(); ++source) {
                rotatedLegacy[hardware::MapLedIndexForBoardRotate(source, boardRotate)] = board[source];
            }
            for (const auto revision : {hardware::BoardRevision::V2S3,
                                        hardware::BoardRevision::V3S3}) {
                const auto priorSleep = hardware::MapLegacyLedFrame({revision}, rotatedLegacy);
                const auto directSleep = hardware::MapLegacyLedFrame(
                    {revision, boardRotate, boardRotate}, board);
                for (uint16_t index = 0U; index < directSleep.size(); ++index) {
                    Check(SameRgb(priorSleep[index], directSleep[index]),
                          "direct V2/V3 sleep mapping equals legacy rotate then physical expansion");
                }
            }
        }
    }
}

void CheckRevisionDetection() {
    using hardware::BoardRevision;

    Check(hardware::DetectS3BoardRevision({false, false, false}) ==
              BoardRevision::V2S3,
          "all-high straps identify V2");
    Check(hardware::DetectS3BoardRevision({true, false, false}) ==
              BoardRevision::V3S3,
          "R3/GPIO35 low identifies V3");
    Check(hardware::DetectS3BoardRevision({true, true, false}) ==
              BoardRevision::V3_3S3,
          "R3 and R4 low identify V3.3 without changing V2 or V3");
    Check(hardware::DetectS3BoardRevision({false, true, false}) ==
              BoardRevision::Unknown,
          "unsupported strap pattern stays unknown");
    Check(hardware::ResolveBoardRevision(BoardRevision::Unknown) ==
              BoardRevision::V2S3,
          "unknown revision falls back to V2 behavior");
}

void CheckPinProfiles() {
    using hardware::BoardRevision;

    const auto v2 = hardware::S3RevisionPinProfileFor(BoardRevision::V2S3);
    const auto v3 = hardware::S3RevisionPinProfileFor(BoardRevision::V3S3);
    const auto v3_3 = hardware::S3RevisionPinProfileFor(BoardRevision::V3_3S3);
    const auto unknown =
        hardware::S3RevisionPinProfileFor(BoardRevision::Unknown);

    Check(v2.fiveVoltEnable == 13U, "V2 keeps GPIO13 5V enable");
    Check(v2.p2.right == 21U, "V2 keeps existing P2-right pin");
    Check(v2.accelerometerInterrupt == hardware::NO_GPIO,
          "V2 has no accelerometer interrupt");
    Check(v3.fiveVoltEnable == 11U, "V3 uses PCB GPIO11 5V enable");
    Check(v3.p2.up == 14U && v3.p2.left == 17U &&
              v3.p2.down == 15U &&
              v3.p2.right == 13U && v3.p2.o == 18U,
          "V3 maps the 180-degree-rotated P2 d-pad to physical directions");
    Check(v3_3.p2.up == 15U && v3_3.p2.left == 13U &&
              v3_3.p2.down == 14U &&
              v3_3.p2.right == 17U && v3_3.p2.x == 12U && v3_3.p2.o == 18U,
          "V3.3 discrete P2 contacts use their shipped nets, not V3's rotated footprint");
    Check(v3.expanderInterrupts[0] == 34U &&
              v3.expanderInterrupts[1] == 38U &&
              v3.expanderInterrupts[2] == 48U &&
              v3.expanderInterrupts[3] == 39U,
          "V3 expander interrupts match module pads");
    Check(hardware::HasAccelerometer(BoardRevision::V3S3) &&
              v3.accelerometerInterrupt == 21U,
          "V3 exposes accelerometer INT1 on GPIO21");
    Check(unknown.fiveVoltEnable == v2.fiveVoltEnable &&
              unknown.p2.right == v2.p2.right,
          "unknown pin profile preserves V2 behavior");

    const auto completeV2 =
        hardware::BoardHardwareProfileFor(BoardRevision::V2S3);
    const auto completeV3 =
        hardware::BoardHardwareProfileFor(BoardRevision::V3S3);
    const auto completeV1 =
        hardware::BoardHardwareProfileFor(BoardRevision::V1C3);
    const auto completeUnknown =
        hardware::BoardHardwareProfileFor(BoardRevision::Unknown);
    Check(!hardware::BoardHardwareProfileFor(BoardRevision::V3_3S3)
               .usesShortFiveVoltPrecharge &&
              !completeV2.usesShortFiveVoltPrecharge &&
              !completeV3.usesShortFiveVoltPrecharge &&
              !completeUnknown.usesShortFiveVoltPrecharge,
          "V3.3 uses the full precharge sequence alongside V2/V3/unknown");
    Check(completeV2.leds.physicalBoardLedCount == 192U &&
              completeV2.leds.physicalStatusLedCount == 32U,
          "V2 complete profile keeps 192+32 physical LEDs");
    Check(completeV3.leds.physicalBoardLedCount == 256U &&
              completeV3.leds.physicalStatusLedCount == 32U,
          "V3 complete profile selects 256+32 physical LEDs");
    // These GPIO identities come from the V3 electrical/netlist contract.
    // Together with the production Quick Settings mapping (Up=increase,
    // Down=decrease), they catch a physical vertical swap rather than merely
    // proving that an internally consistent button table maps to itself.
    Check(completeV3.p1.up == 7U && completeV3.p1.down == 6U &&
              completeV3.p2.up == 14U && completeV3.p2.down == 15U,
          "V3 physical Up increases and physical Down decreases on both control banks");
    Check(completeV2.p1.up == 7U && completeV2.p1.down == 6U &&
              completeV2.p2.up == 17U && completeV2.p2.down == 16U,
          "V2 retains its established directional GPIO mapping");
    Check(!completeV1.isS3 && !completeV1.hasP2Controls &&
              !completeV1.hasFiveVoltEnable &&
              completeV1.boardLedPin == 2U &&
              completeV1.p1.up == 10U && completeV1.p1.left == 4U &&
              completeV1.p1.down == 7U &&
              completeV1.p1.right == 9U &&
              completeV1.leds.physicalBoardLedCount == 192U &&
              completeV1.leds.physicalStatusLedCount == 0U,
          "V1 C3 complete profile preserves shelf prototype wiring");
    Check(completeUnknown.revision == BoardRevision::V2S3 &&
              completeUnknown.fiveVoltEnable == 13U &&
              completeUnknown.p2.right == 21U &&
              completeUnknown.expanderInterrupts[1] == 12U &&
              completeUnknown.accelerometerInterrupt == hardware::NO_GPIO &&
              completeUnknown.leds.physicalBoardLedCount == 192U,
          "unknown never selects any V3-only runtime pin or LED topology");
}

struct RecordingRevisionGpio {
    bool low35 = false;
    bool low36 = false;
    bool low37 = false;
    uint8_t configuredCount = 0U;
    bool readBeforeAllPullups = false;
    uint8_t reads = 0U;

    void ConfigureInputPullup(const uint8_t pin) {
        Check(pin == hardware::S3_REVISION_STRAP_PINS[configuredCount],
              "strap pull-ups are configured in stable GPIO order");
        ++configuredCount;
    }

    bool IsLow(const uint8_t pin) {
        if (configuredCount != hardware::S3_REVISION_STRAP_PINS.size()) {
            readBeforeAllPullups = true;
        }
        ++reads;
        if (pin == 35U) {
            return low35;
        }
        if (pin == 36U) {
            return low36;
        }
        return low37;
    }
};

void CheckRuntimeSamplingOrderAndFallback() {
    RecordingRevisionGpio v3Gpio;
    v3Gpio.low35 = true;
    const hardware::RuntimeBoardHardwareProfile v3 =
        hardware::SampleS3RuntimeHardwareProfile(v3Gpio);
    Check(!v3Gpio.readBeforeAllPullups && v3Gpio.configuredCount == 3U &&
              v3Gpio.reads == 3U,
          "all revision pull-ups are configured before any strap read");
    Check(v3.revisionSampled &&
              v3.detectedRevision == hardware::BoardRevision::V3S3 &&
              v3.resolved.revision == hardware::BoardRevision::V3S3,
          "V3 strap sample exposes raw and resolved V3 revisions");

    RecordingRevisionGpio v3_3Gpio;
    v3_3Gpio.low35 = true;
    v3_3Gpio.low36 = true;
    const hardware::RuntimeBoardHardwareProfile v3_3 =
        hardware::SampleS3RuntimeHardwareProfile(v3_3Gpio);
    Check(!v3_3Gpio.readBeforeAllPullups &&
              v3_3Gpio.configuredCount == 3U && v3_3Gpio.reads == 3U,
          "V3.3 samples both fitted resistors after configuring all pull-ups");
    Check(v3_3.revisionSampled &&
              v3_3.detectedRevision == hardware::BoardRevision::V3_3S3 &&
              v3_3.resolved.revision == hardware::BoardRevision::V3_3S3 &&
              static_cast<uint8_t>(v3_3.detectedRevision) == 4U &&
              v3_3.resolved.leds.physicalBoardLedCount == 256U &&
              v3_3.resolved.leds.physicalStatusLedCount == 44U,
          "binary strap version 3 selects V3.3's 300 LEDs and stable BLE ID 4");

    RecordingRevisionGpio unknownGpio;
    unknownGpio.low36 = true;
    const hardware::RuntimeBoardHardwareProfile unknown =
        hardware::SampleS3RuntimeHardwareProfile(unknownGpio);
    Check(unknown.revisionSampled &&
              unknown.detectedRevision == hardware::BoardRevision::Unknown,
          "unsupported strap pattern remains visible to diagnostics");
    Check(unknown.resolved.revision == hardware::BoardRevision::V2S3 &&
              unknown.resolved.fiveVoltEnable == 13U &&
              unknown.resolved.accelerometerInterrupt == hardware::NO_GPIO,
          "unsupported strap pattern resolves conservatively before peripherals");
}

void CheckV3LedMapping() {
    using hardware::BoardRevision;
    using hardware::SquareLedSlot;

    const auto left = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V3S3, 0U, SquareLedSlot::Square0, false);
    const auto piece = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V3S3, 0U, SquareLedSlot::Piece, false);
    const auto right = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V3S3, 0U, SquareLedSlot::Square2, false);
    Check(left.count == 1U && left.indices[0] == 0U,
          "V3 square0 drives outward left LED");
    Check(piece.count == 2U && piece.indices[0] == 1U &&
              piece.indices[1] == 3U,
          "V3 piece drives inward top and bottom LEDs");
    Check(right.count == 1U && right.indices[0] == 2U,
          "V3 square2 drives outward right LED");

    const auto rotatedLeft = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V3S3, 0U, SquareLedSlot::Square0, true);
    const auto rotatedPiece = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V3S3, 0U, SquareLedSlot::Piece, true);
    Check(rotatedLeft.indices[0] == 63U * 4U + 2U,
          "V3 rotation swaps left outward LED to right");
    Check(rotatedPiece.count == 2U &&
              rotatedPiece.indices[0] == 63U * 4U + 1U &&
              rotatedPiece.indices[1] == 63U * 4U + 3U,
          "V3 rotation preserves inward piece pair");
}

void CheckLegacyLedMapping() {
    using hardware::BoardRevision;
    using hardware::SquareLedSlot;

    const auto v1 = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V1C3, 5U, SquareLedSlot::Piece, false);
    const auto v2 = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V2S3, 5U, SquareLedSlot::Piece, false);
    const auto rotated = hardware::MapSemanticSquareLedTargets(
        BoardRevision::V2S3, 0U, SquareLedSlot::Square0, true);
    Check(v1.count == 1U && v1.indices[0] == 16U,
          "V1 keeps three-LED square layout");
    Check(v2.count == 1U && v2.indices[0] == 16U,
          "V2 keeps three-LED square layout");
    Check(rotated.indices[0] == 63U * 3U + 2U,
          "V2 rotation remains unchanged");
}

void CheckHallProfiles() {
    const auto v2 = hardware::GetExpanderPinToSquareMap(
        hardware::BoardRevision::V2S3);
    const auto v3 = hardware::GetExpanderPinToSquareMap(
        hardware::BoardRevision::V3S3);
    const auto v1 = hardware::GetExpanderPinToSquareMap(
        hardware::BoardRevision::V1C3);

    Check(v2 == hardware::EXPANDER_PIN_TO_SQUARE_S3,
          "V2 selects S3 hall map");
    Check(v3 == hardware::EXPANDER_PIN_TO_SQUARE_S3,
          "V3 preserves V2 S3 hall map");
    Check(v1 == hardware::EXPANDER_PIN_TO_SQUARE_C3,
          "V1 preserves C3 hall map");
}

void CheckV3_3StatusInsertion() {
    bool claimed[hardware::V3_3_STATUS_CHAIN_LED_COUNT]{};
    for (uint8_t semantic = 0U;
         semantic < hardware::SEMANTIC_STATUS_LED_COUNT;
         ++semantic) {
        const uint8_t physical =
            hardware::V3_3PhysicalIndexForSemanticStatus(semantic);
        Check(physical < hardware::V3_3_STATUS_CHAIN_LED_COUNT,
              "V3.3 status mapping stays in range");
        Check(!claimed[physical], "V3.3 status mapping has no collisions");
        claimed[physical] = true;
    }

    Check(hardware::V3_3PhysicalIndexForSemanticStatus(3U) == 3U &&
              hardware::V3_3PhysicalIndexForSemanticStatus(4U) == 10U,
          "P2 controls insert after status 3");
    Check(hardware::V3_3PhysicalIndexForSemanticStatus(19U) == 25U &&
              hardware::V3_3PhysicalIndexForSemanticStatus(20U) == 32U,
          "P1 controls insert after status 19");
    Check(hardware::V3_3PhysicalIndexForSemanticStatus(0U, true) == 22U,
          "rotation occurs in semantic space before insertion");
    constexpr uint8_t controlIndices[2][6] = {
        {26U, 27U, 28U, 29U, 30U, 31U},
        {4U, 5U, 6U, 7U, 8U, 9U},
    };
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        for (uint8_t role = 0U; role < 6U; ++role) {
            Check(hardware::V3_3PhysicalIndexForControl(
                      bank == 1U, static_cast<hardware::ControlLedRole>(role)) == controlIndices[bank][role],
                  "both control banks match switch nets and LED placement in the V3.3 PCB");
        }
    }
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        for (uint8_t role = 0U; role < 6U; ++role) {
            const auto index = hardware::V3_3PhysicalIndexForControl(
                bank == 0U, static_cast<hardware::ControlLedRole>(role));
            Check(index < hardware::V3_3_STATUS_CHAIN_LED_COUNT &&
                      !claimed[index],
                  "each control occupies one otherwise-unused chain slot");
            claimed[index] = true;
        }
    }
    for (const bool slot : claimed) {
        Check(slot, "V3.3 mapping covers all 44 physical status slots");
    }
    for (const bool swapped : {false, true}) {
        for (uint8_t logical = 0U; logical < 12U; ++logical) {
            modules::status_led_frame_adapter::FixedPhysicalStatusOverlay overlay{};
            overlay.controlMask = static_cast<uint16_t>(1U << logical);
            overlay.controlPixels[logical] = {17U, 23U, 31U};
            hardware::PhysicalLedFrame physical{};
            hardware::ApplyFixedPhysicalStatusOverlay(
                hardware::BoardRevision::V3_3S3, overlay, physical, swapped);
            const uint8_t bank = static_cast<uint8_t>((logical >= 6U) != swapped);
            const uint16_t expected = 256U + controlIndices[bank][logical % 6U];
            for (uint16_t index = 0U; index < physical.size(); ++index) {
                Check(SameRgb(physical[index], index == expected
                              ? overlay.controlPixels[logical] : modules::Rgb{}),
                      "one control cue reaches only its PCB emitter after bank swap");
            }
        }
    }
    for (const bool boardRotate : {false, true}) {
        for (const bool firstPlayerIsP2 : {false, true}) {
            for (const bool litchcraft : {false, true}) {
                const bool swapped = modules::ComputeControlBankSwap(
                    boardRotate, firstPlayerIsP2, litchcraft);
                for (uint8_t role = 0U; role < 6U; ++role) {
                    const auto button = static_cast<hardware::ControlLedRole>(role);
                    Check(hardware::V3_3PhysicalIndexForLogicalControl(
                              false, button, swapped) ==
                              hardware::V3_3PhysicalIndexForControl(swapped, button) &&
                          hardware::V3_3PhysicalIndexForLogicalControl(
                              true, button, swapped) ==
                              hardware::V3_3PhysicalIndexForControl(!swapped, button),
                          "button LEDs use the input bank swap without reversing local roles");
                }
            }
        }
    }

    const auto topology = hardware::BoardLedTopologyFor(
        hardware::BoardRevision::V3_3S3);
    Check(topology.physicalBoardLedCount == 256U &&
              topology.physicalStatusLedCount == 44U &&
              hardware::MAX_PHYSICAL_LED_COUNT == 300U,
          "V3.3 physical output covers 256 square and 44 status pixels");
    modules::BoardLedFrame board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> status{};
    for (uint8_t semantic = 0U; semantic < status.size(); ++semantic) {
        status[semantic] = modules::Rgb{static_cast<uint8_t>(semantic + 1U), 0U, 0U};
    }
    // PCB chain traced from STATUS_LEDS_5V; expected indices do not call
    // the production mapper, so a wrong insertion cannot validate itself.
    constexpr uint8_t perimeterIndices[32] = {
        0U, 1U, 2U, 3U, 10U, 11U, 12U, 13U,
        14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U,
        22U, 23U, 24U, 25U, 32U, 33U, 34U, 35U,
        36U, 37U, 38U, 39U, 40U, 41U, 42U, 43U,
    };
    for (const bool rotate : {false, true}) {
        const auto physical = hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V3_3S3, rotate, rotate}, board, status);
        for (uint8_t semantic = 0U; semantic < status.size(); ++semantic) {
            const uint8_t ring = static_cast<uint8_t>((semantic + (rotate ? 16U : 0U)) % 32U);
            const uint8_t chain = perimeterIndices[ring];
            Check(SameRgb(physical[256U + chain], status[semantic]),
                  "production V3.3 mapper rotates 32 semantic positions before insertion");
        }
        for (const auto range : {hardware::V3_3_P2_CONTROL_LED_RANGE,
                                 hardware::V3_3_P1_CONTROL_LED_RANGE}) {
            for (uint8_t offset = 0U; offset < range.count; ++offset) {
                Check(SameRgb(physical[256U + range.first + offset], {}),
                      "perimeter mapping leaves control emitters dark");
            }
        }
    }
}

} // namespace

int main() {
    std::puts("[BOARD_REVISION_PROFILE] start");

    CheckFullFrameMapping();
    CheckRevisionDetection();
    CheckPinProfiles();
    CheckRuntimeSamplingOrderAndFallback();
    CheckV3LedMapping();
    CheckLegacyLedMapping();
    CheckHallProfiles();
    CheckV3_3StatusInsertion();

    if (g_failures == 0) {
        std::puts("[BOARD_REVISION_PROFILE] ok");
        return 0;
    }

    std::printf("[BOARD_REVISION_PROFILE] failures=%d\n", g_failures);
    return 1;
}
