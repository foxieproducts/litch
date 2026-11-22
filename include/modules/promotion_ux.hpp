#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"
#include "modules/user_input.hpp"

namespace modules {

enum class PromotionPiece : uint8_t {
    Queen = 0,
    Rook = 1,
    Bishop = 2,
    Knight = 3,
};

inline const char* PromotionPieceToString(const PromotionPiece piece) {
    switch (piece) {
        case PromotionPiece::Queen:
            return "Queen";
        case PromotionPiece::Rook:
            return "Rook";
        case PromotionPiece::Bishop:
            return "Bishop";
        case PromotionPiece::Knight:
            return "Knight";
    }

    return "?";
}

struct PromotionUxConfig {
    // Choose mode requires confirmation; automatic Queen is a coordinator policy.
    // Tests or a future explicitly approved product policy may opt into a
    // deterministic timeout by setting a non-zero value.
    uint32_t inactivityTimeoutMs = 0U;

    // Which button confirms selection.
    // On the prototype, P1 O is the dedicated confirm/select action.
    UserButton confirmButton = UserButton::P1O;

    // In promotion modal, consume unhandled inputs so other systems do not see them.
    bool consumeUnhandledEvents = true;
};

struct PromotionChoice {
    PromotionPiece piece = PromotionPiece::Queen;

    // True if the choice was made via explicit cancel rather than confirm.
    bool wasCancelled = false;

    // True if inactivity timeout fired; choice is queen.
    bool wasTimeout = false;
};

// Promotion selection UX state machine.
//
// Behaviors:
// - Left/Right adjusts selection.
// - Either O button confirms the current piece.
// - Up/Down are ignored.
// - Optional inactivity timeout can auto-select queen.
class PromotionUx final {
  public:
    explicit PromotionUx(const PromotionUxConfig& config = {}) : m_config(config) {}

    void Start(const PromotionPiece selection = PromotionPiece::Queen) {
        m_active = true;
        m_selection = selection;
        m_lastInteractionElapsed.Reset();
    }

    void Cancel() {
        m_active = false;
    }

    bool IsActive() const { return m_active; }

    PromotionPiece GetSelection() const { return m_selection; }
    uint32_t ElapsedMs() const { return m_lastInteractionElapsed.ElapsedMs(); }

    // \brief Process input + time and optionally produce a one-shot choice.
    // \returns true if outChoice was produced and the modal deactivated.
    bool Tick(IUserInput& input, PromotionChoice& outChoice) {
        if (!m_active) {
            return false;
        }

        if (m_config.inactivityTimeoutMs != 0 &&
            m_lastInteractionElapsed.HasElapsed(m_config.inactivityTimeoutMs)) {
            outChoice = PromotionChoice{PromotionPiece::Queen, false, true};
            m_active = false;
            return true;
        }

        ButtonEvent evt;
        while (input.TryReadButtonEvent(evt)) {
            const bool pressLike = (evt.event == fxe::Button::Event::Press) || (evt.event == fxe::Button::Event::Repeat);
            if (!pressLike) {
                if (!m_config.consumeUnhandledEvents) {
                    return false;
                }
                continue;
            }

            if (evt.button == UserButton::Left) {
                StepSelection(-1);
                m_lastInteractionElapsed.Reset();
                continue;
            }

            if (evt.button == UserButton::Right) {
                StepSelection(+1);
                m_lastInteractionElapsed.Reset();
                continue;
            }

            if (evt.button == UserButton::Up || evt.button == UserButton::Down) {
                // Ignored.
                m_lastInteractionElapsed.Reset();
                continue;
            }

            const bool confirmIsO = modules::IsOButton(m_config.confirmButton);
            const bool eventIsConfirm = confirmIsO ? modules::IsOButton(evt.button) : (evt.button == m_config.confirmButton);
            if (eventIsConfirm) {
                outChoice = PromotionChoice{m_selection, false, false};
                m_active = false;
                return true;
            }

            // Unhandled input.
            if (!m_config.consumeUnhandledEvents) {
                return false;
            }
        }

        return false;
    }

  private:
    void StepSelection(const int delta) {
        int idx = static_cast<int>(m_selection);
        idx += delta;

        constexpr int PIECE_COUNT = 4;
        while (idx < 0) {
            idx += PIECE_COUNT;
        }
        while (idx >= PIECE_COUNT) {
            idx -= PIECE_COUNT;
        }

        m_selection = static_cast<PromotionPiece>(idx);
    }

    PromotionUxConfig m_config;

    bool m_active = false;
    PromotionPiece m_selection = PromotionPiece::Queen;
    fxe::ElapsedTime m_lastInteractionElapsed;
};

} // namespace modules
