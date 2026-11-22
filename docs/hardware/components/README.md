# V3.31 components

The [schematic, bill of materials and placement files](../../../pcb/README.md) give the complete production orderables and footprints. These are the active components used in the launch board.

| Component | LCSC | V3.31 use |
| --- | --- | --- |
| VEML6030 | C132182 | U4/U5 ambient-light sensors |
| LIS2DW12TR | C189624 | U1 accelerometer |
| TMAG5231H1DQDBZR | C20345953 | U12–U75 Hall sensors |
| SN74AHCT1G125DBVR(UMW) | C20617903 | U6/U7 LED-data buffers |
| TPAP2112K-3.3TRG1 | C3021085 | U3 3.3 V regulator |
| USBLC6-2SC6 | C2687116 | D1 CC / D2 USB-data protection |
| ESP32-S3-MINI-1-N8 | C2913206 | U2 MCU/radio module |
| SC015P2G | C41367295 | Q1 switched LED rail |
| PCA9555PW(UMW) | C42420607 | U8–U11 GPIO expanders |
| SK6812SIDE-A | C5378721 | LED1–LED300 RGB LEDs |
| TS-1088-AR02016 | C720477 | SW1–SW12 controls |
| 2N7002 | C8545 | Q2 high-side gate drive |

The 64 Hall switches are on the 3.3 V rail. They remain powered when the LED rail is off. The two ambient light sensors share the I²C bus at distinct addresses. See the [bus map](../bus_devices.md) and [sensor map](../sensor_topology.md).
