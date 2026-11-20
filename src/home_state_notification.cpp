#include "modules/home_state_notification.hpp"

#include <cstdio>
#include <cstring>

#include <litch_max/fen_constants.hpp>

namespace modules {
namespace {

char PersistenceDegradationToken(
    const PersistenceDegradation degradation) {
    switch (degradation) {
        case PersistenceDegradation::QuarantinedRecord:
            return 'q';
        case PersistenceDegradation::StorageIo:
            return 'i';
        case PersistenceDegradation::None:
            return '\0';
    }
    return '\0';
}

char GameVariantToken(const GameVariant variant) {
    switch (variant) {
        case GameVariant::Undecided:
            return 'u';
        case GameVariant::Classic:
            return 'c';
        case GameVariant::Duck:
            return 'd';
    }
    return '?';
}

char OutcomeToken(const VariantOutcomeReason reason) {
    switch (reason) {
        case VariantOutcomeReason::None:
            return 'n';
        case VariantOutcomeReason::KingCaptured:
            return 'k';
        case VariantOutcomeReason::Fowled:
            return 'f';
        case VariantOutcomeReason::Resigned:
            return 'r';
    }
    return '?';
}

char WinnerToken(const GameWinner winner) {
    switch (winner) {
        case GameWinner::None:
            return 'n';
        case GameWinner::White:
            return 'w';
        case GameWinner::Black:
            return 'b';
    }
    return '?';
}

void FormatSquare(const uint8_t square, char (&out)[3]) {
    out[0] = '-';
    out[1] = '-';
    out[2] = '\0';
    if (!IsValidDuckSquare(square)) {
        return;
    }
    out[0] = static_cast<char>('a' + (square % 8U));
    out[1] = static_cast<char>('8' - (square / 8U));
}

} // namespace

const char* HomeStateActionToToken(const HomeStateAction action) {
    switch (action) {
        case HomeStateAction::Idle:
            return "idle";
        case HomeStateAction::Lift:
            return "lift";
        case HomeStateAction::Progress:
            return "progress";
        case HomeStateAction::Duck:
            return "duck";
        case HomeStateAction::Correction:
            return "corr";
        case HomeStateAction::Settle:
            return "settle";
        case HomeStateAction::Promote:
            return "promote";
        case HomeStateAction::Cpu:
            return "cpu";
    }
    return "unknown";
}

uint8_t HomeStateDuckSquareForPresentation(
    const GameVariantStateSnapshot& variant,
    const uint8_t initialDuckCandidateSquare) {
    if (variant.variant == GameVariant::Undecided &&
        IsValidDuckSquare(initialDuckCandidateSquare)) {
        return initialDuckCandidateSquare;
    }
    return variant.duckSquare;
}

bool ShouldPublishHomeStateNotification(
    const GameVariantStateSnapshot& variant,
    const uint64_t physicalOccupancy,
    const bool initialVariantSelectionReversible) {
    return !initialVariantSelectionReversible ||
           variant.variant != GameVariant::Duck ||
           !IsValidDuckSquare(variant.duckSquare) ||
           (physicalOccupancy & (1ULL << variant.duckSquare)) != 0ULL;
}

bool TryFormatHomeStateNotification(
    const HomeStateNotification& snapshot,
    HomeStateNotificationLine& outLine) {
    outLine.Clear();
    if (!IsValidHomeStateNotification(snapshot)) {
        return false;
    }

    char duckSquare[3] = {};
    FormatSquare(snapshot.duckSquare, duckSquare);
    char physicalFromSquare[3] = {};
    char physicalToSquare[3] = {};
    char physicalCaptureSquare[3] = {};
    FormatSquare(snapshot.physicalMoveValid ? snapshot.physicalFromSquare : 255U,
                 physicalFromSquare);
    FormatSquare(snapshot.physicalMoveValid ? snapshot.physicalToSquare : 255U,
                 physicalToSquare);
    FormatSquare(snapshot.physicalCaptureSquareValid
                     ? snapshot.physicalCaptureSquare
                     : 255U,
                 physicalCaptureSquare);
    char whiteLastFrom[3] = {};
    char whiteLastTo[3] = {};
    char blackLastFrom[3] = {};
    char blackLastTo[3] = {};
    FormatSquare(snapshot.whiteLastMoveValid
                     ? snapshot.whiteLastMoveFromSquare
                     : 255U,
                 whiteLastFrom);
    FormatSquare(snapshot.whiteLastMoveValid
                     ? snapshot.whiteLastMoveToSquare
                     : 255U,
                 whiteLastTo);
    FormatSquare(snapshot.blackLastMoveValid
                     ? snapshot.blackLastMoveFromSquare
                     : 255U,
                 blackLastFrom);
    FormatSquare(snapshot.blackLastMoveValid
                     ? snapshot.blackLastMoveToSquare
                     : 255U,
                 blackLastTo);
    char lastMoves[10] = {};
    lastMoves[0] = whiteLastFrom[0];
    lastMoves[1] = whiteLastFrom[1];
    lastMoves[2] = whiteLastTo[0];
    lastMoves[3] = whiteLastTo[1];
    lastMoves[4] = blackLastFrom[0];
    lastMoves[5] = blackLastFrom[1];
    lastMoves[6] = blackLastTo[0];
    lastMoves[7] = blackLastTo[1];
    lastMoves[8] = snapshot.lastMoveMostRecentSide == '\0'
                       ? 'n'
                       : snapshot.lastMoveMostRecentSide;

    char timeoutWinner[10] = {};
    if (snapshot.clockWinner != GameWinner::None) {
        (void)std::snprintf(timeoutWinner,
                            sizeof(timeoutWinner),
                            ",\"tw\":\"%c\"",
                            WinnerToken(snapshot.clockWinner));
    }

    HomeAssistanceExtra assistanceExtra;
    if (!TryFormatHomeAssistanceProjection(
            snapshot.assistance, snapshot.variant, assistanceExtra)) {
        return false;
    }

    const char* const kingQueenSwap =
        snapshot.kingQueenSwapPromptActive ? ",\"kq\":1" : "";
    const auto formatWithExtras = [&](const char* const extras,
                                      const bool includeUndoState,
                                      const bool includeFreshSetup) {
        outLine.Clear();
        const char* const freshSetup =
            includeFreshSetup && snapshot.freshSetup ? ",\"fs\":1" : "";
        const char* const vexMoveFirst =
            snapshot.vexMoveFirst ? ",\"vx\":1" : "";
        if (includeUndoState) {
            return outLine.Format(
                "{\"event\":\"home_state\",\"v\":%u,\"seq\":%lu,"
                "\"a\":\"%s\",\"fen\":\"%s\",\"occ\":\"%016llX\","
                "\"g\":\"%c\",\"duck\":\"%s\",\"out\":\"%c\","
                "\"win\":\"%c\"%s,\"rot\":%u,\"white\":%u,\"pm\":%u%s,"
                "\"up\":%u%s%s%s}",
                static_cast<unsigned>(HOME_STATE_NOTIFICATION_VERSION),
                static_cast<unsigned long>(snapshot.sequence),
                HomeStateActionToToken(snapshot.action), snapshot.fen,
                static_cast<unsigned long long>(snapshot.occupiedMask),
                GameVariantToken(snapshot.variant), duckSquare,
                OutcomeToken(snapshot.outcome.reason),
                WinnerToken(snapshot.outcome.winner),
                timeoutWinner,
                static_cast<unsigned>(snapshot.boardRotated ? 1U : 0U),
                static_cast<unsigned>(snapshot.whiteOwnerIsP2 ? 2U : 1U),
                static_cast<unsigned>(snapshot.placementMode ? 1U : 0U),
                freshSetup,
                static_cast<unsigned>(snapshot.undoPending ? 1U : 0U),
                vexMoveFirst,
                kingQueenSwap,
                extras);
        }
        return outLine.Format(
            "{\"event\":\"home_state\",\"v\":%u,\"seq\":%lu,"
            "\"a\":\"%s\",\"fen\":\"%s\",\"occ\":\"%016llX\","
            "\"g\":\"%c\",\"duck\":\"%s\",\"out\":\"%c\","
            "\"win\":\"%c\"%s,\"rot\":%u,\"white\":%u,\"pm\":%u%s%s%s%s}",
            static_cast<unsigned>(HOME_STATE_NOTIFICATION_VERSION),
            static_cast<unsigned long>(snapshot.sequence),
            HomeStateActionToToken(snapshot.action), snapshot.fen,
            static_cast<unsigned long long>(snapshot.occupiedMask),
            GameVariantToken(snapshot.variant), duckSquare,
            OutcomeToken(snapshot.outcome.reason),
            WinnerToken(snapshot.outcome.winner),
            timeoutWinner,
            static_cast<unsigned>(snapshot.boardRotated ? 1U : 0U),
            static_cast<unsigned>(snapshot.whiteOwnerIsP2 ? 2U : 1U),
            static_cast<unsigned>(snapshot.placementMode ? 1U : 0U),
            freshSetup,
            vexMoveFirst,
            kingQueenSwap,
            extras);
    };

    const bool haveLastMoves =
        snapshot.whiteLastMoveValid || snapshot.blackLastMoveValid;
    fxe::FixedString<16> persistenceExtras;
    if (snapshot.persistenceDegradation !=
        PersistenceDegradation::None) {
        (void)persistenceExtras.Format(
            ",\"ps\":\"%c\"",
            PersistenceDegradationToken(snapshot.persistenceDegradation));
    }

    fxe::FixedString<24> coachingExtras;
    (void)coachingExtras.Assign(persistenceExtras.CStr());
    if (snapshot.coachingStateValid) {
        fxe::FixedString<10> coachingStateExtra;
        (void)coachingStateExtra.Format(
            ",\"co\":%u", static_cast<unsigned>(snapshot.coachingMask));
        coachingExtras += coachingStateExtra;
    }

    fxe::FixedString<96> clockExtras;
    (void)clockExtras.Assign(coachingExtras.CStr());
    if (snapshot.clockStateValid) {
        fxe::FixedString<64> clockStateExtra;
        (void)clockStateExtra.Format(
            ",\"cl\":[%u,%lu,%lu,\"%c\",%u,%u,%u,\"%c\"]",
            static_cast<unsigned>(snapshot.clockEnabled ? 1U : 0U),
            static_cast<unsigned long>(snapshot.clockWhiteRemainingSeconds),
            static_cast<unsigned long>(snapshot.clockBlackRemainingSeconds),
            snapshot.clockActiveSide,
            static_cast<unsigned>(snapshot.clockRunning ? 1U : 0U),
            static_cast<unsigned>(snapshot.clockPaused ? 1U : 0U),
            static_cast<unsigned>(snapshot.clockTimerSetting),
            snapshot.clockSetupState);
        clockExtras += clockStateExtra;
    }

    fxe::FixedString<16> duckRelocationExtra;
    if (snapshot.duckRelocationOwner != '\0') {
        char from[3] = {};
        char to[3] = {};
        FormatSquare(snapshot.duckRelocationFromSquare, from);
        FormatSquare(snapshot.duckRelocationToSquare, to);
        (void)duckRelocationExtra.Format(
            ",\"dr\":\"%c%s%s\"", snapshot.duckRelocationOwner, from, to);
    }
    fxe::FixedString<40> thinkingExtra;
    if (snapshot.cpuThinking.side != '\0') {
        const auto& thinking = snapshot.cpuThinking;
        if (thinking.remainingSeconds == 0U) {
            (void)thinkingExtra.Format(
                ",\"th\":[\"%c\",%lu,null]", thinking.side,
                static_cast<unsigned long>(thinking.elapsedHalfSeconds));
        } else {
            (void)thinkingExtra.Format(
                ",\"th\":[\"%c\",%lu,%u]", thinking.side,
                static_cast<unsigned long>(thinking.elapsedHalfSeconds),
                static_cast<unsigned>(thinking.remainingSeconds));
        }
    }
    // Clock/recovery (96), assistance (32), one of thinking/CPU/Duck (40), history
    // (20): bounded independently of the final 319-byte wire envelope.
    fxe::FixedString<192> extras;
    bool includeThinking = !thinkingExtra.Empty();
    const auto buildExtras = [&](const bool includeAssistance,
                                 const bool includeLastMoves) {
        extras.Clear();
        extras += clockExtras;
        if (includeThinking) {
            extras += thinkingExtra;
        }
        extras += duckRelocationExtra;
        if (includeAssistance) {
            extras += assistanceExtra;
        }
        if (snapshot.physicalMoveValid) {
            fxe::FixedString<32> physicalMoveExtra;
            (void)physicalMoveExtra.Format(
                ",\"c\":[\"%s\",\"%s\",\"%s\"]",
                physicalFromSquare, physicalToSquare, physicalCaptureSquare);
            extras += physicalMoveExtra;
        }
        if (includeLastMoves && haveLastMoves) {
            fxe::FixedString<20> movesExtra;
            (void)movesExtra.Format(",\"m\":\"%s\"", lastMoves);
            extras += movesExtra;
        }
    };

    bool includeAssistance = snapshot.assistance.valid;
    bool includeLastMoves = haveLastMoves;
    buildExtras(includeAssistance, includeLastMoves);
    bool formatted = formatWithExtras(extras.CStr(), true, true);
    if (!formatted && includeLastMoves) {
        includeLastMoves = false;
        buildExtras(includeAssistance, includeLastMoves);
        formatted = formatWithExtras(extras.CStr(), true, true);
    }
    if (!formatted && includeAssistance) {
        includeAssistance = false;
        buildExtras(includeAssistance, includeLastMoves);
        formatted = formatWithExtras(extras.CStr(), true, true);
    }
    if (!formatted && includeThinking) {
        // Thinking is optional presentation. Never trade clock, correction,
        // or selected physical-move authority for a progress estimate.
        includeThinking = false;
        buildExtras(includeAssistance, includeLastMoves);
        formatted = formatWithExtras(extras.CStr(), true, true);
    }
    const bool requiredPromotionPrompt = snapshot.physicalMoveValid &&
        (snapshot.action == HomeStateAction::Promote || snapshot.action == HomeStateAction::Correction);
    if (!formatted && requiredPromotionPrompt) {
        // The pre-move FEN alone would misdirect physical promotion recovery.
        outLine.Clear();
        return false;
    }
    if (!formatted && !extras.Empty()) {
        // A maximum-capacity legacy FEN leaves only one byte under the
        // established 319-byte line bound. Preserve the complete coarse v1
        // snapshot rather than dropping Home state; legal gameplay FENs have
        // ample room for these additive prompt/history hints.
        formatted = formatWithExtras(clockExtras.CStr(), true, true);
    }
    if (!formatted && snapshot.clockStateValid) {
        formatted = formatWithExtras(coachingExtras.CStr(), true, true);
    }
    if (!formatted && snapshot.coachingStateValid) {
        // Coaching is authoritative owner-local state. Drop it only for the
        // same pathological maximum-FEN fallback that preserves the deployed
        // coarse v1 envelope.
        formatted = formatWithExtras(persistenceExtras.CStr(), true, true);
    }
    if (!formatted) {
        // The legacy coarse v1 payload already consumes all but one byte for
        // an artificial 159-character FEN. Preserve that established payload
        // as the final compatibility fallback. Legal gameplay FENs have ample
        // room and always carry the authoritative Undo projection.
        formatted = formatWithExtras(persistenceExtras.CStr(), false, true);
    }
    if (!formatted) {
        // Preserve the original coarse v1 envelope for a pathological maximum
        // FEN. Legal game positions, including Fresh Setup, always fit the
        // lifecycle bit above.
        formatted = formatWithExtras(persistenceExtras.CStr(), false, false);
    }
    if (!formatted) {
        outLine.Clear();
    }
    return formatted;
}

bool HomeStateNotificationPublisher::TryPrepare(
    const HomeStateNotification& snapshot,
    const bool force,
    HomeStateNotificationLine& outLine) {
    const bool changed = !m_hasLast || !MatchesLast(snapshot);
    if (!changed && !force) {
        outLine.Clear();
        return false;
    }

    HomeStateNotification sequenced = snapshot;
    if (changed) {
        sequenced.sequence =
            (m_sequence == UINT32_MAX) ? 1U : m_sequence + 1U;
    } else {
        sequenced.sequence = m_sequence;
    }
    if (!TryFormatHomeStateNotification(sequenced, outLine)) {
        return false;
    }

    if (changed) {
        m_sequence = sequenced.sequence;
        Remember(snapshot);
    }
    return true;
}

bool HomeStateNotificationPublisher::MatchesLast(
    const HomeStateNotification& snapshot) const {
    return snapshot.fen != nullptr &&
           m_lastFen == snapshot.fen &&
           m_last.action == snapshot.action &&
           m_last.occupiedMask == snapshot.occupiedMask &&
           m_last.variant == snapshot.variant &&
           m_last.duckSquare == snapshot.duckSquare &&
           m_last.outcome.reason == snapshot.outcome.reason &&
           m_last.outcome.winner == snapshot.outcome.winner &&
           m_last.clockWinner == snapshot.clockWinner &&
           m_last.boardRotated == snapshot.boardRotated &&
           m_last.whiteOwnerIsP2 == snapshot.whiteOwnerIsP2 &&
           m_last.placementMode == snapshot.placementMode &&
           m_last.freshSetup == snapshot.freshSetup &&
           m_last.kingQueenSwapPromptActive == snapshot.kingQueenSwapPromptActive &&
           m_last.vexMoveFirst == snapshot.vexMoveFirst &&
           m_last.undoPending == snapshot.undoPending &&
           m_last.coachingStateValid == snapshot.coachingStateValid &&
           m_last.coachingMask == snapshot.coachingMask &&
           m_last.assistance == snapshot.assistance &&
           m_last.persistenceDegradation ==
               snapshot.persistenceDegradation &&
           m_last.cpuThinking == snapshot.cpuThinking &&
           m_last.physicalMoveValid == snapshot.physicalMoveValid &&
           m_last.physicalFromSquare == snapshot.physicalFromSquare &&
           m_last.physicalToSquare == snapshot.physicalToSquare &&
           m_last.physicalCaptureSquareValid == snapshot.physicalCaptureSquareValid &&
           m_last.physicalCaptureSquare == snapshot.physicalCaptureSquare &&
           m_last.duckRelocationOwner == snapshot.duckRelocationOwner &&
           m_last.duckRelocationFromSquare ==
               snapshot.duckRelocationFromSquare &&
           m_last.duckRelocationToSquare == snapshot.duckRelocationToSquare &&
           m_last.whiteLastMoveValid == snapshot.whiteLastMoveValid &&
           m_last.whiteLastMoveFromSquare == snapshot.whiteLastMoveFromSquare &&
           m_last.whiteLastMoveToSquare == snapshot.whiteLastMoveToSquare &&
           m_last.blackLastMoveValid == snapshot.blackLastMoveValid &&
           m_last.blackLastMoveFromSquare == snapshot.blackLastMoveFromSquare &&
           m_last.blackLastMoveToSquare == snapshot.blackLastMoveToSquare &&
           m_last.lastMoveMostRecentSide == snapshot.lastMoveMostRecentSide &&
           m_last.clockStateValid == snapshot.clockStateValid &&
           m_last.clockEnabled == snapshot.clockEnabled &&
           m_last.clockRunning == snapshot.clockRunning &&
           m_last.clockPaused == snapshot.clockPaused &&
           m_last.clockActiveSide == snapshot.clockActiveSide &&
           m_last.clockTimerSetting == snapshot.clockTimerSetting &&
           m_last.clockSetupState == snapshot.clockSetupState &&
           m_last.clockWhiteRemainingSeconds ==
               snapshot.clockWhiteRemainingSeconds &&
           m_last.clockBlackRemainingSeconds ==
               snapshot.clockBlackRemainingSeconds;
}

void HomeStateNotificationPublisher::Remember(
    const HomeStateNotification& snapshot) {
    (void)m_lastFen.Assign(snapshot.fen);
    m_last = snapshot;
    m_last.fen = m_lastFen.CStr();
    m_hasLast = true;
}

} // namespace modules
