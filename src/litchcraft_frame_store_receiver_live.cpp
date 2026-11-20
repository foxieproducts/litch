#include "modules/litchcraft_frame_store_receiver.hpp"

#include "foxie_embedded/byte_order.hpp"
#include "modules/litchcraft_frame_stream.hpp"

namespace modules::litchcraft_frame_store {

void StoreReceiver::HandleLiveSnapshot(const uint8_t* bytes,
                                       const uint32_t ownerEpoch,
                                       StoreIngressResult& result) {
    const uint32_t session = fxe::ReadU32LE(bytes + 3U);
    const uint16_t request = fxe::ReadU16LE(bytes + 7U);
    if (request == 0U || ownerEpoch == 0U || session == 0U ||
        session != m_projection.session || !m_projection.active) {
        return State(request, WireStatus::StaleSession, result);
    }
    const uint32_t generation = fxe::ReadU32LE(bytes + 9U);
    const uint32_t cursor = fxe::ReadU32LE(bytes + 13U);
    const uint16_t selector = fxe::ReadU16LE(bytes + 17U);
    const uint32_t revision = fxe::ReadU32LE(bytes + 19U);
    if ((selector >= FRAME_COUNT && selector != 0xffffU && selector != 0xfffeU) ||
        (selector == 0xffffU && cursor != 0U)) {
        return State(request, WireStatus::Malformed, result);
    }
    if (m_transfer != 0U || m_commitAccepted) {
        return State(request, WireStatus::Busy, result);
    }
    if (m_liveStream == nullptr) return State(request, WireStatus::Unsupported, result);
    if (cursor == 0U) {
        RetireRead();
        m_liveRead.snapshot.generation = generation;
        m_liveRead.snapshot.revision = revision;
        const Status status = m_store.CaptureLiveRead(*m_liveStream, selector, m_liveRead);
        if (status != Status::Ok) return State(request, MapStatus(status), result);
        m_liveCaptured = true;
        m_readGeneration = generation;
        m_readSession = session;
        m_readOwnerEpoch = ownerEpoch;
        m_readFrameIndex = m_liveRead.snapshot.frameIndex;
    } else if (!m_liveCaptured || m_readOwnerEpoch != ownerEpoch ||
        m_readSession != session || generation != m_liveRead.snapshot.generation ||
        revision != m_liveRead.snapshot.revision ||
        (m_liveRead.layout ? selector != 0xfffeU : selector != m_readFrameIndex)) {
        return State(request, WireStatus::RevisionConflict, result);
    }
    m_readRequest = request;
    NotifyActivityStarted();
    m_readCursor = cursor;
    LiveSnapshotChunk(request, cursor, result);
    AdvanceRead(result, 78U);
}

void StoreReceiver::LiveSnapshotChunk(const uint16_t request, const uint32_t cursor,
                                      StoreIngressResult& result) {
    constexpr size_t HEADER_BYTES = 78U;
    result.handled = true;
    result.responseReady = true;
    auto& response = result.response;
    response[0U] = 0x4dU;
    response[1U] = 2U;
    response[2U] = STORE_SNAPSHOT_CHUNK;
    fxe::WriteU32LE(m_projection.session, response.data() + 3U);
    fxe::WriteU32LE(m_store.Generation(), response.data() + 7U);
    fxe::WriteU16LE(request, response.data() + 11U);
    result.responseSize = HEADER_BYTES;
    response[13U] = static_cast<uint8_t>(WireStatus::RevisionConflict);
    if (m_liveStream != nullptr && m_liveStream->HasPendingBlankDocument()) {
        response[13U] = static_cast<uint8_t>(WireStatus::Busy);
        return;
    }
    if (!m_liveCaptured || m_liveStream == nullptr || !m_projection.active ||
        m_projection.session != m_readSession || m_commitAccepted ||
        !m_liveStream->MatchesLiveSnapshot(m_liveRead.snapshot)) return;
    size_t bodySize = 0U;
    const Status status = m_store.ReadLiveChunk(m_liveRead, cursor,
        response.data() + HEADER_BYTES, m_snapshotResponseMaximum - HEADER_BYTES,
        bodySize);
    response[13U] = static_cast<uint8_t>(MapStatus(status));
    if (status != Status::Ok) return;
    // Recheck after the worker copy, including durable hidden-frame reads.
    // A racing publication never exposes an apparently valid old header/body.
    if (!m_liveStream->MatchesLiveSnapshot(m_liveRead.snapshot)) {
        response[13U] = static_cast<uint8_t>(WireStatus::RevisionConflict);
        return;
    }
    const auto& manifest = m_store.CurrentManifest();
    const auto& snapshot = m_liveRead.snapshot;
    response[14U] = 1U | (m_projection.playing ? 2U : 0U) | 4U |
        (manifest.playbackOnEntry ? 16U : 0U) | 64U |
        (m_liveRead.layout ? 128U : 32U);
    response[15U] = manifest.topology;
    fxe::WriteU16LE(snapshot.frameCount, response.data() + 16U);
    fxe::WriteU16LE(snapshot.frameIndex, response.data() + 18U);
    fxe::WriteU32LE(m_liveRead.bodyBytes, response.data() + 20U);
    fxe::WriteU32LE(m_liveRead.bodyCrc32, response.data() + 24U);
    fxe::WriteU32LE(cursor, response.data() + 28U);
    fxe::WriteU32LE(manifest.sparkleSeed, response.data() + 32U);
    for (size_t index = 0U; index < manifest.favorites.size(); ++index) {
        const auto& color = manifest.favorites[index];
        response[36U + index * 3U] = color.r;
        response[37U + index * 3U] = color.g;
        response[38U + index * 3U] = color.b;
    }
    fxe::WriteU32LE(snapshot.revision, response.data() + 60U);
    fxe::WriteU32LE(snapshot.documentEpoch, response.data() + 64U);
    fxe::WriteU32LE(snapshot.baseRevision, response.data() + 68U);
    response[72U] = static_cast<uint8_t>(snapshot.scope);
    fxe::WriteU16LE(snapshot.scopeIndex, response.data() + 73U);
    fxe::WriteU16LE(snapshot.scopeAux, response.data() + 75U);
    response[77U] = 0U;
    result.responseSize = static_cast<uint16_t>(HEADER_BYTES + bodySize);
}

} // namespace modules::litchcraft_frame_store
