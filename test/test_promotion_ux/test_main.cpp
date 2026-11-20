#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/led_text_renderer.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "modules/promotion_ux.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

class QueueUserInput final : public modules::IUserInput {
  public:
    void Push(const modules::ButtonEvent& evt) {
        if (m_count >= MAX_EVENTS) {
            std::printf("[FAIL] queue overflow\n");
            ++g_failures;
            return;
        }
        m_events[m_tail] = evt;
        m_tail = (m_tail + 1) % MAX_EVENTS;
        ++m_count;
    }

    bool TryReadButtonEvent(modules::ButtonEvent& outEvent) override {
        if (m_count == 0) {
            return false;
        }

        outEvent = m_events[m_head];
        m_head = (m_head + 1) % MAX_EVENTS;
        --m_count;
        return true;
    }

  private:
    static constexpr size_t MAX_EVENTS = 32;
    modules::ButtonEvent m_events[MAX_EVENTS] = {};
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;
};

} // namespace

int main() {
    std::puts("[PROMOTION_UX] start");
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);

    modules::PromotionUxConfig cfg;
    cfg.inactivityTimeoutMs = 200;

    modules::PromotionUx ux(cfg);
    QueueUserInput input;

    constexpr modules::PromotionPiece PIECES[] = {
        modules::PromotionPiece::Queen,
        modules::PromotionPiece::Rook,
        modules::PromotionPiece::Bishop,
        modules::PromotionPiece::Knight,
    };
    constexpr char SUFFIXES[] = {'q', 'r', 'b', 'n'};
    constexpr char LEGACY_LETTERS[] = {'Q', 'R', 'B', 'N'};
    for (size_t index = 0U; index < 4U; ++index) {
        modules::PromotionPiece parsed = modules::PromotionPiece::Queen;
        Check(modules::promotion_piece_cue::TryPieceFromSuffix(
                  SUFFIXES[index], parsed) &&
                  parsed == PIECES[index],
              "CPU suffix selects the matching chess-piece silhouette");

        const uint64_t iconMask =
            modules::promotion_piece_cue::BoardMask(PIECES[index]);
        const char letter[2] = {LEGACY_LETTERS[index], '\0'};
        const uint64_t letterMask =
            modules::led_text_renderer::RasterizeTextMask(
                letter, 2U, 1U,
                modules::led_text_renderer::FontRole::Readable);
        Check(iconMask != 0ULL && iconMask != letterMask,
              "promotion cue is a chess-piece silhouette, not a letter glyph");
        for (size_t prior = 0U; prior < index; ++prior) {
            Check(iconMask != modules::promotion_piece_cue::BoardMask(
                                  PIECES[prior]),
                  "each promotion choice has a distinct silhouette");
        }
    }
    modules::PromotionPiece invalidSuffixPiece =
        modules::PromotionPiece::Queen;
    Check(!modules::promotion_piece_cue::TryPieceFromSuffix(
              'x', invalidSuffixPiece),
          "unknown CPU suffix has no promotion silhouette");

    // Start + basic selection cycling.
    fxe::ManualMillis::SetNowMs(0);
    ux.Start();
    Check(ux.IsActive(), "active after start");
    Check(ux.GetSelection() == modules::PromotionPiece::Queen, "default queen");

    input.Push({modules::UserButton::Right, fxe::Button::Event::Press});
    modules::PromotionChoice choice;
    fxe::ManualMillis::SetNowMs(10);
    Check(!ux.Tick(input, choice), "right does not finish");
    Check(ux.GetSelection() == modules::PromotionPiece::Rook, "right selects rook");

    input.Push({modules::UserButton::Right, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(11);
    Check(!ux.Tick(input, choice), "right2 does not finish");
    Check(ux.GetSelection() == modules::PromotionPiece::Bishop, "right selects bishop");

    input.Push({modules::UserButton::Right, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(12);
    Check(!ux.Tick(input, choice), "right3 does not finish");
    Check(ux.GetSelection() == modules::PromotionPiece::Knight, "right selects knight");

    // Left cycles back.
    input.Push({modules::UserButton::Left, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(13);
    Check(!ux.Tick(input, choice), "left does not finish");
    Check(ux.GetSelection() == modules::PromotionPiece::Bishop, "left selects bishop");

    // Up/Down ignored.
    input.Push({modules::UserButton::Down, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(14);
    Check(!ux.Tick(input, choice), "down ignored");
    Check(ux.GetSelection() == modules::PromotionPiece::Bishop, "down no change");

    input.Push({modules::UserButton::Up, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(15);
    Check(!ux.Tick(input, choice), "up ignored");
    Check(ux.GetSelection() == modules::PromotionPiece::Bishop, "up no change");

    // Confirm via P1O.
    input.Push({modules::UserButton::P1O, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(16);
    Check(ux.Tick(input, choice), "P1O confirms");
    Check(!ux.IsActive(), "inactive after confirm");
    Check(choice.piece == modules::PromotionPiece::Bishop, "choice bishop");
    Check(!choice.wasCancelled, "not cancelled");
    Check(!choice.wasTimeout, "not timeout");

    // Confirm via P2O (logical O).
    ux.Start();
    Check(ux.IsActive(), "active after restart");

    input.Push({modules::UserButton::Right, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(30);
    Check(!ux.Tick(input, choice), "restart: right does not finish");
    Check(ux.GetSelection() == modules::PromotionPiece::Rook, "restart: right selects rook");

    input.Push({modules::UserButton::P2O, fxe::Button::Event::Press});
    fxe::ManualMillis::SetNowMs(31);
    Check(ux.Tick(input, choice), "P2O confirms");
    Check(!ux.IsActive(), "inactive after P2O confirm");
    Check(choice.piece == modules::PromotionPiece::Rook, "choice rook");
    Check(!choice.wasCancelled, "not cancelled (P2O)");
    Check(!choice.wasTimeout, "not timeout (P2O)");

    // Timeout auto-queen.
    fxe::ManualMillis::SetNowMs(2000);
    ux.Start();
    fxe::ManualMillis::SetNowMs(2100);
    Check(!ux.Tick(input, choice), "before timeout no result");
    fxe::ManualMillis::SetNowMs(2200);
    Check(ux.Tick(input, choice), "timeout fires");
    Check(choice.piece == modules::PromotionPiece::Queen, "timeout => queen");
    Check(choice.wasTimeout, "timeout flagged");

    // Production defaults must wait for an explicit physical confirmation.
    modules::PromotionUx defaultUx;
    fxe::ManualMillis::SetNowMs(3000);
    defaultUx.Start();
    fxe::ManualMillis::SetNowMs(13000);
    Check(!defaultUx.Tick(input, choice),
          "default promotion does not silently choose after 10 seconds");
    fxe::ManualMillis::SetNowMs(63000);
    Check(!defaultUx.Tick(input, choice),
          "default promotion continues waiting for the player");
    input.Push({modules::UserButton::P1O, fxe::Button::Event::Press});
    Check(defaultUx.Tick(input, choice),
          "default promotion completes on explicit Circle confirmation");
    Check(choice.piece == modules::PromotionPiece::Queen && !choice.wasTimeout,
          "explicit default choice is not a timeout");

    if (g_failures == 0) {
        std::puts("[PROMOTION_UX] ok");
        return 0;
    }

    std::printf("[PROMOTION_UX] failures=%d\n", g_failures);
    return 1;
}
