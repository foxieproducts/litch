#include "firmware/manufacturing_test_led_frame.hpp"

#include <algorithm>

namespace firmware {

namespace {

constexpr modules::Rgb FACTORY_BACKGROUND{};
constexpr modules::Rgb FACTORY_PENDING{128U, 64U, 0U};
constexpr modules::Rgb FACTORY_PRESSED{160U, 160U, 160U};
constexpr modules::Rgb FACTORY_OBSERVED{0U, 160U, 0U};
constexpr modules::Rgb ALS_RESPONSE_PENDING{32U, 16U, 0U};
constexpr modules::Rgb ALS_RESPONSE_PASSED{0U, 40U, 0U};
// Physical #2 driver0=0x10 beside H8; #1 driver1=0x48 beside A1.
constexpr uint8_t LIGHT_SENSOR_CORNERS[2] = {7U, 56U};

uint16_t ActiveLedCount(const hardware::BoardLedTopologyProfile& topology) {
    return topology.physicalBoardLedCount + topology.physicalStatusLedCount;
}

modules::Rgb ScanColor(const uint8_t pass) {
    switch (pass) {
        case 0U: return {255U, 0U, 0U};
        case 1U: return {0U, 255U, 0U};
        case 2U: return {0U, 0U, 255U};
        default: return {255U, 255U, 255U};
    }
}

void FillLedScan(const ManufacturingTestFrameInputs& inputs,
                 const hardware::BoardLedTopologyProfile& topology,
                 ManufacturingTestLedFrame& frame) {
    for (uint16_t index = 0U; index < ActiveLedCount(topology); ++index)
        frame[index] = ScanColor(inputs.ledChannel);
}

void FillDim(ManufacturingTestLedFrame& frame,
             const uint16_t count,
             const modules::Rgb color) {
    for (uint16_t index = 0U; index < count; ++index) {
        frame[index] = color;
    }
}

void FillHall(const ManufacturingTestFrameInputs& inputs,
              const hardware::BoardLedTopologyProfile& topology,
              ManufacturingTestLedFrame& frame) {
    for (uint8_t square = 0U; square < 64U; ++square) {
        const uint64_t bit = 1ULL << square;
        const bool highSeen = (inputs.progress.hallClearMask & bit) != 0ULL;
        const bool lowSeen = (inputs.progress.hallActiveMask & bit) != 0ULL;
        // PCA9555 polarity inversion normalizes electrical LOW to occupied=1.
        // White means both electrical states were observed, never two sensors.
        const modules::Rgb color = !inputs.progress.hallSnapshotValid
            ? ((inputs.runElapsedMs / 400U) % 2U == 0U ? FACTORY_PENDING : modules::Rgb{})
            : highSeen && lowSeen ? modules::Rgb{255U, 255U, 255U}
            : FACTORY_PENDING;
        const bool removeRequired = inputs.progress.hallSnapshotValid && highSeen && lowSeen &&
            (inputs.progress.hallOccupiedMask & bit) != 0ULL;
        // A moving dark slot remains spatially distinct from clear white even
        // at the least-salient phase and after current-limit quantization.
        const uint8_t darkSlot = (inputs.runElapsedMs / 250U) % topology.physicalLedsPerSquare;
        for (uint8_t slot = 0U; slot < topology.physicalLedsPerSquare; ++slot)
            frame[square * topology.physicalLedsPerSquare + slot] =
                removeRequired && slot == darkSlot ? modules::Rgb{} : color;
    }
}

void PaintWholeSquare(ManufacturingTestLedFrame& frame,
                       const hardware::BoardLedTopologyProfile& topology,
                       const uint8_t square, const modules::Rgb color) {
    const uint16_t base = square * topology.physicalLedsPerSquare;
    for (uint8_t slot = 0U; slot < topology.physicalLedsPerSquare; ++slot) {
        frame[base + slot] = color;
    }
}

void PaintSquarePair(ManufacturingTestLedFrame& frame,
                     const hardware::BoardLedTopologyProfile& topology,
                     const uint8_t square, const modules::Rgb color) {
    const uint16_t first = square * topology.physicalLedsPerSquare;
    frame[first] = frame[first + 2U] = color;
}

void FillNavigationControls(const ManufacturingTestFrameInputs& inputs,
                            const hardware::BoardLedTopologyProfile& topology,
                            ManufacturingTestLedFrame& frame) {
    struct Control { uint8_t bit; uint8_t square; uint8_t pendingSquare; };
    // P2 faces the opposite end: its X is G8 and O is B8.
    constexpr Control CONTROLS[] = {{5U,57U,49U},{6U,62U,54U},{12U,6U,14U},{13U,1U,9U}};
    for (const auto control : CONTROLS) {
        const uint16_t bit = 1U << control.bit;
        if (!(inputs.progress.expectedButtonMask & bit)) continue;
        PaintSquarePair(frame, topology, control.square,
            inputs.progress.pressedButtonMask & bit ? FACTORY_PRESSED
                : inputs.progress.observedButtonMask & bit ? FACTORY_OBSERVED : FACTORY_PENDING);
        // Untested contact has a two-square extent; observed retains the original
        // action mark. Both outward sides agree and no color distinction is needed.
        if (!(inputs.progress.observedButtonMask & bit))
            PaintSquarePair(frame, topology, control.pendingSquare, FACTORY_PENDING);
    }
}

void FillProgress(const ManufacturingTestFrameInputs& inputs,
                  const hardware::BoardLedTopologyProfile& topology,
                  ManufacturingTestLedFrame& frame) {
    FillDim(frame, ActiveLedCount(topology), FACTORY_BACKGROUND);
    // Canonical square order is A8=0 .. H1=63, independent of game rotation.
    // Each bank's directions face its physical operator, including P2's
    // 180-degree diagram footprint. GPIO directions remain local to that bank.
    struct ButtonFootprint {
        uint8_t bit;
        uint8_t squares[2];
    };
    constexpr ButtonFootprint BUTTONS[8] = {
        {0U, {35U, 36U}}, {1U, {59U, 60U}},
        {2U, {50U, 42U}}, {3U, {53U, 45U}},
        {7U, {27U, 28U}}, {8U, {3U, 4U}},
        {9U, {13U, 21U}}, {10U, {10U, 18U}},
    };
    for (const ButtonFootprint& button : BUTTONS) {
        const uint16_t bit = static_cast<uint16_t>(1U << button.bit);
        if ((inputs.progress.expectedButtonMask & bit) == 0U) {
            continue;
        }
        const bool observed = (inputs.progress.observedButtonMask & bit) != 0U;
        const bool held = (inputs.progress.pressedButtonMask & bit) != 0U;
        const modules::Rgb color = observed ? FACTORY_BACKGROUND
            : FACTORY_PENDING;
        for (const uint8_t square : button.squares) {
            if (square < 64U) {
                // The diagram belongs to the two outward SquareLEDs. Fitted
                // inward PieceLEDs stay dark in the control diagram.
                // Released directions go dark; X/O have their own footprint below.
                PaintSquarePair(frame, topology, square, held ? FACTORY_PRESSED : color);
            }
        }
    }
    FillNavigationControls(inputs, topology, frame);
}

void MapV3_3StatusAndControls(const ManufacturingTestFrameInputs& inputs,
                             ManufacturingTestLedFrame& frame) {
    if (inputs.revision != hardware::BoardRevision::V3_3S3) {
        return;
    }
    constexpr uint16_t first = hardware::MAX_PHYSICAL_BOARD_LED_COUNT;
    std::array<modules::Rgb, hardware::SEMANTIC_STATUS_LED_COUNT> semantic{};
    std::copy_n(frame.begin() + first, semantic.size(), semantic.begin());
    const modules::Rgb controlBackground = frame[first + 32U];
    // Factory effects on the 32-position perimeter keep the same geometry as
    // earlier boards. A neutral control background comes from the filled tail.
    for (uint8_t status = 0U;
         status < hardware::V3_3_STATUS_CHAIN_LED_COUNT; ++status) {
        frame[first + status] = controlBackground;
    }
    for (uint8_t status = 0U; status < semantic.size(); ++status) {
        frame[first + hardware::V3_3PhysicalIndexForSemanticStatus(status)] =
            semantic[status];
    }
    if (inputs.step != HardwareTestMode::Step::Buttons) {
        return;
    }
    constexpr uint8_t p1Bits[6] = {6U, 0U, 3U, 1U, 2U, 5U};
    constexpr uint8_t p2Bits[6] = {13U, 7U, 10U, 8U, 9U, 12U};
    for (uint8_t role = 0U; role < 6U; ++role) {
        const bool navigation = role == 0U || role == 5U;
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            const uint8_t bit = bank == 0U ? p1Bits[role] : p2Bits[role];
            const uint16_t mask = static_cast<uint16_t>(1U << bit);
            if ((inputs.progress.expectedButtonMask & mask) == 0U) {
                continue;
            }
            const bool observed = (inputs.progress.observedButtonMask & mask) != 0U;
            const bool held = (inputs.progress.pressedButtonMask & mask) != 0U;
            const modules::Rgb color = held ? FACTORY_PRESSED
                : navigation && observed ? FACTORY_OBSERVED : observed ? FACTORY_BACKGROUND : FACTORY_PENDING;
            const auto control = static_cast<hardware::ControlLedRole>(role);
            frame[first + hardware::V3_3PhysicalIndexForControl(bank == 1U, control)] = color;
        }
    }
}

void FillCentralRadioSquare(const ManufacturingTestFrameInputs& inputs,
                            const hardware::BoardLedTopologyProfile& topology,
                            ManufacturingTestLedFrame& frame) {
    // C3:F6, with an outline while pending and a filled square only after the
    // controller's durably saved PASS. Election/proof masks never grant green.
    const bool saved = inputs.step == HardwareTestMode::Step::Passed;
    const bool serving = inputs.automaticVerifier ||
        inputs.verifierFeedback == HardwareTestMode::VerifierFeedback::BleProof ||
        inputs.verifierFeedback == HardwareTestMode::VerifierFeedback::Success;
    constexpr uint8_t BORDER[] = {18U,19U,20U,21U,29U,37U,45U,44U,43U,42U,34U,26U};
    const uint8_t whiteSquare = BORDER[(inputs.stepElapsedMs / 250U) % 12U];
    const uint32_t phase = inputs.stepElapsedMs % 3000U;
    const uint32_t ramp = phase <= 1500U ? phase : 3000U - phase;
    const uint8_t amber = static_cast<uint8_t>(64U + ramp * 64U / 1500U);
    const modules::Rgb waiting = inputs.automaticVerifier ? FACTORY_PENDING
        : modules::Rgb{amber, static_cast<uint8_t>(amber / 2U), 0U};
    for (uint8_t row = 2U; row < 6U; ++row) for (uint8_t column = 2U; column < 6U; ++column) {
        const bool boundary = row == 2U || row == 5U || column == 2U || column == 5U;
        if (!saved && !boundary) continue;
        const uint8_t square = row * 8U + column;
        const bool marker = (saved ? serving : inputs.automaticVerifier) && square == whiteSquare;
        const modules::Rgb color = marker ? FACTORY_PRESSED
            : saved ? FACTORY_OBSERVED : waiting;
        PaintSquarePair(frame, topology, square, color);
    }
}

void FillAmbientResponse(const ManufacturingTestFrameInputs& inputs,
                         const hardware::BoardLedTopologyProfile& topology,
                         ManufacturingTestLedFrame& frame) {
    // Steady dim marks keep the optical stimulus independent of the lux reading.
    for (uint8_t index = 0U; index < 2U; ++index) {
        const uint8_t bit = 1U << index;
        if (!(inputs.progress.lightSensorLinkMask & bit)) continue;
        const uint8_t square = LIGHT_SENSOR_CORNERS[index];
        if (!(inputs.progress.lightSensorResponseMask & bit)) {
            PaintSquarePair(frame, topology, square, ALS_RESPONSE_PENDING);
            continue;
        }
        const auto inward = hardware::MapSemanticSquareLedTargets(
            inputs.revision, square, hardware::SquareLedSlot::Piece, false);
        for (uint8_t slot = 0U; slot < inward.count; ++slot) {
            frame[inward.indices[slot]] = ALS_RESPONSE_PASSED;
        }
    }
}

void FillSensorFaults(const ManufacturingTestFrameInputs& inputs,
                      const hardware::BoardLedTopologyProfile& topology,
                      ManufacturingTestLedFrame& frame) {
    const uint8_t lightCheck = 1U << static_cast<uint8_t>(
        modules::ManufacturingTestLocalCheck::LightSensors);
    // A missing link is a fault only after the communication attempt failed;
    // unknown startup state and a valid zero-lux sample must remain distinct.
    if (inputs.progress.failedAutomatedCheckMask & lightCheck) {
        for (uint8_t index = 0U; index < 2U; ++index) {
            if (!(inputs.progress.lightSensorLinkMask & (1U << index)))
                PaintWholeSquare(frame, topology, LIGHT_SENSOR_CORNERS[index], {255U, 0U, 0U});
        }
    }
    // Fixed physical perimeter: top L->R, right T->B, bottom R->L,
    // left B->T (status_led_frame_adapter::VirtualTargetForPhysicalIndex).
    constexpr uint8_t squares[4] = {8U, 15U, 48U, 55U}; // A7 H7 A2 H2
    constexpr uint8_t adjacentStatus[4] = {30U, 9U, 25U, 14U};
    if (inputs.progress.expanderLinksKnown) for (uint8_t index = 0U; index < 4U; ++index) {
        if (inputs.progress.expanderLinkMask & (1U << index)) continue;
        const bool squarePhase = (inputs.runElapsedMs / 400U) % 2U == 0U;
        PaintWholeSquare(frame, topology, squares[index], squarePhase ? modules::Rgb{255U, 0U, 0U} : modules::Rgb{});
        const uint8_t status = inputs.revision == hardware::BoardRevision::V3_3S3
            ? hardware::V3_3PhysicalIndexForSemanticStatus(adjacentStatus[index]) : adjacentStatus[index];
        frame[topology.physicalBoardLedCount + status] = squarePhase ? modules::Rgb{} : modules::Rgb{255U, 0U, 0U};
    }
    if (inputs.progress.motionPresenceKnown && !inputs.progress.motionPresent)
        PaintWholeSquare(frame, topology, 40U, {255U, 0U, 0U}); // A3
}

} // namespace

ManufacturingTestLedFrame ComposeManufacturingTestLedFrame(
    const ManufacturingTestFrameInputs& inputs) {
    ManufacturingTestLedFrame frame{};
    const auto topology = hardware::BoardLedTopologyFor(inputs.revision);
    const uint16_t count = ActiveLedCount(topology);
    const bool local = inputs.step == HardwareTestMode::Step::HallSensors ||
        inputs.step == HardwareTestMode::Step::Buttons;
    switch (inputs.step) {
        case HardwareTestMode::Step::LedRainbow:
        case HardwareTestMode::Step::WhiteChannel:
        case HardwareTestMode::Step::LedInspect:
        case HardwareTestMode::Step::ConfirmLeds:
            FillLedScan(inputs, topology, frame);
            return frame; // All fitted dies belong exclusively to optical inspection.
        case HardwareTestMode::Step::HallSensors: FillHall(inputs, topology, frame); break;
        case HardwareTestMode::Step::Buttons: FillProgress(inputs, topology, frame); break;
        case HardwareTestMode::Step::Radio:
            FillCentralRadioSquare(inputs, topology, frame);
            break;
        case HardwareTestMode::Step::Passed:
            FillCentralRadioSquare(inputs, topology, frame);
            break;
        case HardwareTestMode::Step::Persisting: FillCentralRadioSquare(inputs, topology, frame); break;
        case HardwareTestMode::Step::Failed:
            if (inputs.progress.failedAutomatedCheckMask != 0U) break;
            for (uint8_t square = 0U; square < 64U; ++square)
                if (square / 8U == square % 8U || square / 8U + square % 8U == 7U)
                    PaintWholeSquare(frame, topology, square, {255U, 0U, 0U});
            break;
        case HardwareTestMode::Step::PassedRadioBypassed:
        case HardwareTestMode::Step::LightSensors:
        case HardwareTestMode::Step::MotionSensor: FillDim(frame, count, FACTORY_PENDING); break;
    }
    // A chord has priority over local and radio cues, but never covers the LED
    // inspection field. Threshold exit is immediate, so no forced PASS frame.
    if (inputs.operatorHoldProgress != 0U) {
        frame.fill({});
        const uint16_t filled = (static_cast<uint32_t>(inputs.operatorHoldProgress) * topology.physicalStatusLedCount + 254U) / 255U;
        for (uint16_t offset = 0U; offset < filled; ++offset)
            frame[topology.physicalBoardLedCount + offset] = FACTORY_PRESSED;
        return frame;
    }
    // Local diagrams keep the ring quiet and retain the current V3.3 dedicated
    // control mapping; ambient corners and fault markers have their own owners.
    if (local) MapV3_3StatusAndControls(inputs, frame);
    if (inputs.step == HardwareTestMode::Step::Buttons) FillAmbientResponse(inputs, topology, frame);
    if (local || inputs.step == HardwareTestMode::Step::Failed || inputs.step == HardwareTestMode::Step::Radio)
        FillSensorFaults(inputs, topology, frame);
    return frame;
}

} // namespace firmware
