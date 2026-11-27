#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "bounded_hint_engine.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/orientation.hpp"
#include "modules/settle_animation_setting.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/interaction_model_snapshot_builder.hpp"
#include "runtime/game_state_machine.hpp"
#include "modules/led_frame_composer.hpp"
#include "foxie_embedded/millis.hpp"

namespace {
int failures=0;
constexpr uint64_t Bit(uint8_t square){return UINT64_C(1)<<square;}
void Check(bool ok,const char* message){if(!ok){++failures;std::printf("[FAIL] BOARD_STYLE_PREVIEW %s\n",message);}}
bool Same(virtual_litch_color a,virtual_litch_color b){return a.r==b.r&&a.g==b.g&&a.b==b.b&&a.w==b.w;}
bool SameBoard(const virtual_litch_semantic_frame& a,const virtual_litch_semantic_frame& b){
    for(uint8_t square=0;square<64U;++square)for(uint8_t slot=0;slot<3U;++slot)
        if(!Same(a.board[square][slot],b.board[square][slot]))return false;
    return true;
}
bool Observe(runtime::VirtualLitchSession& s,uint64_t logical){
    uint64_t physical=0;const bool rotate=modules::ComputeSquareRotate(s.BoardRotate(),s.FirstPlayerIsP2());
    for(uint8_t square=0;square<64U;++square)if(logical&Bit(square))physical|=Bit(runtime::MapSquareIndex(square,rotate));
    return s.ObserveHallMask(physical)==VIRTUAL_LITCH_OK;
}
bool Set(runtime::VirtualLitchSession& s,const char* key,const char* json,modules::ControlMutationStatus expected=modules::ControlMutationStatus::Applied){
    modules::ControlMutationOutcome outcome;
    return s.MutateSettingJson(81U,key,json,outcome)==VIRTUAL_LITCH_OK&&outcome.status==expected;
}
bool Align(runtime::VirtualLitchSession& s,uint32_t now){return now>=s.State().now_ms&&s.Advance(now-s.State().now_ms)==VIRTUAL_LITCH_OK;}
virtual_litch_config Config(){virtual_litch_config c{};virtual_litch_default_config(&c);c.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;return c;}
constexpr const char* FEN="7k/8/8/8/8/8/P7/7K w - - 0 1";
constexpr uint64_t OCC=Bit(7U)|Bit(48U)|Bit(63U);
bool Prepare(runtime::VirtualLitchSession& s){return s.Reset()==VIRTUAL_LITCH_OK&&s.LoadExternalFen(FEN)&&Observe(s,OCC)&&s.Advance(2000U)==VIRTUAL_LITCH_OK;}
void Checkpoint(runtime::VirtualLitchSession& s,const char* stage,uint8_t anchor=255U){
    std::printf("[BOARD_STYLE_PREVIEW] stage=%s t=%u phase=%u action=%u occupancy=%016llx history=%u mate_style=%s placement_style=%s next=normal-play-or-next-style",
        stage,s.State().now_ms,s.State().interaction_phase,s.State().required_action,
        static_cast<unsigned long long>(s.State().physical_occupied_mask),s.UndoHistoryMoveCount(),
        s.SettingsForPersistenceTest().ReadValue("CHECKMATE_ANIM_STYLE","fire-lob"),
        s.SettingsForPersistenceTest().ReadValue(modules::settle_animation_setting::KEY,
            modules::settle_animation_setting::DEFAULT_VALUE));
    if(anchor<64U){
        const auto piece=hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3,anchor,hardware::SquareLedSlot::Piece,
            modules::ComputeSquareRotate(s.BoardRotate(),s.FirstPlayerIsP2()));
        const auto rails=hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3,anchor,hardware::SquareLedSlot::Square0,
            modules::ComputeSquareRotate(s.BoardRotate(),s.FirstPlayerIsP2()));
        const auto rail=s.PhysicalFrame().pixels[rails.indices[0]];
        std::printf(" anchor=%u mapped_left=%u,%u,%u,%u semantic_left=%u,%u,%u piece=",anchor,rail.r,rail.g,rail.b,rail.w,s.SemanticFrame().board[anchor][0].r,
            s.SemanticFrame().board[anchor][0].g,s.SemanticFrame().board[anchor][0].b);
        for(uint8_t i=0;i<piece.count;++i){const auto c=s.PhysicalFrame().pixels[piece.indices[i]];std::printf("%u:%u,%u,%u,%u;",piece.indices[i],c.r,c.g,c.b,c.w);}
    }
    std::puts("");
}
void MateSelectionJourney(){
    test_support::BoundedHintSession fixture(Config()),baselineFixture(Config());
    auto& s=fixture.session;auto& ordinary=baselineFixture.session;
    Check(Prepare(s)&&Prepare(ordinary),"same actual non-mate position reaches normal play");
    std::array<uint64_t,5> recipes{};uint8_t recipeIndex=0U;
    for(const char* style:{"\"fireworks\"","\"ripple-bloom\"","\"winner-sweep\"","\"winner-constellation\"","\"fire-lob\""}){
        Check(Set(s,"CHECKMATE_ANIM_STYLE",style),"accepted changed checkmate style uses canonical mutation");
        bool changed=false;uint64_t recipe=UINT64_C(1469598103934665603);
        for(uint8_t sample=0;sample<10U;++sample){
            Check(s.Advance(150U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms),"sample actual style recipe");
            changed=changed||!SameBoard(s.SemanticFrame(),ordinary.SemanticFrame());
            if(sample==0U||sample==4U||sample==6U||sample==9U)Checkpoint(s,"actual-mate-recipe",7U);
            for(const auto& square:s.SemanticFrame().board)for(const auto c:square)
                for(const uint8_t channel:{c.r,c.g,c.b,c.w})recipe=(recipe^channel)*UINT64_C(1099511628211);
            Check(s.UndoHistoryMoveCount()==0U&&std::strcmp(s.HomeStateSnapshot().fen,FEN)==0&&
                s.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle),
                "preview never invents mate, moves, history or a physical plan");
        }
        Check(changed,"every actual selected mate recipe previews without a mate");Checkpoint(s,"mate-preview");
        for(uint8_t prior=0U;prior<recipeIndex;++prior)Check(recipes[prior]!=recipe,"selected recipes differ beyond their common dimmed base frame");
        recipes[recipeIndex++]=recipe;
        Check(s.Advance(501U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),
            "at2001ms preview returns to exact current normal board output");
        Check(Set(s,"CHECKMATE_ANIM_STYLE",style,modules::ControlMutationStatus::Noop)&&
            Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),"same mate style Noop cannot replay");
    }
    const auto quiet=s.SemanticFrame();
    Check(Set(s,"CHECKMATE_ANIM_STYLE","\"bad-style\"",modules::ControlMutationStatus::Rejected)&&SameBoard(quiet,s.SemanticFrame()),
        "rejected style never starts a preview");
    Check(Set(s,"CHECKMATE_ANIM_STYLE","\"fireworks\"")&&s.Advance(500U)==VIRTUAL_LITCH_OK&&
        Set(s,"CHECKMATE_ANIM_STYLE","\"winner-sweep\"")&&s.Advance(1600U)==VIRTUAL_LITCH_OK&&
        Align(ordinary,s.State().now_ms)&&!SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),"latest accepted selection owns its fresh two-second interval");
    Check(s.Advance(401U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),
        "superseded preview cannot replay after latest expiry");
    Check(Set(s,"CHECKMATE_ANIM_STYLE","\"fire-lob\"")&&Observe(s,OCC&~Bit(48U))&&
        s.Advance(10U)==VIRTUAL_LITCH_OK&&Observe(s,OCC)&&s.Advance(561U)==VIRTUAL_LITCH_OK&&
        Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),"physical handling cancels preview without a delayed return");
    Checkpoint(s,"mate-handling-canceled");
    Check(Observe(s,OCC&~Bit(48U))&&Observe(ordinary,OCC&~Bit(48U))&&
        Set(s,"CHECKMATE_ANIM_STYLE","\"fireworks\"")&&s.Advance(150U)==VIRTUAL_LITCH_OK&&
        Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),
        "a required held-piece plan rejects preview presentation and preserves legal next actions");
    Check(Observe(s,OCC)&&Observe(ordinary,OCC)&&s.Advance(561U)==VIRTUAL_LITCH_OK&&
        Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),
        "required-plan cancellation never queues a later preview");
    Check(Set(s,"CHECKMATE_ANIM_STYLE","\"fire-lob\"")&&s.Advance(100U)==VIRTUAL_LITCH_OK&&
        Set(s,"SETTLE_ANIM_STYLE","\"none\"")&&Align(ordinary,s.State().now_ms)&&
        SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),"latest placement None selection cancels a mate preview");
}
void PlacementReplayJourney(){
    test_support::BoundedHintSession fixture(Config()),ordinaryFixture(Config());auto& s=fixture.session;auto& ordinary=ordinaryFixture.session;
    Check(Prepare(s)&&Prepare(ordinary),"placement preview starts with real game");
    for(auto* board:{&s,&ordinary}) {
        Check(Observe(*board,OCC&~Bit(48U))&&Observe(*board,(OCC&~Bit(48U))|Bit(40U))&&board->Advance(3000U)==VIRTUAL_LITCH_OK&&
            board->UndoHistoryMoveCount()==1U,"real a2-a3 commit establishes latest moved-piece fallback");
        Check(board->Advance(3000U)==VIRTUAL_LITCH_OK,"real settle and afterglow finish independently before style preview");
        const auto beforeBlack=board->SemanticFrame();
        Check(Set(*board,"SETTLE_ANIM_STYLE","\"landing_flash\"")&&SameBoard(beforeBlack,board->SemanticFrame()),
            "P2 with no own last move never previews around P1's previous destination");
        const uint64_t afterWhite=(OCC&~Bit(48U))|Bit(40U);
        Check(Observe(*board,afterWhite&~Bit(7U))&&Observe(*board,(afterWhite&~Bit(7U))|Bit(6U))&&
            board->Advance(3000U)==VIRTUAL_LITCH_OK&&board->UndoHistoryMoveCount()==2U,
            "actual P2 king h8-g8 move returns the turn to P1 with distinct own/opponent destinations");
        Check(board->Advance(3000U)==VIRTUAL_LITCH_OK&&Set(*board,"SETTLE_ANIM_STYLE","\"mini_orbit\""),"own-side fallback fixture settles");
        Check(board->Advance(321U)==VIRTUAL_LITCH_OK,"style selection preview expires before paired controls");
    }
    const auto before=s.SemanticFrame();const std::string fen=s.HomeStateSnapshot().fen;
    Check(Set(s,"SETTLE_ANIM_STYLE","\"landing_flash\"")&&!SameBoard(before,s.SemanticFrame()),"changed placement style previews at last moved piece without a Hint");
    Checkpoint(s,"placement-fallback",40U);
    Check(s.Advance(161U)==VIRTUAL_LITCH_OK,"Landing flash preview expires at160ms");
    const auto quiet=s.SemanticFrame();const auto revision=s.SettingsForPersistenceTest().GetRevision();
    Check(Set(s,"SETTLE_ANIM_STYLE","\"landing_flash\"",modules::ControlMutationStatus::Noop)&&!SameBoard(quiet,s.SemanticFrame())&&
        s.SettingsForPersistenceTest().GetRevision()==revision,"fresh acknowledged same-style placement retap replays once without dirtying settings");
    Checkpoint(s,"placement-current-retap",40U);
    Check(s.Advance(161U)==VIRTUAL_LITCH_OK,"retap replay expires independently");
    const auto after=s.SemanticFrame();
    Check(Set(s,"SETTLE_ANIM_STYLE","\"mini_orbit\"")&&!SameBoard(after,s.SemanticFrame())&&
        s.Advance(125U)==VIRTUAL_LITCH_OK&&!SameBoard(after,s.SemanticFrame()),"Mini orbit uses the actual320ms recipe");
    Checkpoint(s,"placement-Mini-orbit-midpoint",40U);
    Check(s.Advance(196U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms)&&SameBoard(ordinary.SemanticFrame(),s.SemanticFrame()),"Mini orbit expires at320ms");
    Check(Set(s,"SETTLE_ANIM_STYLE","\"ripple\"")&&s.Advance(120U)==VIRTUAL_LITCH_OK&&
        !Same(s.SemanticFrame().board[41U][0U],after.board[41U][0U]),"Ripple uses actual neighboring traveling ring");
    Checkpoint(s,"placement-Ripple-travel",41U);
    Check(s.Advance(441U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms)&&SameBoard(ordinary.SemanticFrame(),s.SemanticFrame()),"Ripple expires at560ms");
    Check(Set(s,"SETTLE_ANIM_STYLE","\"none\"")&&SameBoard(ordinary.SemanticFrame(),s.SemanticFrame()),"None adds no optional effect and leaves normal presentation intact");
    Check(s.UndoHistoryMoveCount()==2U&&std::string(s.HomeStateSnapshot().fen)==fen&&s.State().pending_action_token==0U,
        "preview and retap never alter settle, confirmation, engine or history");
    for(auto* board:{&s,&ordinary}) {
        const uint64_t afterTwo=(OCC&~Bit(48U)&~Bit(7U))|Bit(40U)|Bit(6U);
        Check(Observe(*board,afterTwo&~Bit(40U))&&Observe(*board,(afterTwo&~Bit(40U))|Bit(32U))&&
            board->Advance(6000U)==VIRTUAL_LITCH_OK&&board->UndoHistoryMoveCount()==3U,
            "actual third P1 move leaves P2's own g8 destination distinct from P1 a4");
    }
    const auto blackBaseline=s.SemanticFrame();
    Check(Set(s,"SETTLE_ANIM_STYLE","\"rainbow_orbit\"")&&
        !Same(blackBaseline.board[6U][1U],s.SemanticFrame().board[6U][1U])&&
        Same(blackBaseline.board[32U][1U],s.SemanticFrame().board[32U][1U]),
        "P2 preview uses P2's own last-move destination, never the most recent P1 move");
    Check(s.Advance(321U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms)&&!SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),
        "Rainbow preview remains active beyond the unchanged Mini orbit320ms duration");
    Check(s.Advance(430U)==VIRTUAL_LITCH_OK&&Align(ordinary,s.State().now_ms)&&SameBoard(s.SemanticFrame(),ordinary.SemanticFrame())&&
        s.UndoHistoryMoveCount()==3U,"P2 Rainbow preview expires at750ms without changing move authority");
    Checkpoint(s,"placement-current-P2-own-last-expired",6U);
    virtual_litch_runtime_health_fact scan{};
    virtual_litch_default_runtime_health_fact(&scan);
    scan.kind=VIRTUAL_LITCH_RUNTIME_HEALTH_FACT_BOARD_SCAN;
    scan.board_scan=VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_UNCERTAIN;
    for(auto* board:{&s,&ordinary}) Check(board->DispatchRuntimeHealthFact(scan)==VIRTUAL_LITCH_OK&&
        board->Advance(100U)==VIRTUAL_LITCH_OK&&board->DispatchRuntimeHealthFact(scan)==VIRTUAL_LITCH_OK,
        "sustained production health facts pause input before the new preview request");
    unsigned lit=0U;for(const auto pixel:s.SemanticFrame().status)lit+=(pixel.r|pixel.g|pixel.b)!=0U;
    Check(lit==16U&&Set(s,"SETTLE_ANIM_STYLE","\"rainbow_orbit\"",modules::ControlMutationStatus::Noop)&&
        SameBoard(s.SemanticFrame(),ordinary.SemanticFrame()),"fresh current-style Noop cannot replace the visible paused-health presentation");
    scan.board_scan=VIRTUAL_LITCH_RUNTIME_HEALTH_BOARD_SCAN_STABLE;
    for(auto* board:{&s,&ordinary}) Check(board->DispatchRuntimeHealthFact(scan)==VIRTUAL_LITCH_OK&&
        board->Advance(100U)==VIRTUAL_LITCH_OK&&board->DispatchRuntimeHealthFact(scan)==VIRTUAL_LITCH_OK,
        "fresh stable observations recover before the hypothetical320ms preview could expire");
    Check(SameBoard(s.SemanticFrame(),ordinary.SemanticFrame())&&s.UndoHistoryMoveCount()==3U,
        "health recovery cannot replay the rejected preview and preserves actual move history");
    Checkpoint(s,"health-recovery-no-preview-replay",6U);


}
void QuietAndCapturePriorityJourney(){
    test_support::BoundedHintSession fixture(Config());auto& s=fixture.session;
    Check(Prepare(s),"quiet-anchor fixture ready");
    const auto quiet=s.SemanticFrame();
    Check(Set(s,"SETTLE_ANIM_STYLE","\"landing_flash\"")&&SameBoard(quiet,s.SemanticFrame()),
        "no shown Hint or moved piece preserves quiet output without invented demo geometry");
    Check(Set(s,"MINIMAL_ANIMATIONS","true"),"Minimal policy accepted");
    const auto minimalQuiet=s.SemanticFrame();
    Check(Set(s,"CHECKMATE_ANIM_STYLE","\"fireworks\"")&&s.Advance(100U)==VIRTUAL_LITCH_OK,
        "Minimal mate preview begins through the accepted style owner");
    const auto minimalPreview=s.SemanticFrame();
    Check(!SameBoard(minimalQuiet,minimalPreview)&&s.Advance(500U)==VIRTUAL_LITCH_OK&&SameBoard(minimalPreview,s.SemanticFrame()),
        "Minimal mate preview retains actual static local policy instead of forcing animation");
    Check(s.Advance(1401U)==VIRTUAL_LITCH_OK&&SameBoard(minimalQuiet,s.SemanticFrame()),"Minimal mate preview expires at2s");

    const char* captureFen="4k3/8/8/8/4q3/8/4R3/6K1 w - - 0 1";
    const uint64_t captureOcc=Bit(4U)|Bit(36U)|Bit(52U)|Bit(62U);
    Check(s.Reset()==VIRTUAL_LITCH_OK&&s.LoadExternalFen(captureFen)&&Observe(s,captureOcc)&&s.Advance(2000U)==VIRTUAL_LITCH_OK,
        "existing legal e2-e4 capture fixture enters through FEN and physical Hall input");
    Check(s.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,runtime::VirtualLitchPhysicalButton::O,true)==VIRTUAL_LITCH_OK&&
        s.Advance(20U)==VIRTUAL_LITCH_OK&&s.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,runtime::VirtualLitchPhysicalButton::O,false)==VIRTUAL_LITCH_OK&&
        s.Advance(600U)==VIRTUAL_LITCH_OK,"raw O publishes capture Hint");
    uint8_t from=255U,to=255U;
    Check(s.TryGetHintMoveSquares(from,to)&&from==52U&&to==36U,"actual search supplies authoritative capture-removal target");
    const auto capture=s.SemanticFrame();
    Check(Set(s,"CHECKMATE_ANIM_STYLE","\"fireworks\"")&&SameBoard(capture,s.SemanticFrame())&&
        Set(s,"SETTLE_ANIM_STYLE","\"landing_flash\"")&&SameBoard(capture,s.SemanticFrame())&&
        Set(s,"SETTLE_ANIM_STYLE","\"mini_orbit\"")&&SameBoard(capture,s.SemanticFrame())&&
        Set(s,"SETTLE_ANIM_STYLE","\"rainbow_orbit\"")&&SameBoard(capture,s.SemanticFrame())&&
        Set(s,"SETTLE_ANIM_STYLE","\"ripple\"")&&SameBoard(capture,s.SemanticFrame()),
        "suggested amber removal owns the board above checkmate and every non-None placement preview");
    const auto piece=hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3,36U,hardware::SquareLedSlot::Piece,false);
    std::array<bool,60U> bright{};
    for(unsigned sample=0U;sample<bright.size();++sample){
        Check(s.Advance(20U)==VIRTUAL_LITCH_OK,"required amber removal keeps its independent1200ms cadence");
        for(uint8_t index=0U;index<piece.count;++index){const auto c=s.PhysicalFrame().pixels[piece.indices[index]];
            Check(c.r==0U&&c.g==0U&&c.b==0U&&c.w==0U,"both physical capture victim piece emitters remain black above previews");}
        const auto left=s.SemanticFrame().board[36U][0U],right=s.SemanticFrame().board[36U][2U];
        const bool peak=left.r==255U&&left.g==101U&&left.b==0U&&left.w==0U;
        const bool dim=left.r==64U&&left.g==25U&&left.b==0U&&left.w==0U;
        Check(Same(left,right)&&(peak||dim),
            "both capture rails retain the same exact bright or dim amber above every optional preview");
        bright[sample]=peak;
    }
    bool cadence=false;
    for(unsigned offset=0U;offset<bright.size();++offset){
        bool matches=true;
        for(unsigned sample=0U;sample<bright.size();++sample){
            const auto phase=(sample+offset)%bright.size();
            matches=matches&&(bright[sample]==(phase<6U||(phase>=12U&&phase<18U)));
        }
        cadence=cadence||matches;
    }
    Check(cadence,"both amber rails preserve120ms tap,120ms gap,120ms tap and840ms dim rest without preview replay");
    Checkpoint(s,"capture-beats-optional-preview",36U);
}
void HintAnchorJourney(){
    test_support::BoundedHintSession fixture(Config()),controlFixture(Config());auto& s=fixture.session;auto& control=controlFixture.session;
    Check(Prepare(s)&&Prepare(control),"Hint preview fixtures ready");
    const auto tap=[](runtime::VirtualLitchSession& b){return b.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,runtime::VirtualLitchPhysicalButton::O,true)==VIRTUAL_LITCH_OK&&
        b.Advance(20U)==VIRTUAL_LITCH_OK&&b.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,runtime::VirtualLitchPhysicalButton::O,false)==VIRTUAL_LITCH_OK&&b.Advance(20U)==VIRTUAL_LITCH_OK;};
    Check(tap(s)&&tap(control),"raw O reveals current Hint through production control owner");
    uint8_t from=64U,to=64U,cf=64U,ct=64U;
    for(uint8_t i=0;i<30U&&(!s.TryGetHintMoveSquares(from,to)||!control.TryGetHintMoveSquares(cf,ct));++i){(void)s.Advance(100U);(void)Align(control,s.State().now_ms);}
    Check(from<64U&&from==cf&&to==ct&&s.HintDisplayRequested(),"both fixtures publish same actual shown Hint");
    if(from>=64U||from!=cf||to!=ct)return;
    Check(Align(control,s.State().now_ms)&&Set(s,"SETTLE_ANIM_STYLE","\"landing_flash\"")&&
        !Same(s.SemanticFrame().board[from][2U],control.SemanticFrame().board[from][2U]),"placement effect anchors at currently SHOWN Hint source");
    Checkpoint(s,"placement-Hint-source",from);
    Check(s.Advance(161U)==VIRTUAL_LITCH_OK&&Align(control,s.State().now_ms)&&SameBoard(s.SemanticFrame(),control.SemanticFrame()),
        "placement preview expiry restores the same current Hint and legal guidance");
    Check(Set(s,"MINIMAL_ANIMATIONS","true")&&Set(control,"MINIMAL_ANIMATIONS","true")&&
        Set(s,"SETTLE_ANIM_STYLE","\"ripple\"")&&s.Advance(100U)==VIRTUAL_LITCH_OK&&Align(control,s.State().now_ms),"Minimal preview honors existing local policy");
    for(uint8_t square=0;square<64U;++square)if(square!=from)
        Check(Same(s.SemanticFrame().board[square][0U],control.SemanticFrame().board[square][0U])&&
            Same(s.SemanticFrame().board[square][2U],control.SemanticFrame().board[square][2U]),"Minimal placement preview stays static and local to source");
    Checkpoint(s,"Minimal-placement-source",from);
}
// Supporting production-owner coverage: VirtualLitch has no modal-text ingress.
// Use the real state-machine owner, snapshot builder, preview owner and composer;
// the existing Hall/Hint journeys above retain the end-to-end interaction evidence.
void TransientTextCancellationJourney() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    for (const char* style : {"landing_flash", "mini_orbit", "rainbow_orbit", "ripple", "winner-sweep"}) {
        const bool mate = std::strcmp(style, "winner-sweep") == 0;
        for (const bool alreadyVisible : {false, true}) {
            fxe::ManualMillis::SetNowMs(1000U);
            runtime::GameStateMachine state;
            state.Reset();
            state.ApplyPhysicalOccupancySnapshot(OCC);
            state.SetExpectedOccupancyMask(OCC);
            state.SetExpectedSideToMove(modules::PersistedSide::White);
            state.SetExpectedMoverAndOpponentOccupancyMasks(Bit(48U) | Bit(63U), Bit(7U));
            state.SetLastMoveForSide(modules::PersistedSide::White, 56U, 48U);
            modules::EngineDerivedState derived;
            modules::SettingsStore settings;
            const char* key = mate ? "CHECKMATE_ANIM_STYLE" : "SETTLE_ANIM_STYLE";
            settings.Doc()[key] = style;
            settings.Doc()["BOARD_SETUP_ASSISTANCE"] = false;
            settings.Doc()["P1_END_PIECE_WHEEL"] = 0U;
            settings.Doc()["P2_END_PIECE_WHEEL"] = 170U;
            modules::BoardStylePreview preview;
            const auto snapshot = [&]() {
                return runtime::BuildInteractionModelSnapshot({
                    .stateMachine = &state, .derived = &derived, .authoritativeFen = FEN});
            };
            const auto matchesOrdinary = [&](runtime::InteractionModelSnapshot& current,
                                              modules::BoardStylePreviewFrame frame) {
                modules::led_frame_composer::ComposeInputs inputs;
                inputs.settings = &settings;
                inputs.interactionSnapshot = &current;
                modules::BoardLedFrame actual{}, ordinary{};
                std::array<modules::Rgb, modules::EDGE_LED_COUNT> edge{};
                std::array<modules::Rgb, 64> grid{};
                modules::led_frame_composer::Compose(inputs, ordinary, edge, grid);
                inputs.boardStylePreview = frame;
                modules::led_frame_composer::Compose(inputs, actual, edge, grid);
                const auto a = modules::ProjectLegacyBoardFrame(actual);
                const auto b = modules::ProjectLegacyBoardFrame(ordinary);
                for (size_t i = 0U; i < a.size(); ++i)
                    if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) return false;
                return true;
            };
            const auto capture = [&](runtime::InteractionModelSnapshot& current) {
                modules::led_frame_composer::ComposeInputs inputs;
                inputs.settings = &settings;
                inputs.interactionSnapshot = &current;
                return preview.Capture(inputs);
            };
            fxe::ManualMillis::SetNowMs(2000U);
            if (!alreadyVisible) state.SetTextOverlayActive(true);
            preview.SettingAccepted(settings, key);
            fxe::ManualMillis::SetNowMs(2020U);
            auto current = snapshot();
            auto frame = capture(current);
            if (alreadyVisible) {
                Check(frame.kind != modules::BoardStylePreviewKind::None && !matchesOrdinary(current, frame),
                      "selected recipe is visibly active before actual modal text interrupts it");
                state.SetTextOverlayActive(true);
                fxe::ManualMillis::SetNowMs(2040U);
                current = snapshot();
                frame = capture(current);
            }
            Check(current.render.textOverlayActive && frame.kind == modules::BoardStylePreviewKind::None &&
                      matchesOrdinary(current, frame),
                  "actual transient-text owner rejects fresh or active placement/checkmate preview");
            state.SetTextOverlayActive(false);
            fxe::ManualMillis::SetNowMs(2060U); // Before every original 160/320/560/2000ms expiry.
            current = snapshot();
            frame = capture(current);
            Check(!current.render.textOverlayActive && frame.kind == modules::BoardStylePreviewKind::None &&
                      matchesOrdinary(current, frame),
                  "clearing modal text before the original deadline never resumes canceled preview");
            std::printf("[BOARD_STYLE_PREVIEW_TEXT] style=%s prior_visible=%u age=60 text=cleared result=normal-frame next=normal-play\n",
                        style, alreadyVisible);
        }
    }
}

}
int main(){TransientTextCancellationJourney();MateSelectionJourney();PlacementReplayJourney();QuietAndCapturePriorityJourney();HintAnchorJourney();std::printf("[BOARD_STYLE_PREVIEW] failures=%d\n",failures);return failures?1:0;}
