#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_persistence.hpp"
#include "modules/game_persistence.hpp"
#include "modules/settings_store.hpp"
#include "foxie_embedded/console.hpp"
#include "foxie_embedded/console_transport_memory.hpp"
#include "foxie_embedded/test_support/console_test_helpers.hpp"
#include "foxie_embedded/millis.hpp"

namespace {
int failures = 0;
void Check(bool ok, const char* why) { if (!ok) { ++failures; std::printf("[FAIL] %s\n", why); } }
struct Random {
    uint8_t value = 1U;
    unsigned calls = 0U;
    bool fail = false;
    bool repeat = false;
    static bool Fill(void* context, uint8_t* out, size_t length) {
        auto& self = *static_cast<Random*>(context);
        ++self.calls;
        if (self.fail) return false;
        std::memset(out, self.value, length);
        if (!self.repeat) ++self.value;
        return true;
    }
};
void TestCodecAndQuarantine() {
    StaticJsonDocument<4096> doc;
    modules::BoardIdentity id;
    Check(modules::ReadBoardIdentity(doc, id) == modules::BoardIdentityRead::Absent, "absent differs from corrupt");
    id.bytes.fill(0xabU);
    Check(modules::WriteBoardIdentity(doc, id), "board identity encodes");
    Check(std::strcmp(doc[modules::KEY_BOARD_IDENTITY_V1][1U] | "", "abababababababababababababababab") == 0, "fixed lowercase128bit encoding");
    modules::BoardIdentity decoded;
    Check(modules::ReadBoardIdentity(doc, decoded) == modules::BoardIdentityRead::Current && decoded == id, "exact roundtrip");
    for (const char* text : {"", "abc", "00000000000000000000000000000000", "ABABABABABABABABABABABABABABABAB", "abababababababababababababababab0"}) {
        doc[modules::KEY_BOARD_IDENTITY_V1][1U] = text;
        Random random;
        modules::BoardIdentityPersistence state;
        std::string before;
        serializeJson(doc, before);
        Check(!state.Prepare(doc, &Random::Fill, &random) && random.calls == 0U && !state.IsDurable(), "invalid record is not reminted");
        std::string after;
        serializeJson(doc, after);
        Check(before == after, "quarantine preserves exact optional record");
    }
    Check(modules::WriteBoardIdentity(doc, id), "restore valid shape");
    doc[modules::KEY_BOARD_IDENTITY_V1][0U] = 2U;
    Check(modules::ReadBoardIdentity(doc, decoded) == modules::BoardIdentityRead::Invalid, "future schema quarantines");
    doc[modules::KEY_BOARD_IDENTITY_V1] = "malformed auxiliary";
    Check(modules::CompactDocRemovingGameState(doc), "ordinary game compaction works");
    Check(std::strcmp(doc[modules::KEY_BOARD_IDENTITY_V1] | "", "malformed auxiliary") == 0, "ordinary compaction retains malformed board key");
    doc[modules::KEY_BOARD_IDENTITY_V1] = std::string(1100U, 'x');
    Check(!modules::CompactDocRemovingGameState(doc), "oversized remainder takes existing fallback");
    modules::ClearGameState(doc);
    modules::ClearUndoState(doc);
    Check(std::strlen(doc[modules::KEY_BOARD_IDENTITY_V1] | "") == 1100U, "fallback retains malformed board key");
}
void TestGenerationAndDurability() {
    StaticJsonDocument<4096> doc;
    Random random;
    modules::BoardIdentityPersistence state;
    Check(state.Prepare(doc, &Random::Fill, &random) && state.IsPrepared() && !state.IsDurable(), "generated identity remains uncommitted");
    modules::game_history::GameId game;
    Check(state.GenerateGameIdentity(game) && game.bytes[0] == 2U, "game uses independent128bit draw");
    const auto first = game;
    random.value = 2U;
    Check(state.GenerateGameIdentity(game) && game.bytes[0] == 3U, "known collision retries without reusing game identity");
    random.value = 3U;
    random.repeat = true;
    const auto calls = random.calls;
    Check(!state.GenerateGameIdentity(game) && random.calls == calls + 4U, "collision retries are bounded");
    Check(game.bytes[0] == 3U, "failed generation does not publish candidate");
    modules::SettingsStoreConfig cfg;
    cfg.path = "build/test_board_identity.json";
    std::remove(cfg.path); std::remove("build/test_board_identity.json.bak"); std::remove("build/test_board_identity.json.tmp");
    modules::SettingsStore writer(cfg);
    writer.Doc().set(doc);
    // No callback on a failed/absent promotion. A different successful snapshot
    // is also insufficient; only the actual promoted matching record qualifies.
    StaticJsonDocument<256> unrelated;
    state.OnDurableSettingsSnapshot(unrelated);
    Check(!state.IsDurable(), "unrelated promotion cannot grant readiness");
    Check(writer.SaveNowBlocking(), "real settings atomic promotion succeeds");
    modules::SettingsStore loaded(cfg);
    Check(loaded.Load(), "promoted identity reloads");
    state.OnDurableSettingsSnapshot(loaded.Doc());
    Check(state.IsDurable(), "exact postpromotion snapshot grants readiness");
    state.OnDurableSettingsSnapshot(loaded.Doc());
    Check(state.IsDurable(), "duplicate promotion is idempotent");
    Random rebootRandom;
    modules::BoardIdentityPersistence reboot;
    Check(reboot.Prepare(loaded.Doc(), &Random::Fill, &rebootRandom) && rebootRandom.calls == 0U && !reboot.IsDurable(), "reboot retains id but does not infer a new promotion");
    reboot.OnDurableSettingsSnapshot(loaded.Doc());
    Check(reboot.IsDurable(), "reboot exact readback/promotion can validate same identity");
    modules::BoardIdentity replacement;
    replacement.bytes.fill(9U);
    Check(modules::WriteBoardIdentity(unrelated, replacement), "replacement fixture writes");
    state.OnDurableSettingsSnapshot(unrelated);
    Check(!state.IsDurable(), "replacement cannot retain prior readiness");
    doc.clear();
    random.repeat = false;
    random.value = 7U;
    modules::BoardIdentityPersistence reset;
    Check(reset.Prepare(doc, &Random::Fill, &random), "full reset provisions fresh installation identity");
    modules::BoardIdentity afterReset, beforeReset;
    (void)modules::ReadBoardIdentity(doc, afterReset);
    (void)modules::ReadBoardIdentity(loaded.Doc(), beforeReset);
    Check(!(afterReset == beforeReset), "full storage reset breaks identity continuity");
    (void)first;
}
void TestConsoleIdentityProtection() {
    fxe::SettingsStoreConfig config; config.path = "build/test_identity_console.json";
    std::remove(config.path); std::remove("build/test_identity_console.json.bak");
    fxe::SettingsStore settings(config); settings.Load();
    modules::BoardIdentity identity; identity.bytes.fill(0xabU);
    Check(modules::WriteBoardIdentity(settings.Doc(), identity), "console identity fixture");
    Check(modules::WriteBoardOwner(settings.Doc(), modules::BoardOwnerRecord::Active),
          "console owner fixture");
    settings.Doc()["ORDINARY"] = "ordinary-value";
    fxe::MemoryConsoleTransport transport;
    fxe::test_support::ConsoleOutputCapture output; output.Attach(transport);
    fxe::Console console(transport); console.SetSettingsStore(&settings);
    auto command = [&](const char* text) { output.Clear(); fxe::test_support::RunCommand(console, transport, text); };
    command("settings get BOARD_IDENTITY_V1");
    Check(output.Str().find("abababab") != std::string::npos, "default diagnostic policy preserves existing callers");
    console.SetSettingsDiagnosticAccessCallback([](std::string_view key) {
        return !modules::IsBoardIdentitySettingKey(key) &&
               !modules::IsBoardOwnerSettingKey(key);
    });
    console.SetSettingsWritePolicyCallback(
        [](std::string_view key, bool* forceSave) {
            if (forceSave == nullptr || modules::IsBoardIdentitySettingKey(key) ||
                modules::IsBoardOwnerSettingKey(key)) {
                return false;
            }
            *forceSave = false;
            return true;
        });
    console.SetSettingsValidateAndSetCallback(
        [](fxe::SettingsStore&, std::string_view key, JsonVariantConst) {
            return modules::IsBoardIdentitySettingKey(key) ||
                           modules::IsBoardOwnerSettingKey(key)
                       ? fxe::Console::SettingsSetResult::UnknownKey
                       : fxe::Console::SettingsSetResult::Ok;
        });
    for (const char* cmd : {"settings get BOARD_IDENTITY_V1", "settings get board_identity_v1",
                            "settings get BOARD_OWNER_V1", "settings get board_owner_v1",
                            "settings dump", "settings keys", "set", "set board_identity_v1",
                            "set board_owner_v1",
                            "settings set BOARD_OWNER_V1 [1,2]"}) {
        command(cmd);
        Check(output.Str().find("abababab") == std::string::npos, "identity bytes hidden from diagnostic commands");
        if (std::string_view(cmd) == "settings dump" || std::string_view(cmd) == "settings keys" || std::string_view(cmd) == "set") {
            Check(output.Str().find("BOARD_IDENTITY_V1") == std::string::npos && output.Str().find("ORDINARY") != std::string::npos,
                  "diagnostic enumeration hides identity and retains ordinary keys");
            Check(output.Str().find("BOARD_OWNER_V1") == std::string::npos,
                  "diagnostic enumeration hides owner record");
        }
    }
    Check(modules::ReadBoardOwner(settings.Doc()) ==
              modules::BoardOwnerRecord::Active,
          "settings set cannot mutate owner record");
    // A filtered dump remains complete JSON across writer chunks and escaped keys.
    const std::string longValue(1200U, 'x');
    settings.Doc()["quoted\"key"] = longValue;
    command("settings dump");
    const auto jsonStart = output.Str().find('{');
    const auto jsonEnd = output.Str().rfind('}');
    StaticJsonDocument<2048> dumped;
    const bool parsed = jsonStart != std::string::npos && jsonEnd != std::string::npos &&
        deserializeJson(dumped, output.Str().substr(jsonStart, jsonEnd - jsonStart + 1U)) == DeserializationError::Ok;
    Check(parsed && dumped["quoted\"key"].as<std::string>() == longValue &&
              !dumped.containsKey(modules::KEY_BOARD_IDENTITY_V1) &&
              !dumped.containsKey(modules::KEY_BOARD_OWNER_V1),
          "filtered dump streams complete escaped JSON beyond 1024 bytes");
    settings.Doc().remove("quoted\"key");
    command("settings get ORDINARY");
    Check(output.Str().find("ordinary-value") != std::string::npos, "ordinary reads remain available");
    command("settings paste");
    fxe::test_support::FeedConsole(
        console, transport,
        "board_identity_v1=\nboard_owner_v1=[1,2]\nORDINARY=\n");
    transport.PushChar(0x04); console.Update();
    modules::BoardIdentity retained;
    Check(modules::ReadBoardIdentity(settings.Doc(), retained) == modules::BoardIdentityRead::Current && retained == identity &&
          !settings.Doc().containsKey("ORDINARY"), "empty paste preserves identity while ordinary deletion still works");
    Check(modules::ReadBoardOwner(settings.Doc()) == modules::BoardOwnerRecord::Active,
          "paste cannot mutate owner record");
    Check(output.Str().find("applied=1 failed=2") != std::string::npos,
          "protected identity and owner mutations fail explicitly");
    Check(settings.SaveNowBlocking(), "console fixture has durable identity before full reset");
    unsigned attemptedStorageMutations = 0U;
    console.SetSettingsStorageAdmissionCallback(
        [&](fxe::Console::SettingsStorageOperation) {
            ++attemptedStorageMutations;
            return false;
        });
    const uint32_t beforeRevision = settings.GetRevision();
    for (const char* destructive : {"settings clear", "settings erase", "settings load",
                                    "settings corrupt", "settings format ERASE_BOARD_STORAGE"}) {
        command(destructive);
        Check(output.Str().find("physical board recovery") != std::string::npos,
              "refused storage replacement gives the physical recovery next action");
        Check(modules::ReadBoardIdentity(settings.Doc(), retained) ==
                  modules::BoardIdentityRead::Current && retained == identity &&
                  modules::ReadBoardOwner(settings.Doc()) == modules::BoardOwnerRecord::Active &&
                  settings.GetRevision() == beforeRevision,
              "normal Console cannot replace private identity or remotely clear ownership");
        modules::SettingsStore reopened(config);
        Check(reopened.Load() && modules::ReadBoardOwner(reopened.Doc()) ==
                  modules::BoardOwnerRecord::Active,
              "admission refusal precedes actual filesystem deletion and corruption");
    }
    Check(attemptedStorageMutations == 5U, "every whole-storage replacement reaches admission once");
    command("settings format status");
    Check(attemptedStorageMutations == 5U, "format status remains diagnostic");
    console.SetSettingsStorageAdmissionCallback({});
    unsigned resets = 0U;
    bool acceptReset = false;
    console.SetSettingsClearCallback([&]() {
        ++resets;
        modules::SettingsStore reopened(config);
        Check(reopened.Load() && modules::ReadBoardOwner(reopened.Doc()) ==
                  modules::BoardOwnerRecord::Active,
              "reset owner receives original durable owner before any raw deletion");
        return acceptReset;
    });
    command("settings clear");
    acceptReset = true;
    command("settings erase");
    Check(resets == 2U && modules::ReadBoardOwner(settings.Doc()) ==
              modules::BoardOwnerRecord::Active,
          "refusal and accepted asynchronous reset preserve live owner and settings");
    modules::SettingsStore resetPending(config);
    Check(resetPending.Load() && modules::ReadBoardOwner(resetPending.Doc()) ==
              modules::BoardOwnerRecord::Active,
          "accepted request is not deletion or a claim of completed reset");
    console.SetSettingsClearCallback({});
    command("settings clear");
    Check(!settings.Doc().containsKey(modules::KEY_BOARD_IDENTITY_V1), "explicit full-storage clear remains available");
    Check(!settings.Doc().containsKey(modules::KEY_BOARD_OWNER_V1),
          "explicit full-storage clear removes owner record");
}
void TestEntropyFailures() {
    for (bool failure : {false, true}) {
        StaticJsonDocument<256> doc;
        Random random;
        random.value = 0U; random.repeat = true; random.fail = failure;
        modules::BoardIdentityPersistence state;
        Check(!state.Prepare(doc, &Random::Fill, &random) && !state.IsPrepared() && !state.IsDurable(), "zero or failed entropy cannot mint authority");
        Check(random.calls == (failure ? 1U : 4U), "entropy failure/retry count bounded");
        Check(!doc.containsKey(modules::KEY_BOARD_IDENTITY_V1), "failed entropy leaves legacy record absent");
    }
}
void TestPrimaryLoadProofAndReadOnlyRecovery() {
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    for (const unsigned source : {0U, 1U, 2U}) {
        char directory[] = "/tmp/litch-identity-load-proof-XXXXXX";
        Check(::mkdtemp(directory) != nullptr, "temporary reader-proven identity fixture");
        const auto path = std::filesystem::path(directory) / "settings.json";
        const std::string name = path.string();
        const std::string original = "{\"BOARD_IDENTITY_V1\":[1,\"0102030405060708090a0b0c0d0e0f10\"],\"USER_VALUE\":7}";
        std::ofstream(path) << (source == 2U ? "bad json" : original);
        if (source == 1U) std::ofstream(name + ".tmp") << original;
        if (source == 2U) std::ofstream(name + ".bak") << original;
        const auto read = [](const std::string& file) {
            std::ifstream input(file);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        const auto primary = read(name), pending = read(name + ".tmp"), backup = read(name + ".bak");
        modules::SettingsStoreConfig config;
        config.path = name.c_str();
        config.allowSynchronousDeferredSaves = false;
        if (source == 0U) {
            config.migrateFunc = [](JsonDocument& document) { document["RAM_REPAIR"] = 73U; return true; };
        }
        modules::SettingsStore store(config);
        modules::BoardIdentityPersistence identity;
        store.SetVerifiedPrimaryLoadCallback([](void* context, const JsonDocument& document) {
            static_cast<modules::BoardIdentityPersistence*>(context)->ObserveVerifiedPrimaryLoad(document);
        }, &identity);
        Check(store.Load(), "canonical or alternate settings remain usable without a boot write");
        Random random;
        Check(identity.Prepare(store.Doc(), &Random::Fill, &random), "existing loaded identity prepares without entropy");
        Check(identity.IsDurable() == (source == 0U), "only actual canonical-primary read proves loaded identity durability");
        Check(random.calls == 0U &&
                  (source != 0U || store.Doc()["RAM_REPAIR"].as<unsigned>() == 73U),
              "RAM repair keeps exact existing identity without generating a replacement");
        unsigned requests = 0U;
        store.SetAsyncSaveRequestCallback([&](const JsonDocument&) { ++requests; return true; });
        fxe::ManualMillis::AdvanceMs(10000U);
        store.Update();
        Check(requests == 0U && store.GetSaveCount() == 0U, "idle boot repair cannot request a deferred or retry save");
        Check(read(name) == primary && read(name + ".tmp") == pending && read(name + ".bak") == backup,
              "Load and idle preserve primary pending and backup bytes exactly");
        store.Doc()["USER_ACTION"] = 9U;
        store.MarkDirty();
        fxe::ManualMillis::AdvanceMs(10000U);
        store.Update();
        Check(requests == 1U, "a later real user mutation admits pending RAM repairs through the existing save owner");
        std::filesystem::remove_all(directory);
    }
}
} // namespace
int main() {
    TestCodecAndQuarantine();
    TestGenerationAndDurability();
    TestEntropyFailures();
    TestConsoleIdentityProtection();
    TestPrimaryLoadProofAndReadOnlyRecovery();
    if (failures == 0) std::puts("[PASS] board identity persistence");
    return failures == 0 ? 0 : 1;
}
