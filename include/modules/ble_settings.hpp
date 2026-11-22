#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "foxie_embedded/setting_descriptor.hpp"
#include "modules/control_mutation_service.hpp"
#include "modules/ble_event_queue.hpp"
#include "modules/settings_store.hpp"

#if defined(LITCH_BLE)
class NimBLECharacteristic;
class NimBLEService;
#endif

namespace modules {

using BleSettingsMutationCallback =
    void (*)(void* context, const char* key);

#if defined(LITCH_BLE)
class BleSettings {
  public:
    BleSettings() = default;

    void Configure(const SettingsStore* settings, BleEventQueue* eventQueue, std::atomic<uint32_t>* droppedEvents);
    bool CreateCharacteristics(NimBLEService& settingsService);
    void Reset();
    void SetProductAccess(bool allowed, uint16_t handle = 0xffffU) {
        if (!allowed) m_productAccess.store(false, std::memory_order_release);
        m_responseConnectionHandle.store(handle, std::memory_order_release);
        if (allowed) m_productAccess.store(true, std::memory_order_release);
    }
    using AccessPolicy = bool (*)(void*, uint16_t, bool);
    void SetAccessPolicy(AccessPolicy policy, void* context) { m_accessPolicy = policy; m_accessContext = context; }
    bool AllowsConnection(uint16_t handle, bool bootstrap = false) const {
        return (bootstrap || m_productAccess.load(std::memory_order_acquire)) &&
            (m_accessPolicy == nullptr || m_accessPolicy(m_accessContext, handle, bootstrap));
    }
    [[nodiscard]] uint32_t AdvanceConnectionEpoch();
    [[nodiscard]] uint32_t CurrentConnectionEpoch() const;
    [[nodiscard]] bool OwnsConnectionEpoch(uint32_t connectionEpoch) const;
    void ResetConnectionState();
    [[nodiscard]] bool ActivateConnectionEpoch(uint32_t connectionEpoch);
    void SetControlMutationService(ControlMutationService* service) {
        m_controlMutationService = service;
    }

    void HandleSettingWriteEvent(
        SettingsStore& settings,
        const BleSettingWriteEvent& event,
        BleSettingsMutationCallback mutationCallback = nullptr,
        void* mutationContext = nullptr);
    // Reports a change in app-visible values, excluding persistence-only revisions.
    bool UpdateChangeNotifications(const SettingsStore& settings, bool connected);
    void OnEnumerateRead(NimBLECharacteristic& characteristic);
    void OnReadWritePayload(std::string_view payload);
    void OnReadWriteSubscribe(uint16_t subValue);
    [[nodiscard]] bool HasReadWriteSubscription() const;

  private:
#if defined(LITCH_NATIVE)
    friend struct BleServiceConnectionEpochTestPeer;
#endif
    const SettingsStore* m_settings = nullptr;
    std::atomic<bool> m_productAccess{true};
    std::atomic<uint16_t> m_responseConnectionHandle{0xffffU};
    AccessPolicy m_accessPolicy = nullptr;
    void* m_accessContext = nullptr;
    ControlMutationService* m_controlMutationService = nullptr;
    BleEventQueue* m_eventQueue = nullptr;
    std::atomic<uint32_t>* m_droppedEvents = nullptr;
    NimBLECharacteristic* m_enumerateCharacteristic = nullptr;
    NimBLECharacteristic* m_readWriteCharacteristic = nullptr;
    std::atomic<uint16_t> m_subscriptionValue{0U};
    std::atomic<uint16_t> m_ingressSubscriptionValue{0U};
    std::atomic<uint32_t> m_ingressSubscriptionEpoch{0U};
    std::atomic<uint32_t> m_ingressSubscriptionRevision{0U};
#if defined(LITCH_NATIVE)
    void (*m_subscriptionActivationTestHook)(void*) = nullptr;
    void* m_subscriptionActivationTestContext = nullptr;
#endif
    uint32_t m_lastNotifiedRevision = 0;
    uint64_t m_pendingChangedIndices = 0;
    size_t m_enumerateCursor = 0;
    std::atomic<uint32_t> m_ingressConnectionEpoch{1U};
    std::atomic<uint32_t> m_activeConnectionEpoch{1U};
    static constexpr size_t TRACKED_CONTROL_REQUEST_CAPACITY =
        ControlMutationService::COMPLETION_CAPACITY + 1U;
    uint16_t m_trackedControlRequestIds[TRACKED_CONTROL_REQUEST_CAPACITY] = {};
    size_t m_trackedControlRequestCount = 0U;

    struct SnapshotEntry {
        bool used = false;
        char value[32] = {};
    };

    static constexpr size_t SNAPSHOT_CAPACITY = 64;
    SnapshotEntry m_snapshot[SNAPSHOT_CAPACITY] = {};

    void BuildInitialSnapshot(const SettingsStore& settings);
    bool EnqueueSettingRequest(const char* key, const char* valueJson, bool hasValue) const;
    void SendReadWriteSuccess(const SettingsStore& settings,
                              const fxe::SettingDescriptor& descriptor,
                              uint16_t requestId = 0U);
    void SendReadWriteFailure(const char* key,
                              const char* error,
                              uint16_t requestId = 0U);
    bool SendControlOutcome(const ControlMutationOutcome& outcome);
    bool SendAdmittedResponse(uint16_t subscriptionValue, const char* payload,
        size_t payloadLen, bool ok, const char* compactPayload = nullptr,
        size_t compactPayloadLen = 0U);
    [[nodiscard]] bool NotifySettingChanged(const SettingsStore& settings, const fxe::SettingDescriptor& descriptor);
    void TrackControlRequest(uint16_t requestId);
    void ReleaseTrackedControlRequests();
    void RemoveTrackedControlRequestAt(size_t index);
};
#endif

} // namespace modules
