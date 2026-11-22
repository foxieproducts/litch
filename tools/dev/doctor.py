"""Read-only dependency and repository diagnostics for ``./litch doctor``."""

from __future__ import annotations

import importlib.util
import json
import platform
import re
import shlex
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .runner import DevError, Runner
from .tool_requirements import (
    ESPTOOL_INSTALL_REMEDIATION,
    PYSERIAL_REQUIREMENT,
    firmware_python_setup_remediation,
    resolve_esptool_argv,
)


ACTIVE_PORT_OVERRIDE_RE = re.compile(
    r"^\s*(upload_port|monitor_port|test_port)\s*=", re.MULTILINE
)
PIO_VERSION_RE = re.compile(r"PlatformIO Core, version (?P<version>\d+(?:\.\d+){1,2})")
ESPTOOL_VERSION_RE = re.compile(r"(?:esptool(?:\.py)?\s+v?|^)(?P<version>\d+\.\d+(?:\.\d+)?)", re.MULTILINE)


@dataclass(frozen=True)
class Check:
    name: str
    ok: bool
    detail: str
    remediation: str = ""


def active_port_overrides(platformio_ini: Path) -> tuple[str, ...]:
    content = platformio_ini.read_text(encoding="utf-8")
    return tuple(match.group(1) for match in ACTIVE_PORT_OVERRIDE_RE.finditer(content))


def _executable(name: str) -> Check:
    path = shutil.which(name)
    if path:
        return Check(name, True, str(Path(path).resolve()))
    return Check(name, False, "not found in PATH", f"install {name} and rerun ./litch doctor")


def _python_check() -> Check:
    version = ".".join(str(part) for part in sys.version_info[:3])
    ok = sys.version_info >= (3, 9)
    return Check(
        "python",
        ok,
        f"{version} ({sys.executable})",
        "install Python 3.9 or newer" if not ok else "",
    )


def _host_check() -> Check:
    release = platform.release()
    native_linux = sys.platform.startswith("linux") and "microsoft" not in release.lower()
    if sys.platform == "darwin":
        return Check("host", True, f"macOS ({release})")
    if native_linux:
        return Check("host", True, f"native Linux ({release})")
    return Check(
        "host",
        False,
        f"unsupported {sys.platform} ({release})",
        "run ./litch on macOS or native Linux; Windows, WSL, Cygwin, and MSYS2 are unvalidated",
    )


def _platformio_check(workspace: Path, runner: Runner) -> Check:
    executable = shutil.which("pio")
    if executable is None:
        return Check(
            "platformio",
            False,
            "pio not found in PATH",
            firmware_python_setup_remediation(),
        )
    try:
        result = runner.run(
            [executable, "--version"],
            cwd=workspace,
            timeout_seconds=10,
            capture=True,
        )
    except DevError as error:
        return Check("platformio", False, str(error), "repair the PlatformIO installation")
    match = PIO_VERSION_RE.search(result.stdout + result.stderr)
    if match is None:
        return Check("platformio", False, "unrecognized version output", "install PlatformIO 6.x")
    version = match.group("version")
    ok = version.split(".", 1)[0] == "6"
    return Check(
        "platformio",
        ok,
        f"{version} ({Path(executable).resolve()})",
        "install PlatformIO 6.x" if not ok else "",
    )


def _flutter_check() -> Check:
    executable = shutil.which("flutter")
    if executable is None:
        return Check(
            "flutter",
            False,
            "not found in PATH",
            "install the stable Flutter SDK and rerun ./litch doctor --scope flutter",
        )
    resolved = Path(executable).resolve()
    sdk_root = resolved.parent.parent
    metadata = sdk_root / "bin" / "cache" / "flutter.version.json"
    if not metadata.is_file():
        return Check(
            "flutter",
            False,
            f"SDK found at {sdk_root}, but {metadata.name} is missing",
            "run Flutter's documented SDK bootstrap, then rerun doctor",
        )
    try:
        decoded = json.loads(metadata.read_text(encoding="utf-8"))
        version = str(decoded["flutterVersion"])
        channel = str(decoded["channel"])
        dart = str(decoded["dartSdkVersion"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        return Check("flutter", False, f"invalid SDK metadata: {error}", "repair the Flutter SDK")
    ok = channel == "stable"
    detail = f"{version} stable={channel == 'stable'} dart={dart} ({sdk_root})"
    return Check(
        "flutter",
        ok,
        detail,
        "switch the Flutter SDK to the stable channel" if not ok else "",
    )


def _esptool_check(workspace: Path, runner: Runner) -> Check:
    try:
        esptool_argv = resolve_esptool_argv()
    except RuntimeError as error:
        return Check("esptool", False, str(error), ESPTOOL_INSTALL_REMEDIATION)
    if esptool_argv is None:
        return Check(
            "esptool",
            False,
            "not found as a command or Python module",
            ESPTOOL_INSTALL_REMEDIATION,
        )
    try:
        result = runner.run(
            [*esptool_argv, "version"],
            cwd=workspace,
            timeout_seconds=10,
            capture=True,
        )
    except DevError as error:
        return Check("esptool", False, str(error), ESPTOOL_INSTALL_REMEDIATION)
    output = result.stdout + result.stderr
    match = ESPTOOL_VERSION_RE.search(output)
    if match is None:
        return Check(
            "esptool",
            False,
            "unrecognized version output",
            ESPTOOL_INSTALL_REMEDIATION,
        )
    version = match.group("version")
    numbers = tuple(int(value) for value in version.split("."))
    major = numbers[0]
    ok = (major == 4 and numbers >= (4, 9)) or major == 5
    return Check(
        "esptool",
        ok,
        f"{version} ({shlex.join(esptool_argv)})",
        ESPTOOL_INSTALL_REMEDIATION if not ok else "",
    )


def _module_check(module: str, install_name: str) -> Check:
    available = importlib.util.find_spec(module) is not None
    remediation = f"install {install_name} into {sys.executable}"
    if module == "serial":
        remediation = (
            f"run ./litch bootstrap firmware for {sys.executable}; "
            f"if pip is missing or installation is blocked, {firmware_python_setup_remediation()}"
        )
    return Check(
        module,
        available,
        "available" if available else "not importable by this Python",
        remediation if not available else "",
    )


def collect_checks(workspace: Path, scope: str, runner: Runner) -> list[Check]:
    checks = [_python_check(), _host_check(), _executable("git")]
    requested = {scope} if scope != "all" else {"firmware", "flutter", "release"}
    if "hardware" in requested:
        requested.add("firmware")

    if "firmware" in requested:
        checks.append(_platformio_check(workspace, runner))
        checks.append(_esptool_check(workspace, runner))
        checks.append(_module_check("serial", PYSERIAL_REQUIREMENT))
        overrides = active_port_overrides(workspace / "platformio.ini")
        checks.append(
            Check(
                "port-overrides",
                not overrides,
                "none" if not overrides else f"active settings: {', '.join(overrides)}",
                "remove committed upload/monitor/test port overrides" if overrides else "",
            )
        )
        verifier = workspace / "tools" / "dev" / "verify_usb_configuration.py"
        try:
            result = runner.run(
                [sys.executable, str(verifier)],
                cwd=workspace,
                timeout_seconds=30,
                capture=True,
            )
            detail = result.stdout.strip() or "passed"
            checks.append(Check("usb-config", True, detail))
        except DevError as error:
            checks.append(Check("usb-config", False, str(error), "repair board/USB configuration"))

    if "flutter" in requested:
        checks.append(_flutter_check())
        checks.append(
            Check(
                "flutter-project",
                (workspace / "app" / "pubspec.yaml").is_file(),
                str(workspace / "app" / "pubspec.yaml"),
                "restore app/pubspec.yaml",
            )
        )

    if "release" in requested:
        checks.append(
            Check(
                "release-tests",
                (workspace / "tools" / "release" / "tests").is_dir(),
                "tools/release/tests",
                "restore release tooling tests",
            )
        )

    if "hardware" in requested:
        checks.append(_module_check("bleak", "bleak"))
    return checks


def render_checks(checks: Iterable[Check]) -> str:
    lines: list[str] = []
    for check in checks:
        marker = "OK" if check.ok else "FAIL"
        lines.append(f"{marker:4} {check.name:18} {check.detail}")
        if not check.ok and check.remediation:
            lines.append(f"     remedy: {check.remediation}")
    return "\n".join(lines)


def run_doctor(workspace: object, scope: str, runner: Runner) -> None:
    root = Path(getattr(workspace, "root"))
    print(getattr(workspace, "describe")())
    checks = collect_checks(root, scope, runner)
    print(render_checks(checks))
    failures = [check.name for check in checks if not check.ok]
    if failures:
        raise DevError(f"doctor failed: {', '.join(failures)}")
