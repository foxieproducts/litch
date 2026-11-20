from __future__ import annotations

from pathlib import Path
import sys
import unittest
from unittest.mock import patch


CODE_HEALTH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CODE_HEALTH_DIR))

import host_contract  # noqa: E402


class HostContractTests(unittest.TestCase):
    def test_native_macos_is_supported(self) -> None:
        contract = host_contract.require_supported_host(
            system="Darwin",
            release="25.0.0",
            version="Darwin Kernel Version 25",
            environ={},
        )

        self.assertEqual(contract.kind, "macos")
        self.assertTrue(contract.supported)

    def test_native_linux_is_supported(self) -> None:
        contract = host_contract.require_supported_host(
            system="Linux",
            release="6.8.0-generic",
            version="#1 SMP PREEMPT_DYNAMIC",
            environ={},
        )

        self.assertEqual(contract.kind, "linux")
        self.assertTrue(contract.supported)

    def test_wsl_release_marker_is_rejected(self) -> None:
        contract = host_contract.detect_host_contract(
            system="Linux",
            release="5.15.153.1-microsoft-standard-WSL2",
            version="#1 SMP",
            environ={},
        )

        self.assertEqual(contract.kind, "wsl")
        self.assertFalse(contract.supported)

    def test_wsl_environment_marker_is_rejected(self) -> None:
        contract = host_contract.detect_host_contract(
            system="Linux",
            release="6.8.0-generic",
            version="#1 SMP",
            environ={"WSL_INTEROP": "/run/WSL/1_interop"},
        )

        self.assertEqual(contract.kind, "wsl")
        self.assertFalse(contract.supported)

    def test_windows_is_rejected_calmly(self) -> None:
        with self.assertRaisesRegex(
            host_contract.UnsupportedHostError,
            r"unsupported host \(windows\).*only native macOS and native Linux",
        ) as raised:
            host_contract.require_supported_host(
                system="Windows",
                release="11",
                version="10.0.26100",
                environ={},
            )

        self.assertIsInstance(raised.exception, ValueError)

    def test_cygwin_system_or_environment_is_rejected(self) -> None:
        cases = (
            ("CYGWIN_NT-10.0-22631", {}),
            ("Windows", {"OSTYPE": "cygwin"}),
        )
        for system, environ in cases:
            with self.subTest(system=system, environ=environ):
                contract = host_contract.detect_host_contract(
                    system=system,
                    release="3.5.4",
                    version="",
                    environ=environ,
                )
                self.assertEqual(contract.kind, "cygwin")
                self.assertFalse(contract.supported)

    def test_msys2_and_mingw_are_rejected(self) -> None:
        cases = (
            ("MSYS_NT-10.0-22631", {}),
            ("MINGW64_NT-10.0-22631", {}),
            ("Windows", {"MSYSTEM": "MINGW64"}),
        )
        for system, environ in cases:
            with self.subTest(system=system, environ=environ):
                contract = host_contract.detect_host_contract(
                    system=system,
                    release="3.5.4",
                    version="",
                    environ=environ,
                )
                self.assertEqual(contract.kind, "msys2")
                self.assertFalse(contract.supported)

    def test_unknown_host_names_the_problem_without_platform_work(self) -> None:
        contract = host_contract.detect_host_contract(
            system="Haiku",
            release="r1beta5",
            version="",
            environ={},
        )

        self.assertEqual(contract.kind, "unsupported")
        self.assertIn("Haiku", contract.detail)

    def test_default_detection_snapshots_platform_and_environment(self) -> None:
        with patch.object(host_contract.platform, "system", return_value="Linux"), patch.object(
            host_contract.platform, "release", return_value="6.8.0"
        ), patch.object(host_contract.platform, "version", return_value="#1 SMP"), patch.object(
            host_contract.os, "environ", {}
        ):
            contract = host_contract.require_supported_host()

        self.assertEqual(contract.kind, "linux")


if __name__ == "__main__":
    unittest.main()
