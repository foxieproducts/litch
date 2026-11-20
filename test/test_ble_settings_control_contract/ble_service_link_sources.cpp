#include "ble_service_test_config.hpp"
// Replace only the platform sleep boundary; the production loop and watchdog
// ordering remain intact while native tests advance the authoritative clock.
#define DelayDiagnosticSlice DelayNotifyTestSlice
#include "../../src/ble_service_link.cpp"
