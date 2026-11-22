#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_frame_slot_codec.hpp"

namespace modules::litchcraft_frame_store_layout {

inline constexpr uint16_t NO_STAGE = 0xffffU;
inline constexpr uint16_t WIRE_BLOCK_FRAMES =
    litchcraft_frame_store::KEYFRAME_INTERVAL;
inline constexpr size_t WIRE_BLOCK_COUNT =
    litchcraft_frame_store::FRAME_COUNT / WIRE_BLOCK_FRAMES;
inline constexpr size_t ROOT_BYTES = 384U;
inline constexpr size_t FRAME_SLOT_BYTES =
    litchcraft_frame_store::WIRE_RECORD_HEADER_BYTES +
    litchcraft_frame_slot_codec::PACKED_FRAME_BYTES;
inline constexpr size_t BODY_BANK_BYTES =
    litchcraft_frame_store::FRAME_COUNT * FRAME_SLOT_BYTES;
inline constexpr size_t ROOT0_FILE_BYTES = 2U * ROOT_BYTES + FRAME_SLOT_BYTES;
inline constexpr size_t ROOT1_FILE_BYTES = 2U * ROOT_BYTES;
inline constexpr size_t FRAME_FILES_CAPACITY =
    ROOT0_FILE_BYTES + ROOT1_FILE_BYTES + 2U * BODY_BANK_BYTES;
inline constexpr size_t SUMMARY_RESIDENT_BYTES =
    2U * litchcraft_frame_store::WIRE_SUMMARY_PAGE_BYTES;
inline constexpr size_t ARTWORK_FILE_CAPACITY =
    FRAME_FILES_CAPACITY + 2U * SUMMARY_RESIDENT_BYTES;

using Root = litchcraft_frame_store::FixedRoot;

enum class Status : uint8_t {
    Ok,
    Blank,
    Corrupt,
    IoFailure,
    InvalidArgument,
    RevisionConflict,
    StageInUse,
    PrelaunchLC5, // One-time unreleased LC5-to-LC6 reset; never a generic fallback.
};

// The storage layout is independent of the compressed Store wire body. A
// caller owns its wire identity and passes it in the root after validation.
class Layout final {
  public:
    using Ops = litchcraft_frame_store::FrameStore::Ops;
    Layout(Ops ops, uint8_t* scratch, size_t scratchSize)
        : m_ops(ops), m_scratch(scratch), m_scratchSize(scratchSize) {}

    static constexpr size_t RootOffset(uint8_t resident, uint8_t copy) {
        return resident == 0U ? size_t(copy) * ROOT_BYTES
            : ROOT0_FILE_BYTES + size_t(copy) * ROOT_BYTES;
    }
    static constexpr size_t StageOffset() { return 2U * ROOT_BYTES; }
    static constexpr size_t BodyOffset(uint8_t resident) {
        return ROOT0_FILE_BYTES + ROOT1_FILE_BYTES +
               size_t(resident) * BODY_BANK_BYTES;
    }
    static constexpr size_t SlotOffset(uint8_t resident, uint8_t slot) {
        return BodyOffset(resident) + size_t(slot) * FRAME_SLOT_BYTES;
    }
    static constexpr size_t SummaryOffset(uint8_t resident, uint8_t copy) {
        return FRAME_FILES_CAPACITY + size_t(resident) * SUMMARY_RESIDENT_BYTES +
            size_t(copy) * litchcraft_frame_store::WIRE_SUMMARY_PAGE_BYTES;
    }

    Status ReadRoot(uint8_t resident, uint8_t copy, Root& out) const;
    Status LatestRoot(uint8_t resident, Root& out, uint8_t& copy) const;
    Status PublishRoot(const Root& root, uint8_t copy,
        const litchcraft_frame_store::WireSummaries* summaries = nullptr) const;
    Status ReadWireSummaries(const Root& root, uint8_t copy,
        litchcraft_frame_store::WireSummaries& out) const;
    Status WriteWireSummaries(const Root& root, uint8_t copy,
        const litchcraft_frame_store::WireSummaries& summaries) const;
    Status InvalidateRoot(uint8_t resident, uint8_t copy) const;
    Status StageFrame(const litchcraft_frame_store::Frame& frame) const;
    Status CommitFrameEdit(const Root& current, uint8_t currentCopy,
                           uint16_t index,
                           const litchcraft_frame_store::Frame& frame,
                           uint32_t nextGeneration,
                           const Root& wireIdentity,
                           Root& committedRoot,
                           const litchcraft_frame_store::WireSummaries*
                               summaries = nullptr) const;
    Status ReadFrame(const Root& root, uint16_t index,
                     litchcraft_frame_store::Frame& out) const;
    Status WriteSlot(uint8_t resident, uint8_t slot,
                     const litchcraft_frame_store::Frame& frame) const;
    // Only unpublished wire ingress may defer sync and readback to its
    // Checkpoint/PrepareCommit barriers. Edit, Stage and Fold stay durable.
    Status StageIncomingSlot(uint8_t resident, uint8_t slot,
                             const litchcraft_frame_store::Frame& frame) const;
    Status FoldStage(const Root& stagedRoot, uint8_t stagedCopy,
                     litchcraft_frame_store::Frame& frameScratch,
                     Root& foldedRoot) const;

  private:
    Status ReadSummaryPage(const Root& root, uint8_t copy,
        litchcraft_frame_store::WireSummaries* out) const;
    Status RebindSummaryPage(const Root& previous, uint8_t previousCopy,
                            const Root& root, uint8_t copy) const;
    void EncodeSummaryHeader(const Root& root, uint8_t copy,
                             uint8_t* out) const;
    Status ReadSlot(size_t offset, litchcraft_frame_store::Frame& out) const;
    Status WriteEncodedSlot(size_t offset,
                            const litchcraft_frame_store::Frame& frame) const;
    Status WriteSlotAt(size_t offset,
                       const litchcraft_frame_store::Frame& frame) const;
    Ops m_ops{};
    uint8_t* m_scratch = nullptr;
    size_t m_scratchSize = 0U;
};

static_assert(FRAME_FILES_CAPACITY == 564297U,
              "fixed-slot roots, stage and body offsets are unchanged");
static_assert(ARTWORK_FILE_CAPACITY == litchcraft_frame_store::ARTWORK_FILE_CAPACITY);

} // namespace modules::litchcraft_frame_store_layout
