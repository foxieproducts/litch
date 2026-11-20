from __future__ import annotations

import csv
from dataclasses import replace
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from tools.dev import factory_records
from tools.dev.factory_records import FactoryRecords, FactoryRecordsError
from tools.dev.factory_status import FactoryStatus
from tools.dev.locks import LockManager, LockTimeoutError


_SAVED_PROOF = {
    "active": "0", "stage": "inactive", "step": "led_field", "provisioning_state": "passed",
    "completed_result": "allowed", "save_pending": "0", "override_pending": "0", "saved_record": "valid",
    "saved_radio": "3", "verifier": "0", "auto_ms": "900", "operator_ms": "15000", "radio_ms": "8000",
}


class FactoryRecordsTests(unittest.TestCase):
    batch_name = "SERIES.1"
    label = "sample_0034"
    serial = "00005e0053e1"
    firmware_sha = "a" * 64
    source_commit = "b" * 40

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        workspace = SimpleNamespace(root=root, common_git_dir=root / ".git", branch="main", head=self.source_commit)
        self.locks = LockManager(workspace, state_root=root / "locks", argv=[])
        self.path = root / "documents" / "Litch Factory Test Results.csv"
        self.path.parent.mkdir()

    def records(self, batch_name: str = batch_name) -> FactoryRecords:
        return FactoryRecords(self.path, self.locks, batch_name=batch_name, source_commit=self.source_commit)

    def begin(self, records: FactoryRecords) -> str:
        return records.begin(self.label, self.serial, "1-2.3", self.firmware_sha, "1.0.0-0", station_role="verifier")

    def test_factory_csv_preserves_prior_attempt_and_rejects_both_binding_conflicts(self) -> None:
        with self.records() as records:
            first = self.begin(records)
            self.assertTrue(records.update(first, phase="flash_failed", result="fail", failure_text="write failed"))
            original = records.lookup(label=self.label)[0]

        with self.records() as records:
            self.assertEqual(records.lookup(usbserial="00:00:5E:00:53:E1"), (original,))
            previous_bytes = self.path.read_bytes()
            for label, serial in (("sample_0035", self.serial), (self.label, "00005e0053e2")):
                with self.subTest(label=label, serial=serial):
                    with self.assertRaisesRegex(FactoryRecordsError, "conflict"):
                        records.begin(label, serial, "1-1", self.firmware_sha, "1.0.0-0", station_role="dut")
            self.assertEqual(self.path.read_bytes(), previous_bytes)

            second = records.begin(self.label, "00-00-5E-00-53-E1", "1-1", self.firmware_sha, "1.0.0-0", station_role="verifier")
            prior, pending = records.lookup(label=self.label)
            self.assertEqual(prior, original)
            self.assertNotEqual(first, second)
            self.assertEqual((pending["attempt_id"], pending["qr_id"], pending["usb_location"]), (second, self.label, "1-1"))
            self.assertEqual(pending["source_commit"], self.source_commit)
            self.assertEqual((pending["result"], pending["ended_utc"]), ("pending", ""))
            self.assertTrue(prior["ended_utc"].endswith("Z"))
            with self.assertRaisesRegex(FactoryRecordsError, "completed"):
                records.update(first, phase="watching")

    def test_factory_csv_preserves_batch_number_and_station_role_per_row(self) -> None:
        with self.records() as records:
            self.begin(records)
            with self.assertRaisesRegex(FactoryRecordsError, "conflicts with another QR ID"):
                records.begin("other_0034", "00005e0053e2", "1-1", self.firmware_sha, "1.0.0-0", station_role="dut")
            with self.assertRaisesRegex(FactoryRecordsError, "four-digit board number"):
                records.begin("other_34", "00005e0053e2", "1-1", self.firmware_sha, "1.0.0-0", station_role="dut")
            records.begin("sample_0021", "00005e0053e2", "1-1", self.firmware_sha, "1.0.0-0", station_role="dut")

        with self.records("SERIES.2") as records:
            records.begin("other_0034", "00005e0053e3", "1-1", self.firmware_sha, "1.0.0-0", station_role="verifier")
            for label, batch, number, mode in (
                ("sample_0034", "SERIES.1", "0034", "verifier"),
                ("sample_0021", "SERIES.1", "0021", "dut"),
                ("other_0034", "SERIES.2", "0034", "verifier"),
            ):
                row = records.lookup(label=label)[0]
                self.assertEqual((row["qr_id"], row["batch_name"], row["board_number"], row["station_role"]),
                                 (label, batch, number, mode))

    def test_resume_uses_designated_verifier_and_latest_pass_without_changing_history(self) -> None:
        saved = FactoryStatus(
            active=False, stage="inactive", step="led_field", revision=3, radio_role="DUT/testing",
            provisioning_state="passed", saved_radio=3, save_pending=False, override_pending=False,
            durable_pass=True, override=False, current_failures=(), checks={}, observations=_SAVED_PROOF,
        )
        dut_label, dut_serial = "sample_0035", "00005e0053e2"
        with self.records() as records:
            verifier_id = self.begin(records)
            records.update(verifier_id, result="pass", status=saved)
            dut_id = records.begin(dut_label, dut_serial, "1-1", self.firmware_sha, "1.0.0-0", station_role="dut")
            records.update(dut_id, result="pass", status=saved)
            pending_id = records.begin("sample_0036", "00005e0053e3", "1-1", self.firmware_sha,
                                       "1.0.0-0", station_role="dut")
            records.update(pending_id, result="incomplete")
            before = self.path.read_bytes()
            resumed = records.resume()
            self.assertEqual(resumed.verifier["attempt_id"], verifier_id)
            self.assertEqual(resumed.verifier["radio_role"], "DUT/testing")
            self.assertEqual(resumed.verifier["verifier"], "0")
            self.assertEqual([row["attempt_id"] for row in resumed.passed], [verifier_id, dut_id])
            self.assertEqual(self.path.read_bytes(), before)

        # A later reflash in another batch invalidates the older skip authority.
        with self.records("SERIES.2") as records:
            retry = records.begin(dut_label, dut_serial, "2-1", self.firmware_sha, "1.0.0-1", station_role="dut")
            records.update(retry, result="incomplete")
        with self.records() as records:
            before = self.path.read_bytes()
            self.assertEqual([row["attempt_id"] for row in records.resume().passed], [verifier_id])
            self.assertEqual(self.path.read_bytes(), before)

            superseding = self.begin(records)
            records.update(superseding, result="fail", failure_text="write failed")
            before = self.path.read_bytes()
            with self.assertRaisesRegex(FactoryRecordsError, "no current PASS"):
                records.resume()
            self.assertEqual(self.path.read_bytes(), before)

    def test_resume_requires_current_pass_for_latest_designated_verifier(self) -> None:
        saved = FactoryStatus(
            active=False, stage="inactive", step="led_field", revision=3, radio_role="DUT/testing",
            provisioning_state="passed", saved_radio=3, save_pending=False, override_pending=False,
            durable_pass=True, override=False, current_failures=(), checks={}, observations=_SAVED_PROOF,
        )
        with self.records() as records:
            with self.assertRaisesRegex(FactoryRecordsError, "no recorded verifier"):
                records.resume()
            self.assertFalse(self.path.exists())
            old = self.begin(records)
            records.update(old, result="pass", status=saved)
            current = records.begin("sample_0035", "00005e0053e2", "1-1", self.firmware_sha,
                                    "1.0.0-0", station_role="verifier")
            with self.assertRaisesRegex(FactoryRecordsError, "no current PASS"):
                records.resume()
            records.update(current, result="pass", status=saved)
            self.assertEqual(records.resume().verifier["attempt_id"], current)

        with self.records("SERIES.2") as records:
            retry = records.begin("sample_0035", "00005e0053e2", "2-1", self.firmware_sha,
                                  "1.0.0-1", station_role="dut")
            records.update(retry, result="incomplete")
        with self.records() as records:
            before = self.path.read_bytes()
            with self.assertRaisesRegex(FactoryRecordsError, "no current PASS"):
                records.resume()
            self.assertEqual(self.path.read_bytes(), before)

    def test_factory_csv_keeps_live_checks_across_verified_normal_boot(self) -> None:
        live = FactoryStatus(
            active=True, stage="local", step="hall_sweep", revision=3, radio_role="DUT/testing",
            provisioning_state="pending", saved_radio=None, save_pending=False, override_pending=False,
            durable_pass=False, override=False, current_failures=("Board I/O failed",),
            checks={"board_io": "pending", "light_1": "pass", "light_2": "pass", "motion_event": "pending", "radio_ble": "pending"},
            observations={"active": "1", "stage": "local", "buttons": "12/12", "button_mask": "0x37ef/0x37ef",
                          "halls_active": "58", "sensor_1_raw_lux": "35", "sensor_1_room": "44",
                          "sensor_1_covered": "9", "sensor_2_raw_lux": "20",
                          "manual_ambient_response": "required", "ambient_observed_mask": "0x03"},
        )
        saved = replace(
            live, active=False, stage="inactive", provisioning_state="passed", saved_radio=3,
            durable_pass=True, current_failures=(), checks={"board_io": "n/a"},
            observations={**_SAVED_PROOF, "buttons": "0/0", "halls_active": "0"},
        )
        with self.records() as records:
            attempt_id = self.begin(records)
            self.assertTrue(records.update(attempt_id, phase="watching", status=live))
            previous_bytes = self.path.read_bytes()
            self.assertFalse(records.update(attempt_id, phase="watching", status=live))
            jitter = replace(live, observations={**live.observations, "sensor_1_raw_lux": "36"})
            self.assertFalse(records.update(attempt_id, status=jitter))
            with self.assertRaisesRegex(FactoryRecordsError, "verified saved result"):
                records.update(attempt_id, result="pass", status=live)
            with self.assertRaisesRegex(FactoryRecordsError, "saved Override result"):
                records.update(attempt_id, result="override", status=live)
            self.assertEqual(self.path.read_bytes(), previous_bytes)

            records.update(attempt_id, phase="complete", result="pass", status=saved, notes="=formula()\r\nsecond line")
            row = records.lookup(label=self.label)[0]
            self.assertEqual((row["result"], row["saved_radio"], row["operator_ms"], row["active"]), ("pass", "3", "15000", "0"))
            self.assertEqual((row["observed_halls_active"], row["observed_buttons"], row["observed_button_mask"]),
                             ("58", "12/12", "0x37ef/0x37ef"))
            self.assertEqual((row["observed_light_1"], row["observed_sensor_1_covered"], row["observed_motion_event"]),
                             ("pass", "9", "pending"))
            self.assertEqual((row["observed_manual_ambient_response"], row["observed_ambient_observed_mask"], row["observed_light_2"]),
                             ("required", "0x03", "pass"))
            self.assertEqual(row["observed_sensor_1_raw_lux"], "35")
            self.assertEqual(row["observed_radio_ble"], "pending")
            self.assertEqual(row["qualification_source"], "verified_saved_record")
            self.assertEqual((row["qualified_radio_ble"], row["qualified_motion_link"], row["qualified_als_links"]),
                             ("pass", "pass", "pass"))
            self.assertEqual(row["observed_failures"], "Board I/O failed")
            self.assertEqual(row["notes"], "'=formula() second line")

    def test_factory_csv_records_retained_verifier_progress_and_rejects_edited_pass(self) -> None:
        retained = FactoryStatus(
            active=False, stage="passed", step="pass_saved", revision=2, radio_role="VERIFIER",
            provisioning_state="passed", saved_radio=3, save_pending=False, override_pending=False,
            durable_pass=True, override=False, current_failures=(), checks={"radio_ble": "pass", "motion_event": "n/a"},
            observations={**_SAVED_PROOF, "stage": "passed", "step": "pass_saved", "verifier": "1",
                          "radio_proof": "verified", "radio_mask": "0x01", "halls_active": "64"},
        )
        with self.records() as records:
            attempt_id = self.begin(records)
            records.update(attempt_id, result="pass", status=retained)
            row = records.lookup(label=self.label)[0]
            self.assertEqual((row["observed_halls_active"], row["observed_radio_ble"]), ("64", "pass"))
            self.assertEqual((row["qualified_motion_link"], row["qualified_radio_ble"]), ("n/a", "pass"))

        for field, value in (("saved_record", "none"), ("saved_radio", "2"), ("verifier", "0"), ("qualified_radio_ble", "pending")):
            with self.subTest(field=field):
                with self.path.open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=tuple(row))
                    writer.writeheader()
                    writer.writerow({**row, field: value})
                with self.assertRaisesRegex(FactoryRecordsError, "inconsistent"):
                    with self.records():
                        self.fail("edited PASS entered the factory session")

    def test_factory_csv_rejects_edited_override_and_keeps_its_qualification_blank(self) -> None:
        override = FactoryStatus(
            active=False, stage="inactive", step="led_field", revision=4, radio_role="DUT/testing",
            provisioning_state="override", saved_radio=5, save_pending=False, override_pending=False,
            durable_pass=False, override=True, current_failures=(), checks={},
            observations={**_SAVED_PROOF, "provisioning_state": "override", "completed_result": "blocked",
                          "saved_radio": "5"},
        )
        with self.records() as records:
            attempt_id = self.begin(records)
            records.update(attempt_id, result="override", status=override)
            row = records.lookup(label=self.label)[0]
            self.assertEqual((row["result"], row["qualified_radio_ble"], row["qualification_source"]),
                             ("override", "", ""))
        with self.path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=tuple(row))
            writer.writeheader()
            writer.writerow({**row, "override_pending": "1"})
        with self.assertRaisesRegex(FactoryRecordsError, "inconsistent saved evidence"):
            with self.records():
                self.fail("edited Override entered the factory session")

    def test_factory_csv_preflights_schema_and_excludes_a_second_writer(self) -> None:
        self.path.write_text("wrong,header\n", encoding="utf-8")
        with self.assertRaisesRegex(FactoryRecordsError, "header"):
            with self.records():
                self.fail("invalid CSV entered the factory session")
        self.assertEqual(self.path.read_bytes(), b"wrong,header\n")
        self.path.unlink()

        with self.records() as records:
            self.begin(records)
            clone = self.locks.workspace.root / "another-checkout"
            other_workspace = SimpleNamespace(root=clone, common_git_dir=clone / ".git", branch="main", head=self.source_commit)
            other_locks = LockManager(other_workspace, state_root=self.locks.state_root, argv=[])
            with self.assertRaises(LockTimeoutError):
                with FactoryRecords(self.path, other_locks, batch_name=self.batch_name):
                    self.fail("writer from another repository clone entered the factory session")
        before = self.path.read_bytes()
        with mock.patch.object(factory_records.tempfile, "TemporaryFile", side_effect=PermissionError("read-only")):
            with self.assertRaisesRegex(FactoryRecordsError, "Cannot write factory CSV"):
                with self.records():
                    self.fail("unwritable CSV entered the factory session")
        self.assertEqual(self.path.read_bytes(), before)

    def test_factory_csv_failed_replace_leaves_previous_attempts_intact(self) -> None:
        with self.records() as records:
            attempt_id = self.begin(records)
            before = self.path.read_bytes()
            with mock.patch.object(factory_records.os, "replace", side_effect=OSError("disk error")):
                with self.assertRaisesRegex(FactoryRecordsError, "Cannot save factory CSV"):
                    records.update(attempt_id, phase="disconnected", result="incomplete")
            self.assertEqual(self.path.read_bytes(), before)
            self.assertEqual(list(self.path.parent.iterdir()), [self.path])
            self.assertEqual(records.lookup(label=self.label)[0]["result"], "pending")
            records.update(attempt_id, phase="disconnected", result="incomplete")
            self.assertEqual(records.lookup(label=self.label)[0]["result"], "incomplete")
