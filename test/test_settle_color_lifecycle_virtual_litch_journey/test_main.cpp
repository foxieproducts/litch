#include "bounded_hint_engine.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <atomic>
#include <thread>

#include <ArduinoJson.h>

#include "hardware/board_led_output_mapping.hpp"
#include "modules/async_think_worker.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

constexpr uint64_t OPENING = UINT64_C(0xffff00000000ffff);
int g_failures = 0;

template <typename Predicate>
bool WaitFor(Predicate predicate) {
    for (uint32_t attempt = 0U; attempt < 1000000U; ++attempt) {
        if (predicate()) return true;
        std::this_thread::yield();
    }
    return false;
}

// Only the worker's return scheduling is held. Legality, FEN, search result,
// physical interaction, commit and rendering all remain production owners.
class GatedHintEngine final : public modules::IEngine {
  public:
    void Reset() override { engine.Reset(); }
    bool SetFen(const char* fen) override { return engine.SetFen(fen); }
    bool GetFen(char* fen, size_t size) const override { return engine.GetFen(fen, size); }
    bool ApplyUciMove(const char* move) override { ++applies; return engine.ApplyUciMove(move); }
    modules::SideToMove GetSideToMove() const override { return engine.GetSideToMove(); }
    bool IsInCheck(modules::SideToMove side) const override { return engine.IsInCheck(side); }
    bool IsInCheckmate(modules::SideToMove side) const override { return engine.IsInCheckmate(side); }
    bool IsInStalemate(modules::SideToMove side) const override { return engine.IsInStalemate(side); }
    bool GetLegalDestinationsMask(uint8_t from, uint64_t& mask) override {
        return engine.GetLegalDestinationsMask(from, mask);
    }
    bool StartThink(const modules::ThinkConfig& config) override {
        held = holdHints && config.continuousAnalysis;
        entered.store(false, std::memory_order_release);
        released.store(!held, std::memory_order_release);
        return engine.StartThink(config);
    }
    void CancelThink() override { ++cancels; engine.CancelThink(); }
    modules::ThinkState PollThinkProgress(uint8_t& progress, modules::UciMove& move) override {
        entered.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) std::this_thread::yield();
        return engine.PollThinkProgress(progress, move);
    }
    bool GetLastThinkStats(modules::ThinkStats& stats) const override {
        return engine.GetLastThinkStats(stats);
    }
    bool GetLastClassicThinkResult(modules::ClassicThinkResult& result) const override {
        return engine.GetLastClassicThinkResult(result);
    }
    bool GetLastDuckThinkResult(modules::DuckThinkResult& result) const override {
        return engine.GetLastDuckThinkResult(result);
    }
    bool holdHints = false;
    bool held = false;
    uint32_t applies = 0U;
    uint32_t cancels = 0U;
    std::atomic<bool> entered{false};
    std::atomic<bool> released{true};
  private:
    test_support::BoundedHintEngine engine;
};

void Check(bool condition, const char* run, const char* message) {
    if (!condition) {
        std::printf("[FAIL] COLOR_LIFECYCLE %s: %s\n", run, message);
        ++g_failures;
    }
}

bool Set(runtime::VirtualLitchSession& session, const char* key, const char* value) {
    modules::ControlMutationOutcome outcome;
    const bool accepted = session.MutateSettingJson(100U, key, value, outcome) == VIRTUAL_LITCH_OK &&
        (outcome.status == modules::ControlMutationStatus::Applied ||
         outcome.status == modules::ControlMutationStatus::Noop);
    if (!accepted) std::printf("[COLOR_SETTING] key=%s value=%s status=%u reason=%u\n", key, value,
                              static_cast<unsigned>(outcome.status), static_cast<unsigned>(outcome.reason));
    return accepted;
}

bool Observe(runtime::VirtualLitchSession& session, uint64_t logical) {
    const bool rotate = modules::ComputeSquareRotate(
        session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logical & (UINT64_C(1) << square)) != 0ULL) {
            physical |= UINT64_C(1) << modules::MapSquareToPhysical(square, rotate);
        }
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
        session.State().physical_occupied_mask == logical;
}

uint8_t Square(const char* value) {
    return value != nullptr && value[0] >= 'a' && value[0] <= 'h' &&
            value[1] >= '1' && value[1] <= '8'
        ? static_cast<uint8_t>((8 - (value[1] - '0')) * 8 + value[0] - 'a')
        : 64U;
}

uint8_t Channel(virtual_litch_color color) {
    if (color.r > color.g && color.r > color.b) return 0U;
    if (color.g > color.r && color.g > color.b) return 1U;
    if (color.b > color.r && color.b > color.g) return 2U;
    return 3U;
}

bool PieceHasOwner(const runtime::VirtualLitchSession& session, uint8_t square,
                   bool black) {
    const uint8_t expected = black != session.FirstPlayerIsP2() ? 2U : 0U;
    if (Channel(session.SemanticFrame().board[square][1U]) != expected) return false;
    const auto targets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Piece,
        modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2()));
    for (uint8_t index = 0U; index < targets.count; ++index) {
        if (Channel(session.PhysicalFrame().pixels[targets.indices[index]]) != expected) {
            return false;
        }
    }
    return targets.count != 0U;
}

bool RedFault(const runtime::VirtualLitchSession& session) {
    for (const auto color : session.SemanticFrame().status) {
        if (color.r == 0U || color.r <= color.g || color.r <= color.b) return false;
    }
    return true;
}

bool Rainbow(const runtime::VirtualLitchSession& session, uint8_t square) {
    const auto left = session.SemanticFrame().board[square][0U];
    const auto right = session.SemanticFrame().board[square][2U];
    return left.r != right.r || left.g != right.g || left.b != right.b || left.w != right.w;
}

void Checkpoint(const runtime::VirtualLitchSession& session, const char* run,
                const char* stage, uint8_t square, const char* next,
                const char* result = "accepted") {
    const auto& state = session.State();
    const auto color = session.SemanticFrame().board[square][1U];
    const auto side = session.SemanticFrame().board[square][0U];
    const auto targets = hardware::MapSemanticSquareLedTargets(
        hardware::BoardRevision::V3S3, square, hardware::SquareLedSlot::Piece,
        modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2()));
    const auto centerA = session.PhysicalFrame().pixels[targets.indices[0U]];
    const auto centerB = session.PhysicalFrame().pixels[targets.indices[1U]];
    const auto display = session.DisplayText();
    std::printf("[COLOR_LIFECYCLE] run=%s stage=%s t=%u occupancy=%016llx "
                "expected=%016llx phase=%u required=%u action=%u history=%u "
                "correction=%016llx human_illegal=%016llx cpu_illegal=%016llx red_fault=%u "
                "square=%u piece=%u,%u,%u side=%u,%u,%u "
                "physical=%u:%u,%u,%u,%u;%u:%u,%u,%u,%u "
                "display=\"%.*s\" result=%s next=%s home=%s\n",
                run, stage, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask),
                state.interaction_phase, state.required_action, state.pending_action_token,
                session.UndoHistoryMoveCount(),
                static_cast<unsigned long long>(state.correction_mask),
                static_cast<unsigned long long>(state.human_illegal_mask),
                static_cast<unsigned long long>(state.cpu_illegal_mask),
                RedFault(session), square, color.r, color.g, color.b,
                side.r, side.g, side.b,
                targets.indices[0U], centerA.r, centerA.g, centerA.b, centerA.w,
                targets.indices[1U], centerB.r, centerB.g, centerB.b, centerB.w,
                static_cast<int>(display.size()), display.empty() ? "" : display.data(),
                result, next, session.HomeStateLine().CStr());
}

bool Sample(runtime::VirtualLitchSession& session, const char* run, uint8_t square,
            bool black, uint32_t duration, uint8_t blinkingSource = 64U,
            uint8_t completionRippleProbe = 64U) {
    bool allFrames = true;
    bool completionClear = true;
    uint32_t wrongFrames = 0U;
    uint8_t history = session.UndoHistoryMoveCount();
    StaticJsonDocument<2048> home;
    const bool parsed = deserializeJson(home, session.HomeStateLine().CStr()) ==
        DeserializationError::Ok;
    uint64_t whitePieces = 0ULL;
    uint64_t blackPieces = 0ULL;
    uint8_t fenSquare = 0U;
    for (const char* piece = parsed ? (home["fen"] | "") : "";
         *piece != '\0' && *piece != ' ' && fenSquare < 64U; ++piece) {
        if (*piece >= '1' && *piece <= '8') {
            fenSquare = static_cast<uint8_t>(fenSquare + *piece - '0');
        } else if (*piece != '/') {
            (*piece >= 'a' && *piece <= 'z' ? blackPieces : whitePieces) |=
                UINT64_C(1) << fenSquare++;
        }
    }
    const auto ordinaryOwnersStable = [&] {
        for (uint8_t ordinary = 0U; ordinary < 64U; ++ordinary) {
            const uint64_t bit = UINT64_C(1) << ordinary;
            if (ordinary == square || (session.State().physical_occupied_mask & bit) == 0ULL) continue;
            // A required pickup blinks its source center off by contract.
            // Its lit frames must still have the correct piece identity.
            const auto sourceColor = session.SemanticFrame().board[ordinary][1U];
            if (ordinary == blinkingSource && sourceColor.r == 0U &&
                sourceColor.g == 0U && sourceColor.b == 0U && sourceColor.w == 0U) continue;
            if ((whitePieces & bit) != 0ULL && !PieceHasOwner(session, ordinary, false)) return false;
            if ((blackPieces & bit) != 0ULL && !PieceHasOwner(session, ordinary, true)) return false;
        }
        return parsed && fenSquare == 64U;
    };
    for (uint32_t elapsed = 0U; elapsed <= duration; ++elapsed) {
        if (completionRippleProbe < 64U &&
            session.HomeStateSnapshot().action == modules::HomeStateAction::Idle) {
            const auto probe = session.SemanticFrame().board[completionRippleProbe][0U];
            if (probe.r != 0U || probe.g != 0U || probe.b != 0U) {
                if (completionClear) Checkpoint(session, run, "unexpected-post-settle-ripple",
                    completionRippleProbe, "leave-completed-move-still", "unexpected-second-effect");
                completionClear = false;
            }
        }
        if (!PieceHasOwner(session, square, black) || !ordinaryOwnersStable()) {
            if (wrongFrames == 0U) Checkpoint(session, run, "first-wrong-owner", square, "retain-mover-color", "wrong-owner");
            ++wrongFrames;
            allFrames = false;
        }
        if (history != session.UndoHistoryMoveCount()) {
            Checkpoint(session, run, "action-acknowledged", square, "wait-for-presentation-expiry");
            history = session.UndoHistoryMoveCount();
        }
        if (elapsed == 199U || elapsed == 200U || elapsed == 1000U || elapsed == 2199U || elapsed == 2200U) {
            Checkpoint(session, run, "timed-frame", square, "retain-mover-color");
        }
        if (elapsed != duration && session.Advance(1U) != VIRTUAL_LITCH_OK) {
            allFrames = false;
            break;
        }
    }
    std::printf("[COLOR_FRAMES] run=%s samples=%u wrong=%u\n", run, duration + 1U, wrongFrames);
    Check(completionClear, run, "completed CPU settling cannot start another radial ripple");
    return allFrames && completionClear;
}

// Capture positions give check: isolate settle-owner color from the optional
// cue. test_duck_led_render::CheckOrthodoxThreatIntentSuppressedForDuck retains
// the real Classic king/checker and mapped-output Check Cue evidence.
void CpuJourney(bool black, bool rotate, bool swapped, bool capture = false,
                bool targetFirst = false, uint32_t bumpAfterMs = UINT32_MAX,
                const char* style = nullptr) {
    char run[128]{};
    std::snprintf(run, sizeof(run), "CPU-%s/%s/rotate-%u/swapped-%u/target-first-%u/bump-%u/style-%s",
                  black ? "P2" : "P1", capture ? "capture" : "quiet", rotate, swapped,
                  targetFirst, bumpAfterMs, style == nullptr ? "default" : style);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    const bool rippleProof = !black && !rotate && !swapped && !capture &&
                            !targetFirst && bumpAfterMs == UINT32_MAX && style == nullptr;
    const bool noneProof = style != nullptr && std::strcmp(style, "none") == 0;
    config.hardware = rippleProof || noneProof ? VIRTUAL_LITCH_HARDWARE_V3_3 : VIRTUAL_LITCH_HARDWARE_V3;
    if (rippleProof || noneProof) config.brightness_scale_256 = 8U;
    config.board_rotate = rotate;
    config.first_player_is_p2 = swapped;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    bool ready = session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(session, "P2_END_PIECE_WHEEL", "85") &&
        Set(session, "P1_END_PIECE_WHEEL", "0") &&
        Set(session, "P2_END_PIECE_WHEEL", "170") &&
        Set(session, "P1_CHECK_CUE", "false") && Set(session, "P2_CHECK_CUE", "false");
    uint64_t occupancy = OPENING;
    if (rippleProof) {
        ready = ready && Set(session, "SETTLE_ANIM_STYLE", "\"ripple\"") &&
                        Set(session, "WHITE_SQUARES_ON", "false");
    }
    if (noneProof) {
        ready = ready && Set(session, "SETTLE_ANIM_STYLE", "\"none\"") &&
                        Set(session, "WHITE_SQUARES_ON", "false") &&
                        Set(session, "BRIGHTNESS", "0");
    }
    if (capture) {
        const char* fen = black ? "6k1/4r3/8/4Q3/8/8/8/4K3 b - - 0 1"
                                : "4k3/8/8/8/4q3/8/4R3/6K1 w - - 0 1";
        occupancy = black ? (UINT64_C(1) << 6U) | (UINT64_C(1) << 12U) |
                                (UINT64_C(1) << 28U) | (UINT64_C(1) << 60U)
                          : (UINT64_C(1) << 4U) | (UINT64_C(1) << 36U) |
                                (UINT64_C(1) << 52U) | (UINT64_C(1) << 62U);
        ready = ready && session.LoadExternalFen(fen) && Observe(session, occupancy) &&
            session.Advance(200U) == VIRTUAL_LITCH_OK;
    } else if (black) {
        occupancy &= ~(UINT64_C(1) << 52U);
        ready = ready && Observe(session, occupancy);
        occupancy |= UINT64_C(1) << 36U;
        ready = ready && Observe(session, occupancy) && session.Advance(2600U) == VIRTUAL_LITCH_OK;
    }
    ready = ready && Set(session, "VS_CPU_PLAYER", black ? "2" : "1") &&
        Set(session, "VS_CPU_LEVEL", "1");
    StaticJsonDocument<2048> home;
    bool queued = false;
    for (uint32_t attempt = 0U; ready && attempt < 120U; ++attempt) {
        if (deserializeJson(home, session.HomeStateLine().CStr()) != DeserializationError::Ok) break;
        if (std::strcmp(home["a"] | "", "cpu") == 0 && home["c"].is<JsonArray>()) {
            queued = true;
            break;
        }
        ready = session.Advance(100U) == VIRTUAL_LITCH_OK;
    }
    const uint8_t from = Square(home["c"][0U] | "--");
    const uint8_t to = Square(home["c"][1U] | "--");
    Check(ready && queued && from < 64U && to < 64U, run, "production CPU selects a move");
    if (!ready || !queued || from >= 64U || to >= 64U) {
        Checkpoint(session, run, "setup-failed", 0U, "establish-CPU-prompt");
        return;
    }
    Check(((occupancy & (UINT64_C(1) << to)) != 0ULL) == capture, run,
          "production search selects the required quiet or capture move");
    Checkpoint(session, run, "CPU-selected", from, "lift-source");
    const uint8_t history = session.UndoHistoryMoveCount();
    // A wrong-square lift is recoverable without changing the directed move.
    const uint8_t other = black ? 60U : 4U;
    Check(Observe(session, occupancy & ~(UINT64_C(1) << other)) &&
              session.UndoHistoryMoveCount() == history, run,
          "unrelated pickup preserves CPU action and history while requesting correction");
    Checkpoint(session, run, "wrong-square-lift", other, "restore-unrelated-piece", "rejected-requires-restoration");
    Check(Observe(session, occupancy) && session.State().cpu_illegal_mask == 0ULL, run,
          "restoration clears CPU correction before continuing the same move");
    Checkpoint(session, run, "correction-restored", from, "follow-CPU-move");
    Check(session.Advance(20U) == VIRTUAL_LITCH_OK, run,
          "correction acknowledgement completes before the next physical gesture");
    if (capture && targetFirst) {
        occupancy &= ~(UINT64_C(1) << to);
        Check(Observe(session, occupancy), run, "target-first victim removal accepted");
    }
    occupancy &= ~(UINT64_C(1) << from);
    Check(Observe(session, occupancy), run, "raw Hall source pickup accepted");
    if (capture && !targetFirst) {
        const auto dark = [](virtual_litch_color c) { return (c.r | c.g | c.b | c.w) == 0U; };
        bool victimDark = dark(session.SemanticFrame().board[to][1U]);
        const auto targets = hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3S3,
            to,hardware::SquareLedSlot::Piece,
            modules::ComputeSquareRotate(session.BoardRotate(),session.FirstPlayerIsP2()));
        for(uint8_t i=0U;i<targets.count;++i) victimDark = victimDark && dark(session.PhysicalFrame().pixels[targets.indices[i]]);
        Check(victimDark && targets.count == 2U, run,
              "source-first mandatory capture keeps both actual victim Piece LEDs at zero until removal");
        Checkpoint(session, run, "victim-still-present", to, "remove-victim");
        occupancy &= ~(UINT64_C(1) << to);
        Check(Observe(session, occupancy), run, "victim removal acknowledged before mover placement");
    }
    Checkpoint(session, run, "source-lifted", to, "place-destination");
    occupancy |= UINT64_C(1) << to;
    Check(Observe(session, occupancy), run, "raw Hall destination placement accepted");
    Check(!noneProof || PieceHasOwner(session, to, black), run,
          "None's first CPU placement frame keeps the actual mover identity, never neutral Duck yellow");
    Checkpoint(session, run, "destination-placed", to, "wait-for-settle");
    if (bumpAfterMs != UINT32_MAX) {
        const uint8_t bumpedSquare = black ? 48U : 8U;
        const bool beforeStability = bumpAfterMs < 200U;
        Check(session.Advance(bumpAfterMs) == VIRTUAL_LITCH_OK &&
                  Observe(session, occupancy & ~(UINT64_C(1) << bumpedSquare)), run,
              "unrelated next-player piece is bumped during CPU physical completion");
        Checkpoint(session, run, "settle-overlap-bump", bumpedSquare,
                   "restore-unrelated-piece",
                   beforeStability ? "rejected-requires-restoration" : "accepted-next-player-pickup");
        Check(session.Advance(1600U) == VIRTUAL_LITCH_OK &&
                  session.UndoHistoryMoveCount() == history + (beforeStability ? 0U : 1U), run,
              "bumped piece does not commit or duplicate a move");
        Check(beforeStability
                  ? Rainbow(session, bumpedSquare) && RedFault(session)
                  : session.State().interaction_phase ==
                        static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
                        !Rainbow(session, bumpedSquare) && !RedFault(session), run,
              "pre-completion error retains correction; post-ack pickup uses ordinary next-player guidance");
        Checkpoint(session, run, "settle-overlap-correction", bumpedSquare,
                   "restore-unrelated-piece",
                   beforeStability ? "rejected-requires-restoration" : "accepted-next-player-pickup");
        Check(Observe(session, occupancy), run, "bumped piece is restored to its own square");
        Checkpoint(session, run, "settle-overlap-restored", bumpedSquare,
                   "retain-restored-piece-owner");
        Check(Sample(session, run, bumpedSquare, !black, 3000U), run,
              "restored next-player piece never inherits the CPU settle color at any intermediate frame");
        Check(session.State().human_illegal_mask == 0ULL &&
                  session.State().cpu_illegal_mask == 0ULL &&
                  session.State().correction_mask == 0ULL, run,
              "restoration retires correction and its pending feedback");
        Checkpoint(session, run, "settle-overlap-recovered", bumpedSquare,
                   "next-player-move");
    }
    uint8_t completionProbe = 64U;
    if (rippleProof) {
        constexpr int8_t OFFSETS[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& offset : OFFSETS) {
            const int rank = static_cast<int>(to / 8U) + offset[0];
            const int file = static_cast<int>(to % 8U) + offset[1];
            if (rank < 0 || rank >= 8 || file < 0 || file >= 8) continue;
            const uint8_t candidate = static_cast<uint8_t>(rank * 8 + file);
            if (candidate != from && (occupancy & (UINT64_C(1) << candidate)) == 0ULL) {
                completionProbe = candidate;
                break;
            }
        }
        Check(completionProbe < 64U, run, "CPU Ripple has a distinct empty neighbor for completion evidence");
    }
    Check(Sample(session, run, to, black, 3000U, 64U, completionProbe), run,
          "every occupied frame keeps mover hue through stability, settle, acknowledgement and expiry");
    Check(session.UndoHistoryMoveCount() == history + 1U &&
              session.State().expected_occupied_mask == occupancy &&
              session.State().side_to_move_black != black,
          run, "CPU move commits exactly once with the next player to move");
    Checkpoint(session, run, "settle-expired", to, "next-player-move");
    if (!capture) {
        const uint8_t humanSource = black ? 48U : 8U;
        Check(Observe(session, occupancy & ~(UINT64_C(1) << humanSource)) &&
                  Observe(session, occupancy), run,
              "next human player can lift and return a piece after CPU settling");
        const uint8_t expected = (!black != session.FirstPlayerIsP2()) ? 2U : 0U;
        const auto side = session.SemanticFrame().board[humanSource][0U];
        Check((noneProof ? side.r == 0U && side.g == 0U && side.b == 0U
                         : Channel(side) == expected) &&
                  PieceHasOwner(session, humanSource, !black), run,
              "next-player piece keeps its owner and optional return feedback respects None");
        Checkpoint(session, run, "next-player-return", humanSource, "choose-human-move");
    }
}

void BlockedPiecePickup(uint32_t idleMs) {
    char run[64]{};
    std::snprintf(run, sizeof(run), "Blocked-P1/rook-a1/idle-%u", idleMs);
    constexpr uint8_t SOURCE = 56U;
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(session, "P2_END_PIECE_WHEEL", "85") &&
        Set(session, "P1_END_PIECE_WHEEL", "0") &&
        Set(session, "P2_END_PIECE_WHEEL", "170") &&
        session.Advance(idleMs) == VIRTUAL_LITCH_OK;
    Check(ready, run, "normal opening is ready with the P1 rook blocked by its own pieces");
    if (!ready) return;
    Check(Observe(session, OPENING & ~(UINT64_C(1) << SOURCE)), run,
          "physical rook pickup reaches the production legality owner");
    uint32_t wrongFrames = 0U;
    for (uint32_t elapsed = 0U; elapsed <= 2200U; ++elapsed) {
        const auto& state = session.State();
        bool physicalPickupCorrect = true;
        for (const auto slot : {hardware::SquareLedSlot::Square0,
                                hardware::SquareLedSlot::Square2,
                                hardware::SquareLedSlot::Piece}) {
            const auto targets = hardware::MapSemanticSquareLedTargets(
                hardware::BoardRevision::V3S3, SOURCE, slot, false);
            physicalPickupCorrect &= targets.count != 0U;
            for (uint8_t i = 0U; i < targets.count; ++i) {
                const auto color = session.PhysicalFrame().pixels[targets.indices[i]];
                physicalPickupCorrect &= slot == hardware::SquareLedSlot::Piece
                    ? color.r == 0U && color.g == 0U && color.b == 0U && color.w == 0U
                    : Channel(color) == 0U;
            }
        }
        const bool correct = state.interaction_phase ==
                static_cast<uint8_t>(runtime::ChessInteractionPhase::PieceLifted) &&
            state.selected_square == SOURCE && state.valid_destination_mask == 0ULL &&
            (state.correction_mask | state.human_illegal_mask | state.cpu_illegal_mask) == 0ULL &&
            state.pending_action_token == 0U && session.UndoHistoryMoveCount() == 0U &&
            !Rainbow(session, SOURCE) && !RedFault(session) && physicalPickupCorrect &&
            Channel(session.SemanticFrame().board[SOURCE][0U]) == 0U &&
            session.SemanticFrame().board[SOURCE][1U].r == 0U &&
            session.SemanticFrame().board[SOURCE][1U].g == 0U &&
            session.SemanticFrame().board[SOURCE][1U].b == 0U;
        if (!correct && wrongFrames++ == 0U) {
            Checkpoint(session, run, "first-wrong-pickup-frame", SOURCE,
                       "keep-origin-guidance", "invalid-flash");
        }
        if (elapsed == 0U || elapsed == 199U || elapsed == 200U ||
            elapsed == 1500U || elapsed == 2200U) {
            Checkpoint(session, run, "held-rook", SOURCE, "return-rook-to-a1");
        }
        if (elapsed != 2200U && session.Advance(1U) != VIRTUAL_LITCH_OK) {
            Check(false, run, "held-piece clock advances");
            break;
        }
    }
    std::printf("[PICKUP_FRAMES] run=%s samples=2201 wrong=%u\n", run, wrongFrames);
    Check(wrongFrames == 0U, run,
          "blocked own-piece pickup keeps origin guidance without transient invalid or rainbow feedback");
    if (idleMs == 0U) {
        constexpr uint8_t INVALID_DESTINATION = 40U; // a3, behind the a2 pawn.
        const uint64_t held = OPENING & ~(UINT64_C(1) << SOURCE);
        Check(Observe(session, held | (UINT64_C(1) << INVALID_DESTINATION)) &&
                  session.Advance(1600U) == VIRTUAL_LITCH_OK &&
                  Rainbow(session, INVALID_DESTINATION) && RedFault(session) &&
                  session.UndoHistoryMoveCount() == 0U &&
                  session.State().pending_action_token == 0U, run,
              "actually placing the blocked rook elsewhere still receives correction without a move");
        Checkpoint(session, run, "blocked-rook-invalid-placement", INVALID_DESTINATION,
                   "lift-rook-from-a3", "rejected-requires-restoration");
        Check(Observe(session, held) && session.State().selected_square == SOURCE &&
                  session.State().human_illegal_mask == 0ULL && !RedFault(session) &&
                  !Rainbow(session, SOURCE), run,
              "reversing invalid placement restores the same held-source guidance");
        Checkpoint(session, run, "blocked-rook-correction-cleared", SOURCE,
                   "return-rook-to-a1");
    }
    Check(Observe(session, OPENING) && session.Advance(600U) == VIRTUAL_LITCH_OK &&
              session.State().interaction_phase ==
                  static_cast<uint8_t>(runtime::ChessInteractionPhase::Idle) &&
              session.UndoHistoryMoveCount() == 0U && PieceHasOwner(session, SOURCE, false), run,
          "source return ends pickup with original piece ownership and no move");
    Checkpoint(session, run, "rook-restored", SOURCE, "choose-another-piece");
}

void HumanCaptureHeldAcknowledgement(bool black, bool targetFirst) {
    char run[80]{};
    std::snprintf(run, sizeof(run), "Human-%s/capture/held-ack/target-first-%u",
                  black ? "P2" : "P1", targetFirst);
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    modules::AsyncThinkWorker worker;
    GatedHintEngine engine;
    runtime::VirtualLitchSession session(config, engine, worker);
    const bool initialized = session.Reset() == VIRTUAL_LITCH_OK &&
        Observe(session, OPENING) && session.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(session, "P2_END_PIECE_WHEEL", "85") &&
        Set(session, "P1_END_PIECE_WHEEL", "0") &&
        Set(session, "P2_END_PIECE_WHEEL", "170") &&
        Set(session, "P1_CHECK_CUE", "false") && Set(session, "P2_CHECK_CUE", "false");
    const char* fen = black ? "6k1/4r3/8/4Q3/8/8/8/4K3 b - - 0 1"
                            : "4k3/8/8/8/4q3/8/4R3/6K1 w - - 0 1";
    bool loaded = false;
    for (uint8_t attempt = 0U; initialized && attempt < 10U; ++attempt) {
        loaded = session.LoadExternalFen(fen);
        if (loaded) break;
        if (!WaitFor([&] { return !worker.IsActive(); })) break;
    }
    const uint8_t from = black ? 12U : 52U;
    const uint8_t to = black ? 28U : 36U;
    const uint64_t kings = black ? (UINT64_C(1) << 6U) | (UINT64_C(1) << 60U)
                                 : (UINT64_C(1) << 4U) | (UINT64_C(1) << 62U);
    uint64_t occupancy = kings | (UINT64_C(1) << from) | (UINT64_C(1) << to);
    engine.holdHints = true;
    const bool ready = loaded && Observe(session, occupancy) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        WaitFor([&] { return engine.entered.load(std::memory_order_acquire); });
    Check(ready && engine.held, run, "real imported capture position holds asynchronous background analysis");
    if (ready && engine.held) {
        const uint8_t history = session.UndoHistoryMoveCount();
        const uint32_t applies = engine.applies;
        Checkpoint(session, run, "analysis-held", from, "perform-physical-capture");
        if (targetFirst) {
            occupancy &= ~(UINT64_C(1) << to);
            Check(Observe(session, occupancy), run, "human target-first capture stages victim removal");
        }
        occupancy &= ~(UINT64_C(1) << from);
        Check(Observe(session, occupancy), run, "human capture source pickup accepted");
        if (!targetFirst) {
            Check(PieceHasOwner(session, to, !black), run, "uncaptured victim keeps its owner before removal");
            occupancy &= ~(UINT64_C(1) << to);
            Check(Observe(session, occupancy), run, "human source-first capture removes victim");
        }
        occupancy |= UINT64_C(1) << to;
        Check(Observe(session, occupancy), run, "human capturing piece reaches destination");
        Checkpoint(session, run, "capture-settling", to, "wait-for-acknowledgement");
        Check(Sample(session, run, to, black, 2600U), run,
              "every human capture frame preserves mover ownership while asynchronous acknowledgement is held");
        Check(session.State().interaction_phase == static_cast<uint8_t>(runtime::ChessInteractionPhase::AwaitingAction) &&
                  session.State().pending_action_token != 0U && engine.cancels != 0U &&
                  engine.applies == applies && session.UndoHistoryMoveCount() == history, run,
              "held worker leaves exactly one pending action and does not apply capture early");
        Checkpoint(session, run, "acknowledgement-held", to, "release-existing-search-worker");
        // A fresh subscriber forces the same production Home snapshot. This
        // is the publication/reconnect seam, not a simulation of RF transport.
        modules::HomeStateNotificationPublisher reconnected;
        modules::HomeStateNotificationLine line;
        Check(session.TryPrepareHomePublication(reconnected, true, line) &&
                  PieceHasOwner(session, to, black) &&
                  session.State().pending_action_token != 0U, run,
              "fresh Home subscription cannot recolor or consume the pending capture");
        engine.holdHints = false;
        engine.released.store(true, std::memory_order_release);
        Check(WaitFor([&] { return !worker.IsActive(); }) &&
                  session.Advance(1U) == VIRTUAL_LITCH_OK, run,
              "worker quiescence permits the production coordinator acknowledgement");
        Checkpoint(session, run, "capture-acknowledged", to, "next-player-move");
        Check(Sample(session, run, to, black, 1000U) && engine.applies == applies + 1U &&
                  session.UndoHistoryMoveCount() == history + 1U &&
                  session.State().pending_action_token == 0U &&
                  session.State().side_to_move_black != black, run,
              "one real capture commits and all post-acknowledgement frames keep mover identity");
    }
    engine.holdHints = false;
    engine.released.store(true, std::memory_order_release);
    (void)worker.CancelAndJoin(engine);
    Check(WaitFor([&] { return !worker.IsActive(); }), run, "controlled worker is quiescent before teardown");
}

void HumanCastleAfterOppositeTurn() {
    constexpr const char* RUN = "Human-P1/castle-after-P2/rotate-1/swapped-1";
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3;
    config.board_rotate = true;
    config.first_player_is_p2 = true;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    uint64_t occupancy = (UINT64_C(1) << 4U) | (UINT64_C(1) << 15U) |
                         (UINT64_C(1) << 60U) | (UINT64_C(1) << 63U);
    const bool ready = session.Reset() == VIRTUAL_LITCH_OK && Observe(session, OPENING) &&
        session.Advance(200U) == VIRTUAL_LITCH_OK &&
        Set(session, "P2_END_PIECE_WHEEL", "85") &&
        Set(session, "P1_END_PIECE_WHEEL", "0") &&
        Set(session, "P2_END_PIECE_WHEEL", "170") &&
        session.LoadExternalFen("4k3/7p/8/8/8/8/8/4K2R b K - 0 1") &&
        Observe(session, occupancy) && session.Advance(200U) == VIRTUAL_LITCH_OK;
    Check(ready, RUN, "legal castle position begins with the opposite player's turn");
    if (!ready) return;
    occupancy &= ~(UINT64_C(1) << 15U);
    Check(Observe(session, occupancy), RUN, "human P2 lifts h7");
    occupancy |= UINT64_C(1) << 23U;
    Check(Observe(session, occupancy) && Sample(session, RUN, 23U, true, 2600U), RUN,
          "ordinary human quiet move retains its owner through settle and acknowledgement");
    occupancy &= ~(UINT64_C(1) << 60U);
    Check(Observe(session, occupancy), RUN, "P1 king pickup obtains current castling legality");
    occupancy |= UINT64_C(1) << 62U;
    Check(Observe(session, occupancy) && PieceHasOwner(session, 62U, false) &&
              Channel(session.SemanticFrame().board[62U][0U]) == 2U &&
              session.State().required_action == static_cast<uint8_t>(runtime::RequiredPhysicalAction::LiftSecondary), RUN,
          "first king relocation owns its P1 bloom before acknowledgement and immediately prompts the rook");
    Checkpoint(session, RUN, "king-placed-before-rook", 62U, "lift-h1-rook");
    Check(Sample(session, RUN, 62U, false, 300U, 63U), RUN,
          "unacknowledged king placement preserves P1 identity while waiting for rook pickup");
    occupancy &= ~(UINT64_C(1) << 63U);
    Check(Observe(session, occupancy), RUN, "rook pickup follows the common castling plan");
    occupancy |= UINT64_C(1) << 61U;
    Check(Observe(session, occupancy) && Sample(session, RUN, 61U, false, 2600U) &&
              PieceHasOwner(session, 62U, false) && session.UndoHistoryMoveCount() == 2U, RUN,
          "both relocations commit exactly once with consistent P1 ownership");
    Checkpoint(session, RUN, "castling-acknowledged", 61U, "P2-to-move");
}

} // namespace

int main() {
    BlockedPiecePickup(0U);
    BlockedPiecePickup(2000U);
    CpuJourney(true, false, false, false, false, 50U);
    CpuJourney(true, false, false, false, false, 400U);
    CpuJourney(false, false, false, false, false, UINT32_MAX, "none");
    CpuJourney(false, false, false, true, false, UINT32_MAX, "none");
    for (const bool black : {true, false}) {
        for (const bool rotate : {false, true}) {
            for (const bool swapped : {false, true}) CpuJourney(black, rotate, swapped);
        }
        for (const bool targetFirst : {false, true}) CpuJourney(black, false, false, true, targetFirst);
        for (const bool targetFirst : {false, true}) HumanCaptureHeldAcknowledgement(black, targetFirst);
    }
    HumanCastleAfterOppositeTurn();
    std::printf("[COLOR_LIFECYCLE] failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
