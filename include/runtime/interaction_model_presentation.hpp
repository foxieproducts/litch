#pragma once

#include "runtime/interaction_domain_types.hpp"

namespace runtime {

class GameStateMachine;

// Immutable game evidence plus the existing presentation envelopes. No live
// settings, game, engine, clock or persistence owner crosses this boundary.
struct InteractionModelPresentation {
    InteractionModelSnapshot snapshot{};
    TurnIndicatorPresentation turnIndicator{};
    modules::ControlLedThinkingPresentation thinkingControls{};
    modules::ControlLedThinkingPresentation hintBrewing{};
    HintRefinementPresentation hintRefinement{};
    AllPiecesFlashPresentation allPiecesFlash;
    uint32_t captureReturnPeriodMs = 1U;
    uint32_t settleFlashPeriodMs = 1U;
};

InteractionModelPresentation CaptureInteractionModelPresentation(
    const GameStateMachine& stateMachine,
    const InteractionModelSnapshot& snapshot);

void CaptureInteractionModelPresentation(const GameStateMachine& stateMachine,
    const InteractionModelSnapshot& snapshot, InteractionModelPresentation& destination);

// Samples cosmetic time only. Physical phases, legal targets, engine outcomes
// and clock timeout authority remain exactly those of the published snapshot.
InteractionModelSnapshot SampleInteractionModelPresentation(
    const InteractionModelPresentation& published,
    uint32_t nowMs);

} // namespace runtime
