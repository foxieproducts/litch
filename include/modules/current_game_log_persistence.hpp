#pragma once

#include <ArduinoJson.h>

#include "modules/current_game_log.hpp"

namespace modules::current_game_log {

inline constexpr const char* KEY_CURRENT_GAME_LOG_V2 =
    "CURRENT_GAME_LOG_V2";
inline constexpr uint8_t COMMIT_POINTER_VERSION = 2U;

bool WriteCommitPointer(JsonDocument& document,
                        const CommitPointer& pointer);
bool ReadCommitPointer(const JsonDocument& document,
                       CommitPointer& out);

} // namespace modules::current_game_log
