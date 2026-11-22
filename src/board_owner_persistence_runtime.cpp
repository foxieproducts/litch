#include "firmware/board_owner_persistence_runtime.hpp"

#ifdef ARDUINO

namespace firmware {

bool BoardOwnerPersistenceRuntime::Begin() {
    if (m_prepared || m_receipt != 0U || m_settings.IsDisabled()) return false;
    auto& document = m_settings.Doc();
    const auto record = modules::ReadBoardOwner(document);
    if (record == modules::BoardOwnerRecord::Invalid) return false;
    const bool changed = !document.containsKey(modules::KEY_BOARD_OWNER_V1) ||
        document[modules::KEY_BOARD_OWNER_V1][0U].as<uint32_t>() != 3U;
    if (!modules::PrepareBoardOwnerJournal(document)) return false;
    if (changed) m_settings.NotifyDocChanged();
    m_durableOwner = record;
    m_durableBaseline = modules::ReadBoardOwnerAddBaseline(document);
    m_prepared = true;
    return true;
}

bool BoardOwnerPersistenceRuntime::SaveOwnerRecord(
    const modules::BoardOwnerRecord record, Receipt& receipt) {
    if (record == modules::BoardOwnerRecord::Adding) { receipt = 0U; return false; }
    return SaveIntent(record, {}, receipt);
}

bool BoardOwnerPersistenceRuntime::RequestIdentityRepair(const uint64_t lease) {
    if (lease == 0U || lease == m_identityRepairLease ||
        m_identity.IsDurable() || !m_settings.IsLoadRepairPending() ||
        !m_identity.MatchesExistingPreparedIdentity(m_settings.Doc()) ||
        m_durableOwner != modules::BoardOwnerRecord::Active) return false;
    Receipt receipt = 0U;
    if (!SaveIntent(modules::BoardOwnerRecord::Active, {}, receipt)) return false;
    m_identityRepairLease = lease;
    m_job = Job::IdentityRepair;
    return true;
}

bool BoardOwnerPersistenceRuntime::SaveAddingJournal(
    const modules::BoardOwnerAddBaseline& baseline, Receipt& receipt) {
    return SaveIntent(modules::BoardOwnerRecord::Adding, baseline, receipt);
}

bool BoardOwnerPersistenceRuntime::SaveIntent(const modules::BoardOwnerRecord record,
    const modules::BoardOwnerAddBaseline& baseline, Receipt& receipt) {
    receipt = 0U;
    if (!m_prepared || m_job != Job::None || m_settings.IsDisabled() ||
        record == modules::BoardOwnerRecord::Invalid) return false;
    auto& document = m_settings.Doc();
    const auto previous = modules::ReadBoardOwner(document);
    const auto previousBaseline = modules::ReadBoardOwnerAddBaseline(document);
    if (previous != m_durableOwner || !(previousBaseline == m_durableBaseline) ||
        (previous == modules::BoardOwnerRecord::Adding &&
         record == modules::BoardOwnerRecord::Adding && !(baseline == m_durableBaseline)) ||
        document[modules::KEY_BOARD_OWNER_V1][0U].as<uint32_t>() != 3U ||
        !(record == modules::BoardOwnerRecord::Adding
            ? modules::WriteBoardOwnerAdding(document, baseline)
            : modules::WriteBoardOwner(document, record))) return false;
    Receipt accepted = 0U;
    if (!m_worker.EnqueueTracked(document, accepted)) {
        (void)SetPreparedOwner(previous, previousBaseline);
        return false;
    }
    m_previousOwner = previous;
    m_requestedOwner = record;
    m_previousBaseline = previousBaseline;
    m_requestedBaseline = baseline;
    m_receipt = accepted;
    m_job = Job::Owner;
    m_outcome = Outcome::Pending;
    m_settings.NotifyDocChanged();
    receipt = accepted;
    return true;
}

bool BoardOwnerPersistenceRuntime::SetPreparedOwner(
    const modules::BoardOwnerRecord record, const modules::BoardOwnerAddBaseline& baseline) {
    auto& document = m_settings.Doc();
    if (!document.containsKey(modules::KEY_BOARD_OWNER_V1) ||
        modules::ReadBoardOwner(document) == modules::BoardOwnerRecord::Invalid ||
        record == modules::BoardOwnerRecord::Invalid ||
        (record == modules::BoardOwnerRecord::Adding &&
         !baseline.Valid())) return false;
    // A later preference allocation may have set the sticky overflow bit. The
    // private, already prepared scalar slots still require no allocation, so
    // restore their intent even then. Never cache a JsonArray across game saves:
    // those legitimately replace/compact the complete live JSON arena.
    JsonArray value = document[modules::KEY_BOARD_OWNER_V1].as<JsonArray>();
    if (value.size() != modules::OWNER_JOURNAL_SLOTS) return false;
    value[0U].set(3U);
    value[1U].set(static_cast<uint8_t>(record));
    value[2U].set(record == modules::BoardOwnerRecord::Adding ? baseline.saved.count : 0U);
    for (size_t peer = 0U; peer < 2U; ++peer) {
        const bool retained = record == modules::BoardOwnerRecord::Adding && peer < baseline.saved.count;
        value[3U + peer * 7U].set(retained ? baseline.saved.peers[peer].type : 0U);
        for (size_t byte = 0U; byte < 6U; ++byte)
            value[4U + peer * 7U + byte].set(retained ? baseline.saved.peers[peer].address[byte] : 0U);
    }
    const bool adding = record == modules::BoardOwnerRecord::Adding;
    value[17U].set(adding ? baseline.rpaCount : 0U);
    for (size_t i = 0U; i < modules::MAX_SAVED_PHONES; ++i) {
        for (size_t part = 0U; part < 14U; ++part) {
            const auto& address = part < 7U ? baseline.rpas[i].rpa : baseline.rpas[i].identity;
            const auto byte = part % 7U == 0U ? address.type : address.address[part % 7U - 1U];
            value[18U + i * 14U + part].set(adding && i < baseline.rpaCount ? byte : 0U);
        }
    }
    return true;
}

void BoardOwnerPersistenceRuntime::Update() {
    if (m_job == Job::IdentityRepair && m_outcome != Outcome::Pending) {
        (void)ReleaseCurrent();
        return;
    }
    if (m_job == Job::None || m_outcome != Outcome::Pending) return;
    using WorkerOutcome = SettingsPersistenceWorker::SaveOutcome;
    const auto outcome = m_worker.GetSaveOutcome(m_receipt);
    if (outcome == WorkerOutcome::Pending) return;
    if (m_job == Job::Owner || m_job == Job::IdentityRepair) {
        const auto owner = outcome == WorkerOutcome::Saved
            ? m_requestedOwner : m_previousOwner;
        const auto before = modules::ReadBoardOwner(m_settings.Doc());
        const auto& baseline = outcome == WorkerOutcome::Saved ? m_requestedBaseline : m_previousBaseline;
        if (!SetPreparedOwner(owner, baseline)) {
            m_outcome = Outcome::Failed;
            return;
        }
        if (before != owner) m_settings.NotifyDocChanged();
        if (outcome == WorkerOutcome::Saved) {
            m_durableOwner = owner;
            m_durableBaseline = baseline;
        }
    }
    // Terminal evidence is exposed only after the live private intent has been
    // reconciled on its owner task. Preferences/game changes remain untouched.
    m_outcome = outcome == WorkerOutcome::Saved ? Outcome::Saved : Outcome::Failed;
    if (m_job == Job::IdentityRepair) (void)ReleaseCurrent();
}

BoardOwnerPersistenceRuntime::Outcome BoardOwnerPersistenceRuntime::GetOutcome(
    const Receipt receipt) const {
    return m_job == Job::Owner && receipt != 0U && receipt == m_receipt
        ? m_outcome : Outcome::Unknown;
}

bool BoardOwnerPersistenceRuntime::ReleaseCurrent() {
    if ((m_outcome != Outcome::Saved && m_outcome != Outcome::Failed) ||
        !m_worker.ReleaseSaveReceipt(m_receipt)) return false;
    m_receipt = 0U;
    m_outcome = Outcome::Unknown;
    m_job = Job::None;
    m_nativeClearPort = {};
    m_nativeRollbackPort = {};
    return true;
}

bool BoardOwnerPersistenceRuntime::Release(const Receipt receipt) {
    return m_job == Job::Owner && receipt != 0U && receipt == m_receipt &&
        ReleaseCurrent();
}

bool BoardOwnerPersistenceRuntime::RequestNativeClear(const NativeClearPort port) {
    if (m_job != Job::None || port.clearAndReconcile == nullptr) return false;
    m_nativeClearPort = port;
    if (RequestNativeJob(modules::BoardOwnerRecord::Clearing, &ClearNativeOnWorker)) return true;
    m_nativeClearPort = {};
    return false;
}

bool BoardOwnerPersistenceRuntime::RequestNativeRollback(const NativeRollbackPort port) {
    if (m_job != Job::None || !m_durableBaseline.Valid() || port.rollbackAndReconcile == nullptr) return false;
    m_nativeRollbackPort = port;
    if (RequestNativeJob(modules::BoardOwnerRecord::Adding, &RollbackNativeOnWorker)) return true;
    m_nativeRollbackPort = {};
    return false;
}

bool BoardOwnerPersistenceRuntime::RequestNativeJob(const modules::BoardOwnerRecord expected,
    bool (*prerequisite)(void*)) {
    if (!m_prepared || m_job != Job::None || m_settings.IsDisabled() ||
        m_durableOwner != expected ||
        modules::ReadBoardOwner(m_settings.Doc()) != m_durableOwner ||
        !(modules::ReadBoardOwnerAddBaseline(m_settings.Doc()) == m_durableBaseline) ||
        m_settings.Doc()[modules::KEY_BOARD_OWNER_V1][0U].as<uint32_t>() != 3U ||
        m_settings.Doc().overflowed()) return false;
    Receipt accepted = 0U;
    if (!m_worker.EnqueueTracked(m_settings.Doc(), accepted, nullptr, nullptr,
                                prerequisite, this)) return false;
    m_receipt = accepted;
    m_outcome = Outcome::Pending;
    m_job = expected == modules::BoardOwnerRecord::Clearing ? Job::NativeClear : Job::NativeRollback;
    return true;
}

bool BoardOwnerPersistenceRuntime::RollbackNativeOnWorker(void* const context) {
    auto& self = *static_cast<BoardOwnerPersistenceRuntime*>(context);
    // The retained worker receipt excludes replacement until terminal release.
    return self.m_nativeRollbackPort.rollbackAndReconcile(
        self.m_nativeRollbackPort.context, self.m_durableBaseline);
}

BoardOwnerPersistenceRuntime::Outcome
BoardOwnerPersistenceRuntime::GetNativeRollbackOutcome() const {
    return m_job == Job::NativeRollback ? m_outcome : Outcome::Unknown;
}

bool BoardOwnerPersistenceRuntime::ReleaseNativeRollback() {
    return m_job == Job::NativeRollback && ReleaseCurrent();
}

bool BoardOwnerPersistenceRuntime::ClearNativeOnWorker(void* const context) {
    auto& self = *static_cast<BoardOwnerPersistenceRuntime*>(context);
    return self.m_nativeClearPort.clearAndReconcile(
        self.m_nativeClearPort.context);
}

BoardOwnerPersistenceRuntime::Outcome
BoardOwnerPersistenceRuntime::GetNativeClearOutcome() const {
    return m_job == Job::NativeClear ? m_outcome : Outcome::Unknown;
}

bool BoardOwnerPersistenceRuntime::ReleaseNativeClear() {
    return m_job == Job::NativeClear && ReleaseCurrent();
}

} // namespace firmware

#endif
