#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "generated/all_native_registry.hpp"

#include "foxie_embedded/millis.hpp"

// Aggregated native test runner.
//
// This links all existing PlatformIO native test suites into a single executable to
// avoid rebuilding the entire project once per suite.
//
// The per-suite wrapper translation units and this registry header are generated at
// build time by: tools/pio/generate_test_all_native.py

#define DECLARE_TEST_MAIN(suite) int suite##_main();
ALL_NATIVE_TEST_SUITES(DECLARE_TEST_MAIN)
#undef DECLARE_TEST_MAIN

static void PrintSuites() {
    std::puts("[ALL_NATIVE] suites:");

#define PRINT_SUITE(suite) std::printf("[ALL_NATIVE]  - %s\n", #suite);
    ALL_NATIVE_TEST_SUITES(PRINT_SUITE)
#undef PRINT_SUITE
}

static unsigned g_runCount = 0U;

static int RunOne(const char* name, int (*fn)()) {
    const char* filter = std::getenv("LITCH_NATIVE_TEST_FILTER");
    if (filter != nullptr && filter[0] != '\0' && std::strcmp(filter, name) != 0) {
        return 0;
    }
    ++g_runCount;
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    std::printf("[ALL_NATIVE] begin %s\n", name);
    std::fflush(stdout);
    const auto started = std::chrono::steady_clock::now();
    const int rc = fn();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("[ALL_NATIVE] suite %s elapsed_ms=%lld rc=%d\n",
                name, static_cast<long long>(elapsedMs), rc);
    std::fflush(stdout);
    if (rc != 0) {
        std::printf("[ALL_NATIVE] FAIL: %s (rc=%d)\n", name, rc);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    // Keep the last completed checkpoint visible if a suite stalls or exits.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--list") == 0) {
            PrintSuites();
            return 0;
        }
    }
    std::puts("[ALL_NATIVE] start");
    const char* filter = std::getenv("LITCH_NATIVE_TEST_FILTER");
    if (filter == nullptr || filter[0] == '\0') PrintSuites();

    // Default the global millis source to a deterministic clock for native tests.
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    int failures = 0;

#define RUN_TEST_SUITE(suite) failures += RunOne(#suite, &suite##_main);
    ALL_NATIVE_TEST_SUITES(RUN_TEST_SUITE)
#undef RUN_TEST_SUITE

    if (g_runCount == 0U) {
        std::puts("[ALL_NATIVE] no suites matched LITCH_NATIVE_TEST_FILTER");
        return 1;
    }
    if (failures == 0) {
        std::puts("[ALL_NATIVE] ok");
        return 0;
    }

    std::printf("[ALL_NATIVE] failures=%d\n", failures);
    return 1;
}
