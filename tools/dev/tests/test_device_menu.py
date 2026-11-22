from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.dev import cli, device_menu, hardware, local_state
from tools.dev.devices import PhysicalUsbIdentity
from tools.dev.locks import LockError
from tools.dev.runner import CompletedCommand, DevError


def board(serial="board-a", *, location="1-1", port="/dev/ttyACM0", app=True):
    return {"port": port, "vid": "303A", "pid": "4001" if app else "1001",
            "serial_number": serial, "location": location}


class FixtureWorkspace:
    def __init__(self, root):
        self.root = root
        self.common_git_dir = root / ".git"
        self.head = "056e42255420aa62b606f1a5c947f7920b1af320"
        self.branch = "fixture"


class FixtureRunner:
    """Only deterministic JSON inventory and command receipts; no subprocesses."""

    def __init__(self, snapshots):
        self.snapshots = list(snapshots)
        self.calls = []
        self.index = 0

    def run(self, argv, **kwargs):
        argv = tuple(str(item) for item in argv)
        self.calls.append(argv)
        if argv == ("pio", "device", "list", "--json-output"):
            index = min(self.index, len(self.snapshots) - 1)
            self.index += 1
            snapshot = self.snapshots[index]
            if isinstance(snapshot, Exception):
                raise snapshot
            return CompletedCommand(argv, 0, json.dumps(snapshot), "")
        if argv == ("pio", "system", "info", "--json-output"):
            return CompletedCommand(argv, 0, '{"core_dir":{"value":"/fixture/pio"}}', "")
        if any(item.endswith("/tools/dev/verify_usb_configuration.py") for item in argv):
            return CompletedCommand(argv, 0, "fixture USB configuration verified", "")
        if any(item.endswith("/tools/dev/check_sleep_sdk_wake.py") for item in argv):
            return CompletedCommand(argv, 0, "fixture sleep SDK verified", "")
        if argv[:2] == ("pio", "run"):
            return CompletedCommand(argv, 0, "fixture build complete", "")
        if argv == ("fixture-esptool", "version"):
            return CompletedCommand(argv, 0, "esptool v5.3.1", "")
        raise AssertionError(f"unexpected fixture command: {argv}")


class FixtureLocks:
    def __init__(self, *args, **kwargs):
        pass

    def lane(self, *args, **kwargs):
        return contextlib.nullcontext()

    def device(self, *args, **kwargs):
        return contextlib.nullcontext()


class DeviceMenuTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.workspace = FixtureWorkspace(Path(self.temporary.name))
        self.state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(hardware, "LockManager", FixtureLocks).start()
        mock.patch.object(cli, "LockManager", FixtureLocks).start()

    def session(self, inputs, snapshots, *, flash=None):
        runner = FixtureRunner(snapshots)
        terminal = mock.Mock()
        terminal.isatty.return_value = True
        output = io.StringIO()
        with (
            mock.patch.object(cli.Workspace, "discover", return_value=self.workspace),
            mock.patch.object(cli, "Runner", return_value=runner),
            mock.patch.object(cli.sys, "stdin", terminal),
            mock.patch("builtins.input", side_effect=inputs),
            mock.patch.object(hardware, "flash_exact_device", side_effect=flash) as dispatch,
            contextlib.redirect_stdout(output), contextlib.redirect_stderr(output),
        ):
            result = cli.main([])
        return result, output.getvalue(), runner, dispatch

    def test_empty_invalid_refresh_exit_never_builds_or_writes(self):
        result, output, runner, dispatch = self.session(["bad", "f", "l", "q"], [[], []])
        self.assertEqual(result, 0)
        self.assertIn("No possible boards connected", output)
        self.assertIn("No boards selected", output)
        self.assertIn("Use a connected board number", output)
        self.assertIn("litch: exited", output)
        self.assertEqual(runner.index, 2)
        dispatch.assert_not_called()
        self.assertFalse(self.state_path.exists())

    def test_one_known_board_needs_only_number_and_flash_then_remembers(self):
        result, output, runner, dispatch = self.session(["1", "f", "q"], [[board()]])
        self.assertEqual(result, 0)
        self.assertNotIn("Target [", output)
        self.assertIn("flash complete", output)
        self.assertIn("All 1 selected", output)
        self.assertEqual(dispatch.call_args.kwargs["target"], "s3")
        self.assertEqual(dispatch.call_args.kwargs["expected_usb_identity"], PhysicalUsbIdentity("board-a", "1-1"))
        self.assertEqual(local_state.usb_choices(self.state_path), {"board-a": {"target": "s3", "selected": True}})
        # Existing successful flash slots remain owned by the exact-device path.
        self.assertEqual(local_state.read(self.state_path)["s3_firmware"]["usb_identity"], "board-a")

    def test_multiple_selection_preserves_order_and_routes_sequentially(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1")]
        result, output, _, dispatch = self.session(["2,1", "f", "q"], [devices])
        self.assertEqual(result, 0)
        self.assertEqual([call.kwargs["usb_identity"] for call in dispatch.call_args_list], ["board-a", "board-b"])
        self.assertIn("All 2 selected board(s) flashed", output)

    def test_unknown_target_cancel_or_invalid_never_builds(self):
        result, output, runner, dispatch = self.session(["1", "wrong", "q", "f", "q"], [[board(app=False)]])
        self.assertEqual(result, 0)
        self.assertIn("target unknown", output)
        self.assertIn("Target not recognized", output)
        self.assertIn("selection unchanged", output)
        dispatch.assert_not_called()
        self.assertFalse(self.state_path.exists())
        self.assertTrue(all(argv[:3] == ("pio", "device", "list") for argv in runner.calls))

    def test_explicit_mixed_targets_fail_stops_remaining_and_retains_retry_selection(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1", app=False),
                   board("board-c", location="1-3", port="/dev/ttyACM2")]
        def flash(**kwargs):
            if kwargs["target"] == "c3":
                raise DevError("fixture runtime proof failed")
        result, output, _, dispatch = self.session(["1 2 3", "c3", "f", "q"], [devices], flash=flash)
        self.assertEqual(result, 0)
        self.assertEqual([call.kwargs["target"] for call in dispatch.call_args_list], ["s3", "c3"])
        self.assertIn("1 completed; 1 remaining board(s) were not flashed", output)
        self.assertNotIn("All 3 selected", output)
        choices = local_state.usb_choices(self.state_path)
        self.assertFalse(choices["board-a"]["selected"])
        self.assertTrue(choices["board-b"]["selected"])
        self.assertTrue(choices["board-c"]["selected"])
        self.assertIsNone(local_state.read(self.state_path)["c3_firmware"])

    def test_remembered_selection_edits_and_refresh_clears_without_port_authority(self):
        local_state.remember_usb_choices(self.state_path, {
            "board-a": {"target": "s3", "selected": True},
            "missing": {"target": "s3", "selected": True},
        })
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1")]
        result, output, _, dispatch = self.session(["2", "l", "f", "0", "l", "q"], [devices])
        self.assertEqual(result, 0)
        self.assertIn("Remembered missing: not connected (not selected)", output)
        self.assertEqual(dispatch.call_args.kwargs["usb_identity"], "board-b")
        self.assertFalse(local_state.usb_choices(self.state_path)["board-b"]["selected"])
        self.assertIn("[*] board-a", output.replace("board-a |", "board-a"))

    def test_clear_remembered_disconnected_board_does_not_select_on_reconnect(self):
        local_state.remember_usb_choices(self.state_path, {
            "board-a": {"target": "s3", "selected": True},
            "missing": {"target": "s3", "selected": True},
        })
        reconnected = [board(), board("missing", location="1-2", port="/dev/ttyACM1")]
        result, output, _, dispatch = self.session(["0", "l", "f", "q"], [[board()], reconnected])
        self.assertEqual(result, 0)
        self.assertIn("No boards selected", output)
        dispatch.assert_not_called()
        self.assertEqual(local_state.usb_choices(self.state_path), {
            "board-a": {"target": "s3", "selected": False},
            "missing": {"target": "s3", "selected": False},
        })
        _, restart_output, _, restart_dispatch = self.session(["f", "2", "f", "q"], [reconnected])
        self.assertIn("No boards selected", restart_output)
        self.assertEqual([call.kwargs["usb_identity"] for call in restart_dispatch.call_args_list], ["missing"])

    def test_batch_failures_show_unfinished_selection_and_immediate_retry_skips_completed(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1"),
                   board("board-c", location="1-3", port="/dev/ttyACM2")]
        for failure in (DevError("fixture write failed"), LockError("fixture lock occupied")):
            with self.subTest(failure=type(failure).__name__):
                local_state.remember_usb_choices(self.state_path, {})
                result, output, _, dispatch = self.session(
                    ["1 2 3", "f", "f", "q"], [devices], flash=[None, failure, None, None],
                )
                self.assertEqual(result, 0)
                self.assertEqual([call.kwargs["usb_identity"] for call in dispatch.call_args_list],
                                 ["board-a", "board-b", "board-b", "board-c"])
                retry_view = output.split("Retry selection (unfinished boards):", 1)[1].split("litch> ", 1)[0]
                self.assertIn("[ ] board-a", retry_view)
                self.assertIn("[*] board-b", retry_view)
                self.assertIn("[*] board-c", retry_view)
                self.assertIn("Selected: 2 board(s)", retry_view)
                self.assertFalse(local_state.usb_choices(self.state_path)["board-a"]["selected"])

    def test_ctrl_c_mid_batch_remembers_only_unfinished_boards_for_restart(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1")]
        result, output, _, dispatch = self.session(["1 2", "f"], [devices], flash=[None, KeyboardInterrupt()])
        self.assertEqual(result, 130)
        self.assertEqual(dispatch.call_count, 2)
        self.assertIn("Retry selection remembered for the next start", output)
        self.assertIn("[ ] board-a", output.split("Retry selection", 1)[1])
        self.assertFalse(local_state.usb_choices(self.state_path)["board-a"]["selected"])
        _, restart_output, _, restart_dispatch = self.session(["f", "q"], [devices])
        self.assertIn("Selected: 1 board(s)", restart_output)
        self.assertEqual([call.kwargs["usb_identity"] for call in restart_dispatch.call_args_list], ["board-b"])

    def test_number_entry_replaces_previous_selection_and_explains_complete_set(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1")]
        result, output, _, dispatch = self.session(["1", "2", "f", "q"], [devices])
        self.assertEqual(result, 0)
        self.assertIn("Enter all board numbers to select", output)
        self.assertEqual([call.kwargs["usb_identity"] for call in dispatch.call_args_list], ["board-b"])

    def test_ctrl_c_between_callbacks_preserves_completed_boundary(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1")]
        original_print = print
        def interrupt_before_second_callback(*args, **kwargs):
            if args and args[0] == "litch: ./litch flash s3 --device board-b":
                raise KeyboardInterrupt()
            original_print(*args, **kwargs)
        with mock.patch("builtins.print", side_effect=interrupt_before_second_callback):
            result, output, _, dispatch = self.session(["1 2", "f"], [devices])
        self.assertEqual(result, 130)
        self.assertEqual([call.kwargs["usb_identity"] for call in dispatch.call_args_list], ["board-a"])
        self.assertIn("1 completed", output)
        self.assertFalse(local_state.usb_choices(self.state_path)["board-a"]["selected"])
        self.assertTrue(local_state.usb_choices(self.state_path)["board-b"]["selected"])

    def test_control_bearing_usb_metadata_is_quoted_and_discovery_can_recover(self):
        for complete in (True, False):
            for field in ("serial_number", "port", "location", "description", "hwid"):
                with self.subTest(complete=complete, field=field):
                    malformed = board(location="1-1" if complete else None)
                    malformed[field] = "bad\x1b[2Jlabel"
                    result, output, _, dispatch = self.session(["f", "l", "q"], [[malformed], [board()]])
                    self.assertEqual(result, 0)
                    self.assertIn("Device list unavailable", output)
                    self.assertIn(f"invalid {field}", output)
                    self.assertIn("bad\\x1b[2Jlabel", output)
                    self.assertNotIn("\x1b", output)
                    self.assertIn("Refresh the device list", output)
                    self.assertIn("board-a | S3", output)
                    dispatch.assert_not_called()

    def test_printable_international_usb_serial_remains_selectable(self):
        result, output, _, dispatch = self.session(["1", "q"], [[board("Litch-\u00e9")]])
        self.assertEqual(result, 0)
        self.assertIn("[*] litch-\u00e9", output)
        dispatch.assert_not_called()

    def test_saved_legacy_target_never_advertises_or_selects_it_at_startup(self):
        local_state.remember_usb_choices(self.state_path, {"board-a": {"target": "c3", "selected": True}})
        result, output, _, dispatch = self.session(["f", "q"], [[board(app=False)]])
        self.assertEqual(result, 0)
        self.assertIn("target unknown", output)
        self.assertNotIn("c3", output.casefold())
        dispatch.assert_not_called()

    def test_unknown_saved_s3_target_is_editable_and_legacy_requires_explicit_input(self):
        local_state.remember_usb_choices(self.state_path, {"board-a": {"target": "s3", "selected": True}})
        result, output, _, dispatch = self.session(["t 1", "c3", "f", "q"], [[board(app=False)]])
        self.assertEqual(result, 0)
        self.assertEqual(dispatch.call_args.kwargs["target"], "c3")
        self.assertEqual(local_state.usb_choices(self.state_path)["board-a"]["target"], "c3")

    def test_known_s3_never_asks_for_a_model_override(self):
        result, output, _, dispatch = self.session(["t 1", "q"], [[board()]])
        self.assertEqual(result, 0)
        self.assertIn("identifies itself as S3", output)
        self.assertNotIn("USB boot mode", output)
        dispatch.assert_not_called()

    def test_new_board_at_old_port_does_not_inherit_selection_or_target(self):
        local_state.remember_usb_choices(self.state_path, {"old": {"target": "s3", "selected": True}})
        result, output, _, dispatch = self.session(["f", "q"], [[board("new", app=False)]])
        self.assertEqual(result, 0)
        self.assertIn("Remembered old: not connected", output)
        self.assertIn("target unknown", output)
        dispatch.assert_not_called()

    def test_replaced_board_after_selection_is_rejected_before_flash_dispatch(self):
        original = board()
        replacement = board("replacement")
        result, output, _, dispatch = self.session(["1", "f", "l", "q"], [[original], [replacement]])
        self.assertEqual(result, 0)
        self.assertIn("no USB Litch board matched selector", output)
        dispatch.assert_not_called()
        self.assertIn("Remembered board-a: not connected", output)

    def test_same_serial_at_different_location_after_selection_is_rejected(self):
        result, output, _, dispatch = self.session(["1", "f", "q"], [[board()], [board(location="1-9")]])
        self.assertEqual(result, 0)
        self.assertIn("selected USB identity changed", output)
        dispatch.assert_not_called()

    def test_each_sequential_flash_revalidates_identity(self):
        devices = [board(), board("board-b", location="1-2", port="/dev/ttyACM1")]
        replaced = [devices[0], board("replacement", location="1-2", port="/dev/ttyACM1")]
        result, output, _, dispatch = self.session(["1 2", "f", "q"], [devices, devices, replaced])
        self.assertEqual(result, 0)
        self.assertEqual(dispatch.call_count, 1)
        self.assertIn("Flash stopped at board-b", output)
        self.assertNotIn("All 2 selected", output)

    def test_incomplete_identity_stays_visible_and_selection_is_atomic(self):
        incomplete = board("incomplete", location=None, port="/dev/ttyACM1")
        result, output, runner, dispatch = self.session(["1 2", "f", "q"], [[board(), incomplete]])
        self.assertEqual(result, 0)
        self.assertIn("cannot select: USB identity incomplete", output)
        self.assertIn("no complete USB identity", output)
        dispatch.assert_not_called()
        self.assertFalse(self.state_path.exists())
        self.assertEqual(runner.index, 1)

    def test_duplicate_serial_fails_closed_before_saving_or_build(self):
        result, output, _, dispatch = self.session(["1", "f", "q"], [[board(), board(location="1-2", port="/dev/ttyACM1")]])
        self.assertEqual(result, 0)
        self.assertIn("USB serial is ambiguous", output)
        dispatch.assert_not_called()
        self.assertFalse(self.state_path.exists())

    def test_remembered_duplicate_serial_is_visible_but_never_selected(self):
        local_state.remember_usb_choices(self.state_path, {"board-a": {"target": "s3", "selected": True}})
        result, output, _, dispatch = self.session(["f", "q"], [[board(), board(location="1-2", port="/dev/ttyACM1")]])
        self.assertEqual(result, 0)
        self.assertIn("cannot select: duplicate USB serial", output)
        self.assertNotIn("[*]", output)
        dispatch.assert_not_called()

    def test_discovery_failure_keeps_exit_and_retry_available_without_flashing(self):
        result, output, _, dispatch = self.session(["f", "l", "q"], [DevError("fixture inventory failed"), [board()]])
        self.assertEqual(result, 0)
        self.assertIn("Device list unavailable", output)
        self.assertIn("Refresh the device list", output)
        self.assertIn("board-a | S3", output)
        dispatch.assert_not_called()

    def test_inventory_lock_failure_keeps_retry_and_exit_available(self):
        result, output, _, dispatch = self.session(["f", "l", "q"], [LockError("fixture list lock occupied"), [board()]])
        self.assertEqual(result, 0)
        self.assertIn("Device list unavailable: fixture list lock occupied", output)
        self.assertIn("Refresh the device list", output)
        self.assertIn("board-a | S3", output)
        dispatch.assert_not_called()

    def test_eof_and_ctrl_c_produce_calm_exit(self):
        for interruption, code, diagnostic in ((EOFError(), 2, "menu input ended"),
                                                (KeyboardInterrupt(), 130, "interrupted")):
            with self.subTest(interruption=type(interruption).__name__):
                result, output, _, dispatch = self.session([interruption], [[]])
                self.assertEqual(result, code)
                self.assertIn(diagnostic, output)
                dispatch.assert_not_called()

    def test_more_commands_can_return_with_q(self):
        result, output, _, _ = self.session(["m", "q", "q"], [[]])
        self.assertEqual(result, 0)
        self.assertIn("Returned to the device list", output)
        self.assertNotIn("Flash firmware", output)

    def test_selection_remembers_normalized_serial_and_survives_port_changes(self):
        original = board("AA:BB:CC:DD:EE:FF")
        self.session(["1", "q"], [[original]])
        result, output, _, dispatch = self.session(["f", "q"], [[board("aabbccddeeff", port="/dev/ttyACM9")]])
        self.assertEqual(result, 0)
        self.assertIn("[*] aabbccddeeff", output)
        self.assertEqual(dispatch.call_args.kwargs["usb_identity"], "aabbccddeeff")

    def test_saved_preferences_are_bounded_and_malformed_rows_never_select(self):
        for value in ([{"serial": "board-a", "target": "bad", "selected": True}],
                      [{"serial": "board-a\n", "target": "s3", "selected": True},
                       {"serial": "board-a", "target": "s3", "selected": True}],
                      [{"serial": "x", "target": "s3"}] * 17):
            local_state.remember(self.state_path, "usb_choices", value)
            self.assertEqual(local_state.usb_choices(self.state_path), {})

    def test_guard_under_device_lock_rejects_location_drift_before_build(self):
        runner = FixtureRunner([[board(location="1-9")]])
        build = mock.Mock()
        with (
            mock.patch.object(hardware.PySerialFactory, "require_available"),
            mock.patch.object(hardware, "_flash_s3_usb") as write,
        ):
            with self.assertRaisesRegex(DevError, "selected USB identity changed"):
                hardware.flash_exact_device(
                    workspace=self.workspace, target="s3", usb_identity="board-a", ble_identity=None,
                    runner=runner, lock_timeout=0, build_target=build,
                    platformio_core_dir=lambda: Path("/fixture/pio"),
                    expected_usb_identity=PhysicalUsbIdentity("board-a", "1-1"),
                )
        build.assert_not_called()
        write.assert_not_called()

    def test_legacy_identity_drift_during_build_prevents_upload(self):
        runner = FixtureRunner([[board(app=False)], [board(location="1-9", app=False)]])
        with (
            mock.patch.object(hardware.PySerialFactory, "require_available"),
            mock.patch.object(hardware, "_platformio_upload") as write,
        ):
            with self.assertRaisesRegex(DevError, "selected USB identity changed during build"):
                hardware.flash_exact_device(
                    workspace=self.workspace, target="c3", usb_identity="board-a", ble_identity=None,
                    runner=runner, lock_timeout=0, build_target=lambda target: None,
                    platformio_core_dir=lambda: self.fail("legacy target must not request S3 core"),
                    expected_usb_identity=PhysicalUsbIdentity("board-a", "1-1"),
                )
        write.assert_not_called()

    def test_actual_cli_dispatch_builds_only_explicitly_selected_targets(self):
        devices = [board(app=False), board("board-b", location="1-2", port="/dev/ttyACM1", app=False)]
        runner = FixtureRunner([devices])
        output = io.StringIO()
        terminal = mock.Mock()
        terminal.isatty.return_value = True
        with (
            mock.patch.object(cli.Workspace, "discover", return_value=self.workspace),
            mock.patch.object(cli, "Runner", return_value=runner),
            mock.patch.object(cli, "platformio_sleep_dependencies", return_value=(
                Path("/fixture/sdk/esp32s3"), Path("/fixture/ArduinoJson/src"))),
            mock.patch.object(cli.sys, "stdin", terminal),
            mock.patch("builtins.input", side_effect=["1 2", "s3", "c3", "f", "q"]),
            mock.patch.object(hardware.PySerialFactory, "require_available"),
            mock.patch.object(hardware, "_esptool_argv", return_value=("fixture-esptool",)),
            mock.patch.object(hardware, "_flash_s3_usb", return_value=mock.sentinel.s3_context) as s3_write,
            mock.patch.object(hardware, "_upload_c3", return_value=mock.sentinel.c3_context) as c3_write,
            mock.patch.object(hardware, "_verify_c3_runtime") as c3_verify,
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(cli.main([]), 0)
        self.assertEqual([argv[argv.index("-e") + 1] for argv in runner.calls if argv[:2] == ("pio", "run")],
                         ["litch-esp32s3", "litch-esp32c3"])
        self.assertEqual(s3_write.call_count, 1)
        self.assertEqual(
            [argv for argv in runner.calls
             if any(item.endswith("/tools/dev/check_sleep_sdk_wake.py") for item in argv)],
            [(sys.executable, str(self.workspace.root / "tools/dev/check_sleep_sdk_wake.py"),
              "--sdk", "/fixture/sdk/esp32s3", "--json-include", "/fixture/ArduinoJson/src")],
        )
        self.assertEqual(c3_write.call_count, 1)
        self.assertEqual(runner.calls.count(("fixture-esptool", "version")), 1)
        c3_verify.assert_called_once_with(mock.sentinel.c3_context, workspace=self.workspace, runner=runner)
        self.assertIn("All 2 selected board(s) flashed", output.getvalue())


if __name__ == "__main__":
    unittest.main()
