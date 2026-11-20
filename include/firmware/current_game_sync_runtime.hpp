#pragma once

#ifdef ARDUINO

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "firmware/current_game_log_service.hpp"
#include "firmware/settings_persistence_worker.hpp"
#include "modules/current_game_sync_source.hpp"
#include "modules/current_game_sync_transfer.hpp"
#include "modules/game_persistence_coordinator.hpp"

namespace firmware {

// One bounded BLE/settings_io handoff around the existing sync transfer. The
// main loop never touches current-line storage and settings_io never calls BLE.
class CurrentGameSyncRuntime final {
  public:
    CurrentGameSyncRuntime(
        SettingsPersistenceWorker& worker,
        CurrentGameLogService& log,
        modules::current_game_log::StorageOps storage,
        fxe::SettingsStore& settings);

    bool Configure(const JsonDocument& initialDocument);
    bool IsConfigured() const {
        return m_configured.load(std::memory_order_acquire);
    }
    bool Admit(const uint8_t* bytes, size_t size, uint32_t connectionEpoch,
               bool otaBusy);
    bool Peek(uint8_t* out, size_t capacity, size_t& outSize,
              uint32_t& token, uint32_t connectionEpoch);
    void Accept(uint32_t token, uint32_t connectionEpoch);
    // Main-loop side of the archive acknowledgement: ask SettingsStore to
    // snapshot its live document through the existing worker admission path.
    void ServiceMain();
    // Copies worker-published durable content availability without storage I/O.
    // Archive acknowledgement changes do not advance this publication revision.
    bool CopyLinePublication(uint32_t& revision,
                             std::array<uint8_t, 2U>& body) const;
    // Main-loop admission copies only worker-published durable metadata; it
    // never reads CurrentGameLogService's mutable pointer or accesses storage.
    modules::ControlCoordinatorResult CheckReplacementAdmission(
        const modules::GameHistoryPersistenceState& current,
        modules::GamePersistenceCoordinator::ReplacementKind kind) const;

  private:
    static void ServiceThunk(void* context);
    static modules::current_game_sync::ArchiveAdmission AdmitArchiveThunk(
        void* context,
        const modules::current_game_log::CommitPointer& authority,
        const modules::current_game_sync::Digest& digest);
    void ServiceWorker();
    void PublishNextOutput();
    void PublishAuthority();
    struct DurableAuthority {
        modules::game_history::GameId gameId{};
        uint64_t lineRevision = 0U;
        uint16_t currentPly = 0U;
        bool present = false;
        bool archiveRequired = false;
    };
    mutable std::atomic_flag m_authorityLock = ATOMIC_FLAG_INIT;
    DurableAuthority m_authority{};
    uint32_t m_lineStateRevision = 1U;

    static constexpr size_t FRAME_BYTES =
        modules::game_history::MIN_ATT_PAYLOAD_BYTES;
    enum SlotState : uint8_t { Free = 0U, Writing = 1U, Ready = 2U,
                               Accepted = 3U };

    SettingsPersistenceWorker& m_worker;
    CurrentGameLogService& m_log;
    fxe::SettingsStore& m_settings;
    modules::current_game_sync::CurrentGameSyncSource m_source;
    modules::current_game_sync::CurrentGameSyncTransfer m_transfer;
    std::array<uint8_t, FRAME_BYTES> m_input{};
    uint8_t m_inputSize = 0U;
    uint32_t m_inputEpoch = 0U;
    bool m_inputOtaBusy = false;
    std::atomic<uint8_t> m_inputState{Free};
    std::array<uint8_t, FRAME_BYTES> m_output{};
    uint8_t m_outputSize = 0U;
    uint32_t m_outputToken = 0U;
    uint32_t m_outputEpoch = 0U;
    std::atomic<uint8_t> m_outputState{Free};
    uint32_t m_activeEpoch = 0U;
    std::atomic<bool> m_configured{false};
};

} // namespace firmware

#endif
