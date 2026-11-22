#include "modules/home_state_wire_codec.hpp"

#include <algorithm>

#include "foxie_embedded/byte_order.hpp"
#include "modules/game_history_protocol.hpp"

namespace modules::home_state_wire {
namespace {

constexpr uint8_t INVALID_SQUARE = 0xFFU;

uint8_t DuckOwner(const char owner) {
    switch (owner) {
        case 'h': return 1U;
        case 'c': return 2U;
        case 'u': return 3U;
        default: return 0U;
    }
}

uint8_t MostRecentSide(const char side) {
    return side == 'w' ? 1U : side == 'b' ? 2U : 0U;
}

uint8_t ClockSetup(const char setup) {
    return setup == 'y' ? 0U : setup == 'b' ? 1U : 2U;
}

} // namespace

bool Encode(const HomeStateNotification& snapshot, Payload& out) {
    HomeStateNotification validated = snapshot;
    if (validated.sequence == 0U) validated.sequence = 1U;
    HomeAssistanceExtra assistanceValidation;
    if (!IsValidHomeStateNotification(validated) ||
        (validated.classicResult != HomeClassicResult::Ongoing &&
         (validated.classicResult > HomeClassicResult::Draw || validated.variant != GameVariant::Classic ||
          validated.outcome.IsTerminal() || validated.clockWinner != GameWinner::None ||
          validated.cpuThinking.side != '\0' || validated.action != HomeStateAction::Idle)) ||
        (validated.assistance.presentation != HomeAssistancePresentation::LegalGuide &&
         validated.assistance.legalDestinationMask != 0ULL) ||
        !TryFormatHomeAssistanceProjection(
            validated.assistance, validated.variant, assistanceValidation)) {
        return false;
    }

    game_history::Variant positionVariant = game_history::Variant::Classic;
    uint8_t positionDuck = game_history::INVALID_SQUARE;
    if (validated.variant == GameVariant::Duck ||
        (validated.variant == GameVariant::Undecided &&
         IsValidDuckSquare(validated.duckSquare))) {
        positionVariant = game_history::Variant::Duck;
        positionDuck = validated.duckSquare;
    } else if (validated.variant == GameVariant::Classic &&
               IsValidDuckSquare(validated.duckSquare)) {
        return false;
    }

    game_history::Position position;
    if (!game_history::EncodePosition(
            validated.fen, positionVariant, positionDuck, position)) {
        return false;
    }

    Payload encoded{};
    encoded[0U] = validated.promotionSelection != 0U ? PROMOTION_SELECTION_VERSION
        : validated.confirmedPromotion != 0U ? PROMOTION_VERSION : VERSION;
    encoded[1U] = static_cast<uint8_t>(validated.action);
    encoded[2U] = static_cast<uint8_t>(validated.variant);
    encoded[3U] = static_cast<uint8_t>(validated.outcome.reason);
    encoded[4U] = static_cast<uint8_t>(validated.outcome.winner);
    encoded[5U] = static_cast<uint8_t>(validated.clockWinner);
    encoded[6U] =
        static_cast<uint8_t>((validated.boardRotated ? 1U : 0U) |
                             (validated.whiteOwnerIsP2 ? 1U << 1U : 0U) |
                             (validated.placementMode ? 1U << 2U : 0U) |
                             (validated.freshSetup ? 1U << 3U : 0U) |
                             (validated.vexMoveFirst ? 1U << 4U : 0U) |
                             (validated.undoPending ? 1U << 5U : 0U) |
                             (validated.clockStateValid ? 1U << 6U : 0U) |
                             (validated.kingQueenSwapPromptActive ? 1U << 7U : 0U));
    encoded[7U] = static_cast<uint8_t>(
        (validated.coachingMask & 0x03U) |
        (validated.coachingStateValid ? 1U << 2U : 0U) |
        (static_cast<uint8_t>(validated.persistenceDegradation) << 3U) |
        (validated.physicalMoveValid ? 1U << 5U : 0U) |
        (validated.physicalCaptureSquareValid ? 1U << 6U : 0U));
    encoded[8U] = static_cast<uint8_t>(
        DuckOwner(validated.duckRelocationOwner) |
        (validated.whiteLastMoveValid ? 1U << 2U : 0U) |
        (validated.blackLastMoveValid ? 1U << 3U : 0U) |
        (MostRecentSide(validated.lastMoveMostRecentSide) << 4U) |
        (validated.cpuThinking.side != '\0' ? 1U << 6U : 0U) |
        (validated.assistance.valid ? 1U << 7U : 0U));
    encoded[9U] = static_cast<uint8_t>(
        (validated.cpuThinking.side == 'b' ? 1U : 0U) |
        (validated.clockEnabled ? 1U << 1U : 0U) |
        (validated.clockRunning ? 1U << 2U : 0U) |
        (validated.clockPaused ? 1U << 3U : 0U) |
        (validated.clockActiveSide == 'b' ? 1U << 4U : 0U) |
        ((validated.clockTimerSetting & 0x07U) << 5U));
    const uint8_t promotionPiece = validated.promotionSelection != 0U
        ? validated.promotionSelection : validated.confirmedPromotion;
    encoded[10U] = static_cast<uint8_t>(ClockSetup(validated.clockSetupState) |
        (promotionPiece << 2U) |
        (validated.promotionRevealActive ? 1U << 5U : 0U) |
        (validated.promotionAutomatic ? 1U << 6U : 0U));
    std::copy(position.bytes.begin(), position.bytes.end(), encoded.begin() + 11U);
    fxe::WriteU64LE(validated.occupiedMask, encoded.data() + 54U);
    encoded[62U] = validated.physicalMoveValid
        ? validated.physicalFromSquare : INVALID_SQUARE;
    encoded[63U] = validated.physicalMoveValid
        ? validated.physicalToSquare : INVALID_SQUARE;
    encoded[64U] = validated.physicalCaptureSquareValid
        ? validated.physicalCaptureSquare : INVALID_SQUARE;
    encoded[65U] = validated.duckRelocationOwner != '\0'
        ? validated.duckRelocationFromSquare : INVALID_SQUARE;
    encoded[66U] = validated.duckRelocationOwner != '\0'
        ? validated.duckRelocationToSquare : INVALID_SQUARE;
    encoded[67U] = validated.whiteLastMoveValid
        ? validated.whiteLastMoveFromSquare : INVALID_SQUARE;
    encoded[68U] = validated.whiteLastMoveValid
        ? validated.whiteLastMoveToSquare : INVALID_SQUARE;
    encoded[69U] = validated.blackLastMoveValid
        ? validated.blackLastMoveFromSquare : INVALID_SQUARE;
    encoded[70U] = validated.blackLastMoveValid
        ? validated.blackLastMoveToSquare : INVALID_SQUARE;
    // The submitted app ignores CPU timing whenever the thinking flag is clear.
    fxe::WriteU32LE(validated.cpuThinking.side != '\0' ? validated.cpuThinking.elapsedHalfSeconds
                        : static_cast<uint8_t>(validated.classicResult), encoded.data() + 71U);
    fxe::WriteU16LE(validated.cpuThinking.remainingSeconds,
                    encoded.data() + 75U);
    fxe::WriteU32LE(validated.clockWhiteRemainingSeconds,
                    encoded.data() + 77U);
    fxe::WriteU32LE(validated.clockBlackRemainingSeconds,
                    encoded.data() + 81U);
    encoded[85U] = static_cast<uint8_t>(validated.assistance.presentation);
    encoded[86U] = validated.assistance.fromSquare;
    encoded[87U] = validated.assistance.toSquare;
    encoded[88U] = validated.assistance.duckFromSquare;
    encoded[89U] = validated.assistance.duckToSquare;
    encoded[90U] = validated.assistance.legalSourceSquare;
    encoded[91U] = static_cast<uint8_t>(
        (validated.assistance.hintRequested ? 1U : 0U) |
        (validated.assistance.manualAffordanceUseful ? 1U << 1U : 0U) |
        (validated.assistance.coachingForced ? 1U << 2U : 0U));
    // Correction uses the same otherwise idle mask field. Older v1 readers
    // ignore this field outside LegalGuide and retain generic correction.
    fxe::WriteU64LE(validated.duckLandingRemovalMask != 0ULL
                        ? validated.duckLandingRemovalMask
                        : validated.assistance.legalDestinationMask,
                    encoded.data() + 92U);
    out = encoded;
    return true;
}

} // namespace modules::home_state_wire
