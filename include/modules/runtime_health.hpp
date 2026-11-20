#pragma once

#include <cstdint>

#include "modules/board_input.hpp"
#include "runtime/runtime_health.hpp"

namespace modules {

// Decision for one board-input poll. A scan is dispatchable only when it is
// fresh and the reducer's current advice permits board/move ingress.
struct BoardInputHealthDecision {
    runtime::RuntimeHealthSnapshot snapshot{};
    bool dispatchObservation = false;
    bool authoritativeResync = false;
};

// Owns the board-input adapter side of runtime health. NotDue deliberately
// publishes no fact. Recovery remembers the reducer transition until one fresh
// successful scan can be dispatched as the authoritative resynchronization.
class BoardInputRuntimeHealth final {
  public:
    explicit BoardInputRuntimeHealth(
        runtime::RuntimeHealthReducer& reducer)
        : m_reducer(reducer) {}

    BoardInputHealthDecision ObserveScanAttempt(
        uint32_t nowMs,
        const BoardScanAttemptResult& attempt);

    // Consume the pending one-shot only after the canonical dispatcher accepts
    // it. A rejected dispatch is retried by the next fresh successful scan.
    void AcknowledgeAuthoritativeResync(bool accepted);

    runtime::RuntimeHealthSnapshot Snapshot() const {
        return m_reducer.Snapshot();
    }

  private:
    runtime::RuntimeHealthReducer& m_reducer;
    bool m_authoritativeResyncPending = false;
};

} // namespace modules
