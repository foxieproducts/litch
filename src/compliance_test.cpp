#include "firmware/compliance_test.hpp"

#include <algorithm>

namespace firmware::compliance {

bool ParseLoad(const std::string_view action, const std::string_view value,
               Load& load) {
    Load next = load;
    if (value.empty()) {
        next = {};
        if (action == "start" || action == "core") {}
        else if (action == "rail") next.rail = true;
        else if (action == "led") { next.rail = true; next.level = 5U; }
        else if (action == "ble") next.ble = BleLoad::Advertising;
        else if (action == "combined") {
            next.rail = true; next.level = 5U; next.animated = true;
            next.ble = BleLoad::Advertising;
        } else return false;
    } else if (action == "rail") {
        if (value != "on" && value != "off") return false;
        next.rail = value == "on";
    } else if (action == "level") {
        if (value.size() != 1U || value[0] < '0' || value[0] > '5') return false;
        next.level = static_cast<uint8_t>(value[0] - '0');
    } else if (action == "pattern") {
        if (value != "static" && value != "animated") return false;
        next.animated = value == "animated";
    } else if (action == "color") {
        if (value == "white") next.color = Color::White;
        else if (value == "red") next.color = Color::Red;
        else if (value == "green") next.color = Color::Green;
        else if (value == "blue") next.color = Color::Blue;
        else if (value == "rgb") next.color = Color::Rgb;
        else return false;
    } else if (action == "budget") {
        if (value == "detected") next.ceilingMa = 0U;
        else if (value == "500") next.ceilingMa = 500U;
        else if (value == "1500") next.ceilingMa = 1500U;
        else if (value == "3000") next.ceilingMa = 3000U;
        else return false;
    } else if (action == "ble") {
        if (value == "off") next.ble = BleLoad::Off;
        else if (value == "advertise") next.ble = BleLoad::Advertising;
        else if (value == "traffic") next.ble = BleLoad::Traffic;
        else return false;
    } else return false;
    load = next;
    return true;
}

uint16_t SourceCeilingMa(const hardware::UsbCcLevel detected) {
    switch (detected) {
        case hardware::UsbCcLevel::Ma1500: return 1500U;
        case hardware::UsbCcLevel::Ma3000: return 3000U;
        case hardware::UsbCcLevel::Ma500:
        case hardware::UsbCcLevel::Unknown: return 500U;
    }
    return 500U;
}

modules::PowerBudgetPolicyConfig LimitBudget(
    modules::PowerBudgetPolicyConfig runtime,
    const hardware::UsbCcLevel detected, const uint16_t requestedCeilingMa) {
    uint16_t ceiling = SourceCeilingMa(detected);
    if (requestedCeilingMa != 0U) ceiling = std::min(ceiling, requestedCeilingMa);
    runtime.maxTotalCurrentMa = std::min(runtime.maxTotalCurrentMa, ceiling);
    return runtime;
}

hardware::PhysicalLedFrame ComposeLoad(const Load& load,
                                       const hardware::BoardRevision revision,
                                       const uint32_t elapsedMs) {
    hardware::PhysicalLedFrame frame{};
    if (!load.rail || load.level == 0U) return frame;
    const auto topology = hardware::BoardLedTopologyFor(revision);
    const uint16_t count = topology.physicalBoardLedCount + topology.physicalStatusLedCount;
    const uint8_t value = static_cast<uint8_t>(load.level * 51U);
    for (uint16_t index = 0U; index < count; ++index) {
        Color color = load.color;
        if (color == Color::Rgb) color = static_cast<Color>(
            1U + ((index + (load.animated ? elapsedMs / 50U : 0U)) % 3U));
        auto pixel = color == Color::Red ? modules::Rgb{value, 0U, 0U}
            : color == Color::Green ? modules::Rgb{0U, value, 0U}
            : color == Color::Blue ? modules::Rgb{0U, 0U, value}
            : modules::Rgb{value, value, value};
        // Moving dark stripes retain a high load while changing the complete
        // physical frame at the ordinary production rendering cadence.
        if (load.animated && ((index + elapsedMs / 50U) % 8U) == 0U) pixel = {};
        frame[index] = pixel;
    }
    return frame;
}
} // namespace firmware::compliance
