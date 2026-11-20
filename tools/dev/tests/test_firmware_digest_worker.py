"""Exercise the real ESP32 digest worker and software image reader.

Production digest admission, acquisition and partition reads are compiled unchanged.
The fixture dispatches the OTA worker slot and models an OTA-held flash lease; actual
flash contents, scheduler behavior and electrical effects require physical HIL.
The native diagnostic clock uses the same fixture ticks as the RTOS adapter.
"""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "firmware_digest_worker"


class FirmwareDigestWorkerTest(unittest.TestCase):
    def test_sdk_read_waits_for_shared_flash_owner_and_releases_on_every_exit(self):
        with tempfile.TemporaryDirectory(prefix="litch-digest-worker-") as tmp:
            binary = Path(tmp) / "digest_worker"
            image = bytearray(336)
            image[0:2] = bytes((0xE9, 1))
            struct.pack_into("<H", image, 12, 9)  # ESP32-S3
            image[23] = 1  # Appended SHA-256.
            struct.pack_into("<II", image, 24, 0x3C000020, 256)
            struct.pack_into("<I", image, 32, 0xABCD5432)
            image[176:208] = bytes(range(32))  # Build identity.
            checksum = 0xEF
            for value in image[32:288]:
                checksum ^= value
            image[303] = checksum
            image[304:] = hashlib.sha256(image[:304]).digest()
            image_path = Path(tmp) / "image.bin"
            image_path.write_bytes(image)
            result = subprocess.run([
                "c++", "-std=c++17", "-DLITCH_ESP32", "-DLITCH_ESP32S3", "-DLITCH_NATIVE",
                "-I", str(FIXTURE), "-I", str(ROOT / "include"),
                "-I", str(ROOT / "lib/foxie_embedded/include"),
                str(ROOT / "src/firmware_digest_provider_esp32.cpp"),
                str(ROOT / "src/firmware_digest_provider.cpp"),
                str(ROOT / "src/ota_image_identity_esp32.cpp"),
                str(ROOT / "src/ota_image_identity.cpp"),
                str(ROOT / "src/sha256.cpp"),
                str(FIXTURE / "main.cpp"), "-o", str(binary),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary), str(image_path)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
