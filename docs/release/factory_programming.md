# Programming a blank V3.31 board

A factory flash replaces all flash contents, including games, artwork and settings. Use **Board software** in the companion app for routine updates. Production functional testing covers V3.3 and newer supported revisions. V3.31 uses the V3.3 profile.

For one blank ESP32-S3 board, connect a data-capable USB cable. Run `./litch devices` and copy that board's USB serial into the second command.

```sh
./litch devices
./litch flash s3 --device <usb-serial> --factory
```

## Functional test

1. Use Circle to advance through red, green, blue and white. Use X to go back. Check each square, perimeter and button light on both ends.
2. Sweep a magnetic piece or jig over all 64 Hall sensors. Each square turns white once it has seen both occupied and empty. Clear the board.
3. Press and release the six controls on each end. Hold the light steady, then cover or uncover each light sensor at A1 and H8. The cue changes from amber to inward green when a sensor passes.
4. Leave the board powered at the amber wireless cue and complete the local checks on a second compatible board, or use a qualified Litch verifier. The boards exchange a test over Bluetooth. When using the batch station, leave each board connected through its reboot until the station confirms its saved PASS and says it can be unplugged.

A five-second same-end X and Circle hold during testing records an Override. Only a saved PASS confirms completion. Holding either Up button at a later power-on starts a fresh inspection.

## Image for an external programmer

```sh
pio run -e litch-esp32s3
python3 tools/release/prepare_factory_image.py
```

The exporter writes `build/factory/litch-esp32s3-factory.bin` and prints its SHA-256. The image is 8 MiB. Program it at **0x0**, using **DIO, 80 MHz** on an **ESP32-S3**. It contains the bootloader, partition table, OTA selectors and both application slots. An existing output is preserved. See `python3 tools/release/prepare_factory_image.py --help` to select a different output or build directory.
