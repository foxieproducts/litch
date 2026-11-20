#pragma once

#include <ArduinoJson.h>

namespace modules {
// A destructive storage reset keeps only readable hardware identity,
// calibration and protection. Ordinary preferences/game/artwork reset to their
// existing defaults at the next boot. The live document is never mutated.
bool BuildBoardStorageRecoverySnapshot(const JsonDocument& live,
                                       JsonDocument& safe);
} // namespace modules
