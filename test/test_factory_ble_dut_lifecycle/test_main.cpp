#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

// Reuse the existing native BLE header, then bind just the central-role SDK
// boundary. The actual shipping task/admission/cancel/client code is below.
#include "NimBLEDevice.h"
#include "freertos/task.h"
#include "foxie_embedded/millis.hpp"
#include "modules/factory_radio_protocol.hpp"
#include "nimble/nimble/include/nimble/nimble_npl.h"

namespace factory_ble_fixture {
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
bool notified = false, stopping = false, blockRead = false, readEntered = false, allowUnwind = true;
std::atomic<bool> connected{false};
std::atomic<unsigned> deletes{0}, allocations{0}, cancels{0};
std::atomic<bool> taskBlocked{false};
std::atomic<uint8_t> connectedAddress{0U};
std::atomic<bool> requestedMtuExchange{true};
bool holdTail = false, tailEntered = false, hostQueueFull = false, hostCallbackActive = false;
bool hostContext = false, deleteFails = false, taskCreateFails = false, queuePutEntered = false;
bool pauseCancel = false, cancelEntered = false;
bool pauseNotify = false, notifyEntered = false;
unsigned taskCreates = 0U, taskDeletes = 0U, unsafeDeletes = 0U;
int lifecycleCore = 0, taskCore = 0;
ble_npl_event* hostEvent = nullptr;
NimBLECharacteristic serverValue;
struct StopTask {};
template <typename Predicate>
void WaitFor(Predicate predicate) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(2), predicate)) {
        std::puts("[FAIL] factory BLE fixture synchronization timeout");
        std::abort();
    }
}
}

struct FactoryTestNimBLEAddress {
    struct Base { uint8_t val[6]{10U,0U,0U,0U,0U,0U}; } bytes;
    const Base* getBase() const { return &bytes; }
};
struct FactoryTestNimBLEUUID { explicit FactoryTestNimBLEUUID(const char*) {} };
struct FactoryTestNimBLEAdvertisedDevice {
    uint8_t address;
    bool factoryService;
    bool connectable;

    bool isConnectable() const { return connectable; }
    bool isAdvertisingService(const FactoryTestNimBLEUUID&) const { return factoryService; }
    FactoryTestNimBLEAddress getAddress() const {
        FactoryTestNimBLEAddress value;
        value.bytes.val[0] = address;
        return value;
    }
};
struct FactoryTestNimBLEScanResults {
    std::vector<FactoryTestNimBLEAdvertisedDevice> devices;
    int getCount() const { return static_cast<int>(devices.size()); }
    const FactoryTestNimBLEAdvertisedDevice* getDevice(uint32_t index) const { return &devices[index]; }
};
struct FactoryTestNimBLEScanCallbacks {
    virtual ~FactoryTestNimBLEScanCallbacks() = default;
    virtual void onResult(const FactoryTestNimBLEAdvertisedDevice*) {}
};
struct FactoryTestNimBLEScan {
    void setActiveScan(bool active) { activeScan = active; }
    void setMaxResults(uint8_t maximum) { maxResults = maximum; }
    void setScanCallbacks(FactoryTestNimBLEScanCallbacks* owner) { callbacks = owner; }
    bool isScanning() const { return false; }
    FactoryTestNimBLEScanResults getResults(uint32_t, bool) {
        FactoryTestNimBLEScanResults results;
        if (rejectStart) return results;
        for (const auto& advertised : advertisements) {
            // NimBLE admits distinct advertisers to the cache before invoking
            // discovery callbacks. A passive zero-result scan erases each
            // entry immediately after onResult, prior to the next report.
            if (maxResults != 0U && results.devices.size() >= maxResults) continue;
            results.devices.push_back(advertised);
            if (results.devices.size() > peakResults) peakResults = results.devices.size();
            if (!activeScan) {
                if (callbacks != nullptr) callbacks->onResult(&results.devices.back());
                if (maxResults == 0U) results.devices.pop_back();
            }
        }
        return results;
    }
    void clearResults() {}

    std::vector<FactoryTestNimBLEAdvertisedDevice> advertisements{{5U, true, true}};
    FactoryTestNimBLEScanCallbacks* callbacks = nullptr;
    size_t peakResults = 0U;
    bool rejectStart = false;
    bool activeScan = false;
    uint8_t maxResults = 255U;
};
struct FactoryTestNimBLEAttValue {
    modules::FactoryRadioWireFrame bytes{};
    const uint8_t* data() const { return bytes.data(); }
    size_t size() const { return bytes.size(); }
};
struct FactoryTestNimBLERemoteCharacteristic {
    modules::FactoryRadioVerifierHandshake verifier;
    FactoryTestNimBLEAttValue value;
    bool writeValue(const uint8_t* data, size_t length, bool) {
        modules::FactoryRadioFrame input, reply;
        verifier.SetEligible(true);
        return modules::DecodeFactoryRadioFrame(data, length, input) &&
            verifier.Handle(input, 99U, reply) && modules::EncodeFactoryRadioFrame(reply, value.bytes);
    }
    FactoryTestNimBLEAttValue readValue() {
        using namespace factory_ble_fixture;
        std::unique_lock<std::mutex> lock(mutex);
        if (blockRead) {
            readEntered = true;
            changed.notify_all();
            changed.wait(lock, [] { return !connected.load() && allowUnwind; });
            return {}; // canceled GATT completion
        }
        return value;
    }
};
struct FactoryTestNimBLERemoteService {
    FactoryTestNimBLERemoteCharacteristic characteristic;
    FactoryTestNimBLERemoteCharacteristic* getCharacteristic(const char*) { return &characteristic; }
};
class FactoryTestNimBLEClient {
  public:
    void setConnectTimeout(uint32_t) {}
    void setSelfDelete(bool, bool) {}
    bool connect(const FactoryTestNimBLEAdvertisedDevice* peer) { return connect(peer->getAddress()); }
    bool connect(const FactoryTestNimBLEAddress& address, bool = true, bool = false, bool exchangeMtu = true) {
        used = true;
        factory_ble_fixture::connectedAddress.store(address.bytes.val[0]);
        factory_ble_fixture::requestedMtuExchange.store(exchangeMtu);
        factory_ble_fixture::connected.store(true);
        service.characteristic.verifier = {};
        return true;
    }
    bool isConnected() const { return factory_ble_fixture::connected.load(); }
    bool cancelConnect() {
        using namespace factory_ble_fixture;
        ++cancels;
        std::unique_lock<std::mutex> lock(mutex);
        if (pauseCancel && !cancelEntered) {
            cancelEntered = true; changed.notify_all();
            changed.wait(lock, [] { return !pauseCancel; });
        }
        return true;
    }
    bool disconnect() {
        factory_ble_fixture::connected.store(false);
        factory_ble_fixture::changed.notify_all();
        return true;
    }
    FactoryTestNimBLERemoteService* getService(const char*) { return &service; }
    bool used = false;
  private:
    FactoryTestNimBLERemoteService service;
};
struct FactoryTestNimBLEDevice {
    static FactoryTestNimBLEAddress getAddress() { return {}; }
    static FactoryTestNimBLEScan* getScan() { static FactoryTestNimBLEScan scan; return &scan; }
    static FactoryTestNimBLEClient* createClient() { ++factory_ble_fixture::allocations; return new FactoryTestNimBLEClient; }
    static bool deleteClient(FactoryTestNimBLEClient* client) {
        using namespace factory_ble_fixture;
        if (client->used && (!hostContext || hostCallbackActive)) { ++unsafeDeletes; return false; }
        if (deleteFails) return false;
        ++deletes; delete client; return true;
    }
};

struct FactoryTestNimBLEService : NimBLEService { void start() {} };
struct FactoryTestNimBLEServer : NimBLEServer {
    FactoryTestNimBLEService service;
    unsigned disconnects = 0U;
    bool disconnect(uint16_t) { ++disconnects; return true; }
    FactoryTestNimBLEService* createService(const char*) { return &service; }
};
struct FactoryTestNimBLEAdvertising : NimBLEAdvertising {
    void addServiceUUID(const char*) {} void removeServiceUUID(const char*) {}
};
FactoryTestNimBLEServer ordinaryServer;
FactoryTestNimBLEAdvertising ordinaryAdvertising;

TaskHandle_t FactoryTestTaskCreate(TaskFunction_t task, const char*, uint32_t stackBytes, void* context,
                                  uint32_t, StackType_t* stack, StaticTask_t* control, int core) {
    if (factory_ble_fixture::taskCreateFails || stackBytes != 6144U || stack == nullptr || control == nullptr)
        return nullptr;
    ++factory_ble_fixture::taskCreates;
    factory_ble_fixture::taskCore = core;
    factory_ble_fixture::stopping = false;
    factory_ble_fixture::worker = std::thread([=] {
        try { task(context); } catch (const factory_ble_fixture::StopTask&) {}
    });
    return reinterpret_cast<TaskHandle_t>(1U);
}
uint32_t FactoryTestNotifyTake(bool, uint32_t) {
    using namespace factory_ble_fixture;
    std::unique_lock<std::mutex> lock(mutex);
    static thread_local bool first = true;
    if (!first && holdTail) {
        tailEntered = true; changed.notify_all();
        changed.wait(lock, [] { return !holdTail; });
    }
    first = false;
    taskBlocked.store(true);
    changed.notify_all();
    changed.wait(lock, [] { return notified || stopping; });
    if (stopping) { throw StopTask{}; }
    notified = false;
    taskBlocked.store(false);
    return 1U;
}

constexpr unsigned FACTORY_TEST_BLOCKED = 1U;
unsigned FactoryTestTaskState(TaskHandle_t) { return factory_ble_fixture::taskBlocked ? FACTORY_TEST_BLOCKED : 0U; }
int FactoryTestTaskAffinity(TaskHandle_t) { return factory_ble_fixture::taskCore; }
int FactoryTestCore() { return factory_ble_fixture::lifecycleCore; }
void FactoryTestTaskDelete(TaskHandle_t) {
    using namespace factory_ble_fixture;
    { std::lock_guard<std::mutex> lock(mutex); stopping = true; blockRead = false; allowUnwind = true; connected.store(false); changed.notify_all(); }
    worker.join(); ++taskDeletes; taskBlocked.store(false);
}
ble_npl_eventq* nimble_port_get_dflt_eventq() { static ble_npl_eventq queue; return &queue; }
void ble_npl_eventq_put(ble_npl_eventq*, ble_npl_event* event) {
    using namespace factory_ble_fixture;
    std::unique_lock<std::mutex> lock(mutex);
    queuePutEntered = true; changed.notify_all();
    changed.wait(lock, [] { return !hostQueueFull; });
    event->queued = true; hostEvent = event; changed.notify_all();
}
void DrainHostEvent() {
    using namespace factory_ble_fixture;
    ble_npl_event* event;
    { std::lock_guard<std::mutex> lock(mutex); event = hostEvent; hostEvent = nullptr; }
    if (event == nullptr) return;
    event->queued = false; hostContext = true; event->fn(event); hostContext = false;
}
void FactoryTestNotifyGive(TaskHandle_t) {
    using namespace factory_ble_fixture;
    std::unique_lock<std::mutex> lock(mutex);
    notified = true; changed.notify_all();
    if (pauseNotify) {
        notifyEntered = true; changed.notify_all();
        changed.wait(lock, [] { return !pauseNotify; });
    }
}

#define LITCH_BLE 1
#define LITCH_HW_VARIANT_S3 1
#define BleService FactoryTestBleService
#define NimBLEClient FactoryTestNimBLEClient
#define NimBLEAddress FactoryTestNimBLEAddress
#define NimBLEUUID FactoryTestNimBLEUUID
#define NimBLEScan FactoryTestNimBLEScan
#define NimBLEScanCallbacks FactoryTestNimBLEScanCallbacks
#define NimBLEScanResults FactoryTestNimBLEScanResults
#define NimBLEAdvertisedDevice FactoryTestNimBLEAdvertisedDevice
#define NimBLERemoteCharacteristic FactoryTestNimBLERemoteCharacteristic
#define NimBLEAttValue FactoryTestNimBLEAttValue
#define NimBLEDevice FactoryTestNimBLEDevice
#define NimBLEService FactoryTestNimBLEService
#define NimBLEServer FactoryTestNimBLEServer
#define NimBLEAdvertising FactoryTestNimBLEAdvertising
#define xTaskCreateStaticPinnedToCore FactoryTestTaskCreate
#define eTaskGetState FactoryTestTaskState
#define eBlocked FACTORY_TEST_BLOCKED
#define xTaskGetAffinity FactoryTestTaskAffinity
#define xPortGetCoreID FactoryTestCore
#define vTaskDelete FactoryTestTaskDelete
#define ulTaskNotifyTake FactoryTestNotifyTake
#define xTaskNotifyGive FactoryTestNotifyGive
#define pdTRUE true
#define portMAX_DELAY UINT32_MAX
#include "../../src/ble_service_factory_dut.cpp"
#include "../../src/ble_service_factory_radio.cpp"
#undef BleService

// Only normal-stack initialization/consumer presence are simulated. Factory
// operation internals, timeouts, reuse, result fencing and cleanup are shipping code.
void modules::FactoryTestBleService::Begin(const SettingsStore&) {
    m_initialized = true; m_enabled = true; m_factoryVerifierSupported = true;
    m_server = &ordinaryServer; m_advertising = &ordinaryAdvertising;
    m_factoryRadioCharacteristic = &factory_ble_fixture::serverValue;
    m_factoryVerifierEndpointEnabled = true;
    m_factoryVerifierConfiguredEnabled.store(true);
    m_factoryVerifierRuntimeAllowed.store(true);
    m_factoryVerifierHandshake.SetEligible(true);
}
bool modules::FactoryTestBleService::IsConnected() const { return false; }
void modules::FactoryTestBleService::StartAdvertising() { ordinaryAdvertising.SetActive(true); }
bool modules::FactoryTestBleService::StopAdvertising() { ordinaryAdvertising.SetActive(false); return true; }

int main() {
    using namespace factory_ble_fixture;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); } };
    fxe::ScopedMillisProviderOverride time(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    firmware::SettingsFactoryTaskStorage workerStorage;
    modules::FactoryTestBleService service;
    service.SetFactoryDutTaskStorage(workerStorage);
    modules::SettingsStore settings;
    service.Begin(settings);
    ordinaryServer.SetPeerMtu(9U, 185U);
    modules::FactoryBleProof proof;
    const auto finish = [&](uint32_t session, uint32_t nonce) {
        auto result = service.PollFactoryBleDut(session, nonce, false, proof);
        for (unsigned phase = 0U; result == modules::FactoryBleDutResult::Pending && phase < 6U; ++phase) {
            WaitFor([] { return hostEvent != nullptr && taskBlocked.load(); });
            DrainHostEvent();
            result = service.PollFactoryBleDut(session, nonce, false, proof);
        }
        return result;
    };
    if (std::getenv("LITCH_FACTORY_LIFECYCLE_PROOF_PROBE") != nullptr) {
        check(service.PollFactoryBleDut(80U, 81U, false, proof) == modules::FactoryBleDutResult::Pending,
              "production DUT API admits one bounded proof worker");
        WaitFor([&] { return taskBlocked.load() && !service.IsFactoryBleDutRunning(); });
        auto result = service.PollFactoryBleDut(80U, 81U, false, proof);
        check(result == modules::FactoryBleDutResult::Pending && deletes == 0U && taskDeletes == 0U,
              "completed BLE proof must remain pending while dedicated client/task allocations remain owned");
        if (result == modules::FactoryBleDutResult::Pending) result = finish(80U, 81U);
        check(result == modules::FactoryBleDutResult::Verified && allocations == deletes && taskCreates == taskDeletes,
              "BLE proof publication requires a released dedicated worker/client footprint");
        if (worker.joinable()) FactoryTestTaskDelete(reinterpret_cast<TaskHandle_t>(1U));
        std::printf("[FACTORY_BLE_PROOF_RELEASE] failures=%d clients=%u/%u tasks=%u/%u; actual SDK heap/RF not simulated\n",
                    failures, allocations.load(), deletes.load(), taskCreates, taskDeletes);
        return failures == 0 ? 0 : 1;
    }
    blockRead = true; allowUnwind = false;
    check(service.PollFactoryBleDut(1U, 2U, false, proof) == modules::FactoryBleDutResult::Pending,
          "GATT starts asynchronously without blocking local inputs");
    WaitFor([] { return readEntered; });
    check(service.HasFactoryBleCandidate(), "eligible discovery is visible separately from proof");
    fxe::ManualMillis::SetNowMs(5000U);
    pauseCancel = true;
    std::thread timeoutCaller([&] { service.PollFactoryBleDut(1U, 2U, false, proof); });
    WaitFor([] { return cancelEntered; });
    { std::lock_guard<std::mutex> lock(mutex); connected.store(false); allowUnwind = true; changed.notify_all(); }
    WaitFor([&] { return taskBlocked.load() && !service.IsFactoryBleDutRunning(); });
    check(cancels > 0U && !service.QuiesceFactoryBleDut() && deletes == 0U && taskDeletes == 0U,
          "preempted five-second cancellation owns its old-state client across worker completion and forced exit");
    { std::lock_guard<std::mutex> lock(mutex); pauseCancel = false; changed.notify_all(); }
    timeoutCaller.join();
    const auto late = service.PollFactoryBleDut(3U, 4U, false, proof);
    check(late == modules::FactoryBleDutResult::Pending && deletes == 0U,
          "canceled completion waits for serialized host cleanup instead of foreground deletion");
    if (late != modules::FactoryBleDutResult::Pending || unsafeDeletes != 0U) {
        FactoryTestTaskDelete(reinterpret_cast<TaskHandle_t>(1U));
        std::printf("[FACTORY_BLE_LIFECYCLE] old premature release reproduced failures=%d unsafe_deletes=%u\n", failures, unsafeDeletes);
        return 1;
    }
    check(finish(3U, 4U) == modules::FactoryBleDutResult::NoPeer && deletes == 1U && taskDeletes == 1U,
          "late proof cannot cross replacement nonce and quiescent SDK client/task are released once");

    blockRead = false; holdTail = true; tailEntered = false;
    fxe::ManualMillis::SetNowMs(11000U);
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending,
          "fresh session recreates only one dedicated client/task");
    WaitFor([] { return tailEntered; });
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending && hostEvent == nullptr,
          "completed result cannot release a worker still returning to its idle wait");
    { std::lock_guard<std::mutex> lock(mutex); holdTail = false; changed.notify_all(); }
    WaitFor([] { return taskBlocked.load(); });
    lifecycleCore = 1;
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending && hostEvent == nullptr,
          "foreign-core caller cannot claim synchronous task-stack reclamation");
    lifecycleCore = 0;
    hostQueueFull = true; queuePutEntered = false;
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending,
          "ordinary lifecycle core submits host cleanup through its worker");
    WaitFor([] { return queuePutEntered; });
    for (unsigned turn = 0U; turn < 20U; ++turn)
        check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending,
              "full SDK host queue blocks only the worker and keeps foreground admission bounded");
    check(deletes == 1U && taskDeletes == 1U, "queue pressure cannot publish proof or release an owned task");
    { std::lock_guard<std::mutex> lock(mutex); hostQueueFull = false; changed.notify_all(); }
    WaitFor([] { return hostEvent != nullptr && taskBlocked.load(); });
    hostCallbackActive = true;
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending && deletes == 1U,
          "disconnect callback tail owns the client until its host queue event can run");
    hostCallbackActive = false; deleteFails = true;
    DrainHostEvent();
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending && deletes == 1U,
          "SDK client deletion refusal retains proof/resources for a bounded next-turn retry");
    deleteFails = false;
    pauseNotify = true; notifyEntered = false;
    std::thread cleanupCaller([&] { service.PollFactoryBleDut(3U, 4U, false, proof); });
    WaitFor([] { return notifyEntered && hostEvent != nullptr && taskBlocked.load(); });
    DrainHostEvent();
    check(deletes == 1U && taskDeletes == 1U,
          "host cleanup encountering an in-flight foreground client owner retries without waiting or deletion");
    { std::lock_guard<std::mutex> lock(mutex); pauseNotify = false; changed.notify_all(); }
    cleanupCaller.join();
    check(service.PollFactoryBleDut(3U, 4U, false, proof) == modules::FactoryBleDutResult::Pending,
          "host-busy receipt returns ownership for a bounded next-turn retry");
    check(finish(3U, 4U) == modules::FactoryBleDutResult::Verified &&
          proof.sessionId == 3U && proof.dutNonce == 4U && proof.verifierNonce == 99U &&
          deletes == 2U && taskDeletes == 2U,
          "fresh nonce proof is published only after host client deletion and actual blocked worker release");
    check(service.QuiesceFactoryBleDut() && service.QuiesceFactoryBleDut() && taskDeletes == 2U,
          "repeated stopped cleanup cannot double-delete released resources");

    taskCreateFails = true;
    check(service.PollFactoryBleDut(7U, 8U, false, proof) == modules::FactoryBleDutResult::NoPeer &&
          allocations == deletes && taskCreates == taskDeletes,
          "task-create failure releases its never-used client without callback or stack ownership");
    taskCreateFails = false;
    taskCreateFails = true; deleteFails = true;
    check(service.PollFactoryBleDut(9U, 10U, false, proof) == modules::FactoryBleDutResult::NoPeer &&
          !service.QuiesceFactoryBleDut() && allocations == deletes + 1U,
          "failed task creation plus SDK deletion refusal retains the never-used client for recovery");
    deleteFails = false;
    check(service.QuiesceFactoryBleDut() && allocations == deletes,
          "later SDK cleanup recovers task-create failure without leaking client resources");
    taskCreateFails = false;

    // Production server ingress queues a complete terminal proof while a
    // completed discovery job still owns resources. That retained slot wins.
    check(service.PollFactoryBleDut(20U, 21U, false, proof) == modules::FactoryBleDutResult::Pending,
          "new discovery job is admitted after prior release");
    WaitFor([&] { return taskBlocked.load() && !service.IsFactoryBleDutRunning(); });
    service.SetFactoryVerifierRuntimeAllowed(true);
    modules::FactoryRadioDutHandshake peer;
    modules::FactoryRadioFrame hello, challenge, response, terminal, unused;
    modules::FactoryRadioWireFrame wire;
    check(peer.Start(modules::FactoryRadioChannel::Ble, 30U, 31U, hello), "server peer begins exact v3 BLE proof");
    modules::EncodeFactoryRadioFrame(hello, wire); service.OnFactoryRadioCharacteristicWrite(wire.data(), wire.size());
    auto value = serverValue.getValue();
    check(modules::DecodeFactoryRadioFrame(reinterpret_cast<const uint8_t*>(value.data()), value.size(), challenge) &&
          peer.Handle(challenge, response), "actual server returns nonce challenge");
    modules::EncodeFactoryRadioFrame(response, wire); service.OnFactoryRadioCharacteristicWrite(wire.data(), wire.size());
    value = serverValue.getValue();
    check(modules::DecodeFactoryRadioFrame(reinterpret_cast<const uint8_t*>(value.data()), value.size(), terminal) &&
          peer.Handle(terminal, unused) && peer.IsVerified(), "actual terminal GATT value completes bidirectional server proof");
    // Model NimBLE's real write-before-callback ordering, not just the owner API.
    serverValue.setValue(wire.data(), wire.size());
    service.OnFactoryRadioCharacteristicWrite(wire.data(), wire.size());
    value = serverValue.getValue();
    check(modules::DecodeFactoryRadioFrame(reinterpret_cast<const uint8_t*>(value.data()), value.size(), terminal) &&
          terminal.type == modules::FactoryRadioFrameType::Verified,
          "duplicate Response restores terminal READ while exact server proof is queued");
    check(service.PollFactoryBleDut(20U, 21U, false, proof) == modules::FactoryBleDutResult::Pending,
          "completed DUT result waits while server slot and cleanup own the lane");
    check(!service.ConsumeFactoryBleVerifierProof(proof), "server slot remains queued until dedicated resources release");
    WaitFor([] { return hostEvent != nullptr && taskBlocked.load(); });
    DrainHostEvent();
    check(service.PollFactoryBleDut(20U, 21U, false, proof) == modules::FactoryBleDutResult::NoPeer,
          "pending server proof prevents competing completed DUT proof publication");
    const auto beforePending = allocations.load();
    check(service.PollFactoryBleDut(50U, 51U, false, proof) == modules::FactoryBleDutResult::NoPeer && allocations == beforePending,
          "pending server proof admits no new discovery client/task allocation");
    check(service.ConsumeFactoryBleVerifierProof(proof) && proof.sessionId == 30U && proof.dutNonce == 31U &&
          proof.verifierNonce == challenge.senderNonce && !service.ConsumeFactoryBleVerifierProof(proof),
          "queued exact server proof is consumed once after release and retains the BLE-established nonce");

    service.SetFactoryVerifierRuntimeAllowed(false);
    serverValue.setValue(wire.data(), wire.size());
    service.OnFactoryRadioCharacteristicWrite(wire.data(), wire.size());
    value = serverValue.getValue();
    check(modules::DecodeFactoryRadioFrame(reinterpret_cast<const uint8_t*>(value.data()), value.size(), terminal) &&
          terminal.type == modules::FactoryRadioFrameType::Verified &&
          !service.ConsumeFactoryBleVerifierProof(proof),
          "terminal grace restores exact retry after consumption without publishing a second proof");
    auto foreignResponse = response;
    ++foreignResponse.sessionId;
    modules::FactoryRadioWireFrame foreignWire;
    modules::EncodeFactoryRadioFrame(foreignResponse, foreignWire);
    serverValue.setValue(foreignWire.data(), foreignWire.size());
    service.OnFactoryRadioCharacteristicWrite(foreignWire.data(), foreignWire.size());
    value = serverValue.getValue();
    check(modules::DecodeFactoryRadioFrame(reinterpret_cast<const uint8_t*>(value.data()), value.size(), terminal) &&
          terminal.type != modules::FactoryRadioFrameType::Verified &&
          !service.ConsumeFactoryBleVerifierProof(proof),
          "terminal grace rejects a foreign session and cannot manufacture proof");

    check(service.PollFactoryBleDut(60U, 61U, false, proof) == modules::FactoryBleDutResult::Pending,
          "later batch discovery can recreate its released lane");
    WaitFor([&] { return taskBlocked.load() && !service.IsFactoryBleDutRunning(); });
    service.SetFactoryVerifierRuntimeAllowed(true);
    modules::FactoryRadioDutHandshake stalePeer;
    check(stalePeer.Start(modules::FactoryRadioChannel::Ble, 70U, 71U, hello), "stale-generation peer starts proof");
    modules::EncodeFactoryRadioFrame(hello, wire); service.OnFactoryRadioCharacteristicWrite(wire.data(), wire.size());
    value = serverValue.getValue();
    check(modules::DecodeFactoryRadioFrame(reinterpret_cast<const uint8_t*>(value.data()), value.size(), challenge) &&
          stalePeer.Handle(challenge, response), "later server session receives current challenge");
    modules::EncodeFactoryRadioFrame(response, wire); service.OnFactoryRadioCharacteristicWrite(wire.data(), wire.size());
    check(!service.ConsumeFactoryBleVerifierProof(proof), "second server slot waits for cleanup");
    WaitFor([] { return hostEvent != nullptr && taskBlocked.load(); });
    service.SetFactoryVerifierConfiguredEnabled(false);
    check(!service.ConsumeFactoryBleVerifierProof(proof) && !service.QuiesceFactoryBleDut(),
          "stop/generation retirement discards queued proof but cannot free pending host cleanup");
    DrainHostEvent();
    check(service.QuiesceFactoryBleDut() && !service.ConsumeFactoryBleVerifierProof(proof) &&
          service.QuiesceFactoryBleDut(),
          "late cleanup after stopped generation releases once without stale server proof");
    service.SetFactoryVerifierConfiguredEnabled(true);
    check(service.IsFactoryVerifierConfiguredEnabled() && ordinaryServer.getConnectedCount() == 1U &&
          ordinaryServer.disconnects == 0U && unsafeDeletes == 0U &&
          allocations == deletes && taskCreates == taskDeletes,
          "factory cleanup preserves ordinary server and releases all dedicated resources without unsafe deletion");

    auto& scan = *FactoryTestNimBLEDevice::getScan();
    scan.advertisements.clear();
    for (uint8_t address = 20U; address < 29U; ++address) {
        scan.advertisements.push_back({address, false, true});
    }
    // Reject an unconnectable factory advertisement, self, and a higher
    // verifier before choosing the lower of two eligible factory peers.
    constexpr FactoryTestNimBLEAdvertisedDevice FACTORY_ADVERTISEMENTS[]{
        {1U, true, false}, {10U, true, true}, {12U, true, true}, {7U, true, true}, {6U, true, true},
    };
    for (const auto& advertised : FACTORY_ADVERTISEMENTS) {
        scan.advertisements.push_back(advertised);
    }
    scan.peakResults = 0U;
    connectedAddress.store(0U);
    requestedMtuExchange.store(true);
    check(service.PollFactoryBleDut(80U, 81U, true, proof) == modules::FactoryBleDutResult::Pending,
          "a locally ready verifier starts one discovery in a crowded RF environment");
    WaitFor([&] { return taskBlocked.load() && !service.IsFactoryBleDutRunning(); });
    const auto crowded = finish(80U, 81U);
    check(crowded == modules::FactoryBleDutResult::Verified && connectedAddress == 6U &&
          proof.sessionId == 80U && proof.dutNonce == 81U && proof.verifierNonce == 99U,
          "nine unrelated advertisers cannot hide the lowest eligible factory peer or prevent proof");
    check(!requestedMtuExchange && scan.peakResults == 1U && scan.callbacks == nullptr &&
          allocations == deletes && taskCreates == taskDeletes,
          "passive scan releases every SDK result, detaches its callback and proves on the default MTU");
    std::printf("[FACTORY_BLE_CROWDED_SCAN] unrelated=9 result=%u peer=%u sdk_peak_results=%zu mtu_exchange=%u\n",
                static_cast<unsigned>(crowded), unsigned(connectedAddress.load()), scan.peakResults,
                unsigned(requestedMtuExchange.load()));

    scan.rejectStart = true;
    connectedAddress.store(0U);
    check(service.PollFactoryBleDut(90U, 91U, false, proof) == modules::FactoryBleDutResult::Pending,
          "a new worker owns a scan start failure");
    WaitFor([&] { return taskBlocked.load() && !service.IsFactoryBleDutRunning(); });
    check(finish(90U, 91U) == modules::FactoryBleDutResult::NoPeer && scan.callbacks == nullptr &&
          connectedAddress == 0U && allocations == deletes && taskCreates == taskDeletes,
          "failed scan start detaches callbacks and retires resources without stale proof");

    std::printf("[FACTORY_BLE_LIFECYCLE] API ownership/host-tail/queue-pressure/recreation/server-slot failures=%d clients=%u/%u tasks=%u/%u\n",
                failures, allocations.load(), deletes.load(), taskCreates, taskDeletes);
    return failures == 0 ? 0 : 1;
}
