#include <cstdio>
#include <type_traits>

#include <litch_max/chess_notation.hpp>
#include "modules/coach_insight.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

modules::ClassicThinkResult MakeResult(const char* uci,
                                       const uint8_t bestMoveFacts) {
    litch_max::ParsedUciMove parsed;
    modules::ClassicThinkResult result;
    if (!litch_max::TryParseUciMove(uci, parsed)) {
        return result;
    }
    result.candidates[0] = {
        parsed.fromSquare,
        parsed.toSquare,
        parsed.hasPromotion ? parsed.promotionSuffixLower : '\0',
    };
    result.candidateCount = 1U;
    result.bestScoreValid = true;
    result.bestMoveFacts = bestMoveFacts;
    return result;
}

modules::CoachInsight Build(const char* fen,
                            const modules::SideToMove side,
                            const bool startsInCheck,
                            const char* uci,
                            const uint8_t bestMoveFacts,
                            const char* message) {
    modules::CoachInsight insight;
    Check(modules::TryBuildCoachInsight(
              fen, side, startsInCheck,
              MakeResult(uci, bestMoveFacts), insight),
          message);
    return insight;
}

constexpr uint8_t KNOWN = modules::ClassicThinkOutcomeKnown;
constexpr uint8_t CHECK = static_cast<uint8_t>(
    modules::ClassicThinkOutcomeKnown | modules::ClassicThinkGivesCheck);
constexpr uint8_t MATE = static_cast<uint8_t>(
    modules::ClassicThinkOutcomeKnown | modules::ClassicThinkGivesCheck |
    modules::ClassicThinkGivesMate);

} // namespace

int main() {
    std::puts("[COACH_INSIGHT_SELECTION] start");

    static_assert(std::is_trivially_copyable<modules::CoachInsight>::value,
                  "coach insight must remain a simple fixed-size record");
    static_assert(sizeof(modules::CoachInsight) <= 16U,
                  "coach insight must stay lightweight");

    {
        const modules::CoachInsight insight = Build(
            "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1",
            modules::SideToMove::White, true, "e1e2", KNOWN,
            "escape-check capture builds");
        Check(insight.topic == modules::CoachTopic::EscapeCheck &&
                  insight.priority == modules::CoachPriority::Urgent,
              "escaping check outranks the capture");
        Check((insight.moveFacts & modules::CoachMoveCaptures) != 0U &&
                  insight.capturedPiece == modules::CoachPieceType::Rook,
              "escape-check insight retains the exact capture fact");
    }

    {
        const modules::CoachInsight insight = Build(
            "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1",
            modules::SideToMove::White, false, "f7g7", MATE,
            "mate-in-one builds");
        Check(insight.topic == modules::CoachTopic::MateInOne &&
                  insight.priority == modules::CoachPriority::Urgent,
              "immediate mate is an urgent lesson");
        Check((insight.moveFacts & modules::CoachMoveGivesMate) != 0U &&
                  (insight.moveFacts & modules::CoachMoveGivesCheck) != 0U,
              "mate retains both exact outcome flags");
    }

    {
        const modules::CoachInsight insight = Build(
            "4k3/8/8/8/1Q6/8/8/4K3 w - - 0 1",
            modules::SideToMove::White, false, "b4b5", CHECK,
            "checking move builds");
        Check(insight.topic == modules::CoachTopic::GiveCheck &&
                  insight.priority == modules::CoachPriority::Tactical,
              "check without mate is a tactical lesson");
    }

    {
        const modules::CoachInsight insight = Build(
            "4k3/8/8/8/3q4/2B5/8/4K3 w - - 0 1",
            modules::SideToMove::White, false, "c3d4", KNOWN,
            "ordinary capture builds");
        Check(insight.topic == modules::CoachTopic::Capture &&
                  insight.captureSquare == 35U &&
                  insight.movingPiece == modules::CoachPieceType::Bishop &&
                  insight.capturedPiece == modules::CoachPieceType::Queen,
              "ordinary capture identifies both pieces and target square");
    }

    {
        const modules::CoachInsight insight = Build(
            "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
            modules::SideToMove::White, false, "e5d6", KNOWN,
            "en-passant capture builds");
        Check(insight.topic == modules::CoachTopic::Capture &&
                  insight.captureSquare == 27U &&
                  insight.capturedPiece == modules::CoachPieceType::Pawn,
              "en-passant identifies the off-destination captured pawn");
    }

    {
        const modules::CoachInsight insight = Build(
            "k7/8/8/8/8/8/8/4K2R w K - 0 1",
            modules::SideToMove::White, false, "e1g1", KNOWN,
            "castling move builds");
        Check(insight.topic == modules::CoachTopic::Castle &&
                  insight.priority == modules::CoachPriority::Positional &&
                  (insight.moveFacts & modules::CoachMoveCastles) != 0U,
              "castling is named without claiming a position-specific benefit");
    }

    {
        const modules::CoachInsight insight = Build(
            "8/P7/7k/8/8/8/8/7K w - - 0 1",
            modules::SideToMove::White, false, "a7a8q", KNOWN,
            "promotion builds");
        Check(insight.topic == modules::CoachTopic::Promotion &&
                  insight.promotionPiece == modules::CoachPieceType::Queen,
              "promotion identifies the promoted piece");
    }

    {
        const modules::CoachInsight insight = Build(
            "7k/5P2/6K1/8/8/8/8/8 w - - 0 1",
            modules::SideToMove::White, false, "f7f8q", MATE,
            "promotion mate builds");
        Check(insight.topic == modules::CoachTopic::MateInOne &&
                  (insight.moveFacts & modules::CoachMovePromotes) != 0U,
              "mate outranks promotion while retaining the promotion fact");
    }

    {
        const modules::CoachInsight insight = Build(
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            modules::SideToMove::White, false, "e2e4", KNOWN,
            "general move builds");
        Check(insight.topic == modules::CoachTopic::GeneralMove &&
                  insight.priority == modules::CoachPriority::General,
              "quiet move remains a neutral engine recommendation");
    }

    {
        const modules::CoachInsight insight = Build(
            "4k3/8/8/3q3r/8/8/4N3/4K3 w - - 0 1",
            modules::SideToMove::White, false, "e2f4", KNOWN,
            "tempting false-fork fixture builds");
        Check(insight.topic == modules::CoachTopic::GeneralMove,
              "geometry alone never fabricates a fork claim");
    }

    {
        const modules::CoachInsight insight = Build(
            "4k3/8/8/2p5/3p4/2B5/8/4K3 w - - 0 1",
            modules::SideToMove::White, false, "c3d4", KNOWN,
            "bad apparent capture fixture builds");
        Check(insight.topic == modules::CoachTopic::Capture,
              "capture is observed without claiming it wins material");
    }

    {
        modules::CoachInsight insight;
        modules::ClassicThinkResult result = MakeResult("e2e4", 0U);
        Check(!modules::TryBuildCoachInsight(
                  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                  modules::SideToMove::White, false, result, insight),
              "result without a verified outcome is rejected");
        Check(insight.topic == modules::CoachTopic::None,
              "rejected result clears the output");

        result = MakeResult("e2e4", static_cast<uint8_t>(
                                        modules::ClassicThinkOutcomeKnown | modules::ClassicThinkGivesMate));
        Check(!modules::TryBuildCoachInsight(
                  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                  modules::SideToMove::White, false, result, insight),
              "mate without check is rejected as inconsistent");
        Check(!modules::TryBuildCoachInsight(
                  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                  modules::SideToMove::Black, false,
                  MakeResult("e2e4", KNOWN), insight),
              "side mismatch cannot produce an insight");
        Check(!modules::TryBuildCoachInsight(
                  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                  modules::SideToMove::White, false,
                  MakeResult("e3e4", KNOWN), insight),
              "empty recommendation origin is rejected");
    }

    if (g_failures == 0) {
        std::puts("[COACH_INSIGHT_SELECTION] PASS");
    } else {
        std::printf("[COACH_INSIGHT_SELECTION] FAILURES=%d\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
