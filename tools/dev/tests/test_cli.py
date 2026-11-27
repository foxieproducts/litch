from __future__ import annotations

import argparse
import contextlib
import io
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock
from contextlib import redirect_stdout
from io import StringIO

from tools.dev import cli, hardware
from tools.dev import runner as runner_module
from tools.dev.runner import CompletedCommand, DevError, Runner


class FakeWorkspace:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.common_git_dir = root / ".git"


class FakeRunner:
    def __init__(self, results: list[CompletedCommand] | None = None) -> None:
        self.results = list(results or [])
        self.calls: list[dict[str, object]] = []

    def run(self, argv, **kwargs):
        normalized = tuple(str(item) for item in argv)
        self.calls.append({"argv": normalized, **kwargs})
        if self.results:
            return self.results.pop(0)
        return CompletedCommand(normalized, 0, "", "")


class FakeLockManager:
    def __init__(self, *args, **kwargs) -> None:
        pass

    def lane(self, *args, **kwargs):
        return contextlib.nullcontext()


class CliTests(unittest.TestCase):
    def test_serial_bootstrap_real_runner_captures_child_failure_and_remediation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = Runner(output=StringIO(), announce_commands=False)
            real_run = runner.run
            child = (
                cli.sys.executable,
                "-c",
                "import sys; print('pip diagnostic stdout'); "
                "sys.stderr.write('error: externally-managed-environment\\n'); sys.exit(23)",
            )

            def run(argv, **kwargs):
                if argv[0] == "pio":
                    return CompletedCommand(tuple(str(value) for value in argv), 0, "", "")
                return real_run(argv, **kwargs)

            with (
                mock.patch.object(cli, "LockManager", FakeLockManager),
                mock.patch.object(cli, "pyserial_install_argv", return_value=child),
                mock.patch.object(runner, "run", side_effect=run),
            ):
                with self.assertRaises(DevError) as raised:
                    cli._bootstrap(workspace, "firmware", runner, 0)

            self.assertEqual(raised.exception.exit_code, 23)
            self.assertIn("pip diagnostic stdout", str(raised.exception))
            self.assertIn("error: externally-managed-environment", str(raised.exception))
            self.assertIn("-m venv .venv", str(raised.exception))
            self.assertIn("./litch bootstrap firmware", str(raised.exception))

    def test_serial_bootstrap_failure_keeps_detail_and_offers_same_python_venv(self) -> None:
        for detail in ("No module named pip", "error: externally-managed-environment"):
            with self.subTest(detail=detail), tempfile.TemporaryDirectory() as temporary:
                workspace = FakeWorkspace(Path(temporary))
                runner = mock.Mock()
                runner.run.side_effect = [
                    CompletedCommand(("pio",), 0, "", ""),
                    DevError(detail, exit_code=23),
                ]
                with mock.patch.object(cli, "LockManager", FakeLockManager):
                    with self.assertRaises(DevError) as raised:
                        cli._bootstrap(workspace, "firmware", runner, 0)

                message = str(raised.exception)
                self.assertIn(detail, message)
                self.assertIn(cli.sys.executable, message)
                self.assertIn("-m venv .venv", message)
                self.assertIn(". .venv/bin/activate", message)
                self.assertIn("platformio==6.1.19", message)
                self.assertIn("./litch bootstrap firmware", message)
                self.assertEqual(raised.exception.exit_code, 23)
                self.assertNotIn("--break-system-packages", message)
                self.assertNotIn("sudo", message)
                self.assertTrue(runner.run.call_args.kwargs["capture"])

    def test_firmware_bootstrap_installs_platformio_tools_and_pinned_pyserial(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner()
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._bootstrap(workspace, "firmware", runner, 0)

        self.assertEqual(
            [call["argv"] for call in runner.calls],
            [
                (
                    "pio",
                    "pkg",
                    "install",
                    "--project-dir",
                    str(workspace.root),
                    "-e",
                    "native",
                    "-e",
                    "litch-esp32s3",
                ),
                (
                    str(Path(cli.sys.executable)),
                    "-m",
                    "pip",
                    "install",
                    "pyserial==3.5",
                ),
            ],
        )

    def test_interactive_no_args_menu_teaches_the_direct_command(self) -> None:
        output = StringIO()
        terminal_input = mock.Mock()
        terminal_input.isatty.return_value = True
        with (
            mock.patch.object(hardware, "discover_possible_usb_boards", return_value=()),
            mock.patch.object(cli.Workspace, "discover", return_value=FakeWorkspace(Path("/nonexistent-fixture"))),
            mock.patch.object(cli.sys, "stdin", terminal_input),
            mock.patch("builtins.input", side_effect=("m", "6", "4", "q")),
            mock.patch.object(cli, "run", return_value=0) as run,
            redirect_stdout(output),
        ):
            self.assertEqual(cli.main([]), 0)

        self.assertIn("What do you want to do?", output.getvalue())
        self.assertIn("./litch check tools", output.getvalue())
        self.assertEqual(run.call_args.args[0].command, "check")

    def test_menu_keeps_legacy_c3_out_of_default_build_choices(self) -> None:
        output = StringIO()
        terminal_input = mock.Mock()
        terminal_input.isatty.return_value = True
        remembered = {
            "last_build": "c3",
            "ios_flutter": None,
            "android_flutter": None,
        }
        with (
            mock.patch.object(hardware, "discover_possible_usb_boards", return_value=()),
            mock.patch.object(cli.Workspace, "discover", return_value=FakeWorkspace(Path("/nonexistent-fixture"))),
            mock.patch.object(cli.sys, "stdin", terminal_input),
            mock.patch("builtins.input", side_effect=("m", "1", "q")),
            mock.patch.object(cli, "read_local_state", return_value=remembered),
            mock.patch.object(cli, "run", return_value=0) as run,
            redirect_stdout(output),
        ):
            self.assertEqual(cli.main([]), 0)

        self.assertNotIn("C3", output.getvalue())
        self.assertNotIn("Both", output.getvalue())
        self.assertEqual(run.call_args.args[0].target, "s3")

    def test_menu_ctrl_c_is_a_calm_exit(self) -> None:
        terminal_input = mock.Mock()
        terminal_input.isatty.return_value = True
        stderr = StringIO()
        with (
            mock.patch.object(hardware, "discover_possible_usb_boards", return_value=()),
            mock.patch.object(cli.Workspace, "discover", return_value=FakeWorkspace(Path("/nonexistent-fixture"))),
            mock.patch.object(cli.sys, "stdin", terminal_input),
            mock.patch("builtins.input", side_effect=KeyboardInterrupt),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(cli.main([]), 130)
        self.assertIn("litch: interrupted", stderr.getvalue())

    def test_noninteractive_no_args_gives_litch_specific_direct_commands(self) -> None:
        terminal_input = mock.Mock()
        terminal_input.isatty.return_value = False
        stderr = StringIO()
        with (
            mock.patch.object(cli.sys, "stdin", terminal_input),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertEqual(cli.main([]), 2)

        guidance = stderr.getvalue()
        self.assertNotIn("usage:", guidance)
        self.assertIn("./litch --help", guidance)
        self.assertIn("./litch build s3", guidance)

    def test_successful_app_build_becomes_the_interactive_platform_default(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            args = cli._parser().parse_args(["app", "build", "android"])
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._flutter(workspace, args, FakeRunner(), 0)

            output = StringIO()
            terminal_input = mock.Mock()
            terminal_input.isatty.return_value = True
            with (
                mock.patch.object(hardware, "discover_possible_usb_boards", return_value=()),
                mock.patch.object(cli.Workspace, "discover", return_value=workspace),
                mock.patch.object(cli, "REPO_ROOT", workspace.root),
                mock.patch.object(cli.sys, "stdin", terminal_input),
                mock.patch("builtins.input", side_effect=("m", "2", "", "q")),
                mock.patch.object(cli, "run", return_value=0) as run,
                redirect_stdout(output),
            ):
                self.assertEqual(cli.main([]), 0)

        self.assertIn("Android (default)", output.getvalue())
        self.assertEqual(run.call_args.args[0].platform, "android")

    def test_app_build_has_direct_ios_and_android_commands(self) -> None:
        for platform, flutter_target in (("ios", "ios"), ("android", "apk")):
            with self.subTest(platform=platform):
                with tempfile.TemporaryDirectory() as temporary:
                    workspace = FakeWorkspace(Path(temporary))
                    runner = FakeRunner()
                    args = cli._parser().parse_args(["app", "build", platform])
                    output = StringIO()
                    with (
                        mock.patch.object(cli, "LockManager", FakeLockManager),
                        redirect_stdout(output),
                    ):
                        cli._flutter(workspace, args, runner, 0)
                self.assertEqual(
                    runner.calls[0]["argv"],
                    ("flutter", "build", flutter_target),
                )
                self.assertIn(
                    f"building {'iOS' if platform == 'ios' else 'Android'} app",
                    output.getvalue(),
                )

    def test_bare_build_ignores_remembered_legacy_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            cli.remember_local_state(
                workspace.root / ".cache" / "dev-local-state.json",
                "last_build",
                "c3",
            )
            args = cli._parser().parse_args(["build"])
            output = StringIO()
            with (
                mock.patch.object(cli.Workspace, "discover", return_value=workspace),
                mock.patch.object(cli, "_build") as build,
                redirect_stdout(output),
            ):
                self.assertEqual(cli.run(args, runner=FakeRunner()), 0)

        build.assert_called_once_with(workspace, "s3", mock.ANY, 30.0, compile_only=False)
        self.assertIn("building S3", output.getvalue())
        self.assertNotIn("c3", output.getvalue().casefold())

    def test_explicit_c3_build_remains_supported_without_help_advertising_it(self) -> None:
        parser = cli._parser()
        self.assertEqual(parser.parse_args(["build", "c3"]).target, "c3")

        output = StringIO()
        with self.assertRaises(SystemExit):
            with redirect_stdout(output):
                parser.parse_args(["build", "--help"])
        self.assertNotIn("c3", output.getvalue().casefold())

    def test_build_all_cannot_implicitly_expand_to_legacy_c3(self) -> None:
        parser = cli._parser()
        with self.assertRaises(SystemExit):
            parser.parse_args(["build", "all"])

    def test_bare_build_defaults_to_s3_with_malformed_remembered_state(self) -> None:
        workspace = FakeWorkspace(Path("/repo"))
        terminal_input = mock.Mock()
        terminal_input.isatty.return_value = False
        with (
            mock.patch.object(cli.sys, "stdin", terminal_input),
            mock.patch.object(
                cli,
                "read_local_state",
                return_value={"last_build": {"unexpected": "shape"}},
            ),
        ):
            self.assertEqual(cli._build_target(workspace, None), ("s3", False))

    def test_flash_and_monitor_allow_remembered_bare_selection(self) -> None:
        parser = cli._parser()
        parsed = parser.parse_args(["flash"])
        self.assertIsNone(parsed.target)
        self.assertIsNone(parsed.device)
        self.assertFalse(parsed.verbose)
        self.assertFalse(parsed.factory_provision)
        monitor = parser.parse_args(["monitor"])
        self.assertIsNone(monitor.target)
        self.assertIsNone(monitor.device)
        parsed = parser.parse_args(
            ["flash", "s3", "--device", "00005E0053AB"]
        )
        self.assertEqual(parsed.device, "00005E0053AB")
        self.assertIsNone(parsed.ble_device)
        ble = parser.parse_args(["flash", "s3", "--ble-device", "board-uuid"])
        self.assertEqual(ble.ble_device, "board-uuid")
        self.assertIsNone(ble.device)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as rejected:
            parser.parse_args(["flash", "s3", "--device", "USB", "--ble-device", "BLE"])
        self.assertEqual(rejected.exception.code, 2)

        verbose = parser.parse_args(["flash", "s3", "--verbose"])
        self.assertTrue(verbose.verbose)

    def test_flash_forwards_verbose_output_choice(self) -> None:
        workspace = FakeWorkspace(Path("/repo"))
        args = cli._parser().parse_args(["flash", "s3", "--verbose"])
        runner = FakeRunner()

        with (
            mock.patch("tools.dev.hardware.flash_selected_target") as flash,
            mock.patch.object(cli, "_build_one") as build,
        ):
            cli._flash(workspace, args, runner, 0)
            flash.call_args.kwargs["build_target"]("s3")

        self.assertTrue(flash.call_args.kwargs["verbose"])
        build.assert_called_once_with(workspace.root, "s3", runner)

    def test_console_alias_dispatches_to_monitor_with_the_same_options(self) -> None:
        workspace = FakeWorkspace(Path("/repo"))
        for command in ("monitor", "console"):
            for options in ([], ["s3", "--device", "00005E0053BC"]):
                with (
                    self.subTest(command=command, options=options),
                    mock.patch.object(cli.Workspace, "discover", return_value=workspace),
                    mock.patch.object(cli, "_monitor") as monitor,
                ):
                    args = cli._parser().parse_args([command, *options])
                    runner = FakeRunner()
                    self.assertEqual(cli.run(args, runner=runner), 0)
                    monitor.assert_called_once_with(workspace, args, runner, args.lock_timeout)
                    self.assertEqual(args.target, "s3" if options else None)
                    self.assertEqual(args.device, "00005E0053BC" if options else None)

    def test_flash_forwards_explicit_factory_provision(self) -> None:
        workspace = FakeWorkspace(Path("/repo"))
        args = cli._parser().parse_args(
            ["flash", "s3", "--device", "ABC", "--factory-provision"]
        )

        with mock.patch("tools.dev.hardware.flash_selected_target") as flash:
            cli._flash(workspace, args, FakeRunner(), 0)

        self.assertTrue(flash.call_args.kwargs["factory_provision"])

    def test_ordinary_flash_batch_routes_to_s3_usb(self) -> None:
        workspace = FakeWorkspace(Path("/repo"))
        runner = FakeRunner()
        for options in ([], ["s3"]):
            with (
                self.subTest(options=options),
                mock.patch("tools.dev.batch_flash.flash_batch") as batch,
                mock.patch("tools.dev.hardware.flash_selected_target") as single,
                mock.patch.object(cli, "_build_one") as build,
            ):
                args = cli._parser().parse_args(["flash", "--batch", *options])
                cli._flash(workspace, args, runner, 0)
                single.assert_not_called()
                self.assertFalse(batch.call_args.kwargs["factory_provision"])
                batch.call_args.kwargs["build_target"]("s3")
                build.assert_called_once_with(workspace.root, "s3", runner)

    def test_flash_batch_rejects_non_s3_and_manual_selectors(self) -> None:
        parser = cli._parser()
        for selector in ("--device", "--ble-device"):
            with (
                self.subTest(selector=selector),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as rejected,
            ):
                parser.parse_args(["flash", "--batch", selector, "board"])
            self.assertEqual(rejected.exception.code, 2)

        for options in ([], ["--factory"]):
            args = parser.parse_args(["flash", "c3", "--batch", *options])
            with (
                self.subTest(options=options),
                mock.patch("tools.dev.batch_flash.flash_batch") as batch,
                mock.patch("tools.dev.factory_station.build_factory_firmware") as build,
                self.assertRaisesRegex(DevError, "requires the S3 USB target") as rejected,
            ):
                cli._flash(FakeWorkspace(Path("/repo")), args, FakeRunner(), 0)
            self.assertEqual(rejected.exception.exit_code, 2)
            batch.assert_not_called()
            build.assert_not_called()

    def test_flash_batch_ctrl_c_uses_normal_interrupt_exit(self) -> None:
        with (
            mock.patch.object(cli.Workspace, "discover", return_value=FakeWorkspace(Path("/repo"))),
            mock.patch("tools.dev.batch_flash.flash_batch", side_effect=KeyboardInterrupt),
            contextlib.redirect_stderr(io.StringIO()) as stderr,
        ):
            self.assertEqual(cli.main(["flash", "--batch"]), 130)
        self.assertIn("litch: interrupted", stderr.getvalue())

    def test_factory_station_options_require_factory_batch_before_build(self) -> None:
        for options in (
            ["--batch-name", "SERIES.1"],
            ["--factory", "--records", "results.csv"],
            ["--batch", "--records", "results.csv"],
            ["--resume"],
            ["--factory", "--resume"],
            ["--batch", "--resume"],
        ):
            args = cli._parser().parse_args(["flash", *options])
            with (
                self.subTest(options=options),
                mock.patch("tools.dev.factory_station.build_factory_firmware") as build,
                mock.patch("tools.dev.factory_camera.discover_cameras") as discover,
                mock.patch("builtins.input") as prompt,
                self.assertRaisesRegex(DevError, "require --batch --factory") as rejected,
            ):
                cli._flash(FakeWorkspace(Path("/repo")), args, FakeRunner(), 0)
            self.assertEqual(rejected.exception.exit_code, 2)
            build.assert_not_called()
            discover.assert_not_called()
            prompt.assert_not_called()

    def test_flash_rejects_removed_camera_options(self) -> None:
        for option in ("--camera", "--camera-device"):
            with (
                self.subTest(option=option),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
                self.assertRaises(SystemExit) as rejected,
            ):
                cli._parser().parse_args(["flash", "s3", "--batch", "--factory", option, "unused"])
            self.assertEqual(rejected.exception.code, 2)
            self.assertIn(f"unrecognized arguments: {option}", stderr.getvalue())

    def test_factory_build_failure_stops_before_prompts_and_discovery(self) -> None:
        args = cli._parser().parse_args(["flash", "--batch", "--factory"])
        with (
            mock.patch("tools.dev.factory_station.build_factory_firmware", side_effect=DevError("build failed")),
            mock.patch("tools.dev.factory_camera.discover_cameras") as discover,
            mock.patch("tools.dev.factory_station.run_factory_station") as station,
            mock.patch("builtins.input") as prompt,
            self.assertRaisesRegex(DevError, "build failed"),
        ):
            cli._flash(FakeWorkspace(Path("/repo")), args, FakeRunner(), 0)
        prompt.assert_not_called()
        discover.assert_not_called()
        station.assert_not_called()

    def test_factory_build_precedes_prompts_and_passes_firmware_and_selected_camera(self) -> None:
        from tools.dev.factory_camera import CameraChoice

        args = cli._parser().parse_args(["flash", "--batch", "--factory", "--records", "/tmp/results.csv"])
        workspace = FakeWorkspace(Path("/repo"))
        runner = FakeRunner()
        firmware = object()
        native = CameraChoice("native", "native-id", "Built-in camera")
        android = CameraChoice("android", "usb-id", "Android rear camera", "1", "/sdk/adb")
        order = mock.Mock()

        def finish_build(**kwargs):
            kwargs["build_target"]("s3")
            return firmware

        with (
            mock.patch("tools.dev.factory_station.build_factory_firmware", side_effect=finish_build) as build,
            mock.patch.object(cli, "_build_one") as build_one,
            mock.patch.object(cli, "_adb_executable", return_value="/sdk/adb"),
            mock.patch("tools.dev.factory_camera.discover_cameras", return_value=(native, android)) as discover,
            mock.patch("builtins.input", side_effect=["", "SERIES.1", "", "3", " 2 "]) as prompt,
            mock.patch("tools.dev.factory_station.run_factory_station") as station,
            redirect_stdout(StringIO()) as output,
        ):
            order.attach_mock(build, "build")
            order.attach_mock(prompt, "prompt")
            order.attach_mock(discover, "discover")
            order.attach_mock(station, "station")
            cli._flash(workspace, args, runner, 7)

        self.assertEqual([call[0] for call in order.mock_calls], [
            "build", "prompt", "prompt", "discover", "prompt", "prompt", "prompt", "station",
        ])
        build.assert_called_once_with(workspace=workspace, lock_timeout=7, build_target=mock.ANY)
        build_one.assert_called_once_with(workspace.root, "s3", runner)
        discover.assert_called_once_with(adb_executable="/sdk/adb")
        station.assert_called_once_with(
            workspace=workspace, runner=runner, lock_timeout=7, firmware=firmware,
            camera_choice=android, platformio_core_dir=mock.ANY, verbose=False,
            batch_name="SERIES.1", records_path=Path("/tmp/results.csv"), resume=False,
        )
        self.assertIn("1. Built-in camera", output.getvalue())
        self.assertIn("2. Android rear camera", output.getvalue())
        self.assertEqual(output.getvalue().count("Enter a number from 1 to 2."), 2)

    def test_factory_resume_forwards_explicit_batch_and_preserves_record_destination(self) -> None:
        from tools.dev.factory_camera import CameraChoice

        args = cli._parser().parse_args([
            "flash", "s3", "--batch", "--factory", "--resume",
            "--batch-name", "SERIES.1", "--records", "/tmp/existing.csv",
        ])
        native = CameraChoice("native", "native-id", "Built-in camera")
        firmware = object()
        with (
            mock.patch("tools.dev.factory_station.build_factory_firmware", return_value=firmware),
            mock.patch.object(cli, "_adb_executable", return_value=None),
            mock.patch("tools.dev.factory_camera.discover_cameras", return_value=(native,)) as discover,
            mock.patch("builtins.input", return_value="1") as prompt,
            mock.patch("tools.dev.factory_station.run_factory_station") as station,
            redirect_stdout(StringIO()) as output,
        ):
            cli._flash(FakeWorkspace(Path("/repo")), args, FakeRunner(), 0)
        prompt.assert_called_once_with("Camera number (q to quit): ")
        discover.assert_called_once_with(adb_executable=None)
        self.assertIn("1. Built-in camera", output.getvalue())
        self.assertTrue(station.call_args.kwargs["resume"])
        self.assertEqual(station.call_args.kwargs["batch_name"], "SERIES.1")
        self.assertEqual(station.call_args.kwargs["records_path"], Path("/tmp/existing.csv"))
        self.assertIs(station.call_args.kwargs["firmware"], firmware)
        self.assertIs(station.call_args.kwargs["camera_choice"], native)

    def test_factory_q_or_eof_cancels_either_prompt(self) -> None:
        from tools.dev.factory_camera import CameraChoice

        native = CameraChoice("native", "native-id", "Built-in camera")
        for batch_options in ([], ["--batch-name", "SERIES.1"]):
            args = cli._parser().parse_args(["flash", "--batch", "--factory", *batch_options])
            for reply in (" Q ", EOFError()):
                with (
                    self.subTest(batch_options=batch_options, reply=reply),
                    mock.patch("tools.dev.factory_station.build_factory_firmware") as build,
                    mock.patch.object(cli, "_adb_executable", return_value=None),
                    mock.patch("tools.dev.factory_camera.discover_cameras", return_value=(native,)) as discover,
                    mock.patch("builtins.input", side_effect=[reply]),
                    mock.patch("tools.dev.factory_station.run_factory_station") as station,
                    redirect_stdout(StringIO()),
                ):
                    cli._flash(FakeWorkspace(Path("/repo")), args, FakeRunner(), 0)
                build.assert_called_once()
                self.assertEqual(discover.call_count, int(bool(batch_options)))
                station.assert_not_called()

    def test_factory_reports_empty_camera_inventory_after_build(self) -> None:
        args = cli._parser().parse_args(["flash", "--batch", "--factory", "--batch-name", "SERIES.1"])
        with (
            mock.patch("tools.dev.factory_station.build_factory_firmware") as build,
            mock.patch.object(cli, "_adb_executable", return_value=None),
            mock.patch("tools.dev.factory_camera.discover_cameras", return_value=()),
            mock.patch("builtins.input") as prompt,
            mock.patch("tools.dev.factory_station.run_factory_station") as station,
            self.assertRaisesRegex(DevError, "No factory cameras found"),
        ):
            cli._flash(FakeWorkspace(Path("/repo")), args, FakeRunner(), 0)
        build.assert_called_once()
        prompt.assert_not_called()
        station.assert_not_called()


    def test_identity_selectors_reject_duplicates(self) -> None:
        parser = cli._parser()
        duplicated = (
            ["flash", "c3", "--device", "A", "--device", "B"],
            [
                "flash",
                "s3",
                "--device",
                "A",
                "--ble-device",
                "one",
                "--ble-device",
                "two",
            ],
            ["monitor", "s3", "--device", "A", "--device", "B"],
        )
        for argv in duplicated:
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    parser.parse_args(argv)

    def test_s3_build_uses_canonical_pio_and_postbuild_usb_check(self) -> None:
        for compile_only in (False, True):
            with self.subTest(compile_only=compile_only), tempfile.TemporaryDirectory() as temporary:
                workspace = FakeWorkspace(Path(temporary))
                runner = FakeRunner()
                options = ["--compile-only"] if compile_only else []
                args = cli._parser().parse_args(["build", "s3", *options])
                with (
                    mock.patch.object(cli.Workspace, "discover", return_value=workspace),
                    mock.patch.object(cli, "LockManager", FakeLockManager),
                    mock.patch.object(
                        cli, "platformio_sleep_dependencies", wraps=cli.platformio_sleep_dependencies,
                    ) as sleep_dependencies,
                    redirect_stdout(StringIO()),
                ):
                    self.assertEqual(cli.run(args, runner=runner), 0)
                commands = [call["argv"] for call in runner.calls]
                self.assertEqual(commands[1], ("pio", "run", "-e", "litch-esp32s3"))
                self.assertTrue(commands[0][1].endswith("tools/dev/verify_usb_configuration.py"))
                self.assertEqual(len(commands), 3 if compile_only else 4)
                if compile_only:
                    sleep_dependencies.assert_not_called()
                else:
                    sleep_dependencies.assert_called_once_with(workspace.root, "litch-esp32s3")
                    self.assertTrue(commands[2][1].endswith("tools/dev/check_sleep_sdk_wake.py"))
                self.assertTrue(commands[-1][1].endswith("tools/dev/verify_usb_configuration.py"))
                self.assertIn("--firmware", commands[-1])
                self.assertTrue(commands[-1][-1].endswith(".pio/build/litch-esp32s3/firmware.elf"))

    def test_s3_build_refuses_delivery_when_sleep_check_fails_after_compile(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner()
            record = runner.run

            def run(argv, **kwargs):
                result = record(argv, **kwargs)
                if str(argv[1]).endswith("tools/dev/check_sleep_sdk_wake.py"):
                    raise DevError("controlled Sleep regression", exit_code=23)
                return result

            with mock.patch.object(runner, "run", side_effect=run):
                with self.assertRaises(DevError) as raised:
                    cli._build_one(workspace.root, "s3", runner)
            self.assertEqual(raised.exception.exit_code, 23)
            commands = [call["argv"] for call in runner.calls]
            self.assertEqual(commands[1], ("pio", "run", "-e", "litch-esp32s3"))
            self.assertEqual(len(commands), 3)
            self.assertTrue(commands[-1][1].endswith("tools/dev/check_sleep_sdk_wake.py"))
            self.assertFalse(any("--firmware" in command for command in commands))

    def test_s3_sleep_gate_runs_real_checker_with_relocated_dependencies(self) -> None:
        root = cli.REPO_ROOT
        core_source = Path(os.environ.get("PLATFORMIO_CORE_DIR") or Path.home() / ".platformio")
        packages_source = Path(os.environ.get("PLATFORMIO_PACKAGES_DIR") or core_source / "packages").expanduser()
        if not packages_source.is_absolute():
            packages_source = root / packages_source
        installed_sdk = packages_source / "framework-arduinoespressif32/tools/sdk/esp32s3"
        self.assertTrue(installed_sdk.is_dir(), "bootstrap the pinned S3 firmware dependencies")
        common_git = Path(subprocess.check_output(
            ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"], cwd=root, text=True).strip())
        workspace_source = Path(os.environ.get("PLATFORMIO_WORKSPACE_DIR") or root / ".pio")
        libdeps_source = Path(os.environ.get("PLATFORMIO_LIBDEPS_DIR") or workspace_source / "libdeps").expanduser()
        if not libdeps_source.is_absolute():
            libdeps_source = root / libdeps_source
        json_sources = [libdeps_source / "litch-esp32s3/ArduinoJson/src",
                        common_git.parent / ".pio/libdeps/litch-esp32s3/ArduinoJson/src"]
        json_source = next((path for path in json_sources if (path / "ArduinoJson.h").is_file()), None)
        self.assertIsNotNone(json_source, "bootstrap the pinned S3 ArduinoJson dependency")
        with tempfile.TemporaryDirectory() as temporary:
            core = Path(temporary) / "relocated core"
            workspace = Path(temporary) / "relocated workspace"
            sdk = core / "packages/framework-arduinoespressif32/tools/sdk/esp32s3"
            sdk.parent.mkdir(parents=True)
            sdk.symlink_to(installed_sdk, target_is_directory=True)
            json_include = workspace / "libdeps/litch-esp32s3/ArduinoJson/src"
            json_include.parent.mkdir(parents=True)
            json_include.symlink_to(json_source, target_is_directory=True)
            output = StringIO()
            runner = Runner(output=output, announce_commands=False)
            run_real = runner.run
            calls = []
            checker_output = []

            def run(argv, **kwargs):
                calls.append(tuple(str(value) for value in argv))
                if argv[0] == "pio":
                    return CompletedCommand(tuple(argv), 0, "", "")
                result = run_real(argv, capture=True, **kwargs)
                checker_output.append(result.stdout)
                return result

            with (
                mock.patch.dict(os.environ, {"PLATFORMIO_CORE_DIR": str(core),
                    "PLATFORMIO_WORKSPACE_DIR": str(workspace), "PLATFORMIO_LIBDEPS_DIR": "",
                    "PLATFORMIO_PACKAGES_DIR": ""}),
                mock.patch.object(cli, "_verify_usb_configuration"),
                mock.patch.object(runner, "run", side_effect=run),
            ):
                cli._build_one(root, "s3", runner)
            check = calls[1]
            self.assertEqual(check[check.index("--sdk") + 1], str(sdk))
            self.assertEqual(check[check.index("--json-include") + 1], str(json_include))
            self.assertIn("pre_entry_P1=pulse actual_ULP_words=21", checker_output[0])
            self.assertIn("fresh_P1_wakes=1", checker_output[0])

    def test_test_selection_is_explicit_and_reuses_warm_app_inputs(self) -> None:
        with redirect_stdout(StringIO()), contextlib.redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit) as missing:
                cli._parser().parse_args(["test", "native"])
        self.assertEqual(missing.exception.code, 2)
        self.assertTrue(cli._parser().parse_args(["test", "native", "--all"]).all)
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner()
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._native_test(workspace, ["test_one", "test_two"], runner, 0)
                selected = workspace.root / "app/test/example_test.dart"
                selected.parent.mkdir(parents=True)
                selected.write_text("selected existing fixture")
                for arguments in (["app", "test"],
                                  ["app", "test", "--", "test/example_test.dart", "test/"]):
                    with self.assertRaises(DevError):
                        cli._flutter(workspace, cli._parser().parse_args(arguments), runner, 0)
                self.assertEqual(len(runner.calls), 1)
                cli._flutter(workspace, cli._parser().parse_args(
                    ["app", "test", "--", "--plain-name", "named outcome", "test/example_test.dart"]),
                    runner, 0)
                cli._flutter(workspace, cli._parser().parse_args(
                    ["app", "test", "--all"]), runner, 0)
        self.assertEqual(
            runner.calls[0]["argv"],
            ("pio", "test", "-e", "native", "-f", "test_one", "-f", "test_two"),
        )
        self.assertEqual(runner.calls[1]["argv"],
                         ("flutter", "test", "--no-pub", "--concurrency=1",
                          "--plain-name", "named outcome", "test/example_test.dart"))
        self.assertEqual(runner.calls[2]["argv"],
                         ("flutter", "test", "--no-pub", "--concurrency=4"))
        self.assertEqual(runner.calls[1]["timeout_seconds"], 120)
        self.assertEqual(runner.calls[2]["timeout_seconds"], 120)

    def test_flutter_run_maps_options_and_passthrough(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"simulator-1","targetPlatform":"ios"}]',
                        "",
                    )
                ]
            )
            args = argparse.Namespace(
                action="run",
                device="simulator-1",
                release=True,
                flutter_args=["--", "--dart-define", "A=B"],
            )
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._flutter(workspace, args, runner, 0)
        self.assertEqual(
            runner.calls[1]["argv"],
            ("flutter", "run", "-d", "simulator-1", "--release", "--dart-define", "A=B"),
        )
        self.assertTrue(runner.calls[1]["interactive"])

    def test_flutter_parser_keeps_run_options_out_of_passthrough(self) -> None:
        parsed = cli._parser().parse_args(
            [
                "flutter",
                "run",
                "--device",
                "simulator-1",
                "--release",
                "--",
                "--dart-define=A=B",
            ]
        )
        self.assertEqual(parsed.device, "simulator-1")
        self.assertTrue(parsed.release)
        self.assertEqual(parsed.flutter_args, ["--", "--dart-define=A=B"])

    def test_flutter_platform_is_optional_and_device_must_match_it(self) -> None:
        parsed = cli._parser().parse_args(
            ["flutter", "install", "android", "--release", "--device", "android-1"]
        )
        self.assertEqual(parsed.platform, "android")
        self.assertTrue(parsed.release)
        self.assertEqual(parsed.device, "android-1")

        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            with mock.patch.object(
                cli,
                "_flutter_devices",
                return_value=(("iphone-1", "ios"), ("android-1", "android")),
            ):
                with self.assertRaisesRegex(DevError, "is ios, not requested platform android"):
                    cli._select_flutter_device(
                        workspace,
                        "iphone-1",
                        FakeRunner(),
                        requested_platform="android",
                    )

    def test_flutter_install_is_the_noninteractive_success_boundary(self) -> None:
        parsed = cli._parser().parse_args(["flutter", "install", "--device", "phone"])
        self.assertEqual(parsed.action, "install")
        self.assertEqual(parsed.device, "phone")

    def test_flutter_reuses_only_an_available_platform_slot_and_lists_live_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            state_path = workspace.root / ".cache" / "dev-local-state.json"
            from tools.dev.local_state import remember

            remember(state_path, "ios_flutter", "ios-old")
            with mock.patch.object(
                cli,
                "_flutter_devices",
                return_value=(("ios-old", "ios"), ("android-live", "android")),
            ):
                self.assertEqual(
                    cli._select_flutter_device(workspace, None, FakeRunner()),
                    ("ios-old", "ios"),
                )
            with mock.patch.object(
                cli,
                "_flutter_devices",
                return_value=(("android-live", "android"),),
            ):
                self.assertEqual(
                    cli._select_flutter_device(workspace, None, FakeRunner()),
                    ("android-live", "android"),
                )

    def test_single_live_flutter_device_is_selected_without_prompt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            terminal_input = mock.Mock()
            terminal_input.isatty.return_value = True
            with (
                mock.patch.object(cli.sys, "stdin", terminal_input),
                mock.patch.object(
                    cli,
                    "_flutter_devices",
                    return_value=(("iphone-only", "ios"),),
                ),
                mock.patch("builtins.input", side_effect=AssertionError("must not prompt")),
            ):
                self.assertEqual(
                    cli._select_flutter_device(workspace, None, FakeRunner()),
                    ("iphone-only", "ios"),
                )

    def test_single_live_flutter_device_does_not_override_requested_platform(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            with mock.patch.object(
                cli,
                "_flutter_devices",
                return_value=(("android-only", "android"),),
            ):
                with self.assertRaisesRegex(DevError, "no live ios Flutter device"):
                    cli._select_flutter_device(
                        workspace,
                        None,
                        FakeRunner(),
                        requested_platform="ios",
                    )

    def test_flutter_platform_prefers_its_slot_and_no_platform_shows_short_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            from tools.dev.local_state import remember_many

            remember_many(
                workspace.root / ".cache" / "dev-local-state.json",
                {"ios_flutter": "iphone-1", "android_flutter": "android-1"},
            )
            devices = (("iphone-1", "ios"), ("android-1", "android"))
            with mock.patch.object(cli, "_flutter_devices", return_value=devices):
                self.assertEqual(
                    cli._select_flutter_device(
                        workspace,
                        None,
                        FakeRunner(),
                        requested_platform="android",
                    ),
                    ("android-1", "android"),
                )
                with self.assertRaisesRegex(
                    DevError,
                    r"litch app run ios.*litch app run android",
                ):
                    cli._select_flutter_device(workspace, None, FakeRunner())

    def test_flutter_android_family_and_unique_short_selector_are_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"android-1234","targetPlatform":"android-arm64"}]',
                        "",
                    )
                ]
            )
            self.assertEqual(cli._flutter_devices(workspace, runner), (("android-1234", "android"),))
            with mock.patch.object(cli, "_flutter_devices", return_value=(("android-1234", "android"),)):
                self.assertEqual(
                    cli._select_flutter_device(workspace, "1234", FakeRunner()),
                    ("android-1234", "android"),
                )
            with mock.patch.object(
                cli,
                "_flutter_devices",
                return_value=(("android-1234", "android"), ("ios-1234", "ios")),
            ):
                with self.assertRaisesRegex(DevError, "selector '1234' is ambiguous: android-1234 \(android\), ios-1234 \(ios\)"):
                    cli._select_flutter_device(workspace, "1234", FakeRunner())

    def test_wireless_android_device_is_selected_by_verified_hardware_serial(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner([
                CompletedCommand(("adb",), 0, "OTHER\n", ""),
                CompletedCommand(("adb",), 0, "TESTANDROID01\n", ""),
            ])
            devices = (("192.0.2.1:37001", "android"), ("192.0.2.2:37002", "android"))
            with (
                mock.patch.object(cli, "_flutter_devices", return_value=devices),
                mock.patch.object(cli, "_adb_executable", return_value="adb"),
            ):
                self.assertEqual(
                    cli._select_flutter_device(
                        workspace, "TESTANDROID01", runner, requested_platform="android"
                    ),
                    devices[1],
                )
            self.assertEqual(
                [call["argv"] for call in runner.calls],
                [
                    ("adb", "-s", devices[0][0], "shell", "getprop", "ro.serialno"),
                    ("adb", "-s", devices[1][0], "shell", "getprop", "ro.serialno"),
                ],
            )

    def test_wireless_serial_does_not_select_an_unverified_phone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner([CompletedCommand(("adb",), 0, "OTHER\n", "")])
            with (
                mock.patch.object(
                    cli, "_flutter_devices", return_value=(("192.0.2.1:37001", "android"),)
                ),
                mock.patch.object(cli, "_adb_executable", return_value="adb"),
            ):
                with self.assertRaisesRegex(DevError, "is unavailable"):
                    cli._select_flutter_device(
                        workspace, "TESTANDROID01", runner, requested_platform="android"
                    )

    def test_wireless_android_remembered_serial_survives_port_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            from tools.dev.local_state import remember

            remember(
                workspace.root / ".cache" / "dev-local-state.json",
                "android_flutter",
                "TESTANDROID01",
            )
            devices = (("192.0.2.1:37001", "android"), ("192.0.2.2:37002", "android"))
            runner = FakeRunner([
                CompletedCommand(("adb",), 0, "OTHER\n", ""),
                CompletedCommand(("adb",), 0, "TESTANDROID01\n", ""),
            ])
            with (
                mock.patch.object(cli, "_flutter_devices", return_value=devices),
                mock.patch.object(cli, "_adb_executable", return_value="adb"),
            ):
                self.assertEqual(
                    cli._select_flutter_device(
                        workspace, None, runner, requested_platform="android"
                    ),
                    devices[1],
                )

    def test_unavailable_explicit_flutter_device_never_falls_back_to_remembered_phone(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            from tools.dev.local_state import remember

            remember(workspace.root / ".cache" / "dev-local-state.json", "ios_flutter", "iphone-1")
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"iphone-1","targetPlatform":"ios"}]',
                        "",
                    )
                ]
            )
            args = cli._parser().parse_args(["flutter", "install", "--device", "missing"])
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                with self.assertRaisesRegex(DevError, "Flutter device 'missing' is unavailable"):
                    cli._flutter(workspace, args, runner, 0)
            self.assertEqual(
                [call["argv"] for call in runner.calls],
                [("flutter", "devices", "--machine")],
            )

    def test_flutter_install_platform_command_remembers_only_after_success(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"android-1234","targetPlatform":"android-arm64"}]',
                        "",
                    )
                ]
            )
            args = cli._parser().parse_args(
                [
                    "flutter",
                    "install",
                    "android",
                    "--release",
                    "--device",
                    "1234",
                    "--",
                    "--dart-define=A=B",
                ]
            )
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._flutter(workspace, args, runner, 0)
            self.assertEqual(
                runner.calls[1]["argv"],
                (
                    "flutter",
                    "run",
                    "--no-resident",
                    "-d",
                    "android-1234",
                    "--release",
                    "--dart-define=A=B",
                ),
            )
            from tools.dev.local_state import read

            self.assertEqual(read(workspace.root / ".cache" / "dev-local-state.json")["android_flutter"], "android-1234")

    def test_wireless_install_remembers_serial_instead_of_changing_endpoint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner()
            args = cli._parser().parse_args([
                "app", "install", "android", "--device", "192.0.2.1:37001"
            ])
            with (
                mock.patch.object(cli, "_flutter_devices", return_value=(("192.0.2.1:37001", "android"),)),
                mock.patch.object(cli, "_android_hardware_serial", return_value="TESTANDROID01"),
                mock.patch.object(cli, "LockManager", FakeLockManager),
            ):
                cli._flutter(workspace, args, runner, 0)
            from tools.dev.local_state import read

            self.assertEqual(
                read(workspace.root / ".cache" / "dev-local-state.json")["android_flutter"],
                "TESTANDROID01",
            )

    def test_bare_app_install_announces_available_remembered_device_and_override(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            cli.remember_local_state(
                workspace.root / ".cache" / "dev-local-state.json",
                "ios_flutter",
                "iphone-1",
            )
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"iphone-1","targetPlatform":"ios"}]',
                        "",
                    )
                ]
            )
            args = cli._parser().parse_args(["app", "install"])
            output = StringIO()
            with (
                mock.patch.object(cli, "LockManager", FakeLockManager),
                redirect_stdout(output),
            ):
                cli._flutter(workspace, args, runner, 0)

        self.assertIn("remembered iOS device -> iphone-1", output.getvalue())
        self.assertIn(
            "override: ./litch app install ios --device <flutter-device-id>",
            output.getvalue(),
        )

    def test_interactive_app_install_prompts_for_live_device_ambiguity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"iphone-1","targetPlatform":"ios"},'
                        '{"id":"android-1","targetPlatform":"android-arm64"}]',
                        "",
                    )
                ]
            )
            args = cli._parser().parse_args(["app", "install"])
            terminal_input = mock.Mock()
            terminal_input.isatty.return_value = True
            output = StringIO()
            with (
                mock.patch.object(cli.sys, "stdin", terminal_input),
                mock.patch("builtins.input", return_value="2"),
                mock.patch.object(cli, "LockManager", FakeLockManager),
                redirect_stdout(output),
            ):
                cli._flutter(workspace, args, runner, 0)

        self.assertEqual(runner.calls[1]["argv"][4], "android-1")
        self.assertIn("./litch app install ios --device iphone-1", output.getvalue())
        self.assertIn("./litch app install android --device android-1", output.getvalue())

    def test_successful_app_run_remembers_its_platform_device(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"iphone-1","targetPlatform":"ios"}]',
                        "",
                    )
                ]
            )
            args = cli._parser().parse_args(
                ["app", "run", "ios", "--device", "iphone-1"]
            )
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._flutter(workspace, args, runner, 0)

            state = cli.read_local_state(
                workspace.root / ".cache" / "dev-local-state.json"
            )
        self.assertEqual(state["ios_flutter"], "iphone-1")

    def test_flutter_install_ios_uses_a_standalone_mode(self) -> None:
        def install(extra_args: list[str]) -> tuple[tuple[str, ...], str]:
            with tempfile.TemporaryDirectory() as temporary:
                workspace = FakeWorkspace(Path(temporary))
                runner = FakeRunner(
                    [
                        CompletedCommand(
                            ("flutter", "devices", "--machine"),
                            0,
                            '[{"id":"iphone-1","targetPlatform":"ios"}]',
                            "",
                        )
                    ]
                )
                args = cli._parser().parse_args(
                    [
                        "flutter",
                        "install",
                        "ios",
                        *extra_args,
                        "--device",
                        "iphone-1",
                    ]
                )
                output = io.StringIO()
                with (
                    mock.patch.object(cli, "LockManager", FakeLockManager),
                    contextlib.redirect_stdout(output),
                ):
                    cli._flutter(workspace, args, runner, 0)
                return runner.calls[1]["argv"], output.getvalue()

        profile_argv, profile_output = install([])
        self.assertEqual(
            profile_argv,
            ("flutter", "run", "--no-resident", "-d", "iphone-1", "--profile"),
        )
        self.assertIn("iOS profile install", profile_output)

        release_argv, release_output = install(["--release"])
        self.assertEqual(
            release_argv,
            ("flutter", "run", "--no-resident", "-d", "iphone-1", "--release"),
        )
        self.assertIn("iOS release install", release_output)

    def test_flutter_install_device_override_command_selects_first_live_device(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"iphone-1","targetPlatform":"ios"},'
                        '{"id":"android-1234","targetPlatform":"android-arm64"}]',
                        "",
                    )
                ]
            )
            args = cli._parser().parse_args(
                ["flutter", "install", "--device", "1234", "--release"]
            )
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._flutter(workspace, args, runner, 0)
            self.assertEqual(
                runner.calls[1]["argv"],
                (
                    "flutter",
                    "run",
                    "--no-resident",
                    "-d",
                    "android-1234",
                    "--release",
                ),
            )

    def test_flutter_install_does_not_remember_after_deployment_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner(
                [
                    CompletedCommand(
                        ("flutter", "devices", "--machine"),
                        0,
                        '[{"id":"android-1234","targetPlatform":"android-arm64"}]',
                        "",
                    )
                ]
            )
            original_run = runner.run

            def fail_deployment(argv, **kwargs):
                if tuple(argv[:2]) == ("flutter", "run"):
                    raise DevError("deployment failed")
                return original_run(argv, **kwargs)

            runner.run = fail_deployment
            args = cli._parser().parse_args(["flutter", "install", "android", "--release"])
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                with self.assertRaisesRegex(DevError, "deployment failed"):
                    cli._flutter(workspace, args, runner, 0)
            self.assertFalse((workspace.root / ".cache" / "dev-local-state.json").exists())

    def test_bin_path_is_checkout_local(self) -> None:
        workspace = FakeWorkspace(Path("/repo/worktree"))
        self.assertEqual(
            cli._firmware_bin(workspace, "c3"),
            Path("/repo/worktree/.pio/build/litch-esp32c3/firmware.bin"),
        )

    def test_custom_build_directory_is_used_for_bin_and_elf_validation(self) -> None:
        workspace = FakeWorkspace(Path("/repo/worktree"))
        for override, expected in (
            ("/tmp/private-preview", Path("/tmp/private-preview")),
            ("private-preview", workspace.root / "private-preview"),
        ):
            with self.subTest(override=override), mock.patch.dict(
                os.environ, {"PLATFORMIO_BUILD_DIR": override}
            ), mock.patch.object(cli, "_verify_usb_configuration") as verify:
                cli._build_one(workspace.root, "s3", FakeRunner())
                self.assertEqual(
                    cli._firmware_bin(workspace, "s3"),
                    expected / "litch-esp32s3" / "firmware.bin",
                )
                self.assertEqual(
                    verify.call_args.args[2],
                    expected / "litch-esp32s3" / "firmware.elf",
                )

    def test_interpolated_build_paths_reject_before_build(self) -> None:
        for overrides in (
            {"PLATFORMIO_BUILD_DIR": "${PROJECT_DIR}/preview"},
            {"PLATFORMIO_BUILD_DIR": "${platformio.workspace_dir}/preview"},
            {"PLATFORMIO_WORKSPACE_DIR": "${PROJECT_DIR}/workspace"},
        ):
            runner = FakeRunner()
            with (
                self.subTest(overrides=overrides),
                mock.patch.dict(os.environ, {
                    "PLATFORMIO_BUILD_DIR": "", "PLATFORMIO_WORKSPACE_DIR": "", **overrides,
                }),
                mock.patch.object(cli, "_verify_usb_configuration") as verify,
            ):
                with self.assertRaisesRegex(DevError, "literal absolute or project-relative path"):
                    cli._build_one(Path("/repo/worktree"), "s3", runner)
                self.assertEqual(runner.calls, [])
                verify.assert_not_called()

    def test_literal_build_override_ignores_unused_workspace_interpolation(self) -> None:
        workspace = FakeWorkspace(Path("/repo/worktree"))
        with mock.patch.dict(os.environ, {
            "PLATFORMIO_BUILD_DIR": "/tmp/preview",
            "PLATFORMIO_WORKSPACE_DIR": "${PROJECT_DIR}/workspace",
        }):
            self.assertEqual(
                cli._firmware_bin(workspace, "s3"),
                Path("/tmp/preview/litch-esp32s3/firmware.bin"),
            )

    def test_code_health_is_one_passthrough_and_keeps_reporter_arguments(self) -> None:
        parsed = cli._parser().parse_args(
            ["code-health", "native-coverage", "--backend", "llvm"]
        )
        self.assertEqual(
            parsed.code_health_args,
            ["native-coverage", "--backend", "llvm"],
        )

        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = FakeRunner()
            with mock.patch.object(cli, "LockManager", FakeLockManager):
                cli._code_health(workspace, parsed.code_health_args, runner, 0)
        self.assertEqual(
            runner.calls[0]["argv"],
            (
                str(Path(cli.sys.executable)),
                str(workspace.root / "tools" / "code_health" / "code_health.py"),
                "native-coverage",
                "--backend",
                "llvm",
            ),
        )

    def test_code_health_forwards_reporter_help(self) -> None:
        parsed = cli._parser().parse_args(["code-health", "--help"])
        self.assertTrue(parsed.code_health_help)
        self.assertEqual(parsed.code_health_args, [])

    def test_release_check_verifies_artifacts_without_running_unit_tests(self) -> None:
        # The outer command deadline covers both inventory generations.
        deadlines = []
        children = []
        commands = []
        real_spawn = runner_module._spawn_gated_noninteractive

        def spawn(argv, **kwargs):
            commands.append(tuple(str(value) for value in argv))
            process, gate_fd, error_fd = real_spawn(
                [cli.sys.executable, "-c", "pass"], **kwargs
            )
            children.append(process)
            real_communicate = process.communicate

            def communicate(timeout):
                deadlines.append(timeout)
                return real_communicate(timeout=5)

            process.communicate = communicate
            return process, gate_fd, error_fd

        with tempfile.TemporaryDirectory() as temporary:
            workspace = FakeWorkspace(Path(temporary))
            runner = Runner(output=StringIO(), announce_commands=False)
            with (
                mock.patch.object(cli.Workspace, "discover", return_value=workspace),
                mock.patch.object(cli, "LockManager", FakeLockManager),
                mock.patch.object(runner_module, "_spawn_gated_noninteractive", side_effect=spawn),
            ):
                self.assertEqual(
                    cli.run(cli._parser().parse_args(["release", "check"]), runner=runner), 0
                )
        self.assertEqual(commands, [(cli.sys.executable, "tools/release/verify_release_artifacts.py")])
        self.assertEqual(len(deadlines), 1)
        self.assertGreater(deadlines[0], 2 * 120)
        self.assertTrue(all(child.poll() is not None for child in children))

    def test_firmware_release_check_packages_the_captured_build_from_this_checkout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace = FakeWorkspace(root)
            workspace.head = "b" * 40
            runner = FakeRunner()
            with (
                mock.patch.object(cli.Workspace, "discover", return_value=workspace),
                mock.patch.object(cli, "LockManager", FakeLockManager),
                mock.patch.dict(os.environ, {"PLATFORMIO_BUILD_DIR": str(root / "pio-build")}),
            ):
                args = cli._parser().parse_args(["release", "check", "--firmware"])
                self.assertEqual(cli.run(args, runner=runner), 0)
            commands = [call["argv"] for call in runner.calls]
            self.assertIn("tools/release/verify_release_artifacts.py", commands[0])
            self.assertIn(("pio", "run", "-e", "litch-esp32s3"), commands)
            self.assertEqual(commands[-2], ("pio", "run", "-e", "litch-esp32s3", "-t", "relink-info"))
            self.assertEqual(commands[-1][:-1], (
                cli.sys.executable, "tools/release/firmware_compliance.py", "prepare",
                "--root", str(root), "--build-dir", str(root / "pio-build/litch-esp32s3"),
                "--source-sha", workspace.head, "--output",
            ))
            self.assertFalse(Path(commands[-1][-1]).exists())

    def test_lock_timeout_must_be_finite(self) -> None:
        for value in ("nan", "inf", "-1"):
            with self.subTest(value=value):
                args = cli._parser().parse_args(["--lock-timeout", value, "bin", "s3"])
                with self.assertRaisesRegex(Exception, "finite and non-negative"):
                    cli.run(args, runner=FakeRunner())


if __name__ == "__main__":
    unittest.main()
