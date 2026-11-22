#include <cstdio>

#include "runtime/app_event.hpp"
#include "runtime/app_event_dispatch.hpp"
#include "runtime/clock.hpp"
#include "runtime/event_bus.hpp"
#include "runtime/game_state_machine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[GAME_STATE_MACHINE_PROMOTION] start");

#ifndef ARDUINO
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    runtime::ManualClock clock(1000);
    fxe::ManualMillis::SetNowMs(clock.NowMs());

    runtime::GameStateMachineConfig cfg;
    cfg.cpuIdleDelayMs = 999999;
    cfg.promotionUx.inactivityTimeoutMs = 200;

    runtime::GameStateMachine sm(cfg);
    sm.Reset();

    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "starts idle");

    // Start promotion modal.
    Check(sm.StartPromotion(), "StartPromotion succeeds");
    Check(sm.IsPromotionActive(), "state Promotion");
    Check(sm.GetPromotionSelection() == modules::PromotionPiece::Queen, "default queen");

    // Feed a Right press via the AppEvent dispatcher (integration with event bus).
    runtime::EventBus<8> bus;
    bus.Reset();
    (void)bus.Push(runtime::AppEvent::MakeButton(clock.NowMs(), modules::UserButton::Right, fxe::Button::Event::Press));

    runtime::AppEvent e;
    while (bus.TryPop(e)) {
        runtime::DispatchAppEventToStateMachine(sm, e);
    }

    Check(sm.GetPromotionSelection() == modules::PromotionPiece::Rook, "right selects rook");

    // Confirm with P1O.
    sm.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);

    modules::PromotionChoice choice;
    Check(sm.ConsumePromotionChoice(choice), "choice produced");
    Check(choice.piece == modules::PromotionPiece::Rook, "choice rook");
    Check(!choice.wasCancelled, "not cancelled");
    Check(!choice.wasTimeout, "not timeout");
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "returns to idle");

    // Underpromotion choice (Bishop): Q -> R -> B.
    Check(sm.StartPromotion(), "StartPromotion for underpromotion");
    sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
    sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
    Check(sm.GetPromotionSelection() == modules::PromotionPiece::Bishop, "right selects bishop");
    sm.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    Check(sm.ConsumePromotionChoice(choice), "bishop choice produced");
    Check(choice.piece == modules::PromotionPiece::Bishop, "bishop choice bishop");
    Check(!choice.wasCancelled, "bishop not cancelled");
    Check(!choice.wasTimeout, "bishop not timeout");
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "bishop returns idle");

    // Handler restore / stale input isolation: Right pressed while idle must not
    // affect the next promotion modal.
    sm.HandleButtonEvent(modules::UserButton::Right, fxe::Button::Event::Press);
    Check(sm.StartPromotion(), "StartPromotion after idle Right");
    sm.Tick();
    Check(sm.GetPromotionSelection() == modules::PromotionPiece::Queen, "stale idle Right ignored");

    sm.HandleButtonEvent(modules::UserButton::P1O, fxe::Button::Event::Press);
    Check(sm.ConsumePromotionChoice(choice), "stale isolation confirm produced");
    Check(choice.piece == modules::PromotionPiece::Queen, "stale isolation confirm => queen");
    Check(!choice.wasTimeout, "stale isolation confirm not timeout");
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "stale isolation confirm returns idle");

    // Timeout path defaults to queen.
    Check(sm.StartPromotion(), "StartPromotion again");
    clock.AdvanceMs(201);
    sm.Tick();

    Check(sm.ConsumePromotionChoice(choice), "timeout choice produced");
    Check(choice.piece == modules::PromotionPiece::Queen, "timeout => queen");
    Check(choice.wasTimeout, "timeout flagged");
    Check((sm.GetChessInteractionModel().phase.tag == runtime::ChessInteractionPhase::Idle), "timeout returns idle");
#else
    std::puts("[GAME_STATE_MACHINE_PROMOTION] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[GAME_STATE_MACHINE_PROMOTION] ok");
        return 0;
    }

    std::printf("[GAME_STATE_MACHINE_PROMOTION] failures=%d\n", g_failures);
    return 1;
}
