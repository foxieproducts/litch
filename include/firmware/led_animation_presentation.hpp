#pragma once

#include <array>
#include <cstdint>
#include <variant>

#include "firmware/manufacturing_test_led_frame.hpp"
#include "firmware/compliance_test.hpp"
#include "firmware/sleep_controller.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "hardware/led_delivery_telemetry.hpp"
#include "modules/boot_fade_in.hpp"
#include "modules/boot_display.hpp"
#include "modules/ble_identify_presentation.hpp"
#include "modules/board_owner_pairing_runtime.hpp"
#include "modules/water_slosh_session.hpp"
#include "modules/control_led_feedback.hpp"
#include "modules/led_frame_composer.hpp"
#include "modules/litchcraft_presentation.hpp"
#include "modules/ota_led_presentation.hpp"
#include "runtime/interaction_model_presentation.hpp"

namespace firmware {

struct GameLedPresentation {
    runtime::InteractionModelPresentation interaction{};
    modules::BoardStylePreviewFrame stylePreview{};
};
struct OtaLedScene {
    modules::OtaLedPresentation presentation{};
};
struct ManufacturingLedPresentation {
    ManufacturingTestFrameInputs inputs{};
    uint16_t brightnessScale256 = 256U;
};
struct ComplianceLedPresentation {
    compliance::Load load{};
    uint32_t startedMs = 0U;
};
struct HoldLedPresentation {};

// Values only: main owns all source settings, game, input and electrical facts.
// The animation owner applies the existing budget and optical mapping policy.
struct LedOutputPresentation {
    modules::PowerBudgetPolicyConfig power{};
    modules::BudgetAwareLedPipelineConfig pipeline{};
    modules::BootFadeIn bootFade{};
    float rawAmbientFactor01 = 0.0F;
    bool ambientUsable = false;
    bool autoBrightness = true;
    bool canvasVisible = false;
    bool exactOpticalTest = false;
    uint8_t brightnessLevel = 5U;
    uint32_t sourceResetEpoch = 0U;
    uint16_t sourceResetLimitMa = 0U;
};

struct PhysicalLedCommand {
    hardware::PhysicalLedFrame frame{};
    LedOutputPresentation output{};
    std::array<uint8_t, hardware::MAX_PHYSICAL_LED_COUNT> accentMask{};
    uint16_t brightnessScale256 = 256U;
    uint16_t accentScale256 = 256U;
    uint16_t accentCeilingScale256 = 0U;
    bool useSourceSlew = true;
};
struct RawDieLedCommand {
    uint8_t channel = 0U;
    uint8_t value = 0U;
};
struct ResetHoldLedPresentation {
    modules::UiOwner owner = modules::UiOwner::None;
    uint32_t startedAtMs = 0U;
    bool active = false;
    bool triggered = false;
    bool rejected = false;
};

struct LedAnimationPresentation {
    using Scene = std::variant<GameLedPresentation,
        modules::litchcraft_presentation::Inputs, OtaLedScene,
        ManufacturingLedPresentation, ComplianceLedPresentation, HoldLedPresentation,
        PhysicalLedCommand, RawDieLedCommand>;
    Scene scene{};
    modules::led_frame_composer::ComposeSettings settings{};
    modules::MenuLevelOverlay menu{};
    modules::EdgeLedIntent menuIntent{};
    modules::EdgeLedCueIntent edgeCue{};
    modules::ControlLedFeedbackInputs controlInputs{};
    modules::ControlLedFeedbackEvents controlEvents{};
    runtime::RuntimeHealthSnapshot health{};
    modules::BleIdentifyPresentation::Intent identify{};
    modules::BoardOwnerPairingRuntime::CueSnapshot pairing{};
    modules::WaterSloshSession water{};
    uint8_t waterRiderSquare = 255U;
    bool pairingAllowed = false;
    LedOutputPresentation output{};
    ResetHoldLedPresentation resetHold{};
    modules::litchcraft_presentation::EntryHold entryHold{};
    uint64_t physicalOccupiedMask = 0U;
    uint64_t positionRevision = 0U;
    uint32_t sequence = 0U;
    uint32_t hintEpoch = 0U;
    uint32_t contextEpoch = 0U;
    uint32_t capturedAtMs = 0U;
    uint32_t boardObservationSequence = 0U;
    runtime::AllPiecesFlashPresentation allPiecesFlash;
    uint32_t menuHoldThresholdMs = 1000U;
    uint32_t artworkModeGeneration = 0U;
    uint32_t artworkCanvasGeneration = 0U;
    bool boardRotate = false;
    bool firstPlayerIsP2 = false;
    bool quickSettingsActive = false;
    bool menuControllerActive = false;
    uint8_t menuSelection = 0U;
    uint8_t paletteEditOwners = 0U;
    bool beginnerModeP1 = false;
    bool beginnerModeP2 = false;
    modules::UiOwner beginnerStingerOwner = modules::UiOwner::None;
    bool beginnerStingerEnabling = false;
    uint32_t beginnerStingerStartedMs = 0U;
    SleepState sleepState = SleepState::Awake;
    uint32_t sleepRemainingMs = 0U;
    uint32_t sleepDurationMs = 0U;
    uint32_t sleepStartedMs = 0U;
    uint16_t sleepScale256 = 256U;
    bool initialStoreModePending = false;
    bool artworkUnavailable = false;
    bool startupBoardObserved = false;
    bool startupPieceAccent = false;
    bool bulkTransferActive = false;
};

struct LedAnimationReceipt {
    uint32_t sequence = 0U;
    uint32_t hintEpoch = 0U;
    uint32_t contextEpoch = 0U;
    uint64_t positionRevision = 0U;
    uint32_t bootCancelEpoch = 0U;
    uint32_t frameStartedMs = 0U;
    uint32_t completedAtMs = 0U;
    uint32_t startupCompletedAtMs = UINT32_MAX;
    uint32_t stackFreeBytes = 0U;
    runtime::HintPresentationTuple visibleHint{};
    bool visibleHintValid = false;
    bool completed = false;
    bool outputEnabled = false;
    bool quiescent = false;
    bool startupComplete = false;
    modules::litchcraft_presentation::Feedback artwork{};
    modules::BootDisplay boot{};
    hardware::LedDeliverySnapshot delivery{};
    modules::BudgetAwareLedPipelineStats power{};
    uint16_t currentLimitMa = 0U;
    uint16_t brightnessScale256 = 0U;
};

} // namespace firmware
