from __future__ import annotations
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path
from typing import Sequence
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rom_flash


class DirectRunner:
    """Run the shipping command planner against storage at the esptool seam."""
    def __init__(self, table=None, fail=None):
        self.flash = bytearray(b"\xa5") * rom_flash.FLASH_SIZE_BYTES
        if table is not None:
            self.flash[0x8000:0x8C00] = table
        self.calls = []
        self.fail = fail
        self.segments = []

    def run(self, argv, *, timeout_seconds):
        args = tuple(argv)
        self.calls.append(args)
        if self.fail:
            raise self.fail
        if "read-flash" in args or "read_flash" in args:
            command = "read-flash" if "read-flash" in args else "read_flash"
            index = args.index(command)
            offset, size = int(args[index + 1], 0), int(args[index + 2], 0)
            Path(args[index + 3]).write_bytes(self.flash[offset:offset + size])
        elif "erase-flash" in args or "erase_flash" in args:
            self.flash[:] = b"\xff" * len(self.flash)
        else:
            for index, token in enumerate(args[:-1]):
                if not token.startswith("0x"):
                    continue
                offset, content = int(token, 0), Path(args[index + 1]).read_bytes()
                self.segments.append((offset, content))
                self.flash[offset:offset + len(content)] = content
        return rom_flash.CommandResult(args, 0, "esptool transfer complete")


class DirectFlashTests(unittest.TestCase):
    def setUp(self): self.fixture = FixtureTree()
    def tearDown(self): self.fixture.close()

    def test_s3_direct_flash_preserves_current_and_historical_layouts(self):
        for major in (4, 5):
            for legacy, recovery in ((False, False), (True, False), (True, True)):
                with self.subTest(major=major, legacy=legacy, recovery=recovery):
                    runner = DirectRunner(partition_table(legacy=legacy, recovery=recovery))
                    # No source integrity, current table or VALID selector is
                    # supplied. This is direct replacement, not an OTA trial.
                    before = bytes(runner.flash)
                    rom_flash.flash_s3_direct(runner, ("esptool",), rom_flash.EsptoolDialect(major, (major, 9, 0)),
                                              port="/dev/cu.fixture", build_dir=self.fixture.build)
                    self.assertEqual(len(runner.calls), 1)
                    self.assertNotIn("--no-stub", runner.calls[0])
                    self.assertEqual(runner.calls[0][runner.calls[0].index("--after") + 1],
                                     "watchdog_reset" if major == 4 else "watchdog-reset")
                    self.assertEqual([offset for offset, _ in runner.segments], [0x10000, 0xE000])
                    self.assertEqual(runner.flash[0x8000:0xE000], before[0x8000:0xE000])
                    self.assertEqual(runner.flash[0x210000:], before[0x210000:])
                    first, second = rom_flash.ota_trial.parse_otadata(bytes(runner.flash[0xE000:0x10000]))
                    self.assertEqual((first.sequence, first.state, first.slot, first.crc_valid), (1, 2, 0, True))
                    self.assertFalse(second.selectable)

    def test_larger_image_uses_only_actual_capacity_and_never_blindly_crosses_slot(self):
        (self.fixture.build / "firmware.bin").write_bytes(b"x" * (0x200000 + 1))
        for legacy in (False, True):
            with self.subTest(legacy=legacy):
                runner = DirectRunner(partition_table(legacy=legacy))
                if legacy:
                    with self.assertRaisesRegex(rom_flash.RomFlashError, "slot holds"):
                        rom_flash.flash_s3_direct(runner, ("esptool",), rom_flash.EsptoolDialect(5, (5, 3, 1)),
                                                  port="/dev/cu.fixture", build_dir=self.fixture.build)
                    self.assertEqual(runner.segments, [])
                else:
                    rom_flash.flash_s3_direct(runner, ("esptool",), rom_flash.EsptoolDialect(5, (5, 3, 1)),
                                              port="/dev/cu.fixture", build_dir=self.fixture.build)
                    self.assertEqual(len(runner.calls), 2)
                    self.assertEqual(runner.calls[0][runner.calls[0].index("--after") + 1], "no-reset")
                    self.assertEqual(runner.calls[1][runner.calls[1].index("--after") + 1], "watchdog-reset")

    def test_factory_writes_one_application_then_selected_valid_metadata(self):
        runner = DirectRunner()
        dialect = rom_flash.EsptoolDialect(5, (5, 3, 1))
        rom_flash.erase_s3_direct(runner, ("esptool",), dialect, port="/dev/cu.fixture")
        rom_flash.flash_s3_direct(runner, ("esptool",), dialect, port="/dev/cu.fixture",
                                  build_dir=self.fixture.build, factory=True)
        self.assertEqual(len(runner.calls), 2)
        self.assertEqual([offset for offset, _ in runner.segments], [0, 0x8000, 0x10000, 0xE000])
        self.assertNotIn("--no-stub", runner.calls[0])
        self.assertEqual(runner.calls[0][runner.calls[0].index("--after") + 1], "no-reset")
        self.assertEqual(runner.calls[1][runner.calls[1].index("--after") + 1], "watchdog-reset")
        self.assertEqual(runner.flash[0x340000:], b"\xff" * (rom_flash.FLASH_SIZE_BYTES - 0x340000))

    def test_real_esptool_failure_is_not_reported_as_completion_or_retried(self):
        runner = DirectRunner(fail=TimeoutError("USB transfer stopped"))
        with self.assertRaisesRegex(rom_flash.RomFlashError, "USB transfer stopped"):
            rom_flash.flash_s3_direct(runner, ("esptool",), rom_flash.EsptoolDialect(5, (5, 3, 1)),
                                      port="/dev/cu.fixture", build_dir=self.fixture.build)
        self.assertEqual(len(runner.calls), 1)


def partition_table(*, legacy=False, flags=0, checksum=True, padding=0xFF, recovery=False) -> bytes:
    rows = (
        (1, 2, 0x9000, 0x5000, b"nvs"),
        (1, 0, 0xE000, 0x2000, b"otadata"),
        (0, 0x10, 0x10000, 0x200000 if legacy else 0x330000, b"ota_0"),
        (0, 0x11, 0x210000 if legacy else 0x340000,
         0x200000 if legacy else 0x330000, b"ota_1"),
        (1, 0x82, 0x450000 if legacy else 0x670000,
         0x3B0000 if legacy else 0x190000, b"spiffs"),
    )
    entries = b"".join(struct.pack("<HBBII16sI", 0x50AA, *row, flags) for row in rows)
    if recovery:
        entries = entries[:128] + struct.pack("<HBBII16sI", 0x50AA, 0, 0, 0x410000, 0x40000, b"uf2", 0) + entries[128:]
    tail = b"\xEB\xEB" + b"\xFF" * 14 + hashlib.md5(entries).digest() if checksum else b"\xFF" * 32
    return (entries + tail).ljust(rom_flash.PARTITION_TABLE_BINARY_BYTES, bytes((padding,)))

def valid_firmware(
    segment_data: bytes = b"\x01\x02\x03\x04",
    *, extra_segments: Sequence[bytes] = (),
) -> bytes:
    segments = (segment_data, *extra_segments)
    if any(not data or len(data) % 4 for data in segments):
        raise ValueError("test segment must be a non-empty multiple of four")
    header = bytearray(rom_flash.ESP_IMAGE_HEADER_SIZE)
    header[0] = rom_flash.ESP_IMAGE_MAGIC
    header[1] = len(segments)
    struct.pack_into("<H", header, 12, rom_flash.ESP32S3_CHIP_ID)
    header[23] = 1
    image = bytearray(header)
    for index, data in enumerate(segments):
        image.extend(struct.pack("<II", 0x3C000020 + index * 0x1000, len(data)))
        image.extend(data)
    checksum_offset = len(image) + ((15 - len(image)) % 16)
    image.extend(b"\x00" * (checksum_offset - len(image)))
    checksum = rom_flash.ESP_CHECKSUM_MAGIC
    for data in segments:
        for value in data:
            checksum ^= value
    image.append(checksum)
    digest = hashlib.sha256(image).digest()
    image.extend(digest)
    return bytes(image)

def policy_firmware(epoch: int = 2, payload: bytes = b"ABCD", *, schema: int = 1) -> bytes:
    descriptor = bytearray(256)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    return valid_firmware(
        bytes(descriptor) + struct.pack("<4sHHII", b"FXBT", schema, 16, epoch, 0)
        + payload
    )

class FixtureTree:
    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "firmware.bin").write_bytes(policy_firmware())
        (self.build / "bootloader.bin").write_bytes(b"bootloader")
        (self.build / "partitions.bin").write_bytes(partition_table())
    def close(self):
        self.temporary.cleanup()

class ArtifactTests(unittest.TestCase):
    def setUp(self): self.fixture = FixtureTree()
    def tearDown(self): self.fixture.close()


    def test_firmware_validation_rejects_corrupt_digest_and_wrong_chip(self) -> None:
        firmware_path = self.fixture.build / "firmware.bin"
        corrupt = bytearray(firmware_path.read_bytes())
        corrupt[-1] ^= 1
        firmware_path.write_bytes(corrupt)
        with self.assertRaisesRegex(rom_flash.RomFlashError, "SHA-256"):
            rom_flash.inspect_firmware_image(firmware_path)

        wrong_chip = bytearray(valid_firmware())
        struct.pack_into("<H", wrong_chip, 12, 5)
        firmware_path.write_bytes(wrong_chip)
        with self.assertRaisesRegex(rom_flash.RomFlashError, "ESP32-S3"):
            rom_flash.inspect_firmware_image(firmware_path)

    def test_verified_rom_boot_policy_matches_shared_full_image_fixtures(self) -> None:
        fixture = Path(__file__).resolve().parents[3] / "test/fixtures/firmware_boot_policy.json"
        cases = json.loads(fixture.read_text())["cases"]
        path = self.fixture.build / "firmware.bin"
        for case in cases:
            with self.subTest(case=case["name"]):
                path.write_bytes(bytes.fromhex(case["image_hex"]))
                if not case["valid_integrity"]:
                    with self.assertRaises(rom_flash.RomFlashError):
                        rom_flash.inspect_firmware_image_with_policy(path)
                    continue
                inspected = rom_flash.inspect_firmware_image_with_policy(path)
                self.assertIsInstance(inspected.metadata, rom_flash.FirmwareImageMetadata)
                self.assertEqual(asdict(inspected.boot_policy), case["expected"])
                self.assertEqual(inspected.metadata.sha256, case["artifact_sha256"])
                self.assertEqual(inspected.metadata.image_sha256, case["image_sha256"])
                self.assertEqual(inspected.metadata, rom_flash.inspect_firmware_image(path))
                self.assertEqual(set(asdict(inspected.metadata)), {
                    "size_bytes", "sha256", "image_sha256", "chip_id", "segment_count",
                })

    def test_combined_rom_inspection_reads_once_and_preserves_rom_size_limit(self) -> None:
        from tools.release import firmware_artifact

        data = valid_firmware(b"x" * (rom_flash.MAX_ESP32S3_FIRMWARE_SIZE - 80))
        self.assertEqual(len(data), rom_flash.MAX_ESP32S3_FIRMWARE_SIZE)
        path = self.fixture.build / "firmware.bin"
        path.write_bytes(data)
        with mock.patch.object(Path, "read_bytes", side_effect=(data, b"changed")) as read:
            inspected = rom_flash.inspect_firmware_image_with_policy(path)
        read.assert_called_once_with()
        self.assertEqual(inspected.metadata.size_bytes, len(data))
        self.assertEqual(inspected.boot_policy.status.value, "unavailable")
        with self.assertRaisesRegex(firmware_artifact.ArtifactValidationError, "2 MiB"):
            firmware_artifact.inspect_firmware_bytes_with_policy(data)
        path.write_bytes(b"x" * (rom_flash.MAX_ESP32S3_FIRMWARE_SIZE + 1))
        with self.assertRaisesRegex(rom_flash.RomFlashError, "3264 KiB"):
            rom_flash.inspect_firmware_image_with_policy(path)

    def test_header_version_is_exact_and_unambiguous(self) -> None:
        self.assertEqual(
            rom_flash.parse_firmware_header_version(
                'static constexpr const char* FIRMWARE_VERSION = "1.2.3-beta";'
            ),
            "1.2.3-beta",
        )
        invalid = (
            "",
            'FIRMWARE_VERSION = "0.7";\nFIRMWARE_VERSION = "0.8";',
            'FIRMWARE_VERSION = "bad version";',
        )
        for content in invalid:
            with self.subTest(content=content):
                with self.assertRaises(rom_flash.RomFlashError):
                    rom_flash.parse_firmware_header_version(content)
