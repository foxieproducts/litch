#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/control_mutation_service.hpp"
#include "modules/edge_led_cue.hpp"

namespace runtime {
class GameStateMachine;
}

namespace firmware_internal {

enum class LocalControlRequestOrigin : uint8_t {
    Board = 0,
    Console,
};

struct LocalControlRequest {
    uint16_t requestId = 0U;
    LocalControlRequestOrigin origin = LocalControlRequestOrigin::Console;
};

struct LocalControlCompletion {
    LocalControlRequest request{};
    modules::ControlMutationOutcome outcome{};
};

struct BoardControlFeedback {
    bool visible = false;
    bool flashAllPieces = false;
    modules::EdgeLedCueKind edgeCue = modules::EdgeLedCueKind::None;
};

class LocalControlRequestTracker final {
  public:
    // One action may still be pending while every completion slot is occupied.
    static constexpr size_t CAPACITY =
        modules::ControlMutationService::COMPLETION_CAPACITY + 1U;

    [[nodiscard]] bool Track(uint16_t requestId,
                             LocalControlRequestOrigin origin);
    [[nodiscard]] bool TryGet(size_t index,
                              LocalControlRequest& outRequest) const;
    [[nodiscard]] bool Remove(size_t index);
    [[nodiscard]] size_t Count() const { return m_count; }

  private:
    std::array<LocalControlRequest, CAPACITY> m_requests{};
    size_t m_count = 0U;
};

[[nodiscard]] BoardControlFeedback ClassifyBoardControlFeedback(
    LocalControlRequestOrigin origin,
    const modules::ControlMutationOutcome& outcome);

[[nodiscard]] modules::ControlMutationOutcome RunLocalControlAction(
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    LocalControlRequestOrigin origin,
    modules::ControlAction action,
    modules::UiOwner owner,
    modules::CoachingMutation coachingMutation,
    uint8_t undoMoveCount,
    bool& outTrackingFailed);

// Physical O follows the board-attributed Hint mutation path. This shared
// seam keeps firmware and VirtualLitch journey feedback/correlation identical.
[[nodiscard]] modules::ControlMutationOutcome RunBoardHintTap(
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    bool& outTrackingFailed,
    modules::UiOwner owner = modules::UiOwner::None);

// Physical X has one presentation-level precedence rule before semantic Undo:
// while the optional initial king-placement assist is the visible cancelable
// layer, the tap dismisses only that assist and returns an immediate correlated
// Applied outcome. Otherwise it follows the ordinary control-mutation path.
[[nodiscard]] modules::ControlMutationOutcome RunBoardUndoTap(
    runtime::GameStateMachine& stateMachine,
    bool initialKingAssistOwnsX,
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    uint8_t undoMoveCount,
    bool& outTrackingFailed,
    modules::UiOwner owner = modules::UiOwner::None);

[[nodiscard]] bool TryTakeNextLocalControlCompletion(
    modules::ControlMutationService& service,
    LocalControlRequestTracker& tracker,
    LocalControlCompletion& outCompletion);

struct FirmwareSystem;

[[nodiscard]] modules::ControlMutationOutcome RunBoardControlAction(
    FirmwareSystem& sys,
    modules::ControlAction action,
    modules::UiOwner owner = modules::UiOwner::None,
    modules::CoachingMutation coachingMutation =
        modules::CoachingMutation::Toggle,
    uint8_t undoMoveCount = 1U);

[[nodiscard]] modules::ControlMutationOutcome RunBoardUndoTap(
    FirmwareSystem& sys,
    uint8_t undoMoveCount = 1U,
    modules::UiOwner owner = modules::UiOwner::None);

[[nodiscard]] bool TrackLocalControlRequest(
    FirmwareSystem& sys,
    uint16_t requestId,
    LocalControlRequestOrigin origin);

void DrainTrackedLocalControlCompletions(FirmwareSystem& sys);

} // namespace firmware_internal
