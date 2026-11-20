#include "modules/litchcraft_frame_stream.hpp"

#include <algorithm>

#include "foxie_embedded/millis.hpp"

namespace modules::litchcraft_frame_stream {

bool FrameStream::BeginHistoryStroke(const uint16_t frameIndex) {
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    const bool ready = !m_replacementReserved && slot < m_frames.size() &&
        m_indices[slot].load(std::memory_order_acquire) == frameIndex &&
        m_generations[slot].load(std::memory_order_acquire) == Generation();
    if (ready) {
        m_history.Begin(frameIndex, m_frames[slot]);
        m_historyFrame = frameIndex;
    }
    UnlockPublication();
    return ready;
}

FrameStream::GuardedEditResult FrameStream::ClearDisplayedIfRevision(
    const uint32_t expectedRevision, const uint16_t frameIndex) {
    return EditDisplayedCanvasIfRevision(
        {Generation(), expectedRevision, frameIndex, 15U, {}, CanvasAction::Erase});
}

FrameStream::GuardedEditResult FrameStream::EditDisplayedCanvasIfRevision(
    const CanvasEdit& edit, uint32_t* const resultingRevision) {
    const uint8_t turns = static_cast<uint8_t>(edit.action) >= 2U &&
        static_cast<uint8_t>(edit.action) <= 4U
        ? static_cast<uint8_t>(edit.action) - 1U : 0U;
    const bool erase = edit.action == CanvasAction::Erase ||
        edit.action == CanvasAction::LightErase ||
        edit.action == CanvasAction::DarkErase;
    const bool checker = edit.action == CanvasAction::LightFill ||
        edit.action == CanvasAction::DarkFill ||
        edit.action == CanvasAction::LightErase ||
        edit.action == CanvasAction::DarkErase;
    if ((!erase && !checker && turns == 0U && edit.action != CanvasAction::Fill) ||
        edit.mask == 0U || edit.mask > 15U ||
        ((edit.action == CanvasAction::Erase || turns != 0U) && edit.mask != 15U) ||
        ((erase || turns != 0U) && (edit.color.r != 0U || edit.color.g != 0U ||
                   edit.color.b != 0U))) return GuardedEditResult::Conflict;
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    const bool ready = !m_replacementReserved &&
        (m_historyFrame == NO_FRAME || m_historyFrame == edit.frame) &&
        edit.revision != 0U && AcceptsGeneration(edit.generation) &&
        m_liveRevision.load(std::memory_order_relaxed) == edit.revision &&
        m_requested.load(std::memory_order_acquire) == edit.frame &&
        slot < m_frames.size() &&
        m_indices[slot].load(std::memory_order_acquire) == edit.frame &&
        m_generations[slot].load(std::memory_order_acquire) == Generation();
    if (!ready) {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    auto& frame = m_frames[slot];
    // Admission must succeed before an app action ends a resting brush stroke.
    // Keep its inverse separate, then reuse the same preimage for this action.
    if (m_historyFrame != NO_FRAME) {
        m_history.End(m_historyFrame, frame);
        m_historyFrame = NO_FRAME;
    }
    m_history.Begin(edit.frame, frame);
    bool changed = false;
    if (turns != 0U) {
        const auto destination = [turns](const size_t index) {
            size_t square = index / 4U;
            for (uint8_t turn = 0U; turn < turns; ++turn) {
                square = (square % 8U) * 8U + 7U - square / 8U;
            }
            return square * 4U + (index % 4U + turns) % 4U;
        };
        // Every orbit is at most four complete emitters. Only its lowest
        // index moves it, so no visited bitmap or frame scratch is needed.
        for (size_t start = 0U; start < frame.emitters.size(); ++start) {
            size_t cursor = destination(start);
            while (cursor > start) cursor = destination(cursor);
            if (cursor != start) continue;
            auto carried = frame.emitters[start];
            cursor = start;
            do {
                const size_t next = destination(cursor);
                auto& target = frame.emitters[next];
                changed = changed || target.color.r != carried.color.r ||
                    target.color.g != carried.color.g || target.color.b != carried.color.b ||
                    target.effect != carried.effect ||
                    target.effectDurationMs != carried.effectDurationMs;
                const auto displaced = target;
                target = carried;
                carried = displaced;
                cursor = next;
            } while (cursor != start);
        }
    } else for (size_t index = 0U; index < frame.emitters.size(); ++index) {
        if ((edit.mask & (1U << (index % 4U))) == 0U) continue;
        if (checker) {
            const size_t square = index / 4U;
            const bool dark = ((square / 8U + square % 8U) & 1U) != 0U;
            const bool targetDark = edit.action == CanvasAction::DarkFill ||
                edit.action == CanvasAction::DarkErase;
            if (dark != targetDark) continue;
        }
        auto& emitter = frame.emitters[index];
        changed = changed || emitter.color.r != edit.color.r ||
            emitter.color.g != edit.color.g || emitter.color.b != edit.color.b ||
            (erase && (emitter.effect != 0U || emitter.effectDurationMs != 0U));
        emitter.color = edit.color;
        if (erase) {
            emitter.effect = 0U;
            emitter.effectDurationMs = 0U;
        }
    }
    m_history.End(edit.frame, frame);
    if (changed) {
        m_lastEditAtMs.store(fxe::Millis(), std::memory_order_release);
        RecordEdit(slot);
        AdvanceLiveRevision(litchcraft_frame_store::MutationScope::Frame, m_indices[slot].load());
    }
    if (resultingRevision != nullptr) {
        *resultingRevision = m_liveRevision.load(std::memory_order_relaxed);
    }
    UnlockPublication();
    return changed ? GuardedEditResult::Changed : GuardedEditResult::Unchanged;
}

FrameStream::GuardedEditResult FrameStream::EditDisplayedDurationIfRevision(
    const FrameEdit& edit, uint32_t* const resultingRevision) {
    if (edit.action != FrameEditAction::Duration || edit.durationMs < 50U ||
        edit.durationMs > 5000U) return GuardedEditResult::Conflict;
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    const bool ready = !m_replacementReserved &&
        m_structuralEdit.result == StructuralResult::None &&
        edit.revision != 0U && AcceptsGeneration(edit.generation) &&
        m_liveRevision.load(std::memory_order_relaxed) == edit.revision &&
        m_requested.load(std::memory_order_acquire) == edit.source &&
        slot < m_frames.size() && m_indices[slot].load() == edit.source &&
        m_generations[slot].load() == Generation() &&
        (m_historyFrame == NO_FRAME || m_historyFrame == edit.source);
    if (!ready) { UnlockPublication(); return GuardedEditResult::Conflict; }
    auto& frame = m_frames[slot];
    if (m_historyFrame != NO_FRAME) {
        m_history.End(m_historyFrame, frame);
        m_historyFrame = NO_FRAME;
    }
    m_history.Begin(edit.source, frame);
    const bool changed = frame.durationMs != edit.durationMs;
    frame.durationMs = edit.durationMs;
    m_history.EndDuration(edit.source, frame);
    if (changed) {
        m_lastEditAtMs.store(fxe::Millis(), std::memory_order_release);
        RecordEdit(slot);
        AdvanceLiveRevision(litchcraft_frame_store::MutationScope::Frame, m_indices[slot].load());
    }
    if (resultingRevision != nullptr) *resultingRevision = m_liveRevision.load();
    UnlockPublication();
    return changed ? GuardedEditResult::Changed : GuardedEditResult::Unchanged;
}

bool FrameStream::HistoryIsStructural(const bool redo) const {
    LockPublication();
    litchcraft_store_history::StoreHistory::StructuralStep step{};
    const bool result = m_history.NextStructural(redo, step);
    UnlockPublication();
    return result;
}

bool FrameStream::SteppedNeedsRepair(const uint32_t expectedRevision) const {
    LockPublication();
    const bool result = m_liveRevision.load() == expectedRevision &&
        m_history.SteppedNeedsRepair();
    UnlockPublication();
    return result;
}

void FrameStream::EndHistoryStroke() {
    LockPublication();
    if (m_historyFrame != NO_FRAME) {
        const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
        const bool ready = slot < m_frames.size() &&
            m_indices[slot].load(std::memory_order_acquire) == m_historyFrame &&
            m_generations[slot].load(std::memory_order_acquire) == Generation();
        m_history.End(ready ? m_historyFrame : NO_FRAME,
                      ready ? m_frames[slot] : m_frames[0U]);
        m_historyFrame = NO_FRAME;
    }
    UnlockPublication();
}

FrameStream::GuardedEditResult FrameStream::StepHistoryIfRevision(
    const uint32_t expectedRevision, const uint16_t frameIndex,
    const bool redo) {
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    const bool ready = !m_replacementReserved && expectedRevision != 0U &&
        m_liveRevision.load(std::memory_order_relaxed) == expectedRevision &&
        slot < m_frames.size() &&
        m_indices[slot].load(std::memory_order_acquire) == frameIndex &&
        m_generations[slot].load(std::memory_order_acquire) == Generation();
    if (!ready) {
        UnlockPublication();
        return GuardedEditResult::Conflict;
    }
    if (!m_history.Step(redo, frameIndex, m_frames[slot])) {
        UnlockPublication();
        return GuardedEditResult::Unchanged;
    }
    m_lastEditAtMs.store(fxe::Millis(), std::memory_order_release);
    RecordEdit(slot);
    AdvanceLiveRevision(litchcraft_frame_store::MutationScope::Frame, frameIndex);
    UnlockPublication();
    return GuardedEditResult::Changed;
}

uint16_t FrameStream::HistoryTargetFrame(const bool redo) const {
    LockPublication();
    const uint16_t frame = m_history.TargetFrame(redo);
    UnlockPublication();
    return frame;
}

size_t FrameStream::SteppedEmitterCount(
    const uint32_t expectedRevision) const {
    LockPublication();
    const size_t count = m_liveRevision.load(std::memory_order_relaxed) ==
        expectedRevision ? m_history.SteppedEmitterCount() : 0U;
    UnlockPublication();
    return count;
}

bool FrameStream::CopySteppedEmitter(
    const uint32_t expectedRevision, const size_t ordinal,
    uint8_t& index, litchcraft_frame_store::Emitter& emitter) const {
    LockPublication();
    const uint8_t slot = m_activeSlot.load(std::memory_order_acquire);
    const bool ready = expectedRevision != 0U &&
        m_liveRevision.load(std::memory_order_relaxed) == expectedRevision &&
        slot < m_frames.size() &&
        m_history.ReadSteppedEmitter(ordinal, m_frames[slot], index, emitter);
    UnlockPublication();
    return ready;
}

uint8_t FrameStream::UndoCount() const {
    LockPublication();
    const uint8_t count = m_history.UndoCount();
    UnlockPublication();
    return count;
}

uint8_t FrameStream::RedoCount() const {
    LockPublication();
    const uint8_t count = m_history.RedoCount();
    UnlockPublication();
    return count;
}

} // namespace modules::litchcraft_frame_stream
