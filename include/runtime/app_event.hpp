#pragma once

#include <cstdint>

#include "modules/board_input.hpp"
#include "modules/user_input.hpp"

namespace runtime {

enum class AppEventType : uint8_t {
    Tick = 0,
    SensorDelta = 1,
    Button = 2,
    BoardObservation = 3,
    PositionAcknowledged = 4,
    ActionResult = 5,
    SearchResult = 6,
    PersistenceResult = 7,
    RestoreResult = 8,
    UndoRequested = 9,
    HintToggleRequested = 10,
    ResetRequested = 11,
};

struct AppEvent {
    uint32_t tMs = 0;
    uint32_t seq = 0;
    AppEventType type = AppEventType::Tick;

    // Payload (interpret based on type). Kept flat to remain trivially constructible.
    uint8_t square = 0;
    bool nowOccupied = false;
    uint8_t deferredDuckDestinationSquare = 255U;
    uint64_t changedMask = 0;
    uint64_t occupiedMask = 0;
    uint64_t kingMask = 0;
    uint64_t rookMask = 0;
    uint64_t pawnMask = 0;
    uint64_t moverMask = 0;
    uint64_t opponentMask = 0;
    uint8_t enPassantTargetSquare = 255U;
    uint8_t enPassantCaptureSquare = 255U;
    bool sideToMoveValid = false;
    bool sideToMoveBlack = false;
    bool legalCaptureTargetsValid = false;
    uint64_t legalCaptureTargetsMask = 0;
    bool legalCastlingTargetsValid = false;
    uint64_t legalCastlingKingTargetsMask = 0;
    uint32_t token = 0;
    uint8_t count = 0;
    bool succeeded = false;
    modules::UserButton button = modules::UserButton::None;
    fxe::Button::Event event = fxe::Button::Event::Press;

    static AppEvent MakeTick(const uint32_t tMs) {
        AppEvent e;
        e.tMs = tMs;
        e.type = AppEventType::Tick;
        return e;
    }

    static AppEvent MakeSensorDelta(const uint32_t tMs, const uint8_t square, const bool nowOccupied) {
        AppEvent e;
        e.tMs = tMs;
        e.type = AppEventType::SensorDelta;
        e.square = square;
        e.nowOccupied = nowOccupied;
        return e;
    }

    static AppEvent MakeBoardObservation(const uint32_t tMs,
                                         const uint64_t changedMask,
                                         const uint64_t occupiedMask,
                                         const uint8_t deferredDuckDestinationSquare = 255U) {
        AppEvent e;
        e.tMs = tMs;
        e.type = AppEventType::BoardObservation;
        e.changedMask = changedMask;
        e.occupiedMask = occupiedMask;
        e.deferredDuckDestinationSquare = deferredDuckDestinationSquare;
        return e;
    }

    static AppEvent MakeButton(const uint32_t tMs, const modules::UserButton button, const fxe::Button::Event event) {
        AppEvent evt{};
        evt.type = AppEventType::Button;
        evt.tMs = tMs;
        evt.button = button;
        evt.event = event;
        return evt;
    }

    static AppEvent MakeUndoRequested(const uint32_t tMs,
                                      const uint8_t count) {
        AppEvent evt{};
        evt.type = AppEventType::UndoRequested;
        evt.tMs = tMs;
        evt.count = count;
        return evt;
    }

    static AppEvent MakeHintToggleRequested(const uint32_t tMs) {
        AppEvent evt{};
        evt.type = AppEventType::HintToggleRequested;
        evt.tMs = tMs;
        return evt;
    }

    static AppEvent MakeResetRequested(const uint32_t tMs) {
        AppEvent evt{};
        evt.type = AppEventType::ResetRequested;
        evt.tMs = tMs;
        return evt;
    }

    static AppEvent MakePositionAcknowledged(const uint32_t tMs,
                                              const uint64_t occupiedMask,
                                              const uint64_t kingMask,
                                              const uint64_t rookMask,
                                              const uint64_t moverMask = 0,
                                              const uint64_t opponentMask = 0,
                                              const uint64_t pawnMask = 0,
                                              const uint8_t enPassantTargetSquare = 255U,
                                              const uint8_t enPassantCaptureSquare = 255U,
                                              const bool sideToMoveValid = false,
                                              const bool sideToMoveBlack = false,
                                              const bool legalCaptureTargetsValid = false,
                                              const uint64_t legalCaptureTargetsMask = 0,
                                              const bool legalCastlingTargetsValid = false,
                                              const uint64_t legalCastlingKingTargetsMask = 0) {
        AppEvent evt{};
        evt.type = AppEventType::PositionAcknowledged;
        evt.tMs = tMs;
        evt.occupiedMask = occupiedMask;
        evt.kingMask = kingMask;
        evt.rookMask = rookMask;
        evt.pawnMask = pawnMask;
        evt.moverMask = moverMask;
        evt.opponentMask = opponentMask;
        evt.enPassantTargetSquare = enPassantTargetSquare;
        evt.enPassantCaptureSquare = enPassantCaptureSquare;
        evt.sideToMoveValid = sideToMoveValid;
        evt.sideToMoveBlack = sideToMoveBlack;
        evt.legalCaptureTargetsValid = legalCaptureTargetsValid;
        evt.legalCaptureTargetsMask = legalCaptureTargetsMask;
        evt.legalCastlingTargetsValid = legalCastlingTargetsValid;
        evt.legalCastlingKingTargetsMask = legalCastlingKingTargetsMask;
        return evt;
    }

    static AppEvent MakeActionResult(const uint32_t tMs,
                                     const uint32_t token,
                                     const bool succeeded) {
        AppEvent evt{};
        evt.type = AppEventType::ActionResult;
        evt.tMs = tMs;
        evt.token = token;
        evt.succeeded = succeeded;
        return evt;
    }

    static AppEvent MakeAdapterResult(const AppEventType type,
                                      const uint32_t tMs,
                                      const uint32_t token,
                                      const bool succeeded) {
        AppEvent evt = MakeActionResult(tMs, token, succeeded);
        evt.type = type;
        return evt;
    }
};
static_assert(sizeof(AppEvent) <= 112U,
              "Queued app events use the scalar envelope shared by Hall and buttons");

} // namespace runtime
