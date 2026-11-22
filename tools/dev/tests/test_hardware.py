from __future__ import annotations

import contextlib
import asyncio
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.dev import hardware, runtime_verify, ota_trial
from tools.dev import local_state
from tools.dev.devices import (
    ESP32S3_ROM_PID,
    ESPRESSIF_VID,
    LITCH_APP_PID,
    DeviceInventoryError,
    UsbTransitionTimeout,
    PhysicalUsbIdentity,
    UsbParent,
)
from tools.dev.runner import CompletedCommand, DevError, Runner
from tools.dev.runtime_verify import RuntimeVerificationError
from tools.dev.tests.test_runtime_verify import (
    DelayedSerialTransport, FakeSerialFactory, FakeSerialTransport,
)
from tools import ble_update_firmware


TEST_BLE_UUID = "00000000-0000-0000-0000-000000000001"
TEST_S3_MAC = "24:6f:28:ab:cd:ef"




_BLOCKING_SERIAL_CHILD = r"""
import json
import sys
import time
from pathlib import Path

from tools.dev.runtime_verify import SERIAL_PROBE_SCHEMA, verify_serial_version

phase, marker_path = sys.argv[1:3]

def block(name):
    if phase == name:
        Path(marker_path).write_text(name, encoding="utf-8")
        while True:
            time.sleep(3600)

class Transport:
    def set_control_lines(self, *, dtr, rts):
        block("set_control_lines")

    def reset_input_buffer(self):
        block("reset_input_buffer")

    def write(self, data):
        block("write")
        return len(data)

    def read(self, size):
        block("read")
        return b"firmware: 0.7\n"

    def close(self):
        block("close")

class Factory:
    def open(self, port, *, baudrate, read_timeout_seconds, dtr, rts):
        block("open")
        return Transport()

actual = verify_serial_version(
    Factory(),
    port="/dev/cu.fake",
    expected_version="0.7",
)
print(json.dumps({"schema": SERIAL_PROBE_SCHEMA, "version": actual}))
"""


class FakeWorkspace:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.common_git_dir = root / ".git"
        self.branch = "test"
        self.head = "a" * 40


class FakeRunner:
    def __init__(self, results: list[CompletedCommand] | None = None) -> None:
        self.results = list(results or [])
        self.calls: list[dict[str, object]] = []

    def run(self, argv, **kwargs):
        normalized = tuple(str(value) for value in argv)
        self.calls.append({"argv": normalized, **kwargs})
        if self.results:
            return self.results.pop(0)
        return CompletedCommand(normalized, 0, "", "")


class FakeInventory:
    def __init__(self, snapshots) -> None:
        self.snapshots = [tuple(snapshot) for snapshot in snapshots]
        self.index = 0

    def snapshot(self):
        index = min(self.index, len(self.snapshots) - 1)
        self.index += 1
        return self.snapshots[index]


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class RecordingContext:
    def __init__(self, events: list[str], name: str) -> None:
        self.events = events
        self.name = name

    def __enter__(self):
        self.events.append(f"enter:{self.name}")
        return self

    def __exit__(self, *args):
        self.events.append(f"exit:{self.name}")


class FakeLockManager:
    def __init__(self, events: list[str]) -> None:
        self.events = events

    def device(self, serial: str, *, timeout: float):
        self.events.append(f"device:{serial}:{timeout}")
        return RecordingContext(self.events, "device")

    def lane(self, lane: str, *, timeout: float):
        self.events.append(f"lane:{lane}:{timeout}")
        return RecordingContext(self.events, "lane")


def parent(serial: str, location: str, pid: int, port: str) -> UsbParent:
    return UsbParent(
        PhysicalUsbIdentity(serial, location),
        ESPRESSIF_VID,
        pid,
        (port,),
    )


class SelectionAndAdapterTests(unittest.TestCase):
    def test_governed_rom_entry_waits_for_durable_terminal_without_cdc_touch(self) -> None:
        events = []

        class Transport:
            def reset_input_buffer(self):
                events.append("clear")

            def write(self, data):
                events.append(("write", data))
                return len(data)

            def read(self, size):
                return b"> reboot bootloader\r\nreboot bootloader ready: state saved; entering ROM\r\n"

            def set_control_lines(self, *, dtr, rts):
                events.append(("lines", dtr, rts))

            def close(self):
                events.append("close")

        factory = mock.Mock()
        factory.open.return_value = Transport()
        with mock.patch.object(hardware, "PySerialFactory", return_value=factory):
            hardware.touch_s3_application_bootloader("/dev/cu.exact-app")
        self.assertEqual([event for event in events if isinstance(event, tuple) and event[0] == "write"],
                         [("write", b"reboot bootloader\n")])
        self.assertEqual([event for event in events if isinstance(event, tuple) and event[0] == "lines"],
                         [("lines", False, False)])
        factory.open.assert_called_once_with("/dev/cu.exact-app", baudrate=115200,
                                             read_timeout_seconds=0.1, dtr=True, rts=True)

    def test_missing_esptool_points_to_the_pinned_installer(self) -> None:
        with mock.patch.object(hardware, "resolve_esptool_argv", return_value=None):
            with self.assertRaisesRegex(
                DevError,
                r"\./litch bootstrap firmware.*esptool==4\.9\.0",
            ):
                hardware._esptool_argv()

    def test_serial_selection_requires_one_exact_physical_parent_and_pid(self) -> None:
        rom = parent("ABC", "1-2", ESP32S3_ROM_PID, "/dev/cu.rom")
        self.assertIs(
            hardware.select_parent_by_serial(
                (rom,),
                serial="abc",
                vid=ESPRESSIF_VID,
                pid=ESP32S3_ROM_PID,
            ),
            rom,
        )
        invalid = (
            (),
            (rom, parent("ABC", "9-9", ESP32S3_ROM_PID, "/dev/cu.other")),
            (parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app"),),
        )
        for parents in invalid:
            with self.subTest(parents=parents):
                with self.assertRaises(DeviceInventoryError):
                    hardware.select_parent_by_serial(
                        parents,
                        serial="ABC",
                        vid=ESPRESSIF_VID,
                        pid=ESP32S3_ROM_PID,
                    )

    def test_bound_runner_adds_cwd_capture_no_check_and_telemetry_policy(self) -> None:
        runner = FakeRunner()
        root = Path("/repo")
        bound = hardware._BoundCaptureRunner(runner, cwd=root)
        bound.run(("pio", "device", "list"), timeout_seconds=2.0)
        call = runner.calls[0]
        self.assertEqual(call["cwd"], root)
        self.assertEqual(call["timeout_seconds"], 2.0)
        self.assertTrue(call["capture"])
        self.assertFalse(call["check"])
        self.assertFalse(call["announce"])
        self.assertEqual(
            call["extra_env"],
            {"PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no"},
        )

    def test_factory_erase_uses_standard_tool_connection_defaults(self) -> None:
        runner = FakeRunner()
        bound = hardware._BoundCaptureRunner(runner, cwd=Path("/repo"))
        bound.run(("esptool", "--port", "/dev/cu.new", "erase-flash"),
                  timeout_seconds=120.0)
        self.assertEqual(len(runner.calls), 1)
        self.assertNotIn("ESPTOOL_OPEN_PORT_ATTEMPTS", runner.calls[0]["extra_env"])
        for command in ("write-flash", "read-flash", "chip-id"):
            bound.run(("esptool", command), timeout_seconds=120.0)
            self.assertNotIn("ESPTOOL_OPEN_PORT_ATTEMPTS", runner.calls[-1]["extra_env"])


    def test_bound_runner_verbose_mode_announces_commands(self) -> None:
        runner = FakeRunner()
        bound = hardware._BoundCaptureRunner(runner, cwd=Path("/repo"), verbose=True)

        bound.run(("pio", "device", "list"), timeout_seconds=2.0)

        self.assertTrue(runner.calls[0]["announce"])

    def test_bound_runner_surfaces_sanitized_esptool_write_progress(self) -> None:
        output = io.StringIO()

        class ProgressRunner(FakeRunner):
            def run(self, argv, **kwargs):
                callback = kwargs["live_output"]
                callback(
                    "stdout",
                    "\x1b[2KWriting at 0x00001000 [==] 5.0% 5/100 bytes...\r"
                    "\x1b[2KWriting at 0x00002000 [====] 15.0% 15/100 bytes...\r",
                )
                callback(
                    "stdout",
                    "\x1b[2KWrote 100 bytes at 0x00000000 in 0.1 seconds.\n"
                    "Hash of data verified.\n",
                )
                return super().run(argv, **kwargs)

        runner = ProgressRunner()
        bound = hardware._BoundCaptureRunner(runner, cwd=Path("/repo"), output=output)

        bound.run(("esptool", "write-flash"), timeout_seconds=2.0)

        rendered = output.getvalue()
        self.assertIn("S3 flash: writing 5.0%", rendered)
        self.assertIn("S3 flash: writing 15.0%", rendered)
        self.assertIn("S3 flash: wrote segment at 0x00000000", rendered)
        self.assertNotIn("\x1b", rendered)
        self.assertNotIn("Hash of data verified", rendered)

    def test_bound_runner_turns_launch_uncertainty_into_narrow_os_error(self) -> None:
        class FailingRunner:
            def run(self, *args, **kwargs):
                raise DevError("uncertain timeout")

        with self.assertRaisesRegex(OSError, "uncertain timeout"):
            hardware._BoundCaptureRunner(FailingRunner(), cwd=Path("/repo")).run(
                ("esptool", "chip-id"),
                timeout_seconds=1.0,
            )

    def test_governed_request_waits_through_pending_save(self) -> None:
        chunks = iter([b"> reboot bootloader\r\nreboot bootloader pending: saving state\r\n",
                       b"", b"reboot bootloader ready: state saved; entering ROM\r\n"])
        transport = mock.Mock()
        transport.write.side_effect = len
        transport.read.side_effect = lambda size: next(chunks)
        factory = mock.Mock()
        factory.open.return_value = transport
        with mock.patch.object(hardware, "PySerialFactory", return_value=factory):
            hardware.touch_s3_application_bootloader("/dev/cu.app")
        self.assertEqual(transport.read.call_count, 3)
        transport.write.assert_called_once_with(b"reboot bootloader\n")
        transport.set_control_lines.assert_called_once_with(dtr=False, rts=False)

    def test_governed_request_refuses_failure_legacy_and_partial_terminal(self) -> None:
        for reply in (b"reboot bootloader rejected: busy\r\n",
                      b"reboot bootloader aborted: settings save failed\r\n",
                      b"OK\r\n", b"unknown command\r\n",
                      b"reboot bootloader ready: state saved; entering ROM\r\n",
                      b"> reboot bootloader\r\nreboot bootloader pending: saving state\r\n"):
            with self.subTest(reply=reply):
                transport = mock.Mock()
                transport.write.side_effect = len
                transport.read.side_effect = lambda size: reply[:size]
                factory = mock.Mock()
                factory.open.return_value = transport
                with (mock.patch.object(hardware, "PySerialFactory", return_value=factory),
                      contextlib.nullcontext()):
                    with self.assertRaises(RuntimeVerificationError):
                        hardware.touch_s3_application_bootloader("/dev/cu.app")
                transport.write.assert_called_once_with(b"reboot bootloader\n")
                transport.set_control_lines.assert_called_once_with(dtr=False, rts=False)
                transport.close.assert_called_once()

    def test_governed_request_deadline_retains_partial_response(self) -> None:
        transport = mock.Mock()
        transport.write.side_effect = len
        transport.read.return_value = b"> reboot bootloader\r\nreboot bootloader pending: saving state\r\n"
        factory = mock.Mock()
        factory.open.return_value = transport
        with (mock.patch.object(hardware, "PySerialFactory", return_value=factory),
              mock.patch.object(hardware.time, "monotonic", side_effect=[0.0, 0.1, 13.1])):
            with self.assertRaisesRegex(RuntimeVerificationError, "pending: saving state"):
                hardware.touch_s3_application_bootloader("/dev/cu.app")
        transport.write.assert_called_once()

    def test_governed_request_preserves_read_error_when_close_fails(self) -> None:
        primary = OSError("controlled read failure")
        transport = mock.Mock()
        transport.write.side_effect = len
        transport.read.side_effect = primary
        transport.close.side_effect = OSError("controlled close failure")
        factory = mock.Mock()
        factory.open.return_value = transport
        with mock.patch.object(hardware, "PySerialFactory", return_value=factory):
            with self.assertRaises(OSError) as raised:
                hardware.touch_s3_application_bootloader("/dev/cu.app")
        self.assertIs(raised.exception, primary)
        self.assertTrue(any("controlled close failure" in note for note in getattr(primary, "__notes__", [])))

    def test_governed_request_rejects_partial_write(self) -> None:
        transport = mock.Mock()
        transport.write.return_value = 2
        factory = mock.Mock()
        factory.open.return_value = transport
        with mock.patch.object(hardware, "PySerialFactory", return_value=factory):
            with self.assertRaisesRegex(RuntimeVerificationError, "partially written"):
                hardware.touch_s3_application_bootloader("/dev/cu.app")
        transport.read.assert_not_called()
        transport.close.assert_called_once()

    def test_durable_terminal_retains_expected_detach_teardown_error(self) -> None:
        transport = mock.Mock()
        transport.write.side_effect = len
        transport.read.return_value = b"> reboot bootloader\r\nreboot bootloader ready: state saved; entering ROM\r\n"
        transport.set_control_lines.side_effect = OSError("APP endpoint detached")
        factory = mock.Mock()
        factory.open.return_value = transport
        with mock.patch.object(hardware, "PySerialFactory", return_value=factory):
            admission = hardware.touch_s3_application_bootloader("/dev/cu.app")
        self.assertIn("state saved", admission.response)
        self.assertIn("APP endpoint detached", admission.teardown_error)
        transport.close.assert_called_once()

        app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        rom = parent("ABC", "1-2", ESP32S3_ROM_PID, "/dev/cu.rom")
        with (mock.patch.object(hardware, "touch_s3_application_bootloader", return_value=admission) as request,
              mock.patch.object(hardware, "wait_for_exact_usb_transition", return_value=rom) as transition):
            self.assertIs(hardware.enter_s3_rom_from_application(FakeInventory(((app,),)), application=app), rom)
        request.assert_called_once()
        transition.assert_called_once()

    def test_s3_application_request_waits_for_the_same_parent_to_become_rom(self) -> None:
        app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        rom = parent("ABC", "1-2", ESP32S3_ROM_PID, "/dev/cu.rom")
        with (mock.patch.object(hardware, "touch_s3_application_bootloader") as request,
              mock.patch.object(hardware, "wait_for_exact_usb_transition", return_value=rom) as transition):
            result = hardware.enter_s3_rom_from_application(FakeInventory(((app,),)), application=app)
        self.assertIs(result, rom)
        request.assert_called_once_with("/dev/cu.app")
        self.assertEqual(transition.call_args.kwargs["identity"], app.identity)
        self.assertEqual(transition.call_args.kwargs["from_pid"], LITCH_APP_PID)
        self.assertEqual(transition.call_args.kwargs["to_pid"], ESP32S3_ROM_PID)

    def test_old_or_protected_console_falls_back_to_standard_touch_same_parent(self):
        app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        rom = parent("ABC", "1-2", ESP32S3_ROM_PID, "/dev/cu.rom")
        transport = mock.Mock()
        with (
            mock.patch.object(hardware, "touch_s3_application_bootloader", side_effect=RuntimeVerificationError("ERR owner required")),
            mock.patch.object(hardware, "admit_s3_usb_parent", return_value=app),
            mock.patch.object(hardware, "PySerialFactory") as factory,
            mock.patch.object(hardware, "close_serial_transport"),
            mock.patch.object(hardware.time, "sleep"),
            mock.patch.object(hardware, "wait_for_exact_usb_transition", return_value=rom),
            contextlib.redirect_stdout(output := io.StringIO()),
        ):
            factory.return_value.open.return_value = transport
            self.assertIs(hardware.enter_s3_rom_from_application(FakeInventory(((app,),)), application=app), rom)
        self.assertEqual(factory.return_value.open.call_args.kwargs["baudrate"], 1200)
        self.assertNotIn("flash complete", output.getvalue())

    def test_failed_touch_reports_physical_recovery_and_drift_never_touches_other_board(self):
        app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        other = parent("ABC", "1-9", LITCH_APP_PID, "/dev/cu.other")
        for selected in (app, other):
            with (
                self.subTest(location=selected.identity.location),
                mock.patch.object(hardware, "touch_s3_application_bootloader", side_effect=RuntimeVerificationError("no console")),
                mock.patch.object(hardware, "admit_s3_usb_parent", return_value=selected),
                mock.patch.object(hardware, "PySerialFactory") as factory,
                mock.patch.object(hardware, "close_serial_transport"),
                mock.patch.object(hardware.time, "sleep"),
                mock.patch.object(hardware, "wait_for_exact_usb_transition", side_effect=UsbTransitionTimeout("no ROM")),
                contextlib.redirect_stdout(io.StringIO()),
                self.assertRaisesRegex(DeviceInventoryError, "P1-X|identity changed"),
            ):
                hardware.enter_s3_rom_from_application(FakeInventory(((app,),)), application=app)
            if selected is other:
                factory.return_value.open.assert_not_called()

    def test_s3_admission_fails_before_build_when_parent_is_wrong_or_ambiguous(self) -> None:
        app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        wrong = parent("ABC", "1-2", 0x9999, "/dev/cu.other")
        with self.assertRaises(DeviceInventoryError):
            hardware.admit_s3_usb_parent(FakeInventory(((wrong,),)), serial="ABC")
        with self.assertRaises(DeviceInventoryError):
            hardware.admit_s3_usb_parent(
                FakeInventory(
                    (
                        (
                            app,
                            parent(
                                "ABC", "9-9", LITCH_APP_PID, "/dev/cu.other"
                            ),
                        ),
                    )
                ),
                serial="ABC",
            )

    def test_s3_admission_tolerates_a_transient_missing_fresh_rom_parent(self) -> None:
        rom = parent(
            "00:00:5E:00:53:DE",
            "1-1",
            ESP32S3_ROM_PID,
            "/dev/cu.usbmodem1101",
        )
        clock = FakeClock()

        admitted = hardware.admit_s3_usb_parent(
            FakeInventory(((), (rom,), (), (rom,), (rom,))),
            serial="00005e0053de",
            timeout_seconds=1.0,
            poll_interval_seconds=0.1,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )

        self.assertEqual(admitted, rom)

    def test_failed_s3_admission_does_not_build_or_start_rom_flash(self) -> None:
        workspace = FakeWorkspace(Path("/repo"))
        builds: list[str] = []
        manager = FakeLockManager([])
        with (
            mock.patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=mock.Mock())}),
            mock.patch.object(hardware, "LockManager", return_value=manager),
            mock.patch.object(
                hardware,
                "PioUsbInventory",
                return_value=FakeInventory(((),)),
            ),
            mock.patch.object(
                hardware,
                "admit_s3_usb_parent",
                side_effect=DeviceInventoryError("selected board is absent"),
            ),
            mock.patch.object(hardware, "_flash_s3_usb") as flash,
        ):
            with self.assertRaisesRegex(DevError, "selected board is absent"):
                hardware.flash_exact_device(
                    workspace=workspace,
                    target="s3",
                    usb_identity="ABC",
                    ble_identity=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    build_target=builds.append,
                    platformio_core_dir=lambda: Path("/pio"),
                )
        self.assertEqual(builds, [])
        flash.assert_not_called()



    def test_s3_app_build_failure_never_touches_rom_or_records_success(self) -> None:
        app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        candidate = hardware.FlashCandidate("usb", "ABC", "USB ABC", "s3")
        ble_candidate = hardware.FlashCandidate("ble", TEST_BLE_UUID, "Exact board", "s3")
        with (
            mock.patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=mock.Mock())}),
            mock.patch.object(hardware, "LockManager", return_value=FakeLockManager([])),
            mock.patch.object(hardware, "PioUsbInventory", return_value=mock.sentinel.inventory),
            mock.patch.object(hardware, "admit_s3_usb_parent", return_value=app),
            mock.patch.object(hardware, "touch_s3_application_bootloader") as touch,
            mock.patch.object(hardware, "detect_esptool_dialect", return_value=mock.sentinel.dialect),
            mock.patch.object(hardware, "_remember_flash") as remember,
            mock.patch.object(hardware, "_discover_flash_targets", return_value=((candidate,), (ble_candidate,))),
        ):
            with self.assertRaisesRegex(DevError, "build failed"):
                hardware.flash_selected_target(
                    workspace=FakeWorkspace(Path("/repo")),
                    requested_target="s3",
                    usb_selector="ABC",
                    ble_selector=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    build_target=lambda _: (_ for _ in ()).throw(DevError("build failed")),
                    platformio_core_dir=lambda: Path("/pio"),
                )
        touch.assert_not_called()
        remember.assert_not_called()

    def test_dual_selectors_rejected_before_all_actions(self) -> None:
        for ble_selector, options in (
            (TEST_BLE_UUID, {}), ("short", {}),
            (TEST_BLE_UUID, {"factory_provision": True}),
        ):
            with self.subTest(ble_selector=ble_selector, options=options):
                builds = mock.Mock()
                runner = FakeRunner()
                with (
                    mock.patch.object(hardware, "_discover_flash_targets") as discovery,
                    mock.patch.object(hardware, "LockManager") as locks,
                    mock.patch.object(hardware, "flash_exact_device") as flash,
                    mock.patch.object(hardware, "_flash_ble_ota") as ble,
                    self.assertRaisesRegex(DevError, "mutually exclusive.*choose one transport"),
                ):
                    hardware.flash_selected_target(
                        workspace=FakeWorkspace(Path("/repo")), requested_target="s3",
                        usb_selector="ABC", ble_selector=ble_selector,
                        runner=runner, lock_timeout=0, build_target=builds,
                        platformio_core_dir=lambda: Path("/pio"), **options,
                    )
                for action in (discovery, locks, flash, ble, builds):
                    action.assert_not_called()
                self.assertEqual(runner.calls, [])

class FlashSelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.usb = hardware.FlashCandidate("usb", "ABC12345", "USB ABC12345")
        self.other_usb = hardware.FlashCandidate("usb", "ABC67890", "USB ABC67890")
        self.ble = hardware.FlashCandidate("ble", "BLE-BOARD-123", "BLE Foxie", "s3")

    def resolve(self, **overrides):
        values = {
            "requested_target": None,
            "usb_selector": None,
            "ble_selector": None,
            "usb_candidates": (),
            "ble_candidates": (),
            "remembered": None,
        }
        values.update(overrides)
        return hardware.resolve_flash_target(**values)

    def test_port_path_selects_one_of_two_boards_and_preserves_serial_identity(self) -> None:
        candidates = hardware._usb_flash_candidates((
            parent("ABC", "1-1", LITCH_APP_PID, "/dev/cu.active"),
            parent("XYZ", "1-2", ESP32S3_ROM_PID, "/dev/cu.new"),
        ))
        selected = self.resolve(
            requested_target="s3", usb_selector="/dev/cu.new",
            usb_candidates=candidates,
        )
        self.assertEqual(selected, hardware.FlashTarget("s3", "usb", "xyz"))
        for stale in ("/dev/cu.ne", "/dev/cu.missing", "/DEV/CU.NEW"):
            with self.subTest(selector=stale):
                with self.assertRaisesRegex(DevError, "no USB") as rejected:
                    self.resolve(requested_target="s3", usb_selector=stale,
                                 usb_candidates=candidates, remembered=selected)
                self.assertIn("rerun ./litch devices", str(rejected.exception))


    def test_port_path_factory_provision_dispatches_only_selected_board(self) -> None:
        candidates = hardware._usb_flash_candidates((
            parent("ABC", "1-1", LITCH_APP_PID, "/dev/cu.active"),
            parent("XYZ", "1-2", ESP32S3_ROM_PID, "/dev/cu.new"),
        ))
        with (
            mock.patch.object(hardware, "detect_esptool_dialect"),
            mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
            mock.patch.object(hardware, "_discover_flash_targets", return_value=(candidates, ())) as discovery,
            mock.patch.object(hardware, "flash_exact_device") as flash,
            mock.patch.object(hardware, "_remember_flash"),
        ):
            hardware.flash_selected_target(
                workspace=FakeWorkspace(Path("/repo")), requested_target="s3",
                usb_selector="/dev/cu.new", ble_selector=None,
                runner=FakeRunner(), lock_timeout=0,
                build_target=lambda _: None, platformio_core_dir=lambda: Path("/pio"),
                factory_provision=True,
            )
        flash.assert_called_once()
        self.assertEqual(flash.call_args.kwargs["usb_identity"], "xyz")
        self.assertTrue(flash.call_args.kwargs["factory_provision"])
        self.assertTrue(discovery.call_args.kwargs["include_usb"])
        self.assertFalse(discovery.call_args.kwargs["include_ble"])
        with self.assertRaisesRegex(DevError, "ambiguous"):
            self.resolve(requested_target="s3", usb_selector="/dev/cu.new",
                         usb_candidates=(candidates[1], candidates[1]))


    def test_port_path_monitor_selects_exact_board_without_ble(self) -> None:
        candidates = hardware._usb_flash_candidates((
            parent("ABC", "1-1", LITCH_APP_PID, "/dev/cu.active"),
            parent("XYZ", "1-2", LITCH_APP_PID, "/dev/cu.other"),
        ))
        with (
            mock.patch.object(hardware, "_discover_usb_flash_candidates", return_value=candidates),
            mock.patch.object(hardware, "_discover_ble_candidates", side_effect=AssertionError("USB monitor must not scan BLE")) as ble,
            mock.patch.object(hardware, "monitor_exact_device") as monitor,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            hardware.monitor_selected_target(
                workspace=FakeWorkspace(Path("/repo")), requested_target="s3",
                usb_selector="/dev/cu.other", runner=FakeRunner(),
                lock_timeout=0, interactive=False,
            )
        monitor.assert_called_once()
        self.assertEqual(monitor.call_args.kwargs["usb_identity"], "xyz")
        ble.assert_not_called()

    def test_unique_short_usb_selector_and_c3_target_select_usb_only(self) -> None:
        selected = self.resolve(
            requested_target="c3",
            usb_selector="123",
            usb_candidates=(self.usb,),
        )
        self.assertEqual(selected, hardware.FlashTarget("c3", "usb", "ABC12345"))

    def test_esp32_mac_alias_selects_and_recalls_the_same_usb_board(self) -> None:
        usb = hardware.FlashCandidate(
            "usb",
            "00005e0053ab",
            "USB 00005e0053ab",
            "s3",
        )
        selected = self.resolve(
            requested_target="s3",
            usb_selector="00:00:5e:00:53:ab",
            usb_candidates=(usb,),
        )
        recalled = self.resolve(
            usb_candidates=(usb,),
            remembered=hardware.FlashTarget(
                "s3",
                "usb",
                "00:00:5e:00:53:ab",
            ),
        )

        self.assertEqual(
            selected,
            hardware.FlashTarget("s3", "usb", "00005e0053ab"),
        )
        self.assertEqual(recalled, selected)

    def test_explicit_s3_usb_never_falls_back_to_ble(self) -> None:
        selected = self.resolve(
            requested_target="s3",
            usb_selector="123",
            usb_candidates=(self.usb,),
            ble_candidates=(self.ble,),
        )
        self.assertEqual(selected, hardware.FlashTarget("s3", "usb", "ABC12345"))

    def test_ambiguous_short_selector_lists_matching_candidates(self) -> None:
        with self.assertRaisesRegex(DevError, "ambiguous: USB ABC12345; USB ABC67890"):
            self.resolve(
                requested_target="c3",
                usb_selector="ABC",
                usb_candidates=(self.usb, self.other_usb),
            )

    def test_blank_selector_is_rejected_before_it_can_match_every_board(self) -> None:
        with self.assertRaisesRegex(DevError, "must be non-empty"):
            self.resolve(
                requested_target="c3",
                usb_selector=" ",
                usb_candidates=(self.usb, self.other_usb),
            )

    def test_ble_only_inferrs_s3_and_usb_without_target_is_an_error(self) -> None:
        selected = self.resolve(ble_selector="123", ble_candidates=(self.ble,))
        self.assertEqual(selected, hardware.FlashTarget("s3", "ble", ble_identity="BLE-BOARD-123"))
        with self.assertRaisesRegex(DevError, "not safely identifiable"):
            self.resolve(usb_selector="123", usb_candidates=(self.usb,))

    def test_c3_rejects_ble_and_s3_rejects_dual_transport_selectors(self) -> None:
        with self.assertRaisesRegex(DevError, "C3 does not support BLE OTA"):
            self.resolve(requested_target="c3", ble_selector="123", ble_candidates=(self.ble,))
        with self.assertRaisesRegex(DevError, "mutually exclusive"):
            self.resolve(
                requested_target="s3", usb_selector="123", ble_selector="123",
                usb_candidates=(self.usb,), ble_candidates=(self.ble,),
            )

    def test_remembered_c3_requires_an_explicit_target(self) -> None:
        remembered = hardware.FlashTarget("c3", "usb", "ABC12345")
        with self.assertRaises(DevError) as raised:
            self.resolve(
                usb_candidates=(self.usb,),
                ble_candidates=(self.ble,),
                remembered=remembered,
            )
        self.assertNotIn("c3", str(raised.exception).casefold())

        selected = self.resolve(
            requested_target="c3",
            usb_candidates=(self.usb,),
            remembered=remembered,
        )
        self.assertEqual(selected, remembered)

    def test_multiple_discovery_requires_an_actionable_explicit_choice(self) -> None:
        with self.assertRaisesRegex(DevError, "multiple Litch boards are available") as raised:
            self.resolve(usb_candidates=(self.usb,), ble_candidates=(self.ble,))
        self.assertIn("--device <USB serial or exact port path>", str(raised.exception))
        self.assertIn("--ble-device <unique BLE address>", str(raised.exception))

    def test_last_successful_state_is_bounded_private_and_ignores_malformed_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.json"
            selected = hardware.FlashTarget("s3", "ble", ble_identity="BLE-BOARD-123")
            hardware._remember_flash(path, selected)
            self.assertEqual(hardware._remembered_flash(path, "s3"), selected)
            self.assertTrue(path.is_file())
            path.write_text("x" * 5000, encoding="utf-8")
            self.assertIsNone(hardware._remembered_flash(path, "s3"))
            path.write_text(
                json.dumps(
                    {
                        "schema": local_state.SCHEMA,
                        "last_flash": {"unexpected": "shape"},
                    }
                ),
                encoding="utf-8",
            )
            self.assertIsNone(hardware._remembered_flash(path, None))

    def test_local_slots_retain_future_keys_and_rebuild_malformed_convenience_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "state.json"
            local_state.remember(path, "c3_firmware", {"transport": "usb"})
            local_state.remember(path, "ios_flutter", "ios-id")
            state = local_state.read(path)
            self.assertEqual(state["c3_firmware"], {"transport": "usb"})
            self.assertEqual(state["ios_flutter"], "ios-id")
            self.assertIsNone(state["s3_firmware"])
            payload = json.loads(path.read_text(encoding="utf-8"))
            payload["future_slot"] = "keep schema forward-compatible"
            path.write_text(json.dumps(payload), encoding="utf-8")
            self.assertEqual(local_state.read(path)["ios_flutter"], "ios-id")
            path.write_text(
                json.dumps({"schema": "unknown", "c3_firmware": {"transport": "usb"}}),
                encoding="utf-8",
            )
            local_state.remember(path, "android_flutter", "android-id")
            self.assertEqual(local_state.read(path)["c3_firmware"], {"transport": "usb"})
            path.write_text("not json", encoding="utf-8")
            local_state.remember(path, "android_flutter", "android-id")
            self.assertEqual(local_state.read(path)["android_flutter"], "android-id")


class SerialProbeProcessTests(unittest.TestCase):
    def test_success_uses_strict_subprocess_protocol_and_finite_timeout(self) -> None:
        root = Path("/repo")
        runner = FakeRunner(
            [
                CompletedCommand(
                    ("python", "probe"),
                    0,
                    json.dumps(
                        {
                            "schema": "litch.serial-version.v1",
                            "version": "0.7",
                        }
                    ),
                    "",
                )
            ]
        )
        actual = hardware._run_serial_version_probe(
            FakeWorkspace(root),
            runner,
            port="/dev/cu.exact",
            expected_version="0.7",
        )
        self.assertEqual(actual, "0.7")
        call = runner.calls[0]
        self.assertEqual(call["cwd"], root)
        self.assertEqual(
            call["timeout_seconds"],
            hardware.SERIAL_PROBE_HARD_TIMEOUT_SECONDS,
        )
        self.assertTrue(call["capture"])
        self.assertFalse(call["check"])
        self.assertIn("/dev/cu.exact", call["argv"])

    def test_failure_retains_first_post_trial_incident_after_bounded_boot_log(
        self,
    ) -> None:
        incident = 'first_post_trial_incident={"rr":9,"prior_up":431}'
        runner = FakeRunner([
            CompletedCommand(
                ("python", "probe"), 1,
                "first boot " + "x" * 2400 + "; " + incident,
                "",
            )
        ])
        with self.assertRaises(hardware.RuntimeVerificationError) as failure:
            hardware._run_serial_version_probe(
                FakeWorkspace(Path("/repo")), runner,
                port="/dev/cu.exact", expected_version="0.7",
                expected_image_sha256="a" * 64,
            )
        self.assertIn(incident, str(failure.exception))
        self.assertLess(len(str(failure.exception)), 2100)


class BleOtaCompletionTests(unittest.TestCase):
    def test_ble_session_keeps_terminal_and_benchmark_methods(self) -> None:
        for method_name in (
            "wait_for_terminal_ota_status",
            "_log_transfer_profile",
            "run_downlink_benchmark",
        ):
            with self.subTest(method_name=method_name):
                self.assertIn(method_name, ble_update_firmware.LitchBleSession.__dict__)

    def test_non_update_session_caches_the_selected_board(self) -> None:
        candidate = ble_update_firmware.CandidateDevice(
            "Foxie_Litch", "BLE-UUID", -40, (), "service"
        )

        class Session:
            device = candidate

            async def fetch_fw_version(self):
                return "1.2.3"

        args = SimpleNamespace(include_name_match=False, device="UUID", scan_seconds=1)
        with (
            mock.patch.object(
                ble_update_firmware,
                "try_connect_cached_session",
                new=mock.AsyncMock(return_value=Session()),
            ),
            mock.patch.object(ble_update_firmware, "print_session_link_profile"),
            mock.patch.object(ble_update_firmware, "save_cached_candidate") as save,
        ):
            asyncio.run(ble_update_firmware.with_selected_session(args))
        save.assert_called_once_with(candidate)

    def test_terminal_done_update_does_not_update_the_ble_cache(self) -> None:
        candidate = ble_update_firmware.CandidateDevice(
            "Foxie_Litch", "BLE-UUID", -40, (), "service"
        )

        class Session:
            device = candidate

            async def upload_file(self, *_):
                return {
                    "terminal_state": "done",
                    "bytes": 10,
                    "seconds": 1.0,
                    "bytes_per_second": 10.0,
                    "mode": "reliable",
                    "chunk_size": 20,
                    "burst_limit": 1,
                }

            async def stop_notifications(self):
                return None

            class Client:
                async def disconnect(self):
                    return None

            _client = Client()

        args = SimpleNamespace(firmware_bin="firmware.bin", chunk_size="auto", mode="auto",
                               burst=1, begin_attempts=1, retry_delay_seconds=0.1,
                               pre_begin_delay_seconds=0.1, final_settle_seconds=0.1,
                               terminal_timeout_seconds=1)
        with (
            mock.patch.object(ble_update_firmware, "resolve_firmware_bin_path", return_value=Path("firmware.bin")),
            mock.patch.object(ble_update_firmware, "with_selected_session", return_value=Session()),
            mock.patch.object(ble_update_firmware, "save_cached_candidate") as save,
        ):
            self.assertEqual(asyncio.run(ble_update_firmware.run_update_command(args)), 0)
        save.assert_not_called()

    def test_non_done_terminal_never_updates_cached_device(self) -> None:
        candidate = ble_update_firmware.CandidateDevice(
            "Foxie_Litch", "BLE-UUID", -40, (), "service"
        )

        class Session:
            device = candidate

            async def upload_file(self, *_):
                return {"terminal_state": "disconnected"}

            async def stop_notifications(self):
                return None

            class Client:
                async def disconnect(self):
                    return None

            _client = Client()

        args = SimpleNamespace(firmware_bin="firmware.bin", chunk_size="auto", mode="auto",
                               burst=1, begin_attempts=1, retry_delay_seconds=0.1,
                               pre_begin_delay_seconds=0.1, final_settle_seconds=0.1,
                               terminal_timeout_seconds=1)
        with (
            mock.patch.object(ble_update_firmware, "resolve_firmware_bin_path", return_value=Path("firmware.bin")),
            mock.patch.object(ble_update_firmware, "with_selected_session", return_value=Session()),
            mock.patch.object(ble_update_firmware, "save_cached_candidate") as save,
        ):
            with self.assertRaisesRegex(RuntimeError, "terminal success"):
                asyncio.run(ble_update_firmware.run_update_command(args))
        save.assert_not_called()

    def test_every_sync_serial_phase_is_killed_at_process_deadline(self) -> None:
        phases = (
            "open",
            "set_control_lines",
            "reset_input_buffer",
            "write",
            "read",
            "close",
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workspace = FakeWorkspace(Path.cwd())
            for phase in phases:
                with self.subTest(phase=phase):
                    marker = root / f"{phase}.started"
                    command = (
                        sys.executable,
                        "-c",
                        _BLOCKING_SERIAL_CHILD,
                        phase,
                        str(marker),
                    )
                    output = io.StringIO()
                    started = time.monotonic()
                    with (
                        mock.patch.object(
                            hardware,
                            "serial_probe_argv",
                            return_value=command,
                        ),
                        mock.patch.object(
                            hardware,
                            "SERIAL_PROBE_HARD_TIMEOUT_SECONDS",
                            0.4,
                        ),
                    ):
                        with self.assertRaisesRegex(
                            RuntimeVerificationError,
                            "hard process boundary",
                        ):
                            hardware._run_serial_version_probe(
                                workspace,
                                Runner(output=output),
                                port="/dev/cu.fake",
                                expected_version="0.7",
                            )
                    self.assertTrue(marker.exists(), output.getvalue())
                    self.assertLess(time.monotonic() - started, 1.2)


class HighLevelHardwareTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.workspace = FakeWorkspace(Path(self.temporary.name))
        self.rom = parent("ABC", "1-2", ESP32S3_ROM_PID, "/dev/cu.rom")
        self.app = parent("ABC", "1-2", LITCH_APP_PID, "/dev/cu.app")
        self.s3_rom = parent(TEST_S3_MAC, "1-2", ESP32S3_ROM_PID, "/dev/cu.rom")
        self.s3_app = parent(TEST_S3_MAC, "1-2", LITCH_APP_PID, "/dev/cu.app")
        self.events: list[str] = []
        self.manager = FakeLockManager(self.events)
        serial_module = SimpleNamespace(Serial=mock.Mock())
        serial_import = mock.patch.dict(sys.modules, {"serial": serial_module})
        serial_import.start()
        self.addCleanup(serial_import.stop)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_custom_usb_build_directory_writes_new_image_not_stale_default(self) -> None:
        from tools.dev.tests.test_rom_flash import DirectRunner

        stale = self.workspace.root / ".pio" / "build" / "litch-esp32s3"
        custom = self.workspace.root / "private-preview" / "litch-esp32s3"
        for directory, payload in ((stale, b"STALE IMAGE"), (custom, b"PRIVATE IMAGE")):
            directory.mkdir(parents=True)
            (directory / "firmware.bin").write_bytes(payload)

        class CliRunner(DirectRunner):
            def run(self, argv, **kwargs):
                if argv[-1] == "version":
                    self.calls.append(tuple(argv))
                    return CompletedCommand(tuple(argv), 0, "esptool v4.9.0", "")
                if argv[-1] == "read_mac":
                    self.calls.append(tuple(argv))
                    return CompletedCommand(tuple(argv), 0, f"MAC: {TEST_S3_MAC}", "")
                return super().run(argv, timeout_seconds=kwargs["timeout_seconds"])

        for override in (str(custom.parent), "private-preview"):
            runner = CliRunner()
            with (
                self.subTest(override=override),
                mock.patch.dict(os.environ, {"PLATFORMIO_BUILD_DIR": override}),
                mock.patch.object(hardware, "LockManager", return_value=self.manager),
                mock.patch.object(hardware, "PioUsbInventory", return_value=FakeInventory(((self.s3_rom,),))),
                mock.patch.object(hardware, "admit_s3_usb_parent", return_value=self.s3_rom),
                mock.patch.object(hardware, "capture_exact_usb_parent", return_value=self.s3_rom),
                mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                hardware.flash_exact_device(
                    workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                    runner=runner, lock_timeout=0, build_target=lambda _: None,
                    platformio_core_dir=lambda: Path("/unused"),
                )
            self.assertEqual(bytes(runner.flash[0x10000:0x1000D]), b"PRIVATE IMAGE")

    def test_custom_ble_build_directory_uses_same_image_for_update_and_identity(self) -> None:
        custom = self.workspace.root / "private-preview" / "litch-esp32s3" / "firmware.bin"
        runner = mock.Mock()
        runner.run.return_value = CompletedCommand((), 0, "", "")
        with (
            mock.patch.dict(os.environ, {"PLATFORMIO_BUILD_DIR": "private-preview"}),
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "read_firmware_header_version", return_value="0.9.0-7"),
            mock.patch.object(hardware, "inspect_firmware_image", return_value=SimpleNamespace(image_sha256="a" * 64)) as inspect,
            mock.patch.object(hardware, "run_ble_runtime_bounded", return_value=SimpleNamespace(version="0.9.0-7", image_sha256="a" * 64)),
        ):
            hardware._flash_ble_ota(
                workspace=self.workspace,
                target=hardware.FlashTarget("s3", "ble", ble_identity="BLE-UUID"),
                runner=runner, lock_timeout=0, build_target=lambda _: None,
            )
        inspect.assert_called_once_with(custom)
        self.assertEqual(Path(runner.run.call_args.args[0][3]), custom)

    def test_interpolated_build_path_rejects_before_flash_discovery_or_build(self) -> None:
        with (
            mock.patch.dict(os.environ, {"PLATFORMIO_BUILD_DIR": "${PROJECT_DIR}/preview"}),
            mock.patch.object(hardware, "_discover_flash_targets") as discover,
            mock.patch.object(hardware, "PioUsbInventory") as inventory,
            mock.patch.object(hardware, "LockManager") as manager,
        ):
            build = mock.Mock()
            runner = mock.Mock()
            for entry in ("selected", "exact", "ble"):
                with self.subTest(entry=entry):
                    with self.assertRaisesRegex(DevError, "literal absolute or project-relative path"):
                        if entry == "selected":
                            hardware.flash_selected_target(
                                workspace=self.workspace, requested_target="s3", usb_selector="ABC",
                                ble_selector=None, runner=runner, lock_timeout=0, build_target=build,
                                platformio_core_dir=lambda: Path("/unused"),
                            )
                        elif entry == "exact":
                            hardware.flash_exact_device(
                                workspace=self.workspace, target="s3", usb_identity="ABC",
                                ble_identity=None, runner=runner, lock_timeout=0, build_target=build,
                                platformio_core_dir=lambda: Path("/unused"),
                            )
                        else:
                            hardware._flash_ble_ota(
                                workspace=self.workspace,
                                target=hardware.FlashTarget("s3", "ble", ble_identity="BLE-UUID"),
                                runner=runner, lock_timeout=0, build_target=build,
                            )
                    discover.assert_not_called()
                    inventory.assert_not_called()
                    manager.assert_not_called()
                    build.assert_not_called()
                    runner.assert_not_called()

    def test_s3_already_in_rom_flashes_without_host_serial_or_runtime_proof(self):
        build = self.workspace.root / ".pio" / "build" / "litch-esp32s3"
        build.mkdir(parents=True)
        (build / "firmware.bin").write_bytes(b"built application bytes")
        for exit_code in (0, 2):
            with self.subTest(exit_code=exit_code):
                runner = FakeRunner([
                    CompletedCommand((), 0, "esptool v5.3.1", ""),
                    CompletedCommand((), 0, f"MAC: {TEST_S3_MAC}", ""),
                    CompletedCommand((), exit_code, "", "USB disconnected" if exit_code else ""),
                ])
                with (
                    mock.patch.dict(sys.modules, {"serial": None}),
                    mock.patch.object(hardware, "LockManager", return_value=self.manager),
                    mock.patch.object(hardware, "PioUsbInventory", return_value=FakeInventory(((self.s3_rom,),))),
                    mock.patch.object(hardware, "admit_s3_usb_parent", return_value=self.s3_rom),
                    mock.patch.object(hardware, "capture_exact_usb_parent", return_value=self.s3_rom),
                    mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                    mock.patch.object(hardware, "_run_serial_version_probe", side_effect=AssertionError("no runtime probe")),
                    contextlib.redirect_stdout(output := io.StringIO()),
                ):
                    def execute():
                        hardware.flash_exact_device(
                            workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                            runner=runner, lock_timeout=0, build_target=lambda _: None,
                            platformio_core_dir=lambda: self.fail("direct flash needs no framework bootstrap image"),
                        )
                    if exit_code:
                        with self.assertRaisesRegex(DevError, "USB disconnected"):
                            execute()
                    else:
                        execute()
                self.assertEqual(len(runner.calls), 3)
                self.assertEqual("S3 USB flash complete" in output.getvalue(), exit_code == 0)

    def test_s3_writer_uses_watchdog_exit_for_every_usb_entry(self):
        from tools.dev.tests.test_rom_flash import DirectRunner, partition_table
        build = self.workspace.root / ".pio" / "build" / "litch-esp32s3"
        build.mkdir(parents=True)
        firmware = b"built application bytes"
        (build / "firmware.bin").write_bytes(firmware)
        (build / "bootloader.bin").write_bytes(b"boot")
        (build / "partitions.bin").write_bytes(partition_table())

        for major in (4, 5):
            default_reset = "default_reset" if major == 4 else "default-reset"
            no_reset = "no_reset" if major == 4 else "no-reset"
            read_mac = "read_mac" if major == 4 else "read-mac"
            for initial in (self.s3_app, self.s3_rom):
                for factory in (False, True):
                    with self.subTest(major=major, initial=initial.pid, factory=factory):
                        class CliRunner(DirectRunner):
                            # A blank chip can enumerate while macOS returns ENXIO
                            # on open. Only the read-only probe can wait for it.
                            rom_ready = initial.pid == LITCH_APP_PID
                            probe_count = 0

                            def run(self, argv, **kwargs):
                                if argv[-1] == "version":
                                    self.calls.append(tuple(argv))
                                    return CompletedCommand(tuple(argv), 0, f"esptool v{major}.9.0", "")
                                if argv[-1] == read_mac:
                                    self.calls.append(tuple(argv))
                                    self.probe_count += 1
                                    if not (
                                        kwargs["extra_env"].get("ESPTOOL_OPEN_PORT_ATTEMPTS") == "30"
                                        and kwargs["timeout_seconds"] == 30
                                        and argv[argv.index("--before") + 1] == default_reset
                                        and argv[argv.index("--after") + 1] == no_reset
                                        and argv[argv.index("--chip") + 1] == "esp32s3"
                                        and argv[argv.index("--port") + 1] == initial.port
                                        and "--no-stub" in argv
                                    ):
                                        return CompletedCommand(tuple(argv), 2, "", "Invalid ROM probe")
                                    self.rom_ready = True
                                    return CompletedCommand(tuple(argv), 0,
                                                            f"MAC: {TEST_S3_MAC}\nMAC: {TEST_S3_MAC.upper()}\n", "")
                                if "ESPTOOL_OPEN_PORT_ATTEMPTS" in kwargs["extra_env"]:
                                    return CompletedCommand(tuple(argv), 2, "", "Unexpected flash retry")
                                if not self.rom_ready:
                                    self.calls.append(tuple(argv))
                                    return CompletedCommand(tuple(argv), 2, "", "[Errno 6] Device not configured")
                                return super().run(argv, timeout_seconds=kwargs["timeout_seconds"])

                        runner = CliRunner(partition_table())
                        before = bytes(runner.flash)
                        with (
                            mock.patch.object(hardware, "LockManager", return_value=self.manager),
                            mock.patch.object(hardware, "PioUsbInventory", return_value=mock.sentinel.inventory),
                            mock.patch.object(hardware, "admit_s3_usb_parent", return_value=initial),
                            mock.patch.object(hardware, "capture_exact_usb_parent", return_value=self.s3_rom),
                            mock.patch.object(hardware, "wait_for_exact_usb_parent", return_value=self.s3_rom),
                            mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                            mock.patch.object(hardware, "enter_s3_rom_from_application", return_value=self.s3_rom) as entry,
                            mock.patch.object(hardware, "_run_serial_version_probe", side_effect=AssertionError("no runtime proof")),
                            contextlib.redirect_stdout(output := io.StringIO()),
                        ):
                            hardware.flash_exact_device(
                                workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                                runner=runner, lock_timeout=0, build_target=lambda _: None,
                                platformio_core_dir=lambda: Path("/unused"), factory_provision=factory,
                            )
                        # Exercise the shipping CLI, writer and actual segment
                        # bytes after either software or manual ROM entry.
                        # Native USB needs esptool's full-system reset in every
                        # final S3 write, independently of factory provisioning.
                        write = runner.calls[-1]
                        self.assertIn("--after", write)
                        self.assertEqual(write[write.index("--before") + 1], no_reset)
                        self.assertEqual(write[write.index("--after") + 1],
                                         "watchdog_reset" if major == 4 else "watchdog-reset")
                        self.assertNotIn("--no-stub", write)
                        self.assertEqual(runner.probe_count, int(initial.pid == ESP32S3_ROM_PID))
                        self.assertEqual(len(runner.calls), 2 + runner.probe_count + int(factory))
                        self.assertEqual(runner.flash[0x10000:0x10000 + len(firmware)], firmware)
                        first, second = ota_trial.parse_otadata(bytes(runner.flash[0xE000:0x10000]))
                        self.assertEqual((first.sequence, first.state, first.slot, first.crc_valid), (1, 2, 0, True))
                        self.assertFalse(second.selectable)
                        if factory:
                            self.assertEqual([offset for offset, _ in runner.segments], [0, 0x8000, 0x10000, 0xE000])
                            erase = runner.calls[-2]
                            self.assertEqual(erase[erase.index("--before") + 1], default_reset)
                            self.assertEqual(erase[erase.index("--after") + 1], no_reset)
                        else:
                            self.assertEqual([offset for offset, _ in runner.segments], [0x10000, 0xE000])
                            self.assertEqual(runner.flash[0x8000:0xE000], before[0x8000:0xE000])
                            self.assertEqual(runner.flash[0x210000:], before[0x210000:])
                        if initial.pid == LITCH_APP_PID:
                            entry.assert_called_once()
                        else:
                            entry.assert_not_called()

    def test_s3_rom_revalidation_waits_for_only_the_selected_board_before_factory_flash(self):
        from tools.dev.tests.test_rom_flash import partition_table

        build = self.workspace.root / ".pio" / "build" / "litch-esp32s3"
        build.mkdir(parents=True)
        (build / "firmware.bin").write_bytes(b"built application bytes")
        (build / "bootloader.bin").write_bytes(b"boot")
        (build / "partitions.bin").write_bytes(partition_table())
        verifier = parent("24:6f:28:ab:cd:00", "1-1", LITCH_APP_PID, "/dev/cu.verifier")
        returned = parent(TEST_S3_MAC, "1-2", ESP32S3_ROM_PID, "/dev/cu.returned")
        inventory = FakeInventory(((verifier, self.s3_rom),) * 3 + ((verifier,), (verifier, returned)))
        runner = FakeRunner([
            CompletedCommand((), 0, "esptool v5.3.1", ""),
            CompletedCommand((), 0, f"MAC: {TEST_S3_MAC}", ""),
        ])
        build_target = mock.Mock()
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "PioUsbInventory", return_value=inventory),
            mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
            contextlib.redirect_stdout(output := io.StringIO()),
        ):
            hardware.flash_exact_device(
                workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                runner=runner, lock_timeout=0, build_target=build_target,
                platformio_core_dir=lambda: Path("/unused"), factory_provision=True,
                expected_usb_identity=self.s3_rom.identity,
            )
        build_target.assert_called_once_with("s3")
        self.assertEqual(len(runner.calls), 4)
        version, probe, erase, write = (call["argv"] for call in runner.calls)
        self.assertEqual((version[-1], probe[-1], erase[-1]), ("version", "read-mac", "erase-flash"))
        self.assertIn("write-flash", write)
        for argv in (probe, erase, write):
            self.assertEqual(argv[argv.index("--port") + 1], returned.port)
        self.assertIn(f"USB serial {returned.identity.serial} at location {returned.identity.location}. "
                      "Leave this board connected.", output.getvalue())
        self.assertIn("S3 USB flash complete", output.getvalue())

    def test_s3_rom_revalidation_rejects_identity_drift_and_expired_wait_before_probe(self):
        replacement = parent("24:6f:28:ab:cd:00", "1-2", ESP32S3_ROM_PID, "/dev/cu.replacement")
        moved = parent(TEST_S3_MAC, "1-9", ESP32S3_ROM_PID, "/dev/cu.moved")
        cases = (
            ((replacement,), "USB serial changed", True),
            ((self.s3_rom, self.s3_app), "multiple USB parents", True),
            ((self.s3_app,), "unexpected 303a:4001", True),
            ((moved,), "timed out waiting for the exact USB parent", False),
        )
        wait_for_exact_parent = hardware.wait_for_exact_usb_parent
        for snapshot, expected, immediate in cases:
            with self.subTest(expected=expected):
                inventory = FakeInventory(((self.s3_rom,),) * 3 + (snapshot,))
                clock = FakeClock()
                runner = FakeRunner([CompletedCommand((), 0, "esptool v5.3.1", "")])

                def wait(inventory, **kwargs):
                    kwargs.update(monotonic=clock.monotonic, sleep=clock.sleep)
                    return wait_for_exact_parent(inventory, **kwargs)

                with (
                    mock.patch.object(hardware, "LockManager", return_value=self.manager),
                    mock.patch.object(hardware, "PioUsbInventory", return_value=inventory),
                    mock.patch.object(hardware, "wait_for_exact_usb_parent", side_effect=wait),
                    mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                    contextlib.redirect_stdout(io.StringIO()),
                    self.assertRaisesRegex(DevError, rf"Bootloader entry failed before erase or write.*{expected}"),
                ):
                    hardware.flash_exact_device(
                        workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                        runner=runner, lock_timeout=0, build_target=lambda _: None,
                        platformio_core_dir=lambda: Path("/unused"), factory_provision=True,
                        expected_usb_identity=self.s3_rom.identity,
                    )
                self.assertEqual([call["argv"][-1] for call in runner.calls], ["version"])
                self.assertAlmostEqual(clock.now, 0.1 if immediate else 5.1)

    def test_unconfirmed_s3_rom_probe_never_starts_factory_flash(self):
        moved = parent(TEST_S3_MAC, "2-1", ESP32S3_ROM_PID, "/dev/cu.moved")
        other_mac = "24:6f:28:ab:cd:00"
        cases = (
            (2, "", "[Errno 6] Device not configured", self.s3_rom, "Device not configured"),
            (2, "", "Wrong chip argument?", self.s3_rom, "Wrong chip argument"),
            (0, f"MAC: {other_mac}", "", self.s3_rom, "did not confirm"),
            (0, "Staying in bootloader.", "", self.s3_rom, "did not confirm"),
            (0, f"MAC: {TEST_S3_MAC}\nMAC: {other_mac}", "", self.s3_rom, "did not confirm"),
            (0, f"MAC: {TEST_S3_MAC}", "", moved, "serial and location"),
        )
        for status, stdout, stderr, final_parent, expected in cases:
            with self.subTest(expected=expected, stdout=stdout):
                runner = FakeRunner([
                    CompletedCommand((), 0, "esptool v5.3.1", ""),
                    CompletedCommand((), status, stdout, stderr),
                ])
                inventory = FakeInventory(((self.s3_rom,), (final_parent,)))
                with (
                    mock.patch.object(hardware, "LockManager", return_value=self.manager),
                    mock.patch.object(hardware, "PioUsbInventory", return_value=inventory),
                    mock.patch.object(hardware, "admit_s3_usb_parent", return_value=self.s3_rom),
                    mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                    contextlib.redirect_stdout(output := io.StringIO()),
                    self.assertRaisesRegex(DevError, expected),
                ):
                    hardware.flash_exact_device(
                        workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                        runner=runner, lock_timeout=0, build_target=lambda _: None,
                        platformio_core_dir=lambda: Path("/unused"), factory_provision=True,
                    )
                self.assertEqual([call["argv"][-1] for call in runner.calls], ["version", "read-mac"])
                self.assertNotIn("flash complete", output.getvalue())

    def test_entry_failure_and_interrupt_retry_exact_board_and_keep_factory_intent(self):
        hardware._remember_flash(hardware._local_state_path(self.workspace), hardware.FlashTarget("s3", "usb", "xyz"))
        for factory in (False, True):
            for interrupted in (False, True):
                with self.subTest(factory=factory, interrupted=interrupted):
                    failure = KeyboardInterrupt() if interrupted else DeviceInventoryError("console transport blocked")
                    with (
                        mock.patch.object(hardware, "LockManager", return_value=self.manager),
                        mock.patch.object(hardware, "PioUsbInventory", return_value=mock.sentinel.inventory),
                        mock.patch.object(hardware, "admit_s3_usb_parent", return_value=self.app),
                        mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                        mock.patch.object(hardware, "detect_esptool_dialect"),
                        mock.patch.object(hardware, "enter_s3_rom_from_application", side_effect=failure),
                        mock.patch.object(hardware, "_flash_s3_usb") as write,
                        contextlib.redirect_stdout(out := io.StringIO()),
                        contextlib.redirect_stderr(err := io.StringIO()),
                        self.assertRaises(KeyboardInterrupt if interrupted else DevError) as rejected,
                    ):
                        hardware.flash_exact_device(
                            workspace=self.workspace, target="s3", usb_identity="abc", ble_identity=None,
                            runner=FakeRunner(), lock_timeout=0, build_target=lambda _: None,
                            platformio_core_dir=lambda: Path("/pio"), factory_provision=factory,
                        )
                    text = out.getvalue() + err.getvalue() + str(rejected.exception)
                    self.assertIn("./litch flash s3 --device abc" + (" --factory" if factory else ""), text)
                    self.assertIn("location 1-2", text)
                    self.assertNotIn("flash complete", text)
                    write.assert_not_called()

    def test_final_capture_failure_and_interrupt_keep_exact_retry_and_factory_intent(self):
        for factory in (False, True):
            for interrupted in (False, True):
                with self.subTest(factory=factory, interrupted=interrupted):
                    failure = KeyboardInterrupt() if interrupted else DeviceInventoryError("board disconnected")
                    with (
                        mock.patch.object(hardware, "capture_exact_usb_parent", side_effect=failure),
                        mock.patch.object(hardware, "erase_s3_direct") as erase,
                        mock.patch.object(hardware, "flash_s3_direct") as write,
                        contextlib.redirect_stdout(out := io.StringIO()),
                        contextlib.redirect_stderr(err := io.StringIO()),
                        self.assertRaises(KeyboardInterrupt if interrupted else hardware.RomFlashError) as rejected,
                    ):
                        hardware._flash_s3_usb(self.workspace, mock.sentinel.inventory, rom=self.rom,
                                               bound=mock.sentinel.bound, esptool_argv=("esptool",),
                                               dialect=mock.sentinel.dialect, factory_provision=factory)
                    text = out.getvalue() + err.getvalue() + str(rejected.exception)
                    self.assertIn("./litch flash s3 --device abc" + (" --factory" if factory else ""), text)
                    self.assertIn("location 1-2", text)
                    self.assertNotIn("flash complete", text)
                    erase.assert_not_called()
                    write.assert_not_called()

    def test_factory_local_input_errors_never_erase(self):
        from tools.dev.tests.test_rom_flash import partition_table
        build = self.workspace.root / ".pio" / "build" / "litch-esp32s3"
        build.mkdir(parents=True)
        for fault in ("missing", "empty", "oversized"):
            with self.subTest(fault=fault):
                (build / "partitions.bin").write_bytes(partition_table())
                (build / "bootloader.bin").write_bytes(b"boot")
                (build / "firmware.bin").write_bytes(b"x" * (0x330001 if fault == "oversized" else 4))
                if fault == "missing":
                    (build / "bootloader.bin").unlink()
                elif fault == "empty":
                    (build / "firmware.bin").write_bytes(b"")
                with (
                    mock.patch.object(hardware, "capture_exact_usb_parent", return_value=self.rom),
                    mock.patch.object(hardware, "erase_s3_direct") as erase,
                    mock.patch.object(hardware, "flash_s3_direct") as write,
                    contextlib.redirect_stdout(io.StringIO()),
                    self.assertRaises(hardware.RomFlashError),
                ):
                    hardware._flash_s3_usb(self.workspace, mock.sentinel.inventory, rom=self.rom,
                                           bound=mock.sentinel.bound, esptool_argv=("esptool",),
                                           dialect=mock.sentinel.dialect, factory_provision=True)
                erase.assert_not_called()
                write.assert_not_called()

    def test_legacy_cli_writes_without_table_or_source_proof_and_keeps_saved_data(self):
        from tools.dev.tests.test_rom_flash import DirectRunner, partition_table, policy_firmware
        build = self.workspace.root / ".pio" / "build" / "litch-esp32s3"
        build.mkdir(parents=True)
        (build / "firmware.bin").write_bytes(policy_firmware(payload=b"NEW!"))
        (build / "bootloader.bin").write_bytes(b"boot")
        (build / "partitions.bin").write_bytes(partition_table())
        core = self.workspace.root / "pio-core"
        bootstrap = core / "packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
        bootstrap.parent.mkdir(parents=True)
        bootstrap.write_bytes(b"initial metadata")
        header = self.workspace.root / hardware.FIRMWARE_VERSION_HEADER
        header.parent.mkdir(parents=True)
        header.write_text('FIRMWARE_VERSION = "0.9";\n')

        class CliRunner(DirectRunner):
            def run(self, argv, **kwargs):
                if argv[-1] == "version":
                    self.calls.append(tuple(argv))
                    return CompletedCommand(tuple(argv), 0, "esptool v5.3.1", "")
                if argv[-1] == "read-mac":
                    self.calls.append(tuple(argv))
                    return CompletedCommand(tuple(argv), 0, f"MAC: {TEST_S3_MAC}", "")
                return super().run(argv, timeout_seconds=kwargs["timeout_seconds"])

        for recovery in (False, True):
            for active in (0, 1):
                with self.subTest(recovery=recovery, active=active):
                    runner = CliRunner(partition_table(legacy=True, recovery=recovery))
                    source = policy_firmware(1, b"OLD!")
                    for offset in (0x10000, 0x210000):
                        runner.flash[offset:offset + len(source)] = source
                    runner.flash[0xE000:0x10000] = (
                        ota_trial.make_selector_sector(1, 2) + ota_trial.make_selector_sector(2, 2 if active else 4))
                    before = bytes(runner.flash)
                    with (
                        mock.patch.object(hardware, "LockManager", return_value=self.manager),
                        mock.patch.object(hardware, "PioUsbInventory", return_value=mock.sentinel.inventory),
                        mock.patch.object(hardware, "admit_s3_usb_parent", return_value=self.s3_rom),
                        mock.patch.object(hardware, "capture_exact_usb_parent", return_value=self.s3_rom),
                        mock.patch.object(hardware, "wait_for_exact_usb_parent", return_value=self.s3_rom),
                        mock.patch.object(hardware, "_esptool_argv", return_value=("esptool",)),
                        mock.patch.object(hardware, "_run_serial_version_probe", side_effect=AssertionError("no runtime proof")),
                        contextlib.redirect_stdout(output := io.StringIO()),
                    ):
                        hardware.flash_exact_device(
                            workspace=self.workspace, target="s3", usb_identity=TEST_S3_MAC, ble_identity=None,
                            runner=runner, lock_timeout=0, build_target=lambda _: None,
                            platformio_core_dir=lambda: core,
                        )
                    self.assertEqual([call[-1] for call in runner.calls[:2]], ["version", "read-mac"])
                    self.assertEqual(len(runner.calls), 3)
                    self.assertEqual([offset for offset, _ in runner.segments], [0x10000, 0xE000])
                    self.assertEqual(runner.flash[0x8000:0xE000], before[0x8000:0xE000])
                    self.assertEqual(runner.flash[0x210000:], before[0x210000:])
                    self.assertIn("S3 USB flash complete", output.getvalue())






    def write_c3_build_artifacts(self, *, firmware_size: int = 64) -> Path:
        build = self.workspace.root / ".pio" / "build" / "litch-esp32c3"
        build.mkdir(parents=True, exist_ok=True)
        (build / "firmware.bin").write_bytes(b"\xe9" + bytes(firmware_size - 1))
        entries = b"".join(
            struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label, 0)
            for kind, subtype, offset, size, label in (
                (1, 2, 0x9000, 0x5000, b"nvs"),
                (1, 0, 0xE000, 0x2000, b"otadata"),
                (0, 0x10, 0x10000, 0x140000, b"app0"),
                (0, 0x11, 0x150000, 0x140000, b"app1"),
                (1, 0x82, 0x290000, 0x170000, b"spiffs"),
            )
        )
        table = entries + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entries).digest()
        (build / "partitions.bin").write_bytes(table + b"\xff" * (3072 - len(table)))
        return build

    def test_missing_pyserial_blocks_legacy_serial_workflow_before_device_access(self) -> None:
        def missing_serial_at_verification(*_args, **_kwargs):
            # Exercise the same runtime import that failed after the real C3
            # upload. sys.modules below guarantees no serial port can open.
            hardware.PySerialFactory().open(
                "/dev/cu.fake", baudrate=115200, read_timeout_seconds=0.1
            )

        for target in ("c3",):
            with self.subTest(target=target):
                self.events.clear()
                builds: list[str] = []
                runner = FakeRunner()
                inventory = FakeInventory(((self.rom,), (self.rom,)))
                with (
                    mock.patch.dict(sys.modules, {"serial": None}),
                    mock.patch.object(hardware, "LockManager", return_value=self.manager),
                    mock.patch.object(hardware, "PioUsbInventory", return_value=inventory) as discover,
                    mock.patch.object(hardware, "admit_s3_usb_parent", return_value=self.rom),
                    mock.patch.object(hardware, "capture_exact_usb_parent", return_value=self.rom),
                    mock.patch.object(hardware, "read_firmware_header_version", return_value="0.8"),
                    mock.patch.object(hardware, "_flash_s3_usb", return_value=mock.sentinel.context) as rom_flash,
                    mock.patch.object(hardware, "_verify_c3_runtime", side_effect=missing_serial_at_verification),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    with self.assertRaisesRegex(DevError, "pyserial"):
                        hardware.flash_exact_device(
                            workspace=self.workspace,
                            target=target,
                            usb_identity="ABC",
                            ble_identity=None,
                            runner=runner,
                            lock_timeout=0,
                            build_target=builds.append,
                            platformio_core_dir=lambda: Path("/pio"),
                        )
                    self.assertEqual(
                        (builds, len(runner.calls), rom_flash.call_count), ([], 0, 0),
                        "Missing verification dependency must prevent builds and both write paths",
                    )
                    discover.assert_not_called()
                    rom_flash.assert_not_called()
                    self.assertEqual(runner.calls, [])
                    self.assertEqual(self.events, [])

    def test_c3_post_upload_verification_failure_does_not_report_or_remember_success(self) -> None:
        self.write_c3_build_artifacts()
        runner = FakeRunner()
        builds: list[str] = []
        output = io.StringIO()
        candidate = hardware.FlashCandidate("usb", "ABC", "C3 fixture", "c3")
        with (
            mock.patch.object(hardware, "_discover_flash_targets", return_value=((candidate,), ())),
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "PioUsbInventory", return_value=FakeInventory(((self.rom,), (self.rom,)))),
            mock.patch.object(hardware, "read_firmware_header_version", return_value="0.8"),
            mock.patch.object(hardware, "_verify_c3_runtime", side_effect=RuntimeVerificationError("running version mismatch")),
            mock.patch.object(hardware, "_remember_flash") as remember,
            contextlib.redirect_stdout(output),
        ):
            with self.assertRaisesRegex(DevError, "running version mismatch"):
                hardware.flash_selected_target(
                    workspace=self.workspace,
                    requested_target="c3",
                    usb_selector="ABC",
                    ble_selector=None,
                    runner=runner,
                    lock_timeout=0,
                    build_target=builds.append,
                    platformio_core_dir=lambda: Path("/pio"),
                )
        self.assertEqual(builds, ["c3"])
        self.assertTrue(any("upload" in call["argv"] for call in runner.calls))
        remember.assert_not_called()
        self.assertNotIn("flash complete", output.getvalue())

    def test_c3_builds_then_uploads_only_to_twice_captured_exact_port(self) -> None:
        self.write_c3_build_artifacts()
        runner = FakeRunner()
        inventory = FakeInventory(((self.rom,), (self.rom,)))
        builds: list[str] = []
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "PioUsbInventory", return_value=inventory),
            mock.patch.object(
                hardware,
                "read_firmware_header_version",
                return_value="0.7",
            ),
            mock.patch.object(hardware, "_verify_c3_runtime") as verify,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            hardware.flash_exact_device(
                workspace=self.workspace,
                target="c3",
                usb_identity="ABC",
                ble_identity=None,
                runner=runner,
                lock_timeout=3.0,
                build_target=builds.append,
                platformio_core_dir=lambda: self.fail("C3 must not ask for PIO core"),
        )

        self.assertEqual(builds, ["c3"])
        verify.assert_called_once()
        self.assertEqual(
            runner.calls[-1]["argv"],
            (
                "pio",
                "run",
                "-e",
                "litch-esp32c3",
                "-t",
                "upload",
                "--upload-port",
                "/dev/cu.rom",
            ),
        )
        self.assertEqual(
            self.events,
            [
                "device:ABC:3.0",
                "enter:device",
                "lane:platformio:3.0",
                "enter:lane",
                "exit:lane",
                "exit:device",
            ],
        )

    def test_c3_upload_rejects_actual_oversized_binary_before_uploader(self) -> None:
        # The escaped image passed PlatformIO's ELF section size check, but
        # firmware.bin included .eh_frame and exceeded app0 by 1312 bytes.
        self.write_c3_build_artifacts(firmware_size=1_312_032)
        runner = FakeRunner()
        with (
            mock.patch.object(hardware, "read_firmware_header_version", return_value="0.7"),
            mock.patch.object(hardware, "_platformio_upload") as upload,
        ):
            with self.assertRaisesRegex(DevError, "1312032.*1310720"):
                hardware._upload_c3(
                    self.workspace, runner, FakeInventory(((self.rom,),)), serial="ABC",
                )
        upload.assert_not_called()
        self.assertEqual(runner.calls, [])

    def test_c3_upload_accepts_exact_fit_and_rejects_one_byte_over_app0(self) -> None:
        for size in (0x140000, 0x140001):
            with self.subTest(firmware_size=size):
                self.write_c3_build_artifacts(firmware_size=size)
                runner = FakeRunner()
                with (
                    mock.patch.object(hardware, "read_firmware_header_version", return_value="0.7"),
                    mock.patch.object(hardware, "_platformio_upload") as upload,
                ):
                    if size == 0x140000:
                        hardware._upload_c3(
                            self.workspace, runner, FakeInventory(((self.rom,),)), serial="ABC",
                        )
                        upload.assert_called_once_with(
                            self.workspace, runner, target="c3", port=self.rom.port,
                        )
                    else:
                        with self.assertRaisesRegex(DevError, "exceeds.*app0"):
                            hardware._upload_c3(
                                self.workspace, runner, FakeInventory(((self.rom,),)), serial="ABC",
                            )
                        upload.assert_not_called()

    def test_c3_upload_rejects_missing_artifacts_before_uploader(self) -> None:
        for name in ("firmware.bin", "partitions.bin"):
            with self.subTest(missing=name):
                build = self.write_c3_build_artifacts()
                (build / name).unlink()
                with (
                    mock.patch.object(hardware, "read_firmware_header_version", return_value="0.7"),
                    mock.patch.object(hardware, "_platformio_upload") as upload,
                ):
                    with self.assertRaisesRegex(DevError, name):
                        hardware._upload_c3(
                            self.workspace, FakeRunner(), FakeInventory(((self.rom,),)), serial="ABC",
                        )
                upload.assert_not_called()

    def test_s3_blank_usb_stops_before_build_or_device_access(self) -> None:
        builds: list[str] = []
        with (
            mock.patch.object(hardware, "LockManager") as locks,
            mock.patch.object(hardware, "PioUsbInventory") as inventory,
            mock.patch.object(hardware, "_flash_s3_usb") as flash,
            self.assertRaisesRegex(DevError, "exact USB serial"),
        ):
            hardware.flash_exact_device(
                workspace=self.workspace,
                target="s3",
                usb_identity="   ",
                ble_identity=None,
                runner=FakeRunner(),
                lock_timeout=0,
                build_target=builds.append,
                platformio_core_dir=lambda: Path("/pio"),
            )
        self.assertEqual(builds, [])
        locks.assert_not_called()
        inventory.assert_not_called()
        flash.assert_not_called()

    def test_ble_ota_keeps_update_and_acceptance_verification_under_locks(self) -> None:
        runner = mock.Mock()
        selected = hardware.FlashTarget("s3", "ble", ble_identity="BLE-UUID")
        verified = SimpleNamespace(version="1.2.3", image_sha256="a" * 64)

        def updater_run(*_args, **_kwargs):
            self.assertNotIn("exit:lane", self.events)
            self.events.append("updater")
            return CompletedCommand((), 0, "", "")

        def verify_runtime(**_kwargs):
            self.assertNotIn("exit:lane", self.events)
            self.events.append("verified")
            return verified

        runner.run.side_effect = updater_run
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "read_firmware_header_version", return_value="1.2.3"),
            mock.patch.object(
                hardware,
                "inspect_firmware_image",
                return_value=SimpleNamespace(image_sha256="a" * 64),
            ),
            mock.patch.object(
                hardware, "run_ble_runtime_bounded", side_effect=verify_runtime
            ) as runtime_verify,
        ):
            hardware._flash_ble_ota(
                workspace=self.workspace,
                target=selected,
                runner=runner,
                lock_timeout=2.0,
                build_target=lambda target: self.events.append(f"build:{target}"),
            )
        self.assertIn("build:s3", self.events)
        self.assertLess(self.events.index("updater"), self.events.index("exit:lane"))
        self.assertLess(self.events.index("verified"), self.events.index("exit:lane"))
        command = tuple(str(value) for value in runner.run.call_args.args[0])
        self.assertEqual(command[1].endswith("tools/ble_update_firmware.py"), True)
        self.assertEqual(command[2:4], ("update", str(self.workspace.root / ".pio" / "build" / "litch-esp32s3" / "firmware.bin")))
        self.assertEqual(command[-2:], ("--device", "BLE-UUID"))
        self.assertEqual(runtime_verify.call_args.kwargs["ble_selector"], "BLE-UUID")
        self.assertEqual(runtime_verify.call_args.kwargs["expected_version"], "1.2.3")
        self.assertEqual(runtime_verify.call_args.kwargs["expected_image_sha256"], "a" * 64)
        self.assertTrue(runtime_verify.call_args.kwargs["require_ota_accepted"])

    def test_uncertain_end_checks_exact_board_without_claiming_success(self) -> None:
        runner = FakeRunner([CompletedCommand((), ble_update_firmware.UPDATE_END_UNCERTAIN_EXIT_CODE, "", "")])
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "read_firmware_header_version", return_value="1.2.3"),
            mock.patch.object(hardware, "inspect_firmware_image", return_value=SimpleNamespace(image_sha256="a" * 64)),
            mock.patch.object(hardware, "run_ble_runtime_bounded", return_value=SimpleNamespace(version="1.2.3", image_sha256="a" * 64)) as verify,
            self.assertRaisesRegex(DevError, "does not prove that this update ran"),
        ):
            hardware._flash_ble_ota(
                workspace=self.workspace,
                target=hardware.FlashTarget("s3", "ble", ble_identity="BLE-UUID"),
                runner=runner, lock_timeout=0, build_target=lambda _: None,
            )
        self.assertEqual(verify.call_args.kwargs["ble_selector"], "BLE-UUID")
        self.assertEqual(len(runner.calls), 1)

    def test_ble_ota_requires_runtime_acceptance_after_matching_identity(self) -> None:
        from tools.dev.tests.test_runtime_verify import (
            EXPECTED_DIGEST,
            EXPECTED_VERSION,
            FakeBleFactory,
            FakeBleSession,
            digest_messages,
            message,
        )

        selected = hardware.FlashTarget("s3", "ble", ble_identity="BLE-UUID")
        for name, acceptance, accepted in (
            ("absent", (), False),
            ("pending", ({"ov": "p"},), False),
            ("valid", ({"ov": "v"},), True),
            ("pending then valid", ({"ov": "p"}, {"ov": "v"}), True),
        ):
            with self.subTest(acceptance=name):
                session = FakeBleSession((
                    message({"v": EXPECTED_VERSION}),
                    *digest_messages(),
                    *(message(reply) for reply in acceptance),
                ))
                factory = FakeBleFactory(session)
                output = io.StringIO()
                runner = FakeRunner()
                with (
                    mock.patch.object(hardware, "LockManager", return_value=self.manager),
                    mock.patch.object(hardware, "read_firmware_header_version", return_value=EXPECTED_VERSION),
                    mock.patch.object(
                        hardware, "inspect_firmware_image",
                        return_value=SimpleNamespace(image_sha256=EXPECTED_DIGEST),
                    ),
                    mock.patch.object(hardware, "BleakControlFactory", return_value=factory),
                    contextlib.redirect_stdout(output),
                ):
                    # Exercise the real bounded BLE verifier and its fresh
                    # version/digest/acceptance requests through the updater.
                    def update():
                        hardware._flash_ble_ota(
                            workspace=self.workspace, target=selected, runner=runner,
                            lock_timeout=2.0, build_target=lambda _target: None,
                        )

                    if accepted:
                        update()
                    else:
                        with self.assertRaisesRegex(DevError, "OTA runtime acceptance"):
                            update()

                self.assertEqual("S3 BLE OTA verified:" in output.getvalue(), accepted)
                self.assertEqual(factory.calls, [("BLE-UUID", 4.0)])
                self.assertEqual(session.sent[:2], [
                    {"cmd": "fw_version"}, {"cmd": "fw_digest"},
                ])
                self.assertIn({"cmd": "ota_state"}, session.sent)
                self.assertTrue(session.closed)
                self.assertEqual(len(runner.calls), 1)

    def test_selected_flash_saves_state_only_after_transport_success(self) -> None:
        selected = hardware.FlashCandidate("ble", "BLE-UUID", "BLE Foxie", "s3")
        with (
            mock.patch.object(
                hardware,
                "_discover_flash_targets",
                return_value=((), (selected,)),
            ),
            mock.patch.object(hardware, "_flash_ble_ota") as flash,
        ):
            hardware.flash_selected_target(
                workspace=self.workspace,
                requested_target=None,
                usb_selector=None,
                ble_selector="BLE-UUID",
                runner=FakeRunner(),
                lock_timeout=0,
                build_target=lambda _: None,
                platformio_core_dir=lambda: Path("/pio"),
            )
        flash.assert_called_once()
        state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        self.assertEqual(
            hardware._remembered_flash(state_path, "s3"),
            hardware.FlashTarget("s3", "ble", ble_identity="BLE-UUID"),
        )

        state_path.unlink()
        with (
            mock.patch.object(
                hardware,
                "_discover_flash_targets",
                return_value=((), (selected,)),
            ),
            mock.patch.object(hardware, "_flash_ble_ota", side_effect=DevError("OTA failed")),
        ):
            with self.assertRaisesRegex(DevError, "OTA failed"):
                hardware.flash_selected_target(
                    workspace=self.workspace,
                    requested_target=None,
                    usb_selector=None,
                    ble_selector="BLE-UUID",
                    runner=FakeRunner(),
                    lock_timeout=0,
                    build_target=lambda _: None,
                    platformio_core_dir=lambda: Path("/pio"),
                )
        self.assertFalse(state_path.exists())

    def test_factory_provision_rejects_c3_and_ble_transports(self) -> None:
        cases = (
            (
                "c3",
                "ABC",
                None,
                ((hardware.FlashCandidate("usb", "ABC", "C3", "c3"),), ()),
                "factory-provision.*only valid for S3 USB",
            ),
            (
                "s3",
                None,
                "BLE-UUID",
                ((), (hardware.FlashCandidate("ble", "BLE-UUID", "S3", "s3"),)),
                "factory-provision.*only valid for S3 USB",
            ),
        )
        for target, usb_selector, ble_selector, candidates, expected in cases:
            with self.subTest(target=target, ble_selector=ble_selector), mock.patch.object(
                hardware,
                "_discover_flash_targets",
                return_value=candidates,
            ):
                with self.assertRaisesRegex(
                    DevError,
                    expected,
                ):
                    hardware.flash_selected_target(
                        workspace=self.workspace,
                        requested_target=target,
                        usb_selector=usb_selector,
                        ble_selector=ble_selector,
                        runner=FakeRunner(),
                        lock_timeout=0,
                        build_target=lambda _: self.fail("must reject before build"),
                        platformio_core_dir=lambda: Path("/pio"),
                        factory_provision=True,
                    )

    def test_factory_provision_success_hands_operator_to_hardware_test(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ABC", "S3", "s3")
        output = io.StringIO()
        with (
            mock.patch.object(
                hardware,
                "detect_esptool_dialect",
                return_value=SimpleNamespace(major=5, version=(5, 3, 1)),
            ),
            mock.patch.object(
                hardware,
                "_discover_flash_targets",
                return_value=((candidate,), ()),
            ),
            mock.patch.object(hardware, "flash_exact_device") as flash,
            contextlib.redirect_stdout(output),
        ):
            hardware.flash_selected_target(
                workspace=self.workspace,
                requested_target="s3",
                usb_selector="ABC",
                ble_selector=None,
                runner=FakeRunner(),
                lock_timeout=0,
                build_target=lambda _: None,
                platformio_core_dir=lambda: Path("/pio"),
                factory_provision=True,
            )
        self.assertTrue(flash.call_args.kwargs["factory_provision"])
        text = output.getvalue()
        self.assertIn("factory validation is now required", text)
        self.assertIn("V3 starts hardware test automatically", text)
        self.assertIn("hwtest status", text)
        self.assertIn("hwtest restart", text)

    def test_factory_without_device_picks_and_confirms_exact_usb_parent(self) -> None:
        for count, row in ((1, "1"), (2, "2")):
            with self.subTest(count=count):
                candidates = hardware._usb_flash_candidates(tuple(
                    parent(serial, f"1-{index}", ESP32S3_ROM_PID, f"/dev/cu.{serial}")
                    for index, serial in enumerate(("ABC", "XYZ")[:count], 1)
                ))
                selected = candidates[int(row) - 1]
                hardware._remember_flash(hardware._local_state_path(self.workspace),
                                         hardware.FlashTarget("s3", "ble", ble_identity=TEST_BLE_UUID))
                with (
                    mock.patch.object(hardware, "detect_esptool_dialect", return_value=SimpleNamespace(major=5, version=(5, 3, 1))),
                    mock.patch.object(hardware, "_discover_flash_targets", return_value=(candidates, ())) as discover,
                    mock.patch("builtins.input", side_effect=(row,)) as prompt,
                    mock.patch.object(hardware, "flash_exact_device") as flash,
                    contextlib.redirect_stdout(output := io.StringIO()),
                ):
                    hardware.flash_selected_target(
                        workspace=self.workspace, requested_target=None, usb_selector=None,
                        ble_selector=None, runner=FakeRunner(), lock_timeout=0,
                        build_target=lambda _: self.fail("mocked dispatch owns build"),
                        platformio_core_dir=lambda: Path("/pio"), interactive=True, factory_provision=True,
                    )
                flash.assert_called_once()
                prompt.assert_called_once()
                self.assertEqual(flash.call_args.kwargs["usb_identity"], selected.identity)
                self.assertEqual(flash.call_args.kwargs["expected_usb_identity"], selected.usb_parent.identity)
                self.assertTrue(discover.call_args.kwargs["include_usb"])
                self.assertFalse(discover.call_args.kwargs["include_ble"])
                self.assertIn("settings", output.getvalue())
                self.assertIn("saved games", output.getvalue())

    def test_factory_picker_cancellation_absence_and_ambiguity_never_dispatch(self) -> None:
        candidate = hardware._usb_flash_candidates((parent("ABC", "1-1", LITCH_APP_PID, "/dev/cu.ABC"),))[0]
        cases = (((), True, (), "No.*USB"),
                 ((candidate,), False, (), "--factory"),
                 ((candidate,), True, ("q",), "cancelled"),
                 ((candidate,), True, ("invalid", "q"), "cancelled"),
                 ((candidate,), True, (EOFError,), "cancelled"),
                 ((candidate, candidate), True, (), "unique|ambiguous"))
        for candidates, interactive, answers, reason in cases:
            with (
                self.subTest(reason=reason),
                mock.patch.object(hardware, "detect_esptool_dialect", return_value=SimpleNamespace(major=5, version=(5, 3, 1))),
                mock.patch.object(hardware, "_discover_flash_targets", return_value=(candidates, ())),
                mock.patch("builtins.input", side_effect=answers),
                mock.patch.object(hardware, "flash_exact_device") as flash,
                mock.patch.object(hardware, "_remember_flash") as remember,
                contextlib.redirect_stdout(io.StringIO()),
                self.assertRaisesRegex(DevError, reason),
            ):
                hardware.flash_selected_target(
                    workspace=self.workspace, requested_target=None, usb_selector=None,
                    ble_selector=None, runner=FakeRunner(), lock_timeout=0,
                    build_target=lambda _: self.fail("must not build"),
                    platformio_core_dir=lambda: Path("/pio"), interactive=interactive, factory_provision=True,
                )
            flash.assert_not_called()
            remember.assert_not_called()

    def test_factory_picker_preserves_discovery_failure_instead_of_claiming_absence(self) -> None:
        with (
            mock.patch.object(hardware, "detect_esptool_dialect", return_value=SimpleNamespace(major=5, version=(5, 3, 1))),
            mock.patch.object(hardware, "_discover_usb_flash_candidates", side_effect=DevError("USB serial/location unavailable; reconnect one S3 board")),
            mock.patch.object(hardware, "flash_exact_device") as flash,
            mock.patch("builtins.input") as prompt,
            contextlib.redirect_stdout(output := io.StringIO()),
            self.assertRaisesRegex(DevError, "USB serial/location unavailable"),
        ):
            hardware.flash_selected_target(
                workspace=self.workspace, requested_target=None, usb_selector=None, ble_selector=None,
                runner=FakeRunner(), lock_timeout=0, build_target=lambda _: self.fail("must not build"),
                platformio_core_dir=lambda: Path("/pio"), interactive=True, factory_provision=True,
            )
        flash.assert_not_called()
        prompt.assert_not_called()
        self.assertNotIn("No USB boards", output.getvalue())

    def test_bare_flash_ignores_remembered_legacy_c3_target(self) -> None:
        state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        hardware._remember_flash(state_path, hardware.FlashTarget("c3", "usb", "ABC"))
        candidate = hardware.FlashCandidate("usb", "ABC", "USB ABC", "c3")
        with (
            mock.patch.object(
                hardware,
                "_discover_flash_targets",
                side_effect=[((candidate,), ()), ((), ())],
            ) as discover,
            mock.patch.object(hardware, "flash_exact_device") as flash,
        ):
            with self.assertRaises(DevError) as raised:
                hardware.flash_selected_target(
                    workspace=self.workspace,
                    requested_target=None,
                    usb_selector=None,
                    ble_selector=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    build_target=lambda _: None,
                    platformio_core_dir=lambda: Path("/pio"),
                )
        self.assertNotIn("c3", str(raised.exception).casefold())
        self.assertEqual(discover.call_count, 1)
        self.assertFalse(discover.call_args.kwargs["include_ble"])
        flash.assert_not_called()

    def test_remembered_ble_flash_is_announced_before_work_and_completion_is_clear(self) -> None:
        state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        hardware._remember_flash(state_path, hardware.FlashTarget("s3", "ble", ble_identity=TEST_BLE_UUID))
        candidate = hardware.FlashCandidate("ble", TEST_BLE_UUID, "BLE board", "s3")
        output = io.StringIO()

        def flash(**_kwargs):
            self.assertIn(f"remembered S3 over BLE -> {TEST_BLE_UUID}", output.getvalue())
            self.assertIn("starting S3 firmware build", output.getvalue())
            self.assertNotIn("flash complete", output.getvalue())

        with (
            mock.patch.object(
                hardware,
                "_discover_flash_targets",
                return_value=((), (candidate,)),
            ),
            mock.patch.object(hardware, "_flash_ble_ota", side_effect=flash),
            contextlib.redirect_stdout(output),
        ):
            hardware.flash_selected_target(
                workspace=self.workspace,
                requested_target=None,
                usb_selector=None,
                ble_selector=None,
                runner=FakeRunner(),
                lock_timeout=0,
                build_target=lambda _: None,
                platformio_core_dir=lambda: Path("/pio"),
            )

        self.assertIn("override: ./litch flash s3 --ble-device <ble-address>", output.getvalue())
        self.assertIn(f"flash complete: S3 over BLE -> {TEST_BLE_UUID}", output.getvalue())

    def test_bare_monitor_ignores_remembered_c3_and_uses_s3_candidate(self) -> None:
        state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        hardware._remember_flash(state_path, hardware.FlashTarget("c3", "usb", "ABC"))
        candidate = hardware.FlashCandidate("usb", "XYZ", "USB XYZ", "s3")
        output = io.StringIO()

        def monitor(**kwargs):
            self.assertEqual(kwargs["target"], "s3")
            self.assertEqual(kwargs["usb_identity"], "XYZ")

        with (
            mock.patch.object(
                hardware,
                "_discover_usb_flash_candidates",
                return_value=(candidate,),
            ),
            mock.patch.object(hardware, "monitor_exact_device", side_effect=monitor),
            contextlib.redirect_stdout(output),
        ):
            hardware.monitor_selected_target(
                workspace=self.workspace,
                requested_target=None,
                usb_selector=None,
                runner=FakeRunner(),
                lock_timeout=0,
                interactive=False,
            )

        self.assertNotIn("c3", output.getvalue().casefold())
        self.assertIn("monitoring S3 over USB -> XYZ", output.getvalue())

    def test_bare_monitor_never_auto_selects_c3_candidate(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ABC", "USB ABC", "c3")
        output = io.StringIO()
        with (
            mock.patch.object(
                hardware,
                "_discover_usb_flash_candidates",
                return_value=(candidate,),
            ),
            mock.patch.object(hardware, "monitor_exact_device") as monitor,
            contextlib.redirect_stdout(output),
        ):
            with self.assertRaises(DevError) as raised:
                hardware.monitor_selected_target(
                    workspace=self.workspace,
                    requested_target=None,
                    usb_selector=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    interactive=False,
                )

        monitor.assert_not_called()
        self.assertNotIn("c3", str(raised.exception).casefold())

    def test_bare_monitor_hides_one_unknown_rom_target(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ABC", "USB ABC")
        with (
            mock.patch.object(
                hardware,
                "_discover_usb_flash_candidates",
                return_value=(candidate,),
            ),
            mock.patch("builtins.input") as prompt,
            mock.patch.object(hardware, "monitor_exact_device") as monitor,
        ):
            with self.assertRaises(DevError) as raised:
                hardware.monitor_selected_target(
                    workspace=self.workspace,
                    requested_target=None,
                    usb_selector=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    interactive=True,
                )

        prompt.assert_not_called()
        monitor.assert_not_called()
        self.assertNotIn("ABC", str(raised.exception))
        self.assertNotIn("c3", str(raised.exception).casefold())

    def test_interactive_explicit_s3_monitor_never_offers_or_selects_c3(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ABC", "USB ABC")
        output = io.StringIO()
        with (
            mock.patch.object(
                hardware,
                "_discover_usb_flash_candidates",
                return_value=(candidate,),
            ),
            mock.patch("builtins.input", return_value="1"),
            mock.patch.object(hardware, "monitor_exact_device") as monitor,
            contextlib.redirect_stdout(output),
        ):
            hardware.monitor_selected_target(
                workspace=self.workspace,
                requested_target="s3",
                usb_selector=None,
                runner=FakeRunner(),
                lock_timeout=0,
                interactive=True,
            )

        self.assertEqual(monitor.call_args.kwargs["target"], "s3")
        self.assertNotIn("./litch monitor c3", output.getvalue())
        self.assertIn("./litch monitor s3 --device ABC", output.getvalue())

    def test_noninteractive_explicit_c3_monitor_diagnostics_never_offer_s3(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ABC", "USB ABC")
        with mock.patch.object(
            hardware,
            "_discover_usb_flash_candidates",
            return_value=(candidate,),
        ):
            with self.assertRaises(DevError) as raised:
                hardware.monitor_selected_target(
                    workspace=self.workspace,
                    requested_target="c3",
                    usb_selector=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    interactive=False,
                )

        self.assertIn("./litch monitor c3 --device ABC", str(raised.exception))
        self.assertNotIn("./litch monitor s3", str(raised.exception))

    def test_interactive_monitor_prompts_for_genuine_usb_ambiguity(self) -> None:
        s3_a = hardware.FlashCandidate("usb", "ABC", "USB ABC", "s3")
        s3 = hardware.FlashCandidate("usb", "XYZ", "USB XYZ", "s3")
        output = io.StringIO()
        with (
            mock.patch.object(
                hardware,
                "_discover_usb_flash_candidates",
                return_value=(s3_a, s3),
            ),
            mock.patch("builtins.input", return_value="2"),
            mock.patch.object(hardware, "monitor_exact_device") as monitor,
            contextlib.redirect_stdout(output),
        ):
            hardware.monitor_selected_target(
                workspace=self.workspace,
                requested_target=None,
                usb_selector=None,
                runner=FakeRunner(),
                lock_timeout=0,
                interactive=True,
            )

        monitor.assert_called_once()
        self.assertEqual(monitor.call_args.kwargs["target"], "s3")
        self.assertEqual(monitor.call_args.kwargs["usb_identity"], "XYZ")
        self.assertNotIn("c3", output.getvalue().casefold())
        self.assertIn("./litch monitor s3 --device XYZ", output.getvalue())

    def test_usb_selection_routes_never_scan_or_consume_ble(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ABC", "USB board", "s3")
        other = hardware.FlashCandidate("usb", "XYZ", "Other USB board", "s3")
        for route in ("explicit", "bare", "interactive", "remembered", "old_mixed_preference", "ambiguous_prompt", "stale_usb_prompt"):
            with self.subTest(route=route):
                state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
                if state_path.exists():
                    state_path.unlink()
                if route in ("remembered", "old_mixed_preference", "stale_usb_prompt"):
                    remembered_identity = "missing" if route == "stale_usb_prompt" else "ABC"
                    hardware._remember_flash(state_path, hardware.FlashTarget(
                        "s3", "usb", remembered_identity,
                        TEST_BLE_UUID if route == "old_mixed_preference" else None,
                    ))
                candidates = (candidate, other) if route == "ambiguous_prompt" else (candidate,)
                with (
                    mock.patch.object(hardware, "_discover_usb_flash_candidates", return_value=candidates) as usb_discovery,
                    mock.patch.object(hardware, "_discover_ble_candidates", side_effect=AssertionError("Bluetooth off")) as ble_scan,
                    mock.patch.object(hardware, "_scan_ble", side_effect=AssertionError("no BLE script")) as ble_script,
                    mock.patch.object(hardware, "BleakControlFactory", side_effect=AssertionError("phone owns BLE")) as ble_factory,
                    mock.patch.object(hardware, "run_ble_runtime_bounded", side_effect=AssertionError("no BLE proof")) as ble_probe,
                    mock.patch.object(hardware, "_flash_ble_ota", side_effect=AssertionError("no BLE OTA")) as ble_ota,
                    mock.patch.object(hardware, "flash_exact_device") as flash,
                    mock.patch("builtins.input", side_effect=["1"]) as prompt,
                    contextlib.redirect_stdout(output := io.StringIO()),
                ):
                    hardware.flash_selected_target(
                        workspace=self.workspace, requested_target="s3" if route == "explicit" else None,
                        usb_selector="ABC" if route == "explicit" else None, ble_selector=None,
                        runner=FakeRunner(), lock_timeout=0, build_target=lambda _: None,
                        platformio_core_dir=lambda: Path("/pio"),
                        interactive=route in ("interactive", "ambiguous_prompt", "stale_usb_prompt"),
                    )
                usb_discovery.assert_called_once()
                flash.assert_called_once()
                self.assertEqual(flash.call_args.kwargs["usb_identity"], "ABC")
                self.assertIsNone(flash.call_args.kwargs["ble_identity"])
                for action in (ble_scan, ble_script, ble_factory, ble_probe, ble_ota):
                    action.assert_not_called()
                self.assertEqual(prompt.call_count, 1 if route in ("ambiguous_prompt", "stale_usb_prompt") else 0)
                self.assertNotIn("--ble-device", output.getvalue())
                self.assertIn("flash complete: S3 over USB -> ABC", output.getvalue())
                self.assertIsNone(hardware._remembered_flash(state_path, "s3").ble_identity)

    def test_usb_recovery_diagnostics_never_suggest_or_scan_ble(self) -> None:
        cases = (
            ((), "./litch devices", False),
            ((), "./litch devices", True),
            ((hardware.FlashCandidate("usb", "ROM", "Unknown ROM"),), "for an S3 board in ROM mode", False),
            ((hardware.FlashCandidate("usb", "ROM", "Unknown ROM"),), "for an S3 board in ROM mode", True),
            ((hardware.FlashCandidate("usb", "ABC", "Board A", "s3"),
              hardware.FlashCandidate("usb", "XYZ", "Board B", "s3")), "./litch flash s3 --device ABC", False),
        )
        for candidates, expected_recovery, interactive in cases:
            with self.subTest(candidates=candidates):
                with (
                    mock.patch.object(hardware, "_discover_usb_flash_candidates", return_value=candidates) as usb,
                    mock.patch.object(hardware, "_discover_ble_candidates", side_effect=AssertionError("no BLE scan")) as ble,
                    mock.patch.object(hardware, "flash_exact_device") as flash,
                    mock.patch.object(hardware, "_flash_ble_ota") as ota,
                    self.assertRaises(DevError) as rejected,
                ):
                    hardware.flash_selected_target(
                        workspace=self.workspace, requested_target=None, usb_selector=None, ble_selector=None,
                        runner=FakeRunner(), lock_timeout=0, build_target=lambda _: None,
                        platformio_core_dir=lambda: Path("/pio"), interactive=interactive,
                    )
                self.assertIn(expected_recovery, str(rejected.exception))
                self.assertNotIn("BLE", str(rejected.exception))
                self.assertNotIn("--ble", str(rejected.exception))
                usb.assert_called_once()
                for action in (ble, flash, ota):
                    action.assert_not_called()

    def test_flash_choice_eof_reports_direct_transport_commands_before_actions(self) -> None:
        state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        for transport in ("usb", "ble"):
            with self.subTest(transport=transport):
                if state_path.exists():
                    state_path.unlink()
                candidates = tuple(hardware.FlashCandidate(transport, identity, identity, "s3")
                                   for identity in ("ABC", "XYZ"))
                if transport == "ble":
                    hardware._remember_flash(state_path, hardware.FlashTarget("s3", "ble", ble_identity="missing"))
                with (
                    mock.patch.object(hardware, "_discover_usb_flash_candidates", return_value=candidates if transport == "usb" else ()) as usb,
                    mock.patch.object(hardware, "_discover_ble_candidates", new=mock.AsyncMock(return_value=candidates if transport == "ble" else ())) as ble,
                    mock.patch("builtins.input", side_effect=EOFError) as prompt,
                    mock.patch.object(hardware, "flash_exact_device") as flash,
                    mock.patch.object(hardware, "_flash_ble_ota") as ota,
                    mock.patch.object(hardware, "_remember_flash") as remember,
                    contextlib.redirect_stdout(io.StringIO()),
                    self.assertRaisesRegex(DevError, "No board selected") as rejected,
                ):
                    hardware.flash_selected_target(
                        workspace=self.workspace, requested_target=None, usb_selector=None, ble_selector=None,
                        runner=FakeRunner(), lock_timeout=0, build_target=lambda _: None,
                        platformio_core_dir=lambda: Path("/pio"), interactive=True,
                    )
                selector = "--device" if transport == "usb" else "--ble-device"
                self.assertIn(f"./litch flash s3 {selector} ABC", str(rejected.exception))
                self.assertIn(f"./litch flash s3 {selector} XYZ", str(rejected.exception))
                self.assertEqual(rejected.exception.exit_code, 2)
                if transport == "usb":
                    usb.assert_called_once()
                    ble.assert_not_called()
                    self.assertNotIn("BLE", str(rejected.exception))
                    self.assertNotIn("--ble", str(rejected.exception))
                else:
                    usb.assert_not_called()
                    ble.assert_awaited_once()
                    self.assertNotIn("--device", str(rejected.exception))
                prompt.assert_called_once()
                for action in (flash, ota, remember):
                    action.assert_not_called()

    def test_usb_discovery_failure_or_unknown_model_never_falls_back_to_ble(self) -> None:
        for candidates in ((), (hardware.FlashCandidate("usb", "ROM", "Unknown ROM"),)):
            with self.subTest(candidates=candidates):
                with (
                    mock.patch.object(hardware, "_discover_usb_flash_candidates", return_value=candidates) as usb,
                    mock.patch.object(hardware, "_discover_ble_candidates", side_effect=AssertionError("no BLE fallback")) as ble,
                    mock.patch.object(hardware, "flash_exact_device") as flash,
                    mock.patch.object(hardware, "_flash_ble_ota") as ota,
                    self.assertRaisesRegex(DevError, "select one explicitly"),
                ):
                    hardware.flash_selected_target(
                        workspace=self.workspace, requested_target=None,
                        usb_selector=None, ble_selector=None, runner=FakeRunner(), lock_timeout=0,
                        build_target=lambda _: None, platformio_core_dir=lambda: Path("/pio"), interactive=True,
                    )
                usb.assert_called_once()
                for action in (ble, flash, ota):
                    action.assert_not_called()

    def test_interactive_explicit_s3_flash_never_offers_or_selects_c3(self) -> None:
        usb_a = hardware.FlashCandidate("usb", "USB-A", "USB board A")
        usb_b = hardware.FlashCandidate("usb", "USB-B", "USB board B")
        output = io.StringIO()
        with (
            mock.patch.object(
                hardware,
                "_discover_flash_targets",
                side_effect=[((usb_a, usb_b), ()), ((), ())],
            ),
            mock.patch("builtins.input", side_effect=("1",)) as prompt,
            mock.patch.object(hardware, "flash_exact_device") as flash,
            contextlib.redirect_stdout(output),
        ):
            hardware.flash_selected_target(
                workspace=self.workspace,
                requested_target="s3",
                usb_selector=None,
                ble_selector=None,
                runner=FakeRunner(),
                lock_timeout=0,
                build_target=lambda _: None,
                platformio_core_dir=lambda: Path("/pio"),
                interactive=True,
            )

        flash.assert_called_once()
        self.assertIsNone(flash.call_args.kwargs["ble_identity"])
        prompt.assert_called_once()
        self.assertNotIn("./litch flash c3", output.getvalue())
        self.assertIn(
            "./litch flash s3 --device USB-A",
            output.getvalue(),
        )

    def test_noninteractive_explicit_s3_flash_diagnostics_never_offer_c3(self) -> None:
        usb_a = hardware.FlashCandidate("usb", "USB-A", "USB board A")
        usb_b = hardware.FlashCandidate("usb", "USB-B", "USB board B")
        with mock.patch.object(
            hardware,
            "_discover_flash_targets",
            side_effect=[((usb_a, usb_b), ()), ((), ())],
        ):
            with self.assertRaises(DevError) as raised:
                hardware.flash_selected_target(
                    workspace=self.workspace,
                    requested_target="s3",
                    usb_selector=None,
                    ble_selector=None,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    build_target=lambda _: None,
                    platformio_core_dir=lambda: Path("/pio"),
                    interactive=False,
                )

        self.assertNotIn("./litch flash c3", str(raised.exception))
        self.assertIn(
            "./litch flash s3 --device USB-A",
            str(raised.exception),
        )

    def test_unknown_rom_usb_requires_explicit_target_before_selection(self) -> None:
        candidate = hardware.FlashCandidate("usb", "ROM", "USB ROM")

        implicit_flash = hardware._explicit_flash_choices((candidate,), ())
        implicit_monitor = hardware._monitor_choices((candidate,))
        self.assertEqual(hardware._flash_options((candidate,), ()), [])
        self.assertIn("for an S3 board in ROM mode", implicit_flash)
        self.assertIn("./litch flash s3 --device ROM", implicit_flash)
        self.assertNotIn("ROM", implicit_monitor)
        self.assertNotIn("c3", implicit_flash.casefold())
        self.assertNotIn("c3", implicit_monitor.casefold())

        self.assertIn(
            "./litch flash s3 --device ROM",
            hardware._explicit_flash_choices((candidate,), (), "s3"),
        )
        self.assertIn(
            "./litch monitor s3 --device ROM",
            hardware._monitor_choices((candidate,), "s3"),
        )

    def test_stale_remembered_transport_never_scans_the_other_transport(self) -> None:
        state_path = self.workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        for transport in ("usb", "ble"):
            for interactive in (False, True):
                with self.subTest(transport=transport, interactive=interactive):
                    hardware._remember_flash(state_path, hardware.FlashTarget(
                        "s3", transport, "missing" if transport == "usb" else None,
                        TEST_BLE_UUID if transport == "ble" else None,
                    ))
                    with (
                        mock.patch.object(hardware, "_discover_flash_targets", return_value=((), ())) as discovery,
                        mock.patch.object(hardware, "flash_exact_device") as flash,
                        mock.patch.object(hardware, "_flash_ble_ota") as ota,
                        self.assertRaisesRegex(DevError, "select one explicitly") as rejected,
                    ):
                        hardware.flash_selected_target(
                            workspace=self.workspace, requested_target=None,
                            usb_selector=None, ble_selector=None, runner=FakeRunner(), lock_timeout=0,
                            build_target=lambda _: None, platformio_core_dir=lambda: Path("/pio"),
                            interactive=interactive,
                        )
                    if transport == "usb":
                        self.assertIn("./litch devices'", str(rejected.exception))
                        self.assertNotIn("BLE", str(rejected.exception))
                        self.assertNotIn("--ble", str(rejected.exception))
                    else:
                        self.assertIn("./litch devices --ble", str(rejected.exception))
                        self.assertIn("--ble-device", str(rejected.exception))
                        self.assertNotIn("USB", str(rejected.exception))
                        self.assertNotIn("--device", str(rejected.exception))
                    discovery.assert_called_once()
                    self.assertEqual(discovery.call_args.kwargs["include_usb"], transport == "usb")
                    self.assertEqual(discovery.call_args.kwargs["include_ble"], transport == "ble")
                    flash.assert_not_called()
                    ota.assert_not_called()

    def test_implicit_ble_failure_keeps_valid_usb_discovery_available(self) -> None:
        usb = hardware.FlashCandidate("usb", "USB", "USB board", "s3")
        with (
            mock.patch.object(hardware, "_discover_usb_flash_candidates", return_value=(usb,)),
            mock.patch.object(hardware.asyncio, "run", side_effect=RuntimeError("Bluetooth off")),
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            discovered = hardware._discover_flash_targets(
                workspace=self.workspace,
                runner=FakeRunner(),
                lock_timeout=0,
                include_usb=True,
                include_ble=True,
                best_effort=True,
            )
        self.assertEqual(discovered, ((usb,), ()))
        self.assertIn("BLE discovery unavailable", output.getvalue())

    def test_explicit_ble_discovery_failure_still_fails_closed(self) -> None:
        with mock.patch.object(hardware.asyncio, "run", side_effect=RuntimeError("Bluetooth off")):
            with self.assertRaisesRegex(DevError, "BLE discovery failed"):
                hardware._discover_flash_targets(
                    workspace=self.workspace,
                    runner=FakeRunner(),
                    lock_timeout=0,
                    include_usb=False,
                    include_ble=True,
                )

    def test_invalid_c3_ble_request_does_not_start_discovery(self) -> None:
        with mock.patch.object(hardware, "_discover_flash_targets") as discover:
            with self.assertRaisesRegex(DevError, "C3 does not support BLE OTA"):
                hardware.flash_selected_target(
                    workspace=self.workspace,
                    requested_target="c3",
                    usb_selector=None,
                    ble_selector="BLE",
                    runner=FakeRunner(),
                    lock_timeout=0,
                    build_target=lambda _: None,
                    platformio_core_dir=lambda: Path("/pio"),
                )
        discover.assert_not_called()

    def test_blank_usb_selector_fails_before_locks_or_build(self) -> None:
        builds: list[str] = []
        with self.assertRaisesRegex(DevError, "exact USB serial"):
            hardware.flash_exact_device(
                workspace=self.workspace,
                target="c3",
                usb_identity="   ",
                ble_identity=None,
                runner=FakeRunner(),
                lock_timeout=0,
                build_target=builds.append,
                platformio_core_dir=lambda: Path("/pio"),
            )
        self.assertEqual(builds, [])












    def test_c3_runtime_verifies_returned_exact_parent_and_version(self) -> None:
        context = hardware._C3UploadContext(
            FakeInventory(((self.rom,),)),
            self.rom.identity,
            "0.7",
        )
        with (
            mock.patch.object(
                hardware,
                "wait_for_exact_usb_parent",
                return_value=self.rom,
            ) as wait_for_parent,
            mock.patch.object(
                hardware,
                "_run_serial_version_probe",
                return_value="0.7",
            ) as verify_version,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            hardware._verify_c3_runtime(
                context,
                workspace=self.workspace,
                runner=mock.sentinel.runner,
            )
        self.assertEqual(wait_for_parent.call_args.kwargs["identity"], self.rom.identity)
        self.assertEqual(wait_for_parent.call_args.kwargs["pid"], ESP32S3_ROM_PID)
        self.assertEqual(verify_version.call_args.args, (self.workspace, mock.sentinel.runner))
        self.assertEqual(verify_version.call_args.kwargs["port"], "/dev/cu.rom")
        self.assertEqual(verify_version.call_args.kwargs["expected_version"], "0.7")
        self.assertFalse(verify_version.call_args.kwargs["assert_dtr"])


    def test_direct_usb_adapter_rejects_ble_before_build_or_device_access(self) -> None:
        builds = mock.Mock()
        with (
            mock.patch.object(hardware, "LockManager") as locks,
            mock.patch.object(hardware, "PioUsbInventory") as inventory,
            mock.patch.object(hardware, "_flash_s3_usb") as flash,
            self.assertRaisesRegex(DevError, "mutually exclusive"),
        ):
            hardware.flash_exact_device(
                workspace=self.workspace, target="s3", usb_identity="ABC", ble_identity=TEST_BLE_UUID,
                runner=FakeRunner(), lock_timeout=0, build_target=builds,
                platformio_core_dir=lambda: Path("/pio"),
            )
        for action in (locks, inventory, flash, builds):
            action.assert_not_called()

    def test_s3_monitor_requires_application_pid_and_uses_exact_port(self) -> None:
        runner = FakeRunner()
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(
                hardware,
                "PioUsbInventory",
                return_value=FakeInventory(((self.app,),)),
            ),
        ):
            hardware.monitor_exact_device(
                workspace=self.workspace,
                target="s3",
                usb_identity="ABC",
                runner=runner,
                lock_timeout=2.0,
            )
        self.assertEqual(
            runner.calls[-1]["argv"],
            (
                "pio",
                "device",
                "monitor",
                "--port",
                "/dev/cu.app",
                "--baud",
                "115200",
                "--dtr",
                "1",
                "--rts",
                "1",
                "--filter",
                "direct",
                "--no-reconnect",
            ),
        )
        self.assertTrue(runner.calls[-1]["interactive"])
        self.assertIsNone(runner.calls[-1]["timeout_seconds"])

    def test_c3_monitor_leaves_dtr_inactive(self) -> None:
        runner = FakeRunner()
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(
                hardware,
                "PioUsbInventory",
                return_value=FakeInventory(((self.rom,),)),
            ),
        ):
            hardware.monitor_exact_device(
                workspace=self.workspace,
                target="c3",
                usb_identity="ABC",
                runner=runner,
                lock_timeout=2.0,
            )
        command = runner.calls[-1]["argv"]
        self.assertEqual(command[command.index("--dtr") + 1], "0")
        self.assertEqual(command[command.index("--rts") + 1], "0")
        self.assertEqual(command[command.index("--port") + 1], "/dev/cu.rom")

    def test_device_listing_prints_stable_serial_location_and_pid(self) -> None:
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(
                hardware,
                "PioUsbInventory",
                return_value=FakeInventory(((self.rom, self.app),)),
            ),
        ):
            rendered = hardware.list_devices_for_cli(
                self.workspace,
                FakeRunner(),
                include_ble=False,
                lock_timeout=1.0,
            )
        self.assertIn("USB_SERIAL\tLOCATION\tVID:PID\tPORT", rendered)
        self.assertIn("abc\t1-2\t303a:1001\t/dev/cu.rom", rendered)
        self.assertIn("abc\t1-2\t303a:4001\t/dev/cu.app", rendered)

    def test_device_listing_wraps_inventory_failure_without_traceback_type(self) -> None:
        failing_inventory = mock.Mock()
        failing_inventory.snapshot.side_effect = DeviceInventoryError("malformed inventory")
        with (
            mock.patch.object(hardware, "LockManager", return_value=self.manager),
            mock.patch.object(hardware, "PioUsbInventory", return_value=failing_inventory),
        ):
            with self.assertRaisesRegex(DevError, "malformed inventory"):
                hardware.list_devices_for_cli(
                    self.workspace,
                    FakeRunner(),
                    include_ble=False,
                    lock_timeout=1.0,
                )


if __name__ == "__main__":
    unittest.main()
