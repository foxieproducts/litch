#pragma once
namespace firmware_internal {
struct FirmwareSystem;
// Bind before BLE starts, including degraded boot. Does not load/save or unlock
// product authority; the session adapter enforces private/public access.
void ConfigureSupportDiagnostics(FirmwareSystem& system);
}
