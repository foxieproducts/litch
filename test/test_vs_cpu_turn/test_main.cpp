#include <cstdio>

#include "modules/engine.hpp"
#include "modules/vs_cpu_turn.hpp"

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
    std::puts("[VS_CPU_TURN] start");

    // Disabled always means "not CPU's turn".
    Check(!modules::IsVsCpuTurn(modules::SideToMove::Black, 0, false), "vs_cpu_level=0 is never cpu turn (black)");
    Check(!modules::IsVsCpuTurn(modules::SideToMove::White, 0, false), "vs_cpu_level=0 is never cpu turn (white)");
    Check(!modules::IsVsCpuTurn(modules::SideToMove::Black, 0, true), "vs_cpu_level=0 is never cpu turn (black, first=P2)");
    Check(!modules::IsVsCpuTurn(modules::SideToMove::White, 0, true), "vs_cpu_level=0 is never cpu turn (white, first=P2)");

    // VS-CPU supports assigning the CPU to either logical player. Physical
    // player-end placement never changes the CPU's chess color.
    Check(modules::IsVsCpuTurn(modules::SideToMove::Black, 1, false, true),
          "cpu=P2, first=P1: cpu turn when side=Black");
    Check(modules::IsVsCpuTurn(modules::SideToMove::Black, 1, true, true),
          "cpu=P2, opposite end: cpu remains Black");
    Check(modules::IsVsCpuTurn(modules::SideToMove::White, 1, false, false),
          "cpu=P1, first=P1: cpu turn when side=White");
    Check(modules::IsVsCpuTurn(modules::SideToMove::White, 1, true, false),
          "cpu=P1, opposite end: cpu remains White");
    Check(!modules::IsVsCpuTurn(modules::SideToMove::Black, 1, false, false),
          "cpu=P1, first=P1: human turn when side=Black");
    Check(!modules::IsVsCpuTurn(modules::SideToMove::Black, 1, true, false),
          "cpu=P1, opposite end: human remains Black");

    // Undo rewinds a full human+CPU pair only after the CPU has replied. If
    // the CPU side is to move, the latest ply was the human's and stands alone.
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::White, 0, false) == 1U,
          "PVP undo is one ply");
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::Black, 1, false) == 1U,
          "human-White move awaiting CPU-Black response undoes one ply");
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::White, 1, false) == 2U,
          "CPU-Black reply returns to human White with two plies");
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::Black, 1, true) == 1U,
          "opposite end does not change pending CPU-Black response");
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::White, 1, true) == 2U,
          "opposite end does not change completed CPU-Black cycle");
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::White, 1, false, false) == 1U,
          "CPU-P1 opening position has no committed ply to rewind");
    Check(modules::GetVsCpuUndoPlyCount(modules::SideToMove::Black, 1, false, false) == 2U,
          "CPU-P1 White reply returns to human Black with two plies");

    if (g_failures == 0) {
        std::puts("[VS_CPU_TURN] ok");
        return 0;
    }

    std::printf("[VS_CPU_TURN] failures=%d\n", g_failures);
    return 1;
}
