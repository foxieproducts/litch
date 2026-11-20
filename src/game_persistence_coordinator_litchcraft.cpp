#include "modules/game_persistence_coordinator.hpp"

#include "foxie_embedded/millis.hpp"
#include "modules/edge_led_cue.hpp"
#include "modules/initial_fen.hpp"
#include "runtime/game_state_machine.hpp"

#include <string_view>

namespace modules {
bool GamePersistenceCoordinator::TryBeginLitchcraftPresentation(
    const LitchcraftSession::PresentationAccess access) {
    if (access == LitchcraftSession::PresentationAccess::None ||
        m_litchcraftPresentationLease != 0U ||
        m_litchcraftPresentationAdmission.begin == nullptr) return true;
    m_litchcraftPresentationLease = m_litchcraftPresentationAdmission.begin(
        m_litchcraftPresentationAdmission.context, access);
    return m_litchcraftPresentationLease != 0U;
}

bool GamePersistenceCoordinator::TryBeginLitchcraftOrdered(const uint8_t opcode,
    const uint8_t* const bytes, const size_t size, const bool runtimeEligible) {
    if (!runtimeEligible) return true; // existing wrapper refuses without mutation
    if (!TryBeginLitchcraftPresentation(m_litchcraft.OrderedPresentationAccess(opcode)))
        return false;
    if (!m_litchcraft.OrderedWaitsForPresentedFrame(opcode, bytes, size)) return true;
    EndLitchcraftPresentation();
    return false;
}

bool GamePersistenceCoordinator::TryBeginLitchcraftPacket(
    const uint8_t* const bytes, const size_t size) {
    if (!TryBeginLitchcraftPresentation(m_litchcraft.PacketPresentationAccess(bytes, size)))
        return false;
    if (!m_litchcraft.PacketWaitsForPresentedFrame(bytes, size)) return true;
    EndLitchcraftPresentation();
    return false;
}

void GamePersistenceCoordinator::EndLitchcraftPresentation() {
    EndLitchcraftPresentation(m_litchcraftPresentationLease);
}

void GamePersistenceCoordinator::EndLitchcraftPresentation(const uint32_t lease) {
    if (lease == 0U || lease != m_litchcraftPresentationLease) return;
    m_litchcraftPresentationLease = 0U;
    if (lease != 0U && m_litchcraftPresentationAdmission.end != nullptr)
        m_litchcraftPresentationAdmission.end(m_litchcraftPresentationAdmission.context, lease);
}

ControlCoordinatorResult GamePersistenceCoordinator::AdmitControlStartOnlyMutation(
    const char* const canonicalKey) {
    const auto result = CheckControlStartOnlyGate(canonicalKey);
    const std::string_view key = canonicalKey == nullptr ? "" : canonicalKey;
    const bool occupiedBrush = result == ControlCoordinatorResult::Busy &&
        m_litchcraft.IsActive() && m_litchcraft.Occupancy() != 0ULL &&
        (key == "BOARD_ROTATE" || key == "GAME_FIRST_PLAYER");
    if (result == ControlCoordinatorResult::NotAtStart || occupiedBrush) {
        m_occupiedBoardFeedbackPending = true;
    }
    return result;
}

bool GamePersistenceCoordinator::IsLitchcraftEntryContextEligible(
    const bool requireEmptyBoard) const {
    // Empty/fresh setup is eligible, including its normal missing-piece cues.
    // Clock/history/FEN protect a running or resumable game even at idle input.
    return m_stateMachine.GetChessInteractionModel().board.valid &&
           (!requireEmptyBoard ||
            m_stateMachine.GetPhysicalOccupancyMask() == 0ULL) &&
           IsAnyInitialFen(GetAuthoritativeFen()) && !HasUndoHistory() &&
           m_stateMachine.GetMovedSquaresMask() == 0U &&
           !m_gameClock.IsRunning() && !m_gameClock.IsPaused() && !m_clockSessionStarted &&
           !m_hintThinking && !m_vsCpuThinking &&
           !(UsesAsyncThinkWorker() && ThinkWorker().IsActive()) &&
           !m_stateMachine.IsCpuThinkingActive() && !m_stateMachine.HasQueuedCpuMove() &&
           !m_stateMachine.IsHumanPieceInHand() && !m_stateMachine.IsMoveSettlingActive() &&
           !m_stateMachine.IsPromotionActive() && !m_stateMachine.IsUndoPromptActive() &&
           !m_stateMachine.IsKingQueenSwapPromptActive() &&
           !m_stateMachine.IsDuckRelocationActive() && !HasPendingUndoCommand() &&
           !m_boardSettingsActive && !HasClockTerminalOutcome();
}
bool GamePersistenceCoordinator::CanEnterLitchcraft() const {
    return IsLitchcraftEntryContextEligible(true);
}
bool GamePersistenceCoordinator::CanEnterLitchcraftFromBoardControls() const {
    // A board-local chord on an untouched game is an explicit mode choice.
    // Physical entry requires the already-new game canvas to be empty. Any
    // pieces in that untouched setup retain the existing amber correction cue.
    // UpdateLitchcraft() also runs during early boot. Do not read FEN or
    // occupancy authority until the interaction board has been initialized.
    if (!IsLitchcraftEntryContextEligible(false)) {
        return false;
    }
    return m_stateMachine.GetPhysicalOccupancyMask() == 0ULL;
}
bool GamePersistenceCoordinator::IsLitchcraftEntryBlockedByOccupiedBoard() const {
    return !m_litchcraft.IsActive() &&
           m_stateMachine.GetPhysicalOccupancyMask() != 0ULL &&
           IsLitchcraftEntryContextEligible(false);
}
void GamePersistenceCoordinator::HandleLitchcraftPacket(const uint8_t* bytes,
                                                        const size_t size,
                                                        const bool runtimeEligible) {
    m_litchcraft.HandlePacket(bytes, size, runtimeEligible && CanEnterLitchcraft(),
                              m_stateMachine.GetPhysicalOccupancyMask());
    UpdateLitchcraft();
}
void GamePersistenceCoordinator::HandleLitchcraftBulkPacket(
    const uint8_t* bytes, const size_t size, const bool runtimeEligible) {
    m_litchcraft.HandleBulkPacket(
        bytes, size, runtimeEligible && CanEnterLitchcraft(),
        m_stateMachine.GetPhysicalOccupancyMask());
    UpdateLitchcraft();
}
LitchcraftSession::Status
GamePersistenceCoordinator::HandleLitchcraftConnectionMode(
    const uint8_t* const bytes, const size_t size, const bool runtimeEligible) {
    const auto status = m_litchcraft.HandleCompactMode(
        bytes, size, runtimeEligible && CanEnterLitchcraft(),
        m_stateMachine.GetPhysicalOccupancyMask());
    UpdateLitchcraft();
    return status;
}
LitchcraftSession::Status
GamePersistenceCoordinator::HandleLitchcraftConnectionOrdered(
    const uint8_t opcode, const uint8_t* const bytes, const size_t size,
    const bool runtimeEligible, const uint32_t connectionEpoch) {
    if (!runtimeEligible) return LitchcraftSession::Status::Busy;
    const auto status = m_litchcraft.ApplyConnectionOrdered(
        opcode, bytes, size, connectionEpoch);
    UpdateLitchcraft();
    return status;
}
void GamePersistenceCoordinator::UpdateLitchcraft() {
    // The Play-mode arbiter owns a captured face-button edge for its complete
    // priority window. In a live game that edge must survive ordinary update
    // ticks so a second face button can form the bounded New Game chord; its
    // normal expiry path replays an unmatched button to Undo/Hint.
    const bool alreadyAdmitted = m_litchcraftPresentationLease != 0U;
    const bool admitted = alreadyAdmitted || TryBeginLitchcraftPresentation(
        m_litchcraft.UpdatePresentationAccess());
    if (admitted) {
        m_litchcraft.Update();
        if (m_litchcraft.ConsumeOccupiedEntryRejected()) {
            m_occupiedBoardFeedbackPending = true;
        }
        if (m_litchcraft.ConsumeExited()) {
            // Brush observations were intentionally never chess observations.
            // Restore Setup authority before admitting any background save so the
            // accepted Exit owns the very next render.
            const uint64_t occupancy = m_litchcraft.Occupancy();
            m_stateMachine.HandleRecoveryBoardObservation(
                occupancy ^ m_stateMachine.GetPhysicalOccupancyMask(), occupancy,
                fxe::Millis(), m_stateMachine.GetLastBoardObservationSequence() + 1U);
            (void)m_stateMachine.ForceEnterPlacementMode();
        }
        if (!alreadyAdmitted) EndLitchcraftPresentation();
    }
    // External ingress owns its whole bounded batch. Persistence runs on the
    // ordinary next update after that owner has published/released its lease.
    if (m_litchcraftPresentationLease == 0U) m_litchcraftPersistence.Update();
}
void GamePersistenceCoordinator::PresentPendingOccupiedBoardFeedback(
    EdgeLedCueController& edgeCue) {
    if (!m_occupiedBoardFeedbackPending) return;
    m_occupiedBoardFeedbackPending = false;
    const uint32_t nowMs = fxe::Millis();
    // Local adapters may already have presented this same domain rejection.
    // A second request during the bounded warning must not pin or restart it.
    if (edgeCue.Current(nowMs).kind == EdgeLedCueKind::OccupiedBoardBlocked) return;
    m_stateMachine.TriggerAllPiecesFlash(
        EdgeLedCueController::OCCUPIED_BOARD_BLOCKED_DURATION_MS);
    edgeCue.Show(EdgeLedCueKind::OccupiedBoardBlocked, UiOwner::None, nowMs);
}
void GamePersistenceCoordinator::ObserveLitchcraftButton(const ButtonEvent& event) {
    ObserveLitchcraftButton(event, fxe::Millis());
}
void GamePersistenceCoordinator::ObserveLitchcraftButton(
    const ButtonEvent& event, const uint32_t eventTimeMs) {
    m_litchcraft.ObserveButton(event, eventTimeMs);
}
} // namespace modules
