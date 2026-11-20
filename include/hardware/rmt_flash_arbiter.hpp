#pragma once

#include <atomic>
#include <cstdint>

#include "foxie_embedded/millis.hpp"

namespace hardware {

// Current-transfer exclusion for physical flash primitives and RMT output.
// Only already-ready physical work participates in admission. Contending
// transfers alternate; neither owner requests future animation progress.
class RmtFlashArbiter final {
  public:
    struct FlashDiagnostics {
        uint32_t accesses = 0U;
        uint32_t lastHoldMs = 0U;
        uint32_t maxHoldMs = 0U;
    };
    // Individual RAM observations, not a transaction receipt. Acquisition
    // waits are excluded from flash hold duration.
    FlashDiagnostics GetFlashDiagnostics() const {
        return {m_flashAccesses.load(), m_lastFlashHoldMs.load(),
                m_maxFlashHoldMs.load()};
    }
    uint32_t CompletedRenderGeneration() const {
        return m_completedRenderGeneration.load(std::memory_order_acquire);
    }

    uint32_t NextRenderGeneration() const {
        return CompletedRenderGeneration() + 1U;
    }

    bool TryBeginRender() {
        uint32_t state = m_lease.load(std::memory_order_acquire);
        if ((state & (FLASH_ACTIVE | RENDER_ACTIVE | RENDER_WAITER)) != 0U) {
            return false;
        }
        // An unconfirmed transfer may retry to prove hardware quiescence;
        // ordinary new transfers yield to a published flash request.
        if (state >= FLASH_WAITER && (state & RENDER_UNCERTAIN) == 0U) return false;
        return m_lease.compare_exchange_strong(
            state, state | RENDER_ACTIVE,
            std::memory_order_acq_rel);
    }

    // There is one output owner. Publish only while Show has a complete frame
    // ready, and cancel an unconsumed request before that owner stops waiting.
    void RequestRender() { m_lease.fetch_or(RENDER_WAITER, std::memory_order_acq_rel); }
    void CancelRenderRequest() { m_lease.fetch_and(~RENDER_WAITER, std::memory_order_acq_rel); }

    bool TryBeginRequestedRender() {
        uint32_t state = m_lease.load(std::memory_order_acquire);
        while ((state & RENDER_WAITER) != 0U &&
               (state & (FLASH_ACTIVE | RENDER_ACTIVE)) == 0U) {
            if ((state & RENDER_UNCERTAIN) == 0U &&
                state >= FLASH_WAITER && (state & FLASH_TURN) != 0U) return false;
            const uint32_t next =
                (state & ~(RENDER_WAITER | FLASH_TURN)) | RENDER_ACTIVE;
            if (m_lease.compare_exchange_weak(state, next,
                                             std::memory_order_acq_rel)) return true;
        }
        return false;
    }

    void EndRender(const bool completed = true) {
        if (completed) {
            m_completedRenderGeneration.fetch_add(1U,
                                                  std::memory_order_acq_rel);
        }
        uint32_t state = m_lease.load(std::memory_order_acquire);
        uint32_t next;
        do {
            next = state & ~(RENDER_ACTIVE | RENDER_UNCERTAIN | FLASH_TURN);
            if (!completed) next |= RENDER_UNCERTAIN;
            else if (state >= FLASH_WAITER) next |= FLASH_TURN;
        } while (!m_lease.compare_exchange_weak(state, next,
                                               std::memory_order_acq_rel));
    }

    bool TryBeginFlash(const uint32_t minimumRenderGeneration) {
        return TryActivateFlash(minimumRenderGeneration);
    }

    // Each synchronous SDK caller publishes once, then consumes exactly its
    // own request on acquisition. The count shares the render/flash CAS word.
    void RequestFlash() { m_lease.fetch_add(FLASH_WAITER, std::memory_order_acq_rel); }
    // Only the caller that published an unconsumed request may cancel it.
    // Subtract one count; never release another owner's active flash bit or
    // clear the RMT quarantine. Atomic arithmetic preserves other waiters.
    void CancelFlashRequest() {
        uint32_t state = m_lease.load(std::memory_order_acquire);
        uint32_t next;
        do {
            next = state - FLASH_WAITER;
            if (next < FLASH_WAITER) next &= ~FLASH_TURN;
        } while (!m_lease.compare_exchange_weak(state, next,
                                               std::memory_order_acq_rel));
    }
    bool TryBeginRequestedFlash() {
        uint32_t state = m_lease.load(std::memory_order_acquire);
        while (state >= FLASH_WAITER &&
               (state & (RENDER_ACTIVE | RENDER_UNCERTAIN | FLASH_ACTIVE)) == 0U) {
            if ((state & RENDER_WAITER) != 0U &&
                (state & FLASH_TURN) == 0U) return false;
            if (m_lease.compare_exchange_weak(
                    state, ((state - FLASH_WAITER) & ~FLASH_TURN) | FLASH_ACTIVE,
                    std::memory_order_acq_rel)) {
                RecordFlashStart();
                return true;
            }
        }
        return false;
    }

    void EndFlash() {
        if ((m_lease.load(std::memory_order_acquire) & FLASH_ACTIVE) != 0U) {
            RecordFlashHold();
        }
        m_lease.fetch_and(~(FLASH_ACTIVE | FLASH_TURN),
                          std::memory_order_release);
    }

  private:
    static constexpr uint32_t RENDER_ACTIVE = 1U;
    static constexpr uint32_t RENDER_UNCERTAIN = 2U;
    static constexpr uint32_t FLASH_ACTIVE = 4U;
    static constexpr uint32_t RENDER_WAITER = 8U;
    static constexpr uint32_t FLASH_TURN = 16U;
    static constexpr uint32_t FLASH_WAITER = 32U;

    bool TryActivateFlash(const uint32_t minimumRenderGeneration) {
        if (!GenerationReached(CompletedRenderGeneration(),
                               minimumRenderGeneration)) {
            return false;
        }

        uint32_t state = m_lease.load(std::memory_order_acquire);
        while (state < FLASH_WAITER &&
               (state & (RENDER_ACTIVE | RENDER_UNCERTAIN | FLASH_ACTIVE)) == 0U) {
            if ((state & RENDER_WAITER) != 0U &&
                (state & FLASH_TURN) == 0U) return false;
            if (m_lease.compare_exchange_weak(
                    state, (state & ~FLASH_TURN) | FLASH_ACTIVE,
                    std::memory_order_acq_rel)) {
                RecordFlashStart();
                return true;
            }
        }
        return false;
    }

    void RecordFlashStart() {
        m_flashStartedAtMs.store(fxe::Millis());
        m_flashAccesses.fetch_add(1U);
    }
    void RecordFlashHold() {
        const uint32_t heldMs = fxe::Millis() - m_flashStartedAtMs.load();
        m_lastFlashHoldMs.store(heldMs);
        if (heldMs > m_maxFlashHoldMs.load()) m_maxFlashHoldMs.store(heldMs);
    }

    static bool GenerationReached(const uint32_t completed,
                                  const uint32_t target) {
        return static_cast<int32_t>(completed - target) >= 0;
    }

    // One atomic decision owns both resources; separate flags can permit two
    // contenders to miss each other's publication on different cores.
    std::atomic<uint32_t> m_lease{0U};
    std::atomic<uint32_t> m_completedRenderGeneration{0U};
    std::atomic<uint32_t> m_flashStartedAtMs{0U};
    std::atomic<uint32_t> m_flashAccesses{0U};
    std::atomic<uint32_t> m_lastFlashHoldMs{0U};
    std::atomic<uint32_t> m_maxFlashHoldMs{0U};
};

} // namespace hardware
