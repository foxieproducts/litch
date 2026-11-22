#include "modules/board_owner_pairing_runtime.hpp"

#include "foxie_embedded/millis.hpp"

namespace modules {

bool BoardOwnerPairingRuntime::SameConnection(Connection left, Connection right) {
    return left.epoch != 0U && left.sessionToken != 0U &&
           left.handle == right.handle && left.epoch == right.epoch &&
           left.sessionToken == right.sessionToken;
}

void BoardOwnerPairingRuntime::Boot(BoardOwnerRecord record,
                                    Controller::BondInventory bonds,
                                    bool identityDurable) {
    m_custody.Boot(record, bonds);
    m_record = record;
    m_identityDurable = identityDurable;
    m_claim = {};
    m_owner = {};
    m_attempt = 0U;
    m_end = 0U;
    m_promptEnd = 0U;
    m_claimValid = false;
    m_phoneInitiatesSecurity = false;
    m_localClear = false;
    m_localClearAlreadyAbsent = false;
    m_holdingClear = false;
    m_needsDisconnect = false;
    m_clearContextSelected = false;
    m_closeLocalMenuOnSuccess = false;
    m_clearFeedbackActive = false;
    m_clearFeedbackSucceeded = false;
    m_physicalCue = PhysicalCue::None;
    m_previousPhysicalCue = PhysicalCue::None;
    m_consentAcknowledgmentActive = false;
    m_consentAcknowledgmentStartedMs = 0U;
    m_physicalCueStartedMs = 0U;
    m_previousPhysicalCueStartedMs = 0U;
    m_settingsReset = SettingsReset::None;
    RetainHeldTail();
}

void BoardOwnerPairingRuntime::UpdateDurableState(BoardOwnerRecord record,
                                                  bool identityDurable) {
    if (m_localClearAlreadyAbsent && record != BoardOwnerRecord::Absent)
        LeaveLocalClear();
    m_record = record;
    m_identityDurable = identityDurable;
    if (!identityDurable && m_claimValid) Retire(m_claim);
}

bool BoardOwnerPairingRuntime::PairPrompt() const {
    return m_claimValid && m_custody.GetOperation() == Controller::Operation::Pair &&
           m_custody.GetPhase() == Controller::Phase::Armed;
}

bool BoardOwnerPairingRuntime::ClearPrompt() const {
    if (!m_localClear) return false;
    if (m_localClearAlreadyAbsent) return AlreadyClear();
    return m_record == BoardOwnerRecord::Active &&
        m_custody.GetOperation() == Controller::Operation::Unpair &&
        m_custody.GetPhase() == Controller::Phase::Armed;
}

bool BoardOwnerPairingRuntime::AlreadyClear() const {
    return m_record == BoardOwnerRecord::Absent && m_custody.Pairable();
}

bool BoardOwnerPairingRuntime::Begin(const Command& command,
                                     Connection connection,
                                     const BoardOwnerAddBaseline& baseline) {
    Poll();
    const bool yieldIdleClear = m_localClearAlreadyAbsent && ClearPrompt() && !AnyButtonDown();
    if (m_settingsReset != SettingsReset::None || (ClearPrompt() && !yieldIdleClear) ||
        (command.action != owner_pairing_session_wire::Action::Begin &&
         command.action != owner_pairing_session_wire::Action::BeginPhoneSecurity) ||
        command.attempt == 0U || command.physicalEnd > owner_pairing_session_wire::EITHER_END ||
        connection.epoch == 0U || connection.sessionToken == 0U ||
        !m_identityDurable || (m_record != BoardOwnerRecord::Absent && m_record != BoardOwnerRecord::Active) ||
        m_needsDisconnect || m_consumeTail || m_consentPrompt == UINT32_MAX ||
        !m_custody.ArmPair(command.attempt, connection.handle,
                           connection.epoch, command.physicalEnd, baseline)) return false;
    if (yieldIdleClear) LeaveLocalClear();
    m_claim = connection;
    ++m_consentPrompt;
    m_claimValid = true;
    m_phoneInitiatesSecurity = command.action == owner_pairing_session_wire::Action::BeginPhoneSecurity;
    m_attempt = command.attempt;
    m_end = command.physicalEnd;
    m_promptEnd = command.physicalEnd;
    m_startedMs = fxe::Millis();
    m_clearFeedbackActive = false;
    m_consentAcknowledgmentActive = false;
    // Each button must have been released after any pre-existing hold before a
    // new Press can count. A released level at entry is already fresh.
    for (uint8_t end = 0U; end < 2U; ++end) {
        for (uint8_t button = 0U; button < 2U; ++button) {
            m_fresh[end][button] = !m_down[end][button];
            m_freshPress[end][button] = false;
        }
    }
    return true;
}

bool BoardOwnerPairingRuntime::Cancel(const Command& command,
                                      Connection connection) {
    Poll();
    if (command.action != owner_pairing_session_wire::Action::Cancel ||
        command.attempt != m_attempt ||
        (command.physicalEnd != m_end && command.physicalEnd != m_promptEnd) ||
        !SameConnection(connection, m_claim) || !m_claimValid) return false;
    if (PairPrompt()) {
        if (!m_custody.CancelPair(m_attempt, m_claim.handle, m_claim.epoch)) return false;
        m_claimValid = false;
        RetainHeldTail();
        return true;
    }
    if (m_custody.GetOperation() != Controller::Operation::Pair) return false;
    Retire(connection);
    return true;
}

bool BoardOwnerPairingRuntime::AnyButtonDown() const {
    return m_down[0][0] || m_down[0][1] || m_down[1][0] || m_down[1][1];
}

BoardOwnerPairingRuntime::PendingConsent BoardOwnerPairingRuntime::ReadPendingConsent() const {
    return PairPrompt() ? PendingConsent{m_claim, m_attempt, m_end, m_consentPrompt} : PendingConsent{};
}

bool BoardOwnerPairingRuntime::ConfirmPendingConsent(const PendingConsent consent) {
    Poll();
    if (!PairPrompt() || !m_identityDurable ||
        (m_record != BoardOwnerRecord::Absent && m_record != BoardOwnerRecord::Active) ||
        m_needsDisconnect || consent.attempt == 0U || consent.attempt != m_attempt ||
        consent.physicalEnd > 1U ||
        (m_end != owner_pairing_session_wire::EITHER_END && consent.physicalEnd != m_end) ||
        consent.prompt == 0U || consent.prompt != m_consentPrompt ||
        !SameConnection(consent.connection, m_claim) ||
        !m_custody.ConfirmPair(m_attempt, m_claim.handle, m_claim.epoch, consent.physicalEnd)) return false;
    m_end = consent.physicalEnd;
    m_consentAcknowledgmentActive = true;
    m_consentAcknowledgmentStartedMs = fxe::Millis();
    RetainHeldTail();
    return true;
}

void BoardOwnerPairingRuntime::RetainHeldTail() {
    m_consumeTail = AnyButtonDown();
}

bool BoardOwnerPairingRuntime::ObserveButton(PhysicalButton button) {
    if (button.physicalEnd > 1U) return false;
    Poll();
    // Queued releases bound consent by the physical timeline, never service delay.
    const uint32_t heldMs = button.eventTimeMs - m_holdStartedMs;
    if (ClearPrompt() && m_holdingClear && heldMs < 0x80000000U && heldMs >= CLEAR_HOLD_MS)
        (void)ConfirmClear(m_attempt, m_end);
    const uint8_t index = button.isX ? 0U : 1U;
    if (m_clearContextSelected && !m_localClear && !m_clearFeedbackActive &&
        !m_consumeTail && m_custody.GetOperation() == Controller::Operation::None &&
        button.down && !m_down[button.physicalEnd][index]) ArmSelectedLocalClear();
    const bool owned = m_clearContextSelected || m_settingsReset == SettingsReset::Pending ||
        PairPrompt() || ClearPrompt() || m_consumeTail ||
        (m_custody.GetPhase() != Controller::Phase::Failed &&
         (m_custody.GetOperation() == Controller::Operation::Pair ||
          m_custody.GetOperation() == Controller::Operation::Unpair));
    const bool changed = m_down[button.physicalEnd][index] != button.down;
    m_down[button.physicalEnd][index] = button.down;
    // A retired phone claim can consume a held tail while local clear remains
    // armed. Its Release must still cancel the board's continuous-hold consent.
    if (ClearPrompt() && (!m_down[m_end][0] || !m_down[m_end][1]))
        m_holdingClear = false;
    if (m_consumeTail) {
        if (!AnyButtonDown()) m_consumeTail = false;
        return true;
    }
    if (PairPrompt()) {
        if (m_end != owner_pairing_session_wire::EITHER_END && button.physicalEnd != m_end) return true;
        const uint8_t end = button.physicalEnd;
        // Delayed queued events from before Begin cannot supply consent.
        const bool afterBegin = static_cast<int32_t>(button.eventTimeMs - m_startedMs) >= 0;
        if (!button.down) {
            if (afterBegin) m_fresh[end][index] = true;
            m_freshPress[end][index] = false;
        } else if (changed && afterBegin && m_fresh[end][index]) {
            m_freshPress[end][index] = true;
        }
        if (changed && button.down && afterBegin && m_freshPress[end][0] && m_freshPress[end][1] &&
            m_down[end][0] && m_down[end][1]) {
            auto consent = ReadPendingConsent();
            consent.physicalEnd = end;
            (void)ConfirmPendingConsent(consent);
        }
        return true;
    }
    if (ClearPrompt()) {
        if (button.physicalEnd != m_end) return true;
        if (m_down[m_end][0] && m_down[m_end][1] && changed && button.down) {
            m_holdStartedMs = button.eventTimeMs;
            m_holdingClear = true;
        }
        return true;
    }
    return owned;
}

void BoardOwnerPairingRuntime::ReseedPhysicalInput() {
    for (auto& end : m_down) for (bool& down : end) down = false;
    m_consumeTail = false;
    m_holdingClear = false;
    for (auto& end : m_fresh) for (bool& fresh : end) fresh = true;
    for (auto& end : m_freshPress) for (bool& pressed : end) pressed = false;
}

bool BoardOwnerPairingRuntime::SelectLocalClear(uint16_t attempt,
                                                uint8_t physicalEnd) {
    Poll();
    if (ClearPrompt() && attempt == m_attempt && physicalEnd == m_end) return true;
    const bool alreadyClear = AlreadyClear();
    if (attempt == 0U || physicalEnd > 1U || m_settingsReset != SettingsReset::None ||
        m_needsDisconnect || (!alreadyClear && (m_record != BoardOwnerRecord::Active ||
        !m_custody.ArmUnpair(attempt, physicalEnd)))) return false;
    m_localClear = true;
    m_localClearAlreadyAbsent = alreadyClear;
    m_attempt = attempt;
    m_end = physicalEnd;
    m_startedMs = fxe::Millis();
    m_holdingClear = false;
    RetainHeldTail();
    return true;
}

bool BoardOwnerPairingRuntime::ConfirmClear(uint16_t attempt, uint8_t physicalEnd) {
    const bool alreadyClear = m_localClearAlreadyAbsent;
    if (alreadyClear ? !ClearPrompt() || attempt != m_attempt || physicalEnd != m_end
                     : !m_custody.ConfirmUnpair(attempt, physicalEnd)) return false;
    m_localClear = false;
    m_localClearAlreadyAbsent = false;
    // Only a physical hold in this still-selected menu may close that menu.
    m_closeLocalMenuOnSuccess = m_clearContextSelected && m_holdingClear;
    m_holdingClear = false;
    m_clearFeedbackActive = true;
    m_clearFeedbackSucceeded = alreadyClear;
    m_clearFeedbackStartedMs = fxe::Millis();
    m_clearFeedbackSucceededMs = alreadyClear ? m_clearFeedbackStartedMs : 0U;
    RetainHeldTail();
    return true;
}

bool BoardOwnerPairingRuntime::RequestLocalPairingClear() {
    Poll();
    if (m_settingsReset != SettingsReset::None || !m_identityDurable || m_needsDisconnect ||
        m_record != BoardOwnerRecord::Active ||
        m_custody.GetOperation() != Controller::Operation::None) return false;
    ++m_clearContextAttempt;
    if (m_clearContextAttempt == 0U) ++m_clearContextAttempt;
    if (!SelectLocalClear(m_clearContextAttempt, 0U) ||
        !ConfirmClear(m_clearContextAttempt, 0U)) return false;
    m_localClear = false;
    return true;
}

bool BoardOwnerPairingRuntime::RequestOwnerClear(Connection connection) {
    Poll();
    if (!OwnerReady(connection) ||
        m_custody.GetOperation() != Controller::Operation::None) return false;
    ++m_clearContextAttempt;
    if (m_clearContextAttempt == 0U) ++m_clearContextAttempt;
    if (!m_custody.ArmUnpair(m_clearContextAttempt, 0U) ||
        !ConfirmClear(m_clearContextAttempt, 0U)) return false;
    m_localClear = false;
    m_settingsReset = SettingsReset::Pending;
    return true;
}

void BoardOwnerPairingRuntime::CompleteOwnerSettingsReset(const bool saved) {
    m_settingsReset = saved ? SettingsReset::None : SettingsReset::Failed;
    m_clearFeedbackActive = false;
}

void BoardOwnerPairingRuntime::LeaveLocalClear() {
    if (ClearPrompt()) (void)m_custody.CancelUnpair(m_attempt, m_end);
    m_localClear = false;
    m_localClearAlreadyAbsent = false;
    m_holdingClear = false;
    RetainHeldTail();
}

void BoardOwnerPairingRuntime::SyncLocalClearContext(bool selected,
                                                    uint8_t physicalEnd) {
    if (selected == m_clearContextSelected &&
        (!selected || physicalEnd == m_clearContextEnd)) return;
    m_closeLocalMenuOnSuccess = false;
    if (m_clearContextSelected) LeaveLocalClear();
    m_clearContextSelected = selected;
    m_clearContextEnd = physicalEnd;
    if (!selected) return;
    ArmSelectedLocalClear();
}

void BoardOwnerPairingRuntime::ArmSelectedLocalClear() {
    ++m_clearContextAttempt;
    if (m_clearContextAttempt == 0U) ++m_clearContextAttempt;
    (void)SelectLocalClear(m_clearContextAttempt, m_clearContextEnd);
}

void BoardOwnerPairingRuntime::Poll() {
    // A retired claim cannot release a successful owner receipt into live
    // authority while the native disconnect is still in flight. The actual
    // disconnect updates custody first; its next poll journals rollback.
    if (!(m_needsDisconnect &&
          m_custody.GetOperation() == Controller::Operation::Pair &&
          m_custody.GetPhase() == Controller::Phase::SavingOwner)) {
        m_custody.PollPersistence();
    }
    const uint32_t nowMs = fxe::Millis();
    if (m_localClearAlreadyAbsent && !AlreadyClear()) LeaveLocalClear();
    if (PairPrompt() && nowMs - m_startedMs >= CLAIM_WINDOW_MS) {
        (void)m_custody.CancelPair(m_attempt, m_claim.handle, m_claim.epoch,
                                   Reason::Timeout);
        m_claimValid = false;
        RetainHeldTail();
    }
    const uint32_t physicalThroughMs = m_physicalButtonTickTimeValid ? m_physicalButtonTickMs : nowMs;
    if (m_physicalButtonTicksAllowed && ClearPrompt() && m_holdingClear &&
        physicalThroughMs - m_holdStartedMs < 0x80000000U &&
        physicalThroughMs - m_holdStartedMs >= CLEAR_HOLD_MS) {
        (void)ConfirmClear(m_attempt, m_end);
    }
    if (m_clearFeedbackActive) {
        if (m_custody.GetPhase() == Controller::Phase::Failed ||
            (m_settingsReset == SettingsReset::Pending &&
             nowMs - m_clearFeedbackStartedMs >= CLEAR_FEEDBACK_MS) ||
            (m_clearFeedbackSucceeded &&
             nowMs - m_clearFeedbackSucceededMs >= CLEAR_FEEDBACK_MS)) {
            m_clearFeedbackActive = false;
        } else if (m_settingsReset != SettingsReset::Pending &&
                   !m_clearFeedbackSucceeded && m_custody.Pairable() &&
                   m_record == BoardOwnerRecord::Absent) {
            // Pairable follows checked native cleanup, the durable Absent
            // receipt and its completion acknowledgement. A failed claim or
            // boot reconciliation never arms this local-clear episode.
            m_clearFeedbackSucceeded = true;
            m_clearFeedbackSucceededMs = nowMs;
        }
    }
    if (m_custody.GetOperation() != Controller::Operation::Pair &&
        m_custody.HasAuthenticatedOwnerConnection() && m_claimValid) {
        m_owner = m_claim;
    }
    if (m_consentAcknowledgmentActive &&
        (nowMs - m_consentAcknowledgmentStartedMs >= PRESENTATION_FADE_MS ||
         m_custody.GetReason() != Reason::None ||
         m_custody.GetPhase() == Controller::Phase::Failed))
        m_consentAcknowledgmentActive = false;
    UpdatePhysicalCue();
}

void BoardOwnerPairingRuntime::Retire(Connection connection) {
    if (SameConnection(connection, m_owner)) m_owner = {};
    if (!SameConnection(connection, m_claim) || !m_claimValid) return;
    m_consentAcknowledgmentActive = false;
    if (PairPrompt()) {
        (void)m_custody.CancelPair(m_attempt, m_claim.handle, m_claim.epoch);
    } else if (m_custody.GetOperation() == Controller::Operation::Pair) {
        // Keep custody's real live-link facts until native disconnect arrives.
        m_needsDisconnect = true;
    }
    m_claimValid = false;
    RetainHeldTail();
}

void BoardOwnerPairingRuntime::Disconnected(Connection connection) {
    // Native disconnect is fenced by connection epoch even if the shared
    // session was already retired and its token is no longer live.
    if (connection.epoch == 0U) return;
    m_custody.Disconnect(connection.handle, connection.epoch);
    if (connection.handle == m_owner.handle && connection.epoch == m_owner.epoch) m_owner = {};
    if (connection.handle == m_claim.handle && connection.epoch == m_claim.epoch) {
        m_consentAcknowledgmentActive = false;
        m_claimValid = false;
        m_needsDisconnect = false;
        RetainHeldTail();
    }
}

bool BoardOwnerPairingRuntime::ClaimLeaseMatches(
    Connection connection, Controller::Effect effect) const {
    return m_identityDurable && m_claimValid && !m_needsDisconnect &&
        SameConnection(connection, m_claim) && effect.session == m_attempt &&
        effect.connection == m_claim.handle && effect.epoch == m_claim.epoch &&
        effect.physicalEnd == m_end &&
        effect.token == m_custody.PendingEffect().token &&
        effect.type == Controller::EffectType::RequestSecurity &&
        m_custody.GetPhase() == Controller::Phase::AwaitSecurity;
}

bool BoardOwnerPairingRuntime::RegisterOwnerConnection(
    Connection connection, Controller::ConnectionSecurity security) {
    if (!m_identityDurable && m_record == BoardOwnerRecord::Active &&
        connection.epoch != 0U && connection.sessionToken != 0U &&
        m_settingsReset == SettingsReset::None && !m_needsDisconnect && !m_claimValid &&
        m_custody.GetOwnership() == Controller::Ownership::Owned &&
        m_custody.GetOperation() == Controller::Operation::None &&
        m_custody.GetPhase() == Controller::Phase::Idle && security.IsSavedOwner() &&
        security.bonds.count == m_custody.SavedPhoneCount()) {
        // The BLE owner already fenced native proof to this exact connection.
        // Reconnect is recovery intent only; verified promotion still owns Ready.
        (void)m_persistence.RequestIdentityRepair(
            (static_cast<uint64_t>(connection.epoch) << 32U) | connection.sessionToken);
    }
    if (!m_identityDurable || m_record != BoardOwnerRecord::Active ||
        connection.epoch == 0U || connection.sessionToken == 0U ||
        !m_custody.RegisterAuthenticatedOwnerConnection(connection.handle,
                                                        connection.epoch, security)) return false;
    m_owner = connection;
    return true;
}

bool BoardOwnerPairingRuntime::OwnerReady(Connection connection) const {
    const bool owner = SameConnection(connection, m_owner) ||
        (m_claimValid && SameConnection(connection, m_claim));
    return owner && !m_needsDisconnect && m_identityDurable &&
        m_record == BoardOwnerRecord::Active &&
        m_custody.GetOwnership() == Controller::Ownership::Owned &&
        m_custody.HasAuthenticatedOwnerConnection() &&
        (m_custody.GetOperation() == Controller::Operation::None || ClearPrompt());
}

} // namespace modules
