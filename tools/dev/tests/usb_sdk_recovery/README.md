# Testing native USB recovery

Run this command from the repository root.

```sh
python3 tools/dev/check_usb_sdk_recovery.py --output /tmp/usb-recovery.txt
```

The runner uses installed ESP32-S3 TinyUSB headers and the ArduinoJson headers
cached by a native build. Use `--sdk` to specify the header directory. The runner
checks the headers and five pinned TinyUSB C files against [manifest.json](manifest.json).
The S3 firmware and this check use the same [device core](../../../pio/tinyusb/usbd.c).
The bundled TinyUSB sources retain their upstream MIT notices.

The tests run the USB device, CDC, control and FIFO code against simulated
controller events. They cover reset ordering, receive and transmit cleanup,
queue saturation, stale acknowledgements and recovery after IRQ errors.
Host USB enumeration and electrical timing require a physical USB connection.
To compare two implementations, pass `--baseline-root <checkout>` to compile
the recovery sources under that checkout’s `src/` against the same tests.
