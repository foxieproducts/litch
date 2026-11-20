from __future__ import annotations

from pathlib import Path
import json
import tempfile
import unittest

from tools.dev.verify_usb_configuration import (
    verify_boot_policy_descriptor,
    verify_ota_rollback_hook,
    verify_ota_rollback_hook_source,
    verify_tinyusb_power_hook,
)


class VerifyBootPolicyDescriptorTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        corpus = Path(__file__).resolve().parents[3] / "test/fixtures/firmware_boot_policy.json"
        self.images = {
            row["name"]: bytes.fromhex(row["image_hex"])
            for row in json.loads(corpus.read_text())["cases"]
        }
        self.provider = ".pio/build/litch-esp32s3/src/firmware_boot_policy_descriptor.cpp.o"

    def verify(self, *, image: str = "sdk_policy_2", address: int = 0x3C000120,
               size: int = 16, provider: str | None = None,
               duplicate: bool = False, symbol_address: int = 0x3C000120) -> None:
        provider = self.provider if provider is None else provider
        section = (
            ".rodata_custom_desc\n"
            f"  0x{address:x} 0x{size:x} {provider}\n"
            f"  0x{symbol_address:x} litch_boot_policy_descriptor\n"
        )
        text = section + f"litch_boot_policy_descriptor {provider}\n"
        if duplicate:
            text += section
        map_path = self.directory / "firmware.map"
        map_path.write_text(text)
        firmware_path = self.directory / "firmware.bin"
        firmware_path.write_bytes(self.images[image])
        verify_boot_policy_descriptor(map_path, firmware_path)

    def test_accepts_verified_current_descriptor_at_sdk_boundary(self) -> None:
        self.verify()

    def test_rejects_missing_malformed_or_retired_record_policy(self) -> None:
        for image in ("legacy_absent", "reserved_nonzero", "record_policy_1"):
            with self.subTest(image=image), self.assertRaisesRegex(RuntimeError, "actual current"):
                self.verify(image=image)

    def test_rejects_descriptor_from_unverified_image(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "image integrity failed"):
            self.verify(image="bad_digest_no_capability")

    def test_rejects_wrong_provider_or_retained_size(self) -> None:
        for change in ({"provider": "other.o"}, {"size": 20}):
            with self.subTest(change=change), self.assertRaisesRegex(RuntimeError, "provider|size"):
                self.verify(**change)

    def test_rejects_duplicate_or_misplaced_symbols(self) -> None:
        for change in ({"duplicate": True}, {"address": 0x3C000124},
                       {"symbol_address": 0x3C000124}):
            with self.subTest(change=change), self.assertRaisesRegex(RuntimeError, "one retained|must follow"):
                self.verify(**change)

    def test_rejects_stripped_map_evidence(self) -> None:
        map_path = self.directory / "firmware.map"
        map_path.write_text("")
        with self.assertRaisesRegex(RuntimeError, "exactly one Litch"):
            verify_boot_policy_descriptor(map_path, self.directory / "firmware.bin")


class VerifyOtaRollbackHookTests(unittest.TestCase):
    def _map(self, content: str) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "firmware.map"
        path.write_text(content, encoding="utf-8")
        return path

    def test_accepts_litch_strong_hook_provider(self) -> None:
        path = self._map(
            "verifyRollbackLater  .pio/build/litch-esp32s3/src/ota_boot_health.cpp.o\n"
        )
        verify_ota_rollback_hook(path)

    def test_rejects_arduino_weak_default_provider(self) -> None:
        path = self._map(
            "verifyRollbackLater  .pio/build/litch-esp32s3/"
            "libFrameworkArduino.a(esp32-hal-misc.c.o)\n"
        )
        with self.assertRaisesRegex(RuntimeError, "did not resolve"):
            verify_ota_rollback_hook(path)

    def test_rejects_missing_or_ambiguous_provider(self) -> None:
        for content in (
            "",
            "verifyRollbackLater  first.o\nverifyRollbackLater  second.o\n",
        ):
            with self.subTest(content=content):
                with self.assertRaisesRegex(RuntimeError, "exactly one"):
                    verify_ota_rollback_hook(self._map(content))

    def test_rejects_missing_map(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "missing.map"
            with self.assertRaisesRegex(RuntimeError, "missing S3 linker map"):
                verify_ota_rollback_hook(missing)


class VerifyOtaRollbackHookSourceTests(unittest.TestCase):
    def _source(self, content: str) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "ota_boot_health.cpp"
        path.write_text(content, encoding="utf-8")
        return path

    def test_accepts_pure_pre_runtime_decision(self) -> None:
        verify_ota_rollback_hook_source(self._source(
            'extern "C" bool verifyRollbackLater() {\n  return true;\n}\n'
        ))

    def test_rejects_project_owner_call_before_runtime_init(self) -> None:
        source = self._source(
            'extern "C" bool verifyRollbackLater() {\n'
            '  firmware::PlatformBootTiming().arduinoBeginMs = fxe::Millis();\n'
            '  return true;\n'
            '}\n'
        )
        with self.assertRaisesRegex(RuntimeError, "pure pre-runtime"):
            verify_ota_rollback_hook_source(source)

    def test_rejects_missing_or_ambiguous_source_hook(self) -> None:
        for content in (
            "",
            'extern "C" bool verifyRollbackLater() { return true; }\n'
            'extern "C" bool verifyRollbackLater() { return true; }\n',
        ):
            with self.subTest(content=content):
                with self.assertRaisesRegex(RuntimeError, "exactly one"):
                    verify_ota_rollback_hook_source(self._source(content))


class VerifyTinyUsbPowerHookTests(unittest.TestCase):
    def _map(self, content: str) -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "firmware.map"
        path.write_text(content, encoding="utf-8")
        return path

    def test_accepts_litch_power_descriptor_hook_provider(self) -> None:
        path = self._map(
            "__wrap_tinyusb_init  .pio/build/litch-esp32s3/"
            "src/usb_device_power_descriptor.cpp.o\n"
        )
        verify_tinyusb_power_hook(path)

    def test_rejects_wrong_power_descriptor_hook_provider(self) -> None:
        path = self._map("__wrap_tinyusb_init  unexpected/provider.o\n")
        with self.assertRaisesRegex(RuntimeError, "did not resolve"):
            verify_tinyusb_power_hook(path)

    def test_rejects_missing_or_ambiguous_provider(self) -> None:
        for content in (
            "",
            "__wrap_tinyusb_init  first.o\n__wrap_tinyusb_init  second.o\n",
        ):
            with self.subTest(content=content):
                with self.assertRaisesRegex(RuntimeError, "exactly one"):
                    verify_tinyusb_power_hook(self._map(content))

    def test_rejects_missing_map(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "missing.map"
            with self.assertRaisesRegex(RuntimeError, "missing S3 linker map"):
                verify_tinyusb_power_hook(missing)


if __name__ == "__main__":
    unittest.main()
