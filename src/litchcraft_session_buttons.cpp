#include "modules/litchcraft_session.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules {
void LitchcraftSession::ResetPhysicalButtons() {
    m_physicalX.Reset();
    m_physicalO.Reset();
    m_physicalFaceDown = {};
}
void LitchcraftSession::ReportPhysicalFeedback(const ControlFeedbackAction action,
    const UiOwner owner, const ControlRole role, const Rgb color) {
    if (m_controlFeedback != nullptr)
        m_controlFeedback->Trigger(action, owner, role, color, ControlFeedbackScene::Litchcraft);
}
void LitchcraftSession::FillControlFeedbackInputs(ControlLedFeedbackInputs& inputs) const {
    inputs.scene = ControlFeedbackScene::Litchcraft;
    inputs.paintColor = Color();
    const bool historyAvailable = m_active && !m_mutationsPaused &&
        !m_pendingStoreStructural && !m_pendingStoreHistoryOrdered &&
        m_pendingPhysicalHistoryFrame == UINT16_MAX &&
        (!HasStreamedDocument() || !m_frameStream->ReplacementPending());
    uint8_t available = 0U;
    if (historyAvailable && UndoCount() != 0U)
        available |= 1U << static_cast<uint8_t>(ControlRole::X);
    if (historyAvailable && RedoCount() != 0U)
        available |= 1U << static_cast<uint8_t>(ControlRole::O);
    inputs.availableControls = {available, available};
    for (uint8_t bank = 0U; bank < 2U; ++bank) {
        const auto owner = bank == 1U ? UiOwner::P2 : UiOwner::P1;
        inputs.holds[bank][0] = {ControlRole::X, m_physicalX.HeldMs(owner),
            m_physicalX.HoldThresholdMs(), true, m_physicalX.IsHeld(owner)};
        inputs.holds[bank][1] = {ControlRole::O, m_physicalO.HeldMs(owner),
            m_physicalO.HoldThresholdMs(), false, m_physicalO.IsHeld(owner)};
    }
}
void LitchcraftSession::ObserveButton(const ButtonEvent& event) {
    ObserveButton(event, fxe::Millis());
}
void LitchcraftSession::ObserveButton(const ButtonEvent& event, const uint32_t eventTimeMs) {
    PhysicalEditScope edit(*this);
    if (!m_active || m_mutationsPaused) return;
    const bool face = IsXButton(event.button) || IsOButton(event.button);
    if (!face && !IsDpadButton(event.button)) return;
    if (!m_modeChordFormed) {
        // A serviced batch may already include Release. Complete only holds
        // proven before this event, before changing either face-button state.
        TapHoldAction action;
        while (m_physicalX.Tick(action, eventTimeMs)) PerformPhysicalFaceAction(true, action);
        while (m_physicalO.Tick(action, eventTimeMs)) PerformPhysicalFaceAction(false, action);
    }
    if (m_modeChordFormed) {
        if (face && event.event == fxe::Button::Event::Release &&
            event.owner == m_modeChordEvent.owner) {
            const bool complete = eventTimeMs - m_modeChordStartedMs >= ENTRY_HOLD_MS;
            auto stillDown = m_physicalFaceDown;
            const uint8_t bank = event.owner == UiOwner::P2 ? 1U : 0U;
            stillDown[bank] &= static_cast<uint8_t>(~(IsXButton(event.button) ? 1U : 2U));
            CancelEntryHold();
            m_physicalFaceDown = stillDown;
            if (complete) Exit();
        }
        return;
    }
    if (event.event == fxe::Button::Event::Press && (m_pickPending || m_pickConfirmed)) {
        m_pickPending = false;
        m_pickConfirmed = false;
        PublishColorPickStatus();
    }
    if (!face) {
        ObservePhysicalButton(event);
        return;
    }
    const UiOwner owner = ResolveButtonEventOwner(event);
    if (owner == UiOwner::None) return;
    const uint8_t bank = owner == UiOwner::P2 ? 1U : 0U;
    const uint8_t bit = IsXButton(event.button) ? 1U : 2U;
    if (event.event == fxe::Button::Event::Press) {
        const uint8_t before = m_physicalFaceDown[bank];
        if (before == 0U) m_physicalFaceStartedMs[bank] = eventTimeMs;
        m_physicalFaceDown[bank] |= bit;
        if ((before & (3U ^ bit)) != 0U) {
            const uint32_t firstPress = m_physicalFaceStartedMs[bank];
            // A two-button attempt consumes both individual actions. Only the
            // established chord window qualifies exit; a late chord cannot clear art.
            m_physicalX.Reset();
            m_physicalO.Reset();
            if (eventTimeMs - firstPress <= MODE_CHORD_PRIORITY_MS) {
                m_modeChordEvent = event;
                m_modeChordPending = true;
                m_modeChordFormed = true;
                m_modeChordStartedMs = firstPress;
            }
            return;
        }
    } else if (event.event == fxe::Button::Event::Release) {
        m_physicalFaceDown[bank] &= static_cast<uint8_t>(~bit);
    }
    if (m_quickPaletteVisible && event.event == fxe::Button::Event::Press) CloseQuickPalette();
    TapHoldAction action;
    const bool x = IsXButton(event.button);
    (void)(x ? m_physicalX : m_physicalO).HandleButtonEvent(event, action, eventTimeMs);
    PerformPhysicalFaceAction(x, action);
}
void LitchcraftSession::UpdatePhysicalButtons() {
    if (m_modeChordFormed || !m_physicalButtonTicksAllowed) return;
    const uint32_t throughMs = m_physicalButtonTickTimeValid ? m_physicalButtonTickMs : fxe::Millis();
    TapHoldAction action;
    while (m_physicalX.Tick(action, throughMs)) PerformPhysicalFaceAction(true, action);
    while (m_physicalO.Tick(action, throughMs)) PerformPhysicalFaceAction(false, action);
}
void LitchcraftSession::PerformPhysicalFaceAction(const bool x, const TapHoldAction& action) {
    if (action.type == TapHoldActionType::None) return;
    Status status = Status::Ok;
    if (action.type == TapHoldActionType::Hold) {
        if (x) {
            if (ApplyFrameErase(false))
                ReportPhysicalFeedback(ControlFeedbackAction::Clear, action.owner, ControlRole::X);
            else status = FrameEraseEligible() ? Status::NoHistory : Status::Busy;
        } else {
            ChangeTool(!m_erase, m_target, m_color, 0U);
            ReportPhysicalFeedback(ControlFeedbackAction::Acknowledge,
                action.owner, ControlRole::O);
        }
    } else if (HasStreamedDocument()) {
        status = ApplyPhysicalStoreHistory(!x, action.owner);
    } else {
        status = Apply(x ? Op::Undo : Op::Redo, nullptr, 0U, 0U, false, 0U);
        if (status == Status::Ok) ReportPhysicalFeedback(x ? ControlFeedbackAction::Undo : ControlFeedbackAction::Redo,
            action.owner, x ? ControlRole::X : ControlRole::O);
    }
    m_activity = true;
    Ack(0U, status);
}
void LitchcraftSession::ObservePhysicalButton(const ButtonEvent& event) {
    if (event.event != fxe::Button::Event::Press && event.event != fxe::Button::Event::Repeat) return;
    if (event.button == UserButton::Up || event.button == UserButton::Down) {
        const int16_t step = event.event == fxe::Button::Event::Repeat ? 2 : 1;
        const int16_t delta = event.button == UserButton::Up ? step : -step;
        const uint8_t next = static_cast<uint8_t>(static_cast<int16_t>(m_colorWheel) + delta);
        ChangeTool(false, m_target, ui_color_wheel::WheelToRgb(next), 0U, false);
        m_colorWheel = next;
        if (m_quickPaletteVisible) m_quickPaletteTouchedMs = fxe::Millis();
        ReportPhysicalFeedback(ControlFeedbackAction::Acknowledge, event.owner,
            event.button == UserButton::Up ? ControlRole::Up : ControlRole::Down, m_color);
    } else if (event.button == UserButton::Left || event.button == UserButton::Right) {
        OpenOrStepQuickPalette(event);
        ReportPhysicalFeedback(ControlFeedbackAction::Navigate, event.owner,
            event.button == UserButton::Left ? ControlRole::Left : ControlRole::Right);
    } else return;
    m_activity = true;
    Ack(0U);
}
} // namespace modules
