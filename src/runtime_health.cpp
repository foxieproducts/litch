#include "runtime/runtime_health.hpp"

namespace runtime {
namespace {

bool HasElapsed(const uint32_t nowMs, const uint32_t sinceMs,
                const uint32_t durationMs) {
    return static_cast<uint32_t>(nowMs - sinceMs) >= durationMs;
}

bool IsKnown(const RuntimeHealthFactKind kind) {
    return kind <= RuntimeHealthFactKind::LedBudget;
}

bool IsKnown(const RuntimeHealthBoardScan scan) {
    return scan <= RuntimeHealthBoardScan::Uncertain;
}

bool IsKnown(const RuntimeHealthSource source) {
    return source <= RuntimeHealthSource::Unstable;
}

} // namespace

bool RuntimeHealthReducer::Apply(const RuntimeHealthFact& fact) {
    const RuntimeHealthState previousState = m_state;
    const uint16_t previousCauses = m_causes;

    if (!IsKnown(fact.kind)) {
        ProcessInvalidFact(fact.tMs);
    } else {
        switch (fact.kind) {
            case RuntimeHealthFactKind::BoardScan:
                if (!IsKnown(fact.boardScan)) {
                    ProcessInvalidFact(fact.tMs);
                } else {
                    ProcessBoardScan(fact.tMs, fact.boardScan);
                }
                break;
            case RuntimeHealthFactKind::AmbientSensorCount:
                if (fact.ambientSensorCount > 2U) {
                    ProcessInvalidFact(fact.tMs);
                } else {
                    SetCause(RuntimeHealthCause::OneAmbientSensor,
                             fact.ambientSensorCount == 1U);
                    SetCause(RuntimeHealthCause::NoAmbientSensors,
                             fact.ambientSensorCount == 0U);
                }
                break;
            case RuntimeHealthFactKind::PowerSource:
                if (!IsKnown(fact.source)) {
                    ProcessInvalidFact(fact.tMs);
                } else {
                    SetCause(RuntimeHealthCause::SourceUnstable,
                             fact.source == RuntimeHealthSource::Unstable);
                }
                break;
            case RuntimeHealthFactKind::PowerEnvelope:
                if (fact.powerEnvelopePercent > 100U) {
                    ProcessInvalidFact(fact.tMs);
                } else if (!HasRuntimeHealthCause(
                               m_causes,
                               RuntimeHealthCause::SevereProtectionLatched)) {
                    if (fact.powerEnvelopePercent <=
                        RUNTIME_HEALTH_SEVERE_POWER_ENVELOPE_PERCENT) {
                        if (!m_severeSinceValid) {
                            m_severeSinceValid = true;
                            m_severeSinceMs = fact.tMs;
                        }
                        if (HasElapsed(fact.tMs, m_severeSinceMs,
                                       RUNTIME_HEALTH_SEVERE_PROTECTION_LATCH_MS)) {
                            SetCause(RuntimeHealthCause::SevereProtectionLatched,
                                     true);
                            m_severeSinceValid = false;
                        }
                    } else {
                        m_severeSinceValid = false;
                    }
                }
                break;
            case RuntimeHealthFactKind::Brownout:
                if (fact.brownoutDetected) {
                    SetCause(RuntimeHealthCause::BrownoutLatched, true);
                }
                break;
            case RuntimeHealthFactKind::ResetLatchedProtection:
                SetCause(RuntimeHealthCause::BrownoutLatched, false);
                SetCause(RuntimeHealthCause::SevereProtectionLatched, false);
                m_severeSinceValid = false;
                break;
            case RuntimeHealthFactKind::LedBudget:
                break;
        }
    }

    RecomputeState();
    if (m_state == previousState && m_causes == previousCauses) {
        return false;
    }

    ++m_revision;
    return true;
}

RuntimeHealthSnapshot RuntimeHealthReducer::Snapshot() const {
    return RuntimeHealthSnapshot{m_state, m_causes, m_revision,
                                 AdviceFor(m_state)};
}

void RuntimeHealthReducer::ProcessBoardScan(const uint32_t nowMs,
                                             const RuntimeHealthBoardScan scan) {
    if (scan == RuntimeHealthBoardScan::Uncertain) {
        m_stableSinceValid = false;
        if (!m_uncertainSinceValid) {
            m_uncertainSinceValid = true;
            m_uncertainSinceMs = nowMs;
        }
        if (HasElapsed(nowMs, m_uncertainSinceMs,
                       RUNTIME_HEALTH_BOARD_UNCERTAINTY_ENTER_MS)) {
            SetCause(RuntimeHealthCause::BoardScanUncertain, true);
        }
        return;
    }

    m_uncertainSinceValid = false;
    if (!HasRuntimeHealthCause(m_causes,
                               RuntimeHealthCause::BoardScanUncertain)) {
        m_stableSinceValid = false;
        return;
    }

    if (!m_stableSinceValid) {
        m_stableSinceValid = true;
        m_stableSinceMs = nowMs;
    }
    if (HasElapsed(nowMs, m_stableSinceMs,
                   RUNTIME_HEALTH_BOARD_STABLE_RECOVERY_MS)) {
        SetCause(RuntimeHealthCause::BoardScanUncertain, false);
        m_stableSinceValid = false;
    }
}

void RuntimeHealthReducer::ProcessInvalidFact(const uint32_t nowMs) {
    // Unknown data cannot safely authorize board/move/clock progression.
    m_uncertainSinceValid = true;
    m_uncertainSinceMs = nowMs;
    m_stableSinceValid = false;
    SetCause(RuntimeHealthCause::BoardScanUncertain, true);
}

void RuntimeHealthReducer::SetCause(const RuntimeHealthCause cause,
                                    const bool present) {
    const uint16_t mask = RuntimeHealthCauseMask(cause);
    if (present) {
        m_causes = static_cast<uint16_t>(m_causes | mask);
    } else {
        m_causes = static_cast<uint16_t>(m_causes & ~mask);
    }
}

void RuntimeHealthReducer::RecomputeState() {
    if (HasRuntimeHealthCause(m_causes,
                                     RuntimeHealthCause::BoardScanUncertain)) {
        m_state = RuntimeHealthState::InputPaused;
    } else if (HasRuntimeHealthCause(m_causes,
                                     RuntimeHealthCause::OneAmbientSensor) ||
               HasRuntimeHealthCause(m_causes,
                                     RuntimeHealthCause::NoAmbientSensors) ||
               HasRuntimeHealthCause(m_causes,
                                     RuntimeHealthCause::SourceUnstable) ||
               HasRuntimeHealthCause(m_causes,
                                     RuntimeHealthCause::BrownoutLatched) ||
               HasRuntimeHealthCause(
                   m_causes,
                   RuntimeHealthCause::SevereProtectionLatched)) {
        m_state = RuntimeHealthState::DegradedUsable;
    } else {
        m_state = RuntimeHealthState::Healthy;
    }
}

RuntimeHealthAdvice RuntimeHealthReducer::AdviceFor(
    const RuntimeHealthState state) {
    switch (state) {
        case RuntimeHealthState::Healthy:
        case RuntimeHealthState::DegradedUsable:
        case RuntimeHealthState::ProtectionLatched:
            return RuntimeHealthAdvice{true, true, true};
        case RuntimeHealthState::InputPaused:
            return RuntimeHealthAdvice{false, false, false};
    }

    return RuntimeHealthAdvice{false, false, false};
}

} // namespace runtime
