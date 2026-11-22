#include "app.hpp"

#include "foxie_embedded/logger.hpp"

#include "app_buttons.hpp"
#include "app_console.hpp"
#include "app_logging.hpp"
#include "app_pins.hpp"

App::App(fxe::IConsoleTransport& transport)
    : m_console(transport), m_settings(MakeSettingsStoreConfig("/settings.json")) {}

void App::Begin() {
    SetupLogging();
    SetupSettings();
    SetupConsole();
    SetupButtons();

    fxe::Info("app: begin");
    m_console.PrintWelcome();

    m_lastSettingsRevision = m_settings.GetRevision();
}

void App::Update() {
    // Keep the console responsive.
    m_console.Update();

    // Flush deferred settings saves.
    m_settings.Update();

    // Input scan.
    m_primaryButton.Update();

    // If settings were modified (including via built-in console commands),
    // refresh the cached runtime struct.
    SyncRuntimeSettingsFromStoreIfNeeded();
}

void App::SetupLogging() {
    ConfigureLoggerToConsole(m_console);
}

void App::SetupSettings() {
    const bool loadedOk = m_settings.Load();
    if (!loadedOk) {
        fxe::Warn("settings: load failed (%s)", fxe::SettingsStoreErrorToString(m_settings.GetLastError()));
    }

    LoadRuntimeSettings(m_settings, m_runtimeSettings);
}

void App::SetupConsole() {
    m_console.SetSettingsStore(&m_settings);

    // Teach the built-in `set` command about project keys for tab completion.
    m_console.SetSettingsKeyEnumCallback([](const char* prefix, const char** outMatches, const size_t outCap) {
        return EnumerateSettingKeys(prefix, outMatches, outCap);
    });

    // Optional: decide that some keys should use the short must-save tier.
    m_console.SetSettingsWritePolicyCallback([](const std::string_view key, bool* outForceSaveNow) {
        return SettingsWritePolicy(key, outForceSaveNow);
    });

    RegisterAppConsoleCommands(*this, m_console);
}

void App::SetupButtons() {
    ConfigurePrimaryButton(m_primaryButton,
                           PIN_PRIMARY_BUTTON,
                           PRIMARY_BUTTON_PRESSED_STATE,
                           [this](const fxe::Button::Event evt) { OnButtonEvent(evt); });
}

void App::ApplyRuntimeSettings(const RuntimeSettings& settings) {
    m_runtimeSettings = settings;

    // Persist the new settings and schedule a deferred save.
    PersistRuntimeSettings(m_settings, m_runtimeSettings);

    fxe::Info("settings: applied (brightness=%u diagnostics=%u)",
              static_cast<unsigned>(m_runtimeSettings.brightness),
              m_runtimeSettings.diagnosticsEnabled ? 1U : 0U);
}

void App::OnButtonEvent(const fxe::Button::Event evt) {
    switch (evt) {
        case fxe::Button::Event::Press: {
            RuntimeSettings updated = m_runtimeSettings;
            updated.buttonPressCount++;

            // Example action: toggle diagnostics on each press.
            updated.diagnosticsEnabled = !updated.diagnosticsEnabled;
            ApplyRuntimeSettings(updated);

            m_console.PrintLn("button: press (count=%u diagnostics=%u)",
                              static_cast<unsigned>(updated.buttonPressCount),
                              updated.diagnosticsEnabled ? 1U : 0U);
            break;
        }
        case fxe::Button::Event::Release:
            break;
        case fxe::Button::Event::Repeat:
            break;
        case fxe::Button::Event::LongHold:
            m_console.PrintLn("button: long hold");
            break;
    }
}

void App::SyncRuntimeSettingsFromStoreIfNeeded() {
    const uint32_t revision = m_settings.GetRevision();
    if (revision == m_lastSettingsRevision) {
        return;
    }

    m_lastSettingsRevision = revision;
    LoadRuntimeSettings(m_settings, m_runtimeSettings);
    fxe::Info("settings: revision changed, runtime cache refreshed");
}
