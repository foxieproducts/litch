#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

// Compile the actual Arduino queue owner, not its separate native array path.
#define ARDUINO 1
#define modules arduino_ble_queue_contract
#include "../../src/ble_event_queue.cpp"
#undef modules
#undef ARDUINO

namespace {
unsigned g_dynamicQueueCreates = 0U;
unsigned g_staticQueueCreates = 0U;
constexpr size_t MAX_QUEUE_BYTES = arduino_ble_queue_contract::BleEventQueue::CAPACITY *
                                 sizeof(arduino_ble_queue_contract::BleEvent);
std::array<StaticQueue_t, 8U> g_dynamicControls{};
std::array<std::array<uint8_t, MAX_QUEUE_BYTES>, 8U> g_dynamicBytes{};

QueueHandle_t ConfigureQueue(UBaseType_t length, UBaseType_t itemSize,
                            uint8_t* storage, StaticQueue_t* control) {
    if (length == 0U || itemSize == 0U || storage == nullptr || control == nullptr) return nullptr;
    control->storage = storage;
    control->capacity = length;
    control->itemSize = itemSize;
    control->head = control->count = 0U;
    return control;
}
} // namespace

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
    const auto index = g_dynamicQueueCreates++;
    if (index >= g_dynamicControls.size() || length * itemSize > MAX_QUEUE_BYTES) return nullptr;
    return ConfigureQueue(length, itemSize, g_dynamicBytes[index].data(), &g_dynamicControls[index]);
}
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t itemSize,
                               uint8_t* storage, StaticQueue_t* control) {
    ++g_staticQueueCreates;
    return ConfigureQueue(length, itemSize, storage, control);
}
BaseType_t xQueueSend(QueueHandle_t handle, const void* item, TickType_t wait) {
    if (handle == nullptr || wait != 0U) return pdFALSE;
    auto& queue = *static_cast<StaticQueue_t*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.count == queue.capacity) return pdFALSE;
    const auto tail = (queue.head + queue.count) % queue.capacity;
    std::memcpy(queue.storage + tail * queue.itemSize, item, queue.itemSize);
    ++queue.count;
    return pdTRUE;
}
BaseType_t xQueueSendToFront(QueueHandle_t handle, const void* item, TickType_t wait) {
    if (handle == nullptr || wait != 0U) return pdFALSE;
    auto& queue = *static_cast<StaticQueue_t*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.count == queue.capacity) return pdFALSE;
    queue.head = (queue.head + queue.capacity - 1U) % queue.capacity;
    std::memcpy(queue.storage + queue.head * queue.itemSize, item, queue.itemSize);
    ++queue.count;
    return pdTRUE;
}
BaseType_t xQueueOverwrite(QueueHandle_t handle, const void* item) {
    if (handle == nullptr) return pdFALSE;
    auto& queue = *static_cast<StaticQueue_t*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.capacity != 1U) return pdFALSE;
    std::memcpy(queue.storage, item, queue.itemSize);
    queue.head = 0U; queue.count = 1U;
    return pdTRUE;
}
BaseType_t xQueuePeek(QueueHandle_t handle, void* item, TickType_t wait) {
    if (handle == nullptr || wait != 0U) return pdFALSE;
    auto& queue = *static_cast<StaticQueue_t*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.count == 0U) return pdFALSE;
    std::memcpy(item, queue.storage + queue.head * queue.itemSize, queue.itemSize);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void* item, TickType_t wait) {
    if (handle == nullptr || wait != 0U) return pdFALSE;
    auto& queue = *static_cast<StaticQueue_t*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.count == 0U) return pdFALSE;
    std::memcpy(item, queue.storage + queue.head * queue.itemSize, queue.itemSize);
    queue.head = (queue.head + 1U) % queue.capacity;
    --queue.count;
    return pdTRUE;
}

int TestArduinoBleConnectionProjectionPeek() {
    using namespace arduino_ble_queue_contract;
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::printf("[FAIL] %s\n", message); }
    };
    const unsigned priorDynamic = g_dynamicQueueCreates;
    const unsigned priorStatic = g_staticQueueCreates;
    BleConnectionProjection projection, otherProjection;
    BleConnectionChangeEvent observed, connected, disconnected;
    connected.connected = true;
    connected.connectionEpoch = 11U;
    disconnected.reason = 19;
    disconnected.connectionEpoch = 12U;
    check(!projection.TryPeek(observed) && projection.Begin() && !projection.TryPeek(observed),
          "Arduino projection starts empty");
    check(projection.Publish(connected) && projection.Begin() &&
              projection.TryPeek(observed) && observed.connected && observed.connectionEpoch == 11U &&
              projection.TryPeek(observed) && projection.TryTake(observed) && observed.connectionEpoch == 11U &&
              !projection.TryTake(observed),
          "Arduino repeated Begin and peek preserve the callback for one take");
    check(projection.Publish(connected) && projection.Publish(disconnected) &&
              projection.TryPeek(observed) && !observed.connected && observed.connectionEpoch == 12U &&
              observed.reason == 19 && otherProjection.Begin() && otherProjection.Publish(connected) &&
              otherProjection.TryTake(observed) && observed.connectionEpoch == 11U &&
              projection.TryTake(observed) && observed.connectionEpoch == 12U && !projection.TryPeek(observed),
          "independent Arduino mailboxes retain their own latest connection epoch");

    BleEventQueue queue, otherQueue;
    BleEvent input, output;
    input.type = BleEventType::ConsoleInput;
    input.payload.consoleInput.len = 1U;
    check(!queue.Enqueue(input) && queue.Begin() && otherQueue.Begin(),
          "ordinary Arduino ingress requires admission");
    input.payload.consoleInput.input[0] = 'z';
    check(otherQueue.Enqueue(input), "second Arduino queue owns its own item");
    for (unsigned i = 0U; i < BleEventQueue::CAPACITY; ++i) {
        input.payload.consoleInput.input[0] = static_cast<char>(i);
        check(queue.Enqueue(input), "Arduino ingress retains the complete negotiated window");
    }
    check(!queue.Enqueue(input) && !queue.EnqueueNext(input) && queue.Begin(), "Arduino full queue rejects overflow without clearing it");
    BleEvent priority;
    priority.type = BleEventType::SettingWrite;
    priority.payload.setting.connectionEpoch = 23U;
    check(queue.Enqueue(priority) && queue.TryPeek(output) && output.type == BleEventType::SettingWrite &&
              queue.TryDequeueIfOrdinal(output.ingressOrdinal, output) && output.payload.setting.connectionEpoch == 23U,
          "reserved priority still works while ordinary Arduino ingress is full");
    for (unsigned i = 0U; i < BleEventQueue::CAPACITY; ++i) {
        check(queue.TryPeek(output) && output.type == BleEventType::ConsoleInput &&
                  output.payload.consoleInput.input[0] == static_cast<char>(i) &&
                  queue.TryDequeueIfOrdinal(output.ingressOrdinal, output),
              "Arduino ordinary FIFO retains order through repeated Begin and priority dequeue");
    }
    check(!queue.TryDequeue(output) && otherQueue.TryDequeue(output) &&
              output.payload.consoleInput.input[0] == 'z', "Arduino queue storage is independent per owner");
    input.ingressOrdinal = 17U;
    input.payload.consoleInput.input[0] = 'b';
    check(queue.Enqueue(input), "Arduino later envelope enters ordinary FIFO");
    input.ingressOrdinal = 16U;
    input.payload.consoleInput.input[0] = 'a';
    check(queue.EnqueueNext(input) && queue.TryPeekOrdinary(output) &&
              output.ingressOrdinal == 16U &&
              queue.TryDequeueOrdinaryIfOrdinal(16U, output) &&
              output.payload.consoleInput.input[0] == 'a' &&
              queue.TryDequeue(output) && output.ingressOrdinal == 17U &&
              output.payload.consoleInput.input[0] == 'b',
          "actual Arduino front requeue preserves causal head and exact later bytes");
    const unsigned dynamicCreates = g_dynamicQueueCreates - priorDynamic;
    const unsigned staticCreates = g_staticQueueCreates - priorStatic;
    check(dynamicCreates == 0U && staticCreates == 4U,
          "two Arduino ingress queues and two connection mailboxes use only owner-supplied static storage");
    std::printf("[BLE_QUEUE_STATIC] arduino_path=1 dynamic=%u static=%u capacity=%u failures=%d\n",
                dynamicCreates, staticCreates, BleEventQueue::CAPACITY, failures);
    return failures;
}
