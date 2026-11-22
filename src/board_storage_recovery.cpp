#include "modules/board_storage_recovery.hpp"

#include "modules/board_identity_persistence.hpp"
#include "modules/hardware_test_record.hpp"
#include "hardware/usb_cc_sense.hpp"

namespace modules {
bool BuildBoardStorageRecoverySnapshot(const JsonDocument& live,
                                       JsonDocument& safe) {
    safe.clear();
    safe.to<JsonObject>();
    BoardIdentity identity;
    if (ReadBoardIdentity(live, identity) == BoardIdentityRead::Current &&
        !WriteBoardIdentity(safe, identity)) return false;
    const auto record = live[HARDWARE_TEST_RECORD_SETTING_KEY];
    HardwareTestRecord hardwareRecord;
    if (record.is<const char*>() &&
        DecodeHardwareTestRecord(record.as<const char*>(), hardwareRecord) &&
        !WriteHardwareTestRecord(safe, hardwareRecord)) return false;
    const auto latch = live["LED_BROWNOUT_LIMIT_LATCH"];
    const bool latched = latch.is<bool>() ? latch.as<bool>()
        : latch.is<uint64_t>() ? latch.as<uint64_t>() != 0U
        : latch.is<int64_t>() && latch.as<int64_t>() != 0;
    if (latched) safe["LED_BROWNOUT_LIMIT_LATCH"] = true;
    const auto incidents = live["LED_BROWNOUT_INCIDENT_COUNT"];
    const uint32_t count = incidents.is<bool>()
        ? (incidents.as<bool>() ? 1U : 0U) : (incidents | 0U);
    if (count != 0U) {
        safe["LED_BROWNOUT_INCIDENT_COUNT"] = count > 2U ? 2U : count;
    }
    const auto cool = live["RGBW_COOL_TO_WARM_THRESHOLD"];
    if (cool.is<uint32_t>() && cool.as<uint32_t>() <= 255U) {
        safe["RGBW_COOL_TO_WARM_THRESHOLD"] = cool;
    }
    constexpr const char* keys[] = {
        "USB_CC_THRESH_500", "USB_CC_THRESH_1500", "USB_CC_THRESH_3000"};
    bool valid = true;
    uint32_t values[3U]{};
    for (size_t index = 0U; index < 3U; ++index) {
        const auto value = live[keys[index]];
        valid = valid && value.is<uint32_t>();
        values[index] = value | 0U;
        valid = valid && values[index] <= 65535U;
    }
    // Invalid calibration returns to the existing conservative defaults.
    valid = valid && hardware::AreUsbCcThresholdsValid({
        static_cast<uint16_t>(values[0U]), static_cast<uint16_t>(values[1U]),
        static_cast<uint16_t>(values[2U])});
    if (valid) {
        for (size_t index = 0U; index < 3U; ++index) {
            safe[keys[index]] = values[index];
        }
    }
    return !safe.overflowed();
}
} // namespace modules
