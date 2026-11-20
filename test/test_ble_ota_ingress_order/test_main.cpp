#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

#include "modules/ble_ota_chunk_queue.hpp"
#include "modules/ble_event_queue.hpp"
#include "modules/ble_ota_ingress_order.hpp"
#include "modules/litchcraft_frame_store_receiver.hpp"

namespace {

using modules::BleOtaIngressHead;
using modules::BleOtaIngressSource;

bool Expect(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool TestBeginChunksEndStayInIngressOrder() {
    std::vector<uint32_t> controls = {1U, 4U};
    std::vector<uint32_t> chunks = {2U, 3U};
    std::vector<BleOtaIngressSource> observed;

    while (!controls.empty() || !chunks.empty()) {
        const BleOtaIngressHead head{
            !controls.empty(),
            controls.empty() ? 0U : controls.front(),
            !chunks.empty(),
            chunks.empty() ? 0U : chunks.front(),
        };
        const BleOtaIngressSource next = modules::SelectNextBleOtaIngress(head);
        observed.push_back(next);
        if (next == BleOtaIngressSource::Control) {
            controls.erase(controls.begin());
        } else if (next == BleOtaIngressSource::Chunk) {
            chunks.erase(chunks.begin());
        } else {
            return Expect(false, "ordered ingress unexpectedly stalled");
        }
    }

    return Expect(observed.size() == 4U, "ordered ingress emitted every item") &&
           Expect(observed[0] == BleOtaIngressSource::Control, "begin is first") &&
           Expect(observed[1] == BleOtaIngressSource::Chunk, "first chunk follows begin") &&
           Expect(observed[2] == BleOtaIngressSource::Chunk, "final chunk precedes end") &&
           Expect(observed[3] == BleOtaIngressSource::Control, "end is last");
}

bool TestChunkQueuePreservesIngressOrdinal() {
    modules::BleOtaChunkQueue queue;
    const uint8_t payload[] = {0x11U, 0x22U};
    if (!Expect(queue.Begin(), "chunk queue begins")) {
        return false;
    }
    if (!Expect(queue.Enqueue(payload, sizeof(payload), 42U), "chunk with ordinal enqueues")) {
        return false;
    }

    uint32_t ordinal = 0U;
    if (!Expect(queue.TryPeekOrdinal(ordinal), "queued ordinal is visible")) {
        return false;
    }
    if (!Expect(ordinal == 42U, "peek returns the ingress ordinal")) {
        return false;
    }

    const uint8_t* data = nullptr;
    size_t len = 0U;
    ordinal = 0U;
    if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal), "queued chunk is readable")) {
        return false;
    }
    const bool matches = ordinal == 42U && len == sizeof(payload) &&
                         data != nullptr && data[0] == 0x11U && data[1] == 0x22U;
    queue.ReleaseReadSlot();
    return Expect(matches, "read slot preserves data and ordinal");
}

bool TestControlQueueCanPeekWithoutConsumingOrdinal() {
    modules::BleEventQueue queue;
    if (!Expect(queue.Begin(), "control queue begins")) {
        return false;
    }

    modules::BleEvent event;
    event.type = modules::BleEventType::OtaControl;
    event.ingressOrdinal = 17U;
    if (!Expect(queue.Enqueue(event), "control event enqueues")) {
        return false;
    }

    modules::BleEvent peeked;
    if (!Expect(queue.TryPeek(peeked), "control event peeks")) {
        return false;
    }
    if (!Expect(peeked.ingressOrdinal == 17U, "peek preserves control ordinal")) {
        return false;
    }

    modules::BleEvent dequeued;
    return Expect(queue.TryDequeue(dequeued), "peek does not consume control") &&
           Expect(dequeued.ingressOrdinal == 17U, "dequeue preserves control ordinal");
}

bool TestCapabilityProbeHasPriorityWhenControlQueueIsFull() {
    modules::BleEventQueue queue;
    if (!Expect(queue.Begin(), "control queue begins for capability priority")) {
        return false;
    }
    modules::BleEvent filler;
    filler.type = modules::BleEventType::OtaControl;
    constexpr char slow[] = "{\"cmd\":\"runtime_health\"}";
    filler.payload.otaControl.len = sizeof(slow) - 1U;
    std::memcpy(filler.payload.otaControl.input, slow, sizeof(slow));
    for (uint8_t index = 0U; index < modules::BleEventQueue::CAPACITY; ++index) {
        filler.ingressOrdinal = index + 1U;
        if (!Expect(queue.Enqueue(filler), "ordinary control queue fills")) {
            return false;
        }
    }

    modules::BleEvent info;
    info.type = modules::BleEventType::OtaControl;
    constexpr char command[] = "{\"cmd\":\"info\"}";
    info.payload.otaControl.len = sizeof(command) - 1U;
    std::memcpy(info.payload.otaControl.input, command, sizeof(command));
    info.payload.otaControl.sessionEpoch = 17U;
    info.payload.otaControl.connectionEpoch = 23U;
    info.ingressOrdinal = 99U;
    if (!Expect(queue.Enqueue(info), "capability probe bypasses full ordinary queue")) {
        return false;
    }

    modules::BleEvent observed;
    return Expect(queue.TryDequeue(observed), "priority capability probe dequeues") &&
           Expect(observed.type == modules::BleEventType::OtaControl,
                  "priority event remains OTA control") &&
           Expect(observed.ingressOrdinal == 99U,
                  "priority capability ordinal is retained") &&
           Expect(observed.payload.otaControl.sessionEpoch == 17U &&
                      observed.payload.otaControl.connectionEpoch == 23U,
                  "priority capability ownership is retained") &&
           Expect(observed.payload.otaControl.len == sizeof(command) - 1U &&
                      std::memcmp(observed.payload.otaControl.input, command,
                                  sizeof(command) - 1U) == 0,
                  "priority capability payload is retained");
}

bool TestSelectionRestartsWhenPriorityHeadArrivesBeforeDequeue() {
    modules::BleEventQueue queue;
    modules::BleOtaChunkQueue bulk;
    if (!Expect(queue.Begin() && bulk.Begin(),
                "race regression queues begin")) {
        return false;
    }

    const uint8_t snapshot[] = {0x4dU, 2U, 8U, 1U, 0U, 0U, 0U, 1U, 0U};
    modules::BleEvent old;
    if (!Expect(modules::MakeLitchcraftIngressEvent(
                    snapshot, sizeof(snapshot), 1U, old),
                "snapshot ingress is valid")) {
        return false;
    }
    old.ingressOrdinal = 1U;
    if (!Expect(queue.Enqueue(old), "snapshot ingress enqueues")) return false;

    const uint8_t commit[] = {0x4dU, 2U, 16U, 1U, 0U, 0U, 0U, 2U, 0U};
    if (!Expect(bulk.Enqueue(commit, sizeof(commit), 3U),
                "commit barrier enqueues")) {
        return false;
    }

    modules::BleEvent selected;
    if (!Expect(queue.TryPeek(selected) && selected.ingressOrdinal == 1U,
                "ordinary head is selected before callback")) {
        return false;
    }

    const uint8_t exit[] = {0x4cU, 2U, 1U, 0U, 0U, 0U, 3U, 0U};
    modules::BleEvent arrived;
    if (!Expect(modules::MakeLitchcraftIngressEvent(
                    exit, sizeof(exit), 1U, arrived),
                "exit ingress is valid")) {
        return false;
    }
    arrived.ingressOrdinal = 4U;
    if (!Expect(queue.Enqueue(arrived),
                "priority callback arrives before dequeue")) {
        return false;
    }

    modules::BleEvent dispatched;
    if (!Expect(!queue.TryDequeueIfOrdinal(
                    selected.ingressOrdinal, dispatched),
                "stale selection cannot dequeue a different head")) {
        return false;
    }
    if (!Expect(queue.TryPeek(selected) && selected.ingressOrdinal == 4U,
                "selection restarts at the new priority head")) {
        return false;
    }

    uint32_t barrierOrdinal = 0U;
    if (!Expect(bulk.TryPeekFirstLitchcraftBarrierOrdinal(barrierOrdinal),
                "commit barrier remains visible")) {
        return false;
    }
    const auto source = modules::SelectNextBleOtaIngress({
        true,
        selected.ingressOrdinal,
        true,
        barrierOrdinal,
    });
    return Expect(source == BleOtaIngressSource::Chunk,
                  "earlier commit dispatches before the later exit");
}

bool TestConnectionObjectUsesOrdinaryLane() {
    modules::BleEventQueue queue;
    if (!Expect(queue.Begin(), "connection-session lane queue begins")) {
        return false;
    }
    std::array<uint8_t, 512U> object{};
    object[0U] = 0x14U;
    object.back() = 0x5aU;
    for (uint8_t index = 0U; index < modules::BleEventQueue::CAPACITY;
         ++index) {
        modules::BleEvent event;
        if (!Expect(modules::MakeConnectionSessionIngressEvent(
                        object.data(), object.size(), 7U, event),
                    "object event fits negotiated ATT ingress")) {
            return false;
        }
        event.ingressOrdinal = index + 1U;
        if (!Expect(queue.Enqueue(event),
                    "object traffic fills only the ordinary FIFO")) {
            return false;
        }
    }

    std::array<uint8_t, 19U> control{};
    control[0U] = 0x13U;
    control[1U] = 22U;
    control[10U] = 0x4dU;
    control[11U] = 2U;
    control[12U] = 22U;
    modules::BleEvent interactive;
    if (!Expect(modules::MakeConnectionSessionIngressEvent(
                    control.data(), control.size(), 7U, interactive),
                    "ordered Store Cancel is valid session ingress")) {
        return false;
    }
    interactive.ingressOrdinal = 99U;
    modules::BleEvent observed;
    auto oversizedControl = object;
    oversizedControl[0U] = 0x10U;
    modules::BleEvent malformed;
    if (!Expect(modules::MakeConnectionSessionIngressEvent(
                    oversizedControl.data(), oversizedControl.size(), 7U, malformed) &&
                    !queue.Enqueue(malformed),
                "512-byte malformed control cannot truncate into compact priority storage")) {
        return false;
    }
    return Expect(queue.Enqueue(interactive),
                  "ordered Store Cancel bypasses saturated Object traffic") &&
           Expect(queue.TryDequeue(observed) &&
                      observed.ingressOrdinal == 99U,
                  "ordered Store Cancel retains reserved priority") &&
           Expect(queue.TryDequeue(observed) &&
                      observed.payload.connectionSession.len == object.size() &&
                      observed.payload.connectionSession.input[511U] == 0x5aU,
                  "maximum Object callback copy retains length and last byte");
}

bool TestFrameStoreSnapshotRequestWakesStorageOwner() {
    modules::BleOtaChunkQueue queue;
    if (!Expect(queue.Begin(), "snapshot queue begins")) return false;

    std::array<uint8_t, 17U> snapshot{};
    snapshot[0U] = 0x4dU;
    snapshot[1U] = 2U;
    snapshot[2U] = modules::litchcraft_frame_store::STORE_SNAPSHOT;
    snapshot[3U] = 1U;
    snapshot[7U] = 1U;
    snapshot[9U] = 1U;
    if (!Expect(queue.Enqueue(snapshot.data(), snapshot.size(), 41U),
                "cross-phone Store snapshot request enqueues")) {
        return false;
    }
    return Expect(queue.TryPeekLitchcraftFrameStore(),
                  "Store snapshot request wakes the storage owner");
}

} // namespace

int main() {
    if (!TestBeginChunksEndStayInIngressOrder()) {
        return 1;
    }
    if (!TestChunkQueuePreservesIngressOrdinal()) {
        return 1;
    }
    if (!TestControlQueueCanPeekWithoutConsumingOrdinal()) {
        return 1;
    }
    if (!TestCapabilityProbeHasPriorityWhenControlQueueIsFull()) {
        return 1;
    }
    if (!TestSelectionRestartsWhenPriorityHeadArrivesBeforeDequeue()) {
        return 1;
    }
    if (!TestConnectionObjectUsesOrdinaryLane()) {
        return 1;
    }
    if (!TestFrameStoreSnapshotRequestWakesStorageOwner()) {
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
