#pragma once

#ifdef ARDUINO

#include "firmware/settings_persistence_worker.hpp"
#include "modules/game_history_journal.hpp"

namespace firmware {

// LittleFS adapter for the S3 game-history storage worker. The adapter only
// uses the settings worker's existing flash/render windows, including flush
// handoff. Calls from another task fail before filesystem access.
class GameHistoryJournalStorage final {
  public:
    explicit GameHistoryJournalStorage(
        SettingsPersistenceWorker& worker)
        : m_worker(worker) {}

    modules::game_history::GameHistoryJournalStorageOps Ops();

  private:
    class FlashLease final {
      public:
        explicit FlashLease(SettingsPersistenceWorker& worker);
        ~FlashLease();
        bool IsAcquired() const { return m_acquired; }

        FlashLease(const FlashLease&) = delete;
        FlashLease& operator=(const FlashLease&) = delete;

      private:
        SettingsPersistenceWorker& m_worker;
        bool m_acquired = false;
    };

    static modules::game_history::JournalStorageStatus Stat(
        void* context,
        modules::game_history::JournalArtifact artifact,
        uint32_t& outSize);
    static bool Read(void* context,
                     modules::game_history::JournalArtifact artifact,
                     uint32_t offset,
                     uint8_t* out,
                     size_t length);
    static bool Replace(void* context,
                        modules::game_history::JournalArtifact artifact,
                        const uint8_t* bytes,
                        size_t length);
    static bool Append(void* context,
                       modules::game_history::JournalArtifact artifact,
                       const uint8_t* bytes,
                       size_t length);
    static bool Promote(void* context,
                        modules::game_history::JournalArtifact from,
                        modules::game_history::JournalArtifact to);

    SettingsPersistenceWorker& m_worker;
};

} // namespace firmware

#endif
