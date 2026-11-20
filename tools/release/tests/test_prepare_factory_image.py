from __future__ import annotations

import contextlib
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

from tools.dev import ota_trial, rom_flash
from tools.release import prepare_factory_image as factory
from tools.release.tests.test_firmware_artifact import make_esp32s3_image


# Fixed bytes from the current generated S3 table, independent of the exporter.
PARTITION_PREFIX = bytes.fromhex(
    "aa50010200900000005000006e76730000000000000000000000000000000000"
    "aa50010000e00000002000006f74616461746100000000000000000000000000"
    "aa50001000000100000033006f74615f30000000000000000000000000000000"
    "aa50001100003400000033006f74615f31000000000000000000000000000000"
    "aa50018200006700000019007370696666730000000000000000000000000000"
    "ebebffffffffffffffffffffffffffff2fec237655c2a85604b23bf7799a2754"
)
PARTITIONS = PARTITION_PREFIX + b"\xFF" * (3072 - len(PARTITION_PREFIX))


def application_image(epoch: int = 2, *, schema: int = 1) -> bytes:
    app_desc = struct.pack("<I", 0xABCD5432) + bytes(252)
    policy = struct.pack("<4sHHII", b"FXBT", schema, 16, epoch, 0)
    return make_esp32s3_image(app_desc + policy + b"factory-application")


class FactoryImageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "S3 build with spaces"
        self.build.mkdir()
        self.output = self.root / "output with spaces/factory.bin"
        bootloader = bytearray(make_esp32s3_image(b"factory-bootloader"))
        bootloader[2:4] = bytes((2, 0x3F))
        bootloader[-32:] = hashlib.sha256(bootloader[:-32]).digest()
        self.bootloader = bytes(bootloader)
        self.firmware = application_image()
        (self.build / "bootloader.bin").write_bytes(self.bootloader)
        (self.build / "partitions.bin").write_bytes(PARTITIONS)
        (self.build / "firmware.bin").write_bytes(self.firmware)
        self.version = "esptool v5.3.1"
        self.resolve = mock.patch.object(factory, "resolve_esptool_argv", return_value=("esptool",))
        self.resolve.start()
        self.addCleanup(self.resolve.stop)
        self.run = mock.patch.object(factory, "run_esptool", side_effect=self.esptool)
        self.tool = self.run.start()
        self.addCleanup(self.run.stop)

    def esptool(self, argv: tuple[str, ...]) -> str:
        if argv[-1] == "version":
            return self.version
        merged = bytearray(b"\xFF" * 0x800000)
        # Consume the actual argv supplied at the external tool boundary.
        for index in range(len(argv) - 10, len(argv), 2):
            offset = int(argv[index], 0)
            data = Path(argv[index + 1]).read_bytes()
            merged[offset:offset + len(data)] = data
        Path(argv[argv.index("-o") + 1]).write_bytes(merged)
        return "merged successfully"

    def test_factory_artifact_has_both_slots_valid_rescue_and_new_trial(self) -> None:
        for version in ("esptool.py v4.9.0", "esptool v5.3.1"):
            with self.subTest(version=version):
                self.version = version
                output = self.output.with_name(f"factory-{version[-5:]}.bin")
                digest = factory.prepare_factory_image(self.build, output)
                data = output.read_bytes()
                self.assertEqual(len(data), 0x800000)
                self.assertEqual(data[:len(self.bootloader)], self.bootloader)
                self.assertEqual(data[0x8000:0x8C00], PARTITIONS)
                for offset in (0x10000, 0x340000):
                    self.assertEqual(data[offset:offset + len(self.firmware)], self.firmware)
                first, second = ota_trial.parse_otadata(data[0xE000:0x10000])
                self.assertTrue(first.crc_valid and second.crc_valid)
                self.assertEqual((first.slot, first.state), (0, ota_trial.ESP_OTA_IMG_VALID))
                self.assertEqual((second.slot, second.state), (1, ota_trial.ESP_OTA_IMG_NEW))
                self.assertEqual(data[0xE004:0xE018], b"\xFF" * 20)
                self.assertEqual(data[0xF004:0xF018], b"\xFF" * 20)
                self.assertEqual(data[0x9000:0xE000], b"\xFF" * 0x5000)
                self.assertEqual(data[0x670000:], b"\xFF" * 0x190000)
                self.assertEqual(digest, hashlib.sha256(data).hexdigest())
                command = self.tool.call_args.args[0]
                major_four = "4.9" in version
                self.assertIn("merge_bin" if major_four else "merge-bin", command)
                self.assertIn("--fill-flash-size" if major_four else "--pad-to-size", command)
                self.assertEqual(command[command.index("--flash_mode" if major_four else "--flash-mode") + 1], "dio")

    def test_new_factory_trial_requires_epoch_two_before_merge(self) -> None:
        for image in (application_image(1), make_esp32s3_image(),
                      application_image(3), application_image(schema=2)):
            with self.subTest(image_sha256=hashlib.sha256(image).hexdigest()):
                (self.build / "firmware.bin").write_bytes(image)
                self.tool.reset_mock()
                with self.assertRaisesRegex(rom_flash.RomFlashError, "epoch 2"):
                    factory.prepare_factory_image(self.build, self.output)
                self.assertFalse(self.output.exists())
                self.assertEqual(self.tool.call_args_list, [mock.call(("esptool", "version"))])

    def test_rejects_missing_corrupt_or_incompatible_inputs_before_merging(self) -> None:
        cases = (
            ("firmware.bin", None, "missing build input"),
            ("firmware.bin", self.firmware[:-1], "exact size"),
            ("bootloader.bin", self.firmware, "DIO"),
            ("bootloader.bin", b"x" * 0x8001, "invalid size"),
            ("partitions.bin", PARTITIONS[:-1], "current 8 MiB"),
            ("partitions.bin", PARTITIONS[:84] + b"\x01" + PARTITIONS[85:], "current 8 MiB"),
        )
        for name, content, message in cases:
            with self.subTest(name=name, message=message):
                path = self.build / name
                original = path.read_bytes()
                try:
                    if content is None:
                        path.unlink()
                    else:
                        path.write_bytes(content)
                    self.tool.reset_mock()
                    with self.assertRaisesRegex(rom_flash.RomFlashError, message):
                        factory.prepare_factory_image(self.build, self.output)
                    self.assertFalse(self.output.exists())
                    self.assertEqual(self.tool.call_count, 1)  # Version only; no merge.
                finally:
                    path.write_bytes(original)

    def test_rejects_a_complete_image_for_the_wrong_chip(self) -> None:
        image = bytearray(self.firmware)
        image[12:14] = b"\x05\x00"
        image[-32:] = hashlib.sha256(image[:-32]).digest()
        (self.build / "firmware.bin").write_bytes(image)
        with self.assertRaisesRegex(rom_flash.RomFlashError, "chip ID"):
            factory.prepare_factory_image(self.build, self.output)
        self.assertFalse(self.output.exists())

    def test_failed_merge_leaves_no_partial_final_artifact(self) -> None:
        def fail_merge(argv: tuple[str, ...]) -> str:
            if argv[-1] == "version":
                return self.version
            Path(argv[argv.index("-o") + 1]).write_bytes(b"partial")
            raise subprocess.CalledProcessError(1, argv, stderr="merge failed")

        self.tool.side_effect = fail_merge
        with self.assertRaises(subprocess.CalledProcessError):
            factory.prepare_factory_image(self.build, self.output)
        self.assertEqual(list(self.output.parent.iterdir()), [])

    def test_rejects_silent_corruption_in_an_erased_region(self) -> None:
        def corrupt_merge(argv: tuple[str, ...]) -> str:
            result = self.esptool(argv)
            if argv[-1] != "version":
                merged = Path(argv[argv.index("-o") + 1])
                data = bytearray(merged.read_bytes())
                data[0x9000] = 0
                merged.write_bytes(data)
            return result

        self.tool.side_effect = corrupt_merge
        with self.assertRaisesRegex(rom_flash.RomFlashError, "complete factory layout"):
            factory.prepare_factory_image(self.build, self.output)
        self.assertFalse(self.output.exists())

    def test_export_uses_snapshot_when_original_build_changes_during_merge(self) -> None:
        def mutate_original(argv: tuple[str, ...]) -> str:
            if argv[-1] != "version":
                (self.build / "firmware.bin").write_bytes(b"another build")
            return self.esptool(argv)

        self.tool.side_effect = mutate_original
        factory.prepare_factory_image(self.build, self.output)
        data = self.output.read_bytes()
        self.assertEqual(data[0x340000:0x340000 + len(self.firmware)], self.firmware)

    def test_preserves_existing_output_and_rejects_misleading_extension(self) -> None:
        self.output.parent.mkdir()
        self.output.write_bytes(b"previous factory artifact")
        with self.assertRaisesRegex(rom_flash.RomFlashError, "already exists"):
            factory.prepare_factory_image(self.build, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous factory artifact")
        with self.assertRaisesRegex(rom_flash.RomFlashError, "raw binary"):
            factory.prepare_factory_image(self.build, self.output.with_suffix(".hex"))
        self.tool.assert_not_called()

    def test_preserves_another_export_published_while_merging(self) -> None:
        def publish_competing_artifact(argv: tuple[str, ...]) -> str:
            result = self.esptool(argv)
            if argv[-1] != "version":
                self.output.write_bytes(b"another completed factory artifact")
            return result

        self.tool.side_effect = publish_competing_artifact
        with self.assertRaisesRegex(rom_flash.RomFlashError, "already exists"):
            factory.prepare_factory_image(self.build, self.output)
        self.assertEqual(self.output.read_bytes(), b"another completed factory artifact")
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])

    def test_cli_reports_failure_and_programming_details(self) -> None:
        args = ["--build-dir", str(self.build), "--output", str(self.output)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            self.assertEqual(factory.main(args), 0)
            self.assertEqual(factory.main(args), 1)
        self.assertIn("Programming address: 0x0", stdout.getvalue())
        self.assertIn("8388608 bytes", stdout.getvalue())
        self.assertIn("SHA-256:", stdout.getvalue())
        self.assertIn("output already exists", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
