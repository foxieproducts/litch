#pragma once

#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_frame_store_layout.hpp"

namespace modules::litchcraft_fixed_slot_wire_view {

// Reconstructs the one schema-4 compressed body from independent on-flash
// frames. The caller lends the record page and two existing frame buffers.
class WireView final {
  public:
    using Frame = litchcraft_frame_store::Frame;
    using Root = litchcraft_frame_store_layout::Root;
    using Layout = litchcraft_frame_store_layout::Layout;
    using Status = litchcraft_frame_store_layout::Status;

    struct Cursor final {
        uint32_t streamOffset = 0U;
        size_t recordSize = 0U;
        size_t recordRead = 0U;
        uint16_t nextFrame = 0U;
    };

    WireView(const Layout& storage, const Root& root, uint8_t* record,
             size_t recordCapacity, Frame& previous, Frame& current,
             uint16_t overrideIndex = litchcraft_frame_store::FRAME_COUNT,
             const Frame* overrideFrame = nullptr)
        : m_storage(storage), m_root(root), m_record(record),
          m_recordCapacity(recordCapacity), m_previous(previous),
          m_current(current), m_overrideIndex(overrideIndex),
          m_overrideFrame(overrideFrame) {}

    Status Measure(uint32_t& bodyBytes, uint32_t& bodyCrc32);
    Status MeasureBlocks(Root& identity, bool (*continueWork)(void*) = nullptr, void* context = nullptr);
    Status MeasureChangedBlock(uint16_t editedIndex, Root& identity);
    Status MeasureSummaries(litchcraft_frame_store::WireSummaries& summaries,
                            Root& identity, bool (*continueWork)(void*) = nullptr, void* context = nullptr);
    Status MeasureEditedSummaries(
        litchcraft_frame_store::WireSummaries& summaries,
        const litchcraft_frame_store::WireSummaryEdits& edits, Root& identity,
        bool (*continueWork)(void*) = nullptr, void* context = nullptr);
    static Status CombineSummaries(
        const litchcraft_frame_store::WireSummaries& summaries, Root& identity);
    Status Read(uint32_t offset, uint8_t* out, size_t capacity,
                size_t& outSize);
    Cursor CaptureCursor() const;
    bool RestoreCursor(const Cursor& cursor);
    void Reset();

  private:
    Status LoadNextRecord();
    Status LoadFrame(uint16_t index, Frame& out) const;
    Status MeasureRecord(const Frame* previous, const Frame& current,
                         uint16_t& bytes, uint32_t& crc);
    Status Advance(size_t count, uint8_t* out);
    static void CombineBlocks(Root& identity);

    const Layout& m_storage;
    const Root& m_root;
    uint8_t* m_record = nullptr;
    size_t m_recordCapacity = 0U;
    Frame& m_previous;
    Frame& m_current;
    uint16_t m_overrideIndex = litchcraft_frame_store::FRAME_COUNT;
    const Frame* m_overrideFrame = nullptr;
    uint32_t m_streamOffset = 0U;
    size_t m_recordSize = 0U;
    size_t m_recordRead = 0U;
    uint16_t m_nextFrame = 0U;
};

} // namespace modules::litchcraft_fixed_slot_wire_view
