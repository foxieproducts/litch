#include "modules/physical_palette_shortcut.hpp"

#include <ArduinoJson.h>

#include <cstdint>

#include "modules/player_end_palette.hpp"
#include "modules/ui_color_keys.hpp"
#include "modules/ui_color_wheel.hpp"

namespace modules::physical_palette_shortcut {

bool TryApply(const modules::ButtonEvent& event,
              const bool pressLike,
              fxe::SettingsStore& settings,
              modules::ControlMutationService& controlMutation,
              EditPreview* const preview) {
    if (event.button != modules::UserButton::Up &&
         event.button != modules::UserButton::Down) {
        return false;
    }

    if (preview != nullptr) preview->Observe(event, pressLike);
    if (!pressLike) return false;

    const int step = event.event == fxe::Button::Event::Repeat ? 2 : 1;
    const int delta = event.button == modules::UserButton::Up ? 1 : -1;
    const bool firstP2 = settings.ReadValue("GAME_FIRST_PLAYER", uint32_t{1U}) == 2U;
    const bool p2End = (event.owner == modules::UiOwner::P2) != firstP2;
    uint8_t p1Wheel = static_cast<uint8_t>(
        modules::player_end_palette::ReadSettingUInt32(
            settings,
            modules::ui_color_keys::P1_PIECE_WHEEL,
            modules::player_end_palette::P1_END_DEFAULT_WHEEL));
    uint8_t p2Wheel = static_cast<uint8_t>(
        modules::player_end_palette::ReadSettingUInt32(
            settings,
            modules::ui_color_keys::P2_PIECE_WHEEL,
            modules::player_end_palette::P2_END_DEFAULT_WHEEL));

    const char* key = nullptr;
    uint8_t value = 0U;
    if (p2End) {
        for (int index = 0; index < step; ++index) {
            p2Wheel = modules::ui_color_wheel::StepWithExclusion(
                p2Wheel, p1Wheel, delta,
                modules::ui_color_wheel::MIN_SEPARATION);
        }
        key = modules::ui_color_keys::P2_PIECE_WHEEL;
        value = p2Wheel;
    } else {
        for (int index = 0; index < step; ++index) {
            p1Wheel = modules::ui_color_wheel::StepWithExclusion(
                p1Wheel, p2Wheel, delta,
                modules::ui_color_wheel::MIN_SEPARATION);
        }
        key = modules::ui_color_keys::P1_PIECE_WHEEL;
        value = p1Wheel;
    }

    StaticJsonDocument<16> doc;
    doc.set(value);
    const auto outcome = controlMutation.MutateSetting(key, doc.as<JsonVariantConst>());
    if (preview != nullptr && outcome.status == ControlMutationStatus::Applied)
        preview->Applied(event, ui_color_wheel::WheelToRgb(value));
    return true;
}

} // namespace modules::physical_palette_shortcut
