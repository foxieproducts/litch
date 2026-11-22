#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/led_layout.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/game_persistence.hpp"
#include "modules/promotion_piece_cue.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace
{
constexpr const char* FEN = "8/k1P5/2K5/8/8/8/8/8 w - - 0 1";
constexpr uint8_t FROM = 10U; // c7
constexpr uint8_t TO = 2U; // c8
constexpr uint64_t BEFORE = (UINT64_C(1) << 8U) | (UINT64_C(1) << FROM) |
                            (UINT64_C(1) << 18U);
constexpr uint64_t AFTER = (BEFORE & ~(UINT64_C(1) << FROM)) |
                           (UINT64_C(1) << TO);
int failures = 0;

bool Check(const bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::printf("[FAIL] CPU_UNDERPROMOTION: %s\n", message);
    }
    return condition;
}

bool Set(runtime::VirtualLitchSession& session, const uint16_t id,
         const char* key, const char* value)
{
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(id, key, value, outcome) == VIRTUAL_LITCH_OK &&
           (outcome.status == modules::ControlMutationStatus::Applied ||
            outcome.status == modules::ControlMutationStatus::Noop);
}

uint16_t Energy(const virtual_litch_color color)
{
    return static_cast<uint16_t>(color.r) + color.g + color.b + color.w;
}

bool Guided(const runtime::VirtualLitchSession& session, const uint8_t square)
{
    const auto& pixels = session.PhysicalFrame().pixels;
    return Energy(pixels[hardware::LedIndexForSquare(
               square, hardware::SquareLedSlot::Square0)]) != 0U ||
           Energy(pixels[hardware::LedIndexForSquare(
               square, hardware::SquareLedSlot::Square2)]) != 0U;
}

bool AmberGuided(const runtime::VirtualLitchSession& session, const uint8_t square)
{
    const auto color = session.PhysicalFrame().pixels[
        hardware::LedIndexForSquare(square, hardware::SquareLedSlot::Square0)];
    return color.r > color.g && color.g > color.b;
}

bool RookCue(const runtime::VirtualLitchSession& session)
{
    const uint64_t rook = modules::promotion_piece_cue::BoardMask(
        modules::PromotionPiece::Rook);
    for (uint8_t row = 1U; row <= 6U; ++row)
    {
        for (uint8_t file = 1U; file <= 6U; ++file)
        {
            const uint8_t square = row * 8U + file;
            const auto color = session.PhysicalFrame().pixels[
                hardware::LedIndexForSquare(square, hardware::SquareLedSlot::Square0)];
            if ((rook & (UINT64_C(1) << square)) == 0ULL)
            {
                if (Energy(color) != 0U) return false;
            }
            else if (color.b <= color.r || color.b <= color.g)
            {
                return false;
            }
        }
    }
    // CPU owns the choice: the human Left/Right guidance must stay absent.
    for (const uint8_t edge : {11U, 12U, 27U, 28U})
    {
        if (Energy(session.SemanticFrame().status[edge]) != 0U ||
            Energy(session.PhysicalFrame().pixels[
                hardware::FirstEdgeLedIndex() + edge]) != 0U)
            return false;
    }
    return true;
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* stage,
                const char* input, const char* next)
{
    const auto& state = session.State();
    std::printf("[CPU_UNDERPROMOTION_JOURNEY] stage=%s t=%u input=%s "
                "phase=%s required=%u occupancy=%016llx expected=%016llx "
                "correction=%016llx token=%u history=%u promotion_modal=%u "
                "display=%.*s next=%s home=%s\n",
                stage, state.now_ms, input,
                runtime::ChessInteractionPhaseToString(
                    static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                state.required_action,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                static_cast<unsigned long long>(state.correction_mask),
                state.pending_action_token, session.UndoHistoryMoveCount(),
                session.PromotionActive(), static_cast<int>(session.DisplayText().size()),
                session.DisplayText().data(), next, session.HomeStateLine().CStr());
    std::printf("[CPU_UNDERPROMOTION_LEDS] stage=%s semantic_square_rgb=", stage);
    for (uint8_t square = 0U; square < 64U; ++square)
    {
        const auto color = session.SemanticFrame().board[square][0];
        std::printf("%02x%02x%02x,", color.r, color.g, color.b);
    }
    std::printf(" physical_square_rgbw=");
    for (uint8_t square = 0U; square < 64U; ++square)
    {
        const auto color = session.PhysicalFrame().pixels[
            hardware::LedIndexForSquare(square, hardware::SquareLedSlot::Square0)];
        std::printf("%02x%02x%02x%02x,", color.r, color.g, color.b, color.w);
    }
    std::printf("\n");
}

bool CheckImmutableAdapterResult()
{
    // Supporting async boundary coverage. The physical journey below has its
    // own real CPU search; it does not inject this result into a CPU queue.
    modules::LitchMaxEngine engine;
    modules::AsyncThinkWorker worker;
    engine.Reset();
    if (!Check(engine.SetFen(FEN), "real adapter imports underpromotion seed"))
        return false;
    modules::ThinkConfig config;
    config.difficulty = 3U;
    config.maxDepth = 1U;
    config.maxNodes = 4096U;
    config.candidateObserver = worker.PrepareCandidateObserver();
    if (!Check(engine.StartThink(config) && worker.Start(engine),
               "real async worker starts the pure Classic search"))
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!worker.IsDone() || worker.IsActive()) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!Check(worker.IsDone() && !worker.IsActive(), "async publication completes cleanup"))
        return false;
    modules::ThinkCandidate candidate;
    const bool typed = worker.TryGetPublishedCandidate(candidate) &&
                       candidate.fromSquare == FROM && candidate.toSquare == TO &&
                       candidate.promotion == 'r' && candidate.completedDepth == 1U;
    uint8_t progress = 0U;
    modules::UciMove move;
    uint32_t elapsed = 0U;
    uint32_t nodes = 0U;
    const auto state = worker.GetResult(progress, move, elapsed, nodes);
    std::printf("[CPU_UNDERPROMOTION_IMMUTABLE] typed=%u from=%u to=%u suffix=%c "
                "depth=%u state=%u uci=%s nodes=%u scheduling=real-async\n",
                typed, candidate.fromSquare, candidate.toSquare,
                candidate.promotion == '\0' ? '-' : candidate.promotion,
                candidate.completedDepth, static_cast<unsigned>(state), move.CStr(), nodes);
    return Check(typed && state == modules::ThinkState::Done && move.Equals("c7c8r"),
                 "search Rook suffix survives both immutable candidate and final UCI publication");
}

void RunPhysicalJourney()
{
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    // The existing fixture bounds only continuous Human Hint work. CPU search,
    // rules, queue, controls, physical actions, acknowledgement and rendering
    // remain production owners. Its synchronous polling is disclosed here.
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    if (!Check(session.Reset() == VIRTUAL_LITCH_OK &&
                   session.ObserveHallMask(UINT64_C(0xFFFF00000000FFFF)) == VIRTUAL_LITCH_OK &&
                   session.Advance(200U) == VIRTUAL_LITCH_OK &&
                   Set(session, 980U, "WHITE_SQUARES_ON", "false") &&
                   Set(session, 981U, "P1_END_PIECE_WHEEL", "170") &&
                   Set(session, 982U, "P2_END_PIECE_WHEEL", "0") &&
                   Set(session, 983U, "VS_CPU_LEVEL", "3") &&
                   Set(session, 984U, "VS_CPU_PLAYER", "1") &&
                   session.LoadExternalFen(FEN) &&
                   session.ObserveHallMask(BEFORE) == VIRTUAL_LITCH_OK &&
                   session.Advance(200U) == VIRTUAL_LITCH_OK,
               "canonical settings/import/raw observations admit the actual CPU3 turn"))
        return;
    bool ready = false;
    for (uint8_t sample = 0U; sample < 40U; ++sample)
    {
        ready = session.State().interaction_phase == static_cast<uint8_t>(
                    runtime::ChessInteractionPhase::ExecutingPlan) &&
                session.State().required_action == static_cast<uint8_t>(
                    runtime::RequiredPhysicalAction::LiftPrimary);
        if (ready || session.Advance(100U) != VIRTUAL_LITCH_OK) break;
    }
    if (!Check(ready && session.HomeStateSnapshot().physicalMoveValid &&
                   session.HomeStateSnapshot().physicalFromSquare == FROM &&
                   session.HomeStateSnapshot().physicalToSquare == TO &&
                   Guided(session, FROM) && Guided(session, TO) &&
                   session.UndoHistoryMoveCount() == 0U,
               "actual CPU search queues c7-c8 and guides pickup/destination before commit"))
        return;
    Checkpoint(session, "selected", "actual-CPU3-search/synchronous-poll", "lift-pawn:c7");
    if (!Check(session.Advance(800U) == VIRTUAL_LITCH_OK && RookCue(session) &&
                   !session.PromotionActive(),
               "after destination prelude the blue Rook silhouette appears without selection controls"))
        return;
    Checkpoint(session, "rook-cue", "prelude-expired", "lift-pawn:c7/place-Rook:c8");

    if (!Check(session.ObserveHallMask(BEFORE & ~(UINT64_C(1) << FROM)) == VIRTUAL_LITCH_OK &&
                   session.State().required_action == static_cast<uint8_t>(
                       runtime::RequiredPhysicalAction::PlacePrimary) && Guided(session, TO),
               "raw pawn pickup keeps c8 guided for the selected replacement"))
        return;
    Checkpoint(session, "pawn-held", "raw-Hall:remove-c7", "place-Rook:c8");
    constexpr uint64_t WRONG_TARGET = (BEFORE & ~(UINT64_C(1) << FROM)) |
                                      (UINT64_C(1) << 1U); // b8 instead of c8
    if (!Check(session.ObserveHallMask(WRONG_TARGET) == VIRTUAL_LITCH_OK &&
                   session.Advance(200U) == VIRTUAL_LITCH_OK &&
                   session.State().expected_occupied_mask == BEFORE &&
                   session.UndoHistoryMoveCount() == 0U &&
                   session.State().side_to_move_black == 0U &&
                   session.HomeStateSnapshot().physicalMoveValid &&
                   session.HomeStateSnapshot().physicalToSquare == TO &&
                   AmberGuided(session, 1U) && RookCue(session),
               "wrong b8 placement gets tentative amber and retains the uncommitted Rook/c8 plan"))
        return;
    Checkpoint(session, "wrong-square", "raw-Hall:place-b8/stable200ms", "lift-amber-b8/restore-c8-plan");
    if (!Check(session.ObserveHallMask(BEFORE & ~(UINT64_C(1) << FROM)) == VIRTUAL_LITCH_OK &&
                   session.Advance(200U) == VIRTUAL_LITCH_OK &&
                   session.State().required_action == static_cast<uint8_t>(
                       runtime::RequiredPhysicalAction::PlacePrimary) && Guided(session, TO) &&
                   session.State().correction_mask == 0ULL && RookCue(session),
               "removing the wrong replacement resumes c8 guidance and the same Rook choice"))
        return;
    Checkpoint(session, "recovered", "raw-Hall:remove-b8", "place-Rook:c8");
    if (!Check(session.ObserveHallMask(AFTER) == VIRTUAL_LITCH_OK &&
                   session.Advance(200U) == VIRTUAL_LITCH_OK &&
                   session.State().expected_occupied_mask == AFTER &&
                   session.State().side_to_move_black != 0U &&
                   session.UndoHistoryMoveCount() == 1U &&
                   std::strstr(session.HomeStateLine().CStr(),
                       "\"fen\":\"2R5/k7/2K5/8/8/8/8/8 b ") != nullptr &&
                   !fixture.engine.IsInStalemate(modules::SideToMove::Black),
               "acknowledged placement commits an actual Rook and advances one move/turn"))
        return;
    Checkpoint(session, "committed", "raw-Hall:place-c8/stable200ms", "reseat-Rook-or-Black-turn");
    // Occupancy sensors cannot identify the replacement. The committed engine
    // FEN above establishes suffix retention; Hall input establishes handling.
    Check(session.ObserveHallMask(AFTER & ~(UINT64_C(1) << TO)) == VIRTUAL_LITCH_OK &&
              session.Advance(100U) == VIRTUAL_LITCH_OK &&
              session.State().correction_mask == 0ULL && !session.IsUndoActionPending() &&
              session.ObserveHallMask(AFTER) == VIRTUAL_LITCH_OK &&
              session.Advance(2100U) == VIRTUAL_LITCH_OK,
          "brief replacement reseat stays calm and returns through CPU settle");
    const auto completed = session.HomeStateLine();
    Check(session.State().interaction_phase == static_cast<uint8_t>(
              runtime::ChessInteractionPhase::Idle) &&
              session.State().pending_action_token == 0U &&
              session.State().physical_occupied_mask == AFTER &&
              session.State().expected_occupied_mask == AFTER &&
              session.State().correction_mask == 0ULL && !session.PromotionActive() &&
              session.UndoHistoryMoveCount() == 1U &&
              std::strstr(completed.CStr(), "\"m\":\"c7c8----w\"") != nullptr &&
              std::strstr(completed.CStr(), "\"fen\":\"2R5/k7/2K5/8/8/8/8/8 b ") != nullptr &&
              session.Advance(2500U) == VIRTUAL_LITCH_OK &&
              completed.Equals(session.HomeStateLine().View()),
          "expiry clears the physical plan without another promotion or history record");
    Checkpoint(session, "settled", "re-lift100ms/reseat/settle-expiry", "Black-move");
}
}

int main()
{
    failures = 0;
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    if (CheckImmutableAdapterResult()) RunPhysicalJourney();
    std::printf("[CPU_UNDERPROMOTION_RESULT] failures=%d scheduling=split-async-publication/synchronous-physical "
                "bounded-later-Hint=4096nodes\n", failures);
    return failures == 0 ? 0 : 1;
}
