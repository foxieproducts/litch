#include <cstdio>

#include "bounded_hint_engine.hpp"
#include "hardware/status_led_chain_profile.hpp"
#include "modules/orientation.hpp"
#include "runtime/square_mapping.hpp"

namespace {
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
using Button = runtime::VirtualLitchPhysicalButton;
using Role = hardware::ControlLedRole;
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { std::printf("[FAIL] SETUP_COACHING_ELIGIBILITY %s\n", message); ++failures; }
}

void Journey(bool swapped, bool rotated) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.first_player_is_p2 = swapped;
    config.board_rotate = rotated;
    test_support::BoundedHintSession fixture(config);
    auto& board = fixture.session;
    const auto bank = [&](modules::UiOwner owner) {
        return static_cast<uint8_t>(modules::MapOwnerToPhysicalHomeEdge(owner,
            modules::ComputeControlBankSwap(rotated, swapped, false)));
    };
    const auto observe = [&](uint64_t occupancy) {
        uint64_t physical = 0U;
        for (uint8_t square = 0U; square < 64U; ++square)
            if ((occupancy & (UINT64_C(1) << square)) != 0U)
                physical |= UINT64_C(1) << runtime::MapSquareIndex(square,
                    modules::ComputeSquareRotate(rotated, swapped));
        Check(board.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
            board.Advance(200U) == VIRTUAL_LITCH_OK, "raw Hall occupancy reaches the production setup owner");
    };
    const auto setting = [&](const char* key, const char* value) {
        modules::ControlMutationOutcome outcome;
        Check(board.MutateSettingJson(0U, key, value, outcome) == VIRTUAL_LITCH_OK &&
            (outcome.status == modules::ControlMutationStatus::Applied ||
             outcome.status == modules::ControlMutationStatus::Noop), "acknowledged preference/role mutation");
    };
    const auto edge = [&](modules::UiOwner owner, bool down) {
        Check(board.SetButtonDown(bank(owner), Button::O, down) == VIRTUAL_LITCH_OK &&
            board.Advance(20U) == VIRTUAL_LITCH_OK, "raw O passes debounce and chord arbitration");
    };
    const auto checkpoint = [&](const char* stage, const char* result, const char* next) {
        const auto home = board.HomeStateSnapshot();
        const auto text = board.DisplayText();
        std::printf("[SETUP_COACHING_ELIGIBILITY] swapped=%u rotated=%u stage=%s t=%u "
            "occupancy=%016llx phase=%u placement=%u saved=%u Hint=%u useful=%u "
            "result=%s text=\"%.*s\" next=%s\n", swapped, rotated, stage,
            board.State().now_ms, static_cast<unsigned long long>(home.occupiedMask),
            board.State().interaction_phase, home.placementMode, home.coachingMask,
            board.HintDisplayRequested(), home.assistance.manualAffordanceUseful,
            result, int(text.size()), text.empty() ? "" : text.data(), next);
    };
    const auto hold = [&](modules::UiOwner owner, uint8_t expectedMask) {
        const auto revision = board.SettingsForPersistenceTest().GetRevision();
        edge(owner, true);
        Check(board.Advance(999U) == VIRTUAL_LITCH_OK &&
            board.SettingsForPersistenceTest().GetRevision() == revision,
            "a subthreshold O hold cannot change a preference");
        Check(board.Advance(1U) == VIRTUAL_LITCH_OK, "O reaches the one-second boundary");
        modules::ControlMutationOutcome outcome;
        Check(board.TryGetLastBoardControlOutcome(outcome) &&
            outcome.action == modules::ControlAction::Coaching &&
            outcome.status == modules::ControlMutationStatus::Applied &&
            board.HomeStateSnapshot().coachingMask == expectedMask &&
            board.SettingsForPersistenceTest().GetRevision() == revision + 1U,
            "human O hold toggles only its saved preference with one acknowledged revision");
        Check(board.Advance(500U) == VIRTUAL_LITCH_OK, "held input cannot repeat");
        edge(owner, false);
        Check(board.HomeStateSnapshot().coachingMask == expectedMask &&
            board.SettingsForPersistenceTest().GetRevision() == revision + 1U,
            "release cannot also Hint or repeat Coaching");
    };
    board.ObserveBootHallMask(0U);
    observe(OPENING & ~(UINT64_C(1) << 52U));
    setting("P1_AUTO_HINTS", "true");
    Check(board.Advance(3000U) == VIRTUAL_LITCH_OK, "boot reaches ordinary incomplete setup");
    Check(board.HomeStateSnapshot().placementMode && !board.HintDisplayRequested() &&
        !board.HomeStateSnapshot().assistance.manualAffordanceUseful,
        "incomplete setup owns physical guidance and disables Hint");
    checkpoint("P1-saved-on-incomplete", "setup", "hold-P1-O-to-disable-preference");
    hold(modules::UiOwner::P1, 0U);
    hold(modules::UiOwner::P2, 2U);
    Check(board.HomeStateSnapshot().placementMode && !board.HintDisplayRequested(),
        "both human preference changes preserve setup and suppress move guidance");
    edge(modules::UiOwner::P1, true); edge(modules::UiOwner::P1, false);
    Check(!board.HintDisplayRequested() && board.HomeStateSnapshot().coachingMask == 2U,
        "short O cannot request Hint before setup completes");
    checkpoint("setup-preferences-changed", "applied-once", "place-missing-e2-pawn");
    observe(OPENING);
    Check(!board.HomeStateSnapshot().placementMode &&
        board.HomeStateSnapshot().assistance.manualAffordanceUseful,
        "the completed stable opening enables the existing Hint affordance");
    checkpoint("setup-complete", "ready", "play-or-request-Hint");

    // P2 is the opponent while the human P1 retains the playable opening.
    if (rotated) setting("MINIMAL_ANIMATIONS", "true");
    setting("VS_CPU_PLAYER", "2"); setting("VS_CPU_LEVEL", "1");
    const auto cpuRevision = board.SettingsForPersistenceTest().GetRevision();
    edge(modules::UiOwner::P2, true);
    Check(board.Advance(1000U) == VIRTUAL_LITCH_OK, "CPU O reaches its hold threshold");
    edge(modules::UiOwner::P2, false);
    modules::ControlMutationOutcome rejected;
    Check(board.TryGetLastBoardControlOutcome(rejected) &&
        rejected.action == modules::ControlAction::Coaching &&
        rejected.status == modules::ControlMutationStatus::Rejected &&
        rejected.reason == modules::ControlMutationReason::EngineRejected &&
        board.HomeStateSnapshot().coachingMask == 2U &&
        board.SettingsForPersistenceTest().GetRevision() == cpuRevision,
        "CPU O produces a truthful rejection without changing the saved preference");
    const auto pixel = [&]() {
        return board.PhysicalFrame().pixels[256U + hardware::V3_3PhysicalIndexForControl(
            bank(modules::UiOwner::P2) == VIRTUAL_LITCH_OWNER_P2, Role::O)];
    };
    Check(board.Advance(60U) == VIRTUAL_LITCH_OK, "sample the first gentle pulse");
    auto first = pixel();
    Check(first.r > first.g && first.g > first.b && first.g != 0U,
        "first mapped CPU O pulse is amber");
    Check(board.Advance(280U) == VIRTUAL_LITCH_OK, "sample the gap");
    auto gap = pixel();
    Check(gap.r == 0U && gap.g == 0U && gap.b == 0U, "a quiet gap separates the two pulses");
    Check(board.Advance(140U) == VIRTUAL_LITCH_OK, "sample the second gentle pulse");
    auto second = pixel();
    Check(second.r > second.g && second.g > second.b && second.g != 0U,
        "second mapped CPU O pulse is amber");
    Check(board.Advance(500U) == VIRTUAL_LITCH_OK, "feedback expires without replay");
    auto expired = pixel();
    std::printf("[CPU_O_FEEDBACK] logical=P2 physical-bank=%u phases=first/gap/second/expired "
        "RGB=%u,%u,%u/%u,%u,%u/%u,%u,%u/%u,%u,%u pulse=300ms gap=120ms "
        "result=rejected next=human-move-or-change-opponent text=none\n",
        bank(modules::UiOwner::P2), first.r, first.g, first.b, gap.r, gap.g, gap.b,
        second.r, second.g, second.b, expired.r, expired.g, expired.b);
    Check(expired.r == 0U && expired.g == 0U && expired.b == 0U &&
        board.DisplayText().empty() && board.SettingsForPersistenceTest().GetRevision() == cpuRevision,
        "two pulses expire with no text, false success, preference write or held replay");
    Check(board.EdgeCueIntent().kind != modules::EdgeLedCueKind::Unavailable,
        "the CPU rejection cannot also flash a generic perimeter message");
    checkpoint("CPU-O-rejected-two-pulses", "rejected-retained-preference", "human-move-or-change-opponent");
    setting("VS_CPU_LEVEL", "0");
    observe(OPENING | (UINT64_C(1) << 36U) | (UINT64_C(1) << 37U));
    hold(modules::UiOwner::P2, 0U);
    Check(!board.HintDisplayRequested() && !board.HomeStateSnapshot().assistance.manualAffordanceUseful,
        "ordinary setup correction also permits saved preferences without move guidance");
    observe(OPENING);
    checkpoint("correction-recovered", "ready", "play");
}
} // namespace

int main() {
    Journey(false, false);
    Journey(true, false);
    Journey(false, true);
    std::printf("[SETUP_COACHING_ELIGIBILITY] failures=%d; production input/action/render; "
        "only continuous Hint search is node-bounded by the existing fixture\n", failures);
    return failures == 0 ? 0 : 1;
}
