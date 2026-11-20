#include "modules/home_state_notification.hpp"

#include <cstring>

#include "modules/chess_strings.hpp"
#include <litch_max/fen_constants.hpp>

namespace modules {
namespace {

bool IsValidAction(const HomeStateAction action) {
    return action >= HomeStateAction::Idle &&
           action <= HomeStateAction::Cpu;
}

bool IsValidWinner(const GameWinner winner) {
    return winner == GameWinner::None || winner == GameWinner::White ||
           winner == GameWinner::Black;
}

bool IsValidPersistenceDegradation(
    const PersistenceDegradation degradation) {
    return degradation >= PersistenceDegradation::None &&
           degradation <= PersistenceDegradation::StorageIo;
}

bool IsJsonSafeFen(const char* const fen) {
    if (fen == nullptr) {
        return false;
    }
    const size_t length = std::strlen(fen);
    if (length == 0U || length >= litch_max::FEN_BUFFER_CAPACITY) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char c = fen[index];
        if (c == '"' || c == '\\' || c == '\r' || c == '\n' ||
            static_cast<unsigned char>(c) < 0x20U) {
            return false;
        }
    }
    return true;
}

bool IsHumanPromotionPrompt(const HomeStateNotification& snapshot) {
    if (!snapshot.physicalMoveValid || snapshot.physicalCaptureSquareValid ||
        (snapshot.action != HomeStateAction::Promote && snapshot.action != HomeStateAction::Correction) ||
        snapshot.placementMode || snapshot.undoPending || snapshot.kingQueenSwapPromptActive ||
        snapshot.freshSetup || snapshot.vexMoveFirst ||
        snapshot.clockWinner != GameWinner::None || snapshot.outcome.IsTerminal() ||
        snapshot.duckRelocationOwner != '\0' || snapshot.fen == nullptr) {
        return false;
    }
    char pawn = '\0';
    if (!TryGetFenPieceAtSquare(snapshot.fen, snapshot.physicalFromSquare, pawn)) return false;
    const char* side = std::strchr(snapshot.fen, ' ');
    const bool white = pawn == 'P' && side != nullptr && side[1] == 'w';
    const bool black = pawn == 'p' && side != nullptr && side[1] == 'b';
    const uint8_t from = snapshot.physicalFromSquare;
    const uint8_t to = snapshot.physicalToSquare;
    if ((!white && !black) || from >= 64U || to >= 64U ||
        from / 8U != (white ? 1U : 6U) || to / 8U != (white ? 0U : 7U)) return false;
    const int fileDelta = static_cast<int>(to % 8U) - static_cast<int>(from % 8U);
    if (fileDelta < -1 || fileDelta > 1) return false;
    char target = '\0';
    const bool occupied = TryGetFenPieceAtSquare(snapshot.fen, to, target);
    return fileDelta == 0 ? !occupied
        : occupied && (white ? target >= 'a' && target <= 'z' : target >= 'A' && target <= 'Z');
}

} // namespace

bool IsValidHomeStateNotification(
    const HomeStateNotification& snapshot) {
    return !(snapshot.sequence == 0U ||
        !IsValidAction(snapshot.action) ||
        !IsValidGameVariant(snapshot.variant) ||
        !IsValidVariantOutcome(snapshot.outcome) ||
        !IsValidWinner(snapshot.clockWinner) ||
        !IsValidPersistenceDegradation(snapshot.persistenceDegradation) ||
        (snapshot.duckLandingRemovalMask != 0ULL &&
         (snapshot.action != HomeStateAction::Correction ||
          snapshot.variant != GameVariant::Duck || snapshot.placementMode ||
          snapshot.kingQueenSwapPromptActive || snapshot.physicalMoveValid ||
          snapshot.duckRelocationOwner != '\0' || snapshot.cpuThinking.side != '\0' ||
          snapshot.clockWinner != GameWinner::None || snapshot.outcome.IsTerminal() ||
          (snapshot.duckLandingRemovalMask & ~snapshot.occupiedMask) != 0ULL ||
          snapshot.assistance.presentation == HomeAssistancePresentation::LegalGuide)) ||
        (snapshot.coachingStateValid && snapshot.coachingMask > 3U) ||
        (snapshot.clockWinner != GameWinner::None &&
         snapshot.outcome.reason != VariantOutcomeReason::None) ||
        (snapshot.clockStateValid &&
         (snapshot.clockActiveSide != 'w' &&
          snapshot.clockActiveSide != 'b')) ||
        (snapshot.clockStateValid && snapshot.clockTimerSetting > 6U) ||
        (snapshot.clockStateValid && snapshot.clockSetupState != 'y' &&
         snapshot.clockSetupState != 'b' &&
         snapshot.clockSetupState != 'n') ||
        (snapshot.clockStateValid && !snapshot.clockEnabled &&
         (snapshot.clockRunning || snapshot.clockPaused ||
          snapshot.clockWhiteRemainingSeconds != 0U ||
          snapshot.clockBlackRemainingSeconds != 0U)) ||
        (snapshot.clockStateValid && snapshot.clockPaused &&
         !snapshot.clockRunning) ||
        (snapshot.cpuThinking.side != '\0' &&
         ((snapshot.cpuThinking.side != 'w' && snapshot.cpuThinking.side != 'b') ||
          (snapshot.action != HomeStateAction::Idle &&
           snapshot.action != HomeStateAction::Progress &&
           snapshot.action != HomeStateAction::Correction) ||
          snapshot.clockWinner != GameWinner::None ||
          snapshot.outcome.reason != VariantOutcomeReason::None ||
          snapshot.physicalMoveValid || snapshot.duckRelocationOwner != '\0')) ||
        (snapshot.kingQueenSwapPromptActive &&
         (snapshot.action != HomeStateAction::Correction || snapshot.placementMode ||
          snapshot.clockWinner != GameWinner::None || snapshot.outcome.IsTerminal())) ||
        (snapshot.physicalMoveValid &&
         (snapshot.physicalFromSquare >= 64U || snapshot.physicalToSquare >= 64U ||
          (snapshot.action != HomeStateAction::Cpu && !IsHumanPromotionPrompt(snapshot) &&
           ((snapshot.action != HomeStateAction::Lift && snapshot.action != HomeStateAction::Settle) ||
            !snapshot.physicalCaptureSquareValid || snapshot.physicalCaptureSquare >= 64U ||
            (snapshot.occupiedMask & (UINT64_C(1) << snapshot.physicalCaptureSquare)) == 0ULL ||
            snapshot.placementMode || snapshot.clockWinner != GameWinner::None ||
            snapshot.outcome.IsTerminal())))) ||
        (snapshot.physicalCaptureSquareValid &&
         (!snapshot.physicalMoveValid || snapshot.physicalCaptureSquare >= 64U)) ||
        (snapshot.promotionSelection != 0U &&
         (snapshot.promotionSelection > 4U || snapshot.action != HomeStateAction::Promote ||
          !IsHumanPromotionPrompt(snapshot) || snapshot.confirmedPromotion != 0U ||
          snapshot.promotionRevealActive || snapshot.promotionAutomatic)) ||
        (snapshot.duckRelocationOwner != '\0' &&
         ((snapshot.action != HomeStateAction::Duck &&
           snapshot.action != HomeStateAction::Correction) ||
          snapshot.variant != GameVariant::Duck ||
          snapshot.outcome.reason != VariantOutcomeReason::None ||
          snapshot.clockWinner != GameWinner::None ||
          snapshot.duckRelocationFromSquare >= 64U ||
          (snapshot.duckRelocationOwner == 'h'
               ? snapshot.duckRelocationToSquare != INVALID_DUCK_SQUARE
               : ((snapshot.duckRelocationOwner != 'c' &&
                   snapshot.duckRelocationOwner != 'u') ||
                  snapshot.duckRelocationToSquare >= 64U ||
                  snapshot.duckRelocationFromSquare ==
                      snapshot.duckRelocationToSquare)))) ||
        (snapshot.whiteLastMoveValid &&
         (snapshot.whiteLastMoveFromSquare >= 64U ||
          snapshot.whiteLastMoveToSquare >= 64U)) ||
        (snapshot.blackLastMoveValid &&
         (snapshot.blackLastMoveFromSquare >= 64U ||
          snapshot.blackLastMoveToSquare >= 64U)) ||
        (snapshot.lastMoveMostRecentSide != '\0' &&
         snapshot.lastMoveMostRecentSide != 'w' &&
         snapshot.lastMoveMostRecentSide != 'b') ||
        (snapshot.lastMoveMostRecentSide == 'w' &&
         !snapshot.whiteLastMoveValid) ||
        (snapshot.lastMoveMostRecentSide == 'b' &&
         !snapshot.blackLastMoveValid) ||
        !IsJsonSafeFen(snapshot.fen));
}

} // namespace modules
