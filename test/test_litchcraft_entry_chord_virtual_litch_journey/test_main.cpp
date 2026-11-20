#include <cstdio>

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/led_mapping.hpp"
#include "modules/orientation.hpp"
#include "virtual_litch_session.hpp"

namespace {

using Button = runtime::VirtualLitchPhysicalButton;

static_assert(modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS == 750U,
              "Play-mode X+O uses the shared 750ms human chord window");

int g_failures = 0;

void Check(const bool condition, const char* const message) {
    if (condition) return;
    std::printf("[FAIL] LITCHCRAFT_ENTRY_CHORD_JOURNEY: %s\n", message);
    ++g_failures;
}

void Set(runtime::VirtualLitchSession& session, const Button button,
         const bool down,
         const uint8_t owner = VIRTUAL_LITCH_OWNER_P1) {
    Check(session.SetButtonDown(owner, button, down) ==
              VIRTUAL_LITCH_OK &&
              session.Advance(30U) == VIRTUAL_LITCH_OK,
          "raw button level settles through production debounce");
}

size_t LitStatusCount(const virtual_litch_semantic_frame& frame) {
    size_t count = 0U;
    for (const auto& color : frame.status) {
        if (color.r != 0U || color.g != 0U || color.b != 0U) ++count;
    }
    return count;
}

bool Observe(runtime::VirtualLitchSession& session,
             const uint64_t logicalOccupancy) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physicalOccupancy = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logicalOccupancy & (UINT64_C(1) << square)) != 0ULL) {
            physicalOccupancy |= UINT64_C(1)
                << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return session.ObserveHallMask(physicalOccupancy) == VIRTUAL_LITCH_OK &&
           session.State().physical_occupied_mask == logicalOccupancy;
}

void RunEmptyBoardCancelAndRetry() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK &&
              !session.Litchcraft().IsActive(),
          "journey reaches eligible empty Setup");

    const uint8_t undoBeforeQuickExit = session.Litchcraft().UndoCount();
    Set(session, Button::X, true);
    Check(!session.Litchcraft().IsActive() &&
              session.Litchcraft().IsEntryHoldActive(),
          "first X edge waits inside the chord-priority window");
    Set(session, Button::X, false);
    modules::ControlMutationOutcome outcome;
    Check(!session.Litchcraft().IsActive() &&
              !session.Litchcraft().IsEntryHoldActive() &&
              session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Undo,
          "released single X leaves entry arbitration and reaches Undo");
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=cancel input=X-release "
        "occupancy=0000000000000000 mode=Setup result=undo-routed "
        "next=retry-X-plus-O\n");

    Set(session, Button::X, true);
    Check(session.Advance(70U) == VIRTUAL_LITCH_OK,
          "human stagger remains inside the chord-priority window");
    Set(session, Button::O, true);
    Check(!session.Litchcraft().IsActive() &&
              session.Litchcraft().IsEntryHoldActive(),
          "100ms-staggered X+O starts entry confirmation without entering");
    const size_t earlyEntryLeds = LitStatusCount(session.SemanticFrame());
    Check(earlyEntryLeds > 0U && earlyEntryLeds < VIRTUAL_LITCH_STATUS_LED_COUNT,
          "held entry chord starts a progressive perimeter rainbow");
    Check(session.Advance(1440U) == VIRTUAL_LITCH_OK &&
              LitStatusCount(session.SemanticFrame()) ==
                  VIRTUAL_LITCH_STATUS_LED_COUNT &&
              !session.Litchcraft().IsActive(),
          "full rainbow confirms readiness but still waits for release");
    Set(session, Button::X, false);
    Check(session.Litchcraft().IsActive() &&
              LitStatusCount(session.SemanticFrame()) <
                  VIRTUAL_LITCH_STATUS_LED_COUNT,
          "first release after the full rainbow enters and restores ordinary status");
    Set(session, Button::O, false);
    Check(session.Litchcraft().IsActive(),
          "chord releases do not immediately exit the new session");
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=retry "
        "input=X-plus-O-release occupancy=0000000000000000 "
        "mode=Litchcraft result=entered-rainbow-confirmed "
        "next=paint-or-X-plus-O-exit\n");

    Set(session, Button::X, true);
    Check(session.Advance(70U) == VIRTUAL_LITCH_OK,
          "active human stagger remains inside the chord-priority window");
    Set(session, Button::O, true);
    Check(session.Litchcraft().IsActive() &&
              session.Litchcraft().IsExitHoldActive(),
          "100ms-staggered active X+O begins exit confirmation without exiting");
    Check(session.Advance(300U) == VIRTUAL_LITCH_OK,
          "quick active chord remains below the confirmation duration");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    Check(session.Litchcraft().IsActive() &&
              !session.Litchcraft().IsExitHoldActive() &&
              session.Litchcraft().UndoCount() == undoBeforeQuickExit,
          "quick active X+O release cancels without exiting or undoing");

    Set(session, Button::X, true);
    Set(session, Button::O, true);
    const uint8_t undoBeforeInterleavedControl =
        session.Litchcraft().UndoCount();
    Set(session, Button::Left, true);
    Set(session, Button::Left, false);
    Check(session.Litchcraft().IsExitHoldActive() &&
              session.Litchcraft().UndoCount() ==
                  undoBeforeInterleavedControl,
          "formed exit chord owns unrelated controls without replaying Undo");
    const size_t earlyExitLeds = LitStatusCount(session.SemanticFrame());
    Check(earlyExitLeds > 0U &&
              earlyExitLeds < VIRTUAL_LITCH_STATUS_LED_COUNT,
          "held exit chord starts the same progressive perimeter rainbow");
    Check(session.Advance(1379U) == VIRTUAL_LITCH_OK &&
              session.Litchcraft().IsActive() &&
              LitStatusCount(session.SemanticFrame()) <
                  VIRTUAL_LITCH_STATUS_LED_COUNT,
          "rainbow reserves its final LED until the release threshold");
    Check(session.Advance(31U) == VIRTUAL_LITCH_OK &&
              session.Litchcraft().IsActive() &&
              LitStatusCount(session.SemanticFrame()) ==
                  VIRTUAL_LITCH_STATUS_LED_COUNT,
          "full exit rainbow confirms readiness but retains Litchcraft until release");
    Set(session, Button::X, false);
    Check(!session.Litchcraft().IsActive(),
          "first release after the full exit rainbow leaves Litchcraft");
    Set(session, Button::O, false);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=active-exit "
        "input=X-plus-O-hold-release occupancy=0000000000000000 "
        "mode=Setup result=exit-rainbow-confirmed next=place-or-reenter\n");

    Set(session, Button::X, true);
    Set(session, Button::O, true);
    Check(session.Advance(modules::LitchcraftSession::ENTRY_HOLD_MS) ==
              VIRTUAL_LITCH_OK,
          "the post-exit empty new-game state completes another entry hold");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    modules::ControlMutationOutcome replayedOutcome;
    Check(session.Litchcraft().IsActive() &&
              (!session.TryGetLastBoardControlOutcome(replayedOutcome) ||
               replayedOutcome.action != modules::ControlAction::NewGame),
          "X+O re-enters Litchcraft after Play instead of leaking into New Game");
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=reenter-after-exit "
        "input=X-plus-O-hold-release occupancy=0000000000000000 "
        "mode=Litchcraft result=reentered-no-reset next=paint\n");
}

void RunEarlyReleaseCancelsWithoutGhostAction() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "early-release journey reaches eligible empty Setup");

    Set(session, Button::X, true);
    Set(session, Button::O, true);
    Check(session.Advance(500U) == VIRTUAL_LITCH_OK &&
              !session.Litchcraft().IsActive() &&
              session.Litchcraft().IsEntryHoldActive(),
          "partial rainbow remains a pending entry confirmation");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    modules::ControlMutationOutcome outcome;
    Check(!session.Litchcraft().IsActive() &&
              !session.Litchcraft().IsEntryHoldActive() &&
              !session.TryGetLastBoardControlOutcome(outcome),
          "early release cancels without replaying Undo, Hint, or New Game");
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=early-release "
        "input=X-plus-O-release-before-full occupancy=0000000000000000 "
        "mode=Setup result=cancelled-no-ghost-action next=retry\n");
}

void RunHeldXFallback() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "held-X journey reaches eligible empty Setup");

    Set(session, Button::X, true);
    Check(session.Advance(1001U) == VIRTUAL_LITCH_OK &&
              !session.Litchcraft().IsActive() &&
              session.QuickSettingsActive(),
          "timed-out entry candidate retains its original press time and opens Quick Setup");
    Set(session, Button::X, false);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=single-hold "
        "input=X-hold-1001ms occupancy=0000000000000000 mode=Setup "
        "result=quick-setup-open next=adjust-or-close\n");
}

void RunFreshOpeningRejectsWithAmber(const bool nearWrap) {
    constexpr uint64_t OPENING_OCCUPANCY = 0xFFFF00000000FFFFULL;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    if (nearWrap) config.initial_time_ms = UINT32_MAX - 4000U;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(OPENING_OCCUPANCY);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "occupied-reset journey reaches ordinary opening Setup");

    Set(session, Button::X, true);
    Set(session, Button::O, true);
    Check(!session.Litchcraft().IsActive() &&
              !session.Litchcraft().IsEntryHoldActive() &&
              session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "fresh occupied opening rejects the mode chord with amber feedback");
    const auto PieceLit = [&]() {
        const auto piece = session.SemanticFrame().board[8U][1U];
        return piece.r != 0U || piece.g != 0U || piece.b != 0U;
    };
    Check(!PieceLit() && session.DisplayText().empty() &&
              LitStatusCount(session.SemanticFrame()) == VIRTUAL_LITCH_STATUS_LED_COUNT,
          "occupied chord rejection blanks occupied pieces during the off phase with amber perimeter");
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK && PieceLit(),
          "occupied warning restores the piece LEDs during the next on phase");
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK && !PieceLit(),
          "occupied warning continues blinking even when its deadline crosses uptime wrap");
    std::printf(
        "[OCCUPIED_FLASH_JOURNEY] clock=%s now=%u input=X-plus-O "
        "occupancy=ffff00000000ffff mode=Setup result=rejected display=quiet "
        "LEDs=piece-off-on-off,amber-perimeter next=remove-pieces\n",
        nearWrap ? "near-wrap" : "ordinary", session.State().now_ms);
    if (nearWrap) {
        // VirtualLitch rejects advancing beyond UINT32_MAX. The production
        // snapshot suite owns the crossing and post-wrap expiry assertions.
        Set(session, Button::X, false);
        Set(session, Button::O, false);
        Check(Observe(session, 0ULL) && !session.Litchcraft().IsActive(),
              "near-wrap warning leaves Setup intact while pieces are removed");
        return;
    }
    Check(session.Advance(1700U) == VIRTUAL_LITCH_OK,
          "occupied rejection remains consumed beyond the New Game hold time");
    modules::ControlMutationOutcome outcome;
    Check(!session.TryGetLastBoardControlOutcome(outcome) ||
              outcome.action != modules::ControlAction::NewGame,
          "fresh occupied opening cannot also leak into New Game");
    Check(PieceLit() && session.EdgeCueIntent().kind !=
              modules::EdgeLedCueKind::OccupiedBoardBlocked,
          "expired warning restores steady occupied pieces and ordinary status");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=fresh-opening-reject "
        "input=X-plus-O occupancy=ffff00000000ffff "
        "mode=Setup result=amber-no-reset next=remove-pieces\n");
    Check(Observe(session, 0ULL), "warning recovery removes the physical pieces");
    Set(session, Button::X, true);
    Set(session, Button::O, true);
    Check(session.Advance(modules::LitchcraftSession::ENTRY_HOLD_MS) == VIRTUAL_LITCH_OK,
          "warning recovery completes the empty-board entry hold");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    Check(session.Litchcraft().IsActive() && session.DisplayText().empty(),
          "warning recovery enters Litchcraft after the ordinary confirmation release");
    std::puts("[OCCUPIED_FLASH_JOURNEY] clock=ordinary input=remove-pieces,X-plus-O-hold-release "
        "occupancy=0000000000000000 mode=Litchcraft result=accepted display=quiet "
        "LEDs=warning-expired,entry-rainbow-complete next=paint-or-exit");
}

void RunOverWindowOccupiedPairDoesNotReset() {
    constexpr uint64_t OPENING_OCCUPANCY = 0xFFFF00000000FFFFULL;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(OPENING_OCCUPANCY);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "staggered-reset journey reaches occupied Setup");

    Set(session, Button::X, true);
    Check(session.Advance(modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS + 1U) ==
              VIRTUAL_LITCH_OK,
          "first X expires into ordinary hold routing");
    Set(session, Button::O, true);
    Check(session.Advance(31U) == VIRTUAL_LITCH_OK &&
              !session.Litchcraft().IsActive(),
          "late O replays without erasing the already-routed X hold");
    modules::ControlMutationOutcome outcome;
    Check(session.Advance(1968U) == VIRTUAL_LITCH_OK &&
              (!session.TryGetLastBoardControlOutcome(outcome) ||
               outcome.action != modules::ControlAction::NewGame),
          "staggered reset does not fire before 2s from the original O edge");
    Check(session.Advance(1000U) == VIRTUAL_LITCH_OK &&
              (!session.TryGetLastBoardControlOutcome(outcome) ||
               outcome.action != modules::ControlAction::NewGame),
          "greater-than-priority-window X+O never requests New Game");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=over-window-occupied-pair "
        "input=X-plus-priority-window-plus-O-hold occupancy=ffff00000000ffff "
        "mode=Setup result=no-chord next=release-and-retry\n");
}

void RunGameInProgressUsesNewGameBranch() {
    constexpr uint64_t OPENING_OCCUPANCY = 0xFFFF00000000FFFFULL;
    constexpr uint8_t E2 = 52U;
    constexpr uint8_t E4 = 36U;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(OPENING_OCCUPANCY);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK &&
              Observe(session,
                      OPENING_OCCUPANCY ^ (UINT64_C(1) << E2)) &&
              session.Advance(30U) == VIRTUAL_LITCH_OK &&
              Observe(session,
                      (OPENING_OCCUPANCY ^ (UINT64_C(1) << E2)) |
                          (UINT64_C(1) << E4)) &&
              session.Advance(2100U) == VIRTUAL_LITCH_OK &&
              session.DisplayText().empty(),
          "ordinary E2-E4 establishes an authoritative game in progress");

    Set(session, Button::X, true);
    Check(session.Advance(720U) == VIRTUAL_LITCH_OK,
          "second face edge approaches the inclusive 750ms boundary");
    Set(session, Button::O, true);
    Check(!session.Litchcraft().IsActive(),
          "game in progress keeps the shared chord on New Game");
    Check(session.Advance(2001U) == VIRTUAL_LITCH_OK,
          "New Game reaches its two-second hold threshold");
    modules::ControlMutationOutcome outcome;
    const bool hasOutcome = session.TryGetLastBoardControlOutcome(outcome);
    Check(hasOutcome &&
              outcome.action == modules::ControlAction::NewGame,
          "game-in-progress chord dispatches exactly the New Game action");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=game-in-progress "
        "input=X-plus-750ms-plus-O-hold mode=Play result=new-game "
        "next=replace-pieces\n");
}

void RunEntryRevalidatesEmptyBoardOnRelease() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "empty untouched Setup reaches Litchcraft admission");

    Set(session, Button::X, true);
    Set(session, Button::O, true);
    Check(session.Litchcraft().IsEntryHoldActive(),
          "empty-board chord starts release-gated entry confirmation");
    Check(Observe(session, UINT64_C(1) << 20U) &&
              session.Advance(modules::LitchcraftSession::ENTRY_HOLD_MS) ==
                  VIRTUAL_LITCH_OK,
          "a newly placed piece changes admission during the rainbow hold");
    Set(session, Button::X, false);
    Set(session, Button::O, false);
    modules::ControlMutationOutcome outcome;
    Check(!session.Litchcraft().IsActive() &&
              session.EdgeCueIntent().kind ==
                  modules::EdgeLedCueKind::OccupiedBoardBlocked &&
              (!session.TryGetLastBoardControlOutcome(outcome) ||
               outcome.action != modules::ControlAction::NewGame),
          "release revalidates occupancy, flashes amber, and cannot enter or reset");
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=revalidate-release "
        "input=empty-chord-then-place-piece mode=Setup "
        "result=amber-no-entry-no-reset next=remove-piece-and-retry\n");
}

void RunForeignReleaseFallback() {
    constexpr uint64_t OPENING_OCCUPANCY = 0xFFFF00000000FFFFULL;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(OPENING_OCCUPANCY);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "foreign-release journey reaches ordinary occupied Setup");

    Set(session, Button::O, true, VIRTUAL_LITCH_OWNER_P1);
    Check(session.Advance(modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS + 1U) ==
              VIRTUAL_LITCH_OK,
          "P1 O expires into ordinary Hint tracking");
    Set(session, Button::X, true, VIRTUAL_LITCH_OWNER_P2);
    Set(session, Button::O, false, VIRTUAL_LITCH_OWNER_P1);
    modules::ControlMutationOutcome outcome;
    Check(session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint,
          "foreign P1 O release reaches Hint while P2 entry is pending");
    Set(session, Button::X, false, VIRTUAL_LITCH_OWNER_P2);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=foreign-release "
        "input=P1-O-tap-overlap-P2-X occupancy=ffff00000000ffff "
        "mode=Setup result=hint-routed next=play\n");
}

void RunForeignHoldFallback() {
    constexpr uint64_t OPENING_OCCUPANCY = 0xFFFF00000000FFFFULL;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(OPENING_OCCUPANCY);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "foreign-hold journey reaches ordinary occupied Setup");

    Set(session, Button::O, true, VIRTUAL_LITCH_OWNER_P1);
    Check(session.Advance(modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS + 1U) ==
              VIRTUAL_LITCH_OK,
          "P1 O expires into ordinary Coaching hold tracking");
    Set(session, Button::X, true, VIRTUAL_LITCH_OWNER_P2);
    Check(session.Advance(940U) == VIRTUAL_LITCH_OK,
          "P1 O remains held while P2 entry arbitration resolves");
    modules::ControlMutationOutcome outcome;
    Check(session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Coaching,
          "foreign pending X does not cancel the P1 O Coaching hold");
    Set(session, Button::O, false, VIRTUAL_LITCH_OWNER_P1);
    Set(session, Button::X, false, VIRTUAL_LITCH_OWNER_P2);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=foreign-hold "
        "input=P1-O-hold-overlap-P2-X occupancy=ffff00000000ffff "
        "mode=Setup result=coaching-routed next=release\n");
}

void RunCrossOwnerBufferIsolation() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    session.ObserveBootHallMask(0U);
    Check(session.Advance(3000U) == VIRTUAL_LITCH_OK,
          "cross-owner journey reaches eligible empty Setup");

    Set(session, Button::X, true);
    Check(session.Advance(modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS + 1U) ==
              VIRTUAL_LITCH_OK,
          "P1 X expires into its ordinary hold controller");
    Set(session, Button::O, true, VIRTUAL_LITCH_OWNER_P2);
    Check(session.Advance(1001U) == VIRTUAL_LITCH_OK &&
              session.QuickSettingsActive() &&
              !session.Litchcraft().IsActive(),
          "buffering P2 O does not cancel P1 held-X Quick Setup");
    Set(session, Button::O, false, VIRTUAL_LITCH_OWNER_P2);
    Set(session, Button::X, false);
    std::printf(
        "[LITCHCRAFT_ENTRY_CHORD_JOURNEY] stage=cross-owner-isolation "
        "input=P1-X-hold-plus-P2-O occupancy=0000000000000000 "
        "mode=Setup result=quick-setup-open next=adjust-or-close\n");
}

void RunCapturedTimestampClassifier() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(1000U);
    modules::LitchcraftSession art;
    art.Configure(3U, 1U);
    const modules::ButtonEvent x{modules::UserButton::P1X,
                                 fxe::Button::Event::Press,
                                 modules::UiOwner::P1};
    const modules::ButtonEvent o{modules::UserButton::P1O,
                                 fxe::Button::Event::Press,
                                 modules::UiOwner::P1};
    const modules::ButtonEvent xRelease{modules::UserButton::P1X,
                                        fxe::Button::Event::Release,
                                        modules::UiOwner::P1};
    Check(art.ObserveEntryButton(x, 100U, true) ==
                  modules::LitchcraftSession::ModeChordResult::Pending &&
              art.ObserveEntryButton(
                  o,
                  100U + modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS + 1U,
                  true) ==
                  modules::LitchcraftSession::ModeChordResult::Replay &&
              !art.IsActive(),
          "batched edges beyond the priority window cannot become a false chord at processing time");
    modules::ButtonEvent replay;
    uint32_t replayTimeMs = 0U;
    Check(art.TakeEntryButtonForReplay(replay, replayTimeMs) &&
              replayTimeMs == 100U,
          "fallback retains the original queued edge timestamp");
    fxe::ManualMillis::SetNowMs(2000U);
    Check(art.ObserveEntryButton(x, 500U, true) ==
                  modules::LitchcraftSession::ModeChordResult::Pending &&
              art.ObserveEntryButton(
                  o,
                  500U + modules::LitchcraftSession::MODE_CHORD_PRIORITY_MS,
                  true) ==
                  modules::LitchcraftSession::ModeChordResult::Pending &&
              art.ObserveEntryButton(xRelease, 2000U, true) ==
                  modules::LitchcraftSession::ModeChordResult::Entered &&
              art.IsActive(),
          "captured-time chord enters only on release after the complete hold");
}

} // namespace

int main() {
    RunEmptyBoardCancelAndRetry();
    RunEarlyReleaseCancelsWithoutGhostAction();
    RunHeldXFallback();
    RunFreshOpeningRejectsWithAmber(false);
    RunFreshOpeningRejectsWithAmber(true);
    RunOverWindowOccupiedPairDoesNotReset();
    RunGameInProgressUsesNewGameBranch();
    RunEntryRevalidatesEmptyBoardOnRelease();
    RunCrossOwnerBufferIsolation();
    RunForeignReleaseFallback();
    RunForeignHoldFallback();
    RunCapturedTimestampClassifier();
    std::printf("[LITCHCRAFT_ENTRY_CHORD_JOURNEY] failures=%d\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
