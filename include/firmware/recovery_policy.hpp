#pragma once

#include <cstdint>
#include <optional>

namespace firmware {

enum class RecoveryCause : uint8_t {
    None = 0,
    EarlyCrash,
    PersistedStateCorrupt,
    UpdatePending,
    UpdateFailed,
    ManualFactoryReset,
};

enum class RecoveryState : uint8_t {
    Inactive = 0,
    WaitForRelease,
    Ready,
    RetryHold,
    DestructiveHold,
    CancelWindow,
    Cancelled,
    ActionPending,
    ActionFailed,
};

enum class RecoveryAction : uint8_t {
    None = 0,
    RetryBoot,
    FactoryReset,
};

enum class RecoveryActionFailure : uint8_t {
    None = 0,
    PersistedStateDeleteFailed,
    RestartReturned,
};

enum class RecoveryBeginMode : uint8_t {
    RequireNeutralRelease = 0,
    ArmImmediately,
};

enum class RecoveryOwner : uint8_t {
    None = 0,
    P1,
    P2,
};

struct RecoveryEvidence {
    bool earlyCrashDetected = false;
    bool persistedStateCorrupt = false;
    bool updatePending = false;
    bool updateFailed = false;
    bool manualFactoryResetRequested = false;
};

/**
 * \brief Select the most actionable recovery cause supported by current evidence.
 * \note Explicit manual entry wins. Automatic evidence is ordered corrupt
 *       persisted state, pending update, failed update, then early crash.
 */
RecoveryCause SelectRecoveryCause(const RecoveryEvidence& evidence);

struct RecoveryInput {
    bool retryHeld = false;
    bool destructiveChordHeld = false;
    bool controlsReleased = true;
    RecoveryOwner owner = RecoveryOwner::None;
};

/**
 * \brief Raw control levels used by the platform-neutral recovery normalizer.
 * \note A true field means the control is currently pressed. Player-two
 *       fields are ignored unless hasP2Controls is true.
 */
struct RecoveryControlLevels {
    bool p1X = false;
    bool p1O = false;
    bool p1Down = false;
    bool p2X = false;
    bool p2O = false;
    bool p2Down = false;
    bool hasP2Controls = false;
};

/** \brief Normalize X/O levels for the ordinary safe-mode interaction. */
RecoveryInput NormalizeSafeRecoveryInput(
    const RecoveryControlLevels& controls);

/** \brief Select the sole player holding Down at cold-boot entry. */
RecoveryOwner SelectFactoryResetEntryOwner(
    const RecoveryControlLevels& controls);

/**
 * \brief Normalize fresh X/O consent for the physical bank that held Down.
 * \note Neither Down nor controls from the other bank may join the chord.
 */
RecoveryInput NormalizeFactoryResetInput(
    const RecoveryControlLevels& controls,
    RecoveryOwner owner);

struct RecoverySnapshot {
    RecoveryCause cause = RecoveryCause::None;
    RecoveryState state = RecoveryState::Inactive;
    RecoveryAction pendingAction = RecoveryAction::None;
    RecoveryActionFailure actionFailure = RecoveryActionFailure::None;
    RecoveryOwner activeOwner = RecoveryOwner::None;

    // Progress through the active hold, in the range 0..1000. Destructive
    // progress reaches 1000 at consent and stays full through CancelWindow.
    uint16_t totalProgressPermille = 0U;
    uint32_t stateEnteredMs = 0U;
};

struct RecoveryUpdate {
    RecoverySnapshot snapshot;
    RecoveryAction action = RecoveryAction::None;
};

/**
 * \brief Pure recovery interaction policy with explicit time and one-shot actions.
 */
class RecoveryPolicy final {
  public:
    static constexpr uint32_t RETRY_HOLD_MS = 1500U;
    static constexpr uint32_t DESTRUCTIVE_HOLD_MS = 5000U;
    static constexpr uint32_t CANCEL_WINDOW_MS = 2000U;
    static constexpr uint32_t COLD_RESET_HOLD_MS = 2000U;
    static constexpr uint32_t COLD_RESET_PROMPT_MS = 10000U;
    // Nominal total when updates observe the five-second boundary on time. A
    // sparse update still starts a complete CancelWindow before it may act.
    static constexpr uint32_t DESTRUCTIVE_TOTAL_MS =
        DESTRUCTIVE_HOLD_MS + CANCEL_WINDOW_MS;

    /**
     * \brief Start or replace the current recovery interaction.
     * \param cause Evidence-selected recovery cause.
     * \param mode Whether controls must first be observed fully released.
     * \param nowMs Current unsigned millisecond timestamp.
     */
    void Begin(RecoveryCause cause,
               RecoveryBeginMode mode,
               uint32_t nowMs);

    /** \brief Arm a bounded cold-power prompt that requires a fresh chord. */
    void BeginColdFactoryReset(uint32_t nowMs);
    bool IsColdFactoryReset() const { return m_coldResetStartedMs.has_value(); }

    /**
     * \brief Advance policy from one normalized input observation.
     * \return Current snapshot plus an action emitted only on this transition.
     */
    RecoveryUpdate Update(const RecoveryInput& input, uint32_t nowMs);

    /**
     * \brief Mark the currently pending action as failed.
     * \note A failed action cannot rearm until a later neutral input update.
     */
    void ReportActionFailure(RecoveryActionFailure failure, uint32_t nowMs);

    RecoverySnapshot GetSnapshot() const { return m_snapshot; }

  private:
    void EnterState(RecoveryState state, uint32_t nowMs);
    void CancelCurrentGesture(uint32_t nowMs);
    RecoveryAction EmitAction(RecoveryAction action, uint32_t nowMs);
    void UpdateProgress(uint32_t nowMs);
    bool ColdInputStable(bool valid, uint32_t nowMs);

    RecoverySnapshot m_snapshot;
    uint32_t m_gestureStartedMs = 0U;
    std::optional<uint32_t> m_coldResetStartedMs;
    std::optional<uint32_t> m_coldStableInputSinceMs;
};

} // namespace firmware
