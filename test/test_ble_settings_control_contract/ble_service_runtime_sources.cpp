#include "ble_service_test_config.hpp"
#include "../../src/ble_service_runtime.cpp"

#include "../../src/ble_service_owner_pairing.cpp"
// Exercise the production identity reader with only checked SDK inventory and
// GAP connection leaves controlled. RF/security/flash remain outside this test.
#define LITCH_OWNER_NATIVE_TEST 1
#include "../../src/ble_service_owner_native.cpp"
#undef LITCH_OWNER_NATIVE_TEST
