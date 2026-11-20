from __future__ import annotations

import tempfile
import unittest
from io import StringIO
from pathlib import Path
from unittest import mock

from tools.dev import doctor
from tools.dev.doctor import active_port_overrides, collect_checks, render_checks, Check
from tools.dev.runner import Runner


class DoctorTests(unittest.TestCase):
    def test_firmware_and_default_scopes_check_launcher_serial_once(self) -> None:
        for scope in ("firmware", "all", "hardware"):
            with self.subTest(scope=scope), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "platformio.ini").write_text("[platformio]\n", encoding="utf-8")
                runner = mock.Mock()
                runner.run.return_value.stdout = "passed"
                with (
                    mock.patch.object(doctor, "_platformio_check", return_value=Check("platformio", True, "6")),
                    mock.patch.object(doctor, "_esptool_check", return_value=Check("esptool", True, "4.9")),
                    mock.patch.object(doctor, "_flutter_check", return_value=Check("flutter", True, "stable")),
                    mock.patch.object(
                        doctor.importlib.util,
                        "find_spec",
                        side_effect=lambda name: None if name == "serial" else mock.sentinel.module,
                    ),
                ):
                    checks = collect_checks(root, scope, runner)

                serial_checks = [check for check in checks if check.name == "serial"]
                self.assertEqual(len(serial_checks), 1)
                self.assertFalse(serial_checks[0].ok)
                self.assertIn("./litch bootstrap firmware", serial_checks[0].remediation)
                self.assertIn(doctor.sys.executable, serial_checks[0].remediation)
                self.assertEqual("bleak" in {check.name for check in checks}, scope == "hardware")

    def test_missing_esptool_has_copy_pasteable_install_remediation(self) -> None:
        with mock.patch.object(doctor, "resolve_esptool_argv", return_value=None):
            check = doctor._esptool_check(Path("/repo"), mock.Mock())

        self.assertFalse(check.ok)
        self.assertIn("./litch bootstrap firmware", check.remediation)
        self.assertIn("python3 -m pip install esptool==4.9.0", check.remediation)

    def test_firmware_scope_checks_esptool(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "platformio.ini").write_text("[platformio]\n", encoding="utf-8")
            runner = mock.Mock()
            runner.run.return_value.stdout = "Active USB configuration verification passed\n"
            runner.run.return_value.stderr = ""
            with (
                mock.patch.object(
                    doctor,
                    "_platformio_check",
                    return_value=Check("platformio", True, "6.1.19"),
                ),
                mock.patch.object(
                    doctor,
                    "_esptool_check",
                    return_value=Check("esptool", True, "5.3.1"),
                ) as esptool_check,
            ):
                checks = collect_checks(root, "firmware", runner)

        esptool_check.assert_called_once_with(root, runner)
        self.assertIn("esptool", {check.name for check in checks})

    def test_commented_port_examples_are_not_active(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "platformio.ini"
            config.write_text(
                "[env:x]\n; upload_port = /dev/example\n# monitor_port = /dev/example\n",
                encoding="utf-8",
            )
            self.assertEqual(active_port_overrides(config), ())

    def test_active_port_overrides_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / "platformio.ini"
            config.write_text(
                "[env:x]\nupload_port=/dev/a\n  test_port = /dev/b\n",
                encoding="utf-8",
            )
            self.assertEqual(active_port_overrides(config), ("upload_port", "test_port"))

    def test_render_includes_remediation_for_failure(self) -> None:
        rendered = render_checks((Check("thing", False, "missing", "install it"),))
        self.assertIn("FAIL thing", rendered)
        self.assertIn("remedy: install it", rendered)

    def test_host_contract_rejects_native_windows_and_wsl(self) -> None:
        for platform_name in ("win32", "cygwin", "msys"):
            with (
                self.subTest(platform=platform_name),
                mock.patch.object(doctor.sys, "platform", platform_name),
                mock.patch.object(
                    doctor.platform,
                    "release",
                    return_value="Windows-11",
                ),
            ):
                windows_host = doctor._host_check()
            self.assertFalse(windows_host.ok)
            self.assertIn("Windows", windows_host.remediation)

        with (
            mock.patch.object(doctor.sys, "platform", "linux"),
            mock.patch.object(
                doctor.platform,
                "release",
                return_value="6.6.0-microsoft-standard-WSL2",
            ),
        ):
            wsl = doctor._host_check()
        self.assertFalse(wsl.ok)
        self.assertIn("WSL", wsl.remediation)


if __name__ == "__main__":
    unittest.main()
