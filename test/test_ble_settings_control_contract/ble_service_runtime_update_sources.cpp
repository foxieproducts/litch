#include "ble_service_test_config.hpp"
#include "modules/ble_service.hpp"

#include <NimBLEDevice.h>
namespace ota_runtime_test {
struct RestartRequested {};
unsigned shutdowns = 0U;
unsigned restarts = 0U;
struct Platform {
    void restart() { ++restarts; throw RestartRequested{}; }
} platform;
}
#define ESP ota_runtime_test::platform
#include "../../src/ble_service_runtime_update.cpp"
#undef ESP

namespace modules {
// Only factory-radio hardware is outside this updater runtime fixture.
bool BleService::QuiesceFactoryBleDut() { return true; }
bool BleService::ConfigureFactoryRadioEndpoint(const SettingsStore&) { return false; }
void BleService::ReleaseFactoryRadioEndpoint() {}
} // namespace modules

#include "../../src/ble_service_stack.cpp"

namespace {
const bool teardownObserver = [] {
    NimBLEDevice::SetDeinitHook([] { ++ota_runtime_test::shutdowns; });
    return true;
}();
}
