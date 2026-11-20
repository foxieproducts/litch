from __future__ import annotations

from contextlib import redirect_stdout
from dataclasses import replace
import csv
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from tools.dev import factory_station as station
from tools.dev.devices import ESP32S3_ROM_PID, ESPRESSIF_VID, LITCH_APP_PID, PhysicalUsbIdentity, UsbParent
from tools.dev.factory_camera import CameraChoice, CameraReading, CameraUpdate
from tools.dev.factory_records import FactoryRecords, FactoryRecordsError
from tools.dev.factory_serial import FactoryCommandNotSent, FactorySerialError
from tools.dev.factory_status import FactoryStatus
from tools.dev.hardware import FlashCandidate
from tools.dev.locks import LockManager
from tools.dev.runner import DevError


FIRST = PhysicalUsbIdentity("first", "1-2")
SECOND = PhysicalUsbIdentity("second", "1-3")
FIRST_LABEL = "sample_0034"
SECOND_LABEL = "sample_0035"


def usb(identity: PhysicalUsbIdentity, *, booted: bool = False) -> FlashCandidate:
    parent = UsbParent(identity, ESPRESSIF_VID, LITCH_APP_PID if booted else ESP32S3_ROM_PID,
                       (f"/dev/cu.{identity.location}",))
    return FlashCandidate("usb", identity.serial, parent.port, "s3" if booted else None, parent.ports, parent)


def scan(label: str) -> CameraReading:
    return CameraReading("label", label, "QR", 1)


def factory_status(*, stage: str = "radio", active: bool = True, verifier: bool = False, peer: bool = False,
                   faults: tuple[str, ...] = ()) -> FactoryStatus:
    step = {"radio": "peer_radio", "passed": "pass_saved", "inactive": "led_field", "failed": "failed_current_run",
            "override_saved": "local_complete_waiting_for_peer"}[stage]
    passed = stage in {"inactive", "passed"}
    override = stage == "override_saved"
    saved_radio = 1 if passed else 5 if override else None
    observations = {
        "active": str(int(active)), "stage": stage, "step": step,
        "provisioning_state": "passed" if passed else "override" if override else "pending",
        "completed_result": "allowed" if passed else "blocked", "save_pending": "0", "override_pending": "0",
        "peer_candidate": str(int(peer)),
    }
    if saved_radio is not None:
        observations.update(saved_record="valid", saved_radio=str(saved_radio), verifier=str(int(verifier)),
                            auto_ms="900", operator_ms="12000", radio_ms="16000")
    if passed and active:
        observations.update(radio_proof="verified", radio_mask="0x01")
    return FactoryStatus(
        active=active, stage=stage, step=step, revision=3,
        radio_role="VERIFIER" if verifier else "DUT/testing",
        provisioning_state=observations["provisioning_state"], saved_radio=saved_radio,
        save_pending=False, override_pending=False, durable_pass=passed, override=override,
        current_failures=faults, checks={"radio_ble": "pass" if passed else "pending"},
        observations=observations,
    )


class FactoryStationTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.workspace = SimpleNamespace(root=root, head="1" * 40, common_git_dir=root / ".git", branch="test")
        self.records_path = root / "results.csv"
        self.build_dir = root / "s3"
        self.build_dir.mkdir()
        self.header = root / station.FIRMWARE_VERSION_HEADER
        self.header.parent.mkdir(parents=True)
        self.header.write_text('static constexpr const char* FIRMWARE_VERSION = "0.9.0";\n')
        self.locks = LockManager(self.workspace, state_root=root / "locks")
        self.patch("platformio_build_directory", return_value=self.build_dir)
        self.patch("LockManager", return_value=self.locks)
        self.camera = self.patch("FactoryCamera").return_value
        self.camera.__enter__.return_value = self.camera
        self.camera.poll.side_effect = self.poll_camera
        self.keys = self.patch("BatchInput").return_value
        self.keys.__enter__.return_value = self.keys
        self.keys.quit_requested.side_effect = lambda: self.quit
        self.keys.begin_confirmation.side_effect = self.begin_confirmation
        self.keys.confirmation_requested.side_effect = self.confirmation_requested
        self.keys.end_confirmation.side_effect = self.end_confirmation
        self.patch("discover_possible_usb_boards", side_effect=self.discover)
        self.flash = self.patch("flash_exact_device", side_effect=self.write)
        self.patch("FactorySerial", side_effect=self.console)
        sleep = mock.patch.object(station.time, "sleep")
        sleep.start()
        self.addCleanup(sleep.stop)
        self.build = mock.Mock(side_effect=self.build_once)
        self.output = io.StringIO()
        self.quit = False
        self.frame = -1
        self.frames = iter(())
        self.reading = None
        self.current_reading = None
        self.expected_versions = {}
        self.statuses = {}
        self.flash_frames = []
        self.poll_frames = []
        self.command_frames = []
        self.command_errors = {}
        self.pass_before_command = []
        self.yes_frames = set()
        self.confirmation_frame = None
        self.confirmation_starts = []
        self.confirmation_polls = []

    def begin_confirmation(self):
        self.assertIn("Press Y", self.output.getvalue(), "show the exact destructive choice before accepting Y")
        self.confirmation_frame = self.frame
        self.confirmation_starts.append(self.frame)

    def confirmation_requested(self):
        self.confirmation_polls.append(self.frame)
        return (not self.quit and self.confirmation_frame is not None
                and self.frame > self.confirmation_frame and self.frame in self.yes_frames)

    def end_confirmation(self):
        self.confirmation_frame = None

    def patch(self, name: str, **kwargs) -> mock.MagicMock:
        patcher = mock.patch.object(station, name, **kwargs)
        self.addCleanup(patcher.stop)
        return patcher.start()

    def build_once(self, target: str) -> None:
        self.assertEqual(target, "s3")
        self.header.write_text('static constexpr const char* FIRMWARE_VERSION = "1.0.0";\n')
        for name in ("firmware.bin", "bootloader.bin", "partitions.bin"):
            (self.build_dir / name).write_bytes(name.encode())

    def discover(self, *_args):
        frame = next(self.frames, None)
        if frame is None:
            self.quit = True
            return ()
        self.frame += 1
        boards, self.reading, self.statuses = frame
        return boards

    def poll_camera(self):
        if isinstance(self.reading, Exception):
            raise self.reading
        if isinstance(self.reading, CameraUpdate):
            self.current_reading = self.reading.current
            return self.reading
        transitions = ()
        if self.reading is not None and self.reading != self.current_reading:
            transitions = (self.reading,)
            self.current_reading = self.reading
        return CameraUpdate(transitions, self.current_reading)

    def console(self, workspace, runner, identity, timeout, *, expected_version):
        self.assertEqual(expected_version, self.expected_versions.get(identity.serial, "1.0.0"))
        serial = mock.Mock()

        def poll():
            self.poll_frames.append((self.frame, identity.serial))
            # Manual qualification may take arbitrarily long without holding the
            # shared build lane across the hardware interaction.
            with self.locks.lane("platformio", timeout=0):
                status, nonce, serial.ota_accepted = self.statuses[identity.serial]
                serial.boot_nonce = nonce.zfill(16)
            return status

        def command(text, *, expected_boot_nonce):
            self.assertEqual(expected_boot_nonce, serial.boot_nonce)
            self.command_frames.append((self.frame, identity.serial, text, expected_boot_nonce))
            self.pass_before_command.append([row["result"] for row in self.rows()])
            error = self.command_errors.get((self.frame, identity.serial))
            if error is not None:
                raise error
            return "ack"

        serial.poll_status.side_effect = poll
        serial.command.side_effect = command
        return serial

    def write(self, **kwargs) -> None:
        serial = kwargs["usb_identity"]
        self.flash_frames.append((self.frame, serial))
        self.assertTrue(kwargs["factory_provision"])
        self.assertEqual(kwargs["expected_usb_identity"].serial, serial)
        row = self.rows()[-1]
        self.assertEqual((row["usb_serial"], row["phase"], row["result"]), (serial, "flashing", "pending"))
        self.assertEqual(row["firmware_version"], "1.0.0")
        kwargs["build_target"]("s3")

    def rows(self):
        with self.records_path.open(newline="") as stream:
            return list(csv.DictReader(stream))

    def run_station(self, frames, *, resume=False) -> None:
        self.frames = iter(frames)
        with redirect_stdout(self.output):
            firmware = station.build_factory_firmware(
                workspace=self.workspace, lock_timeout=4, build_target=self.build,
            )
            station.run_factory_station(
                workspace=self.workspace, runner=mock.Mock(), lock_timeout=4, firmware=firmware,
                platformio_core_dir=lambda: self.workspace.root, batch_name="SERIES.1",
                records_path=self.records_path, camera_choice=CameraChoice("native", "camera-123", "Test camera"),
                resume=resume,
            )

    def test_records_both_passes_then_moves_verifier_service_to_first_programmed_board(self) -> None:
        first = usb(FIRST, booted=True)
        both = (first, usb(SECOND, booted=True))
        radio = factory_status()
        active_pass = factory_status(stage="passed")
        # Retain the PASS flag in this controlled sample to isolate the
        # station's normal-boot check from the parser's result validation.
        finishing = factory_status(stage="passed", active=False)
        normal = factory_status(stage="inactive", active=False)
        verifier = factory_status(stage="passed", active=False, verifier=True)
        self.command_errors = {
            (7, "first"): FactoryCommandNotSent("boot changed before command"),
            (10, "second"): FactorySerialError("acknowledgement timed out"),
        }
        self.run_station([
            ((), scan(FIRST_LABEL), {}),
            ((usb(FIRST),), CameraReading("clear"), {}),
            ((first,), scan(SECOND_LABEL), {"first": (radio, "1", True)}),
            ((first, usb(SECOND)), None, {"first": (radio, "1", True)}),
            (both, None, {"first": (active_pass, "1", True), "second": (verifier, "a", False)}),
            (both, None, {"first": (finishing, "1", True), "second": (verifier, "a", False)}),
            (both, None, {"first": (normal, "2", False), "second": (verifier, "a", False)}),
            (both, None, {"first": (normal, "2", True), "second": (verifier, "a", False)}),
            (both, None, {"first": (normal, "3", True), "second": (verifier, "a", False)}),
            (both, None, {"first": (verifier, "3", False), "second": (verifier, "a", False)}),
            (both, None, {"first": (verifier, "4", False), "second": (verifier, "a", False)}),
            ((first,), None, {"first": (verifier, "4", False)}),  # DUT USB disappears across its reboot.
            (both, None, {"first": (verifier, "4", False), "second": (normal, "a", True)}),
            (both, None, {"first": (verifier, "4", False), "second": (finishing, "b", False)}),
            (both, None, {"first": (verifier, "4", False), "second": (normal, "b", True)}),
            ((first,), None, {"first": (verifier, "4", False)}),
        ])

        self.build.assert_called_once_with("s3")
        self.assertEqual(self.flash_frames, [(1, "first"), (3, "second")])
        self.assertEqual(self.command_frames, [
            (7, "first", "hwtest verifier on", "0000000000000002"),
            (8, "first", "hwtest verifier on", "0000000000000003"),
            (10, "second", "hwtest verifier off", "000000000000000a"),
        ])
        self.assertEqual(self.pass_before_command, [["pass", "pass"]] * 3)
        self.assertIn((14, "second"), self.poll_frames, "do not allow unplug before an actual normal boot")
        self.assertEqual([(r["batch_name"], r["board_number"], r["qr_id"], r["station_role"], r["result"])
                          for r in self.rows()],
                         [("SERIES.1", "0034", FIRST_LABEL, "verifier", "pass"),
                          ("SERIES.1", "0035", SECOND_LABEL, "dut", "pass")])
        output = self.output.getvalue()
        self.assertIn(f"WAIT [SERIES.1 | {FIRST_LABEL}] Waiting for a wireless peer. "
                      "Keep this board connected and prepare a second board.", output)
        self.assertIn("Verifier request did not start. Will recheck current boot", output)
        self.assertIn("Verifier request unconfirmed. Waiting for boot readback", output)
        first_pass = next(line for line in output.splitlines() if line.startswith(f"PASS [SERIES.1 | {FIRST_LABEL}]"))
        second_pass = next(line for line in output.splitlines() if line.startswith(f"PASS [SERIES.1 | {SECOND_LABEL}]"))
        self.assertIn("Keep this board connected as the verifier for the whole batch.", first_pass)
        self.assertIn("Leave connected until the station confirms normal mode and says you can unplug.", second_pass)
        self.assertIn("Saved verifier ready. Keep this board connected", output)
        self.assertIn(f"If you removed it, reconnect it at {SECOND.location}.", output)
        self.assertEqual(output.count("You can unplug this board"), 1)
        self.camera.__exit__.assert_called_once()
        self.keys.__exit__.assert_called_once()

    def test_ambiguous_admission_requires_unplug_and_fresh_scan(self) -> None:
        first = usb(FIRST, booted=True)
        radio = factory_status()
        self.run_station([
            ((usb(FIRST), usb(SECOND)), scan(FIRST_LABEL), {}),
            ((usb(FIRST),), None, {}),
            ((), CameraReading("clear"), {}),
            ((), scan(FIRST_LABEL), {}),
            ((usb(FIRST), usb(SECOND)), CameraReading("ambiguous"), {}),
            ((usb(SECOND),), CameraReading("clear"), {}),
            ((), scan(FIRST_LABEL), {}),  # A fresh scan is required after ambiguity.
            ((usb(FIRST),), CameraReading("clear"), {}),
            ((first,), scan(FIRST_LABEL), {"first": (radio, "1", True)}),
            ((first, usb(SECOND)), None, {"first": (radio, "1", True)}),
            ((), CameraReading("clear"), {}),
            ((first,), None, {"first": (radio, "2", True)}),  # No repeat flash on replug.
        ])
        self.assertEqual(self.flash_frames, [(7, "first")])
        self.assertEqual([(r["qr_id"], r["usb_serial"], r["result"]) for r in self.rows()],
                         [(FIRST_LABEL, "first", "incomplete")])
        self.assertFalse(self.command_frames)
        self.assertIn("Unplug all unclaimed USB boards", self.output.getvalue())
        self.assertIn("USB disconnected or changed", self.output.getvalue())
        self.camera.__exit__.assert_called_once()

    def test_buttons_prompt_tracks_each_required_light_response_and_legacy_status(self) -> None:
        first = usb(FIRST, booted=True)
        original = factory_status()
        local = replace(original, stage="local", step="physical_buttons",
                        observations={**original.observations, "stage": "local", "step": "physical_buttons",
                                      "manual_ambient_response": "required"},
                        checks={"light_1": "change_light", "light_2": "hold_light_steady"})
        h8_passed = replace(local, checks={"light_1": "pass", "light_2": "change_light"})
        both_passed = replace(local, checks={"light_1": "pass", "light_2": "pass"})
        occupied = replace(both_passed, observations={**both_passed.observations,
                                                     "current_hall_read": "valid", "current_hall_clear": "0"})
        legacy = replace(local, observations={**local.observations, "manual_ambient_response": "not_required"})
        self.run_station([
            ((), scan(FIRST_LABEL), {}),
            ((usb(FIRST),), None, {}),
            ((first,), None, {"first": (local, "1", True)}),
            ((first,), None, {"first": (h8_passed, "1", True)}),
            ((first,), None, {"first": (both_passed, "1", True)}),
            ((first,), None, {"first": (occupied, "1", True)}),
            ((first,), None, {"first": (legacy, "1", True)}),
        ])
        output = self.output.getvalue()
        prefix = f"NEXT [SERIES.1 | {FIRST_LABEL}] "
        self.assertIn(prefix + "Release the buttons and hold the light steady briefly. "
                      "Then cover or uncover A1 and H8 ", output)
        self.assertIn("Then cover or uncover A1 and hold until the inner lights turn green "
                      "or the wireless test's amber center square appears.", output)
        self.assertIn(prefix + "Both light sensors passed. Test and release any remaining buttons.", output)
        self.assertIn(prefix + "Remove the magnet or piece from the board so the test can continue.", output)
        self.assertIn(prefix + "Factory test: physical buttons.", output)
        self.assertFalse(self.command_frames)

    def test_connected_third_board_accepts_its_scan_while_first_verifier_stays_attached(self) -> None:
        third = PhysicalUsbIdentity("third", SECOND.location)
        third_label = "sample_0036"
        first = usb(FIRST, booted=True)
        radio = factory_status()
        verifier = factory_status(stage="passed", active=False, verifier=True)
        normal = factory_status(stage="inactive", active=False)
        primary_ready = {"first": (verifier, "1", False)}
        third_panel = (first, usb(third, booted=True))
        self.run_station([
            ((), scan(FIRST_LABEL), {}),
            ((usb(FIRST),), None, {}),
            ((first,), scan(SECOND_LABEL), {"first": (radio, "1", True)}),
            ((first, usb(SECOND)), None, {"first": (radio, "1", True)}),
            ((first, usb(SECOND, booted=True)), CameraReading("clear"),
             {**primary_ready, "second": (normal, "2", True)}),
            ((first, usb(third)), None, primary_ready),  # Board 2 removed, board 3 connected in its port.
            ((first, usb(third)), scan(third_label), primary_ready),
            (third_panel, CameraReading("clear"), {**primary_ready, "third": (radio, "3", True)}),
            (third_panel, None, {**primary_ready, "third": (factory_status(peer=True), "3", True)}),
            (third_panel, scan(third_label), {**primary_ready, "third": (normal, "4", True)}),
            (third_panel, None, primary_ready),  # A completed board and its QR can remain in place.
        ])
        self.assertEqual(self.flash_frames, [(1, "first"), (3, "second"), (6, "third")])
        self.assertEqual([(r["qr_id"], r["usb_serial"], r["result"]) for r in self.rows()],
                         [(FIRST_LABEL, "first", "pass"), (SECOND_LABEL, "second", "pass"),
                          (third_label, "third", "pass")])
        self.assertFalse(self.command_frames)
        output = self.output.getvalue()
        self.assertIn("NEXT [SERIES.1] USB board third connected. Scan this board's PCB label.", output)
        self.assertIn(f"WAIT [SERIES.1 | {third_label}] Waiting for a wireless peer. Keep the boards connected.", output)
        self.assertIn(f"WAIT [SERIES.1 | {third_label}] Wireless peer found. Verifying the link.", output)
        self.assertIn(f"PASS [SERIES.1 | {third_label}] PASS saved. Wireless wait + test 3.100 s.", output)
        self.assertNotIn("Unplug all unclaimed USB boards", output)
        after_pass = output.split(f"PASS [SERIES.1 | {third_label}] PASS saved.", 1)[1]
        self.assertIn("PASS confirmed in normal mode. You can unplug this board.", after_pass)
        self.assertNotIn("ERROR", after_pass)

    def test_operator_labels_work_in_terminal_and_plain_logs(self) -> None:
        for terminal, no_color in ((True, False), (False, False), (True, True)):
            with self.subTest(terminal=terminal, no_color=no_color):
                output = io.StringIO()
                settings = {"NO_COLOR": "1"} if no_color else {}
                with (redirect_stdout(output), mock.patch.object(output, "isatty", return_value=terminal),
                      mock.patch.dict(station.os.environ, settings, clear=True)):
                    station._say("NEXT", "SERIES.1", "Scan this board's PCB label.")
                tag = "\033[1;36mNEXT\033[0m" if terminal and not no_color else "NEXT"
                self.assertEqual(output.getvalue(), f"{tag} [SERIES.1] Scan this board's PCB label.\n")

    def test_plug_first_waits_for_qr_and_retries_held_scan_after_verifier_reconnect(self) -> None:
        first = usb(FIRST, booted=True)
        radio = factory_status()
        self.run_station([
            ((usb(FIRST),), None, {}),
            ((usb(FIRST),), None, {}),
            ((usb(FIRST),), scan(FIRST_LABEL), {}),
            ((usb(SECOND),), scan(SECOND_LABEL), {}),  # First temporarily disconnected: scan cannot bind yet.
            ((first, usb(SECOND)), None, {"first": (radio, "1", True)}),  # Same QR still visible.
        ])
        self.assertEqual(self.flash_frames, [(2, "first"), (4, "second")])
        self.assertIn("USB board first connected. Scan this board's PCB label.", self.output.getvalue())

    def test_clear_and_next_label_between_polls_recovers_scan_without_representing_label(self) -> None:
        first = usb(FIRST, booted=True)
        radio = factory_status()
        next_scan = scan(SECOND_LABEL)
        self.run_station([
            ((), scan(FIRST_LABEL), {}),
            ((usb(FIRST),), None, {}),
            ((first,), CameraReading("ambiguous"), {"first": (radio, "1", True)}),
            ((first,), CameraUpdate((CameraReading("clear"), next_scan), next_scan),
             {"first": (radio, "1", True)}),
            ((first, usb(SECOND)), None, {"first": (radio, "1", True)}),
        ])
        self.assertEqual(self.flash_frames, [(1, "first"), (4, "second")])

    def test_in_poll_conflict_cannot_be_hidden_by_clear_and_same_label(self) -> None:
        label = scan(FIRST_LABEL)
        self.run_station([
            ((usb(FIRST),), CameraUpdate((label, scan(SECOND_LABEL), CameraReading("clear"), label), label), {}),
            ((usb(FIRST),), None, {}),
            ((), CameraReading("clear"), {}),
            ((usb(FIRST),), label, {}),
        ])
        self.assertEqual(self.flash_frames, [(3, "first")])
        self.assertIn("Unplug all unclaimed USB boards", self.output.getvalue())

    def test_multiple_usb_before_scan_requires_clear_even_after_unplugging(self) -> None:
        label = scan(FIRST_LABEL)
        self.run_station([
            ((usb(FIRST), usb(SECOND)), label, {}),
            ((), None, {}),  # Same held QR must not become armed when ambiguity disappears.
            ((usb(FIRST),), None, {}),
            ((), CameraReading("clear"), {}),
            ((usb(FIRST),), label, {}),
        ])
        self.assertEqual(self.flash_frames, [(4, "first")])
        self.assertIn("Multiple unclaimed USB boards were connected. Scan cancelled.", self.output.getvalue())

    def seed_passed_batch(self) -> bytes:
        with FactoryRecords(self.records_path, self.locks, batch_name="SERIES.1") as records:
            for identity, label, role in ((FIRST, FIRST_LABEL, "verifier"), (SECOND, SECOND_LABEL, "dut")):
                attempt = records.begin(label, identity.serial, identity.location, "a" * 64,
                                        "0.9.9-8", station_role=role)
                records.update(attempt, result="pass", phase="pass_saved",
                               status=factory_status(stage="inactive", active=False))
        self.expected_versions[FIRST.serial] = "0.9.9-8"
        return self.records_path.read_bytes()

    def test_resume_reuses_saved_verifier_and_requires_one_unclaimed_board(self) -> None:
        self.seed_passed_batch()
        previous_rows = self.rows()
        moved = PhysicalUsbIdentity(FIRST.serial, "2-4")
        first = usb(moved, booted=True)
        second = usb(SECOND, booted=True)
        third_identity = PhysicalUsbIdentity("third", "1-4")
        third = usb(third_identity)
        third_label = "sample_0036"
        normal = factory_status(stage="inactive", active=False)
        cold_verifier = replace(normal, observations={**normal.observations, "verifier": "1"})
        verifier = factory_status(stage="passed", active=False, verifier=True)
        self.run_station([
            ((third,), None, {}),  # No verifier: never erase the unscanned board.
            ((first, second, third), scan(SECOND_LABEL), {"first": (cold_verifier, "1", True)}),
            ((first, second, third), scan(third_label), {"first": (cold_verifier, "1", True)}),
            ((first, second, third), None, {"first": (verifier, "2", False)}),
            ((first,), CameraReading("clear"), {"first": (verifier, "2", False)}),
            ((first, third), scan(third_label), {"first": (verifier, "2", False)}),
            ((first, usb(third_identity, booted=True)), None,
             {"first": (verifier, "2", False), "third": (normal, "3", True)}),
        ], resume=True)
        self.assertEqual(self.flash_frames, [(5, "third")])
        self.assertEqual(self.command_frames, [(1, "first", "hwtest verifier on", "0000000000000001")])
        self.assertEqual(self.rows()[:2], previous_rows)
        self.assertEqual(self.rows()[2]["result"], "pass")
        self.assertNotIn((4, "second"), self.poll_frames)
        self.assertIn("Other recorded PASS boards require Y", self.output.getvalue())
        self.assertIn("Multiple unclaimed USB boards were connected. Scan cancelled.", self.output.getvalue())

    def test_resume_without_live_saved_pass_never_admits_or_rewrites_history(self) -> None:
        before = self.seed_passed_batch()
        first = usb(FIRST, booted=True)
        third = usb(PhysicalUsbIdentity("third", "1-4"))
        self.run_station([
            ((first, third), scan("sample_0036"), {"first": (factory_status(), "1", True)}),
            ((first, third), None, {"first": (factory_status(), "1", True)}),
        ], resume=True)
        self.flash.assert_not_called()
        self.assertFalse(self.command_frames)
        self.assertEqual(self.records_path.read_bytes(), before)
        self.assertIn("waiting for this board's saved PASS", self.output.getvalue())

    def test_recorded_pass_requires_fresh_y_and_replug_before_another_rebatch(self) -> None:
        self.seed_passed_batch()
        previous = self.rows()
        first, second = usb(FIRST, booted=True), usb(SECOND, booted=True)
        verifier = factory_status(stage="passed", active=False, verifier=True)
        normal = factory_status(stage="inactive", active=False)
        ready = {"first": (verifier, "1", False)}
        self.yes_frames = {0, 1, 3, 5, 6}
        self.run_station([
            ((first, second), None, ready),  # A key arriving with a new prompt is stale.
            ((first, second), None, ready),
            ((first, second), CameraUpdate((CameraReading("clear"), scan(SECOND_LABEL)), scan(SECOND_LABEL)),
             {**ready, "second": (normal, "2", True)}),
            ((first, second), None, ready),  # Staying plugged in never offers another erase.
            ((first,), CameraReading("clear"), ready),
            ((first, second), None, ready),  # Replug offers a new, separately confirmed attempt.
            ((first, second), None, ready),
        ], resume=True)
        self.assertEqual(self.flash_frames, [(1, "second"), (6, "second")])
        self.assertEqual(self.confirmation_starts, [0, 5])
        self.assertNotIn(0, self.confirmation_polls)
        self.assertNotIn(5, self.confirmation_polls)
        self.assertEqual(self.rows()[:2], previous)
        self.assertEqual([row["result"] for row in self.rows()[2:]], ["pass", "incomplete"])
        self.assertEqual([row["qr_id"] for row in self.rows()[2:]], [SECOND_LABEL, SECOND_LABEL])
        self.assertIn("settings, games and artwork", self.output.getvalue())
        self.assertIn("Unplug to cancel", self.output.getvalue())
        self.assertNotIn("ERROR", self.output.getvalue())

    def test_completed_label_still_rejects_a_different_unclaimed_usb_board(self) -> None:
        before = self.seed_passed_batch()
        first = usb(FIRST, booted=True)
        unknown = usb(PhysicalUsbIdentity("third", "1-4"))
        verifier = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (verifier, "1", False)}
        self.run_station([
            ((first, unknown), scan(FIRST_LABEL), ready),
            ((first, unknown), None, ready),
        ], resume=True)
        self.flash.assert_not_called()
        self.assertEqual(self.records_path.read_bytes(), before)
        self.assertEqual(self.confirmation_starts, [])
        self.assertIn(f"ERROR [SERIES.1] {FIRST_LABEL} is already recorded", self.output.getvalue())

    def test_rebatch_confirmation_cancels_on_replacement_and_cannot_leak_camera_scan(self) -> None:
        self.seed_passed_batch()
        previous = self.rows()
        first = usb(FIRST, booted=True)
        moved_second = usb(PhysicalUsbIdentity(SECOND.serial, "2-3"), booted=True)
        third = PhysicalUsbIdentity("third", SECOND.location)
        third_label = "sample_0036"
        verifier = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (verifier, "1", False)}
        self.yes_frames = {1, 3, 5}
        self.run_station([
            ((first, usb(SECOND, booted=True)), scan(third_label), ready),
            ((first, moved_second), None, ready),  # Exact location changed; Y cannot confirm the old target.
            ((first, moved_second), None, ready),
            # A gap entirely between polls invalidates Y even on the same USB port.
            ((first, moved_second), CameraUpdate((CameraReading("stalled"), scan(third_label)), scan(third_label)), ready),
            ((first, moved_second), None, ready),
            ((first, usb(third)), None, ready),  # Replacement cannot consume the prior board's Y or QR.
            ((first, usb(third)), None, ready),
            ((first, usb(third)), CameraUpdate((CameraReading("clear"), scan(third_label)), scan(third_label)), ready),
        ], resume=True)
        self.assertEqual(self.flash_frames, [(7, "third")])
        self.assertEqual(self.rows()[:2], previous)
        self.assertEqual(self.confirmation_starts, [0, 2, 4])
        self.assertNotIn(1, self.confirmation_polls)
        self.assertNotIn(3, self.confirmation_polls)
        self.assertNotIn(5, self.confirmation_polls)

    def test_recorded_board_confirmation_discards_unrelated_camera_conflict(self) -> None:
        self.seed_passed_batch()
        first = usb(FIRST, booted=True)
        verifier = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (verifier, "1", False)}
        self.yes_frames = {3}
        self.run_station([
            ((first,), scan("unused_0041"), ready),
            ((first,), scan("unused_0042"), ready),
            ((first, usb(SECOND, booted=True)), None, ready),
            ((first, usb(SECOND, booted=True)), None, ready),
        ], resume=True)
        self.assertEqual(self.confirmation_starts, [2])
        self.assertEqual(self.flash_frames, [(3, "second")])
        self.assertEqual(self.rows()[-1]["qr_id"], SECOND_LABEL)

    def test_scans_during_rebatch_confirmation_cannot_authorize_the_next_board(self) -> None:
        self.seed_passed_batch()
        first, second = usb(FIRST, booted=True), usb(SECOND, booted=True)
        third = usb(PhysicalUsbIdentity("third", "1-4"), booted=True)
        third_label = "sample_0036"
        verifier = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (verifier, "1", False)}
        finished_second = {**ready, "second": (factory_status(stage="inactive", active=False), "2", True)}
        self.yes_frames = {2}
        self.run_station([
            ((first, second), CameraReading("clear"), ready),
            ((first, second), CameraUpdate((CameraReading("clear"), scan(third_label)), scan(third_label)), ready),
            ((first, second), None, ready),
            ((first, second, third), None, finished_second),  # The held pre-confirmation label remains quarantined.
            ((first, second, third), CameraUpdate((CameraReading("clear"), scan(third_label)), scan(third_label)),
             {**finished_second, "third": (factory_status(), "3", True)}),
        ], resume=True)
        self.assertEqual(self.flash_frames, [(2, "second"), (4, "third")])

    def test_rebatch_keeps_designated_verifier_protected_and_cancels_when_it_is_lost(self) -> None:
        before = self.seed_passed_batch()
        first, second = usb(FIRST, booted=True), usb(SECOND, booted=True)
        verifier = factory_status(stage="passed", active=False, verifier=True)
        self.yes_frames = {0, 1, 3, 4}
        self.run_station([
            ((usb(FIRST), second), scan(FIRST_LABEL), {}),  # Verifier serial is protected even before attachment.
            ((first, second), None, {"first": (factory_status(), "1", True)}),
            ((first, second), None, {"first": (verifier, "2", False)}),
            ((second,), None, {}),
            ((first,), None, {"first": (verifier, "2", False)}),
        ], resume=True)
        self.flash.assert_not_called()
        self.assertEqual(self.records_path.read_bytes(), before)
        self.assertEqual(self.confirmation_starts, [2])
        self.assertNotIn(3, self.confirmation_polls)

    def test_cross_batch_rebatch_conflict_refuses_erase_and_recovers_after_unplug(self) -> None:
        self.seed_passed_batch()
        other = PhysicalUsbIdentity("old-board", "1-4")
        with FactoryRecords(self.records_path, self.locks, batch_name="EARLIER") as records:
            attempt = records.begin("old_0035", other.serial, other.location, "a" * 64,
                                    "0.9.9-8", station_role="dut")
            records.update(attempt, result="pass", phase="pass_saved",
                           status=factory_status(stage="inactive", active=False))
        previous = self.rows()
        first = usb(FIRST, booted=True)
        fresh = PhysicalUsbIdentity("new-board", "1-4")
        verifier = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (verifier, "1", False)}
        self.yes_frames = {1}
        self.run_station([
            ((first, usb(other, booted=True)), None, ready),
            ((first, usb(other, booted=True)), None, ready),
            ((first,), CameraReading("clear"), ready),
            ((first, usb(fresh)), scan("new_0040"), ready),
        ], resume=True)
        self.assertEqual(self.flash_frames, [(3, "new-board")])
        self.assertEqual(self.rows()[:3], previous)
        self.assertIn("EARLIER", self.output.getvalue())
        self.assertIn("conflicts with another QR ID", self.output.getvalue())

    def test_resume_missing_batch_stops_before_camera_after_initial_build(self) -> None:
        with self.assertRaisesRegex(FactoryRecordsError, "no recorded verifier"):
            self.run_station([], resume=True)
        self.build.assert_called_once_with("s3")
        self.camera.__enter__.assert_not_called()
        self.flash.assert_not_called()

    def test_conflicting_stable_scan_cancels_admission_until_clear_and_fresh_scan(self) -> None:
        self.run_station([
            ((), scan(FIRST_LABEL), {}),
            ((usb(SECOND),), scan(SECOND_LABEL), {}),  # Conflict and cable in the same poll.
            ((), CameraReading("clear"), {}),
            ((), scan(FIRST_LABEL), {}),
            ((), scan(SECOND_LABEL), {}),  # Conflict occurs before a later cable insertion.
            ((usb(SECOND),), None, {}),
            ((), CameraReading("clear"), {}),
            ((), scan(SECOND_LABEL), {}),
            ((usb(SECOND),), None, {}),
        ])
        self.assertEqual(self.flash_frames, [(8, "second")])
        self.assertEqual([(row["qr_id"], row["usb_serial"]) for row in self.rows()], [(SECOND_LABEL, "second")])
        self.assertIn(f"Scan changed from {FIRST_LABEL} to {SECOND_LABEL}. Previous scan cancelled.",
                      self.output.getvalue())
        self.assertIn("Unplug all unclaimed USB boards. Clear the preview", self.output.getvalue())

    def test_conflicting_persisted_binding_stops_before_any_flash(self) -> None:
        with FactoryRecords(self.records_path, self.locks, batch_name="previous") as records:
            attempt = records.begin(FIRST_LABEL, SECOND.serial, SECOND.location, "a" * 64,
                                    "1.0.0", station_role="dut")
            records.update(attempt, result="fail", failure_text="earlier flash failed")
        with self.assertRaisesRegex(FactoryRecordsError, "conflict"):
            self.run_station([((), scan(FIRST_LABEL), {}), ((usb(FIRST),), None, {})])
        self.flash.assert_not_called()
        self.assertEqual(len(self.rows()), 1)
        self.camera.__exit__.assert_called_once()

    def test_flash_error_stops_without_retry_and_preserves_recoverable_live_fault(self) -> None:
        first = usb(FIRST, booted=True)
        fault = factory_status(stage="failed", faults=("Light sensor link failed",))

        def fail_second(**kwargs):
            self.write(**kwargs)
            if kwargs["usb_identity"] == "second":
                raise DevError("esptool lost connection")

        self.flash.side_effect = fail_second
        with self.assertRaisesRegex(DevError, "esptool lost connection"):
            self.run_station([
                ((), scan(FIRST_LABEL), {}),
                ((usb(FIRST),), None, {}),
                ((first,), scan(SECOND_LABEL), {"first": (fault, "1", True)}),
                ((first, usb(SECOND)), None, {"first": (factory_status(), "1", True)}),
            ])
        self.assertEqual(self.flash_frames, [(1, "first"), (3, "second")])
        first_row, second_row = self.rows()
        self.assertEqual(first_row["result"], "incomplete")
        self.assertEqual(first_row["observed_failures"], "Light sensor link failed")
        self.assertEqual(first_row["failure_text"], "")
        self.assertEqual((second_row["result"], second_row["failure_text"]), ("fail", "esptool lost connection"))
        self.assertIn(f"[SERIES.1 | {FIRST_LABEL}] FACTORY FAULT: Light sensor link failed", self.output.getvalue())
        self.assertIn(f"[SERIES.1 | {SECOND_LABEL}] FLASH FAILED", self.output.getvalue())

    def test_failed_resumed_dut_keeps_pass_history_and_gives_recorded_resume_command(self) -> None:
        self.records_path = self.records_path.with_name("factory results.csv")
        self.seed_passed_batch()
        prior_rows = self.rows()
        third = PhysicalUsbIdentity("third", SECOND.location)
        verifier = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (verifier, "1", False)}

        def fail(**kwargs):
            self.write(**kwargs)
            raise DevError("Bootloader entry failed before erase/write")

        self.flash.side_effect = fail
        with self.assertRaisesRegex(DevError, "Factory batch stopped after a flash failure") as stopped:
            self.run_station([
                ((usb(FIRST, booted=True),), scan("sample_0067"), ready),
                ((usb(FIRST, booted=True), usb(third)), None, ready),
            ], resume=True)
        self.assertEqual(self.flash_frames, [(1, "third")])
        self.assertEqual(self.rows()[:2], prior_rows)
        self.assertEqual(self.rows()[-1]["result"], "fail")
        self.assertIn("Bootloader entry failed before erase/write", self.output.getvalue())
        self.assertIn(f"Keep verifier {FIRST_LABEL} connected", str(stopped.exception))
        self.assertIn("./litch flash s3 --batch --factory --resume --batch-name SERIES.1", str(stopped.exception))
        self.assertIn(f"--records '{self.records_path.resolve()}'", str(stopped.exception))
        self.assertIn("Rescan sample_0067", str(stopped.exception))

    def test_camera_stall_preserves_board_checks_and_disconnect_stops_admission(self) -> None:
        first = usb(FIRST, booted=True)
        radio = factory_status()
        passed = factory_status(stage="passed", active=False, verifier=True)
        ready = {"first": (passed, "2", False)}
        gap = CameraReading("stalled")
        second_scan = scan(SECOND_LABEL)
        with self.assertRaisesRegex(DevError, "camera disconnected"):
            self.run_station([
                ((), scan(FIRST_LABEL), {}),
                ((usb(FIRST),), None, {}),
                ((first,), second_scan, {"first": (radio, "1", True)}),
                ((first, usb(SECOND)), CameraUpdate((gap,), stalled=True), {"first": (radio, "1", True)}),
                ((first, usb(SECOND)), CameraUpdate(stalled=True), ready),
                ((first, usb(SECOND)), CameraUpdate(stalled=True), ready),
                ((first, usb(SECOND)), second_scan, ready),  # Held across the gap.
                ((first, usb(SECOND)), CameraUpdate((CameraReading("clear"), second_scan), second_scan), ready),
                ((first, usb(SECOND)), DevError("camera disconnected"), ready),
            ])
        self.assertEqual(self.flash_frames, [(1, "first"), (7, "second")])
        self.assertIn((4, "first"), self.poll_frames)
        self.assertEqual(self.output.getvalue().count("Camera frames paused"), 2)  # New resume option after PASS.
        self.assertIn(f"--resume --batch-name SERIES.1 --records {self.records_path.resolve()}", self.output.getvalue())
        finished, interrupted = self.rows()
        self.assertEqual((finished["result"], interrupted["result"]), ("pass", "incomplete"))
        self.assertIn("camera disconnected", interrupted["notes"])
        self.camera.__exit__.assert_called_once()

    def test_quit_waits_for_current_write_then_leaves_board_and_marks_attempt_incomplete(self) -> None:
        def write_then_quit(**kwargs):
            self.write(**kwargs)
            self.quit = True

        self.flash.side_effect = write_then_quit
        self.run_station([((), scan(FIRST_LABEL), {}), ((usb(FIRST),), None, {})])
        self.assertEqual(self.flash_frames, [(1, "first")])
        self.assertEqual(self.command_frames, [])
        [row] = self.rows()
        self.assertEqual((row["phase"], row["result"]), ("waiting-for-factory-boot", "incomplete"))
        self.camera.__exit__.assert_called_once()

    def test_first_board_override_is_saved_without_promoting_another_board(self) -> None:
        override = factory_status(stage="override_saved", active=False)
        with self.assertRaisesRegex(DevError, "First board .* saved Override"):
            self.run_station([
                ((), scan(FIRST_LABEL), {}),
                ((usb(FIRST),), None, {}),
                ((usb(FIRST, booted=True),), scan(SECOND_LABEL), {"first": (override, "1", True)}),
            ])
        self.assertEqual(self.flash_frames, [(1, "first")])
        self.assertEqual(self.command_frames, [])
        self.assertEqual([row["result"] for row in self.rows()], ["override"])
