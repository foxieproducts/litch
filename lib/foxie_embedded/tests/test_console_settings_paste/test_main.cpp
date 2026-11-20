#include <cstdio>
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

void FinishPaste(fxe::Console& console, fxe::MemoryConsoleTransport& transport) {
    transport.PushChar(0x04);
    console.Update();
}

const fxe::SettingDescriptor* LookupRegisteredDescriptor(
    const std::string_view key) {
    static constexpr fxe::SettingDescriptor REGISTERED_DESCRIPTOR(
        "REGISTERED",
        fxe::SettingType::UInt32,
        fxe::SettingCategory::Behavior,
        7U,
        nullptr,
        0U,
        100U,
        nullptr,
        0U,
        "Registered test setting",
        static_cast<uint32_t>(fxe::PERSISTED));
    return key == "REGISTERED" ? &REGISTERED_DESCRIPTOR : nullptr;
}

} // namespace

int main() {
    fxe::test_support::TestResults results("CONSOLE_SETTINGS_PASTE");
    results.Start();
    fxe::ScopedMillisProviderOverride millisGuard(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);

    fxe::SettingsStoreConfig cfg;
    cfg.path = "build/test_console_settings_paste.json";
    cfg.deferredSaveDelayMs = 2000U;

    (void)std::remove(cfg.path);
    (void)std::remove("build/test_console_settings_paste.json.bak");
    (void)std::remove("build/test_console_settings_paste.json.tmp");

    fxe::SettingsStore settings(cfg);
    results.Check(settings.Load(), "settings load");
    settings.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;

    fxe::MemoryConsoleTransport transport;
    fxe::test_support::ConsoleOutputCapture output;
    output.Attach(transport);
    fxe::Console console(transport);
    console.SetSettingsStore(&settings);
    uint32_t rebootRequests = 0U;
    results.Check(
        console.RegisterCommand(
            "reboot",
            "test reboot",
            [&rebootRequests](const fxe::Console::Args&) {
                ++rebootRequests;
                return true;
            }),
        "register test reboot command");

    std::vector<std::string> mutationKeys;
    std::vector<uint32_t> persistenceSaveCounts;
    console.SetSettingsMutationCallback([&](const std::string_view key) {
        mutationKeys.emplace_back(key);
        settings.ArmPersistenceGuard();
    });
    settings.SetBeforePersistenceCallback([&]() {
        persistenceSaveCounts.push_back(settings.GetSaveCount());
    });

    const uint32_t savesBeforeLargePaste = settings.GetSaveCount();
    fxe::test_support::RunCommand(console, transport, "settings paste");

    const std::string longComment = "#" + std::string(238, 'x') + "\n";
    size_t aggregateBytes = 0;
    for (size_t i = 0; i < 40; ++i) {
        fxe::test_support::FeedConsole(console, transport, longComment.c_str());
        aggregateBytes += longComment.size();
    }
    results.Check(aggregateBytes > 8192, "aggregate paste exceeds former 8 KiB limit");

    fxe::test_support::FeedConsole(console, transport, "plain_key=123\n");
    fxe::test_support::FeedConsole(console, transport, "set quoted = \"hello world\"\n");
    fxe::test_support::FeedConsole(console, transport, "settings set flag=true\n");
    fxe::test_support::FeedConsole(console, transport, "led_brownout_limit_latch=\n");
    FinishPaste(console, transport);

    results.Check(settings.ReadValue("PLAIN_KEY", static_cast<int32_t>(0)) == 123,
                  "large paste applies JSON number");
    results.Check(std::string(settings.ReadValue("QUOTED", "")) == "hello world",
                  "large paste preserves quoted string semantics");
    results.Check(settings.ReadValue("FLAG", false), "large paste applies settings-set prefix");
    results.Check(!settings.Doc().containsKey("LED_BROWNOUT_LIMIT_LATCH"),
                  "large paste removes empty-valued internal key");
    results.Check(!settings.Doc().containsKey("plain_key"), "large paste canonicalizes keys");
    results.Check(settings.GetSaveCount() == savesBeforeLargePaste,
                  "large paste does not save in the mutation path");
    fxe::ManualMillis::SetNowMs(1999U);
    settings.Update();
    console.Update();
    results.Check(settings.GetSaveCount() == savesBeforeLargePaste,
                  "large paste waits through 1999 ms");
    results.Check(rebootRequests == 0U,
                  "large paste does not reboot before persistence deadline");
    fxe::ManualMillis::SetNowMs(2000U);
    settings.Update();
    console.Update();
    results.Check(settings.GetSaveCount() == savesBeforeLargePaste + 1,
                  "large paste saves once at the 2 second deadline");
    results.Check(rebootRequests == 1U,
                  "large paste requests reboot after the save deadline");
    results.Check(mutationKeys.size() == 5 &&
                      mutationKeys[0] == "PLAIN_KEY" &&
                      mutationKeys[1] == "QUOTED" &&
                      mutationKeys[2] == "FLAG" &&
                      mutationKeys[3] == "LED_BROWNOUT_LIMIT_LATCH" &&
                      mutationKeys[4].empty(),
                  "paste announces mutations and bulk persistence");
    results.Check(persistenceSaveCounts.size() == 1 &&
                      persistenceSaveCounts[0] == savesBeforeLargePaste,
                  "paste persistence callback runs before save");
    results.Check(output.Str().find("paste: applied=4 failed=0") != std::string::npos,
                  "large paste reports applied lines");

    output.Clear();
    const uint32_t savesBeforeOverflowPaste = settings.GetSaveCount();
    fxe::ManualMillis::SetNowMs(3000U);
    fxe::test_support::RunCommand(console, transport, "settings paste");

    const std::string overlongLine(fxe::Console::INPUT_CAPACITY, 'z');
    fxe::test_support::FeedConsole(console, transport, overlongLine.c_str());
    fxe::test_support::FeedConsole(console, transport, "\n");
    fxe::test_support::FeedConsole(console, transport, "BACKSPACE=18\b7\n");
    fxe::test_support::FeedConsole(console, transport, "RECOVERED=7\x1b[A");
    FinishPaste(console, transport);

    results.Check(settings.ReadValue("BACKSPACE", static_cast<int32_t>(0)) == 17,
                  "backspace edits the active paste line");
    results.Check(settings.ReadValue("RECOVERED", static_cast<int32_t>(0)) == 7,
                  "Ctrl+D submits final line and escape sequence is ignored");
    results.Check(settings.GetSaveCount() == savesBeforeOverflowPaste,
                  "overflow recovery paste queues its save");
    fxe::ManualMillis::SetNowMs(4999U);
    settings.Update();
    console.Update();
    results.Check(settings.GetSaveCount() == savesBeforeOverflowPaste,
                  "overflow recovery paste waits through 1999 ms");
    fxe::ManualMillis::SetNowMs(5000U);
    settings.Update();
    console.Update();
    results.Check(settings.GetSaveCount() == savesBeforeOverflowPaste + 1,
                  "overflow recovery paste saves once at its deadline");
    results.Check(rebootRequests == 2U,
                  "overflow recovery paste requests one deferred reboot");
    results.Check(output.Str().find("paste: applied=2 failed=1") != std::string::npos,
                  "overlong line fails without losing later lines");

    // A bulk edit containing a must-save key uses the short tier and short
    // reboot delay for the whole snapshot.
    console.SetSettingsWritePolicyCallback(
        [](const std::string_view key, bool* outMustSave) {
            if (outMustSave == nullptr || key != "PRIORITY") {
                return false;
            }
            *outMustSave = true;
            return true;
        });
    const uint32_t savesBeforePriorityPaste = settings.GetSaveCount();
    fxe::ManualMillis::SetNowMs(6000U);
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "PRIORITY=1\n");
    FinishPaste(console, transport);
    fxe::ManualMillis::SetNowMs(6099U);
    settings.Update();
    console.Update();
    results.Check(settings.GetSaveCount() == savesBeforePriorityPaste,
                  "priority paste waits through 99 ms");
    results.Check(rebootRequests == 2U,
                  "priority paste does not reboot before 100 ms");
    fxe::ManualMillis::SetNowMs(6100U);
    settings.Update();
    console.Update();
    results.Check(settings.GetSaveCount() == savesBeforePriorityPaste + 1U,
                  "priority paste saves at 100 ms");
    results.Check(rebootRequests == 3U,
                  "priority paste reboots after the short deadline");

    fxe::test_support::RunCommand(console, transport, "settings set AFTER_PASTE 9");
    results.Check(settings.ReadValue("AFTER_PASTE", static_cast<int32_t>(0)) == 9,
                  "console returns to command mode after paste");

    console.SetSettingsDescriptorLookupCallback(&LookupRegisteredDescriptor);
    fxe::Console::SettingsSetResult resetResult =
        fxe::Console::SettingsSetResult::Applied;
    std::vector<std::string> resetKeys;
    console.SetSettingsResetCallback(
        [&](fxe::SettingsStore& target, const std::string_view key) {
            resetKeys.emplace_back(key);
            if (resetResult == fxe::Console::SettingsSetResult::Applied) {
                (void)target.Doc().remove("REGISTERED");
                target.MarkDirty();
            }
            return resetResult;
        });

    settings.Doc()["REGISTERED"] = 12U;
    const uint32_t revisionBeforeAppliedReset = settings.GetRevision();
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "registered=\n");
    FinishPaste(console, transport);
    results.Check(resetKeys.size() == 1U && resetKeys[0] == "REGISTERED",
                  "registered empty value dispatches the reset callback");
    results.Check(!settings.Doc().containsKey("REGISTERED"),
                  "applied reset leaves document mutation to the callback");
    results.Check(settings.GetRevision() == revisionBeforeAppliedReset + 1U,
                  "applied reset receives exactly the callback revision");
    results.Check(output.Str().find("paste: applied=1 failed=0") != std::string::npos,
                  "applied reset is counted as an applied paste line");

    settings.Doc()["REGISTERED"] = 12U;
    resetResult = fxe::Console::SettingsSetResult::Noop;
    const uint32_t revisionBeforeNoopReset = settings.GetRevision();
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "REGISTERED=\n");
    FinishPaste(console, transport);
    results.Check(settings.Doc()["REGISTERED"].as<uint32_t>() == 12U,
                  "noop reset does not edit the settings document");
    results.Check(settings.GetRevision() == revisionBeforeNoopReset,
                  "noop reset does not bump the settings revision");
    results.Check(output.Str().find("paste: applied=1 failed=0") != std::string::npos,
                  "noop reset is accepted without a raw write");

    resetResult = fxe::Console::SettingsSetResult::StorageUnavailable;
    const uint32_t revisionBeforeRejectedReset = settings.GetRevision();
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "REGISTERED=\n");
    FinishPaste(console, transport);
    results.Check(settings.Doc()["REGISTERED"].as<uint32_t>() == 12U,
                  "rejected reset never falls back to raw removal");
    results.Check(settings.GetRevision() == revisionBeforeRejectedReset,
                  "rejected reset does not bump the settings revision");
    results.Check(output.Str().find("paste: applied=0 failed=1") != std::string::npos,
                  "rejected reset is reported as a failed paste line");

    console.SetSettingsResetCallback(nullptr);
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "REGISTERED=\n");
    FinishPaste(console, transport);
    results.Check(settings.Doc()["REGISTERED"].as<uint32_t>() == 12U,
                  "unavailable reset callback never falls back to raw removal");
    results.Check(output.Str().find("paste: applied=0 failed=1") != std::string::npos,
                  "unavailable reset callback fails explicitly");

    mutationKeys.clear();
    settings.Doc()["legacy_raw"] = 3U;
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "LEGACY_RAW=\n");
    FinishPaste(console, transport);
    results.Check(!settings.Doc().containsKey("legacy_raw"),
                  "unregistered existing raw key can still be removed");
    results.Check(mutationKeys.size() == 2U &&
                      mutationKeys[0] == "LEGACY_RAW" &&
                      mutationKeys[1].empty(),
                  "raw removal announces the key and batch persistence");
    results.Check(output.Str().find("paste: applied=1 failed=0") != std::string::npos,
                  "existing raw-key removal is counted as applied");

    mutationKeys.clear();
    const uint32_t revisionBeforeUnknownReset = settings.GetRevision();
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "UNKNOWN_ABSENT=\n");
    FinishPaste(console, transport);
    results.Check(settings.GetRevision() == revisionBeforeUnknownReset,
                  "unknown absent empty value does not mutate settings");
    results.Check(mutationKeys.empty(),
                  "unknown absent empty value does not announce a mutation");
    results.Check(output.Str().find("paste: applied=0 failed=1") != std::string::npos,
                  "unknown absent empty value fails explicitly");

    settings.Doc()["registered"] = 7U;
    (void)settings.Doc().remove("REGISTERED");
    console.SetSettingsValidateAndSetCallback(
        [](fxe::SettingsStore&, const std::string_view, const JsonVariantConst) {
            return fxe::Console::SettingsSetResult::Noop;
        });
    const uint32_t revisionBeforeAliasNoop = settings.GetRevision();
    output.Clear();
    fxe::test_support::RunCommand(console, transport, "settings paste");
    fxe::test_support::FeedConsole(console, transport, "REGISTERED=7\n");
    FinishPaste(console, transport);
    results.Check(settings.Doc().containsKey("registered") &&
                      !settings.Doc().containsKey("REGISTERED"),
                  "service noop paste preserves an existing case alias");
    results.Check(settings.GetRevision() == revisionBeforeAliasNoop,
                  "service noop paste performs no direct write");

    return results.Finish();
}
