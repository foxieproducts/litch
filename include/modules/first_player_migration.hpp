#pragma once

#include <cstdint>

#include <ArduinoJson.h>

namespace modules::first_player_migration {

[[nodiscard]] uint32_t ReadFirstPlayer1To2FromDoc(const JsonDocument& doc);

[[nodiscard]] bool ReadFirstPlayerIsP2FromDoc(const JsonDocument& doc);

[[nodiscard]] bool CanonicalizeAndCleanup(JsonDocument& doc);

} // namespace modules::first_player_migration
