#include "hardware/hall_sensor_button_manager.hpp"

namespace hardware {

HallSensorButtonManager::HallSensorButtonManager(Pca9555InputScan& scan,
                                                 const HallSensorButtonConfig& cfg)
    : m_scan(scan), m_cfg(cfg) {}

void HallSensorButtonManager::Begin() {
    m_queue.Clear();

    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        fxe::Button::Config cfg;
        cfg.debounceMs = m_cfg.debounceMs;
        cfg.canRepeat = false;
        cfg.longHoldMs = 0;

        const uint8_t square = static_cast<uint8_t>(i);
        cfg.onEvent = [this, square](const fxe::Button::Event evt) {
            HandleButtonEvent(square, evt);
        };

        m_buttons[i].SetConfig(cfg);
        m_buttons[i].SetDetectionFn([this, square]() { return m_scan.IsOccupied(square); });
        m_buttons[i].Reset();
    }

    const bool ok = m_scan.Update();
    if (ok) {
        for (size_t i = 0; i < BUTTON_COUNT; ++i) {
            const bool rawDown = m_scan.IsOccupied(static_cast<uint8_t>(i));
            m_buttons[i].SeedRawDown(rawDown);
        }
    }
}

void HallSensorButtonManager::Update() {
    (void)m_scan.Update();

    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        m_buttons[i].Update();
    }
}

bool HallSensorButtonManager::TryReadDelta(modules::SquareDelta& outDelta) {
    return m_queue.TryPop(outDelta);
}

void HallSensorButtonManager::EnqueueDelta(const uint8_t square, const bool nowOccupied) {
    (void)m_queue.Push(modules::SquareDelta{square, nowOccupied});
}

void HallSensorButtonManager::HandleButtonEvent(const uint8_t square, const fxe::Button::Event evt) {
    switch (evt) {
        case fxe::Button::Event::Press:
            EnqueueDelta(square, true);
            return;
        case fxe::Button::Event::Release:
            EnqueueDelta(square, false);
            return;
        case fxe::Button::Event::Repeat:
        case fxe::Button::Event::LongHold:
            return;
    }
}

} // namespace hardware
