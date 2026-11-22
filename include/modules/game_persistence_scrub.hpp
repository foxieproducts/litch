#pragma once

#include <ArduinoJson.h>

namespace fxe { class SettingsRemovedKeys; }

namespace modules {

struct GamePersistenceScrubReport {
    uint32_t runCount = 0;
    bool removedGameFen = false;
    bool removedUndoBaseFen = false;
};

// Returns a process-lifetime report of all scrubbing done so far.
// Intended for diagnostics via console commands.
const GamePersistenceScrubReport& GetLastGamePersistenceScrubReport();

// Resets the scrub report.
// This is primarily useful for tests or emulator workflows.
void ResetGamePersistenceScrubReport();

// Scrub/validate persisted game fields inside settings JSON.
//
// This is intended to be used as SettingsStoreConfig::scrubFunc so that if keys
// are removed due to corruption, SettingsStore's load-time recovery can merge
// missing values back from .tmp/.bak before anything is re-saved.
bool ScrubGamePersistenceSettings(JsonDocument& doc, fxe::SettingsRemovedKeys* removed = nullptr);

} // namespace modules
