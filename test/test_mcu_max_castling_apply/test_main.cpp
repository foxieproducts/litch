#include <cstdio>
#include <cstring>

#include "modules/litch_max_engine.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

bool FenStartsWith(const char* fen, const char* prefix) {
    const size_t fenLen = std::strlen(fen);
    const size_t prefixLen = std::strlen(prefix);
    if (fenLen < prefixLen) {
        return false;
    }
    return std::strncmp(fen, prefix, prefixLen) == 0;
}

struct CastleRuleCase {
    const char* name;
    const char* fen;
    uint8_t kingFrom;
    uint8_t kingTo;
    const char* uci;
    bool expectedLegal;
};

void CheckCastleRule(const CastleRuleCase& rule) {
    modules::LitchMaxEngine engine;
    engine.Reset();
    if (!engine.SetFen(rule.fen)) {
        std::printf("[FAIL] %s: SetFen\n", rule.name);
        ++g_failures;
        return;
    }

    uint64_t destinations = 0ULL;
    if (!engine.GetLegalDestinationsMask(
            rule.kingFrom, destinations)) {
        std::printf("[FAIL] %s: legal destinations query\n", rule.name);
        ++g_failures;
        return;
    }
    const bool maskLegal =
        (destinations & (1ULL << rule.kingTo)) != 0ULL;
    if (maskLegal != rule.expectedLegal) {
        std::printf("[FAIL] %s: destination mask legal=%u expected=%u\n",
                    rule.name,
                    maskLegal ? 1U : 0U,
                    rule.expectedLegal ? 1U : 0U);
        ++g_failures;
    }

    const bool applyLegal = engine.ApplyUciMove(rule.uci);
    if (applyLegal != rule.expectedLegal) {
        std::printf("[FAIL] %s: apply legal=%u expected=%u\n",
                    rule.name,
                    applyLegal ? 1U : 0U,
                    rule.expectedLegal ? 1U : 0U);
        ++g_failures;
    }
}

} // namespace

int main() {
    std::puts("[MCU_MAX_CASTLING_APPLY] start");

    modules::LitchMaxEngine engine;
    engine.Reset();

    // Minimal castling position: kings and rooks only.
    Check(engine.SetFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"), "SetFen");

    // Kingside castling: e1g1 => rook h1f1.
    Check(engine.ApplyUciMove("e1g1"), "apply e1g1 (O-O)");

    char fen[128] = {0};
    Check(engine.GetFen(fen, sizeof(fen)), "GetFen after O-O");
    Check(FenStartsWith(fen, "r3k2r/8/8/8/8/8/8/R4RK1"), "fen after O-O");

    // Reset and test queenside castling: e1c1 => rook a1d1.
    engine.Reset();
    Check(engine.SetFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"), "SetFen (reset)");
    Check(engine.ApplyUciMove("e1c1"), "apply e1c1 (O-O-O)");

    std::memset(fen, 0, sizeof(fen));
    Check(engine.GetFen(fen, sizeof(fen)), "GetFen after O-O-O");
    Check(FenStartsWith(fen, "r3k2r/8/8/8/8/8/8/2KR3R"), "fen after O-O-O");

    // The board state machine consumes the engine-derived king destination
    // mask. Exercise the complete castling rule boundary here so permissive
    // physical pickup ordering can never make an illegal castle claimable.
    static constexpr CastleRuleCase RULES[] = {
        {"white kingside legal",
         "k7/8/8/8/8/8/8/4K2R w K - 0 1",
         60U, 62U, "e1g1", true},
        {"white queenside legal",
         "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1",
         60U, 58U, "e1c1", true},
        {"black kingside legal",
         "4k2r/8/8/8/8/8/8/K7 b k - 0 1",
         4U, 6U, "e8g8", true},
        {"black queenside legal",
         "r3k3/8/8/8/8/8/8/4K3 b q - 0 1",
         4U, 2U, "e8c8", true},
        {"white kingside no rights",
         "k7/8/8/8/8/8/8/4K2R w - - 0 1",
         60U, 62U, "e1g1", false},
        {"white queenside no rights",
         "4k3/8/8/8/8/8/8/R3K3 w - - 0 1",
         60U, 58U, "e1c1", false},
        {"white kingside blocked path",
         "k7/8/8/8/8/8/8/4KB1R w K - 0 1",
         60U, 62U, "e1g1", false},
        {"white queenside blocked path",
         "4k3/8/8/8/8/8/8/R2BK3 w Q - 0 1",
         60U, 58U, "e1c1", false},
        {"white kingside starts in check",
         "k3r3/8/8/8/8/8/8/4K2R w K - 0 1",
         60U, 62U, "e1g1", false},
        {"white kingside transit attacked",
         "k4r2/8/8/8/8/8/8/4K2R w K - 0 1",
         60U, 62U, "e1g1", false},
        {"white kingside destination attacked",
         "k5r1/8/8/8/8/8/8/4K2R w K - 0 1",
         60U, 62U, "e1g1", false},
        {"white queenside transit attacked",
         "k2r4/8/8/8/8/8/8/R3K3 w Q - 0 1",
         60U, 58U, "e1c1", false},
        {"white queenside destination attacked",
         "k1r5/8/8/8/8/8/8/R3K3 w Q - 0 1",
         60U, 58U, "e1c1", false},
        {"white kingside wrong corner piece",
         "4k3/8/8/8/8/8/8/4K2B w - - 0 1",
         60U, 62U, "e1g1", false},
    };
    for (const CastleRuleCase& rule : RULES) {
        CheckCastleRule(rule);
    }

    Check(!engine.SetFen("4k3/8/8/8/8/8/8/4K2B w K - 0 1"),
          "FEN claiming kingside castling with a bishop on h1 is rejected");

    engine.Reset();
    Check(engine.SetFen(
              "4k3/8/8/8/8/8/8/4K2R w - - 0 1"),
          "SetFen ordinary rook move");
    Check(engine.ApplyUciMove("h1f1"),
          "ordinary rook move is still a normal legal move");
    std::memset(fen, 0, sizeof(fen));
    Check(engine.GetFen(fen, sizeof(fen)),
          "GetFen after ordinary rook move");
    Check(FenStartsWith(fen, "4k3/8/8/8/8/8/8/4KR2"),
          "ordinary rook move never relocates the king");

    if (g_failures == 0) {
        std::puts("[MCU_MAX_CASTLING_APPLY] ok");
        return 0;
    }

    std::printf("[MCU_MAX_CASTLING_APPLY] failures=%d\n", g_failures);
    return 1;
}
