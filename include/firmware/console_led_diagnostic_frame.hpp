#pragma once

#include <array>
#include <cstdint>

#include "hardware/led_layout.hpp"
#include "modules/rgb.hpp"

namespace firmware {

/**
 * \brief Retained physical-board canvas for bounded console LED diagnostics.
 *
 * Edge/status LEDs are deliberately excluded from the mutable surface.
 */
class ConsoleLedDiagnosticFrame final {
  public:
    using Frame = std::array<modules::Rgb, hardware::TOTAL_LED_COUNT>;

    bool SetBoardLed(const uint16_t physicalIndex, const modules::Rgb color) {
        if (physicalIndex >= hardware::BOARD_LED_COUNT) {
            return false;
        }

        m_frame[physicalIndex] = color;
        return true;
    }

    void Clear() {
        m_frame.fill(modules::RGB_BLACK);
    }

    const Frame& GetFrame() const {
        return m_frame;
    }

  private:
    Frame m_frame{};
};

} // namespace firmware
