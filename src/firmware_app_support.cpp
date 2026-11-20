#ifdef ARDUINO

#include <Arduino.h>

#include <cstring>

#include "firmware/firmware_app_internal.hpp"

#include "hardware/board_led_output_mapping.hpp"
#include "logger.hpp"
#include <litch_max/chess_notation.hpp>
#include "modules/chess_strings.hpp"
#include "modules/rgb.hpp"

namespace {

} // namespace

namespace firmware_internal {

void LogStateTransitionInternal(FirmwareSystem& sys,
                                const char* const before,
                                const char* const after) {
    const runtime::PhysicalTransitionPlan& plan =
        sys.stateMachine.GetChessInteractionModel().phase.plan;
    uint8_t fromSq = plan.moveFrom;
    const uint8_t toSq = plan.moveTo;
    (void)sys.stateMachine.TryGetHumanPieceInHandSquare(fromSq);

    uint8_t capSq = 255;
    const bool capValid = sys.stateMachine.TryGetPendingCaptureSquare(capSq);

    char fromBuf[3] = {0};
    char toBuf[3] = {0};
    char capBuf[3] = {0};
    modules::FormatSquareOrUnknown(fromSq, fromBuf);
    modules::FormatSquareOrUnknown(toSq, toBuf);

    if (capValid) {
        modules::FormatSquareOrUnknown(capSq, capBuf);
    } else {
        capBuf[0] = '-';
        capBuf[1] = '\0';
    }

    logger::Trace("state: %s->%s from=%s to=%s cap=%s",
                 before,
                 after,
                 fromBuf,
                 toBuf,
                 capBuf);

    if (sys.stateMachine.IsHumanPieceInHand()) {
        const char* fen = sys.gamePersistence.GetAuthoritativeFen();
        if (fen != nullptr && fen[0] != '\0') {
            char piece = '\0';
            (void)modules::TryGetFenPieceAtSquare(fen, fromSq, piece);
            logger::Trace("pickup: %s%s at %s",
                         modules::DescribeFenPieceSide(piece),
                         modules::DescribeFenPieceType(piece),
                         fromBuf);
        }
    }
}

void LogStateTransitionInternalWithSquares(FirmwareSystem& sys,
                                           const char* const before,
                                           const char* const after,
                                           const uint8_t fromSq,
                                           const uint8_t toSq,
                                           const bool capValid,
                                           const uint8_t capSq) {
    char fromBuf[3] = {0};
    char toBuf[3] = {0};
    char capBuf[3] = {0};
    modules::FormatSquareOrUnknown(fromSq, fromBuf);
    modules::FormatSquareOrUnknown(toSq, toBuf);
    if (capValid) {
        modules::FormatSquareOrUnknown(capSq, capBuf);
    } else {
        capBuf[0] = '-';
        capBuf[1] = '\0';
    }

    logger::Trace("state: %s->%s from=%s to=%s cap=%s",
                 before,
                 after,
                 fromBuf,
                 toBuf,
                 capBuf);
    (void)sys;
}

void LogExpanderInterruptsIfAny() {
#if defined(LITCH_HW_VARIANT_S3)
    static uint32_t lastIntCount[4] = {0, 0, 0, 0};
    for (uint8_t i = 0; i < 4U; ++i) {
        const uint32_t now = firmware::g_expanderIntCount[i];
        if (now == lastIntCount[i]) {
            continue;
        }

        const uint32_t delta = now - lastIntCount[i];
        logger::Trace("pca9555: int exp=%u +%lu (total=%lu)",
                      static_cast<unsigned>(i),
                      static_cast<unsigned long>(delta),
                      static_cast<unsigned long>(now));
        lastIntCount[i] = now;
    }
#endif
}


} // namespace firmware_internal

#endif
