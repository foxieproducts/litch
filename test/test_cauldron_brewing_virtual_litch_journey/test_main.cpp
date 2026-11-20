#include <cstdio>

#include "bounded_hint_engine.hpp"
#include "hardware/status_led_chain_profile.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/game_state_machine_render_state.hpp"

namespace {
using Button=runtime::VirtualLitchPhysicalButton;
int failures=0;
void Check(bool okay,const char* message) {
    if(!okay){++failures;std::printf("[FAIL] CAULDRON %s\n",message);}
}
void Edge(runtime::VirtualLitchSession& board,Button button,bool down) {
    Check(board.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,button,down)==VIRTUAL_LITCH_OK&&
        board.Advance(30U)==VIRTUAL_LITCH_OK,"raw O traverses production debounce and authority");
}
void ReplayRefinementTuple() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    for(const uint32_t elapsed:{0U,60U,119U,120U,239U,240U,300U}) {
        fxe::ManualMillis::SetNowMs(1000U);
        runtime::GameStateMachine machine;
        machine.Reset();
        machine.SetHintThinkingActive(true);
        Check(machine.GetRenderState().thinkingControls.Owner()==modules::UiOwner::None &&
            machine.GetRenderState().hintBrewing.Scale256()==0U,
            "hidden background search stays quiet without manual or Coaching visibility");
        // Coaching's existing eligible-human policy requests Hint visibility;
        // it does not manufacture a manual O request. Exercise that same owner
        // before a qualified result, then while a shown result is improving.
        machine.SetHintDisplayRequested(true);
        fxe::ManualMillis::SetNowMs(1000U+elapsed);
        const auto& pending=machine.GetRenderState();
        Check(!machine.IsManualHintRequested() && machine.IsHintThinkingActive() &&
            pending.hintBrewing.Owner()==modules::UiOwner::P1 &&
            pending.hintBrewing.ElapsedMs()==elapsed &&
            (elapsed==0U ? pending.hintBrewing.Scale256()==0U : pending.hintBrewing.Scale256()>0U),
            "automatic Coaching owns its thinking cue during the first500ms without a manual O");
        fxe::ManualMillis::SetNowMs(1500U);
        machine.SetHintMoveSquares(12U,28U);
        machine.SetHintFeedbackPending(false);
        fxe::ManualMillis::SetNowMs(1500U+elapsed);
        Check(machine.GetRenderState().hintMoveValid && machine.IsHintThinkingActive() &&
            !machine.IsManualHintRequested() &&
            machine.GetRenderState().hintBrewing.Owner()==modules::UiOwner::P1 &&
            machine.GetRenderState().hintBrewing.Scale256()==256U &&
            machine.GetRenderState().hintBrewing.ElapsedMs()==500U+elapsed,
            "a shown automatic Coaching Hint keeps the same thinking phase while its search refines");
        machine.SetHintThinkingActive(false);
        fxe::ManualMillis::AdvanceMs(200U);
        Check(machine.GetRenderState().hintBrewing.Scale256()==0U,
            "actual search termination retires automatic Coaching thinking through the existing200ms fade");
        fxe::ManualMillis::SetNowMs(1000U);
        runtime::GameStateMachineRenderState state;
        runtime::SetHintDisplayRequested(state,true);
        runtime::SetHintMoveSquares(state,1U,2U);
        runtime::SetHintDuckMoveSquares(state,3U,4U);
        fxe::ManualMillis::SetNowMs(2000U);
        runtime::SetHintMoveSquares(state,5U,6U);
        runtime::SetHintDuckMoveSquares(state,7U,8U);
        fxe::ManualMillis::SetNowMs(2000U+elapsed);
        if(elapsed>=180U) {
            runtime::SetHintMoveSquares(state,9U,10U);
            runtime::SetHintDuckMoveSquares(state,11U,12U);
        }
        runtime::ReplayHintMove(state);
        // At 240/300 ms the expired first fade is replaced by a fresh 5-to-9
        // transition, whose outgoing tuple is still the visible 5/6/7/8.
        const runtime::HintPresentationTuple expected=elapsed<120U?runtime::HintPresentationTuple{1U,2U,3U,4U}:
            runtime::HintPresentationTuple{5U,6U,7U,8U};
        Check(runtime::CurrentHintPresentation(state)==expected,
            "replay captures the visible outgoing/incoming tuple and ignores expired refinements");
        runtime::SetHintMoveSquares(state,21U,22U);runtime::SetHintDuckMoveSquares(state,23U,24U);
        Check(runtime::CurrentHintPresentation(state)==expected,
            "a newer root cannot replace the actual primary/Duck tuple during its finite replay acknowledgement");
        fxe::ManualMillis::AdvanceMs(150U);
        runtime::SetHintMoveSquares(state,21U,22U);runtime::SetHintDuckMoveSquares(state,23U,24U);
        Check(runtime::CurrentHintPresentation(state)==runtime::HintPresentationTuple{21U,22U,23U,24U},
            "the finite replay window releases normal refinements after150ms");
    }
}
void ToggleHintAndReplayCoaching(const char* style) {
    virtual_litch_config config{};virtual_litch_default_config(&config);
    config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);auto& board=fixture.session;
    board.ObserveBootHallMask(UINT64_C(0xFFFF00000000FFFF));board.Advance(3000U);
    modules::ControlMutationOutcome changed;
    char json[48]{};std::snprintf(json,sizeof(json),"\"%s\"",style);
    Check(board.MutateSettingJson(1U,"HINT_THINKING_STYLE",json,changed)==VIRTUAL_LITCH_OK&&
        (changed.status==modules::ControlMutationStatus::Applied||changed.status==modules::ControlMutationStatus::Noop),
        "the saved style reaches the existing setting authority");
    board.Advance(3000U);
    Edge(board,Button::O,true);Edge(board,Button::O,false);board.Advance(700U);
    uint8_t from=255U,to=255U;
    Check(board.HintDisplayRequested()&&board.TryGetHintMoveSquares(from,to),"real cached Hint is already shown");
    Edge(board,Button::O,true);Edge(board,Button::O,false);board.Advance(75U);
    Check(!board.HintDisplayRequested(),"second raw O hides the manual Hint when Coaching is off");
    Edge(board,Button::O,true);Edge(board,Button::O,false);board.Advance(220U);
    Check(board.HintDisplayRequested()&&board.TryGetHintMoveSquares(from,to),
        "third raw O reveals a valid manual Hint again without another search request");
    Check(board.MutateSettingJson(2U,"P1_AUTO_HINTS","true",changed)==VIRTUAL_LITCH_OK,
        "existing Coaching setting admits its saved preference");
    Check(board.SettingsForPersistenceTest().ReadValue("P1_AUTO_HINTS",false),
        "the actual Coaching journey retains its selected saved preference");
    board.Advance(220U);
    Edge(board,Button::O,true);Edge(board,Button::O,false);board.Advance(75U);
    uint8_t replayFrom=255U,replayTo=255U;
    Check(board.HintDisplayRequested()&&board.TryGetHintMoveSquares(replayFrom,replayTo)&&
        from==replayFrom&&to==replayTo,"raw O replays the visible Hint when Coaching is on");
    const auto o=board.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(false,hardware::ControlLedRole::O)];
    Check(o.r||o.g||o.b,"accepted shown retap gives a finite O brew for every style including None");
    std::printf("[CAULDRON_JOURNEY] stage=manual-on-off-on-coach-replay style=%s occupancy=%016llx requested=%u shown=%u>%u replay=%u>%u O=%u/%u/%u next=follow-same-shown-hint\n",
        style,static_cast<unsigned long long>(board.State().physical_occupied_mask),board.HintDisplayRequested(),from,to,replayFrom,replayTo,o.r,o.g,o.b);
}
}
int main(){ReplayRefinementTuple();for(const auto style:{"none","player_clockwise","foxie_sweep","beachball","cauldron_brewing"})ToggleHintAndReplayCoaching(style);return failures==0?0:1;}
