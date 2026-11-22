#include "modules/litchcraft_history.hpp"

#include <algorithm>
#include <type_traits>

namespace modules {
using litchcraft_animation::Document;
static_assert(std::is_trivially_copyable<Document>::value,
              "history patches only trivially copyable document storage");
static_assert(sizeof(Document) + ((sizeof(Document) + 254U) / 255U) * 3U +
                  LitchcraftHistory::OP_CAPACITY * 6U + 4U <= LitchcraftHistory::BYTE_CAPACITY,
              "every operation must fit its own undo within the byte budget");

void LitchcraftHistory::Apply(const Operation& operation, uint8_t* const target) const {
    for (size_t cursor = operation.offset;
         cursor < operation.offset + operation.size;) {
        const size_t offset = size_t(Byte(cursor)) |
                              (size_t(Byte(cursor + 1U)) << 8U);
        const size_t count = Byte(cursor + 2U);
        cursor += 3U;
        for (size_t i = 0U; i < count; ++i) target[offset + i] ^= Byte(cursor++);
    }
}

void LitchcraftHistory::EvictOldest() {
    const auto size = m_operations[0].size;
    for (size_t i = 0U; i < m_used - size; ++i) Byte(i) = Byte(i + size);
    m_used -= size;
    for (uint8_t i = 1U; i < m_count; ++i) {
        m_operations[i - 1U] = m_operations[i];
        m_operations[i - 1U].offset -= size;
    }
    --m_count;
    --m_cursor;
}

bool LitchcraftHistory::Record(Document& before, const Document& after,
                               uint8_t beforeFrame, const uint8_t afterFrame,
                               const bool extend) {
    return RecordBytes(reinterpret_cast<uint8_t*>(&before),
                       reinterpret_cast<const uint8_t*>(&after),
                       sizeof(Document), beforeFrame, afterFrame, extend);
}

bool LitchcraftHistory::Record(LitchcraftHistoryCanvas& before,
                               const LitchcraftHistoryCanvas& after,
                               const bool extend) {
    return RecordBytes(reinterpret_cast<uint8_t*>(&before),
                       reinterpret_cast<const uint8_t*>(&after),
                       sizeof(LitchcraftHistoryCanvas), 0U, 0U, extend);
}

bool LitchcraftHistory::RecordBytes(uint8_t* const a, const uint8_t* const b,
                                    const size_t size, uint8_t beforeFrame,
                                    const uint8_t afterFrame,
                                    const bool extend) {
    if (m_cursor < m_count) m_used = m_operations[m_cursor].offset;
    m_count = m_cursor;
    if (extend && m_count != 0U) {
        const auto previous = m_operations[m_count - 1U];
        Apply(previous, a);
        beforeFrame = previous.beforeFrame;
        m_used = previous.offset;
        --m_count;
        --m_cursor;
    }
    size_t required = 0U;
    for (size_t offset = 0U; offset < size;) {
        while (offset < size && a[offset] == b[offset]) ++offset;
        if (offset == size) break;
        size_t count = 0U;
        while (offset + count < size &&
               a[offset + count] != b[offset + count] &&
               count < MAX_RUN_BYTES) {
            ++count;
        }
        required += count + 3U;
        offset += count;
    }
    if (required == 0U) return false;
    // Highly fragmented differences can cost four bytes per changed byte.
    // A dense XOR with zeroes for unchanged bytes is always reversible and
    // gives every possible document edit a hard upper storage bound.
    const size_t denseRequired = size + ((size + MAX_RUN_BYTES - 1U) / MAX_RUN_BYTES) * 3U;
    const bool dense = required > denseRequired;
    if (dense) required = denseRequired;
    if (required > Capacity()) return false;
    while (m_count == OP_CAPACITY || m_used + required > Capacity()) EvictOldest();
    m_operations[m_count++] = {m_used, static_cast<uint16_t>(required), beforeFrame, afterFrame};
    for (size_t offset = 0U; offset < size;) {
        while (!dense && offset < size && a[offset] == b[offset]) ++offset;
        if (offset == size) break;
        size_t count = 0U;
        while (offset + count < size &&
               (dense || a[offset + count] != b[offset + count]) &&
               count < MAX_RUN_BYTES) {
            ++count;
        }
        Byte(m_used++) = static_cast<uint8_t>(offset);
        Byte(m_used++) = static_cast<uint8_t>(offset >> 8U);
        Byte(m_used++) = static_cast<uint8_t>(count);
        for (size_t i = 0U; i < count; ++i) Byte(m_used++) = a[offset + i] ^ b[offset + i];
        offset += count;
    }
    m_cursor = m_count;
    return true;
}

bool LitchcraftHistory::Undo(Document& document, uint8_t& frame) {
    if (m_cursor == 0U) return false;
    const auto& operation = m_operations[--m_cursor];
    Apply(operation, reinterpret_cast<uint8_t*>(&document));
    frame = operation.beforeFrame;
    return true;
}
bool LitchcraftHistory::Redo(Document& document, uint8_t& frame) {
    if (m_cursor == m_count) return false;
    const auto& operation = m_operations[m_cursor++];
    Apply(operation, reinterpret_cast<uint8_t*>(&document));
    frame = operation.afterFrame;
    return true;
}
bool LitchcraftHistory::Undo(LitchcraftHistoryCanvas& canvas) {
    if (m_cursor == 0U) return false;
    Apply(m_operations[--m_cursor], reinterpret_cast<uint8_t*>(&canvas));
    return true;
}
bool LitchcraftHistory::Redo(LitchcraftHistoryCanvas& canvas) {
    if (m_cursor == m_count) return false;
    Apply(m_operations[m_cursor++], reinterpret_cast<uint8_t*>(&canvas));
    return true;
}
} // namespace modules
