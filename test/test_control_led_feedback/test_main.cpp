#include <array>
#include <cstdio>

#include "foxie_embedded/millis.hpp"
#include "modules/control_led_feedback.hpp"
#include "modules/control_led_thinking_feedback.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/brightness_scale_policy.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/control_led_feedback_projection.hpp"
#include "modules/hint_button_gesture_controller.hpp"

namespace {
using namespace modules;
using Overlay = status_led_frame_adapter::FixedPhysicalStatusOverlay;
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] CONTROL_FEEDBACK %s\n", message); }
}
unsigned Energy(const Rgb c) { return unsigned(c.r)+c.g+c.b; }
Rgb Pixel(const Overlay& o, UiOwner owner, ControlRole role) {
    return o.controlPixels[(owner==UiOwner::P2?6U:0U)+static_cast<uint8_t>(role)];
}
void RunPressedAcknowledgmentPreservesThinking() {
    for(const uint8_t alpha:{64U,128U,255U})
    for(const auto owner:{UiOwner::P1,UiOwner::P2})
    for(const auto style:{hint_thinking_style_setting::Style::Beachball,
        hint_thinking_style_setting::Style::PlayerClockwise,hint_thinking_style_setting::Style::FoxieSweep})
    for(const auto pressed:{ControlRole::Up,ControlRole::Down}) {
        fxe::ManualMillis::SetNowMs(1000U);
        ControlLedFeedback feedback;
        feedback.Trigger(ControlFeedbackAction::Acknowledge,owner,pressed,{0U,255U,0U});
        for(const uint32_t elapsed:{0U,75U,150U}) {
            fxe::ManualMillis::SetNowMs(1000U+elapsed);
            Overlay before;
            ComposeControlLedThinkingFeedback({owner,137U+elapsed,style,{20U,190U,90U}},before);
            before.controlAlpha255[owner==UiOwner::P2?1U:0U]=alpha;
            auto after=before;feedback.Compose({},after);
            for(const bool swapped:{false,true}) {
                hardware::PhysicalLedFrame base{},actual{};
                hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,before,base,swapped);
                hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,after,actual,swapped);
                const auto changed=256U+hardware::V3_3PhysicalIndexForLogicalControl(owner==UiOwner::P2,
                    static_cast<hardware::ControlLedRole>(pressed),swapped);
                for(size_t index=0U;index<actual.size();++index)if(index!=changed)
                    Check(actual[index].r==base[index].r&&actual[index].g==base[index].g&&actual[index].b==base[index].b,
                        "pressed palette ACK preserves every other mapped thinking emitter at peak, midpoint and expiry");
            }
        }
    }
    for(const uint8_t alpha:{127U,129U}) {
        fxe::ManualMillis::SetNowMs(2000U);
        ControlLedFeedback feedback;
        feedback.Trigger(ControlFeedbackAction::Acknowledge,UiOwner::P1,ControlRole::Up,{0U,255U,0U});
        Overlay partial;partial.controlMask=1U<<static_cast<uint8_t>(ControlRole::Right);
        partial.controlPixels[static_cast<uint8_t>(ControlRole::Right)]={130U,86U,0U};
        partial.controlAlpha255[0]=alpha;
        const auto expected=AlphaBlendRgb({},Pixel(partial,UiOwner::P1,ControlRole::Right),alpha);
        feedback.Compose({},partial);
        const auto actual=Pixel(partial,UiOwner::P1,ControlRole::Right);
        Check(actual.r==expected.r&&actual.g==expected.g&&actual.b==expected.b,
            "fractional-alpha127/129 palette ACK retains the exact130/86 emitter codes; oldQ8 loses one intensity code");
    }

}
void RunFeedback() {
    fxe::ManualMillis::SetNowMs(100U);
    ControlLedFeedback feedback;
    ControlLedFeedbackInputs inputs;
    Overlay out;
    feedback.Compose(inputs,out);
    Check(out.controlMask==0U,"ordinary idle controls are off");
    feedback.Trigger(ControlFeedbackAction::Undo,UiOwner::P1,ControlRole::X);
    out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::Right))>0U &&
          Energy(Pixel(out,UiOwner::P1,ControlRole::Left))==0U &&
          (out.controlMask&0xFC0U)==0U,"Undo begins on owning Right without touching other bank");
    fxe::ManualMillis::SetNowMs(190U); out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::Left))>0U,"Undo travels toward Left at midpoint");
    feedback.Trigger(ControlFeedbackAction::Acknowledge,UiOwner::P2,ControlRole::Up,{255U,0U,0U});
    feedback.Trigger(ControlFeedbackAction::Acknowledge,UiOwner::P2,ControlRole::Down,{0U,255U,0U});
    out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P2,ControlRole::Up))==0U &&
          Pixel(out,UiOwner::P2,ControlRole::Down).g>0U,
          "newest event replaces earlier transient independently on P2");
    fxe::ManualMillis::SetNowMs(500U); out={}; feedback.Compose(inputs,out);
    Check(out.controlMask==0U,"all transient effects expire without a queue");
    inputs.coaching={true,false}; out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::O))>0U &&
          Energy(Pixel(out,UiOwner::P2,ControlRole::O))==0U,
          "actual per-player Coaching owns only its O indicator");
    inputs.coaching={false,true}; out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::O))==0U &&
          Energy(Pixel(out,UiOwner::P2,ControlRole::O))>0U,
          "app-originated state changes replace persistent indicators");
    inputs.holds[0][0]={ControlRole::X,199U,1000U,true}; out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::X))==0U,"hold waits past short-tap delay");
    inputs.holds[0][0].elapsedMs=600U; out={}; feedback.Compose(inputs,out);
    const unsigned middle=Energy(Pixel(out,UiOwner::P1,ControlRole::X));
    inputs.holds[0][0].elapsedMs=999U; out={}; feedback.Compose(inputs,out);
    Check(middle>0U && Pixel(out,UiOwner::P1,ControlRole::X).r>0U &&
          Pixel(out,UiOwner::P1,ControlRole::X).g<3U,"destructive progress approaches red at threshold");
    inputs.holds={}; out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::X))==0U,"cancelled hold has no latched progress");
    inputs.scene=ControlFeedbackScene::Suppressed; out={}; feedback.Compose(inputs,out);
    Check(out.controlMask==0U,"higher priority owner suppresses and retires feedback");
    inputs.scene=ControlFeedbackScene::Play; inputs.coaching={true,false};
    out={}; out.controlMask=1U<<1U; out.controlPixels[1]={200U,0U,0U};
    out.controlAlpha255[0]=128U; feedback.Compose(inputs,out);
    Check(out.controlPixels[1].r>=99U && out.controlPixels[1].r<=101U,
          "adding status preserves the existing thinking fade rather than resetting its alpha");
    inputs.minimalAnimations=true; inputs.coaching={};
    feedback.Trigger(ControlFeedbackAction::Undo,UiOwner::P1,ControlRole::X);
    out={}; feedback.Compose(inputs,out);
    Check(Energy(Pixel(out,UiOwner::P1,ControlRole::Left))==0U &&
          Energy(Pixel(out,UiOwner::P1,ControlRole::Right))==0U &&
          Energy(Pixel(out,UiOwner::P1,ControlRole::X))>0U,
          "Minimal Animations uses a static history acknowledgment");
    inputs.minimalAnimations=false;
    inputs.scene=ControlFeedbackScene::Litchcraft;
    inputs.availableControls = {1U << static_cast<uint8_t>(ControlRole::O), 0U};
    out={}; feedback.Compose(inputs,out);
    const auto redo = Pixel(out,UiOwner::P1,ControlRole::O);
    Check(redo.r>0U && redo.r==redo.g && redo.g==redo.b &&
          Energy(Pixel(out,UiOwner::P2,ControlRole::O))==0U,
          "only actual Litchcraft history availability drives neutral persistent controls");
    for(unsigned feature=0U;feature<6U;++feature) {
        ControlLedFeedbackConfig cfg;
        if(feature==0U)cfg.enabled=false;
        if(feature==1U)cfg.persistent=false;
        if(feature==2U)cfg.confirmations=false;
        if(feature==3U)cfg.holdProgress=false;
        if(feature==4U)cfg.historySweeps=false;
        if(feature==5U)cfg.menuGuidance=false;
        ControlLedFeedback disabled(cfg);
        ControlLedFeedbackInputs in;
        if(feature<=1U)in.coaching[0]=true;
        if(feature==0U||feature==2U||feature==4U)
            disabled.Trigger(ControlFeedbackAction::Undo,UiOwner::P1,ControlRole::X);
        if(feature==3U)in.holds[0][0]={ControlRole::X,900U,1000U,true};
        if(feature==5U){in.scene=ControlFeedbackScene::QuickSetup;in.availableControls={0x3FU,0x3FU};}
        Overlay frame;disabled.Compose(in,frame);
        if(feature==5U){fxe::ManualMillis::SetNowMs(700U);frame={};disabled.Compose(in,frame);}
        if(feature<=3U||feature==5U)
            Check(frame.controlMask==0U,"disabled cosmetic effect contributes no illumination");
        if(feature==4U)
            Check(Energy(Pixel(frame,UiOwner::P1,ControlRole::X))>0U&&
                Energy(Pixel(frame,UiOwner::P1,ControlRole::Left))==0U&&
                Energy(Pixel(frame,UiOwner::P1,ControlRole::Right))==0U,
                "disabled directional effect retains only its button acknowledgment");
    }
}

void RunCompletionAuthority() {
    struct Context {
        ControlLedFeedback feedback;
        bool complete=false;
        bool rejected=false;
        ControlActionState phase=ControlActionState::Idle;
        unsigned observed=0U;
        UiOwner owner=UiOwner::None;
        ControlActionState initial=ControlActionState::Idle;
    } context;
    fxe::SettingsStoreConfig config;config.path="unused_rgb_completion.json";
    fxe::SettingsStore settings(config);
    ControlMutationCallbacks cb;cb.context=&context;
    cb.requestUndo=[](void* c,uint16_t,uint8_t) {
        auto& x=*static_cast<Context*>(c);
        return x.rejected?ControlCoordinatorResult::NoHistory:ControlCoordinatorResult::Pending;
    };
    cb.pollAction=[](void* c,uint16_t) {
        return static_cast<Context*>(c)->complete?ControlCoordinatorResult::Applied:ControlCoordinatorResult::Pending;
    };
    cb.getActionState=[](void* c){return static_cast<Context*>(c)->phase;};
    cb.actionCompleted=[](void* c,const ControlMutationOutcome& outcome,UiOwner owner,ControlActionState initial) {
        auto& x=*static_cast<Context*>(c);++x.observed;x.owner=owner;x.initial=initial;
        ObserveControlLedActionFeedback(x.feedback,outcome,owner,initial);
    };
    ControlMutationService service;service.Configure(settings,cb);
    auto accepted=service.MutateAction(ControlAction::Undo,41U,UiOwner::P2);
    Check(accepted.status==ControlMutationStatus::Accepted&&context.observed==0U,
        "accepted Undo does not emit success");
    service.Update();Check(context.observed==0U,"pending Undo still has no success");
    context.complete=true;service.Update();service.Update();
    Check(context.observed==1U&&context.owner==UiOwner::P2&&context.initial==ControlActionState::Idle,
        "terminal Undo retains original player and emits exactly once");
    ControlMutationOutcome completed;
    Check(service.TryTakeCompletion(41U,completed)&&completed.status==ControlMutationStatus::Applied,
        "feedback observer does not consume the real correlated client receipt");
    context.phase=ControlActionState::UndoPending;
    accepted=service.MutateAction(ControlAction::Undo,42U,UiOwner::P1);
    service.Update();service.Update();
    Check(context.observed==2U&&context.initial==ControlActionState::UndoPending,
        "cancel Undo retains its original pending context");
    ControlLedFeedbackInputs input;Overlay frame;context.feedback.Compose(input,frame);
    Check(Energy(Pixel(frame,UiOwner::P1,ControlRole::Right))==0U&&
          Pixel(frame,UiOwner::P1,ControlRole::X).r>Pixel(frame,UiOwner::P1,ControlRole::X).b,
        "cancel Undo acknowledges cancellation without a false Undo sweep");
    Check(service.TryTakeCompletion(42U,completed),"cancel receipt remains available");
    context.rejected=true;context.phase=ControlActionState::Idle;
    accepted=service.MutateAction(ControlAction::Undo,43U,UiOwner::P1);service.Update();
    Check(context.observed==2U&&service.TryTakeCompletion(43U,completed)&&
          completed.status==ControlMutationStatus::Rejected,
        "unavailable history produces rejection receipt and no success feedback");
}

void RunMappedOutputEnvelope() {
    using Frame=hardware::PhysicalLedFrame;
    Overlay roles;roles.controlMask=0xFFFU;
    for(unsigned index=0U;index<12U;++index)roles.controlPixels[index]={uint8_t(index+1U),0U,0U};
    for(bool swapped:{false,true}) {
        Frame frame{};
        hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,roles,frame,swapped);
        for(unsigned role=0U;role<6U;++role) {
            Check(frame[256U+26U+role].r==(swapped?role+7U:role+1U)&&
                  frame[256U+4U+role].r==(swapped?role+1U:role+7U),
                "all twelve semantic RGB roles retain the documented physical chain and local directions");
        }
    }
    class Renderer final:public ILedRenderer {
      public:
        std::array<Rgb,hardware::MAX_PHYSICAL_LED_COUNT> pixels{};
        uint16_t GetLedCount()const override{return 300U;}
        void Clear()override{pixels={};}
        void SetLedRgb(uint16_t i,uint8_t r,uint8_t g,uint8_t b)override{pixels[i]={r,g,b};}
        void Show()override{}
    } renderer;
    ControlLedFeedback feedback;ControlLedFeedbackInputs input;input.coaching={true,true};
    Overlay cue;feedback.Compose(input,cue);Frame frame{};
    hardware::ApplyFixedPhysicalStatusOverlay(hardware::BoardRevision::V3_3S3,cue,frame,false);
    auto cfg=led_render_policy::MakePipelineConfig(false,32U);
    cfg.outputBrightnessScale256=128U;cfg.channelMaxCurrentMa=20U;
    PowerBudgetPolicy policy(led_render_policy::MakePowerBudgetPolicyConfig({true,false}));
    BudgetAwareLedPipeline pipeline(renderer,policy,cfg);
    for(bool automatic:{false,true}) {
        for(uint32_t level:{0U,1U,5U,8U}) {
            unsigned prior=0U;
            for(float ambient:{0.0F,0.5F,1.0F}) {
                const auto scale=brightness_scale_policy::ComputeEffectiveScale256(
                    level,true,ambient,256U,automatic);
                pipeline.SetBrightnessScale01(float(scale)/256.0F);
                pipeline.RenderFrame(frame.data(),300U);
                const auto out=renderer.pixels[282U];const auto energy=Energy(out);
                Check(energy>=prior,"ambient/manual scaling stays within the existing monotonic output envelope");
                prior=energy;
                if(level==0U)Check(energy==0U,"optional3percent status may quantize off in near-dark mode");
                if(ambient==0.0F||ambient==1.0F)
                    std::printf("[RGB_FEEDBACK_OUTPUT] automatic=%u level=%u ambient=%.1f scale=%u O=%u,%u,%u current_mA=%u\n",
                        automatic,level,ambient,scale,out.r,out.g,out.b,pipeline.GetLastStats().appliedLedCurrentMa);
            }
        }
    }
}
void RunEstablishedGestureCorpus() {
    using Type=HintButtonActionType;
    for(const auto owner:{UiOwner::P1,UiOwner::P2}) {
        const auto button=owner==UiOwner::P2?UserButton::P2O:UserButton::P1O;
        HintButtonGestureController hint;
        HintButtonAction action;
        fxe::ManualMillis::SetNowMs(100U);
        Check(hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action) &&
              action.type==Type::None,"Hint press retains established delayed tap semantics");
        fxe::ManualMillis::SetNowMs(1099U);
        Check(!hint.Tick(action),"Hint remains below threshold at999ms");
        hint.HandleButtonEvent({button,fxe::Button::Event::Release,owner},action);
        Check(action.type==Type::ToggleHint && action.owner==owner,"early release remains one ordinary Hint tap");
        fxe::ManualMillis::SetNowMs(2000U);
        hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action);
        fxe::ManualMillis::SetNowMs(3000U);
        Check(hint.Tick(action)&&action.type==Type::BeginnerModeHold&&action.owner==owner,
              "Hint recognizes the same exact1000ms hold boundary");
        Check(!hint.Tick(action),"Hint hold never repeats");
        hint.HandleButtonEvent({button,fxe::Button::Event::Release,owner},action);
        Check(action.type==Type::None,"Hint hold release never also taps");
        hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action);
        hint.Reset();
        hint.HandleButtonEvent({button,fxe::Button::Event::Release,owner},action);
        Check(action.type==Type::None&&!hint.IsHeld(owner),"Hint cancellation retires held state");

        fxe::ManualMillis::SetNowMs(4000U);
        hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action,4000U);
        fxe::ManualMillis::SetNowMs(4400U);
        hint.Cancel(owner);
        hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action,4000U);
        Check(!hint.IsHeld(owner)&&action.type==Type::None,
              "CPU cancellation rejects a chord-buffered older Press");
        hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action,4400U);
        Check(!hint.IsHeld(owner),"same-timestamp Press cannot cross the cancellation barrier");
        fxe::ManualMillis::SetNowMs(5400U);
        Check(!hint.Tick(action),"cancelled buffered Press cannot fire a late Coaching hold");
        hint.HandleButtonEvent({button,fxe::Button::Event::Release,owner},action,5400U);
        Check(action.type==Type::None,"cancelled release cannot become a Hint tap");
        fxe::ManualMillis::SetNowMs(5500U);
        hint.HandleButtonEvent({button,fxe::Button::Event::Press,owner},action,5500U);
        fxe::ManualMillis::SetNowMs(6500U);
        Check(hint.Tick(action)&&action.type==Type::BeginnerModeHold&&action.owner==owner,
              "fresh post-cancellation Press retains the original one-second hold");
        hint.HandleButtonEvent({button,fxe::Button::Event::Release,owner},action,6500U);
        Check(action.type==Type::None,"recovered hold release never also taps");
    }
    std::printf("[RGB_CONTROL_MEMORY] feedback=%zu tap_hold=%zu hint_wrapper=%zu mutation_callbacks=%zu\n",
                sizeof(ControlLedFeedback),sizeof(TapHoldGestureController),
                sizeof(HintButtonGestureController),sizeof(ControlMutationCallbacks));
}
}
int main() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    RunPressedAcknowledgmentPreservesThinking(); RunFeedback(); RunCompletionAuthority(); RunMappedOutputEnvelope(); RunEstablishedGestureCorpus();
    return failures==0?0:1;
}
