#include "modules/litchcraft_store_history.hpp"

#include <cstring>

namespace modules::litchcraft_store_history {

using litchcraft_frame_store::Emitter;
using litchcraft_frame_store::Frame;

bool StoreHistory::Same(const Emitter& first, const Emitter& second) {
    return first.color.r == second.color.r &&
           first.color.g == second.color.g &&
           first.color.b == second.color.b &&
           first.effect == second.effect &&
           first.effectDurationMs == second.effectDurationMs;
}

void StoreHistory::WriteEmitter(uint8_t* const target,
                                const Emitter& emitter) {
    target[0U] = emitter.color.r;
    target[1U] = emitter.color.g;
    target[2U] = emitter.color.b;
    target[3U] = emitter.effect;
    target[4U] = static_cast<uint8_t>(emitter.effectDurationMs);
    target[5U] = static_cast<uint8_t>(emitter.effectDurationMs >> 8U);
}

Emitter StoreHistory::ReadEmitter(const uint8_t* const source) {
    Emitter emitter{};
    emitter.color = {source[0U], source[1U], source[2U]};
    emitter.effect = source[3U];
    emitter.effectDurationMs = static_cast<uint16_t>(
        source[4U] | (static_cast<uint16_t>(source[5U]) << 8U));
    return emitter;
}

void StoreHistory::Clear() {
    m_used = 0U;
    m_count = 0U;
    m_cursor = 0U;
    m_staging = false;
    m_lastStepValid = false;
}

void StoreHistory::Begin(const uint16_t frame, const Frame& displayed) {
    m_lastStepValid = false;
    m_before = displayed;
    m_stagedFrame = frame;
    m_staging = true;
}

void StoreHistory::EvictOldest() {
    if (m_count == 0U) return;
    const uint16_t length = m_operations[0U].length;
    std::memmove(m_bytes.data(), m_bytes.data() + length, m_used - length);
    m_used = static_cast<uint16_t>(m_used - length);
    for (uint8_t i = 1U; i < m_count; ++i) {
        m_operations[i - 1U] = m_operations[i];
        m_operations[i - 1U].offset = static_cast<uint16_t>(
            m_operations[i - 1U].offset - length);
    }
    --m_count;
    if (m_cursor != 0U) --m_cursor;
}

void StoreHistory::End(const uint16_t frame, const Frame& displayed) {
    if (!m_staging) return;
    m_staging = false;
    if (frame != m_stagedFrame) return;
    size_t length = 0U;
    for (size_t index = 0U; index < m_before.emitters.size(); ++index) {
        if (!Same(m_before.emitters[index], displayed.emitters[index])) {
            length += RECORD_BYTES;
        }
    }
    if (length == 0U) return;
    if (length > ARENA_BYTES) {
        Clear();
        return;
    }
    const uint16_t start = Append(Kind::Emitters, frame,
                                  static_cast<uint16_t>(length));
    uint16_t written = start;
    for (size_t index = 0U; index < m_before.emitters.size(); ++index) {
        if (Same(m_before.emitters[index], displayed.emitters[index])) continue;
        m_bytes[written++] = static_cast<uint8_t>(index);
        WriteEmitter(m_bytes.data() + written, m_before.emitters[index]);
        written = static_cast<uint16_t>(written + 6U);
    }
}

uint16_t StoreHistory::Append(const Kind kind, const uint16_t frame,
                               const uint16_t length) {
    if (m_cursor < m_count) {
        m_used = m_operations[m_cursor].offset;
        m_count = m_cursor;
    }
    while (m_count == MAX_OPERATIONS || m_used + length > ARENA_BYTES) {
        EvictOldest();
    }
    const uint16_t start = m_used;
    m_operations[m_count++] = {
        start, length, static_cast<uint16_t>((uint16_t(kind) << 8U) | frame)};
    m_used = static_cast<uint16_t>(m_used + length);
    m_cursor = m_count;
    m_lastStepValid = false;
    return start;
}

void StoreHistory::EndDuration(const uint16_t frame, const Frame& displayed) {
    if (!m_staging) return;
    m_staging = false;
    if (frame != m_stagedFrame || m_before.durationMs == displayed.durationMs) return;
    const uint16_t offset = Append(Kind::Duration, frame, 2U);
    m_bytes[offset] = static_cast<uint8_t>(m_before.durationMs);
    m_bytes[offset + 1U] = static_cast<uint8_t>(m_before.durationMs >> 8U);
}

void StoreHistory::CommitStructural(const Kind kind, const uint16_t frame) {
    m_staging = false;
    const uint16_t length = kind == Kind::Delete ? 1538U : 1U;
    const uint16_t offset = Append(kind, frame, length);
    if (kind == Kind::AddCopy) {
        m_bytes[offset] = 0U;
        return;
    }
    m_bytes[offset] = static_cast<uint8_t>(m_before.durationMs);
    m_bytes[offset + 1U] = static_cast<uint8_t>(m_before.durationMs >> 8U);
    for (size_t i = 0U; i < m_before.emitters.size(); ++i) {
        WriteEmitter(m_bytes.data() + offset + 2U + i * 6U, m_before.emitters[i]);
    }
}

bool StoreHistory::NextStructural(const bool redo, StructuralStep& out) const {
    if (m_staging || (redo ? m_cursor == m_count : m_cursor == 0U)) return false;
    const auto& operation = m_operations[redo ? m_cursor : m_cursor - 1U];
    const Kind kind = OperationKind(operation);
    if (kind != Kind::AddCopy && kind != Kind::Delete) return false;
    const uint16_t frame = OperationFrame(operation);
    out.action = kind == Kind::AddCopy ? (redo ? 0U : 1U) : (redo ? 1U : 4U);
    out.source = kind == Kind::AddCopy && redo ? frame - 1U : frame;
    return true;
}

bool StoreHistory::ReadRemovedFrame(const bool redo, Frame& out) const {
    if (m_staging || (redo ? m_cursor == m_count : m_cursor == 0U)) return false;
    const auto& operation = m_operations[redo ? m_cursor : m_cursor - 1U];
    if (OperationKind(operation) != Kind::Delete || operation.length != 1538U) return false;
    const uint8_t* bytes = m_bytes.data() + operation.offset;
    out.durationMs = static_cast<uint16_t>(bytes[0U] | (uint16_t(bytes[1U]) << 8U));
    for (size_t i = 0U; i < out.emitters.size(); ++i) {
        out.emitters[i] = ReadEmitter(bytes + 2U + i * 6U);
    }
    return true;
}

bool StoreHistory::CommitStructuralStep(const bool redo) {
    StructuralStep step{};
    if (!NextStructural(redo, step)) return false;
    m_lastStep = redo ? m_cursor++ : --m_cursor;
    m_lastStepValid = true;
    return true;
}

bool StoreHistory::SteppedNeedsRepair() const {
    return m_lastStepValid && OperationKind(m_operations[m_lastStep]) != Kind::Emitters;
}

bool StoreHistory::Step(const bool redo, const uint16_t frame,
                        Frame& displayed) {
    if (m_staging || (redo ? m_cursor == m_count : m_cursor == 0U)) {
        return false;
    }
    const uint8_t operationIndex = redo ? m_cursor : m_cursor - 1U;
    const Operation operation = m_operations[operationIndex];
    const Kind kind = OperationKind(operation);
    if (OperationFrame(operation) != frame || operation.length == 0U ||
        (kind != Kind::Emitters && kind != Kind::Duration)) return false;
    if (kind == Kind::Duration) {
        const uint16_t previous = displayed.durationMs;
        displayed.durationMs = static_cast<uint16_t>(m_bytes[operation.offset] |
            (uint16_t(m_bytes[operation.offset + 1U]) << 8U));
        m_bytes[operation.offset] = static_cast<uint8_t>(previous);
        m_bytes[operation.offset + 1U] = static_cast<uint8_t>(previous >> 8U);
    } else for (size_t offset = operation.offset;
         offset < static_cast<size_t>(operation.offset + operation.length);
         offset += RECORD_BYTES) {
        const uint8_t emitterIndex = m_bytes[offset];
        auto& current = displayed.emitters[emitterIndex];
        uint8_t replacement[6U]{};
        WriteEmitter(replacement, current);
        current = ReadEmitter(m_bytes.data() + offset + 1U);
        std::memcpy(m_bytes.data() + offset + 1U, replacement,
                    sizeof(replacement));
    }
    if (redo) ++m_cursor;
    else --m_cursor;
    m_lastStep = operationIndex;
    m_lastStepValid = true;
    return true;
}

uint16_t StoreHistory::TargetFrame(const bool redo) const {
    if (m_staging || (redo ? m_cursor == m_count : m_cursor == 0U)) {
        return UINT16_MAX;
    }
    return OperationFrame(m_operations[redo ? m_cursor : m_cursor - 1U]);
}

size_t StoreHistory::SteppedEmitterCount() const {
    return m_lastStepValid
        ? m_operations[m_lastStep].length / RECORD_BYTES : 0U;
}

bool StoreHistory::ReadSteppedEmitter(const size_t ordinal,
                                      const Frame& displayed,
                                      uint8_t& index, Emitter& emitter) const {
    if (ordinal >= SteppedEmitterCount()) return false;
    const auto& operation = m_operations[m_lastStep];
    index = m_bytes[operation.offset + ordinal * RECORD_BYTES];
    emitter = displayed.emitters[index];
    return true;
}

} // namespace modules::litchcraft_store_history
