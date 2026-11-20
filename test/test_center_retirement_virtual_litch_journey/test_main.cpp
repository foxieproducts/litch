#include <array>
#include <cstdio>

#include "bounded_hint_engine.hpp"
#include "firmware/sleep_wake_pin_policy.hpp"
#include "modules/button_state_wire_codec.hpp"
#include "runtime/virtual_litch_session.h"

namespace {
int failures = 0;
void Check(const bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] CENTER_RETIREMENT %s\n", message); }
}

void LegacyWakeAndNormalizedIngress() {
    for (const auto revision : {hardware::BoardRevision::V2S3,
                               hardware::BoardRevision::V3S3,
                               hardware::BoardRevision::V3_3S3}) {
        const auto profile = hardware::BoardHardwareProfileFor(revision);
        const uint8_t unusedP2 = revision == hardware::BoardRevision::V2S3 ? 18U : 16U;
        const auto runtimePins = firmware::BuildRuntimeButtonPinSet(profile);
        for (const bool motion : {false, true}) {
            const auto wake = firmware::BuildDeepSleepWakePinSet(profile, motion);
            Check(!wake.Contains(8U) && !wake.Contains(unusedP2),
                  "unused contact GPIOs cannot wake the board");
        }
        Check(runtimePins.count == 12U && !runtimePins.Contains(8U) && !runtimePins.Contains(unusedP2),
              "runtime and wake-release sampling contain only twelve fitted controls");
        for (const auto owner : {modules::UiOwner::P1, modules::UiOwner::P2})
            Check(modules::button_state_wire_codec::SwitchIndex(
                {static_cast<modules::UserButton>(4U), fxe::Button::Event::Press, owner}) < 0,
                "reserved ID4 never publishes a center switch state");
    }
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    virtual_litch* board = nullptr;
    Check(virtual_litch_create(&config, &board) == VIRTUAL_LITCH_OK && board != nullptr,
          "existing native control ingress fixture creates");
    if (!board) return;
    Check(virtual_litch_request_sleep(board, VIRTUAL_LITCH_SLEEP_ORIGIN_IDLE) == VIRTUAL_LITCH_OK,
          "existing Sleep owner begins cancellable countdown");
    for (const uint8_t owner : {uint8_t(VIRTUAL_LITCH_OWNER_P1), uint8_t(VIRTUAL_LITCH_OWNER_P2)})
        for (const uint8_t event : {uint8_t(VIRTUAL_LITCH_BUTTON_PRESS_EVENT), uint8_t(VIRTUAL_LITCH_BUTTON_RELEASE_EVENT)}) {
            Check(virtual_litch_dispatch_button(board, owner, 4U, event) == VIRTUAL_LITCH_OK,
                  "retired numeric input is ignored without breaking its carrier");
        }
    virtual_litch_sleep_lifecycle sleep{};virtual_litch_default_sleep_lifecycle(&sleep);
    Check(virtual_litch_get_sleep_lifecycle(board, &sleep) == VIRTUAL_LITCH_OK &&
          sleep.state == VIRTUAL_LITCH_SLEEP_COUNTDOWN && sleep.cancel_cause == 0U,
          "reserved input cannot count as activity or cancel Sleep");
    std::printf("[CENTER_RETIREMENT] stage=reserved-ingress sleep=%u cancel=%u next=real-control-only\n",sleep.state,sleep.cancel_cause);
    virtual_litch_destroy(board);
}

void RawPlayJourney(const uint8_t hardwareId) {
    virtual_litch_config config{};virtual_litch_default_config(&config);config.hardware=hardwareId;
    test_support::BoundedHintSession fixture(config);auto& board=fixture.session;
    const uint64_t occupied=(UINT64_C(1)<<7U)|(UINT64_C(1)<<48U)|(UINT64_C(1)<<63U);
    Check(board.Reset()==VIRTUAL_LITCH_OK&&board.LoadExternalFen("7k/8/8/8/8/8/P7/7K w - - 0 1")&&
          board.ObserveHallMask(occupied)==VIRTUAL_LITCH_OK&&board.Advance(2000U)==VIRTUAL_LITCH_OK,
          "raw production journey reaches an actual quiet game");
    for (const uint8_t owner:{uint8_t(VIRTUAL_LITCH_OWNER_P1),uint8_t(VIRTUAL_LITCH_OWNER_P2)}) {
        Check(board.SetButtonDown(owner,static_cast<runtime::VirtualLitchPhysicalButton>(4U),true)==VIRTUAL_LITCH_OK&&
              board.Advance(1020U)==VIRTUAL_LITCH_OK&&
              board.SetButtonDown(owner,static_cast<runtime::VirtualLitchPhysicalButton>(4U),false)==VIRTUAL_LITCH_OK&&
              board.Advance(20U)==VIRTUAL_LITCH_OK&&!board.QuickSettingsActive()&&board.UndoHistoryMoveCount()==0U,
              "raw unused contact tap/hold/release has no menu or game action");
        const auto beforeMenu=board.SemanticFrame();
        Check(board.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,true)==VIRTUAL_LITCH_OK&&
              board.Advance(20U)==VIRTUAL_LITCH_OK&&board.Advance(1000U)==VIRTUAL_LITCH_OK&&board.QuickSettingsActive(),
              "actual X long press opens Quick Setup through production debounce and gesture owners");
        Check(board.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,false)==VIRTUAL_LITCH_OK&&
              board.Advance(20U)==VIRTUAL_LITCH_OK&&board.QuickSettingsActive(),"release retains Quick Setup");
        uint8_t sample=255U;
        for(uint8_t square=0U;square<64U;++square) {
            const auto a=beforeMenu.board[square][0U];const auto b=board.SemanticFrame().board[square][0U];
            if((b.r>32U||b.g>32U||b.b>32U)&&(a.r!=b.r||a.g!=b.g||a.b!=b.b)){sample=square;break;}
        }
        Check(sample<64U,"accepted Quick Setup renders a visible board glyph through production composition");
        if(sample<64U) {
            const auto targets=hardware::MapSemanticSquareLedTargets(hardware::BoardRevisionFromId(hardwareId),sample,hardware::SquareLedSlot::Square0,false);
            const auto semantic=board.SemanticFrame().board[sample][0U];const auto physical=board.PhysicalFrame().pixels[targets.indices[0U]];
            std::printf("[CENTER_RETIREMENT_RENDER] hardware=%u square=%u semantic=%u,%u,%u mapped=%u,%u,%u,%u menu_item=%u next=LeftRight-select-UpDown-adjust\n",
                hardwareId,sample,semantic.r,semantic.g,semantic.b,physical.r,physical.g,physical.b,physical.w,board.QuickSettingsSelectedIndex());
        }
        std::printf("[CENTER_RETIREMENT] hardware=%u owner=%u t=%u occupancy=%016llx phase=%u action=%u menu=%u text=%.*s history=%u next=tap-X-to-close\n",
            hardwareId,owner,board.State().now_ms,(unsigned long long)board.State().physical_occupied_mask,
            board.State().interaction_phase,board.State().required_action,board.QuickSettingsActive(),
            int(board.DisplayText().size()),board.DisplayText().data(),board.UndoHistoryMoveCount());
        Check(board.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,true)==VIRTUAL_LITCH_OK&&
              board.Advance(20U)==VIRTUAL_LITCH_OK&&!board.QuickSettingsActive()&&
              board.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,false)==VIRTUAL_LITCH_OK&&
              board.Advance(20U)==VIRTUAL_LITCH_OK&&board.UndoHistoryMoveCount()==0U,
              "X closes without leaking an Undo action");
    }
}
}
int main() {
    LegacyWakeAndNormalizedIngress();
    for (const uint8_t id:{uint8_t(VIRTUAL_LITCH_HARDWARE_S3),uint8_t(VIRTUAL_LITCH_HARDWARE_V3),uint8_t(VIRTUAL_LITCH_HARDWARE_V3_3)}) RawPlayJourney(id);
    std::printf("[CENTER_RETIREMENT] failures=%d\n",failures);return failures?1:0;
}
