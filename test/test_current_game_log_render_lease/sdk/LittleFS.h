#pragma once
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace batch_sdk {
inline std::filesystem::path root;
inline void (*access)(const char *) = nullptr;
inline std::map<std::string, unsigned> counts;
inline unsigned unleased = 0, calls = 0;
inline std::vector<uint8_t> bytes;
inline bool shortRead = false, failLogRename = false;
inline bool staleSize = false;
inline unsigned long long readBytes = 0, writeBytes = 0;
// Optional controlled stdio accumulation model. Existing worker tests leave it
// disabled; the Arduino save regression uses it to observe submitted bursts.
inline size_t stdioBufferCapacity = 0;
inline void (*programBytes)(size_t) = nullptr;
inline void Touch(const char *k) {
    ++calls;
    ++counts[k];
    if (access)
        access(k);
}
inline std::string Path(const char *p) {
    std::string s = p;
    if (s.rfind("/littlefs/", 0) == 0)
        s = s.substr(10);
    else if (s.rfind("/", 0) == 0 && s.rfind(root.string(), 0) != 0)
        s = s.substr(1);
    if (s.rfind(root.string(), 0) == 0)
        return s;
    return (root / s).string();
}
} // namespace batch_sdk
class Print {
  public:
    virtual ~Print() = default;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *, size_t) = 0;
};
constexpr int SeekSet = 0;
class File : public Print {
    struct State {
        FILE *f = nullptr;
        size_t bufferedBytes = 0;
        ~State() {
            if (f)
                std::fclose(f);
        }
    };
    std::shared_ptr<State> s;

  public:
    File() = default;
    explicit File(FILE *f) : s(std::make_shared<State>()) { s->f = f; }
    explicit operator bool() const { return s && s->f; }
    size_t size() {
        batch_sdk::Touch("size");
        if (batch_sdk::staleSize) { errno = ENOMEM; return 0U; }
        auto p = std::ftell(s->f);
        std::fseek(s->f, 0, SEEK_END);
        auto n = std::ftell(s->f);
        std::fseek(s->f, p, SEEK_SET);
        return n;
    }
    size_t position() { return static_cast<size_t>(std::ftell(s->f)); }
    bool seek(size_t p, int) {
        batch_sdk::Touch("seek");
        return std::fseek(s->f, p, SEEK_SET) == 0;
    }
    size_t read(uint8_t *out, size_t n) {
        batch_sdk::Touch("read");
        if (batch_sdk::shortRead && n) {
            batch_sdk::shortRead = false;
            --n;
        }
        auto got = std::fread(out, 1, n, s->f);
        batch_sdk::readBytes += got;
        return got;
    }
    size_t write(uint8_t v) override { return write(&v, 1); }
    size_t write(const uint8_t *b, size_t n) override {
        batch_sdk::Touch("write");
        auto wrote = std::fwrite(b, 1, n, s->f);
        batch_sdk::writeBytes += wrote;
        if (batch_sdk::programBytes && batch_sdk::stdioBufferCapacity) {
            s->bufferedBytes += wrote;
            while (s->bufferedBytes >= batch_sdk::stdioBufferCapacity) {
                batch_sdk::programBytes(batch_sdk::stdioBufferCapacity);
                s->bufferedBytes -= batch_sdk::stdioBufferCapacity;
            }
        }
        return wrote;
    }
    void flush() {
        batch_sdk::Touch("flush");
        if (batch_sdk::programBytes && s->bufferedBytes) {
            batch_sdk::programBytes(s->bufferedBytes);
            s->bufferedBytes = 0;
        }
        std::fflush(s->f);
    }
    void close() {
        batch_sdk::Touch("close");
        if (*this) {
            if (batch_sdk::programBytes && s->bufferedBytes) {
                batch_sdk::programBytes(s->bufferedBytes);
                s->bufferedBytes = 0;
            }
            std::fclose(s->f);
            s->f = nullptr;
        }
    }
};
struct BatchLittleFS {
    File open(const char *p, const char *mode) {
        batch_sdk::Touch("open");
        return File(std::fopen(batch_sdk::Path(p).c_str(), mode));
    }
    bool exists(const char *p) {
        batch_sdk::Touch("exists");
        return std::filesystem::exists(batch_sdk::Path(p));
    }
    bool remove(const char *p) {
        batch_sdk::Touch("remove");
        return std::remove(batch_sdk::Path(p).c_str()) == 0;
    }
    bool rename(const char *a, const char *b) {
        batch_sdk::Touch("rename");
        if (batch_sdk::failLogRename && std::string(a).find("current_game") != std::string::npos) {
            batch_sdk::failLogRename = false;
            return false;
        }
        return std::rename(batch_sdk::Path(a).c_str(), batch_sdk::Path(b).c_str()) == 0;
    }
};
inline BatchLittleFS LittleFS;
