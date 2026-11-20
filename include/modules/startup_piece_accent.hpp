#pragma once

#include <cstdint>
#include "foxie_embedded/millis.hpp"
#include "modules/startup_choreography.hpp"

namespace modules {

// One boot-owned episode. Real Hall input gates admission; unavailable ambient
// samples use the existing conservative brightness fallback after a bounded wait.
class StartupPieceAccent final {
  public:
    static constexpr uint32_t INTRO_MS = 400U;
    static constexpr uint32_t AMBIENT_WAIT_MS = 1000U;

    void Begin() { m_armed = true; m_started = false; m_seenBoard = false; m_beginMs = fxe::Millis(); }
    void Cancel() { m_armed = false; }
    bool IsArmed() const { return m_armed; }

    uint16_t PieceScale256(const uint8_t square) const {
        if (!m_armed) { return 256U; }
        if (!m_started) { return 0U; }
        const uint32_t elapsed = fxe::Millis() - m_startMs;
        if (elapsed >= INTRO_MS) { return 256U; }
        return startup_choreography::PieceScale256(
            static_cast<uint16_t>(elapsed * 256U / INTRO_MS),
            m_occupied, square);
    }

    uint16_t Update(bool boardObserved, uint64_t occupied, bool ambientReady,
                    bool ordinaryPiecesEligible, bool bootPresentationPending = false) {
        if (!m_armed) { return 256U; }
        if (!boardObserved) { return 256U; }
        if (!ordinaryPiecesEligible || (m_seenBoard && occupied != m_occupied)) {
            Cancel();
            return 256U;
        }
        m_seenBoard = true;
        m_occupied = occupied;
        if (bootPresentationPending) { return 256U; }
        const uint32_t now = fxe::Millis();
        if (!m_started) {
            if (!ambientReady && now - m_beginMs < AMBIENT_WAIT_MS) { return 256U; }
            m_started = true;
            m_startMs = now;
        }
        const uint32_t elapsed = now - m_startMs;
        if (elapsed < INTRO_MS) { return 256U; }
        // The sequential introduction ends at the normal requested level.
        // Do not add a post-boot over-bright pulse: on physical boards that
        // looked like an intermittent corrupted/RMT frame under occupied
        // squares as services and Bluetooth came online.
        Cancel();
        return 256U;
    }

  private:
    bool m_armed = false;
    bool m_started = false;
    bool m_seenBoard = false;
    uint64_t m_occupied = 0ULL;
    uint32_t m_beginMs = 0U;
    uint32_t m_startMs = 0U;
};

} // namespace modules
