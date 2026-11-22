# Pin map

V3.3 and V3.31 launch boards use the same ESP32-S3 GPIO profile. The earlier
V3, V2 and V1 columns describe retained firmware profiles for earlier hardware;
the V1 C3 prototype wiring has not been reverified. GPIO numbers refer to the
ESP module. A dash means the profile does not use that signal.

## Physical controls

Each fitted bank has six active-low inputs with internal pull-ups. The columns
give physical positions as seen by the player seated at that end. Gameplay
orientation can swap which player uses each bank. GPIO assignments are fixed.
[Runtime board profiles](../../include/hardware/board_hardware_profile.hpp) · [Button map](../../config/hardware/controls.yaml)

| Physical signal | V3.3 / V3.31 | V3 | V2 | V1 (C3) |
| --- | ---: | ---: | ---: | ---: |
| P1 Up | 7 | 7 | 7 | 10 |
| P1 Left | 5 | 5 | 5 | 4 |
| P1 Down | 6 | 6 | 6 | 7 |
| P1 Right | 9 | 9 | 9 | 9 |
| P1 X | 0 | 0 | 0 | 21 |
| P1 O | 10 | 10 | 10 | 20 |
| P2 Up | 15 | 14 | 17 | — |
| P2 Left | 13 | 17 | 15 | — |
| P2 Down | 14 | 15 | 16 | — |
| P2 Right | 17 | 13 | 21 | — |
| P2 X | 12 | 12 | 14 | — |
| P2 O | 18 | 18 | 26 | — |

The launch PCB uses individual directional switches. Earlier center contacts
(V1 P1 GPIO3, V2 P1/P2 GPIO8/18, V3 P1/P2 GPIO8/16) are retired. Firmware
scans four directions, X and O on each fitted bank. GPIO18 is **P2 O** on the
launch PCB. [Control scanning](../../src/gpio_user_input.cpp) · [Launch board wiring](components/README.md)

## LEDs, buses and power

| Signal or group | V3.3 / V3.31 | V3 | V2 | V1 (C3) | Function |
| --- | --- | --- | --- | --- | --- |
| Board LED data | 47 | 47 | 47 | 2 | Output to 256 board LEDs on launch and V3, 192 on V2 and V1. |
| Status LED data | 33 | 33 | 33 | — | Output to 44 launch LEDs (32 edge + 12 controls), 32 on V3/V2. V1 projects status onto its board perimeter. |
| `5V_EN` | 11 | 11 | 13 | — | Active-high 5 V rail enable. On launch boards it switches LED power. The Hall sensors and expanders use 3.3 V. |
| I²C SDA / SCL | 3 / 4 | 3 / 4 | 3 / 4 | 5 / 6 | Shared open-drain bus, 100 kHz by default. V1 firmware takes these GPIOs from the project Arduino variant. |
| PCA9555 INT, addresses 0x20 / 0x21 / 0x22 / 0x23 | 34 / 38 / 48 / 39 | 34 / 38 / 48 / 39 | 34 / 12 / 48 / 11 | — | Active-low, pulled up. A falling edge schedules a complete Hall scan. V1 polls. |
| USB-C CC1 / CC2 | 1 / 2 | 1 / 2 | 1 / 2 | — | Analog inputs for source-current detection. |
| LIS2DW12 INT1 | 21 | 21 | — | — | Optional active-low motion wake. INT2 is unconnected. |
| Revision straps, weights 1 / 2 / 4 | 35 / 36 / 37 | 35 / 36 / 37 | 35 / 36 / 37 | — | Sampled as inputs with pull-ups. Fitted resistors pull their inputs low. |

S3 strap values 3, 1 and 0 select V3.3/V3.31, V3 and V2 respectively. Other
values report an unknown revision and use the V2 pin map. S3 LED data lines
are held low during rail sequencing. [Revision decoding](../../include/hardware/board_revision.hpp) · [LED topology](../../include/hardware/board_led_output_mapping.hpp) · [Rail sequencing](../../src/power_rail.cpp) · [Launch regulator and loads](components/README.md)

The C3 firmware calls `Wire.begin()`. Its Arduino variant assigns SDA/SCL to
GPIO5/6. Old zero-valued placeholders for missing peripherals and the unused
5 V-enable constant do not describe connected signals.
[C3 variant](../../variants/litch_esp32c3/pins_arduino.h) · [Bus and interrupt setup](../../src/firmware_app_boot_services.cpp) · [Hall wiring](sensor_topology.md)

## Wake and cold-boot controls

| Revision | Button wake sources |
| --- | --- |
| V3.3 / V3.31 and V3 | All eight directions, P1 O, P2 X and P2 O use active-low EXT1. P1 X uses the ULP to wake on release. GPIO21 can also wake on motion when armed. |
| V2 | All eight directions use active-low EXT1. P1 X also wakes on release through the ULP. |
| V1 (C3) | The four P1 directions form the wake set. Firmware uses light sleep if those pins cannot all support deep-sleep wake. |

On S3 the ULP watches GPIO0 for a press and five consecutive released samples
(200 ms). Holding P1 X across cold power-on selects the ROM downloader.
After wake, normal input waits for the configured wake buttons to stay released
for 200 ms. [Wake pin selection](../../include/firmware/sleep_wake_pin_policy.hpp) · [ULP wake](../../src/p1x_deep_wake.cpp) · [Sleep entry and input guard](../../src/sleep_controller_enter_sleep.cpp)

For a user factory reset, hold Down on exactly one physical bank at cold
power-on. When the amber prompt appears, release Down and all face buttons.
Freshly hold X and O together on the bank that entered the prompt for two
seconds, within the ten-second window. On launch boards the X/O LEDs on both
banks blink amber, then flash green twice after the settings are erased.
Earlier boards use home-edge markers. [Cold reset](../../src/cold_factory_reset.cpp) · [Recovery policy](../../src/recovery_policy.cpp) · [Reset cues](../../src/recovery_led_frame.cpp)

For launch factory inspection, hold either physical Up during cold boot.
Holding X and O on one bank for five seconds during an unfinished inspection
requests an Override. The station reports a PASS only after a verified test
result has been saved. [Factory procedure](../release/factory_programming.md) · [Override gesture](../../src/hardware_test_mode.cpp)
