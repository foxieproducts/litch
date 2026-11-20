#pragma once

#define LITCH_BLE 1
#define LITCH_BLE_OTA 1
#define LITCH_HW_VARIANT_S3 1
// Keep the BLE-enabled service distinct from the non-BLE native build. Missing
// production sources must fail to link instead of using a different layout.
#define BleService NativeTestBleService

// Compile the real RTC guard alongside the ordinary native no-RTC guard.
#define BootGuard NativeTestBootGuard
