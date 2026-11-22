#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

// Controlled SDK/filesystem boundary for the production explicit formatter.
// Ordinary SettingsStore writes still use its real native filesystem backend.
class Print {
  public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t byte) = 0;
    virtual size_t write(const uint8_t* bytes, size_t size) = 0;
};
namespace reformat_sdk {
enum class Fault { None, Format, Mount, Write, Read, Corrupt, Open };
inline Fault fault = Fault::None;
inline std::filesystem::path root;
inline uint32_t formatCalls = 0U;
inline uint32_t unmounts = 0U;
inline uint32_t mounts = 0U;
inline uint32_t unleased = 0U;
inline std::string label;
inline int* lease = nullptr;
inline void Access() { if (lease != nullptr && *lease != 1) ++unleased; }
inline void Reset(const std::filesystem::path& target, Fault targetFault) {
    root = target;
    fault = targetFault;
    formatCalls = unmounts = mounts = unleased = 0U;
    label.clear();
    lease = nullptr;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        std::filesystem::remove_all(entry.path());
    }
}
}
class File {
    struct State {
        std::FILE* file = nullptr;
        ~State() { if (file != nullptr) std::fclose(file); }
    };
    std::shared_ptr<State> m_state;
  public:
    File() = default;
    explicit File(std::FILE* file) : m_state(std::make_shared<State>()) { m_state->file = file; }
    explicit operator bool() const { return m_state && m_state->file; }
    size_t size() {
        reformat_sdk::Access();
        std::fseek(m_state->file, 0, SEEK_END);
        const long size = std::ftell(m_state->file);
        std::rewind(m_state->file);
        return size < 0 ? 0U : static_cast<size_t>(size);
    }
    size_t read(uint8_t* out, size_t requested) {
        reformat_sdk::Access();
        if (reformat_sdk::fault == reformat_sdk::Fault::Read) return 0U;
        const size_t read = std::fread(out, 1U, requested, m_state->file);
        if (read != 0U && reformat_sdk::fault == reformat_sdk::Fault::Corrupt) out[0] ^= 1U;
        return read;
    }
    void close() {
        reformat_sdk::Access();
        if (*this) { std::fclose(m_state->file); m_state->file = nullptr; }
    }
};
struct RecoveryLittleFS {
    void end() { reformat_sdk::Access(); ++reformat_sdk::unmounts; }
    bool begin(bool formatOnFail, const char* path, uint8_t maximumOpen, const char* label) {
        reformat_sdk::Access();
        ++reformat_sdk::mounts;
        return !formatOnFail && std::string(path) == "/littlefs" && maximumOpen == 10U &&
            std::string(label) == "spiffs" && reformat_sdk::fault != reformat_sdk::Fault::Mount;
    }
    File open(const char* path, const char*) {
        reformat_sdk::Access();
        if (reformat_sdk::fault == reformat_sdk::Fault::Open) return {};
        return File(std::fopen(path, "rb"));
    }
};
inline RecoveryLittleFS LittleFS;
inline void disableCore0WDT() {}
inline void enableCore0WDT() {}
