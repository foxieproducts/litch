from __future__ import annotations

from contextlib import contextmanager, redirect_stderr, redirect_stdout
from dataclasses import asdict
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

from tools.dev import factory_serial
from tools.dev.devices import PhysicalUsbIdentity
from tools.dev.factory_serial import FactoryCommandNotSent, FactorySerial, FactorySerialError
from tools.dev.locks import LockTimeoutError
from tools.dev.runner import CompletedCommand, DevError, Runner
from tools.dev.workspace import Workspace


VERSION = "1.0.0-0"
BOOT_NONCE = "7abc910d2ef54301"
VERSION_REPLY = (f"firmware: {VERSION}\r\nimage_sha256: pending\r\nota_accepted: 0\r\n"
                 f"boot_nonce: {BOOT_NONCE}\r\nota_admission: available=0\r\n")
STATUS_REPLY = """\
hwtest: radio_role=DUT/testing channel=red
hwtest: active=1 stage=local step=led_field revision=4 buttons=0/12 halls_clear=0/64 halls_active=0/64
hwtest: button_mask=0x0000/0x37ef hall_clear=0x0000000000000000 hall_active=0x0000000000000000
hwtest: responding_expanders=0x0f known=1 motion_present=1 known=1 forced_faults=0x00 current_failed_checks=0x00
hwtest: current_hall_read=valid current_hall_clear=1
hwtest: sensor=1 raw_lux=0 room=0 covered=0 recovered=0 next=n/a
hwtest: sensor=2 raw_lux=0 room=0 covered=0 recovered=0 next=n/a
hwtest: auto_mask=0x0e/0x0e auto_under_10s=1 motion_event=0 leds_confirmed=0 radio_mask=0x00
hwtest: checks led_scan=pending board_io=pass light_1=n/a light_2=n/a motion_link=pass motion_event=pending led_review=pending radio_ble=pending
hwtest: radio_proof=pending_not_broken_radio_proof peer_candidate=0 discovery_deadline_ms=0 ble_endpoint_error=0 save_pending=0 override_pending=0
hwtest: retained_radio_phase current=0 previous=0 previous_valid=0 reset_reason=0
hwtest: manual_ambient_response=not_required observed_mask=0x00 manual_motion=not_required observed=0
hwtest: provisioning_state=pending completed_result=blocked next=complete_full_test_and_save
hwtest: saved_record=none
"""


class ConsoleTransport:
    def __init__(self, replies: dict[bytes, str]) -> None:
        self.replies = replies
        self.pending = b""
        self.calls: list[object] = []

    def reset_input_buffer(self) -> None:
        self.calls.append("discard-input")
        self.pending = b""

    def write(self, data: bytes) -> int:
        self.calls.append(data)
        self.pending += self.replies[data].encode("utf-8")
        return len(data)

    def read(self, size: int) -> bytes:
        # Split both the command echo and records like a busy native-USB console.
        count = min(size, 79)
        chunk, self.pending = self.pending[:count], self.pending[count:]
        return chunk

    def set_control_lines(self, *, dtr: bool, rts: bool) -> None:
        self.calls.append((dtr, rts))

    def close(self) -> None:
        self.calls.append("close")


class FactorySerialExchangeTests(unittest.TestCase):
    def exchange(self, command: str, responses: dict[bytes, str], *, version: str = VERSION_REPLY):
        transport = ConsoleTransport({b"version\n": "\r> v\r> version\r\n" + version, **responses})
        factory = mock.Mock()
        factory.open.return_value = transport
        clock = iter(index / 1000 for index in range(3000))
        try:
            with mock.patch.object(factory_serial.time, "monotonic", side_effect=clock):
                return factory_serial._exchange(factory, "/dev/cu.exact", command, VERSION, 1.2,
                                                expected_boot_nonce=BOOT_NONCE)
        finally:
            self.transport = transport
            factory.open.assert_called_once_with("/dev/cu.exact", baudrate=115200,
                                                 read_timeout_seconds=0.05, dtr=True, rts=True)
            self.assertEqual(transport.calls[-2:], [(False, False), "close"])

    def test_status_ignores_pre_echo_rows_and_waits_for_complete_saved_record(self) -> None:
        stale = STATUS_REPLY.replace("revision=4", "revision=99")
        current = STATUS_REPLY.replace("\n", "\r\n\r> \r")
        exchange = self.exchange("hwtest status", {
            b"hwtest status\n": stale + "\r> hwtest statu\r> hwtest status\r\n\x1b[0m" + current,
        })

        self.assertEqual(factory_serial.parse_factory_status(exchange.response).revision, 4)
        self.assertEqual(asdict(exchange.runtime), {"version": VERSION, "boot_nonce": BOOT_NONCE, "ota_accepted": False})
        self.assertEqual(self.transport.calls, ["discard-input", b"version\n", b"hwtest status\n", (False, False), "close"])

    def test_missing_echo_or_truncated_terminal_record_never_returns_a_status(self) -> None:
        for reply in (STATUS_REPLY, "\r> hwtest status\r\n" + STATUS_REPLY.rstrip()):
            with self.subTest(reply=reply[-40:]):
                with self.assertRaisesRegex(FactorySerialError, "no complete current response"):
                    self.exchange("hwtest status", {b"hwtest status\n": reply})

    def test_wrong_version_stops_before_mutating_the_console(self) -> None:
        with self.assertRaisesRegex(FactoryCommandNotSent, "does not match expected"):
            self.exchange("hwtest verifier on", {}, version=VERSION_REPLY.replace(VERSION, "9.9.9"))
        self.assertEqual(self.transport.calls, ["discard-input", b"version\n", (False, False), "close"])

    def test_expired_budget_is_known_not_sent_before_write(self) -> None:
        transport = mock.Mock(spec=ConsoleTransport)
        with mock.patch.object(factory_serial.time, "monotonic", return_value=1.97):
            with self.assertRaisesRegex(FactoryCommandNotSent, "before sending"):
                factory_serial._query(transport, "hwtest verifier off", 2.0, lambda rows: None)
        transport.write.assert_not_called()

    def test_verifier_uses_exact_acknowledgement_and_preserves_refusal(self) -> None:
        command = "hwtest verifier on"
        accepted = "hwtest: saving verifier=on; clean reboot into factory verifier essentials"
        reply = "> hwtest verifier on\r\n" + accepted + "\r\n"
        exchange = self.exchange(command, {b"hwtest verifier on\n": reply})
        self.assertEqual(exchange.response, accepted)
        self.assertEqual(exchange.runtime.boot_nonce, BOOT_NONCE)
        refused = "> hwtest verifier on\r\nhwtest: verifier transition busy; no boot request armed\r\n"
        with self.assertRaisesRegex(FactorySerialError, "verifier transition busy") as denied:
            self.exchange(command, {b"hwtest verifier on\n": accepted + "\r\n" + refused})
        self.assertNotIsInstance(denied.exception, FactoryCommandNotSent)
        self.assertEqual(self.transport.calls.count(b"hwtest verifier on\n"), 1)


class FactorySerialAdmissionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runner = mock.Mock(spec=Runner)
        self.locks = mock.Mock()
        self.locked = False
        self.locks.device.side_effect = self.lock_device
        patch = mock.patch.object(factory_serial, "LockManager", return_value=self.locks)
        patch.start()
        self.addCleanup(patch.stop)
        root = Path(__file__).resolve().parents[3]
        workspace = Workspace(root, root / ".git", None, "abc", False)
        self.identity = PhysicalUsbIdentity("00:00:5E:00:53:E1", "1-1")
        self.observer = FactorySerial(workspace, self.runner, self.identity, 0.1, expected_version=VERSION)

    @contextmanager
    def lock_device(self, serial: str, *, timeout: float):
        self.assertEqual((serial, timeout), (self.identity.serial, 0.1))
        self.assertFalse(self.locked)
        self.locked = True
        try:
            yield
        finally:
            self.locked = False

    def endpoint(self, port: str, *, location: str = "1-1", pid: str = "4001") -> dict[str, str]:
        return {"port": port, "hwid": f"USB VID:PID=303A:{pid} SER={self.identity.serial} LOCATION={location}"}

    def test_each_poll_recaptures_the_exact_app_port_while_locked(self) -> None:
        ports = iter(("/dev/cu.first", "/dev/cu.reenumerated"))
        selected_ports = []

        def run(argv, *, timeout_seconds, **kwargs):
            self.assertTrue(self.locked)
            if argv[0] == "pio":
                selected_ports.append(next(ports))
                endpoints = [self.endpoint(selected_ports[-1]), self.endpoint("/dev/cu.wrong", location="2-2")]
                return CompletedCommand(argv, 0, json.dumps(endpoints), "")
            self.assertEqual(argv[argv.index("--port") + 1], selected_ports[-1])
            self.assertLessEqual(timeout_seconds, 2.0)
            self.assertLess(float(argv[argv.index("--io-timeout") + 1]), timeout_seconds)
            self.assertEqual(argv[argv.index("--expected-version") + 1], VERSION)
            runtime = {"version": VERSION, "boot_nonce": f"{len(selected_ports):016x}", "ota_accepted": False}
            return CompletedCommand(argv, 0, json.dumps({"runtime": runtime, "response": STATUS_REPLY}), "")

        self.runner.run.side_effect = run
        self.assertTrue(self.observer.poll_status().active)
        self.assertEqual(self.observer.boot_nonce, "0000000000000001")
        self.assertTrue(self.observer.poll_status().active)
        self.assertEqual(self.observer.boot_nonce, "0000000000000002")
        self.assertEqual(self.observer.firmware_version, VERSION)
        self.assertFalse(self.observer.ota_accepted)
        self.assertEqual(self.runner.run.call_count, 4)
        self.assertFalse(self.locked)

    def test_rom_location_drift_and_unsupported_commands_never_open_serial(self) -> None:
        for endpoint in (self.endpoint("/dev/cu.rom", pid="1001"),
                         self.endpoint("/dev/cu.other", location="2-2")):
            with self.subTest(endpoint=endpoint):
                self.runner.run.reset_mock()
                self.runner.run.return_value = CompletedCommand((), 0, json.dumps([endpoint]), "")
                with self.assertRaises(FactorySerialError):
                    self.observer.poll_status()
                self.runner.run.assert_called_once()
                self.assertFalse(self.locked)
        self.runner.reset_mock()
        with self.assertRaisesRegex(FactoryCommandNotSent, "unsupported"):
            self.observer.command("hwtest verifier on\nreboot", expected_boot_nonce=BOOT_NONCE)
        self.runner.run.assert_not_called()

    def test_lock_timeout_and_failed_exact_admission_confirm_no_verifier_write(self) -> None:
        self.locks.device.side_effect = LockTimeoutError(resource="device", path=Path("lock"), timeout=0.1, owner=None)
        with self.assertRaises(FactoryCommandNotSent):
            self.observer.command("hwtest verifier on", expected_boot_nonce=BOOT_NONCE)
        self.runner.run.assert_not_called()

        self.locks.device.side_effect = self.lock_device
        self.runner.run.return_value = CompletedCommand((), 0, "[]", "")
        with self.assertRaises(FactoryCommandNotSent):
            self.observer.command("hwtest verifier on", expected_boot_nonce=BOOT_NONCE)
        self.runner.run.assert_called_once()
        self.assertFalse(self.locked)

    def test_child_boot_nonce_fence_confirms_no_verifier_write(self) -> None:
        versions = (VERSION_REPLY.replace(BOOT_NONCE, "a" * 16), VERSION_REPLY.replace(BOOT_NONCE, "0" * 16))
        for version in versions:
            with self.subTest(version=version):
                transport = ConsoleTransport({b"version\n": "\r> version\r\n" + version})
                factory = mock.Mock()
                factory.open.return_value = transport

                def run(argv, **kwargs):
                    self.assertTrue(self.locked)
                    if argv[0] == "pio":
                        return CompletedCommand(argv, 0, json.dumps([self.endpoint("/dev/cu.exact")]), "")
                    stdout, stderr = io.StringIO(), io.StringIO()
                    with mock.patch.object(sys, "argv", [argv[2], *argv[3:]]):
                        with mock.patch.object(factory_serial, "PySerialFactory", return_value=factory):
                            with redirect_stdout(stdout), redirect_stderr(stderr):
                                status = factory_serial._serial_main()
                    return CompletedCommand(argv, status, stdout.getvalue(), stderr.getvalue())

                self.runner.run.side_effect = run
                with self.assertRaisesRegex(FactoryCommandNotSent, "boot nonce changed or is unavailable"):
                    self.observer.command("hwtest verifier on", expected_boot_nonce=BOOT_NONCE)
                self.assertEqual(transport.calls, ["discard-input", b"version\n", (False, False), "close"])
                self.assertFalse(self.locked)

    def test_killed_serial_child_releases_lock_without_retry(self) -> None:
        self.runner.run.side_effect = [
            CompletedCommand((), 0, json.dumps([self.endpoint("/dev/cu.hung")]), ""),
            DevError("hard deadline exceeded"),
        ]
        with self.assertRaisesRegex(FactorySerialError, "hard deadline") as timed_out:
            self.observer.command("hwtest verifier off", expected_boot_nonce=BOOT_NONCE)
        self.assertNotIsInstance(timed_out.exception, FactoryCommandNotSent)
        self.assertFalse(self.locked)
        self.assertEqual(self.runner.run.call_count, 2)
        self.assertIsNone(self.observer.boot_nonce)


if __name__ == "__main__":
    unittest.main()
