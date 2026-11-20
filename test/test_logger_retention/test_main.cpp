#include <cstdio>
#include <cstring>

#include "foxie_embedded/fixed_string.hpp"
#include "logger.hpp"

int TestArduinoLoggerAllocation();

namespace {

int g_failures = 0;

void Check(const bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::printf("[FAIL] %s\n", message);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[LOGGER_RETENTION] start");
    g_failures += TestArduinoLoggerAllocation();
    logger::SetMinLevel(logger::Level::INFO);
    logger::SetOutputLineCallback([](const char*) {});
    logger::ClearBufferedLines();

    for (uint8_t i = 0U; i < 20U; ++i) {
        logger::Info("retained entry %u", static_cast<unsigned>(i));
    }

    uint8_t count = 0U;
    fxe::FixedString<256> first;
    fxe::FixedString<256> last;
    logger::DumpBufferedLines([&](const char* line) {
        if (count == 0U) {
            (void)first.Assign(line);
        }
        (void)last.Assign(line);
        ++count;
    });

    Check(count == 16U, "logger retains sixteen newest lines");
    Check(std::strstr(first.CStr(), "retained entry 4") != nullptr,
          "logger evicts the oldest lines first");
    Check(std::strstr(last.CStr(), "retained entry 19") != nullptr,
          "logger keeps the newest line");

    logger::ClearBufferedLines();
    logger::SetOutputLineCallback(nullptr);
    if (g_failures == 0) {
        std::puts("[LOGGER_RETENTION] ok");
        return 0;
    }
    std::printf("[LOGGER_RETENTION] failures=%d\n", g_failures);
    return 1;
}
