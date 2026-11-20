#pragma once

#include <cstdint>

#include <ArduinoJson.h>

#include "modules/game_history_capture.hpp"
#include "modules/game_history_snapshot_reader.hpp"

namespace firmware {

// Runs exclusively from the settings storage worker after its source snapshot
// has reached LittleFS. This preserves the settings snapshot as authority and
// keeps journal I/O away from the cooperative loop and BLE callbacks.
class GameHistoryArchiveService final {
  public:
    explicit GameHistoryArchiveService(
        modules::game_history::GameHistoryJournalStorageOps storage)
        : m_journal(storage),
          m_capture(m_journal) {}

    bool Configure(uint64_t newJournalId);
    void OnDurableSettingsSnapshot(const JsonDocument& document);

    bool IsReady() const { return m_journal.IsOpen(); }
    uint32_t FailureCount() const { return m_failureCount; }
    modules::game_history::GameHistoryJournal& Journal() {
        return m_journal;
    }

  private:
    modules::game_history::GameHistoryJournal m_journal;
    modules::game_history::GameHistoryCapture m_capture;
    modules::GamePersistenceState m_snapshot{};
    uint64_t m_newJournalId = 0U;
    uint32_t m_failureCount = 0U;
    bool m_configured = false;
};

} // namespace firmware
