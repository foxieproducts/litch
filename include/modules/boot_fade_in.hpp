#pragma once

#include <cstdint>

#include "foxie_embedded/elapsed_time.hpp"

namespace modules {

struct BootFadeInConfig {
#ifdef ARDUINO
    uint32_t fadeDurationMs = 500;
#else
    uint32_t fadeDurationMs = 30;
#endif

    // Legacy parity: force a higher LED render cadence during the fade so the
    // board visibly ramps even if no other state changes occur.
    uint32_t renderCadenceMs = 25;
};

// Deterministic boot fade-in policy.
// - Starts at 0 brightness scale and ramps to 1.0 over fadeDurationMs.
// - Pure logic: no rendering side effects.
class BootFadeIn final {
  public:
    explicit BootFadeIn(const BootFadeInConfig& cfg = {})
        : m_cfg(cfg) {}

    void Reset() {
        m_started = false;
        m_elapsed = fxe::ElapsedTime();
    }

    void Start() {
        m_started = true;
        m_elapsed.Reset();
    }

    bool IsStarted() const { return m_started; }

    bool IsActive() const {
        if (!m_started) {
            return false;
        }

        const uint32_t d = m_cfg.fadeDurationMs;
        if (d == 0) {
            return false;
        }

        return m_elapsed.ElapsedMs() < d;
    }

    // Returns a 0..256 scale factor.
    uint16_t GetFadeScale256() const {
        if (!m_started) {
            return 256;
        }

        const uint32_t d = m_cfg.fadeDurationMs;
        if (d == 0) {
            return 256;
        }

        const uint32_t elapsed = m_elapsed.ElapsedMs();
        if (elapsed >= d) {
            return 256;
        }

        // Linear ramp. (elapsed/d) in fixed-point.
        const uint32_t scaled = (elapsed * 256U) / d;
        return (scaled > 256U) ? 256U : static_cast<uint16_t>(scaled);
    }

    uint32_t GetPreferredRenderCadenceMs() const {
        return IsActive() ? m_cfg.renderCadenceMs : 0U;
    }

  private:
    BootFadeInConfig m_cfg;

    bool m_started = false;
    fxe::ElapsedTime m_elapsed;
};

} // namespace modules
