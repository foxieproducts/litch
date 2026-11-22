#include <cstdio>

#include "generated/all_native_registry.hpp"

#include "foxie_embedded/millis.hpp"

// Aggregated foxie_embedded native test runner.
//
// The per-suite wrapper translation units and this registry header are generated at
// build time by: tools/pio/generate_test_all_native_foxie.py

#define DECLARE_TEST_MAIN(suite) int suite##_main();
ALL_NATIVE_TEST_SUITES(DECLARE_TEST_MAIN)
#undef DECLARE_TEST_MAIN

static void PrintSuites() {
    std::puts("[FOXIE_ALL_NATIVE] suites:");

#define PRINT_SUITE(suite) std::printf("[FOXIE_ALL_NATIVE]  - %s\n", #suite);
    ALL_NATIVE_TEST_SUITES(PRINT_SUITE)
#undef PRINT_SUITE
}

static int RunOne(const char* name, int (*fn)()) {
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    const int rc = fn();
    if (rc != 0) {
        std::printf("[FOXIE_ALL_NATIVE] FAIL: %s (rc=%d)\n", name, rc);
        return 1;
    }
    return 0;
}

int main() {
    std::puts("[FOXIE_ALL_NATIVE] start");
    PrintSuites();

    // Default the global millis source to a deterministic clock for native tests.
    fxe::SetMillisProviderForTests(&fxe::ManualMillisProvider);
    fxe::ManualMillis::SetNowMs(0);

    int failures = 0;

#define RUN_TEST_SUITE(suite) failures += RunOne(#suite, &suite##_main);
    ALL_NATIVE_TEST_SUITES(RUN_TEST_SUITE)
#undef RUN_TEST_SUITE

    if (failures == 0) {
        std::puts("[FOXIE_ALL_NATIVE] ok");
        return 0;
    }

    std::printf("[FOXIE_ALL_NATIVE] failures=%d\n", failures);
    return 1;
}
