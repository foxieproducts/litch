#pragma once

#include "runtime/game_state_machine_render_state.hpp"

namespace runtime {

struct PendingUndoCommandHandoffState {
    bool hasPendingCommand = false;
    MoveRenderTransitionKind transition = MoveRenderTransitionKind::None;
    bool renderSuppressed = false;
};

// Preserve the cancel transition's responsive frame without allowing a newer
// sensor observation to overtake the coordinator-owned engine mutation.
template <typename RenderHandoffFn, typename ConsumeCommandFn>
bool RunPendingUndoCommandHandoff(
    const PendingUndoCommandHandoffState& state,
    RenderHandoffFn&& renderHandoff,
    ConsumeCommandFn&& consumeCommand) {
    if (!state.hasPendingCommand) {
        return true;
    }

    if (!state.renderSuppressed &&
        state.transition == MoveRenderTransitionKind::CancelUndo) {
        if (!renderHandoff()) return false;
    }
    consumeCommand();
    return true;
}

} // namespace runtime
