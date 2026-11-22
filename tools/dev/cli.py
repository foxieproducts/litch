"""Implementation of the canonical ``./litch`` command surface."""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import shlex
import shutil
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Sequence

from .checks import run_tools_check
from .build_paths import platformio_build_directory, platformio_sleep_dependencies
from .doctor import run_doctor
from .runner import DevError, Runner
from .tool_requirements import firmware_python_setup_remediation, pyserial_install_argv
from .workspace import (
    Workspace,
    WorkspaceError,
    accepted_workspace_selectors,
    list_workspaces,
)
from .locks import LockError, LockManager
from .local_state import read as read_local_state, remember as remember_local_state

if TYPE_CHECKING:
    from .factory_camera import CameraChoice


REPO_ROOT = Path(__file__).resolve().parents[2]
PIO_ENVIRONMENTS = {
    "c3": "litch-esp32c3",
    "s3": "litch-esp32s3",
}
COMMAND_TIMEOUT_SECONDS = {
    "bootstrap": 20 * 60,
    "native": 30 * 60,
    "build": 30 * 60,
    "flutter": 30 * 60,
    "flutter-test": 2 * 60,
    "release": 10 * 60,
    "code-health": 60 * 60,
}


def _numbered_choice(title: str, options: Sequence[str], *, default: int = 1) -> int:
    """Read one calm numbered menu choice from an interactive terminal."""

    print(title)
    for index, option in enumerate(options, start=1):
        marker = " (default)" if index == default else ""
        print(f"  {index}. {option}{marker}")
    while True:
        raw = input(f"Choice [{default}, q returns]: ").strip()
        if raw.lower() == "q":
            raise DevError("Returned to the device list.", exit_code=2)
        if not raw:
            return default
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return int(raw)
        print(f"Enter a number from 1 to {len(options)}.")


def _interactive_command() -> list[str]:
    """Return the direct command taught by the no-argument menu."""

    state = read_local_state(REPO_ROOT / ".cache" / "dev-local-state.json")
    remembered_app_build = None if state is None else state.get("last_app_build")
    remembered_ios = state is not None and isinstance(state.get("ios_flutter"), str)
    remembered_android = state is not None and isinstance(state.get("android_flutter"), str)
    if remembered_app_build in {"ios", "android"}:
        app_default = 1 if remembered_app_build == "ios" else 2
    else:
        app_default = 2 if remembered_android and not remembered_ios else 1

    choice = _numbered_choice(
        "What do you want to do?",
        (
            "Build firmware",
            "Build the app",
            "Monitor a board",
            "Install the app",
            "Run the app",
            "Run tests or checks",
        ),
    )
    if choice == 1:
        return ["build", "s3"]
    if choice == 2:
        platform = _numbered_choice(
            "Which app?", ("iOS", "Android"), default=app_default
        )
        return ["app", "build", ("ios", "android")[platform - 1]]
    if choice == 3:
        return ["monitor"]
    if choice == 4:
        return ["app", "install"]
    if choice == 5:
        return ["app", "run"]

    check = _numbered_choice(
        "What should Litch verify?",
        (
            "Native firmware suite",
            "Flutter test file",
            "Flutter analysis",
            "Developer tooling",
            "Release tooling",
        ),
    )
    if check == 1:
        suite = input("Native suite (for example test_trace_replay): ").strip()
        if not suite:
            raise DevError("Select a native suite with --filter.", exit_code=2)
        return ["test", "native", "--filter", suite]
    if check == 2:
        path = input("App-relative test file (test/..._test.dart): ").strip()
        if not path:
            raise DevError("Select an app test file.", exit_code=2)
        return ["app", "test", "--", path]
    return (
        ["flutter", "analyze"],
        ["check", "tools"],
        ["release", "check"],
    )[check - 3]


class _StoreOnce(argparse.Action):
    """Reject duplicated identity selectors instead of silently taking one."""

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: str,
        option_string: str | None = None,
    ) -> None:
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"{option_string or self.dest} may be specified only once")
        setattr(namespace, self.dest, values)


class _FlutterActionParser(argparse.ArgumentParser):
    """Keep Flutter's ``--`` passthrough separate from the platform positional."""

    def parse_known_args(self, args=None, namespace=None):
        values = list(sys.argv[1:] if args is None else args)
        try:
            separator = values.index("--")
        except ValueError:
            prefix, passthrough = values, []
        else:
            prefix, passthrough = values[:separator], values[separator:]
        if self.prog.rsplit(" ", 1)[-1] in {"run", "install"}:
            prefix = self._normalize_execution_arguments(prefix)
        parsed, unknown = super().parse_known_args(prefix, namespace)
        if passthrough:
            parsed.flutter_args = passthrough
        return parsed, unknown

    def _normalize_execution_arguments(self, values: list[str]) -> list[str]:
        """Allow dev-owned options on either side of the optional platform."""

        options: list[str] = []
        platforms: list[str] = []
        index = 0
        while index < len(values):
            value = values[index]
            if value in {"ios", "android"}:
                platforms.append(value)
            elif value == "--device":
                options.append(value)
                if index + 1 < len(values):
                    options.append(values[index + 1])
                    index += 1
            else:
                options.append(value)
            index += 1
        if len(platforms) > 1:
            self.error("platform may be specified only once")
        return [*options, *platforms]


def _firmware_target(value: str) -> str:
    if value not in PIO_ENVIRONMENTS:
        raise argparse.ArgumentTypeError("unsupported firmware target")
    return value


def _single_firmware_target(value: str) -> str:
    if value not in PIO_ENVIRONMENTS:
        raise argparse.ArgumentTypeError("unsupported firmware target")
    return value


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="./litch",
        description="Editor-neutral Litch development, verification, and exact-device tooling.",
    )
    parser.add_argument(
        "--worktree",
        help="same-repository worktree path, directory selector, or branch (default: this checkout)",
    )
    parser.add_argument(
        "--lock-timeout",
        type=float,
        default=30.0,
        metavar="SECONDS",
        help="bounded wait for a writer/device lock (default: 30)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    doctor = subparsers.add_parser("doctor", help="run read-only dependency and repository diagnostics")
    doctor.add_argument(
        "--scope",
        choices=("all", "firmware", "flutter", "release", "hardware"),
        default="all",
    )

    bootstrap = subparsers.add_parser("bootstrap", help="hydrate declared project dependencies")
    bootstrap.add_argument("scope", choices=("all", "firmware", "flutter"), nargs="?", default="all")

    test = subparsers.add_parser("test", help="run a canonical test lane")
    test_subparsers = test.add_subparsers(dest="test_command", required=True)
    native = test_subparsers.add_parser("native", help="run native firmware tests")
    native_scope = native.add_mutually_exclusive_group(required=True)
    native_scope.add_argument("--filter", action="append", dest="filters", metavar="SUITE")
    native_scope.add_argument("--all", action="store_true", help="explicitly run the native aggregate")

    build = subparsers.add_parser("build", help="build firmware without touching a device")
    build.add_argument(
        "target",
        type=_firmware_target,
        metavar="TARGET",
        nargs="?",
        help="firmware target (default: S3; legacy targets require an explicit name)",
    )

    build.add_argument(
        "--compile-only",
        action="store_true",
        help="compile and verify firmware artifacts without running behavioral checks",
    )

    binary = subparsers.add_parser("bin", help="print a built firmware.bin path")
    binary.add_argument(
        "target",
        type=_single_firmware_target,
        metavar="TARGET",
        help="explicit firmware target",
    )

    flutter = subparsers.add_parser(
        "app",
        aliases=("flutter",),
        help="build, test, install, or run the Flutter app",
    )
    flutter_subparsers = flutter.add_subparsers(
        dest="action", required=True, parser_class=_FlutterActionParser
    )
    flutter_test = flutter_subparsers.add_parser("test", help="run Flutter tests")
    flutter_test.add_argument("--all", action="store_true", help="explicitly run all Flutter tests")
    flutter_test.add_argument(
        "flutter_args", nargs=argparse.REMAINDER, help="arguments after -- are passed through"
    )
    flutter_analyze = flutter_subparsers.add_parser("analyze", help="run Flutter analysis")
    flutter_analyze.add_argument(
        "flutter_args", nargs=argparse.REMAINDER, help="arguments after -- are passed through"
    )
    flutter_build = flutter_subparsers.add_parser("build", help="build the app for one platform")
    flutter_build.add_argument("platform", choices=("ios", "android"))
    flutter_build.add_argument(
        "flutter_args", nargs=argparse.REMAINDER, help="arguments after -- are passed through"
    )
    flutter_run = flutter_subparsers.add_parser("run", help="run the Flutter app")
    flutter_run.add_argument(
        "platform",
        choices=("ios", "android"),
        nargs="?",
        help="reuse the remembered live iOS or Android device",
    )
    flutter_run.add_argument("--device", action=_StoreOnce, help="Flutter device ID or Android hardware serial")
    flutter_run.add_argument("--release", action="store_true", help="run in release mode")
    flutter_run.add_argument(
        "flutter_args", nargs=argparse.REMAINDER, help="arguments after -- are passed through"
    )
    flutter_install = flutter_subparsers.add_parser(
        "install",
        help=(
            "build and non-residently deploy current source, then remember the device "
            "(iOS defaults to standalone profile mode)"
        ),
    )
    flutter_install.add_argument(
        "platform",
        choices=("ios", "android"),
        nargs="?",
        help="reuse the remembered live iOS or Android device",
    )
    flutter_install.add_argument("--device", action=_StoreOnce, help="Flutter device ID or Android hardware serial")
    flutter_install.add_argument("--release", action="store_true", help="install a release build")
    flutter_install.add_argument(
        "flutter_args", nargs=argparse.REMAINDER, help="arguments after -- are passed through"
    )

    release = subparsers.add_parser("release", help="run non-publishing release tooling")
    release.add_argument("action", choices=("check",))

    code_health = subparsers.add_parser(
        "code-health",
        help="run bounded code-health evidence tooling",
        add_help=False,
    )
    code_health.add_argument(
        "--help",
        action="store_true",
        dest="code_health_help",
        help=argparse.SUPPRESS,
    )
    code_health.add_argument("code_health_args", nargs=argparse.REMAINDER)

    devices = subparsers.add_parser("devices", help="list exact USB identities usable by device commands")
    devices.add_argument("--ble", action="store_true", help="also list Litch BLE addresses")

    flash = subparsers.add_parser("flash", help="build and flash physical firmware")
    flash.add_argument(
        "target",
        type=_single_firmware_target,
        metavar="TARGET",
        nargs="?",
        help="firmware target (default: S3; legacy targets require an explicit name)",
    )
    flash_transport = flash.add_mutually_exclusive_group()
    flash_transport.add_argument(
        "--device",
        action=_StoreOnce,
        help="USB serial (unique substring accepted) or exact serial-port path",
    )
    flash_transport.add_argument(
        "--ble-device",
        action=_StoreOnce,
        help="BLE-only OTA address/UUID (a unique short selector is accepted; excludes --device)",
    )
    flash_transport.add_argument(
        "--batch",
        action="store_true",
        help="build once and flash each next S3 board over USB; q exits, Ctrl-C interrupts",
    )
    flash.add_argument(
        "--factory-provision",
        "--factory",
        action="store_true",
        help="erase and provision one S3, or start a camera-assisted factory station with --batch",
    )
    flash.add_argument("--batch-name", help="factory batch name; otherwise ask after building")
    flash.add_argument("--records", metavar="CSV",
                       help="factory results CSV (default: ~/Documents/litch-factory-results.csv)")
    flash.add_argument("--resume", action="store_true",
                       help="reuse the recorded verifier in a --batch --factory session")
    flash.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="show subprocess command lines during discovery, build, and flash",
    )

    monitor = subparsers.add_parser(
        "monitor", aliases=("console",), help="monitor one exact physical board"
    )
    monitor.add_argument(
        "target",
        type=_single_firmware_target,
        metavar="TARGET",
        nargs="?",
        help="firmware target (default: S3; legacy targets require an explicit name)",
    )
    monitor.add_argument(
        "--device",
        action=_StoreOnce,
        help="USB serial (unique substring accepted) or exact serial-port path",
    )

    subparsers.add_parser("worktrees", help="list same-repository worktrees and selectors")

    check = subparsers.add_parser("check", help="run workflow/static checks")
    check.add_argument("scope", choices=("tools",))
    check.add_argument("--all", action="store_true", help="also run all developer-tool unit tests")
    return parser


def _pio_environment() -> dict[str, str]:
    return {"PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no"}


def _verify_usb_configuration(root: Path, runner: Runner, firmware: Path | None = None) -> None:
    argv: list[str | Path] = [
        sys.executable,
        root / "tools" / "dev" / "verify_usb_configuration.py",
    ]
    if firmware is not None:
        argv.extend(("--firmware", firmware))
    runner.run(argv, cwd=root, timeout_seconds=60, extra_env=_pio_environment())


def _bootstrap(workspace: Workspace, scope: str, runner: Runner, lock_timeout: float) -> None:
    manager = LockManager(workspace, argv=sys.argv)
    if scope in {"all", "firmware"}:
        with manager.lane("platformio", timeout=lock_timeout):
            runner.run(
                [
                    "pio", "pkg", "install", "--project-dir", workspace.root,
                    "-e", "native", "-e", "litch-esp32s3",
                ],
                cwd=workspace.root,
                timeout_seconds=COMMAND_TIMEOUT_SECONDS["bootstrap"],
                extra_env=_pio_environment(),
            )
            try:
                runner.run(
                    pyserial_install_argv(),
                    cwd=workspace.root,
                    timeout_seconds=COMMAND_TIMEOUT_SECONDS["bootstrap"],
                    capture=True,
                )
            except DevError as error:
                raise DevError(
                    f"{error}\nIf pip is missing or this Python cannot install packages "
                    "(including an externally managed environment), "
                    f"{firmware_python_setup_remediation()}",
                    exit_code=error.exit_code,
                ) from error
    if scope in {"all", "flutter"}:
        with manager.lane("flutter", timeout=lock_timeout):
            runner.run(
                ["flutter", "pub", "get"],
                cwd=workspace.root / "app",
                timeout_seconds=COMMAND_TIMEOUT_SECONDS["bootstrap"],
            )


def _native_test(workspace: Workspace, filters: Sequence[str] | None, runner: Runner, lock_timeout: float) -> None:
    argv: list[str] = ["pio", "test", "-e", "native"]
    for suite in filters or ():
        argv.extend(("-f", suite))
    with LockManager(workspace, argv=sys.argv).lane("platformio", timeout=lock_timeout):
        runner.run(
            argv,
            cwd=workspace.root,
            timeout_seconds=COMMAND_TIMEOUT_SECONDS["native"],
            extra_env=_pio_environment(),
        )


def _build_one(root: Path, target: str, runner: Runner, *, compile_only: bool = False) -> None:
    environment = PIO_ENVIRONMENTS[target]
    build_dir = platformio_build_directory(root, environment)
    _verify_usb_configuration(root, runner)
    runner.run(
        ["pio", "run", "-e", environment],
        cwd=root,
        timeout_seconds=COMMAND_TIMEOUT_SECONDS["build"],
        extra_env=_pio_environment(),
    )
    if target == "s3":
        if not compile_only:
            sdk, json_include = platformio_sleep_dependencies(root, environment)
            runner.run(
                [sys.executable, root / "tools" / "dev" / "check_sleep_sdk_wake.py",
                 "--sdk", sdk, "--json-include", json_include],
                cwd=root,
                timeout_seconds=60,
                extra_env=_pio_environment(),
            )
        _verify_usb_configuration(root, runner, build_dir / "firmware.elf")


def _build(
    workspace: Workspace, target: str, runner: Runner, lock_timeout: float, *, compile_only: bool = False,
) -> None:
    with LockManager(workspace, argv=sys.argv).lane("platformio", timeout=lock_timeout):
        _build_one(workspace.root, target, runner, compile_only=compile_only)


def _build_target(workspace: Workspace, requested: str | None) -> tuple[str, bool]:
    """Resolve a firmware build target without exposing legacy targets by default."""

    if requested is not None:
        return requested, False
    return "s3", False


def _firmware_bin(workspace: Workspace, target: str) -> Path:
    return platformio_build_directory(workspace.root, PIO_ENVIRONMENTS[target]) / "firmware.bin"


def _flutter_devices(workspace: Workspace, runner: Runner) -> tuple[tuple[str, str], ...]:
    result = runner.run(
        ["flutter", "devices", "--machine"],
        cwd=workspace.root / "app",
        timeout_seconds=30,
        capture=True,
        check=False,
    )
    if result.returncode != 0:
        raise DevError((result.stderr or result.stdout or "flutter devices failed").strip())
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise DevError("flutter devices returned invalid JSON") from error
    if not isinstance(payload, list):
        raise DevError("flutter devices returned an invalid device list")
    devices = []
    for item in payload:
        if not isinstance(item, dict):
            continue
        identifier, platform = item.get("id"), item.get("targetPlatform")
        if isinstance(identifier, str) and isinstance(platform, str):
            family = "ios" if platform.startswith("ios") else "android" if platform.startswith("android") else None
            if family is not None:
                devices.append((identifier, family))
    return tuple(devices)


def _prompt_flutter_device(
    devices: Sequence[tuple[str, str]], *, action: str
) -> tuple[str, str]:
    print("Choose the exact app device:")
    for index, (identifier, platform) in enumerate(devices, start=1):
        command = shlex.join(
            ["./litch", "app", action, platform, "--device", identifier]
        )
        print(f"  {index}. {command}")
    while True:
        raw = input("Choice: ").strip()
        if raw.isdigit() and 1 <= int(raw) <= len(devices):
            return devices[int(raw) - 1]
        print(f"Enter a number from 1 to {len(devices)}.")


def _adb_executable() -> str | None:
    if executable := shutil.which("adb"):
        return executable
    sdk_roots = [
        os.environ.get("ANDROID_SDK_ROOT"),
        os.environ.get("ANDROID_HOME"),
        str(Path.home() / "Library" / "Android" / "sdk"),
        str(Path.home() / "Android" / "Sdk"),
    ]
    for root in sdk_roots:
        if root and (candidate := Path(root) / "platform-tools" / "adb").is_file():
            return str(candidate)
    return None


def _android_hardware_serial(workspace: Workspace, runner: Runner, identifier: str) -> str | None:
    adb = _adb_executable()
    if adb is None:
        return None
    try:
        result = runner.run(
            [adb, "-s", identifier, "shell", "getprop", "ro.serialno"],
            cwd=workspace.root,
            timeout_seconds=5,
            capture=True,
            check=False,
            announce=False,
        )
    except DevError:
        return None
    serial = result.stdout.strip()
    return serial if result.returncode == 0 and serial and serial != "unknown" else None


def _select_flutter_device(
    workspace: Workspace,
    requested: str | None,
    runner: Runner,
    *,
    requested_platform: str | None = None,
    action: str = "run",
) -> tuple[str, str]:
    devices = _flutter_devices(workspace, runner)
    if requested:
        matches = [device for device in devices if requested.casefold() in device[0].casefold()]
        if not matches:
            for identifier, platform in devices:
                if platform != "android":
                    continue
                serial = _android_hardware_serial(workspace, runner, identifier)
                if serial is not None and serial.casefold() == requested.casefold():
                    matches.append((identifier, platform))
        if len(matches) == 1:
            if requested_platform is not None and matches[0][1] != requested_platform:
                raise DevError(
                    f"Flutter device {matches[0][0]!r} is {matches[0][1]}, "
                    f"not requested platform {requested_platform}"
                )
            return matches[0]
        if len(matches) > 1:
            listing = ", ".join(
                f"{identifier} ({platform})" for identifier, platform in matches
            )
            raise DevError(f"Flutter device selector {requested!r} is ambiguous: {listing}")
        listing = ", ".join(f"{identifier} ({platform})" for identifier, platform in devices)
        raise DevError(
            f"Flutter device {requested!r} is unavailable; "
            f"live iOS/Android targets: {listing or '(none)'}"
        )
    state = read_local_state(workspace.root / ".cache" / "dev-local-state.json")
    remembered = {
        (value, platform)
        for platform, slot in (("ios", "ios_flutter"), ("android", "android_flutter"))
        if state is not None and isinstance((value := state[slot]), str)
    }
    if requested_platform:
        platform_devices = [device for device in devices if device[1] == requested_platform]
        remembered_platform = [device for device in platform_devices if device in remembered]
        if not remembered_platform and requested_platform == "android" and state is not None:
            remembered_serial = state.get("android_flutter")
            if isinstance(remembered_serial, str) and ":" not in remembered_serial:
                remembered_platform = [
                    device for device in platform_devices
                    if _android_hardware_serial(workspace, runner, device[0]) == remembered_serial
                ]
        if len(remembered_platform) == 1:
            return remembered_platform[0]
        if len(platform_devices) == 1:
            return platform_devices[0]
        listing = ", ".join(f"{identifier} ({platform})" for identifier, platform in platform_devices)
        if not platform_devices:
            all_listing = ", ".join(
                f"{identifier} ({platform})" for identifier, platform in devices
            )
            raise DevError(
                f"no live {requested_platform} Flutter device; "
                f"live iOS/Android targets: {all_listing or '(none)'}"
            )
        if sys.stdin.isatty():
            return _prompt_flutter_device(platform_devices, action=action)
        raise DevError(
            f"no unambiguous {requested_platform} Flutter device; use --device <unique-short-id>: {listing}"
        )
    if len(devices) == 1:
        return devices[0]
    choices = [device for device in devices if device in remembered]
    if not choices and state is not None:
        remembered_serial = state.get("android_flutter")
        if isinstance(remembered_serial, str) and ":" not in remembered_serial:
            choices = [
                device for device in devices if device[1] == "android"
                and _android_hardware_serial(workspace, runner, device[0]) == remembered_serial
            ]
    if len(choices) == 1:
        return choices[0]
    if sys.stdin.isatty() and devices:
        return _prompt_flutter_device(devices, action=action)
    listing = ", ".join(f"{identifier} ({platform})" for identifier, platform in devices)
    raise DevError(
        "no unambiguous remembered Flutter device; "
        f"live iOS/Android targets: {listing or '(none)'}. "
        f"Choose './litch app {action} ios' or './litch app {action} android', "
        "or use --device <unique-short-id>."
    )


def _flutter(workspace: Workspace, args: argparse.Namespace, runner: Runner, lock_timeout: float) -> None:
    if args.action == "build":
        argv = ["flutter", "build", "ios" if args.platform == "ios" else "apk"]
    else:
        argv = ["flutter", "run" if args.action == "install" else args.action]
    selected_device: tuple[str, str] | None = None
    if args.action in {"run", "install"}:
        selected_device = _select_flutter_device(
            workspace,
            args.device,
            runner,
            requested_platform=getattr(args, "platform", None),
            action=args.action,
        )
        if args.action == "install":
            argv.append("--no-resident")
        argv.extend(("-d", selected_device[0]))
        if args.release:
            argv.append("--release")
        elif args.action == "install" and selected_device[1] == "ios":
            # Detached iOS debug builds require a resident Flutter tool on
            # every launch. Profile mode keeps runtime diagnostics available
            # while leaving an app that can be launched normally.
            argv.append("--profile")
    passthrough = list(args.flutter_args)
    if passthrough and passthrough[0] == "--":
        passthrough.pop(0)
    if args.action == "test":
        if not args.all:
            selected = []
            # Separate common option values from actual positional targets.
            # Other valued Flutter options can use --option=value, avoiding a
            # second copy of the SDK's full argument parser in this wrapper.
            value_options = {
                "--name", "--plain-name", "--concurrency", "-j", "--reporter", "-r",
                "--tags", "-t", "--exclude-tags", "-x", "--timeout",
            }
            remaining = iter(passthrough)
            for value in remaining:
                if value == "--":
                    selected.extend(remaining)
                    break
                if value in value_options:
                    if next(remaining, None) is None:
                        raise DevError(f"Missing Flutter option value: {value}", exit_code=2)
                elif not value.startswith("-"):
                    selected.append(value)
            if not selected:
                raise DevError(
                    "Select an app test file: ./litch app test -- test/<file>_test.dart. "
                    "Use --all only for an explicitly requested aggregate; use "
                    "tools/qa/interactive_sync.py --profile focused for validated journey selections.",
                    exit_code=2,
                )
            test_root = (workspace.root / "app" / "test").resolve()
            for value in selected:
                path = (workspace.root / "app" / value).resolve()
                if path.suffix != ".dart" or not path.is_file() or test_root not in path.parents:
                    raise DevError(f"Select an existing file inside app/test: {value}", exit_code=2)
        if not any(value in {"--pub", "--no-pub"} for value in passthrough):
            argv.append("--no-pub")
        if not any(value == "--concurrency" or value.startswith("--concurrency=")
                   for value in passthrough):
            argv.append("--concurrency=4" if args.all else "--concurrency=1")
    argv.extend(passthrough)
    if args.action == "build":
        platform_label = "iOS" if args.platform == "ios" else "Android"
        print(f"litch: building {platform_label} app", flush=True)
    interactive = args.action == "run"
    if selected_device is not None:
        identifier, platform = selected_device
        state = read_local_state(workspace.root / ".cache" / "dev-local-state.json")
        slot = f"{platform}_flutter"
        remembered = (
            args.device is None
            and state is not None
            and state.get(slot) == identifier
        )
        platform_label = "iOS" if platform == "ios" else "Android"
        if remembered:
            print(
                f"litch: using remembered {platform_label} device -> {identifier} "
                f"(override: ./litch app {args.action} {platform} "
                "--device <flutter-device-id>)",
                flush=True,
            )
        else:
            print(
                f"litch: selected {platform_label} device -> {identifier}",
                flush=True,
            )
    if selected_device is not None and args.action == "install":
        identifier, platform = selected_device
        mode = (
            "release"
            if args.release
            else "profile"
            if platform == "ios"
            else "debug"
        )
        platform_label = "iOS" if platform == "ios" else "Android"
        print(f"litch: {platform_label} {mode} install -> {identifier}", flush=True)
    with LockManager(workspace, argv=sys.argv).lane("flutter", timeout=lock_timeout):
        runner.run(
            argv,
            cwd=workspace.root / "app",
            timeout_seconds=None if interactive else COMMAND_TIMEOUT_SECONDS[
                "flutter-test" if args.action == "test" else "flutter"],
            interactive=interactive,
        )
    if args.action == "build":
        remember_local_state(
            workspace.root / ".cache" / "dev-local-state.json",
            "last_app_build",
            args.platform,
        )
    elif selected_device is not None and args.action in {"install", "run"}:
        identifier, platform = selected_device
        remembered_identifier = identifier
        if platform == "android" and ":" in identifier:
            remembered_identifier = _android_hardware_serial(workspace, runner, identifier)
        remember_local_state(
            workspace.root / ".cache" / "dev-local-state.json",
            f"{platform}_flutter",
            remembered_identifier,
        )


def _release_check(workspace: Workspace, runner: Runner, lock_timeout: float) -> None:
    from tools.release.verify_release_artifacts import VERIFICATION_TIMEOUT_SECONDS

    with LockManager(workspace, argv=sys.argv).lane("release", timeout=lock_timeout):
        runner.run(
            [sys.executable, "tools/release/verify_release_artifacts.py"],
            cwd=workspace.root,
            timeout_seconds=VERIFICATION_TIMEOUT_SECONDS,
        )


def _platformio_core_dir(root: Path, runner: Runner) -> Path:
    result = runner.run(
        ["pio", "system", "info", "--json-output"],
        cwd=root,
        timeout_seconds=30,
        capture=True,
        extra_env=_pio_environment(),
    )
    try:
        decoded = json.loads(result.stdout)
        value = decoded["core_dir"]["value"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise DevError("PlatformIO did not report a usable core directory") from error
    return Path(str(value)).expanduser().resolve()


def _list_devices(
    workspace: Workspace,
    runner: Runner,
    include_ble: bool,
    lock_timeout: float,
) -> None:
    from .hardware import list_devices_for_cli

    print(
        list_devices_for_cli(
            workspace,
            runner,
            include_ble=include_ble,
            lock_timeout=lock_timeout,
        )
    )


def _choose_factory_camera() -> CameraChoice | None:
    from .factory_camera import discover_cameras

    cameras = discover_cameras(adb_executable=_adb_executable())
    if not cameras:
        raise DevError("No factory cameras found. Connect a supported camera and try again.")
    print("Choose a factory camera:")
    choices = {str(index): camera for index, camera in enumerate(cameras, start=1)}
    for number, camera in choices.items():
        print(f"  {number}. {camera.label}")
    while True:
        try:
            number = input("Camera number (q to quit): ").strip()
        except EOFError:
            return None
        if number.lower() == "q":
            return None
        if number in choices:
            return choices[number]
        print(f"Enter a number from 1 to {len(choices)}.")


def _flash(workspace: Workspace, args: argparse.Namespace, runner: Runner, lock_timeout: float) -> None:
    if args.batch and args.target not in (None, "s3"):
        raise DevError("--batch requires the S3 USB target", exit_code=2)
    factory_batch = args.batch and args.factory_provision
    if not factory_batch and (args.batch_name is not None or args.records is not None or args.resume):
        raise DevError("--batch-name, --records and --resume require --batch --factory", exit_code=2)
    if factory_batch:
        from .factory_station import build_factory_firmware, run_factory_station

        firmware = build_factory_firmware(
            workspace=workspace,
            lock_timeout=lock_timeout,
            build_target=lambda target: _build_one(workspace.root, target, runner),
        )
        batch_name = args.batch_name
        while not batch_name:
            try:
                batch_name = input("Factory batch name (q to quit): ").strip()
            except EOFError:
                return
            if batch_name.lower() == "q":
                return
        camera_choice = _choose_factory_camera()
        if camera_choice is None:
            return
        run_factory_station(
            workspace=workspace,
            runner=runner,
            lock_timeout=lock_timeout,
            firmware=firmware,
            camera_choice=camera_choice,
            platformio_core_dir=lambda: _platformio_core_dir(workspace.root, runner),
            verbose=args.verbose,
            batch_name=batch_name,
            records_path=(Path(args.records).expanduser() if args.records
                          else Path.home() / "Documents" / "litch-factory-results.csv"),
            resume=args.resume,
        )
        return
    if args.batch:
        from .batch_flash import flash_batch

        flash_batch(
            workspace=workspace,
            runner=runner,
            lock_timeout=lock_timeout,
            build_target=lambda target: _build_one(workspace.root, target, runner),
            platformio_core_dir=lambda: _platformio_core_dir(workspace.root, runner),
            verbose=args.verbose,
            factory_provision=args.factory_provision,
        )
        return

    from .hardware import flash_selected_target

    flash_selected_target(
        workspace=workspace,
        requested_target=args.target,
        usb_selector=args.device,
        ble_selector=args.ble_device,
        runner=runner,
        lock_timeout=lock_timeout,
        build_target=lambda target: _build_one(workspace.root, target, runner),
        platformio_core_dir=lambda: _platformio_core_dir(workspace.root, runner),
        interactive=sys.stdin.isatty(),
        verbose=args.verbose,
        factory_provision=getattr(args, "factory_provision", False),
    )


def _monitor(workspace: Workspace, args: argparse.Namespace, runner: Runner, lock_timeout: float) -> None:
    from .hardware import monitor_selected_target

    monitor_selected_target(
        workspace=workspace,
        requested_target=args.target,
        usb_selector=args.device,
        runner=runner,
        lock_timeout=lock_timeout,
        interactive=sys.stdin.isatty(),
    )


def _print_worktrees(workspace: Workspace) -> None:
    candidates = list_workspaces(workspace.root)
    for candidate in candidates:
        print(candidate.describe())
        selectors = accepted_workspace_selectors(candidate, candidates)
        print(f"  recommended selector: {selectors[0]}")
        print(f"  accepted selectors: {', '.join(selectors)}")


def _code_health(
    workspace: Workspace,
    arguments: Sequence[str],
    runner: Runner,
    lock_timeout: float,
) -> None:
    """Run the single code-health implementation under its writing lanes."""

    toolchain_lane: str | None = None
    if arguments:
        if arguments[0] == "native-coverage":
            toolchain_lane = "platformio"
        elif arguments[0] == "flutter-coverage":
            toolchain_lane = "flutter"
        elif arguments[0] == "size-snapshot" and "--build" in arguments[1:]:
            toolchain_lane = "platformio"

    manager = LockManager(workspace, argv=sys.argv)
    with contextlib.ExitStack() as locks:
        locks.enter_context(manager.lane("code-health", timeout=lock_timeout))
        if toolchain_lane is not None:
            locks.enter_context(manager.lane(toolchain_lane, timeout=lock_timeout))
        runner.run(
            [
                sys.executable,
                workspace.root / "tools" / "code_health" / "code_health.py",
                *arguments,
            ],
            cwd=workspace.root,
            timeout_seconds=COMMAND_TIMEOUT_SECONDS["code-health"],
        )


def run(args: argparse.Namespace, *, runner: Runner | None = None) -> int:
    if not math.isfinite(args.lock_timeout) or args.lock_timeout < 0:
        raise DevError("--lock-timeout must be finite and non-negative", exit_code=2)
    command_runner = runner or Runner(
        announce_commands=(args.command != "flash" or args.verbose)
    )
    workspace = Workspace.discover(repo_root=REPO_ROOT, selector=args.worktree)

    if args.command == "doctor":
        run_doctor(workspace, args.scope, command_runner)
    elif args.command == "bootstrap":
        _bootstrap(workspace, args.scope, command_runner, args.lock_timeout)
    elif args.command == "test":
        _native_test(workspace, args.filters, command_runner, args.lock_timeout)
    elif args.command == "build":
        target, remembered = _build_target(workspace, args.target)
        print(f"litch: building {target.upper()} firmware", flush=True)
        _build(workspace, target, command_runner, args.lock_timeout, compile_only=args.compile_only)
        remember_local_state(
            workspace.root / ".cache" / "dev-local-state.json",
            "last_build",
            target,
        )
    elif args.command == "bin":
        print(_firmware_bin(workspace, args.target))
    elif args.command in {"app", "flutter"}:
        _flutter(workspace, args, command_runner, args.lock_timeout)
    elif args.command == "release":
        _release_check(workspace, command_runner, args.lock_timeout)
    elif args.command == "code-health":
        code_health_args = (
            ["--help"] if args.code_health_help else []
        ) + args.code_health_args
        _code_health(workspace, code_health_args, command_runner, args.lock_timeout)
    elif args.command == "devices":
        _list_devices(workspace, command_runner, args.ble, args.lock_timeout)
    elif args.command == "flash":
        _flash(workspace, args, command_runner, args.lock_timeout)
    elif args.command in {"monitor", "console"}:
        _monitor(workspace, args, command_runner, args.lock_timeout)
    elif args.command == "worktrees":
        _print_worktrees(workspace)
    elif args.command == "check":
        run_tools_check(workspace, command_runner, include_tests=args.all)
    else:
        raise DevError(f"unsupported command: {args.command}", exit_code=2)
    return 0


def _interactive_session() -> int:
    from .device_menu import run_device_menu

    workspace = Workspace.discover(repo_root=REPO_ROOT)
    runner = Runner(announce_commands=False)

    def other_commands() -> None:
        arguments = _interactive_command()
        print(f"litch: {shlex.join(['./litch', *arguments])}", flush=True)
        run(_parser().parse_args(arguments))

    return run_device_menu(
        workspace, runner,
        build_target=lambda target: _build_one(workspace.root, target, runner),
        core_directory=lambda: _platformio_core_dir(workspace.root, runner),
        other_commands=other_commands,
    )


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if not arguments:
        if not sys.stdin.isatty():
            print(
                "litch: no command in this non-interactive session; run "
                "'./litch --help' or a direct command such as './litch build s3', "
                "'./litch flash s3 --device <usb-serial-or-port>', or './litch check tools'",
                file=sys.stderr,
            )
            return 2
        try:
            return _interactive_session()
        except (DevError, WorkspaceError, LockError) as error:
            print(f"litch: {error}", file=sys.stderr)
            return error.exit_code if isinstance(error, DevError) else 1
        except EOFError:
            print("litch: menu input ended; run './litch --help' for direct commands", file=sys.stderr)
            return 2
        except KeyboardInterrupt:
            print("litch: interrupted", file=sys.stderr)
            return 130
    parser = _parser()
    args = parser.parse_args(arguments)
    try:
        return run(args)
    except (DevError, WorkspaceError, LockError) as error:
        print(f"litch: {error}", file=sys.stderr)
        return error.exit_code if isinstance(error, DevError) else 1
    except KeyboardInterrupt:
        print("litch: interrupted", file=sys.stderr)
        return 130
