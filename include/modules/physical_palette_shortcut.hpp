#pragma once

#include "modules/control_mutation_service.hpp"
#include "modules/control_led_feedback.hpp"
#include "modules/settings_store.hpp"
#include "modules/user_input.hpp"
#include "foxie_embedded/millis.hpp"

namespace modules::physical_palette_shortcut {

// A missed release cannot leave cosmetic ownership latched after recovery.
class EditPreview {
  public:
    void SetFeedback(ControlLedFeedback* feedback) { m_feedback = feedback; }
    void Applied(const ButtonEvent& event, Rgb color) {
        if (m_feedback != nullptr) m_feedback->Trigger(ControlFeedbackAction::PaletteEdit,
            event.owner, event.button == UserButton::Up ? ControlRole::Up : ControlRole::Down, color);
    }
    void Observe(const ButtonEvent& event, bool pressLike) {
        if (event.owner == UiOwner::None) return;
        const uint8_t bank = event.owner == UiOwner::P2 ? 1U : 0U;
        const uint8_t bit = event.button == UserButton::Up ? 1U : 2U;
        if (pressLike) { m_held[bank] |= bit; m_last[bank] = fxe::Millis(); }
        else if (event.event == fxe::Button::Event::Release) m_held[bank] &= ~bit;
    }
    uint8_t VisibleOwners() const {
        uint8_t mask = 0U;
        for (uint8_t bank = 0U; bank < 2U; ++bank) {
            if (m_held[bank] != 0U &&
                static_cast<uint32_t>(fxe::Millis() - m_last[bank]) < 900U)
                mask |= static_cast<uint8_t>(1U << bank);
        }
        return mask;
    }
    void Reset() { m_held[0] = m_held[1] = 0U; m_last[0] = m_last[1] = 0U; }
  private:
    ControlLedFeedback* m_feedback = nullptr;
    uint8_t m_held[2]{};
    uint32_t m_last[2]{};
};

// Handles the outside-overlay Up/Down color shortcut. The incoming event owner
// is gameplay-normalized, so its logical-player alias selects the palette
// currently visible at the pressed controls, including after literal rotation.
[[nodiscard]] bool TryApply(const modules::ButtonEvent& event,
                            bool pressLike,
                            fxe::SettingsStore& settings,
                            modules::ControlMutationService& controlMutation,
                            EditPreview* preview = nullptr);

} // namespace modules::physical_palette_shortcut
