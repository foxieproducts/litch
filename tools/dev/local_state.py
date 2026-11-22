"""Small, host-local preferences for successful developer device operations."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path


SCHEMA = "litch.dev.local-state.v1"
SLOTS = (
    "c3_firmware",
    "s3_firmware",
    "ios_flutter",
    "android_flutter",
    "last_flash",
    "last_build",
    "last_app_build",
    "usb_choices",
)
MAX_BYTES = 8192
MAX_USB_CHOICES = 16


def read(path: Path) -> dict[str, object] | None:
    if not path.exists():
        return {slot: None for slot in SLOTS}
    try:
        raw = path.read_text(encoding="utf-8")
        if len(raw) > MAX_BYTES:
            return None
        value = json.loads(raw)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        return None
    return {slot: value.get(slot) for slot in SLOTS}


def remember(path: Path, slot: str, value: object) -> None:
    """Atomically change one successful-operation slot without clearing others."""

    if slot not in SLOTS:
        raise ValueError(f"unknown local-state slot {slot!r}")
    _write(path, {slot: value})


def remember_many(path: Path, values: dict[str, object]) -> None:
    if not values or any(slot not in SLOTS for slot in values):
        raise ValueError("unknown or empty local-state update")
    _write(path, values)


def _write(path: Path, values: dict[str, object]) -> None:
    state = read(path)
    if state is None:
        state = _recognized_slots(path)
    state.update(values)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent, text=True
    )
    os.close(temporary_fd)
    temporary = Path(temporary_name)
    try:
        temporary.write_text(
            json.dumps({"schema": SCHEMA, **state}, separators=(",", ":")),
            encoding="utf-8",
        )
        os.replace(temporary, path)
    except Exception:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def _recognized_slots(path: Path) -> dict[str, object]:
    """Best-effort recovery for this ignored, non-authoritative convenience file."""

    try:
        raw = path.read_text(encoding="utf-8")
        value = json.loads(raw) if len(raw) <= MAX_BYTES else None
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        value = None
    if not isinstance(value, dict):
        return {slot: None for slot in SLOTS}
    return {slot: value.get(slot) for slot in SLOTS}


def usb_choices(path: Path) -> dict[str, dict[str, object]]:
    """Read bounded stable-serial hints; never treat them as device authority."""

    from .devices import normalize_usb_serial

    state = read(path)
    rows = None if state is None else state.get("usb_choices")
    if not isinstance(rows, list) or len(rows) > MAX_USB_CHOICES:
        return {}
    choices = {}
    for row in rows:
        if not isinstance(row, dict) or row.get("target") not in {"s3", "c3"}:
            continue
        serial = row.get("serial")
        if not isinstance(serial, str) or len(serial) > 256:
            continue
        try:
            serial = normalize_usb_serial(serial)
        except ValueError:
            continue
        if serial in choices:
            return {}  # Ambiguous saved identity is never a selection.
        choices[serial] = {"target": row["target"], "selected": row.get("selected") is True}
    return choices


def remember_usb_choices(path: Path, choices: dict[str, dict[str, object]]) -> None:
    if len(choices) > MAX_USB_CHOICES:
        raise ValueError(f"at most {MAX_USB_CHOICES} USB choices can be remembered")
    rows = [{"serial": serial, **choice} for serial, choice in choices.items()]
    remember(path, "usb_choices", rows)
