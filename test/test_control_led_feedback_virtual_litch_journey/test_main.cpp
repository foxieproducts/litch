#include <cstdio>
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstring>
#include <vector>
#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_frame_store.hpp"
#include "modules/litchcraft_frame_stream.hpp"

#include "hardware/status_led_chain_profile.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/control_led_feedback.hpp"
#include "virtual_litch_session.hpp"
#include "bounded_hint_engine.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {
using Button = runtime::VirtualLitchPhysicalButton;
using Role = hardware::ControlLedRole;
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::printf("[FAIL] RGB_CONTROL_JOURNEY %s\n", message); ++failures; }
}
struct Fixture {
    runtime::VirtualLitchSession board;
    explicit Fixture(bool rotate = false) : board(Config(rotate)) {
        board.ObserveBootHallMask(0U);
        board.Advance(3000U);
        Set(Button::X, true); Set(Button::O, true);
        board.Advance(1500U); Set(Button::X, false); Set(Button::O, false);
        Check(board.Litchcraft().IsActive(), "physical X+O enters Craft");
        board.Advance(300U);
    }
    static virtual_litch_config Config(bool rotate) {
        virtual_litch_config cfg{}; virtual_litch_default_config(&cfg);
        cfg.hardware = VIRTUAL_LITCH_HARDWARE_V3_3; cfg.board_rotate = rotate;
        return cfg;
    }
    void Set(Button button, bool down, uint8_t owner = VIRTUAL_LITCH_OWNER_P1) {
        Check(board.SetButtonDown(owner, button, down) == VIRTUAL_LITCH_OK &&
              board.Advance(30U) == VIRTUAL_LITCH_OK, "raw control settles through production debounce");
    }
    void Tap(Button button, uint8_t owner = VIRTUAL_LITCH_OWNER_P1) {
        Set(button, true, owner); Set(button, false, owner);
    }
    virtual_litch_color Pixel(Role role, uint8_t owner = VIRTUAL_LITCH_OWNER_P1) {
        const bool physicalP2 = owner == VIRTUAL_LITCH_OWNER_P2;
        return board.PhysicalFrame().pixels[256U + hardware::V3_3PhysicalIndexForControl(physicalP2, role)];
    }
    void Capture(const char* stage, const char* result, const char* next,
                 uint8_t owner = VIRTUAL_LITCH_OWNER_P1) {
        auto x = Pixel(Role::X,owner); auto o = Pixel(Role::O,owner);
        auto left=Pixel(Role::Left,owner);auto right=Pixel(Role::Right,owner);
        auto up=Pixel(Role::Up,owner);auto down=Pixel(Role::Down,owner);
        std::printf("[RGB_CONTROL_JOURNEY] stage=%s occupancy=%016llx mode=%s undo=%u redo=%u erase=%u "
                    "result=%s display=canvas bank=%u X=%u,%u,%u O=%u,%u,%u L=%u,%u,%u R=%u,%u,%u "
                    "Up=%u,%u,%u Down=%u,%u,%u next=%s expiry=150ms-action/180ms-history\n",
            stage, static_cast<unsigned long long>(board.Litchcraft().Occupancy()),
            board.Litchcraft().IsActive() ? "Craft" : "Setup",
            board.Litchcraft().UndoCount(), board.Litchcraft().RedoCount(), board.Litchcraft().IsErasing(),
            result, owner,x.r,x.g,x.b,o.r,o.g,o.b,left.r,left.g,left.b,right.r,right.g,right.b,
            up.r,up.g,up.b,down.r,down.g,down.b,next);
    }
};
unsigned Energy(virtual_litch_color c) { return unsigned(c.r)+c.g+c.b; }
bool Neutral(virtual_litch_color c) { return c.r != 0U && c.r == c.g && c.g == c.b; }
bool SameControlColor(virtual_litch_color a, virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}
void RunCraftHistoryAvailabilityJourney() {
    Fixture f;
    auto& art = f.board.Litchcraft();
    const auto steady = [&](const char* stage) {
        for (const auto bank : {VIRTUAL_LITCH_OWNER_P1, VIRTUAL_LITCH_OWNER_P2}) {
            const auto x = f.Pixel(Role::X, bank);
            const auto o = f.Pixel(Role::O, bank);
            const auto neutral = [](virtual_litch_color color) {
                return color.r != 0U && color.r == color.g && color.g == color.b;
            };
            Check(art.UndoCount() != 0U ? neutral(x) : Energy(x) == 0U,
                "Craft X neutrally reports actual Undo availability on each bank");
            Check(art.RedoCount() != 0U ? neutral(o) : Energy(o) == 0U,
                "Craft O neutrally reports actual Redo availability, never Paint/Erase color");
            f.Capture(stage, "actual-history-availability", "tap-available-history-or-hold-for-tool", bank);
        }
    };
    steady("history-empty-Paint");
    f.Tap(Button::Up); f.board.Advance(300U);
    steady("paint-color-changed-no-Redo");
    f.board.ObserveHallMask(UINT64_C(1) << 35U); f.board.Advance(30U);
    f.board.ObserveHallMask(0U); f.board.Advance(400U);
    Check(art.UndoCount() != 0U && art.RedoCount() == 0U, "real Hall painting creates Undo only");
    steady("paint-stroke-Undo-available");
    f.Tap(Button::X);
    f.Capture("short-X-neutral-Undo", "applied-one-history-step", "wait-for-history-sweep");
    f.board.Advance(250U);
    Check(art.RedoCount() != 0U, "real Undo creates Redo availability");
    steady("Undo-complete-Redo-available");
    f.Tap(Button::O);
    f.Capture("short-O-neutral-Redo", "applied-one-history-step", "wait-for-history-sweep");
    f.board.Advance(250U);
    Check(art.RedoCount() == 0U, "real Redo exhausts its actual history");
    steady("Redo-complete-O-dark");
    f.Set(Button::O, true); f.board.Advance(1000U);
    const auto confirmation = f.Pixel(Role::O);
    Check(confirmation.r != 0U && confirmation.r == confirmation.g && confirmation.g == confirmation.b,
        "O tool-change confirmation stays neutral rather than falsely indicating destructive Redo");
    f.Capture("O-hold-neutral-tool-confirmation", "applied-Erase-once", "release-without-Redo");
    f.Set(Button::O, false); f.board.Advance(400U);
    Check(art.IsErasing(), "O hold retains its existing Erase toggle");
    steady("Erase-on-no-Redo-O-dark");
    f.board.Litchcraft().SetMutationsPaused(true); f.board.Advance(1U);
    Check(Energy(f.Pixel(Role::X)) == 0U && Energy(f.Pixel(Role::O)) == 0U,
        "paused edits do not advertise unavailable history");
    f.board.Litchcraft().SetMutationsPaused(false); f.board.Advance(1U);
    steady("edit-resumed-history-restored");
}

void Run(bool rotate) {
    Fixture f(rotate); auto& art=f.board.Litchcraft();
    Check(!art.IsErasing(), "entry starts in Paint");
    const auto initial=art.Color(); f.Tap(Button::Up);
    Check(Energy(f.Pixel(Role::Up))>0U && Energy(f.Pixel(Role::Down))==0U &&
        (art.Color().r!=initial.r||art.Color().g!=initial.g||art.Color().b!=initial.b),
        "raw Up changes actual paint color and only its direction acknowledges it");
    f.Capture("color-Up","applied-selected-RGB","release-or-change-palette");
    f.board.Advance(200U);Check(Energy(f.Pixel(Role::Up))==0U,"color acknowledgment expires");
    f.Tap(Button::Right);Check(art.IsQuickPaletteVisible()&&Energy(f.Pixel(Role::Right))>0U,
        "physical palette navigation owns a directional acknowledgment");
    f.Capture("palette-Right","applied-palette-navigation","select-swatch-or-wait");
    f.board.Advance(2400U);
    f.board.ShowIdentifyOverlay(1U,{255U,255U,255U});f.board.Advance(1U);
    Check(Energy(f.Pixel(Role::O))==0U,"Identify preempts ordinary Craft control feedback");
    f.board.StopIdentifyOverlay();f.board.Advance(1U);
    Check(Energy(f.Pixel(Role::O))==0U,"Identify exit restores unavailable Redo without a tool-color O");
    f.Tap(Button::Up);art.EndConnection();f.board.Advance(1U);
    Check(art.IsActive()&&Energy(f.Pixel(Role::Up))==0U&&Energy(f.Pixel(Role::O))==0U,
        "disconnect clears transient feedback while physical Craft remains operable");
    f.Capture("disconnect-recovery","retained-mode-retired-transient","continue-physical-editing");
    f.Tap(Button::O);
    Check(!art.IsErasing(), "short O with no history cannot toggle Paint/Erase");
    f.Capture("short-O-no-history", "rejected-no-history", "hold-O-for-Erase");
    f.Set(Button::O,true); f.board.Advance(500U);
    Check(!art.IsErasing(), "interrupted hold leaves tool unchanged until release");
    Check(Energy(f.Pixel(Role::O)) > 0U, "hold has mapped physical progress before threshold");
    f.Set(Button::O,false);
    Check(!art.IsErasing(), "cancelled O hold only attempts unavailable Redo");
    f.Set(Button::O,true); f.board.Advance(1000U);
    Check(art.IsErasing(), "established one-second hold toggles Erase once");
    f.Capture("O-hold-threshold","applied-Erase","release-without-Redo");
    f.board.Advance(1500U); f.Set(Button::O,false);
    Check(art.IsErasing(), "held O and release do not repeat or fire short Redo");
    f.Capture("O-hold", "accepted-Erase-once", "hold-O-for-Paint");
    f.Set(Button::O,true,VIRTUAL_LITCH_OWNER_P2); f.board.Advance(1000U);
    f.Set(Button::O,false,VIRTUAL_LITCH_OWNER_P2);
    Check(!art.IsErasing(), "other control bank operates the same shared tool");
    f.board.ObserveHallMask(UINT64_C(1)<<35U); f.board.Advance(30U);
    f.board.ObserveHallMask(0U); f.board.Advance(400U);
    Check(art.UndoCount()!=0U, "Hall painting creates real history");
    const auto undoBefore=art.UndoCount();
    f.Set(Button::X,true);f.board.Advance(500U);
    Check(art.UndoCount()==undoBefore,"early X hold does not clear before threshold");
    f.Set(Button::X,false);
    Check(art.UndoCount()<undoBefore && art.RedoCount()!=0U, "short X performs real Undo");
    f.Capture("X-early-release", "applied-short-Undo-no-clear", "tap-O-to-Redo");
    f.Tap(Button::O);
    Check(art.RedoCount()==0U && art.UndoCount()==undoBefore, "short O performs real Redo");
    f.Capture("O-tap", "applied-Redo", "hold-X-to-clear");
    f.Set(Button::X,true); f.board.Advance(1000U);
    const auto count=art.UndoCount();
    f.Capture("X-hold-threshold","applied-frame-clear","release-without-Undo");
    Check(count==undoBefore+1U, "long X clears current frame with one Undo boundary");
    f.board.Advance(1300U); f.Set(Button::X,false);
    Check(art.UndoCount()==count, "long X does not repeat or Undo on release");
    f.Capture("X-hold", "applied-current-frame-clear-once", "tap-X-to-restore");
    f.Tap(Button::X); Check(art.UndoCount()==undoBefore, "short X restores cleared frame");
    for(unsigned repeat=0U;repeat<6U;++repeat){f.Tap(Button::O);f.Tap(Button::X);}
    Check(art.UndoCount()==undoBefore&&art.RedoCount()==1U,
        "rapid repeated Undo/Redo remains bounded and preserves exact history");
    f.Capture("rapid-history","applied-six-Redo-Undo-pairs","continue-editing");
    f.board.Advance(300U);
    Check((Energy(f.Pixel(Role::X))>0U)==(art.UndoCount()!=0U)&&Energy(f.Pixel(Role::Left))==0U&&Energy(f.Pixel(Role::Right))==0U,
        "rapid history leaves no queued or stuck effects");
    const auto beforeChord=art.UndoCount();
    f.Set(Button::X,true); f.Set(Button::O,true); f.board.Advance(350U);
    f.Set(Button::X,false); f.Set(Button::O,false);
    Check(art.IsActive() && art.UndoCount()==beforeChord && !art.IsErasing(),
        "cancelled X+O consumes both actions without clear, history or tool change");
    f.Capture("cancelled-exit", "cancelled-no-ghost-action", "continue-painting");
    const auto beforeRemap=art.UndoCount();
    f.Set(Button::X,true);f.board.Advance(500U);
    virtual_litch_setting_request setting{};
    setting.struct_size=sizeof(setting);setting.abi_version=VIRTUAL_LITCH_CONTROL_ABI_VERSION;
    setting.request_id=900U;
    std::snprintf(setting.key,sizeof(setting.key),"BOARD_ROTATE");
    std::snprintf(setting.value_json,sizeof(setting.value_json),"%s",rotate?"false":"true");
    modules::ControlMutationOutcome changed;
    Check(f.board.MutateSetting(setting,changed)==VIRTUAL_LITCH_OK &&
        changed.status==modules::ControlMutationStatus::Applied,
        "actual app orientation mutation is accepted on the empty Craft board");
    Check(Neutral(f.Pixel(Role::X)) && Neutral(f.Pixel(Role::X,VIRTUAL_LITCH_OWNER_P2)),
        "orientation projection replaces old destructive hold progress with actual neutral Undo availability immediately");
    f.board.Advance(1200U);f.Set(Button::X,false);
    Check(art.UndoCount()==beforeRemap&&art.IsActive(),
        "old held X cannot erase or Undo after orientation changes");
    f.Capture("orientation-mid-hold","cancelled-old-owner","release-and-retry");
    f.Set(Button::X,true); f.Set(Button::O,true); f.board.Advance(1500U);
    f.Set(Button::X,false); f.Set(Button::O,false);
    Check(!art.IsActive(), "existing held X+O exits only on release");
    f.board.Advance(300U);
    Check(Energy(f.Pixel(Role::X))==0U && Energy(f.Pixel(Role::O))==0U,
        "mode exit retires obsolete Craft control feedback");
}

void RunCpuCoachingOwnershipJourney(const bool rotate) {
    // The session uses the real engine and renderer with synchronous polling;
    // this journey observes the directed CPU plan, with no asynchronous
    // thinking interval. The fixture also bounds Hint work and removes its
    // calculation-time minimum.
    test_support::BoundedHintSession fixture(Fixture::Config(rotate));
    auto& board=fixture.session;
    constexpr uint64_t opening=UINT64_C(0xFFFF00000000FFFF);
    constexpr uint8_t e7=12U, e2=52U, e4=36U;
    constexpr uint32_t breathMs=4096U;
    const bool swapped=modules::ComputeControlBankSwap(rotate,false,false);
    const uint8_t bank=swapped ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    const uint8_t cpuBank=swapped ? VIRTUAL_LITCH_OWNER_P1 : VIRTUAL_LITCH_OWNER_P2;
    const auto physicalPixel=[&](uint8_t physicalBank,Role role) {
        return board.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(
            physicalBank==VIRTUAL_LITCH_OWNER_P2,role)];
    };
    const auto pixel=[&](Role role) { return physicalPixel(bank,role); };
    const auto cpuPixel=[&](Role role) { return physicalPixel(cpuBank,role); };
    const auto darkDirections=[&](uint8_t physicalBank) {
        const virtual_litch_color dark{};
        return SameControlColor(physicalPixel(physicalBank,Role::Up),dark) &&
               SameControlColor(physicalPixel(physicalBank,Role::Down),dark);
    };
    const auto redCpuPair=[&]() {
        const auto up=cpuPixel(Role::Up);
        return up.r>0U && up.g==0U && up.b==0U && up.w==0U &&
               SameControlColor(up,cpuPixel(Role::Down)) && darkDirections(bank);
    };
    const auto rawBit=[&](uint8_t logicalSquare) {
        return UINT64_C(1)<<modules::MapSquareToPhysical(logicalSquare,rotate);
    };
    const auto advance=[&](uint32_t ms) {
        Check(board.Advance(ms)==VIRTUAL_LITCH_OK,"CPU/Coaching advances the virtual board clock");
    };
    const auto setting=[&](const char* key,const char* value) {
        modules::ControlMutationOutcome outcome;
        Check(board.MutateSettingJson(0U,key,value,outcome)==VIRTUAL_LITCH_OK &&
              (outcome.status==modules::ControlMutationStatus::Applied ||
               outcome.status==modules::ControlMutationStatus::Noop),
              "CPU/Coaching journey uses the production acknowledged setting owner");
    };
    const auto edge=[&](bool down,Button button=Button::O) {
        Check(board.SetButtonDown(bank,button,down)==VIRTUAL_LITCH_OK &&
              board.Advance(30U)==VIRTUAL_LITCH_OK,"CPU/Coaching uses raw physical control edges");
    };
    const auto litPerimeter=[&]() {
        const auto& status=board.SemanticFrame().status;
        return static_cast<unsigned>(std::count_if(
            status,status+VIRTUAL_LITCH_STATUS_LED_COUNT,
            [](virtual_litch_color color) { return Energy(color)>0U; }));
    };
    const auto checkpoint=[&](const char* stage,const char* next,const char* result="accepted") {
        const auto& state=board.State();
        const auto home=board.HomeStateSnapshot();
        const auto display=board.DisplayText();
        std::printf("[RGB_CPU_COACHING] rotate=%u stage=%s t=%u occupancy=%016llx mode=%s "
                    "phase=%u required=%u turn=P%u saved=%u cpu=P%u/level%u hint=%u "
                    "thinking=%u result=%s display=\"%.*s\" edgeLit=%u",
            rotate,stage,state.now_ms,static_cast<unsigned long long>(state.physical_occupied_mask),
            board.QuickSettingsActive()?"Quick-Setup":modules::HomeStateActionToToken(home.action),
            state.interaction_phase,state.required_action,state.side_to_move_black?2U:1U,
            home.coachingMask,board.SettingsForPersistenceTest().ReadValue("VS_CPU_PLAYER",2U),
            board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL",0U),
            board.HintDisplayRequested(),board.CpuThinkingVisible(),result,
            int(display.size()),display.empty()?"":display.data(),litPerimeter());
        for(uint8_t physicalBank:{VIRTUAL_LITCH_OWNER_P1,VIRTUAL_LITCH_OWNER_P2}) {
            std::printf(" physicalP%u[O,Up,Right,Down,Left,X]=",physicalBank);
            for(unsigned role=0U;role<6U;++role) {
                const auto c=physicalPixel(physicalBank,static_cast<Role>(role));
                std::printf("%u/%u/%u/%u%s",c.r,c.g,c.b,c.w,role==5U?"":";");
            }
        }
        std::printf(" next=%s\n",next);
    };
    setting("VS_CPU_PLAYER","2");setting("VS_CPU_LEVEL","1");
    board.BeginStartupPieceAccent();
    Check(board.ObserveHallMask(opening)==VIRTUAL_LITCH_OK,"CPU/Coaching uses the raw Hall opening");
    advance(300U);
    Check(board.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
          board.State().expected_occupied_mask==opening && darkDirections(cpuBank),
          "startup piece introduction keeps the configured CPU role dark even when the opening matches");
    checkpoint("startup-piece-introduction","wait-for-Play");
    advance(250U);advance(2450U);
    Check(Energy(cpuPixel(Role::Up))>0U && SameControlColor(cpuPixel(Role::Up),cpuPixel(Role::Down)) &&
          darkDirections(bank),"the CPU role appears on the mapped bank after startup and setup complete");
    checkpoint("boot-complete","play-P1");
    setting("VS_CPU_LEVEL","0");
    setting("P1_AUTO_HINTS","true");board.Advance(2000U);
    Check(board.HomeStateSnapshot().coachingMask==1U&&Energy(pixel(Role::O))>0U,
          "saved human Automatic Hints drives the mapped persistent O");
    checkpoint("human-saved-on","hold-O-or-change-CPU-role");
    edge(true);board.Advance(400U);
    setting("VS_CPU_PLAYER","1");setting("VS_CPU_LEVEL","1");setting("VS_CPU_LEVEL","0");
    const auto cancelledRevision=board.SettingsForPersistenceTest().GetRevision();
    board.Advance(800U);edge(false);board.Advance(1200U);
    Check(board.HomeStateSnapshot().coachingMask==1U&&
          board.SettingsForPersistenceTest().GetRevision()==cancelledRevision&&Energy(pixel(Role::O))>0U,
          "CPU on/off during a buffered O hold cancels it without changing saved Coaching");
    checkpoint("CPU-on-off-during-hold","release-then-use-a-fresh-gesture");
    setting("VS_CPU_LEVEL","1");board.Advance(2000U);
    const auto cpuRevision=board.SettingsForPersistenceTest().GetRevision();
    Check(board.HomeStateSnapshot().coachingMask==1U&&Energy(pixel(Role::O))==0U,
          "CPU role keeps saved Coaching on while effective mapped O is off");
    edge(true);board.Advance(1200U);edge(false);board.Advance(800U);
    Check(board.HomeStateSnapshot().coachingMask==1U&&
          board.SettingsForPersistenceTest().GetRevision()==cpuRevision&&Energy(pixel(Role::O))==0U,
          "CPU optional O hold expires its rejection without a preference write or false success feedback");
    checkpoint("CPU-effective-off","follow-required-CPU-guidance-or-disable-CPU");
    setting("VS_CPU_LEVEL","0");board.Advance(1200U);
    Check(board.HomeStateSnapshot().coachingMask==1U&&Energy(pixel(Role::O))>0U,
          "CPU off restores the same saved preference and mapped O without a Coaching write");
    checkpoint("human-restored-on","fresh-O-hold-can-disable-Coaching");
    edge(true);board.Advance(1000U);edge(false);board.Advance(1200U);
    Check(board.HomeStateSnapshot().coachingMask==0U&&Energy(pixel(Role::O))==0U,
          "fresh physical O hold works exactly once after role cancellation");

    setting("VS_CPU_PLAYER","2");
    setting("P2_END_PIECE_WHEEL","255");
    Check(darkDirections(bank) && darkDirections(cpuBank),"both direction pairs are dark in settled CPU-off Play");
    checkpoint("CPU-off","enable-CPU-for-P2");
    setting("VS_CPU_LEVEL","1");
    advance(breathMs-board.State().now_ms%breathMs);
    const auto trough=cpuPixel(Role::Up);
    Check(board.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
          !board.State().side_to_move_black && !board.CpuThinkingVisible() && redCpuPair(),
          "on the human turn the mapped CPU pair uses the CPU piece hue and never goes dark");
    checkpoint("CPU-seat-trough","play-P1");
    advance(breathMs/2U);
    const auto peak=cpuPixel(Role::Up);
    Check(redCpuPair() && peak.r>trough.r,"both CPU directions breathe together");
    checkpoint("CPU-seat-peak","play-P1");

    setting("MINIMAL_ANIMATIONS","true");
    const auto steady=cpuPixel(Role::Up);
    Check(redCpuPair() && trough.r<steady.r && steady.r<peak.r,"Minimal Animations sets a steady intermediate CPU role brightness");
    advance(breathMs/2U);
    Check(redCpuPair() && SameControlColor(steady,cpuPixel(Role::Up)),"the Minimal CPU pair holds through the next trough");
    checkpoint("CPU-seat-Minimal","play-P1");
    setting("HINT_THINKING_STYLE","\"none\"");
    Check(redCpuPair() && SameControlColor(steady,cpuPixel(Role::Up)),"disabling thinking animation preserves the CPU role");
    setting("P1_AUTO_HINTS","true");
    advance(900U);
    Check(Energy(pixel(Role::O))>0U && redCpuPair(),"human Coaching and the separate CPU role coexist");
    checkpoint("human-Coaching-and-CPU-seat","play-P1");
    setting("P1_AUTO_HINTS","false");

    Check(board.ObserveHallMask(opening & ~rawBit(e7))==VIRTUAL_LITCH_OK,
          "lift the CPU's e7 pawn on the human turn through raw Hall");
    advance(1500U);
    Check(board.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::Correction) &&
          board.State().correction_mask==(UINT64_C(1)<<e7) && darkDirections(cpuBank),
          "wrong-side pickup enters real correction and takes priority over the CPU pair");
    checkpoint("wrong-side-e7-held","replace-e7","rejected");
    Check(board.ObserveHallMask(opening)==VIRTUAL_LITCH_OK,"return e7 through raw Hall");
    advance(560U);
    Check(board.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
          board.State().correction_mask==0ULL && board.UndoHistoryMoveCount()==0U && redCpuPair(),
          "returning e7 clears correction and restores the CPU role without a move");
    checkpoint("wrong-side-e7-restored","play-P1-or-hold-X","recovered");

    const auto ordinaryPerimeter=board.SemanticFrame().status_hash;
    edge(true,Button::X);advance(500U);
    Check(!board.QuickSettingsActive() && darkDirections(cpuBank) && litPerimeter()>0U &&
          board.SemanticFrame().status_hash!=ordinaryPerimeter,
          "X hold perimeter progress preempts the CPU role before Quick Setup opens");
    checkpoint("X-menu-hold","keep-holding-X");
    advance(500U);edge(false,Button::X);advance(200U);
    Check(board.QuickSettingsActive() && Energy(cpuPixel(Role::Up))>0U &&
          SameControlColor(cpuPixel(Role::Up),pixel(Role::Up)) &&
          SameControlColor(cpuPixel(Role::Down),pixel(Role::Down)),
          "Quick Setup gives both banks the same navigation guidance");
    checkpoint("Quick-Setup","adjust-with-either-bank-or-tap-X");
    edge(true,Button::X);edge(false,Button::X);advance(300U);
    Check(!board.QuickSettingsActive() && redCpuPair(),"menu exit restores the mapped CPU role after its fade");
    checkpoint("Quick-Setup-closed","play-e2-e4","recovered");

    const uint64_t lifted=opening & ~rawBit(e2);
    Check(board.ObserveHallMask(lifted)==VIRTUAL_LITCH_OK &&
          board.ObserveHallMask(lifted | rawBit(e4))==VIRTUAL_LITCH_OK,
          "human moves e2 to e4 through the real Hall opening");
    advance(2000U);
    for(unsigned tick=0U;tick<40U && !board.HomeStateSnapshot().physicalMoveValid;++tick) {
        advance(100U);
    }
    const auto directed=board.HomeStateSnapshot();
    Check(board.UndoHistoryMoveCount()==1U && board.State().side_to_move_black &&
          directed.action==modules::HomeStateAction::Cpu && directed.physicalMoveValid &&
          board.State().required_action==static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary) &&
          darkDirections(cpuBank),
          "directed CPU movement takes priority over the idle role pair");
    char next[24];
    std::snprintf(next,sizeof(next),"lift-%c%c",
        'a'+directed.physicalFromSquare%8U,'8'-directed.physicalFromSquare/8U);
    checkpoint("CPU-move-directed",next);
}

void RunPlayMenuJourney() {
    auto config=Fixture::Config(false);
    test_support::BoundedHintSession fixture(config);
    auto& board=fixture.session;
    constexpr uint64_t opening=UINT64_C(0xFFFF00000000FFFF);
    board.ObserveHallMask(opening);board.Advance(3000U);
    const auto pixel=[&](Role role,uint8_t bank=VIRTUAL_LITCH_OWNER_P1) {
        return board.PhysicalFrame().pixels[256U+hardware::V3_3PhysicalIndexForControl(
            bank==VIRTUAL_LITCH_OWNER_P2,role)];
    };
    const auto edge=[&](Button button,bool down,uint8_t bank=VIRTUAL_LITCH_OWNER_P1) {
        Check(board.SetButtonDown(bank,button,down)==VIRTUAL_LITCH_OK&&board.Advance(30U)==VIRTUAL_LITCH_OK,
            "Play/menu uses raw button input and production debounce");
    };
    const auto checkpoint=[&](const char* stage,const char* result,const char* next,
                              uint32_t paletteAgeMs=UINT32_MAX) {
        const auto text=board.DisplayText();
        std::printf("[RGB_PLAY_MENU_JOURNEY] stage=%s occupancy=%016llx mode=%s selection=%u result=%s text=\"%.*s\" next=%s",
            stage,static_cast<unsigned long long>(board.State().physical_occupied_mask),
            board.QuickSettingsActive()?"Quick-Setup":"Play",board.QuickSettingsSelectedIndex(),result,
            int(text.size()),text.empty()?"":text.data(),next);
        if (paletteAgeMs != UINT32_MAX) {
            std::printf(" elapsed_since_palette_step_ms=%u expiry=500ms", paletteAgeMs);
        } else {
            std::printf(" expiry=150ms-action/menu-fade");
        }
        for(uint8_t bank:{VIRTUAL_LITCH_OWNER_P1,VIRTUAL_LITCH_OWNER_P2}) {
            std::printf(" bank%u[O,Up,Right,Down,Left,X]=",bank);
            for(unsigned role=0U;role<6U;++role) {
                const auto c=pixel(static_cast<Role>(role),bank);std::printf("%u/%u/%u%s",c.r,c.g,c.b,role==5U?"":";");
            }
        }
        std::puts("");
    };
    for(uint8_t owner:{VIRTUAL_LITCH_OWNER_P1,VIRTUAL_LITCH_OWNER_P2}) {
        virtual_litch_action_request action{};action.struct_size=sizeof(action);
        action.abi_version=VIRTUAL_LITCH_CONTROL_ABI_VERSION;action.action=VIRTUAL_LITCH_CONTROL_COACHING;
        action.owner=owner;action.coaching_mutation=static_cast<uint8_t>(modules::CoachingMutation::Disable);
        modules::ControlMutationOutcome outcome;board.DispatchControlAction(action,outcome);
    }
    board.Advance(1200U);
    Check(Energy(pixel(Role::O))==0U&&Energy(pixel(Role::O,VIRTUAL_LITCH_OWNER_P2))==0U,
        "actual disabled Coaching leaves both idle O controls off");
    edge(Button::O,true);board.Advance(1000U);edge(Button::O,false);board.Advance(1200U);
    Check(pixel(Role::O).b>pixel(Role::O).g&&Energy(pixel(Role::O,VIRTUAL_LITCH_OWNER_P2))==0U,
        "real O hold enables only the player's persistent Coaching indication");
    checkpoint("coach-enabled","applied-once","hold-X-for-menu");
    edge(Button::X,true);board.Advance(1000U);edge(Button::X,false);board.Advance(200U);
    Check(board.QuickSettingsActive(),"real X hold opens the existing Quick Setup");
    for(uint8_t bank:{VIRTUAL_LITCH_OWNER_P1,VIRTUAL_LITCH_OWNER_P2}) {
        Check(Energy(pixel(Role::O,bank))==0U&&Energy(pixel(Role::Left,bank))==0U&&
            Energy(pixel(Role::X,bank))>0U&&Energy(pixel(Role::Right,bank))>0U&&Energy(pixel(Role::Up,bank))>0U,
            "menu owns both accepted banks with valid guidance, no O select or Left at first item");
    }
    checkpoint("menu-enter","accepted","adjust-with-either-bank");
    const auto cpu=board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL",0U);
    edge(Button::Up,true,VIRTUAL_LITCH_OWNER_P2);edge(Button::Up,false,VIRTUAL_LITCH_OWNER_P2);
    Check(board.SettingsForPersistenceTest().ReadValue("VS_CPU_LEVEL",0U)==cpu+1U&&
        Energy(pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2))>Energy(pixel(Role::Up)),
        "other-bank Up changes real CPU setting and replaces only its guidance with acknowledgment");
    checkpoint("menu-Up-P2","applied-setting","wait-for-guidance");
    board.Advance(200U);
    Check(Energy(pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2))==Energy(pixel(Role::Up)),
        "menu adjustment acknowledgment expires back to current guidance");
    edge(Button::X,true,VIRTUAL_LITCH_OWNER_P2);edge(Button::X,false,VIRTUAL_LITCH_OWNER_P2);board.Advance(300U);
    Check(!board.QuickSettingsActive()&&Energy(pixel(Role::X))==0U&&Energy(pixel(Role::X,VIRTUAL_LITCH_OWNER_P2))==0U&&
        Energy(pixel(Role::O))>0U&&Energy(pixel(Role::O,VIRTUAL_LITCH_OWNER_P2))==0U,
        "menu exit clears guidance and restores actual per-player Coaching");
    checkpoint("menu-exit","cancelled-and-restored","continue-play");
    edge(Button::O,true);board.Advance(1000U);edge(Button::O,false);board.Advance(1200U);
    Check(Energy(pixel(Role::O))==0U,"second real O hold disables Coaching and fades off");

    modules::ControlMutationOutcome minimal;
    Check(board.MutateSettingJson(4U, "MINIMAL_ANIMATIONS", "true", minimal)==VIRTUAL_LITCH_OK &&
          minimal.status==modules::ControlMutationStatus::Applied,
          "Minimal Animations is active while sampling the palette confirmation and CPU marker");
    const auto cpuUp=pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2);
    const auto cpuDown=pixel(Role::Down,VIRTUAL_LITCH_OWNER_P2);
    Check(Energy(cpuUp)>0U && SameControlColor(cpuUp,cpuDown),
          "the established Minimal CPU seat pair is lit before palette editing");
    constexpr auto DEBOUNCE_MS=hardware::user_input_timing::BUTTON_DEBOUNCE_MS;
    constexpr auto REPEAT_DELAY_MS=hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS;
    const auto rawPaletteEdge=[&](Button button,bool down,uint8_t bank) {
        Check(board.SetButtonDown(bank,button,down)==VIRTUAL_LITCH_OK &&
              board.Advance(DEBOUNCE_MS)==VIRTUAL_LITCH_OK,
              "raw palette edge reaches the shared debounce boundary");
    };
    rawPaletteEdge(Button::Up,true,VIRTUAL_LITCH_OWNER_P1);
    Check(Energy(pixel(Role::Up))>0U&&Energy(pixel(Role::Down))==0U,
        "real piece-color change acknowledges only the pressed direction");
    checkpoint("piece-color-Up","applied-RGB","hold-to-repeat-or-release",0U);
    board.Advance(REPEAT_DELAY_MS-1U);
    const auto beforeRepeat=Energy(pixel(Role::Up));
    board.Advance(1U);
    const auto repeatPeak=Energy(pixel(Role::Up));
    const uint32_t repeatAt=board.State().now_ms;
    Check(beforeRepeat>0U && repeatPeak>beforeRepeat,
          "held palette repeat renews the fade from the currently selected color");
    checkpoint("piece-color-repeat","renewed","release-or-keep-editing",0U);
    rawPaletteEdge(Button::Up,false,VIRTUAL_LITCH_OWNER_P1);
    auto previousEnergy=repeatPeak;
    for (const uint32_t age : {250U,400U,500U}) {
        Check(board.Advance(repeatAt+age-board.State().now_ms)==VIRTUAL_LITCH_OK,
              "sample elapsed time from the last accepted palette step");
        const auto energy=Energy(pixel(Role::Up));
        Check(age==500U ? energy==0U : energy>0U && energy<previousEnergy,
              "the addressed direction fades continuously and is dark at 500ms");
        Check(Energy(pixel(Role::Down))==0U &&
              SameControlColor(cpuUp,pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2)) &&
              SameControlColor(cpuDown,pixel(Role::Down,VIRTUAL_LITCH_OWNER_P2)),
              "palette fade preserves the other direction and the independent CPU bank");
        previousEnergy=energy;
        checkpoint("piece-color-release",age==500U?"expired":"fading","continue-play",age);
    }

    rawPaletteEdge(Button::Down,true,VIRTUAL_LITCH_OWNER_P2);
    const uint32_t otherStepAt=board.State().now_ms;
    Check(!SameControlColor(pixel(Role::Down,VIRTUAL_LITCH_OWNER_P2),
                            pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2)),
          "a P2 Down edit temporarily owns only its addressed CPU control");
    checkpoint("piece-color-Down-P2","applied-RGB","release",0U);
    rawPaletteEdge(Button::Down,false,VIRTUAL_LITCH_OWNER_P2);
    Check(board.Advance(otherStepAt+500U-board.State().now_ms)==VIRTUAL_LITCH_OK &&
          Energy(pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2))>0U &&
          SameControlColor(pixel(Role::Down,VIRTUAL_LITCH_OWNER_P2),
                           pixel(Role::Up,VIRTUAL_LITCH_OWNER_P2)),
          "P2 palette expiry restores the current CPU pair at 500ms");
    checkpoint("piece-color-Down-P2","restored-CPU-pair","continue-play",500U);
}

struct V3TurnTrace {
    std::array<std::array<virtual_litch_color,8>,7> pixels{};
    std::array<unsigned,7> turnEnergy{};
};
V3TurnTrace CaptureV3TurnTrace(uint8_t owner,bool swapped,bool attributed) {
    auto config=Fixture::Config(swapped);config.hardware=VIRTUAL_LITCH_HARDWARE_V3;
    modules::LitchMaxEngine engine;modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession board(config,engine,worker);
    board.SettingsForPersistenceTest().Doc()["MINIMAL_ANIMATIONS"]=true;
    board.SettingsForPersistenceTest().Doc()["TURN_ANIM_STYLE"]="active_end_only";
    Check(board.LoadExternalFen(owner==VIRTUAL_LITCH_OWNER_P2?
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1":
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
        "V3 fade fixture uses the real external-position owner for the requested human turn");
    board.ObserveBootHallMask(UINT64_C(0xFFFF00000000FFFF));board.Advance(4000U);
    virtual_litch_action_request request{};request.struct_size=sizeof(request);
    request.abi_version=VIRTUAL_LITCH_CONTROL_ABI_VERSION;request.action=VIRTUAL_LITCH_CONTROL_HINT;
    // Both requests start the same real Hint. None is the existing app-style
    // attribution with no new per-control acknowledgement, used as reference.
    request.owner=attributed?owner:VIRTUAL_LITCH_OWNER_NONE;request.undo_move_count=1U;
    modules::ControlMutationOutcome outcome;
    Check(board.DispatchControlAction(request,outcome)==VIRTUAL_LITCH_OK&&
        outcome.status==modules::ControlMutationStatus::Applied,"V3 Hint request is genuinely accepted");
    board.Advance(1U);
    constexpr uint8_t middle[]{21U,20U,19U,18U,5U,4U,3U,2U};
    constexpr uint32_t delays[]{0U,50U,50U,50U,49U,1U,50U};
    V3TurnTrace trace;
    for(size_t sample=0U;sample<trace.pixels.size();++sample) {
        board.Advance(delays[sample]);
        for(size_t i=0U;i<trace.pixels[sample].size();++i) {
            trace.pixels[sample][i]=board.PhysicalFrame().pixels[256U+middle[i]];
            trace.turnEnergy[sample]+=Energy(board.SemanticFrame().status[middle[i]]);
        }
        Check(trace.turnEnergy[sample]>0U,"production ordinary Turn underlay is lit during V3 thinking fade");
    }
    const bool physicalP2=(owner==VIRTUAL_LITCH_OWNER_P2)!=swapped;
    const uint8_t physicalBank=physicalP2?VIRTUAL_LITCH_OWNER_P2:VIRTUAL_LITCH_OWNER_P1;
    Check(board.SetButtonDown(physicalBank,Button::Up,true)==VIRTUAL_LITCH_OK&&board.Advance(30U)==VIRTUAL_LITCH_OK,
        "V3 existing palette preview receives raw Up while thinking");
    const size_t first=physicalP2?4U:0U;
    const auto preview=board.PhysicalFrame().pixels[256U+middle[first]];
    bool allFour=Energy(preview)>0U;
    for(size_t i=first;i<first+4U;++i) {
        const auto c=board.PhysicalFrame().pixels[256U+middle[i]];
        allFour=allFour&&c.r==preview.r&&c.g==preview.g&&c.b==preview.b&&c.w==preview.w;
    }
    Check(allFour,"V3 profile retains its original four-pixel held palette preview");
    board.SetButtonDown(physicalBank,Button::Up,false);board.Advance(30U);
    engine.CancelThink();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!worker.CancelAndJoin(engine)&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::yield();
    Check(!worker.IsActive(),"V3 comparison releases the real search owner before the next session");
    return trace;
}
void RunV3ThinkingOverOrdinaryTurn() {
    for(const uint8_t owner:{VIRTUAL_LITCH_OWNER_P1,VIRTUAL_LITCH_OWNER_P2}) {
        for(const bool swapped:{false,true}) {
            // Sequential sessions retain the real engine's single-owner lane.
            const auto reference=CaptureV3TurnTrace(owner,swapped,false);
            const auto overlapping=CaptureV3TurnTrace(owner,swapped,true);
            for(size_t sample=0U;sample<reference.pixels.size();++sample) {
                bool same=reference.turnEnergy[sample]==overlapping.turnEnergy[sample];
                for(size_t i=0U;i<reference.pixels[sample].size();++i) {
                    const auto a=reference.pixels[sample][i],b=overlapping.pixels[sample][i];
                    same=same&&a.r==b.r&&a.g==b.g&&a.b==b.b&&a.w==b.w;
                }
                Check(same,"new feedback preserves the entire genuine V3 thinking/ordinary-Turn crossfade");
                const size_t actingPixel=((owner==VIRTUAL_LITCH_OWNER_P2)!=swapped)?5U:1U;
                const auto a=reference.pixels[sample][actingPixel],b=overlapping.pixels[sample][actingPixel];
                std::printf("[RGB_V3_ORDINARY_TURN] owner=%u swapped=%u sample=%zu underlay_energy=%u expected=%u/%u/%u/%u actual=%u/%u/%u/%u result=%s\n",
                    owner,swapped,sample,reference.turnEnergy[sample],a.r,a.g,a.b,a.w,b.r,b.g,b.b,b.w,same?"preserved":"changed");
            }
        }
    }
}

namespace store = modules::litchcraft_frame_store;
using Stream = modules::litchcraft_frame_stream::FrameStream;
struct Medium {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(store::ARTWORK_FILE_CAPACITY,0xFFU);
    uint32_t writes=0U;
    static bool Read(void* c,size_t at,uint8_t* out,size_t n) {
        auto& m=*static_cast<Medium*>(c); if(at+n>m.bytes.size())return false;
        std::memcpy(out,m.bytes.data()+at,n);return true;
    }
    static bool Write(void* c,size_t at,const uint8_t* in,size_t n) {
        auto& m=*static_cast<Medium*>(c);if(at+n>m.bytes.size())return false;
        std::memcpy(m.bytes.data()+at,in,n);++m.writes;return true;
    }
    static bool Sync(void*) {return true;}
    store::FrameStore::Ops Ops() {return {this,&Read,&Write,&Sync};}
};
void Service(Stream& stream,store::FrameStore& frames,Fixture& fixture,unsigned turns=24U) {
    for(unsigned i=0U;i<turns;++i) {
        (void)stream.Service(frames,false);
        fixture.board.Advance(1U);
    }
}
void Select(Fixture& f,Stream& stream,store::FrameStore& frames,uint8_t frame) {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(f.board.State().now_ms);
    uint8_t payload[6]{2U,frame};fxe::WriteU32LE(stream.Generation(),payload+2U);
    Check(f.board.Litchcraft().ApplyConnectionOrdered(2U,payload,sizeof(payload))==
        modules::LitchcraftSession::Status::Ok,"existing generation-checked app frame selection applies");
    Service(stream,frames,f);
    Check(stream.DisplayedIndex()==frame,"production pager presents selected frame");
}
void RunStoreHistoryJourney() {
    Medium medium;store::FrameStore frames(medium.Ops());
    store::Manifest manifest{};manifest.frameCount=5U;manifest.active=true;manifest.topology=3U;
    store::TransactionToken token{};
    Check(frames.Recover()==store::Status::NoArtwork &&
        frames.Begin(manifest,0U,73U,token)==store::Status::Ok,"Store fixture begins through production owner");
    for(uint16_t i=0U;i<manifest.frameCount;++i) {
        store::Frame frame{};frame.durationMs=1000U;frame.emitters[i].color={50U,80U,120U};
        Check(frames.WriteFrame(token,i,frame)==store::Status::Ok,"Store fixture writes canonical frame");
    }
    Check(frames.Commit(token)==store::Status::Ok,"Store fixture commits authority");
    Stream stream;Check(stream.Configure(frames.CurrentManifest(),frames.Generation()),"Store pager configures");
    Fixture f;f.board.Litchcraft().AttachFrameStream(&stream);Service(stream,frames,f);
    auto& art=f.board.Litchcraft();
    f.board.ObserveHallMask(UINT64_C(1)<<35U);f.board.Advance(30U);
    f.board.ObserveHallMask(0U);f.board.Advance(400U);
    Check(art.UndoCount()==1U,"raw Store brush has one history boundary");
    f.Tap(Button::X,VIRTUAL_LITCH_OWNER_P2);
    Check(art.UndoCount()==0U&&art.RedoCount()==1U,"physical P2 Undo uses Store journal");
    stream.RequestImmediateSave();Service(stream,frames,f,64U);
    Select(f,stream,frames,4U);Select(f,stream,frames,3U);Select(f,stream,frames,2U);Select(f,stream,frames,1U);
    f.board.Advance(200U);
    f.Tap(Button::O,VIRTUAL_LITCH_OWNER_P2);
    Check(art.RedoCount()==1U&&stream.DisplayedIndex()!=0U,
        "deferred Redo waits for target frame without consuming history early");
    const auto pending=f.Pixel(Role::O,VIRTUAL_LITCH_OWNER_P2);
    Check(!(pending.r==pending.g&&pending.g==pending.b&&pending.r>8U),
        "pending Redo has no false physical success pulse");
    f.Capture("Store-Redo-await-page","accepted-pending-no-success","wait-for-frame",VIRTUAL_LITCH_OWNER_P2);
    Service(stream,frames,f);
    Check(art.RedoCount()==0U&&art.UndoCount()==1U&&stream.DisplayedIndex()==0U,
        "deferred completion applies Redo direction rather than replaying Undo");
    const auto done=f.Pixel(Role::O,VIRTUAL_LITCH_OWNER_P2);
    Check(done.g>pending.g,"completed Redo acknowledges the original P2 control bank");
    f.Capture("Store-Redo-complete","applied-Redo-P2","continue-editing",VIRTUAL_LITCH_OWNER_P2);
    f.board.Advance(250U);
    Check(stream.QueueStructuralEdit({stream.Generation(),stream.LiveRevision(),0U,0U,
        Stream::FrameEditAction::AddCopy})==Stream::StructuralResult::Pending,
        "structural fixture queues production Add Copy");
    for(unsigned i=0U;i<96U&&stream.HasPendingStructuralEdit();++i)(void)stream.Service(frames,false);
    uint16_t selected=0U;uint32_t revision=0U;
    Check(stream.TakeStructuralResult(selected,revision)==Stream::StructuralResult::Changed,
        "production storage owner completes structural history fixture");
    f.board.Advance(1U);const auto framesAfterAdd=stream.FrameCount();
    const auto revisionBeforeUndo=stream.LiveRevision();
    const auto writesBeforeUndo=medium.writes;
    const auto quietUndo=f.Pixel(Role::X,VIRTUAL_LITCH_OWNER_P2);
    f.Tap(Button::X,VIRTUAL_LITCH_OWNER_P2);
    Check(stream.FrameCount()+1U==framesAfterAdd,"physical structural Undo applies its RAM layout immediately");
    Check(art.RedoCount()!=0U,"physical structural Undo advances history immediately");
    Check(stream.LiveRevision()==revisionBeforeUndo+1U,"physical structural Undo advances live revision exactly once");
    Check(medium.writes==writesBeforeUndo,"physical structural Undo performs no storage write before RAM publication");
    // Release publishes the layout after this update's structural-completion
    // poll. The next production update renders success without servicing storage.
    f.board.Advance(1U);
    const auto undoPulse=f.Pixel(Role::X,VIRTUAL_LITCH_OWNER_P2);
    const auto undoSweep=f.Pixel(Role::Right,VIRTUAL_LITCH_OWNER_P2);
    Check(undoPulse.r>quietUndo.r&&undoPulse.g>quietUndo.g&&undoPulse.b>quietUndo.b&&
        undoSweep.r==undoPulse.r&&undoSweep.g==undoPulse.g&&undoSweep.b==undoPulse.b&&
        Energy(f.Pixel(Role::Left,VIRTUAL_LITCH_OWNER_P2))==0U&&
        Neutral(f.Pixel(Role::X,VIRTUAL_LITCH_OWNER_P1))&&
        Energy(f.Pixel(Role::X,VIRTUAL_LITCH_OWNER_P1))<Energy(undoPulse)&&
        Energy(f.Pixel(Role::Right,VIRTUAL_LITCH_OWNER_P1))==0U,
        "next update gives only P2 the bright Undo/sweep; P1 retains dim actual Undo availability");
    Check(stream.LiveRevision()==revisionBeforeUndo+1U&&medium.writes==writesBeforeUndo,
        "physical structural Undo success cue needs no storage write or repeated live revision");
    f.Capture("Store-structural-Undo-live","Applied-RAM-without-storage","continue-editing",VIRTUAL_LITCH_OWNER_P2);
    Service(stream,frames,f,96U);
    Check(stream.FrameCount()+1U==framesAfterAdd&&art.RedoCount()!=0U&&
        frames.CurrentManifest().frameCount==stream.FrameCount()&&!stream.HasUnpersistedEdit()&&
        stream.LiveRevision()==revisionBeforeUndo+1U,
        "later Undo durability converges without repeating the live revision or history step");
    const auto revisionBeforeRedo=stream.LiveRevision();
    const auto writesBeforeRedo=medium.writes;
    const auto quietRedo=f.Pixel(Role::O,VIRTUAL_LITCH_OWNER_P2);
    f.Tap(Button::O,VIRTUAL_LITCH_OWNER_P2);
    Check(stream.FrameCount()==framesAfterAdd,"physical structural Redo applies its RAM layout immediately");
    Check(art.RedoCount()==0U,"physical structural Redo advances history immediately");
    Check(stream.LiveRevision()==revisionBeforeRedo+1U,"physical structural Redo advances live revision exactly once");
    Check(medium.writes==writesBeforeRedo,"physical structural Redo performs no storage write before RAM publication");
    f.board.Advance(1U);
    const auto redoPulse=f.Pixel(Role::O,VIRTUAL_LITCH_OWNER_P2);
    const auto redoSweep=f.Pixel(Role::Left,VIRTUAL_LITCH_OWNER_P2);
    Check(redoPulse.r>quietRedo.r&&redoPulse.g>quietRedo.g&&redoPulse.b>quietRedo.b&&
        redoSweep.r==redoPulse.r&&redoSweep.g==redoPulse.g&&redoSweep.b==redoPulse.b&&
        Energy(f.Pixel(Role::Right,VIRTUAL_LITCH_OWNER_P2))==0U&&
        Energy(f.Pixel(Role::Left,VIRTUAL_LITCH_OWNER_P1))==0U&&
        Energy(f.Pixel(Role::Right,VIRTUAL_LITCH_OWNER_P1))==0U,
        "next production update acknowledges P2 structural Redo with O and a Left-to-Right sweep on only its bank");
    Check(stream.LiveRevision()==revisionBeforeRedo+1U&&medium.writes==writesBeforeRedo,
        "physical structural Redo success cue needs no storage write or repeated live revision");
    f.Capture("Store-structural-Redo-live","Applied-RAM-without-storage","continue-editing",VIRTUAL_LITCH_OWNER_P2);
    Service(stream,frames,f,96U);
    Check(stream.FrameCount()==framesAfterAdd&&art.RedoCount()==0U&&
        frames.CurrentManifest().frameCount==stream.FrameCount()&&!stream.HasUnpersistedEdit()&&
        stream.LiveRevision()==revisionBeforeRedo+1U,
        "later Redo durability converges without repeating the live revision or history step");
    f.Capture("Store-structural-history","durable-Undo-Redo-after-live-ACK","continue-editing",VIRTUAL_LITCH_OWNER_P2);
}
}
int main() { RunCraftHistoryAvailabilityJourney(); Run(false); Run(true); RunStoreHistoryJourney(); RunPlayMenuJourney(); RunV3ThinkingOverOrdinaryTurn();
    RunCpuCoachingOwnershipJourney(false); RunCpuCoachingOwnershipJourney(true); return failures==0?0:1; }
