#include "modules/litchcraft_fixed_slot_wire_view.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"
#include "modules/litchcraft_wire_codec.hpp"

namespace modules::litchcraft_fixed_slot_wire_view {
namespace {

constexpr size_t HEADER_BYTES = litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES;
constexpr size_t RECORD_CAPACITY = HEADER_BYTES +
    litchcraft_frame_store::WIRE_FRAME_MAX_BYTES;

uint32_t UpdateCrc32(uint32_t state, const uint8_t* const bytes,
                     const size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        state ^= bytes[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (state & 1U) != 0U ? 0xedb88320U : 0U;
            state = (state >> 1U) ^ mask;
        }
    }
    return state;
}

} // namespace

WireView::Status WireView::LoadFrame(const uint16_t index, Frame& out) const {
    if (index >= m_root.manifest.frameCount) return Status::InvalidArgument;
    if (index == m_overrideIndex && m_overrideFrame != nullptr) {
        out = *m_overrideFrame;
        return Status::Ok;
    }
    return m_storage.ReadFrame(m_root, index, out);
}

WireView::Status WireView::MeasureRecord(
    const Frame* const previous, const Frame& current,
    uint16_t& bytes, uint32_t& crc) {
    size_t encoded = 0U;
    if (m_record == nullptr || m_recordCapacity < RECORD_CAPACITY ||
        !litchcraft_wire_codec::FrameCodec::Encode(
            previous, current, m_record + HEADER_BYTES,
            m_recordCapacity - HEADER_BYTES, encoded)) {
        return Status::InvalidArgument;
    }
    fxe::WriteU16LE(static_cast<uint16_t>(encoded), m_record);
    fxe::WriteU32LE(fxe::Crc32(m_record + HEADER_BYTES, encoded),
                    m_record + 2U);
    bytes = static_cast<uint16_t>(HEADER_BYTES + encoded);
    crc = fxe::Crc32(m_record, bytes);
    return Status::Ok;
}

WireView::Status WireView::CombineSummaries(
    const litchcraft_frame_store::WireSummaries& summaries, Root& identity) {
    if (!identity.manifest.predecessorDeltas ||
        identity.manifest.frameCount == 0U ||
        identity.manifest.frameCount > litchcraft_frame_store::FRAME_COUNT) {
        return Status::InvalidArgument;
    }
    identity.blockBytes.fill(0U);
    identity.blockCrc32.fill(0U);
    for (uint16_t index = 0U; index < identity.manifest.frameCount; ++index) {
        const auto& summary = summaries[index];
        if (summary.absoluteBytes < HEADER_BYTES + 3U ||
            summary.absoluteBytes > RECORD_CAPACITY ||
            summary.deltaBytes < HEADER_BYTES + 3U ||
            summary.deltaBytes > RECORD_CAPACITY) return Status::Corrupt;
        const bool keyframe = index %
            litchcraft_frame_store::KEYFRAME_INTERVAL == 0U;
        const uint16_t bytes = keyframe
            ? summary.absoluteBytes : summary.deltaBytes;
        const uint32_t crc = keyframe
            ? summary.absoluteCrc32 : summary.deltaCrc32;
        const size_t block = index / litchcraft_frame_store::KEYFRAME_INTERVAL;
        identity.blockCrc32[block] = fxe::CombineCrc32(
            identity.blockCrc32[block], crc, bytes);
        identity.blockBytes[block] += bytes;
    }
    CombineBlocks(identity);
    return Status::Ok;
}

WireView::Status WireView::MeasureEditedSummaries(
    litchcraft_frame_store::WireSummaries& summaries,
    const litchcraft_frame_store::WireSummaryEdits& edits, Root& identity,
    bool (*continueWork)(void*), void* context) {
    identity = m_root;
    if (!m_root.manifest.predecessorDeltas ||
        m_root.manifest.frameCount == 0U ||
        m_root.manifest.frameCount > litchcraft_frame_store::FRAME_COUNT ||
        (m_overrideFrame != nullptr &&
         m_overrideIndex >= m_root.manifest.frameCount)) {
        return Status::InvalidArgument;
    }
    uint16_t previousIndex = litchcraft_frame_store::FRAME_COUNT;
    for (uint16_t index = 0U; index < m_root.manifest.frameCount; ++index) {
        if (!edits[index]) continue;
        if (continueWork != nullptr && !continueWork(context)) return Status::IoFailure;
        if (index != 0U && previousIndex != index - 1U) {
            const Status previous = LoadFrame(index - 1U, m_previous);
            if (previous != Status::Ok) return previous;
        }
        const Status current = LoadFrame(index, m_current);
        if (current != Status::Ok) return current;
        auto& summary = summaries[index];
        Status measured = MeasureRecord(nullptr, m_current,
            summary.absoluteBytes, summary.absoluteCrc32);
        if (measured == Status::Ok) {
            measured = MeasureRecord(index == 0U ? nullptr : &m_previous,
                m_current, summary.deltaBytes, summary.deltaCrc32);
        }
        if (measured != Status::Ok) return measured;
        m_previous = m_current;
        previousIndex = index;
    }
    Reset();
    return CombineSummaries(summaries, identity);
}

WireView::Status WireView::MeasureSummaries(
    litchcraft_frame_store::WireSummaries& summaries, Root& identity,
    bool (*continueWork)(void*), void* context) {
    summaries.fill(litchcraft_frame_store::WireSummary{});
    litchcraft_frame_store::WireSummaryEdits edits{};
    edits.fill(true);
    return MeasureEditedSummaries(summaries, edits, identity, continueWork, context);
}

void WireView::Reset() {
    m_streamOffset = 0U;
    m_recordSize = 0U;
    m_recordRead = 0U;
    m_nextFrame = 0U;
}

WireView::Cursor WireView::CaptureCursor() const {
    return {m_streamOffset, m_recordSize, m_recordRead, m_nextFrame};
}

bool WireView::RestoreCursor(const Cursor& cursor) {
    if (cursor.recordSize > m_recordCapacity ||
        cursor.recordRead > cursor.recordSize ||
        cursor.nextFrame > m_root.manifest.frameCount ||
        (cursor.nextFrame == 0U && cursor.streamOffset != 0U)) {
        return false;
    }
    m_streamOffset = cursor.streamOffset;
    m_recordSize = cursor.recordSize;
    m_recordRead = cursor.recordRead;
    m_nextFrame = cursor.nextFrame;
    return true;
}

WireView::Status WireView::LoadNextRecord() {
    if (m_record == nullptr || m_recordCapacity < RECORD_CAPACITY ||
        m_root.manifest.frameCount == 0U ||
        m_root.manifest.frameCount > litchcraft_frame_store::FRAME_COUNT ||
        m_nextFrame >= m_root.manifest.frameCount ||
        (m_overrideFrame != nullptr &&
         m_overrideIndex >= m_root.manifest.frameCount)) {
        return Status::InvalidArgument;
    }
    const uint16_t index = m_nextFrame;
    if (index == m_overrideIndex && m_overrideFrame != nullptr) {
        m_current = *m_overrideFrame;
    } else {
        const Status read = m_storage.ReadFrame(m_root, index, m_current);
        if (read != Status::Ok) return read;
    }
    const Frame* previous = index == 0U ||
            (m_root.manifest.predecessorDeltas &&
             index % litchcraft_frame_store::KEYFRAME_INTERVAL == 0U)
        ? nullptr : &m_previous;
    size_t encoded = 0U;
    if (!litchcraft_wire_codec::FrameCodec::Encode(
            previous, m_current, m_record + HEADER_BYTES,
            m_recordCapacity - HEADER_BYTES, encoded) ||
        encoded > litchcraft_frame_store::WIRE_FRAME_MAX_BYTES) {
        return Status::InvalidArgument;
    }
    fxe::WriteU16LE(static_cast<uint16_t>(encoded), m_record);
    fxe::WriteU32LE(fxe::Crc32(m_record + HEADER_BYTES, encoded),
                    m_record + 2U);
    if (index == 0U || m_root.manifest.predecessorDeltas) {
        m_previous = m_current;
    }
    m_recordSize = HEADER_BYTES + encoded;
    m_recordRead = 0U;
    ++m_nextFrame;
    return Status::Ok;
}

WireView::Status WireView::Advance(size_t count, uint8_t* const out) {
    size_t copied = 0U;
    while (copied < count) {
        if (m_recordRead == m_recordSize) {
            if (m_nextFrame >= m_root.manifest.frameCount) {
                return Status::InvalidArgument;
            }
            const Status loaded = LoadNextRecord();
            if (loaded != Status::Ok) return loaded;
        }
        const size_t step = std::min(count - copied,
                                     m_recordSize - m_recordRead);
        if (out != nullptr) {
            std::memcpy(out + copied, m_record + m_recordRead, step);
        }
        m_recordRead += step;
        m_streamOffset += static_cast<uint32_t>(step);
        copied += step;
    }
    return Status::Ok;
}

void WireView::CombineBlocks(Root& identity) {
    identity.bodyBytes = 0U;
    identity.bodyCrc32 = 0U;
    for (size_t block = 0U; block < litchcraft_frame_store_layout::WIRE_BLOCK_COUNT;
         ++block) {
        identity.bodyCrc32 = fxe::CombineCrc32(
            identity.bodyCrc32, identity.blockCrc32[block],
            identity.blockBytes[block]);
        identity.bodyBytes += identity.blockBytes[block];
    }
}

WireView::Status WireView::MeasureBlocks(Root& identity, bool (*continueWork)(void*), void* context) {
    identity = m_root;
    Reset();
    if (m_root.manifest.frameCount == 0U ||
        m_root.manifest.frameCount > litchcraft_frame_store::FRAME_COUNT) {
        return Status::InvalidArgument;
    }
    identity.blockBytes.fill(0U);
    identity.blockCrc32.fill(0U);
    for (size_t block = 0U;
         block < litchcraft_frame_store_layout::WIRE_BLOCK_COUNT; ++block) {
        uint32_t crc = 0xffffffffU;
        const size_t end = std::min<size_t>(
            m_root.manifest.frameCount,
            (block + 1U) * litchcraft_frame_store_layout::WIRE_BLOCK_FRAMES);
        while (m_nextFrame < end) {
            if (continueWork != nullptr && !continueWork(context)) return Status::IoFailure;
            const Status loaded = LoadNextRecord();
            if (loaded != Status::Ok) {
                Reset();
                return loaded;
            }
            crc = UpdateCrc32(crc, m_record, m_recordSize);
            identity.blockBytes[block] +=
                static_cast<uint32_t>(m_recordSize);
            m_recordRead = m_recordSize;
        }
        identity.blockCrc32[block] = ~crc;
    }
    CombineBlocks(identity);
    Reset();
    return Status::Ok;
}

WireView::Status WireView::Measure(uint32_t& bodyBytes,
                                  uint32_t& bodyCrc32) {
    bodyBytes = 0U;
    bodyCrc32 = 0U;
    Root identity{};
    const Status measured = MeasureBlocks(identity);
    if (measured == Status::Ok) {
        bodyBytes = identity.bodyBytes;
        bodyCrc32 = identity.bodyCrc32;
    }
    return measured;
}

WireView::Status WireView::MeasureChangedBlock(const uint16_t editedIndex,
                                               Root& identity) {
    identity = m_root;
    if (m_overrideFrame == nullptr || m_overrideIndex != editedIndex ||
        editedIndex >= m_root.manifest.frameCount) {
        return Status::InvalidArgument;
    }
    Root previous = m_root;
    CombineBlocks(previous);
    if (previous.bodyBytes != m_root.bodyBytes ||
        previous.bodyCrc32 != m_root.bodyCrc32) {
        return Status::Corrupt;
    }
    if (!m_root.manifest.predecessorDeltas && editedIndex == 0U) {
        return MeasureBlocks(identity);
    }
    const size_t block =
        editedIndex / litchcraft_frame_store_layout::WIRE_BLOCK_FRAMES;
    const size_t start =
        block * litchcraft_frame_store_layout::WIRE_BLOCK_FRAMES;
    const size_t end = std::min<size_t>(
        m_root.manifest.frameCount,
        start + litchcraft_frame_store_layout::WIRE_BLOCK_FRAMES);
    Reset();
    m_nextFrame = static_cast<uint16_t>(start);
    if (!m_root.manifest.predecessorDeltas && start != 0U) {
        const Status base = m_storage.ReadFrame(m_root, 0U, m_previous);
        if (base != Status::Ok) {
            Reset();
            return base;
        }
    }
    uint32_t crc = 0xffffffffU;
    identity.blockBytes[block] = 0U;
    while (m_nextFrame < end) {
        const Status loaded = LoadNextRecord();
        if (loaded != Status::Ok) {
            Reset();
            return loaded;
        }
        crc = UpdateCrc32(crc, m_record, m_recordSize);
        identity.blockBytes[block] += static_cast<uint32_t>(m_recordSize);
        m_recordRead = m_recordSize;
    }
    identity.blockCrc32[block] = ~crc;
    CombineBlocks(identity);
    Reset();
    return Status::Ok;
}

WireView::Status WireView::Read(const uint32_t offset, uint8_t* const out,
                                const size_t capacity, size_t& outSize) {
    outSize = 0U;
    if (out == nullptr || capacity == 0U ||
        m_record == nullptr || m_recordCapacity < RECORD_CAPACITY ||
        m_root.manifest.frameCount == 0U ||
        m_root.manifest.frameCount > litchcraft_frame_store::FRAME_COUNT) {
        return Status::InvalidArgument;
    }
    if (offset < m_streamOffset) Reset();
    const Status skipped = Advance(offset - m_streamOffset, nullptr);
    if (skipped != Status::Ok) {
        Reset();
        return skipped;
    }
    size_t copied = 0U;
    while (copied < capacity &&
           (m_recordRead != m_recordSize ||
            m_nextFrame < m_root.manifest.frameCount)) {
        if (m_recordRead == m_recordSize) {
            const Status loaded = LoadNextRecord();
            if (loaded != Status::Ok) {
                Reset();
                return loaded;
            }
        }
        const size_t step = std::min(capacity - copied,
                                     m_recordSize - m_recordRead);
        std::memcpy(out + copied, m_record + m_recordRead, step);
        m_recordRead += step;
        m_streamOffset += static_cast<uint32_t>(step);
        copied += step;
    }
    outSize = copied;
    return copied == 0U ? Status::InvalidArgument : Status::Ok;
}

} // namespace modules::litchcraft_fixed_slot_wire_view
