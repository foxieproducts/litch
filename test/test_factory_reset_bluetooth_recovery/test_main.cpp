#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

#include "firmware/factory_reset_settings.hpp"
#include "modules/board_identity_persistence.hpp"
#include "modules/board_owner_pairing_controller.hpp"

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (!ok) { ++failures; std::printf("[FAIL] factory Bluetooth reset: %s\n", message); }
}
using Store = modules::SettingsStore;
using Record = modules::BoardOwnerRecord;
using Controller = modules::BoardOwnerPairingController;
using Kind = fxe::SettingsSaveOperationKind;
using Role = fxe::SettingsSavePathRole;

struct Persistence : modules::BoardOwnerPersistence {
    Store& store;
    Receipt receipt = 0;
    explicit Persistence(Store& value) : store(value) {}
    bool SaveOwnerRecord(Record record, Receipt& out) override {
        if (!modules::WriteBoardOwner(store.Doc(), record) || !store.SaveNowBlocking()) return false;
        out = ++receipt; return true;
    }
    Outcome GetOutcome(Receipt value) const override { return value == receipt ? Outcome::Saved : Outcome::Unknown; }
    bool Release(Receipt value) override { return value == receipt; }
};

void CompleteFirstPair(Store& store) {
    Persistence persistence(store); Controller owner(persistence);
    owner.Boot(modules::ReadBoardOwner(store.Doc()), {true, 0U, {}, true});
    Check(!owner.Pairable() && owner.PendingEffect().type == Controller::EffectType::QuiesceAndRejectWork,
          "factory tombstone resumes cleanup before advertising first-phone access");
    owner.AcknowledgeQuiesce(owner.PendingEffect().token, true);
    owner.AcknowledgeBondDelete(owner.PendingEffect().token, true);
    owner.PollPersistence();
    owner.AcknowledgeSuccess(owner.PendingEffect().token);
    Check(owner.Pairable() && modules::ReadBoardOwner(store.Doc()) == Record::Absent,
          "only verified remote cleanup and durable absence release first-phone pairing");
    Check(owner.ArmPair(1U, 7U, 9U, 0U) && owner.ConfirmPair(1U, 7U, 9U, 0U),
          "fresh first phone can complete physical consent after reset");
    Controller::ConnectionSecurity security{{true, 1U}, true, true, true, true};
    owner.AcknowledgeSecurity(owner.PendingEffect().token, 1U, 7U, 9U, 0U, security);
    owner.PollPersistence();
    Check(modules::ReadBoardOwner(store.Doc()) == Record::Active,
          "new first phone reaches durable ownership");
}

void Run(const std::string& path, bool pairing, bool pending, unsigned cutAfterRename,
         bool safeMode = false) {
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) std::filesystem::remove(path + suffix);
    modules::SettingsStoreConfig config{}; config.path = path.c_str();
    Store original(config);
    modules::BoardIdentity identity{}; identity.bytes[0] = 91U; identity.bytes[15] = 47U;
    modules::WriteBoardIdentity(original.Doc(), identity);
    modules::WriteBoardOwner(original.Doc(), Record::Active);
    original.Doc()["GAME_FEN"] = "old private game";
    original.Doc()["BRIGHTNESS"] = 7U;
    original.Doc()["LED_BROWNOUT_LIMIT_LATCH"] = true;
    Check(original.SaveNowBlocking(), "seed durable old identity and private state");
    if (pending) std::filesystem::copy_file(path, path + ".tmp");
    std::filesystem::copy_file(path, path + ".retry");
    if (safeMode) original.DisableForBoot();
    unsigned renames = 0;
    original.SetSaveOperationCallback([&](const fxe::SettingsSaveOperation& operation) {
        if (operation.kind == Kind::Rename && operation.succeeded && ++renames == cutAfterRename) _exit(75);
    });
    const pid_t child = fork();
    Check(child >= 0, "spawn isolated persistence process for a real abrupt stop");
    if (child == 0) _exit(firmware::ResetFactorySettings(original, pairing) ? 0 : 2);
    int status = 0;
    Check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status), "observe persistence process exit");
    const bool completed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const bool interrupted = WIFEXITED(status) && WEXITSTATUS(status) == 75;
    Check(completed != interrupted, "reset either verifies completion or reaches selected power cut");
    Store reopened(config); Check(reopened.Load(), "reset state reopens after interruption");
    modules::BoardIdentity read{};
    Check(modules::ReadBoardIdentity(reopened.Doc(), read) == modules::BoardIdentityRead::Current && read == identity,
          "every staging/promotion interruption preserves the exact board identity");
    Check(!reopened.Doc().containsKey("GAME_FEN") && !reopened.Doc().containsKey("BRIGHTNESS") &&
              reopened.Doc()["LED_BROWNOUT_LIMIT_LATCH"].as<bool>(),
          "preferred reset snapshot removes private state and retains hardware protection");
    Check(modules::ReadBoardOwner(reopened.Doc()) == (pairing ? Record::Clearing : Record::Absent),
          "only owner-pairing builds carry a crash-resumable native clear intent");
    if (interrupted) Check(firmware::ResetFactorySettings(reopened, pairing), "interrupted reset is safely repeatable");
    Check(!std::filesystem::exists(path + ".tmp") && !std::filesystem::exists(path + ".retry"),
          "completed reset leaves no old preferred or retry document");
    // A damaged primary must recover only the sanitized backup after success.
    { std::ofstream corrupt(path); corrupt << "{"; }
    Store backup(config); Check(backup.Load(), "sanitized backup survives primary corruption");
    Check(!backup.Doc().containsKey("GAME_FEN") && modules::ReadBoardOwner(backup.Doc()) ==
              (pairing ? Record::Clearing : Record::Absent), "old owner/game cannot return through backup");
    if (pairing) CompleteFirstPair(backup);
    std::printf("[FACTORY_RESET_BLUETOOTH] pairing=%u pending=%u cut=%u safe=%u identity=retained cleanup=resumable\n",
                pairing, pending, cutAfterRename, safeMode);
}

void RecoveryEdges(const std::string& path) {
    modules::SettingsStoreConfig config{}; config.path = path.c_str();
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) std::filesystem::remove(path + suffix);
    Store legacy(config);
    modules::BoardIdentity identity{}; identity.bytes[0] = 27U;
    modules::WriteBoardIdentity(legacy.Doc(), identity);
    modules::WriteBoardOwner(legacy.Doc(), Record::Active);
    Check(legacy.SaveNowBlocking() && legacy.DeleteFile(), "exercise baseline physical-reset storage action");
    Store oldReopen(config); Check(oldReopen.Load(), "baseline reset reopens missing settings");
    modules::BoardIdentity oldIdentity{};
    Check(modules::ReadBoardIdentity(oldReopen.Doc(), oldIdentity) == modules::BoardIdentityRead::Absent,
          "baseline delete loses the canonical identity and native cleanup intent");
    std::puts("[FACTORY_RESET_BASELINE] DeleteFile identity=lost owner_intent=absent");

    for (const char* suffix : {"", ".tmp", ".bak"}) { std::ofstream file(path + suffix); file << "{"; }
    Store corrupt(config); corrupt.DisableForBoot();
    Check(firmware::ResetFactorySettings(corrupt, true), "explicit reset can replace wholly corrupt documents in safe mode");
    Store reset(config); Check(reset.Load() && modules::ReadBoardOwner(reset.Doc()) == Record::Clearing,
          "irrecoverable personal state still yields verified native cleanup intent");
    for (const char* suffix : {"", ".tmp", ".bak", ".retry"}) std::filesystem::remove(path + suffix);
    std::filesystem::create_directory(path);
    Store unavailable(config);
    Check(!firmware::ResetFactorySettings(unavailable, true) && std::filesystem::is_directory(path),
          "an unavailable read is not treated as proof of corrupt or missing identity");
    std::filesystem::remove(path);
}
}

int main() {
    char directory[] = "/tmp/litch-reset-XXXXXX";
    Check(mkdtemp(directory) != nullptr, "temporary reset fixture directory");
    const std::string path = std::string(directory) + "/settings.json";
    for (bool pairing : {false, true}) {
        Run(path, pairing, false, 0U, true);
        Run(path, pairing, true, 0U);
        for (unsigned cut = 1U; cut <= 4U; ++cut) Run(path, pairing, false, cut);
        for (unsigned cut = 1U; cut <= 5U; ++cut) Run(path, pairing, true, cut);
    }
    RecoveryEdges(path);
    std::filesystem::remove_all(directory);
    if (!failures) std::puts("[FACTORY_RESET_BLUETOOTH] ok");
    return failures ? 1 : 0;
}
