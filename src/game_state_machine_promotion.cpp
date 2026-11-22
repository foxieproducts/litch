#include "runtime/game_state_machine.hpp"

namespace runtime {

class GameStateMachine::PromotionInputAdapter final : public modules::IUserInput {
  public:
    explicit PromotionInputAdapter(GameStateMachine& sm)
        : m_sm(sm) {}

    bool TryReadButtonEvent(modules::ButtonEvent& outEvent) override {
        if (m_sm.m_buttonQueueCount == 0) {
            return false;
        }

        outEvent = m_sm.m_buttonQueue[m_sm.m_buttonQueueHead];
        m_sm.m_buttonQueueHead =
            static_cast<uint8_t>((m_sm.m_buttonQueueHead + 1U) % BUTTON_QUEUE_CAPACITY);
        --m_sm.m_buttonQueueCount;
        return true;
    }

  private:
    GameStateMachine& m_sm;
};

bool GameStateMachine::StartPromotion() {
    if (m_promotionUx.IsActive()) {
        return true;
    }

    const TaggedInteractionState& phase = m_chessInteractionModel.phase;
    const bool pendingHumanPromotion =
        phase.tag == ChessInteractionPhase::AwaitingAction &&
        phase.plan.source == PhysicalTransitionSource::Human &&
        phase.plan.promotionRequired;
    if (phase.tag != ChessInteractionPhase::Idle &&
        !pendingHumanPromotion) {
        return false;
    }

    // Discard any queued non-promotion button events (e.g. Up/Down pressed in
    // Idle) so they can't perturb the promotion modal once it starts.
    m_buttonQueueHead = 0;
    m_buttonQueueTail = 0;
    m_buttonQueueCount = 0;

    m_promotionChoicePending = false;
    m_promotionUx.Start();

    return true;
}

bool GameStateMachine::ConsumePromotionChoice(modules::PromotionChoice& outChoice) {
    if (!m_promotionChoicePending) {
        return false;
    }

    m_promotionChoicePending = false;
    // Hall may change after O but before the coordinator consumes the choice.
    // Restore the same selector and discard stale confirmation, never defer it.
    if (!m_promotionChoice.wasCancelled &&
        !IsPhysicalOccupancyConsistentWithInteraction()) {
        m_promotionUx.Start(m_promotionChoice.piece);
        m_buttonQueueHead = 0U;
        m_buttonQueueTail = 0U;
        m_buttonQueueCount = 0U;
        return false;
    }
    outChoice = m_promotionChoice;
    return true;
}

void GameStateMachine::HandleButtonEvent(const modules::UserButton button,
                                         const fxe::Button::Event event) {
    if (button == modules::UserButton::Press) return;
    if (m_placementModeActive && event == fxe::Button::Event::Press && modules::IsXButton(button)) {
        // Keep placement mode state fresh for this interaction. This also lets
        // us immediately exit placement mode once the mismatch is resolved and
        // sensors are stable (without needing an explicit Tick first).
        UpdatePlacementMode();

        // Placement mode is purely corrective; do not allow button presses to
        // bypass it (e.g. by triggering undo handling).
        if (m_placementModeActive) {
            return;
        }
    }

    if (event == fxe::Button::Event::Press &&
        (button == modules::UserButton::P1O || button == modules::UserButton::P2O) &&
        IsHumanOConfirmSettleActive()) {
        StartHumanOConfirmFinalizeSettle();
        return;
    }

    if (event == fxe::Button::Event::Press &&
        (button == modules::UserButton::P1X || button == modules::UserButton::P2X)) {
        // Undo is a coordinator-owned semantic command. Direct button events
        // do not have access to engine history and are ignored here.
        return;
    }

    if (m_promotionUx.IsActive() && modules::IsOButton(button) &&
        !IsPhysicalOccupancyConsistentWithInteraction()) {
        return;
    }

    if (m_buttonQueueCount >= BUTTON_QUEUE_CAPACITY) {
        // Drop new events when saturated; input gating should keep this small.
        return;
    }

    m_buttonQueue[m_buttonQueueTail] = modules::ButtonEvent{button, event};
    m_buttonQueueTail = static_cast<uint8_t>((m_buttonQueueTail + 1U) % BUTTON_QUEUE_CAPACITY);
    ++m_buttonQueueCount;

    if (m_promotionUx.IsActive()) {
        UpdatePromotion();
    }
}

void GameStateMachine::UpdatePromotion() {
    if (m_promotionChoicePending) {
        return;
    }

    PromotionInputAdapter input(*this);
    modules::PromotionChoice choice;
    if (!m_promotionUx.Tick(input, choice)) {
        return;
    }

    m_promotionChoice = choice;
    m_promotionChoicePending = true;

    // Clear any trailing events (e.g. button releases) to avoid polluting the
    // next promotion modal.
    m_buttonQueueHead = 0;
    m_buttonQueueTail = 0;
    m_buttonQueueCount = 0;
}

} // namespace runtime
