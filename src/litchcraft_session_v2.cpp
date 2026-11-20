#include "modules/litchcraft_session.hpp"

#include <algorithm>

#include "foxie_embedded/checksum.hpp"
#include "foxie_embedded/millis.hpp"
#include "modules/litchcraft_animation_codec.hpp"
#include "modules/litchcraft_persistence.hpp"

namespace modules {
namespace {
constexpr uint8_t V2_MAGIC = 0x4dU;
constexpr uint8_t VERSION = 2U;
constexpr uint32_t IDLE_MS = 6000U;
constexpr uint32_t ABSOLUTE_MS = 180000U;
constexpr uint8_t INSPECT = 1U;
constexpr uint8_t SNAPSHOT = 8U;
constexpr uint8_t REPLACE_BEGIN = 9U;
constexpr uint8_t REPLACE_CHUNK = 10U;
constexpr uint8_t REPLACE_COMMIT = 11U;
constexpr uint8_t CANCEL = 12U;
constexpr uint8_t CANVAS_EDIT = 13U;
constexpr uint8_t STREAM_BEGIN = 14U;
constexpr uint8_t STREAM_CHUNK = 15U;
constexpr uint8_t STREAM_COMMIT = 16U;
constexpr uint8_t STREAM_CHECKPOINT = 17U;
constexpr uint8_t STORE_STATE = 0x8dU;
constexpr uint8_t STORE_RUNTIME = 23U;
constexpr uint8_t OPTIMISTIC_PAINT = 24U;
constexpr uint8_t OPTIMISTIC_EFFECT = 25U;
constexpr uint8_t OPTIMISTIC_CONTROL = 26U;
constexpr uint8_t OPTIMISTIC_WATERMARK = 0x90U;
void Put16(uint8_t* const out, const uint16_t value) {
    out[0] = value;
    out[1] = value >> 8U;
}
void Put32(uint8_t* const out, const uint32_t value) {
    for (uint8_t i = 0U; i < 4U; ++i) out[i] = value >> (i * 8U);
}
bool HasUnsupportedEffect(const uint8_t* const bytes, const size_t size) {
    using namespace litchcraft_animation;
    // Begin already admitted an exact whole-frame canonical size. Scan only
    // enum slots; full range/header validation remains the codec's authority.
    for (size_t frame = ENCODED_HEADER_BYTES; frame < size; frame += ENCODED_FRAME_BYTES) {
        for (size_t emitter = 0U; emitter < EMITTER_COUNT; ++emitter) {
            if (!IsValidEffect(bytes[frame + 5U + emitter * ENCODED_EMITTER_BYTES])) return true;
        }
    }
    return false;
}
} // namespace

LitchcraftSession::PresentationAccess LitchcraftSession::PacketPresentationAccess(
    const uint8_t* const bytes, const size_t size) const {
    if (bytes == nullptr || size < 2U) return PresentationAccess::None;
    if (bytes[0U] == MAGIC) {
        switch (static_cast<Op>(bytes[1U])) {
        case Op::Enter: case Op::Exit: case Op::Tool: case Op::PaletteChunk:
        case Op::ReplaceCommit: return PresentationAccess::Control;
        case Op::Paint: case Op::Undo: case Op::Redo: case Op::Clear:
        case Op::ShakeErase: return FramePresentationAccess();
        default: return PresentationAccess::None;
        }
    }
    if (bytes[0U] != V2_MAGIC || size < 9U || bytes[1U] != VERSION)
        return PresentationAccess::None;
    // Transfer/metadata packets stay on their existing staging/storage owner.
    // In particular, conversion/JSON and FS work never hold this short lease.
    switch (bytes[2U]) {
    case 2U: case STORE_RUNTIME: return PresentationAccess::Control;
    case 3U: case 4U: case 5U: case 6U: case 7U: case CANVAS_EDIT:
    case OPTIMISTIC_PAINT: case OPTIMISTIC_EFFECT: case OPTIMISTIC_CONTROL:
        return FramePresentationAccess();
    default: return PresentationAccess::None;
    }
}

bool LitchcraftSession::PacketWaitsForPresentedFrame(
    const uint8_t* const bytes, const size_t size) const {
    return bytes != nullptr && size >= 9U && bytes[0U] == V2_MAGIC &&
        bytes[1U] == VERSION && (bytes[2U] == 3U || bytes[2U] == 4U) &&
        OrderedWaitsForPresentedFrame(bytes[2U], bytes + 9U, size - 9U);
}

LitchcraftSession::Packet LitchcraftSession::OptimisticWatermark(
    const Status status) const {
    Packet packet{};
    packet.bytes[0] = V2_MAGIC;
    packet.bytes[1] = VERSION;
    packet.bytes[2] = OPTIMISTIC_WATERMARK;
    Put32(packet.bytes.data() + 3U, m_session);
    Put32(packet.bytes.data() + 7U, m_advancedPersistence->AdvancedGeneration());
    Put16(packet.bytes.data() + 11U, m_optimisticHighestSequence);
    packet.bytes[13] = static_cast<uint8_t>(m_optimisticResync
        ? Status::StaleRequest : status);
    Put16(packet.bytes.data() + 14U, m_optimisticEpoch);
    std::array<uint8_t, 1538U> canonical{};
    const auto& frame = m_advancedPersistence->AdvancedDocument().Value()
        .frames[m_optimisticFrame];
    Put16(canonical.data(), frame.durationMs);
    for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        const auto& emitter = frame.emitters[index];
        const size_t offset = 2U + index * 6U;
        canonical[offset] = emitter.color.r;
        canonical[offset + 1U] = emitter.color.g;
        canonical[offset + 2U] = emitter.color.b;
        canonical[offset + 3U] = static_cast<uint8_t>(emitter.effect);
        Put16(canonical.data() + offset + 4U, emitter.durationMs);
    }
    Put32(packet.bytes.data() + 16U, fxe::Crc32(canonical.data(), canonical.size()));
    packet.size = 20U;
    return packet;
}

LitchcraftSession::Status LitchcraftSession::ApplyOptimisticControl(
    const uint16_t request, const uint8_t* const payload, const size_t size) {
    if (!HasAdvancedDocument() || payload == nullptr || size != 8U ||
        payload[0] > 2U || payload[1] == 0U ||
        payload[7] >= m_advancedPersistence->AdvancedDocument().Value().frameCount) {
        return Status::Malformed;
    }
    const uint8_t action = payload[0];
    const uint16_t epoch = Read16(payload + 1U);
    const uint8_t frame = payload[7];
    if (action == 0U) {
        if (Read32(payload + 3U) != m_advancedPersistence->AdvancedGeneration()) {
            return Status::RevisionConflict;
        }
        EndStroke();
        m_optimisticEpoch = epoch;
        m_optimisticHighestSequence = 0U;
        m_optimisticFrame = frame;
        m_optimisticAcceptedSinceWatermark = 0U;
        m_optimisticResync = false;
        m_optimisticActive = true;
    } else if (!m_optimisticActive || epoch != m_optimisticEpoch ||
               frame != m_optimisticFrame) {
        return Status::StaleRequest;
    } else if (action == 2U) {
        EndStroke();
        m_optimisticActive = false;
    }
    (void)request;
    return Status::Ok;
}

void LitchcraftSession::AttachAdvancedPersistence(
    LitchcraftPersistence* const persistence) {
    m_advancedPersistence = persistence;
}

LitchcraftSession::Packet LitchcraftSession::V2State(
    const uint16_t request, const Status status) const {
    // Unsolicited state and timeline headers describe the current authority.
    return V2State(request, status, HasStreamedDocument() ? STORE_RUNTIME : INSPECT);
}

LitchcraftSession::Packet LitchcraftSession::V2State(
    const uint16_t request, const Status status, const uint8_t operation) const {
    Packet packet{};
    packet.bytes[0] = V2_MAGIC;
    packet.bytes[1] = VERSION;
    const bool streamed = HasStreamedDocument();
    if (operation == STORE_RUNTIME) {
        packet.bytes[2] = STORE_STATE;
        Put32(packet.bytes.data() + 3U, m_session);
        Put32(packet.bytes.data() + 7U, streamed ? m_frameStream->Generation() : 0U);
        Put16(packet.bytes.data() + 11U, request);
        packet.bytes[13] = static_cast<uint8_t>(status);
        packet.bytes[14] = (IsActive() ? 1U : 0U) |
            (streamed && m_advancedPlaying ? 2U : 0U) | (streamed ? 4U : 0U) |
            static_cast<uint8_t>((AnimationTopology() - 1U) << 3U);
        Put16(packet.bytes.data() + 15U, streamed ? m_frameStream->FrameCount() : 0U);
        Put16(packet.bytes.data() + 17U, streamed ? m_advancedSelectedFrame : 0U);
        packet.size = 19U;
        return packet;
    }
    packet.bytes[2] = 0x81U;
    Put32(packet.bytes.data() + 3U, m_session);
    Put32(packet.bytes.data() + 7U, m_advancedPersistence == nullptr ? 0U :
          m_advancedPersistence->AdvancedGeneration());
    Put16(packet.bytes.data() + 11U, request);
    packet.bytes[13] = static_cast<uint8_t>(status);
    // A streamed frame index cannot describe the hidden schema-3 draft.
    const bool hasDocument = !streamed && m_advancedPersistence != nullptr &&
        m_advancedPersistence->HasAdvancedDocument();
    const bool hasHistory = hasDocument && m_advancedRuntime;
    packet.bytes[14] = (IsActive() ? 1U : 0U) |
        (hasDocument && m_advancedPlaying ? 2U : 0U) |
        (hasHistory && UndoCount() != 0U ? 4U : 0U) |
        (hasHistory && RedoCount() != 0U ? 8U : 0U);
    packet.bytes[15] = hasDocument
        ? m_advancedPersistence->AdvancedDocument().Value().frameCount : 0U;
    packet.bytes[16] = hasDocument ? m_advancedSelectedFrame : 0U;
    packet.bytes[17] = hasDocument ? litchcraft_animation::SCHEMA : 0U;
    packet.size = 18U;
    return packet;
}

void LitchcraftSession::RetireV2Transfer() {
    if (m_v2EnvelopeLeased && m_advancedPersistence != nullptr) {
        m_advancedPersistence->ReleaseAdvancedTransportLease();
    }
    m_v2EnvelopeLeased = false;
    m_v2StreamedReplace = false;
    m_v2Transfer = 0U;
    m_v2SnapshotActive = false;
    m_v2SnapshotSessionZero = false;
    m_v2Cursor = 0U;
    m_v2PacketPeeked = false;
}

void LitchcraftSession::UpdateV2Transport() {
    const uint32_t now = fxe::Millis();
    const bool sessionEnded = m_v2Session != 0U && m_v2Session != m_session;
    const bool activeTransferEnded =
        m_v2Transfer != 0U && m_v2Session != 0U && !m_active;
    if (m_v2EnvelopeLeased &&
        (now - m_v2ProgressAtMs >= IDLE_MS ||
         now - m_v2StartedAtMs >= ABSOLUTE_MS ||
         sessionEnded || activeTransferEnded)) {
        RetireV2Transfer();
    }
}

bool LitchcraftSession::HandleV2Packet(const uint8_t* const bytes,
                                       const size_t size,
                                       const bool eligible,
                                       const uint64_t occupancy) {
    (void)eligible;
    (void)occupancy;
    if (!m_connected || bytes == nullptr || size < 9U ||
        size > BulkPacket{}.bytes.size()) return true;
    const uint8_t op = bytes[2];
    const uint32_t session = Read32(bytes + 3U);
    const uint16_t request = Read16(bytes + 7U);
    const auto identity = op == STREAM_CHUNK
        ? std::array<uint8_t, 32U>{} : Receipt::Identity(bytes, size);
    const auto refuse = [&](const Status status) { Queue(V2State(request, status, op)); };
    // Failed hashing cannot match a receipt or consume an ID through mutation.
    if (op != STREAM_CHUNK && size > identity.size() &&
        std::all_of(identity.begin(), identity.end(),
                    [](const uint8_t byte) { return byte == 0U; })) {
        refuse(Status::Busy);
        return true;
    }
    UpdateV2Transport();
    if (HasStreamedDocument()) SampleStreamedPlayback();
    else SampleAdvancedPlayback();
    if (op == STREAM_CHUNK) {
        // The ordered bulk lane stages bytes only. Missing/reordered/invalid
        // bytes cannot publish or create notification pressure: Commit is the
        // sole length/CRC/revision authority, and the ordinary lease expires.
        if (bytes[1] != VERSION || size < 14U || !m_v2StreamedReplace ||
            m_advancedPersistence == nullptr || session != m_v2Session ||
            Read16(bytes + 9U) != m_v2Transfer || request == 0U) return true;
        auto* const storage = m_advancedPersistence->AdvancedTransportStorage();
        const auto offset = Read16(bytes + 11U);
        const size_t count = size - 13U;
        if (storage == nullptr || offset != m_v2Cursor ||
            offset + count > m_v2Length) return true;
        std::copy_n(bytes + 13U, count, storage->envelope.bytes.begin() + offset);
        m_v2Cursor = static_cast<uint16_t>(offset + count);
        m_v2ProgressAtMs = fxe::Millis();
        return true;
    }
    if (bytes[1] != VERSION) { refuse(Status::Unsupported); return true; }
    if (request == 0U) { refuse(Status::Malformed); return true; }
    m_resyncRequired = m_resyncSent = false;
    const bool metadata = op == INSPECT || op == SNAPSHOT;
    // A completed cancellation no longer owns a live lease. Its
    // exact same-epoch receipt can still acknowledge a lost reply; it must not
    // re-run cancellation against any snapshot started in the meantime.
    if (op == CANCEL && (session == 0U || session == m_session) && size == 11U) {
        for (uint8_t i = 0U; i < m_receiptCount; ++i) {
            const auto& receipt = m_receipts[i];
            if (receipt.status == Status::Ok && receipt.Matches(size, identity) &&
                receipt.response.bytes[0] == V2_MAGIC &&
                Read32(receipt.response.bytes.data() + 3U) == m_session) {
                Queue(receipt.response);
                return true;
            }
        }
    }
    const bool ownsOutgoingSnapshot = op == CANCEL && size == 11U &&
        m_v2SnapshotActive &&
        (session == m_v2Session || (session == 0U && m_v2SnapshotSessionZero)) &&
        Read16(bytes + 9U) == m_v2Request;
    const bool ownsInactiveStorageTransfer = op == CANCEL && size == 11U &&
        !m_active && session == 0U && m_v2Session == 0U &&
        m_v2Transfer != 0U && Read16(bytes + 9U) == m_v2Transfer;
    const bool inactiveStorage = !m_active && session == 0U &&
        (op == REPLACE_BEGIN || op == STREAM_BEGIN || op == REPLACE_CHUNK ||
         op == REPLACE_COMMIT || op == STREAM_COMMIT || op == STREAM_CHECKPOINT ||
         ownsInactiveStorageTransfer);
    if ((!metadata && !ownsOutgoingSnapshot && !inactiveStorage &&
         (!m_active || session != m_session)) ||
        (metadata && session != 0U && session != m_session)) {
        refuse(Status::StaleSession); return true;
    }
    if (op == OPTIMISTIC_PAINT || op == OPTIMISTIC_EFFECT) {
        // Streaming request IDs are strictly the per-epoch sequence, not the
        // ordinary receipt ID space. They deliberately produce no per-edit
        // State: admitted RAM/session edits render before any watermark.
        if (!HasAdvancedDocument()) {
            refuse(Status::Unsupported); return true;
        }
        if (!m_optimisticActive || m_optimisticResync) {
            Queue(OptimisticWatermark(Status::StaleRequest)); return true;
        }
        if (request <= m_optimisticHighestSequence) return true;
        if (request != static_cast<uint16_t>(m_optimisticHighestSequence + 1U)) {
            m_optimisticResync = true;
            Queue(OptimisticWatermark(Status::StaleRequest)); return true;
        }
        const Status status = ApplyOptimisticEdit(op, request, bytes + 9U,
                                                   size - 9U);
        if (status != Status::Ok) {
            m_optimisticResync = true;
            Queue(OptimisticWatermark(status)); return true;
        }
        const uint8_t flags = bytes[9U + (op == OPTIMISTIC_PAINT ? 3U : 7U)];
        if (m_optimisticAcceptedSinceWatermark >= 8U || (flags & 2U) != 0U) {
            m_optimisticAcceptedSinceWatermark = 0U;
            Queue(OptimisticWatermark(Status::Ok));
        }
        return true;
    }
    for (uint8_t i = 0U; i < m_receiptCount; ++i) {
        const auto& receipt = m_receipts[i];
        if (receipt.request != request) continue;
        const bool same = receipt.advanced && receipt.Matches(size, identity);
        if (same) Queue(receipt.response);
        else refuse(Status::Malformed);
        return true;
    }
    if (m_requestHighwater != 0U &&
        static_cast<int16_t>(request - m_requestHighwater) <= 0) {
        refuse(Status::StaleRequest); return true;
    }
    // Refusals leave the ID available for retry after the preceding ACK enters
    // the notify lane. Neither a refusal nor replay extends transfer progress.
    if (m_v2PendingRequest != 0U) { refuse(Status::Busy); return true; }
    const uint32_t revisionBefore = m_advancedPersistence == nullptr
        ? 0U : m_advancedPersistence->AdvancedGeneration();
    const auto finish = [&](const Status status) {
        if (status == Status::Ok && m_advancedPersistence != nullptr &&
            m_advancedPersistence->AdvancedGeneration() != revisionBefore) {
            ++m_lastAdvancedMutation.count;
            m_lastAdvancedMutation.revision = m_advancedPersistence->AdvancedGeneration();
            m_lastAdvancedMutation.op = op;
            m_lastAdvancedMutation.detail = op == 5U ? bytes[13U] : 0U;
            m_lastAdvancedMutation.frames =
                m_advancedPersistence->AdvancedDocument().Value().frameCount;
        }
        const Packet response = V2State(request, status, op);
        auto& receipt = m_receipts[m_receiptNext];
        receipt.Remember(bytes, size, identity);
        receipt.status = status;
        receipt.response = response;
        m_receiptNext = (m_receiptNext + 1U) % m_receipts.size();
        m_receiptCount = std::min<size_t>(m_receiptCount + 1U, m_receipts.size());
        m_requestHighwater = request;
        m_v2PendingRequest = request;
        Queue(response);
    };
    const bool retiredTransfer = op == SNAPSHOT ||
        (op >= REPLACE_BEGIN && op <= CANCEL) ||
        (op >= STREAM_BEGIN && op <= STREAM_CHECKPOINT);
    if (retiredTransfer &&
        (m_advancedPersistence == nullptr ||
         !m_advancedPersistence->IsAdvancedConfigured())) {
        // A recovered Store is not an advanced-document staging buffer.
        // Refuse old transfers before their handlers inspect that buffer.
        finish(Status::Unsupported); return true;
    }
    // Mode metadata remains available before the first Store root exists.
    // It neither reads nor allocates the retired schema-3 staging buffer.
    if (op != INSPECT && !HasStreamedDocument() &&
        (m_advancedPersistence == nullptr ||
         !m_advancedPersistence->IsAdvancedConfigured())) {
        finish(Status::Unsupported); return true;
    }
    m_v2Subscribed = true;
    if (op == CANVAS_EDIT &&
        (size == 9U || (size == 10U && (bytes[9] == 2U || bytes[9] == 3U)))) {
        // Capability probe uses the unchanged minimum-MTU State envelope.
        finish(Status::Ok); return true;
    }
    if (!metadata && op != CANCEL && m_mutationsPaused) {
        finish(Status::Busy); return true;
    }
    const uint32_t now = fxe::Millis();
    if (op == CANVAS_EDIT) {
        const auto status = ApplyV2CanvasEdit(bytes + 9U, size - 9U);
        finish(status);
        if (status == Status::Ok && bytes[18U] == 0U) {
            // A held swatch selects Paint and fills in one command. Publish
            // session tool readback after the V2 receipt without a second
            // document/history mutation or repainting a resting brush.
            SelectTool(false, bytes[14U], {bytes[15U], bytes[16U], bytes[17U]},
                       0U, false);
        }
        return true;
    }
    if (op >= 2U && op <= 7U) {
        const auto status = ApplyV2Edit(op, bytes + 9U, size - 9U);
        finish(status);
        if (op == 2U && status == Status::Ok) QueueTimeline(request);
        return true;
    }
    if (op == STORE_RUNTIME) {
        const auto status = ApplyStoreRuntime(bytes + 9U, size - 9U);
        finish(status);
        if (status == Status::Ok) QueueTimeline(request);
        return true;
    }
    if (op == OPTIMISTIC_CONTROL) {
        const auto status = ApplyOptimisticControl(request, bytes + 9U, size - 9U);
        finish(status);
        if (status == Status::Ok) {
            m_optimisticAcceptedSinceWatermark = 0U;
            Queue(OptimisticWatermark(Status::Ok));
        }
        return true;
    }
    if (op == INSPECT || op == SNAPSHOT) {
        if (op == INSPECT && size == 10U && (bytes[9] == 2U || bytes[9] == 3U)) {
            finish(Status::Ok); return true;
        }
        const bool requestEffectClock =
            op == INSPECT && size == 10U && bytes[9] == 1U;
        if (size != 9U && !requestEffectClock) { finish(Status::Malformed); return true; }
        if (requestEffectClock) m_v2EffectClockSubscribed = true;
        if (op == INSPECT) {
            finish(Status::Ok);
            QueueTimeline(request);
            // V2 State carries document metadata, not the shared Paint/Erase
            // tool. Entry/reconnect must recover it without a new selection.
            Queue(ToolPacket(0U));
            return true;
        }
        if (HasStreamedDocument()) {
            // Store transfer intentionally has no snapshot/readback opcode.
            // A fabricated legacy envelope would create a second document
            // authority and truncate a 256-frame root.
            finish(Status::Unsupported); return true;
        }
        if (!m_advancedPersistence->HasAdvancedDocument() || m_v2EnvelopeLeased) {
            finish(Status::Busy); return true;
        }
        auto* const storage = m_advancedPersistence->AcquireAdvancedTransportLease();
        if (storage == nullptr) { finish(Status::Busy); return true; }
        size_t length = 0U;
        if (!litchcraft_animation::EncodeDocumentToBuffer(storage->document,
                storage->envelope.bytes.data(), storage->envelope.bytes.size(), length, m_v2Crc)) {
            m_advancedPersistence->ReleaseAdvancedTransportLease();
            finish(Status::IntegrityFailure); return true;
        }
        m_v2EnvelopeLeased = true;
        m_v2SnapshotActive = true;
        m_v2SnapshotSessionZero = session == 0U;
        m_v2Request = request;
        m_v2Cursor = 0U;
        m_v2Length = static_cast<uint16_t>(length);
        m_v2Revision = m_advancedPersistence->AdvancedGeneration();
        m_v2Session = m_session;
        m_v2StartedAtMs = m_v2ProgressAtMs = now;
        m_v2ObservedRevision = m_v2Revision;
        m_snapshotActive = false;
        m_resyncRequired = m_resyncSent = false;
        finish(Status::Ok); return true;
    }
    if (op == REPLACE_BEGIN || op == STREAM_BEGIN) {
        if (size != 19U) { finish(Status::Malformed); return true; }
        const uint32_t base = Read32(bytes + 9U);
        const uint16_t length = Read16(bytes + 13U);
        if (length < litchcraft_animation::ENCODED_HEADER_BYTES + litchcraft_animation::ENCODED_FRAME_BYTES ||
            length > litchcraft_animation::MAX_ENCODED_BYTES ||
            (length - litchcraft_animation::ENCODED_HEADER_BYTES) % litchcraft_animation::ENCODED_FRAME_BYTES != 0U) {
            finish(Status::Malformed); return true;
        }
        if (m_v2EnvelopeLeased) { finish(Status::Busy); return true; }
        if (base != m_advancedPersistence->AdvancedGeneration()) { finish(Status::RevisionConflict); return true; }
        if (m_advancedPersistence->AcquireAdvancedTransportLease() == nullptr) { finish(Status::Busy); return true; }
        m_v2EnvelopeLeased = true;
        m_v2StreamedReplace = op == STREAM_BEGIN;
        m_v2Transfer = request;
        // Zero is explicit storage authority, not the retained last mode ID.
        m_v2Session = session;
        m_v2BaseRevision = base;
        m_v2Length = length;
        m_v2Crc = Read32(bytes + 15U);
        m_v2Cursor = 0U;
        m_v2StartedAtMs = m_v2ProgressAtMs = now;
        finish(Status::Ok); return true;
    }
    if (op == REPLACE_CHUNK) {
        if (size < 14U) { finish(Status::Malformed); return true; }
        const uint16_t transfer = Read16(bytes + 9U);
        const uint16_t offset = Read16(bytes + 11U);
        const size_t count = size - 13U;
        auto* const storage = m_advancedPersistence->AdvancedTransportStorage();
        if (storage == nullptr || m_v2Transfer == 0U || transfer != m_v2Transfer ||
            offset != m_v2Cursor || offset + count > m_v2Length) {
            finish(Status::Incomplete); return true;
        }
        std::copy_n(bytes + 13U, count, storage->envelope.bytes.begin() + offset);
        m_v2Cursor = static_cast<uint16_t>(offset + count);
        m_v2ProgressAtMs = now;
        finish(Status::Ok); return true;
    }
    if (op == STREAM_CHECKPOINT) {
        if (size != 13U) { finish(Status::Malformed); return true; }
        const auto* storage = m_advancedPersistence->AdvancedTransportStorage();
        if (!m_v2StreamedReplace || storage == nullptr ||
            Read16(bytes + 9U) != m_v2Transfer || m_v2Transfer == 0U ||
            Read16(bytes + 11U) != m_v2Cursor) {
            finish(Status::Incomplete); return true;
        }
        // This request shares the data FIFO. Its exact receipt grants the next
        // bounded window only after every preceding byte has reached staging.
        if (m_v2BaseRevision != m_advancedPersistence->AdvancedGeneration()) {
            finish(Status::RevisionConflict); return true;
        }
        finish(Status::Ok); return true;
    }
    if (op == REPLACE_COMMIT || op == STREAM_COMMIT || op == CANCEL) {
        if (size != 11U) { finish(Status::Malformed); return true; }
        const uint16_t transfer = Read16(bytes + 9U);
        if (transfer == 0U || (transfer != m_v2Transfer &&
            !(op == CANCEL && m_v2SnapshotActive && transfer == m_v2Request))) {
            finish(Status::Incomplete); return true;
        }
        if (op == CANCEL) { RetireV2Transfer(); finish(Status::Ok); return true; }
        if ((op == STREAM_COMMIT) != m_v2StreamedReplace) {
            finish(Status::Malformed); return true;
        }
        if (m_strokeSource != Source::None) { finish(Status::Busy); return true; }
        Status status = Status::Ok;
        auto* const storage = m_advancedPersistence->AdvancedTransportStorage();
        if (m_v2Cursor != m_v2Length || storage == nullptr) status = Status::Incomplete;
        else if (m_v2BaseRevision != m_advancedPersistence->AdvancedGeneration()) status = Status::RevisionConflict;
        else if (fxe::Crc32(storage->envelope.bytes.data(), m_v2Length) != m_v2Crc) status = Status::IntegrityFailure;
        else if (storage->envelope.bytes[0] != litchcraft_animation::SCHEMA) status = Status::Unsupported;
        else if (HasUnsupportedEffect(storage->envelope.bytes.data(), m_v2Length)) status = Status::Unsupported;
        else if (!m_advancedPersistence->AdoptAdvancedTransportDocument(
                     m_v2Length, m_v2Crc, m_v2Session != 0U)) {
            status = Status::IntegrityFailure;
        } else if (m_v2Session == 0U) {
            m_advancedPersistence->RequestFlush();
        }
        RetireV2Transfer();
        finish(status); return true;
    }
    finish(Status::Unsupported);
    return true;
}

bool LitchcraftSession::PeekV2Packet(BulkPacket& out, const size_t maximumPayload) {
    m_v2PacketPeeked = false;
    UpdateV2Transport();
    if (maximumPayload < 20U || m_queueCount != 0U || !m_v2Subscribed ||
        m_advancedPersistence == nullptr) return false;
    out = {};
    out.bytes[0] = V2_MAGIC;
    out.bytes[1] = VERSION;
    const auto header = [&](const uint8_t event, const uint32_t revision, const uint16_t request) {
        out.bytes[2] = event;
        Put32(out.bytes.data() + 3U, m_v2SnapshotActive ? m_v2Session : m_session);
        Put32(out.bytes.data() + 7U, revision);
        Put16(out.bytes.data() + 11U, request);
    };
    if (!m_v2SnapshotActive) {
        const auto revision = m_advancedPersistence->AdvancedGeneration();
        if (revision == m_v2ObservedRevision) return false;
        header(0x87U, revision, 0U);
        m_v2Revision = revision;
        out.size = 13U;
    } else {
        const auto* const storage = m_advancedPersistence->AdvancedTransportStorage();
        if (storage == nullptr) return false;
        if (m_v2Cursor == 0U) {
            header(0x84U, m_v2Revision, m_v2Request);
            out.bytes[13] = litchcraft_animation::SCHEMA;
            Put16(out.bytes.data() + 14U, m_v2Length);
            Put32(out.bytes.data() + 16U, m_v2Crc);
            out.size = 20U;
        } else if (m_v2Cursor <= m_v2Length) {
            header(0x85U, m_v2Revision, m_v2Request);
            const uint16_t offset = static_cast<uint16_t>(m_v2Cursor - 1U);
            const size_t count = std::min<size_t>(m_v2Length - offset,
                std::min(maximumPayload, out.bytes.size()) - 15U);
            Put16(out.bytes.data() + 13U, offset);
            std::copy_n(storage->envelope.bytes.begin() + offset, count, out.bytes.begin() + 15U);
            out.size = static_cast<uint16_t>(15U + count);
            m_v2ChunkBytes = static_cast<uint16_t>(count);
        } else { header(0x86U, m_v2Revision, m_v2Request); out.size = 13U; }
    }
    m_v2PacketPeeked = true;
    return true;
}

void LitchcraftSession::AcceptV2Packet() {
    m_v2PacketPeeked = false;
    if (!m_v2SnapshotActive) { m_v2ObservedRevision = m_v2Revision; return; }
    if (m_v2Cursor == 0U) m_v2Cursor = 1U;
    else if (m_v2Cursor <= m_v2Length) {
        m_v2Cursor = static_cast<uint16_t>(m_v2Cursor + m_v2ChunkBytes);
        m_v2ProgressAtMs = fxe::Millis();
    } else RetireV2Transfer();
}
} // namespace modules
