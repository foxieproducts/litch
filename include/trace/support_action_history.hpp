#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "modules/support_runtime_stats.hpp"

namespace trace {

enum class SupportActionKind : uint8_t {
    PiecesChanged = 1U, Paint, Erase, ArtworkLoad, ArtworkSave,
    SettingChange, Hint, Undo, NewGame, Coaching, Sleep, StorageRecovery,
    EnterLitchcraft, ExitLitchcraft, SelectTool, SelectFrame, Playback,
    Fill, Clear, Redo, Effect, EditFrames,
};
enum class SupportActionOrigin : uint8_t { Board = 0U, App = 1U, Console = 2U };
enum class SupportActionOutcome : uint8_t {
    Requested = 0U, Accepted, Applied, Rejected, Failed, Canceled,
    Unconfirmed, Observed, NoChange,
};
struct SupportAction {
    uint64_t uptimeMs = 0U;
    uint32_t sequence = 0U;
    uint32_t durationMs = UINT32_MAX;
    SupportActionKind kind = SupportActionKind::PiecesChanged;
    SupportActionOrigin origin = SupportActionOrigin::Board;
    SupportActionOutcome outcome = SupportActionOutcome::Requested;
    uint8_t count = 0U;
    uint8_t secondCount = 0U;
};

// Always-on, typed RAM projection owned by TraceRecorder; independent of raw
// replay logging. No payloads, text, squares, colors or filesystem activity.
class SupportActionHistory final {
  public:
    static constexpr size_t CAPACITY = 20U;
    using Clock = uint64_t (*)();
    explicit SupportActionHistory(Clock clock = &modules::SupportUptimeMs)
        : m_clock(clock) {}
    uint32_t Begin(SupportActionKind kind,
                   SupportActionOrigin origin = SupportActionOrigin::Board,
                   uint8_t count = 0U, uint8_t secondCount = 0U) {
        ++m_sequence;
        if (m_sequence == 0U) ++m_sequence;
        auto& action = m_actions[m_next];
        action = {m_clock(), m_sequence, UINT32_MAX, kind, origin,
                  SupportActionOutcome::Requested, count, secondCount};
        m_next = (m_next + 1U) % CAPACITY;
        if (m_count < CAPACITY) ++m_count;
        else if (m_omitted != UINT32_MAX) ++m_omitted;
        return m_sequence;
    }
    void Finish(uint32_t sequence, SupportActionOutcome outcome,
                uint8_t count = 0U, uint8_t secondCount = 0U) {
        if (sequence == 0U) return;
        for (auto& action : m_actions) {
            if (action.sequence != sequence) continue;
            // An accepted action may later settle. A settled row never becomes
            // a second user action when its packet/receipt is replayed.
            if (action.outcome != SupportActionOutcome::Requested &&
                action.outcome != SupportActionOutcome::Accepted) return;
            action.outcome = outcome;
            const uint64_t now = m_clock();
            const uint64_t elapsed = now >= action.uptimeMs ? now - action.uptimeMs : UINT64_MAX;
            action.durationMs = elapsed < UINT32_MAX ? static_cast<uint32_t>(elapsed) : UINT32_MAX;
            if (count != 0U || secondCount != 0U) {
                action.count = count;
                action.secondCount = secondCount;
            }
            return;
        }
    }
    void SetKind(uint32_t sequence, SupportActionKind kind) {
        for (auto& action : m_actions) {
            if (action.sequence == sequence && (action.outcome == SupportActionOutcome::Requested ||
                action.outcome == SupportActionOutcome::Accepted)) { action.kind = kind; return; }
        }
    }
    size_t Size() const { return m_count; }
    uint32_t Omitted() const { return m_omitted; }
    const SupportAction& At(size_t index) const {
        return m_actions[(m_next + CAPACITY - m_count + index) % CAPACITY];
    }
  private:
    Clock m_clock;
    std::array<SupportAction, CAPACITY> m_actions{};
    size_t m_next = 0U;
    size_t m_count = 0U;
    uint32_t m_sequence = 0U;
    uint32_t m_omitted = 0U;
};
} // namespace trace
