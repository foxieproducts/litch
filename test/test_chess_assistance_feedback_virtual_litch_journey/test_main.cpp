#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <string>

#include "bounded_hint_engine.hpp"
#include "foxie_embedded/millis.hpp"
#include "hardware/board_led_output_mapping.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/control_led_feedback.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/edge_led_constants.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/game_persistence_coordinator.hpp"
#include "modules/home_state_wire_codec.hpp"
#include "modules/led_frame_composer_internal.hpp"
#include "modules/led_render_policy.hpp"
#include "modules/orientation.hpp"
#include "modules/ui_color_wheel.hpp"
#include "runtime/chess_interaction_model.hpp"
#include "runtime/game_state_machine.hpp"
#include "runtime/interaction_dispatcher.hpp"
#include "runtime/square_mapping.hpp"

namespace {
constexpr uint64_t OPENING = UINT64_C(0xFFFF00000000FFFF);
int failures = 0;
void Check(bool ok, const char* run, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] %s: %s\n", run, message); }
}
uint64_t Bit(uint8_t square) { return UINT64_C(1) << square; }
uint16_t Energy(virtual_litch_color c) { return c.r + c.g + c.b + c.w; }
bool Equal(virtual_litch_color a, virtual_litch_color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.w == b.w;
}
bool Rotated(const runtime::VirtualLitchSession& s) {
    return modules::ComputeSquareRotate(s.BoardRotate(), s.FirstPlayerIsP2());
}
bool Observe(runtime::VirtualLitchSession& s, uint64_t logical) {
    uint64_t physical = 0ULL;
    for (uint8_t sq = 0U; sq < 64U; ++sq)
        if ((logical & Bit(sq)) != 0ULL)
            physical |= Bit(runtime::MapSquareIndex(sq, Rotated(s)));
    const auto result = s.ObserveHallMask(physical);
    std::printf("[CHESS_ASSISTANCE_INPUT] t=%u logical=%016llx physical=%016llx admission=%u\n",
                s.State().now_ms, static_cast<unsigned long long>(logical),
                static_cast<unsigned long long>(physical), static_cast<unsigned>(result));
    return result == VIRTUAL_LITCH_OK;
}
// Config selects V3. Its physical topology differs from the native semantic
// frame's three-slot projection; use the existing runtime mapper for both.
constexpr auto PHYSICAL_TOPOLOGY = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3S3);
uint16_t EdgeBase(const runtime::VirtualLitchSession&) {
    return PHYSICAL_TOPOLOGY.physicalBoardLedCount;
}
virtual_litch_color PhysicalSlot(const runtime::VirtualLitchSession& s, uint8_t logical,
                                 hardware::SquareLedSlot slot) {
    const auto targets = hardware::MapSemanticSquareLedTargets(
        (s.PhysicalFrame().led_count == 300U ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3), logical, slot, Rotated(s));
    return s.PhysicalFrame().pixels[targets.indices[0U]];
}
virtual_litch_color Square(const runtime::VirtualLitchSession& s, uint8_t logical) {
    return PhysicalSlot(s, logical, hardware::SquareLedSlot::Square0);
}
virtual_litch_color Piece(const runtime::VirtualLitchSession& s, uint8_t logical) {
    return PhysicalSlot(s, logical, hardware::SquareLedSlot::Piece);
}
struct SquarePixels {
    std::array<virtual_litch_color, 3U> semantic{};
    std::array<virtual_litch_color, 4U> mapped{};
};
SquarePixels AllSquarePixels(const runtime::VirtualLitchSession& s, uint8_t square) {
    SquarePixels pixels; uint8_t output = 0U;
    for (uint8_t slot = 0U; slot < 3U; ++slot) {
        pixels.semantic[slot] = s.SemanticFrame().board[square][slot];
        const auto targets = hardware::MapSemanticSquareLedTargets(
            (s.PhysicalFrame().led_count == 300U ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3), square, static_cast<hardware::SquareLedSlot>(slot), Rotated(s));
        for (uint8_t target = 0U; target < targets.count; ++target)
            pixels.mapped[output++] = s.PhysicalFrame().pixels[targets.indices[target]];
    }
    return pixels;
}
bool SameSquarePixels(const SquarePixels& a, const SquarePixels& b) {
    for (uint8_t slot = 0U; slot < a.semantic.size(); ++slot)
        if (!Equal(a.semantic[slot], b.semantic[slot])) return false;
    for (uint8_t target = 0U; target < a.mapped.size(); ++target)
        if (!Equal(a.mapped[target], b.mapped[target])) return false;
    return true;
}
bool HasSteadyWhiteLegalRails(const SquarePixels& square, const bool firstPlayerIsP2) {
    // These route fixtures configure near/far wheels to 0/85 respectively.
    const uint8_t whiteWheel = firstPlayerIsP2 ? 85U : 0U;
    const auto rgb = modules::ScaleRgbSaturatingBy256(
        modules::ui_color_wheel::WheelToRgb(static_cast<uint8_t>(whiteWheel + 16U)),
        modules::led_render_policy::COMPOSER_GLOBAL_BRIGHTNESS_BOOST_SCALE256);
    const virtual_litch_color expected{rgb.r, rgb.g, rgb.b, 0U};
    return Equal(square.semantic[0U], expected) && Equal(square.semantic[2U], expected) &&
           Energy(square.mapped[0U]) > 0U && Energy(square.mapped[3U]) > 0U;
}
bool EmptyPieceTargets(const runtime::VirtualLitchSession& s, uint8_t square) {
    if (Energy(s.SemanticFrame().board[square][1U]) != 0U) return false;
    const auto targets = hardware::MapSemanticSquareLedTargets(
        (s.PhysicalFrame().led_count == 300U ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3), square, hardware::SquareLedSlot::Piece, Rotated(s));
    for (uint8_t target = 0U; target < targets.count; ++target)
        if (Energy(s.PhysicalFrame().pixels[targets.indices[target]]) != 0U) return false;
    return true;
}
modules::UiOwner Owner(const runtime::VirtualLitchSession& s, bool black = false) {
    return modules::MapOwnerToPhysicalHomeEdge(
        black ? modules::UiOwner::P2 : modules::UiOwner::P1,
        modules::ComputeControlBankSwap(s.BoardRotate(), s.FirstPlayerIsP2(), false));
}
bool Button(runtime::VirtualLitchSession& s, bool down) {
    return s.SetButtonDown(Owner(s, s.State().side_to_move_black != 0U) == modules::UiOwner::P2 ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1,
                          runtime::VirtualLitchPhysicalButton::O, down) == VIRTUAL_LITCH_OK &&
           s.Advance(20U) == VIRTUAL_LITCH_OK;
}
bool Tap(runtime::VirtualLitchSession& s) { return Button(s, true) && Button(s, false); }
bool Coach(runtime::VirtualLitchSession& s) {
    bool ok = Button(s, true);
    for (uint8_t i = 0U; i < 11U; ++i) ok = ok && s.Advance(100U) == VIRTUAL_LITCH_OK;
    ok = ok && Button(s, false);
    modules::ControlMutationOutcome outcome;
    return ok && s.TryGetLastBoardControlOutcome(outcome) &&
           outcome.action == modules::ControlAction::Coaching &&
           outcome.status == modules::ControlMutationStatus::Applied &&
           s.Advance(1000U) == VIRTUAL_LITCH_OK;
}
bool Setting(runtime::VirtualLitchSession& s, const char* key, const char* value) {
    modules::ControlMutationOutcome result;
    return s.MutateSettingJson(91U, key, value, result) == VIRTUAL_LITCH_OK &&
           (result.status == modules::ControlMutationStatus::Applied || result.status == modules::ControlMutationStatus::Noop);
}
bool Align(runtime::VirtualLitchSession& s, uint32_t now) {
    return now >= s.State().now_ms && s.Advance(now - s.State().now_ms) == VIRTUAL_LITCH_OK;
}
virtual_litch_config Config(bool rotate = false) {
    virtual_litch_config config{}; virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = rotate;
    return config;
}
struct Facts { uint64_t capturers = 0ULL; bool completed = false; };
Facts Read(runtime::VirtualLitchSession& s) {
    Facts facts;
    s.RunControlIngress([](void* context, modules::SettingsStore&, modules::ControlMutationService&,
                           modules::GamePersistenceCoordinator& coordinator) {
        auto& f = *static_cast<Facts*>(context);
        f.capturers = coordinator.GetCaptureFirstCapturerMask();
        modules::UciMove best; uint32_t ms = 0U, nodes = 0U; uint8_t difficulty = 0U;
        f.completed = coordinator.TryGetLastHintThinkStats(best, ms, nodes, difficulty);
    }, &facts);
    return facts;
}
void Checkpoint(runtime::VirtualLitchSession& s, const char* run, const char* stage, const char* next,
                const char* outcome = "accepted", uint64_t watchedSquares = 0ULL) {
    const auto facts = Read(s);
    const auto& state = s.State();
    const auto display = s.DisplayText();
    uint8_t from = 64U, to = 64U;
    const bool hint = s.TryGetHintMoveSquares(from, to);
    std::printf("[CHESS_ASSISTANCE_JOURNEY] run=%s stage=%s t=%u phase=%s action=%u token=%u outcome=%s "
                "occupancy=%016llx expected=%016llx correction=%016llx illegal=%016llx "
                "hint_requested=%u hint=%u:%u:%u capturers=%016llx completed=%u history=%u "
                "display=\"%.*s\" home=%s next=%s\n", run, stage, state.now_ms,
                runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                state.required_action, state.pending_action_token, outcome,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                static_cast<unsigned long long>(state.correction_mask),
                static_cast<unsigned long long>(state.human_illegal_mask | state.cpu_illegal_mask),
                s.HintDisplayRequested(), hint, from, to,
                static_cast<unsigned long long>(facts.capturers), facts.completed, s.UndoHistoryMoveCount(),
                static_cast<int>(display.size()), display.data(), s.HomeStateLine().CStr(), next);
    std::printf("[CHESS_ASSISTANCE_RGBW] run=%s stage=%s corners=", run, stage);
    for (const uint8_t index : modules::PlayerCornerLedIndices(Owner(s))) {
        const uint8_t chainIndex = s.PhysicalFrame().led_count == 300U
            ? hardware::V3_3PhysicalIndexForSemanticStatus(index) : index;
        const auto c = s.PhysicalFrame().pixels[EdgeBase(s) + chainIndex];
        std::printf("%u:%u,%u,%u,%u;", index, c.r, c.g, c.b, c.w);
    }
    std::printf(" middle_bank=");
    for (uint8_t local = 2U; local < 6U; ++local) {
        const uint8_t index = modules::PlayerHomeLedIndex(Owner(s), local);
        const uint8_t chainIndex = s.PhysicalFrame().led_count == 300U
            ? hardware::V3_3PhysicalIndexForSemanticStatus(index) : index;
        const auto color = s.PhysicalFrame().pixels[EdgeBase(s) + chainIndex];
        std::printf("%u:%u,%u,%u,%u;", index, color.r, color.g, color.b, color.w);
    }
    std::printf(" capture_squares=");
    for (uint8_t sq = 0U; sq < 64U; ++sq) if (((facts.capturers | watchedSquares) & Bit(sq)) != 0ULL) {
        std::printf("%u:", sq);
        for (uint8_t slot = 0U; slot < 3U; ++slot) {
            const auto c = s.SemanticFrame().board[sq][slot];
            std::printf("slot%u_semantic=%u,%u,%u,%u/mapped=", slot, c.r, c.g, c.b, c.w);
            const auto targets = hardware::MapSemanticSquareLedTargets(
                (s.PhysicalFrame().led_count == 300U ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3), sq, static_cast<hardware::SquareLedSlot>(slot), Rotated(s));
            for (uint8_t target = 0U; target < targets.count; ++target) {
                const auto mapped = s.PhysicalFrame().pixels[targets.indices[target]];
                std::printf("%u:%u,%u,%u,%u,", targets.indices[target], mapped.r, mapped.g, mapped.b, mapped.w);
            }
            std::printf("/");
        }
        std::printf(";");
    }
    std::puts("");
}
bool CornersWhite(const runtime::VirtualLitchSession& s) {
    for (const uint8_t index : modules::PlayerCornerLedIndices(Owner(s))) {
        const auto semantic = s.SemanticFrame().status[index];
        const auto physical = s.PhysicalFrame().pixels[EdgeBase(s) + index];
        if (semantic.r == 0U || semantic.r != semantic.g || semantic.g != semantic.b ||
            physical.r == 0U || physical.r != physical.g || physical.g != physical.b ||
            physical.b != physical.w) return false;
    }
    return true;
}
// This journey selects older V3: pending thinking is delivered on the middle
// four physical home LEDs, leaving completed-result corner feedback separate.
bool PendingBankVisible(const runtime::VirtualLitchSession& s) {
    uint8_t lit = 0U;
    for (uint8_t local = 2U; local < 6U; ++local) {
        const auto color = s.PhysicalFrame().pixels[
            EdgeBase(s) + modules::PlayerHomeLedIndex(Owner(s), local)];
        if (Energy(color) == 0U) continue;
        ++lit;
        const bool purple = color.r > 0U && color.g == 0U && color.b > color.r;
        const bool orange = color.r > color.g && color.g > 0U && color.b == 0U;
        if (color.w != 0U || (!purple && !orange)) return false;
    }
    return lit > 0U && lit <= 2U;
}
bool SameCorners(const runtime::VirtualLitchSession& a, const runtime::VirtualLitchSession& b) {
    for (const uint8_t index : modules::PlayerCornerLedIndices(Owner(a)))
        if (!Equal(a.PhysicalFrame().pixels[EdgeBase(a) + index],
                   b.PhysicalFrame().pixels[EdgeBase(b) + index])) return false;
    return true;
}

// Controlled search output is used ONLY for exact publication/timing failures.
// FEN, legality and actual moves delegate to the real LitchMaxEngine. Session,
// raw controls/Halls, mailbox, coordinator, snapshots and RGBW remain production.
// This fixture does not claim search quality or physical wall responsiveness.
class PublicationEngine final : public modules::IEngine {
public:
    void Reset() override { rules.Reset(); cancelled = false; }
    bool SetFen(const char* fen) override { return rules.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { return rules.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override {
        if (polling.load(std::memory_order_acquire)) ++appliedDuringPoll;
        return rules.ApplyUciMove(move);
    }
    modules::SideToMove GetSideToMove() const override { return rules.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return rules.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return rules.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return rules.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        ++legalReads;
        if (legalPreparationMs != 0U) {
            const uint32_t elapsed = legalPreparationMs; legalPreparationMs = 0U;
            fxe::ManualMillis::AdvanceMs(elapsed);
        }
        if (failLegal) { mask = 0ULL; return false; }
        return rules.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig& config) override {
        cfg = config; startedMaxThinkMs = config.maxThinkMs;
        cancelled = false; ++starts;
        if (!realSearch) return true;
        auto bounded = config;
        bounded.maxDepth = 5U;
        bounded.progressObserver = {[](void* context, uint32_t nodes) {
            auto& self = *static_cast<PublicationEngine*>(context);
            self.cfg.progressObserver.Publish(nodes);
            if (nodes >= 128U && !self.realSearchPaused.exchange(true)) {
                // Controlled scheduling stall inside the actual production
                // search, before Full5. Session remains the only clock owner.
                while (!self.releaseRealSearch.load() && !self.cancelled.load())
                    std::this_thread::yield();
            }
        }, this};
        return rules.StartThink(bounded);
    }
    void CancelThink() override {
        cancelled = true; ++cancels;
        if (realSearch) rules.CancelThink();
    }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override {
        struct PollScope {
            std::atomic<bool>& active;
            explicit PollScope(std::atomic<bool>& value) : active(value) { active.store(true, std::memory_order_release); }
            ~PollScope() { active.store(false, std::memory_order_release); }
        } scope(polling);
        if (realSearch) return rules.PollThinkProgress(progress, move);
        do {
            // A completed generation must observe the candidate stored before finish.
            const bool done = cfg.continuousAnalysis ? finish.load() : finishCpu.load();
            const uint16_t encoded = candidate.load();
            const uint8_t from = static_cast<uint8_t>(encoded >> 8U), to = static_cast<uint8_t>(encoded);
            if (from < 64U && to < 64U) {
                modules::ThinkCandidate published; published.fromSquare = from; published.toSquare = to;
                published.completedDepth = completedDepth.load(); cfg.candidateObserver.Publish(published);
                (void)litch_max::TryFormatUciMove(from, to, '\0', move);
            }
            progress = 50U;
            if (cancelled) return modules::ThinkState::Canceled;
            if (done) return modules::ThinkState::Done;
            if (!blockPoll) return modules::ThinkState::Thinking;
            std::this_thread::yield();
        } while (true);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        if (realSearch) return rules.GetLastThinkStats(stats);
        stats.nodeCount = 100U; return true;
    }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        return realSearch && rules.GetLastClassicThinkResult(result);
    }
    void Choose(uint8_t from, uint8_t to) { candidate = static_cast<uint16_t>((from << 8U) | to); }
    modules::LitchMaxEngine rules;
    std::atomic<bool> finish{false}, finishCpu{false}, cancelled{false};
    std::atomic<uint16_t> candidate{0x4040U};
    std::atomic<bool> polling{false};
    std::atomic<uint32_t> appliedDuringPoll{0U};
    std::atomic<uint8_t> completedDepth{5U};
    uint32_t starts = 0U, cancels = 0U, legalReads = 0U;
    uint32_t legalPreparationMs = 0U, startedMaxThinkMs = 0U;
    bool failLegal = false, blockPoll = false;
    bool realSearch = false;
    std::atomic<bool> realSearchPaused{false}, releaseRealSearch{false};
private:
    modules::ThinkConfig cfg;
};
bool Ready(runtime::VirtualLitchSession& s, uint64_t occupancy = OPENING) {
    return s.Reset() == VIRTUAL_LITCH_OK && Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
           s.PhysicalFrame().led_count == PHYSICAL_TOPOLOGY.physicalBoardLedCount + PHYSICAL_TOPOLOGY.physicalStatusLedCount;
}
void PublicationAndShowMoveJourney() {
    const char* run = "HintFeedback/real-controls-controlled-publication";
    PublicationEngine engine, hiddenEngine;
    runtime::VirtualLitchSession session(Config(), engine), hidden(Config(), hiddenEngine);
    Check(Ready(session) && Ready(hidden) &&
              Setting(session, "HINT_THINKING_STYLE", "\"foxie_sweep\"") &&
              Setting(hidden, "HINT_THINKING_STYLE", "\"foxie_sweep\"") &&
              Tap(session) && Align(hidden, session.State().now_ms),
          run, "raw O requests Hint with the explicitly selected Litch Sweep style");
    Check(session.HintDisplayRequested() && !CornersWhite(session), run,
          "unavailable requested Hint starts its fade without the retired white corners");
    Checkpoint(session, run, "pending-fade-begin", "wait-or-select-any-legal-move");
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              PendingBankVisible(session), run,
          "after200ms the requested Hint delivers Foxie thinking on the V3 middle-four bank");
    Check(!PendingBankVisible(hidden), run,
          "background-only analysis cannot substitute ordinary output for requested thinking-bank delivery");
    Checkpoint(session, run, "pending-request", "wait-or-select-any-legal-move");
    const auto pending = session.PhysicalFrame();
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) && PendingBankVisible(session),
          run, "pending Foxie bank remains visible at the next sweep position");
    bool bankChanged = false;
    for (uint8_t local = 2U; local < 6U; ++local) {
        const uint8_t index = modules::PlayerHomeLedIndex(Owner(session), local);
        bankChanged = bankChanged || !Equal(pending.pixels[EdgeBase(session) + index],
            session.PhysicalFrame().pixels[EdgeBase(session) + index]);
    }
    Check(bankChanged, run, "pending thinking moves between actual middle-bank emitters");
    Checkpoint(session, run, "pending-next-position", "wait-or-select-any-legal-move");
    Check(session.Advance(800U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) && PendingBankVisible(session),
          run, "unavailable Hint retains thinking-bank feedback beyond the 720 ms exhausted-result fade");
    Checkpoint(session, run, "pending-beyond720ms", "wait-or-select-any-legal-move");
    engine.Choose(52U, 36U); hiddenEngine.Choose(52U, 36U);
    Check(session.Advance(400U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms), run, "completed mailbox publication crosses initial reveal boundary");
    uint8_t from = 64U, to = 64U;
    Check(session.TryGetHintMoveSquares(from, to) && from == 52U && to == 36U && CornersWhite(session),
          run, "first legal completed tuple reveals with its result feedback");
    modules::ControlMutationOutcome hintOutcome;
    Check(session.TryGetLastBoardControlOutcome(hintOutcome) &&
              hintOutcome.action == modules::ControlAction::Hint &&
              hintOutcome.status == modules::ControlMutationStatus::Applied,
          run, "raw O acknowledgement owns the revealed Hint");
    const uint32_t offAt = (session.State().now_ms / 800U + 1U) * 800U + 650U;
    Check(Align(session, offAt) && Align(hidden, offAt) &&
              EmptyPieceTargets(session, 52U) &&
              Energy(Piece(session, 51U)) > 0U && Energy(Square(session, 36U)) > 0U,
          run, "Hint uses the CPU OFF phase on only its still-present source in semantic and mapped output");
    Checkpoint(session, run, "cpu-cadence-source-off", "lift-e2-or-select-any-legal-piece");
    Check(session.Advance(200U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              Equal(Piece(session, 52U), Piece(hidden, 52U)) && Energy(Piece(session, 52U)) > 0U,
          run, "CPU ON phase restores the hinted source's ordinary piece color");
    engine.completedDepth = 6U; hiddenEngine.completedDepth = 6U;
    Check(session.Advance(1300U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) && SameCorners(session, hidden),
          run, "deeper completed publication of the same tuple does not restart result feedback");
    Checkpoint(session, run, "same-tuple-settled", "consider-e2e4");
    engine.Choose(52U, 44U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
          session.TryGetHintMoveSquares(from, to) && to == 44U && CornersWhite(session) && !SameCorners(session, hidden),
          run, "changed legal tuple alone starts a new feedback episode");
    Checkpoint(session, run, "changed-tuple", "consider-e2e3");
    Check(session.Advance(60U) == VIRTUAL_LITCH_OK, run, "advance the outgoing refinement before a burst");
    engine.Choose(50U, 34U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK && session.TryGetHintMoveSquares(from, to) &&
              from == 50U && to == 34U, run, "a completed changed-source root coalesces before fade-in");
    engine.Choose(52U, 44U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK && session.TryGetHintMoveSquares(from, to) &&
              from == 52U && to == 44U, run, "latest completed root remains authoritative through the burst");
    Check(session.Advance(180U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              SameCorners(session, hidden),
          run, "coalesced changed roots do not restart the ordinary-corner gap between two pulses");
    Checkpoint(session, run, "changed-tuple-gap", "consider-current-e2e3");
    Check(session.Advance(140U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              CornersWhite(session) && !SameCorners(session, hidden),
          run, "changed Hint attracts attention again with its second white corner pulse");
    Checkpoint(session, run, "changed-tuple-second-pulse", "consider-current-e2e3");
    Check(session.Advance(300U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              SameCorners(session, hidden),
          run, "result acknowledgement expires fully after720ms without changing the legal Hint");
    engine.Choose(50U, 34U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK && CornersWhite(session) &&
              Observe(session, OPENING & ~Bit(52U)) &&
              session.TryGetHintMoveSquares(from, to) && from == 52U && to == 44U &&
              Energy(Piece(session, 44U)) > 0U && Observe(session, OPENING) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              SameCorners(session, hidden),
          run, "pickup during outgoing fade freezes the actually visible e2e3 tuple; return cancels attention without replay");
    Checkpoint(session, run, "changed-source-pickup-cancelled", "choose-current-Hint-or-any-legal-move");
    engine.Choose(52U, 44U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK &&
              Setting(session, "MINIMAL_ANIMATIONS", "true") &&
              Observe(session, OPENING & ~Bit(52U)) &&
              session.TryGetHintMoveSquares(from, to) && from == 52U && to == 44U &&
              Observe(session, OPENING) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK &&
              Setting(session, "MINIMAL_ANIMATIONS", "false") &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms) &&
              SameCorners(session, hidden),
          run, "enabling Minimal before same-scan pickup keeps the already static current tuple; disabling it cannot replay hidden pulses");
    Checkpoint(session, run, "minimal-no-corner-replay", "consider-current-e2e3");
    Check(Coach(session), run, "raw O hold enables Coaching without changing search owner");
    const auto coachHome = session.HomeStateSnapshot();
    Check(coachHome.assistance.coachingForced &&
          coachHome.assistance.manualAffordanceUseful, run,
          "eligible Coach turn publishes Show Move availability in actual Home snapshot");
    modules::home_state_wire::Payload coachWire{};
    Check(modules::home_state_wire::Encode(coachHome, coachWire) && coachWire[91U] == 7U,
          run, "actual Coach Home encodes requested, available and Coaching bits together");
    Check(coachHome.assistance.presentation == modules::HomeAssistancePresentation::HintRefreshing &&
          std::strstr(session.HomeStateLine().CStr(), "\"as\":\"7r") != nullptr,
          run, "actual JSON Home retains eligible Coach flags while the same analysis refines");
    const uint32_t starts = engine.starts, cancels = engine.cancels;
    virtual_litch_action_request request{}; request.struct_size = sizeof(request); request.abi_version = VIRTUAL_LITCH_ABI_VERSION;
    request.request_id = 93U; request.action = VIRTUAL_LITCH_CONTROL_HINT;
    modules::ControlMutationOutcome result;
    Check(session.DispatchControlAction(request, result) == VIRTUAL_LITCH_OK &&
          result.status == modules::ControlMutationStatus::Applied && session.HintDisplayRequested() && CornersWhite(session) &&
          engine.starts == starts && engine.cancels == cancels,
          run, "typed Show Move replays current guidance while retaining Coach and the same analysis");
    Check(Tap(session) && session.HintDisplayRequested() && engine.starts == starts && engine.cancels == cancels,
          run, "short raw O under Coach requests Show Move rather than hiding it");
    Checkpoint(session, run, "show-move", "lift-e2-or-select-another-legal-piece");
    Check(Align(hidden, session.State().now_ms) && Coach(hidden) && Align(session, hidden.State().now_ms),
          run, "ordinary comparison uses the same Coaching badge before held-piece corner cancellation checks");
    Check(Observe(session, OPENING & ~Bit(52U)), run, "raw hinted-piece pickup enters ordinary legal guidance");
    Check(Observe(hidden, OPENING & ~Bit(52U)) &&
              session.Advance(1U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms),
          run, "matching raw pickup renders both hinted and ordinary legal guidance");
    Check(!session.HomeStateSnapshot().assistance.manualAffordanceUseful, run,
          "held piece keeps optional Show Move unavailable despite Coach latch");
    const uint64_t legal = session.State().valid_destination_mask;
    uint16_t low = 65535U, high = 0U;
    bool firstBrighter = false, secondBrighter = false;
    for (uint8_t i = 0U; i <= 24U; ++i) {
        const auto target = AllSquarePixels(session, 44U);
        const uint16_t top = Energy(target.mapped[1U]), bottom = Energy(target.mapped[2U]);
        low = std::min(low, std::min(top, bottom)); high = std::max(high, std::max(top, bottom));
        firstBrighter = firstBrighter || top > bottom;
        secondBrighter = secondBrighter || bottom > top;
        Check(session.State().valid_destination_mask == legal && (legal & Bit(44U)) != 0ULL &&
                  session.State().selected_square == 52U && Energy(target.semantic[1U]) > 0U &&
                  top > 0U && bottom > 0U,
              run, "matching held Hint alternates actual target piece emitters above a visible floor while retaining all legal choices");
        if (i == 2U) {
            Check(Energy(Square(hidden, 44U)) > 0U,
                  run, "ordinary e3 guidance is visible after its distance band completes");
        }
        Check(Equal(Square(session, 43U), Square(hidden, 43U)) && EmptyPieceTargets(session, 43U),
              run, "unrecommended d3 stays ordinary without a false landing marker");
        if (i >= 3U) {
            Check(Energy(session.SemanticFrame().board[36U][0U]) > 0U &&
                      Equal(session.SemanticFrame().board[36U][0U], session.SemanticFrame().board[36U][2U]),
                  run, "other legal e4 retains its ordinary guidance after its reveal");
        }
        if (i != 24U) Check(session.Advance(50U) == VIRTUAL_LITCH_OK && Align(hidden, session.State().now_ms),
                            run, "advance held guidance through a complete gentle alternation");
    }
    Check(low > 0U && high > low && firstBrighter && secondBrighter, run,
          "both real destination piece LEDs take the brighter role without either going dark");
    std::printf("[CHESS_ASSISTANCE_HELD_LOCATOR] min=%u max=%u legal=%016llx next=place-e3-or-another-legal-target\n",
                low, high, static_cast<unsigned long long>(legal));
    Checkpoint(session, run, "held-target", "place-e3-or-return-pawn", "accepted", Bit(44U));
    Check(Observe(session, OPENING) && Observe(hidden, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
              Align(hidden, session.State().now_ms) && SameCorners(session, hidden) && session.HintDisplayRequested(),
          run, "returning matching source to Idle restores guidance without replaying attention corners");
    Check(Observe(session, OPENING & ~Bit(62U)) && !session.HintDisplayRequested(),
          run, "another held piece suppresses unrelated Hint while retaining ordinary legal targets");
    Check(Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK && session.HintDisplayRequested(),
          run, "return to Idle recovers Coach guidance without a new search");
}

// Logical V3.3 positions, independently resolved through the physical rotation.
virtual_litch_color Emitter(const runtime::VirtualLitchSession& s, uint8_t square, uint8_t position) {
    const uint8_t mappedPosition = Rotated(s) ? (position + 2U) % 4U : position;
    return s.PhysicalFrame().pixels[runtime::MapSquareIndex(square, Rotated(s)) * 4U + mappedPosition];
}
struct TravelCase {
    const char* name;
    const char* fen;
    uint64_t occupancy;
    uint8_t source, target;
    std::array<uint8_t, 4U> order;
    uint8_t slots;
};
void DirectionalTravelJourney(const TravelCase& route, bool firstP2, bool rotate) {
    char run[120];
    std::snprintf(run, sizeof(run), "DirectionalTravel/V3.3/raw-O/%s/first-p%u/rotate-%u",
                  route.name, firstP2 ? 2U : 1U, rotate);
    auto config = Config(rotate); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.first_player_is_p2 = firstP2;
    PublicationEngine engine; runtime::VirtualLitchSession s(config, engine);
    const bool ready = s.Reset() == VIRTUAL_LITCH_OK && s.LoadExternalFen(route.fen) &&
        Observe(s, route.occupancy) && s.Advance(1000U) == VIRTUAL_LITCH_OK &&
        Setting(s, "P2_END_PIECE_WHEEL", "85") && Setting(s, "P1_END_PIECE_WHEEL", "0") &&
        s.Advance(20U) == VIRTUAL_LITCH_OK;
    Check(ready, run, "legal minimized geometry enters production Play with distinct actual player palettes");
    if (!ready) return;
    std::array<SquarePixels, 64U> ordinary{};
    for (uint8_t square = 0U; square < 64U; ++square) ordinary[square] = AllSquarePixels(s, square);
    Check(Tap(s), run, "raw O enters acknowledged Hint through physical debounce");
    engine.Choose(route.source, route.target);
    uint8_t from = 64U, to = 64U;
    for (uint8_t tick = 0U; tick < 35U && !s.TryGetHintMoveSquares(from, to); ++tick)
        Check(s.Advance(20U) == VIRTUAL_LITCH_OK, run, "wait for existing accepted initial reveal");
    Check(from == route.source && to == route.target, run, "real McuMax legality admits the published tuple");
    if (from != route.source || to != route.target) return;
    const int dr = int(to / 8U) - from / 8U, df = int(to % 8U) - from % 8U;
    const uint8_t distance = static_cast<uint8_t>(std::max(std::abs(dr), std::abs(df)));
    const int rowStep = (dr > 0) - (dr < 0), fileStep = (df > 0) - (df < 0);
    const auto squareAt = [&](uint8_t step) { return static_cast<uint8_t>(
        (from / 8U + rowStep * step) * 8 + from % 8U + fileStep * step); };
    uint64_t routeMask = 0ULL;
    for (uint8_t step = 0U; step <= distance; ++step) routeMask |= Bit(squareAt(step));
    const uint32_t duration = std::max(900U, 480U * distance), pass = duration + 240U;
    const bool diagonal = dr != 0 && df != 0;
    const uint8_t phases = route.slots;
    const uint8_t nodes = static_cast<uint8_t>((distance + 1U) * phases);
    const auto maskAt = [&](uint8_t local) {
        if (diagonal) return static_cast<uint8_t>((1U << route.order[local * 2U]) | (1U << route.order[local * 2U + 1U]));
        return local == 1U ? static_cast<uint8_t>((1U << route.order[1U]) | (1U << route.order[2U]))
            : static_cast<uint8_t>(1U << route.order[local == 0U ? 0U : 3U]);
    };
    const auto moverComponent = [firstP2](virtual_litch_color color) { return firstP2 ? color.g : color.r; };
    bool sourceOn = false, sourceOff = false;
    for (const bool held : {false, true}) {
        if (held) Check(Observe(s, route.occupancy & ~Bit(from)) && s.State().selected_square == from,
                        run, "raw source pickup preserves directed Hint and ordinary legal choices");
        const uint64_t legal = s.State().valid_destination_mask;
        std::array<SquarePixels, 64U> baseline{};
        for (uint8_t sq = 0U; sq < 64U; ++sq) baseline[sq] = held ? AllSquarePixels(s, sq) : ordinary[sq];
        // The current owner starts its clock on reveal / pickup. Two complete
        // passes prove ordered handoff, destination dwell and one-way restart.
        const uint32_t start = s.State().now_ms;
        bool ordinaryRevealCompleted = false;
        for (uint8_t cycle = 0U; cycle < 2U; ++cycle) {
            for (uint8_t node = 0U; node < nodes; ++node) {
                const uint32_t age = cycle * pass + (node * duration + nodes - 2U) / (nodes - 1U);
                if (node > 0U) {
                    const uint32_t midpoint = cycle * pass + ((2U * node - 1U) * duration) / (2U * (nodes - 1U));
                    Check(Align(s, start + midpoint), run, "sample the continuous midpoint before the next emitter peak");
                    const uint8_t priorSquare = squareAt((node - 1U) / phases);
                    const uint8_t nextSquare = squareAt(node / phases);
                    if (priorSquare != from && nextSquare != from && priorSquare != to && nextSquare != to) {
                        bool priorLit = false, nextLit = false;
                        for (uint8_t pos = 0U; pos < 4U; ++pos) {
                            priorLit = priorLit || ((maskAt((node - 1U) % phases) & (1U << pos)) && Energy(Emitter(s, priorSquare, pos)) > 20U);
                            nextLit = nextLit || ((maskAt(node % phases) & (1U << pos)) && Energy(Emitter(s, nextSquare, pos)) > 20U);
                        }
                        Check(priorLit && nextLit, run, "adjacent directional phases crossfade instead of jumping between peaks");
                    }
                }
                Check(Align(s, start + age), run, "advance existing owner clock to a directional phase peak");
                const uint8_t square = squareAt(node / phases);
                const bool occupied = (route.occupancy & Bit(square)) != 0ULL && !(held && square == from);
                const uint8_t mask = maskAt(node % phases);
                uint8_t slot = 0U;
                while (slot < 4U && (mask & (1U << slot)) == 0U) ++slot;
                if (occupied) {
                    Check(Energy(Emitter(s, square, 0U)) > 0U || Energy(Emitter(s, square, 2U)) > 0U, run,
                          "occupied route head uses side rails without disturbing piece identity");
                } else {
                    for (uint8_t pos = 0U; pos < 4U; ++pos) if ((mask & (1U << pos)) != 0U) {
                        const auto color = Emitter(s, square, pos);
                        Check(Energy(color) >= 115U, run, "each expected mapped emitter receives the moving head");
                        for (uint8_t other = 0U; other < 4U; ++other) if ((mask & (1U << other)) == 0U) {
                            const auto leading = moverComponent(color);
                            const auto trailing = moverComponent(Emitter(s, square, other));
                            const bool middleTransit = !diagonal && node % phases == 1U &&
                                square != from && square != to;
                            Check(middleTransit ? int(leading) > int(trailing) + 20 : leading >= trailing, run,
                                  "the mapped middle pair visibly leads entry and exit on empty cardinal transit");
                        }
                    }
                    if ((mask & (mask - 1U)) != 0U) {
                        uint8_t partner = slot + 1U;
                        while (partner < 4U && (mask & (1U << partner)) == 0U) ++partner;
                        Check(std::abs(int(moverComponent(Emitter(s, square, slot))) -
                                       int(moverComponent(Emitter(s, square, partner)))) <= 3, run,
                              "the middle cardinal pair or diagonal entry/exit pair peaks together");
                    }
                }
                const auto color = Emitter(s, square, slot);
                Check(Energy(Emitter(s, to, 1U)) > 0U && Energy(Emitter(s, to, 3U)) > 0U, run,
                      "both actual target Piece emitters retain a visible landing floor throughout travel");
                for (uint8_t sq = 0U; sq < 64U; ++sq) {
                    if ((routeMask & Bit(sq)) != 0ULL) continue;
                    const auto actual = AllSquarePixels(s, sq);
                    // Off-route legal rails can take part in the pickup wave.
                    // The Hint head must leave their Piece emitters alone.
                    if (held && !ordinaryRevealCompleted && (legal & Bit(sq)) != 0ULL) {
                        Check(Equal(actual.semantic[1U], baseline[sq].semantic[1U]) &&
                                  Equal(actual.mapped[1U], baseline[sq].mapped[1U]) &&
                                  Equal(actual.mapped[2U], baseline[sq].mapped[2U]),
                              run, "off-route legal reveal leaves both mapped Piece emitters unchanged");
                        if (age >= 400U) {
                            Check(HasSteadyWhiteLegalRails(actual, firstP2), run,
                                  "off-route legal rails finish at the configured color and both mapped rails stay visible");
                            baseline[sq] = actual;
                        }
                    } else {
                        Check(SameSquarePixels(actual, baseline[sq]), run,
                              "nonroute cells outside the pickup wave stay byte exact ordinary");
                    }
                }
                ordinaryRevealCompleted = ordinaryRevealCompleted || age >= 400U;
                for (uint8_t step = 1U; step < distance; ++step) {
                    const uint8_t transit = squareAt(step);
                    if (held) {
                        Check(s.State().valid_destination_mask == legal && (legal & Bit(transit)) != 0ULL,
                              run, "the original legal set includes this transit square throughout the wave and Hint");
                        if (transit == square || age >= (step + 1U) * 50U) {
                            Check(Energy(Emitter(s, transit, 0U)) > 0U && Energy(Emitter(s, transit, 2U)) > 0U,
                                  run, "both legal rails stay readable after their reveal and while the Hint head crosses them");
                        }
                    }
                    if (transit != square) {
                        const bool nextHandoff = node % phases + 1U == phases &&
                            node + 1U < nodes && transit == squareAt(node / phases + 1U);
                        Check(nextHandoff
                            ? Energy(Emitter(s, transit, 1U)) <= 2U && Energy(Emitter(s, transit, 3U)) <= 2U
                            : EmptyPieceTargets(s, transit), run,
                            "only a one-byte rounded next-node handoff may outlive the current cell; all other empty Piece emitters are black");
                    }
                }
                if (!held) {
                    const auto top = Energy(Emitter(s, from, 1U)), bottom = Energy(Emitter(s, from, 3U));
                    const bool blinkOn = s.State().now_ms % 800U < 600U;
                    Check(blinkOn ? top > 0U && bottom > 0U : top == 0U && bottom == 0U, run,
                        "both mapped source Piece emitters preserve the exact existing 600/200 blink even when the head crosses them");
                    sourceOn = sourceOn || (top > 0U && bottom > 0U);
                    sourceOff = sourceOff || (top == 0U && bottom == 0U);
                }
                if ((cycle == 0U && ((!firstP2 && !rotate) || node == 0U ||
                     node == phases || node + 1U == nodes)) || (cycle == 1U && node == 0U)) {
                    std::printf("[DIRECTIONAL_EMITTER] run=%s held=%u age=%u node=%u square=%u slot=%u rgb=%u,%u,%u,%u\n",
                        run, held, age, node, square, slot, color.r, color.g, color.b, color.w);
                    if (node % phases == 0U || node + 1U == nodes)
                        Checkpoint(s, run, cycle == 1U ? (held ? "held-source-repeat" : "idle-source-repeat")
                            : held ? "held-forward-head" : "idle-forward-head",
                            "follow-Hint-or-choose-any-legal-move", "accepted", routeMask);
                }
            }
        }
    }
    Check(sourceOn && sourceOff, run, "still-present source retains both ON and fully OFF parts of its 600/200 blink");
    Check(Observe(s, route.occupancy) && s.Advance(250U) == VIRTUAL_LITCH_OK, run,
        "return source to Idle without a move or stale held route");
    if (from == 17U && to == 53U && !firstP2 && !rotate) {
        Check(Observe(s, route.occupancy & ~Bit(45U)) && EmptyPieceTargets(s, to), run,
            "picking up the other f3 pawn suppresses this optional bishop route and landing");
        for (uint8_t step = 1U; step < distance; ++step) Check(EmptyPieceTargets(s, squareAt(step)), run,
            "other-piece handling retires all transient bishop Piece travel");
        Checkpoint(s, run, "other-piece-guidance", "return-f3-or-play-its-legal-move", "accepted", routeMask|Bit(45U));
        Check(Observe(s, route.occupancy) && s.Advance(250U) == VIRTUAL_LITCH_OK && !EmptyPieceTargets(s, to), run,
            "returning the other pawn restores the same legal bishop Hint without another search");
    }
    Check(Setting(s, "MINIMAL_ANIMATIONS", "true") && s.Advance(20U) == VIRTUAL_LITCH_OK,
        run, "enter existing Minimal policy without a move");
    const auto still = AllSquarePixels(s, to);
    Check(Energy(Emitter(s, to, 1U)) > 0U && Equal(Emitter(s, to, 1U), Emitter(s, to, 3U)) &&
        s.Advance(pass) == VIRTUAL_LITCH_OK && SameSquarePixels(still, AllSquarePixels(s, to)), run,
        "Minimal keeps one static clear endpoint and no travel");
    for (uint8_t step = 1U; step < distance; ++step) Check(SameSquarePixels(
        AllSquarePixels(s, squareAt(step)), ordinary[squareAt(step)]), run, "Minimal clears every transient route emitter");
    if (!firstP2 && !rotate) Checkpoint(s, run, "Minimal-static-endpoint", "follow-Hint-or-hide-it", "accepted", routeMask);
    uint8_t replayFrom=64U,replayTo=64U;
    const uint32_t starts = engine.starts, cancels = engine.cancels;
    Check(Tap(s)&&!s.HintDisplayRequested()&&s.TryGetHintMoveSquares(replayFrom,replayTo)&&
          replayFrom==from&&replayTo==to&&EmptyPieceTargets(s,to)&&
          engine.starts==starts&&engine.cancels==cancels,run,
          "Coaching-off retap hides the route while retaining its exact cached tuple and worker");
    Check(Tap(s)&&s.HintDisplayRequested()&&s.TryGetHintMoveSquares(replayFrom,replayTo)&&
          replayFrom==from&&replayTo==to&&engine.starts==starts&&engine.cancels==cancels,run,
          "the next raw O reveals the same route without another search");
    Check(s.LoadExternalFen(route.fen)&&!s.HintDisplayRequested()&&s.Advance(250U)==VIRTUAL_LITCH_OK,run,
          "the existing external-position import invalidates that route through the real mutation owner");
    for (uint8_t sq = 0U; sq < 64U; ++sq) Check(SameSquarePixels(AllSquarePixels(s, sq), ordinary[sq]), run,
        "position invalidation restores all ordinary emitters without phantom occupancy or neighbor cues");
    if (!firstP2 && !rotate) Checkpoint(s, run, "position-invalidated-ordinary-restored", "choose-any-legal-move", "accepted", routeMask);
}
void BishopSingleDestinationJourney() {
    const std::array<TravelCase, 8U> cases{{
        {"b6-f2-SE", "7k/8/1B6/8/8/5P2/6K1/8 w - - 0 1", Bit(7)|Bit(17)|Bit(45)|Bit(54), 17U,53U,{1U,0U,3U,2U},2U},
        {"f2-b6-NW", "7k/8/8/8/8/5P2/5BK1/8 w - - 0 1", Bit(7)|Bit(45)|Bit(53)|Bit(54),53U,17U,{3U,2U,1U,0U},2U},
        {"b2-f6-NE", "k7/8/8/8/8/5P2/1B6/7K w - - 0 1", Bit(0)|Bit(45)|Bit(49)|Bit(63),49U,21U,{3U,0U,1U,2U},2U},
        {"f6-b2-SW", "k7/8/5B2/8/8/5P2/8/7K w - - 0 1", Bit(0)|Bit(21)|Bit(45)|Bit(63),21U,49U,{1U,2U,3U,0U},2U},
        {"a4-d4-right", "7k/8/8/8/R7/8/8/7K w - - 0 1", Bit(7)|Bit(32)|Bit(63),32U,35U,{0U,1U,3U,2U},3U},
        {"d4-a4-left", "7k/8/8/8/3R4/8/8/7K w - - 0 1", Bit(7)|Bit(35)|Bit(63),35U,32U,{2U,1U,3U,0U},3U},
        {"a2-a5-up", "7k/8/8/8/8/8/R7/7K w - - 0 1", Bit(7)|Bit(48)|Bit(63),48U,24U,{3U,0U,2U,1U},3U},
        {"a5-a2-down", "7k/8/8/R7/8/8/8/7K w - - 0 1", Bit(7)|Bit(24)|Bit(63),24U,48U,{1U,0U,2U,3U},3U}
    }};
    for (const auto& route : cases) for (bool first : {false,true}) for (bool rotate : {false,true})
        DirectionalTravelJourney(route, first, rotate);
}
void AdjacentMoveTravelJourney(bool firstP2, bool rotated) {
    DirectionalTravelJourney({"e2-e3-up", "7k/8/8/8/8/8/4P3/K7 w - - 0 1",
        Bit(7)|Bit(52)|Bit(56),52U,44U,{3U,0U,2U,1U},3U}, firstP2, rotated);
    DirectionalTravelJourney({"e3-e2-down", "7k/8/8/8/8/4R3/8/K7 w - - 0 1",
        Bit(7)|Bit(44)|Bit(56),44U,52U,{1U,0U,2U,3U},3U}, firstP2, rotated);
}

void EnPassantRemovalBeforeLandingJourney() {
    const char* run = "DirectedLanding/V3.3/raw-O-EP-remove-before-landing";
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    PublicationEngine engine; runtime::VirtualLitchSession s(config, engine);
    const uint64_t occupancy = Bit(4U)|Bit(26U)|Bit(27U)|Bit(28U)|Bit(60U);
    Check(s.Reset() == VIRTUAL_LITCH_OK && s.LoadExternalFen("4k3/8/8/2PpP3/8/8/8/4K3 w - d6 0 1") &&
              Observe(s, occupancy) && s.Advance(1000U) == VIRTUAL_LITCH_OK && Tap(s), run,
          "legal en-passant FEN, Hall and raw O enter production Hint");
    engine.Choose(26U, 19U); uint8_t from = 64U, to = 64U;
    Check(s.Advance(600U) == VIRTUAL_LITCH_OK && s.TryGetHintMoveSquares(from, to) && from == 26U && to == 19U,
          run, "real rules publish c5-d6 with off-destination victim d5");
    if (from != 26U || to != 19U) return;
    for (const bool held : {false, true}) {
        if (held) Check(Observe(s, occupancy & ~Bit(26U)), run, "raw source-first c5 pickup keeps victim-removal action");
        const auto initialLeft = Emitter(s, 19U, 0U), initialRight = Emitter(s, 19U, 2U);
        bool targetTravel = false;
        for (uint8_t sample = 0U; sample < 58U; ++sample) {
            Check(EmptyPieceTargets(s, 19U) && EmptyPieceTargets(s, 27U), run,
                  "both actual d6 target emitters and d5 captured-victim emitters stay dark while removal is pending");
            targetTravel = targetTravel || !Equal(initialLeft, Emitter(s, 19U, 0U)) ||
                !Equal(initialRight, Emitter(s, 19U, 2U));
            Check(s.Advance(20U) == VIRTUAL_LITCH_OK, run, "sample one directed pass while EP removal retains priority");
        }
        Check(targetTravel, run, "pending EP capture still reaches its actual destination with directional Square LEDs alone");
        Checkpoint(s, run, held ? "source-held-victim-pending" : "victim-pending", "remove-d5-victim", "accepted", Bit(26U)|Bit(27U)|Bit(19U));
    }
    const uint64_t lifted = occupancy & ~Bit(26U) & ~Bit(27U);
    Check(Observe(s, lifted), run, "raw victim removal acknowledges d5 before landing starts");
    bool top = false, bottom = false;
    for (uint8_t sample = 0U; sample <= 24U; ++sample) {
        const auto target = AllSquarePixels(s, 19U);
        Check(Energy(target.mapped[1U]) > 0U && Energy(target.mapped[2U]) > 0U && EmptyPieceTargets(s, 27U), run,
              "cleared EP victim stays black while both actual landing emitters now have a visible floor");
        top = top || Energy(target.mapped[1U]) > Energy(target.mapped[2U]);
        bottom = bottom || Energy(target.mapped[2U]) > Energy(target.mapped[1U]);
        if (sample % 6U == 0U) Checkpoint(s, run, "victim-removed-landing-active", "place-c5-pawn-d6", "accepted", Bit(27U)|Bit(19U));
        if (sample != 24U) Check(s.Advance(50U) == VIRTUAL_LITCH_OK, run, "sample EP landing after removal");
    }
    Check(top && bottom && Observe(s, lifted | Bit(19U)) && s.Advance(3000U) == VIRTUAL_LITCH_OK &&
              s.UndoHistoryMoveCount() == 1U && s.State().correction_mask == 0ULL && EmptyPieceTargets(s, 27U), run,
          "EP landing alternates then commits once with off-destination victim still dark");
    Checkpoint(s, run, "EP-acknowledged", "Black-to-play", "accepted", Bit(27U)|Bit(19U));
}

struct RemovalFadeCase { bool rotate,firstP2;uint16_t brightness,currentLimit; };
void CaptureEdgeBlockedBracketJourney(const RemovalFadeCase profile) {
    const bool rotate=profile.rotate,firstP2=profile.firstP2;const uint16_t currentLimit=profile.currentLimit;
    char run[140];std::snprintf(run,sizeof(run),"CaptureWhiteFade/V3.3/raw-O/a5/rotate-%u/firstP2-%u/brightness-%u/cap-%u",rotate,firstP2,profile.brightness,currentLimit);
    auto config=Config(rotate);config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;
    config.first_player_is_p2=firstP2;config.brightness_scale_256=profile.brightness;config.max_led_current_ma=currentLimit;
    PublicationEngine engine;runtime::VirtualLitchSession s(config,engine);
    const uint64_t occupancy=Bit(7U)|Bit(24U)|Bit(25U)|Bit(48U)|Bit(62U);
    Check(s.Reset()==VIRTUAL_LITCH_OK && s.LoadExternalFen("7k/8/8/qB6/8/8/R7/6K1 w - - 0 1") &&
        Observe(s,occupancy) && s.Advance(1000U)==VIRTUAL_LITCH_OK,run,"legal edge capture enters real Play");
    const auto neighbor=AllSquarePixels(s,25U),victim=AllSquarePixels(s,24U);
    Check(Tap(s),run,"raw O requests legal a2xa5 Hint");engine.Choose(48U,24U);
    uint8_t from=64U,to=64U;
    Check(s.Advance(600U)==VIRTUAL_LITCH_OK && s.TryGetHintMoveSquares(from,to) && from==48U && to==24U,
        run,"real legality publishes the edge capture with occupied b5 neighbor");
    if(from!=48U || to!=24U)return;
    const uint32_t start=s.State().now_ms;
    bool strike=false,floor=false,nearHead=false,farHead=false,routeCleared=false;
    bool shortGap=false,longRest=false,seenStrike=false;
    uint8_t strikeFrames=0U,floorFrames=0U,dimRun=0U;
    uint8_t brightestMappedStrike=0U,brightestMappedRest=0U;
    uint32_t previousFrame=s.PhysicalFrame().frame_sequence;
    for(uint8_t sample=0U;sample<=120U;++sample){
        Check(Align(s,start+sample*20U),run,"sample two complete prompt-owned removal cycles");
        const auto target=AllSquarePixels(s,24U);const auto left=target.semantic[0U],right=target.semantic[2U];
        const auto close=[](int x,int y){return std::abs(x-y)<=2;};
        const bool isStrike=close(left.r,255) && close(left.g,101);
        const bool isFloor=close(left.r,63) && close(left.g,25);
        Check(EmptyPieceTargets(s,24U) && Equal(left,right) && left.b==0U && left.w==0U &&
            (isStrike||isFloor),run,
            "both victim rails show synchronized amber strikes or their dim amber rest while Piece LEDs stay dark");
        Check(Equal(target.mapped[0U],target.mapped[3U]) && Energy(target.mapped[0U])>0U &&
            target.mapped[0U].b==0U && target.mapped[0U].w==0U,run,
            "both actual V3.3 rails remain synchronized and visible at every pulse and floor checkpoint");
        strike=strike||isStrike; floor=floor||isFloor;
        strikeFrames+=isStrike;floorFrames+=isFloor;
        if(isStrike) {
            brightestMappedStrike=std::max(brightestMappedStrike,target.mapped[0U].r);
            if(seenStrike && dimRun>0U) {
                shortGap=shortGap || (dimRun>=5U && dimRun<=7U);
                longRest=longRest || (dimRun>=40U && dimRun<=44U);
            }
            seenStrike=true;dimRun=0U;
        } else if(isFloor) {
            brightestMappedRest=std::max(brightestMappedRest,target.mapped[0U].r);
            if(seenStrike) ++dimRun;
        }
        const auto& physical=s.PhysicalFrame();
        if(sample!=0U)Check(physical.now_ms==start+sample*20U && physical.frame_sequence!=previousFrame,run,
            "each sample is a fresh actual mapped frame");
        previousFrame=physical.frame_sequence;
        const auto near=AllSquarePixels(s,40U),far=AllSquarePixels(s,32U);
        nearHead=nearHead || Energy(near.mapped[1U])+Energy(near.mapped[2U])>0U;
        farHead=farHead || Energy(far.mapped[1U])+Energy(far.mapped[2U])>0U;
        routeCleared=routeCleared || (EmptyPieceTargets(s,40U) && EmptyPieceTargets(s,32U));
        if(currentLimit)Check(physical.power_limited && physical.applied_led_current_ma<=currentLimit,run,
            "actual warm pulses obey the hard current cap throughout the cycle");
        const auto currentNeighbor=AllSquarePixels(s,25U);bool ordinary=true;
        for(uint8_t slot=0U;slot<3U;++slot)ordinary=ordinary && Equal(currentNeighbor.semantic[slot],neighbor.semantic[slot]);
        Check(currentLimit?ordinary:SameSquarePixels(currentNeighbor,neighbor),run,
            "occupied neighbor keeps every rail and both Piece identities; only global current scaling may vary");
        if(sample%20U==0U)Checkpoint(s,run,"edge-warm-removal","remove-a5-victim-or-hide-Hint","accepted",Bit(24U)|Bit(25U)|Bit(48U));
    }
    Check(strike && floor && strikeFrames>=22U && floorFrames>=90U && shortGap && longRest,run,
        "paired 120ms amber strikes are separated by 120ms dim then an extended 840ms dim rest");
    Check(nearHead && farHead && routeCleared,run,
        "capture Hint crosses both empty route cells and clears behind the head while the victim remains warm");
    Check(Setting(s,"MINIMAL_ANIMATIONS","true") && s.Advance(20U)==VIRTUAL_LITCH_OK,run,"Minimal freezes both victim rails orange");
    const auto still=AllSquarePixels(s,24U);
    Check(still.semantic[0].r==255U && std::abs(int(still.semantic[0].g)-85)<=2 && still.semantic[0].b==0U &&
        Equal(still.semantic[0],still.semantic[2]) && EmptyPieceTargets(s,24U) &&
        EmptyPieceTargets(s,32U) && EmptyPieceTargets(s,40U) && s.Advance(650U)==VIRTUAL_LITCH_OK,run,
        "Minimal is static orange on both rails with both actual victim Piece LEDs zero");
    const auto later=AllSquarePixels(s,24U);
    if(!currentLimit && profile.brightness<256U) {
        Check(brightestMappedStrike>still.mapped[0U].r,run,
            "actual removal strikes use spare output headroom to exceed steady Minimal brightness");
        Check(std::abs(int(brightestMappedRest)*4-int(still.mapped[0U].r))<=6,run,
            "dim amber rest uses one-quarter ordinary output without the strike-only accent");
    }
    Check(currentLimit?Equal(still.semantic[0],later.semantic[0]) && Equal(still.semantic[2],later.semantic[2]):SameSquarePixels(still,later),run,
        "Minimal has no local pulse; hard current arbitration retains authority");
    Checkpoint(s,run,"Minimal-static-orange","remove-victim-or-hide-Hint","accepted",Bit(24U)|Bit(25U));
    uint8_t replayFrom=64U,replayTo=64U;
    Check(Tap(s)&&!s.HintDisplayRequested(),run,"manual O hides the optional capture Hint through its existing toggle owner");
    Check(Tap(s)&&s.HintDisplayRequested()&&s.TryGetHintMoveSquares(replayFrom,replayTo)&&
        replayFrom==from&&replayTo==to,run,"the next manual O reveals the retained capture tuple without another search");
    Check(s.LoadExternalFen("7k/8/8/qB6/8/8/R7/6K1 w - - 0 1")&&!s.HintDisplayRequested()&&
        s.Advance(250U)==VIRTUAL_LITCH_OK,run,"real external-position import retires the old capture presentation");
    const auto restored=AllSquarePixels(s,24U);bool identity=true;
    for(uint8_t slot=0U;slot<3U;++slot)identity=identity && Equal(restored.semantic[slot],victim.semantic[slot]);
    Check(currentLimit?identity:SameSquarePixels(restored,victim),run,"position invalidation restores the actual victim identity with no white removal residue");
}

void UndoLocalReturnJourney(bool rotate, bool minimal, bool litBoard = false) {
    const char* run=rotate ? "Undo/V3.3/rotated/Minimal" : litBoard ? "Undo/V3.3/lit-board" : "Undo/V3.3/normal";
    auto config=Config(rotate);config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;
    test_support::BoundedHintSession fixture(config);auto& s=fixture.session;
    uint64_t occupancy=Bit(7U)|Bit(52U)|Bit(62U);
    Check(s.Reset()==VIRTUAL_LITCH_OK && s.LoadExternalFen("7k/8/8/8/8/8/4P3/6K1 w - - 0 1") &&
              Observe(s,occupancy) && s.Advance(1000U)==VIRTUAL_LITCH_OK && Setting(s,"WHITE_SQUARES_ON",litBoard ? "true" : "false"),run,
          "legal minimal pawn position enters production play with selected square background");
    occupancy&=~Bit(52U);Check(Observe(s,occupancy),run,"raw e2 pickup");
    occupancy|=Bit(36U);Check(Observe(s,occupancy) && s.Advance(3000U)==VIRTUAL_LITCH_OK && s.UndoHistoryMoveCount()==1U,
          run,"raw e2-e4 commits once before Undo");
    Check(Setting(s,"MINIMAL_ANIMATIONS",minimal ? "true" : "false") && s.Advance(20U)==VIRTUAL_LITCH_OK,run,"select animation preference");
    const auto backgroundBefore = AllSquarePixels(s,62U);
    const auto squareBefore = AllSquarePixels(s,63U);
    const auto owner=Owner(s,true)==modules::UiOwner::P2?VIRTUAL_LITCH_OWNER_P2:VIRTUAL_LITCH_OWNER_P1;
    Check(s.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,true)==VIRTUAL_LITCH_OK && s.Advance(20U)==VIRTUAL_LITCH_OK &&
              s.SetButtonDown(owner,runtime::VirtualLitchPhysicalButton::X,false)==VIRTUAL_LITCH_OK && s.Advance(20U)==VIRTUAL_LITCH_OK &&
              s.HomeStateSnapshot().undoPending,run,"raw X acknowledges precise pawn-return Undo");
    Check(s.Advance(1U)==VIRTUAL_LITCH_OK &&
              s.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan),run,
          "the next production tick hands accepted Undo to its physical return plan");
    const auto backgroundDuring = AllSquarePixels(s,62U);
    const auto squareDuring = AllSquarePixels(s,63U);
    for (uint8_t slot : {1U, 2U}) {
        const auto before = Energy(backgroundBefore.mapped[slot]);
        const auto during = Energy(backgroundDuring.mapped[slot]);
        std::printf("[UNDO_DIM] run=%s mapped_piece=%u before=%u during=%u\n",run,slot,before,during);
        Check(before > 4U && during * 2U < before,run,"both physical LEDs of an uninvolved piece dim during the return");
    }
    if (litBoard) {
        for (uint8_t slot : {0U, 3U}) {
            const auto before = Energy(squareBefore.mapped[slot]);
            const auto during = Energy(squareDuring.mapped[slot]);
            std::printf("[UNDO_DIM] run=%s mapped_rail=%u before=%u during=%u\n",run,slot,before,during);
            Check(before > 4U && during * 2U < before,run,"both physical background rails dim during the return");
        }
    }
    const auto squareDark = [](const SquarePixels& cell) {
        for (const auto color : cell.semantic) if (Energy(color) != 0U) return false;
        for (const auto color : cell.mapped) if (Energy(color) != 0U) return false;
        return true;
    };
    bool routeVisible = false;
    bool routeCleared = false;
    bool sourceProminent = false;
    for(uint8_t sample=0U;sample<=16U;++sample){
        const auto source = AllSquarePixels(s,36U);
        sourceProminent = sourceProminent ||
            (Energy(source.mapped[1U]) > Energy(backgroundDuring.mapped[1U]) &&
             Energy(source.mapped[2U]) > Energy(backgroundDuring.mapped[2U]));
        const auto landing = AllSquarePixels(s, 52U);
        Check(Energy(Square(s,52U))>0U && Energy(landing.semantic[1U])>0U &&
                  Energy(landing.mapped[1U])>0U && Energy(landing.mapped[2U])>0U,run,
              "the exact e2 return target retains its semantic and both mapped landing Piece LEDs");
        for(const uint8_t neighbor:{51U,53U,60U}){
            if (!litBoard) Check(squareDark(AllSquarePixels(s,neighbor)),run,
                  "Undo leaves off-route surrounding squares dark without a stationary focus cross");
        }
        const auto route = AllSquarePixels(s, 44U);
        bool routeLit = true;
        for (const auto color : route.semantic) routeLit = routeLit && Energy(color)>0U;
        for (const auto color : route.mapped) routeLit = routeLit && Energy(color)>0U;
        routeVisible = routeVisible || routeLit;
        routeCleared = routeCleared || (routeVisible && squareDark(route));
        if(sample%4U==0U)Checkpoint(s,run,"directed-return","lift-e4-and-return-e2","accepted",Bit(36U)|Bit(52U)|Bit(44U)|Bit(51U)|Bit(53U));
        if(sample!=16U)Check(s.Advance(50U)==VIRTUAL_LITCH_OK,run,"sample one-way Undo travel through e3");
    }
    Check(sourceProminent,run,"both actual LEDs of the returning piece stand out from the dimmed board");
    if (!minimal && !litBoard) Check(routeVisible && routeCleared, run,
          "the e3 route lights all four mapped LEDs transiently, then clears between travel heads");
    std::printf("[UNDO_ROUTE] run=%s e3_visible=%u e3_cleared=%u landing=e2 next=lift-e4-and-return-e2\n",
                run, static_cast<unsigned>(routeVisible), static_cast<unsigned>(routeCleared));
    occupancy&=~Bit(36U);Check(Observe(s,occupancy),run,"raw Undo source pickup");
    Check(Observe(s,occupancy|Bit(37U)) && s.UndoHistoryMoveCount()==0U,run,"wrong f4 return cannot create another move");
    Checkpoint(s,run,"wrong-return","remove-f4-and-return-e2","rejected-recoverable",Bit(37U)|Bit(52U));
    Check(Observe(s,occupancy) && Observe(s,occupancy|Bit(52U)) && s.Advance(3000U)==VIRTUAL_LITCH_OK &&
              !s.HomeStateSnapshot().undoPending && s.State().correction_mask==0ULL && s.UndoHistoryMoveCount()==0U,run,
          "restoring the exact e2 return completes Undo with no stale correction or duplicate action");
    const auto backgroundAfter = AllSquarePixels(s,62U);
    for (uint8_t slot : {1U, 2U}) Check(Equal(backgroundBefore.mapped[slot],backgroundAfter.mapped[slot]),run,
        "both unrelated physical piece LEDs return to their original brightness after Undo");
    if (!litBoard) Check(squareDark(AllSquarePixels(s,44U)) && squareDark(AllSquarePixels(s,36U)),run,
          "restoration clears the empty e3 route and e4 source in semantic and mapped output");
    Checkpoint(s,run,"Undo-complete","Black-or-White-next-action","accepted",Bit(52U)|Bit(44U));
}

struct KnightRoute {
    const char* name;
    std::array<uint8_t, 4U> squares;
    std::array<uint8_t, 8U> positions;
};
const std::array<KnightRoute, 9U> KNIGHT_ROUTES{{
    {"up-two-left", {35U,27U,19U,18U}, {3U,1U,3U,1U,3U,0U,2U,0U}},
    {"up-two-right", {35U,27U,19U,20U}, {3U,1U,3U,1U,3U,2U,0U,2U}},
    {"down-two-left", {35U,43U,51U,50U}, {1U,3U,1U,3U,1U,0U,2U,0U}},
    {"down-two-right", {35U,43U,51U,52U}, {1U,3U,1U,3U,1U,2U,0U,2U}},
    {"left-two-up", {35U,34U,33U,25U}, {2U,0U,2U,0U,2U,1U,3U,1U}},
    {"left-two-down", {35U,34U,33U,41U}, {2U,0U,2U,0U,2U,3U,1U,3U}},
    {"right-two-up", {35U,36U,37U,29U}, {0U,2U,0U,2U,0U,1U,3U,1U}},
    {"right-two-down", {35U,36U,37U,45U}, {0U,2U,0U,2U,0U,3U,1U,3U}},
    {"reverse-c6-d4", {18U,26U,34U,35U}, {1U,3U,1U,3U,1U,2U,0U,2U}}
}};
std::string KnightFen(const KnightRoute& route, bool cpu, bool blocked, bool capture) {
    std::array<char, 64U> pieces{}; pieces[0U]='k'; pieces[63U]='K'; pieces[15U]='p'; pieces[48U]='P';
    pieces[route.squares[0U]]=cpu?'n':'N';
    if(blocked) { pieces[route.squares[1U]]=cpu?'p':'P'; pieces[route.squares[2U]]=cpu?'P':'p'; }
    if(capture) pieces[route.squares[3U]]=cpu?'P':'p';
    std::string fen;
    for(uint8_t rank=0U;rank<8U;++rank) {
        uint8_t empty=0U;
        for(uint8_t file=0U;file<8U;++file) {
            const char piece=pieces[rank*8U+file];
            if(piece=='\0') { ++empty; continue; }
            if(empty!=0U) { fen+=char('0'+empty); empty=0U; }
            fen+=piece;
        }
        if(empty!=0U) fen+=char('0'+empty);
        if(rank!=7U) fen+='/';
    }
    return fen+(cpu?" b - - 0 1":" w - - 0 1");
}
bool RevealKnight(runtime::VirtualLitchSession& s, PublicationEngine& engine,
                  const KnightRoute& route, bool cpu, bool firstP2, const char* run) {
    engine.Choose(route.squares[0U],route.squares[3U]);
    if(cpu) {
        engine.finishCpu=true;
        (void)firstP2; // Player 2 remains Black; end preference changes physical mapping only.
        if(!Setting(s,"VS_CPU_PLAYER","2") || !Setting(s,"VS_CPU_LEVEL","2")) return false;
    } else if(!Tap(s)) return false;
    for(uint8_t tick=0U;tick<100U;++tick) {
        uint8_t from=64U,to=64U;
        const auto home=s.HomeStateSnapshot();
        if(cpu ? home.physicalMoveValid && home.physicalFromSquare==route.squares[0U] && home.physicalToSquare==route.squares[3U]
               : s.TryGetHintMoveSquares(from,to) && from==route.squares[0U] && to==route.squares[3U]) return true;
        if(s.Advance(20U)!=VIRTUAL_LITCH_OK) return false;
    }
    Checkpoint(s,run,"unexpected-reveal","inspect-authoritative-Hint-or-CPU-tuple","rejected");
    return false;
}
void KnightTravelJourney(const KnightRoute& route, bool cpu, bool firstP2, bool rotate, bool blocked=false, uint16_t currentLimit=0U) {
    char run[150]; std::snprintf(run,sizeof(run),"KnightL/V3.3/%s/%s/P%u/rotate%u/blocked%u/cap%u",
        cpu?"CPU":"raw-O",route.name,firstP2?2U:1U,rotate,blocked,currentLimit);
    auto config=Config(rotate); config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3; config.first_player_is_p2=firstP2; config.max_led_current_ma=currentLimit;
    PublicationEngine engine; runtime::VirtualLitchSession s(config,engine);
    const auto fen=KnightFen(route,cpu,blocked,false);
    uint64_t occupancy=Bit(0U)|Bit(63U)|Bit(15U)|Bit(48U)|Bit(route.squares[0U]);
    if(blocked) occupancy|=Bit(route.squares[1U])|Bit(route.squares[2U]);
    // Move P2 off its default red before selecting red for P1; otherwise the
    // normal minimum color separation adjusts the first mutation away from 0.
    const bool ready=s.Reset()==VIRTUAL_LITCH_OK && s.LoadExternalFen(fen.c_str()) && Observe(s,occupancy) &&
        s.Advance(1000U)==VIRTUAL_LITCH_OK && Setting(s,"P2_END_PIECE_WHEEL","85") && Setting(s,"P1_END_PIECE_WHEEL","0");
    Check(ready,run,"real legal knight position, raw Hall and distinct player colors enter production Play"); if(!ready)return;
    std::array<SquarePixels,64U> ordinary{};
    for(uint8_t square=0U;square<64U;++square) ordinary[square]=AllSquarePixels(s,square);
    const bool revealed=RevealKnight(s,engine,route,cpu,firstP2,run);
    Check(revealed,run,"raw O Hint or acknowledged CPU settings publish the exact real-rules legal knight tuple"); if(!revealed)return;
    uint64_t routeMask=0ULL; for(const auto square:route.squares)routeMask|=Bit(square);
    bool sourceOn=false,sourceOff=false;
    for(const bool held:{false,true}) {
        if(held) Check(s.Advance(20U)==VIRTUAL_LITCH_OK && Observe(s,occupancy&~Bit(route.squares[0U])),run,
            "raw source pickup after destination dwell retains the directed knight plan");
        const uint64_t legal=s.State().valid_destination_mask;
        std::array<SquarePixels,64U> baseline{};
        for(uint8_t square=0U;square<64U;++square) baseline[square]=held?AllSquarePixels(s,square):ordinary[square];
        const uint32_t start=s.State().now_ms;
        bool ordinaryRevealCompleted = false;
        const auto phaseMask = [&](uint8_t step, uint8_t local) {
            const uint8_t incoming=static_cast<uint8_t>(1U<<route.positions[step*2U]);
            const uint8_t outgoing=static_cast<uint8_t>(1U<<route.positions[step*2U+1U]);
            return local==0U?incoming:local==2U?outgoing:static_cast<uint8_t>(15U & ~(incoming|outgoing));
        };
        for(uint8_t cycle=0U;cycle<2U;++cycle)for(uint8_t node=0U;node<12U;++node) {
            const uint32_t age=cycle*1680U+(node*1440U+10U)/11U;
            if(node>1U) {
                const uint32_t midpoint=cycle*1680U+((2U*node-1U)*1440U)/22U;
                Check(Align(s,start+midpoint),run,"sample continuous axis handoff before its next peak");
                const uint8_t previous=route.squares[(node-1U)/3U],next=route.squares[node/3U];
                if(previous!=route.squares[0U] && next!=route.squares[3U] && !blocked) {
                    bool priorLit=false,nextLit=false;
                    for(uint8_t pos=0U;pos<4U;++pos) {
                        priorLit=priorLit || ((phaseMask((node-1U)/3U,(node-1U)%3U)&(1U<<pos)) && Energy(Emitter(s,previous,pos))>0U);
                        nextLit=nextLit || ((phaseMask(node/3U,node%3U)&(1U<<pos)) && Energy(Emitter(s,next,pos))>0U);
                    }
                    Check(priorLit && nextLit,run,"the crossfade joins both L legs and the elbow");
                }
            }
            Check(Align(s,start+age),run,"sample each three-beat L-route phase");
            const uint8_t step=node/3U, square=route.squares[step];
            const bool occupied=(occupancy&Bit(square))!=0ULL && !(held && step==0U);
            const uint8_t mask=phaseMask(step,node%3U);
            uint8_t position=0U;while(position<4U && (mask&(1U<<position))==0U)++position;
            const auto color=Emitter(s,square,position);
            if(occupied) {
                Check(Energy(Emitter(s,square,0U))>0U || Energy(Emitter(s,square,2U))>0U,run,
                    "occupied route phases use side rails while preserving Piece identity");
            } else {
                const auto mover=ordinary[route.squares[0U]].mapped[1U];
                for(uint8_t pos=0U;pos<4U;++pos) if(mask&(1U<<pos)) {
                    const auto head=Emitter(s,square,pos);
                    Check(Energy(head)>=(currentLimit?1U:115U),run,
                        "incoming, middle pair and outgoing use the actual mapped L-route emitters");
                    if(!currentLimit && step!=3U) {
                        const int tolerance=3*std::max({mover.r,mover.g,mover.b});
                        Check(std::abs(int(head.r*mover.g)-int(head.g*mover.r))<=tolerance &&
                            std::abs(int(head.r*mover.b)-int(head.b*mover.r))<=tolerance &&
                            std::abs(int(head.g*mover.b)-int(head.b*mover.g))<=tolerance,run,
                            "L travel retains the actual moving player's RGB through physical mapping");
                    }
                }
            }
            Check(Energy(Emitter(s,route.squares[3U],1U))>0U && Energy(Emitter(s,route.squares[3U],3U))>0U,
                run,"both actual destination Piece LEDs retain a visible landing floor");
            for (uint8_t sq = 0U; sq < 64U; ++sq) {
                if ((routeMask & Bit(sq)) != 0ULL) continue;
                const auto now = AllSquarePixels(s, sq);
                if (held && !cpu && !ordinaryRevealCompleted && (legal & Bit(sq)) != 0ULL) {
                    Check(Equal(now.semantic[1U], baseline[sq].semantic[1U]) &&
                              (currentLimit || (Equal(now.mapped[1U], baseline[sq].mapped[1U]) &&
                                                Equal(now.mapped[2U], baseline[sq].mapped[2U]))),
                          run, "off-route legal reveal preserves Piece identity; only the hard limiter may scale it");
                    if (age >= 400U) {
                        Check(HasSteadyWhiteLegalRails(now, firstP2), run,
                              "off-route knight choices finish at the configured color on both visible rails");
                        baseline[sq] = now;
                    }
                } else {
                    const bool identity = Equal(now.semantic[0U], baseline[sq].semantic[0U]) &&
                        Equal(now.semantic[1U], baseline[sq].semantic[1U]) &&
                        Equal(now.semantic[2U], baseline[sq].semantic[2U]);
                    Check(currentLimit ? identity : SameSquarePixels(now, baseline[sq]), run,
                          "all off-route emitters retain identity outside the reveal; only the hard limiter may scale physical output");
                }
            }
            ordinaryRevealCompleted = ordinaryRevealCompleted || age >= 400U;
            if(blocked)for(uint8_t step=1U;step<3U;++step) {
                const uint8_t transit=route.squares[step];
                const auto now=AllSquarePixels(s,transit);
                Check(Equal(now.semantic[1U],baseline[transit].semantic[1U]) &&
                    Equal(now.mapped[1U],baseline[transit].mapped[1U]) &&
                    Equal(now.mapped[2U],baseline[transit].mapped[2U]),run,
                    "the moving head crosses occupied transit on Square LEDs while both actual Piece LEDs retain ordinary identity");
            }
            if(currentLimit)Check(s.PhysicalFrame().applied_led_current_ma<=currentLimit,run,"actual L-route output remains within the unchanged hard current cap");
            for(uint8_t step=1U;step<3U;++step)if(!blocked && route.squares[step]!=square) {
                const bool roundedHandoff=node%3U==2U && node+1U<12U && route.squares[step]==route.squares[node/3U+1U];
                Check(roundedHandoff?Energy(Emitter(s,route.squares[step],1U))<=2U && Energy(Emitter(s,route.squares[step],3U))<=2U:
                    EmptyPieceTargets(s,route.squares[step]),run,"only the current L head or its immediate handoff may light empty transit Piece LEDs");
            }
            if(!held) {
                const auto top=Energy(Emitter(s,route.squares[0U],1U)),bottom=Energy(Emitter(s,route.squares[0U],3U));
                Check(s.State().now_ms%800U<600U?top>0U&&bottom>0U:top==0U&&bottom==0U,run,
                    "source Piece LEDs retain their authoritative 600/200 ON and completely OFF cadence");
                sourceOn=sourceOn||(top>0U&&bottom>0U);sourceOff=sourceOff||(top==0U&&bottom==0U);
            } else Check(s.State().valid_destination_mask==legal,run,"L travel cannot change ordinary held-piece legal admission");
            if(cycle==0U && (!firstP2&&!rotate || node==3U || node==7U || node==11U) || cycle==1U&&node==0U) {
                std::printf("[KNIGHT_L_EMITTER] run=%s held=%u age=%u node=%u logical=%u position=%u mapped=%u rgb=%u,%u,%u,%u current=%u limited=%u\n",
                    run,held,age,node,square,position,runtime::MapSquareIndex(square,Rotated(s))*4U+(Rotated(s)?(position+2U)%4U:position),
                    color.r,color.g,color.b,color.w,s.PhysicalFrame().applied_led_current_ma,s.PhysicalFrame().power_limited);
                if(node==3U||node==7U||node==11U||cycle==1U)Checkpoint(s,run,cycle==1U?"one-way-source-repeat":"L-head",
                    cpu?"move-the-computer-knight-to-its-target":"follow-Hint-or-choose-any-legal-move","accepted",routeMask);
            }
            if(node==11U)for(const uint32_t dwell:{1560U,1660U}) {
                Check(Align(s,start+cycle*1680U+dwell),run,"hold the head at the destination through its existing240ms dwell");
                Check(Energy(Emitter(s,route.squares[3U],route.positions[7U]))>=(currentLimit?1U:115U),run,
                    "destination dwell retains a visible mover-color head instead of starting reverse travel");
                if(!blocked)for(uint8_t step=1U;step<3U;++step)Check(EmptyPieceTargets(s,route.squares[step]),run,
                    "destination dwell leaves no empty Piece trail behind either L leg");
            }
        }
    }
    Check(sourceOn&&sourceOff,run,"timed raw route playback includes both actual source blink states");
    Check(Observe(s,occupancy)&&s.Advance(250U)==VIRTUAL_LITCH_OK,run,"returning held knight restores Idle guidance");
    if(blocked) {
        Check(Observe(s,occupancy&~Bit(route.squares[1U]))&&s.Advance(200U)==VIRTUAL_LITCH_OK,run,"lifting another friendly piece invokes ordinary legal or required CPU correction guidance");
        Check(EmptyPieceTargets(s,route.squares[3U]),run,"wrong-piece handling suppresses the optional knight landing and route");
        if(!firstP2&&!rotate)Checkpoint(s,run,"wrong-piece-suppressed","return-the-other-piece-or-follow-required-correction","accepted",routeMask);
        Check(Observe(s,occupancy)&&s.Advance(250U)==VIRTUAL_LITCH_OK,run,"returning the other piece recovers the existing knight owner");
    }
    Check(Setting(s,"MINIMAL_ANIMATIONS","true")&&s.Advance(20U)==VIRTUAL_LITCH_OK,run,"existing Minimal setting suppresses optional travel");
    const auto still=AllSquarePixels(s,route.squares[3U]);
    Check(s.Advance(1680U)==VIRTUAL_LITCH_OK,run,"advance one complete cycle under Minimal");
    const auto later=AllSquarePixels(s,route.squares[3U]);
    bool staticTarget=true;for(uint8_t slot=0U;slot<3U;++slot)staticTarget=staticTarget&&Equal(still.semantic[slot],later.semantic[slot]);
    Check(currentLimit?staticTarget:SameSquarePixels(still,later),run,
        "Minimal keeps a static actual landing pair; the hard limiter alone may scale its mapped output");
    for (uint8_t step = 1U; step < 3U; ++step) {
        const auto restored = AllSquarePixels(s, route.squares[step]);
        const auto& expected = ordinary[route.squares[step]];
        const bool identity = Equal(restored.semantic[0U], expected.semantic[0U]) &&
            Equal(restored.semantic[1U], expected.semantic[1U]) &&
            Equal(restored.semantic[2U], expected.semantic[2U]);
        Check(currentLimit ? identity : SameSquarePixels(restored, expected), run,
              "Minimal restores transit identity; only the hard limiter may scale mapped output");
    }
    if(!firstP2&&!rotate)Checkpoint(s,run,"Minimal-static-L-retired",cpu?"follow-the-CPU-endpoint":"follow-Hint-or-hide-it","accepted",routeMask);
    if(!cpu && currentLimit==0U) {
        uint8_t replayFrom=64U,replayTo=64U;
        const uint32_t starts=engine.starts,cancels=engine.cancels;
        Check(Tap(s)&&!s.HintDisplayRequested()&&s.TryGetHintMoveSquares(replayFrom,replayTo)&&
            replayFrom==route.squares[0U]&&replayTo==route.squares[3U]&&EmptyPieceTargets(s,route.squares[3U])&&
            engine.starts==starts&&engine.cancels==cancels,run,
            "Coaching-off retap hides the knight route while retaining its cached tuple and worker");
        Check(Tap(s)&&s.HintDisplayRequested()&&s.TryGetHintMoveSquares(replayFrom,replayTo)&&
            replayFrom==route.squares[0U]&&replayTo==route.squares[3U]&&
            engine.starts==starts&&engine.cancels==cancels,run,
            "the next raw O reveals the same knight route without another search");
        Check(s.LoadExternalFen(fen.c_str())&&!s.HintDisplayRequested()&&s.Advance(250U)==VIRTUAL_LITCH_OK,run,
            "the real external-position owner invalidates the old knight route");
        for(uint8_t square=0U;square<64U;++square)Check(SameSquarePixels(AllSquarePixels(s,square),ordinary[square]),run,
            "position invalidation clears the whole L cue without affecting unrelated identities");
        if(!firstP2&&!rotate)Checkpoint(s,run,"position-invalidated-L-cleared","choose-any-legal-move","accepted",routeMask);
    }
}
void KnightDirectionalMatrix() {
    for(const auto& route:KNIGHT_ROUTES)for(const bool cpu:{false,true})for(const bool first:{false,true})for(const bool rotate:{false,true})
        KnightTravelJourney(route,cpu,first,rotate);
    for(const bool cpu:{false,true})for(const bool first:{false,true})for(const bool rotate:{false,true})
        KnightTravelJourney(KNIGHT_ROUTES[1U],cpu,first,rotate,true);
    for(const bool cpu:{false,true})KnightTravelJourney(KNIGHT_ROUTES[0U],cpu,false,true,false,80U);
}

bool RawUndoTap(runtime::VirtualLitchSession& s);
void KnightCaptureJourney(bool cpu, bool sourceFirst, bool firstP2, bool rotate) {
    const auto& route=KNIGHT_ROUTES[0U];
    char run[140];std::snprintf(run,sizeof(run),"KnightL/capture-%s/%s/P%u/rotate%u",
        sourceFirst?"source-first":"victim-first",cpu?"CPU":"raw-O",firstP2?2U:1U,rotate);
    auto config=Config(rotate);config.hardware=VIRTUAL_LITCH_HARDWARE_V3_3;config.first_player_is_p2=firstP2;
    PublicationEngine engine;runtime::VirtualLitchSession s(config,engine);
    const auto fen=KnightFen(route,cpu,false,true);
    uint64_t occupancy=Bit(0U)|Bit(15U)|Bit(48U)|Bit(63U)|Bit(35U)|Bit(18U);
    Check(s.Reset()==VIRTUAL_LITCH_OK && s.LoadExternalFen(fen.c_str()) && Observe(s,occupancy) &&
        s.Advance(1000U)==VIRTUAL_LITCH_OK && RevealKnight(s,engine,route,cpu,firstP2,run),run,
        "real legal knight capture, Hall and existing Hint or CPU owner enter mandatory victim removal");
    const auto pending=[&]() {
        bool longLeg=false,elbow=false,routeCleared=false;
        for(uint8_t sample=0U;sample<85U;++sample) {
            const auto left=Emitter(s,18U,0U),right=Emitter(s,18U,2U);
            Check(EmptyPieceTargets(s,18U) && Equal(left,right) && left.r>0U && left.b==0U && left.w==0U,run,
                "pending victim keeps both actual Piece LEDs dark and its synchronized warm side rails visible");
            const auto transit=AllSquarePixels(s,27U),corner=AllSquarePixels(s,19U);
            const bool longNow=Energy(transit.mapped[1U])+Energy(transit.mapped[2U])>0U;
            const bool elbowNow=Energy(corner.mapped[1U])+Energy(corner.mapped[2U])>0U;
            if((longNow && !longLeg) || (elbowNow && !elbow))
                Checkpoint(s,run,longNow&&!longLeg?"pending-long-leg":"pending-elbow",
                    "remove-c6-victim","accepted",Bit(35U)|Bit(27U)|Bit(19U)|Bit(18U));
            longLeg=longLeg || longNow;elbow=elbow || elbowNow;
            routeCleared=routeCleared || (EmptyPieceTargets(s,27U) && EmptyPieceTargets(s,19U));
            Check(s.Advance(20U)==VIRTUAL_LITCH_OK,run,"sample one physical L pass while awaiting Hall removal");
        }
        Check(longLeg && elbow && routeCleared,run,
            "pending capture keeps its moving L route through both empty cells without relighting the victim's Piece LEDs");
    };
    pending();
    Checkpoint(s,run,"warm-victim-L-active","remove-the-captured-piece","accepted",Bit(35U)|Bit(27U)|Bit(19U)|Bit(18U));
    if(sourceFirst) {
        occupancy&=~Bit(35U);Check(Observe(s,occupancy),run,"raw source-first knight lift keeps victim-removal priority");pending();
        occupancy&=~Bit(18U);Check(Observe(s,occupancy),run,"actual victim removal releases the waiting landing pair beside the continuing L cue");
    } else {
        occupancy&=~Bit(18U);Check(Observe(s,occupancy),run,"raw victim-first removal selects its real legal knight capturer");
        occupancy&=~Bit(35U);Check(Observe(s,occupancy),run,"lifting the selected knight admits its existing capture execution plan");
    }
    bool longLeg=false,corner=false,shortLeg=false;
    for(uint8_t sample=0U;sample<85U;++sample) {
        Check(Energy(Emitter(s,18U,1U))>0U && Energy(Emitter(s,18U,3U))>0U,run,
            "after removal both actual target Piece emitters show the unambiguous landing floor");
        longLeg=longLeg||Energy(Emitter(s,27U,1U))>0U||Energy(Emitter(s,27U,3U))>0U;
        corner=corner||Energy(Emitter(s,19U,3U))>0U;
        shortLeg=shortLeg||Energy(Emitter(s,19U,0U))>0U;
        Check(s.Advance(20U)==VIRTUAL_LITCH_OK,run,"raw held-capture presentation traverses one full L pass at real output cadence");
    }
    Check(longLeg&&corner&&shortLeg,run,"source-first and victim-first capture both retain the actual two-axis L route after removal");
    Checkpoint(s,run,"removed-held-L-active","place-the-knight-on-c6","accepted",Bit(35U)|Bit(27U)|Bit(19U)|Bit(18U));
    occupancy|=Bit(18U);Check(Observe(s,occupancy)&&s.Advance(2100U)==VIRTUAL_LITCH_OK&&s.UndoHistoryMoveCount()==1U,run,
        "placing the knight commits exactly one real capture after ordinary settling");
    Check(EmptyPieceTargets(s,27U)&&EmptyPieceTargets(s,19U),run,"committed capture clears all transient empty-route Piece emitters");
    const auto captureBackgroundBefore = AllSquarePixels(s,63U);
    Check(RawUndoTap(s)&&s.IsUndoActionPending()&&s.Advance(1U)==VIRTUAL_LITCH_OK,run,"raw X enters the existing capture Undo choreography");
    const auto captureBackgroundDuring = AllSquarePixels(s,63U);
    for (uint8_t slot : {1U, 2U}) {
        const auto before = Energy(captureBackgroundBefore.mapped[slot]);
        const auto during = Energy(captureBackgroundDuring.mapped[slot]);
        Check(before > 4U && during * 2U < before,run,"capture Undo dims both LEDs of the unrelated king");
    }
    Check(EmptyPieceTargets(s,27U)&&EmptyPieceTargets(s,19U),run,"Undo owns precise return prompts and cannot retain a knight L route");
    occupancy&=~Bit(18U);Check(Observe(s,occupancy),run,"Undo lifts the captured-move knight from its actual destination");
    occupancy|=Bit(35U);Check(Observe(s,occupancy),run,"Undo returns the knight to its actual origin");
    Check(Energy(Square(s,18U)) > 0U,run,"capture Undo points to the victim restoration square after the mover returns");
    occupancy|=Bit(18U);Check(Observe(s,occupancy)&&s.Advance(2100U)==VIRTUAL_LITCH_OK&&s.UndoHistoryMoveCount()==0U,run,
        "Undo completes required captured-piece restoration and removes exactly one history move");
    if(cpu) {
        const auto home=s.HomeStateSnapshot();
        Check(!s.IsUndoActionPending() && s.State().interaction_phase==static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan) &&
            s.State().required_action==static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftPrimary) &&
            home.physicalMoveValid && home.physicalFromSquare==35U && home.physicalToSquare==18U && EmptyPieceTargets(s,18U),run,
            "completed CPU Undo retires its return prompts and admits the same real CPU capture with warm removal guidance");
    } else {
        Check(EmptyPieceTargets(s,27U)&&EmptyPieceTargets(s,19U)&&Energy(Emitter(s,18U,1U))>0U&&Energy(Emitter(s,18U,3U))>0U,run,
            "completed human Undo restores victim identity and leaves no phantom L or removal cue");
    }
    Checkpoint(s,run,"capture-Undo-restored",cpu?"follow-the-new-CPU-capture":"choose-the-next-legal-move","accepted",Bit(35U)|Bit(27U)|Bit(19U)|Bit(18U));
}

void HintFeedbackV33PhysicalJourney() {
    const char* run = "HintFeedback/V3.3/raw-O-and-mapped-emitters";
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    PublicationEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    Check(session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
              session.Advance(500U) == VIRTUAL_LITCH_OK && Tap(session),
          run, "raw starting Hall and O request enter production Hint ownership on V3.3");
    engine.Choose(52U, 36U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK && session.PhysicalFrame().led_count == 300U,
          run, "completed controlled publication reaches the real V3.3 output mapper");
    modules::ControlMutationOutcome outcome;
    uint8_t from = 64U, to = 64U;
    Check(session.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint && outcome.status == modules::ControlMutationStatus::Applied &&
              session.TryGetHintMoveSquares(from, to) && from == 52U && to == 36U,
          run, "actual Hint acknowledgement and legal current tuple agree");
    const uint32_t offAt = (session.State().now_ms / 800U + 1U) * 800U + 650U;
    Check(Align(session, offAt) && EmptyPieceTargets(session, from) &&
              Energy(Square(session, from)) > 0U && Energy(Square(session, to)) > 0U,
          run, "both V3.3 piece emitters blink off while active source and destination side cues stay lit");
    Checkpoint(session, run, "source-off", "lift-e2-or-select-any-legal-piece");
    engine.Choose(50U, 34U);
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK &&
              session.TryGetHintMoveSquares(from, to) && from == 50U && to == 34U,
          run, "changed-source publication starts its bounded visible handoff");
    const auto whiteCorners = [&] {
        bool white = true;
        for (const uint8_t index : modules::PlayerCornerLedIndices(Owner(session))) {
            const auto semantic = session.SemanticFrame().status[index];
            const auto mapped = session.PhysicalFrame().pixels[256U + hardware::V3_3PhysicalIndexForSemanticStatus(index)];
            std::printf("[HINT_V33_CORNERS] t=%u corner=%u semantic=%u,%u,%u,%u mapped=%u,%u,%u,%u\n",
                        session.State().now_ms, index, semantic.r, semantic.g, semantic.b, semantic.w,
                        mapped.r, mapped.g, mapped.b, mapped.w);
            white = white && semantic.r > 0U && semantic.r == semantic.g && semantic.g == semantic.b &&
                Energy(mapped) > 0U && mapped.r == mapped.g && mapped.g == mapped.b;
        }
        return white;
    };
    Check(whiteCorners() && session.Advance(280U) == VIRTUAL_LITCH_OK && !whiteCorners() &&
              session.Advance(140U) == VIRTUAL_LITCH_OK && whiteCorners() &&
              session.Advance(300U) == VIRTUAL_LITCH_OK && !whiteCorners(),
          run, "V3.3 mapped player corners deliver first pulse, ordinary gap, second pulse and expiry");
    Checkpoint(session, run, "updated-c2c4-ready", "lift-c2-or-select-any-legal-piece");
    Check(Observe(session, OPENING & ~Bit(50U)) &&
              session.State().selected_square == 50U && (session.State().valid_destination_mask & Bit(34U)) != 0ULL &&
              Observe(session, OPENING) && session.Advance(1U) == VIRTUAL_LITCH_OK,
          run, "real source pickup retains legal c4, and return restores the current Hint without committing a move");
    Checkpoint(session, run, "pickup-return", "choose-any-legal-move");
}

struct CaptureCase { const char* name; const char* fen; uint64_t occupancy; uint8_t victim; uint64_t capturers; uint8_t excluded; bool visibleHint = false; };
void CaptureFirstGuidanceJourney(const CaptureCase& position, bool rotate) {
    test_support::BoundedHintSession assisted(Config(rotate)), ordinary(Config(rotate));
    auto& s = ordinary.session; auto& control = assisted.session;
    const auto checkpoint = [&](const char* stage, const char* next, const char* outcome = "accepted") {
        const uint64_t recoverySquares = position.capturers == (Bit(26U) | Bit(28U) | Bit(51U))
            ? Bit(19U) | Bit(60U) | Bit(16U) : 0ULL;
        Checkpoint(s, position.name, stage, next, outcome,
                   position.capturers | Bit(position.victim) | recoverySquares);
    };
    const auto ordinarySources = [&](const std::array<SquarePixels, 64U>& baseline, uint64_t mask) {
        for (uint8_t sq = 0U; sq < 64U; ++sq)
            if ((mask & Bit(sq)) != 0ULL && !SameSquarePixels(AllSquarePixels(s, sq), baseline[sq])) return false;
        return true;
    };
    const auto prepare = [&](runtime::VirtualLitchSession& target) {
        return Ready(target) && target.LoadExternalFen(position.fen) && Observe(target, position.occupancy) &&
               target.Advance(200U) == VIRTUAL_LITCH_OK;
    };
    if (!(prepare(s) && prepare(control))) { Check(false, position.name, "real FEN/Hall setup reaches physical play"); return; }
    Check(Coach(control) && Align(s, control.State().now_ms) &&
          !s.HomeStateSnapshot().assistance.coachingForced &&
          control.HomeStateSnapshot().assistance.coachingForced,
          position.name, "reported Coaching-off turn is paired with the existing Coaching-on appearance");
    Check(Read(s).completed, position.name, "actual bounded production engine has completed before victim-first pickup");
    Check(s.Advance(90000U) == VIRTUAL_LITCH_OK && Align(control, s.State().now_ms),
          position.name, "unchanged-position guidance remains usable after the analysis window");
    std::array<SquarePixels, 64U> ordinarySquares{};
    for (uint8_t sq = 0U; sq < 64U; ++sq) ordinarySquares[sq] = AllSquarePixels(s, sq);
    if (position.visibleHint) {
        Check(Tap(s) && Align(control, s.State().now_ms), position.name,
              "raw O reveals completed capture Hint through production control acknowledgement");
        uint8_t from = 64U, to = 64U; modules::ControlMutationOutcome outcome;
        Check(s.TryGetLastBoardControlOutcome(outcome) &&
              outcome.action == modules::ControlAction::Hint &&
              outcome.status == modules::ControlMutationStatus::Applied &&
              s.TryGetHintMoveSquares(from, to) && (position.capturers & Bit(from)) != 0ULL &&
              (to == position.victim || (position.victim == 27U && to == 19U &&
                                        (from == 26U || from == 28U))),
              position.name, "actual production engine publishes a capture Hint for the lifted victim");
        checkpoint("capture-Hint-visible", "remove-hinted-victim-first");
    }
    const uint64_t lifted = position.occupancy & ~Bit(position.victim);
    Check(Observe(s, lifted) && Observe(control, lifted) &&
          s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::CaptureStaged),
          position.name, "raw victim lift enters authoritative capture-first stage");
    Check(Read(s).capturers == position.capturers && Read(control).capturers == position.capturers,
          position.name, "completed-position cache includes all and only legal still-present capturers");
    uint64_t shownSources = 0ULL, sweptOrigins = 0ULL;
    bool rookTransitLit = false, rookTransitCleared = false, epLandingLit = false, epLandingCleared = false;
    std::array<std::array<uint16_t, 2U>, 64U> lowPiece{}, highPiece{};
    for (auto& low : lowPiece) low.fill(65535U);
    for (uint8_t frame = 0U; frame <= 45U; ++frame) {
        uint8_t sweepingSources = 0U;
        for (uint8_t sq = 0U; sq < 64U; ++sq) if ((position.capturers & Bit(sq)) != 0ULL) {
            if (!SameSquarePixels(AllSquarePixels(s, sq), ordinarySquares[sq])) shownSources |= Bit(sq);
            if (!Equal(Emitter(s, sq, 0U), Emitter(s, sq, 2U))) {
                sweptOrigins |= Bit(sq);
                ++sweepingSources;
            }
            Check(SameSquarePixels(AllSquarePixels(s, sq), AllSquarePixels(control, sq)),
                  position.name, "Coaching off and on use the same semantic and actual V3 capture pulse");
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3S3, sq, hardware::SquareLedSlot::Piece, Rotated(s));
            for (uint8_t target = 0U; target < targets.count; ++target) {
                const uint16_t energy = Energy(s.PhysicalFrame().pixels[targets.indices[target]]);
                if (energy < lowPiece[sq][target]) lowPiece[sq][target] = energy;
                if (energy > highPiece[sq][target]) highPiece[sq][target] = energy;
            }
            const auto piece = Piece(s, sq), ordinaryPiece = Piece(control, sq);
            Check((ordinaryPiece.r != 0U || piece.r == 0U) &&
                  (ordinaryPiece.g != 0U || piece.g == 0U) &&
                  (ordinaryPiece.b != 0U || piece.b == 0U) &&
                  (ordinaryPiece.w != 0U || piece.w == 0U),
                  position.name, "capture source pulse preserves ordinary own-piece color channels");
        }
        Check(sweepingSources <= 1U, position.name,
              "ambiguous capture choices never launch competing source sweeps at once");
        const auto victim = AllSquarePixels(s, position.victim);
        Check(!Equal(victim.semantic[0U], ordinarySquares[position.victim].semantic[0U]) &&
              !Equal(victim.semantic[2U], ordinarySquares[position.victim].semantic[2U]) &&
              !Equal(victim.mapped[0U], ordinarySquares[position.victim].mapped[0U]) &&
              !Equal(victim.mapped[3U], ordinarySquares[position.victim].mapped[3U]) &&
              Energy(victim.mapped[0U]) > 0U && Energy(victim.mapped[3U]) > 0U,
              position.name, "empty victim socket retains distinct guidance on both semantic and actual V3 side emitters");
        Check(EmptyPieceTargets(s, position.victim) && EmptyPieceTargets(control, position.victim),
              position.name, "removed victim never keeps a false piece-presence light");
        if (position.excluded < 64U)
            Check(SameSquarePixels(AllSquarePixels(s, position.excluded), ordinarySquares[position.excluded]), position.name, "pinned noncapturer retains ordinary identity and square");
        if (position.victim == 35U && position.capturers == Bit(51U)) {
            rookTransitLit = rookTransitLit || !EmptyPieceTargets(s, 43U);
            rookTransitCleared = rookTransitCleared || (rookTransitLit && EmptyPieceTargets(s, 43U));
        }
        if (position.capturers == (Bit(26U) | Bit(28U) | Bit(51U))) {
            const auto landing=AllSquarePixels(control,19U);
            const bool visible=!Equal(landing.mapped[0U],ordinarySquares[19U].mapped[0U]) ||
                               !Equal(landing.mapped[3U],ordinarySquares[19U].mapped[3U]);
            epLandingLit=epLandingLit || visible;
            epLandingCleared=epLandingCleared || (epLandingLit && !visible);
            for(const uint8_t side:{0U,2U}) {
                const auto rail=landing.semantic[side];
                Check(!(rail.r>0U && rail.g<=rail.r && rail.b==0U),position.name,
                      "separate en-passant landing never displays the warm victim-removal cue");
            }
        }
        checkpoint("capture-pulse-Coaching-off", "lift-one-legal-capturer-or-return-victim");
        if (frame != 45U) Check(s.Advance(100U) == VIRTUAL_LITCH_OK && Align(control, s.State().now_ms), position.name, "sample capture-first assistance pulse");
    }
    for (uint8_t sq = 0U; sq < 64U; ++sq) if ((position.capturers & Bit(sq)) != 0ULL) {
        for (uint8_t target = 0U; target < 2U; ++target) {
            Check(lowPiece[sq][target] > 0U && highPiece[sq][target] > lowPiece[sq][target], position.name,
                  "every mapped V3 piece emitter keeps the temporal own-piece pulse");
            std::printf("[CHESS_ASSISTANCE_CAPTURE_SOURCE] run=%s square=%u target=%u visible=%u piece_min=%u piece_max=%u\n",
                        position.name, sq, target, (shownSources & Bit(sq)) != 0ULL,
                        lowPiece[sq][target], highPiece[sq][target]);
        }
    }
    Check(sweptOrigins == position.capturers, position.name,
          "one complete cycle visits every legal origin, including en-passant pawns with their separate landing");
    if (position.capturers == (Bit(26U) | Bit(28U) | Bit(51U)))
        Check(epLandingLit && epLandingCleared, position.name,
              "en-passant travel reaches its separate d6 landing then clears it while the d5 victim socket stays dark");
    if (position.victim == 35U && position.capturers == Bit(51U))
        Check(rookTransitLit && rookTransitCleared, position.name,
              "victim-first rook travel crosses the actual empty d3 Piece LEDs then clears them");
    Check(Setting(s, "MINIMAL_ANIMATIONS", "true") && s.Advance(20U) == VIRTUAL_LITCH_OK,
          position.name, "Minimal keeps source invitations and victim rails without travel");
    const auto minimalVictim = AllSquarePixels(s, position.victim);
    std::array<SquarePixels,64U> minimalSources{};
    for(uint8_t sq=0U;sq<64U;++sq) if((position.capturers&Bit(sq))!=0ULL)
        minimalSources[sq]=AllSquarePixels(s,sq);
    const auto minimalEpLanding=AllSquarePixels(s,19U);
    Check(EmptyPieceTargets(s, position.victim) && s.Advance(4500U) == VIRTUAL_LITCH_OK &&
              SameSquarePixels(minimalVictim, AllSquarePixels(s, position.victim)),
          position.name, "Minimal leaves the unchosen victim socket steady with its Piece LEDs dark over a full selection cycle");
    for(uint8_t sq=0U;sq<64U;++sq) if((position.capturers&Bit(sq))!=0ULL)
        Check(SameSquarePixels(minimalSources[sq],AllSquarePixels(s,sq)),position.name,
              "Minimal keeps each legal capturer invitation steady across a full selection cycle");
    if(position.capturers == (Bit(26U) | Bit(28U) | Bit(51U)))
        Check(SameSquarePixels(minimalEpLanding,AllSquarePixels(s,19U)) && EmptyPieceTargets(s,19U),position.name,
              "Minimal keeps the separate unchosen en-passant landing steady and dark across a full selection cycle");
    Check(Setting(s, "MINIMAL_ANIMATIONS", "false") && s.Advance(20U) == VIRTUAL_LITCH_OK &&
              Align(control, s.State().now_ms), position.name, "resume ordinary capture choices after Minimal");
    Check(shownSources == position.capturers && s.UndoHistoryMoveCount() == 0U && s.State().correction_mask == 0ULL &&
          s.State().expected_occupied_mask == position.occupancy,
          position.name, "Coaching-off legal capturers visibly pulse without changing game authority");
    checkpoint("victim-first-after-completion", "lift-one-of-the-legal-capturers-or-restore-victim");
    if (position.visibleHint && position.victim == 27U) {
        uint8_t shownFrom=64U,shownTo=64U,replayFrom=64U,replayTo=64U;
        Check(Observe(s,position.occupancy)&&Read(s).capturers==0ULL&&
              s.TryGetHintMoveSquares(shownFrom,shownTo)&&Tap(s)&&!s.HintDisplayRequested()&&
              s.TryGetHintMoveSquares(replayFrom,replayTo)&&shownFrom==replayFrom&&shownTo==replayTo&&
              Tap(s)&&s.HintDisplayRequested()&&
              s.TryGetHintMoveSquares(replayFrom,replayTo)&&shownFrom==replayFrom&&shownTo==replayTo,
              position.name,"restoring the victim permits OFF then ON with the same cached compound capture Hint");
        Check(s.LoadExternalFen(position.fen)&&!s.HintDisplayRequested()&&Observe(s,lifted)&&
              Read(s).capturers==position.capturers,
              position.name,"real position invalidation retires the old Hint; retry retains every legal capture source");
        checkpoint("capture-Hint-replayed-then-invalidated-retry", "choose-any-legal-capturer-or-return-victim");
    }
    if (position.capturers == (Bit(26U) | Bit(28U) | Bit(51U))) {
        Check(Observe(s, position.occupancy) && Read(s).capturers == 0ULL &&
              ordinarySources(ordinarySquares, position.capturers) &&
              s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle),
              position.name, "returning the victim cancels source cues without a move");
        checkpoint("victim-return-cancelled", "remove-d5-again-to-retry");
        const uint64_t kingHeld = lifted & ~Bit(60U);
        const uint64_t missing = Bit(60U) | Bit(position.victim);
        Check(Observe(s, lifted) && Observe(s, kingHeld) &&
              s.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::CorrectBoard) &&
              s.State().correction_mask == missing && s.State().pending_action_token == 0U &&
              Read(s).capturers == 0ULL && s.UndoHistoryMoveCount() == 0U && EmptyPieceTargets(s, 19U),
              position.name, "lifting the ineligible king immediately requests return of e1 and d5 with no d6 landing cue");
        Check(s.Advance(320U) == VIRTUAL_LITCH_OK, position.name, "sample the wrong-source return cue");
        const auto amberRails = [&](uint8_t square) {
            const auto pixels = AllSquarePixels(s, square);
            const auto amber = [](virtual_litch_color color) { return color.r > color.g && color.g > color.b; };
            return amber(pixels.semantic[0U]) && amber(pixels.semantic[2U]) && EmptyPieceTargets(s, square);
        };
        Check(amberRails(60U) && amberRails(position.victim), position.name,
              "missing king and victim show amber return rails with their Piece LEDs dark");
        checkpoint("invalid-noncapturer-lift", "return-king-to-e1-and-victim-to-d5", "rejected-recoverable");
        Check(Observe(s, kingHeld | Bit(19U)) &&
              s.Advance(3000U) == VIRTUAL_LITCH_OK &&
              Read(s).capturers == 0ULL && s.State().pending_action_token == 0U &&
              s.State().correction_mask == (missing | Bit(19U)) && s.State().expected_occupied_mask == position.occupancy &&
              ordinarySources(ordinarySquares, position.capturers) && s.UndoHistoryMoveCount() == 0U &&
              s.HomeStateSnapshot().action == modules::HomeStateAction::Correction,
              position.name, "placing the king on d6 adds that square to correction without submitting a move");
        checkpoint("invalid-noncapturer-move", "return-king-d6-to-e1-and-victim-d5", "rejected-recoverable");
        Check(Observe(s, position.occupancy) && s.State().correction_mask == 0ULL &&
              s.State().human_illegal_mask == 0ULL && s.UndoHistoryMoveCount() == 0U &&
              Read(s).capturers == 0ULL && s.State().pending_action_token == 0U &&
              s.HomeStateSnapshot().action == modules::HomeStateAction::Idle,
              position.name, "canonical restoration clears invalid capture without an engine action");
        Check(Observe(s, lifted) && Observe(s, lifted | Bit(16U)) &&
              ordinarySources(ordinarySquares, position.capturers) &&
              s.State().pending_action_token == 0U && s.UndoHistoryMoveCount() == 0U,
              position.name, "unrelated a6 placement suppresses rendered source cues without exposing an action");
        checkpoint("victim-misplaced", "return-victim-a6-to-d5", "invalid-no-action");
        Check(Observe(s, position.occupancy) && s.State().correction_mask == 0ULL &&
              Read(s).capturers == 0ULL && Observe(s, lifted) &&
              Read(s).capturers == position.capturers,
              position.name, "restoration recovers the same capture cue without setup");
        checkpoint("recovered-capture-staged", "choose-legal-capturer-or-return-victim");
    }
    if (position.visibleHint && position.victim == 35U) {
        Check(Observe(s, position.occupancy) && Read(s).capturers == 0ULL &&
              s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle),
              position.name, "returning hinted victim cancels staging without hiding the existing Hint");
        checkpoint("hinted-victim-returned", "remove-d4-again-or-choose-another-move");
        Check(Observe(s, lifted) && Observe(s, lifted & ~Bit(51U)) &&
              s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan) &&
              Read(s).capturers == 0ULL && Energy(Square(s, position.victim)) > 0U &&
              !EmptyPieceTargets(s, position.victim),
              position.name, "lifting highlighted rook replaces source choices with the directed capture landing");
        checkpoint("hinted-capturer-held", "place-rook-d4");
        Check(Observe(s, (lifted & ~Bit(51U)) | Bit(position.victim)) &&
              s.Advance(3000U) == VIRTUAL_LITCH_OK && s.UndoHistoryMoveCount() == 1U &&
              s.State().side_to_move_black != 0U && Read(s).capturers == 0ULL &&
              s.State().correction_mask == 0ULL && s.State().pending_action_token == 0U,
              position.name, "hinted target-first capture settles and acknowledges exactly once");
        const auto home = s.HomeStateSnapshot();
        Check(home.whiteLastMoveValid && home.whiteLastMoveFromSquare == 51U &&
              home.whiteLastMoveToSquare == position.victim,
              position.name, "published last-move authority identifies the physically selected d2-d4 capture");
        checkpoint("hinted-capture-acknowledged", "Black-to-play");
        return;
    }
    Check(Setting(s, "VS_CPU_PLAYER", s.State().side_to_move_black != 0U ? "2" : "1") &&
          Setting(s, "VS_CPU_LEVEL", "3") && Read(s).capturers == 0ULL &&
          ordinarySources(ordinarySquares, position.capturers),
          position.name, "canonical CPU ownership change while CaptureStaged suppresses human assistance");
    checkpoint("CPU-takeover-while-staged", "restore-victim-before-CPU-play");
    Check(Observe(s, position.occupancy) && Read(s).capturers == 0ULL &&
          ordinarySources(ordinarySquares, position.capturers) && s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) && s.UndoHistoryMoveCount() == 0U,
          position.name, "victim restoration exits assistance immediately without committing a move");
    checkpoint("victim-restored", "wait-for-CPU-selection");
    if (position.capturers == (Bit(26U) | Bit(28U) | Bit(51U))) {
        // The EP/ordinary union is the success discriminator: choose c5 from
        // the optional source cue, rather than the engine's e5 suggestion.
        Check(Setting(s, "VS_CPU_LEVEL", "0") && Observe(control, position.occupancy) &&
              Align(control, s.State().now_ms) && Observe(s, lifted) && Observe(control, lifted) &&
              Read(s).capturers == position.capturers,
              position.name, "human ownership restores the same complete capture-first map");
        const uint64_t held = lifted & ~Bit(26U);
        Check(Observe(s, held) && Observe(control, held) && Read(s).capturers == 0ULL &&
              s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan) &&
              s.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary) &&
              Energy(Square(s, 19U)) > 0U && s.UndoHistoryMoveCount() == 0U,
              position.name, "choosing highlighted c5 retires source choices and lights the directed d6 landing");
        const auto target = AllSquarePixels(s, 19U), ordinaryDark = AllSquarePixels(s, 17U);
        Check(Energy(target.semantic[0U]) > 0U && Energy(target.semantic[2U]) > 0U &&
              !Equal(target.semantic[0U], ordinaryDark.semantic[0U]) &&
              !Equal(target.semantic[2U], ordinaryDark.semantic[2U]) &&
              Energy(target.mapped[0U]) > 0U && Energy(target.mapped[3U]) > 0U &&
              !Equal(target.mapped[0U], ordinaryDark.mapped[0U]) &&
              !Equal(target.mapped[3U], ordinaryDark.mapped[3U]) && !EmptyPieceTargets(s, 19U) &&
              EmptyPieceTargets(s, position.victim),
              position.name, "d6 owns its landing piece pair while the off-destination en-passant victim stays black");
        Check(ordinarySources(ordinarySquares, Bit(28U) | Bit(51U)) && (s.State().physical_occupied_mask & Bit(26U)) == 0ULL,
              position.name, "unchosen capturers retire while only the selected vacated source may carry the transient head");
        Check(s.Advance(900U) == VIRTUAL_LITCH_OK && EmptyPieceTargets(s, 26U), position.name,
              "the selected empty source Piece emitters clear once its head reaches the actual landing");
        checkpoint("highlighted-capturer-held", "place-c5-pawn-on-d6");
        const uint64_t finished = held | Bit(19U);
        Check(Observe(s, finished) && s.State().correction_mask == 0ULL &&
              Read(s).capturers == 0ULL && s.State().required_action != static_cast<uint8_t>(runtime::RequiredPhysicalAction::RemoveCapture),
              position.name, "EP landing acknowledges already-removed off-destination victim without repeating removal");
        checkpoint("EP-landed", "wait-for-move-acknowledgement");
        // The matched renderer control stops before commit: two adapters
        // share the legacy global mutation backend; only this real journey
        // commits, so a second adapter cannot inherit the advanced position.
        Check(s.Advance(3000U) == VIRTUAL_LITCH_OK &&
              s.UndoHistoryMoveCount() == 1U && s.State().expected_occupied_mask == finished &&
              s.State().physical_occupied_mask == finished && s.State().side_to_move_black != 0U &&
              s.State().correction_mask == 0ULL && s.State().human_illegal_mask == 0ULL &&
              s.State().pending_action_token == 0U && Read(s).capturers == 0ULL,
              position.name, "chosen assisted EP commits exactly once and exits to the next player's turn");
        const auto completedHome = s.HomeStateSnapshot();
        Check(completedHome.whiteLastMoveValid && completedHome.whiteLastMoveFromSquare == 26U &&
              completedHome.whiteLastMoveToSquare == 19U,
              position.name, "actual published last-move authority identifies chosen c5d6");
        checkpoint("assisted-EP-committed", "Black-next-move");
    }
}

bool CompoundMove(runtime::VirtualLitchSession& s, uint64_t& occupancy,
                  uint8_t from, uint8_t to, uint8_t duckFrom, uint8_t duckTo) {
    const bool beforeBlack = s.State().side_to_move_black != 0U;
    occupancy &= ~Bit(from); if (!Observe(s, occupancy)) return false;
    occupancy |= Bit(to); if (!Observe(s, occupancy) || s.Advance(1U) != VIRTUAL_LITCH_OK) return false;
    occupancy &= ~Bit(duckFrom); if (!Observe(s, occupancy)) return false;
    occupancy |= Bit(duckTo); if (!Observe(s, occupancy)) return false;
    for (uint8_t i = 0U; i < 60U && (s.State().side_to_move_black != 0U) == beforeBlack; ++i)
        if (s.Advance(100U) != VIRTUAL_LITCH_OK) return false;
    return (s.State().side_to_move_black != 0U) != beforeBlack && s.UndoHistoryMoveCount() > 0U;
}
void DuckCaptureFirstJourney() {
    const char* run = "DuckCaptureFirst/real-compound-turns";
    test_support::BoundedHintSession fixture(Config(true)); auto& s = fixture.session;
    uint64_t occupancy = OPENING | Bit(24U);
    if (!Ready(s, occupancy)) { Check(false, run, "raw initial Duck reaches variant latch"); return; }
    Check(s.GameVariant() == modules::GameVariant::Duck && !s.HomeStateSnapshot().assistance.coachingForced, run, "actual Duck turn starts with Coaching off");
    if (!CompoundMove(s, occupancy, 52U, 36U, 24U, 25U) ||
        !CompoundMove(s, occupancy, 11U, 27U, 25U, 24U)) {
        Check(false, run, "real e4/Duck-b5 and d5/Duck-a5 compound turns complete"); return;
    }
    Check(s.Advance(1000U) == VIRTUAL_LITCH_OK, run, "settle previous compound-turn presentation");
    Check(Observe(s, occupancy & ~Bit(36U)) && (s.State().valid_destination_mask & Bit(27U)) != 0ULL,
          run, "mover-first comparison immediately admits the normal legal capture destination");
    Check(s.Advance(100U) == VIRTUAL_LITCH_OK, run, "wait for the adjacent capture destination's reveal to complete");
    const auto legalTarget = AllSquarePixels(s, 27U);
    Check(Observe(s, occupancy) && s.Advance(runtime::SETTLE_AFTERGLOW_MS) == VIRTUAL_LITCH_OK,
          run, "return the mover and let its local restoration cue expire before the victim-first comparison");
    const auto ordinarySource = AllSquarePixels(s, 36U);
    Check(Observe(s, occupancy & ~Bit(27U)), run, "White removes actual d5 victim after two committed compound turns");
    Check(Read(s).capturers == Bit(36U) && s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::CaptureStaged) &&
          s.State().correction_mask == 0ULL && s.UndoHistoryMoveCount() == 2U,
          run, "Duck pure-rules cache exposes only the legal e4 pawn capturer");
    std::array<uint16_t, 2U> low{65535U, 65535U}, high{};
    const auto targets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, 36U, hardware::SquareLedSlot::Piece, Rotated(s));
    for (uint8_t frame = 0U; frame < 10U; ++frame) {
        for (uint8_t target = 0U; target < targets.count; ++target) {
            const uint16_t energy = Energy(s.PhysicalFrame().pixels[targets.indices[target]]);
            if (energy < low[target]) low[target] = energy;
            if (energy > high[target]) high[target] = energy;
        }
        const auto socket = AllSquarePixels(s, 27U);
        Check(Energy(socket.semantic[0U]) > 0U && Energy(socket.semantic[2U]) > 0U &&
              Energy(socket.mapped[0U]) > 0U && Energy(socket.mapped[3U]) > 0U &&
              Energy(legalTarget.mapped[0U]) > 0U && EmptyPieceTargets(s, 27U), run,
              "Duck victim-first travel reaches both visible side rails while the unchosen Piece pair stays black");
        Checkpoint(s, run, "Duck-victim-first-Coaching-off", "lift-e4-pawn-or-restore-d5", "accepted", Bit(27U) | Bit(36U));
        if (frame != 9U) Check(s.Advance(100U) == VIRTUAL_LITCH_OK, run, "sample Duck capture-source pulse");
    }
    for (uint8_t target = 0U; target < targets.count; ++target)
        Check(low[target] > 0U && high[target] > low[target], run, "Coaching-off Duck capturer pulses on every actual V3 piece emitter");
    // e4 is the prior White move destination, so its ordinary piece and
    // side-field pulse must continue. Match that existing 2200 ms phase to
    // the pre-capture baseline after the 900 ms capture-pulse samples above.
    Check(s.Advance(1300U) == VIRTUAL_LITCH_OK, run, "retain staged guidance through a full last-move cycle");
    Check(Observe(s, occupancy) && Read(s).capturers == 0ULL && s.UndoHistoryMoveCount() == 2U &&
          SameSquarePixels(AllSquarePixels(s, 36U), ordinarySource),
          run, "all V3 capture-source emitters recover the same ordinary last-move presentation");
    Checkpoint(s, run, "Duck-victim-restored", "choose-any-legal-White-move", "accepted", Bit(27U) | Bit(36U));
}

bool WaitDone(modules::AsyncThinkWorker& worker) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (worker.IsActive() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return !worker.IsActive();
}

void NoThinkingAnimationJourney(const bool fittedControls) {
    const char* run = fittedControls ? "HintThinkingNone/V3.3-real-worker-controls-and-result"
                                    : "HintThinkingNone/V3-real-worker-controls-and-result";
    auto config = Config();
    config.hardware = fittedControls ? VIRTUAL_LITCH_HARDWARE_V3_3
                                     : VIRTUAL_LITCH_HARDWARE_V3;
    const auto topology = hardware::BoardLedTopologyFor(fittedControls
        ? hardware::BoardRevision::V3_3S3 : hardware::BoardRevision::V3S3);
    PublicationEngine engine, ordinaryEngine;
    engine.blockPoll = ordinaryEngine.blockPoll = true;
    modules::AsyncThinkWorker worker, ordinaryWorker;
    runtime::VirtualLitchSession s(config, engine, worker);
    runtime::VirtualLitchSession ordinary(config, ordinaryEngine, ordinaryWorker);
    const auto ready = [&](runtime::VirtualLitchSession& board) {
        return board.Reset() == VIRTUAL_LITCH_OK && Observe(board, OPENING) &&
            board.Advance(200U) == VIRTUAL_LITCH_OK &&
            board.PhysicalFrame().led_count == topology.physicalBoardLedCount +
                topology.physicalStatusLedCount;
    };
    const auto checkpoint = [&](const char* stage, const char* next) {
        const auto& state = s.State();
        const auto display = s.DisplayText();
        std::printf("[HINT_NONE_JOURNEY] run=%s stage=%s t=%u phase=%s action=%u token=%u "
                    "occupancy=%016llx expected=%016llx requested=%u worker=%u cpu_cue=%u "
                    "outcome=accepted display=\"%.*s\" next=%s expiry=existing-search-owner bank=",
                    run, stage, state.now_ms,
                    runtime::ChessInteractionPhaseToString(static_cast<runtime::ChessInteractionPhase>(state.interaction_phase)),
                    state.required_action, state.pending_action_token,
                    static_cast<unsigned long long>(state.physical_occupied_mask),
                    static_cast<unsigned long long>(state.expected_occupied_mask),
                    s.HintDisplayRequested(), worker.IsActive(), s.CpuThinkingVisible(),
                    static_cast<int>(display.size()), display.data(), next);
        const uint8_t count = fittedControls ? 6U : 4U;
        for (uint8_t role = 0U; role < count; ++role) {
            const uint8_t index = fittedControls
                ? hardware::V3_3PhysicalIndexForControl(Owner(s) == modules::UiOwner::P2,
                    static_cast<hardware::ControlLedRole>(role))
                : modules::PlayerHomeLedIndex(Owner(s), static_cast<uint8_t>(role + 2U));
            const auto color = s.PhysicalFrame().pixels[topology.physicalBoardLedCount + index];
            std::printf("%u:%u,%u,%u,%u;", index, color.r, color.g, color.b, color.w);
        }
        std::printf(" home=%s\n", s.HomeStateLine().CStr());
    };
    using Role = hardware::ControlLedRole;
    const auto fittedIndex = [&](const Role role) {
        return hardware::V3_3PhysicalIndexForControl(Owner(s) == modules::UiOwner::P2, role);
    };
    const auto sameBank = [&](const uint8_t skipUp = UINT8_MAX, const uint8_t skipDown = UINT8_MAX) {
        for (uint8_t index = 0U; index < topology.physicalStatusLedCount; ++index) {
            if (index == skipUp || index == skipDown) continue;
            if (!Equal(s.PhysicalFrame().pixels[topology.physicalBoardLedCount + index],
                       ordinary.PhysicalFrame().pixels[topology.physicalBoardLedCount + index])) return false;
        }
        return true;
    };
    Check(ready(s) && ready(ordinary) &&
              s.Advance(modules::EdgeLedCueController::SETUP_COMPLETE_DURATION_MS + 200U) == VIRTUAL_LITCH_OK &&
              Align(ordinary, s.State().now_ms), run,
          "ordinary baseline begins after the 1800ms SetupComplete acknowledgement expires");
    checkpoint("setup-ack-expired-ordinary-baseline", "request-Hint");
    Check(Setting(s, "HINT_THINKING_STYLE", "\"none\"") &&
              Setting(ordinary, "HINT_THINKING_STYLE", "\"none\"") && Tap(s),
          run, "the common setting owner accepts None before raw O requests a real Hint");
    uint8_t from = 64U, to = 64U;
    Check(worker.IsActive() && s.HintDisplayRequested() &&
              !s.TryGetHintMoveSquares(from, to) && !engine.cancelled.load(),
          run, "None retains pending Hint and worker ownership without inventing a result");
    for (const uint32_t ms : {200U, 800U}) {
        Check(s.Advance(ms) == VIRTUAL_LITCH_OK && Align(ordinary, s.State().now_ms) &&
                  worker.IsActive() && sameBank() && !s.CpuThinkingVisible(),
              run, "pending None keeps the exact ordinary mapped bank across fade and search time");
    }
    checkpoint("None-pending-ordinary-Turn", "wait-or-play-any-legal-move");
    Check(Setting(s, "HINT_THINKING_STYLE", "\"player_clockwise\"") &&
              s.Advance(200U) == VIRTUAL_LITCH_OK && Align(ordinary, s.State().now_ms) &&
              !sameBank() && worker.IsActive() && !engine.cancelled.load(),
          run, "choosing an animation during the same search restores its cue without restarting search");
    checkpoint("animated-style-during-same-search", "choose-None-or-wait");
    Check(Setting(s, "HINT_THINKING_STYLE", "\"none\"") &&
              s.Advance(20U) == VIRTUAL_LITCH_OK && Align(ordinary, s.State().now_ms) && sameBank() &&
              worker.IsActive() && !engine.cancelled.load(),
          run, "switching back to None releases cosmetic ownership while keeping the same worker");
    checkpoint("None-restored-during-same-search", "wait-or-play-any-legal-move");
    Check(Tap(s) && !s.HintDisplayRequested() && worker.IsActive(),
          run, "raw O hides the request and leaves background analysis running");
    if (fittedControls) {
        Check(s.Advance(75U)==VIRTUAL_LITCH_OK,run,"sample the accepted quick brew at its actual150ms midpoint");
        for (uint8_t role = 0U; role < 6U; ++role) {
            const auto index = hardware::V3_3PhysicalIndexForControl(Owner(s) == modules::UiOwner::P2,
                static_cast<hardware::ControlLedRole>(role));
            const auto color = s.PhysicalFrame().pixels[topology.physicalBoardLedCount + index];
            Check(role == 0U ? Energy(color) > 0U : Energy(color) == 0U, run,
                  "accepted O input has one finite O acknowledgment without claiming thinking directions");
        }
        checkpoint("None-hide-input-ack", "wait-for-input-ack-expiry-or-play");
        Check(s.Advance(modules::DEFAULT_CONTROL_LED_FEEDBACK.actionMs-75U) == VIRTUAL_LITCH_OK,
              run, "sample the exact150ms input acknowledgment expiry");
    }
    Check(Align(ordinary, s.State().now_ms) && sameBank() && worker.IsActive(), run,
          "None restores the exact ordinary mapped bank after input acknowledgment, with worker unchanged");
    checkpoint("None-request-hidden", "play-or-request-Hint-again");
    Check(Tap(s) && s.HintDisplayRequested(), run, "a fresh raw O request remains available with None");
    engine.Choose(52U, 36U);
    engine.finish = true;
    Check(WaitDone(worker) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
              Read(s).completed && s.TryGetHintMoveSquares(from, to) && from == 52U && to == 36U,
          run, "a real completed worker result still reveals e2e4 with None selected");
    checkpoint("None-Hint-result-visible", "consider-e2e4-or-play-another-move");

    const uint8_t cpuUp = fittedControls ? fittedIndex(Role::Up) : UINT8_MAX;
    const uint8_t cpuDown = fittedControls ? fittedIndex(Role::Down) : UINT8_MAX;
    const auto fittedPixel = [&](const Role role) {
        return s.PhysicalFrame().pixels[topology.physicalBoardLedCount + fittedIndex(role)];
    };
    const auto hasSeatPair = [&]() {
        return Energy(fittedPixel(Role::Up)) > 0U && Equal(fittedPixel(Role::Up), fittedPixel(Role::Down));
    };
    Check(Setting(s, "VS_CPU_PLAYER", "1") && Setting(s, "VS_CPU_LEVEL", "3") &&
              s.Advance(200U) == VIRTUAL_LITCH_OK && worker.IsActive() &&
              !s.CpuThinkingVisible() && !s.TryGetHintMoveSquares(from, to) &&
              Align(ordinary, s.State().now_ms) && sameBank(cpuUp, cpuDown) &&
              (!fittedControls || hasSeatPair()),
          run, "None keeps ordinary Turn and only the fitted CPU seat pair during real search");
    checkpoint("None-CPU-pending", "wait-for-CPU-selection");
    const auto physicalOwner = Owner(s) == modules::UiOwner::P2
        ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    Check(s.SetButtonDown(physicalOwner, runtime::VirtualLitchPhysicalButton::Up, true) == VIRTUAL_LITCH_OK &&
              s.Advance(20U) == VIRTUAL_LITCH_OK, run, "physical Up still edits the palette during None");
    const auto previewIndex = [&](const uint8_t role) {
        return fittedControls
            ? hardware::V3_3PhysicalIndexForControl(Owner(s) == modules::UiOwner::P2,
                static_cast<hardware::ControlLedRole>(role))
            : modules::PlayerHomeLedIndex(Owner(s), static_cast<uint8_t>(role + 2U));
    };
    bool previewVisible = true;
    if (fittedControls) {
        for (uint8_t role = 0U; role < 6U; ++role) {
            const auto control = static_cast<Role>(role);
            const auto color = fittedPixel(control);
            const bool cpuSeatDirection = control == Role::Up || control == Role::Down;
            previewVisible = previewVisible && (cpuSeatDirection
                ? color.b > color.r && color.b > color.g && color.w == 0U : Energy(color) == 0U);
        }
        previewVisible = previewVisible && !Equal(fittedPixel(Role::Up), fittedPixel(Role::Down));
    } else {
        const auto preview = s.PhysicalFrame().pixels[topology.physicalBoardLedCount + previewIndex(0U)];
        previewVisible = Energy(preview) != 0U;
        for (uint8_t role = 1U; role < 4U; ++role)
            previewVisible = previewVisible && Equal(preview,
                s.PhysicalFrame().pixels[topology.physicalBoardLedCount + previewIndex(role)]);
    }
    Check(previewVisible && worker.IsActive(), run,
          "None preserves the fitted Up acknowledgement over the CPU seat pair or the older V3 four-pixel preview");
    checkpoint("None-palette-preview", "release-Up");
    Check(s.SetButtonDown(physicalOwner, runtime::VirtualLitchPhysicalButton::Up, false) == VIRTUAL_LITCH_OK &&
              s.Advance(1000U) == VIRTUAL_LITCH_OK && worker.IsActive() && !s.CpuThinkingVisible() &&
              (!fittedControls || hasSeatPair()),
          run, "palette release recovers ordinary output without changing CPU ownership");
    engine.finishCpu = true;
    Check(WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK &&
              !s.TryGetHintMoveSquares(from, to), run,
          "CPU selection completes without restoring the retired human Hint");
    checkpoint("None-CPU-selection-complete", "follow-the-normal-CPU-move-cue");
    ordinaryEngine.finish = true;
    Check(WaitDone(ordinaryWorker), run, "the comparison background worker also retires cleanly");
}

void BeachballResultJourney(const bool cpu, const uint16_t brightness = 256U,
                           const uint16_t currentLimit = 0U) {
    char run[140];
    std::snprintf(run,sizeof(run),"Beachball/V3.3/%s/brightness-%u/cap-%u",
        cpu ? "CPU-pending-to-directed-move" : "raw-Hint-pending-to-completed-guidance", brightness,currentLimit);
    const bool normal = brightness == 256U && currentLimit == 0U;
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    config.brightness_scale_256 = brightness; config.max_led_current_ma = currentLimit;
    PublicationEngine engine; engine.blockPoll = true;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession s(config, engine, worker);
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
              s.Advance(modules::EdgeLedCueController::SETUP_COMPLETE_DURATION_MS + 200U) == VIRTUAL_LITCH_OK &&
              s.PhysicalFrame().led_count == 300U && worker.IsActive(),
          run, "raw opening occupancy starts the real engine after setup acknowledgement");
    modules::ControlMutationOutcome styleOutcome;
    Check(s.MutateSettingJson(91U, "HINT_THINKING_STYLE", "\"player_clockwise\"", styleOutcome) == VIRTUAL_LITCH_OK &&
              styleOutcome.status == modules::ControlMutationStatus::Applied &&
              s.MutateSettingJson(92U, "HINT_THINKING_STYLE", "\"beachball\"", styleOutcome) == VIRTUAL_LITCH_OK &&
              styleOutcome.status == modules::ControlMutationStatus::Applied && worker.IsActive() &&
              std::strcmp(s.SettingsForPersistenceTest().ReadValue("HINT_THINKING_STYLE", ""), "beachball") == 0,
          run, "existing acknowledged setting owner applies Beachball from a saved alternative during the same search");
    const auto ordinaryTarget = AllSquarePixels(s, 36U);
    constexpr uint8_t neighbors[]{28U, 35U, 37U, 45U};
    std::array<SquarePixels, 4U> ordinaryNeighbors{};
    for (uint8_t i = 0U; i < 4U; ++i) ordinaryNeighbors[i] = AllSquarePixels(s, neighbors[i]);
    if (cpu) {
        engine.finish = true;
        Check(WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK &&
                  Setting(s, "VS_CPU_PLAYER", "1") && Setting(s, "VS_CPU_LEVEL", "3") &&
                  s.Advance(200U) == VIRTUAL_LITCH_OK && worker.IsActive() && s.CpuThinkingVisible(),
              run, "completed quiet background work hands over to actual owned CPU search");
    } else {
        Check(Tap(s) && s.HintDisplayRequested(), run, "physical O debounce/release requests visible Hint");
        modules::ControlMutationOutcome outcome;
        Check(s.TryGetLastBoardControlOutcome(outcome) && outcome.status == modules::ControlMutationStatus::Applied,
              run, "the physical Hint request has its real applied acknowledgement");
    }
    const bool physicalP2 = Owner(s) == modules::UiOwner::P2;
    constexpr hardware::ControlLedRole roles[]{hardware::ControlLedRole::Up, hardware::ControlLedRole::Right,
        hardware::ControlLedRole::Down, hardware::ControlLedRole::Left};
    const auto readBank = [&]() {
        std::array<virtual_litch_color, 4U> colors{};
        for (uint8_t i = 0U; i < 4U; ++i)
            colors[i] = s.PhysicalFrame().pixels[256U + hardware::V3_3PhysicalIndexForControl(physicalP2, roles[i])];
        return colors;
    };
    const auto checkpoint = [&](const char* stage, const char* next) {
        Checkpoint(s, run, stage, next, "accepted", Bit(52U) | Bit(36U));
        std::printf("[BEACHBALL_RGBW] run=%s stage=%s bank=%u style=%s worker=%u controls=",
                    run, stage, physicalP2 ? 2U : 1U,
                    s.SettingsForPersistenceTest().ReadValue("HINT_THINKING_STYLE", ""), worker.IsActive());
        for (uint8_t role = 0U; role < 6U; ++role) {
            const auto index = hardware::V3_3PhysicalIndexForControl(physicalP2, static_cast<hardware::ControlLedRole>(role));
            const auto color = s.PhysicalFrame().pixels[256U + index];
            std::printf("%u:%u,%u,%u,%u;", index, color.r, color.g, color.b, color.w);
        }
        std::putchar('\n');
        const auto colors=readBank(); uint8_t visible=0U,distinct=0U; uint16_t total=0U;
        for(uint8_t x=0U;x<4U;++x){
            visible+=Energy(colors[x])>0U;total+=Energy(colors[x]);bool unique=true;
            for(uint8_t y=0U;y<x;++y)unique=unique && !Equal(colors[x],colors[y]);
            distinct+=unique;
        }
        const auto& frame=s.PhysicalFrame();
        std::printf("[BEACHBALL_OUTPUT] run=%s stage=%s visible=%u unique_rgb=%u total=%u scale=%u limited=%u current=%u\n",
            run,stage,visible,distinct,total,frame.applied_brightness_scale_65535,frame.power_limited,frame.applied_led_current_ma);
    };
    Check(s.Advance(250U) == VIRTUAL_LITCH_OK && worker.IsActive(), run, "pending Beachball reaches its existing fade strength");
    int minHueDistance=1530; uint16_t quantizedPairs=0U;
    std::array<virtual_litch_color,4U> nearestHues{};
    const auto checkBank = [&](const std::array<virtual_litch_color,4U>& colors) {
        uint8_t head=0U;for(uint8_t i=1U;i<4U;++i)if(Energy(colors[i])>Energy(colors[head]))head=i;
        uint16_t total=0U; for (const auto color : colors) total+=Energy(color);
        Check(!normal || (total>=170U && total<=183U),run,
              "mapped normal Beachball has the reduced one-eighth tail envelope, leaving its moving head dominant");
        Check(Energy(colors[head])>0U && (!currentLimit ||
            s.PhysicalFrame().applied_led_current_ma<=currentLimit),run,
            "the actual primary head survives output scaling without bypassing the hard current cap");
        for(uint8_t x=0U;x<4U;++x){
            Check(Energy(colors[x])>0U && (!normal || 8U*Energy(colors[x])+9U>=Energy(colors[head])),run,
                  "all four mapped positions stay nonzero; normal one-eighth floor allows integer rounding");
            for(uint8_t y=x+1U;y<4U;++y){
                const int ea=Energy(colors[x]),eb=Energy(colors[y]);
                if(ea==0 || eb==0)continue; // The nonzero check above already reports the failed emitter.
                const int distance=std::abs(int(colors[x].r)*255/ea-int(colors[y].r)*255/eb)+
                    std::abs(int(colors[x].g)*255/ea-int(colors[y].g)*255/eb)+
                    std::abs(int(colors[x].b)*255/ea-int(colors[y].b)*255/eb);
                if(distance<minHueDistance){minHueDistance=distance;nearestHues=colors;}
                if(!normal && distance<=80)++quantizedPairs;
                // At very low output, integer quantization can merge hues.
                // Characterize that limit; the semantic four-hue recipe and
                // normal mapped separation remain strict tests above/below.
                Check(!normal || distance>80,run,"normal mapped D-pad simultaneously shows four distinct rotating hues");
            }
        }
        uint8_t headMask=0U;
        for(uint8_t i=0U;i<4U;++i)if(Energy(colors[i])*4U>=Energy(colors[head])*3U)headMask|=1U<<i;
        return headMask;
    };
    const auto first = readBank();
    uint8_t head = checkBank(first);
    const uint8_t firstHead = head;
    const uint32_t starts = engine.starts;
    checkpoint("pending-four-hue-head", "wait-for-current-search-or-cancel");
    for (uint8_t step = 0U; step < 4U; ++step) {
        Check(s.Advance(500U) == VIRTUAL_LITCH_OK && engine.starts == starts && worker.IsActive(),
              run, "Beachball timing retains the same real search owner");
        const auto colors = readBank();
        const uint8_t next = checkBank(colors);
        Check(!normal || next == static_cast<uint8_t>(((head<<1U)|(head>>3U))&0x0fU),run,
              "the shared adjacent bright-head positions rotate clockwise through each500ms handoff");
        head = next;
        checkpoint("pending-clockwise-500ms", "wait-for-current-search-or-cancel");
    }
    bool repeats=head==firstHead;
    for(uint8_t i=0U;i<4U;++i)repeats=repeats && Equal(readBank()[i],first[i]);
    Check(!normal || repeats,run,"all four physical colors and the crossfaded head repeat after two seconds without phase restart");
    const auto beforeColors=readBank();
    Check(s.Advance(20U) == VIRTUAL_LITCH_OK && engine.starts == starts && worker.IsActive(),
          run, "spectrum reaches the next existing20ms output frame");
    const auto colors = readBank();
    (void)checkBank(colors);
    const auto smooth = [](uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) <= 24U; };
    for(uint8_t i=0U;i<4U;++i)Check(smooth(beforeColors[i].r,colors[i].r) && smooth(beforeColors[i].g,colors[i].g) &&
              smooth(beforeColors[i].b,colors[i].b) && smooth(beforeColors[i].w,colors[i].w),run,
          "each actual physical LED changes gently between output frames while the head crosses positions");
    for(uint8_t sample=0U;sample<100U;++sample){
        Check(s.Advance(20U)==VIRTUAL_LITCH_OK && worker.IsActive() && engine.starts==starts,run,
            "one full physical output cycle retains the same pending search owner");
        (void)checkBank(readBank());
    }
    std::printf("[BEACHBALL_OUTPUT_FIDELITY] run=%s min_hue_distance=%d quantized_pairs=%u nearest=",run,minHueDistance,quantizedPairs);
    for(const auto color:nearestHues)std::printf("%u,%u,%u,%u;",color.r,color.g,color.b,color.w);
    std::putchar('\n');
    checkpoint("pending-full-cycle-20ms-qualified","wait-for-current-search-or-cancel");
    engine.Choose(52U, 36U);
    if (cpu) engine.finishCpu = true; else engine.finish = true;
    Check(WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK &&
              s.Advance(250U) == VIRTUAL_LITCH_OK && !worker.IsActive() && !s.CpuThinkingVisible(),
          run, "real completed worker retires Beachball through the existing presentation fade");
    for (const auto color : readBank()) Check(Energy(color) == 0U, run, "completed work leaves no stale D-pad rainbow");
    uint8_t from = 64U, to = 64U;
    Check(cpu ? !s.TryGetHintMoveSquares(from, to)
              : s.TryGetHintMoveSquares(from, to) && from == 52U && to == 36U,
          run, "completion retains the correct CPU or explicit Hint result authority");
    Check(!SameSquarePixels(AllSquarePixels(s, 36U), ordinaryTarget),
          run, "the actual e4 destination emitter carries completed physical move guidance");
    for (uint8_t i = 0U; i < 4U; ++i)
        {
            const auto current=AllSquarePixels(s,neighbors[i]); bool unchanged=true;
            for(uint8_t slot=0U;slot<3U;++slot)unchanged=unchanged && Equal(current.semantic[slot],ordinaryNeighbors[i].semantic[slot]);
            Check(normal ? SameSquarePixels(current,ordinaryNeighbors[i]) : unchanged,
                run,"completed Hint and directed CPU keep nonroute semantics ordinary; hard limiting alone may change physical global gain");
        }
    checkpoint("result-guidance-visible", cpu ? "lift-e2-and-move-to-e4" : "consider-Hint-e2e4-or-play-another-move");
    if (cpu) {
        bool top = false, bottom = false;
        for (uint8_t sample = 0U; sample <= 24U; ++sample) {
            const auto landing = AllSquarePixels(s, 36U);
            Check(Energy(landing.mapped[1U]) > 0U && Energy(landing.mapped[2U]) > 0U, run,
                  "quiet CPU result keeps both actual target piece emitters visible");
            top = top || Energy(landing.mapped[1U]) > Energy(landing.mapped[2U]);
            bottom = bottom || Energy(landing.mapped[2U]) > Energy(landing.mapped[1U]);
            if (sample == 0U || sample == 12U || sample == 24U) checkpoint("CPU-quiet-landing", "lift-e2-and-place-e4");
            if (sample != 24U) Check(s.Advance(50U) == VIRTUAL_LITCH_OK, run, "sample CPU-owned landing cadence");
        }
        Check(top && bottom && !s.HintDisplayRequested(), run,
              "directed CPU landing alternates without requiring a human Hint request");
    }

}

void CompletedHintCpuTakeoverJourney() {
    const char* run = "CompletedHintCpuTakeover/real-worker-and-controls";
    PublicationEngine engine; engine.blockPoll = true; modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession s(Config(), engine, worker);
    Check(Ready(s) && s.Advance(1300U) == VIRTUAL_LITCH_OK, run, "real worker owns background search and CPU idle token is ready");
    Check(Setting(s, "P2_END_PIECE_WHEEL", "85") && Setting(s, "P1_END_PIECE_WHEEL", "0"), run,
          "CPU mover uses an explicit actual red palette distinct from the other player");
    const auto ordinaryTarget = Square(s, 36U);
    engine.Choose(52U, 36U); engine.finish = true;
    Check(WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK && Tap(s), run, "completed worker result is revealed by raw O before takeover");
    uint8_t from = 64U, to = 64U;
    Check(Read(s).completed && s.TryGetHintMoveSquares(from, to) && !Equal(Square(s, 36U), ordinaryTarget),
          run, "completed manual Hint reaches actual board RGBW before CPU eligibility changes");
    Checkpoint(s, run, "completed-Hint-visible", "consider-e2e4");
    Check(Setting(s, "VS_CPU_PLAYER", "1") && Setting(s, "VS_CPU_LEVEL", "3") && s.Advance(1U) == VIRTUAL_LITCH_OK &&
          worker.IsActive() && s.CpuThinkingVisible() && !s.TryGetHintMoveSquares(from, to) && Equal(Square(s, 36U), ordinaryTarget),
          run, "completed-Hint takeover hides board guidance while the real replacement worker is still thinking");
    Check(!s.HomeStateSnapshot().assistance.manualAffordanceUseful, run,
          "CPU takeover keeps Show Move unavailable while owned CPU search starts");
    Checkpoint(s, run, "completed-Hint-CPU-thinking", "wait-for-CPU-selection");
    engine.finishCpu = true;
    Check(WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK && !s.TryGetHintMoveSquares(from, to),
          run, "CPU replacement completes without restoring old human Hint authority");
    Check(s.Advance(700U) == VIRTUAL_LITCH_OK && s.HomeStateSnapshot().physicalMoveValid,
          run, "completed CPU search publishes its production directed plan");
    const auto cpuRoute = AllSquarePixels(s, 44U).semantic[1U];
    Check(cpuRoute.r > 0U && cpuRoute.g == 0U && cpuRoute.b == 0U,
          run, "real CPU intermediate vertical Piece head uses the actual red mover hue");
    Check(Observe(s, OPENING & ~Bit(52U)), run, "raw Hall pickup advances the actual CPU plan");
    const auto heldRoute = AllSquarePixels(s, 44U).semantic[1U];
    Check(heldRoute.r > 0U && heldRoute.g == 0U && heldRoute.b == 0U,
          run, "held CPU intermediate Piece head retains actual mover hue on the same continuing plan clock");
    Checkpoint(s, run, "CPU-held-actual-mover-color", "place-e4", "accepted", Bit(52U)|Bit(44U)|Bit(36U));
}

void CaptureFirstOverlaySuppressionSupportingControl() {
    const char* run = "CaptureFirstWithoutCoaching/overlay-suppression-supporting-renderer";
    std::puts("[CHESS_ASSISTANCE_SEAM] supporting production square composer only; full Session above owns input, legality, acknowledgement and V3 proof.");
    using namespace modules::led_frame_composer::internal;
    modules::led_frame_composer::ComposeInputs inputs;
    runtime::InteractionModelSnapshot contextSnapshot;
    ComposeContext context; context.snapshot = &contextSnapshot; BoardComposeRuntime timing;
    context.in = &inputs;
    contextSnapshot.interaction.taggedState.tag = runtime::ChessInteractionPhase::CaptureStaged;
    contextSnapshot.interaction.captureFirstCapturerMask = Bit(26U);
    contextSnapshot.interaction.taggedState.selectedSquare = 27U;
    context.availablePromptRgb = modules::Rgb{0U, 32U, 64U};
    const auto socketVisible = [&](bool off = false, bool occupied = false) {
        modules::Rgb square{}, piece{}; BoardSquarePromptState prompt;
        ApplySquareStatePrompts(context, timing, off, occupied, 27U, square, piece, prompt);
        return square.r == context.availablePromptRgb.r &&
               square.g == context.availablePromptRgb.g &&
               square.b == context.availablePromptRgb.b;
    };
    const auto scale = [&](bool off = false) {
        modules::Rgb square{}, piece{32U, 64U, 0U}; BoardSquarePromptState prompt;
        ApplySquareStatePrompts(context, timing, off, true, 26U, square, piece, prompt);
        return prompt.pieceFinalScale256;
    };
    Check(scale() < 256U && socketVisible(), run, "eligible source has the existing gentle pulse with Coaching off");
    context.renderIntent.menuForegroundVisible = true;
    Check(scale() == 256U && !socketVisible(), run, "menu foreground suppresses capture-source scaling");
    context.renderIntent.menuForegroundVisible = false;
    context.renderIntent.centerTextOverlayVisible = true;
    Check(scale() == 256U && !socketVisible(), run, "center overlay suppresses capture-source scaling");
    context.renderIntent.centerTextOverlayVisible = false;
    Check(scale(true) == 256U && !socketVisible(true), run, "forced-off square cannot acquire capture-source scaling");
    context.invalidBoard.extraMask = Bit(16U);
    Check(scale() == 256U && !socketVisible(), run, "unrelated board mismatch suppresses source choices and empty socket");
    context.invalidBoard.extraMask = 0ULL;
    Check(!socketVisible(false, true), run, "restored victim does not receive empty-socket guidance");
    contextSnapshot.interaction.captureFirstCapturerMask = 0ULL;
    Check(scale() == 256U && !socketVisible(), run, "ineligible CPU or terminal turn has no capture-staging cue");
}

void DirectedTravelUsesActualMoverPalette() {
    using namespace modules::led_frame_composer::internal;
    const char* run = "DirectedTravel/actual-mover-palette";
    const auto same = [](modules::Rgb a, modules::Rgb b) {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    };
    for (const bool black : {false, true}) {
        for (const bool rotated : {false, true}) {
            for (uint8_t flow = 0U; flow < 3U; ++flow) {
                modules::led_frame_composer::ComposeInputs inputs;
                runtime::InteractionModelSnapshot contextSnapshot;
                ComposeContext context; context.snapshot = &contextSnapshot; context.in = &inputs;
                context.p1PieceRgb = {243U, 17U, 61U};
                context.p2PieceRgb = {13U, 71U, 231U};
                context.activePromptRgb = {113U, 67U, 3U};
                context.availablePromptRgb = {0U, 191U, 23U};
                const auto mover = black ? context.p2PieceRgb : context.p1PieceRgb;
                const uint8_t source = black ? 7U : 56U;
                const uint8_t target = black ? 31U : 32U;
                const uint8_t head = black ? 15U : 48U;
                const uint8_t next = black ? 23U : 40U;
                context.physicalOcc64 = flow == 1U ? 0ULL : Bit(source);
                context.validDestMask = Bit(head) | Bit(next) | Bit(target);
                auto& snapshot = contextSnapshot;
                snapshot.board.enginePiecesValid = true;
                snapshot.board.enginePieces[source] = black ? 'r' : 'R';
                // CPU rendering may follow an advanced engine turn; the mover
                // identity at the directed source still owns this route.
                snapshot.game.sideToMove = black ? modules::SideToMove::White : modules::SideToMove::Black;
                auto& interaction = snapshot.interaction;
                interaction.lastInteractionElapsedMs = 1440U;
                interaction.cpuMovePromptElapsedMs = 1440U;
                context.renderIntent.hintPromptVisible = flow != 2U;
                context.hintMoveValid = flow != 2U;
                context.hintFromSquare = source; context.hintToSquare = target;
                interaction.taggedState.tag = flow == 1U
                    ? runtime::ChessInteractionPhase::PieceLifted
                    : flow == 2U ? runtime::ChessInteractionPhase::ExecutingPlan
                                 : runtime::ChessInteractionPhase::Idle;
                interaction.taggedState.selectedSquare = source;
                if (flow == 2U) {
                    context.cpuTargetOccValid = true;
                    auto& plan = interaction.taggedState.plan;
                    plan.kind = runtime::PhysicalTransitionKind::QuietMove;
                    plan.source = runtime::PhysicalTransitionSource::Directed;
                    plan.relocationCount = 1U; plan.relocations[0U] = {source, target};
                    interaction.taggedState.requiredAction = runtime::RequiredPhysicalAction::LiftPrimary;
                }
                const auto legalBoard = [&]() {
                    modules::BoardLedFrame board{};
                    for (const uint8_t square : {head, next}) {
                        board[square].left = board[square].right = context.availablePromptRgb;
                    }
                    return board;
                };
                auto board = legalBoard();
                ApplyDirectedMoveLanding(context, board);
                const auto targetHead = black ? board[target].bottom : board[target].top;
                Check(same(targetHead, mover), run,
                      "vertical final emitter uses actual source-owner RGB for Hint, held and advanced-turn CPU");
                Check(same(board[head].left, context.availablePromptRgb) &&
                          same(board[head].right, context.availablePromptRgb) &&
                          same(board[next].left, context.availablePromptRgb) &&
                          same(board[next].right, context.availablePromptRgb), run,
                      "vertical Piece travel leaves ordinary legal side rails exact and continuously available");
                const auto physical = hardware::MapSemanticLedFrame(
                    {hardware::BoardRevision::V3_3S3, rotated, rotated}, board,
                    std::array<modules::Rgb, modules::EDGE_LED_COUNT>{});
                const uint8_t position = black ? 3U : 1U;
                const uint16_t index = runtime::MapSquareIndex(target, rotated) * 4U +
                    (rotated ? (position + 2U) % 4U : position);
                Check(same(physical[index], mover), run,
                      "both rotation states map the actual mover's final vertical emitter correctly");
                context.minimalAnimations = true;
                board = legalBoard(); ApplyDirectedMoveLanding(context, board);
                Check(same(board[head].left, context.availablePromptRgb) &&
                          same(board[source].left, modules::RGB_BLACK), run,
                      "Minimal Animations leaves legal rails steady and emits no travel");
                context.minimalAnimations = false;
                context.physicalOcc64 |= Bit(target);
                context.hintCaptureSquare = target;
                board = legalBoard(); board[target].left = board[target].right = modules::RGB_WHITE;
                ApplyDirectedMoveLanding(context, board);
                Check(same(board[target].left, modules::RGB_WHITE) &&
                          same(board[target].right, modules::RGB_WHITE) &&
                          same(board[target].top, modules::RGB_BLACK) &&
                          same(board[target].bottom, modules::RGB_BLACK), run,
                      "occupied WHITE victim and dark Piece LEDs retain removal ownership");
            }
        }
    }
    // The cached future Duck destination may overlap the chess path. Until
    // DuckRelocation owns the turn, it cannot reserve or color that square.
    for(uint8_t stage=0U;stage<3U;++stage) {
        modules::led_frame_composer::ComposeInputs inputs;
        runtime::InteractionModelSnapshot snapshot;
        ComposeContext context;context.snapshot=&snapshot;context.in=&inputs;
        snapshot.game.variant=modules::GameVariant::Duck;
        snapshot.board.enginePiecesValid=true;snapshot.board.enginePieces[52U]='P';
        snapshot.render.hintFeedbackTimingValid=true;snapshot.render.hintFeedbackElapsedMs=480U;
        snapshot.interaction.lastInteractionElapsedMs=480U;
        snapshot.interaction.cpuMovePromptElapsedMs=480U;
        context.p1PieceRgb={243U,17U,61U};context.availablePromptRgb={0U,191U,23U};
        context.hintMoveValid=true;context.hintFromSquare=52U;context.hintToSquare=36U;
        context.hintRefinementAlpha255=255U;context.renderIntent.hintPromptVisible=true;
        context.hintDuckFromSquare=24U;context.hintDuckToSquare=44U;
        context.physicalOcc64=Bit(24U)|(stage==1U?0ULL:Bit(52U));
        auto& interaction=snapshot.interaction.taggedState;
        interaction.tag=stage==0U?runtime::ChessInteractionPhase::Idle:
            stage==1U?runtime::ChessInteractionPhase::PieceLifted:runtime::ChessInteractionPhase::ExecutingPlan;
        interaction.selectedSquare=52U;
        if(stage==2U) {
            context.cpuTargetOccValid=true;
            interaction.requiredAction=runtime::RequiredPhysicalAction::LiftPrimary;
            interaction.plan.kind=runtime::PhysicalTransitionKind::QuietMove;
            interaction.plan.source=runtime::PhysicalTransitionSource::Directed;
            interaction.plan.relocationCount=1U;interaction.plan.relocations[0U]={52U,36U};
        }
        modules::BoardLedFrame chessOnly{},withCachedDuck{};
        context.hintDuckMoveValid=false;ApplyDirectedMoveLanding(context,chessOnly);
        context.hintDuckMoveValid=true;ApplyDirectedMoveLanding(context,withCachedDuck);
        bool unchanged=true;
        for(uint8_t sq=0U;sq<64U;++sq) {
            const auto& a=chessOnly[sq];const auto& b=withCachedDuck[sq];
            unchanged=unchanged && same(a.left,b.left) && same(a.top,b.top) &&
                same(a.right,b.right) && same(a.bottom,b.bottom);
        }
        Check(unchanged && (chessOnly[44U].left.r>0U || chessOnly[44U].right.r>0U),run,
              "idle, held and fixed chess movement ignore the future Duck route even where its target overlaps chess transit");
    }
    // Once DuckRelocation owns the next action, check both fixed CPU and
    // human Hint routes through their actual entry, middle and exit emitters.
    struct DuckDirection {uint8_t from,to,transit,entry,exit,phases;};
    const std::array<DuckDirection,8U> routes{{
        {27U,29U,28U,1U<<0U,1U<<2U,3U},{29U,27U,28U,1U<<2U,1U<<0U,3U},
        {27U,43U,35U,1U<<1U,1U<<3U,3U},{43U,27U,35U,1U<<3U,1U<<1U,3U},
        {27U,45U,36U,(1U<<0U)|(1U<<1U),(1U<<2U)|(1U<<3U),2U},
        {45U,27U,36U,(1U<<2U)|(1U<<3U),(1U<<0U)|(1U<<1U),2U},
        {29U,43U,36U,(1U<<1U)|(1U<<2U),(1U<<0U)|(1U<<3U),2U},
        {43U,29U,36U,(1U<<0U)|(1U<<3U),(1U<<1U)|(1U<<2U),2U}
    }};
    for(const auto& route:routes)for(const bool fixed:{false,true})for(const bool rotated:{false,true}) {
        modules::led_frame_composer::ComposeInputs inputs;
        runtime::InteractionModelSnapshot snapshot;
        ComposeContext context;context.snapshot=&snapshot;context.in=&inputs;
        snapshot.game.variant=modules::GameVariant::Duck;
        auto& interaction=snapshot.interaction.taggedState;
        interaction.tag=runtime::ChessInteractionPhase::DuckRelocation;
        interaction.requiredAction=runtime::RequiredPhysicalAction::PlacePrimary;
        interaction.plan.kind=runtime::PhysicalTransitionKind::DuckRelocation;
        interaction.plan.source=fixed?runtime::PhysicalTransitionSource::Cpu:runtime::PhysicalTransitionSource::Human;
        interaction.plan.relocationCount=1U;interaction.plan.moveFrom=route.from;
        interaction.plan.relocations[0U]={route.from,fixed?route.to:runtime::CHESS_INTERACTION_INVALID_SQUARE};
        context.renderIntent.hintPromptVisible=true;context.hintMoveValid=true;
        context.hintFromSquare=route.from;context.hintToSquare=route.to;
        context.hintRefinementAlpha255=255U;
        snapshot.render.hintFeedbackTimingValid=true;
        const auto render=[&](uint32_t age) {
            snapshot.render.hintFeedbackElapsedMs=age;snapshot.interaction.lastInteractionElapsedMs=age;
            modules::BoardLedFrame board{};ApplyDirectedMoveLanding(context,board);return board;
        };
        for(uint8_t phase=0U;phase<(route.phases==3U?1U:2U);++phase) {
            const uint32_t age=route.phases==3U?480U:phase==0U?384U:576U;
            const uint8_t mask=route.phases==3U?static_cast<uint8_t>(15U & ~(route.entry|route.exit)):
                phase==0U?route.entry:route.exit;
            const auto board=render(age);const auto& cell=board[route.transit];
            const std::array<modules::Rgb,4U> logical{{cell.left,cell.top,cell.right,cell.bottom}};
            const auto physical=hardware::MapSemanticLedFrame(
                {hardware::BoardRevision::V3_3S3,rotated,rotated},board,
                std::array<modules::Rgb,modules::EDGE_LED_COUNT>{});
            for(uint8_t pos=0U;pos<4U;++pos) {
                const uint16_t index=runtime::MapSquareIndex(route.transit,rotated)*4U+
                    (rotated?(pos+2U)%4U:pos);
                Check(same(physical[index],logical[pos]),run,
                      "actual Duck-phase directional emitters map through physical 180-degree rotation");
                if(mask&(1U<<pos)) {
                    Check(logical[pos].r>150U && logical[pos].r==logical[pos].g && logical[pos].b==0U,run,
                          "Duck-phase paired diagonal or cardinal-middle emitters lead in yellow");
                    for(uint8_t other=0U;other<4U;++other) if(!(mask&(1U<<other)))
                        Check(int(logical[pos].r)>int(logical[other].r)+20,run,
                              "Duck-phase head visibly leads its opposite directional emitters");
                }
            }
        }
        context.minimalAnimations=true;
        const auto initial=render(0U),later=render(1200U);
        const auto& a=initial[route.transit];const auto& b=later[route.transit];
        Check(same(a.left,b.left)&&same(a.top,b.top)&&same(a.right,b.right)&&same(a.bottom,b.bottom) &&
            a.left.r==0U && a.top.r==0U && a.right.r==0U && a.bottom.r==0U &&
            initial[route.to].top.r>0U && same(initial[route.to].top,later[route.to].top),run,
            "Minimal retains the fixed Duck landing but suppresses its moving route");
    }
    // A 2:1 Duck line crosses a half-cell tie. Reverse traversal must visit
    // the same logical cell in the real Duck phase, including fixed CPU moves.
    for(const bool reverse:{false,true})for(const bool fixed:{false,true})for(const bool rotated:{false,true}) {
        modules::led_frame_composer::ComposeInputs inputs;runtime::InteractionModelSnapshot snapshot;
        ComposeContext context;context.snapshot=&snapshot;context.in=&inputs;
        snapshot.game.variant=modules::GameVariant::Duck;
        auto& interaction=snapshot.interaction.taggedState;
        interaction.tag=runtime::ChessInteractionPhase::DuckRelocation;
        interaction.requiredAction=runtime::RequiredPhysicalAction::PlacePrimary;
        interaction.plan.kind=runtime::PhysicalTransitionKind::DuckRelocation;
        interaction.plan.source=fixed?runtime::PhysicalTransitionSource::Cpu:runtime::PhysicalTransitionSource::Human;
        interaction.plan.relocationCount=1U;
        const uint8_t source=reverse?41U:24U,target=reverse?24U:41U;
        interaction.plan.moveFrom=source;
        interaction.plan.relocations[0U]={source,fixed?target:runtime::CHESS_INTERACTION_INVALID_SQUARE};
        snapshot.render.hintFeedbackTimingValid=true;snapshot.render.hintFeedbackElapsedMs=480U;
        snapshot.interaction.lastInteractionElapsedMs=480U;
        context.renderIntent.hintPromptVisible=true;context.hintMoveValid=true;
        context.hintFromSquare=source;context.hintToSquare=target;context.hintRefinementAlpha255=255U;
        modules::BoardLedFrame board{};ApplyDirectedMoveLanding(context,board);
        const auto physical=hardware::MapSemanticLedFrame(
            {hardware::BoardRevision::V3_3S3,rotated,rotated},board,
            std::array<modules::Rgb,modules::EDGE_LED_COUNT>{});
        const auto lit=[](modules::Rgb c){return c.r!=0U || c.g!=0U || c.b!=0U;};
        const auto& chosen=board[32U];const auto& other=board[33U];
        Check((lit(chosen.left)||lit(chosen.top)||lit(chosen.right)||lit(chosen.bottom)) &&
              !lit(other.left)&&!lit(other.top)&&!lit(other.right)&&!lit(other.bottom),run,
              "both directions of the 2:1 Duck-phase tie cross the same even-color cell");
        const std::array<modules::Rgb,4U> logical{{chosen.left,chosen.top,chosen.right,chosen.bottom}};
        for(uint8_t position=0U;position<4U;++position){
            const uint16_t index=runtime::MapSquareIndex(32U,rotated)*4U+
                (rotated?(position+2U)%4U:position);
            Check(same(physical[index],logical[position]),run,
                  "2:1 Duck-phase tie maps each selected emitter through physical rotation");
        }
    }

}

void RemovalFadeRefinementSupportingControl(){
    using namespace modules::led_frame_composer::internal;
    const char* run="CaptureWhiteFade/supporting-renderer/refinement-priority-wrap";
    modules::led_frame_composer::ComposeInputs inputs;
    runtime::InteractionModelSnapshot contextSnapshot;
    ComposeContext context; context.snapshot = &contextSnapshot;context.in=&inputs;
    context.physicalOcc64=Bit(27U);context.renderIntent.hintPromptVisible=true;context.hintMoveValid=true;
    context.hintCaptureSquare=27U;
    for(const uint8_t alpha:{1U,128U,255U})for(const uint32_t age:{0U,1U,119U,120U,239U,240U,359U,360U,1199U,1200U,UINT32_MAX-1U,UINT32_MAX}){
        context.hintRefinementAlpha255=alpha;contextSnapshot.render.hintFeedbackTimingValid=true;
        contextSnapshot.render.hintFeedbackElapsedMs=age;
        modules::BoardLedFrame board{};board[27U].left=modules::RGB_RED;board[27U].right=modules::RGB_BLUE;
        modules::SetPiecePair(board[27U],modules::RGB_RED);
        const uint64_t tapMask=ApplyPhysicalRemovalBrackets(context,board);ApplyGlobalBrightnessBoost(board);
        const auto left=board[27U].left,right=board[27U].right;
        const auto close=[](int x,int y){return std::abs(x-y)<=3;};
        const uint16_t phase=age%1200U;
        const bool dim=phase>=360U || (phase>=120U && phase<240U);
        const int expectedR=(dim?63:255)*int(alpha)/255;
        const int expectedG=(dim?25:101)*int(alpha)/255;
        Check(left.r==right.r && left.g==right.g && left.b==right.b && close(left.r,expectedR) && close(left.g,expectedG) && left.b==0U &&
            modules::RoundedPieceAverage(board[27U]).r==0U && PhysicalRemovalSquareMask(context)==Bit(27U) &&
            tapMask==(dim?0ULL:Bit(27U)),run,
            "refinement scales paired amber strikes and dim rest while retaining persistent ownership and strike-only accent across clock wrap");
    }
    context.minimalAnimations=true;context.hintRefinementAlpha255=255U;
    contextSnapshot.render.hintFeedbackElapsedMs=0U;
    modules::BoardLedFrame minimal{};
    const uint64_t minimalTapMask=ApplyPhysicalRemovalBrackets(context,minimal);
    ApplyGlobalBrightnessBoost(minimal);
    Check(minimalTapMask==0ULL && PhysicalRemovalSquareMask(context)==Bit(27U) &&
        minimal[27U].left.r==255U && std::abs(int(minimal[27U].left.g)-85)<=2 &&
        minimal[27U].left.b==0U && minimal[27U].right.r==minimal[27U].left.r &&
        minimal[27U].right.g==minimal[27U].left.g,run,
        "Minimal retains ordinary steady orange ownership and never requests the strike accent");
    context.minimalAnimations=false;context.undoPromptActive=true;modules::BoardLedFrame restored{};
    restored[27U].left=modules::RGB_RED;restored[27U].right=modules::RGB_BLUE;modules::SetPiecePair(restored[27U],modules::RGB_GREEN);
    const uint64_t restoredTapMask=ApplyPhysicalRemovalBrackets(context,restored);
    Check(restoredTapMask==0ULL && PhysicalRemovalSquareMask(context)==0ULL && restored[27U].left.r==255U && restored[27U].right.b==255U &&
        restored[27U].top.g==255U && restored[27U].bottom.g==255U,run,
        "Undo victim restoration preserves its own return colors and Piece identity, never WHITE removal");
}

void EqualPaletteHeldLocatorSupportingControl() {
    const char* run = "DirectedLanding/equal-palette-and-priority-supporting-composer";
    std::puts("[CHESS_ASSISTANCE_SEAM] isolated composer checks equal palettes and precedence; Session journeys own legality, Hall, acknowledgement and V3.3 output.");
    using namespace modules::led_frame_composer::internal;
    modules::led_frame_composer::ComposeInputs inputs;
    runtime::InteractionModelSnapshot contextSnapshot;
    ComposeContext context; context.snapshot = &contextSnapshot; context.in = &inputs;
    context.renderIntent.hintPromptVisible = true; context.hintMoveValid = true;
    context.hintFromSquare = 52U; context.hintToSquare = 36U;
    context.validDestMask = Bit(44U) | Bit(36U);
    context.physicalOcc64 = OPENING & ~Bit(52U);
    contextSnapshot.interaction.taggedState.tag = runtime::ChessInteractionPhase::PieceLifted;
    contextSnapshot.interaction.taggedState.selectedSquare = 52U;
    contextSnapshot.board.enginePiecesValid = true;
    contextSnapshot.board.enginePieces[52U] = 'P';
    context.activePromptRgb = context.availablePromptRgb = modules::Rgb{128U, 64U, 32U};
    modules::BoardLedFrame board{};
    ApplyDirectedMoveLanding(context, board);
    Check(board[36U].top.r > 0U && board[36U].bottom.r > 0U &&
              board[35U].left.r == 0U && board[35U].right.r == 0U,
          run, "equal prompt palettes retain a unique target piece pair without a neighbor cross");
    for (uint8_t owner = 0U; owner < 5U; ++owner) {
        context.renderIntent.menuForegroundVisible = owner == 0U;
        context.undoPromptActive = owner == 1U;
        context.invalidBoard.extraMask = owner == 2U ? Bit(16U) : 0ULL;
        inputs.higherPriorityStatusOverlayActive = owner == 3U;
        contextSnapshot.system.safeModeActive = owner == 4U;
        board = {};
        ApplyDirectedMoveLanding(context, board);
        Check(board[36U].top.r == 0U && board[36U].bottom.r == 0U, run,
              "menu, Undo, correction, system overlay and safety suppress optional landing ownership");
    }
}

void PreflightChargesSearchWindow() {
    const char* run = "HintPreflightTime/production-coordinator";
    std::puts("[CHESS_ASSISTANCE_SEAM] legal query advances ManualMillis to measure admission allowance, without changing legality.");
    for (const uint32_t preparation : {5000U, 90000U}) {
        fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
        fxe::ManualMillis::SetNowMs(1000U);
        modules::SettingsStoreConfig cfg; cfg.path = "build/test_chess_assistance_preflight_time.json";
        (void)std::remove(cfg.path); (void)std::remove("build/test_chess_assistance_preflight_time.json.bak");
        (void)std::remove("build/test_chess_assistance_preflight_time.json.tmp");
        modules::SettingsStore settings(cfg); Check(settings.Load(), run, "bounded test settings load");
        PublicationEngine engine;
        runtime::GameStateMachineConfig smConfig; smConfig.placementStableRequiredMs = 0U; smConfig.cpuIdleDelayMs = 999999U;
        runtime::GameStateMachine sm(smConfig); sm.Reset();
        modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
        Check(coordinator.LoadOrInitialize(), run, "real coordinator initializes before costly preflight");
        runtime::InteractionDispatcher dispatcher(sm, coordinator);
        (void)dispatcher.DispatchBoardObservation({1000U, 1U, OPENING, OPENING});
        engine.legalPreparationMs = preparation;
        coordinator.RequestHintToggle(); coordinator.Update();
        if (preparation < 90000U) {
            Check(engine.starts == 1U && engine.startedMaxThinkMs == 90000U - preparation,
                  run, "search receives only position-window time remaining after successful legality preparation");
        } else {
            Check(engine.starts == 0U && !sm.GetRenderState().hintFeedbackPending,
                  run, "exhausting the position window in legality preparation cannot start search or leave pending feedback");
            fxe::ManualMillis::AdvanceMs(4000U); coordinator.Update();
            Check(engine.starts == 0U, run, "exhausted preparation cannot restart the same-position window");
        }
        std::printf("[CHESS_ASSISTANCE_PREFLIGHT_TIME] preparation=%u starts=%u budget=%u now=%u pending=%u\n",
                    preparation, engine.starts, engine.startedMaxThinkMs, fxe::Millis(), sm.GetRenderState().hintFeedbackPending);
    }
}

void FailedPreflightDeadlineAndWrap() {
    const char* run = "HintPreflightFailure/production-coordinator-wrap";
    std::puts("[CHESS_ASSISTANCE_SEAM] wrap uses production coordinator/dispatcher with ManualMillis; VirtualLitch intentionally rejects time overflow.");
    fxe::ScopedMillisProviderOverride guard(&fxe::ManualMillisProvider);
    constexpr uint32_t START = UINT32_MAX - 45000U;
    fxe::ManualMillis::SetNowMs(START);
    modules::SettingsStoreConfig cfg; cfg.path = "build/test_chess_assistance_feedback_wrap.json";
    (void)std::remove(cfg.path); (void)std::remove("build/test_chess_assistance_feedback_wrap.json.bak");
    (void)std::remove("build/test_chess_assistance_feedback_wrap.json.tmp");
    modules::SettingsStore settings(cfg); Check(settings.Load(), run, "bounded test settings load");
    PublicationEngine engine; engine.failLegal = true;
    runtime::GameStateMachineConfig smConfig; smConfig.placementStableRequiredMs = 0U; smConfig.cpuIdleDelayMs = 999999U;
    runtime::GameStateMachine sm(smConfig); sm.Reset();
    modules::GamePersistenceCoordinator coordinator(settings, engine, sm);
    Check(coordinator.LoadOrInitialize(), run, "real coordinator initialization");
    runtime::InteractionDispatcher dispatcher(sm, coordinator);
    (void)dispatcher.DispatchBoardObservation({START, 1U, OPENING, OPENING});
    coordinator.RequestHintToggle(); coordinator.Update();
    const uint32_t firstReads = engine.legalReads;
    Check(sm.GetRenderState().hintFeedbackPending && engine.starts == 0U, run, "failed real legality seam has pending request without starting mutable search");
    fxe::ManualMillis::AdvanceMs(249U); coordinator.Update();
    Check(engine.legalReads == firstReads, run, "preflight retry honors initial250ms backoff");
    fxe::ManualMillis::AdvanceMs(1U); coordinator.Update();
    Check(engine.legalReads > firstReads, run, "preflight retries after initial backoff");
    for (uint8_t i = 0U; i < 6U; ++i) { fxe::ManualMillis::AdvanceMs(4000U); coordinator.Update(); }
    const uint32_t beforeBackoff = engine.legalReads;
    fxe::ManualMillis::AdvanceMs(3999U); coordinator.Update();
    Check(engine.legalReads == beforeBackoff, run, "repeated preflight failures cap retry backoff at4seconds");
    fxe::ManualMillis::SetNowMs(static_cast<uint32_t>(START + 90000U)); coordinator.Update();
    const uint32_t exhaustedReads = engine.legalReads;
    Check(!sm.GetRenderState().hintFeedbackPending && sm.IsHintDisplayRequested(), run,
          "same-position first attempt expires90seconds across wrap while retaining request ownership");
    fxe::ManualMillis::AdvanceMs(8000U); coordinator.Update();
    Check(engine.legalReads == exhaustedReads && engine.starts == 0U, run, "exhausted preflight stops retries and cannot restart analysis");
    std::printf("[CHESS_ASSISTANCE_PREFLIGHT] started=%u now=%u reads=%u starts=%u pending=%u next=hide-Hint-or-play-a-legal-move\n",
                START, fxe::Millis(), engine.legalReads, engine.starts, sm.GetRenderState().hintFeedbackPending);
}


bool RawMove(runtime::VirtualLitchSession& s, uint64_t& occupancy,
             uint8_t from, uint8_t to, uint32_t settleMs = 2100U) {
    occupancy &= ~Bit(from);
    if (!Observe(s, occupancy)) return false;
    occupancy |= Bit(to);
    return Observe(s, occupancy) && s.Advance(settleMs) == VIRTUAL_LITCH_OK;
}

bool RawUndoTap(runtime::VirtualLitchSession& s) {
    const auto owner = Owner(s, s.State().side_to_move_black != 0U) == modules::UiOwner::P2
        ? VIRTUAL_LITCH_OWNER_P2 : VIRTUAL_LITCH_OWNER_P1;
    return s.SetButtonDown(owner, runtime::VirtualLitchPhysicalButton::X, true) == VIRTUAL_LITCH_OK &&
           s.Advance(20U) == VIRTUAL_LITCH_OK &&
           s.SetButtonDown(owner, runtime::VirtualLitchPhysicalButton::X, false) == VIRTUAL_LITCH_OK &&
           s.Advance(20U) == VIRTUAL_LITCH_OK;
}

// The actual CPU move is acknowledged through the shared reducer before the
// next human Hint is published. Engine choice is controlled; chess legality,
// both Hall inputs, presentation and mapped LED output remain production.
void CpuLastMoveCaptureHintJourney(bool automatic, bool minimal, bool firstP2,
                                  bool rotate, bool sourceFirst, uint16_t brightness, bool v3 = false) {
    char run[160];
    std::snprintf(run, sizeof(run), "CpuLastMoveCaptureHint/%s/%s/%s/P%u/rotate%u/%s/brightness%u",
                  v3 ? "V3" : "V3.3", automatic ? "Coach" : "manual", minimal ? "minimal" : "normal",
                  firstP2 ? 2U : 1U, rotate, sourceFirst ? "mover-first" : "victim-first", brightness);
    auto config = Config(rotate);
    config.hardware = v3 ? VIRTUAL_LITCH_HARDWARE_V3 : VIRTUAL_LITCH_HARDWARE_V3_3;
    config.first_player_is_p2 = firstP2;
    config.brightness_scale_256 = brightness;
    PublicationEngine engine;
    runtime::VirtualLitchSession session(config, engine);
    uint64_t occupied = Bit(4U) | Bit(11U) | Bit(36U) | Bit(60U);
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK &&
        session.LoadExternalFen("4k3/3p4/8/8/4P3/8/8/4K3 b - - 0 1") &&
        Observe(session, occupied) && session.Advance(1000U) == VIRTUAL_LITCH_OK &&
        (!minimal || Setting(session, "MINIMAL_ANIMATIONS", "true")) &&
        (!automatic || Setting(session, "P1_AUTO_HINTS", "true"));
    Check(ready, run, "real position and settings enter the production session");
    if (!ready) return;
    engine.Choose(11U, 27U);
    engine.finishCpu = true;
    engine.finish = true;
    Check(Setting(session, "VS_CPU_PLAYER", "2") && Setting(session, "VS_CPU_LEVEL", "2"),
          run, "Black CPU and controlled legal d7-d5 result are selected");
    for (uint8_t tick = 0U; tick < 80U && !session.HomeStateSnapshot().physicalMoveValid; ++tick)
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK, run, "wait for CPU physical move");
    auto home = session.HomeStateSnapshot();
    const bool cpuReady = home.physicalMoveValid && home.physicalFromSquare == 11U &&
        home.physicalToSquare == 27U;
    Check(cpuReady, run, "CPU directs the pawn from d7 to d5");
    if (!cpuReady) return;
    occupied &= ~Bit(11U);
    Check(Observe(session, occupied), run, "lift CPU pawn from d7");
    occupied |= Bit(27U);
    Check(Observe(session, occupied), run, "place CPU pawn on d5");
    engine.Choose(36U, 27U);
    engine.finish = false;
    for (uint8_t tick = 0U; tick < 40U && session.UndoHistoryMoveCount() != 1U; ++tick)
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK, run, "wait for CPU move acknowledgement");
    const bool committed = session.UndoHistoryMoveCount() == 1U &&
        session.State().side_to_move_black == 0U &&
        std::strstr(session.HomeStateLine().CStr(), "\"m\":\"----d7d5b\"") != nullptr;
    Check(committed, run, "CPU move commits once and remains the last move at d5");
    if (!committed) return;
    if (!automatic) {
        Check(session.Advance(2500U) == VIRTUAL_LITCH_OK && Tap(session), run,
              "a physical Circle tap requests the next human Hint");
    }
    uint8_t from = 64U, to = 64U;
    for (uint8_t tick = 0U; tick < 120U && !session.TryGetHintMoveSquares(from, to); ++tick)
        Check(session.Advance(100U) == VIRTUAL_LITCH_OK, run, "wait for the current human Hint");
    const bool hintReady = session.HintDisplayRequested() && from == 36U && to == 27U &&
        std::strstr(session.HomeStateLine().CStr(), "\"m\":\"----d7d5b\"") != nullptr;
    Check(hintReady, run, "human Hint captures the still-present CPU last-move destination");
    if (!hintReady) return;

    const auto first = AllSquarePixels(session, 27U);
    bool railsAgree = true, victimPieceDark = true, steady = true;
    bool wasBright = false;
    unsigned brightStarts = 0U;
    uint8_t dimmest = 255U, brightest = 0U;
    for (uint16_t sample = 0U; sample <= 120U; ++sample) {
        const auto pixels = AllSquarePixels(session, 27U);
        const auto left = pixels.mapped[0U], right = pixels.mapped[3U];
        railsAgree = railsAgree && Equal(left, right) && left.r > 0U &&
            left.r >= left.g && left.g > 0U && left.b == 0U && left.w == 0U;
        victimPieceDark = victimPieceDark && Energy(pixels.semantic[1U]) == 0U &&
            Energy(pixels.mapped[1U]) == 0U && Energy(pixels.mapped[2U]) == 0U;
        steady = steady && SameSquarePixels(first, pixels);
        const bool bright = pixels.semantic[0U].r > 200U;
        if (bright && !wasBright) ++brightStarts;
        wasBright = bright;
        dimmest = std::min(dimmest, left.r);
        brightest = std::max(brightest, left.r);
        if (sample != 120U) Check(session.Advance(20U) == VIRTUAL_LITCH_OK, run,
                                  "sample actual victim emitters through two full cue cycles");
    }
    Check(railsAgree && victimPieceDark, run,
          "both mapped victim rails agree while both occupied Piece LEDs remain dark");
    Check(minimal ? steady : brightStarts >= 4U && brightest > dimmest, run,
          minimal ? "Minimal keeps the victim's steady removal cue"
                  : "the last-move overlap retains repeated amber strikes and a dim rest");
    std::printf("[CPU_LAST_CAPTURE] run=%s bright_starts=%u mapped_red_min=%u mapped_red_max=%u "
                "victim_piece_dark=%u minimal_steady=%u next=remove-d5-or-lift-e4\n",
                run, brightStarts, dimmest, brightest, victimPieceDark, steady);
    Checkpoint(session, run, "hinted-victim-overlap", "remove-d5-or-lift-e4", "accepted", Bit(11U)|Bit(27U)|Bit(36U));

    const uint8_t firstLift = sourceFirst ? 36U : 27U;
    const uint8_t secondLift = sourceFirst ? 27U : 36U;
    occupied &= ~Bit(firstLift);
    Check(Observe(session, occupied), run, "first capture pickup reaches the real Hall reducer");
    occupied &= ~Bit(secondLift);
    Check(Observe(session, occupied), run, "both capture pickups are observed");
    const auto emptyTarget = AllSquarePixels(session, 27U);
    Check(session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::PlacePrimary) &&
              Energy(emptyTarget.mapped[1U]) > 0U && Energy(emptyTarget.mapped[2U]) > 0U &&
              !Equal(emptyTarget.semantic[0U], {255U,101U,0U,0U}) &&
              !Equal(emptyTarget.semantic[0U], {64U,25U,0U,0U}) &&
              !Equal(emptyTarget.semantic[0U], {255U,85U,0U,0U}), run,
          "removing the victim replaces amber brackets with the empty landing invitation");
    Checkpoint(session, run, "both-pieces-held", "seat-white-pawn-d5", "accepted", Bit(27U)|Bit(36U));
    occupied |= Bit(27U);
    Check(Observe(session, occupied) && Energy(Piece(session, 27U)) > 0U, run,
          "seating the capturing pawn restores its actual Piece light immediately");
    Check(session.Advance(2500U) == VIRTUAL_LITCH_OK && session.UndoHistoryMoveCount() == 2U &&
              session.State().side_to_move_black != 0U &&
              std::string(session.HomeStateSnapshot().fen) == "4k3/8/8/3P4/8/8/8/4K3 b - - 0 2",
          run, "the hinted capture commits exactly once and play advances");
    Checkpoint(session, run, "capture-complete", "Black-to-move", "accepted", Bit(27U)|Bit(36U));
}

void CpuOffTargetAndPendingUndoJourney() {
    const char* run = "CPUOffTarget/V3.3/d7d5-pawn-on-d6-pending-Undo";
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    PublicationEngine engine;
    runtime::VirtualLitchSession s(config, engine);
    uint64_t occupancy = OPENING;
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, occupancy) && s.Advance(500U) == VIRTUAL_LITCH_OK &&
              Setting(s, "WHITE_SQUARES_ON", "false") && Setting(s, "SETTLE_ANIM_STYLE", "\"none\"") &&
              Setting(s, "P1_END_PIECE_WHEEL", "0") && Setting(s, "P2_END_PIECE_WHEEL", "85"),
          run, "actual opening occupancy and distinct end palettes enter production V3.3 play");
    Check(RawMove(s, occupancy, 52U, 36U) && RawMove(s, occupancy, 1U, 18U) &&
              RawMove(s, occupancy, 51U, 35U) && s.UndoHistoryMoveCount() == 3U,
          run, "raw e2e4 b8c6 d2d4 establish the witnessed live position and three committed plies");
    const std::string beforeFen = s.HomeStateSnapshot().fen;
    Check(beforeFen == "r1bqkbnr/pppppppp/2n5/8/3PP3/8/PPP2PPP/RNBQKBNR b KQkq d3 0 2",
          run, "authoritative FEN records the witnessed position and advances the fullmove after Black before CPU pickup");
    engine.Choose(11U, 27U); engine.finish = true;
    Check(Coach(s) && s.HomeStateSnapshot().coachingMask == 2U,
          run, "raw P2 O hold latches the same retained Coach setting seen beside CPU mode");
    // Keep this controlled root through the existing novice selector. Search
    // strength and random-choice quality are outside this physical journey.
    s.RunControlIngress([](void*, modules::SettingsStore&, modules::ControlMutationService&,
                           modules::GamePersistenceCoordinator& coordinator) { coordinator.SetRngSeed(1U); }, nullptr);
    engine.finishCpu = true;
    Check(Setting(s, "VS_CPU_PLAYER", "2") && Setting(s, "VS_CPU_LEVEL", "2") &&
              s.Advance(1300U) == VIRTUAL_LITCH_OK && s.HomeStateSnapshot().physicalMoveValid &&
              s.HomeStateSnapshot().physicalFromSquare == 11U && s.HomeStateSnapshot().physicalToSquare == 27U &&
              !s.HintDisplayRequested(),
          run, "CPU owns directed d7d5 despite the retained P2 Coach toggle");
    const auto ordinaryWhite = Piece(s, 36U), ordinaryBlack = Piece(s, 8U);
    Checkpoint(s, run, "CPU-plan-d7d5", "lift-d7-and-place-d5", "accepted", Bit(11U)|Bit(19U)|Bit(27U));
    const uint64_t beforeOccupancy = occupancy;
    occupancy &= ~Bit(11U); Check(Observe(s, occupancy), run, "raw D7 pickup enters the fixed CPU relocation");
    occupancy |= Bit(19U); Check(Observe(s, occupancy), run, "raw off-target D6 placement is observed without accepting another move");
    for (const uint32_t interval : {200U, 1500U, 5000U}) {
        Check(s.Advance(interval) == VIRTUAL_LITCH_OK, run, "sample tentative correction without promoting it on elapsed time");
        const auto square = Square(s, 19U), piece = Piece(s, 19U);
        Check(square.r > square.g && square.g > square.b && piece.r > piece.g && piece.g > piece.b,
              run, "only the misplaced pawn square carries the existing subdued amber correction");
        Check(std::string(s.HomeStateSnapshot().fen) == beforeFen && s.UndoHistoryMoveCount() == 3U &&
                  s.State().side_to_move_black != 0U && s.State().expected_occupied_mask == beforeOccupancy &&
                  s.State().cpu_illegal_mask == 0ULL && s.HomeStateSnapshot().physicalMoveValid &&
                  s.HomeStateSnapshot().physicalToSquare == 27U,
              run, "D6 does not replace CPU D5, flip turn, append history or promote into a hard error");
        Check(Equal(Piece(s, 36U), ordinaryWhite) && Equal(Piece(s, 8U), ordinaryBlack),
              run, "unrelated White and Black pieces retain their own identities during local amber correction");
    }
    Checkpoint(s, run, "off-target-D6-amber", "lift-amber-d6-and-place-the-CPU-pawn-on-d5", "uncommitted-correction", Bit(11U)|Bit(19U)|Bit(27U));
    occupancy &= ~Bit(19U);
    Check(Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
              s.State().cpu_illegal_mask == 0ULL && s.HomeStateSnapshot().physicalToSquare == 27U,
          run, "lifting the off-target pawn immediately returns to the same CPU destination");
    occupancy |= Bit(11U);
    Check(Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
              s.UndoHistoryMoveCount() == 3U && std::string(s.HomeStateSnapshot().fen) == beforeFen,
          run, "returning D7 cancels physical progress without replacing the CPU plan or history");
    Checkpoint(s, run, "off-target-recovered", "follow-d7d5-or-cancel-the-pending-CPU-move", "accepted", Bit(11U)|Bit(19U)|Bit(27U));
    occupancy &= ~Bit(11U); Check(Observe(s, occupancy), run, "repeat the witnessed CPU pawn pickup");
    occupancy |= Bit(19U); Check(Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK && RawUndoTap(s) &&
              s.Advance(200U) == VIRTUAL_LITCH_OK,
          run, "raw X cancels the pending CPU plan while its pawn is still at D6");
    const std::string undoFen = s.HomeStateSnapshot().fen;
    Check(s.HomeStateSnapshot().undoPending && s.UndoHistoryMoveCount() == 2U &&
              undoFen == "r1bqkbnr/pppppppp/2n5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2" &&
              s.State().physical_occupied_mask == occupancy,
          run, "pending-CPU Undo rewinds the previous White d2d4 first; uncommitted D6 has no history entry");
    Checkpoint(s, run, "pending-CPU-Undo-White-first", "return-white-d4-to-d2-then-correct-black-d6", "accepted", Bit(35U)|Bit(51U)|Bit(19U)|Bit(11U));
    occupancy &= ~Bit(35U); Check(Observe(s, occupancy), run, "raw White D4 pickup follows the actual Undo prompt");
    occupancy |= Bit(51U); Check(Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK,
          run, "White D2 return completes the sole committed Undo before remaining board correction");
    occupancy &= ~Bit(19U); Check(Observe(s, occupancy), run, "raw Black D6 pickup corrects its uncommitted displacement");
    occupancy |= Bit(11U);
    Check(Observe(s, occupancy) && s.Advance(1000U) == VIRTUAL_LITCH_OK && !s.HomeStateSnapshot().undoPending &&
              !s.HomeStateSnapshot().placementMode && s.UndoHistoryMoveCount() == 2U &&
              std::string(s.HomeStateSnapshot().fen) == undoFen && s.State().correction_mask == 0ULL,
          run, "D6-to-D7 correction does not undo Black b8c6 or alter the authoritative two-ply prefix");
    Checkpoint(s, run, "pending-CPU-Undo-corrected", "White-to-move-from-the-restored-position", "accepted", Bit(19U)|Bit(11U)|Bit(35U)|Bit(51U));
}

void HumanCoachAlternateQuietMoveJourney(const char* policy, uint32_t durationMs) {
    const std::string name = std::string("CoachAlternate/V3.3/d7d5-to-d7d6/") + policy;
    const char* run = name.c_str();
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    PublicationEngine engine; engine.blockPoll = true; engine.finish = true;
    modules::AsyncThinkWorker worker;
    runtime::VirtualLitchSession s(config, engine, worker);
    uint64_t occupancy = OPENING;
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, occupancy) && s.Advance(500U) == VIRTUAL_LITCH_OK &&
              Setting(s, "WHITE_SQUARES_ON", "false") && Setting(s, "SETTLE_ANIM_STYLE", "\"none\"") &&
              Setting(s, "P1_END_PIECE_WHEEL", "0") && Setting(s, "P2_END_PIECE_WHEEL", "85"),
          run, "actual V3.3 initial position uses real rules with the production async worker");
    Check(RawMove(s, occupancy, 52U, 36U) && WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK &&
              s.UndoHistoryMoveCount() == 1U && s.State().side_to_move_black != 0U,
          run, "raw White e2e4 reaches the actual Black turn without enabling CPU mode");
    const std::string beforeFen = s.HomeStateSnapshot().fen;
    engine.Choose(11U, 27U); engine.finish = false;
    Check(Coach(s), run, "raw Black O hold enables optional Coach guidance");
    uint8_t from = 64U, to = 64U;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!s.TryGetHintMoveSquares(from, to) || from != 11U || to != 27U) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield(); (void)s.Advance(20U);
    }
    const std::string policyJson = std::string("\"") + policy + "\"";
    Check(s.TryGetHintMoveSquares(from, to) && from == 11U && to == 27U && worker.IsActive() &&
              s.HomeStateSnapshot().coachingMask == 2U && !s.HomeStateSnapshot().physicalMoveValid &&
              Setting(s, "MOVE_CONFIRM", policyJson.c_str()),
          run, "active real worker publishes optional d7d5 while Black remains a human player");
    Checkpoint(s, run, "Coach-d7d5-optional", "choose-d5-or-another-legal-pawn-destination", "accepted", Bit(11U)|Bit(19U)|Bit(27U));
    const auto cancels = engine.cancels;
    occupancy &= ~Bit(11U); Check(Observe(s, occupancy), run, "raw D7 pickup retains optional D5 guidance and both legal choices");
    Check((s.State().valid_destination_mask & (Bit(19U)|Bit(27U))) == (Bit(19U)|Bit(27U)),
          run, "human legality includes both D6 and D5 rather than treating Coach as a fixed CPU plan");
    occupancy |= Bit(19U); Check(Observe(s, occupancy) && s.Advance(100U) == VIRTUAL_LITCH_OK,
          run, "raw alternative D6 placement enters the normal human settle boundary");
    Check(s.UndoHistoryMoveCount() == 1U && std::string(s.HomeStateSnapshot().fen) == beforeFen &&
              s.State().interaction_phase == uint8_t(runtime::ChessInteractionPhase::Settling) &&
              Equal(Piece(s, 19U), Piece(s, 8U)) && EmptyPieceTargets(s, 27U) && Energy(Square(s, 27U)) == 0U,
          run, "before confirmation the pawn stays P2-colored and stale D5 guidance is already invisible");
    Checkpoint(s, run, "human-D6-settling", durationMs == 0U ? "confirm-with-Black-Circle" : "wait-for-the-selected-settle-deadline", "pending", Bit(19U)|Bit(27U));
    if (durationMs == 0U) {
        Check(s.Advance(5200U) == VIRTUAL_LITCH_OK && s.UndoHistoryMoveCount() == 1U && Tap(s),
              run, "Circle mode never auto-commits the different legal destination");
        Check(s.Advance(1100U) == VIRTUAL_LITCH_OK, run, "raw Circle starts the documented one-second finalize settle");
    } else {
        Check(s.Advance(durationMs + 50U) == VIRTUAL_LITCH_OK, run, "the configured timed settle reaches its production commit boundary");
    }
    const auto committedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (s.UndoHistoryMoveCount() != 2U && std::chrono::steady_clock::now() < committedDeadline) {
        std::this_thread::yield(); (void)s.Advance(20U);
    }
    Check(s.UndoHistoryMoveCount() == 2U && s.State().side_to_move_black == 0U &&
              std::string(s.HomeStateSnapshot().fen) == "rnbqkbnr/ppp1pppp/3p4/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
          run, "D7D6 commits once and publishes the actual Black pawn with White to move");
    Check(!s.TryGetHintMoveSquares(from, to) && EmptyPieceTargets(s, 27U) && Energy(Square(s, 27U)) == 0U,
          run, "the previous D5 Hint is cleared rather than forcing or replaying its destination");
    // The normal last-move pulse can add white to the moved piece. Its P2
    // green hue must remain distinct from the configured P1 magenta palette.
    const auto movedPiece = Piece(s, 19U), whitePiece = Piece(s, 36U);
    Check(movedPiece.g > movedPiece.r && movedPiece.g > movedPiece.b &&
              whitePiece.r > whitePiece.g && whitePiece.b > whitePiece.g,
          run, "normal last-move emphasis retains Black/P2 identity instead of adopting the White/P1 palette");
    Check(engine.cancels > cancels && engine.appliedDuringPoll == 0U,
          run, "the real async worker cancels before any actual rules-engine move mutation");
    Checkpoint(s, run, "human-alternative-D6-committed", "White-to-move; Undo-will-return-the-Black-pawn", "accepted", Bit(19U)|Bit(27U)|Bit(11U));
    Check(RawUndoTap(s), run, "raw X requests one human-versus-human Undo");
    const auto undoDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (s.UndoHistoryMoveCount() != 1U && std::chrono::steady_clock::now() < undoDeadline) {
        std::this_thread::yield(); (void)s.Advance(20U);
    }
    Check(s.HomeStateSnapshot().undoPending && s.UndoHistoryMoveCount() == 1U &&
              std::string(s.HomeStateSnapshot().fen) == beforeFen,
          run, "optional Coach alternative owns the latest Black Undo entry rather than skipping to White history");
    occupancy &= ~Bit(19U); Check(Observe(s, occupancy), run, "raw D6 Undo pickup");
    occupancy |= Bit(11U);
    Check(Observe(s, occupancy) && s.Advance(1000U) == VIRTUAL_LITCH_OK && !s.HomeStateSnapshot().undoPending &&
              s.UndoHistoryMoveCount() == 1U && std::string(s.HomeStateSnapshot().fen) == beforeFen &&
              engine.appliedDuringPoll == 0U,
          run, "D6-to-D7 physical Undo retains White e2e4 and never overlaps engine mutation with search");
    Checkpoint(s, run, "human-alternative-Undo-complete", "Black-human-turn-with-optional-Coach", "accepted", Bit(19U)|Bit(11U));
    // End this deliberately blocked publication before destroying its callback
    // owner; background Coach analysis after Undo is expected to be live.
    engine.finish = true;
    Check(WaitDone(worker), run, "controlled background publication finishes before fixture teardown");
}

void ReportedLateHintJourney() {
    const char* run = "HintReliability/reported-2026-10-25-real-worker";
    constexpr const char* FEN =
        "2r1kb1r/p2q1p2/Q2ppn1p/3P2p1/P7/2N2N1P/1PP2PP1/R1B1R1K1 w k - 0 1";
    constexpr uint64_t OCCUPANCY = UINT64_C(0x5566a40148b929b4);
    // A single provider spans the real worker lifetime. Session calls restore
    // this provider, rather than switching an active engine to another clock.
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    test_support::BoundedHintEngine engine;
    modules::AsyncThinkWorker worker;
    auto config = Config();
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    runtime::VirtualLitchSession s(config, engine, worker);
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) &&
              s.Advance(200U) == VIRTUAL_LITCH_OK && WaitDone(worker) &&
              s.Advance(600U) == VIRTUAL_LITCH_OK &&
              s.LoadExternalFen(FEN), run, "real initial worker quiesces before exact FEN import");
    Check(Tap(s), run, "raw O reaches the correction gate");
    modules::ControlMutationOutcome outcome;
    Check(!s.HintDisplayRequested() && !worker.IsActive(),
          run, "a physically unmatched board cannot claim a ready Hint");
    Checkpoint(s, run, "correction", "match-reported-position", "not-admitted");
    Check(Observe(s, OCCUPANCY) && s.Advance(200U) == VIRTUAL_LITCH_OK && Tap(s) &&
              s.TryGetLastBoardControlOutcome(outcome) &&
              outcome.status == modules::ControlMutationStatus::Applied,
          run, "matching Hall input and raw O admit the actual Hint request");
    Check(WaitDone(worker), run, "the bounded real engine finishes its first Full5 result");
    // Delay coordinator consumption, not engine computation. This proves a
    // completed immutable result survives a late loop; it measures no S3 time.
    Check(s.Advance(20000U) == VIRTUAL_LITCH_OK, run, "consume the result after a controlled late interval");
    uint8_t from = 64U, to = 64U;
    Check(s.TryGetHintMoveSquares(from, to) && from == 16U && to == 52U &&
              s.HomeStateSnapshot().assistance.presentation == modules::HomeAssistancePresentation::HintReady,
          run, "actual a6-e2 analysis reaches the board and authoritative app projection");
    modules::ThinkDiagnostics diagnostic;
    s.RunControlIngress([](void* context, modules::SettingsStore&, modules::ControlMutationService&,
                           modules::GamePersistenceCoordinator& coordinator) {
        (void)coordinator.TryGetThinkDiagnostics(modules::ThinkRequestKind::Hint,
            *static_cast<modules::ThinkDiagnostics*>(context));
    }, &diagnostic);
    // This bounded helper explicitly removes the calculation minimum and its
    // preliminary pass; first scored Full1 and eventual Full5 are separate facts.
    Check(diagnostic.firstPublicationValid && diagnostic.firstAcceptedHintValid &&
              diagnostic.firstPublication.source == modules::ThinkCandidateSource::Search &&
              diagnostic.firstPublication.rootKind == modules::ThinkRootKind::ClassicFull &&
              diagnostic.firstPublication.completedDepth == 1U && diagnostic.firstPublication.scoreValid &&
              diagnostic.firstPublication.scoreUnits == modules::ThinkScoreUnits::ClassicPawn100 &&
              diagnostic.firstPublication.scorePerspective == modules::SideToMove::White &&
              !diagnostic.firstPublication.replayed && diagnostic.firstFullRootValid && diagnostic.fullDepth >= 5U,
          run, "first scored root, eventual Full5 and coordinator acceptance retain actual bounded-engine provenance");
    Checkpoint(s, run, "late-result-accepted", "lift-a6-queen-or-play-another-legal-move");
    if (from >= 64U || to >= 64U) return;
    Check(Observe(s, OCCUPANCY & ~Bit(from)) && s.Advance(100U) == VIRTUAL_LITCH_OK &&
              s.TryGetHintMoveSquares(from, to) && from == 16U && to == 52U &&
              (s.State().valid_destination_mask & Bit(to)) != 0ULL &&
              Energy(Piece(s, to)) > 0U,
          run, "holding the actual source preserves its Hint and physical next-action landing cue");
    Checkpoint(s, run, "source-held", "place-queen-e2-or-return-a6");
    Check(Observe(s, OCCUPANCY) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
              Observe(s, OCCUPANCY | Bit(0U)) && s.Advance(100U) == VIRTUAL_LITCH_OK &&
              s.HomeStateSnapshot().assistance.presentation == modules::HomeAssistancePresentation::Hidden,
          run, "an extra piece takes correction priority over the optional Hint");
    Check(Observe(s, OCCUPANCY) && s.Advance(200U) == VIRTUAL_LITCH_OK &&
              s.TryGetHintMoveSquares(from, to) && from == 16U && to == 52U,
          run, "repair restores the cached analyzed Hint without changing the game");
    Checkpoint(s, run, "correction-recovered", "continue-white-turn");
    struct Terminal { const char* fen; uint64_t occupancy; };
    for (const auto& terminal : std::array<Terminal, 2U>{{
             {"7k/6Q1/6K1/8/8/8/8/8 b - - 0 1", Bit(7U) | Bit(14U) | Bit(22U)},
             {"7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", Bit(7U) | Bit(13U) | Bit(22U)}}}) {
        Check(s.LoadExternalFen(terminal.fen) && Observe(s, terminal.occupancy) &&
                  s.Advance(200U) == VIRTUAL_LITCH_OK && Tap(s) &&
                  s.TryGetLastBoardControlOutcome(outcome) &&
                  outcome.status == modules::ControlMutationStatus::Rejected &&
                  outcome.reason == modules::ControlMutationReason::GameOver &&
                  !s.TryGetHintMoveSquares(from, to) && !worker.IsActive(),
              run, "actual mate/stalemate rejects raw O truthfully without an invented move");
    }
}

void CoachingNoPublicationDeadlineJourney() {
    const char* run = "HintReliability/Coaching-real-deadline-no-publication";
    constexpr const char* FEN =
        "2r1kb1r/p2q1p2/Q2ppn1p/3P2p1/P7/2N2N1P/1PP2PP1/R1B1R1K1 w k - 0 1";
    uint64_t occupancy = UINT64_C(0x5566a40148b929b4);
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    PublicationEngine engine; engine.finish = true;
    modules::AsyncThinkWorker worker;
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    runtime::VirtualLitchSession s(config, engine, worker);
    struct ReleaseOnExit {
        PublicationEngine& engine;
        modules::AsyncThinkWorker& worker;
        const char* run;
        ~ReleaseOnExit() {
            engine.CancelThink();
            engine.releaseRealSearch = true;
            Check(WaitDone(worker), run, "real search retires before fixture teardown");
        }
    } releaseOnExit{engine, worker, run};
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) &&
              s.Advance(200U) == VIRTUAL_LITCH_OK && WaitDone(worker) &&
              s.LoadExternalFen(FEN) && Setting(s, "P1_AUTO_HINTS", "true") &&
              Setting(s, "HINT_THINKING_STYLE", "\"beachball\""),
          run, "saved P1 Automatic Hints uses the normal preference owner before matching the reported position");
    engine.realSearch = true;
    Check(Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK,
          run, "raw Hall matching starts actual background Hint without a manual request");
    const auto pauseLimit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!engine.realSearchPaused.load() && std::chrono::steady_clock::now() < pauseLimit)
        std::this_thread::yield();
    const bool admitted = engine.realSearchPaused.load() && worker.IsActive() &&
        worker.PublishedNodeCount() == 128U && engine.startedMaxThinkMs == 90000U;
    Check(admitted,
          run, "real search is held at its 128-node callback before any qualifying publication");
    if (!admitted) return;
    const uint32_t starts = engine.starts;
    Checkpoint(s, run, "controlled-work-stall", "wait-or-play-a-legal-move");
    Check(s.Advance(90000U) == VIRTUAL_LITCH_OK, run,
          "session clock reaches the production deadline while input/render owners keep running");
    engine.releaseRealSearch = true;
    Check(WaitDone(worker) && s.Advance(1000U) == VIRTUAL_LITCH_OK,
          run, "actual engine observes its expired deadline and the worker cleans up without a result");
    modules::ThinkDiagnostics diagnostic;
    s.RunControlIngress([](void* context, modules::SettingsStore&, modules::ControlMutationService&,
                           modules::GamePersistenceCoordinator& coordinator) {
        (void)coordinator.TryGetThinkDiagnostics(modules::ThinkRequestKind::Hint,
            *static_cast<modules::ThinkDiagnostics*>(context));
    }, &diagnostic);
    uint8_t from = 64U, to = 64U;
    Check(diagnostic.state == modules::ThinkState::Done && diagnostic.stopReason == modules::ThinkStopReason::Deadline &&
              diagnostic.finalNodes == 128U &&
              !diagnostic.firstPublicationValid && !diagnostic.firstAcceptedHintValid &&
              !worker.IsActive() && !s.TryGetHintMoveSquares(from, to) &&
              s.HomeStateSnapshot().assistance.coachingForced,
          run, "deadline exhaustion preserves saved Coaching but cannot invent a board/app move");
    const auto expiredPresentation = s.HomeStateSnapshot().assistance.presentation;
    Check(expiredPresentation != modules::HomeAssistancePresentation::Thinking,
          run, "Home does not claim that the quiescent worker is still calculating");
    std::printf("[HINT_DEADLINE_STATUS_OBSERVATION] home_presentation=%u worker=%u first_publication=%u "
                "nodes=%u elapsed=%u policy_gap=%s\n",
                static_cast<unsigned>(expiredPresentation), worker.IsActive(),
                diagnostic.firstPublicationValid, diagnostic.finalNodes,
                s.State().now_ms - diagnostic.requestAtMs,
                expiredPresentation == modules::HomeAssistancePresentation::Pending
                    ? "Pending-has-no-future-work-in-this-context" : "none-observed");
    Checkpoint(s, run, "deadline-expired", "no-current-context-search-remains", "no-result");
    bool nonCircleControlsLit = false;
    std::printf("[HINT_DEADLINE_CONTROLS] fitted_P1=");
    for (uint8_t role = 0U; role < 6U; ++role) {
        const uint16_t index = 256U + hardware::V3_3PhysicalIndexForControl(false,
            static_cast<hardware::ControlLedRole>(role));
        const auto color = s.PhysicalFrame().pixels[index];
        std::printf("%u:%u,%u,%u,%u;", index, color.r, color.g, color.b, color.w);
        if (role != 0U && Energy(color) != 0U) nonCircleControlsLit = true;
    }
    std::printf(" non_circle_lit=%u expired_status_policy=unspecified\n", nonCircleControlsLit);
    // Characterize this output without inventing an expiry presentation:
    // the contract both stops search at 90s and retains pending requested
    // guidance, but specifies no distinct exhausted-without-result status.
    Check(Tap(s) && s.Advance(8000U) == VIRTUAL_LITCH_OK && engine.starts == starts &&
              !worker.IsActive() && !s.TryGetHintMoveSquares(from, to) &&
              s.SettingsForPersistenceTest().ReadValue("P1_AUTO_HINTS", false),
          run, "same-context Show Move preserves Coaching and does not restart the exhausted window");
    Checkpoint(s, run, "same-context-ShowMove", "change-position-for-a-fresh-analysis-context", "applied-without-result");
    Check(RawMove(s, occupancy, 16U, 52U) &&
              s.UndoHistoryMoveCount() == 1U && Tap(s),
          run, "a real a6-e2 move admits Black analysis in the next context");
    const auto recoveryPollLimit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (worker.IsActive() && !engine.polling.load() &&
           std::chrono::steady_clock::now() < recoveryPollLimit) std::this_thread::yield();
    Check((!worker.IsActive() || engine.polling.load()) && s.Advance(600U) == VIRTUAL_LITCH_OK &&
              WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK && engine.starts > starts &&
              s.TryGetHintMoveSquares(from, to),
          run, "a real a6-e2 move starts the next context and Black can reveal its actual result");
    s.RunControlIngress([](void* context, modules::SettingsStore&, modules::ControlMutationService&,
                           modules::GamePersistenceCoordinator& coordinator) {
        (void)coordinator.TryGetThinkDiagnostics(modules::ThinkRequestKind::Hint,
            *static_cast<modules::ThinkDiagnostics*>(context));
    }, &diagnostic);
    Check(diagnostic.side == modules::SideToMove::Black && diagnostic.firstAcceptedHintValid &&
              diagnostic.preliminaryDepth == 2U && diagnostic.fullDepth >= 5U &&
              diagnostic.latestPublication.source == modules::ThinkCandidateSource::Search &&
              diagnostic.latestPublication.rootKind == modules::ThinkRootKind::ClassicFull &&
              diagnostic.latestPublication.scoreValid && !diagnostic.latestPublication.replayed &&
              diagnostic.latestPublication.scoreUnits == modules::ThinkScoreUnits::ClassicPawn100 &&
              diagnostic.latestPublication.scorePerspective == modules::SideToMove::Black &&
              diagnostic.latestPublication.fromSquare == from && diagnostic.latestPublication.toSquare == to,
          run, "Black recovery retains its exact scored Full result after the real calculation minimum");
    Checkpoint(s, run, "position-change-recovered", "follow-Black-Hint-or-play-another-legal-move");
}

void PinnedLiftKingExposureJourney() {
    const char* run = "PinnedLiftKingExposure/rook-real-Hint-worker";
    constexpr const char* FEN = "k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1";
    const uint64_t occupancy = Bit(0U)|Bit(4U)|Bit(52U)|Bit(60U);
    const uint64_t destinations = Bit(4U)|Bit(12U)|Bit(20U)|Bit(28U)|Bit(36U)|Bit(44U);
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    PublicationEngine engine; engine.finish = true;
    modules::AsyncThinkWorker worker;
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    runtime::VirtualLitchSession s(config, engine, worker);
    struct ReleaseOnExit {
        PublicationEngine& engine;
        modules::AsyncThinkWorker& worker;
        const char* run;
        ~ReleaseOnExit() {
            engine.CancelThink();
            engine.releaseRealSearch = true;
            Check(WaitDone(worker), run, "real search retires before fixture teardown");
        }
    } releaseOnExit{engine, worker, run};
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) &&
              s.Advance(200U) == VIRTUAL_LITCH_OK && WaitDone(worker) &&
              s.LoadExternalFen(FEN) && Setting(s, "P1_CHECK_CUE", "false"),
          run, "normal FEN entry preserves safety feedback independently of optional Check Cue");
    engine.realSearch = true;
    Check(Observe(s, occupancy) && s.Advance(200U) == VIRTUAL_LITCH_OK,
          run, "matching raw Hall occupancy admits the real search and ordinary human turn");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!engine.realSearchPaused.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool active = engine.realSearchPaused.load() && worker.IsActive();
    Check(active && s.Advance(2000U) == VIRTUAL_LITCH_OK, run,
          "actual search is held at its existing progress callback while setup feedback expires");
    if (!active) return;
    const auto plainKing = AllSquarePixels(s, 60U), plainAttacker = AllSquarePixels(s, 4U);
    const uint32_t legalReads = engine.legalReads;
    const uint64_t lifted = occupancy & ~Bit(52U);
    Check(Observe(s, lifted) && worker.IsActive() && engine.legalReads == legalReads,
          run, "pinned lift uses immutable FEN and cached legality without entering the busy engine");
    Check(s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
              s.State().human_illegal_mask == 0ULL && s.State().correction_mask == 0ULL &&
              s.State().valid_destination_mask == destinations && s.UndoHistoryMoveCount() == 0U,
          run, "legal pinned Rook stays held with every along-ray destination and no illegal mode");
    const auto onKing = AllSquarePixels(s, 60U), onAttacker = AllSquarePixels(s, 4U);
    Check(!SameSquarePixels(onKing, plainKing) && !SameSquarePixels(onAttacker, plainAttacker),
          run, "first lift immediately explains the exposed king and actual pinning Rook");
    Checkpoint(s, run, "pin-lift-on", "move-along-e-file-or-restore-e2", "accepted", Bit(4U)|Bit(52U)|Bit(60U));
    Check(s.Advance(300U) == VIRTUAL_LITCH_OK, run, "unchanged held position reaches existing warning off phase");
    const auto offKing = AllSquarePixels(s, 60U), offAttacker = AllSquarePixels(s, 4U);
    Check(!SameSquarePixels(onKing, offKing) && !SameSquarePixels(onAttacker, offAttacker), run,
          "king and pinner warning flashes turn off without restarting on unchanged bookkeeping");
    Checkpoint(s, run, "pin-held-off", "legal-destinations-still-available", "accepted", Bit(4U)|Bit(60U));
    Check(s.Advance(300U) == VIRTUAL_LITCH_OK &&
              !SameSquarePixels(offKing, AllSquarePixels(s, 60U)) &&
              !SameSquarePixels(offAttacker, AllSquarePixels(s, 4U)), run,
          "second synchronized flash returns while the same pinned piece remains held");
    Check(Observe(s, occupancy) && s.UndoHistoryMoveCount() == 0U &&
              SameSquarePixels(AllSquarePixels(s, 60U), plainKing) &&
              SameSquarePixels(AllSquarePixels(s, 4U), plainAttacker), run,
          "restoring source immediately clears both explanations without committing a move");
    Checkpoint(s, run, "source-restored", "ordinary-White-turn", "accepted", Bit(4U)|Bit(52U)|Bit(60U));
    engine.releaseRealSearch = true;
    Check(WaitDone(worker) && s.Advance(20U) == VIRTUAL_LITCH_OK &&
              Observe(s, lifted) && Observe(s, lifted | Bit(44U)) &&
              s.Advance(2200U) == VIRTUAL_LITCH_OK, run,
          "raw Rook placement along the pin ray completes the ordinary action and acknowledgement");
    const auto applied = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (s.UndoHistoryMoveCount() != 1U && std::chrono::steady_clock::now() < applied) {
        std::this_thread::yield(); (void)s.Advance(20U);
    }
    Check(s.UndoHistoryMoveCount() == 1U &&
              std::string(s.HomeStateSnapshot().fen) == "k3r3/8/8/8/8/4R3/8/4K3 b - - 0 1" &&
              s.State().human_illegal_mask == 0ULL, run,
          "legal along-ray move commits once and clears the held-piece explanation");
    Checkpoint(s, run, "ray-move-acknowledged", "Black-to-move", "accepted", Bit(4U)|Bit(44U)|Bit(60U));
}

void PinnedCaptureJourney(bool victimFirst) {
    const char* run = victimFirst ? "PinnedCapture/victim-first/rotated-Minimal-preferences-off" :
                                   "PinnedCapture/source-first/visible-capture-Hint";
    PublicationEngine engine; engine.finish = true;
    auto config = Config(victimFirst); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    runtime::VirtualLitchSession s(config, engine);
    uint64_t occupancy = Bit(0U)|Bit(4U)|Bit(52U)|Bit(60U);
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) &&
              s.Advance(200U) == VIRTUAL_LITCH_OK &&
              s.LoadExternalFen("k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1") &&
              Setting(s, "P1_CHECK_CUE", "false") &&
              (!victimFirst || (Setting(s, "P1_LEGAL_MOVES", "false") &&
                                Setting(s, "MINIMAL_ANIMATIONS", "true"))),
          run, "normal ingress sets an absolute pin and the selected saved presentation profile");
    engine.Choose(52U, 4U);
    Check(Observe(s, occupancy) && s.Advance(2200U) == VIRTUAL_LITCH_OK,
          run, "raw matching occupancy finishes setup and expires entry feedback");
    if (!victimFirst) {
        uint8_t from = 64U, to = 64U;
        Check(Tap(s) && s.Advance(600U) == VIRTUAL_LITCH_OK &&
                  s.TryGetHintMoveSquares(from, to) && from == 52U && to == 4U,
              run, "existing controlled publication reveals the legal e2xe8 capture Hint");
    }
    const auto kingBaseline = AllSquarePixels(s, 60U);
    if (victimFirst) {
        occupancy &= ~Bit(4U);
        Check(Observe(s, occupancy) && Read(s).capturers == Bit(52U), run,
              "removing the pinner first offers the legal pinned Rook as the sole capturer");
        Checkpoint(s, run, "pinner-removed-first", "lift-e2-Rook", "accepted", Bit(4U)|Bit(52U)|Bit(60U));
        occupancy &= ~Bit(52U);
        Check(Observe(s, occupancy), run, "raw legal capturer pickup selects the existing capture plan");
    } else {
        occupancy &= ~Bit(52U);
        Check(Observe(s, occupancy) && s.State().human_illegal_mask == 0ULL &&
                  !SameSquarePixels(kingBaseline, AllSquarePixels(s, 60U)),
              run, "source-first lift warns about the pin without rejecting the legal capture");
        const auto left = s.SemanticFrame().board[4U][0U];
        const auto right = s.SemanticFrame().board[4U][2U];
        Check(Equal(left, right) && left.r > 0U && left.g <= left.r && left.b == 0U &&
                  EmptyPieceTargets(s, 4U),
              run, "visible capture Hint keeps its synchronized warm removal rails during the king warning");
        Checkpoint(s, run, "pinned-mover-held-Hint-removal", "remove-e8-pinner", "accepted", Bit(4U)|Bit(52U)|Bit(60U));
        occupancy &= ~Bit(4U);
        Check(Observe(s, occupancy), run, "raw pinner removal hands ownership to capture landing");
    }
    Check((s.State().physical_occupied_mask & Bit(4U)) == 0ULL &&
              s.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::ExecutingPlan) &&
              SameSquarePixels(kingBaseline, AllSquarePixels(s, 60U)) &&
              s.SemanticFrame().board[4U][0U].g == 0U &&
              s.State().human_illegal_mask == 0ULL && s.UndoHistoryMoveCount() == 0U,
          run, "absent pinner receives ordinary landing guidance and the king warning retires before capture ACK");
    Checkpoint(s, run, "capture-landing", "place-Rook-on-e8", "accepted", Bit(4U)|Bit(60U));
    occupancy |= Bit(4U);
    Check(Observe(s, occupancy) && s.Advance(2200U) == VIRTUAL_LITCH_OK &&
              s.UndoHistoryMoveCount() == 1U &&
              std::string(s.HomeStateSnapshot().fen) == "k3R3/8/8/8/8/8/8/4K3 b - - 0 1" &&
              s.State().human_illegal_mask == 0ULL && s.State().correction_mask == 0ULL,
          run, "pinner capture commits exactly once through the actual action and acknowledgement");
    Checkpoint(s, run, "capture-acknowledged", "Black-to-move", "accepted", Bit(4U)|Bit(60U));
}

void PinnedOffRayRecoveryJourney() {
    const char* run = "PinnedLift/off-ray-rejection-and-recovery";
    PublicationEngine engine; engine.finish = true;
    auto config = Config(); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    runtime::VirtualLitchSession s(config, engine);
    const uint64_t occupancy = Bit(0U)|Bit(4U)|Bit(52U)|Bit(60U);
    Check(s.Reset() == VIRTUAL_LITCH_OK &&
              s.LoadExternalFen("k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1") &&
              Observe(s, occupancy) && s.Advance(2200U) == VIRTUAL_LITCH_OK &&
              Observe(s, occupancy & ~Bit(52U)) &&
              Observe(s, (occupancy & ~Bit(52U)) | Bit(51U)) &&
              s.Advance(1200U) == VIRTUAL_LITCH_OK,
          run, "raw off-ray e2-d2 placement enters ordinary invalid placement recovery");
    Check(s.UndoHistoryMoveCount() == 0U &&
              ((s.State().human_illegal_mask | s.State().correction_mask) & Bit(51U)) != 0ULL &&
              std::string(s.HomeStateSnapshot().fen) == "k3r3/8/8/8/8/8/4R3/4K3 w - - 0 1",
          run, "illegal off-ray placement cannot bypass the pin or reach engine acknowledgement");
    Checkpoint(s, run, "off-ray-rejected", "lift-d2-and-return-e2", "rejected", Bit(4U)|Bit(51U)|Bit(52U)|Bit(60U));
    Check(Observe(s, occupancy & ~Bit(52U)) && Observe(s, occupancy) &&
              s.Advance(1200U) == VIRTUAL_LITCH_OK &&
              s.State().human_illegal_mask == 0ULL && s.State().correction_mask == 0ULL &&
              s.UndoHistoryMoveCount() == 0U,
          run, "raw piece restoration clears correction and leaves the original turn intact");
    Checkpoint(s, run, "restored", "choose-a-legal-along-ray-move", "accepted", Bit(4U)|Bit(52U)|Bit(60U));
}

void ImmobilePinnedHintJourney() {
    const char* run = "PinnedLift/Black-Knight/real-Hint-worker/rotated-Minimal";
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    PublicationEngine engine; engine.finish = true;
    modules::AsyncThinkWorker worker;
    auto config = Config(true); config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    runtime::VirtualLitchSession s(config, engine, worker);
    struct ReleaseOnExit {
        PublicationEngine& engine;
        modules::AsyncThinkWorker& worker;
        const char* run;
        ~ReleaseOnExit() {
            engine.CancelThink();
            engine.releaseRealSearch = true;
            Check(WaitDone(worker), run, "real search retires before fixture teardown");
        }
    } release{engine, worker, run};
    const uint64_t occupancy = Bit(4U)|Bit(12U)|Bit(56U)|Bit(60U);
    Check(s.Reset() == VIRTUAL_LITCH_OK && Observe(s, OPENING) &&
              s.Advance(200U) == VIRTUAL_LITCH_OK && WaitDone(worker) &&
              s.LoadExternalFen("4k3/4n3/8/8/8/8/8/K3R3 b - - 0 1") &&
              Setting(s, "P2_CHECK_CUE", "false") && Setting(s, "P2_LEGAL_MOVES", "false") &&
              Setting(s, "MINIMAL_ANIMATIONS", "true"),
          run, "Black pin uses the correct owner and retains required feedback with optional cues disabled");
    engine.realSearch = true;
    Check(Observe(s, occupancy) && s.Advance(2200U) == VIRTUAL_LITCH_OK, run, "raw Hall setup starts real analysis");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!engine.realSearchPaused.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const auto king = AllSquarePixels(s, 4U), pinner = AllSquarePixels(s, 60U);
    const uint32_t reads = engine.legalReads;
    Check(engine.realSearchPaused.load() && worker.IsActive() &&
              Observe(s, occupancy & ~Bit(12U)) && engine.legalReads == reads &&
              s.State().human_illegal_mask == Bit(12U) && s.State().valid_destination_mask == 0ULL,
          run, "cached zero-legality pin rejects immobile Knight immediately without entering the busy engine");
    Check(!SameSquarePixels(king, AllSquarePixels(s, 4U)) &&
              !SameSquarePixels(pinner, AllSquarePixels(s, 60U)), run,
          "both mapped squares flash on the first Black Knight lift even under Minimal Animations");
    Checkpoint(s, run, "Knight-lift-rejected", "return-Knight-to-e7", "rejected", Bit(4U)|Bit(12U)|Bit(60U));
    Check(s.Advance(300U) == VIRTUAL_LITCH_OK &&
              SameSquarePixels(king, AllSquarePixels(s, 4U)) &&
              SameSquarePixels(pinner, AllSquarePixels(s, 60U)), run,
          "required 300ms off phase survives the same real background search");
    Check(Observe(s, occupancy) && s.State().human_illegal_mask == 0ULL &&
              s.UndoHistoryMoveCount() == 0U, run, "raw restoration clears the warning and the rejected pickup");
    Checkpoint(s, run, "Knight-restored", "Black-turn-continues", "accepted", Bit(4U)|Bit(12U)|Bit(60U));
    engine.CancelThink();
    engine.releaseRealSearch = true;
    Check(WaitDone(worker), run, "the shared worker shuts down without a device or test-only engine query");
}

} // namespace

int main() {
    std::puts("[CHESS_ASSISTANCE_FEEDBACK] production VirtualLitch journeys; appearance/HIL pending");
    PinnedLiftKingExposureJourney();
    PinnedCaptureJourney(false);
    PinnedCaptureJourney(true);
    PinnedOffRayRecoveryJourney();
    ImmobilePinnedHintJourney();
    ReportedLateHintJourney();
    CoachingNoPublicationDeadlineJourney();
    CpuLastMoveCaptureHintJourney(false, false, false, false, true, 128U);
    CpuLastMoveCaptureHintJourney(true, false, true, true, false, 128U);
    CpuLastMoveCaptureHintJourney(false, true, true, false, false, 128U);
    CpuLastMoveCaptureHintJourney(true, false, false, true, true, 8U);
    CpuLastMoveCaptureHintJourney(false, false, false, false, true, 128U, true);
    CpuOffTargetAndPendingUndoJourney();
    HumanCoachAlternateQuietMoveJourney("settle_2s", 2000U);
    HumanCoachAlternateQuietMoveJourney("settle_5s", 5000U);
    HumanCoachAlternateQuietMoveJourney("press_circle_to_confirm", 0U);
    PublicationAndShowMoveJourney();
    HintFeedbackV33PhysicalJourney();
    BishopSingleDestinationJourney();
    KnightDirectionalMatrix();
    for(const bool cpu:{false,true})for(const bool sourceFirst:{false,true})for(const bool first:{false,true})for(const bool rotate:{false,true})
        KnightCaptureJourney(cpu,sourceFirst,first,rotate);
    for (const bool firstP2 : {false, true})
        for (const bool rotated : {false, true}) AdjacentMoveTravelJourney(firstP2, rotated);
    EnPassantRemovalBeforeLandingJourney();
    for(bool rotate:{false,true})for(bool firstP2:{false,true})CaptureEdgeBlockedBracketJourney({rotate,firstP2,230U,0U});
    CaptureEdgeBlockedBracketJourney({false,false,256U,0U});
    CaptureEdgeBlockedBracketJourney({true,false,230U,80U});
    UndoLocalReturnJourney(false, false);
    UndoLocalReturnJourney(true, true);
    UndoLocalReturnJourney(false, false, true);
    const std::array<CaptureCase, 6U> positions{{
        {"CaptureFirstVisibleHint/completed-Classical", "4k3/8/8/8/3p4/8/3R4/4K3 w - - 0 1", Bit(4U)|Bit(35U)|Bit(51U)|Bit(60U), 35U, Bit(51U), 64U, true},
        {"CaptureFirstWithoutCoaching/completed-Classical", "4k3/8/8/8/3p4/8/3R4/4K3 w - - 0 1", Bit(4U)|Bit(35U)|Bit(51U)|Bit(60U), 35U, Bit(51U), 64U},
        {"CaptureFirstWithoutCoaching/pinned-exclusion", "4r2k/8/8/8/8/8/4Rn2/4K1B1 w - - 0 1", Bit(4U)|Bit(7U)|Bit(52U)|Bit(53U)|Bit(60U)|Bit(62U), 53U, Bit(60U)|Bit(62U), 52U},
        {"CaptureFirstVisibleHint/EP-and-ordinary-union", "4k3/8/8/2PpP3/8/8/3R4/4K3 w - d6 0 1", Bit(4U)|Bit(26U)|Bit(27U)|Bit(28U)|Bit(51U)|Bit(60U), 27U, Bit(26U)|Bit(28U)|Bit(51U), 64U, true},
        {"CaptureFirstWithoutCoaching/EP-and-ordinary-union", "4k3/8/8/2PpP3/8/8/3R4/4K3 w - d6 0 1", Bit(4U)|Bit(26U)|Bit(27U)|Bit(28U)|Bit(51U)|Bit(60U), 27U, Bit(26U)|Bit(28U)|Bit(51U), 64U},
        {"CaptureFirstWithoutCoaching/Black-rotated", "4k3/3r4/8/3P4/8/8/8/4K3 b - - 0 1", Bit(4U)|Bit(11U)|Bit(27U)|Bit(60U), 27U, Bit(11U), 64U},
    }};
    for (const auto& position : positions) CaptureFirstGuidanceJourney(position, position.capturers == Bit(11U));
    DuckCaptureFirstJourney();
    CompletedHintCpuTakeoverJourney();
    NoThinkingAnimationJourney(false);
    NoThinkingAnimationJourney(true);
    BeachballResultJourney(false);
    BeachballResultJourney(true);
    BeachballResultJourney(false,8U);
    BeachballResultJourney(true,86U,80U);
    CaptureFirstOverlaySuppressionSupportingControl();
    EqualPaletteHeldLocatorSupportingControl();
    DirectedTravelUsesActualMoverPalette();
    RemovalFadeRefinementSupportingControl();
    PreflightChargesSearchWindow();
    FailedPreflightDeadlineAndWrap();
    std::printf("[CHESS_ASSISTANCE_FEEDBACK] %s failures=%d\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
