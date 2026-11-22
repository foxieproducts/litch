#include "modules/litchcraft_frame_store_receiver.hpp"

#include <algorithm>
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "modules/connection_session.hpp"
#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_store {
namespace {
constexpr uint8_t MAGIC = 0x4dU;
constexpr uint8_t VERSION = 2U;
constexpr uint8_t INSPECT = 1U;
constexpr size_t HEADER_BYTES = 9U;
constexpr size_t BEGIN_BYTES = 53U;
constexpr size_t CURSOR_BYTES = 13U;
// The shared session owns packet sizing. Leave room for its envelope and the
// current reverse Object begin header so a read receipt takes one Object slot.
constexpr size_t SNAPSHOT_RESPONSE_BYTES =
    ConnectionSession::REVERSE_OBJECT_PACKET_BYTES -
    connection_session_codec::HEADER_BYTES - 4U;
static_assert(SNAPSHOT_RESPONSE_BYTES > 60U &&
              SNAPSHOT_RESPONSE_BYTES <= STORE_RESPONSE_MAX_BYTES);

void Put16(uint8_t* const out, const uint16_t value) {
    fxe::WriteU16LE(value, out);
}

void Put32(uint8_t* const out, const uint32_t value) {
    fxe::WriteU32LE(value, out);
}

bool ValidProjection(const StoreProjection& projection) {
    return projection.topology >= 1U && projection.topology <= 3U &&
           (!projection.playing || projection.active);
}

} // namespace

void StoreReceiver::SetSnapshotPacketMaximum(const uint16_t packetMaximum) {
    if (packetMaximum < connection_session_codec::MIN_PACKET_BYTES ||
        packetMaximum > ConnectionSession::REVERSE_OBJECT_PACKET_BYTES) {
        return;
    }
    // A minimum-MTU link needs small complete receipts so its six-second
    // application deadline does not span dozens of reverse fragments.
    m_snapshotResponseMaximum = static_cast<uint16_t>(std::min<size_t>(
        SNAPSHOT_RESPONSE_BYTES,
        std::max<size_t>(100U, packetMaximum -
            connection_session_codec::HEADER_BYTES - 4U)));
}

Status StoreReceiver::Recover(StoreProjection projection,
                              const FrameStore::RecoveryPurpose purpose) {
    if (!ValidProjection(projection)) return Status::InvalidArgument;
    Retire(false);
    RetireRead();
    m_projection = projection;
    const Status status = m_store.Recover(purpose);
    if (status == Status::Ok) {
        const Manifest& manifest = m_store.CurrentManifest();
        m_projection.topology = manifest.topology;
        m_projection.active = manifest.active;
        m_projection.playing = false;
        m_projection.selectedFrame = 0U;
    }
    return status;
}

Status StoreReceiver::RecoverOrCreateBlank(const StoreProjection projection,
    const FrameStore::RecoveryPurpose purpose) {
    const Status recovered = Recover(projection, purpose);
    if (recovered != Status::NoArtwork) return recovered;
    // An empty board has no mode authority to persist until someone enters
    // Litchcraft. Boot recovery must not publish an inactive root over entry.
    if (projection.session == 0U || !projection.active) return recovered;

    Manifest blank{};
    blank.topology = projection.topology;
    blank.active = projection.active;
    TransactionToken token{};
    const Status begun = m_store.Begin(
        blank, m_store.Generation(), UINT32_MAX, token);
    if (begun != Status::Ok) return begun;
    const Status written = m_store.WriteBlankFirstFrame(token);
    const Status committed = written == Status::Ok
        ? m_store.Commit(token) : written;
    if (committed != Status::Ok) {
        (void)m_store.Abort(token);
        return committed;
    }
    return Recover(projection, purpose);
}

void StoreReceiver::Unavailable(const uint16_t request,
                                StoreIngressResult& result) const {
    result.Reset();
    return State(request, WireStatus::ResourceLimit, result);
}

void StoreReceiver::SetProjection(const StoreProjection projection) {
    if (!ValidProjection(projection)) return;
    if (projection.session != m_projection.session ||
        projection.topology != m_projection.topology ||
        projection.active != m_projection.active ||
        (m_liveCaptured && projection.playing != m_projection.playing)) RetireRead();
    if (m_transfer != 0U && !m_commitAccepted &&
        (projection.session != m_projection.session ||
         (m_projection.active && !projection.active))) {
        Retire(true);
        m_receipt = {};
    }
    m_projection = projection;
    if (m_store.HasArtwork() &&
        m_projection.selectedFrame >= m_store.CurrentManifest().frameCount) {
        m_projection.selectedFrame = 0U;
    }
}

void StoreReceiver::RejectResidentPresentation(
    const StoreProjection previous, const uint16_t request,
    const WireStatus status, StoreIngressResult& result) {
    result.Reset();
    SetProjection(previous);
    return Residents(request, status, result);
}

void StoreReceiver::EndOwner(const uint32_t ownerEpoch) {
    if (m_readOwnerEpoch == ownerEpoch) RetireRead();
    if (m_transfer != 0U && !m_commitAccepted &&
        m_ownerEpoch == ownerEpoch) Retire(true);
}

void StoreReceiver::RetireRead() {
    m_liveCaptured = false;
    m_liveRead = {};
    m_readGeneration = 0U;
    m_readSession = 0U;
    m_readOwnerEpoch = 0U;
    m_readCursor = 0U;
    m_readRequest = 0U;
    m_readFrameIndex = FRAME_COUNT;
}

void StoreReceiver::ContinueRead(const uint32_t ownerEpoch,
                                 StoreIngressResult& result) {
    result.Reset();
    if (!ReadActive()) return;
    if (ownerEpoch == 0U || ownerEpoch != m_readOwnerEpoch ||
        m_readSession != m_projection.session) {
        RetireRead();
        return;
    }
    if (m_liveStream != nullptr && m_liveStream->HasPendingBlankDocument()) {
        State(m_readRequest, WireStatus::Busy, result);
        RetireRead();
        return;
    }
    if (m_liveCaptured) {
        LiveSnapshotChunk(m_readRequest, m_readCursor, result);
        AdvanceRead(result, 78U);
    } else {
        SnapshotChunk(m_readRequest, m_readGeneration, m_readCursor,
                      m_readFrameIndex, result);
        AdvanceRead(result, 60U);
    }
}

void StoreReceiver::AdvanceRead(const StoreIngressResult& result,
                                const size_t headerBytes) {
    if (!result.responseReady || result.responseSize <= headerBytes ||
        result.response[13U] != static_cast<uint8_t>(WireStatus::Ok)) {
        RetireRead();
        return;
    }
    const uint32_t next = m_readCursor + result.responseSize - headerBytes;
    const uint32_t total = fxe::ReadU32LE(result.response.data() + 20U);
    if (next < total) m_readCursor = next;
    else if (m_liveCaptured) m_readRequest = 0U;
    else RetireRead();
}

WireStatus StoreReceiver::MapStatus(const Status status) const {
    switch (status) {
        case Status::Ok: return WireStatus::Ok;
        case Status::NotConfigured: return WireStatus::Unsupported;
        case Status::PrelaunchLC5: return WireStatus::Unsupported;
        case Status::NoArtwork: return WireStatus::NoHistory;
        case Status::InvalidArgument: return WireStatus::Malformed;
        case Status::RevisionConflict: return WireStatus::RevisionConflict;
        case Status::Incomplete: return WireStatus::Incomplete;
        case Status::Corrupt: return WireStatus::IntegrityFailure;
        case Status::IoFailure: return WireStatus::ResourceLimit;
    }
    return WireStatus::IntegrityFailure;
}

void StoreReceiver::State(const uint16_t request, const WireStatus status,
                          StoreIngressResult& result) const {
    result.handled = true;
    result.responseReady = true;
    result.responseSize = 19U;
    result.response[0] = MAGIC;
    result.response[1] = VERSION;
    result.response[2] = STORE_STATE;
    Put32(result.response.data() + 3U, m_projection.session);
    Put32(result.response.data() + 7U, m_store.Generation());
    Put16(result.response.data() + 11U, request);
    result.response[13] = static_cast<uint8_t>(status);
    const bool hasArtwork = m_store.HasArtwork();
    const uint8_t topology = hasArtwork
        ? m_store.CurrentManifest().topology : m_projection.topology;
    result.response[14] = (m_projection.active ? 1U : 0U) |
        (m_projection.playing ? 2U : 0U) | (hasArtwork ? 4U : 0U) |
        static_cast<uint8_t>((topology - 1U) << 3U) | 0x20U;
    const uint16_t frameCount = hasArtwork
        ? m_liveStream != nullptr && m_liveStream->Generation() == m_store.Generation()
            ? m_liveStream->FrameCount() : m_store.CurrentManifest().frameCount
        : 0U;
    Put16(result.response.data() + 15U, frameCount);
    Put16(result.response.data() + 17U,
          hasArtwork ? m_projection.selectedFrame : 0U);
    return;
}

void StoreReceiver::InspectState(
    const uint16_t request, const WireStatus status,
    StoreIngressResult& result) const {
    Residents(request, status, result);
    const uint8_t selected = result.response[14U];
    const uint8_t count = result.response[15U];
    const size_t descriptorBytes = result.responseSize - 16U;
    // Inspect shares the resident descriptors. Move them within the same
    // output slot before writing its longer State header.
    std::move_backward(result.response.begin() + 16U,
                       result.response.begin() + 16U + descriptorBytes,
                       result.response.begin() + 21U + descriptorBytes);
    State(request, status, result);
    result.response[19U] = selected;
    result.response[20U] = count;
    result.responseSize = static_cast<uint16_t>(21U + descriptorBytes);
    return;
}

void StoreReceiver::CheckpointState(
    const uint16_t request, const uint32_t cursor,
    const WireStatus status, StoreIngressResult& result) const {
    State(request, status, result);
    result.response[2] = STORE_CHECKPOINT_RECEIPT;
    result.responseSize = 18U;
    Put32(result.response.data() + 14U, cursor);
    return;
}

void StoreReceiver::SnapshotChunk(
    const uint16_t request, const uint32_t expectedGeneration,
    const uint32_t cursor, const uint16_t frameIndex,
    StoreIngressResult& result) const {
    constexpr size_t HEADER_BYTES = 60U;
    result.handled = true;
    result.responseReady = true;
    auto& response = result.response;
    response[0U] = MAGIC;
    response[1U] = VERSION;
    response[2U] = STORE_SNAPSHOT_CHUNK;
    Put32(response.data() + 3U, m_projection.session);
    Put32(response.data() + 7U, m_store.Generation());
    Put16(response.data() + 11U, request);
    if (m_commitAccepted ||
        (m_liveStream != nullptr && m_liveStream->HasPendingBlankDocument())) {
        response[13U] = static_cast<uint8_t>(m_commitAccepted
            ? WireStatus::ResourceLimit : WireStatus::Busy);
        result.responseSize = HEADER_BYTES;
        return;
    }
    size_t bodySize = 0U;
    uint32_t bodyBytes = m_store.CurrentBodyBytes();
    uint32_t bodyCrc32 = m_store.CurrentBodyCrc32();
    const bool framePreview = frameIndex != FRAME_COUNT;
    const auto status = framePreview
        ? m_store.ReadWireFrame(expectedGeneration, frameIndex, cursor,
            response.data() + HEADER_BYTES,
            m_snapshotResponseMaximum - HEADER_BYTES, bodySize,
            bodyBytes, bodyCrc32)
        : m_store.ReadWireBody(expectedGeneration, cursor,
            response.data() + HEADER_BYTES,
            m_snapshotResponseMaximum - HEADER_BYTES, bodySize);
    response[13U] = static_cast<uint8_t>(MapStatus(status));
    if (status != Status::Ok) {
        result.responseSize = HEADER_BYTES;
        return;
    }
    const auto& manifest = m_store.CurrentManifest();
    response[14U] = (m_projection.active ? 1U : 0U) |
        (m_projection.playing ? 2U : 0U) | 4U |
        (!framePreview && manifest.predecessorDeltas ? 8U : 0U) |
        (manifest.playbackOnEntry ? 16U : 0U) |
        (framePreview ? 32U : 0U);
    response[15U] = manifest.topology;
    Put16(response.data() + 16U, manifest.frameCount);
    Put16(response.data() + 18U,
          framePreview ? frameIndex : m_projection.selectedFrame);
    Put32(response.data() + 20U, bodyBytes);
    Put32(response.data() + 24U, bodyCrc32);
    Put32(response.data() + 28U, cursor);
    Put32(response.data() + 32U, manifest.sparkleSeed);
    for (size_t index = 0U; index < manifest.favorites.size(); ++index) {
        const auto& color = manifest.favorites[index];
        response[36U + index * 3U] = color.r;
        response[37U + index * 3U] = color.g;
        response[38U + index * 3U] = color.b;
    }
    result.responseSize = static_cast<uint16_t>(HEADER_BYTES + bodySize);
    return;
}

void StoreReceiver::Residents(const uint16_t request, const WireStatus status,
                              StoreIngressResult& result) const {
    constexpr size_t HEADER_BYTES = 16U;
    constexpr size_t DESCRIPTOR_BYTES = 45U;
    result.handled = true;
    result.responseReady = true;
    auto& response = result.response;
    response[0U] = MAGIC;
    response[1U] = VERSION;
    response[2U] = STORE_RESIDENTS_STATE;
    Put32(response.data() + 3U, m_projection.session);
    Put32(response.data() + 7U, m_store.Generation());
    Put16(response.data() + 11U, request);
    response[13U] = static_cast<uint8_t>(
        m_commitAccepted && status == WireStatus::Ok
            ? WireStatus::ResourceLimit : status);
    response[14U] = 0xffU;
    response[15U] = 0U;
    if (m_commitAccepted) {
        result.responseSize = HEADER_BYTES;
        return;
    }
    size_t offset = HEADER_BYTES;
    for (uint8_t slot = 0U; slot < 2U; ++slot) {
        FrameStore::ResidentDescriptor resident{};
        if (!m_store.Resident(slot, resident)) continue;
        response[14U] = m_store.Generation() == resident.generation ? slot
                                                                   : response[14U];
        response[offset] = slot;
        Put32(response.data() + offset + 1U, resident.generation);
        Put32(response.data() + offset + 5U, resident.bodyCrc32);
        Put32(response.data() + offset + 9U, resident.bodyBytes);
        Put16(response.data() + offset + 13U, resident.manifest.frameCount);
        response[offset + 15U] = resident.manifest.topology;
        response[offset + 16U] =
            (resident.manifest.active ? 1U : 0U) |
            (resident.manifest.playbackOnEntry ? 2U : 0U) |
            (resident.manifest.predecessorDeltas ? 4U : 0U);
        Put32(response.data() + offset + 17U,
              resident.manifest.sparkleSeed);
        for (size_t index = 0U;
             index < resident.manifest.favorites.size(); ++index) {
            const auto& color = resident.manifest.favorites[index];
            response[offset + 21U + index * 3U] = color.r;
            response[offset + 22U + index * 3U] = color.g;
            response[offset + 23U + index * 3U] = color.b;
        }
        offset += DESCRIPTOR_BYTES;
        ++response[15U];
    }
    result.responseSize = static_cast<uint16_t>(offset);
    return;
}

void StoreReceiver::RememberTerminal(
    const uint8_t operation, const uint32_t session,
    const uint16_t transfer, const uint32_t ownerEpoch,
    const uint32_t identity, const StoreIngressResult& response) {
    m_receipt.response = response;
    m_receipt.identity = identity;
    m_receipt.session = session;
    m_receipt.ownerEpoch = ownerEpoch;
    m_receipt.transfer = transfer;
    m_receipt.operation = operation;
}

void StoreReceiver::Retire(const bool abort) {
    if (m_commitAccepted) return;
    if (abort && m_transfer != 0U) (void)m_store.Abort(m_token);
    m_incomingManifest = {};
    m_token = {};
    m_ownerEpoch = 0U;
    m_bodyCursor = 0U;
    m_bodyBytes = 0U;
    m_pendingBodyBytes = 0U;
    m_beginIdentity = 0U;
    m_transferSession = 0U;
    m_transfer = 0U;
    m_transferFailure = WireStatus::Ok;
    m_commitRootPublished = false;
    m_commitExpectedGeneration = 0U;
    m_commitIdentity = 0U;
}

void StoreReceiver::RetryPendingCommit(StoreIngressResult& result) {
    result.Reset();
    if (!m_commitAccepted) return;
    if (!m_commitRootPublished) {
        if (m_store.PublishPreparedCommit(m_token) != Status::Ok) return;
        m_commitRootPublished = true;
    }
    if (m_commitPublication != nullptr &&
        !m_commitPublication(m_commitContext, m_ownerEpoch,
                             m_commitExpectedGeneration, m_incomingManifest,
                             m_store.Generation(),
                             m_store.IncomingFirstFrame())) return;
    const uint16_t transfer = m_transfer;
    const uint32_t session = m_transferSession;
    const uint32_t ownerEpoch = m_ownerEpoch;
    const uint32_t identity = m_commitIdentity;
    m_commitAccepted = false;
    Retire(false);
    InspectState(transfer, WireStatus::Ok, result);
    RememberTerminal(STORE_COMMIT, session, transfer, ownerEpoch,
                     identity, result);
    return;
}

Status StoreReceiver::FlushPendingBody(uint16_t& frameBudget) {
    if (m_pendingBodyBytes == 0U) return Status::Ok;
    const uint32_t offset =
        m_bodyCursor - static_cast<uint32_t>(m_pendingBodyBytes);
    auto& pending = m_store.IngressBodyBuffer();
    size_t consumed = 0U;
    const Status status = m_store.WriteBodyChunk(
        m_token, offset, pending.data(), m_pendingBodyBytes, consumed, frameBudget);
    m_pendingBodyBytes -= consumed;
    if (m_pendingBodyBytes != 0U && consumed != 0U)
        std::memmove(pending.data(), pending.data() + consumed, m_pendingBodyBytes);
    return status;
}

void StoreReceiver::ContinueBody(uint16_t& frameBudget) {
    if (!BodyPending() || frameBudget == 0U) return;
    const Status status = FlushPendingBody(frameBudget);
    if (status != Status::Ok) m_transferFailure = status == Status::InvalidArgument
        ? WireStatus::Incomplete : MapStatus(status);
}

Status StoreReceiver::BufferBody(const uint8_t* bytes, size_t size) {
    if (bytes == nullptr || size == 0U) return Status::InvalidArgument;
    if (size > m_store.IngressBodyBuffer().size() - m_pendingBodyBytes ||
        m_bodyCursor > m_bodyBytes || size > m_bodyBytes - m_bodyCursor) return Status::InvalidArgument;
    std::copy_n(bytes, size, m_store.IngressBodyBuffer().data() + m_pendingBodyBytes);
    m_pendingBodyBytes += size;
    m_bodyCursor += static_cast<uint32_t>(size);
    return Status::Ok;
}

void StoreReceiver::Handle(const uint8_t* const bytes, const size_t size,
                           const uint32_t ownerEpoch, StoreIngressResult& result,
                           uint16_t* frameBudget,
                           const bool cancelDeferredSelection) {
    result.Reset();
    if (bytes == nullptr || size < 3U || bytes[0] != MAGIC ||
        bytes[1] != VERSION) {
        return;
    }
    const uint8_t operation = bytes[2];
    if (operation != INSPECT && operation != STORE_SNAPSHOT &&
        operation != STORE_RESIDENTS && operation != STORE_SELECT &&
        (operation < STORE_BEGIN || operation > STORE_CANCEL)) {
        return;
    }
    result.handled = true;
    if (size < HEADER_BYTES) return State(0U, WireStatus::Malformed, result);
    const uint32_t session = fxe::ReadU32LE(bytes + 3U);
    const uint16_t transfer = fxe::ReadU16LE(bytes + 7U);
    if (m_liveStream != nullptr &&
        (m_liveStream->HasPendingBlankDocument() ||
         (!m_store.HasArtwork() && m_liveStream->Generation() != 0U &&
          m_liveStream->HasUnpersistedEdit())) && operation != STORE_CANCEL) {
        // New or an uncertain save can leave no readable saved descriptor while
        // accepted RAM artwork remains authoritative. Retry metadata after
        // recovery; this healthy barrier must not retire the app's stroke tail.
        return State(transfer, WireStatus::Busy, result);
    }
    if (operation == INSPECT) {
        if (size != 10U || transfer == 0U) {
            return State(transfer, WireStatus::Malformed, result);
        }
        if (m_commitAccepted) return State(transfer, WireStatus::ResourceLimit, result);
        if (bytes[9] != RESIDENT_LISTING_PROBE) {
            return State(transfer, WireStatus::Unsupported, result);
        }
        if (session != 0U && session != m_projection.session) {
            return State(transfer, WireStatus::StaleSession, result);
        }
        return InspectState(transfer, WireStatus::Ok, result);
    }
    if (operation == STORE_SNAPSHOT && size == 23U) {
        return HandleLiveSnapshot(bytes, ownerEpoch, result);
    }
    if (operation == STORE_SNAPSHOT) {
        const bool validSize = size == 17U || size == 19U;
        if (!validSize || transfer == 0U || session == 0U ||
            session != m_projection.session || ownerEpoch == 0U) {
            return State(transfer, validSize
                ? WireStatus::StaleSession : WireStatus::Malformed, result);
        }
        RetireRead();
        const uint32_t generation = fxe::ReadU32LE(bytes + 9U);
        const uint32_t cursor = fxe::ReadU32LE(bytes + 13U);
        uint16_t frameIndex = FRAME_COUNT;
        if (size == 19U) {
            frameIndex = fxe::ReadU16LE(bytes + 17U);
            if (frameIndex == 0xffffU) {
                // The storage worker samples current selection once. Playback
                // may advance while the same captured record is streamed.
                if (cursor != 0U) {
                    return State(transfer, WireStatus::Malformed, result);
                }
                frameIndex = m_projection.selectedFrame;
            } else if (frameIndex >= FRAME_COUNT) {
                return State(transfer, WireStatus::Malformed, result);
            }
        }
        SnapshotChunk(transfer, generation, cursor, frameIndex, result);
        if (result.responseReady && result.responseSize > 60U &&
            result.response[13U] == static_cast<uint8_t>(WireStatus::Ok)) {
            const uint32_t next = cursor + result.responseSize - 60U;
            const uint32_t total = fxe::ReadU32LE(result.response.data() + 20U);
            if (next < total) {
                m_readGeneration = generation;
                m_readSession = session;
                m_readOwnerEpoch = ownerEpoch;
                m_readCursor = next;
                m_readRequest = transfer;
                NotifyActivityStarted();
                m_readFrameIndex = frameIndex;
            }
        }
        return;
    }
    if (operation == STORE_RESIDENTS) {
        if (size != HEADER_BYTES || transfer == 0U || session == 0U ||
            session != m_projection.session) {
            return Residents(transfer, size == HEADER_BYTES
                ? WireStatus::StaleSession : WireStatus::Malformed, result);
        }
        return Residents(transfer, WireStatus::Ok, result);
    }
    if (transfer == 0U) return State(transfer, WireStatus::Malformed, result);
    if (ownerEpoch == 0U || session == 0U ||
        session != m_projection.session) {
        return State(transfer, WireStatus::StaleSession, result);
    }
    if (operation == STORE_CANCEL && size == HEADER_BYTES && m_liveCaptured &&
        m_readOwnerEpoch == ownerEpoch && m_readSession == session) {
        RetireRead();
        return State(transfer, WireStatus::Ok, result);
    }
    if (m_commitAccepted) {
        if (operation == STORE_COMMIT &&
            ownerEpoch == m_ownerEpoch && session == m_transferSession &&
            transfer == m_transfer && fxe::Crc32(bytes, size) ==
                m_commitIdentity) {
            return;
        }
        return State(transfer, WireStatus::Busy, result);
    }
    if (operation == STORE_SELECT) {
        if (size != 18U || m_transfer != 0U) {
            return Residents(transfer, size == 18U
                ? WireStatus::Busy : WireStatus::Malformed, result);
        }
        const Status status = m_store.SelectResident(
            bytes[9U], fxe::ReadU32LE(bytes + 10U),
            fxe::ReadU32LE(bytes + 14U));
        if (status == Status::Ok) {
            RetireRead();
            const Manifest& manifest = m_store.CurrentManifest();
            m_projection.topology = manifest.topology;
            m_projection.active = manifest.active;
            m_projection.playing = false;
            m_projection.selectedFrame = 0U;
        }
        Residents(transfer, MapStatus(status), result);
        result.playbackReady = status == Status::Ok;
        return;
    }
    const uint32_t identity = fxe::Crc32(bytes, size);
    if ((m_transfer == 0U || operation == STORE_CHECKPOINT) &&
        (operation == STORE_CHECKPOINT || operation == STORE_COMMIT ||
         operation == STORE_CANCEL) &&
        operation == m_receipt.operation && session == m_receipt.session &&
        transfer == m_receipt.transfer && ownerEpoch == m_receipt.ownerEpoch &&
        identity == m_receipt.identity) {
        result = m_receipt.response;
        return;
    }

    if (operation == STORE_BEGIN) {
        if (size != BEGIN_BYTES) {
            return State(transfer, WireStatus::Malformed, result);
        }
        if (m_transfer != 0U) {
            if (m_ownerEpoch == ownerEpoch && m_transferSession == session &&
                m_transfer == transfer && m_beginIdentity == identity) {
                return;
            }
            return State(transfer, WireStatus::Busy, result);
        }
        Manifest manifest{};
        const uint32_t expectedGeneration = fxe::ReadU32LE(bytes + 9U);
        const uint32_t bodyBytes = fxe::ReadU32LE(bytes + 13U);
        const uint32_t bodyCrc = fxe::ReadU32LE(bytes + 17U);
        manifest.frameCount = fxe::ReadU16LE(bytes + 21U);
        manifest.topology = bytes[23U];
        const uint8_t flags = bytes[24U];
        manifest.active = (flags & 1U) != 0U;
        manifest.playbackOnEntry = (flags & 2U) != 0U;
        manifest.predecessorDeltas = (flags & 4U) != 0U;
        manifest.sparkleSeed = fxe::ReadU32LE(bytes + 25U);
        for (size_t index = 0U; index < manifest.favorites.size(); ++index) {
            manifest.favorites[index] = {bytes[29U + index * 3U],
                                         bytes[30U + index * 3U],
                                         bytes[31U + index * 3U]};
        }
        if ((flags & ~0x0fU) != 0U ||
            (manifest.playbackOnEntry && !manifest.active)) {
            return State(transfer, WireStatus::Malformed, result);
        }
        if (manifest.active && !m_projection.active) {
            return State(transfer, WireStatus::StaleSession, result);
        }
        TransactionToken token{};
        const Status status = m_store.BeginWire(
            manifest, expectedGeneration, ownerEpoch, bodyBytes, bodyCrc,
            token, (flags & 0x08U) != 0U
                ? WireBodyMode::Rgb : WireBodyMode::Legacy);
        if (status != Status::Ok) {
            return State(transfer, MapStatus(status), result);
        }
        m_incomingManifest = manifest;
        m_token = token;
        m_ownerEpoch = ownerEpoch;
        m_bodyCursor = 0U;
        m_bodyBytes = bodyBytes;
        m_beginIdentity = identity;
        m_transferSession = session;
        m_transfer = transfer;
        NotifyActivityStarted();
        m_transferFailure = WireStatus::Ok;
        return;
    }

    const bool cancellingSelection = cancelDeferredSelection &&
        operation == STORE_CANCEL && m_transfer == 0U;
    if (!cancellingSelection && (m_transfer == 0U || m_ownerEpoch != ownerEpoch ||
        m_transferSession != session || m_transfer != transfer)) {
        return State(transfer, WireStatus::Incomplete, result);
    }
    if (operation == STORE_CANCEL) {
        if (size != HEADER_BYTES) {
            return State(transfer, WireStatus::Malformed, result);
        }
        RetireRead();
        Retire(true);
        State(transfer, WireStatus::Ok, result);
        RememberTerminal(operation, session, transfer, ownerEpoch, identity,
                         result);
        return;
    }
    if (operation == STORE_CHUNK) {
        if (size <= CURSOR_BYTES || m_transferFailure != WireStatus::Ok) {
            return;
        }
        const uint32_t cursor = fxe::ReadU32LE(bytes + 9U);
        Status status = cursor == m_bodyCursor
            ? BufferBody(bytes + CURSOR_BYTES, size - CURSOR_BYTES)
            : Status::InvalidArgument;
        if (status == Status::Ok && frameBudget == nullptr) {
            // Direct/native callers use the same prefix drain synchronously;
            // only the worker supplies a finite continuation budget.
            uint16_t budget = FRAME_COUNT;
            status = FlushPendingBody(budget);
        }
        if (status != Status::Ok) {
            m_transferFailure = status == Status::InvalidArgument
                ? WireStatus::Incomplete : MapStatus(status);
        }
        return;
    }
    if (size != CURSOR_BYTES) {
        return State(transfer, WireStatus::Malformed, result);
    }
    const uint32_t cursor = fxe::ReadU32LE(bytes + 9U);
    if (cursor == m_bodyCursor && m_transferFailure == WireStatus::Ok) {
        uint16_t unlimited = FRAME_COUNT;
        const Status flush = FlushPendingBody(frameBudget == nullptr ? unlimited : *frameBudget);
        if (flush != Status::Ok) m_transferFailure = MapStatus(flush);
    }
    // The worker retains Checkpoint/Commit in its FIFO until no body remains.
    // Direct callers using a bounded budget must observe the same barrier.
    if (BodyPending()) { result.handled = false; return; }
    if (m_transferFailure != WireStatus::Ok || cursor != m_bodyCursor) {
        return State(transfer, m_transferFailure != WireStatus::Ok
            ? m_transferFailure : WireStatus::Incomplete, result);
    }
    if (operation == STORE_CHECKPOINT) {
        CheckpointState(transfer, cursor,
                        MapStatus(m_store.Checkpoint(m_token, cursor)), result);
        RememberTerminal(operation, session, transfer, ownerEpoch, identity,
                         result);
        return;
    }
    if (operation == STORE_COMMIT) {
        WireStatus wireStatus = WireStatus::Incomplete;
        if (cursor == m_bodyBytes) {
            wireStatus = MapStatus(m_store.PrepareCommit(m_token));
        }
        if (wireStatus == WireStatus::Ok) {
            const PresentationAdmission admission = m_commitReservation == nullptr
                ? PresentationAdmission{PresentationAdmission::Kind::Accepted, false}
                : m_store.IncomingPreviewReady()
                    ? m_commitReservation(m_commitContext, ownerEpoch,
                        m_store.Generation(), m_incomingManifest,
                        m_store.IncomingGeneration(), m_store.IncomingFirstFrame())
                    : PresentationAdmission{};
            if (admission.kind == PresentationAdmission::Kind::PendingPresentation) {
                // PrepareCommit owns the complete body. Keep this original
                // request queued until physical Show releases admission.
                result.Reset();
                return;
            }
            wireStatus = admission.kind == PresentationAdmission::Kind::Accepted
                ? WireStatus::Ok : admission.dirty ? WireStatus::Busy : WireStatus::ResourceLimit;
            if (wireStatus == WireStatus::Ok) {
                m_commitExpectedGeneration = m_store.Generation();
                m_projection.topology = m_incomingManifest.topology;
                m_projection.active = m_incomingManifest.active;
                m_projection.playing = false;
                m_projection.selectedFrame = 0U;
                m_commitAccepted = true;
                m_commitIdentity = identity;
                return RetryPendingCommit(result);
            }
        }
        return State(transfer, wireStatus, result);
    }
    return State(transfer, WireStatus::Unsupported, result);
}

} // namespace modules::litchcraft_frame_store
