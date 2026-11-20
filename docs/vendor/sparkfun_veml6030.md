# SparkFun VEML6030 ambient light library

The ESP32 firmware uses SparkFun’s VEML6030 library 1.0.4 at commit `efde0817bd6857863067bd1653a2cfafe6c68732`. Litch records failed register reads so an I²C error can be distinguished from a valid dark reading.

## License and attribution disposition

Upstream provides no standalone license file or SPDX identifier in this tag.
The reviewed custom identifier `LicenseRef-SparkFun-VEML6030` preserves the
following exact wording together, rather than replacing it with MIT, a standard
Beerware license, or public-domain-only terms. The pinned
[README license section](https://github.com/sparkfun/SparkFun_Ambient_Light_Sensor_Arduino_Library/blob/efde0817bd6857863067bd1653a2cfafe6c68732/README.MD#license-information)
says:

```text
Various bits of the code have different licenses applied. Anything SparkFun wrote is beerware; if you see me (or any other SparkFun employee) at the local, and you've found our code helpful, please buy us a round!

Please use, reuse, and modify these files as you see fit. Please maintain attribution to SparkFun Electronics and release anything derivative under the same license.

Distributed as-is; no warranty is given.

- Your friends at SparkFun.
```

The [implementation header](https://github.com/sparkfun/SparkFun_Ambient_Light_Sensor_Arduino_Library/blob/efde0817bd6857863067bd1653a2cfafe6c68732/src/SparkFun_VEML6030_Ambient_Light_Sensor.cpp#L1-L9)
names Elias Santistevan, July 2019, and says:

```text
License: This code is public domain but you buy me a beer if you use this and
we meet someday (Beerware license).
```

The four example headers name SparkFun Electronics and Elias Santistevan and
use this variant (see the [basic example](https://github.com/sparkfun/SparkFun_Ambient_Light_Sensor_Arduino_Library/blob/efde0817bd6857863067bd1653a2cfafe6c68732/Examples/Example1_Read_Ambient_Basics/Example1_Read_Ambient_Basics.ino#L12-L16)):

```text
License: This code is public domain but if you use this and we meet someday, get me a beer!
```

The excerpts preserve wording; the linked source retains the original
formatting. The `.h` file has no separate attribution/license header. The
retained README SHA-256 at `1.0.4` is
`5a4d17d726caee56db5aec9d4dbbe34c1f1f95402ae470d201ce3fa5453f10dd`.
The pinned upstream text and matching local notices were checked November 9,
2026.

Keep SparkFun’s attribution and same-license terms with the modified library files.

## Complete diff classification

The [vendor comparison manifest](../../tools/vendor/sources.json) identifies the two modified files and their exact Git blobs. The [vendor comparison tool](../../tools/vendor/README.md) checks the remaining files against the original library.
