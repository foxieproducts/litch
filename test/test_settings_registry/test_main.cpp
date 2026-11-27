#include <cstdio>
#include <cstring>

#include "foxie_embedded/settings_store.hpp"
#include "modules/settings_registry.hpp"
#include "modules/checkmate_animation_setting.hpp"
#include "modules/hint_thinking_style_setting.hpp"
#include "modules/settle_animation_setting.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }

    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void CheckKnownKeysPresent() {
    static constexpr const char* REQUIRED_KEYS[] = {
        "BRIGHTNESS",
        "WHITE_SQUARES_ON",
        "P1_PIECE_WHEEL",
        "P2_PIECE_WHEEL",
        "P1_END_PIECE_WHEEL",
        "P2_END_PIECE_WHEEL",
        "TURN_ANIM_STYLE",
        "HINT_THINKING_STYLE",
        "CHECKMATE_ANIM_STYLE",
        "MINIMAL_ANIMATIONS",
        "SETTLE_ANIM_STYLE",
        "MOVE_CONFIRM",
        "TAKEBACK_GRACE_SECONDS",
        "P1_X_DEEP_WAKE_ENABLED",
        "P1_X_WAKE_HOLD_MS",
        "BOARD_SETUP_ASSISTANCE",
        "VS_CPU_LEVEL",
        "TIMER",
        "SLEEP_TIMEOUT",
        "HINT_LEVEL",
        "BOARD_ROTATE",
        "GAME_FIRST_PLAYER",
        "RGBW_COOL_TO_WARM_THRESHOLD",
        "USB_CC_THRESH_500",
        "USB_CC_THRESH_1500",
        "USB_CC_THRESH_3000",
        "DEBUG_PICKUP",
        "GAME_FEN",
        "GAME_MOVED_SQUARES",
        "GAME_UNDO_BASE_FEN",
        "GAME_UNDO_BASE_MOVED_SQUARES",
        "GAME_UNDO_HISTORY",
        "SETTINGS_SAVE_TOTAL",
        "BLE_CONNECTION_INDICATOR",
        "V3_BUTTON_LED_EMULATION",
        "LIGHT_SQUARE_MODE",
        "LIGHT_SQUARE_HUE",
        "LIGHT_SQUARE_BRIGHTNESS",
    };

    for (const char* key : REQUIRED_KEYS) {
        const auto* descriptor = modules::settings_registry::LookupByKey(key);
        if (descriptor == nullptr) {
            std::printf("[FAIL] missing descriptor for key %s\n", key);
            ++g_failures;
            continue;
        }
        if (descriptor->key == nullptr) {
            std::printf("[FAIL] descriptor key pointer null for %s\n", key);
            ++g_failures;
            continue;
        }
    }
}

void CheckLookupBehavior() {
    const auto* upper = modules::settings_registry::LookupByKey("VS_CPU_LEVEL");

    Check(upper != nullptr, "canonical VS_CPU_LEVEL key exists");
    Check(modules::settings_registry::LookupByKey("vs_cpu_level") == nullptr,
          "legacy vs_cpu_level key does not exist");

    Check(modules::settings_registry::LookupByKey("DOES_NOT_EXIST") == nullptr,
          "unknown key lookup returns nullptr");
    Check(modules::settings_registry::LookupByKey(static_cast<const char*>(nullptr)) == nullptr,
          "null c-string key lookup returns nullptr");

    fxe::SettingsStoreConfig cfg;
    cfg.path = "build/test_settings_registry_lookup_ci.json";
    fxe::SettingsStore settings(cfg);

    StaticJsonDocument<64> doc;
    doc.set(4U);
    Check(modules::settings_registry::ValidateAndSet(settings, "brightness", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts lowercase key via case-insensitive lookup");
    Check(settings.Doc()["BRIGHTNESS"].as<uint32_t>() == 4U,
          "validate/set writes canonical key for case-insensitive lookup");
}

void CheckEnumeration() {
    size_t count = 0;
    const auto* all = modules::settings_registry::EnumerateAll(count);
    Check(all != nullptr, "enumeration returns non-null pointer");
    Check(count >= 22U, "registry contains expected minimum number of descriptors");

    size_t persistedCount = 0;
    size_t forceSaveCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& descriptor = all[i];
        Check(descriptor.key != nullptr, "enumerated descriptor has key");
        if (descriptor.IsPersisted()) {
            ++persistedCount;
        }
        if (descriptor.IsForceSave()) {
            ++forceSaveCount;
        }
    }

    Check(persistedCount + 4U == count,
          "all active descriptors are persisted; retired hardware-option slots are not");
    Check(forceSaveCount == 3U,
          "only the three USB-C thresholds use the must-save tier");
}

void CheckSpecificMetadata() {
    const auto* brightness = modules::settings_registry::LookupByKey("BRIGHTNESS");
    const auto* turnAnim = modules::settings_registry::LookupByKey("TURN_ANIM_STYLE");
    const auto* checkmateAnim =
        modules::settings_registry::LookupByKey("CHECKMATE_ANIM_STYLE");
    const auto* minimalAnimations =
        modules::settings_registry::LookupByKey("MINIMAL_ANIMATIONS");
    const auto* settleAnim =
        modules::settings_registry::LookupByKey("SETTLE_ANIM_STYLE");
    const auto* gameFen = modules::settings_registry::LookupByKey("GAME_FEN");
    const auto* rotate = modules::settings_registry::LookupByKey("BOARD_ROTATE");
    const auto* firstPlayer = modules::settings_registry::LookupByKey("GAME_FIRST_PLAYER");
    const auto* cpuPlayer = modules::settings_registry::LookupByKey("VS_CPU_PLAYER");
    const auto* sleepTimeout = modules::settings_registry::LookupByKey("SLEEP_TIMEOUT");
    const auto* v3ButtonLedEmulation =
        modules::settings_registry::LookupByKey("V3_BUTTON_LED_EMULATION");
    const auto* lightSquareMode =
        modules::settings_registry::LookupByKey("LIGHT_SQUARE_MODE");
    const auto* lightSquareHue =
        modules::settings_registry::LookupByKey("LIGHT_SQUARE_HUE");
    const auto* lightSquareBrightness =
        modules::settings_registry::LookupByKey("LIGHT_SQUARE_BRIGHTNESS");
    const auto* cc500 = modules::settings_registry::LookupByKey("USB_CC_THRESH_500");
    const auto* takebackGrace =
        modules::settings_registry::LookupByKey("TAKEBACK_GRACE_SECONDS");
    const auto* p1xHoldMs =
        modules::settings_registry::LookupByKey("P1_X_WAKE_HOLD_MS");
    const auto* boardSetupAssistance =
        modules::settings_registry::LookupByKey("BOARD_SETUP_ASSISTANCE");

    Check(brightness != nullptr, "brightness descriptor exists");
    if (brightness != nullptr) {
        Check(brightness->type == fxe::SettingType::UInt32, "brightness type is UInt32");
        Check(brightness->defaultUint == 5U, "brightness default is 5");
        Check(brightness->min == 0U && brightness->max == 8U, "brightness range is 0..8");
        Check(brightness->IsVisibleToApp(), "brightness visible flag set");
    }

    Check(turnAnim != nullptr, "turn animation descriptor exists");
    if (turnAnim != nullptr) {
        Check(turnAnim->type == fxe::SettingType::Enum, "turn animation type is Enum");
        Check(turnAnim->allowedCount == 6U, "turn animation appends None");
        const char* expected[] = {"active_end_only", "active_end_pingpong_sides", "both_ends", "side_middle_band", "away_from_active_end", "none"};
        for (uint8_t i = 0U; i < 6U; ++i) {
            Check(std::strcmp(turnAnim->allowedValues[i], expected[i]) == 0,
                  "historical Turn enum indices unchanged; None appended last");
        }
        Check(turnAnim->allowedValues != nullptr, "turn animation allowed values list exists");
    }

    Check(checkmateAnim != nullptr, "checkmate animation descriptor exists");
    if (checkmateAnim != nullptr) {
        Check(checkmateAnim->type == fxe::SettingType::Enum,
              "checkmate animation type is Enum");
        Check(checkmateAnim->allowedCount == 5U,
              "checkmate animation has 5 allowed values");
        Check(checkmateAnim->defaultStr != nullptr &&
                  std::strcmp(checkmateAnim->defaultStr, "fire-lob") == 0,
              "checkmate animation defaults to Fire Lob");
        static constexpr const char* EXPECTED_CHECKMATE_STYLES[] = {
            "fireworks",
            "fire-lob",
            "ripple-bloom",
            "winner-sweep",
            "winner-constellation",
        };
        for (size_t index = 0U;
             index < sizeof(EXPECTED_CHECKMATE_STYLES) /
                         sizeof(EXPECTED_CHECKMATE_STYLES[0]);
             ++index) {
            Check(checkmateAnim->allowedValues != nullptr &&
                      std::strcmp(checkmateAnim->allowedValues[index],
                                  EXPECTED_CHECKMATE_STYLES[index]) == 0,
                  "checkmate animation style order remains stable");
        }
    }

    Check(minimalAnimations != nullptr,
          "minimal animations descriptor exists");
    if (minimalAnimations != nullptr) {
        Check(minimalAnimations->type == fxe::SettingType::Bool,
              "minimal animations is a bool setting");
        Check(minimalAnimations->category == fxe::SettingCategory::Animation,
              "minimal animations is an animation setting");
        Check(minimalAnimations->defaultUint == 0U,
              "minimal animations defaults off");
        Check(minimalAnimations->IsVisibleToApp() &&
                  minimalAnimations->IsPersisted() &&
                  !minimalAnimations->IsInternal(),
              "minimal animations is an app-visible persisted setting");
    }

    Check(settleAnim != nullptr, "placement effect descriptor exists");
    if (settleAnim != nullptr) {
        Check(settleAnim->type == fxe::SettingType::Enum &&
                  settleAnim->category == fxe::SettingCategory::Animation,
              "placement effect is a named animation choice");
        Check(settleAnim->defaultStr != nullptr &&
                  std::strcmp(settleAnim->defaultStr, "ripple") == 0,
              "placement effect defaults to Ripple");
        Check(settleAnim->IsVisibleToApp() && settleAnim->IsPersisted() &&
                  !settleAnim->IsForceSave() && !settleAnim->IsInternal(),
              "placement effect uses ordinary persisted app settings");
        static constexpr const char* EXPECTED_STYLES[] = {"landing_flash", "mini_orbit", "ripple", "none", "rainbow_orbit"};
        Check(settleAnim->allowedCount == 5U,
              "placement effect offers five named choices");
        for (size_t index = 0U; index < 5U; ++index) {
            Check(settleAnim->allowedValues != nullptr &&
                      index < settleAnim->allowedCount &&
                      std::strcmp(settleAnim->allowedValues[index], EXPECTED_STYLES[index]) == 0,
                  "placement effect wire choice order remains LandingFlash0, MiniOrbit1, Ripple2, None3, RainbowOrbit4");
        }
    }
    Check(modules::settle_animation_setting::Parse(nullptr) ==
                  modules::SettleAnimationStyle::Ripple &&
              modules::settle_animation_setting::Parse("unknown") ==
                  modules::SettleAnimationStyle::Ripple &&
              modules::settle_animation_setting::Parse("mini_orbit") ==
                  modules::SettleAnimationStyle::MiniOrbit,
          "missing or stale style falls back to Ripple and an explicit Mini orbit uses its saved style");

    const auto* promotion = modules::settings_registry::LookupByKey("PROMOTION_MODE");
    Check(promotion != nullptr && promotion->type == fxe::SettingType::Enum &&
              promotion->category == fxe::SettingCategory::Behavior && promotion->IsPersisted() &&
              (promotion->flags & fxe::VISIBLE_TO_APP) != 0U && std::strcmp(promotion->defaultStr, "queen") == 0 &&
              promotion->allowedCount == 2U &&
              std::strcmp(promotion->allowedValues[0], "queen") == 0 &&
              std::strcmp(promotion->allowedValues[1], "choose") == 0,
          "promotion appends a persisted Queen-default / Choose Behavior enum");

    Check(lightSquareMode != nullptr &&
              lightSquareMode->type == fxe::SettingType::Enum &&
              lightSquareMode->defaultStr != nullptr &&
              std::strcmp(lightSquareMode->defaultStr, "white") == 0 &&
              lightSquareMode->allowedCount == 2U &&
              std::strcmp(lightSquareMode->allowedValues[0], "white") == 0 &&
              std::strcmp(lightSquareMode->allowedValues[1], "color") == 0,
          "light-square mode offers explicit White then Color and defaults to White");
    Check(lightSquareHue != nullptr &&
              lightSquareHue->type == fxe::SettingType::UInt32 &&
              lightSquareHue->min == 0U && lightSquareHue->max == 255U,
          "light-square hue exposes the continuous color wheel");
    Check(lightSquareBrightness != nullptr &&
              lightSquareBrightness->type == fxe::SettingType::UInt32 &&
              lightSquareBrightness->defaultUint == 4U &&
              lightSquareBrightness->min == 1U &&
              lightSquareBrightness->max == 8U,
          "light-square brightness preserves existing output by default and has no duplicate off value");

    Check(gameFen != nullptr, "game fen descriptor exists");
    if (gameFen != nullptr) {
        Check(gameFen->type == fxe::SettingType::String, "game fen type is String");
        Check(gameFen->IsInternal(), "game fen marked internal");
    }

    Check(rotate != nullptr, "board rotate descriptor exists");
    if (rotate != nullptr) {
        Check(rotate->type == fxe::SettingType::Bool, "board rotate type is Bool");
        Check(!rotate->IsForceSave(), "board rotate uses deferred save semantics");
    }

    Check(firstPlayer != nullptr, "game first player descriptor exists");
    if (firstPlayer != nullptr) {
        Check(firstPlayer->type == fxe::SettingType::UInt32, "game first player type is UInt32");
        Check(firstPlayer->defaultUint == 1U, "game first player default is 1");
        Check(firstPlayer->min == 1U && firstPlayer->max == 2U, "game first player range is 1..2");
        Check(!firstPlayer->IsForceSave(),
              "game first player uses deferred save semantics");
    }

    Check(cpuPlayer != nullptr, "VS CPU player descriptor exists");
    if (cpuPlayer != nullptr) {
        Check(cpuPlayer->type == fxe::SettingType::UInt32, "VS CPU player type is UInt32");
        Check(cpuPlayer->defaultUint == 2U, "VS CPU player defaults to P2");
        Check(cpuPlayer->min == 1U && cpuPlayer->max == 2U,
              "VS CPU player range is 1..2");
        Check(!cpuPlayer->IsForceSave(),
              "VS CPU player uses deferred save semantics");
    }

    Check(sleepTimeout != nullptr, "sleep timeout descriptor exists");
    if (sleepTimeout != nullptr) {
        Check(sleepTimeout->type == fxe::SettingType::Enum, "sleep timeout type is Enum");
        Check(sleepTimeout->allowedCount == 4U, "sleep timeout has 4 allowed values");
        Check(sleepTimeout->defaultStr != nullptr, "sleep timeout default string exists");
        Check(std::strcmp(sleepTimeout->defaultStr, "60m") == 0,
              "sleep timeout default string matches expected");
    }

    Check(v3ButtonLedEmulation != nullptr,
          "V3 button LED emulation descriptor exists");
    if (v3ButtonLedEmulation != nullptr) {
        Check(v3ButtonLedEmulation->type == fxe::SettingType::Bool &&
                  v3ButtonLedEmulation->defaultUint == 0U &&
                  v3ButtonLedEmulation->category ==
                      fxe::SettingCategory::Display &&
                  v3ButtonLedEmulation->IsRetired() &&
                  !v3ButtonLedEmulation->IsPersisted() &&
                  !modules::settings_registry::IsDirectAppAccessible(*v3ButtonLedEmulation),
              "V3 button LED emulation retains a retired, unsaved, unmodifiable Display slot");
    }

    Check(cc500 != nullptr && cc500->IsForceSave(),
          "USB-C thresholds use the must-save tier");
    Check(modules::settings_registry::UsesMustSaveTier("usb_cc_thresh_500"),
          "must-save policy lookup is case-insensitive");

    Check(takebackGrace != nullptr,
          "takeback grace descriptor exists");
    if (takebackGrace != nullptr) {
        Check(takebackGrace->type == fxe::SettingType::UInt32,
              "takeback grace type is UInt32");
        Check(takebackGrace->category ==
                  fxe::SettingCategory::Behavior,
              "takeback grace is a behavior setting");
        Check(takebackGrace->defaultUint == 0U,
              "takeback grace defaults off so Undo remains explicit");
        Check(takebackGrace->min == 0U &&
                  takebackGrace->max == 10U,
              "takeback grace range is 0..10 seconds");
        Check(takebackGrace->IsVisibleToApp(),
              "takeback grace is app-visible");
    }

    Check(p1xHoldMs != nullptr, "P1_X_WAKE_HOLD_MS descriptor exists");
    if (p1xHoldMs != nullptr) {
        Check(p1xHoldMs->IsRetired() &&
                  !modules::settings_registry::IsDirectAppAccessible(*p1xHoldMs),
              "the obsolete P1-X timing field is retired and cannot be mutated");
    }

    Check(boardSetupAssistance != nullptr,
          "board setup assistance descriptor exists");
    if (boardSetupAssistance != nullptr) {
        Check(boardSetupAssistance->type == fxe::SettingType::Bool,
              "board setup assistance is a bool setting");
        Check(boardSetupAssistance->category ==
                  fxe::SettingCategory::Behavior,
              "board setup assistance is a behavior setting");
        Check(boardSetupAssistance->defaultUint == 1U,
              "board setup assistance defaults on");
        Check(boardSetupAssistance->min == 0U &&
                  boardSetupAssistance->max == 1U,
              "board setup assistance has bool bounds");
        Check(boardSetupAssistance->description != nullptr &&
                  std::strcmp(boardSetupAssistance->description,
                              "Board setup assistance") == 0,
              "board setup assistance exposes the exact app title");
        Check(boardSetupAssistance->IsVisibleToApp() &&
                  boardSetupAssistance->IsPersisted() &&
                  !boardSetupAssistance->IsForceSave() &&
                  !boardSetupAssistance->IsInternal(),
              "board setup assistance is app-visible ordinary persistence");
    }

    size_t descriptorCount = 0U;
    const auto* descriptors =
        modules::settings_registry::EnumerateAll(descriptorCount);
    // Retirement must not renumber compact settings IDs on deployed apps.
    static constexpr const char* DEPLOYED_PREFIX[] = {
        "BRIGHTNESS", "WHITE_SQUARES_ON", "P1_PIECE_WHEEL", "P2_PIECE_WHEEL",
        "TURN_ANIM_STYLE", "CHECKMATE_ANIM_STYLE", "MOVE_CONFIRM", "VS_CPU_LEVEL",
        "HINT_LEVEL", "VS_CPU_PLAYER", "BOARD_ROTATE",
    };
    size_t visibleIndex = 0U;
    for (size_t index = 0U; index < descriptorCount; ++index) {
        const auto& descriptor = descriptors[index];
        if (!descriptor.IsVisibleToApp() || descriptor.IsInternal()) continue;
        if (visibleIndex < sizeof(DEPLOYED_PREFIX) / sizeof(DEPLOYED_PREFIX[0])) {
            Check(std::strcmp(descriptor.key, DEPLOYED_PREFIX[visibleIndex]) == 0,
                  "retired Hint slot preserves deployed BLE settings indices");
        }
        ++visibleIndex;
    }
    Check(visibleIndex >= sizeof(DEPLOYED_PREFIX) / sizeof(DEPLOYED_PREFIX[0]),
          "all deployed setting slots remain enumerable");
    const auto* automatic = modules::settings_registry::LookupByKey("AUTO_BRIGHTNESS");
    Check(automatic != nullptr, "automatic brightness is enumerated");
    Check(automatic != nullptr && automatic->defaultUint == 1U &&
              automatic->IsVisibleToApp() && automatic->IsPersisted() &&
              !automatic->IsForceSave(),
          "automatic brightness defaults on with ordinary persisted app visibility");
    bool takebackPrecedesMotion = false;
    for (size_t index = 1U; descriptors != nullptr &&
                            index < descriptorCount;
         ++index) {
        if (std::strcmp(descriptors[index - 1U].key,
                        "TAKEBACK_GRACE_SECONDS") == 0 &&
            std::strcmp(descriptors[index].key,
                        "MOTION_WAKE_ENABLED") == 0) {
            takebackPrecedesMotion = true;
            break;
        }
    }
    Check(takebackPrecedesMotion,
          "new BLE descriptors append after every deployed visible index");
    const size_t legacyCount = descriptorCount >= 7U ? descriptorCount - 7U : 0U;
    Check(descriptors != nullptr && legacyCount >= 12U &&
              std::strcmp(descriptors[legacyCount - 12U].key,
                          "P1_X_WAKE_HOLD_MS") == 0 &&
              std::strcmp(descriptors[legacyCount - 11U].key,
                          "BOARD_SETUP_ASSISTANCE") == 0 &&
              std::strcmp(descriptors[legacyCount - 10U].key,
                          "MINIMAL_ANIMATIONS") == 0 &&
              std::strcmp(descriptors[legacyCount - 9U].key,
                          "SETTLE_ANIM_STYLE") == 0 &&
              std::strcmp(descriptors[legacyCount - 8U].key,
                          "AUTO_BRIGHTNESS") == 0 &&
              std::strcmp(descriptors[legacyCount - 7U].key,
                          "BLE_CONNECTION_INDICATOR") == 0 &&
              std::strcmp(descriptors[legacyCount - 6U].key,
                          "V3_BUTTON_LED_EMULATION") == 0 &&
              std::strcmp(descriptors[legacyCount - 5U].key,
                          "LIGHT_SQUARE_MODE") == 0 &&
              std::strcmp(descriptors[legacyCount - 4U].key,
                          "LIGHT_SQUARE_HUE") == 0 &&
              std::strcmp(descriptors[legacyCount - 3U].key,
                          "LIGHT_SQUARE_BRIGHTNESS") == 0 &&
              std::strcmp(descriptors[legacyCount - 2U].key,
                          "HINT_THINKING_STYLE") == 0 &&
              std::strcmp(descriptors[legacyCount - 1U].key, "PROMOTION_MODE") == 0,
          "new display settings append after every deployed descriptor");
    const char* assistanceKeys[] = {"P1_LEGAL_MOVES", "P2_LEGAL_MOVES", "P1_CHECK_CUE", "P2_CHECK_CUE", "P1_AUTO_HINTS", "P2_AUTO_HINTS"};
    for (size_t index = 0U; index < 6U; ++index) {
        const auto& descriptor = descriptors[legacyCount + index];
        Check(std::strcmp(descriptor.key, assistanceKeys[index]) == 0 &&
                  descriptor.IsPersisted() && !descriptor.IsVisibleToApp(),
              "typed-only assistance appends without changing legacy metadata");
    }
    const auto& startup = descriptors[legacyCount + 6U];
    Check(std::strcmp(startup.key,"BOOT_DISPLAY")==0 && startup.IsPersisted() &&
              !startup.IsVisibleToApp() && startup.allowedCount==3U,
          "startup choice appends after assistance without exposing a legacy fallback");
}

void CheckCategoryAndFlagFiltering() {
    size_t count = 0;
    const auto* all = modules::settings_registry::EnumerateAll(count);
    Check(all != nullptr, "category/filter enumeration returns descriptors");

    size_t displayCount = 0;
    size_t internalCount = 0;
    size_t visibleCount = 0;
    size_t visibleInternalCount = 0;
    size_t retiredCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& descriptor = all[i];
        if (descriptor.category == fxe::SettingCategory::Display) {
            ++displayCount;
        }
        if (descriptor.category == fxe::SettingCategory::Internal) {
            ++internalCount;
        }
        if (descriptor.IsRetired()) {
            ++retiredCount;
            Check(!descriptor.IsPersisted() &&
                      !modules::settings_registry::IsDirectAppAccessible(descriptor),
                  "retired setting is neither persisted nor mutable through app");
        }
        if (descriptor.IsVisibleToApp()) {
            ++visibleCount;
            if (descriptor.IsInternal()) {
                ++visibleInternalCount;
            }
            if (descriptor.type == fxe::SettingType::String) {
                Check(false,
                      "app-visible strings require an explicit 31-byte snapshot bound");
            }
            if (descriptor.type == fxe::SettingType::Enum) {
                Check(descriptor.defaultStr != nullptr &&
                          std::strlen(descriptor.defaultStr) < 32U,
                      "visible enum default fits BLE snapshot value");
                for (size_t option = 0U;
                     option < descriptor.allowedCount;
                     ++option) {
                    Check(descriptor.allowedValues != nullptr &&
                              descriptor.allowedValues[option] != nullptr &&
                              std::strlen(descriptor.allowedValues[option]) < 32U,
                          "visible enum option fits BLE snapshot value");
                }
            }
        }
    }

    Check(retiredCount == 4U, "removed hardware options retain four reserved indices");
    Check(displayCount >= 4U, "display category descriptors present");
    Check(internalCount >= 1U, "internal category descriptors present");
    Check(visibleCount >= 1U, "visible-to-app descriptors present");
    Check(visibleCount <= 64U,
          "app-visible descriptors fit BLE notification snapshot");
    Check(visibleInternalCount == 0U, "VISIBLE_TO_APP filter excludes INTERNAL descriptors");
}

void CheckDefaultsAndFlags() {
    const auto* whiteSquares = modules::settings_registry::LookupByKey("WHITE_SQUARES_ON");
    const auto* turnAnim = modules::settings_registry::LookupByKey("TURN_ANIM_STYLE");
    const auto* gameFen = modules::settings_registry::LookupByKey("GAME_FEN");
    const auto* bleDeviceName =
        modules::settings_registry::LookupByKey("BLE_DEVICE_NAME");

    Check(whiteSquares != nullptr, "white squares descriptor exists");
    if (whiteSquares != nullptr) {
        Check(whiteSquares->defaultUint == 1U, "white squares default uint is 1");
    }

    Check(turnAnim != nullptr, "turn animation descriptor exists for default check");
    if (turnAnim != nullptr) {
        Check(turnAnim->defaultStr != nullptr, "turn animation default string exists");
        Check(std::strcmp(turnAnim->defaultStr, "active_end_pingpong_sides") == 0,
              "turn animation default string matches expected");
    }

    Check(gameFen != nullptr, "game fen descriptor exists for visibility/internal check");
    if (gameFen != nullptr) {
        Check(gameFen->IsInternal(), "game fen is internal");
        Check(!gameFen->IsVisibleToApp(), "game fen is not visible to app");
    }
    Check(bleDeviceName != nullptr,
          "BLE device name descriptor exists for direct app access");
    if (bleDeviceName != nullptr) {
        Check(bleDeviceName->defaultStr != nullptr &&
                  std::strcmp(bleDeviceName->defaultStr, "Foxie Litch") == 0,
              "default Bluetooth name uses the spaced product name");
        Check(!bleDeviceName->IsVisibleToApp(),
              "BLE device name stays out of the deployed indexed snapshot");
        Check(modules::settings_registry::IsDirectAppAccessible(*bleDeviceName),
              "BLE device name is available through exact-key app access");
    }
}

void CheckValidateAndSet() {
    fxe::SettingsStoreConfig cfg;
    cfg.path = "build/test_settings_registry_validation.json";
    fxe::SettingsStore settings(cfg);

    StaticJsonDocument<64> doc;

    doc.set(6U);
    Check(modules::settings_registry::ValidateAndSet(settings, "BRIGHTNESS", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts in-range uint32");
    Check(settings.Doc()["BRIGHTNESS"].as<uint32_t>() == 6U, "validate/set writes uint32 value");

    doc.set(99U);
    Check(modules::settings_registry::ValidateAndSet(settings, "BRIGHTNESS", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::OutOfRange,
          "validate/set rejects out-of-range uint32");
    Check(settings.Doc()["BRIGHTNESS"].as<uint32_t>() == 6U, "out-of-range write does not mutate setting");

    doc.set("6");
    Check(modules::settings_registry::ValidateAndSet(settings, "BRIGHTNESS", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::TypeMismatch,
          "validate/set rejects string for uint32 key");

    static constexpr const char* STYLES[] = {"landing_flash", "mini_orbit", "ripple", "none", "rainbow_orbit"};
    for (const char* style : STYLES) {
        doc.set(style);
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "SETTLE_ANIM_STYLE", doc.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::Ok,
              "all placement effects use the existing acknowledged settings path");
        Check(std::strcmp(settings.Doc()["SETTLE_ANIM_STYLE"].as<const char*>(), style) == 0,
              "placement effect write preserves the canonical token");
    }
    for (const char* retired : {"glow", "pulse"}) {
        doc.set(retired);
        Check(modules::settings_registry::ValidateAndSet(settings, "SETTLE_ANIM_STYLE", doc.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::InvalidEnumValue,
              "retired stored labels are migration-only and rejected at mutation ingress");
    }
    doc.set("splash");
    Check(modules::settings_registry::ValidateAndSet(
              settings, "SETTLE_ANIM_STYLE", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::InvalidEnumValue &&
              std::strcmp(settings.Doc()["SETTLE_ANIM_STYLE"].as<const char*>(), "rainbow_orbit") == 0,
          "unsupported placement effect is rejected without changing the board value");

    doc.set("settle_5s");
    Check(modules::settings_registry::ValidateAndSet(settings, "MOVE_CONFIRM", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts allowed enum value");
    Check(std::strcmp(settings.Doc()["MOVE_CONFIRM"].as<const char*>(), "settle_5s") == 0,
          "validate/set writes enum value");

    doc.set("invalid");
    Check(modules::settings_registry::ValidateAndSet(settings, "MOVE_CONFIRM", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::InvalidEnumValue,
          "validate/set rejects invalid enum value");

    doc.set("fire-lob");
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "CHECKMATE_ANIM_STYLE",
              doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts fire-lob checkmate animation");
    const char* storedCheckmateStyle =
        settings.Doc()["CHECKMATE_ANIM_STYLE"].as<const char*>();
    Check(storedCheckmateStyle != nullptr &&
              std::strcmp(storedCheckmateStyle, "fire-lob") == 0,
          "validate/set writes checkmate animation style");

    doc.set("ripple-bloom");
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "CHECKMATE_ANIM_STYLE",
              doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts ripple-bloom checkmate animation");

    doc.set(true);
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "MINIMAL_ANIMATIONS",
              doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok &&
              settings.Doc()["MINIMAL_ANIMATIONS"].as<bool>(),
          "validate/set accepts Minimal Animations");

    doc.set("ripple");
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "CHECKMATE_ANIM_STYLE",
              doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::InvalidEnumValue,
          "validate/set rejects unknown checkmate animation style");

    doc.set("30m");
    Check(modules::settings_registry::ValidateAndSet(settings, "SLEEP_TIMEOUT", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts allowed sleep timeout enum value");
    Check(std::strcmp(settings.Doc()["SLEEP_TIMEOUT"].as<const char*>(), "30m") == 0,
          "validate/set writes sleep timeout enum value");

    doc.set("disabled");
    Check(modules::settings_registry::ValidateAndSet(settings, "SLEEP_TIMEOUT", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts disabled sleep timeout enum value");
    Check(std::strcmp(settings.Doc()["SLEEP_TIMEOUT"].as<const char*>(), "disabled") == 0,
          "validate/set writes disabled sleep timeout enum value");

    doc.set("45m");
    Check(modules::settings_registry::ValidateAndSet(settings, "SLEEP_TIMEOUT", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::InvalidEnumValue,
          "validate/set rejects invalid sleep timeout enum value");

    doc.set(2U);
    Check(modules::settings_registry::ValidateAndSet(settings, "GAME_FIRST_PLAYER", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts GAME_FIRST_PLAYER numeric value");
    Check(settings.Doc()["GAME_FIRST_PLAYER"].as<uint32_t>() == 2U,
          "validate/set writes GAME_FIRST_PLAYER numeric value");

    doc.set("invalid");
    Check(modules::settings_registry::ValidateAndSet(settings, "GAME_FIRST_PLAYER", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::TypeMismatch,
          "validate/set rejects string GAME_FIRST_PLAYER value");

    doc.set(1U);
    Check(modules::settings_registry::ValidateAndSet(settings, "WHITE_SQUARES_ON", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts numeric bool coercion");
    Check(settings.Doc()["WHITE_SQUARES_ON"].as<bool>(), "validate/set writes bool value");

    doc.set(2U);
    Check(modules::settings_registry::ValidateAndSet(settings, "WHITE_SQUARES_ON", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::OutOfRange,
          "validate/set rejects invalid numeric bool value");

    doc.set("value");
    Check(modules::settings_registry::ValidateAndSet(settings, "UNKNOWN_KEY", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::UnknownKey,
          "validate/set rejects unknown key");

    doc.set(0U);
    Check(modules::settings_registry::ValidateAndSet(settings, "BRIGHTNESS", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts new near-dark zero");
    Check(settings.Doc()["BRIGHTNESS"].as<uint32_t>() == 0U && settings.Doc().containsKey("WHITE_SQUARES_ON"),
          "zero stores independent white-square marker");

    doc.set(8U);
    Check(modules::settings_registry::ValidateAndSet(settings, "BRIGHTNESS", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts brightness upper bound");
    Check(settings.Doc()["BRIGHTNESS"].as<uint32_t>() == 8U,
          "upper-bound write mutates brightness");

    doc.set(true);
    Check(modules::settings_registry::ValidateAndSet(settings, "WHITE_SQUARES_ON", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts bool type for bool descriptor");

    doc.set(100U);
    Check(modules::settings_registry::ValidateAndSet(settings, "P1_PIECE_WHEEL", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts P1 wheel write");

    doc.set(132U);
    Check(modules::settings_registry::ValidateAndSet(settings, "P2_PIECE_WHEEL", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts P2 wheel write at exclusion boundary");
    Check(settings.Doc()["P2_PIECE_WHEEL"].as<uint32_t>() == 132U,
          "P2 wheel accepts exactly +/-32 separation");

    doc.set(5U);
    Check(modules::settings_registry::ValidateAndSet(settings, "P1_PIECE_WHEEL", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts second P1 wheel write");

    doc.set(250U);
    Check(modules::settings_registry::ValidateAndSet(settings, "P2_PIECE_WHEEL", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts wrapped P2 wheel write");
    Check(settings.Doc()["P2_PIECE_WHEEL"].as<uint32_t>() == 229U,
          "P2 wheel wrap-around snaps to nearest allowed value");

    doc.set(60U);
    Check(modules::settings_registry::ValidateAndSet(settings, "P2_PIECE_WHEEL", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts P2 wheel baseline write");

    doc.set(80U);
    Check(modules::settings_registry::ValidateAndSet(settings, "P1_PIECE_WHEEL", doc.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "validate/set accepts P1 wheel close write");
    Check(settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() == 92U,
          "P1 wheel also enforces exclusion relative to P2");
}


void CheckAnimationPreferenceDefaults() {
    namespace mate = modules::checkmate_animation_setting;
    namespace hint = modules::hint_thinking_style_setting;
    modules::SettingsStore settings;
    Check(std::strcmp(mate::DEFAULT_VALUE, "fire-lob") == 0 &&
              mate::Read(settings) == modules::CheckmateAnimationStyle::FireLob &&
              mate::Parse(nullptr) == modules::CheckmateAnimationStyle::FireLob &&
              mate::Parse("unknown") == modules::CheckmateAnimationStyle::FireLob,
          "absent and invalid renderer preferences use Fire Lob");
    const char* values[]{"fireworks", "fire-lob", "ripple-bloom",
                         "winner-sweep", "winner-constellation"};
    for (uint8_t index = 0U; index < 5U; ++index) {
        settings.Doc()[mate::KEY] = values[index];
        (void)modules::settings_registry::CanonicalizeLoadedSettings(settings.Doc());
        Check(std::strcmp(settings.ReadValue(mate::KEY, ""), values[index]) == 0 &&
                  static_cast<uint8_t>(mate::Read(settings)) == index,
              "new default preserves every saved valid checkmate choice and enum index");
    }
    const auto* descriptor = modules::settings_registry::LookupByKey(hint::KEY);
    Check(descriptor != nullptr && descriptor->allowedCount == 5U &&
              std::strcmp(descriptor->defaultStr, "cauldron_brewing") == 0 &&
              std::strcmp(descriptor->allowedValues[0U], "foxie_sweep") == 0 &&
              std::strcmp(descriptor->allowedValues[1U], "player_clockwise") == 0 &&
              std::strcmp(descriptor->allowedValues[2U], "none") == 0 &&
              std::strcmp(descriptor->allowedValues[3U], "beachball") == 0 &&
              std::strcmp(descriptor->allowedValues[4U], "cauldron_brewing") == 0,
          "Cauldron is the fresh default without moving any saved alternative wire value");
    StaticJsonDocument<32> value;
    value.set("none");
    Check(modules::settings_registry::ValidateAndSet(
              settings, hint::KEY, value.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok &&
              hint::Parse(settings.ReadValue(hint::KEY, "")) == hint::Style::None,
          "the common mutation owner accepts and preserves explicit None");
    for (const char* saved : {"foxie_sweep", "player_clockwise", "none", "beachball", "cauldron_brewing"}) {
        settings.Doc()[hint::KEY] = saved;
        (void)modules::settings_registry::CanonicalizeLoadedSettings(settings.Doc());
        Check(std::strcmp(settings.ReadValue(hint::KEY, ""), saved) == 0,
              "canonical load retains every historical Hint style and Beachball preference");
    }
    value.set("beachball");
    Check(modules::settings_registry::ValidateAndSet(settings, hint::KEY, value.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok &&
              hint::Parse(settings.ReadValue(hint::KEY, "")) == hint::Style::Beachball,
          "the existing mutation owner validates persisted Beachball");
    for (const char* invalid : {"", "unknown"}) {
        settings.Doc()[hint::KEY] = invalid;
        (void)modules::settings_registry::CanonicalizeLoadedSettings(settings.Doc());
        Check(hint::Parse(settings.ReadValue(hint::KEY, "")) == hint::Style::CauldronBrewing &&
                  std::strcmp(settings.ReadValue(hint::KEY, ""), invalid) == 0,
              "invalid stored tokens render the Cauldron fallback without silently rewriting persistence");
    }
    settings.Doc().remove(hint::KEY);
    Check(std::strcmp(settings.ReadValue(hint::KEY, hint::DEFAULT_VALUE), "cauldron_brewing") == 0,
          "an absent preference uses the same Cauldron default");
}

void CheckCanonicalizeLoadedSettings() {
    StaticJsonDocument<512> doc;
    doc["WHITE_SQUARES_ON"] = 1U;
    doc["BOARD_ROTATE"] = 0U;
    doc["BOARD_SETUP_ASSISTANCE"] = 1U;
    doc["MINIMAL_ANIMATIONS"] = 0U;
    doc["V3_BUTTON_LED_EMULATION"] = 1U;
    doc["SLEEP_TIMEOUT"] = "5m";
    doc["BRIGHTNESS"] = 4U;
    doc["BLE_CONNECTION_INDICATOR"] = true;

    const bool mutated = modules::settings_registry::CanonicalizeLoadedSettings(doc);
    Check(!doc.containsKey("BLE_CONNECTION_INDICATOR"),
          "loaded retired connection-light preference is permanently discarded");
    Check(mutated, "canonicalize loaded settings mutates numeric bool values");
    Check(doc["WHITE_SQUARES_ON"].is<bool>() && doc["WHITE_SQUARES_ON"].as<bool>(),
          "white squares converted to bool true");
    Check(doc["BOARD_ROTATE"].is<bool>() && !doc["BOARD_ROTATE"].as<bool>(),
          "board rotate converted to bool false");
    Check(doc["BOARD_SETUP_ASSISTANCE"].is<bool>() &&
              doc["BOARD_SETUP_ASSISTANCE"].as<bool>(),
          "board setup assistance converted to bool true");
    Check(doc["MINIMAL_ANIMATIONS"].is<bool>() &&
              !doc["MINIMAL_ANIMATIONS"].as<bool>(),
          "minimal animations converted to bool false");
    Check(!doc.containsKey("V3_BUTTON_LED_EMULATION"),
          "loaded retired button-emulation preference is discarded without resetting other data");
    Check(std::strcmp(doc["SLEEP_TIMEOUT"].as<const char*>(), "5m") == 0,
          "new sleep default preserves an existing persisted timeout");
    Check(doc["BRIGHTNESS"].is<uint32_t>() && doc["BRIGHTNESS"].as<uint32_t>() == 4U,
          "non-bool setting remains unchanged");
}

void CheckPlayerEndPaletteCompatibility() {
    StaticJsonDocument<512> migrated;
    migrated["GAME_FIRST_PLAYER"] = 2U;
    migrated["P1_PIECE_WHEEL"] = 170U;
    migrated["P2_PIECE_WHEEL"] = 0U;
    Check(modules::settings_registry::CanonicalizeLoadedSettings(migrated),
          "palette migration adds canonical physical-end values");
    Check(migrated["P1_END_PIECE_WHEEL"].as<uint32_t>() == 170U &&
              migrated["P2_END_PIECE_WHEEL"].as<uint32_t>() == 0U,
          "legacy-only upgrade imports same-player palettes regardless of first mover");

    fxe::SettingsStoreConfig cfg;
    cfg.path = "build/test_settings_registry_palette_compat.json";
    fxe::SettingsStore settings(cfg);
    settings.Doc().set(migrated.as<JsonVariantConst>());
    settings.NotifyDocChanged();

    const auto* legacyP1 =
        modules::settings_registry::LookupByKey("P1_PIECE_WHEEL");
    const auto* legacyP2 =
        modules::settings_registry::LookupByKey("P2_PIECE_WHEEL");
    const auto* physicalP1 =
        modules::settings_registry::LookupByKey("P1_END_PIECE_WHEEL");
    const auto* physicalP2 =
        modules::settings_registry::LookupByKey("P2_END_PIECE_WHEEL");
    Check(legacyP1 != nullptr && legacyP2 != nullptr &&
              physicalP1 != nullptr && physicalP2 != nullptr,
          "logical aliases and physical-end descriptors all exist");
    if (legacyP1 != nullptr && legacyP2 != nullptr &&
        physicalP1 != nullptr && physicalP2 != nullptr) {
        Check(modules::settings_registry::ReadCurrentUInt32(
                  settings, *legacyP1) == 170U &&
                  modules::settings_registry::ReadCurrentUInt32(
                      settings, *legacyP2) == 0U,
              "old app reads logical-player palette aliases");
        Check(modules::settings_registry::ReadCurrentUInt32(
                  settings, *physicalP1) == 170U &&
                  modules::settings_registry::ReadCurrentUInt32(
                      settings, *physicalP2) == 0U,
              "new app direct reads see fixed physical-end palettes");
        Check(legacyP1->IsVisibleToApp() &&
                  modules::settings_registry::IsDirectAppAccessible(*legacyP1),
              "legacy palette aliases remain app-visible");
        Check(!physicalP1->IsVisibleToApp() && physicalP1->IsInternal() &&
                  modules::settings_registry::IsDirectAppAccessible(*physicalP1),
              "physical palette key is direct-only and hidden from enumeration");
    }

    StaticJsonDocument<32> value;
    value.set(43U);
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "P1_END_PIECE_WHEEL",
              value.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "new app writes physical P1-controls palette directly");
    Check(settings.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == 43U &&
              settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() == 43U,
          "physical write synchronizes the same-player logical P1 alias");

    value.set(213U);
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "P1_PIECE_WHEEL",
              value.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "old app writes logical P1 palette alias");
    Check(settings.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == 213U &&
              settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() == 213U,
          "logical P1 write always targets the P1 physical end");

    value.set(1U);
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "GAME_FIRST_PLAYER",
              value.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "player-end reassignment succeeds in compatibility fixture");
    Check(settings.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == 213U &&
              settings.Doc()["P2_END_PIECE_WHEEL"].as<uint32_t>() == 0U &&
              settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() == 213U &&
              settings.Doc()["P2_PIECE_WHEEL"].as<uint32_t>() == 0U,
          "first-mover change preserves physical colors and same-player aliases");

    // After the first migration, a legacy-only edit made by downgraded
    // firmware is imported into the same logical player's canonical value.
    settings.Doc()["P1_PIECE_WHEEL"] = 85U;
    Check(modules::settings_registry::CanonicalizeLoadedSettings(
              settings.Doc()),
          "upgrade detects a legacy palette write after downgrade");
    Check(settings.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == 85U &&
              settings.Doc()["P2_END_PIECE_WHEEL"].as<uint32_t>() == 0U,
          "post-migration downgrade edits remain recoverable");

    value.set(2U);
    Check(modules::settings_registry::ValidateAndSet(
              settings,
              "GAME_FIRST_PLAYER",
              value.as<JsonVariantConst>()) ==
              modules::settings_registry::ValidateAndSetResult::Ok,
          "downgrade reset fixture moves logical P1 to the opposite end");
    settings.Doc().remove("P1_PIECE_WHEEL");
    Check(modules::settings_registry::CanonicalizeLoadedSettings(
              settings.Doc()),
          "re-upgrade detects an old-firmware alias reset to default");
    Check(settings.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == 170U &&
              settings.Doc()["P2_END_PIECE_WHEEL"].as<uint32_t>() == 0U &&
              settings.Doc()["P1_PIECE_WHEEL"].as<uint32_t>() == 170U &&
              settings.Doc()["P2_PIECE_WHEEL"].as<uint32_t>() == 0U,
          "post-migration downgrade alias reset imports the same-player default");
}

void CheckPaletteMigrationProvenanceRoundtrip() {
    constexpr const char* marker = "PLAYER_PALETTE_V1";
    constexpr const char* path = "build/test_palette_provenance.json";
    const auto clean = [&]() {
        (void)std::remove(path);
        (void)std::remove("build/test_palette_provenance.json.bak");
        (void)std::remove("build/test_palette_provenance.json.tmp");
    };
    Check(modules::settings_registry::LookupByKey(marker) == nullptr,
          "migration marker is private provenance, not an app-visible setting");
    for (const uint32_t firstPlayer : {1U, 2U}) {
        clean();
        fxe::SettingsStoreConfig cfg;
        cfg.path = path;
        {
            fxe::SettingsStore firstUpgrade(cfg);
            auto& doc = firstUpgrade.Doc();
            doc["GAME_FIRST_PLAYER"] = firstPlayer;
            doc["P1_END_PIECE_WHEEL"] = 40U;
            doc["P2_END_PIECE_WHEEL"] = 200U;
            doc["P1_PIECE_WHEEL"] = 200U;
            doc["P2_PIECE_WHEEL"] = 40U;
            Check(modules::settings_registry::CanonicalizeLoadedSettings(doc) &&
                      doc["P1_END_PIECE_WHEEL"].as<uint32_t>() == 40U &&
                      doc["P2_END_PIECE_WHEEL"].as<uint32_t>() == 200U &&
                      doc["P1_PIECE_WHEEL"].as<uint32_t>() == 40U &&
                      doc["P2_PIECE_WHEEL"].as<uint32_t>() == 200U &&
                      doc[marker].as<uint32_t>() == 1U && !doc.overflowed(),
                  "first upgrade preserves canonical colors over swapped aliases and establishes provenance");
            Check(!modules::settings_registry::CanonicalizeLoadedSettings(doc),
                  "completed palette migration is idempotent");
            Check(firstUpgrade.SaveNow(), "first palette migration persists with settings");
        }
        {
            // Generic SettingsStore is the old alias-only firmware seam: it
            // does not know the private marker, but preserves it through I/O.
            fxe::SettingsStore older(cfg);
            Check(older.Load() && older.Doc()[marker].as<uint32_t>() == 1U,
                  "older settings reader retains unknown provenance key");
            older.Doc()["P1_PIECE_WHEEL"] = 85U;
            Check(older.SaveNow(), "downgraded firmware saves genuine P1 alias edit");
        }
        cfg.migrateFunc = [](JsonDocument& doc) {
            return modules::settings_registry::CanonicalizeLoadedSettings(doc);
        };
        {
            fxe::SettingsStore upgraded(cfg);
            Check(upgraded.Load() &&
                      upgraded.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == 85U &&
                      upgraded.Doc()["P2_END_PIECE_WHEEL"].as<uint32_t>() == 200U &&
                      upgraded.Doc()[marker].as<uint32_t>() == 1U,
                  "later upgrade imports the genuine same-player alias edit for either first-player setting");
            Check(upgraded.SaveNow(), "accepted downgrade edit saves synchronized aliases");
        }
        for (const bool resetP1 : {false, true}) {
            auto oldCfg = cfg;
            oldCfg.migrateFunc = {};
            {
                fxe::SettingsStore older(oldCfg);
                Check(older.Load(), "downgrade reset reloads persisted palette provenance");
                older.Doc().remove(resetP1 ? "P1_PIECE_WHEEL" : "P2_PIECE_WHEEL");
                Check(older.SaveNow(), "older firmware persists an alias reset by removal");
            }
            fxe::SettingsStore upgraded(cfg);
            Check(upgraded.Load() &&
                      upgraded.Doc()["P1_END_PIECE_WHEEL"].as<uint32_t>() == (resetP1 ? 170U : 85U) &&
                      upgraded.Doc()["P2_END_PIECE_WHEEL"].as<uint32_t>() == 0U &&
                      upgraded.Doc()[marker].as<uint32_t>() == 1U,
                  "later downgrade reset restores only the same-player default without swapping colors");
            Check(upgraded.SaveNow(), "reset palette aliases persist coherently");
        }
    }
    clean();
}

void CheckPlacementMigrationRoundtrip() {
    constexpr const char* path = "build/test_placement_style_migration.json";
    const auto clean = [&]() {
        (void)std::remove(path);
        (void)std::remove("build/test_placement_style_migration.json.bak");
        (void)std::remove("build/test_placement_style_migration.json.tmp");
    };
    for (const char* previous : {"glow", "pulse", "mini_orbit"}) {
        clean();
        fxe::SettingsStoreConfig config;
        config.path = path;
        {
            fxe::SettingsStore older(config);
            Check(older.Load(), "old settings file loads before the upgrade");
            older.Doc()["SETTLE_ANIM_STYLE"] = previous;
            older.Doc()["BRIGHTNESS"] = 3U;
            older.Doc()["UNCHANGED_SETTING"] = "retained";
            Check(older.SaveNow(), "existing placement choice is genuinely persisted");
        }
        config.migrateFunc = [](JsonDocument& document) {
            return modules::settings_registry::CanonicalizeLoadedSettings(document);
        };
        const char* expected = std::strcmp(previous, "glow") == 0 ? "landing_flash" : "mini_orbit";
        {
            fxe::SettingsStore current(config);
            Check(current.Load() && std::strcmp(current.ReadValue("SETTLE_ANIM_STYLE", ""), expected) == 0 &&
                      current.ReadValue("BRIGHTNESS", 0U) == 3U &&
                      std::strcmp(current.ReadValue("UNCHANGED_SETTING", ""), "retained") == 0,
                  "production load migration retains ordinal selection and unrelated saved values");
            Check(current.SaveNow(), "canonical choice is durably saved through the existing settings owner");
        }
        config.migrateFunc = {};
        fxe::SettingsStore reloaded(config);
        Check(reloaded.Load() && std::strcmp(reloaded.ReadValue("SETTLE_ANIM_STYLE", ""), expected) == 0 &&
                  reloaded.ReadValue("BRIGHTNESS", 0U) == 3U,
              "a second real reload reads the canonical saved value without an alias fallback");
    }
    clean();
}

void CheckBoardSetupAssistancePersistence() {
    static constexpr const char* PATH =
        "build/test_settings_registry_board_setup_assistance.json";
    static constexpr const char* BACKUP_PATH =
        "build/test_settings_registry_board_setup_assistance.json.bak";
    static constexpr const char* TMP_PATH =
        "build/test_settings_registry_board_setup_assistance.json.tmp";
    (void)std::remove(PATH);
    (void)std::remove(BACKUP_PATH);
    (void)std::remove(TMP_PATH);

    {
        fxe::SettingsStoreConfig cfg;
        cfg.path = PATH;
        fxe::SettingsStore settings(cfg);
        Check(settings.Load(),
              "board setup assistance persistence writer loads defaults");
        Check(settings.ReadValue("BOARD_SETUP_ASSISTANCE", true),
              "absent board setup assistance resolves to default on");

        StaticJsonDocument<16> value;
        value.set(false);
        Check(modules::settings_registry::ValidateAndSet(
                  settings,
                  "BOARD_SETUP_ASSISTANCE",
                  value.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::Ok,
              "board setup assistance accepts false before persistence");
        Check(modules::settings_registry::ValidateAndSet(settings, "AUTO_BRIGHTNESS",
                  value.as<JsonVariantConst>()) == modules::settings_registry::ValidateAndSetResult::Ok,
              "automatic brightness accepts false");
        StaticJsonDocument<32> appearance;
        appearance.set("color");
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "LIGHT_SQUARE_MODE",
                  appearance.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::Ok,
              "light-square color mode validates before persistence");
        appearance.set(91U);
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "LIGHT_SQUARE_HUE",
                  appearance.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::Ok,
              "light-square hue validates before persistence");
        appearance.set(7U);
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "LIGHT_SQUARE_BRIGHTNESS",
                  appearance.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::Ok,
              "light-square brightness validates before persistence");
        Check(settings.SaveNow(),
              "board setup assistance and automatic brightness false values persist");
    }

    {
        fxe::SettingsStoreConfig cfg;
        cfg.path = PATH;
        fxe::SettingsStore settings(cfg);
        Check(settings.Load(),
              "board setup assistance persistence reader loads saved value");
        Check(settings.Doc()["AUTO_BRIGHTNESS"].is<bool>() &&
                  !settings.ReadValue("AUTO_BRIGHTNESS", true),
              "automatic brightness false survives save and reload");
        Check(settings.Doc()["BOARD_SETUP_ASSISTANCE"].is<bool>() &&
                  !settings.ReadValue("BOARD_SETUP_ASSISTANCE", true),
              "board setup assistance false value survives save and reload");
        Check(std::strcmp(
                  settings.ReadValue("LIGHT_SQUARE_MODE", "white"),
                  "color") == 0 &&
                  settings.ReadValue("LIGHT_SQUARE_HUE", 0U) == 91U &&
                  modules::settings_registry::ReadCurrentUInt32(settings,
                      *modules::settings_registry::LookupByKey("LIGHT_SQUARE_BRIGHTNESS")) == 7U,
              "light-square mode, hue, and brightness survive save and reload together");
    }

    (void)std::remove(PATH);
    (void)std::remove(BACKUP_PATH);
    (void)std::remove(TMP_PATH);
}

} // namespace

int main() {
    {
        fxe::SettingsStore settings;
        StaticJsonDocument<256> value;
        for (const char* text : {"\"hello\\u0000hidden\"", "\"hello\\nworld\"", "\"hello\\u001b[31m\""}) {
            Check(!deserializeJson(value, text), "hostile string fixture parses");
            Check(modules::settings_registry::ValidateAndSet(
                      settings, "BLE_DEVICE_NAME", value.as<JsonVariantConst>()) ==
                      modules::settings_registry::ValidateAndSetResult::OutOfRange,
                  "registry rejects controls before runtime mutation or persistence scrub");
        }

        value.set("Lích chess");
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "BLE_DEVICE_NAME", value.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::Ok,
              "registry accepts a bounded international board name");
        const std::string retained =
            settings.ReadValue("BLE_DEVICE_NAME", "");

        const char malformed[] = {static_cast<char>(0xE6),
                                  static_cast<char>(0xA3), '\0'};
        value.set(malformed);
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "BLE_DEVICE_NAME", value.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::OutOfRange,
              "registry rejects malformed UTF-8");

        value.set("name\xE2\x80\xAEhidden");
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "BLE_DEVICE_NAME", value.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::OutOfRange,
              "registry rejects directional overrides");

        value.set("123456789012345678901");
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "BLE_DEVICE_NAME", value.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::OutOfRange,
              "registry enforces the 20-character board-name limit");

        value.set("棋棋棋棋棋棋棋棋棋棋棋");
        Check(modules::settings_registry::ValidateAndSet(
                  settings, "BLE_DEVICE_NAME", value.as<JsonVariantConst>()) ==
                  modules::settings_registry::ValidateAndSetResult::OutOfRange,
              "registry rejects a multibyte name beyond the transport buffer");
        Check(std::strcmp(settings.ReadValue("BLE_DEVICE_NAME", ""),
                          retained.c_str()) == 0,
              "rejected board names preserve the previous valid value");
    }
    CheckKnownKeysPresent();
    CheckLookupBehavior();
    CheckEnumeration();
    CheckSpecificMetadata();
    CheckCategoryAndFlagFiltering();
    CheckDefaultsAndFlags();
    CheckAnimationPreferenceDefaults();
    CheckValidateAndSet();
    CheckCanonicalizeLoadedSettings();
    CheckPaletteMigrationProvenanceRoundtrip();
    CheckPlayerEndPaletteCompatibility();
    CheckBoardSetupAssistancePersistence();
    CheckPlacementMigrationRoundtrip();

    if (g_failures == 0) {
        std::puts("[TEST] settings_registry ok");
        return 0;
    }

    std::printf("[TEST] settings_registry failures=%d\n", g_failures);
    return 1;
}
