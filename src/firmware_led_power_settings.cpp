#ifdef ARDUINO

#include "firmware/firmware_internal.hpp"

namespace firmware_internal {

bool IsLedBrownoutLimitLatched(const modules::SettingsStore& settings) {
    const JsonVariantConst raw =
        settings.Doc()[LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY];
    if (raw.isNull()) {
        return false;
    }
    if (raw.is<bool>()) {
        return raw.as<bool>();
    }
    return settings.ReadValue(
        LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY, false);
}

bool SetLedBrownoutLimitLatched(modules::SettingsStore& settings,
                                const bool latched) {
    JsonObject doc = settings.Doc().as<JsonObject>();
    const bool current = IsLedBrownoutLimitLatched(settings);
    if (latched == current &&
        (latched ||
         !doc.containsKey(LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY))) {
        return false;
    }

    if (latched) {
        doc[LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY] = true;
    } else {
        doc.remove(LED_BROWNOUT_LIMIT_LATCH_SETTING_KEY);
    }

    settings.ArmPersistenceGuard();
    settings.MarkMustSaveDirty();
    return true;
}

hardware::UsbCcThresholds LoadUsbCcThresholds(
    const modules::SettingsStore& settings) {
    const hardware::UsbCcThresholds defaults =
        hardware::GetDefaultUsbCcThresholds();
    const uint32_t t500 = settings.ReadValue(
        "USB_CC_THRESH_500", static_cast<uint32_t>(defaults.level500));
    const uint32_t t1500 = settings.ReadValue(
        "USB_CC_THRESH_1500", static_cast<uint32_t>(defaults.level1500));
    const uint32_t t3000 = settings.ReadValue(
        "USB_CC_THRESH_3000", static_cast<uint32_t>(defaults.level3000));
    return hardware::ResolveUsbCcThresholds(t500, t1500, t3000);
}

} // namespace firmware_internal

#endif
