#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "modules/ble_event_queue.hpp"
#include "modules/ble_ota_chunk_queue.hpp"
#include "modules/ble_ota_transaction.hpp"

namespace {

using modules::BleOtaBeginRequest;
using modules::BleOtaBeginCandidateResolution;
using modules::BleOtaEndRequest;
using modules::BleOtaTransaction;
using modules::BleOtaTransactionOps;
using modules::BleOtaTransactionResult;

struct FakeBackend {
    bool beginSucceeds = true;
    bool writeSucceeds = true;
    bool flushSucceeds = true;
    bool endSucceeds = true;
    uint32_t beginSize = 0U;
    size_t writtenBytes = 0U;
    uint32_t beginCalls = 0U;
    uint32_t writeCalls = 0U;
    uint32_t flushCalls = 0U;
    uint32_t endCalls = 0U;
    uint32_t abortCalls = 0U;
    uint32_t callSequence = 0U;
    uint32_t flushSequence = 0U;
    uint32_t endSequence = 0U;
    bool lastEndEvenIfRemaining = true;
};

bool Begin(void* const context, const uint32_t size) {
    FakeBackend& backend = *static_cast<FakeBackend*>(context);
    ++backend.beginCalls;
    backend.beginSize = size;
    return backend.beginSucceeds;
}

bool Write(void* const context, const uint8_t*, const size_t len) {
    FakeBackend& backend = *static_cast<FakeBackend*>(context);
    ++backend.writeCalls;
    if (backend.writeSucceeds) {
        backend.writtenBytes += len;
    }
    return backend.writeSucceeds;
}

bool Flush(void* const context) {
    FakeBackend& backend = *static_cast<FakeBackend*>(context);
    ++backend.flushCalls;
    backend.flushSequence = ++backend.callSequence;
    return backend.flushSucceeds;
}

bool End(void* const context, const bool evenIfRemaining) {
    FakeBackend& backend = *static_cast<FakeBackend*>(context);
    ++backend.endCalls;
    backend.endSequence = ++backend.callSequence;
    backend.lastEndEvenIfRemaining = evenIfRemaining;
    return backend.endSucceeds;
}

void Abort(void* const context) {
    FakeBackend& backend = *static_cast<FakeBackend*>(context);
    ++backend.abortCalls;
}

BleOtaTransactionOps Ops(FakeBackend& backend) {
    return BleOtaTransactionOps{&backend, &Begin, &Write, &Flush, &End, &Abort};
}

BleOtaBeginRequest Request(const uint32_t size,
                           const uint16_t chunkBytes = 512U,
                           const uint32_t epoch = 7U,
                           const uint32_t nowMs = 100U,
                           const bool strict = true) {
    return BleOtaBeginRequest{size, chunkBytes, epoch, nowMs, strict};
}

bool Expect(const bool condition, const char* const message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        return false;
    }
    return true;
}

bool TryParseBeginCommand(const char* const data,
                          const size_t len,
                          bool& outParsedBegin) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, data, len)) {
        return false;
    }
    const char* const cmd = doc["cmd"] | "";
    outParsedBegin = std::strcmp(cmd, "ota_begin") == 0 ||
                     std::strcmp(cmd, "ota_sim_begin") == 0;
    return true;
}

bool TestBeginBoundsAndImmutableSize() {
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;

    if (!Expect(transaction.Begin(Request(0U), ops) ==
                    BleOtaTransactionResult::InvalidSize,
                "zero-sized image is rejected") ||
        !Expect(transaction.Begin(Request(modules::BLE_OTA_SLOT_BYTES + 1U), ops) ==
                    BleOtaTransactionResult::ImageTooLarge,
                "image larger than the OTA slot is rejected") ||
        !Expect(transaction.Begin(Request(1U, 0U), ops) ==
                    BleOtaTransactionResult::InvalidNegotiatedChunk,
                "zero negotiated chunk is rejected") ||
        !Expect(transaction.Begin(Request(1U, 513U), ops) ==
                    BleOtaTransactionResult::InvalidNegotiatedChunk,
                "negotiated chunk above 512 is rejected") ||
        !Expect(backend.beginCalls == 0U,
                "invalid begin never reaches the backend")) {
        return false;
    }

    if (!Expect(transaction.Begin(Request(modules::BLE_OTA_SLOT_BYTES), ops) ==
                    BleOtaTransactionResult::Accepted,
                "exactly one 3264 KiB S3 slot is accepted") ||
        !Expect(transaction.Begin(Request(10U), ops) ==
                    BleOtaTransactionResult::AlreadyActive,
                "duplicate direct begin is explicit") ||
        !Expect(transaction.ExpectedSize() == modules::BLE_OTA_SLOT_BYTES,
                "active transaction size is immutable")) {
        return false;
    }

    transaction.Abort(ops);
    transaction.Abort(ops);
    return Expect(backend.abortCalls == 1U, "abort cleanup is idempotent");
}

bool TestChunkBoundsBeforeMutation() {
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;
    const uint8_t bytes[513] = {1U, 2U, 3U, 4U};
    if (!Expect(transaction.Begin(Request(4U, 3U), ops) ==
                    BleOtaTransactionResult::Accepted,
                "bounded chunk test begins")) {
        return false;
    }

    const uint32_t initialCrc = transaction.ComputedCrc32();
    const uint32_t initialActivity = transaction.LastActivityMs();
    if (!Expect(transaction.WriteChunk(7U, bytes, 0U, 101U, ops) ==
                    BleOtaTransactionResult::EmptyChunk,
                "empty chunk is rejected") ||
        !Expect(transaction.WriteChunk(7U, bytes, 513U, 102U, ops) ==
                    BleOtaTransactionResult::ChunkTooLarge,
                "chunk above 512 is rejected") ||
        !Expect(transaction.WriteChunk(7U, bytes, 4U, 103U, ops) ==
                    BleOtaTransactionResult::ChunkOverNegotiated,
                "chunk above negotiated size is rejected") ||
        !Expect(transaction.WriteChunk(6U, bytes, 3U, 104U, ops) ==
                    BleOtaTransactionResult::StaleSession,
                "stale session chunk is rejected") ||
        !Expect(transaction.ReceivedSize() == 0U &&
                    transaction.ComputedCrc32() == initialCrc &&
                    transaction.LastActivityMs() == initialActivity &&
                    backend.writeCalls == 0U,
                "rejected chunks do not mutate or stage")) {
        return false;
    }

    if (!Expect(transaction.WriteChunk(7U, bytes, 3U, 105U, ops) ==
                    BleOtaTransactionResult::Accepted,
                "valid negotiated chunk is accepted") ||
        !Expect(transaction.WriteChunk(7U, bytes, 2U, 106U, ops) ==
                    BleOtaTransactionResult::ChunkOvershoot,
                "overshooting chunk is rejected") ||
        !Expect(transaction.ReceivedSize() == 3U && backend.writtenBytes == 3U &&
                    transaction.LastActivityMs() == 105U,
                "overshoot rejection is pre-mutation")) {
        return false;
    }
    transaction.Abort(ops);
    return true;
}

bool TestStrictEndClaimsAndCrc() {
    const uint8_t payload[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    auto runRejectedEnd = [&](const BleOtaEndRequest& endRequest,
                              const BleOtaTransactionResult expected,
                              const char* const message) {
        FakeBackend backend;
        const BleOtaTransactionOps ops = Ops(backend);
        BleOtaTransaction transaction;
        if (transaction.Begin(Request(sizeof(payload)), ops) !=
                BleOtaTransactionResult::Accepted ||
            transaction.WriteChunk(7U, payload, sizeof(payload), 101U, ops) !=
                BleOtaTransactionResult::Accepted) {
            return Expect(false, "strict end fixture setup succeeds");
        }
        const bool rejected = transaction.End(endRequest, ops) == expected;
        const bool untouched = transaction.IsActive() && backend.flushCalls == 0U &&
                               backend.endCalls == 0U;
        transaction.Abort(ops);
        return Expect(rejected && untouched, message);
    };

    if (!runRejectedEnd(BleOtaEndRequest{false, 0U, "cbf43926"},
                        BleOtaTransactionResult::MissingEndSize,
                        "strict end requires size") ||
        !runRejectedEnd(BleOtaEndRequest{true, 8U, "cbf43926"},
                        BleOtaTransactionResult::EndSizeMismatch,
                        "end size must match immutable begin size") ||
        !runRejectedEnd(BleOtaEndRequest{true, 9U, nullptr},
                        BleOtaTransactionResult::MissingCrc,
                        "strict end requires CRC") ||
        !runRejectedEnd(BleOtaEndRequest{true, 9U, "bf43926"},
                        BleOtaTransactionResult::MalformedCrc,
                        "CRC must contain exactly eight digits") ||
        !runRejectedEnd(BleOtaEndRequest{true, 9U, "cbf4392g"},
                        BleOtaTransactionResult::MalformedCrc,
                        "CRC rejects non-hex digits") ||
        !runRejectedEnd(BleOtaEndRequest{true, 9U, "00000000"},
                        BleOtaTransactionResult::ChecksumMismatch,
                        "CRC mismatch is rejected")) {
        return false;
    }

    FakeBackend incompleteBackend;
    const BleOtaTransactionOps incompleteOps = Ops(incompleteBackend);
    BleOtaTransaction incomplete;
    if (!Expect(incomplete.Begin(Request(9U), incompleteOps) ==
                    BleOtaTransactionResult::Accepted,
                "incomplete fixture begins") ||
        !Expect(incomplete.WriteChunk(7U, payload, 8U, 101U, incompleteOps) ==
                    BleOtaTransactionResult::Accepted,
                "incomplete fixture receives prefix") ||
        !Expect(incomplete.End(BleOtaEndRequest{true, 9U, "9ae0daaf"}, incompleteOps) ==
                    BleOtaTransactionResult::Incomplete,
                "end cannot force-finalize incomplete receive") ||
        !Expect(incompleteBackend.flushCalls == 0U && incompleteBackend.endCalls == 0U,
                "incomplete end never flushes or finalizes")) {
        return false;
    }
    incomplete.Abort(incompleteOps);

    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;
    return Expect(transaction.Begin(Request(sizeof(payload)), ops) ==
                      BleOtaTransactionResult::Accepted,
                  "valid CRC fixture begins") &&
           Expect(transaction.WriteChunk(7U, payload, sizeof(payload), 101U, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "valid CRC fixture receives payload") &&
           Expect(transaction.ComputedCrc32() == 0xCBF43926U,
                  "CRC preserves deployed IEEE convention") &&
           Expect(transaction.End(BleOtaEndRequest{true, 9U, "CBF43926"}, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "exact claims and CRC finalize") &&
           Expect(backend.flushCalls == 1U && backend.endCalls == 1U &&
                      backend.flushSequence < backend.endSequence &&
                      !backend.lastEndEvenIfRemaining,
                  "finalization flushes then calls end(false)") &&
           Expect(transaction.End(BleOtaEndRequest{true, 9U, "cbf43926"}, ops) ==
                      BleOtaTransactionResult::AlreadyComplete,
                  "duplicate end is explicit");
}

bool TestLegacyGateStillRequiresCompleteReceive() {
    const uint8_t payload[] = {0x10U, 0x20U};
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;
    return Expect(transaction.Begin(Request(2U, 20U, 4U, 0U, false), ops) ==
                      BleOtaTransactionResult::Accepted,
                  "legacy profile begins") &&
           Expect(transaction.WriteChunk(4U, payload, sizeof(payload), 1U, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "legacy profile receives exact bytes") &&
           Expect(transaction.End(BleOtaEndRequest{}, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "legacy profile permits omitted end claims") &&
           Expect(!backend.lastEndEvenIfRemaining,
                  "legacy profile also never force-finalizes");
}

bool TestBackendFailuresCleanUp() {
    const uint8_t payload[] = {0xA5U};
    {
        FakeBackend backend;
        backend.beginSucceeds = false;
        BleOtaTransaction transaction;
        const BleOtaTransactionOps ops = Ops(backend);
        if (!Expect(transaction.Begin(Request(1U), ops) ==
                        BleOtaTransactionResult::BeginFailed &&
                        !transaction.IsActive() && backend.abortCalls == 1U,
                    "begin failure is cleaned up")) {
            return false;
        }
    }
    {
        FakeBackend backend;
        backend.writeSucceeds = false;
        BleOtaTransaction transaction;
        const BleOtaTransactionOps ops = Ops(backend);
        transaction.Begin(Request(1U), ops);
        if (!Expect(transaction.WriteChunk(7U, payload, 1U, 1U, ops) ==
                        BleOtaTransactionResult::WriteFailed &&
                        !transaction.IsActive() && backend.abortCalls == 1U,
                    "write failure aborts and clears")) {
            return false;
        }
    }
    for (uint8_t failure = 0U; failure < 2U; ++failure) {
        FakeBackend backend;
        backend.flushSucceeds = failure != 0U;
        backend.endSucceeds = failure == 0U;
        BleOtaTransaction transaction;
        const BleOtaTransactionOps ops = Ops(backend);
        transaction.Begin(Request(1U), ops);
        transaction.WriteChunk(7U, payload, 1U, 1U, ops);
        const BleOtaTransactionResult expected =
            (failure == 0U) ? BleOtaTransactionResult::FlushFailed
                            : BleOtaTransactionResult::EndFailed;
        if (!Expect(transaction.End(BleOtaEndRequest{true, 1U, "74beb8ea"}, ops) == expected &&
                        !transaction.IsActive() && backend.abortCalls == 1U,
                    (failure == 0U) ? "flush failure aborts and clears"
                                    : "end failure aborts and clears")) {
            return false;
        }
        if (failure != 0U &&
            !Expect(!backend.lastEndEvenIfRemaining,
                    "failed backend end still receives false")) {
            return false;
        }
    }
    return true;
}

bool TestTimeoutWrapRetryAndRawBeforeBegin() {
    const uint8_t byte = 0x01U;
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;
    if (!Expect(transaction.WriteChunk(1U, &byte, 1U, 0U, ops) ==
                    BleOtaTransactionResult::NotActive,
                "raw data before begin is harmless") ||
        !Expect(transaction.Begin(Request(1U, 20U, 9U, 0xFFFFFFF0U), ops) ==
                    BleOtaTransactionResult::Accepted,
                "wrap timeout fixture begins") ||
        !Expect(transaction.CheckTimeout(
                    0xFFFFFFF0U + modules::BLE_OTA_INACTIVITY_TIMEOUT_MS - 1U,
                    ops) == BleOtaTransactionResult::Accepted,
                "timeout waits through wrap until deadline") ||
        !Expect(transaction.CheckTimeout(
                    0xFFFFFFF0U + modules::BLE_OTA_INACTIVITY_TIMEOUT_MS,
                    ops) == BleOtaTransactionResult::TimedOut &&
                    !transaction.IsActive() && backend.abortCalls == 1U,
                "timeout fires wrap-safely and cleans up") ||
        !Expect(transaction.Begin(Request(1U, 20U, 10U, 1U), ops) ==
                    BleOtaTransactionResult::Accepted,
                "retry after timeout begins cleanly")) {
        return false;
    }
    transaction.Abort(ops);
    return Expect(transaction.Begin(Request(1U, 20U, 11U, 2U), ops) ==
                      BleOtaTransactionResult::Accepted,
                  "rebegin after disconnect-style abort succeeds");
}

bool TestDisconnectAndShutdownCleanupAreIdempotent() {
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;
    if (!Expect(transaction.Begin(Request(16U), ops) ==
                    BleOtaTransactionResult::Accepted,
                "disconnect cleanup fixture begins")) {
        return false;
    }
    transaction.Abort(ops);
    transaction.Abort(ops);
    if (!Expect(!transaction.IsActive() && backend.abortCalls == 1U,
                "disconnect plus shutdown cleanup aborts backend once")) {
        return false;
    }
    if (!Expect(transaction.Begin(Request(8U, 20U, 8U), ops) ==
                    BleOtaTransactionResult::Accepted,
                "retry after lifecycle cleanup begins")) {
        return false;
    }
    transaction.Abort(ops);
    return Expect(backend.abortCalls == 2U,
                  "new session owns one new cleanup");
}

bool TestQueueEpochOverflowAndOrderedCompatibility() {
    modules::BleOtaChunkQueue queue;
    const uint8_t byte = 0x42U;
    if (!Expect(queue.Begin(), "queue begins")) {
        return false;
    }
    const uint32_t oldEpoch = queue.CurrentEpoch();
    if (!Expect(queue.Enqueue(&byte, 1U, 1U), "old epoch chunk enqueues")) {
        return false;
    }
    const uint32_t newEpoch = queue.AdvanceEpoch();
    if (!Expect(queue.Enqueue(&byte, 1U, 3U), "new epoch chunk enqueues")) {
        return false;
    }
    if (!Expect(!queue.AdvanceEpochIfCurrent(oldEpoch) &&
                    queue.CurrentEpoch() == newEpoch,
                "old-session cleanup cannot invalidate a queued new epoch")) {
        return false;
    }

    const uint8_t* data = nullptr;
    size_t len = 0U;
    uint32_t ordinal = 0U;
    uint32_t epoch = 0U;
    if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                    ordinal == 1U && epoch == oldEpoch,
                "queued chunk retains stale epoch")) {
        return false;
    }
    queue.ReleaseReadSlot();
    if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                    ordinal == 3U && epoch == newEpoch,
                "ordered next chunk owns new epoch")) {
        return false;
    }
    queue.ReleaseReadSlot();

    for (uint32_t index = 0U; index < modules::BleOtaChunkQueue::CAPACITY; ++index) {
        if (!Expect(queue.Enqueue(&byte, 1U, index + 10U),
                    "queue fills to its fixed bound")) {
            return false;
        }
    }
    if (!Expect(!queue.Enqueue(&byte, 1U, 100U),
                "active queue overflow is rejected")) {
        return false;
    }
    modules::BleOtaChunkQueueFailure failure =
        modules::BleOtaChunkQueueFailure::None;
    return Expect(queue.TryConsumeFailure(newEpoch, failure) &&
                      failure == modules::BleOtaChunkQueueFailure::Full,
                  "queue_full is reported for the owning epoch");
}

bool TestRejectedBeginCandidatesFailClosed() {
    const char probe[] = "{\"cmd\":\"ota_probe\"}";
    const char validBegin[] = "{\"cmd\":\"ota_begin\",\"size\":1}";
    const char truncated[] = "{\"cmd\":\"ota_begin\"";
    const char duplicate[] =
        "{\"cmd\":\"ota_begin\",\"cmd\":\"ping\"}";
    const char nested[] =
        "{\"wrapper\":{\"cmd\":\"ota_begin\"},\"cmd\":\"ping\"}";
    const bool truncatedCandidate = modules::IsBleOtaBeginControlCandidate(
        truncated, sizeof(truncated) - 1U);
    const bool duplicateCandidate = modules::IsBleOtaBeginControlCandidate(
        duplicate, sizeof(duplicate) - 1U);
    const bool nestedCandidate = modules::IsBleOtaBeginControlCandidate(
        nested, sizeof(nested) - 1U);
    bool parsedBegin = false;
    const bool validBeginParsed = TryParseBeginCommand(
        validBegin, sizeof(validBegin) - 1U, parsedBegin) && parsedBegin;
    const bool truncatedParsed = TryParseBeginCommand(
        truncated, sizeof(truncated) - 1U, parsedBegin);
    const bool duplicateResolvesNonBegin = TryParseBeginCommand(
        duplicate, sizeof(duplicate) - 1U, parsedBegin) && !parsedBegin;
    const bool nestedResolvesNonBegin = TryParseBeginCommand(
        nested, sizeof(nested) - 1U, parsedBegin) && !parsedBegin;

    return Expect(!modules::IsBleOtaBeginControlCandidate(
                      probe, sizeof(probe) - 1U),
                  "probe leaves the current epoch unchanged") &&
           Expect(modules::IsBleOtaBeginControlCandidate(
                      validBegin, sizeof(validBegin) - 1U),
                  "valid begin owns a candidate epoch") &&
           Expect(validBeginParsed,
                  "normal begin agrees with the full JSON parser") &&
           Expect(truncatedCandidate,
                  "truncated begin-like JSON owns a candidate epoch") &&
           Expect(!truncatedParsed,
                  "truncated begin-like JSON fails full parsing") &&
           Expect(duplicateCandidate && nestedCandidate,
                  "duplicate and nested begin-like commands expose scanner/parser disagreement") &&
           Expect(duplicateResolvesNonBegin && nestedResolvesNonBegin,
                  "full parser resolves duplicate and nested cases as non-begin") &&
           Expect(modules::ResolveBleOtaBeginCandidate(
                      truncatedCandidate, false, false) ==
                      BleOtaBeginCandidateResolution::RejectAndInvalidate,
                  "malformed begin candidate must terminate and invalidate") &&
           Expect(modules::ResolveBleOtaBeginCandidate(
                      duplicateCandidate, false, false) ==
                      BleOtaBeginCandidateResolution::RejectAndInvalidate,
                  "duplicate command disagreement must terminate and invalidate") &&
           Expect(modules::ResolveBleOtaBeginCandidate(
                      nestedCandidate, false, false) ==
                      BleOtaBeginCandidateResolution::RejectAndInvalidate,
                  "nested command disagreement must terminate and invalidate") &&
           Expect(modules::ResolveBleOtaBeginCandidate(
                      false, true, false) ==
                      BleOtaBeginCandidateResolution::RejectAndInvalidate,
                  "parser-only begin disagreement fails closed") &&
           Expect(modules::ResolveBleOtaBeginCandidate(
                      true, true, true) ==
                      BleOtaBeginCandidateResolution::Accepted,
                  "accepted begin commits the candidate epoch") &&
           Expect(modules::ResolveBleOtaBeginCandidate(
                      false, false, false) ==
                      BleOtaBeginCandidateResolution::Unrelated,
                  "ordinary control remains unrelated");
}

bool TestRejectedCandidateCleansUpForRetry() {
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    modules::BleOtaChunkQueue queue;
    BleOtaTransaction transaction;
    const uint8_t byte = 0x21U;
    if (!Expect(queue.Begin(), "candidate retry queue begins")) {
        return false;
    }
    const uint32_t initialEpoch = queue.AdvanceEpoch();
    if (!Expect(transaction.Begin(Request(1U, 20U, initialEpoch), ops) ==
                    BleOtaTransactionResult::Accepted,
                "candidate retry initial transaction begins")) {
        return false;
    }

    const uint32_t rejectedEpoch = queue.AdvanceEpoch();
    const BleOtaBeginCandidateResolution rejected =
        modules::ResolveBleOtaBeginCandidate(true, false, false);
    if (rejected == BleOtaBeginCandidateResolution::RejectAndInvalidate) {
        transaction.Abort(ops);
        (void)queue.AdvanceEpochIfCurrent(rejectedEpoch);
    }
    if (!Expect(!transaction.IsActive() && backend.abortCalls == 1U,
                "rejected candidate immediately aborts the old transaction")) {
        return false;
    }

    const uint32_t retryEpoch = queue.AdvanceEpoch();
    return Expect(transaction.Begin(Request(1U, 20U, retryEpoch), ops) ==
                      BleOtaTransactionResult::Accepted,
                  "clean retry begins after rejected candidate") &&
           Expect(transaction.WriteChunk(retryEpoch, &byte, 1U, 101U, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "clean retry accepts its owned chunk") &&
           Expect(transaction.End(BleOtaEndRequest{true, 1U, "9e6bffd3"}, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "clean retry completes normally");
}

bool TestQueueFullRejectedBeginSignalsSafeCleanup() {
    modules::BleEventQueue eventQueue;
    modules::BleOtaChunkQueue chunkQueue;
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    BleOtaTransaction transaction;
    if (!Expect(eventQueue.Begin(), "control queue begins") ||
        !Expect(chunkQueue.Begin(), "queue-full cleanup chunk queue begins")) {
        return false;
    }

    const uint32_t activeEpoch = chunkQueue.AdvanceEpoch();
    if (!Expect(transaction.Begin(Request(1U, 20U, activeEpoch), ops) ==
                    BleOtaTransactionResult::Accepted,
                "queue-full cleanup transaction begins")) {
        return false;
    }

    modules::BleEvent filler;
    filler.type = modules::BleEventType::OtaControl;
    filler.payload.otaControl.input[0] = 'x';
    filler.payload.otaControl.input[1] = '\0';
    filler.payload.otaControl.len = 1U;
    for (uint32_t index = 0U;
         index < modules::BleEventQueue::CAPACITY;
         ++index) {
        if (!Expect(eventQueue.Enqueue(filler),
                    "control queue fills deterministically")) {
            return false;
        }
    }

    const uint32_t rejectedEpoch = chunkQueue.AdvanceEpoch();
    modules::BleEvent beginEvent;
    beginEvent.type = modules::BleEventType::OtaControl;
    beginEvent.payload.otaControl.sessionEpoch = rejectedEpoch;
    const bool enqueued = eventQueue.Enqueue(beginEvent);
    const modules::BleOtaRejectedBeginSignal rejectedBegin =
        modules::ResolveBleOtaBeginIngressRejectSignal(
            true,
            enqueued,
            beginEvent.payload.otaControl.sessionEpoch,
            modules::BleOtaRejectedBeginReason::EventQueueFull);
    if (rejectedBegin.cleanupEpoch != 0U) {
        transaction.Abort(ops);
        (void)chunkQueue.AdvanceEpochIfCurrent(
            rejectedBegin.cleanupEpoch);
    }

    return Expect(!enqueued,
                  "filled control queue rejects the begin event") &&
           Expect(rejectedBegin.cleanupEpoch == rejectedEpoch,
                  "rejected begin overflow preserves its candidate epoch") &&
           Expect(rejectedBegin.reason ==
                      modules::BleOtaRejectedBeginReason::EventQueueFull,
                  "queue-full cleanup preserves its rejection reason") &&
           Expect(std::strcmp(
                      modules::BleOtaRejectedBeginErrorDetail(
                          rejectedBegin.reason),
                      "queue_full") == 0,
                  "queue-full cleanup maps to a host-visible queue_full error") &&
           Expect(!transaction.IsActive() && backend.abortCalls == 1U,
                  "safe cleanup aborts the active transfer once") &&
           Expect(chunkQueue.CurrentEpoch() != rejectedEpoch,
                  "safe cleanup invalidates the rejected candidate epoch") &&
           Expect(transaction.Begin(
                      Request(1U, 20U, chunkQueue.CurrentEpoch()), ops) ==
                      BleOtaTransactionResult::Accepted,
                  "queue-full rejection still leaves a clean retry path");
}

bool TestOversizedRejectedBeginSignalsSafeCleanup() {
    const char oversizedBegin[] =
        "{\"cmd\":\"ota_begin\",\"size\":1,\"padding\":\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}";
    const modules::BleOtaControlIngressClassification classification =
        modules::ClassifyBleOtaControlIngress(
            oversizedBegin,
            sizeof(oversizedBegin) - 1U,
            192U);
    FakeBackend backend;
    const BleOtaTransactionOps ops = Ops(backend);
    modules::BleOtaChunkQueue queue;
    BleOtaTransaction transaction;
    const uint8_t byte = 0x22U;
    if (!Expect(queue.Begin(), "oversized cleanup queue begins")) {
        return false;
    }

    const uint32_t activeEpoch = queue.AdvanceEpoch();
    if (!Expect(transaction.Begin(Request(1U, 20U, activeEpoch), ops) ==
                    BleOtaTransactionResult::Accepted,
                "oversized cleanup transaction begins")) {
        return false;
    }
    const uint32_t rejectedEpoch = classification.beginCandidate
                                       ? queue.AdvanceEpoch()
                                       : queue.CurrentEpoch();
    const modules::BleOtaRejectedBeginSignal rejectedBegin =
        modules::ResolveBleOtaBeginIngressRejectSignal(
            classification.beginCandidate,
            classification.acceptForQueue,
            rejectedEpoch,
            classification.rejectReason);
    if (rejectedBegin.cleanupEpoch != 0U) {
        transaction.Abort(ops);
        (void)queue.AdvanceEpochIfCurrent(
            rejectedBegin.cleanupEpoch);
    }

    return Expect(classification.beginCandidate,
                  "oversized recognizable begin still claims candidate ownership") &&
           Expect(!classification.acceptForQueue,
                  "oversized begin is rejected before queueing") &&
           Expect(classification.rejectReason ==
                      modules::BleOtaRejectedBeginReason::OversizedControl,
                  "oversized begin preserves its rejection reason") &&
           Expect(rejectedBegin.reason ==
                      modules::BleOtaRejectedBeginReason::OversizedControl,
                  "oversized cleanup preserves its rejection reason") &&
           Expect(std::strcmp(
                      modules::BleOtaRejectedBeginErrorDetail(
                          rejectedBegin.reason),
                      "invalid_begin") == 0,
                  "oversized cleanup maps to a host-visible invalid_begin error") &&
           Expect(!transaction.IsActive() && backend.abortCalls == 1U,
                  "oversized begin aborts the active transfer exactly once") &&
           Expect(queue.CurrentEpoch() != rejectedBegin.cleanupEpoch,
                  "oversized begin invalidates its candidate epoch") &&
           Expect(transaction.Begin(
                      Request(1U, 20U, queue.CurrentEpoch()), ops) ==
                      BleOtaTransactionResult::Accepted,
                  "oversized rejected begin still leaves a clean retry path") &&
           Expect(transaction.WriteChunk(
                      queue.CurrentEpoch(), &byte, 1U, 101U, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "clean retry still accepts data") &&
           Expect(transaction.End(BleOtaEndRequest{true, 1U, "0762ae69"}, ops) ==
                      BleOtaTransactionResult::Accepted,
                  "clean retry still completes after oversized rejection");
}

bool TestNegotiationBoundsMatchQueueCapacity() {
    return Expect(modules::ClampBleOtaBurstCount(0U) == 1U,
                  "zero burst clamps to one") &&
           Expect(modules::ClampBleOtaBurstCount(48U) == 48U,
                  "recommended burst remains unchanged") &&
           Expect(modules::ClampBleOtaBurstCount(UINT32_MAX) ==
                      modules::BLE_OTA_MAX_BURST_COUNT,
                  "extreme burst clamps to queue slots") &&
           Expect(modules::ClampBleOtaWindowBytes(0U, 512U) == 1024U,
                  "zero window clamps to minimum") &&
           Expect(modules::ClampBleOtaWindowBytes(UINT32_MAX, 512U) ==
                      modules::BleOtaChunkQueue::GUARANTEED_PAYLOAD_BYTES,
                  "extreme 512-byte window clamps to owned payload bytes") &&
           Expect(modules::ClampBleOtaWindowBytes(UINT32_MAX, 20U) ==
                      20U * modules::BLE_OTA_MAX_BURST_COUNT,
                  "small-chunk window cannot exceed queue bytes") &&
           Expect(modules::ClampBleOtaWindowBytes(0U, 0U) == 0U,
                  "invalid negotiated chunk has no window");
}

bool TestClaimedQueueHeadStillCountsOnce() {
    modules::BleOtaChunkQueue queue;
    const uint8_t byte = 0x44U;
    if (!Expect(queue.Begin() && queue.EnqueueReserved(&byte, 1U, 0U, 71U) ==
                    modules::BleOtaChunkQueue::Admission::Accepted,
                "claimed-capacity queue begins with one credited item and ordinal zero")) {
        return false;
    }
    const uint8_t* data = nullptr;
    size_t len = 0U;
    uint32_t ordinal = 0U;
    uint32_t epoch = 0U;
    if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch),
                "queue head is claimed")) {
        return false;
    }
    for (uint32_t index = 1U;
         index < modules::BleOtaChunkQueue::CAPACITY;
         ++index) {
        if (!Expect(queue.Enqueue(&byte, 1U, index + 1U),
                    "claimed head is not double-counted")) {
            return false;
        }
    }
    const bool fullRejected = !queue.Enqueue(&byte, 1U, 100U);
    queue.DeferReadSlot();
    uint32_t creditToken = 0U, credits = 0U;
    if (!Expect(!queue.TakeReturnedObjectCredits(creditToken, credits) &&
                    !queue.Enqueue(&byte, 1U, 101U) &&
                    queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                    ordinal == 0U && len == 1U && data[0U] == byte,
                "deferring retains the exact head, byte capacity and Object credit")) return false;
    queue.ReleaseReadSlot();
    if (!Expect(queue.TakeReturnedObjectCredits(creditToken, credits) &&
                    creditToken == 71U && credits == 1U,
                "only consumption returns the deferred Object credit once")) return false;
    if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch),
                "defer/consume leaves the next FIFO item readable")) return false;
    queue.DeferReadSlot();
    queue.AdvanceEpoch();
    if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                    epoch != queue.CurrentEpoch(),
                "epoch retirement remains visible after releasing a deferred claim")) return false;
    queue.ReleaseReadSlot();
    return Expect(fullRejected,
                  "queue rejects the first byte beyond its fixed slots");
}

bool TestBytePackedQueueWrapAndMaximumRecord() {
    modules::BleOtaChunkQueue queue;
    std::array<uint8_t, 244U> ordinary{};
    std::array<uint8_t, 300U> tail{};
    std::array<uint8_t, modules::BleOtaChunkQueue::MAX_CHUNK_BYTES> maximum{};
    if (!Expect(queue.Begin(), "byte-packed queue begins")) return false;
    for (uint32_t ordinal = 1U;
         ordinal <= modules::BleOtaChunkQueue::CAPACITY;
         ++ordinal) {
        for (size_t index = 0U; index < ordinary.size(); ++index) {
            ordinary[index] = static_cast<uint8_t>(ordinal + index);
        }
        if (!Expect(queue.Enqueue(ordinary.data(), ordinary.size(), ordinal),
                    "ordinary ATT244 record enqueues")) return false;
    }
    if (!Expect(queue.HighWaterMark() == modules::BleOtaChunkQueue::CAPACITY,
                "all 64 ATT244 descriptors fit the byte arena")) return false;
    if (!Expect(!queue.Enqueue(ordinary.data(), ordinary.size(), 65U),
                "65th descriptor is rejected")) return false;
    modules::BleOtaChunkQueueFailure failure =
        modules::BleOtaChunkQueueFailure::None;
    if (!Expect(queue.TryConsumeFailure(queue.CurrentEpoch(), failure) &&
                    failure == modules::BleOtaChunkQueueFailure::Full,
                "descriptor overflow reports Full in the current epoch")) {
        return false;
    }

    const uint8_t* data = nullptr;
    size_t len = 0U;
    uint32_t ordinal = 0U;
    uint32_t epoch = 0U;
    for (uint32_t expected = 1U;
         expected <= modules::BleOtaChunkQueue::CAPACITY;
         ++expected) {
        if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                        ordinal == expected && len == ordinary.size(),
                    "ordinary record survives packed dequeue")) return false;
        for (size_t index = 0U; index < len; ++index) {
            if (!Expect(data[index] ==
                            static_cast<uint8_t>(expected + index),
                        "all 64 packed records remain byte-exact")) {
                return false;
            }
        }
        queue.ReleaseReadSlot();
    }

    for (uint32_t cycle = 0U; cycle < 3U; ++cycle) {
        if (!Expect(queue.Begin(), "wrap cycle resets the queue")) return false;
        for (uint32_t expected = 1U; expected <= 63U; ++expected) {
            std::fill(ordinary.begin(), ordinary.end(),
                      static_cast<uint8_t>(expected + cycle));
            if (!Expect(queue.Enqueue(
                            ordinary.data(), ordinary.size(), expected),
                        "pre-wrap ordinary record enqueues")) return false;
        }
        std::fill(tail.begin(), tail.end(), static_cast<uint8_t>(0x70U + cycle));
        if (!Expect(queue.Enqueue(tail.data(), tail.size(), 64U),
                    "tail record leaves less than one maximum chunk")) {
            return false;
        }
        for (uint32_t expected = 1U; expected <= 3U; ++expected) {
            if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                            ordinal == expected && len == ordinary.size(),
                        "head space is reclaimed before wrap")) return false;
            queue.ReleaseReadSlot();
        }

        for (size_t index = 0U; index < maximum.size(); ++index) {
            maximum[index] = static_cast<uint8_t>(index ^ (0xa5U + cycle));
        }
        maximum[0] = 0x4dU;
        maximum[1] = 2U;
        maximum[2] = 16U;
        if (!Expect(queue.Enqueue(maximum.data(), maximum.size(), 65U),
                    "maximum record enters the byte-ring wrap branch")) {
            return false;
        }
        uint32_t barrierOrdinal = 0U;
        if (!Expect(queue.TryPeekFirstLitchcraftBarrierOrdinal(barrierOrdinal) &&
                        barrierOrdinal == 65U,
                    "barrier scan sees the wrapped contiguous record")) {
            return false;
        }
        const uint32_t wrappedEpoch = queue.CurrentEpoch();
        if (!Expect(queue.AdvanceEpoch() != wrappedEpoch &&
                        !queue.TryPeekFirstLitchcraftBarrierOrdinal(
                            barrierOrdinal),
                    "epoch advance hides stale wrapped barriers")) {
            return false;
        }

        for (uint32_t expected = 4U; expected <= 63U; ++expected) {
            if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                            ordinal == expected && len == ordinary.size() &&
                            epoch == wrappedEpoch,
                        "pre-wrap FIFO and epoch remain intact")) return false;
            const uint8_t expectedByte =
                static_cast<uint8_t>(expected + cycle);
            if (!Expect(std::all_of(data, data + len,
                                   [expectedByte](uint8_t value) {
                                       return value == expectedByte;
                                   }),
                        "pre-wrap payload remains byte-exact")) return false;
            queue.ReleaseReadSlot();
        }
        if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                        ordinal == 64U && len == tail.size() &&
                        std::equal(tail.begin(), tail.end(), data),
                    "tail payload remains byte-exact")) return false;
        queue.ReleaseReadSlot();
        if (!Expect(queue.TryAcquireReadSlot(data, len, ordinal, epoch) &&
                        ordinal == 65U && len == maximum.size() &&
                        epoch == wrappedEpoch &&
                        std::equal(maximum.begin(), maximum.end(), data),
                    "wrapped maximum record remains byte-exact")) {
            return false;
        }
        queue.ReleaseReadSlot();
    }
    return Expect(sizeof(modules::BleOtaChunkQueue) <= 18U * 1024U,
                  "bulk queue preserves the NimBLE heap reserve");
}

bool TestMinimumMtuErrorPayloadsStayStable() {
    struct Case {
        const char* canonical;
        const char* minimumPayload;
    };

    static constexpr Case kCases[] = {
        {"unknown_cmd", "{\"e\":\"unknown_cmd\"}"},
        {"invalid_begin", "{\"e\":\"inv_begin\"}"},
        {"invalid_test_args", "{\"e\":\"test_args\"}"},
        {"notify_test_failed", "{\"e\":\"notify_fail\"}"},
        {"image_too_large", "{\"e\":\"image_large\"}"},
        {"not_in_progress", "{\"e\":\"idle_ota\"}"},
        {"size_overshoot", "{\"e\":\"overshoot\"}"},
        {"size_mismatch", "{\"e\":\"wrong_size\"}"},
        {"checksum_mismatch", "{\"e\":\"crc_mismatch\"}"},
        {"chunk_too_large", "{\"e\":\"large_chunk\"}"},
    };

    char payload[32] = {};
    for (const Case& testCase : kCases) {
        const size_t len = modules::FormatBleOtaErrorPayload(
            testCase.canonical, 20U, payload, sizeof(payload));
        if (!Expect(len == std::strlen(testCase.minimumPayload) &&
                        std::strcmp(payload, testCase.minimumPayload) == 0,
                    testCase.canonical)) {
            return false;
        }
    }

    char roomyInvalidBeginPayload[32] = {};
    char roomyOvershootPayload[32] = {};
    const size_t roomyLen = modules::FormatBleOtaErrorPayload(
        "invalid_begin",
        64U,
        roomyInvalidBeginPayload,
        sizeof(roomyInvalidBeginPayload));
    const size_t roomyOvershootLen = modules::FormatBleOtaErrorPayload(
        "size_overshoot",
        64U,
        roomyOvershootPayload,
        sizeof(roomyOvershootPayload));
    return Expect(roomyLen == std::strlen("{\"e\":\"invalid_begin\"}") &&
                      std::strcmp(
                          roomyInvalidBeginPayload,
                          "{\"e\":\"invalid_begin\"}") == 0,
                  "roomy MTU keeps the canonical invalid_begin token") &&
           Expect(roomyOvershootLen ==
                      std::strlen("{\"e\":\"size_overshoot\"}") &&
                      std::strcmp(
                          roomyOvershootPayload,
                          "{\"e\":\"size_overshoot\"}") == 0,
                  "roomy MTU keeps the canonical size_overshoot token") &&
           Expect(modules::FormatBleOtaErrorPayload(
                      "unknown_cmd", 20U, payload, 8U) == 0U,
                  "undersized output buffer rejects truncation") &&
           Expect(payload[0] == '\0',
                  "truncation rejection clears the payload buffer");
}

bool TestSchema4UsesSharedPackedIngressAndOrderedBarriers() {
    modules::BleOtaChunkQueue queue;
    if (!Expect(queue.Begin(), "schema-4 shared queue begins")) return false;
    std::array<uint8_t, 10U> probe{{0x4dU, 2U, 1U, 1U, 0U,
                                    0U, 0U, 1U, 0U, 4U}};
    if (!Expect(queue.Enqueue(probe.data(), probe.size(), 1U) &&
                    queue.TryPeekLitchcraftFrameStore(),
                "Inspect[4] enters the existing bulk queue")) return false;
    const uint8_t* bytes = nullptr;
    size_t size = 0U;
    uint32_t ordinal = 0U;
    uint32_t epoch = 0U;
    if (!Expect(queue.TryAcquireReadSlot(
                    bytes, size, ordinal, epoch) && bytes != probe.data() &&
                    size == probe.size() && ordinal == 1U,
                "worker reads the packed copy rather than a second queue")) {
        return false;
    }
    queue.ReleaseReadSlot();

    probe[9U] = 5U;
    if (!Expect(queue.Enqueue(probe.data(), probe.size(), 2U) &&
                    queue.TryPeekLitchcraftFrameStore(),
                "Inspect[5] predecessor-delta probe reaches the Store worker")) {
        return false;
    }
    if (!Expect(queue.TryAcquireReadSlot(
                    bytes, size, ordinal, epoch) && ordinal == 2U,
                "schema-5 probe remains byte-packed at queue head")) {
        return false;
    }
    queue.ReleaseReadSlot();

    probe[9U] = 6U;
    if (!Expect(queue.Enqueue(probe.data(), probe.size(), 3U) &&
                    queue.TryPeekLitchcraftFrameStore(),
                "Inspect[6] resident-selection probe reaches the Store worker")) {
        return false;
    }
    if (!Expect(queue.TryAcquireReadSlot(
                    bytes, size, ordinal, epoch) && ordinal == 3U,
                "resident-selection probe remains byte-packed at queue head")) {
        return false;
    }
    queue.ReleaseReadSlot();

    for (const uint8_t operation : {18U, 19U, 20U, 21U, 22U, 24U, 25U, 26U}) {
        std::array<uint8_t, 13U> packet{};
        packet[0] = 0x4dU;
        packet[1] = 2U;
        packet[2] = operation;
        if (!Expect(queue.Enqueue(
                        packet.data(), packet.size(), operation) &&
                        queue.TryPeekLitchcraftFrameStore(),
                    "every schema-4 transfer operation shares ingress")) {
            return false;
        }
        uint32_t barrier = 0U;
        const bool expectsBarrier = operation == 18U || (operation >= 20U && operation <= 21U);
        if (!Expect(queue.TryPeekFirstLitchcraftBarrierOrdinal(barrier) ==
                        expectsBarrier,
                    "Begin checkpoint and commit preserve FIFO barriers before priority Cancel")) {
            return false;
        }
        if (!Expect(queue.TryAcquireReadSlot(
                        bytes, size, ordinal, epoch) &&
                        ordinal == operation,
                    "schema-4 operation remains byte-packed at queue head")) {
            return false;
        }
        queue.ReleaseReadSlot();
    }
    const std::array<uint8_t, 3U> ordinary{{1U, 2U, 3U}};
    return Expect(queue.Enqueue(ordinary.data(), ordinary.size(), 99U) &&
                      !queue.TryPeekLitchcraftFrameStore(),
                  "ordinary OTA bytes retain their existing owner");
}

} // namespace

int main() {
    if (!TestBeginBoundsAndImmutableSize() ||
        !TestChunkBoundsBeforeMutation() ||
        !TestStrictEndClaimsAndCrc() ||
        !TestLegacyGateStillRequiresCompleteReceive() ||
        !TestBackendFailuresCleanUp() ||
        !TestTimeoutWrapRetryAndRawBeforeBegin() ||
        !TestDisconnectAndShutdownCleanupAreIdempotent() ||
        !TestQueueEpochOverflowAndOrderedCompatibility() ||
        !TestRejectedBeginCandidatesFailClosed() ||
        !TestRejectedCandidateCleansUpForRetry() ||
        !TestQueueFullRejectedBeginSignalsSafeCleanup() ||
        !TestOversizedRejectedBeginSignalsSafeCleanup() ||
        !TestNegotiationBoundsMatchQueueCapacity() ||
        !TestClaimedQueueHeadStillCountsOnce() ||
        !TestBytePackedQueueWrapAndMaximumRecord() ||
        !TestSchema4UsesSharedPackedIngressAndOrderedBarriers() ||
        !TestMinimumMtuErrorPayloadsStayStable()) {
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
