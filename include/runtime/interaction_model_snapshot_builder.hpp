#pragma once

#include "modules/engine_derived_state.hpp"
#include "runtime/interaction_domain_types.hpp"

namespace firmware_internal {
struct FirmwareSystem;
}

namespace runtime {

class GameStateMachine;

struct InteractionModelSnapshotBuildInputs {
    const GameStateMachine* stateMachine = nullptr;
    const modules::EngineDerivedState* derived = nullptr;
    // Published position owned by the coordinator, never the mutable search
    // board. The builder copies its pieces synchronously into the snapshot.
    const char* authoritativeFen = nullptr;
    uint64_t captureFirstCapturerMask = 0ULL;
    const GameClock* gameClock = nullptr;
    modules::GameWinner clockWinner = modules::GameWinner::None;
    uint32_t clockTerminalStartMs = 0U;
    bool menuOverlayActive = false;
    bool quickSettingsOverlayActive = false;
    bool bootFadeActive = false;
    bool startupPresentationActive = false;
    bool safeModeActive = false;
    bool hardwareTestActive = false;
    bool amptestActive = false;
    bool otaInProgress = false;
    modules::GameVariant variant = modules::GameVariant::Classic;
    uint8_t duckSquare = modules::INVALID_DUCK_SQUARE;
    uint8_t initialDuckCandidateSquare = modules::INVALID_DUCK_SQUARE;
    bool initialVariantValidationActive = false;
    SetupLifecycle setupLifecycle = SetupLifecycle::Gameplay;
    modules::DuckTurnPhase duckTurnPhase = modules::DuckTurnPhase::ChessMove;
    modules::VariantOutcome variantOutcome{};
    uint32_t variantTerminalStartMs = 0U;
    modules::UiOwner kingResignationOwner = modules::UiOwner::None;
    uint8_t kingResignationSquare = 255U;
    uint32_t kingResignationElapsedMs = 0U;
    bool kingResignationArmed = false;
};

InteractionModelSnapshot BuildInteractionModelSnapshot(const InteractionModelSnapshotBuildInputs& inputs);

#ifdef ARDUINO
InteractionModelSnapshot BuildInteractionModelSnapshot(const firmware_internal::FirmwareSystem& sys);
#endif

} // namespace runtime
