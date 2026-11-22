#pragma once

#include <cstdint>
#include <string_view>

#include "hardware/board_led_output_mapping.hpp"
#include "hardware/usb_cc_sense.hpp"
#include "modules/power_budget_policy.hpp"

namespace firmware::compliance {

enum class BleLoad : uint8_t { Off, Advertising, Traffic };
enum class Color : uint8_t { White, Red, Green, Blue, Rgb };
struct Load {
    bool rail = false;
    uint8_t level = 0U; // Diagnostic 0..5, independent of ordinary UI brightness.
    bool animated = false;
    Color color = Color::White;
    uint16_t ceilingMa = 0U; // 0 = detected; other values only lower the ceiling.
    BleLoad ble = BleLoad::Off;
};

bool ParseLoad(std::string_view action, std::string_view value, Load& load);
hardware::PhysicalLedFrame ComposeLoad(const Load& load,
                                       hardware::BoardRevision revision,
                                       uint32_t elapsedMs);
modules::PowerBudgetPolicyConfig LimitBudget(
    modules::PowerBudgetPolicyConfig runtime,
    hardware::UsbCcLevel detected, uint16_t requestedCeilingMa);
uint16_t SourceCeilingMa(hardware::UsbCcLevel detected);

} // namespace firmware::compliance

#ifdef ARDUINO
namespace fxe { class Console; }
namespace firmware::compliance {
class Session final {
  public:
    bool Command(fxe::Console& console, std::string_view action,
                 std::string_view value);
    void Update();
    void Render();
    bool Stop();
    void PrintStatus(fxe::Console& console) const;
    bool IsActive() const { return m_active; }
    const Load& CurrentLoad() const { return m_load; }
    uint32_t StartedAtMs() const { return m_startedAtMs; }
  private:
    bool Begin(fxe::Console& console, const Load& load);
    bool Apply(const Load& load);
    Load m_load{};
    bool m_active = false;
    bool m_savedRail = true;
    bool m_savedAmbient = true;
    uint32_t m_startedAtMs = 0U;
    uint32_t m_lastTrafficMs = 0U;
    uint32_t m_trafficPublished = 0U;
    uint32_t m_trafficSkipped = 0U;
    uint32_t m_rxCount = 0U, m_txCount = 0U;
    uint32_t m_rxBaseline = 0U;
    uint32_t m_txBaseline = 0U;
};
} // namespace firmware::compliance
#endif
