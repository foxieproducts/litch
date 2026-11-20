#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include <mcu-max.h>

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

uint16_t MoveKey(const mcumax_move mv) {
    const uint8_t from = static_cast<uint8_t>(mv.from);
    const uint8_t to = static_cast<uint8_t>(mv.to);
    return static_cast<uint16_t>((static_cast<uint16_t>(from) << 8) | static_cast<uint16_t>(to));
}

template <size_t N>
bool ReadAndSortMoves(const uint32_t count,
                      const std::array<mcumax_move, N>& buf,
                      std::array<uint16_t, N>& outKeys,
                      uint32_t& outKeyCount) {
    if (count > N) {
        return false;
    }

    outKeyCount = count;
    for (uint32_t i = 0; i < count; ++i) {
        outKeys[i] = MoveKey(buf[i]);
    }

    std::sort(outKeys.begin(), outKeys.begin() + static_cast<size_t>(outKeyCount));
    return true;
}

template <size_t N>
bool KeysEqual(const std::array<uint16_t, N>& a,
               const uint32_t aCount,
               const std::array<uint16_t, N>& b,
               const uint32_t bCount) {
    if (aCount != bCount) {
        return false;
    }

    for (uint32_t i = 0; i < aCount; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }

    return true;
}

void RunOneFen(const char* fen) {
    static constexpr size_t CAP = 512;

    modules::LitchMaxEngine engine;
    engine.Reset();

    const bool setOk = engine.SetFen(fen);
    Check(setOk, "SetFen ok");
    if (!setOk) {
        return;
    }

    std::array<mcumax_move, CAP> fastBuf{};
    std::array<mcumax_move, CAP> legacyBuf{};

    const uint32_t fastCount = mcumax_search_valid_moves_fast(fastBuf.data(), static_cast<uint32_t>(fastBuf.size()));
    const uint32_t legacyCount =
        mcumax_search_valid_moves_legacy(legacyBuf.data(), static_cast<uint32_t>(legacyBuf.size()));

    std::array<uint16_t, CAP> fastKeys{};
    std::array<uint16_t, CAP> legacyKeys{};

    uint32_t fastKeyCount = 0;
    uint32_t legacyKeyCount = 0;

    Check(ReadAndSortMoves(fastCount, fastBuf, fastKeys, fastKeyCount), "read/sort fast keys");
    Check(ReadAndSortMoves(legacyCount, legacyBuf, legacyKeys, legacyKeyCount), "read/sort legacy keys");

    Check(KeysEqual(fastKeys, fastKeyCount, legacyKeys, legacyKeyCount), "fast vs legacy equivalence");

    // Sanity: validate that fast moves are strict-legal according to mcu-max.
    for (uint32_t i = 0; i < fastCount && i < CAP; ++i) {
        Check(mcumax_is_move_legal_strict(fastBuf[i]), "fast move is strict-legal");
    }
}

} // namespace

int main() {
    std::puts("[VALID_MOVES_EQUIVALENCE] start");

    static constexpr const char* FENS[] = {
        // Initial position.
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",

        // Simple midgame.
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",

        // Castling rights present.
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",

        // En passant available (black to move after e2e4).
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",

        // Checkmate: no legal moves.
        "7k/6Q1/7K/8/8/8/8/8 b - - 0 1",

        // Promotion imminent.
        "4k3/4P3/8/8/8/8/8/4K3 w - - 0 1",
    };

    for (const char* fen : FENS) {
        RunOneFen(fen);
    }

    if (g_failures == 0) {
        std::puts("[VALID_MOVES_EQUIVALENCE] ok");
        return 0;
    }

    std::printf("[VALID_MOVES_EQUIVALENCE] failures=%d\n", g_failures);
    return 1;
}
