"""Exact USB-device discovery for hardware-facing developer commands.

The helpers in this module deliberately model the physical USB parent rather
than a volatile ``/dev/cu.*`` child.  Callers must supply both the serial and
location captured for the selected board; there is no "only device" fallback.
"""

from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass
from typing import Callable, Protocol, Sequence


ESPRESSIF_VID = 0x303A
ESP32S3_ROM_PID = 0x1001
LITCH_APP_PID = 0x4001

_COMPACT_MAC_SERIAL_RE = re.compile(r"^[0-9A-Fa-f]{12}$")
_COLON_MAC_SERIAL_RE = re.compile(r"^(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")
_HYPHEN_MAC_SERIAL_RE = re.compile(r"^(?:[0-9A-Fa-f]{2}-){5}[0-9A-Fa-f]{2}$")


class DeviceInventoryError(RuntimeError):
    """Raised when USB identity cannot be established without guessing."""


class UsbTransitionTimeout(DeviceInventoryError):
    """The selected parent never reached the requested USB mode."""


def normalize_usb_serial(serial: str) -> str:
    """Normalize one USB serial, including ESP32 MAC-format aliases."""

    if not isinstance(serial, str):
        raise TypeError("USB serial must be a string")
    normalized = serial.strip().casefold()
    if not normalized:
        raise ValueError("USB serial must not be empty")
    if any(not character.isprintable() for character in normalized):
        raise ValueError("USB serial must contain printable characters only")
    if (
        _COMPACT_MAC_SERIAL_RE.fullmatch(normalized)
        or _COLON_MAC_SERIAL_RE.fullmatch(normalized)
        or _HYPHEN_MAC_SERIAL_RE.fullmatch(normalized)
    ):
        return normalized.replace(":", "").replace("-", "")
    return normalized


class CommandResultLike(Protocol):
    returncode: int
    stdout: str
    stderr: str


class Runner(Protocol):
    def run(
        self,
        argv: Sequence[str],
        *,
        timeout_seconds: float,
    ) -> CommandResultLike: ...


@dataclass(frozen=True, order=True)
class PhysicalUsbIdentity:
    """Stable identity used across ROM/application USB re-enumeration."""

    serial: str
    location: str

    def __post_init__(self) -> None:
        serial = normalize_usb_serial(self.serial)
        location = self.location.strip().casefold()
        if not serial or not location:
            raise DeviceInventoryError(
                "exact USB identity requires both serial number and physical location"
            )
        object.__setattr__(self, "serial", serial)
        object.__setattr__(self, "location", location)


@dataclass(frozen=True)
class UsbEndpoint:
    port: str
    vid: int | None
    pid: int | None
    serial: str | None
    location: str | None
    description: str = ""
    hwid: str = ""


@dataclass(frozen=True)
class UsbParent:
    """One physical USB device with any tty/cu interface aliases collapsed."""

    identity: PhysicalUsbIdentity
    vid: int
    pid: int
    ports: tuple[str, ...]

    @property
    def port(self) -> str:
        """Preferred serial child; macOS ``cu`` is preferred over ``tty``."""

        if not self.ports:
            raise DeviceInventoryError("USB parent has no serial child")
        return self.ports[0]


_HWID_VID_PID_RE = re.compile(
    r"\bVID:PID=(?P<vid>[0-9A-Fa-f]{4}):(?P<pid>[0-9A-Fa-f]{4})\b"
)
_HWID_SERIAL_RE = re.compile(r"(?:^|\s)SER=(?P<value>\S+)", re.IGNORECASE)
_HWID_LOCATION_RE = re.compile(
    r"(?:^|\s)LOCATION=(?P<value>\S+)", re.IGNORECASE
)


def _parse_usb_number(value: object) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value if 0 <= value <= 0xFFFF else None
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    try:
        if text.lower().startswith("0x") or re.fullmatch(r"[0-9A-Fa-f]{4}", text):
            number = int(text, 16)
        else:
            number = int(text, 10)
    except ValueError:
        return None
    return number if 0 <= number <= 0xFFFF else None


def _optional_text(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    stripped = value.strip()
    return stripped or None


def parse_pio_device_list(payload: str | bytes) -> tuple[UsbEndpoint, ...]:
    """Parse ``pio device list --json-output`` without consulting hardware."""

    if isinstance(payload, bytes):
        try:
            payload = payload.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise DeviceInventoryError("PlatformIO device JSON is not UTF-8") from exc
    try:
        decoded = json.loads(payload)
    except (TypeError, json.JSONDecodeError) as exc:
        raise DeviceInventoryError("PlatformIO device list returned invalid JSON") from exc

    if isinstance(decoded, dict) and isinstance(decoded.get("devices"), list):
        decoded = decoded["devices"]
    if not isinstance(decoded, list):
        raise DeviceInventoryError("PlatformIO device JSON must be a list")

    endpoints: list[UsbEndpoint] = []
    seen_ports: set[str] = set()
    for index, raw in enumerate(decoded):
        if not isinstance(raw, dict):
            raise DeviceInventoryError(f"device record {index} is not an object")
        for field in ("port", "serial_number", "location", "description", "hwid"):
            value = raw.get(field)
            if isinstance(value, str) and any(not char.isprintable() for char in value):
                raise DeviceInventoryError(
                    f"device record {index} has invalid {field}: {value!r}"
                )
        port = _optional_text(raw.get("port"))
        if port is None:
            raise DeviceInventoryError(f"device record {index} has no port")
        if port in seen_ports:
            raise DeviceInventoryError(f"device list repeats port {port}")
        seen_ports.add(port)

        hwid = _optional_text(raw.get("hwid")) or ""
        vid = _parse_usb_number(raw.get("vid"))
        pid = _parse_usb_number(raw.get("pid"))
        hwid_match = _HWID_VID_PID_RE.search(hwid)
        if hwid_match is not None:
            hwid_vid = int(hwid_match.group("vid"), 16)
            hwid_pid = int(hwid_match.group("pid"), 16)
            if vid is not None and vid != hwid_vid:
                raise DeviceInventoryError(f"VID disagreement for {port}")
            if pid is not None and pid != hwid_pid:
                raise DeviceInventoryError(f"PID disagreement for {port}")
            vid = hwid_vid
            pid = hwid_pid

        serial = _optional_text(raw.get("serial_number"))
        serial_match = _HWID_SERIAL_RE.search(hwid)
        if serial_match is not None:
            hwid_serial = serial_match.group("value")
            if serial is not None and serial.casefold() != hwid_serial.casefold():
                raise DeviceInventoryError(f"serial disagreement for {port}")
            serial = hwid_serial

        location = _optional_text(raw.get("location"))
        location_match = _HWID_LOCATION_RE.search(hwid)
        if location_match is not None:
            hwid_location = location_match.group("value")
            if location is not None and location.casefold() != hwid_location.casefold():
                raise DeviceInventoryError(f"location disagreement for {port}")
            location = hwid_location

        endpoints.append(
            UsbEndpoint(
                port=port,
                vid=vid,
                pid=pid,
                serial=serial,
                location=location,
                description=_optional_text(raw.get("description")) or "",
                hwid=hwid,
            )
        )
    return tuple(endpoints)


def _port_sort_key(port: str) -> tuple[int, str]:
    if port.startswith("/dev/cu."):
        rank = 0
    elif port.startswith("/dev/tty."):
        rank = 1
    else:
        rank = 2
    return rank, port


def normalize_usb_parents(
    endpoints: Sequence[UsbEndpoint],
    *,
    strict_vid: int = ESPRESSIF_VID,
) -> tuple[UsbParent, ...]:
    """Collapse tty/cu aliases and reject incomplete target identities."""

    grouped: dict[tuple[PhysicalUsbIdentity, int, int], set[str]] = {}
    for endpoint in endpoints:
        if endpoint.vid is None or endpoint.pid is None:
            continue
        if endpoint.serial is None or endpoint.location is None:
            if endpoint.vid == strict_vid:
                raise DeviceInventoryError(
                    f"Espressif endpoint {endpoint.port} lacks serial or location"
                )
            continue
        identity = PhysicalUsbIdentity(endpoint.serial, endpoint.location)
        key = identity, endpoint.vid, endpoint.pid
        grouped.setdefault(key, set()).add(endpoint.port)

    parents = [
        UsbParent(
            identity=identity,
            vid=vid,
            pid=pid,
            ports=tuple(sorted(ports, key=_port_sort_key)),
        )
        for (identity, vid, pid), ports in grouped.items()
    ]
    parents.sort(
        key=lambda item: (
            item.identity.location,
            item.identity.serial,
            item.vid,
            item.pid,
        )
    )
    return tuple(parents)


def select_exact_parent(
    parents: Sequence[UsbParent],
    *,
    identity: PhysicalUsbIdentity,
    vid: int,
    pid: int,
) -> UsbParent:
    """Select only an exact serial+location+VID+PID match."""

    matches = [
        parent
        for parent in parents
        if parent.identity == identity and parent.vid == vid and parent.pid == pid
    ]
    if not matches:
        raise DeviceInventoryError(
            "no USB parent matched the exact serial, location, VID, and PID"
        )
    if len(matches) != 1:
        raise DeviceInventoryError("exact USB identity is ambiguous")
    return matches[0]


class UsbInventory(Protocol):
    def snapshot(self) -> Sequence[UsbParent]: ...


class PioUsbInventory:
    """Runtime inventory adapter using a caller-provided command runner."""

    def __init__(self, runner: Runner, *, timeout_seconds: float = 5.0) -> None:
        if timeout_seconds <= 0:
            raise ValueError("device inventory timeout must be positive")
        self._runner = runner
        self._timeout_seconds = timeout_seconds

    def endpoints(self) -> tuple[UsbEndpoint, ...]:
        """Read informational endpoints before exact-parent admission."""

        try:
            result = self._runner.run(
                ("pio", "device", "list", "--json-output"),
                timeout_seconds=self._timeout_seconds,
            )
        except (OSError, TimeoutError) as exc:
            raise DeviceInventoryError(
                f"PlatformIO device inventory could not complete: {exc}"
            ) from exc
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise DeviceInventoryError(
                f"PlatformIO device inventory failed: {detail or 'no diagnostic'}"
            )
        return parse_pio_device_list(result.stdout)

    def snapshot(self) -> tuple[UsbParent, ...]:
        return normalize_usb_parents(self.endpoints())


def _parents_at_location(
    parents: Sequence[UsbParent], location: str
) -> list[UsbParent]:
    return [parent for parent in parents if parent.identity.location == location]


def capture_exact_usb_parent(
    inventory: UsbInventory,
    *,
    identity: PhysicalUsbIdentity,
    vid: int,
    pid: int,
) -> UsbParent:
    """Capture one exact physical parent for use across a later transition."""

    parents = tuple(inventory.snapshot())
    exact_identity = [parent for parent in parents if parent.identity == identity]
    if len(exact_identity) != 1:
        raise DeviceInventoryError(
            "selected serial and location must identify exactly one USB parent"
        )
    return select_exact_parent(
        parents,
        identity=identity,
        vid=vid,
        pid=pid,
    )


def wait_for_exact_usb_parent(
    inventory: UsbInventory,
    *,
    identity: PhysicalUsbIdentity,
    vid: int,
    pid: int,
    timeout_seconds: float = 15.0,
    poll_interval_seconds: float = 0.1,
    stable_reads: int = 2,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> UsbParent:
    """Wait boundedly for one exact parent to return with stable identity."""

    if timeout_seconds <= 0 or poll_interval_seconds <= 0:
        raise ValueError("USB parent waits must be positive")
    if stable_reads < 1:
        raise ValueError("stable_reads must be at least one")

    deadline = monotonic() + timeout_seconds
    streak = 0
    latest: UsbParent | None = None
    first_poll = True
    while True:
        if not first_poll:
            sleep(min(poll_interval_seconds, max(0.0, deadline - monotonic())))
        first_poll = False
        if monotonic() >= deadline:
            break
        parents = tuple(inventory.snapshot())
        same_location = _parents_at_location(parents, identity.location)
        if any(
            parent.vid == vid and parent.identity.serial != identity.serial
            for parent in same_location
        ):
            raise DeviceInventoryError(
                "USB serial changed at the selected physical location"
            )
        exact = [parent for parent in parents if parent.identity == identity]
        if len(exact) > 1:
            raise DeviceInventoryError(
                "multiple USB parents claim the selected serial and location"
            )
        if not exact:
            streak = 0
            latest = None
            continue
        current = exact[0]
        if (current.vid, current.pid) != (vid, pid):
            raise DeviceInventoryError(
                f"selected USB parent returned as unexpected "
                f"{current.vid:04x}:{current.pid:04x}"
            )
        streak += 1
        latest = current
        if streak >= stable_reads:
            return latest

    raise DeviceInventoryError(
        "timed out waiting for the exact USB parent to return stably"
    )


def wait_for_exact_usb_transition(
    inventory: UsbInventory,
    *,
    identity: PhysicalUsbIdentity,
    initial_source: UsbParent | None = None,
    from_vid: int = ESPRESSIF_VID,
    from_pid: int = ESP32S3_ROM_PID,
    to_vid: int = ESPRESSIF_VID,
    to_pid: int = LITCH_APP_PID,
    timeout_seconds: float = 15.0,
    poll_interval_seconds: float = 0.1,
    stable_target_reads: int = 2,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> UsbParent:
    """Follow one physical parent through a bounded VID/PID re-enumeration."""

    if timeout_seconds <= 0 or poll_interval_seconds <= 0:
        raise ValueError("USB transition waits must be positive")
    if stable_target_reads < 1:
        raise ValueError("stable_target_reads must be at least one")

    if initial_source is None:
        source = capture_exact_usb_parent(
            inventory,
            identity=identity,
            vid=from_vid,
            pid=from_pid,
        )
    else:
        source = initial_source
        if (
            source.identity != identity
            or source.vid != from_vid
            or source.pid != from_pid
            or not source.ports
        ):
            raise DeviceInventoryError(
                "captured USB source does not match the exact expected parent"
            )

    deadline = monotonic() + timeout_seconds
    source_gone = False
    target_streak = 0
    latest_target: UsbParent | None = None
    first_poll = True

    while True:
        if not first_poll:
            sleep(min(poll_interval_seconds, max(0.0, deadline - monotonic())))
        first_poll = False
        if monotonic() >= deadline:
            break
        parents = tuple(inventory.snapshot())

        # A device at the same physical port with a different serial is not the
        # selected board.  Treat it as identity drift instead of waiting for a
        # coincidental globally-unique device.
        same_location = _parents_at_location(parents, identity.location)
        serial_mismatches = [
            parent
            for parent in same_location
            if parent.vid in {from_vid, to_vid} and parent.identity.serial != identity.serial
        ]
        if serial_mismatches:
            raise DeviceInventoryError(
                "USB serial changed at the selected physical location"
            )

        exact = [parent for parent in parents if parent.identity == identity]
        if len(exact) > 1:
            raise DeviceInventoryError(
                "multiple USB parents claim the selected serial and location"
            )
        if not exact:
            source_gone = True
            target_streak = 0
            latest_target = None
            continue

        current = exact[0]
        current_pair = current.vid, current.pid
        if current_pair == (from_vid, from_pid):
            # Serial/JTAG can enumerate again before TinyUSB starts. Its PID
            # is controller identity, not proof that the candidate is in ROM.
            # Only post-flash observation admits this bounded intermediate.
            if source_gone and (from_pid, to_pid) != (ESP32S3_ROM_PID, LITCH_APP_PID):
                raise DeviceInventoryError(
                    "source USB parent reappeared after disappearing"
                )
            target_streak = 0
            latest_target = None
            continue
        if current_pair != (to_vid, to_pid):
            raise DeviceInventoryError(
                f"selected USB parent re-enumerated as unexpected "
                f"{current.vid:04x}:{current.pid:04x}"
            )

        source_gone = True
        target_streak += 1
        latest_target = current
        if target_streak >= stable_target_reads:
            return latest_target

    raise UsbTransitionTimeout(
        f"timed out waiting for exact USB parent {from_vid:04x}:{from_pid:04x} "
        f"to become {to_vid:04x}:{to_pid:04x}"
    )
