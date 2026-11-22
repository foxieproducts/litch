#include "modules/litchcraft_frame_store_layout.hpp"

#include <algorithm>
#include <array>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"

namespace modules::litchcraft_frame_store_layout {
namespace {
namespace store = litchcraft_frame_store;
constexpr size_t CHUNK_NODES = 64U;
constexpr size_t CHUNK_BYTES = CHUNK_NODES * store::WIRE_SUMMARY_BYTES;
constexpr size_t DATA_BYTES = store::FRAME_COUNT * store::WIRE_SUMMARY_BYTES;
using Header = std::array<uint8_t, store::WIRE_SUMMARY_HEADER_BYTES>;

void Encode(const store::WireSummary& summary, uint8_t* const out) {
    fxe::WriteU16LE(summary.absoluteBytes, out);
    fxe::WriteU16LE(summary.deltaBytes, out + 2U);
    fxe::WriteU32LE(summary.absoluteCrc32, out + 4U);
    fxe::WriteU32LE(summary.deltaCrc32, out + 8U);
}

void Decode(const uint8_t* const bytes, store::WireSummary& out) {
    out.absoluteBytes = fxe::ReadU16LE(bytes);
    out.deltaBytes = fxe::ReadU16LE(bytes + 2U);
    out.absoluteCrc32 = fxe::ReadU32LE(bytes + 4U);
    out.deltaCrc32 = fxe::ReadU32LE(bytes + 8U);
}
} // namespace

Status Layout::ReadSummaryPage(const Root& root, const uint8_t copy,
                               store::WireSummaries* const out) const {
    if (root.resident > 1U || copy > 1U || m_ops.read == nullptr ||
        m_scratch == nullptr || m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    Header expected{};
    EncodeSummaryHeader(root, copy, expected.data());
    const size_t offset = SummaryOffset(root.resident, copy);
    if (!m_ops.read(m_ops.context, offset, m_scratch, expected.size())) {
        return Status::IoFailure;
    }
    if (!std::equal(expected.begin(), expected.end(), m_scratch)) {
        const bool blank = std::all_of(m_scratch,
            m_scratch + expected.size(), [](uint8_t value) {
                return value == 0xffU;
            });
        return blank ? Status::Blank : Status::RevisionConflict;
    }
    uint32_t state = ~fxe::Crc32(expected.data(), expected.size());
    for (size_t cursor = 0U; cursor < DATA_BYTES; cursor += CHUNK_BYTES) {
        if (!m_ops.read(m_ops.context, offset + expected.size() + cursor,
                        m_scratch, CHUNK_BYTES)) return Status::IoFailure;
        state = ~fxe::Crc32(m_scratch, CHUNK_BYTES, state);
        if (out == nullptr) continue;
        for (size_t index = 0U; index < CHUNK_NODES; ++index) {
            Decode(m_scratch + index * store::WIRE_SUMMARY_BYTES,
                (*out)[cursor / store::WIRE_SUMMARY_BYTES + index]);
        }
    }
    if (!m_ops.read(m_ops.context, offset + expected.size() + DATA_BYTES,
                    m_scratch, 4U)) return Status::IoFailure;
    return fxe::ReadU32LE(m_scratch) == ~state
        ? Status::Ok : Status::Corrupt;
}

Status Layout::ReadWireSummaries(const Root& root, const uint8_t copy,
                                 store::WireSummaries& out) const {
    return ReadSummaryPage(root, copy, &out);
}

Status Layout::WriteWireSummaries(
    const Root& root, const uint8_t copy,
    const store::WireSummaries& summaries) const {
    if (root.resident > 1U || copy > 1U || m_ops.write == nullptr ||
        m_ops.read == nullptr || m_ops.sync == nullptr ||
        m_scratch == nullptr || m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    Header header{};
    EncodeSummaryHeader(root, copy, header.data());
    const size_t offset = SummaryOffset(root.resident, copy);
    if (!m_ops.write(m_ops.context, offset, header.data(), header.size())) {
        return Status::IoFailure;
    }
    uint32_t state = ~fxe::Crc32(header.data(), header.size());
    for (size_t cursor = 0U; cursor < DATA_BYTES; cursor += CHUNK_BYTES) {
        for (size_t index = 0U; index < CHUNK_NODES; ++index) {
            Encode(summaries[cursor / store::WIRE_SUMMARY_BYTES + index],
                m_scratch + index * store::WIRE_SUMMARY_BYTES);
        }
        state = ~fxe::Crc32(m_scratch, CHUNK_BYTES, state);
        if (!m_ops.write(m_ops.context, offset + header.size() + cursor,
                         m_scratch, CHUNK_BYTES)) return Status::IoFailure;
    }
    fxe::WriteU32LE(~state, m_scratch);
    if (!m_ops.write(m_ops.context, offset + header.size() + DATA_BYTES,
                     m_scratch, 4U) || !m_ops.sync(m_ops.context)) {
        return Status::IoFailure;
    }
    return ReadSummaryPage(root, copy, nullptr);
}

Status Layout::RebindSummaryPage(const Root& previous,
                                const uint8_t previousCopy,
                                const Root& root, const uint8_t copy) const {
    Header header{};
    EncodeSummaryHeader(root, copy, header.data());
    const size_t source = SummaryOffset(previous.resident, previousCopy);
    const size_t destination = SummaryOffset(root.resident, copy);
    if (!m_ops.write(m_ops.context, destination, header.data(), header.size())) {
        return Status::IoFailure;
    }
    uint32_t state = ~fxe::Crc32(header.data(), header.size());
    for (size_t cursor = 0U; cursor < DATA_BYTES; cursor += CHUNK_BYTES) {
        if (!m_ops.read(m_ops.context, source + header.size() + cursor,
                        m_scratch, CHUNK_BYTES)) return Status::IoFailure;
        state = ~fxe::Crc32(m_scratch, CHUNK_BYTES, state);
        if (!m_ops.write(m_ops.context, destination + header.size() + cursor,
                         m_scratch, CHUNK_BYTES)) return Status::IoFailure;
    }
    fxe::WriteU32LE(~state, m_scratch);
    if (!m_ops.write(m_ops.context, destination + header.size() + DATA_BYTES,
                     m_scratch, 4U) || !m_ops.sync(m_ops.context)) {
        return Status::IoFailure;
    }
    return ReadSummaryPage(root, copy, nullptr) == Status::Ok
        ? Status::Ok : Status::IoFailure;
}

} // namespace modules::litchcraft_frame_store_layout
