#pragma once

#include "modules/settings_store.hpp"

namespace firmware {
// Physical factory-reset action only. Native peer cleanup resumes at boot;
// the separate factory NVS marker and native local IRK are never erased here.
bool ResetFactorySettings(modules::SettingsStore& settings, bool ownerPairing);
}
