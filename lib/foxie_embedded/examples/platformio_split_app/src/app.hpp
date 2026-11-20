#pragma once

#include <cstdint>

#include "foxie_embedded/button.hpp"
#include "foxie_embedded/console.hpp"
#include "foxie_embedded/settings_store.hpp"

#include "app_settings.hpp"

namespace fxe {
class IConsoleTransport;
}

class App final {
  public:
    explicit App(fxe::IConsoleTransport& transport);

    void Begin();
    void Update();

    fxe::Console& GetConsole() { return m_console; }
    fxe::SettingsStore& GetSettings() { return m_settings; }

    const RuntimeSettings& GetRuntimeSettings() const { return m_runtimeSettings; }

    void ApplyRuntimeSettings(const RuntimeSettings& settings);

    void OnButtonEvent(fxe::Button::Event evt);

  private:
    void SetupLogging();
    void SetupSettings();
    void SetupConsole();
    void SetupButtons();

    void SyncRuntimeSettingsFromStoreIfNeeded();

    fxe::Console m_console;
    fxe::SettingsStore m_settings;

    RuntimeSettings m_runtimeSettings;
    uint32_t m_lastSettingsRevision = 0;

    fxe::Button m_primaryButton;
};
