#include "bounded_hint_engine.hpp"
#include <cstdio>
#include <string>
#include <ArduinoJson.h>
#include "hardware/led_mapping.hpp"
#include "modules/game_persistence.hpp"
#include "modules/orientation.hpp"
#include "runtime/game_clock.hpp"
#include "virtual_litch_session.hpp"

namespace {
constexpr uint64_t OPENING = UINT64_C(0xffff00000000ffff);
constexpr uint8_t DUCK_FROM = 24U;
constexpr uint8_t DUCK_TO = 25U;
int failures = 0;
enum class EarlyPickupMode : uint8_t {
    None,
    Timed,
    BeforeCircle,
    AfterCircle,
};
void Check(bool ok, const char* mode, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] DuckSettling/%s: %s\n", mode, message); }
}
std::string Fen(runtime::VirtualLitchSession& session) {
    StaticJsonDocument<2048> json;
    const auto line = session.HomeStateLine();
    if (deserializeJson(json, line.CStr())) return {};
    return json["fen"] | "";
}
void Checkpoint(runtime::VirtualLitchSession& session, const char* mode,
                const char* stage, const char* next, uint8_t target) {
    const auto& state = session.State();
    const auto square = session.SemanticFrame().board[target][0];
    const auto piece = session.SemanticFrame().board[target][1];
    const auto text = session.DisplayText();
    std::printf("[DUCK_SETTLING_CHECKPOINT] mode=%s stage=%s t=%u occupancy=%016llx "
                "expected=%016llx phase=%u required=%u token=%u history=%u side=%u "
                "hint=%u target=%u square=%u/%u/%u piece=%u/%u/%u text=%.*s next=%s\n",
                mode, stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                state.interaction_phase, state.required_action, state.pending_action_token,
                session.UndoHistoryMoveCount(), state.side_to_move_black,
                session.HintDisplayRequested(), target, square.r, square.g, square.b,
                piece.r, piece.g, piece.b, static_cast<int>(text.size()),
                text.empty() ? "" : text.data(), next);
}
bool Circle(runtime::VirtualLitchSession& session, uint32_t* acceptedAt = nullptr) {
    if (session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
            runtime::VirtualLitchPhysicalButton::O, true) != VIRTUAL_LITCH_OK ||
        session.Advance(20U) != VIRTUAL_LITCH_OK) return false;
    const bool released = session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1,
               runtime::VirtualLitchPhysicalButton::O, false) == VIRTUAL_LITCH_OK &&
           session.Advance(20U) == VIRTUAL_LITCH_OK;
    // The unmatched O press reaches gameplay on debounced release, after
    // X+O arbitration. Its finalize animation starts at that acceptance.
    if (released && acceptedAt != nullptr) *acceptedAt = session.State().now_ms;
    return released;
}
void Journey(const char* mode, const uint32_t duration, const bool confirm,
             const bool relift = false, const bool quickReplacement = false,
             const EarlyPickupMode earlyPickup = EarlyPickupMode::None,
             const char* settleStyle = nullptr,
             const bool atomicReplacement = false,
             const bool acceptedCircleCorrection = false) {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_S3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    uint64_t occupancy = OPENING | (UINT64_C(1) << DUCK_FROM);
    Check(session.Reset() == VIRTUAL_LITCH_OK &&
          session.ObserveHallMask(occupancy) == VIRTUAL_LITCH_OK &&
          session.Advance(200U) == VIRTUAL_LITCH_OK, mode, "opening Duck board ready");
    modules::ControlMutationOutcome outcome;
    const std::string value = std::string("\"") + mode + "\"";
    Check(session.MutateSettingJson(1U, "MOVE_CONFIRM", value.c_str(), outcome) == VIRTUAL_LITCH_OK &&
          (outcome.status == modules::ControlMutationStatus::Applied ||
           outcome.status == modules::ControlMutationStatus::Noop), mode, "production setting accepted");
    if (settleStyle != nullptr) {
        const std::string styleValue =
            std::string("\"") + settleStyle + "\"";
        Check(session.MutateSettingJson(
                  2U, "SETTLE_ANIM_STYLE", styleValue.c_str(), outcome) ==
                  VIRTUAL_LITCH_OK &&
                  (outcome.status == modules::ControlMutationStatus::Applied ||
                   outcome.status == modules::ControlMutationStatus::Noop),
              mode,
              "selected presentation is active for the early-P2 settling boundary");
    }
    const auto initialFen = Fen(session);
    auto observe = [&](uint64_t next) { occupancy = next; return session.ObserveHallMask(next) == VIRTUAL_LITCH_OK; };
    Check(observe(occupancy & ~(UINT64_C(1) << 52U)) &&
          observe(occupancy | (UINT64_C(1) << 36U)), mode, "physical e2-e4 placed");
    if (confirm) {
        Check(Circle(session) && session.Advance(1000U) == VIRTUAL_LITCH_OK,
              mode, "Circle confirms chess half before Duck relocation");
    }
    Check(observe(occupancy & ~(UINT64_C(1) << DUCK_FROM)), mode,
          "Duck pickup bridges the timed chess settle or follows confirmed chess half");
    Check(session.UndoHistoryMoveCount() == 0U && Fen(session) == initialFen &&
          session.State().side_to_move_black == 0U, mode, "chess half remains staged");
    Check(observe(occupancy | (UINT64_C(1) << DUCK_TO)), mode, "valid Duck destination placed");
    const uint32_t placedAt = session.State().now_ms;
    auto pending = [&]() { return session.UndoHistoryMoveCount() == 0U &&
        Fen(session) == initialFen && session.State().side_to_move_black == 0U; };
    Check(pending(), mode, "Duck placement must not immediately commit compound turn");
    Checkpoint(session, mode, "duck-placed-pending", confirm ? "press-Circle-or-relift-Duck" : "wait-or-relift-Duck", DUCK_TO);
    if (acceptedCircleCorrection) {
        Check(confirm && Circle(session) && pending(), mode,
              "Circle accepts Duck confirmation before corrective input");
        const uint64_t replacement =
            (occupancy & ~(UINT64_C(1) << DUCK_TO)) |
            (UINT64_C(1) << (DUCK_TO + 1U));
        Check(observe(replacement) && pending() &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::Correction),
              mode,
              "invalid Duck replacement after Circle enters correction");
        Check(session.Advance(1200U) == VIRTUAL_LITCH_OK && pending() &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::Correction),
              mode,
              "expired accepted-Circle deadline waits for physical correction");
        Checkpoint(session, mode, "accepted-Circle-expired-in-correction",
                   "restore-original-Duck-destination", DUCK_TO);
        const uint64_t restored =
            (occupancy & ~(UINT64_C(1) << (DUCK_TO + 1U))) |
            (UINT64_C(1) << DUCK_TO);
        Check(observe(restored) &&
                  session.Advance(1U) == VIRTUAL_LITCH_OK &&
                  session.UndoHistoryMoveCount() == 1U &&
                  session.State().side_to_move_black == 1U &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::Idle),
              mode,
              "restoration after accepted deadline commits once without a second Circle");
        Checkpoint(session, mode, "accepted-Circle-correction-restored",
                   "P2-selects-chess-move", DUCK_TO);
        return;
    }
    if (earlyPickup != EarlyPickupMode::None) {
        constexpr uint8_t F7 = 13U;
        constexpr uint8_t F5 = 29U;
        if (earlyPickup == EarlyPickupMode::AfterCircle) {
            Check(Circle(session) && pending(), mode,
                  "Circle accepts Duck confirmation without waiting out its animation");
        }
        occupancy &= ~(UINT64_C(1) << F7);
        Check(observe(occupancy), mode,
              "P2 pickup is observed while the Duck placement is settling");
        if (earlyPickup == EarlyPickupMode::BeforeCircle) {
            Check(pending() &&
                      session.State().human_illegal_mask != 0ULL &&
                      session.State().side_to_move_black == 0U,
                  mode,
                  "P2 pickup before Circle is correction and cannot commit the Duck turn");
            occupancy |= UINT64_C(1) << F7;
            Check(observe(occupancy) && pending() &&
                      session.State().interaction_phase == static_cast<uint8_t>(
                          runtime::ChessInteractionPhase::Settling) &&
                      session.State().human_illegal_mask == 0ULL &&
                      session.State().correction_mask == 0ULL,
                  mode,
                  "restoring pre-Circle P2 pickup resumes the pending Duck confirmation");
            return;
        }
        Check(session.UndoHistoryMoveCount() == 1U &&
                  Fen(session) != initialFen &&
                  session.State().side_to_move_black == 1U &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::PieceLifted) &&
                  session.State().selected_square == F7 &&
                  session.State().human_illegal_mask == 0ULL &&
                  session.State().correction_mask == 0ULL,
              mode,
              "P2 pickup immediately confirms the Duck turn and owns ordinary move guidance");
        const bool rotate = modules::ComputeSquareRotate(
            session.BoardRotate(), session.FirstPlayerIsP2());
        const uint8_t physicalF5 = modules::MapSquareToPhysical(F5, rotate);
        const auto semanticF5 = session.SemanticFrame().board[F5][0];
        const auto physicalF5Color = session.PhysicalFrame().pixels[
            hardware::LedIndexForSquare(
                physicalF5, hardware::SquareLedSlot::Square0)];
        Check((semanticF5.r != 0U || semanticF5.g != 0U || semanticF5.b != 0U ||
               semanticF5.w != 0U) &&
                  semanticF5.r == physicalF5Color.r &&
                  semanticF5.g == physicalF5Color.g &&
                  semanticF5.b == physicalF5Color.b &&
                  semanticF5.w == physicalF5Color.w,
              mode,
              "P2 F5 next-action cue reaches the mapped physical square");
        modules::GamePersistenceState persisted;
        Check(session.GameClock() != nullptr &&
                  session.GameClock()->GetActiveSide() == runtime::ClockSide::Black,
              mode,
              "early pickup hands the active clock from P1 to P2 exactly once");
        Check(modules::ReadGamePersistenceState(
                  session.SettingsForPersistenceTest().Doc(), persisted) &&
                  persisted.undoHistoryCount ==
                      modules::UNDO_HISTORY_FIELDS_PER_MOVE,
              mode,
              "early pickup durably records one complete compound turn");
        Checkpoint(session, mode, "early-P2-pickup", "place-f7-piece-or-return-it", F5);
        occupancy |= UINT64_C(1) << F5;
        Check(observe(occupancy) &&
                  session.State().interaction_phase == static_cast<uint8_t>(
                      runtime::ChessInteractionPhase::Settling) &&
                  session.UndoHistoryMoveCount() == 1U &&
                  session.State().side_to_move_black == 1U &&
                  session.State().human_illegal_mask == 0ULL &&
                  session.State().correction_mask == 0ULL,
              mode,
              "P2 placement enters its own ordinary settle without correction debt");
        Checkpoint(session, mode, "early-P2-placed", "finish-P2-move-then-relocate-Duck", F5);
        return;
    }
    if (relift) {
        if (atomicReplacement) {
            const uint64_t replacement =
                (occupancy & ~(UINT64_C(1) << DUCK_TO)) |
                (UINT64_C(1) << (DUCK_TO + 1U));
            Check(observe(replacement) && pending() &&
                      session.State().human_illegal_mask != 0ULL &&
                      session.State().correction_mask != 0ULL,
                  mode,
                  "atomic Duck relift plus replacement enters correction without committing");
            Checkpoint(session, mode, "atomic-replacement-correction",
                       "restore-original-Duck-destination", DUCK_TO);
            const uint64_t restored =
                (occupancy & ~(UINT64_C(1) << (DUCK_TO + 1U))) |
                (UINT64_C(1) << DUCK_TO);
            Check(observe(restored) && pending() &&
                      session.State().interaction_phase == static_cast<uint8_t>(
                          runtime::ChessInteractionPhase::Settling) &&
                      session.State().human_illegal_mask == 0ULL &&
                      session.State().correction_mask == 0ULL,
                  mode,
                  "restoring the atomic Duck change resumes the original pending destination");
            Checkpoint(session, mode, "atomic-replacement-restored",
                       "wait-for-fresh-confirmation", DUCK_TO);
            return;
        }
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK &&
              observe(occupancy & ~(UINT64_C(1) << DUCK_TO)) &&
              session.Advance(quickReplacement ? 100U : 6000U) == VIRTUAL_LITCH_OK && pending(), mode,
              "relifting Duck cancels old deadline and preserves staged chess half");
        Checkpoint(session, mode, "duck-relifted", "choose-a-Duck-destination", DUCK_TO);
        Check(observe(occupancy | (UINT64_C(1) << (DUCK_TO + 1U))) && pending(), mode,
              "replacement Duck destination starts a new uncommitted placement");
    }
    const uint8_t target = relift ? DUCK_TO + 1U : DUCK_TO;
    if (confirm) {
        Check(session.Advance(6000U) == VIRTUAL_LITCH_OK && pending(), mode,
              "Confirm mode never auto-commits Duck placement");
        uint32_t acceptedAt = 0U;
        Checkpoint(session, mode, "waiting-for-Circle", "press-Circle-or-relift-Duck", target);
        Check(Circle(session, &acceptedAt), mode, "Circle accepts Duck confirmation");
        Check(pending(), mode, "Circle retains finalize animation before commit");
        Check(session.Advance(999U - (session.State().now_ms - acceptedAt)) == VIRTUAL_LITCH_OK && pending(), mode,
              "accepted Duck confirmation stays pending at 999 ms");
        Checkpoint(session, mode, "Circle-999ms", "wait-for-finalize", target);
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK, mode, "finish confirmation animation at 1000 ms");
    } else {
        Check(session.Advance(duration - 1U) == VIRTUAL_LITCH_OK && pending(), mode,
              "Duck placement remains pending one millisecond before deadline");
        Checkpoint(session, mode, "deadline-minus-1ms", "wait-or-relift-Duck", target);
        Check(session.Advance(1U) == VIRTUAL_LITCH_OK, mode, "reach configured deadline");
    }
    Check(session.UndoHistoryMoveCount() == 1U && Fen(session) != initialFen &&
          session.State().side_to_move_black == 1U, mode, "deadline commits compound turn exactly once");
    Checkpoint(session, mode, "compound-turn-committed", "P2-selects-chess-move", target);
    Check(session.Advance(1000U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 1U,
          mode, "later ticks do not duplicate history");
    std::printf("[DUCK_SETTLING] mode=%s relift=%u quick_replacement=%u placed_at=%u now=%u history=%u side=%u phase=%u fen=%s\n",
                mode, relift, quickReplacement, placedAt, session.State().now_ms, session.UndoHistoryMoveCount(),
                session.State().side_to_move_black, session.State().interaction_phase, Fen(session).c_str());
}
}
int main() {
    Journey("settle_2s", 2000U, false);
    Journey("settle_5s", 5000U, false);
    Journey("press_circle_to_confirm", 0U, true);
    Journey("settle_2s", 2000U, false, true);
    Journey("settle_5s", 5000U, false, true);
    Journey("press_circle_to_confirm", 0U, true, true);
    Journey("settle_2s", 2000U, false, true, true);
    Journey("settle_5s", 5000U, false, true, true);
    for (const char* style : {"landing_flash", "mini_orbit", "ripple"}) {
        Journey("settle_2s", 2000U, false, false, false,
                EarlyPickupMode::Timed, style);
        Journey("settle_5s", 5000U, false, false, false,
                EarlyPickupMode::Timed, style);
    }
    Journey("press_circle_to_confirm", 0U, true, false, false,
            EarlyPickupMode::BeforeCircle);
    Journey("press_circle_to_confirm", 0U, true, false, false,
            EarlyPickupMode::AfterCircle);
    Journey("settle_2s", 2000U, false, true, false,
            EarlyPickupMode::None, nullptr, true);
    Journey("press_circle_to_confirm", 0U, true, false, false,
            EarlyPickupMode::None, nullptr, false, true);
    std::printf("DuckSettling failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
