#include "modules/runtime_health.hpp"

namespace modules {

BoardInputHealthDecision BoardInputRuntimeHealth::ObserveScanAttempt(
    const uint32_t nowMs,
    const BoardScanAttemptResult& attempt) {
    if (attempt.status == BoardScanAttemptStatus::NotDue) {
        return BoardInputHealthDecision{m_reducer.Snapshot(), false, false};
    }

    const runtime::RuntimeHealthSnapshot before = m_reducer.Snapshot();
    runtime::RuntimeHealthFact fact;
    fact.tMs = nowMs;
    fact.kind = runtime::RuntimeHealthFactKind::BoardScan;
    fact.boardScan = attempt.status == BoardScanAttemptStatus::Succeeded
                         ? runtime::RuntimeHealthBoardScan::Stable
                         : runtime::RuntimeHealthBoardScan::Uncertain;
    (void)m_reducer.Apply(fact);

    const runtime::RuntimeHealthSnapshot after = m_reducer.Snapshot();
    const bool wasBoardUncertain = runtime::HasRuntimeHealthCause(
        before.causes, runtime::RuntimeHealthCause::BoardScanUncertain);
    const bool isBoardUncertain = runtime::HasRuntimeHealthCause(
        after.causes, runtime::RuntimeHealthCause::BoardScanUncertain);
    if (wasBoardUncertain && !isBoardUncertain) {
        m_authoritativeResyncPending = true;
    }

    const bool succeeded =
        attempt.status == BoardScanAttemptStatus::Succeeded;
    const bool dispatchAllowed =
        succeeded && after.advice.boardInputAllowed &&
        after.advice.moveAcceptanceAllowed;
    const bool authoritativeResync =
        dispatchAllowed && m_authoritativeResyncPending;

    return BoardInputHealthDecision{
        after,
        dispatchAllowed,
        authoritativeResync,
    };
}

void BoardInputRuntimeHealth::AcknowledgeAuthoritativeResync(
    const bool accepted) {
    if (accepted) {
        m_authoritativeResyncPending = false;
    }
}

} // namespace modules
