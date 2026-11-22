# Bus And Device Inventory

This file records the hardware-visible devices that the current firmware depends on, along with the bring-up details that matter to a new low-level port.

## ESP32-S3 V3.3 and V3.31 launch boards

V3.31 uses the V3.3 runtime profile. The shared I²C bus has SDA on GPIO 3 and
SCL on GPIO 4. The 3.3 V domain supplies the Hall sensors, expanders, two
ambient-light sensors and accelerometer. The switched 5 V rail supplies the
LEDs. [Production components](components/README.md)

| Device | Bus / Type | Address Or Pins | Purpose | Init Notes | Source File(s) | Confidence |
| --- | --- | --- | --- | --- | --- | --- |
| `onboard_flash` | integrated flash | [tools/partitions/partitions-8MB-ota-spiffs.csv](../../tools/partitions/partitions-8MB-ota-spiffs.csv) | Firmware images, OTA slots, settings filesystem | Current repo uses an explicit `8 MB` partition CSV with equal `3264 KiB` OTA slots and a `1600 KiB` settings filesystem. | [platformio.ini](../../platformio.ini), [tools/partitions/partitions-8MB-ota-spiffs.csv](../../tools/partitions/partitions-8MB-ota-spiffs.csv) | `confirmed` |
| `littlefs_settings_partition` | filesystem partition | label `spiffs`, mount `/littlefs` | Persistent settings storage | Mounted with LittleFS at `/littlefs`, max open files `10`. Auto-format is conservative, not unconditional. | [lib/foxie_embedded/src/settings_store_io.cpp](../../lib/foxie_embedded/src/settings_store_io.cpp) | `confirmed` |
| `board_led_strip` | NeoPixel / RMT0 | GPIO `47`, 256 pixels | Four LEDs per square | SK6812SIDE-A, 24-bit GRB, 300 µs reset. | [Topology](led_topology.md), [renderer](../../include/hardware/neopixel_led_renderer.hpp) | `confirmed` |
| `edge_led_strip` | NeoPixel / RMT1 | GPIO `33`, 44 pixels | 32 perimeter positions and 12 button LEDs | Uses the same SK6812SIDE-A GRB transport as the board chain. | [Status chain](../../include/hardware/status_led_chain_profile.hpp), [renderer](../../include/hardware/neopixel_led_renderer.hpp) | `confirmed` |
| TMAG5231 Hall switches | PCA9555 inputs | 64, one per square | Piece occupancy | Powered from 3.3 V independently of the LED supply. | [Production Hall array](components/README.md), [square mapping](../../include/hardware/pca9555_occupancy_decode.hpp) | `confirmed` |
| `pca9555[0]` | I2C expander | `0x20`, IRQ GPIO `34` | Hall-sensor bank 0 | All ports configured as input with polarity inversion. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `pca9555[1]` | I2C expander | `0x21`, IRQ GPIO `38` | Hall-sensor bank 1 | Same as above. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `pca9555[2]` | I2C expander | `0x22`, IRQ GPIO `48` | Hall-sensor bank 2 | Same as above. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `pca9555[3]` | I2C expander | `0x23`, IRQ GPIO `39` | Hall-sensor bank 3 | Same as above. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `veml6030[0]` (U5) | I²C ambient sensor | `0x10`, beside H8 | Ambient brightness and corner light-response test | 50 ms integration, 0.25× gain. | [Ambient-light driver](../../src/ambient_light_sensors.cpp), [PCB wiring](components/README.md) | `confirmed` |
| `veml6030[1]` (U4) | I²C ambient sensor | `0x48`, beside A1 | Ambient brightness and corner light-response test | 50 ms integration, 0.25× gain. | [Ambient-light driver](../../src/ambient_light_sensors.cpp), [PCB wiring](components/README.md) | `confirmed` |
| LIS2DW12 (U1) | I²C accelerometer | `0x19`, INT1 on GPIO `21`, INT2 unconnected | Motion sensing, motion/tap wake, factory sensor check | Firmware reads WHO_AM_I (`0x44`) and enables the interrupt through the revision profile. | [Accelerometer driver](../../src/lis2dw12.cpp), [PCB wiring](components/README.md) | `confirmed` |
| `usb_cc_source_detect` | ADC inputs | GPIO `1` + GPIO `2` | Detect advertised USB-C current source | A bounded `1 Hz` confirmation primes each channel, then takes an `8`-sample trimmed mean with `2 ms` spacing. After authorization, only the active pin gets a lightweight three-sample median every `10 ms`; a downgrade must persist for `20 ms`. Default reporting thresholds are `320`, `580`, `850`; normal-power entry requires `620` raw and exactly one active pin. | [include/hardware/pins_esp32s3.hpp](../../include/hardware/pins_esp32s3.hpp), [src/usb_cc_sense.cpp](../../src/usb_cc_sense.cpp) | `confirmed` |
| `five_volt_rail_gate` | GPIO-controlled power switch | GPIO `11` | Switch the LED 5 V domain | Hold data pins low, allow 5 ms off-settle, apply the 5 ms hardware PWM ramp, then hold the rail on for 5 ms before LED output. | [Power sequencing](power_constraints.md#switched-led-rail), [rail driver](../../src/power_rail.cpp) | `confirmed` |
| `usb_cdc_serial` | TinyUSB CDC | product `Litch`, `0x303A:0x4001`, `Serial` at `115200` | Console and diagnostics | S3 custom board profile enables TinyUSB CDC on boot. ROM download mode remains the chip-owned `0x303A:0x1001`. | [boards/litch_esp32s3.json](../../boards/litch_esp32s3.json), [variants/litch_esp32s3/pins_arduino.h](../../variants/litch_esp32s3/pins_arduino.h), [src/firmware_app_boot.cpp](../../src/firmware_app_boot.cpp) | `confirmed` |
| `ble_radio` | integrated BLE radio | device name default `Foxie Litch` | Console, settings, and BLE OTA transport | Current S3 build enables BLE, sets preferred MTU `517`, TX power `9 dBm`, fast connection interval requests of `6` or `12` units, and OTA chunk size `512` bytes. | [platformio.ini](../../platformio.ini), [include/modules/ble_service_internal.hpp](../../include/modules/ble_service_internal.hpp), [src/ble_service_stack.cpp](../../src/ble_service_stack.cpp), [src/ble_service_ota.cpp](../../src/ble_service_ota.cpp) | `confirmed` |

## Earlier ESP32-S3 boards

V2 and V3 retain ordinary firmware support. Both use RGBW emitters in GRBW
order, with 32 physical perimeter LEDs. V2 has 192 board LEDs, GPIO 13 for
`5V_EN` and no accelerometer. V3 has 256 board LEDs, GPIO 11 for `5V_EN`,
and the LIS2DW12 on GPIO 21. Both use GPIO precharge and 10 ms post-enable
settling. The [pin map](pin_map.md) lists both older interrupt maps.

## ESP32-C3 V1 prototype

The table describes the V1 prototype firmware profile. The confidence column identifies wiring inferred from firmware.

| Device | Bus / Type | Address Or Pins | Purpose | Init Notes | Source File(s) | Confidence |
| --- | --- | --- | --- | --- | --- | --- |
| `onboard_flash` | integrated flash | board default layout | Firmware + settings storage | Current repo does not override the C3 partition table, so only the LittleFS mount contract is explicit here. | [platformio.ini](../../platformio.ini), [lib/foxie_embedded/src/settings_store_io.cpp](../../lib/foxie_embedded/src/settings_store_io.cpp) | `unknown` |
| `littlefs_settings_partition` | filesystem partition | label `spiffs`, mount `/littlefs` | Persistent settings storage | Same LittleFS mount code is shared with S3, but the exact C3 partition layout is not declared in this repo. | [lib/foxie_embedded/src/settings_store_io.cpp](../../lib/foxie_embedded/src/settings_store_io.cpp) | `inferred` |
| `board_led_strip` | NeoPixel | GPIO `2`, `192` logical pixels | Board square rendering | Uses RGB-only WS2812x timing with GRB channel order. | [include/hardware/pins_esp32c3.hpp](../../include/hardware/pins_esp32c3.hpp), [include/hardware/neopixel_led_renderer.hpp](../../include/hardware/neopixel_led_renderer.hpp) | `confirmed` |
| `virtual_edge_surface` | virtual LED model | backed by perimeter board LEDs | Emulated edge/status UI | The same `32`-pixel semantic status frame used by S3 is blended into collision-free perimeter square slots after board composition and orientation. | [include/modules/status_led_frame_adapter.hpp](../../include/modules/status_led_frame_adapter.hpp), [include/modules/edge_led_state_selection.hpp](../../include/modules/edge_led_state_selection.hpp) | `confirmed` |
| `optional_appended_pixels` | inferred shared-chain pixels | GPIO `2`, additional `32` clear-only pixels | Clear any physically present extra pixels on the V1 C3 prototype | Current renderer can clear appended pixels on the same chain even when logical edge count is `0`. Physical population is not formally documented. | [include/firmware/firmware_internal.hpp](../../include/firmware/firmware_internal.hpp), [src/neopixel_led_renderer.cpp](../../src/neopixel_led_renderer.cpp) | `inferred` |
| `pca9555[0]` | I2C expander | `0x20` | Hall-sensor bank 0 | Same input + inversion configuration as S3. No interrupt pins are used on C3. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `pca9555[1]` | I2C expander | `0x21` | Hall-sensor bank 1 | Same as above. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `pca9555[2]` | I2C expander | `0x22` | Hall-sensor bank 2 | Same as above. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `pca9555[3]` | I2C expander | `0x23` | Hall-sensor bank 3 | Same as above. | [include/hardware/pca9555_occupancy_decode.hpp](../../include/hardware/pca9555_occupancy_decode.hpp), [src/pca9555_input_scan.cpp](../../src/pca9555_input_scan.cpp) | `confirmed` |
| `serial_console` | board USB serial | `Serial` at `115200` | Console and diagnostics | Current firmware uses `Serial.begin(115200)`, but the exact C3 upload / monitor transport is board-profile-defined rather than explicitly configured in repo code. | [src/firmware_app_boot.cpp](../../src/firmware_app_boot.cpp), [platformio.ini](../../platformio.ini) | `inferred` |

## I²C initialization

- The default I²C clock is `100 kHz`.
- PCA9555 reads allow up to `4` attempts and escalate to an I2C bus reset after `2` consecutive failures.
- S3 enables interrupt-driven hall reads after initial bring-up; C3 stays in polling mode.
- There is no WiFi OTA contract in the current repo. OTA support is BLE-only on the S3 build.
