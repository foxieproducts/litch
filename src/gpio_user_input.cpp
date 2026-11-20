#include "hardware/gpio_user_input.hpp"

#ifdef ARDUINO

#include <Arduino.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <soc/gpio_struct.h>
#endif

#include "hardware/hardware.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/orientation.hpp"

namespace hardware {

namespace {

#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr uint16_t CAPTURE_CAPACITY = 128U;
constexpr uint8_t CAPTURE_BUTTON_LIMIT = 12U;
struct ButtonCapture {
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    // Each slot is one observation. Separate arrays avoid six padding bytes per slot.
    uint64_t sampleTimesUs[CAPTURE_CAPACITY] = {};
    uint16_t sampleMasks[CAPTURE_CAPACITY] = {};
    uint8_t pins[CAPTURE_BUTTON_LIMIT] = {};
    uint8_t buttonCount = 0U;
    uint16_t read = 0U;
    uint16_t write = 0U;
    uint16_t count = 0U;
    uint16_t lastMask = 0U;
    uint64_t lastAtUs = 0U;
    uint32_t rawCount = 0U;
    uint32_t overflows = 0U;
    bool collecting = false;
    bool fault = false;
};
// The SDK owns ISR callback lifetime, including failed detach. No callback
// references the GpioUserInput object, heap, configuration or flash constants.
DRAM_ATTR ButtonCapture g_capture;

inline __attribute__((always_inline)) uint16_t ReadButtonMask() {
    const uint32_t low = GPIO.in;
    const uint32_t high = GPIO.in1.val;
    uint16_t down = 0U;
    for (uint8_t index = 0U; index < g_capture.buttonCount; ++index) {
        const uint8_t pin = g_capture.pins[index];
        const uint32_t level = pin < 32U ? (low >> pin) & 1U : (high >> (pin - 32U)) & 1U;
        if (level == 0U) down |= uint16_t{1} << index;
    }
    return down;
}
void IRAM_ATTR OnButtonChange(void*) {
    portENTER_CRITICAL_ISR(&g_capture.lock);
    if (g_capture.collecting) {
        ++g_capture.rawCount;
        const uint16_t mask = ReadButtonMask();
        const uint64_t atUs = esp_timer_get_time();
        g_capture.lastAtUs = atUs;
        if (mask != g_capture.lastMask) {
            g_capture.lastMask = mask;
            if (!g_capture.fault) {
                if (g_capture.count == CAPTURE_CAPACITY) {
                    ++g_capture.overflows;
                    g_capture.fault = true;
                } else {
                    g_capture.sampleTimesUs[g_capture.write] = atUs;
                    g_capture.sampleMasks[g_capture.write] = mask;
                    g_capture.write = (g_capture.write + 1U) % CAPTURE_CAPACITY;
                    ++g_capture.count;
                }
            }
        }
    }
    portEXIT_CRITICAL_ISR(&g_capture.lock);
}
#endif

bool ReadActiveLowDown(const uint8_t pin) {
    return digitalRead(pin) == LOW;
}

fxe::Button::Config MakeDefaultConfig(const GpioUserInputConfig& cfg,
                                      const modules::UserButton btn) {
    fxe::Button::Config c;
    c.debounceMs = cfg.debounceMs;

    // Match existing emulator behavior: only directions auto-repeat.
    const bool isDir = (btn == modules::UserButton::Up) || (btn == modules::UserButton::Down) ||
                       (btn == modules::UserButton::Left) || (btn == modules::UserButton::Right);
    c.canRepeat = isDir;

    if (isDir) {
        // Human-scale repeat rate for held navigation.
        c.beforeRepeatMs = hardware::user_input_timing::DPAD_BEFORE_REPEAT_MS;
        const bool isValueAdjustment =
            btn == modules::UserButton::Up || btn == modules::UserButton::Down;
        c.repeatRateMs = isValueAdjustment
                             ? hardware::user_input_timing::DPAD_VALUE_REPEAT_RATE_MS
                             : hardware::user_input_timing::DPAD_NAV_REPEAT_RATE_MS;
    }

    // Long-hold is opt-in at policy sites; keep disabled by default.
    c.longHoldMs = 0;
    return c;
}

} // namespace

void GpioUserInput::InitButtons() {
    // Order matters only for determinism of simultaneous edges.
    const PlayerControlPinProfile& p1 = m_pinProfile.p1;
    m_buttons[0] = ButtonInfo{p1.up, modules::UserButton::Up, modules::UiOwner::P1};
    m_buttons[1] = ButtonInfo{p1.down, modules::UserButton::Down, modules::UiOwner::P1};
    m_buttons[2] = ButtonInfo{p1.left, modules::UserButton::Left, modules::UiOwner::P1};
    m_buttons[3] = ButtonInfo{p1.right, modules::UserButton::Right, modules::UiOwner::P1};
    m_buttons[4] = ButtonInfo{p1.x, modules::UserButton::P1X, modules::UiOwner::P1};
    m_buttons[5] = ButtonInfo{p1.o, modules::UserButton::P1O, modules::UiOwner::P1};

    if constexpr (hardware::HW_HAS_P2_CONTROLS) {
        const PlayerControlPinProfile& p2 = m_pinProfile.p2;
        // P2 d-pad is mapped onto the shared directional buttons.
        // This allows either side to navigate menus and operate bring-up.
        m_buttons[6] = ButtonInfo{p2.up, modules::UserButton::Up, modules::UiOwner::P2};
        m_buttons[7] = ButtonInfo{p2.down, modules::UserButton::Down, modules::UiOwner::P2};
        m_buttons[8] = ButtonInfo{p2.left, modules::UserButton::Left, modules::UiOwner::P2};
        m_buttons[9] = ButtonInfo{p2.right, modules::UserButton::Right, modules::UiOwner::P2};
        m_buttons[10] = ButtonInfo{p2.x, modules::UserButton::P2X, modules::UiOwner::P2};
        m_buttons[11] = ButtonInfo{p2.o, modules::UserButton::P2O, modules::UiOwner::P2};
    }

    if (m_rotate180) {
        for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
            m_buttons[i].owner = modules::SwapP1P2Owner(m_buttons[i].owner);
            m_buttons[i].button = modules::SwapP1P2Button(m_buttons[i].button);
        }
    }
}

void GpioUserInput::ConfigureManager(const bool seedRawDown) {
    // One physical snapshot seeds every button under the same chronology.
    if (seedRawDown) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
        const uint16_t mask = ReadButtonMask();
#else
        uint16_t mask = 0U;
        for (uint8_t i = 0U; i < BUTTON_COUNT; ++i)
            if (ReadActiveLowDown(m_buttons[i].pin)) mask |= uint16_t{1} << i;
#endif
        m_replayMask = mask;
        for (uint8_t i = 0U; i < BUTTON_COUNT; ++i) {
            m_rawStateKnown[i] = true;
            m_lastRawDown[i] = (mask & (uint16_t{1} << i)) != 0U;
        }
    }
    m_seedingSnapshot = seedRawDown;
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        const ButtonInfo& b = m_buttons[i];

        auto c = MakeDefaultConfig(m_cfg, b.button);
        c.immediatePress = m_useInterrupts;
        const auto detectionFn = fxe::Button::DetectionFn([this, i, pin = b.pin]() {
            return ReadTrackedActiveLowDown(i, pin);
        });

        m_manager.ConfigureButton(i, b.button, b.owner, c, detectionFn, seedRawDown);
    }
    m_seedingSnapshot = false;
}

bool GpioUserInput::ReadTrackedActiveLowDown(const uint8_t index, const uint8_t pin) {
    if (m_seedingSnapshot) return m_lastRawDown[index];
    const bool rawDown = ReadActiveLowDown(pin);

    if (!m_rawStateKnown[index]) {
        m_rawStateKnown[index] = true;
        m_lastRawDown[index] = rawDown;
        return rawDown;
    }

    if (m_lastRawDown[index] != rawDown) {
        m_lastRawDown[index] = rawDown;
    }

    return rawDown;
}

GpioUserInput::~GpioUserInput() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    portENTER_CRITICAL(&g_capture.lock);
    g_capture.collecting = false;
    portEXIT_CRITICAL(&g_capture.lock);
    (void)ClearButtonInterrupts();
#endif
}

void GpioUserInput::Begin() {
    if (m_initialized) (void)SetButtonInterruptsEnabled(false);
    InitButtons();
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    portENTER_CRITICAL(&g_capture.lock);
    g_capture.collecting = false;
    static_assert(BUTTON_COUNT <= CAPTURE_BUTTON_LIMIT);
    g_capture.buttonCount = BUTTON_COUNT;
    for (uint8_t index = 0U; index < BUTTON_COUNT; ++index) g_capture.pins[index] = m_buttons[index].pin;
    portEXIT_CRITICAL(&g_capture.lock);
#endif

    // Active-low with pullups.
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        pinMode(m_buttons[i].pin, INPUT_PULLUP);
    }

    ReseedInput();
    m_initialized = true;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (m_pinProfile.revision == BoardRevision::V3_3S3) {
        (void)SetButtonInterruptsEnabled(true);
    }
#endif
}

bool GpioUserInput::ClearButtonInterrupts() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        const uint16_t bit = uint16_t{1} << i;
        const auto pin = static_cast<gpio_num_t>(m_buttons[i].pin);
        if ((m_configuredIrqs & bit) && gpio_set_intr_type(pin, GPIO_INTR_DISABLE) == ESP_OK) {
            m_configuredIrqs &= ~bit;
        }
        if ((m_attachedIrqs & bit) && gpio_isr_handler_remove(pin) == ESP_OK) {
            m_attachedIrqs &= ~bit;
        }
    }
#endif
    return m_configuredIrqs == 0 && m_attachedIrqs == 0;
}

bool GpioUserInput::SetButtonInterruptsEnabled(const bool enabled) {
    if (enabled && m_useInterrupts) return true;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    portENTER_CRITICAL(&g_capture.lock);
    g_capture.collecting = false;
    portEXIT_CRITICAL(&g_capture.lock);
#endif
    m_useInterrupts = false;
    const bool clean = ClearButtonInterrupts();
    if (!enabled || !clean) {
        if (m_initialized) ReseedInput();
        return !enabled && clean;
    }
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (!m_initialized || m_pinProfile.revision != BoardRevision::V3_3S3) return false;
    // Share the Arduino service's cache-safety policy with existing Hall
    // attachInterrupt trampolines. Our callback/data remain IRAM/DRAM safe,
    // but a cache-disabled window can delay/coalesce physical edge delivery.
    const esp_err_t result = gpio_install_isr_service(ARDUINO_ISR_FLAG);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ReseedInput();
        return false;
    }
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        const uint16_t bit = uint16_t{1} << i;
        const auto pin = static_cast<gpio_num_t>(m_buttons[i].pin);
        if (gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE) != ESP_OK) {
            (void)ClearButtonInterrupts();
            ReseedInput();
            return false;
        }
        m_configuredIrqs |= bit;
        if (gpio_isr_handler_add(pin, OnButtonChange, nullptr) != ESP_OK) {
            (void)ClearButtonInterrupts();
            ReseedInput();
            return false;
        }
        m_attachedIrqs |= bit;
    }
    m_useInterrupts = true;
    ReseedInput();
#endif
    return m_useInterrupts;
}

void GpioUserInput::ReseedInput() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    portENTER_CRITICAL(&g_capture.lock);
    g_capture.collecting = false;
    g_capture.read = g_capture.write = g_capture.count = 0U;
    g_capture.fault = false;
#endif
    ConfigureManager(true);
    m_manager.Begin();
    m_pollThroughMs = fxe::Millis();
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    g_capture.lastMask = m_replayMask;
    g_capture.collecting = m_useInterrupts;
    portEXIT_CRITICAL(&g_capture.lock);
#endif
    m_inputReset = true;
}

void GpioUserInput::SuppressHeldControlsUntilRelease() {
    if (m_initialized) ReseedInput();
}

bool GpioUserInput::ConsumeInputReset() {
    const bool reset = m_inputReset;
    m_inputReset = false;
    return reset;
}

GpioButtonInterruptStatus GpioUserInput::GetButtonInterruptStatus() const {
    GpioButtonInterruptStatus status;
    status.enabled = m_useInterrupts;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    portENTER_CRITICAL(&g_capture.lock);
    status.rawCount = g_capture.rawCount;
    status.overflows = g_capture.overflows;
    status.pending = g_capture.count;
    status.downMask = g_capture.lastMask;
    const uint64_t atUs = g_capture.lastAtUs;
    portEXIT_CRITICAL(&g_capture.lock);
    status.lastEdgeAtMs = static_cast<uint32_t>(atUs / 1000U);
    if (!m_useInterrupts) {
        status.downMask = 0U;
        for (uint8_t i = 0U; i < BUTTON_COUNT; ++i)
            if (m_lastRawDown[i]) status.downMask |= uint16_t{1} << i;
    }
#else
    for (uint8_t i = 0U; i < BUTTON_COUNT; ++i)
        if (m_lastRawDown[i]) status.downMask |= uint16_t{1} << i;
#endif
    return status;
}

bool GpioUserInput::HasPendingButtonRelease() const {
    if (!m_useInterrupts) return false;
    return GetButtonInterruptStatus().pending != 0U || m_manager.HasPendingRelease();
}

uint32_t GpioUserInput::ButtonTimelineThroughMs() const {
    return m_useInterrupts ? m_manager.ButtonTimelineThroughMs() : m_pollThroughMs;
}

bool GpioUserInput::SetRotate180(const bool rotate180) {
    if (m_rotate180 == rotate180) {
        return false;
    }

    m_rotate180 = rotate180;

    // If we're already running, reconfigure button ownership/IDs immediately.
    if (m_initialized) {
        InitButtons();
        // Avoid spurious edges when toggling; seed with raw-down state.
        ReseedInput();
    }

    return true;
}

bool GpioUserInput::TryReadButtonEvent(modules::ButtonEvent& outEvent) {
    if (!m_initialized) {
        return false;
    }

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (m_useInterrupts) {
        // Bound this batch at one common clock cut. Later ISR samples remain
        // queued for the next call, never replayed behind an advanced clock.
        portENTER_CRITICAL(&g_capture.lock);
        const uint64_t throughUs = esp_timer_get_time();
        const uint16_t batch = g_capture.count;
        const bool fault = g_capture.fault;
        portEXIT_CRITICAL(&g_capture.lock);
        if (fault) {
            ReseedInput();
            return false;
        }
        for (uint16_t count = 0U; count < batch; ++count) {
            portENTER_CRITICAL(&g_capture.lock);
            const uint64_t atUs = g_capture.sampleTimesUs[g_capture.read];
            const uint16_t downMask = g_capture.sampleMasks[g_capture.read];
            g_capture.read = (g_capture.read + 1U) % CAPTURE_CAPACITY;
            --g_capture.count;
            portEXIT_CRITICAL(&g_capture.lock);
            const uint16_t changed = downMask ^ m_replayMask;
            const uint32_t atMs = static_cast<uint32_t>(atUs / 1000U);
            // A whole-bank observation cannot establish the order of changes
            // inside that instant. Apply releases before presses so exclusive
            // observed states never invent an unwitnessed X/O overlap.
            for (uint8_t down = 0U; down < 2U; ++down) {
                for (uint8_t index = 0U; index < BUTTON_COUNT; ++index) {
                    const uint16_t bit = uint16_t{1} << index;
                    const bool rawDown = (downMask & bit) != 0U;
                    if ((changed & bit) == 0U || rawDown != (down != 0U)) continue;
                    m_rawStateKnown[index] = true;
                    m_lastRawDown[index] = rawDown;
                    m_manager.FeedEdge(index, rawDown, atMs);
                }
            }
            m_replayMask = downMask;
        }
        m_manager.AdvanceTo(static_cast<uint32_t>(throughUs / 1000U));
        portENTER_CRITICAL(&g_capture.lock);
        const bool lost = g_capture.fault;
        portEXIT_CRITICAL(&g_capture.lock);
        const bool semanticOverflow = m_manager.ConsumeOverflow();
        if (semanticOverflow) {
            portENTER_CRITICAL(&g_capture.lock);
            ++g_capture.overflows;
            portEXIT_CRITICAL(&g_capture.lock);
        }
        if (lost || semanticOverflow) {
            ReseedInput();
            return false;
        }
        return m_manager.TryReadQueuedEvent(outEvent);
    }
#endif
    m_pollThroughMs = fxe::Millis();
    return m_manager.TryReadButtonEvent(outEvent);
}

} // namespace hardware

#else

namespace hardware {

GpioUserInput::~GpioUserInput() = default;
void GpioUserInput::Begin() {}
GpioButtonInterruptStatus GpioUserInput::GetButtonInterruptStatus() const { return {}; }
bool GpioUserInput::ConsumeInputReset() { return false; }
bool GpioUserInput::HasPendingButtonRelease() const { return false; }
uint32_t GpioUserInput::ButtonTimelineThroughMs() const { return 0U; }
bool GpioUserInput::SetButtonInterruptsEnabled(const bool enabled) { return !enabled; }
void GpioUserInput::SuppressHeldControlsUntilRelease() {}

bool GpioUserInput::TryReadButtonEvent(modules::ButtonEvent&) {
    return false;
}

bool GpioUserInput::SetRotate180(const bool) {
    return false;
}

} // namespace hardware

#endif
