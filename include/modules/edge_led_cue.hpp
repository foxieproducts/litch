#pragma once

#include <cstdint>

#include "modules/ui_types.hpp"

namespace modules {

enum class EdgeLedCueKind : uint8_t {
    None = 0,
    Unavailable,
    OccupiedBoardBlocked,
    SetupComplete,
};

struct EdgeLedCueIntent {
    EdgeLedCueKind kind = EdgeLedCueKind::None;
    UiOwner owner = UiOwner::None;
    uint32_t startMs = 0U;

    bool IsActive() const { return kind != EdgeLedCueKind::None; }
};

class EdgeLedCueController final {
  public:
    static constexpr uint32_t UNAVAILABLE_DURATION_MS = 480U;
    static constexpr uint32_t OCCUPIED_BOARD_BLOCKED_DURATION_MS = 1000U;
    static constexpr uint32_t SETUP_COMPLETE_DURATION_MS = 1800U;
    static constexpr uint32_t SETUP_COMPLETE_HANDOFF_MS = 240U;

    static constexpr uint8_t SetupCompleteLayerAlpha255(
        const uint32_t elapsedMs) {
        if (elapsedMs >= SETUP_COMPLETE_DURATION_MS) {
            return 0U;
        }
        const uint32_t handoffStartMs =
            SETUP_COMPLETE_DURATION_MS - SETUP_COMPLETE_HANDOFF_MS;
        if (elapsedMs <= handoffStartMs) {
            return 255U;
        }
        const uint32_t remainingMs =
            SETUP_COMPLETE_DURATION_MS - elapsedMs;
        return static_cast<uint8_t>(
            (remainingMs * 255U + (SETUP_COMPLETE_HANDOFF_MS / 2U)) /
            SETUP_COMPLETE_HANDOFF_MS);
    }

    void Show(const EdgeLedCueKind kind,
              const UiOwner owner,
              const uint32_t nowMs) {
        if (kind == EdgeLedCueKind::None) {
            Clear();
            return;
        }
        m_kind = kind;
        m_owner = owner;
        m_startMs = nowMs;
    }

    void Clear() {
        m_kind = EdgeLedCueKind::None;
        m_owner = UiOwner::None;
        m_startMs = 0U;
    }

    EdgeLedCueIntent Current(const uint32_t nowMs) const {
        if (m_kind == EdgeLedCueKind::None ||
            static_cast<uint32_t>(nowMs - m_startMs) >=
                DurationMs(m_kind)) {
            return EdgeLedCueIntent{};
        }
        return EdgeLedCueIntent{m_kind, m_owner, m_startMs};
    }

  private:
    static constexpr uint32_t DurationMs(const EdgeLedCueKind kind) {
        switch (kind) {
            case EdgeLedCueKind::Unavailable:
                return UNAVAILABLE_DURATION_MS;
            case EdgeLedCueKind::OccupiedBoardBlocked:
                return OCCUPIED_BOARD_BLOCKED_DURATION_MS;
            case EdgeLedCueKind::SetupComplete:
                return SETUP_COMPLETE_DURATION_MS;
            case EdgeLedCueKind::None:
            default:
                return 0U;
        }
    }

    EdgeLedCueKind m_kind = EdgeLedCueKind::None;
    UiOwner m_owner = UiOwner::None;
    uint32_t m_startMs = 0U;
};

} // namespace modules
