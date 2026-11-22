#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/setting_descriptor.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"

namespace {

const fxe::SettingDescriptor* LookupDescriptor(const std::string_view key) {
    static constexpr fxe::SettingDescriptor BRIGHTNESS_DESCRIPTOR(
        "BRIGHTNESS",
        fxe::SettingType::UInt32,
        fxe::SettingCategory::Display,
        5U,
        nullptr,
        1U,
        8U,
        nullptr,
        0U,
        "Brightness",
        static_cast<uint32_t>(fxe::VISIBLE_TO_APP | fxe::PERSISTED));

    static constexpr fxe::SettingDescriptor BOARD_ROTATE_DESCRIPTOR(
        "BOARD_ROTATE",
        fxe::SettingType::Bool,
        fxe::SettingCategory::Behavior,
        0U,
        nullptr,
        0U,
        1U,
        nullptr,
        0U,
        "Rotate board",
        static_cast<uint32_t>(fxe::VISIBLE_TO_APP | fxe::PERSISTED));

    if (key == "BRIGHTNESS") {
        return &BRIGHTNESS_DESCRIPTOR;
    }

    if (key == "BOARD_ROTATE") {
        return &BOARD_ROTATE_DESCRIPTOR;
    }

    return nullptr;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CONSOLE_SETTINGS_SAVE_SEMANTICS");
    results.Start();
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    fxe::SettingsStoreConfig cfg;
    cfg.path = "build/test_console_settings_save_semantics.json";
    cfg.deferredSaveDelayMs = 100000;
    cfg.mustSaveDelayMs = 100U;

    (void)std::remove(cfg.path);

    fxe::SettingsStore settings(cfg);
    results.Check(settings.Load(), "settings load");

    fxe::MemoryConsoleTransport transport;
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    fxe::Console console(transport);
    console.SetSettingsStore(&settings);
    console.SetSettingsDescriptorLookupCallback(&LookupDescriptor);

    std::vector<std::string> mutationKeys;
    std::vector<uint32_t> persistenceSaveCounts;
    console.SetSettingsMutationCallback([&](const std::string_view key) {
        mutationKeys.emplace_back(key);
        settings.ArmPersistenceGuard();
    });
    settings.SetBeforePersistenceCallback([&]() {
        persistenceSaveCounts.push_back(settings.GetSaveCount());
    });

    console.SetSettingsWritePolicyCallback([](const std::string_view key, bool* outForceSaveNow) -> bool {
        if (!outForceSaveNow) {
            return false;
        }

        if (key == "FORCE_SAVE" || key == "force_save") {
            *outForceSaveNow = true;
            return true;
        }

        return false;
    });

    const uint32_t saves0 = settings.GetSaveCount();

    // Force-save policy keys use the short debounce tier; they never write in
    // the mutation call itself.
    fxe::test_support::RunCommand(console, transport, "settings set FORCE_SAVE 1");
    results.Check(settings.GetSaveCount() == saves0,
                  "FORCE_SAVE does not save in the console mutation");
    fxe::ManualMillis::SetNowMs(99U);
    settings.Update();
    results.Check(settings.GetSaveCount() == saves0,
                  "FORCE_SAVE waits through 99 ms");
    fxe::ManualMillis::SetNowMs(100U);
    settings.Update();
    results.Check(settings.GetSaveCount() == saves0 + 1,
                  "FORCE_SAVE persists at the 100 ms deadline");
    results.Check(mutationKeys.size() == 1 &&
                      mutationKeys[0] == "FORCE_SAVE",
                  "short-tier save announces key mutation");
    results.Check(persistenceSaveCounts.size() == 1 &&
                      persistenceSaveCounts[0] == saves0,
                  "persistence callback runs before short-tier save");

    // Mixed-case/lowercase setting names are accepted and canonicalized to uppercase.
    fxe::test_support::RunCommand(console, transport, "settings set brightness 9");
    results.Check(settings.Doc().containsKey("BRIGHTNESS"), "settings set canonicalizes key to uppercase");
    results.Check(!settings.Doc().containsKey("brightness"), "lowercase key is not stored");

    fxe::test_support::RunCommand(console, transport, "set feature_flag 1");
    results.Check(settings.Doc().containsKey("FEATURE_FLAG"), "set canonicalizes key to uppercase");

    const int32_t brightness = settings.ReadValue("brightness", static_cast<int32_t>(0));
    results.Check(brightness == 9, "ReadValue remains case-insensitive for keys");

    output.Clear();
    fxe::test_support::RunCommand(console, transport, "set BRIGHTNESS");
    results.Check(settings.Doc().containsKey("BRIGHTNESS"), "set key-query preserves canonical key");
    results.Check(output.Str().find("BRIGHTNESS: uint32, range 1-8, default 5, current 9") != std::string::npos,
                  "set key-query prints descriptor metadata and current value");

    // Bool descriptor summaries should print textual bools for default/current values.
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "set BOARD_ROTATE true");
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "set BOARD_ROTATE");
    results.Check(output.Str().find("BOARD_ROTATE: bool, range true/false, default false, current true") != std::string::npos,
                  "set key-query prints bool default/current as false/true");

    // Legacy numeric values should still display as textual bool in summaries and settings get.
    settings.Doc()["BOARD_ROTATE"] = 1U;
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "set BOARD_ROTATE");
    results.Check(output.Str().find("current true") != std::string::npos,
                  "set key-query maps legacy numeric bool current value to true/false");

    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings get BOARD_ROTATE");
    results.Check(output.Str().find("true") != std::string::npos,
                  "settings get prints textual bool for bool descriptors");

    (void)settings.Doc().remove("BRIGHTNESS");
    settings.Doc()["brightness"] = 5U;
    size_t serviceNoopCalls = 0U;
    console.SetSettingsValidateAndSetCallback(
        [&](fxe::SettingsStore&, const std::string_view, const JsonVariantConst) {
            ++serviceNoopCalls;
            return fxe::Console::SettingsSetResult::Noop;
        });
    const uint32_t revisionBeforeAliasNoops = settings.GetRevision();
    fxe::test_support::RunCommand(console, transport, "set BRIGHTNESS 5");
    results.Check(settings.Doc().containsKey("brightness") &&
                      !settings.Doc().containsKey("BRIGHTNESS"),
                  "set service noop preserves an existing case alias");
    results.Check(settings.GetRevision() == revisionBeforeAliasNoops,
                  "set service noop performs no direct write");

    fxe::test_support::RunCommand(console, transport, "settings set BRIGHTNESS 5");
    results.Check(settings.Doc().containsKey("brightness") &&
                      !settings.Doc().containsKey("BRIGHTNESS"),
                  "settings set service noop preserves an existing case alias");
    results.Check(settings.GetRevision() == revisionBeforeAliasNoops,
                  "settings set service noop performs no direct write");
    results.Check(serviceNoopCalls == 2U,
                  "both setting builtins delegate noop decisions to the service");
    console.SetSettingsValidateAndSetCallback(nullptr);

    // Corrupt + erase are used for emulator/native failure simulation.
    mutationKeys.clear();
    persistenceSaveCounts.clear();
    fxe::test_support::RunCommand(console, transport, "settings corrupt");
    results.Check(mutationKeys.size() == 1 && mutationKeys[0].empty(),
                  "corrupt announces whole-store mutation");
    results.Check(persistenceSaveCounts.size() == 1,
                  "corrupt announces persistence operation");
    results.Check(!settings.Load(), "corrupt causes load failure");
    results.Check(persistenceSaveCounts.size() == 2,
                  "pending dirty save keeps guard armed across load");

    mutationKeys.clear();
    persistenceSaveCounts.clear();
    fxe::test_support::RunCommand(console, transport, "settings erase");
    results.Check(mutationKeys.size() == 1 && mutationKeys[0].empty(),
                  "erase announces whole-store mutation");
    results.Check(persistenceSaveCounts.size() == 1,
                  "successful erase clears guard before in-command reload");
    results.Check(settings.Load(), "erase removes file; load ok");

    // Alias: clear == erase.
    fxe::test_support::RunCommand(console, transport, "settings corrupt");
    results.Check(!settings.Load(), "corrupt causes load failure (2)");

    fxe::test_support::RunCommand(console, transport, "settings clear");
    results.Check(settings.Load(), "clear removes file; load ok");

    // Status should always succeed (smoke).
    fxe::test_support::RunCommand(console, transport, "settings status");

    return results.Finish();
}
