#pragma once

#include <cstdint>

#include "hardware/button_manager.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "hardware/hardware.hpp"
#include "hardware/user_input_timing.hpp"
#include "modules/user_input.hpp"

namespace hardware {

struct GpioButtonInterruptStatus {
    bool enabled = false;
    uint32_t rawCount = 0U; // ISR callbacks, including unchanged bank snapshots.
    uint32_t overflows = 0U; // Invalidated raw/semantic batches, cumulative.
    uint16_t pending = 0U; // Captured snapshots awaiting replay.
    uint16_t downMask = 0U; // Physical button-index mask, before rotation.
    uint32_t lastEdgeAtMs = 0U;
};

struct GpioUserInputConfig {
    // Release debounce in interrupt mode; both transitions in polling mode.
    uint32_t debounceMs = user_input_timing::BUTTON_DEBOUNCE_MS;
};

// Device-only GPIO user input reader.
//
// Produces debounced edge events for:
// - DPad: Up/Down/Left/Right
// - X/O on each fitted physical control bank
//
// Notes:
// - Active-low inputs are assumed (INPUT_PULLUP).
// - Heap-free: uses fixed-size ring buffer.
class GpioUserInput final : public modules::IUserInput {
  public:
    explicit GpioUserInput(const GpioUserInputConfig& cfg = {})
        : m_cfg(cfg) {}

    ~GpioUserInput() override;
    void Begin();
    // Select interrupt capture or polling on the board-loop task after Begin().
    bool SetButtonInterruptsEnabled(bool enabled);
    GpioButtonInterruptStatus GetButtonInterruptStatus() const;
    // A chronology reset has already cleared/reseeded input; callers clear
    // their held gestures and queues, without inventing a Release/tap.
    bool ConsumeInputReset();
    bool HasPendingButtonRelease() const;
    uint32_t ButtonTimelineThroughMs() const;
    /** \brief Discard queued gestures and suppress currently held controls until release. */
    void SuppressHeldControlsUntilRelease();

    // Must be called before Begin() on multi-revision S3 firmware.
    void ConfigureHardwareProfile(const BoardHardwareProfile& profile) {
        m_pinProfile = profile;
    }

    // When enabled, normalize the printed physical P1/P2 control banks onto
    // the opposite logical game owner. X/O logical IDs and every owner tag
    // swap; directional action IDs remain shared.
    // Returns true when live ownership/button mappings were reseeded.
    bool SetRotate180(bool rotate180);

    bool TryReadButtonEvent(modules::ButtonEvent& outEvent) override;
    // Cached GPIO observations only; never samples a pin from diagnostics.
    bool AreCachedControlsReleased() const {
        if (!m_initialized || HasPendingButtonRelease()) return false;
        for (uint8_t i = 0U; i < BUTTON_COUNT; ++i) {
            if (m_rawStateKnown[i] && m_lastRawDown[i]) return false;
        }
        return true;
    }

  private:
    struct ButtonInfo {
        uint8_t pin = 0;
        modules::UserButton button = modules::UserButton::None;
        modules::UiOwner owner = modules::UiOwner::None;
    };

    bool ClearButtonInterrupts();
    void InitButtons();
    void ConfigureManager(bool seedRawDown);
    void ReseedInput();
    bool ReadTrackedActiveLowDown(uint8_t index, uint8_t pin);

    GpioUserInputConfig m_cfg;
    BoardHardwareProfile m_pinProfile = BoardHardwareProfileFor(
        BoardRevisionFromId(hardware::HW_REVISION_ID));

    bool m_rotate180 = false;

    // Configured in Begin().
    bool m_initialized = false;
    bool m_useInterrupts = false;
    bool m_inputReset = false;
    bool m_seedingSnapshot = false;
    uint16_t m_replayMask = 0U;
    uint32_t m_pollThroughMs = 0U;
    uint16_t m_configuredIrqs = 0;
    uint16_t m_attachedIrqs = 0;

    static constexpr uint8_t BASE_BUTTON_COUNT = 6;
    // S3 hardware includes a second four-direction d-pad plus X/O.
    static constexpr uint8_t P2_BUTTON_COUNT = hardware::HW_HAS_P2_CONTROLS ? 6U : 0U;
    static constexpr uint8_t BUTTON_COUNT = static_cast<uint8_t>(BASE_BUTTON_COUNT + P2_BUTTON_COUNT);
    ButtonInfo m_buttons[BUTTON_COUNT] = {};
    // The S3 can replay a full batch of edges and any intervening repeats.
    ButtonManager<BUTTON_COUNT, hardware::HW_HAS_P2_CONTROLS ? 256U : 16U> m_manager;
    bool m_rawStateKnown[BUTTON_COUNT] = {};
    bool m_lastRawDown[BUTTON_COUNT] = {};
};

} // namespace hardware
