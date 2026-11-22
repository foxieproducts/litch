#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "nimble_advertising_payload.hpp"

inline constexpr uint8_t BLE_GAP_LE_PHY_2M_MASK = 0x02U;

class NimBLECharacteristic;
class NimBLEAdvertising;
class NimBLEServer;
class NimBLEAddress {
  public:
    const uint8_t* getVal() const { return m_bytes; }
  private:
    uint8_t m_bytes[6]{};
};

class NimBLEConnInfo {
  public:
    explicit NimBLEConnInfo(uint16_t mtu = 23U, uint16_t handle = 7U) : m_mtu(mtu), m_handle(handle) {}
    uint16_t getMTU() const { return m_mtu; }
    uint16_t getConnHandle() const { return m_handle; }
    NimBLEAddress getAddress() const { return {}; }
    uint16_t getConnInterval() const { return 12U; }
    uint16_t getConnTimeout() const { return 200U; }
    uint16_t getConnLatency() const { return 0U; }
  private:
    uint16_t m_mtu;
    uint16_t m_handle;
};

class NimBLECharacteristicCallbacks {
  public:
    virtual ~NimBLECharacteristicCallbacks() = default;
    virtual void onRead(NimBLECharacteristic* characteristic,
                        NimBLEConnInfo& connInfo) {
        (void)characteristic;
        (void)connInfo;
    }
    virtual void onWrite(NimBLECharacteristic* characteristic,
                         NimBLEConnInfo& connInfo) {
        (void)characteristic;
        (void)connInfo;
    }
    virtual void onSubscribe(NimBLECharacteristic* characteristic,
                             NimBLEConnInfo& connInfo,
                             uint16_t subValue) {
        (void)characteristic;
        (void)connInfo;
        (void)subValue;
    }
};

namespace NIMBLE_PROPERTY {
constexpr uint32_t READ = 1U << 0;
constexpr uint32_t WRITE = 1U << 1;
constexpr uint32_t WRITE_NR = 1U << 2;
constexpr uint32_t NOTIFY = 1U << 3;
constexpr uint32_t INDICATE = 1U << 4;
} // namespace NIMBLE_PROPERTY

class NimBLECharacteristic {
  public:
    NimBLECharacteristic() = default;
    NimBLECharacteristic(std::string uuid, uint32_t properties, uint16_t maxLen)
        : m_uuid(std::move(uuid)),
          m_properties(properties),
          m_maxLen(maxLen) {}

    const std::string& Uuid() const { return m_uuid; }
    NimBLECharacteristicCallbacks* Callbacks() const { return m_callbacks; }
    void setCallbacks(NimBLECharacteristicCallbacks* callbacks) {
        m_callbacks = callbacks;
    }

    bool setValue(const uint8_t* data, size_t len) {
        m_value.assign(reinterpret_cast<const char*>(data), len);
        return true;
    }

    std::string getValue() const { return m_value; }
    template <typename T>
    std::enable_if_t<std::is_trivially_copyable_v<T>, T> getValue() const {
        T value{};
        if (m_value.size() >= sizeof(value)) {
            std::memcpy(&value, m_value.data(), sizeof(value));
        }
        return value;
    }
    size_t getLength() const { return m_value.size(); }

    bool notify(const uint8_t* data, size_t len, uint16_t connHandle) {
        m_lastConnHandle = connHandle;
        return notify(data, len);
    }

    bool notify(const uint8_t* data, size_t len) {
        if (m_notifyAdmission != nullptr && !m_notifyAdmission(m_notifyAdmissionContext, data, len)) return false;
        if (!m_notifyResult) return false;
        m_lastTx.assign(reinterpret_cast<const char*>(data),
                       len < m_notifyPayloadLimit ? len : m_notifyPayloadLimit);
        m_lastWasIndication = false;
        ++m_notifyCount;
        if (m_notifyHook != nullptr) {
            m_notifyHook(m_notifyHookContext);
        }
        return true;
    }

    bool notify() {
        // This overload only schedules an attribute update in NimBLE.
        m_lastTx = m_value.substr(0, m_notifyPayloadLimit);
        m_lastWasIndication = false;
        ++m_notifyCount;
        if (m_notifyHook != nullptr) {
            m_notifyHook(m_notifyHookContext);
        }
        return true;
    }

    void SetNotifyPayloadLimit(size_t limit) { m_notifyPayloadLimit = limit; }
    void SetNotifyResult(bool accepted) { m_notifyResult = accepted; }
    void SetIndicateResult(bool accepted) { m_indicateResult = accepted; }
    uint16_t LastConnHandle() const { return m_lastConnHandle; }

    void SetNotifyHook(void (*hook)(void*), void* context = nullptr) {
        m_notifyHook = hook;
        m_notifyHookContext = context;
    }

    bool indicate(const uint8_t* data, size_t len) {
        if (!m_indicateResult) return false;
        m_lastTx.assign(reinterpret_cast<const char*>(data), len);
        m_lastWasIndication = true;
        ++m_indicateCount;
        if (m_notifyHook != nullptr) {
            m_notifyHook(m_notifyHookContext);
        }
        return true;
    }
    bool indicate(const uint8_t* data, size_t len, uint16_t connHandle) {
        m_lastConnHandle = connHandle;
        return indicate(data, len);
    }

    void ClearTx() {
        m_lastTx.clear();
        m_lastWasIndication = false;
        m_notifyCount = 0U;
        m_indicateCount = 0U;
    }

    const std::string& LastTx() const { return m_lastTx; }
    bool LastWasIndication() const { return m_lastWasIndication; }
    void SetNotifyAdmission(bool (*hook)(void*, const uint8_t*, size_t), void* context) {
        m_notifyAdmission = hook; m_notifyAdmissionContext = context;
    }
    uint32_t NotifyCount() const { return m_notifyCount; }
    uint32_t IndicateCount() const { return m_indicateCount; }

  private:
    std::string m_uuid;
    uint32_t m_properties = 0U;
    uint16_t m_maxLen = 0U;
    std::string m_value;
    std::string m_lastTx;
    bool m_lastWasIndication = false;
    uint32_t m_notifyCount = 0U;
    uint32_t m_indicateCount = 0U;
    size_t m_notifyPayloadLimit = static_cast<size_t>(-1);
    bool (*m_notifyAdmission)(void*, const uint8_t*, size_t) = nullptr;
    void* m_notifyAdmissionContext = nullptr;
    bool m_notifyResult = true;
    bool m_indicateResult = true;
    uint16_t m_lastConnHandle = 0xffffU;
    NimBLECharacteristicCallbacks* m_callbacks = nullptr;
    void (*m_notifyHook)(void*) = nullptr;
    void* m_notifyHookContext = nullptr;
};

class NimBLEServerCallbacks {
  public:
    virtual ~NimBLEServerCallbacks() = default;
    virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
    virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
    virtual void onConnParamsUpdate(NimBLEConnInfo&) {}
    virtual void onMTUChange(uint16_t, NimBLEConnInfo&) {}
    virtual void onPhyUpdate(NimBLEConnInfo&, uint8_t, uint8_t) {}
};

class NimBLEService {
  public:
    NimBLECharacteristic* createCharacteristic(const char* uuid,
                                               uint32_t properties,
                                               uint16_t maxLen = 512U) {
        m_characteristics.push_back(std::make_unique<NimBLECharacteristic>(
            uuid != nullptr ? uuid : "", properties, maxLen));
        return m_characteristics.back().get();
    }

    bool start() { return ++s_startCalls != s_failStartCall; }
    static void FailStartOnCall(uint32_t ordinal) {
        s_startCalls = 0U; s_failStartCall = ordinal;
    }
    NimBLECharacteristic* FindCharacteristic(const std::string& uuid) const {
        for (const auto& c : m_characteristics) if (c->Uuid() == uuid) return c.get();
        return nullptr;
    }
    NimBLECharacteristic* CharacteristicAt(const size_t index) const {
        return index < m_characteristics.size()
                   ? m_characteristics[index].get()
                   : nullptr;
    }

  private:
    std::vector<std::unique_ptr<NimBLECharacteristic>> m_characteristics;
    inline static uint32_t s_startCalls = 0U;
    inline static uint32_t s_failStartCall = 0U;
};

class NimBLEServer {
  public:
    void setCallbacks(NimBLEServerCallbacks* callbacks, bool) { m_callbacks = callbacks; }
    NimBLEServerCallbacks* Callbacks() const { return m_callbacks; }
    bool updateConnParams(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {
        ++m_connParamRequests; return true;
    }
    bool updatePhy(uint16_t handle, uint8_t txMask, uint8_t rxMask, uint16_t options) {
        ++m_phyRequests; m_phyHandle = handle; m_phyTx = txMask; m_phyRx = rxMask;
        m_phyOptions = options; m_phyAfterConnParams = m_connParamRequests != 0U;
        return m_phyResult;
    }
    bool getPhy(uint16_t handle, uint8_t* txPhy, uint8_t* rxPhy) {
        ++m_phyReads; m_phyReadHandle = handle;
        if (!m_phyReadResult) return false;
        *txPhy = m_observedTx; *rxPhy = m_observedRx; return true;
    }
    void ResetPhyReadProbe(bool result = false, uint8_t txPhy = 0U, uint8_t rxPhy = 0U) {
        m_phyReads = 0U; m_phyReadResult = result; m_observedTx = txPhy; m_observedRx = rxPhy;
        m_phyReadHandle = 0xffffU;
    }
    uint32_t PhyReadCount() const { return m_phyReads; }
    uint16_t LastPhyReadHandle() const { return m_phyReadHandle; }
    void ResetPhyRequestProbe(bool result = true) {
        m_phyResult = result; m_phyRequests = 0U; m_connParamRequests = 0U;
        m_phyHandle = 0xffffU; m_phyTx = 0U; m_phyRx = 0U; m_phyOptions = 0xffffU;
        m_phyAfterConnParams = false;
    }
    uint32_t PhyRequestCount() const { return m_phyRequests; }
    uint32_t ConnParamRequestCount() const { return m_connParamRequests; }
    uint16_t LastPhyHandle() const { return m_phyHandle; }
    uint8_t LastPhyTxMask() const { return m_phyTx; }
    uint8_t LastPhyRxMask() const { return m_phyRx; }
    uint16_t LastPhyOptions() const { return m_phyOptions; }
    bool PhyAfterConnParams() const { return m_phyAfterConnParams; }
    NimBLEService* createService(const char* uuid) {
        m_services.emplace_back(uuid, std::make_unique<NimBLEService>());
        return m_services.back().second.get();
    }
    NimBLEService* FindService(const std::string& uuid) const {
        for (const auto& service : m_services) if (service.first == uuid) return service.second.get();
        return nullptr;
    }
    void ClearServices() { m_services.clear(); m_callbacks = nullptr; }
    bool disconnect(uint16_t handle) { ++m_disconnectCount; m_lastDisconnect = handle; return m_disconnectAccepted; }
    void SetDisconnectAccepted(bool accepted) { m_disconnectAccepted = accepted; }
    uint32_t DisconnectCount() const { return m_disconnectCount; }
    uint16_t LastDisconnect() const { return m_lastDisconnect; }
    uint16_t getPeerMTU(uint16_t connHandle) const {
        for (const auto& peer : m_peers) {
            if (peer.first == connHandle && connHandle != 0xffffU) return peer.second;
        }
        return 0U;
    }
    std::vector<uint16_t> getPeerDevices() const {
        std::vector<uint16_t> handles;
        for (const auto& peer : m_peers) {
            if (peer.first != 0xffffU) handles.push_back(peer.first);
        }
        return handles;
    }
    uint8_t getConnectedCount() const { return getPeerDevices().size(); }
    NimBLEConnInfo getPeerInfo(uint8_t index) const {
        // Match the pinned SDK's sparse-slot bug: a missing slot resolves the
        // default connection info (handle zero), not the later live handle.
        uint8_t count = 0U;
        for (const auto& peer : m_peers) {
            if (peer.first == 0xffffU) continue;
            if (count == index) {
                const uint16_t handle = m_peers[count].first;
                return NimBLEConnInfo(getPeerMTU(handle == 0xffffU ? 0U : handle));
            }
            ++count;
        }
        return NimBLEConnInfo(getPeerMTU(0U));
    }
    void SetPeerMtu(uint16_t connHandle, uint16_t mtu) {
        m_peers = {{connHandle, mtu}};
    }
    void SetPeerSlots(std::initializer_list<std::pair<uint16_t, uint16_t>> peers) {
        m_peers = peers;
    }

  private:
    bool m_phyResult = true;
    bool m_phyReadResult = false;
    uint32_t m_phyReads = 0U;
    uint16_t m_phyReadHandle = 0xffffU;
    uint8_t m_observedTx = 0U, m_observedRx = 0U;
    bool m_phyAfterConnParams = false;
    uint32_t m_phyRequests = 0U, m_connParamRequests = 0U;
    uint16_t m_phyHandle = 0xffffU, m_phyOptions = 0xffffU;
    uint8_t m_phyTx = 0U, m_phyRx = 0U;
    std::vector<std::pair<uint16_t, uint16_t>> m_peers;
    std::vector<std::pair<std::string, std::unique_ptr<NimBLEService>>> m_services;
    NimBLEServerCallbacks* m_callbacks = nullptr;
    uint32_t m_disconnectCount = 0U;
    uint16_t m_lastDisconnect = 0xffffU;
    bool m_disconnectAccepted = true;
};

int ble_svc_gap_device_name_set(const char* name);
class NimBLEDevice {
  public:
    static bool init(const std::string& name) { s_name = name; s_initialized = true; return true; }
    static bool deinit(bool clear);
    static void FailNextHostStop() { s_failNextHostStop = true; }
    static void SetDeinitHook(void (*hook)()) { s_deinitHook = hook; }
    static bool setMTU(uint16_t mtu) { s_mtu = mtu; return true; }
    static bool setPower(int) { return true; }
    static NimBLEServer* createServer() { return &s_server; }
    static NimBLEAdvertising* getAdvertising();
    static bool startSecurity(uint16_t handle, int* result = nullptr) {
        ++s_securityRequests;
        s_securityHandle = handle;
        if (result != nullptr) *result = s_securityResult;
        return s_securityResult == 0 || s_securityResult == 2;
    }
    static void ResetSecurityRequests() { s_securityRequests = 0U; s_securityResult = 0; }
    static uint32_t SecurityRequests() { return s_securityRequests; }
    static uint16_t SecurityHandle() { return s_securityHandle; }
    static void SetSecurityResult(int result) { s_securityResult = result; }
    static const std::string& Name() { return s_name; }
    static uint16_t getMTU() { return s_mtu; }
    static NimBLEServer* getServer() { return &s_server; }
    // Existing fixtures configure both sides together; transport regressions
    // override the peer independently to exercise pre-negotiation behavior.
    static void SetTestMtu(const uint16_t mtu) {
        s_mtu = mtu;
        s_server.SetPeerMtu(7U, mtu);
    }

  private:
    friend int ble_svc_gap_device_name_set(const char* name);
    inline static uint32_t s_securityRequests = 0U;
    inline static uint16_t s_securityHandle = 0xffffU;
    inline static int s_securityResult = 0;
    inline static void (*s_deinitHook)() = nullptr;
    inline static bool s_initialized = false;
    inline static bool s_failNextHostStop = false;
    inline static std::string s_name;
    inline static uint16_t s_mtu = 23U;
    inline static NimBLEServer s_server;
};

inline int ble_svc_gap_device_name_set(const char* name) {
    NimBLEDevice::s_name = name;
    return 0;
}

class NimBLEAdvertising {
  public:
    NimBLEAdvertising() { m_advData.setFlags(BLE_HS_ADV_F_DISC_GEN); }
    bool start() {
        ++m_startCalls;
        if (!m_startResult) return false;
        m_active = true;
        return true;
    }
    bool stop() { m_active = false; return true; }
    bool addServiceUUID(const char* uuid) {
        const NimBLEUUID serviceUUID{std::string(uuid)};
        return m_advData.addServiceUUID(serviceUUID) || m_scanData.addServiceUUID(serviceUUID);
    }
    bool setName(const std::string& name) {
        return (m_scanResp && m_scanData.setName(name)) || m_advData.setName(name);
    }
    void enableScanResponse(bool enabled) { m_scanResp = enabled; }
    bool setScanResponseData(const NimBLEAdvertisementData& data) { m_scanData = data; return true; }
    const NimBLEAdvertisementData& getAdvertisementData() const { return m_advData; }
    const NimBLEAdvertisementData& ScanData() const { return m_scanData; }
    void clearData() {
        m_advData.clearData(); m_advData.setFlags(BLE_HS_ADV_F_DISC_GEN);
        m_scanData.clearData();
    }
    std::string Name() const {
        for (const auto* data : {&m_scanData, &m_advData}) {
            const auto bytes = data->getPayload();
            for (size_t offset = 0U; offset < bytes.size(); offset += bytes[offset] + 1U) {
                if (bytes[offset + 1U] == BLE_HS_ADV_TYPE_COMP_NAME ||
                    bytes[offset + 1U] == BLE_HS_ADV_TYPE_INCOMP_NAME) {
                    return std::string(bytes.begin() + offset + 2U,
                                       bytes.begin() + offset + bytes[offset] + 1U);
                }
            }
        }
        return {};
    }
    bool isAdvertising() { return m_active; }
    void SetActive(bool active) { m_active = active; }
    void SetStartResult(bool result) { m_startResult = result; }
    uint32_t StartCalls() const { return m_startCalls; }

  private:
    NimBLEAdvertisementData m_advData, m_scanData;
    bool m_scanResp = false;
    bool m_active = false;
    bool m_startResult = true;
    uint32_t m_startCalls = 0U;
};

inline NimBLEAdvertising* NimBLEDevice::getAdvertising() {
    static NimBLEAdvertising advertising;
    return &advertising;
}

inline bool NimBLEDevice::deinit(bool clear) {
    bool stopped = true;
    if (s_initialized) {
        stopped = !s_failNextHostStop;
        s_failNextHostStop = false;
        if (stopped) {
            s_initialized = false;
            if (s_deinitHook != nullptr) s_deinitHook();
        }
    }
    getAdvertising()->stop();
    if (clear) { s_server.ClearServices(); getAdvertising()->clearData(); getAdvertising()->enableScanResponse(false); }
    return stopped;
}
