#include "bounded_hint_engine.hpp"

#include <array>
#include <cstdio>
#include <string>

#include "hardware/board_led_output_mapping.hpp"
#include "modules/chess_strings.hpp"
#include "modules/orientation.hpp"
#include "runtime/chess_interaction_model.hpp"

namespace {

using runtime::ChessInteractionPhase;
using runtime::RequiredPhysicalAction;
using runtime::VirtualLitchSession;
using Side = modules::LitchMaxEngine::PieceSide;

constexpr uint64_t OPENING = UINT64_C(0xffff00000000ffff);
constexpr uint32_t SEED = UINT32_C(0x6c697463);
constexpr unsigned BRANCHES = 4U;
constexpr unsigned POSITIONS = 12U;
constexpr unsigned SEQUENCES_PER_POSITION = 20U;

constexpr uint64_t Bit(const uint8_t square) { return UINT64_C(1) << square; }

virtual_litch_config V33() {
    virtual_litch_config config{};
    virtual_litch_default_config(&config);
    config.hardware = VIRTUAL_LITCH_HARDWARE_V3_3;
    return config;
}

bool Observe(VirtualLitchSession& session, const uint64_t logicalMask) {
    // The firmware dispatches a gameplay Tick before each Hall poll, including
    // the fresh poll after an observed correction restores a partially made move.
    if (session.Advance(10U) != VIRTUAL_LITCH_OK) return false;
    const bool rotate = modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2());
    uint64_t physical = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((logicalMask & Bit(square)) != 0ULL) physical |= Bit(modules::MapSquareToPhysical(square, rotate));
    }
    return session.ObserveHallMask(physical) == VIRTUAL_LITCH_OK &&
           session.State().physical_occupied_mask == logicalMask;
}

bool EmptyPieceDark(const VirtualLitchSession& session, const uint8_t square) {
    const auto dark = [](const virtual_litch_color color) {
        return (color.r | color.g | color.b | color.w) == 0U;
    };
    const auto slot = hardware::SquareLedSlot::Piece;
    const bool rotate = modules::ComputeSquareRotate(session.BoardRotate(), session.FirstPlayerIsP2());
    const auto mapped = hardware::MapSemanticSquareLedTargets(hardware::BoardRevision::V3_3S3, square, slot, rotate);
    return dark(session.SemanticFrame().board[square][static_cast<uint8_t>(slot)]) &&
           mapped.count == 2U && dark(session.PhysicalFrame().pixels[mapped.indices[0U]]) &&
           dark(session.PhysicalFrame().pixels[mapped.indices[1U]]);
}

bool Restore(VirtualLitchSession& session, const uint64_t occupancy) {
    return Observe(session, occupancy) && session.Advance(600U) == VIRTUAL_LITCH_OK &&
           session.State().correction_mask == 0ULL && session.State().human_illegal_mask == 0ULL &&
           session.State().cpu_illegal_mask == 0ULL && session.State().pending_action_token == 0U;
}

bool Open(VirtualLitchSession& session, const uint64_t occupancy, const char* fen = nullptr) {
    return session.Reset() == VIRTUAL_LITCH_OK && (fen == nullptr || session.LoadExternalFen(fen)) &&
           Observe(session, occupancy) && session.Advance(2400U) == VIRTUAL_LITCH_OK &&
           session.State().expected_occupied_mask == occupancy;
}

void DescribeStage(const VirtualLitchSession& session, const char* stage,
                   const char* result, const char* next, const unsigned sequence,
                   const uint8_t mover, const uint8_t squareIndex) {
    const auto& state = session.State();
    const auto square = session.SemanticFrame().board[squareIndex];
    const auto home = session.HomeStateSnapshot();
    const auto display = session.DisplayText();
    std::printf("[MULTI_PIECE_AUDIT] %s=%s seed=%08x sequence=%u mover=%u square=%u "
                "time=%u mask=%016llx expected=%016llx phase=%u action=%u correction=%016llx "
                "history=%u piece=%u/%u/%u rails=%u/%u/%u,%u/%u/%u display=\"%.*s\" fen=%s next=%s\n",
                result, stage, SEED, sequence, mover, squareIndex, state.now_ms,
                static_cast<unsigned long long>(state.physical_occupied_mask),
                static_cast<unsigned long long>(state.expected_occupied_mask), state.interaction_phase,
                state.required_action, static_cast<unsigned long long>(state.correction_mask),
                session.UndoHistoryMoveCount(), square[1U].r, square[1U].g, square[1U].b,
                square[0U].r, square[0U].g, square[0U].b, square[2U].r, square[2U].g, square[2U].b,
                static_cast<int>(display.size()), display.empty() ? "" : display.data(), home.fen, next);
}

bool ExplainFailure(const VirtualLitchSession& session, const char* stage,
                    const unsigned sequence, const uint8_t mover, const uint8_t unrelated) {
    DescribeStage(session, stage, "failure", "inspect-observation", sequence, mover, unrelated);
    return false;
}

bool SameAuthority(const VirtualLitchSession& session, const uint64_t occupancy,
                   const std::string& fen, const unsigned moves) {
    return session.State().expected_occupied_mask == occupancy &&
           session.UndoHistoryMoveCount() == moves && fen == session.HomeStateSnapshot().fen;
}

bool NoFalseCapture(const VirtualLitchSession& session, const uint8_t unrelated) {
    return EmptyPieceDark(session, unrelated) &&
           session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::PlacePrimary) &&
           session.State().pending_action_token == 0U;
}

uint32_t Next(uint32_t& random) {
    random ^= random << 13U;
    random ^= random >> 17U;
    random ^= random << 5U;
    return random;
}

uint8_t Pick(uint64_t mask, uint32_t& random) {
    std::array<uint8_t, 64U> squares{};
    unsigned count = 0U;
    for (uint8_t square = 0U; square < 64U; ++square) {
        if ((mask & Bit(square)) != 0ULL) squares[count++] = square;
    }
    return count == 0U ? 64U : squares[Next(random) % count];
}

bool OpenSeed(VirtualLitchSession& session, unsigned branch, uint64_t& occupied) {
    if (branch == 0U) {
        occupied = OPENING;
        return Open(session, occupied);
    }
    // Generate the two developed seeds from legal game histories. Import only
    // their resulting FEN; every sampled disturbance and later ply uses Hall.
    constexpr const char* SPANISH[]{"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6",
        "e1g1", "f8e7", "f1e1", "b7b5", "a4b3", "d7d6", "c2c3", "e8g8", "h2h3"};
    constexpr const char* SICILIAN[]{"e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4", "f3d4", "g8f6",
        "b1c3", "a7a6", "c1e3", "e7e5", "d4b3", "c8e6"};
    char developed[128]{};
    const char* fen = "r5k1/8/8/3n4/8/3B4/8/R5K1 b - - 0 1";
    if (branch != 3U) {
        modules::LitchMaxEngine seed;
        seed.Reset();
        const char* const* line = branch == 1U ? SPANISH : SICILIAN;
        for (size_t ply = 0U; ply < (branch == 1U ? std::size(SPANISH) : std::size(SICILIAN)); ++ply) {
            if (!seed.ApplyUciMove(line[ply])) return false;
        }
        if (!seed.GetFen(developed, sizeof(developed))) return false;
        fen = developed;
    }
    occupied = 0ULL;
    for (uint8_t square = 0U; square < 64U; ++square) {
        char piece = '\0';
        if (modules::TryGetFenPieceAtSquare(fen, square, piece) && piece != '\0') occupied |= Bit(square);
    }
    return Open(session, occupied, fen);
}

bool SeededPositions(uint32_t& random, const unsigned branch) {
    auto config = V33();
    config.board_rotate = (branch & 1U) != 0U;
    config.first_player_is_p2 = (branch & 2U) != 0U;
    test_support::BoundedHintSession fixture(config);
    auto& session = fixture.session;
    uint64_t occupied = 0ULL;
    unsigned completed = branch * POSITIONS * SEQUENCES_PER_POSITION;
    if (!OpenSeed(session, branch, occupied)) return ExplainFailure(session, "seeded-open", completed, 52U, 12U);
    std::printf("[MULTI_PIECE_AUDIT] root=%u rotate=%u first_p2=%u fen=%s\n", branch,
                unsigned(session.BoardRotate()), unsigned(session.FirstPlayerIsP2()), session.HomeStateSnapshot().fen);
    for (unsigned position = 0U; position < POSITIONS; ++position) {
        std::array<uint64_t, 64U> legal{};
        uint64_t movers = 0ULL, opposing = 0ULL, quietMovers = 0ULL;
        const Side movingSide = session.State().side_to_move_black ? Side::Black : Side::White;
        const auto home = session.HomeStateSnapshot();
        const std::string fen(home.fen);
        const unsigned moves = session.UndoHistoryMoveCount();
        for (uint8_t square = 0U; square < 64U; ++square) {
            if ((occupied & Bit(square)) == 0ULL) continue;
            if (fixture.engine.GetPieceSideAtSquare(square) != movingSide) {
                opposing |= Bit(square);
                continue;
            }
            if (!fixture.engine.GetLegalDestinationsMask(square, legal[square])) {
                return ExplainFailure(session, "seeded-legal-query", completed, square, square);
            }
        }
        std::array<uint64_t, 64U> quiet{};
        for (uint8_t square = 0U; square < 64U; ++square) {
            if (legal[square] != 0ULL && (opposing & ~legal[square]) != 0ULL) movers |= Bit(square);
            if ((legal[square] & ~occupied) == 0ULL) continue;
            char piece = '\0';
            if (!modules::TryGetFenPieceAtSquare(fen.c_str(), square, piece)) {
                return ExplainFailure(session, "seeded-piece-query", completed, square, square);
            }
            uint64_t plainLandings = ~occupied;
            if (piece == 'P' || piece == 'p') {
                const int toward = static_cast<int>(square) + (piece == 'P' ? -8 : 8);
                plainLandings &= toward >= 8 && toward < 56 ? Bit(static_cast<uint8_t>(toward)) : 0ULL;
            } else if (piece == 'K' || piece == 'k') {
                if (square % 8U >= 2U) plainLandings &= ~Bit(square - 2U);
                if (square % 8U <= 5U) plainLandings &= ~Bit(square + 2U);
            }
            // Named journeys own castling, promotion and EP. All ordinary
            // piece types, including a king's quiet step, can progress here.
            quiet[square] = legal[square] & plainLandings;
            if (quiet[square] != 0ULL) quietMovers |= Bit(square);
        }
        if (movers == 0ULL || quietMovers == 0ULL) {
            return ExplainFailure(session, "seeded-position-exhausted", completed, 0U, 0U);
        }
        for (unsigned sample = 0U; sample < SEQUENCES_PER_POSITION; ++sample, ++completed) {
            const uint8_t mover = Pick(movers, random);
            const uint8_t opponent = Pick(opposing & ~legal[mover], random);
            const uint8_t extra = Pick(~occupied & ~legal[mover], random);
            const uint64_t held = occupied & ~Bit(mover) & ~Bit(opponent);
            const bool coalesced = (sample & 1U) != 0U;
            if ((!coalesced && !Observe(session, occupied & ~Bit(mover))) || !Observe(session, held)) {
                return ExplainFailure(session, "seeded-lifts", completed, mover, opponent);
            }
            const unsigned timedSamples = (sample % 4U) == 0U ? 5U : 3U;
            constexpr uint32_t gaps[]{0U, 20U, 279U, 1U, 1200U};
            for (unsigned probe = 0U; probe < timedSamples; ++probe) {
                const uint32_t delta = gaps[probe];
                if (session.Advance(delta) != VIRTUAL_LITCH_OK || !NoFalseCapture(session, opponent) ||
                    !SameAuthority(session, occupied, fen, moves)) {
                    return ExplainFailure(session, "seeded-false-landing", completed, mover, opponent);
                }
            }
            if ((sample & 2U) != 0U) {
                if (extra >= 64U || !Observe(session, held | Bit(extra)) || !NoFalseCapture(session, opponent)) {
                    return ExplainFailure(session, "seeded-extra-placement", completed, mover, opponent);
                }
                if ((sample & 8U) != 0U) {
                    const uint8_t another = Pick(~occupied & ~legal[mover] & ~Bit(extra), random);
                    if (another >= 64U || !Observe(session, held | Bit(another)) ||
                        !NoFalseCapture(session, opponent) || !EmptyPieceDark(session, extra)) {
                        return ExplainFailure(session, "seeded-extra-relocation", completed, mover, opponent);
                    }
                }
                if (!Observe(session, held) || !SameAuthority(session, occupied, fen, moves)) {
                    return ExplainFailure(session, "seeded-extra-removal", completed, mover, opponent);
                }
            }
            const uint64_t firstReturn = held | Bit((sample & 4U) != 0U ? mover : opponent);
            if (!Observe(session, firstReturn) || !Restore(session, occupied) ||
                !SameAuthority(session, occupied, fen, moves) ||
                session.State().interaction_phase != static_cast<uint8_t>(ChessInteractionPhase::Idle)) {
                return ExplainFailure(session, "seeded-recovery", completed, mover, opponent);
            }
        }
        if (position + 1U == POSITIONS) break;
        const uint8_t source = Pick(quietMovers, random);
        const uint8_t target = Pick(quiet[source], random);
        const uint64_t next = (occupied & ~Bit(source)) | Bit(target);
        if (!Observe(session, occupied & ~Bit(source)) || !Observe(session, next) ||
            session.Advance(2500U) != VIRTUAL_LITCH_OK || session.UndoHistoryMoveCount() != moves + 1U ||
            session.State().expected_occupied_mask != next) {
            return ExplainFailure(session, "seeded-legal-progression", completed, source, target);
        }
        occupied = next;
    }
    return true;
}

bool ExactCaptureSource() {
    test_support::BoundedHintSession fixture(V33());
    auto& session = fixture.session;
    constexpr auto fen = "4k3/8/8/8/3p4/8/3R3P/4K3 w - - 0 1";
    constexpr uint8_t H2 = 55U, D2 = 51U, D4 = 35U;
    constexpr uint64_t base = Bit(4U) | Bit(60U) | Bit(H2) | Bit(D2) | Bit(D4);
    if (!Open(session, base, fen)) return ExplainFailure(session, "capture-exact-open", 0U, H2, D4);
    const std::string acknowledged(session.HomeStateSnapshot().fen);
    for (const bool victimFirst : {false, true}) {
        const uint64_t firstLift = base & ~Bit(victimFirst ? D4 : H2);
        if (!Observe(session, firstLift) ||
            (victimFirst && session.State().interaction_phase != static_cast<uint8_t>(ChessInteractionPhase::CaptureStaged)) ||
            !Observe(session, firstLift & ~Bit(victimFirst ? H2 : D4)) || !NoFalseCapture(session, D4)) {
            return ExplainFailure(session, victimFirst ? "false-victim-first" : "false-mover-first", 0U, H2, D4);
        }
        if (victimFirst) {
            if (session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::CorrectBoard) ||
                ((session.State().correction_mask | session.State().human_illegal_mask) & Bit(H2)) == 0ULL ||
                session.Advance(320U) != VIRTUAL_LITCH_OK) {
                return ExplainFailure(session, "victim-first-wrong-source-next-action", 0U, H2, H2);
            }
            const auto amber = [](virtual_litch_color color) { return color.r > color.g && color.g > color.b; };
            const auto& warning = session.SemanticFrame().board[H2];
            if (!amber(warning[0U]) || !amber(warning[2U]) || !EmptyPieceDark(session, H2)) {
                return ExplainFailure(session, "victim-first-wrong-source-rails", 0U, H2, H2);
            }
            DescribeStage(session, "victim-first-wrong-source", "correction", "return-h2-and-d4", 0U, H2, H2);
        }
        if (!Restore(session, base) || !SameAuthority(session, base, acknowledged, 0U)) {
            return ExplainFailure(session, "wrong-source-recovery", 0U, H2, D4);
        }
    }
    const uint64_t rookLift = base & ~Bit(D2);
    if (!Observe(session, rookLift) || !Observe(session, rookLift & ~Bit(D4)) ||
        session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::PlacePrimary) ||
        EmptyPieceDark(session, D4)) {
        return ExplainFailure(session, "legal-d2-d4-landing", 0U, D2, D4);
    }
    DescribeStage(session, "legal-d2-d4-landing", "accepted", "place-White-rook-on-d4", 0U, D2, D4);
    if (!Observe(session, rookLift) ||
        session.Advance(2500U) != VIRTUAL_LITCH_OK || session.UndoHistoryMoveCount() != 1U) {
        return ExplainFailure(session, "legal-d2-d4-capture", 0U, D2, D4);
    }
    DescribeStage(session, "legal-d2-d4-committed", "accepted", "Black-to-move", 0U, D2, D4);
    return true;
}

bool CastlingCorrection() {
    test_support::BoundedHintSession fixture(V33());
    auto& session = fixture.session;
    constexpr auto fen = "4k3/8/p7/8/8/8/8/4K2R w K - 0 1";
    constexpr uint8_t E1 = 60U, G1 = 62U, H1 = 63U, F1 = 61U, A6 = 16U;
    constexpr uint64_t base = Bit(4U) | Bit(A6) | Bit(E1) | Bit(H1);
    constexpr uint64_t movedKing = (base & ~Bit(E1)) | Bit(G1);
    if (!Open(session, base, fen) || !Observe(session, base & ~Bit(E1)) || !Observe(session, movedKing) ||
        session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::LiftSecondary)) {
        return ExplainFailure(session, "castle-rook-entry", 0U, E1, A6);
    }
    if (!Observe(session, movedKing & ~Bit(A6)) || !EmptyPieceDark(session, A6) ||
        session.Advance(320U) != VIRTUAL_LITCH_OK || !EmptyPieceDark(session, A6) ||
        session.UndoHistoryMoveCount() != 0U) {
        return ExplainFailure(session, "castle-unrelated-removal", 0U, E1, A6);
    }
    DescribeStage(session, "castle-a6-removed", "correction", "return-Black-pawn-to-a6", 0U, E1, A6);
    if (!Observe(session, movedKing) ||
        session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::LiftSecondary)) {
        return ExplainFailure(session, "castle-restore-then-lift-rook", 0U, E1, A6);
    }
    DescribeStage(session, "castle-a6-restored", "recovered", "lift-h1-rook-then-place-f1", 0U, E1, H1);
    if (!Observe(session, movedKing & ~Bit(H1))) {
        return ExplainFailure(session, "castle-rook-recovery-lift", 0U, E1, H1);
    }
    if (!Observe(session, (movedKing & ~Bit(H1)) | Bit(F1)) ||
        session.Advance(2500U) != VIRTUAL_LITCH_OK || session.UndoHistoryMoveCount() != 1U) {
        return ExplainFailure(session, "castle-rook-recovery-commit", 0U, E1, F1);
    }
    DescribeStage(session, "castle-committed", "accepted", "Black-to-move", 0U, E1, F1);
    return true;
}

bool EnPassantCorrection() {
    test_support::BoundedHintSession fixture(V33());
    auto& session = fixture.session;
    constexpr auto fen = "4k3/p7/8/3pP3/8/8/8/4K3 w - d6 0 2";
    constexpr uint8_t E5 = 28U, D5 = 27U, D6 = 19U, A7 = 8U;
    constexpr uint64_t base = Bit(4U) | Bit(60U) | Bit(E5) | Bit(D5) | Bit(A7);
    constexpr uint64_t sourceHeld = base & ~Bit(E5);
    if (!Open(session, base, fen) || !Observe(session, sourceHeld) ||
        !Observe(session, sourceHeld & ~Bit(A7)) || !NoFalseCapture(session, A7) ||
        !Restore(session, base) || session.UndoHistoryMoveCount() != 0U ||
        !Observe(session, sourceHeld) || !Observe(session, sourceHeld & ~Bit(D5)) ||
        session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::PlacePrimary) ||
        !EmptyPieceDark(session, D5) || !Observe(session, (sourceHeld & ~Bit(D5)) | Bit(D6)) ||
        session.Advance(2500U) != VIRTUAL_LITCH_OK || session.UndoHistoryMoveCount() != 1U) {
        return ExplainFailure(session, "en-passant-correction", 0U, E5, A7);
    }
    return true;
}

bool PromotionCorrection() {
    test_support::BoundedHintSession fixture(V33());
    auto& session = fixture.session;
    constexpr auto fen = "7k/P5p1/8/8/8/8/8/4K3 w - - 0 1";
    constexpr uint8_t A7 = 8U, G7 = 14U, A8 = 0U;
    constexpr uint64_t base = Bit(7U) | Bit(60U) | Bit(A7) | Bit(G7);
    constexpr uint64_t sourceHeld = base & ~Bit(A7);
    if (!Open(session, base, fen) || !Observe(session, sourceHeld) ||
        !Observe(session, sourceHeld & ~Bit(G7)) || !NoFalseCapture(session, G7) ||
        !Restore(session, base) || session.UndoHistoryMoveCount() != 0U ||
        !Observe(session, sourceHeld) || !Observe(session, sourceHeld | Bit(A8)) ||
        session.Advance(2500U) != VIRTUAL_LITCH_OK || session.UndoHistoryMoveCount() != 1U ||
        session.HomeStateSnapshot().fen[0] != 'Q') {
        return ExplainFailure(session, "promotion-correction", 0U, A7, G7);
    }
    return true;
}

bool HintCorrection() {
    test_support::BoundedHintSession fixture(V33());
    auto& session = fixture.session;
    using Button = runtime::VirtualLitchPhysicalButton;
    uint8_t from = 64U, to = 64U;
    constexpr uint8_t A7 = 8U;
    if (!Open(session, OPENING) || session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::O, true) != VIRTUAL_LITCH_OK ||
        session.Advance(20U) != VIRTUAL_LITCH_OK ||
        session.SetButtonDown(VIRTUAL_LITCH_OWNER_P1, Button::O, false) != VIRTUAL_LITCH_OK ||
        session.Advance(20U) != VIRTUAL_LITCH_OK) {
        return ExplainFailure(session, "hint-ingress", 0U, from, A7);
    }
    for (uint8_t poll = 0U; poll < 30U &&
         (!session.HintDisplayRequested() || !session.TryGetHintMoveSquares(from, to)); ++poll) {
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) break;
    }
    if (from >= 64U || to >= 64U || to == A7 || !session.HintDisplayRequested() ||
        !Observe(session, OPENING & ~Bit(from)) || !Observe(session, OPENING & ~Bit(from) & ~Bit(A7)) ||
        !NoFalseCapture(session, A7) || session.Advance(320U) != VIRTUAL_LITCH_OK || !NoFalseCapture(session, A7)) {
        return ExplainFailure(session, "hint-unrelated-removal", 0U, from, A7);
    }
    DescribeStage(session, "hint-a7-removed", "correction", "return-a7-and-hint-source", 0U, from, A7);
    if (!Restore(session, OPENING) || session.UndoHistoryMoveCount() != 0U) {
        return ExplainFailure(session, "hint-unrelated-recovery", 0U, from, A7);
    }
    DescribeStage(session, "hint-occupancy-restored", "recovered",
                  session.HintDisplayRequested() ? "follow-visible-Hint-or-play" : "play-or-ask-Hint",
                  0U, from, to);
    return true;
}

bool CpuCaptureCorrection() {
    test_support::BoundedHintSession fixture(V33());
    auto& session = fixture.session;
    constexpr auto fen = "4k3/p7/8/8/3q4/3R4/P7/4K3 b - - 0 1";
    constexpr uint8_t D4 = 35U, D3 = 43U, A2 = 48U;
    constexpr uint64_t base = Bit(4U) | Bit(8U) | Bit(D4) | Bit(D3) | Bit(A2) | Bit(60U);
    modules::ControlMutationOutcome outcome;
    if (session.Reset() != VIRTUAL_LITCH_OK || session.MutateSettingJson(940U, "VS_CPU_LEVEL", "1", outcome) != VIRTUAL_LITCH_OK ||
        outcome.status != modules::ControlMutationStatus::Applied || !session.LoadExternalFen(fen) || !Observe(session, base)) {
        return ExplainFailure(session, "cpu-capture-entry", 0U, D4, A2);
    }
    for (uint8_t poll = 0U; poll < 40U && !session.HomeStateSnapshot().physicalMoveValid; ++poll) {
        if (session.Advance(100U) != VIRTUAL_LITCH_OK) break;
    }
    const auto home = session.HomeStateSnapshot();
    const uint64_t lifted = base & ~Bit(D4);
    if (!home.physicalMoveValid || home.physicalFromSquare != D4 || home.physicalToSquare != D3 ||
        !Observe(session, lifted) || session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::RemoveCapture)) {
        return ExplainFailure(session, "cpu-capture-victim-pending", 0U, D4, A2);
    }
    const auto rail = session.SemanticFrame().board[A2][0U];
    if (!Observe(session, lifted & ~Bit(A2)) || session.Advance(20U) != VIRTUAL_LITCH_OK ||
        !EmptyPieceDark(session, A2) || (session.State().cpu_illegal_mask & Bit(A2)) == 0ULL) {
        return ExplainFailure(session, "cpu-capture-unrelated-removal", 0U, D4, A2);
    }
    const auto quiet = session.SemanticFrame().board[A2][0U];
    if (quiet.r != rail.r || quiet.g != rail.g || quiet.b != rail.b || session.Advance(300U) != VIRTUAL_LITCH_OK) {
        return ExplainFailure(session, "cpu-capture-unrelated-quiet-color", 0U, D4, A2);
    }
    const auto amber = session.SemanticFrame().board[A2][0U];
    if (!(amber.r > amber.g && amber.g > amber.b) || !EmptyPieceDark(session, A2) ||
        session.UndoHistoryMoveCount() != 0U) {
        return ExplainFailure(session, "cpu-capture-unrelated-amber", 0U, D4, A2);
    }
    DescribeStage(session, "cpu-capture-a2-removed", "correction", "return-White-pawn-to-a2", 0U, D4, A2);
    if (!Observe(session, lifted) ||
        session.State().required_action != static_cast<uint8_t>(RequiredPhysicalAction::RemoveCapture)) {
        return ExplainFailure(session, "cpu-capture-unrelated-recovery", 0U, D4, A2);
    }
    DescribeStage(session, "cpu-capture-a2-restored", "recovered", "remove-White-rook-from-d3", 0U, D4, D3);
    if (!Observe(session, lifted & ~Bit(D3)) || !Observe(session, lifted) ||
        session.Advance(2500U) != VIRTUAL_LITCH_OK || session.UndoHistoryMoveCount() != 1U) {
        return ExplainFailure(session, "cpu-capture-commit", 0U, D4, D3);
    }
    DescribeStage(session, "cpu-capture-committed", "accepted", "White-to-move", 0U, D4, D3);
    return true;
}

} // namespace

bool RunMultiPieceCaptureAudit() {
    if (!ExactCaptureSource() || !CastlingCorrection() || !EnPassantCorrection() ||
        !PromotionCorrection() || !HintCorrection() || !CpuCaptureCorrection()) return false;
    uint32_t random = SEED;
    for (unsigned branch = 0U; branch < BRANCHES; ++branch) {
        if (!SeededPositions(random, branch)) return false;
    }
    std::printf("[MULTI_PIECE_AUDIT] seed=%08x positions=%u sequences=%u named=6 restored=all\n",
                SEED, BRANCHES * POSITIONS, BRANCHES * POSITIONS * SEQUENCES_PER_POSITION);
    return true;
}
