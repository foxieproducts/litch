#pragma once

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace fxe::test_support {

class TestResults final {
  public:
    explicit TestResults(const char* suiteName) : m_suiteName(suiteName) {}

    void Start() const {
        if (!m_suiteName) {
            return;
        }

        std::printf("[%s] start\n", m_suiteName);
    }

    void Check(const bool cond, const char* msg) {
        if (cond) {
            return;
        }

        std::printf("[FAIL] %s\n", msg);
        ++m_failures;
    }

    void CheckEqInt(const int got, const int want, const char* msg) {
        if (got == want) {
            return;
        }

        std::printf("[FAIL] %s (got=%d want=%d)\n", msg, got, want);
        ++m_failures;
    }

    void CheckEqU32(const uint32_t got, const uint32_t want, const char* msg) {
        if (got == want) {
            return;
        }

        std::printf("[FAIL] %s (got=%u want=%u)\n", msg, got, want);
        ++m_failures;
    }

    void CheckEqStr(const std::string_view got, const std::string_view want, const char* msg) {
        if (got == want) {
            return;
        }

        std::printf("[FAIL] %s (got='%.*s' want='%.*s')\n",
                    msg,
                    static_cast<int>(got.size()),
                    got.data(),
                    static_cast<int>(want.size()),
                    want.data());
        ++m_failures;
    }

    int Failures() const { return m_failures; }

    int Finish() const {
        if (!m_suiteName) {
            return m_failures == 0 ? 0 : 1;
        }

        if (m_failures == 0) {
            std::printf("[%s] ok\n", m_suiteName);
            return 0;
        }

        std::printf("[%s] failures=%d\n", m_suiteName, m_failures);
        return 1;
    }

  private:
    const char* m_suiteName = nullptr;
    int m_failures = 0;
};

} // namespace fxe::test_support
