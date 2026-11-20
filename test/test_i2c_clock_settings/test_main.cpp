#include <cstdio>
#include <string>
#include <string_view>

#include "foxie_embedded/millis.hpp"
#include "foxie_embedded/test_support/test_assert.hpp"
#include "hardware/i2c_clock_settings.hpp"

int I2cConsoleJourney();

namespace {

using fxe::test_support::TestResults;
namespace rates = hardware::i2c_clock_settings;
constexpr const char* PATH = "test_i2c_clock_settings.json";

void Cleanup() {
    (void)std::remove(PATH);
    (void)std::remove((std::string(PATH) + ".tmp").c_str());
    (void)std::remove((std::string(PATH) + ".bak").c_str());
}

void CheckSelectionAndReboot(TestResults& results) {
    Cleanup();
    fxe::SettingsStoreConfig config;
    config.path = PATH;
    config.allowSynchronousDeferredSaves = false;
    fxe::SettingsStore settings(config);
    results.Check(settings.Load(), "missing saved document starts normally");
    results.CheckEqU32(rates::Read(settings), 100000U,
                      "absent saved rate boots at 100 kHz");

    struct Selection {
        const char* token;
        uint32_t hz;
    };
    constexpr Selection SELECTIONS[] = {
        {"100k", 100000U}, {"400k", 400000U}, {"1m", 1000000U},
        {"100000", 100000U}, {"400000", 400000U}, {"1000000", 1000000U},
        {"100KHZ", 100000U}, {"400kHz", 400000U}, {"1M", 1000000U},
    };
    for (const auto& selection : SELECTIONS) {
        uint32_t runtimeHz = 0U;
        results.Check(rates::TrySelect(settings, selection.token, runtimeHz),
                      selection.token);
        results.CheckEqU32(runtimeHz, selection.hz,
                          "selected rate is available to the runtime owner");
        results.CheckEqU32(rates::Read(settings), selection.hz,
                          "selection updates the live settings value");
    }
    results.CheckEqU32(settings.GetSaveCount(), 0U,
                      "interactive selection performs no synchronous save");

    // Explicit blocking writes stand in for the private persistence worker or
    // lifecycle flush. The actual SettingsStore writes and reloads the file.
    for (const auto& selection : {SELECTIONS[1], SELECTIONS[2], SELECTIONS[0]}) {
        uint32_t runtimeHz = 0U;
        results.Check(rates::TrySelect(settings, selection.token, runtimeHz),
                      "choose a rate before a normal save/reboot boundary");
        results.Check(settings.SaveNowBlocking(), "real settings file saves");
        fxe::SettingsStore rebooted(config);
        results.Check(rebooted.Load(), "fresh settings owner reloads saved file");
        results.CheckEqU32(rates::Read(rebooted), selection.hz,
                          "production boot policy restores the saved rate");
    }

    settings.Doc()[rates::SETTING_KEY] = 400001U;
    settings.MarkDirty();
    results.Check(settings.SaveNowBlocking(), "invalid stored-rate fixture saves");
    fxe::SettingsStore invalidReboot(config);
    results.Check(invalidReboot.Load(), "invalid rate does not invalidate the document");
    results.CheckEqU32(rates::Read(invalidReboot), 100000U,
                      "invalid saved rate boots at 100 kHz");
    invalidReboot.Doc()[rates::SETTING_KEY] = "1000000";
    results.CheckEqU32(rates::Read(invalidReboot), 100000U,
                      "malformed saved type falls back to 100 kHz");
    Cleanup();
}

void CheckInvalidInputAndSaveFailure(TestResults& results) {
    Cleanup();
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    fxe::SettingsStoreConfig config;
    config.path = PATH;
    config.allowSynchronousDeferredSaves = false;
    fxe::SettingsStore settings(config);
    uint32_t runtimeHz = 0U;
    results.Check(rates::TrySelect(settings, "400k", runtimeHz),
                  "start with a valid runtime/saved rate");
    results.Check(settings.SaveNowBlocking(), "last verified rate is durable");
    const uint32_t savedRevision = settings.GetRevision();
    constexpr std::string_view INVALID[] = {
        "", "1mhz", "500000", "-100000", "1000000junk", "4294967296",
        std::string_view("1m\0junk", 7U),
    };
    for (const auto token : INVALID) {
        results.Check(!rates::TrySelect(settings, token, runtimeHz),
                      "invalid token is rejected before mutation");
        results.CheckEqU32(runtimeHz, 400000U,
                          "invalid input retains the running rate");
        results.CheckEqU32(rates::Read(settings), 400000U,
                          "invalid input retains the stored selection");
        results.CheckEqU32(settings.GetRevision(), savedRevision,
                          "invalid input adds no save intent");
    }

    uint32_t saveRequests = 0U;
    const uint32_t errorsBeforeRejectedSave = settings.GetErrorCount();
    settings.SetAsyncSaveAdmissionCallback([&saveRequests](const JsonDocument&) {
        ++saveRequests;
        return fxe::SettingsStore::AsyncSaveAdmission::Rejected;
    });
    results.Check(rates::TrySelect(settings, "1m", runtimeHz),
                  "valid command recovers after invalid input");
    settings.Update();
    results.CheckEqU32(saveRequests, 0U, "save waits for ordinary debounce");
    fxe::ManualMillis::SetNowMs(2000U);
    settings.Update();
    results.CheckEqU32(saveRequests, 1U, "ordinary save reaches async owner");
    results.Check(settings.GetLastError() == fxe::SettingsStoreError::AdmissionRejected &&
                      settings.GetErrorCount() == errorsBeforeRejectedSave + 1U &&
                      !settings.HasTerminalSaveFailure() && !settings.HasConfirmedStorageFailure() &&
                      !settings.IsDisabled(),
                  "rejected admission retains its error without inventing confirmed storage failure");

    // The production settings worker/recovery owner establishes this existing
    // session-only boundary. This suite exercises the setting after it does.
    settings.DisablePersistenceForSession();
    results.Check(rates::TrySelect(settings, "100k", runtimeHz),
                  "saving failure still permits runtime rate changes");
    results.CheckEqU32(runtimeHz, 100000U,
                      "session-only command publishes its requested runtime rate");
    results.CheckEqU32(rates::Read(settings), 100000U,
                      "session-only selection remains usable in RAM");
    results.Check(!settings.SaveNow(), "session-only state cannot claim a save");
    fxe::SettingsStore rebooted(config);
    results.Check(rebooted.Load(), "saving failure preserves last verified file");
    results.CheckEqU32(rates::Read(rebooted), 400000U,
                      "failed/disabled saving leaves the old durable rate intact");
    Cleanup();
}

} // namespace

int main() {
    TestResults results("I2C_CLOCK_SETTINGS");
    results.Start();
    CheckSelectionAndReboot(results);
    CheckInvalidInputAndSaveFailure(results);
    results.Check(I2cConsoleJourney() == 0, "production Console rate authority and recovery");
    return results.Finish();
}
