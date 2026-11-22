#pragma once

#ifdef ARDUINO

#include <array>
#include <LittleFS.h>

#include "firmware/settings_persistence_worker.hpp"
#include "modules/current_game_log.hpp"

namespace firmware {

// LittleFS adapter used only by CurrentGameLogService on settings_io. Every
// filesystem operation takes the configured storage-access lease. Bounded cached parser reads copy RAM without taking flash ownership.
class CurrentGameLogStorage final {
  public:
    explicit CurrentGameLogStorage(SettingsPersistenceWorker& worker)
        : m_worker(worker) {}

    modules::current_game_log::StorageOps Ops();

  private:
    class FlashLease final {
      public:
        explicit FlashLease(CurrentGameLogStorage& storage);
        ~FlashLease();
        bool IsAcquired() const { return m_acquired; }

        FlashLease(const FlashLease&) = delete;
        FlashLease& operator=(const FlashLease&) = delete;

      private:
        CurrentGameLogStorage& m_storage;
        bool m_acquired = false;
    };

    static modules::current_game_log::StorageStatus Stat(
        void* context,
        modules::current_game_log::Slot slot,
        uint32_t& outSize);
    static bool Read(void* context,
                     modules::current_game_log::Slot slot,
                     uint32_t offset,
                     uint8_t* out,
                     size_t length);
    static bool Replace(void* context,
                        modules::current_game_log::Slot slot,
                        const uint8_t* bytes,
                        size_t length);
    static bool Append(void* context,
                       modules::current_game_log::Slot slot,
                       const uint8_t* bytes,
                       size_t length);
    static bool Truncate(void* context,
                         modules::current_game_log::Slot slot,
                         uint32_t length);
    static bool BeginBatch(void* context);
    static void EndBatch(void* context);
    void CloseBatchRead();
    void CompleteBatchOperation();
    void NoteIoFailure(fxe::SettingsStoreError error);

    SettingsPersistenceWorker& m_worker;
    File m_batchReadFile;
    modules::current_game_log::Slot m_batchReadSlot =
        modules::current_game_log::Slot::A;
    // Bounded read-ahead avoids repeated storage access for small parser reads.
    std::array<uint8_t, 512U> m_batchReadBytes{};
    uint32_t m_batchReadOffset = 0U;
    size_t m_batchReadLength = 0U;
    bool m_batchActive = false;
};

} // namespace firmware

#endif
