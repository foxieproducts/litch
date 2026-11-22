#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

#include "foxie_embedded/fixed_function.hpp"
#include "foxie_embedded/format.hpp"
#include "foxie_embedded/millis.hpp"
#include "freertos/semphr.h"

namespace logger_arduino_fixture {
struct Semaphore { std::recursive_mutex mutex; };
using Handle = Semaphore*;
Semaphore dynamicStorage;
std::atomic<unsigned> dynamicCreates{0U}, staticCreates{0U};
Handle CreateDynamic() { ++dynamicCreates; return &dynamicStorage; }
Handle CreateStatic(Semaphore* storage) { ++staticCreates; return storage; }
BaseType_t Take(Handle handle, TickType_t) { handle->mutex.lock(); return pdTRUE; }
BaseType_t Give(Handle handle) { handle->mutex.unlock(); return pdTRUE; }
struct SerialSink {
    explicit operator bool() const { return false; }
    void printf(const char*, ...) {}
    void println(const char*) {}
} serial;
} // namespace logger_arduino_fixture

namespace arduino_logger_contract {
using fxe::FixedFunction;
using fxe::Millis;
namespace format = fxe::format;
}

// Execute the actual Arduino owner with controlled semaphore/Serial boundaries.
// This is not proof of SDK internals or of the unrelated BLE bootstrap failure.
#define ARDUINO 1
#define fxe arduino_logger_contract
#define StaticSemaphore_t logger_arduino_fixture::Semaphore
#define SemaphoreHandle_t logger_arduino_fixture::Handle
#define xSemaphoreCreateRecursiveMutex logger_arduino_fixture::CreateDynamic
#define xSemaphoreCreateRecursiveMutexStatic logger_arduino_fixture::CreateStatic
#define xSemaphoreTakeRecursive logger_arduino_fixture::Take
#define xSemaphoreGiveRecursive logger_arduino_fixture::Give
#define Serial logger_arduino_fixture::serial
#include "../../lib/foxie_embedded/src/logger.cpp"
#undef Serial
#undef xSemaphoreGiveRecursive
#undef xSemaphoreTakeRecursive
#undef xSemaphoreCreateRecursiveMutexStatic
#undef xSemaphoreCreateRecursiveMutex
#undef SemaphoreHandle_t
#undef StaticSemaphore_t
#undef fxe
#undef ARDUINO

int TestArduinoLoggerAllocation() {
    using namespace arduino_logger_contract;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::printf("[FAIL] %s\n", message); }
    };
    // First use crosses the real lazy-init owner concurrently on two tasks.
    std::thread first([] { SetMinLevel(Level::INFO); });
    std::thread second([] { SetMinLevel(Level::INFO); });
    first.join(); second.join();
    ClearBufferedLines();
    std::atomic<unsigned> delivered{0U};
    SetOutputLineCallback([&](const char*) {
        // The sink can reenter logger APIs while the recursive lock is held.
        if (GetMinLevel() == Level::INFO) ++delivered;
    });
    std::thread writer([] { for (unsigned i = 0U; i < 32U; ++i) Info("worker %u", i); });
    for (unsigned i = 0U; i < 32U; ++i) Info("loop %u", i);
    writer.join();
    unsigned retained = 0U;
    DumpBufferedLines([&](const char*) { ++retained; });
    check(delivered == 64U && retained == 16U,
          "Arduino logger serializes both tasks, permits reentry and retains sixteen lines");
    check(logger_arduino_fixture::dynamicCreates == 0U &&
              logger_arduino_fixture::staticCreates == 1U,
          "Arduino first use and later logging create one static lock and no dynamic lock");
    SetOutputLineCallback(nullptr);
    std::printf("[LOGGER_STATIC] arduino_path=1 dynamic=%u static=%u delivered=%u retained=%u failures=%d\n",
                logger_arduino_fixture::dynamicCreates.load(),
                logger_arduino_fixture::staticCreates.load(), delivered.load(), retained, failures);
    return failures;
}
