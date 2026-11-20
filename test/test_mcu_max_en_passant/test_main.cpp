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

} // namespace

int main() {
    std::puts("[MCU_MAX_EN_PASSANT] start");

    modules::LitchMaxEngine engine;
    engine.Reset();

    Check(engine.ApplyUciMove("e2e4"), "apply e2e4");
    Check(engine.ApplyUciMove("a7a6"), "apply a7a6");
    Check(engine.ApplyUciMove("e4e5"), "apply e4e5");
    Check(engine.ApplyUciMove("d7d5"), "apply d7d5");

    // En-passant: after ...d7d5, white pawn on e5 can capture d6.
    Check(engine.ApplyUciMove("e5d6"), "apply e5d6 (en passant)");

    char fen[128] = {0};
    Check(engine.GetFen(fen, sizeof(fen)), "GetFen");

    // Board should reflect: a6 black pawn; d6 white pawn; d5 empty.
    Check(FenStartsWith(fen, "rnbqkbnr/1pp1pppp/p2P4/8/8/8/PPPP1PPP/RNBQKBNR"),
          "fen after en passant");

    if (g_failures == 0) {
        std::puts("[MCU_MAX_EN_PASSANT] ok");
        return 0;
    }

    std::printf("[MCU_MAX_EN_PASSANT] failures=%d\n", g_failures);
    return 1;
}
