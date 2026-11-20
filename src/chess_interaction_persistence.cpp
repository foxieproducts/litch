#include "runtime/chess_interaction_persistence.hpp"

#include <cstring>

#include "chess_interaction_capture.hpp"

namespace runtime {
namespace {

constexpr uint8_t DURABLE_VERSION = 3U;
constexpr uint8_t PREVIOUS_DURABLE_VERSION = 2U;
constexpr uint8_t LEGACY_DURABLE_VERSION = 1U;
constexpr size_t DURABLE_BYTES = 52U;
constexpr size_t LEGACY_DURABLE_BYTES = 53U;

void PutU32(uint8_t* bytes, size_t& cursor, const uint32_t value) {
    for (uint8_t i = 0; i < 4U; ++i) {
        bytes[cursor++] = static_cast<uint8_t>(value >> (i * 8U));
    }
}

void PutU64(uint8_t* bytes, size_t& cursor, const uint64_t value) {
    for (uint8_t i = 0; i < 8U; ++i) {
        bytes[cursor++] = static_cast<uint8_t>(value >> (i * 8U));
    }
}

uint32_t GetU32(const uint8_t* bytes, size_t& cursor) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < 4U; ++i) {
        value |= static_cast<uint32_t>(bytes[cursor++]) << (i * 8U);
    }
    return value;
}

uint64_t GetU64(const uint8_t* bytes, size_t& cursor) {
    uint64_t value = 0;
    for (uint8_t i = 0; i < 8U; ++i) {
        value |= static_cast<uint64_t>(bytes[cursor++]) << (i * 8U);
    }
    return value;
}

char HexDigit(const uint8_t nibble) {
    return static_cast<char>(nibble < 10U ? '0' + nibble
                                         : 'a' + (nibble - 10U));
}

bool ParseHex(const char c, uint8_t& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<uint8_t>(10U + c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<uint8_t>(10U + c - 'A');
        return true;
    }
    return false;
}

bool ValidateDuckPlan(const TaggedInteractionState& phase) {
    const PhysicalTransitionPlan& plan = phase.plan;
    const uint64_t sourceMask = plan.moveFrom < 64U
                                    ? (1ULL << plan.moveFrom)
                                    : 0ULL;
    const uint64_t destinationMask = plan.moveTo < 64U
                                         ? (1ULL << plan.moveTo)
                                         : 0ULL;
    const bool relocationPhase =
        phase.tag == ChessInteractionPhase::DuckRelocation;
    const bool settling = phase.tag == ChessInteractionPhase::Settling;
    const bool awaitingAction =
        phase.tag == ChessInteractionPhase::AwaitingAction;
    const bool actionCorrection =
        phase.tag == ChessInteractionPhase::Correction;
    if ((!relocationPhase && !settling && !awaitingAction && !actionCorrection) ||
        (plan.source != PhysicalTransitionSource::Human &&
         plan.source != PhysicalTransitionSource::Directed) ||
        (plan.source == PhysicalTransitionSource::Directed &&
         (destinationMask == 0ULL || phase.selectedSquare >= 64U)) ||
        plan.relocationCount != 1U || sourceMask == 0ULL ||
        (plan.beforeOccupiedMask & sourceMask) == 0ULL ||
        plan.relocations[0].from != plan.moveFrom ||
        plan.relocations[0].to != plan.moveTo ||
        plan.removalRequired || plan.promotionRequired ||
        plan.restorationRequired || plan.progress.actionAcknowledged ||
        (plan.progress.relocationsLiftedMask & ~0x01U) != 0U ||
        (plan.progress.relocationsPlacedMask & ~0x01U) != 0U) {
        return false;
    }
    if (phase.selectedSquare < 64U &&
        (phase.selectedSquare != plan.moveTo ||
         phase.selectedSquare == plan.moveFrom ||
         (plan.beforeOccupiedMask &
          (1ULL << phase.selectedSquare)) != 0ULL)) {
        return false;
    }
    if (plan.moveTo < 64U &&
        (plan.moveTo == plan.moveFrom ||
         (plan.beforeOccupiedMask & destinationMask) != 0ULL)) {
        return false;
    }
    if (relocationPhase) {
        const bool validAction =
            phase.requiredAction == RequiredPhysicalAction::LiftPrimary ||
            phase.requiredAction == RequiredPhysicalAction::PlacePrimary ||
            phase.requiredAction == RequiredPhysicalAction::CorrectBoard;
        const bool destinationStateValid =
            phase.selectedSquare < 64U
                ? plan.moveTo == phase.selectedSquare
                : plan.moveTo == CHESS_INTERACTION_INVALID_SQUARE;
        const bool actionProgressValid =
            phase.requiredAction == RequiredPhysicalAction::CorrectBoard ||
            (phase.requiredAction == RequiredPhysicalAction::LiftPrimary &&
             plan.progress.relocationsLiftedMask == 0U) ||
            (phase.requiredAction == RequiredPhysicalAction::PlacePrimary &&
             plan.progress.relocationsLiftedMask == 0x01U);
        return validAction && phase.pendingActionToken == 0U &&
               destinationStateValid && actionProgressValid &&
               plan.progress.relocationsPlacedMask == 0U &&
               plan.afterOccupiedMask == plan.beforeOccupiedMask &&
               ((phase.requiredAction == RequiredPhysicalAction::CorrectBoard) ==
                (phase.correctionMask != 0ULL));
    }

    const uint64_t expectedAfter =
        (plan.beforeOccupiedMask & ~sourceMask) | destinationMask;
    const bool completeProgress =
        plan.progress.relocationsLiftedMask == 0x01U &&
        plan.progress.relocationsPlacedMask == 0x01U;
    if (destinationMask == 0ULL || plan.afterOccupiedMask != expectedAfter ||
        !completeProgress) {
        return false;
    }
    if (settling) {
        return plan.source == PhysicalTransitionSource::Human &&
               phase.selectedSquare == CHESS_INTERACTION_INVALID_SQUARE &&
               phase.requiredAction == RequiredPhysicalAction::None &&
               phase.pendingActionToken == 0U && phase.correctionMask == 0ULL;
    }
    if (awaitingAction) {
        return phase.requiredAction ==
                   RequiredPhysicalAction::AwaitAcknowledgement &&
               phase.pendingActionToken != 0U &&
               phase.correctionMask == 0ULL;
    }
    return phase.requiredAction == RequiredPhysicalAction::CorrectBoard &&
           phase.pendingActionToken == 0U && phase.correctionMask != 0ULL;
}

bool Validate(const ChessInteractionDurableState& state,
              const uint8_t schemaVersion) {
    const bool allowDuck = schemaVersion == DURABLE_VERSION;
    const bool knownSchema = allowDuck ||
                             schemaVersion == PREVIOUS_DURABLE_VERSION ||
                             schemaVersion == LEGACY_DURABLE_VERSION;
    const bool phaseValid =
        state.phase.tag == ChessInteractionPhase::PieceLifted ||
        state.phase.tag == ChessInteractionPhase::CaptureStaged ||
        state.phase.tag == ChessInteractionPhase::ExecutingPlan ||
        state.phase.tag == ChessInteractionPhase::AwaitingAction ||
        state.phase.tag == ChessInteractionPhase::Settling ||
        (allowDuck &&
         state.phase.tag == ChessInteractionPhase::DuckRelocation) ||
        state.phase.tag == ChessInteractionPhase::Correction;
    const uint8_t maxKind = static_cast<uint8_t>(
        allowDuck ? PhysicalTransitionKind::DuckRelocation
                  : PhysicalTransitionKind::Undo);
    if (!knownSchema || state.version != DURABLE_VERSION ||
        !phaseValid ||
        static_cast<uint8_t>(state.phase.requiredAction) >
            static_cast<uint8_t>(RequiredPhysicalAction::CorrectBoard) ||
        static_cast<uint8_t>(state.phase.requiredAction) == 6U ||
        static_cast<uint8_t>(state.phase.plan.kind) > maxKind ||
        static_cast<uint8_t>(state.phase.plan.source) >
            static_cast<uint8_t>(PhysicalTransitionSource::Undo) ||
        state.phase.plan.relocationCount > 2U ||
        state.nextActionToken == 0U ||
        (state.phase.selectedSquare >= 64U &&
         state.phase.selectedSquare != CHESS_INTERACTION_INVALID_SQUARE)) {
        return false;
    }
    if (state.phase.tag == ChessInteractionPhase::AwaitingAction &&
        state.phase.pendingActionToken == 0U) {
        return false;
    }
    if (state.phase.plan.kind != PhysicalTransitionKind::None) {
        const bool duckPlan = state.phase.plan.kind ==
                              PhysicalTransitionKind::DuckRelocation;
        const bool selectableDuckDestination =
            allowDuck && duckPlan &&
            state.phase.tag == ChessInteractionPhase::DuckRelocation &&
            state.phase.plan.moveTo == CHESS_INTERACTION_INVALID_SQUARE;
        if (state.phase.plan.relocationCount == 0U ||
            state.phase.plan.moveFrom >= 64U ||
            (state.phase.plan.moveTo >= 64U &&
             !selectableDuckDestination)) {
            return false;
        }
        const uint8_t validProgressMask = static_cast<uint8_t>(
            (1U << state.phase.plan.relocationCount) - 1U);
        if ((state.phase.plan.progress.relocationsLiftedMask &
             ~validProgressMask) != 0U ||
            (state.phase.plan.progress.relocationsPlacedMask &
             ~validProgressMask) != 0U) {
            return false;
        }
        for (uint8_t i = 0U;
             i < state.phase.plan.relocationCount;
             ++i) {
            const bool selectableRelocation =
                selectableDuckDestination && i == 0U &&
                state.phase.plan.relocations[i].to ==
                    CHESS_INTERACTION_INVALID_SQUARE;
            if (state.phase.plan.relocations[i].from >= 64U ||
                (state.phase.plan.relocations[i].to >= 64U &&
                 !selectableRelocation)) {
                return false;
            }
        }
    }
    if ((state.phase.plan.removalRequired &&
         state.phase.plan.removalSquare >= 64U) ||
        (state.phase.plan.restorationRequired &&
         (state.phase.plan.restorationSquare >= 64U ||
          state.phase.plan.restorationPieceFen == '\0'))) {
        return false;
    }
    return state.phase.plan.kind != PhysicalTransitionKind::DuckRelocation ||
           (allowDuck && ValidateDuckPlan(state.phase));
}

} // namespace

bool HasDurableChessInteractionState(const ChessInteractionModel& model) {
    return model.phase.tag != ChessInteractionPhase::Idle;
}

bool CanSafelyPersistChessInteractionState(const ChessInteractionModel& model) {
    if (model.phase.tag == ChessInteractionPhase::ExecutingPlan &&
        model.phase.plan.progress.actionAcknowledged &&
        model.phase.plan.source != PhysicalTransitionSource::Undo) {
        return false;
    }
    if (model.phase.tag != ChessInteractionPhase::AwaitingAction) {
        return true;
    }
    const auto source = model.phase.plan.source;
    return (source == PhysicalTransitionSource::Human &&
            model.phase.plan.kind == PhysicalTransitionKind::Promotion) ||
           ((source == PhysicalTransitionSource::Human ||
             IsCpuDirectedTransition(model.phase.plan)) &&
            model.phase.plan.kind == PhysicalTransitionKind::DuckRelocation);
}

bool ExportChessInteractionDurableState(
    const ChessInteractionModel& model,
    ChessInteractionDurableState& outState) {
    if (!HasDurableChessInteractionState(model)) {
        outState = ChessInteractionDurableState{};
        return false;
    }
    outState.version = DURABLE_VERSION;
    outState.phase = model.phase;
    outState.nextActionToken = model.nextActionToken;
    return Validate(outState, DURABLE_VERSION);
}

bool RestoreChessInteractionDurableState(
    ChessInteractionModel& model,
    const ChessInteractionDurableState& state) {
    if (!Validate(state, DURABLE_VERSION)) {
        return false;
    }
    ChessInteractionModel candidate = model;
    const uint32_t inputFaultEpoch = candidate.inputFaultEpoch;
    ReduceChessInteraction(
        candidate,
        ChessInteractionEvent::MakeDurableStateRestored(
            state.phase, state.nextActionToken));
    if (candidate.inputFaultEpoch != inputFaultEpoch) {
        return false;
    }
    InvariantReport<8> report;
    CheckChessInteractionModelInvariants(candidate, report);
    if (!report.IsOk()) {
        return false;
    }
    candidate.board.valid = true;
    candidate.board.changedMask = 0ULL;
    candidate.board.occupiedMask =
        ProjectPhysicalOccupancyForInteractionProgress(
        candidate.acknowledged, candidate.phase);
    if (chess_interaction_capture::IsKnownInvalidHumanCapture(
            candidate.acknowledgedCaptureLegality, candidate.phase.plan)) {
        // Retire an impossible provisional capture saved by older firmware
        // while keeping the restored game and its history.
        EnterChessInteractionCorrection(candidate,
            candidate.board.occupiedMask ^ candidate.acknowledged.occupiedMask);
    }
    model = candidate;
    return true;
}

bool EncodeChessInteractionDurableState(const ChessInteractionModel& model,
                                        char* out,
                                        const size_t capacity) {
    ChessInteractionDurableState state;
    if (out == nullptr || capacity < DURABLE_BYTES * 2U + 1U ||
        !ExportChessInteractionDurableState(model, state)) {
        return false;
    }

    uint8_t bytes[DURABLE_BYTES] = {};
    size_t cursor = 0;
    const TaggedInteractionState& phase = state.phase;
    const PhysicalTransitionPlan& plan = phase.plan;
    bytes[cursor++] = state.version;
    bytes[cursor++] = static_cast<uint8_t>(phase.tag);
    bytes[cursor++] = phase.selectedSquare;
    bytes[cursor++] = static_cast<uint8_t>(phase.requiredAction);
    PutU64(bytes, cursor, phase.correctionMask);
    PutU32(bytes, cursor, phase.pendingActionToken);
    PutU32(bytes, cursor, state.nextActionToken);
    bytes[cursor++] = static_cast<uint8_t>(plan.kind);
    bytes[cursor++] = static_cast<uint8_t>(plan.source);
    PutU64(bytes, cursor, plan.beforeOccupiedMask);
    PutU64(bytes, cursor, plan.afterOccupiedMask);
    bytes[cursor++] = plan.moveFrom;
    bytes[cursor++] = plan.moveTo;
    bytes[cursor++] = plan.relocationCount;
    for (const PieceRelocation& relocation : plan.relocations) {
        bytes[cursor++] = relocation.from;
        bytes[cursor++] = relocation.to;
    }
    bytes[cursor++] = static_cast<uint8_t>(
        (plan.removalRequired ? 1U : 0U) |
        (plan.promotionRequired ? 2U : 0U) |
        (plan.restorationRequired ? 4U : 0U));
    bytes[cursor++] = plan.removalSquare;
    bytes[cursor++] = plan.restorationSquare;
    bytes[cursor++] = static_cast<uint8_t>(plan.restorationPieceFen);
    bytes[cursor++] = plan.progress.relocationsLiftedMask;
    bytes[cursor++] = plan.progress.relocationsPlacedMask;
    bytes[cursor++] = static_cast<uint8_t>(
        (plan.progress.removalComplete ? 1U : 0U) |
        (plan.progress.restorationComplete ? 2U : 0U) |
        (plan.progress.actionAcknowledged ? 4U : 0U));
    if (cursor != DURABLE_BYTES) {
        return false;
    }
    for (size_t i = 0; i < DURABLE_BYTES; ++i) {
        out[i * 2U] = HexDigit(static_cast<uint8_t>(bytes[i] >> 4U));
        out[i * 2U + 1U] = HexDigit(static_cast<uint8_t>(bytes[i] & 0x0FU));
    }
    out[DURABLE_BYTES * 2U] = '\0';
    return true;
}

bool DecodeChessInteractionDurableState(
    const char* encoded,
    ChessInteractionDurableState& outState) {
    if (encoded == nullptr) {
        return false;
    }
    const size_t encodedBytes = std::strlen(encoded) / 2U;
    if ((encodedBytes != DURABLE_BYTES &&
         encodedBytes != LEGACY_DURABLE_BYTES) ||
        std::strlen(encoded) != encodedBytes * 2U) {
        return false;
    }
    uint8_t bytes[LEGACY_DURABLE_BYTES] = {};
    for (size_t i = 0; i < encodedBytes; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!ParseHex(encoded[i * 2U], hi) ||
            !ParseHex(encoded[i * 2U + 1U], lo)) {
            return false;
        }
        bytes[i] = static_cast<uint8_t>((hi << 4U) | lo);
    }

    outState = ChessInteractionDurableState{};
    size_t cursor = 0;
    const uint8_t encodedVersion = bytes[cursor++];
    if ((encodedVersion != DURABLE_VERSION &&
         encodedVersion != PREVIOUS_DURABLE_VERSION &&
         encodedVersion != LEGACY_DURABLE_VERSION) ||
        ((encodedVersion == DURABLE_VERSION ||
          encodedVersion == PREVIOUS_DURABLE_VERSION) &&
         encodedBytes != DURABLE_BYTES) ||
        (encodedVersion == LEGACY_DURABLE_VERSION &&
         encodedBytes != LEGACY_DURABLE_BYTES)) {
        return false;
    }
    outState.version = DURABLE_VERSION;
    TaggedInteractionState& phase = outState.phase;
    PhysicalTransitionPlan& plan = phase.plan;
    phase.tag = static_cast<ChessInteractionPhase>(bytes[cursor++]);
    phase.selectedSquare = bytes[cursor++];
    phase.requiredAction = static_cast<RequiredPhysicalAction>(bytes[cursor++]);
    phase.correctionMask = GetU64(bytes, cursor);
    phase.pendingActionToken = GetU32(bytes, cursor);
    outState.nextActionToken = GetU32(bytes, cursor);
    plan.kind = static_cast<PhysicalTransitionKind>(bytes[cursor++]);
    plan.source = static_cast<PhysicalTransitionSource>(bytes[cursor++]);
    // Value 1 predates provider-directed play and therefore always restores as
    // CPU-owned. Provider correlation/origin will be persisted orthogonally by
    // game coordination rather than changing this deployed record.
    plan.directedOrigin = DirectedMoveOrigin::Cpu;
    if (encodedVersion == LEGACY_DURABLE_VERSION) {
        const uint8_t unusedPickupOrder = bytes[cursor++];
        if (unusedPickupOrder > 2U) {
            return false;
        }
    }
    plan.beforeOccupiedMask = GetU64(bytes, cursor);
    plan.afterOccupiedMask = GetU64(bytes, cursor);
    plan.moveFrom = bytes[cursor++];
    plan.moveTo = bytes[cursor++];
    plan.relocationCount = bytes[cursor++];
    for (PieceRelocation& relocation : plan.relocations) {
        relocation.from = bytes[cursor++];
        relocation.to = bytes[cursor++];
    }
    const uint8_t flags = bytes[cursor++];
    if ((flags & ~0x07U) != 0U) {
        return false;
    }
    plan.removalRequired = (flags & 1U) != 0U;
    plan.promotionRequired = (flags & 2U) != 0U;
    plan.restorationRequired = (flags & 4U) != 0U;
    plan.removalSquare = bytes[cursor++];
    plan.restorationSquare = bytes[cursor++];
    plan.restorationPieceFen = static_cast<char>(bytes[cursor++]);
    plan.progress.relocationsLiftedMask = bytes[cursor++];
    plan.progress.relocationsPlacedMask = bytes[cursor++];
    const uint8_t progress = bytes[cursor++];
    if ((progress & ~0x07U) != 0U) {
        return false;
    }
    plan.progress.removalComplete = (progress & 1U) != 0U;
    plan.progress.restorationComplete = (progress & 2U) != 0U;
    plan.progress.actionAcknowledged = (progress & 4U) != 0U;
    // Persisted CPU destination captures may have mistaken the still-present
    // target for an already placed mover when the plan was first queued. The
    // removal progress plus the authoritative boot scan reconstructs the
    // actual state; the seeded placement bit carries no reliable chronology.
    if (phase.tag == ChessInteractionPhase::ExecutingPlan &&
        IsCpuDirectedTransition(plan) &&
        plan.removalRequired) {
        for (uint8_t i = 0U; i < plan.relocationCount; ++i) {
            if (plan.relocations[i].to == plan.removalSquare) {
                plan.progress.relocationsPlacedMask &=
                    static_cast<uint8_t>(~(1U << i));
            }
        }
    }
    return cursor == encodedBytes && Validate(outState, encodedVersion);
}

} // namespace runtime
