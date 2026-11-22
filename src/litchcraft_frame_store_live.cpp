#include "modules/litchcraft_frame_store.hpp"

#include <algorithm>
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "modules/litchcraft_frame_store_layout.hpp"
#include "modules/litchcraft_frame_stream.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace modules::litchcraft_frame_store {
namespace {
constexpr uint16_t LIVE_LAYOUT = 0xfffeU;
constexpr uint16_t LIVE_FRAME = 0xfffdU;
constexpr size_t LIVE_ROW_BYTES = 6U;

void EncodeSummary(const WireSummary& summary, uint8_t* out) {
    fxe::WriteU16LE(summary.absoluteBytes, out);
    fxe::WriteU32LE(summary.absoluteCrc32, out + 2U);
}
} // namespace

Status FrameStore::EncodeLiveFrame() {
    size_t encoded = 0U;
    if (!litchcraft_wire_codec::FrameCodec::Encode(nullptr, m_decodeCachedFrame,
            m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES,
            WIRE_FRAME_MAX_BYTES, encoded)) return Status::Corrupt;
    fxe::WriteU16LE(static_cast<uint16_t>(encoded), m_wireRecord.data());
    fxe::WriteU32LE(fxe::Crc32(m_wireRecord.data() + WIRE_RECORD_HEADER_BYTES, encoded),
                    m_wireRecord.data() + 2U);
    m_wireRecordSize = WIRE_RECORD_HEADER_BYTES + encoded;
    return Status::Ok;
}

Status FrameStore::CaptureLiveRead(litchcraft_frame_stream::FrameStream& stream,
                                   const uint16_t selector, LiveRead& read) {
    using Result = litchcraft_frame_stream::FrameStream::LiveFrameResult;
    if (!IsConfigured()) return Status::NotConfigured;
    if (m_transactionActive) return Status::Incomplete;
    if (!m_hasCurrent) return Status::NoArtwork;
    if (read.snapshot.generation != Generation() ||
        !stream.CaptureLiveSnapshot(selector, read.snapshot)) {
        return Status::RevisionConflict;
    }
    ResetWireCursor();
    read.layout = selector == LIVE_LAYOUT;
    if (read.layout) {
        // This observational path never repairs a missing cache by scanning
        // artwork or writing flash. Only the root-bound cache is admissible.
        if (!m_wireSummariesReady) {
            const Status cached = LoadBoundWireSummaries();
            if (cached != Status::Ok) return cached;
        }
        // Capture only the bounded dirty cache before borrowing the existing
        // wire/slot buffers for the compact live table. Durable summaries stay
        // root-bound and are indexed through the live identity map.
        struct Overlay final { uint16_t index = FRAME_COUNT; WireSummary summary{}; };
        std::array<Overlay, litchcraft_frame_stream::FrameStream::CACHE_SLOTS> overlays{};
        for (uint8_t slot = 0U; slot < overlays.size(); ++slot) {
            uint16_t index = 0U;
            const auto captured = stream.CopyDirtyFrame(read.snapshot, slot,
                                                        m_decodeCachedFrame, index);
            if (captured == Result::Conflict) return Status::RevisionConflict;
            if (captured != Result::Cached) continue;
            const Status encoded = EncodeLiveFrame();
            if (encoded != Status::Ok) return encoded;
            overlays[slot].index = index;
            overlays[slot].summary.absoluteBytes = static_cast<uint16_t>(m_wireRecordSize);
            overlays[slot].summary.absoluteCrc32 = fxe::Crc32(m_wireRecord.data(), m_wireRecordSize);
        }
        static_assert(FRAME_COUNT * LIVE_ROW_BYTES <=
            WIRE_RECORD_HEADER_BYTES + 2U * WIRE_FRAME_MAX_BYTES);
        read.bodyBytes = read.snapshot.frameCount * LIVE_ROW_BYTES;
        uint32_t state = 0xffffffffU;
        uint8_t row[LIVE_ROW_BYTES]{};
        for (uint16_t index = 0U; index < read.snapshot.frameCount; ++index) {
            WireSummary summary{};
            bool dirty = false;
            for (const auto& overlay : overlays) {
                if (overlay.index == index) { summary = overlay.summary; dirty = true; break; }
            }
            if (!dirty) {
                const uint16_t durable = stream.LiveDurableIndex(read.snapshot, index);
                if (durable >= m_currentRoot.manifest.frameCount) return Status::RevisionConflict;
                summary = m_workspace.summaries[durable];
            }
            if (summary.absoluteBytes < WIRE_RECORD_HEADER_BYTES + 3U ||
                summary.absoluteBytes > PageCacheBytes()) return Status::Corrupt;
            EncodeSummary(summary, row);
            state = ~fxe::Crc32(row, sizeof(row), state);
            for (size_t byte = 0U; byte < LIVE_ROW_BYTES; ++byte) {
                const size_t position = size_t(index) * LIVE_ROW_BYTES + byte;
                if (position < m_wireRecord.size()) m_wireRecord[position] = row[byte];
                else m_slotScratch[position - m_wireRecord.size()] = row[byte];
            }
        }
        read.bodyCrc32 = ~state;
    } else {
        const auto captured = stream.CopyLiveFrame(read.snapshot, m_decodeCachedFrame);
        if (captured == Result::Conflict) return Status::RevisionConflict;
        if (captured == Result::Durable) {
            litchcraft_frame_store_layout::Layout storage(m_ops,
                m_slotScratch.data(), m_slotScratch.size());
            const auto status = storage.ReadFrame(m_currentRoot,
                stream.LiveDurableIndex(read.snapshot, read.snapshot.frameIndex), m_decodeCachedFrame);
            if (status != litchcraft_frame_store_layout::Status::Ok) {
                return status == litchcraft_frame_store_layout::Status::IoFailure
                    ? Status::IoFailure : Status::Corrupt;
            }
        }
        const Status encoded = EncodeLiveFrame();
        if (encoded != Status::Ok) return encoded;
        read.bodyBytes = static_cast<uint32_t>(m_wireRecordSize);
        read.bodyCrc32 = fxe::Crc32(m_wireRecord.data(), m_wireRecordSize);
    }
    if (!stream.MatchesLiveSnapshot(read.snapshot)) return Status::RevisionConflict;
    m_wireGeneration = read.snapshot.generation;
    m_wireFrameIndex = read.layout ? LIVE_LAYOUT : LIVE_FRAME;
    read.pageEpoch = m_pageEpoch;
    return Status::Ok;
}

Status FrameStore::ReadLiveChunk(const LiveRead& read, const uint32_t cursor,
                                 uint8_t* out, const size_t capacity,
                                 size_t& outSize) const {
    outSize = 0U;
    if (m_transactionActive) return Status::Incomplete;
    if (read.pageEpoch != m_pageEpoch || Generation() != read.snapshot.generation ||
        m_wireGeneration != read.snapshot.generation ||
        m_wireFrameIndex != (read.layout ? LIVE_LAYOUT : LIVE_FRAME)) {
        return Status::RevisionConflict;
    }
    if (out == nullptr || capacity == 0U || cursor >= read.bodyBytes) {
        return Status::InvalidArgument;
    }
    outSize = std::min<size_t>(capacity, read.bodyBytes - cursor);
    if (!read.layout) {
        std::memcpy(out, m_wireRecord.data() + cursor, outSize);
        return Status::Ok;
    }
    for (size_t offset = 0U; offset < outSize; ++offset) {
        const size_t position = cursor + offset;
        out[offset] = position < m_wireRecord.size() ? m_wireRecord[position]
            : m_slotScratch[position - m_wireRecord.size()];
    }
    return Status::Ok;
}

} // namespace modules::litchcraft_frame_store
