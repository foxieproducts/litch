from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
DEV = REPO_ROOT / "dev"
LITCH = REPO_ROOT / "litch"
MINIMUM_PYTHON = (3, 9)


def _version(executable: str | Path) -> tuple[int, int, int]:
    probe = subprocess.run(
        [str(executable), "-c", "import sys; print(*sys.version_info[:3])"],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    )
    major, minor, micro = probe.stdout.split()
    return int(major), int(minor), int(micro)


class EntrypointTests(unittest.TestCase):
    def test_litch_is_the_primary_launcher(self) -> None:
        result = subprocess.run(
            [sys.executable, str(LITCH), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertIn("usage: ./litch", result.stdout)

    def test_system_python_39_runs_primary_help(self) -> None:
        system_python = Path("/usr/bin/python3")
        if not system_python.is_file() or _version(system_python) < MINIMUM_PYTHON:
            self.skipTest("system Python 3.9+ is not available")
        result = subprocess.run(
            [str(system_python), str(LITCH), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertIn("usage: ./litch", result.stdout)
        self.assertNotIn("Traceback", result.stderr)

    def _assert_launcher_result(
        self,
        result: subprocess.CompletedProcess[str],
        version: tuple[int, int, int],
    ) -> None:
        if version < MINIMUM_PYTHON:
            self.assertEqual(result.returncode, 2, result)
            self.assertIn("./litch requires Python 3.9 or newer", result.stderr)
            self.assertIn("ensure `python3` on PATH resolves to it", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result)
            self.assertIn("usage: ./litch", result.stdout)

    def test_system_python_exercises_real_launcher_guard(self) -> None:
        system_python = Path("/usr/bin/python3")
        if not system_python.is_file():
            self.skipTest("/usr/bin/python3 is not available")
        result = subprocess.run(
            [str(system_python), str(LITCH), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self._assert_launcher_result(result, _version(system_python))

    def test_shebang_uses_the_path_default_interpreter_safely(self) -> None:
        default_python = subprocess.run(
            ["/usr/bin/env", "python3", "-c", "import sys; print(sys.executable)"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
        result = subprocess.run(
            [str(LITCH), "--help"],
            cwd=REPO_ROOT,
            env=os.environ.copy(),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self._assert_launcher_result(result, _version(default_python))

    @unittest.skipUnless(sys.version_info >= MINIMUM_PYTHON, "requires supported Python")
    def test_windows_hosts_fail_with_explicit_host_diagnostic(self) -> None:
        probe = (
            "import runpy,sys; "
            "sys.platform=sys.argv[1]; "
            "runpy.run_path(sys.argv[2], run_name='__main__')"
        )
        for platform_name, diagnostic in (
            ("win32", "Native Windows"),
            ("cygwin", "Cygwin"),
            ("msys", "MSYS2"),
        ):
            with self.subTest(platform=platform_name):
                result = subprocess.run(
                    [sys.executable, "-c", probe, platform_name, str(LITCH)],
                    cwd=REPO_ROOT,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                self.assertEqual(result.returncode, 2, result)
                self.assertIn("supports macOS and native Linux hosts", result.stderr)
                self.assertIn(diagnostic, result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    @unittest.skipUnless(sys.version_info >= MINIMUM_PYTHON, "requires supported Python")
    def test_unvalidated_wsl_host_is_rejected(self) -> None:
        probe = (
            "import platform,runpy,sys; "
            "sys.platform='linux'; "
            "platform.release=lambda: '6.6.0-microsoft-standard-WSL2'; "
            "runpy.run_path(sys.argv[1], run_name='__main__')"
        )
        result = subprocess.run(
            [sys.executable, "-c", probe, str(LITCH)],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 2, result)
        self.assertIn("WSL", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    @unittest.skipUnless(sys.version_info >= MINIMUM_PYTHON, "requires supported Python")
    def test_supported_python_runs_cli_help(self) -> None:
        result = subprocess.run(
            [sys.executable, str(LITCH), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self._assert_launcher_result(result, tuple(sys.version_info[:3]))

    @unittest.skipUnless(sys.version_info >= MINIMUM_PYTHON, "requires supported Python")
    def test_supported_python_runs_release_doctor(self) -> None:
        result = subprocess.run(
            [sys.executable, str(LITCH), "doctor", "--scope", "release"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertIn(result.returncode, (0, 1), result)
        self.assertIn("OK   python", result.stdout)
        self.assertIn("OK   host", result.stdout)
        self.assertNotIn("Traceback", result.stderr)

    def test_dev_is_a_compatibility_alias_for_litch(self) -> None:
        result = subprocess.run(
            [str(DEV), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result)
        self.assertIn("usage: ./litch", result.stdout)


if __name__ == "__main__":
    unittest.main()
