#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "foxie_embedded/button.hpp"
#include "foxie_embedded/fixed_ring_buffer.hpp"
#include "hardware/pca9555_input_scan.hpp"
#include "modules/board_input.hpp"

namespace hardware {

struct HallSensorButtonConfig {
    uint32_t debounceMs = 5;
};

class HallSensorButtonManager final {
  public:
    explicit HallSensorButtonManager(Pca9555InputScan& scan, const HallSensorButtonConfig& cfg = {});

    void Begin();

    void Update();

    bool TryReadDelta(modules::SquareDelta& outDelta);

    bool TryReadSnapshot(uint64_t& outOcc64) const {
        outOcc64 = m_scan.GetOcc64();
        return m_scan.LastReadOk();
    }

    bool IsOccupied(const uint8_t square) const {
        return m_scan.IsOccupied(square);
    }

  private:
    static constexpr size_t BUTTON_COUNT = 64;
    static constexpr size_t QUEUE_CAPACITY = 64;

    void EnqueueDelta(const uint8_t square, const bool nowOccupied);
    void HandleButtonEvent(const uint8_t square, const fxe::Button::Event evt);

    Pca9555InputScan& m_scan;
    HallSensorButtonConfig m_cfg;

    std::array<fxe::Button, BUTTON_COUNT> m_buttons{};

    fxe::FixedRingBuffer<modules::SquareDelta, QUEUE_CAPACITY> m_queue;
};

} // namespace hardware
