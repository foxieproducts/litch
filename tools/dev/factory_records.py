"""One durable CSV row for each factory attempt on a labeled board."""

from __future__ import annotations

import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import re
import stat
import tempfile
from types import TracebackType
from uuid import uuid4

from .devices import normalize_usb_serial
from .factory_status import FactoryStatus, saved_factory_result
from .locks import LockManager
from .runner import DevError


class FactoryRecordsError(DevError):
    """The factory CSV cannot safely record this operation."""


@dataclass(frozen=True)
class FactoryResume:
    """Recorded identities to verify live or skip without another factory erase."""

    verifier: dict[str, str]
    passed: tuple[dict[str, str], ...]


_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")
_BATCH_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_. -]{0,127}")
_BOARD_NUMBER = re.compile(r"[0-9]{4}\Z")
_LOCATION = re.compile(r"[a-z0-9][a-z0-9_.:/-]{0,127}")
_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,127}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_COMMIT = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
_ATTEMPT = re.compile(r"[0-9a-f]{32}")
_PHASE = re.compile(r"[a-z0-9][a-z0-9_-]{0,63}")
_RESULTS = {"pending", "pass", "fail", "override", "incomplete"}
_STATION_ROLES = {"verifier", "dut"}
_STATUS_COLUMNS = (
    "active", "stage", "step", "provisioning_state", "completed_result",
    "provisioning_next", "save_pending", "override_pending", "saved_record",
    "saved_radio", "verifier", "auto_ms", "operator_ms", "radio_ms", "radio_mask", "radio_proof",
)
_LIVE_COLUMNS = (
    "channel", "buttons", "halls_clear", "halls_active", "button_mask", "hall_clear",
    "hall_active", "responding_expanders", "expander_links_known", "motion_present",
    "motion_presence_known", "forced_faults", "current_failed_checks", "current_hall_read",
    "current_hall_clear", "sensor_1_raw_lux", "sensor_1_room", "sensor_1_covered",
    "sensor_1_recovered", "sensor_2_raw_lux", "sensor_2_room", "sensor_2_covered",
    "sensor_2_recovered", "auto_mask", "auto_under_10s", "motion_event_observed",
    "leds_confirmed", "radio_mask", "radio_proof", "peer_candidate", "discovery_deadline_ms",
    "ble_endpoint_error", "retained_current", "retained_previous", "retained_previous_valid",
    "reset_reason", "manual_ambient_response", "ambient_observed_mask", "manual_motion",
    "manual_motion_observed",
)
_CHECK_COLUMNS = (
    "led_scan", "board_io", "light_1", "light_2", "motion_link", "motion_event", "led_review", "radio_ble",
)
_RAW_LUX_COLUMNS = {"observed_sensor_1_raw_lux", "observed_sensor_2_raw_lux"}
_QUALIFIED_COLUMNS = (
    "qualified_led_review", "qualified_buttons", "qualified_hall_sweep", "qualified_board_io",
    "qualified_als_links", "qualified_motion_link", "qualified_automatic_within_10s", "qualified_radio_ble",
)
_COLUMNS = (
    "started_utc", "updated_utc", "ended_utc", "attempt_id", "batch_name", "board_number",
    "qr_id", "station_role", "usb_serial",
    "usb_location", "firmware_version", "firmware_sha256", "source_commit", "revision",
    "phase", "result", "failure_text", "observed_failures", "notes", "radio_role",
    "qualification_source", *_QUALIFIED_COLUMNS, *_STATUS_COLUMNS,
    *(f"observed_{key}" for key in (*_LIVE_COLUMNS, *_CHECK_COLUMNS)),
)


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _required(pattern: re.Pattern[str], value: str, field: str) -> str:
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise FactoryRecordsError(f"Invalid factory CSV {field}")
    return value


def _serial(value: str) -> str:
    try:
        return _required(_IDENTIFIER, normalize_usb_serial(value), "USB serial")
    except (TypeError, ValueError) as error:
        raise FactoryRecordsError("Invalid factory CSV USB serial") from error


def _board_number(label: str) -> str:
    _required(_IDENTIFIER, label, "board QR ID")
    number = _BOARD_NUMBER.search(label)
    if number is None:
        raise FactoryRecordsError("Board QR ID must end in a four-digit board number")
    return number.group()


def _human_text(value: str) -> str:
    # Keep operator messages readable and prevent spreadsheets running cell formulas.
    line = " ".join(value.splitlines()).replace("\t", " ").strip()
    return "'" + line if line.startswith(("=", "+", "-", "@")) else line


def _qualified_checks(revision: str) -> dict[str, str]:
    # ManufacturingTestSession::IsLocalComplete and verified radio persistence
    # cover these requirements even on older firmware. ALS response and optional
    # motion evidence stay in observed_*; do not infer them from an older PASS.
    checks = dict.fromkeys(_QUALIFIED_COLUMNS, "pass")
    if revision == "2":
        checks["qualified_motion_link"] = "n/a"
    return checks


def _validate_outcome(row: dict[str, str]) -> None:
    result = row["result"]
    if result in {"pass", "override"} and saved_factory_result(row) != result:
        raise FactoryRecordsError(f"Factory CSV {result} has inconsistent saved evidence")
    if result == "pass":
        if (row["qualification_source"] != "verified_saved_record"
                or any(row[key] != value for key, value in _qualified_checks(row["revision"]).items())):
            raise FactoryRecordsError("Factory CSV PASS has inconsistent required checks")
    elif row["qualification_source"] or any(row[key] for key in _QUALIFIED_COLUMNS):
        raise FactoryRecordsError("Factory CSV has required check results without a PASS")


class FactoryRecords:
    """Lock one result path before any board work, and persist each accepted change."""

    def __init__(self, path: Path, locks: LockManager, *, batch_name: str, source_commit: str = "") -> None:
        path = Path(path).expanduser()
        self.path = path.parent.resolve() / path.name
        self.batch_name = _required(_BATCH_NAME, batch_name, "batch name")
        self.source_commit = _required(_COMMIT, source_commit, "source commit") if source_commit else ""
        path_digest = hashlib.sha256(os.fsencode(self.path)).hexdigest()
        # The device lock namespace is per user across clones. Reserve a key
        # that cannot be a chip serial so any checkout writing this path agrees.
        self._lock = locks.device(f"factory-results:{path_digest}", timeout=0)
        self._opened = False

    def __enter__(self) -> FactoryRecords:
        self._lock.acquire()
        try:
            self._read()
            # The destination directory must permit the same atomic replacement
            # used by begin(), even if an existing CSV can be opened for reading.
            with tempfile.TemporaryFile(dir=self.path.parent):
                pass
        except BaseException as error:
            self._lock.release()
            if isinstance(error, OSError):
                raise FactoryRecordsError(f"Cannot write factory CSV {self.path}: {error}") from error
            raise
        self._opened = True
        return self

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self._opened = False
        self._lock.release()

    def begin(self, label: str, usbserial: str, location: str, fwsha: str, fwversion: str, *, station_role: str) -> str:
        """Record the QR binding and pending attempt before the first flash write."""
        board_number = _board_number(label)
        serial = _serial(usbserial)
        location = _required(_LOCATION, location, "USB location")
        fwsha = _required(_SHA256, fwsha, "firmware SHA-256")
        fwversion = _required(_VERSION, fwversion, "firmware version")
        if station_role not in _STATION_ROLES:
            raise FactoryRecordsError("Invalid factory CSV station role")
        rows = self._read_open()
        for row in rows:
            if (row["qr_id"] == label) != (row["usb_serial"] == serial):
                raise FactoryRecordsError(
                    f"Board label {label!r} and USB serial {serial} conflict with an existing factory CSV binding"
                )
            if (row["batch_name"], row["board_number"]) == (self.batch_name, board_number) and row["qr_id"] != label:
                raise FactoryRecordsError(
                    f"Board number {board_number} conflicts with another QR ID in batch {self.batch_name!r}"
                )
        attempt_id = uuid4().hex
        now = _utc_now()
        row = dict.fromkeys(_COLUMNS, "")
        row.update(
            started_utc=now, updated_utc=now, attempt_id=attempt_id, batch_name=self.batch_name,
            board_number=board_number, qr_id=label, station_role=station_role,
            usb_serial=serial, usb_location=location, firmware_version=fwversion,
            firmware_sha256=fwsha, source_commit=self.source_commit, phase="admitted", result="pending",
        )
        self._write([*rows, row])
        return attempt_id

    def lookup(self, *, label: str | None = None, usbserial: str | None = None) -> tuple[dict[str, str], ...]:
        """Return earlier attempts for an exact label or normalized USB serial."""
        if label is None and usbserial is None:
            raise FactoryRecordsError("Factory CSV lookup requires a board label or USB serial")
        serial = _serial(usbserial) if usbserial is not None else None
        return tuple(
            row for row in self._read_open()
            if (label is None or row["qr_id"] == label) and (serial is None or row["usb_serial"] == serial)
        )

    def resume(self) -> FactoryResume:
        """Select the latest designated verifier and current PASS records, without writing."""
        rows = self._read_open()
        # begin() appends attempts. Any later attempt, even in another batch,
        # supersedes an older PASS because it may have erased that board.
        latest = {row["usb_serial"]: row for row in rows}
        verifier = next((row for row in reversed(rows)
                         if row["batch_name"] == self.batch_name and row["station_role"] == "verifier"), None)
        if verifier is None:
            raise FactoryRecordsError(f"Batch {self.batch_name!r} has no recorded verifier; start without --resume")
        if verifier["result"] != "pass" or latest[verifier["usb_serial"]]["attempt_id"] != verifier["attempt_id"]:
            raise FactoryRecordsError(
                f"Batch {self.batch_name!r} verifier {verifier['qr_id']} has no current PASS; "
                "resume cannot reuse an unqualified or subsequently reflashed board"
            )
        passed = tuple(row for row in rows
                       if row["batch_name"] == self.batch_name and row["result"] == "pass"
                       and latest[row["usb_serial"]]["attempt_id"] == row["attempt_id"])
        return FactoryResume(verifier=verifier, passed=passed)

    def update(
        self,
        attempt_id: str,
        *,
        phase: str | None = None,
        result: str | None = None,
        failure_text: str | None = None,
        status: FactoryStatus | None = None,
        notes: str | None = None,
    ) -> bool:
        """Persist meaningful changes. The caller verifies the status attempt and USB identity."""
        if result is not None and result not in _RESULTS:
            raise FactoryRecordsError("Invalid factory CSV result")
        if result == "pass" and (status is None or not status.durable_pass):
            raise FactoryRecordsError("Factory PASS requires a complete verified saved result")
        if result == "override" and (status is None or not status.override):
            raise FactoryRecordsError("Factory Override requires a complete saved Override result")
        rows = self._read_open()
        for index, row in enumerate(rows):
            if row["attempt_id"] == attempt_id:
                break
        else:
            raise FactoryRecordsError("Unknown factory CSV attempt ID")
        if row["result"] != "pending":
            raise FactoryRecordsError("A completed factory CSV attempt cannot be changed")

        updated = row.copy()
        if phase is not None:
            updated["phase"] = _required(_PHASE, phase, "phase")
        if result is not None:
            updated["result"] = result
        if failure_text is not None:
            updated["failure_text"] = _human_text(failure_text)
        if notes is not None:
            updated["notes"] = _human_text(notes)
        if status is not None:
            updated.update({key: status.observations.get(key, "") for key in _STATUS_COLUMNS})
            updated["radio_role"] = status.radio_role
            if status.revision:
                updated["revision"] = str(status.revision)
            # The freshly saved verifier retains its run's progress after Stop.
            # A rebooted saved verifier has no live radio mask or observations.
            retained_run = (status.durable_pass
                            and (status.stage, status.step, status.radio_role) == ("passed", "pass_saved", "VERIFIER")
                            and status.observations.get("radio_proof") == "verified"
                            and status.observations.get("radio_mask") == "0x01")
            if status.active or retained_run:
                updated.update({f"observed_{key}": status.observations.get(key, "") for key in _LIVE_COLUMNS})
                updated.update({f"observed_{key}": status.checks.get(key, "") for key in _CHECK_COLUMNS})
            if status.current_failures:
                prior = updated["observed_failures"].split(" | ") if updated["observed_failures"] else []
                updated["observed_failures"] = " | ".join(dict.fromkeys((*prior, *status.current_failures)))
        if result == "pass":
            updated["qualification_source"] = "verified_saved_record"
            updated.update(_qualified_checks(updated["revision"]))
        _validate_outcome(updated)
        # A new raw ALS reading alone is sampling noise. Retain readings at
        # progress/status boundaries alongside the latched room/cover samples.
        if not any(updated[key] != row[key] for key in _COLUMNS if key not in _RAW_LUX_COLUMNS):
            return False
        updated["updated_utc"] = _utc_now()
        if updated["result"] != "pending":
            updated["ended_utc"] = updated["updated_utc"]
        rows[index] = updated
        self._write(rows)
        return True

    def _read_open(self) -> list[dict[str, str]]:
        if not self._opened:
            raise FactoryRecordsError("Open the factory CSV before recording attempts")
        return self._read()

    def _read(self) -> list[dict[str, str]]:
        try:
            try:
                metadata = self.path.lstat()
            except FileNotFoundError:
                return []
            if not stat.S_ISREG(metadata.st_mode):
                raise FactoryRecordsError(f"Factory CSV path must be a regular file: {self.path}")
            with self.path.open("r+", encoding="utf-8", newline="") as stream:
                reader = csv.DictReader(stream, strict=True)
                if tuple(reader.fieldnames or ()) != _COLUMNS:
                    raise FactoryRecordsError(f"Factory CSV header does not match the expected columns: {self.path}")
                rows = list(reader)
            _validate_rows(rows)
            return rows
        except (OSError, UnicodeError, csv.Error) as error:
            raise FactoryRecordsError(f"Cannot read factory CSV {self.path}: {error}") from error

    def _write(self, rows: list[dict[str, str]]) -> None:
        temporary: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", newline="", prefix=f".{self.path.name}.",
                dir=self.path.parent, delete=False,
            ) as stream:
                temporary = Path(stream.name)
                writer = csv.DictWriter(stream, fieldnames=_COLUMNS)
                writer.writeheader()
                writer.writerows(rows)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        except (OSError, UnicodeError, csv.Error) as error:
            raise FactoryRecordsError(f"Cannot save factory CSV {self.path}: {error}") from error
        finally:
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass


def _validate_rows(rows: list[dict[str, str]]) -> None:
    attempts: set[str] = set()
    bindings: dict[str, str] = {}
    serial_labels: dict[str, str] = {}
    batch_labels: dict[tuple[str, str], str] = {}
    for row in rows:
        if None in row or any(value is None for value in row.values()):
            raise FactoryRecordsError("Factory CSV has an incomplete or extra column")
        label = row["qr_id"]
        batch_name = _required(_BATCH_NAME, row["batch_name"], "batch name")
        board_number = _board_number(label)
        if row["board_number"] != board_number or row["station_role"] not in _STATION_ROLES:
            raise FactoryRecordsError("Factory CSV has an invalid board number or station role")
        serial = _serial(row["usb_serial"])
        attempt = _required(_ATTEMPT, row["attempt_id"], "attempt ID")
        _required(_LOCATION, row["usb_location"], "USB location")
        _required(_SHA256, row["firmware_sha256"], "firmware SHA-256")
        _required(_VERSION, row["firmware_version"], "firmware version")
        _required(_PHASE, row["phase"], "phase")
        if row["source_commit"]:
            _required(_COMMIT, row["source_commit"], "source commit")
        if (serial != row["usb_serial"] or row["result"] not in _RESULTS or attempt in attempts
                or (row["result"] == "pending") == bool(row["ended_utc"])):
            raise FactoryRecordsError("Factory CSV has an invalid attempt or result")
        _validate_outcome(row)
        for timestamp in (row["started_utc"], row["updated_utc"], row["ended_utc"] or row["updated_utc"]):
            try:
                if not timestamp.endswith("Z"):
                    raise ValueError("expected UTC")
                datetime.fromisoformat(timestamp[:-1] + "+00:00")
            except ValueError as error:
                raise FactoryRecordsError("Factory CSV has an invalid UTC timestamp") from error
        if bindings.setdefault(label, serial) != serial or serial_labels.setdefault(serial, label) != label:
            raise FactoryRecordsError("Factory CSV has conflicting board label and USB serial bindings")
        if batch_labels.setdefault((batch_name, board_number), label) != label:
            raise FactoryRecordsError("Factory CSV has conflicting QR IDs for a batch board number")
        attempts.add(attempt)
