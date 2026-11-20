#pragma once

#include <cstdint>

namespace runtime {

// These values are initial safety targets, intentionally kept at this boundary
// for later HIL calibration rather than replicated by hardware adapters.
constexpr uint32_t RUNTIME_HEALTH_BOARD_UNCERTAINTY_ENTER_MS = 100U;
constexpr uint32_t RUNTIME_HEALTH_BOARD_STABLE_RECOVERY_MS = 100U;
constexpr uint8_t RUNTIME_HEALTH_SEVERE_POWER_ENVELOPE_PERCENT = 50U;
constexpr uint32_t RUNTIME_HEALTH_SEVERE_PROTECTION_LATCH_MS = 1000U;

enum class RuntimeHealthState : uint8_t {
    Healthy = 0,
    DegradedUsable,
    InputPaused,
    ProtectionLatched,
};

// Numeric assignments are an append-only observable cause-mask contract.
enum class RuntimeHealthCause : uint16_t {
    None = 0U,
    OneAmbientSensor = 1U << 0U,
    NoAmbientSensors = 1U << 1U,
    BoardScanUncertain = 1U << 2U,
    SourceUnstable = 1U << 3U,
    BrownoutLatched = 1U << 4U,
    SevereProtectionLatched = 1U << 5U,
};

constexpr uint16_t RuntimeHealthCauseMask(const RuntimeHealthCause cause) {
    return static_cast<uint16_t>(cause);
}

constexpr bool HasRuntimeHealthCause(const uint16_t causes,
                                     const RuntimeHealthCause cause) {
    return (causes & RuntimeHealthCauseMask(cause)) != 0U;
}

enum class RuntimeHealthFactKind : uint8_t {
    BoardScan = 0,
    AmbientSensorCount,
    PowerSource,
    PowerEnvelope,
    Brownout,
    ResetLatchedProtection,
    LedBudget,
};

enum class RuntimeHealthBoardScan : uint8_t {
    Stable = 0,
    Uncertain,
};

enum class RuntimeHealthSource : uint8_t {
    // Stable is positive, adapter-validated recovery evidence. Raw source
    // samples must be debounced before they reach this reducer.
    Stable = 0,
    Unstable,
};

// A normalized observation. Only the member relevant to kind is consumed.
// LedBudget is deliberately advisory-free: ordinary instantaneous LED limiting
// is not a runtime-health fault. tMs must be monotonic modulo uint32_t wrap;
// adapters must not replay older observations out of order.
struct RuntimeHealthFact {
    uint32_t tMs = 0U;
    RuntimeHealthFactKind kind = RuntimeHealthFactKind::BoardScan;
    RuntimeHealthBoardScan boardScan = RuntimeHealthBoardScan::Stable;
    uint8_t ambientSensorCount = 2U;
    RuntimeHealthSource source = RuntimeHealthSource::Stable;
    uint8_t powerEnvelopePercent = 100U;
    bool brownoutDetected = false;
};

struct RuntimeHealthAdvice {
    bool boardInputAllowed = true;
    bool moveAcceptanceAllowed = true;
    bool clocksAllowed = true;
};

struct RuntimeHealthSnapshot {
    RuntimeHealthState state = RuntimeHealthState::Healthy;
    uint16_t causes = 0U;
    uint16_t revision = 0U;
    RuntimeHealthAdvice advice{};
};

constexpr bool IsRuntimeHealthPlayPaused(
    const RuntimeHealthSnapshot& snapshot) {
    return !snapshot.advice.boardInputAllowed ||
           !snapshot.advice.moveAcceptanceAllowed ||
           !snapshot.advice.clocksAllowed;
}

// Pure value reducer. Its caller supplies all observations and time; this
// class performs no I/O and owns no global state.
class RuntimeHealthReducer {
  public:
    RuntimeHealthReducer() = default;

    // Returns true exactly when state or cause mask changed externally.
    bool Apply(const RuntimeHealthFact& fact);

    RuntimeHealthSnapshot Snapshot() const;

  private:
    void ProcessBoardScan(uint32_t nowMs, RuntimeHealthBoardScan scan);
    void ProcessInvalidFact(uint32_t nowMs);
    void SetCause(RuntimeHealthCause cause, bool present);
    void RecomputeState();
    static RuntimeHealthAdvice AdviceFor(RuntimeHealthState state);

    RuntimeHealthState m_state = RuntimeHealthState::Healthy;
    uint16_t m_causes = 0U;
    uint16_t m_revision = 0U;

    bool m_uncertainSinceValid = false;
    uint32_t m_uncertainSinceMs = 0U;
    bool m_stableSinceValid = false;
    uint32_t m_stableSinceMs = 0U;
    bool m_severeSinceValid = false;
    uint32_t m_severeSinceMs = 0U;
};

} // namespace runtime
