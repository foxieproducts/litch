#include "modules/litchcraft_frame_store_layout.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "foxie_embedded/byte_order.hpp"
#include "foxie_embedded/checksum.hpp"

namespace modules::litchcraft_frame_store_layout {
namespace {

constexpr std::array<uint8_t, 4U> ROOT_MAGIC{{'L', 'C', '6', 'R'}};
constexpr uint8_t STORAGE_SCHEMA = 6U;
constexpr uint8_t COMMITTED = 0xa5U;
constexpr uint8_t RETIRED = 0x5aU;
constexpr size_t MAP_OFFSET = 60U;
constexpr size_t BLOCK_OFFSET = MAP_OFFSET +
    litchcraft_frame_store::FRAME_COUNT;
constexpr size_t ROOT_CRC_OFFSET = ROOT_BYTES - 4U;
static_assert(BLOCK_OFFSET + WIRE_BLOCK_COUNT * 8U == ROOT_CRC_OFFSET);
constexpr size_t VERIFY_BYTES = 256U;

bool ValidManifest(const litchcraft_frame_store::Manifest& value) {
    return value.frameCount >= 1U &&
           value.frameCount <= litchcraft_frame_store::FRAME_COUNT &&
           value.topology >= 1U && value.topology <= 3U &&
           value.sparkleSeed != 0U;
}

void EncodeRoot(const Root& root, const uint8_t copy, uint8_t* const out) {
    std::fill_n(out, ROOT_BYTES, 0U);
    std::copy(ROOT_MAGIC.begin(), ROOT_MAGIC.end(), out);
    out[4] = STORAGE_SCHEMA;
    out[5] = root.retired ? RETIRED : COMMITTED;
    out[6] = root.resident;
    out[7] = copy;
    fxe::WriteU32LE(root.generation, out + 8U);
    fxe::WriteU32LE(root.serial, out + 12U);
    fxe::WriteU16LE(root.manifest.frameCount, out + 16U);
    out[18] = root.manifest.topology;
    out[19] = (root.manifest.active ? 1U : 0U) |
              (root.manifest.playbackOnEntry ? 2U : 0U) |
              (root.manifest.predecessorDeltas ? 4U : 0U);
    fxe::WriteU32LE(root.manifest.sparkleSeed, out + 20U);
    for (size_t index = 0U; index < root.manifest.favorites.size(); ++index) {
        const auto color = root.manifest.favorites[index];
        out[24U + index * 3U] = color.r;
        out[25U + index * 3U] = color.g;
        out[26U + index * 3U] = color.b;
    }
    fxe::WriteU32LE(root.bodyBytes, out + 48U);
    fxe::WriteU32LE(root.bodyCrc32, out + 52U);
    fxe::WriteU16LE(root.stageIndex, out + 56U);
    std::copy(root.slots.begin(), root.slots.end(), out + MAP_OFFSET);
    for (size_t index = 0U; index < WIRE_BLOCK_COUNT; ++index) {
        fxe::WriteU32LE(root.blockBytes[index],
                        out + BLOCK_OFFSET + index * 8U);
        fxe::WriteU32LE(root.blockCrc32[index],
                        out + BLOCK_OFFSET + index * 8U + 4U);
    }
    fxe::WriteU32LE(fxe::Crc32(out, ROOT_CRC_OFFSET), out + ROOT_CRC_OFFSET);
}

bool DecodeRoot(const uint8_t* const bytes, const uint8_t resident,
                const uint8_t copy, Root& out) {
    if (!std::equal(ROOT_MAGIC.begin(), ROOT_MAGIC.end(), bytes) ||
        bytes[4] != STORAGE_SCHEMA ||
        (bytes[5] != COMMITTED && bytes[5] != RETIRED) ||
        bytes[6] != resident || bytes[7] != copy ||
        bytes[58] != 0U || bytes[59] != 0U ||
        fxe::ReadU32LE(bytes + ROOT_CRC_OFFSET) !=
            fxe::Crc32(bytes, ROOT_CRC_OFFSET)) {
        return false;
    }
    litchcraft_frame_store::Manifest manifest{};
    const uint32_t generation = fxe::ReadU32LE(bytes + 8U);
    const uint32_t serial = fxe::ReadU32LE(bytes + 12U);
    const uint32_t bodyBytes = fxe::ReadU32LE(bytes + 48U);
    const uint16_t stageIndex = fxe::ReadU16LE(bytes + 56U);
    manifest.frameCount = fxe::ReadU16LE(bytes + 16U);
    manifest.topology = bytes[18];
    if ((bytes[19] & ~7U) != 0U) return false;
    manifest.active = (bytes[19] & 1U) != 0U;
    manifest.playbackOnEntry = (bytes[19] & 2U) != 0U;
    manifest.predecessorDeltas = (bytes[19] & 4U) != 0U;
    manifest.sparkleSeed = fxe::ReadU32LE(bytes + 20U);
    for (size_t index = 0U; index < manifest.favorites.size(); ++index) {
        manifest.favorites[index] =
            {bytes[24U + index * 3U], bytes[25U + index * 3U],
             bytes[26U + index * 3U]};
    }
    if (generation == 0U || serial == 0U || !ValidManifest(manifest) ||
        bodyBytes < manifest.frameCount *
                                  litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES ||
        bodyBytes > litchcraft_frame_store::WIRE_BODY_MAX_BYTES ||
        (stageIndex != NO_STAGE && stageIndex >= manifest.frameCount)) {
        return false;
    }
    out.resident = resident;
    out.retired = bytes[5] == RETIRED;
    out.generation = generation;
    out.serial = serial;
    out.manifest = manifest;
    out.bodyBytes = bodyBytes;
    out.bodyCrc32 = fxe::ReadU32LE(bytes + 52U);
    out.stageIndex = stageIndex;
    std::copy_n(bytes + MAP_OFFSET, out.slots.size(), out.slots.begin());
    for (size_t index = 0U; index < WIRE_BLOCK_COUNT; ++index) {
        out.blockBytes[index] =
            fxe::ReadU32LE(bytes + BLOCK_OFFSET + index * 8U);
        out.blockCrc32[index] =
            fxe::ReadU32LE(bytes + BLOCK_OFFSET + index * 8U + 4U);
    }
    return true;
}

Status ClassifyRoot(const uint8_t* const bytes, const uint8_t resident,
                    const uint8_t copy, Root& out) {
    if (DecodeRoot(bytes, resident, copy, out)) return Status::Ok;
    // Only the exact prelaunch format authorizes its one-time migration.
    if (bytes[0] == 'L' && bytes[1] == 'C' &&
        bytes[2] == '5' && bytes[3] == 'R' && bytes[4] == 5U &&
        (bytes[5] == COMMITTED || bytes[5] == RETIRED) &&
        bytes[6] == resident && bytes[7] == copy &&
        fxe::ReadU32LE(bytes + ROOT_CRC_OFFSET) ==
            fxe::Crc32(bytes, ROOT_CRC_OFFSET)) {
        return Status::PrelaunchLC5;
    }
    const bool blankZero = std::all_of(bytes, bytes + ROOT_BYTES,
        [](const uint8_t value) { return value == 0U; });
    const bool blankErased = std::all_of(bytes, bytes + ROOT_BYTES,
        [](const uint8_t value) { return value == 0xffU; });
    return blankZero || blankErased ? Status::Blank : Status::Corrupt;
}

bool Newer(const uint32_t left, const uint32_t right) {
    return static_cast<int32_t>(left - right) > 0;
}

} // namespace

void Layout::EncodeSummaryHeader(const Root& root, const uint8_t copy,
                                  uint8_t* const out) const {
    EncodeRoot(root, copy, m_scratch);
    const uint32_t rootCrc = fxe::ReadU32LE(m_scratch + ROOT_CRC_OFFSET);
    std::fill_n(out, litchcraft_frame_store::WIRE_SUMMARY_HEADER_BYTES, 0U);
    out[0U] = 'L'; out[1U] = 'C'; out[2U] = 'W'; out[3U] = 'C';
    out[4U] = 1U;
    out[6U] = root.resident;
    out[7U] = copy;
    fxe::WriteU32LE(rootCrc, out + 8U);
    fxe::WriteU32LE(root.generation, out + 12U);
    fxe::WriteU32LE(root.serial, out + 16U);
    fxe::WriteU32LE(root.bodyBytes, out + 20U);
    fxe::WriteU32LE(root.bodyCrc32, out + 24U);
    fxe::WriteU16LE(root.manifest.frameCount, out + 28U);
    out[30U] = root.manifest.predecessorDeltas ? 1U : 0U;
}

Status Layout::ReadRoot(const uint8_t resident, const uint8_t copy,
                        Root& out) const {
    if (resident > 1U || copy > 1U || m_ops.read == nullptr ||
        m_scratch == nullptr || m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    if (!m_ops.read(m_ops.context, RootOffset(resident, copy), m_scratch,
                    ROOT_BYTES)) {
        return Status::IoFailure;
    }
    return ClassifyRoot(m_scratch, resident, copy, out);
}

Status Layout::LatestRoot(const uint8_t resident, Root& out,
                          uint8_t& copy) const {
    static_assert(2U * ROOT_BYTES <= FRAME_SLOT_BYTES);
    if (resident > 1U || m_ops.read == nullptr ||
        m_scratch == nullptr || m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    // Both copies occupy the same file. One fresh read avoids reopening it
    // while preserving independent CRC/format checks and serial selection.
    if (!m_ops.read(m_ops.context, RootOffset(resident, 0U), m_scratch,
                    2U * ROOT_BYTES)) {
        return Status::IoFailure;
    }
    Root second{};
    const Status left = ClassifyRoot(m_scratch, resident, 0U, out);
    const Status right = ClassifyRoot(m_scratch + ROOT_BYTES, resident, 1U, second);
    if (left == Status::Ok || right == Status::Ok) {
        copy = left == Status::Ok &&
               (right != Status::Ok || !Newer(second.serial, out.serial))
            ? 0U : 1U;
        if (copy == 1U) out = second;
        return Status::Ok;
    }
    if ((left == Status::Blank || left == Status::PrelaunchLC5) &&
        (right == Status::Blank || right == Status::PrelaunchLC5) &&
        (left == Status::PrelaunchLC5 || right == Status::PrelaunchLC5)) {
        return Status::PrelaunchLC5;
    }
    return left == Status::Blank && right == Status::Blank
        ? Status::Blank : Status::Corrupt;
}

Status Layout::PublishRoot(
    const Root& root, const uint8_t copy,
    const litchcraft_frame_store::WireSummaries* const summaries) const {
    if (root.resident > 1U || copy > 1U || root.generation == 0U ||
        root.serial == 0U || !ValidManifest(root.manifest) ||
        root.bodyBytes < root.manifest.frameCount *
                             litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES ||
        root.bodyBytes > litchcraft_frame_store::WIRE_BODY_MAX_BYTES ||
        (root.stageIndex != NO_STAGE &&
         root.stageIndex >= root.manifest.frameCount) ||
        m_ops.write == nullptr || m_ops.read == nullptr ||
        m_ops.sync == nullptr || m_scratch == nullptr ||
        m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    if (summaries != nullptr) {
        const Status cached = WriteWireSummaries(root, copy, *summaries);
        if (cached != Status::Ok) return cached;
    } else if (!root.retired) {
        Root previous{};
        uint8_t previousCopy = 0U;
        const Status latest = LatestRoot(root.resident, previous, previousCopy);
        if (latest == Status::IoFailure) return latest;
        if (latest == Status::Ok && !previous.retired &&
            previous.bodyBytes == root.bodyBytes &&
            previous.bodyCrc32 == root.bodyCrc32 &&
            previous.manifest.frameCount == root.manifest.frameCount &&
            previous.manifest.predecessorDeltas ==
                root.manifest.predecessorDeltas) {
            // Optional old-page reads cannot block mode/Exit or a valid fold.
            // Once a usable cache is found, a failed new-page publication
            // remains a persistence failure rather than false completion.
            if (ReadSummaryPage(previous, previousCopy, nullptr) == Status::Ok) {
                const Status rebound = RebindSummaryPage(
                    previous, previousCopy, root, copy);
                if (rebound != Status::Ok) return rebound;
            }
        }
    }
    EncodeRoot(root, copy, m_scratch);
    const size_t offset = RootOffset(root.resident, copy);
    if (!m_ops.write(m_ops.context, offset, m_scratch, ROOT_BYTES) ||
        !m_ops.sync(m_ops.context)) {
        return Status::IoFailure;
    }
    // Root encoding occupies only the scratch prefix. Verify the complete root
    // in its unused tail instead of reopening the root file for tiny chunks.
    static_assert(2U * ROOT_BYTES <= FRAME_SLOT_BYTES);
    uint8_t* const verify = m_scratch + ROOT_BYTES;
    if (!m_ops.read(m_ops.context, offset, verify, ROOT_BYTES) ||
        !std::equal(verify, verify + ROOT_BYTES, m_scratch)) {
        return Status::IoFailure;
    }
    return Status::Ok;
}

Status Layout::InvalidateRoot(const uint8_t resident,
                              const uint8_t copy) const {
    if (resident > 1U || copy > 1U || m_ops.write == nullptr ||
        m_ops.sync == nullptr || m_scratch == nullptr ||
        m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    std::fill_n(m_scratch, ROOT_BYTES, 0xffU);
    return m_ops.write(m_ops.context, RootOffset(resident, copy), m_scratch,
                       ROOT_BYTES) && m_ops.sync(m_ops.context)
        ? Status::Ok : Status::IoFailure;
}

Status Layout::ReadSlot(const size_t offset,
                        litchcraft_frame_store::Frame& out) const {
    if (m_ops.read == nullptr || m_scratch == nullptr ||
        m_scratchSize < FRAME_SLOT_BYTES ||
        !m_ops.read(m_ops.context, offset, m_scratch, FRAME_SLOT_BYTES)) {
        return Status::IoFailure;
    }
    const uint16_t length = fxe::ReadU16LE(m_scratch);
    if (length != litchcraft_frame_slot_codec::PACKED_FRAME_BYTES ||
        fxe::ReadU32LE(m_scratch + 2U) !=
            fxe::Crc32(m_scratch + 6U, length) ||
        !litchcraft_frame_slot_codec::FrameSlotCodec::Decode(
            m_scratch + 6U, length, out)) {
        return Status::Corrupt;
    }
    return Status::Ok;
}

Status Layout::ReadFrame(const Root& root, const uint16_t index,
                         litchcraft_frame_store::Frame& out) const {
    if (root.resident > 1U || index >= root.manifest.frameCount) {
        return Status::InvalidArgument;
    }
    return ReadSlot(index == root.stageIndex ? StageOffset()
        : SlotOffset(root.resident, root.slots[index]), out);
}

Status Layout::WriteEncodedSlot(const size_t offset,
                           const litchcraft_frame_store::Frame& frame) const {
    if (m_ops.write == nullptr || m_ops.read == nullptr ||
        m_ops.sync == nullptr || m_scratch == nullptr ||
        m_scratchSize < FRAME_SLOT_BYTES) {
        return Status::InvalidArgument;
    }
    size_t encoded = 0U;
    if (!litchcraft_frame_slot_codec::FrameSlotCodec::Encode(
            frame, m_scratch + 6U, m_scratchSize - 6U, encoded) ||
        encoded != litchcraft_frame_slot_codec::PACKED_FRAME_BYTES) {
        return Status::InvalidArgument;
    }
    fxe::WriteU16LE(static_cast<uint16_t>(encoded), m_scratch);
    fxe::WriteU32LE(fxe::Crc32(m_scratch + 6U, encoded), m_scratch + 2U);
    return m_ops.write(m_ops.context, offset, m_scratch, FRAME_SLOT_BYTES)
        ? Status::Ok : Status::IoFailure;
}

Status Layout::WriteSlotAt(const size_t offset,
                           const litchcraft_frame_store::Frame& frame) const {
    const Status written = WriteEncodedSlot(offset, frame);
    if (written != Status::Ok) return written;
    if (!m_ops.sync(m_ops.context)) return Status::IoFailure;
    std::array<uint8_t, VERIFY_BYTES> verify{};
    for (size_t cursor = 0U; cursor < FRAME_SLOT_BYTES; cursor += verify.size()) {
        const size_t count = std::min(verify.size(), FRAME_SLOT_BYTES - cursor);
        if (!m_ops.read(m_ops.context, offset + cursor, verify.data(), count) ||
            !std::equal(verify.begin(), verify.begin() + count,
                        m_scratch + cursor)) {
            return Status::IoFailure;
        }
    }
    return Status::Ok;
}

Status Layout::WriteSlot(const uint8_t resident, const uint8_t slot,
                         const litchcraft_frame_store::Frame& frame) const {
    if (resident > 1U) return Status::InvalidArgument;
    return WriteSlotAt(SlotOffset(resident, slot), frame);
}

Status Layout::StageIncomingSlot(const uint8_t resident, const uint8_t slot,
                                 const litchcraft_frame_store::Frame& frame) const {
    if (resident > 1U) return Status::InvalidArgument;
    return WriteEncodedSlot(SlotOffset(resident, slot), frame);
}

Status Layout::StageFrame(const litchcraft_frame_store::Frame& frame) const {
    for (uint8_t resident = 0U; resident < 2U; ++resident) {
        Root root{};
        const Status left = ReadRoot(resident, 0U, root);
        const bool leftStage = left == Status::Ok && root.stageIndex != NO_STAGE;
        const uint32_t leftSerial = left == Status::Ok ? root.serial : 0U;
        const Status right = ReadRoot(resident, 1U, root);
        const bool rightStage = right == Status::Ok && root.stageIndex != NO_STAGE;
        const uint32_t rightSerial = right == Status::Ok ? root.serial : 0U;
        if (left == Status::IoFailure || right == Status::IoFailure) {
            return Status::IoFailure;
        }
        if (leftStage) {
            if (right != Status::Ok || rightStage ||
                !Newer(rightSerial, leftSerial)) return Status::StageInUse;
            // A completed fold can lose power while retiring its prior stage
            // root. Retire it before overwriting the one reusable stage.
            const Status invalidated = InvalidateRoot(resident, 0U);
            if (invalidated != Status::Ok) return invalidated;
        }
        if (rightStage) {
            if (left != Status::Ok || leftStage ||
                !Newer(leftSerial, rightSerial)) return Status::StageInUse;
            const Status invalidated = InvalidateRoot(resident, 1U);
            if (invalidated != Status::Ok) return invalidated;
        }
    }
    return WriteSlotAt(StageOffset(), frame);
}

Status Layout::CommitFrameEdit(
    const Root& current, const uint8_t currentCopy, const uint16_t index,
    const litchcraft_frame_store::Frame& frame,
    const uint32_t nextGeneration, const Root& wireIdentity,
    Root& committedRoot,
    const litchcraft_frame_store::WireSummaries* const summaries) const {
    if (current.resident > 1U || currentCopy > 1U ||
        index >= current.manifest.frameCount ||
        current.stageIndex != NO_STAGE || current.serial == 0xffffffffU ||
        nextGeneration == 0U || !Newer(nextGeneration, current.generation) ||
        wireIdentity.bodyBytes < current.manifest.frameCount *
                                     litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES ||
        wireIdentity.bodyBytes > litchcraft_frame_store::WIRE_BODY_MAX_BYTES) {
        return Status::InvalidArgument;
    }
    uint8_t latestCopy = 0U;
    if (LatestRoot(current.resident, committedRoot, latestCopy) != Status::Ok ||
        latestCopy != currentCopy ||
        committedRoot.serial != current.serial ||
        committedRoot.generation != current.generation) {
        return Status::RevisionConflict;
    }
    const Status stage = StageFrame(frame);
    if (stage != Status::Ok) return stage;
    committedRoot = current;
    committedRoot.generation = nextGeneration;
    committedRoot.serial = current.serial + 1U;
    committedRoot.manifest.predecessorDeltas =
        wireIdentity.manifest.predecessorDeltas;
    committedRoot.bodyBytes = wireIdentity.bodyBytes;
    committedRoot.bodyCrc32 = wireIdentity.bodyCrc32;
    committedRoot.blockBytes = wireIdentity.blockBytes;
    committedRoot.blockCrc32 = wireIdentity.blockCrc32;
    committedRoot.stageIndex = index;
    return PublishRoot(committedRoot, currentCopy ^ 1U, summaries);
}

Status Layout::FoldStage(const Root& stagedRoot, const uint8_t stagedCopy,
                         litchcraft_frame_store::Frame& frameScratch,
                         Root& foldedRoot) const {
    if (stagedRoot.resident > 1U || stagedCopy > 1U ||
        stagedRoot.stageIndex >= stagedRoot.manifest.frameCount ||
        stagedRoot.serial == 0xffffffffU) {
        return Status::InvalidArgument;
    }
    const Status read = ReadFrame(stagedRoot, stagedRoot.stageIndex,
                                  frameScratch);
    if (read != Status::Ok) return read;
    std::array<bool, litchcraft_frame_store::FRAME_COUNT> used{};
    for (uint16_t index = 0U; index < stagedRoot.manifest.frameCount; ++index) {
        if (index != stagedRoot.stageIndex) {
            used[stagedRoot.slots[index]] = true;
        }
    }
    uint16_t vacant = litchcraft_frame_store::FRAME_COUNT;
    for (uint16_t slot = 0U; slot < used.size(); ++slot) {
        if (!used[slot]) {
            vacant = slot;
            break;
        }
    }
    if (vacant == litchcraft_frame_store::FRAME_COUNT) {
        return Status::Corrupt;
    }
    const Status write = WriteSlot(stagedRoot.resident,
                                   static_cast<uint8_t>(vacant), frameScratch);
    if (write != Status::Ok) return write;
    foldedRoot = stagedRoot;
    foldedRoot.slots[stagedRoot.stageIndex] = static_cast<uint8_t>(vacant);
    foldedRoot.stageIndex = NO_STAGE;
    ++foldedRoot.serial;
    const Status publish = PublishRoot(foldedRoot, stagedCopy ^ 1U);
    if (publish != Status::Ok) return publish;
    return InvalidateRoot(stagedRoot.resident, stagedCopy);
}

} // namespace modules::litchcraft_frame_store_layout
