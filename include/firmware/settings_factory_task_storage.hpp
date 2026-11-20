#pragma once

#include <atomic>
#include <cstdint>

#include <freertos/FreeRTOS.h>

namespace firmware {

// A factory boot finishes and deletes its same-core radio test task before
// starting persistence. Normal boots give this storage directly to settings.
struct SettingsFactoryTaskStorage final {
    static constexpr uint32_t SETTINGS_STACK_BYTES = 7168U;
    static constexpr uint32_t FACTORY_STACK_BYTES = 6144U;
    static_assert(FACTORY_STACK_BYTES <= SETTINGS_STACK_BYTES);
    static_assert(SETTINGS_STACK_BYTES % sizeof(StackType_t) == 0U);

    bool TryClaim() { return !m_claimed.test_and_set(std::memory_order_acq_rel); }
    void Release() { m_claimed.clear(std::memory_order_release); }

    alignas(16) StackType_t stack[SETTINGS_STACK_BYTES / sizeof(StackType_t)]{};
    StaticTask_t control{};

  private:
    std::atomic_flag m_claimed = ATOMIC_FLAG_INIT;
};

} // namespace firmware
