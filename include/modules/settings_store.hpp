#pragma once

#include "foxie_embedded/settings_store.hpp"

namespace modules {

using SettingsStore = fxe::SettingsStore;
using SettingsStoreConfig = fxe::SettingsStoreConfig;
using SettingsStoreError = fxe::SettingsStoreError;

inline const char* SettingsStoreErrorToString(const SettingsStoreError error) {
    return fxe::SettingsStoreErrorToString(error);
}

} // namespace modules
