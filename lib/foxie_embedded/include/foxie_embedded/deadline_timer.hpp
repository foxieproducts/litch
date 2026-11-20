#pragma once

#include <cstdint>

#include "foxie_embedded/fixed_function.hpp"
#include "foxie_embedded/millis.hpp"

namespace fxe {

class DeadlineTimer final {
  public:
    static constexpr size_t CALLBACK_STORAGE_BYTES = 32;
    using Callback = fxe::FixedFunction<void(), CALLBACK_STORAGE_BYTES>;

    void Start(const uint32_t durationMs) {
        m_active = true;
        m_deadlineMs = Millis() + durationMs;
        m_fired = false;
    }

    void Stop() {
        m_active = false;
        m_fired = false;
    }

    bool IsActive() const { return m_active; }

    uint32_t DeadlineMs() const { return m_deadlineMs; }

    // Wrap-safe compare: expired if nowMs >= deadlineMs.
    bool IsExpired() const {
        if (!m_active) {
            return false;
        }

        const uint32_t nowMs = Millis();
        return static_cast<int32_t>(nowMs - m_deadlineMs) >= 0;
    }

    void SetCallback(Callback cb) { m_onExpired = std::move(cb); }

    void ClearCallback() { m_onExpired = nullptr; }

    // Checks expiration and invokes the callback at most once.
    // Returns true if the timer fired this update.
    bool Update() {
        if (m_fired) {
            return false;
        }

        if (!IsExpired()) {
            return false;
        }

        m_fired = true;
        m_active = false;

        if (m_onExpired) {
            m_onExpired();
        }

        return true;
    }

  private:
    bool m_active = false;
    bool m_fired = false;
    uint32_t m_deadlineMs = 0;
    Callback m_onExpired{};
};

} // namespace fxe
