#include <cstdio>
#include <string>
#include <string_view>

#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/settings_store.hpp"
#include "modules/hardware_test_record.hpp"
#include "modules/settings_registry.hpp"

namespace {
using Source = fxe::ConsoleInputSource;
using Result = fxe::Console::SettingsSetResult;
int failures = 0;
void Check(bool accepted, const char* message) {
    if (!accepted) { ++failures; std::printf("[FAIL] %s\n", message); }
}

// Native transport batches retain their real source at the parser boundary.
class SourcedMemoryTransport final : public fxe::IConsoleTransport {
  public:
    bool TryReadChar(char& value) override { return input.TryReadChar(value); }
    Source LastInputSource() const override { return source; }
    uint64_t RemoteInputGeneration() const override { return generation; }
    void Write(const char* data, size_t size) override { output.append(data, size); }
    void Flush() override {}
    fxe::MemoryConsoleTransport input;
    Source source = Source::Unknown;
    uint64_t generation = 0U;
    std::string output;
};

struct Harness {
    SourcedMemoryTransport transport;
    fxe::Console console{transport};
    fxe::SettingsStore settings;
    unsigned validations = 0U;
    unsigned resets = 0U;
    Source observedSource = Source::Unknown;
    uint64_t observedGeneration = 0U;
    Harness() {
        settings.Doc()[modules::HARDWARE_TEST_RECORD_SETTING_KEY] = "original";
        console.SetSettingsStore(&settings);
        console.SetSettingsDescriptorLookupCallback([](std::string_view key) {
            const std::string text(key);
            return modules::settings_registry::LookupByKey(text.c_str());
        });
        console.SetSettingsValidateAndSetCallback(
            [this](fxe::SettingsStore& store, std::string_view key, JsonVariantConst value) {
                observedSource = console.CommandInputSource();
                observedGeneration = console.CommandRemoteInputGeneration();
                ++validations;
                const std::string text(key);
                const auto result = modules::settings_registry::ValidateAndSet(
                    store, text.c_str(), value);
                if (result == modules::settings_registry::ValidateAndSetResult::Ok) {
                    return Result::Applied;
                }
                return result == modules::settings_registry::ValidateAndSetResult::UnknownKey
                    ? Result::UnknownKey : Result::TypeMismatch;
            });
        console.SetSettingsResetCallback(
            [this](fxe::SettingsStore& store, std::string_view key) {
                ++resets;
                const std::string text(key);
                store.Doc().remove(text.c_str());
                return Result::Applied;
            });
    }
    void Feed(Source source, std::string_view text) {
        transport.source = source;
        transport.input.PushBytes(text.data(), text.size());
        console.Update();
    }
    bool Original() const {
        return std::string_view(settings.ReadValue(
                   modules::HARDWARE_TEST_RECORD_SETTING_KEY, "missing")) == "original";
    }
};

void TypedCommands() {
    for (const Source source : {Source::Remote, Source::Unknown}) {
        for (const char* command : {
                 "set HARDWARE_TEST_RECORD changed\n",
                 "settings set HARDWARE_TEST_RECORD changed\n",
                 "set hardware_test_record changed\n"}) {
            Harness h;
            h.Feed(source, command);
            Check(!h.Original() && h.validations == 1U,
                  "remote/unknown typed writes reach canonical factory validation");
            Check(h.transport.output.find("requires a fresh USB command or paste") ==
                      std::string::npos,
                  "accepted Console commands do not request fresh USB input");
        }
    }
    Harness mixed;
    mixed.Feed(Source::Remote, "set HARDWARE_TEST_");
    mixed.Feed(Source::Local, "RECORD changed\n");
    Check(!mixed.Original() && mixed.validations == 1U,
          "mixed typed command retains canonical validation");
    Harness recalled;
    recalled.Feed(Source::Local, "set HARDWARE_TEST_RECORD changed\n");
    Check(!recalled.Original() && recalled.validations == 1U,
          "fresh complete USB typed write retains canonical validation");
    recalled.settings.Doc()[modules::HARDWARE_TEST_RECORD_SETTING_KEY] = "original";
    recalled.Feed(Source::Local, "\x1b[A\n");
    Check(!recalled.Original() && recalled.validations == 2U,
          "USB history recall repeats the validated factory write");
    Harness invalid;
    invalid.Feed(Source::Local, "settings set HARDWARE_TEST_RECORD 12\n");
    Check(invalid.Original() && invalid.validations == 1U,
          "USB admission does not bypass registered String validation");
}

void PasteCommands() {
    for (const Source begin : {Source::Local, Source::Remote, Source::Unknown}) {
        for (const Source payload : {Source::Local, Source::Remote, Source::Unknown}) {
            for (const char* line : {"HARDWARE_TEST_RECORD=changed\n",
                                     "hardware_test_record=\n",
                                     "set HARDWARE_TEST_RECORD changed\n",
                                     "settings set HARDWARE_TEST_RECORD changed\n",
                                     "set HARDWARE_TEST_RECORD\n",
                                     "settings set HARDWARE_TEST_RECORD\n"}) {
                Harness h;
                h.Feed(begin, "settings paste\n");
                h.Feed(payload, line);
                h.Feed(payload, "\x04");
                Check(!h.Original(),
                      "factory paste write/reset accepts every Console input source");
                Check(h.validations + h.resets == 1U,
                      "each paste line reaches its existing mutation owner once");
                Check(h.console.CommandInputSource() == Source::Unknown,
                      "paste callback provenance cannot leak into later commands");
            }
        }
    }
    Harness mixed;
    mixed.Feed(Source::Local, "settings paste\nHARDWARE_TEST_");
    mixed.Feed(Source::Remote, "RECORD=");
    mixed.Feed(Source::Local, "\n\x04");
    Check(!mixed.Original() && mixed.resets == 1U,
          "mixed-source paste uses the existing reset owner");
    Harness remoteFinish;
    remoteFinish.Feed(Source::Local,
        "settings paste\nHARDWARE_TEST_RECORD=changed");
    remoteFinish.Feed(Source::Remote, "\x04");
    Check(!remoteFinish.Original() && remoteFinish.validations == 1U,
          "remote paste termination completes a valid partial line");
    Harness recalled;
    recalled.Feed(Source::Local, "settings paste\n\x04");
    recalled.Feed(Source::Local, "\x1b[A\n");
    recalled.Feed(Source::Local, "HARDWARE_TEST_RECORD=changed\n\x04");
    Check(!recalled.Original() && recalled.validations == 1U,
          "history-recalled paste begin accepts valid factory data");
    Harness batch;
    batch.Feed(Source::Local, "settings paste\n");
    batch.Feed(Source::Remote, "BRIGHTNESS=3\nHARDWARE_TEST_RECORD=changed\n");
    batch.Feed(Source::Local, "HARDWARE_TEST_RECORD=\n");
    batch.Feed(Source::Remote, "BRIGHTNESS=4\n\x04");
    Check(!batch.settings.Doc().containsKey(modules::HARDWARE_TEST_RECORD_SETTING_KEY) &&
              batch.settings.ReadValue("BRIGHTNESS", 0U) == 4U &&
              batch.validations == 3U && batch.resets == 1U,
          "ordinary and factory paste lines apply through the same mutation owners");
}

void AllowedReadsAndSettings() {
    Harness h;
    h.Feed(Source::Remote, "set HARDWARE_TEST_RECORD\nsettings get HARDWARE_TEST_RECORD\n");
    Check(h.Original() && h.validations == 0U && h.resets == 0U &&
              h.transport.output.find("original") != std::string::npos,
          "factory record reads remain available over remote Console");
    h.Feed(Source::Remote, "set BRIGHTNESS 2\nsettings set USB_CC_THRESH_500 330\n");
    Check(h.settings.ReadValue("BRIGHTNESS", 0U) == 2U &&
              h.settings.ReadValue("USB_CC_THRESH_500", 0U) == 330U,
          "ordinary remote settings and CC calibration retain existing admission");
    h.Feed(Source::Remote, "settings paste\nDEBUG_PICKUP=true\n\x04");
    Check(h.settings.ReadValue("DEBUG_PICKUP", false),
          "factory-specific admission does not blanket-block internal settings");
    h.settings.Doc()["UNREGISTERED_NOTE"] = "temporary";
    h.Feed(Source::Remote, "settings paste\nUNREGISTERED_NOTE=\n\x04");
    Check(!h.settings.Doc().containsKey("UNREGISTERED_NOTE"),
          "ordinary unregistered per-key paste removal retains existing behavior");
    const auto* descriptor = modules::settings_registry::LookupByKey(
        modules::HARDWARE_TEST_RECORD_SETTING_KEY);
    Check(descriptor != nullptr && descriptor->IsInternal() &&
              !modules::settings_registry::IsDirectAppAccessible(*descriptor),
          "factory record remains excluded from the existing direct app settings route");
    StaticJsonDocument<32> value;
    value.set("changed");
    const auto programmatic = h.console.ValidateAndSetSettingValue(
        h.settings, modules::HARDWARE_TEST_RECORD_SETTING_KEY,
        value.as<JsonVariantConst>());
    Check(programmatic == Result::Applied && !h.Original(),
          "programmatic Console settings calls retain canonical validation");
    Harness generation;
    generation.transport.generation = 71U;
    generation.Feed(Source::Remote, "settings paste\nBRIGHTNESS=2\n");
    Check(generation.observedSource == Source::Remote &&
              generation.observedGeneration == 71U &&
              generation.console.CommandRemoteInputGeneration() == 0U,
          "paste payload callbacks expose scoped current remote generation");
    generation.Feed(Source::Remote, "HARDWARE_TEST_RECORD=changed");
    ++generation.transport.generation;
    generation.Feed(Source::Local, "\n\x04");
    Check(generation.Original(),
          "retiring a remote paste cannot promote its buffered factory line");
}

void MutationOutput() {
    Harness h;
    h.Feed(Source::Remote, "set HARDWARE_TEST_RECORD changed\n");
    const std::string bareOutput = h.transport.output;
    h.transport.output.clear();
    h.Feed(Source::Remote, "settings set HARDWARE_TEST_RECORD changed\n");
    const std::string settingsOutput = h.transport.output;
    h.transport.output.clear();
    h.Feed(Source::Remote,
        "settings paste\nHARDWARE_TEST_RECORD=changed\nHARDWARE_TEST_RECORD=\n\x04");
    Check(!h.Original() && h.validations == 3U && h.resets == 1U &&
              bareOutput.find("access_denied") == std::string::npos &&
              settingsOutput.find("access_denied") == std::string::npos &&
              h.transport.output.find("requires a fresh USB") == std::string::npos,
          "valid Console mutations no longer report source-policy refusal");
    h.settings.Doc()[modules::HARDWARE_TEST_RECORD_SETTING_KEY] = "original";
    h.console.SetSettingsValidateAndSetCallback(
        [](fxe::SettingsStore&, std::string_view, JsonVariantConst) {
            return Result::StorageUnavailable;
        });
    h.transport.output.clear();
    h.Feed(Source::Remote, "set HARDWARE_TEST_RECORD changed\n");
    Check(h.Original() &&
              h.transport.output.find("set: storage_unavailable\r\n") != std::string::npos &&
              h.transport.output.find("access_denied") == std::string::npos,
          "genuine storage failure retains its distinct existing result");
}

} // namespace

int main() {
    TypedCommands();
    PasteCommands();
    AllowedReadsAndSettings();
    MutationOutput();
    std::printf("[CONSOLE_FACTORY_ACCESS] failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
