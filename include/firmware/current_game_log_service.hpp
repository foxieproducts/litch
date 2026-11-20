#pragma once

#ifdef ARDUINO

#include <atomic>
#include <cstdint>

#include <ArduinoJson.h>

#include "firmware/settings_persistence_worker.hpp"
#include "modules/current_game_log.hpp"
#include "modules/current_game_sync_transfer.hpp"
#include "modules/game_history_capture.hpp"
#include "modules/game_persistence.hpp"

namespace firmware {

// Reconciles the exact game snapshot already selected by the settings worker
// into the two-slot current-line log before that same JSON document is saved.
// It owns no task or queue; all callbacks run synchronously on settings_io.
class CurrentGameLogService final {
  public:
    explicit CurrentGameLogService(
        modules::current_game_log::StorageOps storage);

    bool Configure(SettingsPersistenceWorker& worker,
                   const JsonDocument& initialDocument);
    uint32_t FailureCount() const { return m_failureCount; }
    modules::current_game_log::Result LastResult() const {
        return m_lastResult;
    }
    modules::current_game_log::CommitPointer CommittedPointer() const {
        return m_committedPointer;
    }
    modules::current_game_sync::ArchiveAdmission AdmitArchiveAcknowledgement(
        const modules::current_game_log::CommitPointer& authority,
        const modules::current_game_sync::Digest& digest);
    bool ConsumeArchiveSaveRequest();
    bool TakeArchiveCompletion(
        bool& outSucceeded,
        modules::current_game_log::CommitPointer& outAuthority);

  private:
    struct LineContext;

    static bool PrepareThunk(void* context, JsonDocument& document);
    static void ResultThunk(
        void* context,
        SettingsPersistenceWorker::SnapshotTransactionOutcome outcome,
        const JsonDocument& document);
    static bool EnqueuePointerThunk(
        void* context,
        const modules::current_game_log::CommitPointer& pointer,
        modules::current_game_log::SettingsOps::Receipt& outReceipt);
    static bool ReadLineRecordThunk(
        void* context,
        uint16_t index,
        modules::game_history::Record& out);

    bool Prepare(JsonDocument& document);
    bool PrepareArchiveAcknowledgement(
        JsonDocument& document,
        const modules::current_game_log::CommitPointer& authority);
    void Complete(
        SettingsPersistenceWorker::SnapshotTransactionOutcome outcome);
    bool EnqueuePointer(
        const modules::current_game_log::CommitPointer& pointer,
        modules::current_game_log::SettingsOps::Receipt& outReceipt);
    bool BuildLineRecord(LineContext& context,
                         uint16_t index,
                         modules::game_history::Record& out);
    bool ReadStoredRecord(LineContext& context,
                          uint16_t index,
                          modules::game_history::Record& out) const;
    bool BuildSnapshotRecord(
        const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
        uint16_t ply,
        uint64_t revision,
        modules::game_history::Record& out) const;
    bool PrepareReplacement(JsonDocument& document);
    bool ReplaceSnapshot(
        JsonDocument& document,
        const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
        const modules::current_game_log::CommitPointer& authority);
    bool ReconcileSnapshot(
        JsonDocument& document,
        const modules::game_history::GameHistoryCaptureSnapshot& snapshot,
        const modules::current_game_log::CommitPointer& authority);
    bool RewriteSnapshot(
        const modules::current_game_log::CommitPointer& authority,
        const modules::game_history::GameHistoryCaptureSnapshot& snapshot);
    void ResetLog();
    modules::current_game_log::Result EnsureOpen(
        const modules::current_game_log::CommitPointer& authority);
    bool Fail(modules::current_game_log::Result result);

    modules::current_game_log::StorageOps m_storage;
    SettingsPersistenceWorker* m_worker = nullptr;
    modules::current_game_log::SettingsOps m_settingsOps;
    modules::current_game_log::CurrentGameLog m_log;
    JsonDocument* m_preparedDocument = nullptr;
    modules::current_game_log::CommitPointer m_pendingPointer{};
    modules::current_game_log::CommitPointer m_committedPointer{};
    modules::current_game_log::SettingsOps::Receipt m_pendingReceipt = 0U;
    modules::current_game_log::SettingsOps::Receipt m_nextReceipt = 0U;
    modules::current_game_log::Result m_lastResult =
        modules::current_game_log::Result::Ready;
    uint32_t m_failureCount = 0U;
    bool m_pendingRemoval = false;
    bool m_configured = false;
    modules::current_game_log::CommitPointer m_archiveRequestAuthority{};
    modules::current_game_sync::Digest m_archiveRequestDigest{};
    std::atomic<bool> m_archiveSaveRequested{false};
    std::atomic<uint8_t> m_archiveCompletion{0U}; // none/saved/failed
    bool m_archiveRequestPending = false;
    bool m_archiveTransactionPrepared = false;
};

} // namespace firmware

#endif
