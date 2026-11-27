#include "modules/led_frame_composer.hpp"

#include "modules/assistance_preferences.hpp"
#include "modules/light_square_appearance.hpp"
#include "modules/minimal_animations_setting.hpp"
#include "modules/player_end_palette.hpp"
#include "modules/turn_anim_style_setting.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"
#include "modules/white_squares_setting.hpp"

namespace modules::led_frame_composer {
namespace {

fxe::CachedSetting<uint32_t> s_p1EndWheelCache(
    modules::ui_color_keys::P1_END_PIECE_WHEEL,
    modules::player_end_palette::P1_END_DEFAULT_WHEEL);
fxe::CachedSetting<uint32_t> s_p2EndWheelCache(
    modules::ui_color_keys::P2_END_PIECE_WHEEL,
    modules::player_end_palette::P2_END_DEFAULT_WHEEL);

} // namespace

ComposeSettings CaptureComposeSettings(const fxe::SettingsStore& settings) {
    ComposeSettings captured;
    const uint32_t displayLevel = white_squares_setting::ReadBrightness0To8(settings);
    captured.minimumBrightness = displayLevel == 0U;
    captured.lightSquareMinimumChannel = light_square_appearance::MinimumVisibleChannel(
        displayLevel, settings.ReadValue("AUTO_BRIGHTNESS", true));
    captured.whiteSquaresOn = white_squares_setting::ReadWhiteSquaresOn(settings);
    captured.coloredLightSquares = light_square_appearance::UsesColor(settings);
    captured.lightSquareHue = light_square_appearance::ReadHue(settings);
    captured.lightSquareBrightness = light_square_appearance::ReadBrightness(settings);
    captured.nearEndPieceWheel = ui_color_wheel::ClampWheelValue01To255(
        settings.Doc().containsKey(ui_color_keys::P1_END_PIECE_WHEEL)
            ? s_p1EndWheelCache.Get(settings)
            : player_end_palette::ReadSettingUInt32(settings,
                ui_color_keys::P1_END_PIECE_WHEEL, player_end_palette::P1_END_DEFAULT_WHEEL));
    captured.farEndPieceWheel = ui_color_wheel::ClampWheelValue01To255(
        settings.Doc().containsKey(ui_color_keys::P2_END_PIECE_WHEEL)
            ? s_p2EndWheelCache.Get(settings)
            : player_end_palette::ReadSettingUInt32(settings,
                ui_color_keys::P2_END_PIECE_WHEEL, player_end_palette::P2_END_DEFAULT_WHEEL));
    captured.initialKingPlacementAssistance = settings.ReadValue("BOARD_SETUP_ASSISTANCE", true);
    captured.persistenceDegraded = settings.HasConfirmedStorageFailure();
    captured.p1LegalMoves = assistance_preferences::Enabled(settings, UiOwner::P1,
        assistance_preferences::Feature::LegalMoves);
    captured.p2LegalMoves = assistance_preferences::Enabled(settings, UiOwner::P2,
        assistance_preferences::Feature::LegalMoves);
    captured.p1CheckCue = assistance_preferences::Enabled(settings, UiOwner::P1,
        assistance_preferences::Feature::CheckCue);
    captured.p2CheckCue = assistance_preferences::Enabled(settings, UiOwner::P2,
        assistance_preferences::Feature::CheckCue);
    captured.checkmateStyle = checkmate_animation_setting::Read(settings);
    captured.minimalAnimations = minimal_animations_setting::Read(settings);
    if (settings.ReadValue("VS_CPU_LEVEL", uint32_t{0U}) != 0U) {
        // Match the engine's seat predicate even for a malformed stored value.
        captured.cpuOwner = settings.ReadValue("VS_CPU_PLAYER", uint32_t{2U}) == 2U
            ? UiOwner::P2 : UiOwner::P1;
    }
    captured.settleStyle = settle_animation_setting::Read(settings);
    captured.turnStyle = turn_anim_style_setting::ParseTurnAnimStyle(settings.ReadValue(
        turn_anim_style_setting::KEY_TURN_ANIM_STYLE, turn_anim_style_setting::DEFAULT_VALUE));
    captured.thinkingStyle = hint_thinking_style_setting::Parse(settings.ReadValue(
        hint_thinking_style_setting::KEY, hint_thinking_style_setting::DEFAULT_VALUE));
    return captured;
}

} // namespace modules::led_frame_composer
