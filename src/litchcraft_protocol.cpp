#include "modules/litchcraft_session.hpp"

#include "modules/litchcraft_persistence.hpp"
#include "modules/sha256.hpp"

#include <algorithm>
#include <cstring>
#include "foxie_embedded/millis.hpp"

namespace modules {
namespace {
void Write16(uint8_t* out, const uint16_t value) {
    out[0] = value;
    out[1] = value >> 8U;
}
void Write32(uint8_t* out, const uint32_t value) {
    for (uint8_t i = 0; i < 4U; ++i) {
        out[i] = value >> (i * 8U);
    }
}
} // namespace
uint16_t LitchcraftSession::Read16(const uint8_t* b) {
    return b[0] | (uint16_t(b[1]) << 8U);
}
uint32_t LitchcraftSession::Read32(const uint8_t* b) {
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8U) | (uint32_t(b[2]) << 16U) | (uint32_t(b[3]) << 24U);
}
LitchcraftSession::Packet LitchcraftSession::Header(const Event event,
                                                    const uint16_t request,
                                                    const uint32_t revision) const {
    Packet out;
    out.bytes[0] = MAGIC;
    out.bytes[1] = static_cast<uint8_t>(event);
    Write32(out.bytes.data() + 2U, m_session);
    Write32(out.bytes.data() + 6U, revision);
    Write16(out.bytes.data() + 10U, request);
    out.size = 12U;
    return out;
}
LitchcraftSession::Packet LitchcraftSession::StatePacket(const Event event,
                                                         const uint16_t request,
                                                         const Status status) const {
    auto out = Header(event, request, m_revision);
    out.bytes[12] = static_cast<uint8_t>(status);
    // Keep this legacy byte bounded to bits 0/1. Deployed apps reject State
    // packets with any other bit set; additive gesture state uses Event 0x88.
    out.bytes[13] = (IsActive() ? 1U : 0U) |
        (IsWaitingForLift() ? 2U : 0U);
    out.bytes[14] = m_hardware;
    out.bytes[15] = UndoCount();
    out.bytes[16] = RedoCount();
    out.bytes[17] = m_erase ? 1U : 0U;
    out.bytes[18] = m_target;
    out.bytes[19] = m_palette;
    out.size = 20U;
    return out;
}
LitchcraftSession::Packet LitchcraftSession::ToolPacket(const uint16_t request) const {
    auto out = Header(Event::Tool, request, m_revision);
    out.bytes[12] = m_color.r;
    out.bytes[13] = m_color.g;
    out.bytes[14] = m_color.b;
    out.bytes[15] = m_erase ? 1U : 0U;
    out.bytes[16] = m_target;
    out.size = 17U;
    return out;
}
LitchcraftSession::Packet LitchcraftSession::ColorPickPacket() const {
    auto out = Header(Event::ColorPick, 0U, m_revision);
    out.bytes[12] = m_pickPending ? 1U : (m_pickConfirmed ? 2U : 0U);
    out.size = 13U;
    return out;
}
LitchcraftSession::Packet LitchcraftSession::GesturePacket(
    const uint8_t action, const uint16_t token) const {
    const uint32_t authorityRevision = HasAdvancedDocument()
        ? m_advancedPersistence->AdvancedGeneration()
        : m_revision;
    auto out = Header(Event::Gesture, 0U, authorityRevision);
    out.bytes[12] = action;
    Write16(out.bytes.data() + 13U, token);
    out.size = 15U;
    return out;
}
void LitchcraftSession::PublishColorPickStatus() {
    Queue(ColorPickPacket());
}
void LitchcraftSession::BeginConnection() {
    // A replacement connection can supersede its queued disconnect event.
    // Retire staging and finish the old app stroke before accepting new IDs.
    EndConnection();
    m_connected = true;
    m_queueHead = 0U;
    m_priorityQueueOffset = 0xffU;
    m_resyncRequired = false;
    m_resyncSent = false;
    m_receiptCount = 0U;
    m_receiptNext = 0U;
    m_requestHighwater = 0U;
    m_bulkCommittedRequest = 0U;
    m_shakeDetector.Reset();
}
void LitchcraftSession::EndConnection() {
    if (m_controlFeedback != nullptr) m_controlFeedback->Reset();
    // Retire response IDs and transport buffers; artwork and its independent
    // persistence flush remain session-owned across a radio disconnect.
    m_connected = false;
    m_queueCount = 0U;
    m_priorityQueueOffset = 0xffU;
    m_snapshotActive = false;
    m_replacing = false;
    m_received.reset();
    m_bulkRequest = 0U;
    m_bulkNextOffset = 0U;
    m_paletteStagingCount = 0U;
    RetireV2Transfer();
    m_v2Subscribed = false;
    m_v2EffectClockSubscribed = false;
    m_v2PendingRequest = 0U;
    m_v2ObservedRevision = 0U;
    m_shakeDetector.Reset();
    // An interrupted touch stroke is retained as one complete undo operation.
    if (m_strokeSource == Source::App) {
        EndStroke();
    }
}
void LitchcraftSession::RequireResync() {
    RetireV2Transfer();
    m_v2PendingRequest = 0U;
    m_pickPending = false;
    m_pickConfirmed = false;
    m_resyncRequired = true;
    m_resyncSent = false;
    m_queueCount = 0U;
    m_priorityQueueOffset = 0xffU;
    m_snapshotActive = false;
}
void LitchcraftSession::Queue(const Packet& packet) {
    // Artwork repair must not hide the disposition of an accepted or refused
    // control. Only exact compact request receipts cross the resync embargo.
    const bool controlAck = packet.size == 20U && packet.bytes[0U] == MAGIC &&
        packet.bytes[1U] == static_cast<uint8_t>(Event::State) &&
        Read16(packet.bytes.data() + 10U) != 0U;
    if (!m_connected || (m_resyncRequired && !controlAck)) {
        return;
    }
    if (m_queueCount == QUEUE_CAPACITY) {
        RequireResync();
        if (!controlAck) return;
    }
    m_queue[(m_queueHead + m_queueCount) % QUEUE_CAPACITY] = packet;
    ++m_queueCount;
}
void LitchcraftSession::Ack(const uint16_t request, const Status status) {
    if ((HasAdvancedDocument() || HasStreamedDocument()) && request == 0U &&
        m_v2Subscribed) {
        Queue(V2State(0U, status));
        return;
    }
    if (m_snapshotActive && m_snapshotRequest == request && status == Status::Ok) {
        return;
    }
    Queue(StatePacket(Event::State, request, status));
    if (m_canvasInvalidated) {
        Queue(Header(Event::ResyncRequired, request, m_revision));
        m_canvasInvalidated = false;
    }
}
void LitchcraftSession::StartSnapshot(const uint16_t request, const bool bulk) {
    m_snapshot = m_canvas;
    m_snapshotRevision = m_revision;
    m_snapshotSession = m_session;
    m_snapshotRequest = request;
    m_snapshotCursor = 0U;
    m_snapshotPeekElements = 0U;
    m_snapshotBulk = bulk;
    m_snapshotActive = true;
    m_snapshotAck = true;
    m_snapshotState = StatePacket(Event::SnapshotBegin, request, Status::Ok);
    m_snapshotTool = ToolPacket(request);
    m_resyncRequired = false;
    m_resyncSent = false;
    // The snapshot replaces older canvas events; newer changes queue behind it.
    m_queueCount = 0U;
    m_queueHead = 0U;
    m_priorityQueueOffset = 0xffU;
}
bool LitchcraftSession::PeekPacket(Packet& out) {
    BulkPacket bulk;
    if (!PeekPacket(bulk, out.bytes.size())) {
        return false;
    }
    std::copy_n(bulk.bytes.begin(), bulk.size, out.bytes.begin());
    out.size = static_cast<uint8_t>(bulk.size);
    return true;
}
bool LitchcraftSession::PeekPacket(BulkPacket& out, const size_t maximumPayload) {
    if (!m_connected) {
        return false;
    }
    if (m_priorityQueueOffset == 0xffU) {
        const auto& head = m_queue[m_queueHead];
        const bool ordinaryCellHead =
            head.bytes[0] == 0x4cU &&
            head.bytes[1] == static_cast<uint8_t>(Event::Cell);
        // Compact authority may bypass per-cell projections, but not the
        // contiguous State/Timeline/Tool bundle produced by Inspect or other
        // semantic events whose order carries meaning.
        const size_t searchCount =
            (m_snapshotActive || m_v2SnapshotActive || ordinaryCellHead)
            ? m_queueCount : std::min<size_t>(m_queueCount, 1U);
        for (size_t offset = 0U; offset < searchCount; ++offset) {
            const auto& queued =
                m_queue[(m_queueHead + offset) % QUEUE_CAPACITY];
            const bool compactControlReply =
                queued.bytes[0] == 0x4cU &&
                (queued.bytes[1] == static_cast<uint8_t>(Event::State) ||
                 queued.bytes[1] == static_cast<uint8_t>(Event::Tool));
            const bool v2ControlReply =
                queued.bytes[0] == 0x4dU &&
                (queued.bytes[2] == 0x81U ||
                 queued.bytes[2] == 0x8dU ||
                 queued.bytes[2] == 0x88U ||
                 queued.bytes[2] == 0x89U ||
                 queued.bytes[2] == 0x8bU ||
                 queued.bytes[2] == 0x8cU);
            if (compactControlReply || v2ControlReply) {
                m_priorityQueueOffset = static_cast<uint8_t>(offset);
                break;
            }
        }
    }
    if (m_priorityQueueOffset != 0xffU) {
        const auto& queued = m_queue[
            (m_queueHead + m_priorityQueueOffset) % QUEUE_CAPACITY];
        std::copy_n(queued.bytes.begin(), queued.size, out.bytes.begin());
        out.size = queued.size;
        return true;
    }
    if (PeekV2Packet(out, maximumPayload)) return true;
    if (m_resyncRequired) {
        if (m_resyncSent) {
            return false;
        }
        auto required = Header(Event::ResyncRequired, 0U, m_revision);
        if (m_v2Subscribed) {
            required = V2State(0U, Status::Ok);
            required.bytes[2] = 0x87U;
            required.size = 13U;
        }
        std::copy_n(required.bytes.begin(), required.size, out.bytes.begin());
        out.size = required.size;
        return true;
    }
    if (m_snapshotActive) {
        if (m_snapshotAck) {
            std::copy_n(m_snapshotState.bytes.begin(), m_snapshotState.size, out.bytes.begin());
            out.size = m_snapshotState.size;
            out.bytes[1] = static_cast<uint8_t>(Event::State);
        } else if (m_snapshotCursor == 0U) {
            std::copy_n(m_snapshotState.bytes.begin(), m_snapshotState.size, out.bytes.begin());
            out.size = m_snapshotState.size;
        } else if (m_snapshotCursor == 1U) {
            std::copy_n(m_snapshotTool.bytes.begin(), m_snapshotTool.size, out.bytes.begin());
            out.size = m_snapshotTool.size;
        } else if (m_snapshotCursor < 258U) {
            const auto header = Header(Event::SnapshotChunk, m_snapshotRequest, m_snapshotRevision);
            std::copy_n(header.bytes.begin(), 14U, out.bytes.begin());
            const uint16_t offset = m_snapshotCursor - 2U;
            Write16(out.bytes.data() + 12U, offset);
            const size_t boundedPayload = m_snapshotBulk
                ? std::max<size_t>(20U, std::min(maximumPayload, out.bytes.size()))
                : 20U;
            const uint8_t count = static_cast<uint8_t>(std::min<size_t>(
                256U - offset, (boundedPayload - 14U) / 3U));
            m_snapshotPeekElements = count;
            for (uint8_t i = 0U; i < count; ++i) {
                const auto color = m_snapshot[offset + i];
                out.bytes[14U + i * 3U] = color.r;
                out.bytes[15U + i * 3U] = color.g;
                out.bytes[16U + i * 3U] = color.b;
            }
            out.size = 14U + size_t(count) * 3U;
        } else {
            const auto end = Header(Event::SnapshotEnd, m_snapshotRequest, m_snapshotRevision);
            std::copy_n(end.bytes.begin(), end.size, out.bytes.begin());
            out.size = end.size;
        }
        Write32(out.bytes.data() + 2U, m_snapshotSession);
        return true;
    }
    if (m_queueCount == 0U) {
        return false;
    }
    const auto& queued = m_queue[m_queueHead];
    std::copy_n(queued.bytes.begin(), queued.size, out.bytes.begin());
    out.size = queued.size;
    return true;
}
void LitchcraftSession::AcceptPacket() {
    if (m_priorityQueueOffset != 0xffU) {
        if (m_queueCount == 0U || m_priorityQueueOffset >= m_queueCount) {
            m_priorityQueueOffset = 0xffU;
            return;
        }
        const auto packet = m_queue[
            (m_queueHead + m_priorityQueueOffset) % QUEUE_CAPACITY];
        if (packet.bytes[0] == 0x4dU &&
            (packet.bytes[2] == 0x81U || packet.bytes[2] == 0x8dU) &&
            Read16(packet.bytes.data() + 11U) == m_v2PendingRequest) {
            m_v2PendingRequest = 0U;
        }
        for (size_t offset = m_priorityQueueOffset;
             offset + 1U < m_queueCount; ++offset) {
            m_queue[(m_queueHead + offset) % QUEUE_CAPACITY] =
                m_queue[(m_queueHead + offset + 1U) % QUEUE_CAPACITY];
        }
        --m_queueCount;
        m_priorityQueueOffset = 0xffU;
        return;
    }
    if (m_v2PacketPeeked) {
        AcceptV2Packet();
        return;
    }
    if (m_resyncRequired) {
        m_resyncSent = true;
        return;
    }
    if (m_snapshotActive) {
        if (m_snapshotAck) {
            m_snapshotAck = false;
        } else if (m_snapshotCursor < 2U) {
            ++m_snapshotCursor;
        } else if (m_snapshotCursor < 258U) {
            m_snapshotCursor = static_cast<uint16_t>(m_snapshotCursor + m_snapshotPeekElements);
        } else {
            m_snapshotActive = false;
            // Snapshot admission may have cleared an earlier optional event.
            // Republish the current phase after SnapshotEnd so the client has
            // already bound the authoritative session when it receives it.
            PublishColorPickStatus();
        }
    } else if (m_queueCount != 0U) {
        const auto& packet = m_queue[m_queueHead];
        if (packet.bytes[0] == 0x4dU &&
            (packet.bytes[2] == 0x81U || packet.bytes[2] == 0x8dU) &&
            Read16(packet.bytes.data() + 11U) == m_v2PendingRequest) {
            m_v2PendingRequest = 0U;
        }
        m_queueHead = (m_queueHead + 1U) % QUEUE_CAPACITY;
        --m_queueCount;
    }
}
std::array<uint8_t, 32U> LitchcraftSession::Receipt::Identity(
    const uint8_t* const bytes, const size_t size) {
    std::array<uint8_t, 32U> fingerprint{};
    if (size <= fingerprint.size()) {
        std::copy_n(bytes, size, fingerprint.begin());
        return fingerprint;
    }
    Sha256 digest;
    if (!digest.Update(bytes, size)) return fingerprint;
    return digest.Finish();
}
void LitchcraftSession::Receipt::Remember(
    const uint8_t* const bytes, const size_t length,
    const std::array<uint8_t, 32U>& fingerprint) {
    identity = fingerprint;
    size = static_cast<uint16_t>(length);
    advanced = bytes[0] == 0x4dU;
    request = Read16(bytes + (advanced ? 7U : 6U));
}
bool LitchcraftSession::Receipt::Matches(
    const size_t length, const std::array<uint8_t, 32U>& fingerprint) const {
    return size == length && identity == fingerprint;
}
void LitchcraftSession::Remember(const uint8_t* bytes, const size_t size, const Status status) {
    auto& receipt = m_receipts[m_receiptNext];
    receipt.Remember(bytes, size, Receipt::Identity(bytes, size));
    receipt.status = status;
    m_receiptNext = (m_receiptNext + 1U) % m_receipts.size();
    if (m_receiptCount < m_receipts.size()) {
        ++m_receiptCount;
    }
    const auto request = Read16(bytes + 6U);
    if (m_requestHighwater == 0U || static_cast<int16_t>(request - m_requestHighwater) > 0) {
        m_requestHighwater = request;
    }
}
void LitchcraftSession::HandlePacket(const uint8_t* bytes, const size_t size, const bool eligible, const uint64_t occupancy) {
    if (bytes != nullptr && size >= 2U && bytes[0] == 0x4dU) {
        (void)HandleV2Packet(bytes, size, eligible, occupancy);
        return;
    }
    (void)HandleCompactPacket(bytes, size, eligible, occupancy);
}

LitchcraftSession::Status LitchcraftSession::HandleCompactMode(
    const uint8_t* const bytes, const size_t size,
    const bool eligible, const uint64_t occupancy) {
    if (bytes == nullptr || size != 8U || bytes[0U] != MAGIC ||
        (bytes[1U] != uint8_t(Op::Enter) && bytes[1U] != uint8_t(Op::Exit))) {
        Ack(bytes != nullptr && size >= 8U ? Read16(bytes + 6U) : 0U, Status::Malformed);
        return Status::Malformed;
    }
    return HandleCompactPacket(bytes, size, eligible, occupancy);
}

LitchcraftSession::Status LitchcraftSession::HandleCompactPacket(
    const uint8_t* const bytes, const size_t size,
    const bool eligible, const uint64_t occupancy) {
    if (bytes == nullptr || size < 8U || size > 20U || bytes[0] != MAGIC) {
        Ack(0U, Status::Malformed);
        return Status::Malformed;
    }
    const uint16_t request = Read16(bytes + 6U);
    const uint32_t session = Read32(bytes + 2U);
    const Op op = static_cast<Op>(bytes[1]);
    if (request == 0U) {
        Ack(request, Status::Malformed);
        return Status::Malformed;
    }
    if (((op != Op::Enter && op != Op::Snapshot && op != Op::Inspect) || session != 0U) && session != m_session) {
        Ack(request, Status::StaleSession);
        return Status::StaleSession;
    }
    // Schema ownership also fences receipts created before the schema switch:
    // replaying an old legacy Snapshot may never expose a flattened document.
    if (StoreIsSoleArtworkOwner() && op != Op::Exit && op != Op::Enter &&
        op != Op::Tool && (op != Op::ShakeErase || !HasStreamedDocument())) {
        Ack(request, Status::Unsupported);
        return Status::Unsupported;
    }
    if ((HasAdvancedDocument() || HasStreamedDocument()) && op != Op::Exit &&
        op != Op::Enter && op != Op::ShakeErase && op != Op::Tool) {
        const auto status = op == Op::Inspect || op == Op::Snapshot
            ? Status::Unsupported : Status::Busy;
        Ack(request, status);
        return status;
    }
    for (uint8_t i = 0U; i < m_receiptCount; ++i) {
        const auto& receipt = m_receipts[i];
        if (receipt.request != request) {
            continue;
        }
        const bool same = !receipt.advanced && receipt.Matches(size, Receipt::Identity(bytes, size));
        if (same && op == Op::Snapshot && receipt.status == Status::Ok) {
            if (m_snapshotActive && m_snapshotRequest == request) {
                // Re-acknowledge a lost admission response without restarting
                // the frozen snapshot or discarding newer queued mutations.
                m_snapshotAck = true;
            } else {
                StartSnapshot(request,
                              size == 9U && bytes[8] == 1U);
            }
        }
        if (same && op == Op::Tool && receipt.status == Status::Ok) {
            // A successful retry is observational. Republish current shared
            // brush authority so a lost original Tool event cannot leave the
            // app behind a later physical Draw/Erase/color selection.
            Queue(ToolPacket(request));
        }
        Ack(request, same ? receipt.status : Status::Malformed);
        if (same && op == Op::Inspect && receipt.status == Status::Ok) {
            PublishColorPickStatus();
        }
        return same ? receipt.status : Status::Malformed;
    }
    if (m_requestHighwater != 0U && static_cast<int16_t>(request - m_requestHighwater) <= 0) {
        Ack(request, Status::StaleRequest);
        return Status::StaleRequest;
    }
    if (m_mutationsPaused && op != Op::Inspect && op != Op::Snapshot) {
        Ack(request, Status::Busy);
        return Status::Busy;
    }
    const auto status = Apply(op, bytes + 8U, size - 8U, request, eligible, occupancy);
    Remember(bytes, size, status);
    Ack(request, status);
    if (op == Op::Inspect && status == Status::Ok) {
        PublishColorPickStatus();
    }
    return status;
}

void LitchcraftSession::HandleBulkPacket(const uint8_t* bytes,
                                         const size_t size,
                                         const bool eligible,
                                         const uint64_t occupancy) {
    if (bytes != nullptr && size >= 9U && bytes[0] == 0x4dU &&
        (bytes[2] == 15U || bytes[2] == 16U || bytes[2] == 17U)) {
        HandleV2Packet(bytes, size, eligible, occupancy);
        return;
    }
    (void)eligible;
    (void)occupancy;
    // Header: magic, bulk opcode, session:u32, request:u16, element offset:u16,
    // final:u8, element count:u8, followed by count packed RGB triples.
    const uint16_t request = bytes != nullptr && size >= 8U
        ? Read16(bytes + 6U) : 0U;
    if (StoreIsSoleArtworkOwner()) {
        Ack(request, Status::Unsupported);
        return;
    }
    if (HasAdvancedDocument() || HasStreamedDocument()) {
        Ack(request, Status::Busy);
        return;
    }
    if (bytes == nullptr || size < 15U || bytes[0] != MAGIC ||
        bytes[1] != BULK_REPLACE || request == 0U) {
        Ack(request, Status::Malformed);
        return;
    }
    if (!m_active || Read32(bytes + 2U) != m_session) {
        Ack(request, Status::StaleSession);
        return;
    }
    if (m_mutationsPaused) {
        Ack(request, Status::Busy);
        return;
    }
    const uint16_t offset = Read16(bytes + 8U);
    const uint8_t final = bytes[10U];
    const uint8_t count = bytes[11U];
    if (final > 1U || count == 0U ||
        size != 12U + size_t(count) * 3U ||
        offset + count > ELEMENT_COUNT) {
        m_replacing = false;
        Ack(request, Status::Malformed);
        return;
    }
    if (offset == 0U) {
        if (request == m_bulkCommittedRequest) {
            Ack(request, Status::Ok);
            return;
        }
        if (m_requestHighwater != 0U &&
            static_cast<int16_t>(request - m_requestHighwater) <= 0) {
            Ack(request, Status::StaleRequest);
            return;
        }
        m_replacing = true;
        m_bulkRequest = request;
        m_bulkNextOffset = 0U;
    }
    if (!m_replacing || request != m_bulkRequest ||
        offset != m_bulkNextOffset) {
        m_replacing = false;
        Ack(request, Status::Incomplete);
        return;
    }
    for (uint16_t index = 0U; index < count; ++index) {
        const size_t source = 12U + size_t(index) * 3U;
        m_replacement[offset + index] = {
            bytes[source], bytes[source + 1U], bytes[source + 2U]};
    }
    m_bulkNextOffset = static_cast<uint16_t>(offset + count);
    m_replaceAtMs = fxe::Millis();
    if (final == 0U) {
        return;
    }
    if (m_bulkNextOffset != ELEMENT_COUNT) {
        m_replacing = false;
        Ack(request, Status::Incomplete);
        return;
    }
    EndStroke();
    StartStroke(Source::App);
    m_strokeChanged = ReplaceCanvas(m_replacement);
    EndStroke();
    ++m_revision;
    m_replacing = false;
    m_bulkCommittedRequest = request;
    m_requestHighwater = request;
    m_activity = true;
    Ack(request, Status::Ok);
}
} // namespace modules
