#pragma once

#include <cstdint>

#include "foxie_embedded/millis.hpp"
#include "modules/draw_rules.hpp"
#include "modules/engine.hpp"

namespace modules {

struct EngineDerivedState {
    SideToMove sideToMove = SideToMove::White;

    bool inCheck = false;
    bool inCheckmate = false;
    bool inStalemate = false;

    // Draw state (includes stalemate and other forced draws tracked by the coordinator).
    bool inDraw = false;
    DrawReason drawReason = DrawReason::None;
    uint8_t threefoldRepetitionCount = 0;
    uint16_t halfmoveClockPlies = 0;

    // Draw animation timing.
    // When a draw first becomes true, capture the current time so renderers can anchor
    // fade/celebration effects without needing persistent state.
    bool drawStartValid = false;
    uint32_t drawStartMs = 0;

    bool threatenedKingSquareValid = false;
    uint8_t threatenedKingSquare = 0; // 0=a8..63=h1
    uint64_t threatenedKingMask = 0;
    uint64_t checkingPiecesMask = 0;
    bool checkStartValid = false;
    uint32_t checkStartMs = 0;

    // Checkmate animation timing.
    // When checkmate first becomes true, capture the current time so renderers can anchor
    // fade/celebration effects without needing persistent state.
    bool checkmateStartValid = false;
    uint32_t checkmateStartMs = 0;

    void UpdateDrawStartTiming(const uint32_t nowMs) {
        if (inDraw) {
            if (!drawStartValid) {
                drawStartValid = true;
                drawStartMs = nowMs;
            }
        } else {
            drawStartValid = false;
            drawStartMs = 0;
        }
    }

    void Reset() {
        sideToMove = SideToMove::White;
        inCheck = false;
        inCheckmate = false;
        inStalemate = false;

        inDraw = false;
        drawReason = DrawReason::None;
        threefoldRepetitionCount = 0;
        halfmoveClockPlies = 0;
        drawStartValid = false;
        drawStartMs = 0;
        threatenedKingSquareValid = false;
        threatenedKingSquare = 0;
        threatenedKingMask = 0;
        checkingPiecesMask = 0;
        checkStartValid = false;
        checkStartMs = 0;

        checkmateStartValid = false;
        checkmateStartMs = 0;
    }

    void RefreshFromEngine(const IEngine& engine);
};

} // namespace modules
