from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.dev import tool_requirements


class ToolRequirementsTests(unittest.TestCase):







    def test_firmware_environment_guidance_quotes_the_launcher_python(self) -> None:
        with mock.patch.object(tool_requirements.sys, "executable", "/fixture/python with spaces/bin/python3"):
            guidance = tool_requirements.firmware_python_setup_remediation()

        self.assertIn("'/fixture/python with spaces/bin/python3' -m venv .venv", guidance)
        self.assertIn(". .venv/bin/activate", guidance)
        self.assertIn("platformio==6.1.19", guidance)
        self.assertIn("./litch bootstrap firmware", guidance)
        self.assertIn("distribution's Python venv package", guidance)

    def test_esptool_console_script_is_used_when_module_is_unavailable(self) -> None:
        with (
            mock.patch.object(
                tool_requirements.importlib.util,
                "find_spec",
                return_value=None,
            ),
            mock.patch.object(
                tool_requirements.shutil,
                "which",
                return_value="/opt/bin/esptool",
            ),
        ):
            self.assertEqual(
                tool_requirements.resolve_esptool_argv(),
                (str(Path("/opt/bin/esptool").resolve()),),
            )

    def test_current_python_module_works_without_console_script_on_path(self) -> None:
        with (
            mock.patch.object(tool_requirements.shutil, "which", return_value=None),
            mock.patch.object(
                tool_requirements.importlib.util,
                "find_spec",
                return_value=mock.sentinel.module,
            ),
        ):
            self.assertEqual(
                tool_requirements.resolve_esptool_argv(),
                (str(Path(sys.executable)), "-m", "esptool"),
            )

    def test_current_python_module_wins_over_an_incompatible_path_copy(self) -> None:
        with (
            mock.patch.object(
                tool_requirements.importlib.util,
                "find_spec",
                return_value=mock.sentinel.module,
            ),
            mock.patch.object(
                tool_requirements.shutil,
                "which",
                return_value="/old/bin/esptool",
            ) as which,
        ):
            self.assertEqual(
                tool_requirements.resolve_esptool_argv(),
                (str(Path(sys.executable)), "-m", "esptool"),
            )
        which.assert_not_called()

    def test_missing_console_script_and_module_returns_none(self) -> None:
        with (
            mock.patch.object(tool_requirements.shutil, "which", return_value=None),
            mock.patch.object(
                tool_requirements.importlib.util,
                "find_spec",
                return_value=None,
            ),
        ):
            self.assertIsNone(tool_requirements.resolve_esptool_argv())

    def test_platformio_managed_esptool_is_the_final_fallback(self) -> None:
        def which(command: str):
            return "/opt/bin/pio" if command == "pio" else None

        with (
            mock.patch.object(tool_requirements.shutil, "which", side_effect=which),
            mock.patch.object(
                tool_requirements.importlib.util,
                "find_spec",
                return_value=None,
            ),
        ):
            self.assertEqual(
                tool_requirements.resolve_esptool_argv(),
                (
                    str(Path("/opt/bin/pio").resolve()),
                    "pkg",
                    "exec",
                    "--package",
                    "tool-esptoolpy",
                    "--",
                    "esptool.py",
                ),
            )


if __name__ == "__main__":
    unittest.main()
