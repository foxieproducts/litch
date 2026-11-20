#include "modules/game_persistence_coordinator.hpp"

#include "logger.hpp"
#include "modules/draw_rules.hpp"
#include "modules/game_persistence_coordinator_update_internal.hpp"
#include "modules/vs_cpu_rng.hpp"

namespace modules {

bool GamePersistenceCoordinator::ApplyHumanMoveTransactional(
    const runtime::PhysicalTransitionPlan& move) {
    if (IsDuckGame()) {
        return StageDuckChessMove(move, '\0');
    }

    UciMove uci;
    if (!MakeUciMove(move.moveFrom, move.moveTo, '\0', uci)) {
        DispatchPendingInteractionActionResult(false);
        logger::Warn("engine: invalid human move identity from=%u to=%u",
                     static_cast<unsigned>(move.moveFrom),
                     static_cast<unsigned>(move.moveTo));
        return false;
    }

    uint8_t fields[UNDO_HISTORY_FIELDS_PER_MOVE] = {};
    game_persistence_update_internal::BuildUndoPersistFieldsFromFen(
        m_cachedFen.CStr(),
        m_engine.GetSideToMove(),
        move.moveFrom,
        move.moveTo,
        move.removalRequired,
        move.removalSquare,
        0,
        fields);
    const uint32_t nextHalfmoveClock = NextHalfmoveClockPlies(
        m_cachedFen.CStr(),
        m_halfmoveClockPlies,
        move.moveFrom,
        move.moveTo,
        move.removalRequired,
        move.removalSquare);

    PrepareTakebackGrace(move);
    logger::Trace("human: apply %s", uci.CStr());
    const bool applied = m_engine.ApplyUciMove(uci.CStr());
    if (!applied) {
        ClearTakebackGrace();
        DispatchPendingInteractionActionResult(false);
        logger::Warn("engine: ApplyUciMove failed (%s)", uci.CStr());
        return false;
    }

    (void)AppendUndoHistoryEntry(fields[0],
                                 fields[1],
                                 fields[2],
                                 fields[3],
                                 fields[4],
                                 fields[5],
                                 fields[6]);
    m_halfmoveClockPlies = nextHalfmoveClock;
    m_vsCpuRngSeed = NextVsCpuSeedAfterMove(m_vsCpuRngSeed, uci.CStr());
    DispatchPendingInteractionActionResult(true);
    return true;
}

} // namespace modules
