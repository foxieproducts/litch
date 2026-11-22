#pragma once

#include <array>
#include <cstdint>

#include "runtime/invariant_checks.hpp"

namespace runtime {

constexpr uint8_t CHESS_INTERACTION_INVALID_SQUARE = 255U;

enum class ChessInteractionPhase : uint8_t {
    Idle = 0,
    PieceLifted = 1,
    CaptureStaged = 2,
    ExecutingPlan = 3,
    AwaitingAction = 4,
    Settling = 5,
    DuckRelocation = 6,
    Correction = 8,
};

enum class PhysicalTransitionKind : uint8_t {
    None = 0,
    QuietMove,
    Capture,
    Castling,
    EnPassant,
    Promotion,
    Undo,
    DuckRelocation,
};

enum class PhysicalTransitionSource : uint8_t {
    Human = 0,
    Directed = 1,
    // Compatibility spelling for deployed value 1. New code should name the
    // source-neutral choreography Directed and inspect directedOrigin when CPU
    // ownership matters.
    Cpu = Directed,
    Undo = 2,
};

enum class DirectedMoveOrigin : uint8_t {
    Cpu = 0,
    Provider,
};

enum class RequiredPhysicalAction : uint8_t {
    None = 0,
    LiftPrimary = 1,
    LiftSecondary = 2,
    RemoveCapture = 3,
    PlacePrimary = 4,
    PlaceSecondary = 5,
    // Durable value 6 is reserved; promotion choice is an orthogonal modal.
    RestoreCapture = 7,
    AwaitAcknowledgement = 8,
    CorrectBoard = 9,
};

struct CaptureLegality {
    uint64_t knownSources = 0;
    std::array<uint64_t, 64U> landingsBySource{};

    void Clear() {
        knownSources = 0ULL;
        landingsBySource.fill(0ULL);
    }
};
static_assert(sizeof(CaptureLegality) == 65U * sizeof(uint64_t),
              "Capture authority stores one row per square and one known-source mask");

struct ChessPositionState {
    uint64_t occupiedMask = 0;
    uint64_t kingMask = 0;
    uint64_t rookMask = 0;
    uint64_t pawnMask = 0;
    uint64_t moverOccupiedMask = 0;
    uint64_t opponentOccupiedMask = 0;
    uint8_t enPassantTargetSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t enPassantCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE;
    bool sideToMoveValid = false;
    bool sideToMoveBlack = false;
    bool legalCaptureTargetsValid = false;
    uint64_t legalCaptureTargetsMask = 0;
    bool legalCastlingTargetsValid = false;
    uint64_t legalCastlingKingTargetsMask = 0;
    uint64_t revision = 0;
};
static_assert(sizeof(ChessPositionState) <= 88U,
              "Projected positions carry only scalar chess facts, not capture rows");

struct AtomicBoardObservationState {
    bool valid = false;
    uint64_t changedMask = 0;
    uint64_t occupiedMask = 0;
    uint32_t timestampMs = 0;
    uint32_t stableSinceMs = 0;
    uint32_t sequence = 0;
};

struct PieceRelocation {
    uint8_t from = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t to = CHESS_INTERACTION_INVALID_SQUARE;
};

struct PhysicalTransitionProgress {
    uint8_t relocationsLiftedMask = 0;
    uint8_t relocationsPlacedMask = 0;
    bool removalComplete = false;
    bool restorationComplete = false;
    bool actionAcknowledged = false;
};

struct PhysicalTransitionPlan {
    PhysicalTransitionKind kind = PhysicalTransitionKind::None;
    PhysicalTransitionSource source = PhysicalTransitionSource::Human;
    DirectedMoveOrigin directedOrigin = DirectedMoveOrigin::Cpu;
    uint64_t beforeOccupiedMask = 0;
    uint64_t afterOccupiedMask = 0;
    uint8_t moveFrom = CHESS_INTERACTION_INVALID_SQUARE;
    uint8_t moveTo = CHESS_INTERACTION_INVALID_SQUARE;
    std::array<PieceRelocation, 2> relocations{};
    uint8_t relocationCount = 0;
    bool removalRequired = false;
    uint8_t removalSquare = CHESS_INTERACTION_INVALID_SQUARE;
    bool promotionRequired = false;
    bool restorationRequired = false;
    uint8_t restorationSquare = CHESS_INTERACTION_INVALID_SQUARE;
    char restorationPieceFen = '\0';
    PhysicalTransitionProgress progress{};
};

static_assert(static_cast<uint8_t>(PhysicalTransitionSource::Directed) == 1U,
              "deployed directed transition value must remain stable");
static_assert(static_cast<uint8_t>(PhysicalTransitionSource::Cpu) == 1U,
              "legacy CPU spelling must remain a value-1 alias");

inline bool IsDirectedTransition(const PhysicalTransitionPlan& plan) {
    return plan.source == PhysicalTransitionSource::Directed;
}

inline bool IsCpuDirectedTransition(const PhysicalTransitionSource source,
                                    const DirectedMoveOrigin origin) {
    return source == PhysicalTransitionSource::Directed &&
           origin == DirectedMoveOrigin::Cpu;
}

inline bool IsCpuDirectedTransition(const PhysicalTransitionPlan& plan) {
    return IsCpuDirectedTransition(plan.source, plan.directedOrigin);
}

inline bool IsProviderDirectedTransition(const PhysicalTransitionPlan& plan) {
    return IsDirectedTransition(plan) &&
           plan.directedOrigin == DirectedMoveOrigin::Provider;
}

struct TaggedInteractionState {
    ChessInteractionPhase tag = ChessInteractionPhase::Idle;
    uint8_t selectedSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint64_t correctionMask = 0;
    RequiredPhysicalAction requiredAction = RequiredPhysicalAction::None;
    uint32_t pendingActionToken = 0;
    PhysicalTransitionPlan plan{};
};

struct ChessInteractionModel {
    ChessPositionState acknowledged{};
    // Exact capture rows belong to this acknowledged position only.
    CaptureLegality acknowledgedCaptureLegality{};
    AtomicBoardObservationState board{};
    TaggedInteractionState phase{};
    uint32_t nextActionToken = 1;
    uint32_t feedbackEpoch = 0;
    uint32_t inputFaultEpoch = 0;
};

enum class InteractionInvariantStatusCode : uint8_t {
    None = 0,
    InvariantViolation,
};

struct InteractionInvariantStatus {
    uint32_t sequence = 0;
    InteractionInvariantStatusCode code = InteractionInvariantStatusCode::None;
    uint8_t violationCount = 0;

    bool IsActive() const {
        return code != InteractionInvariantStatusCode::None;
    }
};

enum class ChessInteractionEventType : uint8_t {
    Reset = 0,
    ResetPreservingPosition,
    DurableStateRestored,
    PlanRequested,
    PlanCanceled,
    UndoCompleted,
    SettleConfirmed,
    SettleProgressRegressed,
    PlanProgressObserved,
    UndoRestorationConfigured,
    BoardObservation,
    DeferredBoardObservation,
    ModalBoardObservation,
    PositionAcknowledged,
    ActionResult,
    Tick,
    CaptureProgressBoardObservation,
};

struct ChessInteractionEvent {
    ChessInteractionEventType type = ChessInteractionEventType::Reset;
    uint32_t timestampMs = 0;
    uint32_t sequence = 0;
    uint8_t square = CHESS_INTERACTION_INVALID_SQUARE;
    // BoardObservation only: ingress identified one uniquely legal chess
    // landing and defers this other landing to the existing Duck owner.
    uint8_t deferredDuckDestinationSquare = CHESS_INTERACTION_INVALID_SQUARE;
    uint64_t changedMask = 0;
    uint64_t occupiedMask = 0;
    uint32_t actionToken = 0;
    bool actionSucceeded = false;
    PhysicalTransitionPlan requestedPlan{};
    TaggedInteractionState restoredPhase{};
    ChessPositionState position{};
    uint32_t restoredNextActionToken = 1U;

    static ChessInteractionEvent MakeResetPreservingPosition();
    static ChessInteractionEvent MakeDurableStateRestored(
        const TaggedInteractionState& phase,
        uint32_t nextActionToken);
    static ChessInteractionEvent MakePlanRequested(const PhysicalTransitionPlan& plan);
    static ChessInteractionEvent MakePlanCanceled(
        uint8_t selectedSquare = CHESS_INTERACTION_INVALID_SQUARE);
    static ChessInteractionEvent MakeUndoCompleted();
    static ChessInteractionEvent MakeSettleConfirmed();
    static ChessInteractionEvent MakeSettleProgressRegressed();
    static ChessInteractionEvent MakePlanProgressObserved(uint64_t occupiedMask);
    static ChessInteractionEvent MakeUndoRestorationConfigured(uint8_t square,
                                                                char pieceFen);
    static ChessInteractionEvent MakeBoardObservation(uint32_t timestampMs,
                                                       uint32_t sequence,
                                                       uint64_t changedMask,
                                                       uint64_t occupiedMask);
    static ChessInteractionEvent MakeDeferredBoardObservation(
        uint32_t timestampMs,
        uint32_t sequence,
        uint64_t changedMask,
                                                       uint64_t occupiedMask);
    static ChessInteractionEvent MakeModalBoardObservation(
        uint32_t timestampMs,
        uint32_t sequence,
        uint64_t changedMask,
        uint64_t occupiedMask);
    // Preserve capture facts during unrelated correction without admitting
    // completion, confirmation or a new mover.
    static ChessInteractionEvent MakeCaptureProgressBoardObservation(
        uint32_t timestampMs,
        uint32_t sequence,
        uint64_t changedMask,
        uint64_t occupiedMask);
    static ChessInteractionEvent MakePositionAcknowledged(uint64_t occupiedMask,
                                                           uint64_t kingMask,
                                                           uint64_t rookMask,
                                                           uint64_t moverOccupiedMask = 0,
                                                           uint64_t opponentOccupiedMask = 0,
                                                           uint64_t pawnMask = 0,
                                                           uint8_t enPassantTargetSquare = CHESS_INTERACTION_INVALID_SQUARE,
                                                           uint8_t enPassantCaptureSquare = CHESS_INTERACTION_INVALID_SQUARE,
                                                           bool sideToMoveValid = false,
                                                           bool sideToMoveBlack = false,
                                                           bool legalCaptureTargetsValid = false,
                                                           uint64_t legalCaptureTargetsMask = 0,
                                                           bool legalCastlingTargetsValid = false,
                                                           uint64_t legalCastlingKingTargetsMask = 0);
    static ChessInteractionEvent MakeActionResult(uint32_t token, bool succeeded);
    static ChessInteractionEvent MakeTick(uint32_t timestampMs, uint32_t sequence);
};
static_assert(sizeof(ChessInteractionEvent) <= 256U,
              "Reducer events keep the existing scalar position and plan envelope");

void ResetChessInteractionModel(ChessInteractionModel& model);
ChessPositionState ProjectChessPositionAfterPlan(
    const ChessPositionState& acknowledged,
    const PhysicalTransitionPlan& plan);
uint64_t GetLegalProgressPhysicalMask(
    const TaggedInteractionState& state);
uint64_t ProjectPhysicalOccupancyForInteractionProgress(
    const ChessPositionState& acknowledged,
    const TaggedInteractionState& state);

// Reducer implementation seams split across bounded translation units.
void EnterChessInteractionCorrection(ChessInteractionModel& model,
                                     uint64_t correctionMask);
void ApplyChessInteractionBoardObservation(ChessInteractionModel& model,
                                           const ChessInteractionEvent& event);
void ApplyChessInteractionPositionAcknowledged(ChessInteractionModel& model,
                                                const ChessInteractionEvent& event,
                                                const CaptureLegality* captureLegality);
void ApplyChessInteractionActionResult(ChessInteractionModel& model,
                                       const ChessInteractionEvent& event);
void ApplyChessInteractionPlanRequested(ChessInteractionModel& model,
                                        const ChessInteractionEvent& event);
void ApplyChessInteractionPlanProgressObserved(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event);
void ApplyDuckRelocationBoardObservation(
    ChessInteractionModel& model,
    const ChessInteractionEvent& event);
bool BeginDuckRelocationPlan(ChessInteractionModel& model,
                             uint8_t fromSquare,
                             uint8_t requiredDestination,
                             PhysicalTransitionSource source =
                                 PhysicalTransitionSource::Human,
                             DirectedMoveOrigin directedOrigin =
                                 DirectedMoveOrigin::Cpu);

// Position acknowledgement borrows the optional rows only for this call.
void ReduceChessInteraction(ChessInteractionModel& model,
                            const ChessInteractionEvent& event,
                            const CaptureLegality* captureLegality = nullptr);

void CheckChessInteractionModelInvariants(const ChessInteractionModel& model,
                                           InvariantReport<8>& outReport);

const char* ChessInteractionPhaseToString(ChessInteractionPhase phase);
const char* PhysicalTransitionKindToString(PhysicalTransitionKind kind);
const char* RequiredPhysicalActionToString(RequiredPhysicalAction action);
const char* InteractionInvariantStatusCodeToString(
    InteractionInvariantStatusCode code);
uint64_t ComputeChessInteractionModelHash(const ChessInteractionModel& model);

} // namespace runtime
