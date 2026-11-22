#ifdef main
#undef main
#define SETTINGS_STORE_AGGREGATE_ENTRY 1
#endif

#define main test_settings_store_upstream_main
#include "../../lib/foxie_embedded/tests/test_settings_store/test_main.cpp"
#undef main

#include <filesystem>

namespace settings_store_delete_regression_test {

namespace fs = std::filesystem;

constexpr const char* DELETE_PATH =
    "test_settings_store_delete_success.json";
constexpr const char* DELETE_FAILURE_PATH =
    "test_settings_store_delete_failure.json";

void Cleanup(const char* const path) {
    std::error_code error;
    (void)fs::remove_all(path, error);
    (void)fs::remove_all(std::string(path) + ".tmp", error);
    (void)fs::remove_all(std::string(path) + ".bak", error);
}

bool WriteFile(const std::string& path, const char* const contents) {
    std::FILE* const file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const size_t size = std::strlen(contents);
    const bool written = std::fwrite(contents, 1U, size, file) == size;
    (void)std::fclose(file);
    return written;
}

int Run() {
    fxe::test_support::TestResults results("SETTINGS_STORE_DELETE");
    results.Start();

    Cleanup(DELETE_PATH);
    const std::string tmpPath = std::string(DELETE_PATH) + ".tmp";
    const std::string backupPath = std::string(DELETE_PATH) + ".bak";
    results.Check(WriteFile(DELETE_PATH, R"({"PRIMARY":1})"),
                  "delete fixture creates primary artifact");
    results.Check(WriteFile(tmpPath, R"({"TMP":1})"),
                  "delete fixture creates tmp artifact");
    results.Check(WriteFile(backupPath, R"({"BACKUP":1})"),
                  "delete fixture creates backup artifact");

    fxe::SettingsStoreConfig config;
    config.path = DELETE_PATH;
    fxe::SettingsStore store(config);
    store.Doc()["RAM_SENTINEL"] = 73U;

    results.Check(store.DeleteFile(),
                  "DeleteFile succeeds after removing every artifact");
    results.Check(!fs::exists(DELETE_PATH) &&
                      !fs::exists(tmpPath) &&
                      !fs::exists(backupPath),
                  "successful DeleteFile removes primary, tmp, and backup");
    results.Check(store.ReadValue("RAM_SENTINEL", 0U) == 0U,
                  "successful DeleteFile clears the in-memory document");

    Cleanup(DELETE_PATH);

    Cleanup(DELETE_FAILURE_PATH);
    const std::string failureTmpPath =
        std::string(DELETE_FAILURE_PATH) + ".tmp";
    const std::string failureBackupPath =
        std::string(DELETE_FAILURE_PATH) + ".bak";
    const std::string backupBlockerPath = failureBackupPath + "/blocker";
    results.Check(WriteFile(DELETE_FAILURE_PATH, R"({"PRIMARY":2})"),
                  "failed-delete fixture creates primary artifact");
    results.Check(WriteFile(failureTmpPath, R"({"TMP":2})"),
                  "failed-delete fixture creates tmp artifact");
    std::error_code directoryError;
    results.Check(fs::create_directory(failureBackupPath, directoryError),
                  "failed-delete fixture creates backup directory");
    results.Check(WriteFile(backupBlockerPath, "block deletion"),
                  "failed-delete fixture makes backup removal fail");

    fxe::SettingsStoreConfig failureConfig;
    failureConfig.path = DELETE_FAILURE_PATH;
    fxe::SettingsStore failureStore(failureConfig);
    failureStore.Doc()["RAM_SENTINEL"] = 91U;

    results.Check(!failureStore.DeleteFile(),
                  "DeleteFile reports a surviving backup artifact");
    results.Check(failureStore.GetLastError() ==
                      fxe::SettingsStoreError::DeleteFailed,
                  "backup removal failure reports DeleteFailed");
    results.Check(failureStore.ReadValue("RAM_SENTINEL", 0U) == 91U,
                  "failed DeleteFile retains the in-memory document");
    results.Check(fs::exists(failureBackupPath),
                  "failed DeleteFile does not claim a surviving backup is gone");

    Cleanup(DELETE_FAILURE_PATH);
    return results.Finish();
}

} // namespace settings_store_delete_regression_test

namespace settings_store_admission_regression_test {
int Run() {
    fxe::test_support::TestResults results("SETTINGS_STORE_ADMISSION");
    results.Start();
    fxe::ScopedMillisProviderOverride clock(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0U);
    fxe::SettingsStoreConfig config;
    config.path = "build/t_admission.json";
    config.allowSynchronousDeferredSaves = false;
    config.mustSaveDelayMs = 100U;
    config.deferredSaveDelayMs = 50U;
    fxe::SettingsStore store(config);
    using Admission = fxe::SettingsStore::AsyncSaveAdmission;
    auto admission = Admission::Deferred;
    uint32_t requests = 0U, queued = 0U;
    store.SetAsyncSaveAdmissionCallback([&](const JsonDocument& doc) {
        ++requests;
        if (admission == Admission::Accepted) { queued = doc["VALUE"] | 0U; }
        return admission;
    });
    store.Doc()["VALUE"] = 7U;
    store.ArmPersistenceGuard();
    results.Check(store.HasAsyncSaveRequestCallback() && !store.SaveNow(),
                  "deferred callback is asynchronous but does not fabricate acceptance");
    results.Check(store.GetErrorCount() == 0U && store.GetLastError() == fxe::SettingsStoreError::None &&
                  store.GetSaveCount() == 0U && store.IsPersistenceGuardArmed(),
                  "initial deferral retains the guard without manufacturing error or durability");
    fxe::ManualMillis::SetNowMs(99U); store.Update();
    results.Check(requests == 1U, "deferred explicit SaveNow retains a normal must-save request");
    fxe::ManualMillis::SetNowMs(100U); store.Update();
    results.Check(requests == 2U && store.GetErrorCount() == 0U,
                  "a due source Update retries deferred admission without a failure");
    admission = Admission::Accepted; store.Update();
    results.Check(queued == 7U && !store.IsPersistenceGuardArmed() && store.GetSaveCount() == 0U,
                  "later acceptance clears pending intent but is not durable completion");
    const auto acceptedRequests = requests; store.Update();
    results.Check(requests == acceptedRequests, "accepted request clears both deferred tiers");

    store.ReportAsyncSaveFailure(fxe::SettingsStoreError::VerifyFailed);
    const auto priorErrors = store.GetErrorCount();
    admission = Admission::Deferred;
    store.Doc()["VALUE"] = 8U; store.MarkDirty();
    fxe::ManualMillis::SetNowMs(150U); store.Update();
    results.Check(!store.SaveNow() && store.GetErrorCount() == priorErrors &&
                  store.GetLastError() == fxe::SettingsStoreError::VerifyFailed,
                  "temporary deferral preserves a real prior verification failure");
    admission = Admission::Rejected;
    results.Check(!store.SaveNow() && store.GetErrorCount() == priorErrors + 1U &&
                  store.GetLastError() == fxe::SettingsStoreError::AdmissionRejected,
                  "definitive rejection still reports a new failure");
    store.SetAsyncSaveRequestCallback([](const JsonDocument&) { return false; });
    results.Check(!store.SaveNow() && store.GetErrorCount() == priorErrors + 2U,
                  "legacy bool false replaces typed callback and retains rejection semantics");
    store.SetAsyncSaveRequestCallback([](const JsonDocument&) { return true; });
    results.Check(store.SaveNow() && store.GetLastError() == fxe::SettingsStoreError::AdmissionRejected,
                  "legacy acceptance still cannot erase a durability error");
    store.ReportAsyncSaveSuccess(100U, 1U);
    results.Check(store.GetLastError() == fxe::SettingsStoreError::None,
                  "only confirmed completion clears the outstanding asynchronous error");
    store.SetAsyncSaveAdmissionCallback(nullptr);
    results.Check(!store.HasAsyncSaveRequestCallback(), "clearing typed callback also replaces the legacy owner");
    return results.Finish();
}
} // namespace settings_store_admission_regression_test


namespace settings_readonly_diagnostic_test {
int Run() {
    using namespace settings_store_delete_regression_test;
    int failures = 0;
    const char* path = "test_settings_primary_readonly.json";
    Cleanup(path);
    const std::string bytes = "{\"VALUE\":7,\"FEN\":\"preserved\"} \n\t";
    auto hash = [](const std::string& value) {
        uint32_t result = 2166136261U;
        for (const unsigned char byte : value) { result ^= byte; result *= 16777619U; }
        return result;
    };
    fxe::SettingsStoreConfig config; config.path = path;
    fxe::SettingsStore reader(config);
    size_t actual = 0U; uint32_t fnv = 0U;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("[FAIL] %s\n", message); }
    };
    check(WriteFile(path, bytes.c_str()), "seed primary bytes with trailing whitespace");
    check(reader.ReadPrimaryForDiagnostics(actual, fnv) && actual == bytes.size() &&
              fnv == hash(bytes) && reader.Doc()["VALUE"] == 7U &&
              reader.GetSaveCount() == 0U && reader.GetErrorCount() == 0U,
          "readonly production reader consumes exact EOF/hash without saving or changing store health");
    const std::string large = "{\"PAYLOAD\":\"" + std::string(1300U, 'q') + "\"}";
    unsigned acquired = 0U, released = 0U;
    reader.SetStorageAccessCallbacks([&] { ++acquired; }, [&] { ++released; });
    check(WriteFile(path, large.c_str()) && reader.ReadPrimaryForDiagnostics(actual, fnv) &&
              actual == large.size() && fnv == hash(large) && acquired == released && acquired >= 6U,
          "readonly production reader hands off and reacquires balanced storage leases between bounded chunks");
    reader.SetStorageAccessCallbacks(nullptr, nullptr);
    check(WriteFile(path, "{bad") && WriteFile(std::string(path)+".bak", "{\"VALUE\":99}"),
          "seed corrupt primary and a valid alternate");
    check(!reader.ReadPrimaryForDiagnostics(actual, fnv) && actual == 4U &&
              fnv == hash("{bad") && !reader.Doc().containsKey("VALUE") &&
              reader.GetSaveCount() == 0U && reader.GetErrorCount() == 0U,
          "readonly diagnostic rejects corrupt primary without fallback, repair, save or live error mutation");
    std::FILE* file = std::fopen(path, "rb"); char retained[4]{};
    check(file != nullptr && std::fread(retained, 1U, 4U, file) == 4U &&
              std::memcmp(retained, "{bad", 4U) == 0,
          "corrupt primary remains byte exact despite valid backup");
    if (file != nullptr) std::fclose(file);
    check(WriteFile(path, "[1,2,3]") && !reader.ReadPrimaryForDiagnostics(actual, fnv),
          "nonobject primary is not validated as settings");
    Cleanup(path);
    check(!reader.ReadPrimaryForDiagnostics(actual, fnv) && actual == 0U &&
              !std::filesystem::exists(path), "missing primary is not created or formatted");
    std::puts("[SETTINGS_READONLY_DIAGNOSTIC] exact bytes/hash, corruption, missing, no writes");
    return failures == 0 ? 0 : 1;
}
}

#ifdef SETTINGS_STORE_AGGREGATE_ENTRY
int test_settings_store_main() {
#else
int main() {
#endif
    const int upstream = test_settings_store_upstream_main();
    const int regression = settings_store_delete_regression_test::Run();
    const int admission = settings_store_admission_regression_test::Run();
    const int readonly = settings_readonly_diagnostic_test::Run();
    return upstream != 0 || regression != 0 || admission != 0 || readonly != 0 ? 1 : 0;
}
