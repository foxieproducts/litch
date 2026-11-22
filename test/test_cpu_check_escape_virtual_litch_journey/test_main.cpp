#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/chess_strings.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/litch_max_engine.hpp"
#include "modules/orientation.hpp"
#include "modules/vs_cpu_turn.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/square_mapping.hpp"
#include "virtual_litch_session.hpp"

namespace {
constexpr const char* BEFORE_FEN =
    "rnbqkb1r/p1ppp2p/5n2/5p2/4PP1p/2P5/P1PP2P1/1RBQKBNR w Kkq - 0 1";
constexpr const char* CHECKED_FEN =
    "rnbqkb1r/p1ppp2p/5n2/5p1Q/4PP1p/2P5/P1PP2P1/1RB1KBNR b Kkq - 0 1";
// Black's acknowledged Nxh5 completes move one.
constexpr const char* AFTER_FEN =
    "rnbqkb1r/p1ppp2p/8/5p1n/4PP1p/2P5/P1PP2P1/1RB1KBNR w Kkq - 0 2";
constexpr uint8_t QUEEN_FROM = 59U, KNIGHT_FROM = 21U, CAPTURE = 31U, KING = 4U;
int failures = 0;

bool Check(bool condition, const char* run, const char* message) {
    if (!condition) { ++failures; std::printf("[FAIL] %s: %s\n", run, message); }
    return condition;
}
uint64_t Bit(uint8_t square) { return UINT64_C(1) << square; }
uint16_t Energy(virtual_litch_color c) { return c.r + c.g + c.b + c.w; }
bool Rotated(const runtime::VirtualLitchSession& session) {
    return modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2());
}
bool Observe(runtime::VirtualLitchSession& session, uint64_t logical) {
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square)
        if ((logical & Bit(square)) != 0ULL)
            physical |= Bit(runtime::MapSquareIndex(square, Rotated(session)));
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK;
}
uint64_t Occupancy(const char* fen) {
    uint64_t occupied = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        char piece = '\0';
        if (modules::TryGetFenPieceAtSquare(fen, square, piece)) occupied |= Bit(square);
    }
    return occupied;
}
bool Setting(runtime::VirtualLitchSession& session, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    return session.MutateSettingJson(990U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
}
struct Facts {
    bool checked = false, mate = false, cpuValid = false, blackCpu = false;
    uint8_t undoPlies = 0U;
    modules::ThinkDiagnostics cpu{};
};
Facts Read(runtime::VirtualLitchSession& session) {
    Facts facts;
    session.RunControlIngress([](void* context, modules::SettingsStore& settings,
        modules::ControlMutationService&, modules::GamePersistenceCoordinator& coordinator) {
        auto& result = *static_cast<Facts*>(context);
        const auto& derived = coordinator.GetEngineDerivedState();
        result.checked = derived.inCheck;
        result.mate = derived.inCheckmate;
        result.cpuValid = coordinator.TryGetThinkDiagnostics(modules::ThinkRequestKind::Cpu, result.cpu);
        const auto level = settings.ReadValue("VS_CPU_LEVEL", uint32_t{0U});
        const bool firstP2 = settings.ReadValue("GAME_FIRST_PLAYER", uint32_t{1U}) == 2U;
        const bool cpuP2 = settings.ReadValue("VS_CPU_PLAYER", uint32_t{2U}) == 2U;
        result.blackCpu = modules::IsVsCpuTurn(modules::SideToMove::Black, level, firstP2, cpuP2) &&
            !modules::IsVsCpuTurn(modules::SideToMove::White, level, firstP2, cpuP2);
        result.undoPlies = modules::GetVsCpuUndoPlyCount(derived.sideToMove, level, firstP2, cpuP2);
    }, &facts);
    return facts;
}
bool Clean(const runtime::VirtualLitchSession& session) {
    const auto& state = session.State();
    return state.correction_mask == 0ULL && state.human_illegal_mask == 0ULL &&
        state.cpu_illegal_mask == 0ULL && !session.PlacementModeActive();
}
bool FenIs(const runtime::VirtualLitchSession& session, const char* expected) {
    const auto home = session.HomeStateSnapshot();
    return home.fen != nullptr && std::strcmp(home.fen, expected) == 0;
}
bool RawWhiteUndoTap(runtime::VirtualLitchSession& session) {
    const auto owner = modules::MapOwnerToPhysicalHomeEdge(modules::UiOwner::P1,
        modules::ComputeControlBankSwap(session.BoardRotate(), session.FirstPlayerIsP2(), false));
    const auto physicalOwner = owner == modules::UiOwner::P2
        ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    return session.SetButtonDown(physicalOwner, runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
        session.Advance(20U) == VIRTUAL_LITCH_OK &&
        session.SetButtonDown(physicalOwner, runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
        session.Advance(20U) == VIRTUAL_LITCH_OK;
}
bool PieceEmitters(const runtime::VirtualLitchSession& session, uint8_t square, bool lit) {
    const auto targets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3_3S3, square, hardware::SquareLedSlot::Piece, Rotated(session));
    for (uint8_t index = 0U; index < targets.count; ++index)
        if ((Energy(session.PhysicalFrame().pixels[targets.indices[index]]) != 0U) != lit) return false;
    return (Energy(session.SemanticFrame().board[square][1U]) != 0U) == lit;
}
void Checkpoint(runtime::VirtualLitchSession& session, const char* run,
                const char* stage, const char* input, const char* next) {
    const auto& state = session.State();
    const auto facts = Read(session);
    const auto display = session.DisplayText();
    const auto home = session.HomeStateSnapshot();
    std::printf("[CPU_CHECK_ESCAPE_JOURNEY] run=%s stage=%s t=%u input=%s phase=%s action=%u "
        "token=%u occupancy=%016llx expected=%016llx correction=%016llx illegal=%016llx "
        "history=%u black=%u checked=%u mate=%u undo=%u undo_plies=%u hint=%u cpu_request=%u cpu_active=%u cpu_state=%u "
        "directed=%u:%u:%u victim=%u:%u display=\"%.*s\" fen=\"%s\" next=%s\n",
        run, stage, state.now_ms, input,
        runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
        state.required_action, state.pending_action_token,
        static_cast<unsigned long long>(state.physical_occupied_mask),
        static_cast<unsigned long long>(state.expected_occupied_mask),
        static_cast<unsigned long long>(state.correction_mask),
        static_cast<unsigned long long>(state.human_illegal_mask | state.cpu_illegal_mask),
        session.UndoHistoryMoveCount(), state.side_to_move_black, facts.checked, facts.mate,
        session.IsUndoActionPending(), facts.undoPlies, session.HintDisplayRequested(),
        facts.cpu.requestId, facts.cpu.active, static_cast<unsigned>(facts.cpu.state),
        home.physicalMoveValid, home.physicalFromSquare, home.physicalToSquare,
        home.physicalCaptureSquareValid, home.physicalCaptureSquare,
        static_cast<int>(display.size()), display.empty() ? "" : display.data(),
        home.fen == nullptr ? "" : home.fen, next);
    for (const uint8_t square : {KING, KNIGHT_FROM, CAPTURE, QUEEN_FROM}) {
        std::printf("[CPU_CHECK_ESCAPE_LEDS] run=%s stage=%s square=%u ", run, stage, square);
        for (uint8_t slot = 0U; slot < 3U; ++slot) {
            const auto color = session.SemanticFrame().board[square][slot];
            std::printf("semantic%u=%u,%u,%u,%u mapped=", slot, color.r, color.g, color.b, color.w);
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3_3S3, square,
                static_cast<hardware::SquareLedSlot>(slot), Rotated(session));
            for (uint8_t index = 0U; index < targets.count; ++index) {
                const auto pixel = session.PhysicalFrame().pixels[targets.indices[index]];
                std::printf("%u:%u,%u,%u,%u;", targets.indices[index], pixel.r, pixel.g, pixel.b, pixel.w);
            }
        }
        std::puts("");
    }
}

template<class Predicate>
bool Pump(runtime::VirtualLitchSession& session, modules::AsyncThinkWorker& worker,
          Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const uint32_t began = session.State().now_ms;
    while (!ready() && std::chrono::steady_clock::now() < deadline &&
           session.State().now_ms - began < 10000U) {
        // Keep CPU profiles intact instead of racing virtual milliseconds
        // against host thread scheduling. This is work/lifecycle evidence,
        // not an S3 or host latency measurement.
        if (session.Advance(worker.IsActive() ? 0U : 20U) != VIRTUAL_LITCH_OK || !Clean(session)) return false;
        std::this_thread::yield();
    }
    return ready();
}
struct StopWorker {
    modules::LitchMaxEngine& engine;
    modules::AsyncThinkWorker& worker;
    ~StopWorker() {
        // A final human turn may own background Hint work. Cancellation is
        // the only engine operation allowed until its worker has quiesced.
        while (!worker.CancelAndJoin(engine)) std::this_thread::yield();
    }
};

void Journey(uint8_t level, bool rotate, bool firstP2) {
    char run[96], levelJson[4];
    std::snprintf(run, sizeof(run), "Qh5-Nf6xh5/CPU%u/rotate%u/firstP%u",
        level, rotate, firstP2 ? 2U : 1U);
    std::snprintf(levelJson, sizeof(levelJson), "%u", level);
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    modules::LitchMaxEngine engine;
    modules::AsyncThinkWorker worker;
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.board_rotate = rotate;
    config.first_player_is_p2 = firstP2;
    runtime::VirtualLitchSession session(config, engine, worker);
    StopWorker stop{engine, worker};
    uint64_t occupied = Occupancy(BEFORE_FEN);
    if (!Check(session.Reset() == VIRTUAL_LITCH_OK &&
        Setting(session, "VS_CPU_PLAYER", "2") &&
        Setting(session, "VS_CPU_LEVEL", levelJson) && session.LoadExternalFen(BEFORE_FEN) &&
        Observe(session, occupied) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
        FenIs(session, BEFORE_FEN) && !session.State().side_to_move_black && Clean(session) && Read(session).blackCpu,
        run, "real pre-Q position and raw Hall setup admit White human with Black CPU ownership")) return;
    Checkpoint(session, run, "White-ready", "import-pre-Q-FEN/raw-Hall", "lift-White-Queen-d1");

    occupied &= ~Bit(QUEEN_FROM);
    if (!Check(Observe(session, occupied), run, "raw Queen pickup enters production interaction")) return;
    Checkpoint(session, run, "Queen-held", "raw-Hall-remove-d1", "place-White-Queen-h5");
    occupied |= Bit(CAPTURE);
    if (!Check(Observe(session, occupied) && session.Advance(2100U) == VIRTUAL_LITCH_OK &&
        Pump(session, worker, [&] { return session.UndoHistoryMoveCount() == 1U &&
            session.State().side_to_move_black != 0U; }) && FenIs(session, CHECKED_FEN) &&
        Clean(session), run, "Qh5 is acknowledged once through the real move/cancellation seam")) return;
    const auto checked = Read(session);
    if (!Check(checked.checked && !checked.mate, run,
        "actual acknowledged Qh5 checks Black but is not checkmate")) return;
    Checkpoint(session, run, "Qh5-acknowledged", "raw-Hall-place-h5/settle", "wait-for-Black-CPU-directed-capture");

    uint32_t dispatchRequest = checked.cpuValid ? checked.cpu.requestId : 0U;
    bool restarted = false;
    if (!Check(Pump(session, worker, [&] {
        const auto current = Read(session);
        if (current.cpuValid) {
            if (dispatchRequest != 0U && dispatchRequest != current.cpu.requestId) restarted = true;
            dispatchRequest = current.cpu.requestId;
        }
        const auto home = session.HomeStateSnapshot();
        return restarted || (home.action == modules::HomeStateAction::Cpu && home.physicalMoveValid);
    }) && !restarted, run, "actual CPU worker dispatches a physical move without a stalled retry")) {
        Checkpoint(session, run, "CPU-dispatch-failed", "bounded-production-pump", "inspect-worker-and-authority");
        return;
    }
    const auto directed = session.HomeStateSnapshot();
    const auto searched = Read(session);
    const uint32_t request = searched.cpu.requestId;
    if (!Check(directed.physicalFromSquare == KNIGHT_FROM && directed.physicalToSquare == CAPTURE &&
        directed.physicalCaptureSquareValid && directed.physicalCaptureSquare == CAPTURE &&
        searched.cpuValid && request != 0U && request == dispatchRequest &&
        searched.cpu.configuredCpuLevel == level &&
        searched.cpu.latestPublication.fromSquare == KNIGHT_FROM &&
        searched.cpu.latestPublication.toSquare == CAPTURE && searched.cpu.quiescentValid &&
        searched.cpu.state == modules::ThinkState::Done && !worker.IsActive() &&
        session.UndoHistoryMoveCount() == 1U && Clean(session) &&
        PieceEmitters(session, CAPTURE, false), run,
        "real CPU search and production selection both direct Nf6xh5, with Queen removal priority")) return;
    Checkpoint(session, run, "Nf6xh5-directed", "real-engine/real-worker/real-CPU-selector", "remove-White-Queen-h5");
    if (!Check(session.Advance(600U) == VIRTUAL_LITCH_OK &&
        Read(session).cpu.requestId == request && session.UndoHistoryMoveCount() == 1U &&
        Clean(session), run, "waiting for the physical capture retains one completed CPU search")) return;
    Checkpoint(session, run, "CPU-plan-retained", "wait-600ms", "remove-White-Queen-h5");

    occupied &= ~Bit(CAPTURE);
    if (!Check(Observe(session, occupied) && Clean(session) &&
        Read(session).cpu.requestId == request && session.UndoHistoryMoveCount() == 1U,
        run, "victim-first Queen removal is accepted without committing or restarting CPU search")) return;
    Checkpoint(session, run, "Queen-removed", "raw-Hall-remove-h5", "lift-Black-Knight-f6");
    occupied &= ~Bit(KNIGHT_FROM);
    if (!Check(Observe(session, occupied) && Clean(session) &&
        session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary) &&
        PieceEmitters(session, CAPTURE, true) && Read(session).cpu.requestId == request,
        run, "raw Knight pickup keeps both mapped h5 landing emitters visible")) return;
    Checkpoint(session, run, "Knight-held", "raw-Hall-remove-f6", "place-Black-Knight-h5");
    occupied |= Bit(CAPTURE);
    if (!Check(Observe(session, occupied) && Pump(session, worker, [&] {
        return session.UndoHistoryMoveCount() == 2U && !session.State().side_to_move_black &&
            session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle);
    }) && FenIs(session, AFTER_FEN) && Clean(session) &&
        session.State().physical_occupied_mask == occupied &&
        session.State().expected_occupied_mask == occupied && session.State().pending_action_token == 0U &&
        !session.HomeStateSnapshot().physicalMoveValid && !Read(session).checked && !Read(session).mate,
        run, "Nxh5 acknowledges once, removes check, and returns the exact normalized position to White")) return;
    Checkpoint(session, run, "Nxh5-acknowledged", "raw-Hall-place-h5/settle", "White-human-X-takes-back-both-plies");

    const auto undoHealthy = [&](runtime::RequiredPhysicalAction action) {
        const auto facts = Read(session);
        uint8_t hintFrom = 64U, hintTo = 64U;
        return Clean(session) && session.IsUndoActionPending() &&
            session.State().required_action == static_cast<uint8_t>(action) &&
            !session.HomeStateSnapshot().physicalMoveValid && !session.CpuThinkingVisible() &&
            !session.HintDisplayRequested() && !session.TryGetHintMoveSquares(hintFrom, hintTo) &&
            !worker.IsActive() && (!facts.cpuValid || (!facts.cpu.active && facts.cpu.requestId == request));
    };
    // The same raw X must use the firmware's VS-CPU two-ply policy. No direct
    // coordinator request or injected undo count substitutes for that adapter.
    if (!Check(Read(session).undoPlies == 2U && RawWhiteUndoTap(session) &&
        Pump(session, worker, [&] {
            return session.UndoHistoryMoveCount() == 1U &&
                undoHealthy(runtime::RequiredPhysicalAction::LiftPrimary);
        }) && FenIs(session, CHECKED_FEN) &&
        session.State().expected_occupied_mask == Occupancy(CHECKED_FEN),
        run, "White-owned raw X begins the CPU capture inverse and cancels stale thinking")) return;
    Checkpoint(session, run, "Undo-entered", "White-owned-raw-X", "lift-Black-Knight-h5");

    occupied &= ~Bit(CAPTURE);
    if (!Check(Observe(session, occupied) && undoHealthy(runtime::RequiredPhysicalAction::PlacePrimary),
        run, "capture Undo requires the Black Knight to return to f6")) return;
    Checkpoint(session, run, "Undo-Knight-held", "raw-Hall-remove-h5", "return-Black-Knight-f6");
    occupied |= Bit(KNIGHT_FROM);
    if (!Check(Observe(session, occupied) && undoHealthy(runtime::RequiredPhysicalAction::RestoreCapture) &&
        session.UndoHistoryMoveCount() == 1U && FenIs(session, CHECKED_FEN),
        run, "returning the Knight requires captured White Queen restoration before the next inverse")) return;
    Checkpoint(session, run, "Undo-Queen-required", "raw-Hall-place-f6", "restore-captured-White-Queen-h5");

    occupied |= Bit(CAPTURE);
    if (!Check(Observe(session, occupied) && Pump(session, worker, [&] {
        // Fail promptly when a one-ply adapter closes Undo instead of chaining.
        return session.UndoHistoryMoveCount() == 0U || !session.IsUndoActionPending();
    }) && undoHealthy(runtime::RequiredPhysicalAction::LiftPrimary) &&
        session.UndoHistoryMoveCount() == 0U && FenIs(session, BEFORE_FEN) &&
        session.State().expected_occupied_mask == Occupancy(BEFORE_FEN),
        run, "restoring the Queen chains the same raw X into the preceding Qh5 inverse")) {
        Checkpoint(session, run, "Undo-chain-failed", "raw-Hall-restore-Queen-h5", "inspect-raw-X-VS-CPU-ply-policy");
        return;
    }
    Checkpoint(session, run, "Undo-Queen-restored", "raw-Hall-restore-Queen-h5", "lift-White-Queen-h5");
    occupied &= ~Bit(CAPTURE);
    if (!Check(Observe(session, occupied) && undoHealthy(runtime::RequiredPhysicalAction::PlacePrimary),
        run, "the second inverse guides the White Queen back to d1")) return;
    Checkpoint(session, run, "Undo-Queen-held", "raw-Hall-remove-h5", "return-White-Queen-d1");
    occupied |= Bit(QUEEN_FROM);
    uint8_t hintFrom = 64U, hintTo = 64U;
    if (!Check(Observe(session, occupied) && Pump(session, worker, [&] {
        return session.UndoHistoryMoveCount() == 0U && !session.IsUndoActionPending() &&
            session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle);
    }) && FenIs(session, BEFORE_FEN) && !session.State().side_to_move_black && Clean(session) &&
        session.State().physical_occupied_mask == occupied && occupied == Occupancy(BEFORE_FEN) &&
        session.State().expected_occupied_mask == occupied && session.State().pending_action_token == 0U &&
        session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::None) &&
        !session.HomeStateSnapshot().physicalMoveValid && !session.CpuThinkingVisible() &&
        !session.HintDisplayRequested() && !session.TryGetHintMoveSquares(hintFrom, hintTo) &&
        !Read(session).checked && !Read(session).mate,
        run, "two-ply Undo restores exact pre-Q position, empty history and White ownership without stale plans")) return;
    Checkpoint(session, run, "Undo-complete", "raw-Hall-place-Queen-d1/ACK", "White-human-chooses-a-new-move");
}
} // namespace

int main() {
    failures = 0;
    Journey(1U, false, false);
    Journey(3U, true, false);
    Journey(11U, false, true);
    std::printf("[CPU_CHECK_ESCAPE_RESULT] failures=%d engine=real worker=real-async "
        "raw-Hall=Qh5/Nf6xh5/two-ply-Undo physical-S3=unperformed reply-fullmove=2\n", failures);
    return failures == 0 ? 0 : 1;
}
