from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.dev import batch_flash
from tools.dev.devices import ESP32S3_ROM_PID, ESPRESSIF_VID, LITCH_APP_PID, PhysicalUsbIdentity, UsbParent
from tools.dev.hardware import FlashCandidate
from tools.dev.runner import DevError


def usb_candidate(serial: str, location: str, pid: int = ESP32S3_ROM_PID) -> FlashCandidate:
    identity = PhysicalUsbIdentity(serial, location)
    parent = UsbParent(identity, ESPRESSIF_VID, pid, (f"/dev/cu.{location}",))
    target_hint = "s3" if pid == LITCH_APP_PID else None
    return FlashCandidate("usb", identity.serial, parent.port, target_hint, parent.ports, parent)


class BatchFlashTests(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.workspace = SimpleNamespace(root=Path(temp.name))
        self.build_dir = self.workspace.root / "litch-esp32s3"
        self.build_dir.mkdir()
        self.runner = mock.Mock()
        self.build = mock.Mock(side_effect=self.write_build)
        self.discovery = self.replace("discover_possible_usb_boards")
        self.flash = self.replace("flash_exact_device")
        self.flash.side_effect = lambda **kwargs: kwargs["build_target"]("s3")
        self.locks = self.replace("LockManager")
        self.locks.return_value.lane.return_value = contextlib.nullcontext()
        self.replace("platformio_build_directory", return_value=self.build_dir)
        sleep = mock.patch.object(batch_flash.time, "sleep")
        self.sleep = sleep.start()
        self.addCleanup(sleep.stop)
        self.output = io.StringIO()

    def replace(self, name: str, **kwargs) -> mock.Mock:
        replacement = mock.patch.object(batch_flash, name, **kwargs)
        result = replacement.start()
        self.addCleanup(replacement.stop)
        return result

    def write_build(self, target: str) -> None:
        self.assertEqual(target, "s3")
        for name in ("firmware.bin", "bootloader.bin", "partitions.bin"):
            (self.build_dir / name).write_bytes(b"original")

    def run_batch(self, *, factory: bool = False) -> None:
        with contextlib.redirect_stdout(self.output):
            batch_flash.flash_batch(
                workspace=self.workspace,
                runner=self.runner,
                lock_timeout=7,
                build_target=self.build,
                platformio_core_dir=lambda: self.workspace.root,
                factory_provision=factory,
            )

    def test_builds_once_and_skips_reenumerated_or_replugged_serial(self) -> None:
        first = usb_candidate("AA:BB:CC:00:11:22", "1-2")
        same_app = usb_candidate("AABBCC001122", "1-2", LITCH_APP_PID)
        same_replug = usb_candidate("aa-bb-cc-00-11-22", "1-7")
        second = usb_candidate("AABBCC001123", "1-2", LITCH_APP_PID)
        self.discovery.side_effect = [
            (), (first,), (same_app,), (same_replug, second), KeyboardInterrupt(),
        ]
        with self.assertRaises(KeyboardInterrupt):
            self.run_batch()

        self.build.assert_called_once_with("s3")
        self.locks.return_value.lane.assert_called_once_with("platformio", timeout=7)
        self.assertEqual(self.sleep.call_count, 2)
        self.assertEqual(self.flash.call_count, 2)
        for write, candidate in zip(self.flash.call_args_list, (first, second)):
            self.assertEqual(write.kwargs["target"], "s3")
            self.assertEqual(write.kwargs["usb_identity"], candidate.identity)
            self.assertEqual(write.kwargs["expected_usb_identity"], candidate.usb_parent.identity)
            self.assertIsNone(write.kwargs["ble_identity"])
            self.assertFalse(write.kwargs["factory_provision"])
        self.assertIn(f"wrote firmware to {second.identity}. Batch total 2.", self.output.getvalue())
        self.assertIn(f"{first.identity} already flashed in this batch", self.output.getvalue())

    def test_waits_for_one_complete_candidate_without_prompting(self) -> None:
        first = usb_candidate("first", "1-2")
        second = usb_candidate("second", "1-3")
        duplicate = usb_candidate("first", "1-4")
        incomplete = FlashCandidate("usb", "", "/dev/missing")
        self.discovery.side_effect = [
            (incomplete,), (first, second), (first, duplicate), (first,), KeyboardInterrupt(),
        ]
        with (
            self.assertRaises(KeyboardInterrupt),
            mock.patch("builtins.input", side_effect=AssertionError("batch cannot prompt")),
        ):
            self.run_batch(factory=True)

        self.assertEqual(self.sleep.call_count, 3)
        self.flash.assert_called_once()
        self.assertEqual(self.flash.call_args.kwargs["usb_identity"], first.identity)
        self.assertTrue(self.flash.call_args.kwargs["factory_provision"])
        output = self.output.getvalue()
        self.assertIn("erase and provision every admitted board", output)
        self.assertIn("A single eligible board already connected is included", output)
        self.assertIn("requires factory qualification", output)
        self.assertIn("waiting for the USB board to report both serial and location", output)
        self.assertEqual(output.count("multiple unflashed USB boards detected"), 1)

    def test_stops_before_write_when_any_built_file_changes(self) -> None:
        first = usb_candidate("first", "1-2")
        for name in ("firmware.bin", "bootloader.bin", "partitions.bin"):
            with self.subTest(name=name):
                def rediscover(_workspace, _runner, _lock_timeout):
                    (self.build_dir / name).write_bytes(b"modified")
                    return (first,)

                self.discovery.side_effect = rediscover
                writes = mock.Mock()

                def write(**kwargs):
                    kwargs["build_target"]("s3")
                    writes()

                self.flash.side_effect = write
                with self.assertRaisesRegex(DevError, "S3 flash files changed"):
                    self.run_batch()
                writes.assert_not_called()
        self.assertNotIn("wrote firmware", self.output.getvalue())

    def test_failure_stops_batch_without_retries_or_success_receipt(self) -> None:
        first = usb_candidate("first", "1-2")
        second = usb_candidate("second", "1-3")
        self.discovery.side_effect = [(first,), (second,)]
        self.flash.side_effect = DevError("write may have been interrupted")
        with self.assertRaisesRegex(DevError, "write may have been interrupted"):
            self.run_batch()
        self.discovery.assert_called_once()
        self.flash.assert_called_once()
        self.sleep.assert_not_called()
        self.assertNotIn("wrote firmware", self.output.getvalue())

    def test_q_stops_after_current_write_before_another_board_is_admitted(self) -> None:
        first = usb_candidate("AABBCC001122", "1-2")
        self.discovery.return_value = (first,)
        with mock.patch.object(batch_flash, "BatchInput") as keyboard:
            keyboard.return_value.__enter__.return_value.quit_requested.side_effect = [False, False, True]
            self.run_batch(factory=True)
        self.flash.assert_called_once()
        self.discovery.assert_called_once()
        self.assertIn("batch stopped. Wrote 1 board(s).", self.output.getvalue())
