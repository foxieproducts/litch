from __future__ import annotations

import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.dev.devices import DeviceInventoryError
from tools.dev import macos_usb_reset


class ExactUsbResetTests(unittest.TestCase):
    def test_rejects_non_mac_and_non_exact_serial_before_any_process(self) -> None:
        with mock.patch.object(macos_usb_reset.subprocess, "run") as run:
            with mock.patch.object(macos_usb_reset.platform, "system", return_value="Linux"):
                with self.assertRaisesRegex(DeviceInventoryError, "only on macOS"):
                    macos_usb_reset.reset_exact_litch_application_usb("00005e0053cd")
            with mock.patch.object(macos_usb_reset.platform, "system", return_value="Darwin"):
                with self.assertRaisesRegex(DeviceInventoryError, "exact S3 serial"):
                    macos_usb_reset.reset_exact_litch_application_usb("other")
            run.assert_not_called()

    def test_builds_and_invokes_helper_for_one_exact_serial(self) -> None:
        result = SimpleNamespace(returncode=0, stdout="", stderr="")
        with (
            mock.patch.object(macos_usb_reset.platform, "system", return_value="Darwin"),
            mock.patch.object(macos_usb_reset.subprocess, "run", return_value=result) as run,
        ):
            macos_usb_reset.reset_exact_litch_application_usb("00005e0053cd")
        compile_argv = run.call_args_list[0].args[0]
        reset_argv = run.call_args_list[1].args[0]
        self.assertEqual(compile_argv[0], "/usr/bin/clang")
        self.assertEqual(Path(compile_argv[-3]).name, "macos_usb_reset.m")
        self.assertEqual(reset_argv[1], "00005e0053cd")
        self.assertEqual(reset_argv[0], compile_argv[-1])

    def test_helper_failure_is_reported(self) -> None:
        compile_ok = SimpleNamespace(returncode=0, stdout="", stderr="")
        reset_failed = SimpleNamespace(returncode=5, stdout="", stderr="busy")
        with (
            mock.patch.object(macos_usb_reset.platform, "system", return_value="Darwin"),
            mock.patch.object(
                macos_usb_reset.subprocess,
                "run",
                side_effect=(compile_ok, reset_failed),
            ),
        ):
            with self.assertRaisesRegex(DeviceInventoryError, "busy"):
                macos_usb_reset.reset_exact_litch_application_usb("00005e0053cd")
