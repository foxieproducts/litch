#include "modules/control_mutation_service.hpp"
#include "modules/assistance_preferences.hpp"

namespace modules {

void ControlMutationService::Configure(
    SettingsStore& settings,
    const ControlMutationCallbacks& callbacks) {
    m_settings = &settings;
    m_callbacks = callbacks;
    ResetTransientState();
}

void ControlMutationService::ResetTransientState() {
    m_nextRequestId = 1U;
    m_pending = PendingAction{};
    m_completionHead = 0U;
    m_completionTail = 0U;
    m_completionCount = 0U;
}

ControlMutationOutcome ControlMutationService::MutateAction(
    const ControlAction action, const uint16_t requestId, const UiOwner owner,
    const CoachingMutation coachingMutation, const uint8_t undoMoveCount) {
    // Correlated repeats belong to the same action, including pending polls.
    const bool duplicate = requestId != 0U && IsRequestIdOutstanding(requestId);
    uint32_t supportAction = 0U;
    if (m_supportActions != nullptr && !duplicate && action != ControlAction::Unknown) {
        const auto kind = action == ControlAction::Hint ? trace::SupportActionKind::Hint
            : action == ControlAction::Undo ? trace::SupportActionKind::Undo
            : action == ControlAction::NewGame ? trace::SupportActionKind::NewGame
            : action == ControlAction::Coaching ? trace::SupportActionKind::Coaching
            : action == ControlAction::Sleep ? trace::SupportActionKind::Sleep
            : trace::SupportActionKind::StorageRecovery;
        supportAction = m_supportActions->Begin(kind);
    }
    const auto initialState = GetActionState();
    m_recordingAction = true;
    const auto outcome = MutateActionInternal(action, requestId, owner,
                                              coachingMutation, undoMoveCount);
    m_recordingAction = false;
    if (outcome.status == ControlMutationStatus::Accepted) {
        m_pending.supportAction = supportAction;
        m_pending.owner = owner;
        m_pending.initialState = initialState;
    } else if (outcome.status == ControlMutationStatus::Applied &&
               m_callbacks.actionCompleted != nullptr) {
        m_callbacks.actionCompleted(m_callbacks.context, outcome, owner, initialState);
    }
    RecordSupportOutcome(supportAction, outcome);
    return outcome;
}

void ControlMutationService::RecordSupportOutcome(const uint32_t action,
    const ControlMutationOutcome& outcome) {
    if (m_supportActions == nullptr) return;
    const auto status = outcome.status == ControlMutationStatus::Accepted
        ? trace::SupportActionOutcome::Accepted
        : outcome.status == ControlMutationStatus::Applied
        ? trace::SupportActionOutcome::Applied
        : outcome.status == ControlMutationStatus::Noop
        ? trace::SupportActionOutcome::NoChange : trace::SupportActionOutcome::Rejected;
    m_supportActions->Finish(action, status);
}

ControlMutationOutcome ControlMutationService::MutateActionInternal(
    const ControlAction action,
    uint16_t requestId,
    const UiOwner owner,
    const CoachingMutation coachingMutation,
    const uint8_t undoMoveCount) {
    ControlMutationOutcome outcome;
    outcome.action = action;
    outcome.actionState = GetActionState();
    if (requestId == 0U) {
        requestId = AllocateRequestId();
    } else if (m_pending.active && m_pending.requestId == requestId) {
        outcome.requestId = requestId;
        outcome.reason = ControlMutationReason::Busy;
        return outcome;
    } else if (IsRequestIdOutstanding(requestId)) {
        outcome.requestId = requestId;
        return outcome;
    }
    outcome.requestId = requestId;
    if (requestId == 0U) {
        outcome.reason = ControlMutationReason::QueueFull;
        return outcome;
    }

    if (action == ControlAction::Unknown) {
        outcome.reason = ControlMutationReason::UnknownAction;
        return outcome;
    }

    if (m_pending.active) {
        outcome.reason = ControlMutationReason::Busy;
        return outcome;
    }

    if (action == ControlAction::Coaching) {
        if ((owner != UiOwner::P1 && owner != UiOwner::P2) || m_settings == nullptr) {
            outcome.reason = owner == UiOwner::None
                                 ? ControlMutationReason::BadRequest
                                 : ControlMutationReason::Unsupported;
            return outcome;
        }

        const bool current = assistance_preferences::Enabled(
            *m_settings, owner, assistance_preferences::Feature::AutomaticHints);
        const bool enabled = coachingMutation == CoachingMutation::Toggle
                                 ? !current
                                 : coachingMutation == CoachingMutation::Enable;
        StaticJsonDocument<16> value;
        value.set(enabled);
        outcome = MutateSetting(assistance_preferences::Key(
            owner, assistance_preferences::Feature::AutomaticHints),
            value.as<JsonVariantConst>(), requestId);
        outcome.action = action;
        outcome.actionState = GetActionState();
        return outcome;
    }

    if (action == ControlAction::Sleep) {
        if (owner != UiOwner::None || coachingMutation != CoachingMutation::Toggle ||
            undoMoveCount != 1U) return outcome;
        return MakeActionOutcome(requestId, action,
            m_callbacks.requestSleep != nullptr
                ? m_callbacks.requestSleep(m_callbacks.context)
                : ControlCoordinatorResult::Unsupported);
    }

    if (action == ControlAction::Hint) {
        if (m_callbacks.requestHint == nullptr) {
            outcome.reason = ControlMutationReason::Unsupported;
            return outcome;
        }
        return MakeActionOutcome(
            requestId, action,
            m_callbacks.requestHint(m_callbacks.context, requestId));
    }

    if ((action == ControlAction::Undo && m_callbacks.requestUndo == nullptr) ||
        (action == ControlAction::NewGame &&
         m_callbacks.requestNewGame == nullptr) ||
        (action == ControlAction::ReformatStorage &&
         m_callbacks.requestStorageReformat == nullptr)) {
        outcome.reason = ControlMutationReason::Unsupported;
        return outcome;
    }
    if (action == ControlAction::Undo && undoMoveCount == 0U) {
        outcome.reason = ControlMutationReason::BadRequest;
        return outcome;
    }

    m_pending.active = true;
    m_pending.requestId = requestId;
    m_pending.action = action;
    m_pending.undoMoveCount = undoMoveCount;
    outcome.status = ControlMutationStatus::Accepted;
    outcome.reason = ControlMutationReason::None;
    return outcome;
}

void ControlMutationService::Update() {
    if (!m_pending.active ||
        (!m_pending.discardCompletion &&
         m_completionCount >= COMPLETION_CAPACITY)) {
        return;
    }

    ControlCoordinatorResult result = ControlCoordinatorResult::Unsupported;
    if (m_pending.coordinatorPending &&
        m_pending.action == ControlAction::ReformatStorage) {
        result = m_callbacks.pollStorageReformat != nullptr
                     ? m_callbacks.pollStorageReformat(m_callbacks.context,
                                                       m_pending.requestId)
                     : ControlCoordinatorResult::Unsupported;
    } else if (m_pending.coordinatorPending) {
        result = m_callbacks.pollAction != nullptr
                     ? m_callbacks.pollAction(m_callbacks.context,
                                              m_pending.requestId)
                     : ControlCoordinatorResult::Unsupported;
    } else if (m_pending.action == ControlAction::Undo) {
        result = m_callbacks.requestUndo(
            m_callbacks.context, m_pending.requestId,
            m_pending.undoMoveCount);
    } else if (m_pending.action == ControlAction::ReformatStorage) {
        result = m_callbacks.requestStorageReformat(
            m_callbacks.context, m_pending.requestId);
    } else if (m_pending.action == ControlAction::NewGame) {
        result = m_callbacks.requestNewGame(
            m_callbacks.context, m_pending.requestId);
    }

    if (result == ControlCoordinatorResult::Pending) {
        m_pending.coordinatorPending = true;
        return;
    }

    const ControlMutationOutcome completion = MakeActionOutcome(
        m_pending.requestId, m_pending.action, result);
    if (completion.status == ControlMutationStatus::Applied &&
        m_callbacks.actionCompleted != nullptr) {
        m_callbacks.actionCompleted(m_callbacks.context, completion,
                                     m_pending.owner, m_pending.initialState);
    }
    RecordSupportOutcome(m_pending.supportAction, completion);
    if (m_pending.discardCompletion || EnqueueCompletion(completion)) {
        m_pending = PendingAction{};
    }
}

ControlActionState ControlMutationService::GetActionState() const {
    return m_callbacks.getActionState != nullptr
               ? m_callbacks.getActionState(m_callbacks.context)
               : ControlActionState::Idle;
}

bool ControlMutationService::TryTakeCompletion(
    ControlMutationOutcome& outOutcome) {
    if (m_completionCount == 0U) {
        return false;
    }
    outOutcome = m_completions[m_completionHead];
    m_completionHead = (m_completionHead + 1U) % COMPLETION_CAPACITY;
    --m_completionCount;
    return true;
}

bool ControlMutationService::TryPeekCompletion(
    const uint16_t requestId,
    ControlMutationOutcome& outOutcome) const {
    if (requestId == 0U || m_completionCount == 0U) {
        return false;
    }
    for (size_t offset = 0U; offset < m_completionCount; ++offset) {
        const size_t index = (m_completionHead + offset) % COMPLETION_CAPACITY;
        if (m_completions[index].requestId == requestId) {
            outOutcome = m_completions[index];
            return true;
        }
    }
    return false;
}

bool ControlMutationService::AcceptCompletion(const uint16_t requestId) {
    ControlMutationOutcome accepted;
    return TryTakeCompletion(requestId, accepted);
}

bool ControlMutationService::TryTakeCompletion(
    const uint16_t requestId,
    ControlMutationOutcome& outOutcome) {
    if (requestId == 0U || m_completionCount == 0U) {
        return false;
    }

    ControlMutationOutcome retained[COMPLETION_CAPACITY] = {};
    size_t retainedCount = 0U;
    bool found = false;
    const size_t originalCount = m_completionCount;
    for (size_t offset = 0U; offset < originalCount; ++offset) {
        const size_t index =
            (m_completionHead + offset) % COMPLETION_CAPACITY;
        const ControlMutationOutcome& candidate = m_completions[index];
        if (!found && candidate.requestId == requestId) {
            outOutcome = candidate;
            found = true;
        } else {
            retained[retainedCount++] = candidate;
        }
    }
    if (!found) {
        return false;
    }

    m_completionHead = 0U;
    m_completionTail = 0U;
    m_completionCount = 0U;
    for (size_t index = 0U; index < retainedCount; ++index) {
        (void)EnqueueCompletion(retained[index]);
    }
    return true;
}

bool ControlMutationService::AbandonCompletion(const uint16_t requestId) {
    if (requestId == 0U) {
        return false;
    }
    if (m_pending.active && m_pending.requestId == requestId) {
        m_pending.discardCompletion = true;
        return true;
    }
    ControlMutationOutcome discarded;
    return TryTakeCompletion(requestId, discarded);
}

uint16_t ControlMutationService::AllocateRequestId() {
    for (size_t attempt = 0U; attempt < COMPLETION_CAPACITY + 2U; ++attempt) {
        const uint16_t candidate = m_nextRequestId++;
        if (m_nextRequestId == 0U) {
            m_nextRequestId = 1U;
        }
        if (candidate != 0U && !IsRequestIdOutstanding(candidate)) {
            return candidate;
        }
    }
    return 0U;
}

bool ControlMutationService::IsRequestIdOutstanding(
    const uint16_t requestId) const {
    if (requestId == 0U) {
        return false;
    }
    if (m_pending.active && m_pending.requestId == requestId) {
        return true;
    }
    for (size_t offset = 0U; offset < m_completionCount; ++offset) {
        const size_t index =
            (m_completionHead + offset) % COMPLETION_CAPACITY;
        if (m_completions[index].requestId == requestId) {
            return true;
        }
    }
    return false;
}

bool ControlMutationService::EnqueueCompletion(
    const ControlMutationOutcome& outcome) {
    if (m_completionCount >= COMPLETION_CAPACITY) {
        return false;
    }
    m_completions[m_completionTail] = outcome;
    m_completionTail = (m_completionTail + 1U) % COMPLETION_CAPACITY;
    ++m_completionCount;
    return true;
}

ControlMutationOutcome ControlMutationService::MakeActionOutcome(
    const uint16_t requestId,
    const ControlAction action,
    const ControlCoordinatorResult result) const {
    ControlMutationOutcome outcome;
    outcome.requestId = requestId;
    outcome.action = action;
    outcome.actionState = GetActionState();
    outcome.settingsRevision =
        m_settings != nullptr ? m_settings->GetRevision() : 0U;
    if (result == ControlCoordinatorResult::Pending) {
        outcome.status = ControlMutationStatus::Accepted;
        outcome.reason = ControlMutationReason::None;
    } else if (result == ControlCoordinatorResult::Applied) {
        outcome.status = ControlMutationStatus::Applied;
        outcome.reason = ControlMutationReason::None;
    } else {
        outcome.status = ControlMutationStatus::Rejected;
        outcome.reason = ControlMutationReasonFromCoordinatorResult(result);
    }
    return outcome;
}

} // namespace modules
