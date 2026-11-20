#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/edge_led_constants.hpp"
#include "modules/rgb.hpp"

namespace modules::status_led_overlay {

inline constexpr uint8_t PRIORITY_BASE = 0U;
inline constexpr uint8_t PRIORITY_OTA = 128U;
inline constexpr uint8_t PRIORITY_BLE = 224U;

struct Slot {
    modules::Rgb color = modules::RGB_BLACK;
    uint8_t priority = PRIORITY_BASE;
    bool active = false;
};

class Buffer {
  public:
    static constexpr size_t kStatusLedCount = modules::EDGE_LED_COUNT;

    void Clear() {
        m_slots.fill(Slot{});
    }

    void Set(const uint8_t localStatusIndex, const uint8_t priority, const modules::Rgb color) {
        if (localStatusIndex >= m_slots.size()) {
            return;
        }

        Slot& slot = m_slots[localStatusIndex];
        if (slot.active && priority < slot.priority) {
            return;
        }

        slot.color = color;
        slot.priority = priority;
        slot.active = true;
    }

    template <typename Setter>
    void Apply(Setter&& setter) const {
        for (uint8_t index = 0; index < m_slots.size(); ++index) {
            const Slot& slot = m_slots[index];
            if (!slot.active) {
                continue;
            }

            setter(index, slot.color);
        }
    }

  private:
    std::array<Slot, kStatusLedCount> m_slots = {};
};

} // namespace modules::status_led_overlay
