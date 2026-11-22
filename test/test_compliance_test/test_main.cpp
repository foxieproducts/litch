#include <cstdio>
#include "firmware/compliance_test.hpp"
#include "modules/led_render_policy.hpp"

namespace {
int failures = 0;
void Check(bool ok, const char* why) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); } }
}
int ComplianceProductionJourney();
int ComplianceGpioExitJourney();
int main() {
    using namespace firmware::compliance;
    Load load;
    Check(ParseLoad("combined", {}, load) && load.rail && load.level == 5U && load.ble == BleLoad::Advertising,
          "combined preset requests all governed loads");
    const auto before = load;
    for (const auto value : {"", "on", "off"})
        Check(!ParseLoad("espnow", value, load) && load.rail && load.level == before.level && load.ble == before.ble,
              "removed ESP-NOW command rejects without mutating the current load");
    for (const auto bad : {"6", "-1", "5x", "", "500"})
        Check(!ParseLoad("level", bad, load) && load.level == before.level,
              "invalid ingress retains the last valid diagnostic level");
    Check(ParseLoad("level", "0", load) && load.level == 0U, "zero is explicit off");
    auto off = ComposeLoad(load, hardware::BoardRevision::V3_3S3, 0U);
    for (auto p : off) Check(p.r == 0U && p.g == 0U && p.b == 0U, "level zero is dark");
    Check(ParseLoad("level", "5", load), "five remains independent of UI naming");
    load.animated = false;
    auto full = ComposeLoad(load, hardware::BoardRevision::V3_3S3, 0U);
    const auto topo = hardware::BoardLedTopologyFor(hardware::BoardRevision::V3_3S3);
    const uint16_t count = topo.physicalBoardLedCount + topo.physicalStatusLedCount;
    Check(count == 300U && full[count-1U].r == 255U && full[count-1U].g == 255U,
          "V3.3 white reaches all fitted square/status/control LEDs");
    load.animated = true;
    auto first = ComposeLoad(load, hardware::BoardRevision::V3S3, 0U);
    auto second = ComposeLoad(load, hardware::BoardRevision::V3S3, 50U);
    Check(first[0U].r == 0U && second[0U].r != 0U, "animation changes real physical pixels at regular time");
    for (const auto detected : {hardware::UsbCcLevel::Unknown, hardware::UsbCcLevel::Ma500,
                               hardware::UsbCcLevel::Ma1500, hardware::UsbCcLevel::Ma3000}) {
        for (const uint16_t requested : {0U, 500U, 1500U, 3000U}) {
            const auto normal = modules::led_render_policy::MakePowerBudgetPolicyConfig({true, false});
            const auto cap = LimitBudget(normal, detected, requested);
            modules::PowerBudgetPolicy policy(cap); policy.SetEstimatedLedCurrentMa(20000U);
            const auto decision = policy.GetDecision();
            Check(cap.maxTotalCurrentMa <= normal.maxTotalCurrentMa &&
                  cap.maxTotalCurrentMa <= SourceCeilingMa(detected) &&
                  (requested == 0U || cap.maxTotalCurrentMa <= requested) &&
                  decision.allowedLedCurrentMa <= 1000U &&
                  decision.allowedLedCurrentMa + cap.fixedSystemCurrentMa <= cap.maxTotalCurrentMa,
                  "CC/request ceiling only lowers production caps and retains system headroom");
            const auto conservative = LimitBudget(
                modules::led_render_policy::MakePowerBudgetPolicyConfig({true, true}), detected, requested);
            Check(conservative.maxTotalCurrentMa <= 450U, "recovery protection cannot be overridden by diagnostics");
        }
    }
    failures += ComplianceProductionJourney();
    failures += ComplianceGpioExitJourney();
    std::printf("[COMPLIANCE_TEST] failures=%d\n", failures);
    return failures ? 1 : 0;
}
