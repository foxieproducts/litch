#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/litchcraft_animation.hpp"

namespace modules {

using LitchcraftHistoryCanvas =
    std::array<Rgb, litchcraft_animation::EMITTER_COUNT>;

// Reversible XOR byte runs, rather than document snapshots. The current
// operation may grow as a stroke continues; whole oldest operations are
// evicted first. Sparse edits pay one three-byte run header, while dense edits
// remain close to one byte per changed document byte.
class LitchcraftHistory final {
  public:
    // XOR history stores the compact five-byte emitters, not six-byte wire
    // records. The worst full-document run plus all descriptors fits 12 KiB;
    // litchcraft_history.cpp enforces that bound for the actual target layout.
    static constexpr size_t BYTE_CAPACITY = 12U * 1024U;
    static constexpr size_t MIN_TYPICAL_UNDO_STEPS = 50U;
    static constexpr size_t OP_CAPACITY = 64U;
    bool Record(litchcraft_animation::Document& before,
                const litchcraft_animation::Document& after,
                uint8_t beforeFrame, uint8_t afterFrame, bool extend);
    bool Record(LitchcraftHistoryCanvas& before,
                const LitchcraftHistoryCanvas& after, bool extend);
    bool Undo(litchcraft_animation::Document& document, uint8_t& frame);
    bool Redo(litchcraft_animation::Document& document, uint8_t& frame);
    bool Undo(LitchcraftHistoryCanvas& canvas);
    bool Redo(LitchcraftHistoryCanvas& canvas);
    uint8_t UndoCount() const { return m_cursor; }
    uint8_t RedoCount() const { return m_count - m_cursor; }
    size_t BytesUsed() const { return m_used + sizeof(m_operations); }

  private:
    struct Operation {
        uint16_t offset = 0U;
        uint16_t size = 0U;
        uint8_t beforeFrame = 0U;
        uint8_t afterFrame = 0U;
    };
    static constexpr size_t MAX_RUN_BYTES = 255U;
    void Apply(const Operation& operation, uint8_t* target) const;
    bool RecordBytes(uint8_t* before, const uint8_t* after, size_t size,
                     uint8_t beforeFrame, uint8_t afterFrame, bool extend);
    size_t Capacity() const {
        return m_bytes.size();
    }
    void EvictOldest();
    uint8_t& Byte(size_t index) {
        return m_bytes[index];
    }
    const uint8_t& Byte(size_t index) const {
        return m_bytes[index];
    }
    std::array<Operation, OP_CAPACITY> m_operations{};
    std::array<uint8_t, BYTE_CAPACITY - sizeof(m_operations) - 4U> m_bytes{};
    uint16_t m_used = 0U;
    uint8_t m_count = 0U;
    uint8_t m_cursor = 0U;
};
static_assert(sizeof(LitchcraftHistory) <= LitchcraftHistory::BYTE_CAPACITY,
              "static and advanced history share one fixed 12KiB arena");
} // namespace modules
