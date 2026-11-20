#include "foxie_embedded/settings_store.hpp"

#include <cstring>

#include "settings_store_internal.hpp"

#include "foxie_embedded/logger.hpp"

namespace fxe {

namespace {

uint32_t SaveRetryDelayMs(const uint8_t failures) {
    // Retry failures, not ordinary edits, at 100/200/.../5000 ms. The cap
    // leaves recovery possible without a tight loop on a persistently bad path.
    const uint8_t shift = failures == 0U ? 0U : (failures > 7U ? 6U : failures - 1U);
    const uint32_t delay = 100U << shift;
    return delay > 5000U ? 5000U : delay;
}

const char* ResolveStoredKey(const JsonDocument& doc, const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return nullptr;
    }

    const char* existing = FindTopLevelObjectKeyCaseInsensitive(doc, key);
    if (existing != nullptr) {
        return existing;
    }

    char upperKey[64] = {};
    if (!CopyUppercaseSettingKey(key, upperKey, sizeof(upperKey))) {
        return nullptr;
    }

    existing = FindTopLevelObjectKeyCaseInsensitive(doc, upperKey);
    if (existing != nullptr) {
        return existing;
    }

    return key;
}

uint32_t NextSettingsStoreInstanceId() {
    static uint32_t s_next = 0;
    ++s_next;
    if (s_next == 0U) {
        ++s_next;
    }
    return s_next;
}

} // namespace

void SettingsStore::SetError(const SettingsStoreError err, const char* msg,
                           const StorageFailureEvidence evidence) {
    if (evidence == StorageFailureEvidence::Confirmed) {
        m_confirmedStorageFailure.store(true, std::memory_order_release);
        m_confirmedStorageFailureCount.fetch_add(1U, std::memory_order_acq_rel);
    }
    m_lastError = err;
    ++m_errorCount;

    if (msg && msg[0] != '\0') {
        fxe::Error("settings: %s", msg);
    }
}

void SettingsStore::MarkLoadRepairPending(const char* message, const SettingsStoreError error) {
    // Read/repair provenance is not a user mutation or an attempted save.
    // Keep the recovered RAM and original files until a later legitimate save.
    m_loadRepairPending.store(true, std::memory_order_release);
    if (error != SettingsStoreError::None) SetError(error, message);
    fxe::Warn("settings: %s; recovered values pending user mutation", message);
}

void SettingsStore::NoteSaveFailure() {
    uint8_t count = m_consecutiveSaveFailures.load(std::memory_order_relaxed);
    while (count < 7U && !m_consecutiveSaveFailures.compare_exchange_weak(
               count, static_cast<uint8_t>(count + 1U),
               std::memory_order_release, std::memory_order_relaxed)) {}
    m_asyncRetryNeeded.store(true, std::memory_order_release);
}

SettingsStore::SettingsStore(const SettingsStoreConfig& cfg)
    : m_cfg(cfg), m_instanceId(NextSettingsStoreInstanceId()) {
    const char* src = (m_cfg.path != nullptr) ? m_cfg.path : "settings.json";
    std::strncpy(m_path, src, sizeof(m_path));
    m_path[sizeof(m_path) - 1] = '\0';
}

void SettingsStore::DisableForBoot() {
    m_doc.clear();
    m_disabled = true;
    m_loadRepairPending.store(false, std::memory_order_release);
    m_mounted = false;
    m_lastError = SettingsStoreError::Unsupported;
    ++m_errorCount;
    ++m_revision;
}

void SettingsStore::DisablePersistenceForSession() {
    // Change durability policy, never the live game, preferences or safety facts.
    if (m_disabled.exchange(true, std::memory_order_acq_rel)) return;
    m_loadRepairPending.store(false, std::memory_order_release);
    if (GetLastError() == SettingsStoreError::None) {
        SetError(SettingsStoreError::Unsupported, "persistence owner unavailable");
    }
    ++m_revision;
}

void SettingsStore::MarkDirty() {
    NotifyDocChanged();
    // Debounce deferred saves from the most recent modification. This avoids
    // doing filesystem IO while the user is still actively adjusting settings
    // (e.g., holding a button that auto-repeats).
    if (!m_dirty) {
        m_dirty = true;
    }
    m_dirtyTime.Reset();
}

void SettingsStore::MarkMustSaveDirty() {
    NotifyDocChanged();
    m_mustSaveDirty = true;
    m_mustSaveDirtyTime.Reset();
}

void SettingsStore::NotifyDocChanged() {
    ++m_revision;
}

bool SettingsStore::WaitForDirectStorageAccess() {
    if (!m_beforeDirectStorageAccessCb ||
        m_beforeDirectStorageAccessCb()) {
        return true;
    }
    SetError(SettingsStoreError::AccessTimeout,
             "timed out waiting for asynchronous settings I/O");
    return false;
}

uint32_t SettingsStore::ReadValue(const char* key, const uint32_t defaultValue) const {
    const char* resolved = ResolveStoredKey(m_doc, key);
    if (resolved == nullptr) {
        return defaultValue;
    }

    JsonVariantConst value = m_doc[resolved];
    if (value.is<bool>()) {
        return value.as<bool>() ? 1U : 0U;
    }

    return m_doc[resolved] | defaultValue;
}

int32_t SettingsStore::ReadValue(const char* key, const int32_t defaultValue) const {
    const char* resolved = ResolveStoredKey(m_doc, key);
    if (resolved == nullptr) {
        return defaultValue;
    }

    JsonVariantConst value = m_doc[resolved];
    if (value.is<bool>()) {
        return value.as<bool>() ? 1 : 0;
    }

    return m_doc[resolved] | defaultValue;
}

bool SettingsStore::ReadValue(const char* key, const bool defaultValue) const {
    const char* resolved = ResolveStoredKey(m_doc, key);
    if (resolved == nullptr) {
        return defaultValue;
    }

    JsonVariantConst value = m_doc[resolved];
    if (value.is<bool>()) {
        return value.as<bool>();
    }

    if (value.is<uint64_t>()) {
        return value.as<uint64_t>() != 0U;
    }

    if (value.is<int64_t>()) {
        return value.as<int64_t>() != 0;
    }

    return defaultValue;
}

const char* SettingsStore::ReadValue(const char* key, const char* defaultValue) const {
    const char* resolved = ResolveStoredKey(m_doc, key);
    if (resolved == nullptr) {
        return defaultValue;
    }

    return m_doc[resolved] | defaultValue;
}

void SettingsStore::Update() {
    if (m_disabled) { return; }
    if (m_asyncRetryNeeded.exchange(false, std::memory_order_acq_rel)) {
        m_mustSaveDirty = true;
        m_mustSaveDirtyTime.Reset();
        m_retryCooldown.Reset();
        m_retryCooldownActive = true;
    }
    if (m_retryCooldownActive) {
        const uint8_t failures = m_consecutiveSaveFailures.load(std::memory_order_acquire);
        if (failures != 0U && !m_retryCooldown.HasElapsed(SaveRetryDelayMs(failures))) {
            return;
        }
        m_retryCooldownActive = false;
    }
    if (!m_dirty && !m_mustSaveDirty) {
        return;
    }

    const bool ordinarySaveDue =
        m_dirty && m_dirtyTime.HasElapsed(m_cfg.deferredSaveDelayMs);
    const bool mustSaveDue =
        m_mustSaveDirty &&
        m_mustSaveDirtyTime.HasElapsed(m_cfg.mustSaveDelayMs);
    if (!ordinarySaveDue && !mustSaveDue) {
        return;
    }

    if (!HasAsyncSaveRequestCallback() &&
        !m_cfg.allowSynchronousDeferredSaves) {
        return;
    }

    (void)SaveNow();
}

bool SettingsStore::SaveNow() {
    if (m_disabled) { return false; }
    if (!HasAsyncSaveRequestCallback()) {
        return SaveNowBlocking();
    }

    // Admission only prepares and queues a RAM snapshot. The asynchronous
    // owner guards its actual storage access; running the caller's blocking
    // guard here would repeat it on every Deferred retry.
    // Queue acceptance still cannot clear a prior durability failure.
    PrepareDocumentForPersistence();

    const auto admission = m_asyncSaveAdmissionCb
        ? m_asyncSaveAdmissionCb(m_doc)
        : (m_asyncSaveRequestCb(m_doc) ? AsyncSaveAdmission::Accepted
                                     : AsyncSaveAdmission::Rejected);
    if (admission == AsyncSaveAdmission::Accepted) {
        m_dirty = false;
        m_mustSaveDirty = false;
        m_persistenceGuardArmed = false;
        return true;
    }
    if (admission == AsyncSaveAdmission::Deferred) {
        // An explicit SaveNow may not have been dirty. Preserve that request
        // too, without postponing an existing due save or changing errors.
        if (!m_dirty && !m_mustSaveDirty) {
            m_mustSaveDirty = true;
            m_mustSaveDirtyTime.Reset();
        }
        return false;
    }

    ReportAsyncSaveFailure(SettingsStoreError::AdmissionRejected);
    fxe::Error("settings: async save request rejected");
    m_mustSaveDirty = true;
    m_mustSaveDirtyTime.Reset();
    return false;
}

bool SettingsStore::SaveNowBlocking() {
    if (m_disabled) { return false; }
    // Clear prior error so a failure reflects the current attempt.
    m_lastError = SettingsStoreError::None;
    if (!WaitForDirectStorageAccess()) {
        return false;
    }

    const bool ok = SaveImpl();
    if (ok) {
        m_loadRepairPending.store(false, std::memory_order_release);
        m_terminalSaveFailure.store(false, std::memory_order_release);
        m_confirmedStorageFailure.store(false, std::memory_order_release);
        m_asyncRetryNeeded.store(false, std::memory_order_release);
        m_consecutiveSaveFailures.store(0U, std::memory_order_release);
        m_retryCooldownActive = false;
        m_dirty = false;
        m_mustSaveDirty = false;
        m_persistenceGuardArmed = false;
        return true;
    }

    // Defensive: if SaveImpl() returns false without setting an error, force a
    // generic error so callers can detect/diagnose the failure.
    if (m_lastError == SettingsStoreError::None) {
        SetError(SettingsStoreError::PreparationFailed, "save failed (no error set)");
    }

    m_terminalSaveFailure.store(true, std::memory_order_release);
    // The library retains retry state; the board runtime may choose session-only.
    // Preserve dirty so Update() will retry later.
    m_mustSaveDirty = true;
    m_mustSaveDirtyTime.Reset();
    NoteSaveFailure();
    // Direct I/O runs on this owner, so its cooldown starts at the failure,
    // rather than being restarted by the next deferred Update.
    m_asyncRetryNeeded.store(false, std::memory_order_release);
    m_retryCooldown.Reset();
    m_retryCooldownActive = true;
    return false;
}

} // namespace fxe
