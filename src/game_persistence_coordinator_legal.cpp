#include "modules/game_persistence_coordinator.hpp"

#include <cstring>

#include "logger.hpp"
#include "king_safety.hpp"
#include "modules/chess_strings.hpp"
#include <litch_max/duck_chess_rules.hpp>
#include "modules/vs_cpu_turn.hpp"
#include "runtime/directed_physical_prompt.hpp"

namespace modules {

namespace {

bool ReadDebugPickupSetting(const modules::SettingsStore& settings) {
    return settings.ReadValue("DEBUG_PICKUP", false);
}

bool ReadFirstPlayerIsP2SettingLocal(const modules::SettingsStore& settings) {
    return settings.ReadValue("GAME_FIRST_PLAYER", static_cast<uint32_t>(1U)) == 2U;
}


} // namespace

void GamePersistenceCoordinator::ClearLegalDestinationsMask() {
    m_stateMachine.ClearValidDestMask();
    m_cachedLegalFromSquare = 255;
    m_cachedLegalFromFen.Clear();
}

void GamePersistenceCoordinator::ClearCachedLegalDestinations() {
    m_cachedLegalDestMapValid = false;
    m_cachedLegalDestFen.Clear();
    m_cachedLegalComputedFromSquaresMask64 = 0;
    std::memset(m_cachedLegalDestByFromSquare, 0, sizeof(m_cachedLegalDestByFromSquare));
}

bool GamePersistenceCoordinator::BuildCachedLegalDestinationsForCurrentPosition() {
    ClearCachedLegalDestinations();

    if (m_cachedFen.Empty()) {
        return false;
    }

    m_cachedLegalDestFen = m_cachedFen;

    const uint64_t moverOcc64 = m_stateMachine.GetExpectedMoverOccupancyMask();
    uint8_t moverSquares[64] = {};
    const uint8_t moverSquareCount = MaskToSquareListAscending(moverOcc64, moverSquares, sizeof(moverSquares));

    for (uint8_t i = 0; i < moverSquareCount; ++i) {
        const uint8_t fromSquare = moverSquares[i];
        if (fromSquare >= 64U) {
            continue;
        }

        uint64_t destMask64 = 0;
        const bool legalOk = IsDuckGame()
            ? litch_max::GetDuckLegalDestinationsMask(
                  m_cachedFen.CStr(), m_duckSquare,
                  fromSquare, destMask64)
            : m_engine.GetLegalDestinationsMask(
                  fromSquare, destMask64);
        if (!legalOk) {
            ClearCachedLegalDestinations();
            return false;
        }

        m_cachedLegalDestByFromSquare[fromSquare] = destMask64;
        m_cachedLegalComputedFromSquaresMask64 |= (1ULL << fromSquare);
    }

    m_cachedLegalDestMapValid = true;
    return true;
}

bool GamePersistenceCoordinator::TryGetCachedLegalDestinationsMask(const uint8_t fromSquare,
                                                                   uint64_t& outDestMask64) const {
    outDestMask64 = 0;

    if (fromSquare >= 64U) {
        return false;
    }
    if (!m_cachedLegalDestMapValid) {
        return false;
    }
    if (m_cachedLegalDestFen.Empty() || !m_cachedFen.Equals(m_cachedLegalDestFen.View())) {
        return false;
    }

    const uint64_t fromMask = (1ULL << fromSquare);
    if ((m_cachedLegalComputedFromSquaresMask64 & fromMask) == 0) {
        return false;
    }

    outDestMask64 = m_cachedLegalDestByFromSquare[fromSquare];
    return true;
}

uint64_t GamePersistenceCoordinator::GetCaptureFirstCapturerMask() const {
    if (IsGameOverVariantAware() ||
        IsVsCpuTurn(m_engineDerived.sideToMove, ReadVsCpuLevelSetting(),
                    ReadFirstPlayerIsP2SettingLocal(m_settings),
                    m_settings.ReadValue("VS_CPU_PLAYER", static_cast<uint32_t>(2U)) == 2U)) {
        return 0ULL;
    }
    const runtime::ChessInteractionModel& model =
        m_stateMachine.GetChessInteractionModel();
    const uint8_t victim = model.phase.selectedSquare;
    if (model.phase.tag != runtime::ChessInteractionPhase::CaptureStaged ||
        victim >= 64U || !m_cachedLegalDestMapValid ||
        m_cachedLegalDestFen.Empty() ||
        !m_cachedFen.Equals(m_cachedLegalDestFen.View())) {
        return 0ULL;
    }

    const uint64_t victimBit = UINT64_C(1) << victim;
    const uint64_t physicalOccupancy = m_stateMachine.GetPhysicalOccupancyMask();
    if ((model.acknowledged.opponentOccupiedMask & victimBit) == 0ULL ||
        (physicalOccupancy & victimBit) != 0ULL) {
        return 0ULL;
    }
    const uint64_t candidates = model.acknowledged.moverOccupiedMask &
                                physicalOccupancy &
                                m_cachedLegalComputedFromSquaresMask64;
    const uint64_t enPassantTargetBit =
        model.acknowledged.enPassantCaptureSquare == victim &&
                model.acknowledged.enPassantTargetSquare < 64U
            ? UINT64_C(1) << model.acknowledged.enPassantTargetSquare
            : 0ULL;
    uint64_t capturers = 0ULL;
    for (uint8_t from = 0U; from < 64U; ++from) {
        const uint64_t fromBit = UINT64_C(1) << from;
        if ((candidates & fromBit) == 0ULL) {
            continue;
        }
        const uint64_t destinations = m_cachedLegalDestByFromSquare[from];
        const bool ordinaryCapture = (destinations & victimBit) != 0ULL;
        // EP removes a pawn away from its landing square. A normal capture
        // of that same pawn remains eligible alongside legal EP captures.
        const bool enPassantCapture =
            (model.acknowledged.pawnMask & fromBit) != 0ULL &&
            (destinations & enPassantTargetBit) != 0ULL;
        if (ordinaryCapture || enPassantCapture) {
            capturers |= fromBit;
        }
    }
    return capturers;
}

void GamePersistenceCoordinator::UpdateLegalDestinationsMask() {
    if (IsGameOverVariantAware()) {
        uint8_t fromSquare = 255U;
        const runtime::TaggedInteractionState& interaction =
            m_stateMachine.GetChessInteractionModel().phase;
        const bool selectedCapture =
            interaction.tag ==
                runtime::ChessInteractionPhase::CaptureStaged &&
            interaction.selectedSquare < 64U;
        bool hasRejectedSquare =
            m_stateMachine.TryGetHumanPieceInHandSquare(fromSquare);
        if (!hasRejectedSquare && selectedCapture) {
            fromSquare = interaction.selectedSquare;
            hasRejectedSquare = true;
        }
        if (hasRejectedSquare) {
            (void)m_stateMachine.RejectCurrentPickupAsIllegal(fromSquare);
        }
        ClearLegalDestinationsMask();
        return;
    }

    if (m_stateMachine.IsMoveActionAwaitingResult()) {
        // The physical observation may already contain the next side's pickup,
        // but engine/cache side masks still describe the unacknowledged move.
        // Its action result replays that observation against the new position.
        // Until then, do not reject it using the previous side's authority.
        ClearLegalDestinationsMask();
        return;
    }

    if (m_vsCpuThinking || m_stateMachine.IsCpuThinkingActive()) {
        if (m_cachedLegalFromSquare != 255) {
            ClearLegalDestinationsMask();
        }
        return;
    }

    if (m_stateMachine.IsKingQueenSwapPromptActive()) {
        if (m_cachedLegalFromSquare != 255) {
            ClearLegalDestinationsMask();
        }
        return;
    }

    const runtime::TaggedInteractionState& interaction =
        m_stateMachine.GetChessInteractionModel().phase;
    const bool activeHumanCastle =
        interaction.tag == runtime::ChessInteractionPhase::ExecutingPlan &&
        interaction.plan.source == runtime::PhysicalTransitionSource::Human &&
        interaction.plan.kind == runtime::PhysicalTransitionKind::Castling;
    if (activeHumanCastle) {
        runtime::DirectedPhysicalPrompt prompt;
        ClearLegalDestinationsMask();
        if (runtime::TryGetDirectedPhysicalPrompt(interaction, prompt) &&
            (prompt.action == runtime::RequiredPhysicalAction::PlacePrimary ||
             prompt.action == runtime::RequiredPhysicalAction::PlaceSecondary)) {
            m_stateMachine.SetValidDestMask(1ULL << prompt.toSquare);
        }
        return;
    }

    const bool inHumanMove = m_stateMachine.IsHumanPieceInHand() ||
                             m_stateMachine.IsPlacementModeActive();
    if (!inHumanMove || m_stateMachine.IsPlacementModeActive() || m_promotionPending) {
        if (m_cachedLegalFromSquare != 255) {
            ClearLegalDestinationsMask();
        }
        return;
    }

    uint8_t fromSquare = 255U;
    if (!m_stateMachine.TryGetHumanPieceInHandSquare(fromSquare)) {
        if (m_cachedLegalFromSquare != 255) {
            ClearLegalDestinationsMask();
        }
        return;
    }

    const bool debugPickup = ReadDebugPickupSetting(m_settings);
    const uint64_t fromMask = (1ULL << fromSquare);
    const uint64_t moverOcc64 = m_stateMachine.GetExpectedMoverOccupancyMask();
    if ((moverOcc64 & fromMask) == 0ULL) {
        if (debugPickup) {
            char piece = '?';
            (void)modules::TryGetFenPieceAtSquare(m_cachedFen.CStr(), fromSquare, piece);
            logger::Trace("pickup: reject from=%u piece=%c stm=%u first_p2=%u", // NOLINT
                         static_cast<unsigned>(fromSquare),
                         piece,
                         static_cast<unsigned>(m_engineDerived.sideToMove == SideToMove::Black ? 1U : 0U),
                         static_cast<unsigned>(ReadFirstPlayerIsP2SettingLocal(m_settings) ? 1U : 0U));
        }
        (void)m_stateMachine.RejectCurrentPickupAsIllegal(fromSquare);
        m_stateMachine.ClearKingSafetyPrompt();
        m_cachedLegalFromSquare = 255;
        return;
    }

    if (debugPickup && (fromSquare != m_cachedLegalFromSquare)) {
        char piece = '?';
        (void)modules::TryGetFenPieceAtSquare(m_cachedFen.CStr(), fromSquare, piece);
        logger::Trace("pickup: accept from=%u piece=%c stm=%u first_p2=%u", // NOLINT
                     static_cast<unsigned>(fromSquare),
                     piece,
                     static_cast<unsigned>(m_engineDerived.sideToMove == SideToMove::Black ? 1U : 0U),
                     static_cast<unsigned>(ReadFirstPlayerIsP2SettingLocal(m_settings) ? 1U : 0U));
    }

    const bool cachedFromMatches = (fromSquare == m_cachedLegalFromSquare);
    const bool cachedFenMatches = !m_cachedLegalFromFen.Empty() &&
                                  m_cachedFen.Equals(m_cachedLegalFromFen.View());
    if (cachedFromMatches && cachedFenMatches) {
        return;
    }

    m_stateMachine.ClearKingSafetyPrompt();

    uint8_t pinKingSquare = 255U;
    uint8_t pinningSquare = 255U;
    const bool pinnedLift = !IsDuckGame() &&
        interaction.tag == runtime::ChessInteractionPhase::PieceLifted &&
        m_stateMachine.TryGetExpectedMoverKingSquare(pinKingSquare) &&
        king_safety::TryFindPinningPiece(
            {m_cachedFen.CStr(), fromSquare, pinKingSquare, m_engineDerived.sideToMove},
            pinningSquare);
    if (pinnedLift) {
        m_stateMachine.SetKingSafetyPrompt(fromSquare, pinKingSquare, pinningSquare);
    }

    const bool engineThinking = m_hintThinking || m_vsCpuThinking || m_ponderThinking ||
                                m_stateMachine.IsHintThinkingActive() ||
                                m_stateMachine.IsCpuThinkingActive() ||
                                (UsesAsyncThinkWorker() &&
                                 ThinkWorker().IsActive());
    if (engineThinking) {
        uint64_t cachedMask64 = 0;
        if (!IsDuckGame() && (m_hintThinking || m_ponderThinking) &&
            TryGetCachedLegalDestinationsMask(fromSquare, cachedMask64)) {
            if (pinnedLift && cachedMask64 == 0ULL) {
                (void)m_stateMachine.RejectCurrentPickupAsIllegal(fromSquare);
                m_stateMachine.SetKingSafetyPrompt(fromSquare, pinKingSquare, pinningSquare);
                m_cachedLegalFromSquare = 255U;
                return;
            }
            m_stateMachine.SetValidDestMask(cachedMask64);
            m_cachedLegalFromSquare = fromSquare;
            m_cachedLegalFromFen = m_cachedFen;
            return;
        }
        // Classic Hint precomputes legality; pondering retains the same exact
        // position map after Hint completes. Otherwise wait for quiescence.
        // Duck legality is pure rules work, so a pickup can still receive
        // immediate guidance while background Duck search is active.
        if (!IsDuckGame()) {
            ClearLegalDestinationsMask();
            return;
        }
    }

    uint64_t destMask64 = 0;
    const bool legalOk = IsDuckGame()
        ? litch_max::GetDuckLegalDestinationsMask(
              m_cachedFen.CStr(), m_duckSquare,
              fromSquare, destMask64)
        : m_engine.GetLegalDestinationsMask(
              fromSquare, destMask64);
    if (!legalOk) {
        ClearLegalDestinationsMask();
        return;
    }

    if (!IsDuckGame() && destMask64 == 0ULL &&
        !m_cachedFen.Empty() &&
        m_engine.SetFen(m_cachedFen.CStr()) &&
        m_engine.GetLegalDestinationsMask(fromSquare, destMask64)) {
        m_engineDerived.RefreshFromEngine(m_engine);
    }

    if (destMask64 == 0ULL) {
        if (debugPickup) {
            logger::Trace("pickup: zero-legal from=%u", static_cast<unsigned>(fromSquare));
        }
        if (IsDuckGame() &&
            IsInitialVariantSelectionReversible() &&
            !m_initialDuckObstructionRelocationPending &&
            litch_max::IsDuckSoleBlockerForPiece(
                m_cachedFen.CStr(), m_duckSquare, fromSquare)) {
            m_initialDuckObstructionRelocationPending = true;
            m_initialDuckObstructedFromSquare = fromSquare;
            (void)m_stateMachine.RejectCurrentPickupAsIllegal(fromSquare);
            m_stateMachine.ClearKingSafetyPrompt();
            ClearLegalDestinationsMask();
            return;
        }
        uint8_t moverKingSquare = pinKingSquare;
        uint8_t attackerSquare = pinningSquare;
        const bool exposesKing = pinnedLift ||
            (!IsDuckGame() &&
             m_stateMachine.TryGetExpectedMoverKingSquare(moverKingSquare) &&
             king_safety::TryFindSelfCheckAttacker(
                 {m_cachedFen.CStr(), fromSquare, moverKingSquare, m_engineDerived.sideToMove},
                 attackerSquare));
        if (exposesKing) {
            (void)m_stateMachine.RejectCurrentPickupAsIllegal(fromSquare);
            m_stateMachine.SetKingSafetyPrompt(fromSquare, moverKingSquare, attackerSquare);
            m_cachedLegalFromSquare = 255;
            return;
        }
    }

    m_stateMachine.SetValidDestMask(destMask64);
    m_cachedLegalFromSquare = fromSquare;
    m_cachedLegalFromFen = m_cachedFen;
}

} // namespace modules
