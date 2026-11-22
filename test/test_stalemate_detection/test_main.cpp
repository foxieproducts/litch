#include <cstdio>

#include "modules/engine_derived_state.hpp"
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

} // namespace

int main() {
    std::puts("[STALEMATE_DETECTION] start");

#ifndef ARDUINO
    modules::LitchMaxEngine engine;
    engine.Reset();

    // Black to move stalemate (Kh8 vs Kg6 Qf7).
    static constexpr const char* BLACK_STALEMATE_FEN = "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1";
    Check(engine.SetFen(BLACK_STALEMATE_FEN), "SetFen black stalemate");
    Check(engine.GetSideToMove() == modules::SideToMove::Black, "sideToMove black");
    Check(!engine.IsInCheck(modules::SideToMove::Black), "black not in check");
    Check(!engine.IsInCheckmate(modules::SideToMove::Black), "black not in checkmate");
    Check(engine.IsInStalemate(modules::SideToMove::Black), "black is stalemated");

    modules::EngineDerivedState derived;
    derived.Reset();
    derived.RefreshFromEngine(engine);
    Check(derived.sideToMove == modules::SideToMove::Black, "derived sideToMove black");
    Check(!derived.inCheck, "derived not in check (black)");
    Check(!derived.inCheckmate, "derived not in checkmate (black)");
    Check(derived.inStalemate, "derived in stalemate (black)");

    // White to move stalemate (Ka1 vs Kc2 Qb3).
    static constexpr const char* WHITE_STALEMATE_FEN = "8/8/8/8/8/1q6/2k5/K7 w - - 0 1";
    Check(engine.SetFen(WHITE_STALEMATE_FEN), "SetFen white stalemate");
    Check(engine.GetSideToMove() == modules::SideToMove::White, "sideToMove white");
    Check(!engine.IsInCheck(modules::SideToMove::White), "white not in check");
    Check(!engine.IsInCheckmate(modules::SideToMove::White), "white not in checkmate");
    Check(engine.IsInStalemate(modules::SideToMove::White), "white is stalemated");

    derived.Reset();
    derived.RefreshFromEngine(engine);
    Check(derived.sideToMove == modules::SideToMove::White, "derived sideToMove white");
    Check(!derived.inCheck, "derived not in check (white)");
    Check(!derived.inCheckmate, "derived not in checkmate (white)");
    Check(derived.inStalemate, "derived in stalemate (white)");
#else
    std::puts("[STALEMATE_DETECTION] skipped (ARDUINO)");
#endif

    if (g_failures == 0) {
        std::puts("[STALEMATE_DETECTION] ok");
        return 0;
    }

    std::printf("[STALEMATE_DETECTION] failures=%d\n", g_failures);
    return 1;
}
