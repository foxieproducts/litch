# Sensor Topology

## Launch hardware and shared scan behavior

V3.3 is the launch firmware profile for PCB V3.31. Its 64 Hall switches and
four PCA9555 expanders use the 3.3 V supply and remain powered when the LED
5 V rail switches off. SDA uses GPIO3 and SCL uses GPIO4.
[Hall switch wiring](components/README.md) · [Expander wiring](components/README.md)

| Firmware profile | Decoder table | Interrupt GPIOs in address order, `0x20`–`0x23` |
| --- | --- | --- |
| V3.3 (launch, PCB V3.31) | S3 | 34, 38, 48, 39 |
| V3 (earlier S3) | S3 | 34, 38, 48, 39 |
| V2 (earlier S3) | S3 | 34, 12, 48, 11 |
| V1 (C3 prototype) | C3 | Polling |

Each expander supplies 16 inputs with inverted polarity. After reading a bank,
the decoder treats a set bit at `15 - pin` as occupied and maps it to a raw
square from `0=a8` to `63=h1`. The I2C bus defaults to 100 kHz. The
firmware applies the configured clock again after a bus reset.
[Decoder](../../include/hardware/pca9555_occupancy_decode.hpp) · [I2C clock settings](../../include/hardware/i2c_clock_settings.hpp)

`Pca9555InputScan` requests a full read at boot and resync. It makes up to
four attempts per expander, with one bus reset and expander reconfiguration
after two consecutive read failures. A new occupancy snapshot requires
successful configuration and complete two-byte reads from all four banks.
Changed squares from that scan belong to a single observation.
[Input scan](../../src/pca9555_input_scan.cpp) · [Board input](../../src/pca9555_board_input.cpp)

## Live Versus Dormant Debounce Behavior

- Live runtime path: `Pca9555BoardInput`, which diff-checks snapshots and does not add explicit debounce on top of sensor sampling.
- Dormant helper: `HallSensorButtonManager`, which has a `5 ms` button-style debounce, but is not wired into `FirmwareSystem` in the current build.

Any port that changes this behavior should treat it as a deliberate behavioral change, not as a harmless refactor.

## S3 expander pin to square map

Expander-pin order is the direct source of truth. The inverse square-to-device map is in [config/hardware/sensor_map.yaml](../../config/hardware/sensor_map.yaml).

```text
0x20: [0, 1, 2, 3, 11, 10, 9, 8, 16, 17, 18, 19, 27, 26, 25, 24]
0x21: [31, 30, 29, 28, 20, 21, 22, 23, 15, 14, 13, 12, 4, 5, 6, 7]
0x22: [32, 33, 34, 35, 43, 42, 41, 40, 48, 49, 50, 51, 59, 58, 57, 56]
0x23: [63, 62, 61, 60, 52, 53, 54, 55, 47, 46, 45, 44, 36, 37, 38, 39]
```

On S3, an interrupt from **any** bank requests a full scan at the next sensor
opportunity. An interrupt received during I2C transfer requests another scan.
A full scan is also due 50 ms after the previous success, even if every
interrupt line stays quiet. The ordinary runtime checks on a nominal 5 ms
cadence, changing to 10 ms during CPU thinking and 20 ms during OTA. The
minimal factory loop polls active test sensors on its own schedule and
suspends that work during OTA.
[Scan schedule](../../include/hardware/pca9555_scan_schedule.hpp) · [Ordinary runtime](../../src/firmware_app_runtime.cpp) · [Factory runtime](../../src/firmware_factory_runtime.cpp)

The PCA interrupt cannot recover a transition that both begins and reverses
between two completed snapshots (for example, an occupied capture square that
is removed and replaced too quickly to observe the empty state). The firmware
therefore treats each complete occupancy snapshot as authoritative and never
invents chronology or piece identity. Prompt interrupt scans plus the 50 ms
reconciliation scan reduce coalescing risk, but physical interactions still
need to expose each meaningful occupancy state for at least one scan.

## V1 prototype expander pin to square map

```text
0x20: [19, 18, 17, 16, 27, 26, 25, 24, 0, 1, 2, 3, 8, 9, 10, 11]
0x21: [31, 30, 29, 28, 23, 22, 21, 20, 12, 13, 14, 15, 4, 5, 6, 7]
0x22: [51, 50, 49, 48, 59, 58, 57, 56, 32, 33, 34, 35, 40, 41, 42, 43]
0x23: [63, 62, 61, 60, 55, 54, 53, 52, 44, 45, 46, 47, 36, 37, 38, 39]
```

The V1 C3 prototype polls all four banks at each sensor opportunity through
the same scan and snapshot path. Its Arduino board variant selects SDA on
GPIO5 and SCL on GPIO6 for `Wire.begin()`. Physical wiring on the prototype
has not been reverified.
[C3 board variant](../../variants/litch_esp32c3/pins_arduino.h)

## Porting Notes

- Preserve canonical raw square numbering before applying board rotation or swapping player ends.
- Keep the hall map and the LED rotation logic independent.
- The V1 C3 profile uses placeholder pin constants. Consult the prototype wiring before assigning I2C or interrupt GPIOs.
