from __future__ import annotations

import json
import sys
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import devices  # noqa: E402


@dataclass(frozen=True)
class FakeResult:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


class FakeRunner:
    def __init__(self, result: FakeResult | BaseException) -> None:
        self.result = result
        self.calls: list[tuple[tuple[str, ...], float]] = []

    def run(self, argv: Sequence[str], *, timeout_seconds: float) -> FakeResult:
        self.calls.append((tuple(argv), timeout_seconds))
        if isinstance(self.result, BaseException):
            raise self.result
        return self.result


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class FakeInventory:
    def __init__(self, snapshots: Sequence[Sequence[devices.UsbParent]]) -> None:
        self.snapshots = [tuple(value) for value in snapshots]
        self.index = 0

    def snapshot(self) -> tuple[devices.UsbParent, ...]:
        if not self.snapshots:
            return ()
        index = min(self.index, len(self.snapshots) - 1)
        self.index += 1
        return self.snapshots[index]


def parent(
    identity: devices.PhysicalUsbIdentity,
    pid: int,
    port: str,
    *,
    vid: int = devices.ESPRESSIF_VID,
) -> devices.UsbParent:
    return devices.UsbParent(identity, vid, pid, (port,))


class DeviceInventoryParserTests(unittest.TestCase):
    def test_esp32_mac_serial_spellings_share_one_physical_identity(self) -> None:
        compact = devices.PhysicalUsbIdentity("00005E0053AB", "1-1")
        colon = devices.PhysicalUsbIdentity("00:00:5e:00:53:ab", "1-1")
        hyphen = devices.PhysicalUsbIdentity("00-00-5e-00-53-ab", "1-1")

        self.assertEqual(compact, colon)
        self.assertEqual(compact, hyphen)
        self.assertEqual(compact.serial, "00005e0053ab")

    def test_parses_pio_fixture_and_normalizes_tty_cu_aliases(self) -> None:
        payload = json.dumps(
            [
                {
                    "port": "/dev/tty.usbmodem1101",
                    "description": "USB JTAG/serial debug unit",
                    "hwid": "USB VID:PID=303A:1001 SER=ABC123 LOCATION=20-1",
                    "vid": "303A",
                    "pid": "1001",
                    "serial_number": "abc123",
                    "location": "20-1",
                },
                {
                    "port": "/dev/cu.usbmodem1101",
                    "description": "USB JTAG/serial debug unit",
                    "hwid": "USB VID:PID=303A:1001 SER=ABC123 LOCATION=20-1",
                    "vid": 0x303A,
                    "pid": 0x1001,
                    "serial_number": "ABC123",
                    "location": "20-1",
                },
                {
                    "port": "/dev/cu.Bluetooth-Incoming-Port",
                    "description": "n/a",
                    "hwid": "n/a",
                },
            ]
        )
        endpoints = devices.parse_pio_device_list(payload)
        parents = devices.normalize_usb_parents(endpoints)

        self.assertEqual(len(parents), 1)
        self.assertEqual(
            parents[0].identity,
            devices.PhysicalUsbIdentity("ABC123", "20-1"),
        )
        self.assertEqual(parents[0].vid, 0x303A)
        self.assertEqual(parents[0].pid, 0x1001)
        self.assertEqual(
            parents[0].ports,
            ("/dev/cu.usbmodem1101", "/dev/tty.usbmodem1101"),
        )
        self.assertEqual(parents[0].port, "/dev/cu.usbmodem1101")

    def test_accepts_devices_wrapper_and_hwid_only_identity(self) -> None:
        payload = json.dumps(
            {
                "devices": [
                    {
                        "port": "/dev/cu.usbmodem1",
                        "hwid": "USB VID:PID=303A:4001 SER=S3 LOCATION=1-4",
                    }
                ]
            }
        )
        parents = devices.normalize_usb_parents(
            devices.parse_pio_device_list(payload)
        )
        self.assertEqual(parents[0].pid, devices.LITCH_APP_PID)
        self.assertEqual(parents[0].identity.serial, "s3")

    def test_rejects_invalid_json_duplicate_ports_and_field_disagreement(self) -> None:
        invalid_payloads = (
            "not-json",
            "{}",
            json.dumps([{"port": "x"}, {"port": "x"}]),
            json.dumps(
                [
                    {
                        "port": "x",
                        "vid": 0x303A,
                        "pid": 0x4001,
                        "hwid": "USB VID:PID=303A:1001 SER=s LOCATION=l",
                    }
                ]
            ),
            json.dumps(
                [
                    {
                        "port": "x",
                        "serial_number": "one",
                        "hwid": "USB VID:PID=303A:1001 SER=two LOCATION=l",
                    }
                ]
            ),
        )
        for payload in invalid_payloads:
            with self.subTest(payload=payload):
                with self.assertRaises(devices.DeviceInventoryError):
                    devices.parse_pio_device_list(payload)

    def test_rejects_espressif_endpoint_without_exact_identity(self) -> None:
        endpoint = devices.UsbEndpoint(
            port="/dev/cu.usb",
            vid=devices.ESPRESSIF_VID,
            pid=devices.ESP32S3_ROM_PID,
            serial="serial",
            location=None,
        )
        with self.assertRaises(devices.DeviceInventoryError):
            devices.normalize_usb_parents((endpoint,))

    def test_pio_inventory_is_injected_and_errors_are_actionable(self) -> None:
        payload = json.dumps(
            [
                {
                    "port": "/dev/cu.usb",
                    "hwid": "USB VID:PID=303A:1001 SER=s LOCATION=l",
                }
            ]
        )
        runner = FakeRunner(FakeResult(stdout=payload))
        inventory = devices.PioUsbInventory(runner, timeout_seconds=2.5)
        self.assertEqual(inventory.snapshot()[0].pid, 0x1001)
        self.assertEqual(
            runner.calls,
            [(('pio', 'device', 'list', '--json-output'), 2.5)],
        )

        for result in (FakeResult(returncode=2, stderr="pio failed"), TimeoutError("late")):
            with self.subTest(result=result):
                with self.assertRaises(devices.DeviceInventoryError):
                    devices.PioUsbInventory(FakeRunner(result)).snapshot()


class UsbTransitionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.identity = devices.PhysicalUsbIdentity("ABC", "20-1")
        self.rom = parent(
            self.identity,
            devices.ESP32S3_ROM_PID,
            "/dev/cu.rom",
        )
        self.app = parent(
            self.identity,
            devices.LITCH_APP_PID,
            "/dev/cu.app",
        )

    def wait(
        self,
        snapshots: Sequence[Sequence[devices.UsbParent]],
        *,
        timeout_seconds: float = 1.0,
    ) -> devices.UsbParent:
        clock = FakeClock()
        return devices.wait_for_exact_usb_transition(
            FakeInventory(snapshots),
            identity=self.identity,
            timeout_seconds=timeout_seconds,
            poll_interval_seconds=0.1,
            stable_target_reads=2,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )

    def test_tracks_exact_parent_rom_to_app_with_stable_target(self) -> None:
        result = self.wait(((self.rom,), (), (self.app,), (self.app,)))
        self.assertEqual(result, self.app)

    def test_tracks_same_esp32_when_rom_adds_mac_separators(self) -> None:
        app = parent(
            devices.PhysicalUsbIdentity("00005e0053ab", "1-1"),
            devices.LITCH_APP_PID,
            "/dev/cu.app",
        )
        rom = parent(
            devices.PhysicalUsbIdentity("00:00:5e:00:53:ab", "1-1"),
            devices.ESP32S3_ROM_PID,
            "/dev/cu.rom",
        )
        clock = FakeClock()
        result = devices.wait_for_exact_usb_transition(
            FakeInventory(((app,), (), (rom,), (rom,))),
            identity=app.identity,
            from_pid=devices.LITCH_APP_PID,
            to_pid=devices.ESP32S3_ROM_PID,
            timeout_seconds=1.0,
            poll_interval_seconds=0.1,
            stable_target_reads=2,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )

        self.assertEqual(result, rom)

    def test_directly_sampled_transition_is_accepted(self) -> None:
        result = self.wait(((self.rom,), (self.app,), (self.app,)))
        self.assertEqual(result, self.app)

    def test_pre_reset_capture_allows_app_on_first_post_reset_poll(self) -> None:
        capture_inventory = FakeInventory(((self.rom,),))
        captured = devices.capture_exact_usb_parent(
            capture_inventory,
            identity=self.identity,
            vid=devices.ESPRESSIF_VID,
            pid=devices.ESP32S3_ROM_PID,
        )
        post_reset_inventory = FakeInventory(((self.app,), (self.app,)))
        clock = FakeClock()
        result = devices.wait_for_exact_usb_transition(
            post_reset_inventory,
            identity=self.identity,
            initial_source=captured,
            timeout_seconds=1.0,
            poll_interval_seconds=0.1,
            stable_target_reads=2,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )
        self.assertEqual(result, self.app)
        self.assertEqual(post_reset_inventory.index, 2)

    def test_rejects_mismatched_captured_source_without_polling(self) -> None:
        wrong = parent(
            devices.PhysicalUsbIdentity("OTHER", self.identity.location),
            devices.ESP32S3_ROM_PID,
            "/dev/cu.other",
        )
        inventory = FakeInventory(((self.app,),))
        with self.assertRaises(devices.DeviceInventoryError):
            devices.wait_for_exact_usb_transition(
                inventory,
                identity=self.identity,
                initial_source=wrong,
            )
        self.assertEqual(inventory.index, 0)

    def test_no_global_only_device_fallback(self) -> None:
        other_identity = devices.PhysicalUsbIdentity("OTHER", "9-9")
        only_other = parent(other_identity, devices.ESP32S3_ROM_PID, "/dev/cu.other")
        with self.assertRaises(devices.DeviceInventoryError):
            self.wait(((only_other,),))

    def test_initial_same_identity_with_two_pids_is_ambiguous(self) -> None:
        with self.assertRaises(devices.DeviceInventoryError):
            self.wait(((self.rom, self.app),))

    def test_serial_drift_at_same_location_fails_closed(self) -> None:
        drift = parent(
            devices.PhysicalUsbIdentity("DIFFERENT", self.identity.location),
            devices.LITCH_APP_PID,
            "/dev/cu.drift",
        )
        with self.assertRaises(devices.DeviceInventoryError):
            self.wait(((self.rom,), (), (drift,)))

    def test_unexpected_pid_and_rom_reappearance_fail_closed(self) -> None:
        unexpected = parent(self.identity, 0x9999, "/dev/cu.unexpected")
        bad_sequences = (
            ((self.rom,), (unexpected,)),
            ((self.rom,), (), (self.rom,)),
        )
        for snapshots in bad_sequences:
            with self.subTest(snapshots=snapshots):
                with self.assertRaises(devices.DeviceInventoryError):
                    self.wait(snapshots)

    def test_postflash_controller_reappearance_can_precede_application(self) -> None:
        result = self.wait(((self.rom,), (), (self.rom,), (),
                            (self.app,), (self.app,)))
        self.assertEqual(result, self.app)

    def test_bounded_exact_parent_return_requires_stable_identity(self) -> None:
        clock = FakeClock()
        result = devices.wait_for_exact_usb_parent(
            FakeInventory(((), (self.rom,), (self.rom,))),
            identity=self.identity,
            vid=devices.ESPRESSIF_VID,
            pid=devices.ESP32S3_ROM_PID,
            timeout_seconds=1.0,
            poll_interval_seconds=0.1,
            stable_reads=2,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )
        self.assertEqual(result, self.rom)

    def test_missing_application_identity_is_reported_as_post_reset_timeout(self) -> None:
        with self.assertRaisesRegex(
            devices.DeviceInventoryError,
            r"303a:1001 to become 303a:4001",
        ):
            self.wait(((self.rom,), ()), timeout_seconds=0.3)

    def test_exact_parent_return_rejects_identity_drift_and_wrong_pid(self) -> None:
        drift = parent(
            devices.PhysicalUsbIdentity("OTHER", self.identity.location),
            devices.ESP32S3_ROM_PID,
            "/dev/cu.other",
        )
        for snapshots in (((drift,),), ((self.app,),)):
            with self.subTest(snapshots=snapshots):
                clock = FakeClock()
                with self.assertRaises(devices.DeviceInventoryError):
                    devices.wait_for_exact_usb_parent(
                        FakeInventory(snapshots),
                        identity=self.identity,
                        vid=devices.ESPRESSIF_VID,
                        pid=devices.ESP32S3_ROM_PID,
                        timeout_seconds=0.5,
                        poll_interval_seconds=0.1,
                        monotonic=clock.monotonic,
                        sleep=clock.sleep,
                    )

    def test_transition_is_bounded(self) -> None:
        with self.assertRaisesRegex(devices.DeviceInventoryError, "timed out"):
            self.wait(((self.rom,),), timeout_seconds=0.25)


if __name__ == "__main__":
    unittest.main()
