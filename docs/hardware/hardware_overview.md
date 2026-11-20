# Litch hardware

Litch launches on ESP32-S3 V3.3 hardware. The V3.31 production PCB uses the
same firmware profile. The hardware contract, manufacturing files and component
information are in the [hardware specification](../spec/hardware_platform.md),
[PCB package](../../pcb/README.md) and [component index](components/README.md).

## Target Variants

| Hardware | Role | Strap value / revision ID | Firmware LED configuration |
| --- | --- | --- | --- |
| V3.3 S3, including V3.31 PCB | Launch hardware | 3 / 4 | 256 RGB board + 32 perimeter + 12 button |
| V3.0 S3 | Earlier S3 prototype | 1 / 3 | 256 RGBW board + 32 perimeter |
| V2 S3 | Earlier S3 prototype | 0 / 2 | 192 RGBW board + 32 perimeter |
| V1 C3 | First prototype | Fixed / 1 | 192 RGB board, with 32 virtual perimeter positions |

Production functional testing covers V3.3 and newer supported hardware. V3.3
is the currently implemented launch profile. The firmware also retains hardware
test support for the known V2 and V3.0 S3 profiles. An unknown strap combination
uses V2 pin and LED routing for compatibility and remains ineligible for a
factory result. On V3.31, fitting R3 and R4 pulls GPIO35 and GPIO36 low.
Leaving R5 unpopulated keeps GPIO37 high. For V1 service work, check the
prototype against the wiring notes in the [pin map](pin_map.md).

## Hardware Summary

- MCU / board profiles:
  - S3 build uses `env:litch-esp32s3`, `board = litch_esp32s3`, `default_envs = litch-esp32s3`, and build flags `LITCH_ESP32S3`, `LITCH_BLE`, `LITCH_HW_VARIANT_S3`, `LITCH_EDGE_LED_COUNT=32`. Its board manifest enables TinyUSB CDC on boot in USB mode `0`.
  - C3 build uses `env:litch-esp32c3`, `board = litch_esp32c3`, and `LITCH_EDGE_LED_COUNT=0`.
- On V3.3 the board chain has four LEDs per square in left, top, right, bottom
  order on GPIO47. The second chain on GPIO33 has 44 LEDs. Its zero-based
  positions 4–9 light the P2 controls and 26–31 light P1. Both banks run O,
  Up, Right, Down, Left, X in the local player's orientation. The other 32
  positions form the perimeter. Both chains use SK6812SIDE-A, GRB byte order
  and a 300 µs reset. The older S3 RGBW chains use GRBW and an 80 µs reset.
- Occupancy sensing:
  - Both variants use `4` PCA9555 expanders at `0x20..0x23` for `64` hall channels.
  - S3 supports dedicated expander interrupt lines and uses interrupt-driven reads after boot.
  - C3 polls the expanders; no dedicated interrupt lines are present in the current contract.
- Human controls:
  - All buttons are active-low with internal pull-ups.
  - Each V3.3 bank has four directional switches and X/O. Earlier S3 d-pad
    center contacts are unused. C3 has a single P1 control bank.
  - These names follow the printed control banks. Gameplay may normalize a
    bank to the opposite logical game player after player-end placement or
    orientation changes.
- Power / sensing:
  - S3 has a GPIO-controlled `5V_EN`, dual ambient light sensors, USB-C CC analog sensing, and LED power budgeting.
  - V3.3 uses GPIO11 with hardware LEDC precharge and routes the LIS2DW12
    motion interrupt to GPIO21. V3.31 Hall sensors stay powered from +3.3V when
    the 5V LED rail is off. V3.0 also uses GPIO11 and GPIO21. V2 enables
    the rail on GPIO13. The earlier S3 profiles use timed GPIO precharge.
  - C3 has none of those extra subsystems in the live firmware contract.
- Storage / update:
  - Settings use LittleFS mounted from a partition labeled `spiffs` at `/littlefs`.
  - S3 uses an explicit `8 MB` partition layout with equal `3264 KiB` `ota_0`/`ota_1` slots and a `1600 KiB` `spiffs` partition.
  - OTA in the current firmware is BLE OTA using the Arduino `Update` API, not WiFi OTA.
- USB identity / recovery:
  - Normal S3 firmware enumerates as product `Litch`, manufacturer `Foxie Products`, using the shared TinyUSB CDC identity `0x303A:0x4001`.
  - The S3 ROM download mode is chip-owned and remains `0x303A:0x1001`; application firmware cannot rename it.
  - No USB mass-storage recovery application is injected by the board profile. Dead-app recovery uses the S3 ROM download mode.
  - All supported S3 revisions, including the V3.31 production board, use the same USB flashing procedure and exit download mode automatically. Ordinary `./litch flash` uses standard esptool stub/compression, transfer checks, retries and reset. It replaces OTA0 and writes explicit VALID selectors, preserving the installed table, NVS, filesystem and optional `uf2` image. Current and historical installed maps share these locations. For images larger than 2 MiB, the CLI first reads the installed slot capacity. The USB transfer can replace the rollback image. After reboot, use the board’s startup display or the CLI to verify the running firmware.
  - Factory provisioning erases the selected S3 and installs the current partition
    layout. A later user factory reset preserves the NVS hardware-test marker.
  - Without a factory application, an invalid or boot-looping application may require physical GPIO0/ROM recovery; late application boot loops are not guaranteed to trigger OTA rollback in the current Arduino core.
  - The C3 has only the fixed USB Serial/JTAG peripheral, so its USB product descriptor is not firmware-customizable.
- Runtime assumptions that matter to an ESP-IDF port:
  - The saved `I2C_CLOCK_HZ` sets the bus clock. It defaults to 100 kHz.
  - S3 code assumes a dual-core chip and pins the async think task to Core 0 while the main Arduino loop runs on Core 1.
  - C3 path is treated as single-core/cooperative.

## Primary Source Files

- [platformio.ini](../../platformio.ini)
- [boards/litch_esp32s3.json](../../boards/litch_esp32s3.json)
- [boards/litch_esp32c3.json](../../boards/litch_esp32c3.json)
- [variants/litch_esp32s3/pins_arduino.h](../../variants/litch_esp32s3/pins_arduino.h)
- [variants/litch_esp32c3/pins_arduino.h](../../variants/litch_esp32c3/pins_arduino.h)
- [tools/partitions/partitions-8MB-ota-spiffs.csv](../../tools/partitions/partitions-8MB-ota-spiffs.csv)
- [include/hardware/pins_esp32s3.hpp](../../include/hardware/pins_esp32s3.hpp)
- [include/hardware/pins_esp32c3.hpp](../../include/hardware/pins_esp32c3.hpp)
- [Runtime revision detection](../../include/hardware/board_revision.hpp)
- [Revision-specific pin profiles](../../include/hardware/board_hardware_profile.hpp)
- [Physical LED mapping](../../include/hardware/board_led_output_mapping.hpp)
- [Perimeter and button chain indices](../../include/hardware/status_led_chain_profile.hpp)
- [include/hardware/led_layout.hpp](../../include/hardware/led_layout.hpp)
- [include/hardware/led_mapping.hpp](../../include/hardware/led_mapping.hpp)
- [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp)
- [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp)
- [src/pca9555_board_input.cpp](../../src/pca9555_board_input.cpp)
- [src/gpio_user_input.cpp](../../src/gpio_user_input.cpp)
- [src/firmware_app_boot.cpp](../../src/firmware_app_boot.cpp)
- [src/power_rail.cpp](../../src/power_rail.cpp)
- [src/ambient_light_sensors.cpp](../../src/ambient_light_sensors.cpp)
- [src/usb_cc_sense.cpp](../../src/usb_cc_sense.cpp)
- [src/firmware_led_power_budget.cpp](../../src/firmware_led_power_budget.cpp)
- [src/async_think_worker.cpp](../../src/async_think_worker.cpp)
- [src/boot_guard.cpp](../../src/boot_guard.cpp)
- [lib/foxie_embedded/src/settings_store_io.cpp](../../lib/foxie_embedded/src/settings_store_io.cpp)
- [src/ble_service_stack.cpp](../../src/ble_service_stack.cpp)
- [src/ble_service_ota.cpp](../../src/ble_service_ota.cpp)
- [test/test_pca9555_occupancy_decode/test_main.cpp](../../test/test_pca9555_occupancy_decode/test_main.cpp)
- [test/test_led_layout/test_main.cpp](../../test/test_led_layout/test_main.cpp)
