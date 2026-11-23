#pragma once

#include <array>
#include <cstdint>

#include "firmware/led_animation_presentation.hpp"
#include "hardware/board_hardware_profile.hpp"
#include "modules/ambient_lux_averager.hpp"
#include "modules/boot_display.hpp"
#include "modules/litchcraft_physical_presentation.hpp"
#include "modules/startup_piece_accent.hpp"
#include "modules/upward_output_slew.hpp"
#include "modules/water_slosh_session.hpp"

namespace hardware { class NeoPixelLedRenderer; }

namespace firmware {

struct LedAnimationFrameResult {
    LedAnimationReceipt receipt{};
    bool retainPriorHint = false;
    bool attempted = false;
    std::array<modules::Rgb, 64> grid{};
    std::array<modules::Rgb, 64> menuBackground{};
};

// This object has exactly one execution owner: initialization before task
// start, then the animation task for its entire live lifetime.
class LedAnimationRenderer final {
  public:
    void Bind(hardware::NeoPixelLedRenderer& output,
              const hardware::BoardHardwareProfile& profile,
              const modules::litchcraft_presentation::StreamPort& artwork);
    void InitializeBoot(const modules::BootDisplay& boot) { m_boot = boot; }
    void InitializeGameBackground(const GameLedPresentation& game) {
        m_gameBackground = game;
        m_haveGameBackground = true;
    }
    void CancelBoot() { m_boot.Cancel(); }
    void InitializeBrightness(const modules::AmbientLuxAverager& ambient,
                              const modules::UpwardOutputSlew& source);
    void Render(LedAnimationPresentation& presentation,
                LedAnimationFrameResult& result);
    bool RenderPhysical(const PhysicalLedCommand& command);
    bool RenderRawDie(const RawDieLedCommand& command);
    bool Quiesce();
    void CancelArtwork();
    void NoteArtworkPresented(modules::litchcraft_presentation::Feedback& feedback);
    hardware::LedDeliverySnapshot Delivery() const;
    const modules::BudgetAwareLedPipelineStats& PowerStats() const { return m_powerStats; }
    uint16_t CurrentLimitMa() const { return m_currentLimitMa; }
    bool OutputEnabled() const;
    const modules::BootDisplay& BootState() const { return m_boot; }
    bool StartupComplete() const {
        return m_boot.IsComplete() && m_startupAccentStarted && !m_startupAccent.IsArmed();
    }

  private:
    void RenderGame(LedAnimationPresentation& presentation,
                    GameLedPresentation& game,
                    LedAnimationFrameResult& result);
    void RenderArtwork(LedAnimationPresentation& presentation,
                       modules::litchcraft_presentation::Inputs& artwork,
                       LedAnimationFrameResult& result);
    struct FrameOwnership {
        modules::GridForegroundLayer foreground{};
        modules::status_led_frame_adapter::FixedPhysicalStatusOverlay status{};
        uint64_t prioritySquares = 0U;
        uint64_t ordinaryPieces = 0U;
        uint64_t captureRemoval = 0U;
        uint64_t captureTaps = 0U;
        uint64_t duckReminder = 0U;
        bool menuActive = false;
    };
    void ComposeGame(LedAnimationPresentation& presentation,
        GameLedPresentation& game, LedAnimationFrameResult& result, FrameOwnership& ownership);
    void RenderSpecial(LedAnimationPresentation& presentation, LedAnimationFrameResult& result);
    void OutputGameFrame(LedAnimationPresentation& presentation,
                         FrameOwnership& ownership,
                         LedAnimationFrameResult& result);
    uint16_t Brightness(const LedOutputPresentation& presentation,
                        uint32_t nowMs);
    bool Output(const hardware::PhysicalLedFrame& frame,
                const LedOutputPresentation& presentation,
                uint16_t scale256, const modules::LedFrameAccent& accent,
                bool useSourceSlew = true);
    bool ArtworkCurrent(const modules::litchcraft_presentation::Inputs& inputs) const;
    void BeginResult(const LedAnimationPresentation& presentation,
                     LedAnimationFrameResult& result);

    hardware::NeoPixelLedRenderer* m_output = nullptr;
    const hardware::BoardHardwareProfile* m_profile = nullptr;
    modules::litchcraft_presentation::StreamPort m_artworkPort{};
    modules::litchcraft_presentation::Renderer m_artwork{};
    modules::AmbientLuxAveragerConfig m_ambientConfig{};
    modules::AmbientLuxAverager::SmoothingCheckpoint m_ambient{};
    modules::UpwardOutputSlew m_sourceLimit{};
    modules::ControlLedFeedback m_controlFeedback{};
    FrameOwnership m_frameOwnership{};
    modules::BootDisplay m_boot{};
    modules::StartupPieceAccent m_startupAccent{};
    modules::BleIdentifyPresentation m_identify{};
    bool m_restartMessageVisible = false;
    modules::litchcraft_physical_presentation::ModeCrossfade<
        hardware::MAX_PHYSICAL_LED_COUNT> m_crossfade{};
    GameLedPresentation m_gameBackground{};
    bool m_haveGameBackground = false;
    modules::BoardLedFrame m_board{};
    std::array<modules::Rgb, modules::EDGE_LED_COUNT> m_status{};
    modules::BudgetAwareLedPipelineStats m_powerStats{};
    uint16_t m_currentLimitMa = 0U;
    uint16_t m_startupPieceScale256 = 256U;
    uint32_t m_sourceResetEpoch = 0U;
    bool m_startupAccentStarted = false;
    uint32_t m_startupCompletedAtMs = UINT32_MAX;
    bool m_artworkSceneActive = false;
};

} // namespace firmware
