#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ArduinoJson.h>

#include "modules/ble_control_protocol.hpp"

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

void TestCompactSettingFallback() {
    char compact[24] = {};
    size_t len = modules::FormatBleSettingCompactResponse(
        65535U, true, modules::ControlMutationReason::None, compact,
        sizeof(compact));
    Check(len == 17U &&
              std::strcmp(compact, "{\"y\":[65535,1,0]}") == 0,
          "success fallback preserves request ID within ATT MTU 23");

    len = modules::FormatBleSettingCompactResponse(
        65535U, false,
        modules::ControlMutationReason::StorageUnavailable, compact,
        sizeof(compact));
    Check(len == 18U &&
              std::strcmp(compact, "{\"y\":[65535,0,14]}") == 0,
          "failure fallback preserves stable reason within ATT MTU 23");
    Check(modules::BleSettingReasonFromErrorToken("response_too_large") ==
                  modules::ControlMutationReason::BadRequest &&
              modules::BleSettingReasonFromErrorToken(
                  "storage_unavailable") ==
                  modules::ControlMutationReason::StorageUnavailable,
          "compact fallback reason mapping stays stable");
}

void TestControlRequestParsing() {
    StaticJsonDocument<128> doc;
    modules::ParsedBleControlActionRequest parsed;

    doc.clear();
    doc["a"] = "undo";
    doc["n"] = 2U;
    Check(modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed) &&
              parsed.action == modules::ControlAction::Undo &&
              parsed.undoMoveCount == 2U &&
              parsed.owner == modules::UiOwner::None,
          "typed undo count survives parsing");

    doc.clear();
    doc["a"] = "coaching";
    doc["o"] = "p2";
    doc["e"] = false;
    Check(modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed) &&
              parsed.action == modules::ControlAction::Coaching &&
              parsed.owner == modules::UiOwner::P2 &&
              parsed.coachingMutation ==
                  modules::CoachingMutation::Disable,
          "coaching request preserves explicit owner and state");

    doc.clear();
    doc["a"] = "coaching";
    doc["e"] = "false";
    Check(!modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed),
          "wrong-type coaching state is rejected");

    doc.clear();
    doc["a"] = "undo";
    doc["n"] = "1";
    Check(!modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed),
          "wrong-type undo count is rejected");

    doc.clear();
    doc["a"] = "undo";
    doc["n"] = 0U;
    Check(!modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed),
          "zero undo count is rejected as bad request");

    doc.clear();
    doc["a"] = "undo";
    doc["o"] = "p3";
    Check(!modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed),
          "unknown explicit owner token is rejected");

    doc.clear();
    doc["a"] = "undo";
    doc["o"] = 1U;
    Check(!modules::TryParseBleControlActionRequest(
              doc.as<JsonVariantConst>(), parsed),
          "non-string explicit owner is rejected");
}

void TestConnectionControlActionDecoding() {
    modules::ParsedBleControlActionRequest parsed;
    const uint8_t undo[] = {1U, 2U, 0U, 0U, 3U};
    Check(modules::TryDecodeConnectionControlAction(
              undo, sizeof(undo), parsed) &&
              parsed.action == modules::ControlAction::Undo &&
              parsed.undoMoveCount == 3U,
          "connection action preserves typed undo count");

    const uint8_t coaching[] = {1U, 4U, 2U, 1U, 1U};
    Check(modules::TryDecodeConnectionControlAction(
              coaching, sizeof(coaching), parsed) &&
              parsed.action == modules::ControlAction::Coaching &&
              parsed.owner == modules::UiOwner::P2 &&
              parsed.coachingMutation == modules::CoachingMutation::Disable,
          "connection action preserves coaching owner and mutation");

    const uint8_t sleep[] = {1U, 6U, 0U, 0U, 1U};
    Check(modules::TryDecodeConnectionControlAction(sleep, sizeof(sleep), parsed) &&
              parsed.action == modules::ControlAction::Sleep,
          "typed Sleep uses six without colliding with storage reformat five");
    Check(modules::ParseBleControlActionToken("sleep") == modules::ControlAction::Unknown,
          "Sleep has no legacy JSON command fallback");
    const uint8_t malformed[][5] = {
        {1U, 6U, 1U, 0U, 1U},
        {1U, 6U, 0U, 1U, 1U},
        {1U, 6U, 0U, 0U, 2U},
        {2U, 1U, 0U, 0U, 1U},
        {1U, 0U, 0U, 0U, 1U},
        {1U, 1U, 1U, 0U, 1U},
        {1U, 2U, 0U, 0U, 0U},
        {1U, 4U, 0U, 0U, 1U},
        {1U, 4U, 1U, 3U, 1U},
    };
    for (const auto& body : malformed) {
        Check(!modules::TryDecodeConnectionControlAction(
                  body, sizeof(body), parsed),
              "non-canonical connection action is rejected");
    }
    Check(!modules::TryDecodeConnectionControlAction(
              undo, sizeof(undo) - 1U, parsed),
          "truncated connection action is rejected");
}

} // namespace

int main() {
    std::puts("[BLE_CONTROL_PROTOCOL] start");
    TestCompactSettingFallback();
    TestControlRequestParsing();
    TestConnectionControlActionDecoding();

    if (g_failures == 0) {
        std::puts("[BLE_CONTROL_PROTOCOL] ok");
        return 0;
    }
    std::printf("[BLE_CONTROL_PROTOCOL] failures=%d\n", g_failures);
    return 1;
}
