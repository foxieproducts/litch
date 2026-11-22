#include <cstdio>
#include <filesystem>
#include <fstream>

// Includes the production POSIX probe; Arduino flash scheduling and LittleFS
// writes remain S3/HIL boundaries, not a second fake filesystem implementation.
#include "../../src/game_history_journal_storage.cpp"

int main() {
    using modules::game_history::JournalStorageStatus;
    const char* directory="build/test_history_file_probe";
    std::filesystem::create_directories(directory);
    const std::string file=std::string(directory)+"/state";
    { std::ofstream out(file); out << "retained journal state"; }
    uint32_t bytes=99U;
    int failures=0;
    auto check=[&](bool ok,const char* message) {
        if (!ok) { ++failures; std::printf("[FAIL] %s\n",message); }
    };
    check(firmware::ProbeJournalFile(file.c_str(),bytes)==JournalStorageStatus::Present && bytes==22U,
          "actual regular file has exact retained size");
    const std::string missing=std::string(directory)+"/missing";
    std::filesystem::remove(missing);
    check(firmware::ProbeJournalFile(missing.c_str(),bytes)==JournalStorageStatus::Missing && bytes==0U,
          "confirmed ENOENT alone admits a missing artifact");
    const std::string invalid=file+"/state";
    check(firmware::ProbeJournalFile(invalid.c_str(),bytes)==JournalStorageStatus::IoFailure && bytes==0U,
          "actual ENOTDIR failure cannot masquerade as missing state");
    check(firmware::ProbeJournalFile(directory,bytes)==JournalStorageStatus::IoFailure,
          "directory cannot masquerade as a journal file");
    std::filesystem::remove_all(directory);
    std::printf("[GAME_HISTORY_STORAGE_PROBE] failures=%d\n",failures);
    return failures==0 ? 0 : 1;
}
