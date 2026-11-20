#pragma once

#include <cstddef>
#include <cstdint>

#include <ArduinoJson.h>

#include "modules/settings_store.hpp"
#include "modules/ui_types.hpp"
#include "trace/support_action_history.hpp"

namespace modules {

enum class ControlMutationStatus : uint8_t {
    Accepted = 0,
    Applied,
    Rejected,
    Noop,
};

enum class ControlMutationReason : uint8_t {
    None = 0,
    BadRequest,
    UnknownAction,
    QueueFull,
    UnknownKey,
    TypeMismatch,
    OutOfRange,
    InvalidEnumValue,
    NotAtStart,
    Busy,
    NoHistory,
    GameOver,
    Unsupported,
    EngineRejected,
    StorageUnavailable,
    ArchiveRequired,
};

enum class ControlMutationPersistenceTier : uint8_t {
    None = 0,
    Ordinary,
    MustSave,
};

enum class ControlAction : uint8_t {
    Unknown = 0,
    Hint,
    Undo,
    NewGame,
    Coaching,
    ReformatStorage,
    Sleep = 6,
};

enum class ControlActionState : uint8_t {
    Idle = 0,
    UndoPending,
};

enum class CoachingMutation : uint8_t {
    Toggle = 0,
    Disable,
    Enable,
};

enum class ControlCoordinatorResult : uint8_t {
    Applied = 0,
    Pending,
    QueueFull,
    NotAtStart,
    Busy,
    NoHistory,
    GameOver,
    Unsupported,
    EngineRejected,
    StorageUnavailable,
    ArchiveRequired,
};

struct ControlMutationOutcome {
    uint16_t requestId = 0U;
    ControlAction action = ControlAction::Unknown;
    ControlMutationStatus status = ControlMutationStatus::Rejected;
    ControlMutationReason reason = ControlMutationReason::BadRequest;
    ControlMutationPersistenceTier persistenceTier =
        ControlMutationPersistenceTier::None;
    uint32_t settingsRevision = 0U;
    ControlActionState actionState = ControlActionState::Idle;
};

struct ControlMutationCallbacks {
    void* context = nullptr;
    // Runtime policy permission. Hardware/calibration/internal settings
    // still require durable storage; normal validation and domain gates remain.
    bool allowVolatileUserSettings = false;
    void (*beforeSettingMutation)(void* context, const char* canonicalKey) =
        nullptr;
    void (*settingProjected)(void* context, const char* canonicalKey) = nullptr;
    ControlCoordinatorResult (*checkStartOnlyGate)(
        void* context,
        const char* canonicalKey) = nullptr;
    void (*cancelCpuWork)(void* context) = nullptr;
    void (*cancelHintWorkAndRestart)(void* context) = nullptr;
    ControlCoordinatorResult (*requestHint)(void* context,
                                            uint16_t requestId) = nullptr;
    ControlCoordinatorResult (*requestUndo)(void* context,
                                            uint16_t requestId,
                                            uint8_t moveCount) = nullptr;
    ControlCoordinatorResult (*requestNewGame)(void* context,
                                               uint16_t requestId) = nullptr;
    ControlCoordinatorResult (*requestSleep)(void* context) = nullptr;
    ControlCoordinatorResult (*requestStorageReformat)(void* context,
                                                        uint16_t requestId) = nullptr;
    ControlCoordinatorResult (*pollStorageReformat)(void* context,
                                                     uint16_t requestId) = nullptr;
    ControlCoordinatorResult (*pollAction)(void* context,
                                           uint16_t requestId) = nullptr;
    ControlActionState (*getActionState)(void* context) = nullptr;
    // Presentation observes terminal authority without consuming a client's receipt.
    void (*actionCompleted)(void* context, const ControlMutationOutcome& outcome,
                            UiOwner owner, ControlActionState initialState) = nullptr;
    // Transient feedback only; the canonical Automatic Hints setting owns value.
    void (*coachingChanged)(void* context,
                               UiOwner owner,
                               bool enabled) = nullptr;
};

/**
 * \brief Firmware-owned setting and board-equivalent action mutation policy.
 *
 * Validation precedence is stable: registry/shape failure, effective-value
 * Noop, storage availability, domain gate, pre-mutation callback, apply,
 * side effects, then exactly one persistence-tier revision bump.
 *
 * Action precedence is stable: request shape, known/supported action, bounded
 * in-flight capacity, then coordinator-owned game gates. Undo and New Game are
 * accepted first and completed later with the same request ID.
 */
class ControlMutationService final {
  public:
    static constexpr size_t COMPLETION_CAPACITY = 4U;

    void SetSupportActions(trace::SupportActionHistory* actions) { m_supportActions = actions; }
    void Configure(SettingsStore& settings,
                   const ControlMutationCallbacks& callbacks);
    [[nodiscard]] bool IsConfigured() const { return m_settings != nullptr; }
    // Does not discard in-flight actions or their correlated completions.
    void AllowVolatileUserSettings() { m_callbacks.allowVolatileUserSettings = true; }

    [[nodiscard]] ControlMutationOutcome MutateSetting(
        const char* key,
        JsonVariantConst value,
        uint16_t requestId = 0U);

    [[nodiscard]] ControlMutationOutcome ResetSetting(
        const char* key,
        uint16_t requestId = 0U);

    [[nodiscard]] ControlMutationOutcome MutateUsbCcThresholds(
        uint16_t threshold500,
        uint16_t threshold1500,
        uint16_t threshold3000,
        uint16_t requestId = 0U);

    [[nodiscard]] ControlMutationOutcome ResetUsbCcThresholds(
        uint16_t requestId = 0U);

    [[nodiscard]] ControlMutationOutcome MutateAction(
        ControlAction action,
        uint16_t requestId = 0U,
        UiOwner owner = UiOwner::None,
        CoachingMutation coachingMutation = CoachingMutation::Toggle,
        uint8_t undoMoveCount = 1U);

    void SetActionCompletedCallback(decltype(ControlMutationCallbacks::actionCompleted) callback) {
        m_callbacks.actionCompleted = callback;
    }
    void Update();
    [[nodiscard]] bool TryPeekCompletion(uint16_t requestId,
                                         ControlMutationOutcome& outOutcome) const;
    [[nodiscard]] bool AcceptCompletion(uint16_t requestId);
    [[nodiscard]] bool TryTakeCompletion(ControlMutationOutcome& outOutcome);
    [[nodiscard]] bool TryTakeCompletion(uint16_t requestId,
                                         ControlMutationOutcome& outOutcome);
    [[nodiscard]] bool AbandonCompletion(uint16_t requestId);
    void ResetTransientState();

    [[nodiscard]] ControlActionState GetActionState() const;

    [[nodiscard]] uint16_t AllocateRequestId();

  private:
    struct PendingAction {
        uint32_t supportAction = 0U;
        bool active = false;
        bool coordinatorPending = false;
        bool discardCompletion = false;
        uint16_t requestId = 0U;
        ControlAction action = ControlAction::Unknown;
        uint8_t undoMoveCount = 1U;
        UiOwner owner = UiOwner::None;
        ControlActionState initialState = ControlActionState::Idle;
    };

    ControlMutationOutcome MutateActionInternal(ControlAction action, uint16_t requestId,
        UiOwner owner, CoachingMutation coachingMutation, uint8_t undoMoveCount);
    [[nodiscard]] bool IsRequestIdOutstanding(uint16_t requestId) const;
    [[nodiscard]] bool EnqueueCompletion(
        const ControlMutationOutcome& outcome);
    [[nodiscard]] ControlMutationOutcome MakeActionOutcome(
        uint16_t requestId,
        ControlAction action,
        ControlCoordinatorResult result) const;

    ControlMutationOutcome MutateSettingInternal(const char* key, JsonVariantConst value, uint16_t requestId);
    bool m_recordingAction = false;
    void RecordSupportOutcome(uint32_t action, const ControlMutationOutcome& outcome);
    trace::SupportActionHistory* m_supportActions = nullptr;
    SettingsStore* m_settings = nullptr;
    ControlMutationCallbacks m_callbacks{};
    uint16_t m_nextRequestId = 1U;
    PendingAction m_pending{};
    ControlMutationOutcome m_completions[COMPLETION_CAPACITY] = {};
    size_t m_completionHead = 0U;
    size_t m_completionTail = 0U;
    size_t m_completionCount = 0U;
};

[[nodiscard]] const char* ControlMutationStatusToString(
    ControlMutationStatus status);
[[nodiscard]] const char* ControlMutationReasonToString(
    ControlMutationReason reason);
[[nodiscard]] const char* ControlActionStateToString(
    ControlActionState state);
[[nodiscard]] ControlMutationReason ControlMutationReasonFromCoordinatorResult(
    ControlCoordinatorResult result);
[[nodiscard]] char ControlMutationStatusToCompactCode(
    ControlMutationStatus status);
[[nodiscard]] char ControlMutationReasonToCompactCode(
    ControlMutationReason reason);
[[nodiscard]] size_t FormatControlMutationCompactResponse(
    const ControlMutationOutcome& outcome,
    char* outBuffer,
    size_t outCapacity);

} // namespace modules
