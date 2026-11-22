#include "hardware/i2c_clock_settings.hpp"

#include "modules/string_parsing.hpp"

namespace hardware::i2c_clock_settings {
namespace {

bool IsSupported(const uint32_t hz) {
    return hz == DEFAULT_CLOCK_HZ || hz == 400000U || hz == 1000000U;
}

} // namespace

bool TryParse(const std::string_view token, uint32_t& outHz) {
    uint32_t hz = 0U;
    if (modules::string_parsing::EqualsNoCaseAscii(token, "100k") ||
        modules::string_parsing::EqualsNoCaseAscii(token, "100khz")) {
        hz = DEFAULT_CLOCK_HZ;
    } else if (modules::string_parsing::EqualsNoCaseAscii(token, "400k") ||
               modules::string_parsing::EqualsNoCaseAscii(token, "400khz")) {
        hz = 400000U;
    } else if (modules::string_parsing::EqualsNoCaseAscii(token, "1m")) {
        hz = 1000000U;
    } else if (!modules::string_parsing::TryParseU32(token, hz) ||
               !IsSupported(hz)) {
        return false;
    }
    outHz = hz;
    return true;
}

uint32_t Read(const fxe::SettingsStore& settings) {
    const uint32_t hz = settings.ReadValue(SETTING_KEY, DEFAULT_CLOCK_HZ);
    return IsSupported(hz) ? hz : DEFAULT_CLOCK_HZ;
}

bool TrySelect(fxe::SettingsStore& settings,
               const std::string_view token,
               uint32_t& outHz) {
    uint32_t hz = 0U;
    if (!TryParse(token, hz)) {
        return false;
    }
    settings.Doc()[SETTING_KEY] = hz;
    settings.MarkDirty();
    outHz = hz;
    return true;
}

} // namespace hardware::i2c_clock_settings
