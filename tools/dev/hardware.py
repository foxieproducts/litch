"""High-level exact-device workflows for the canonical ``./litch`` command.

This module joins the pure USB, ROM-flash, and runtime-verification seams to
the repository runner and lock model.  Importing it never enumerates, opens,
or resets a device; hardware access happens only in an explicitly selected
command.
"""

from __future__ import annotations

import asyncio
import re
import struct
import sys
import time
from contextlib import contextmanager
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, TextIO

from .devices import (
    ESP32S3_ROM_PID,
    ESPRESSIF_VID,
    LITCH_APP_PID,
    DeviceInventoryError,
    UsbTransitionTimeout,
    PhysicalUsbIdentity,
    PioUsbInventory,
    UsbParent,
    capture_exact_usb_parent,
    normalize_usb_serial,
    normalize_usb_parents,
    wait_for_exact_usb_parent,
    wait_for_exact_usb_transition,
)
from .locks import LockManager, normalize_device_identity
from .build_paths import platformio_build_directory
from .tool_requirements import ESPTOOL_INSTALL_REMEDIATION, resolve_esptool_argv
from .local_state import read as read_local_state, remember_many as remember_local_state
from .rom_flash import (
    PARTITION_TABLE_BINARY_BYTES,
    EsptoolDialect,
    RomFlashError,
    detect_esptool_dialect,
    erase_s3_direct,
    flash_s3_direct,
    inspect_firmware_image,
    read_firmware_header_version,
    require_direct_inputs,
)

from .runner import CompletedCommand, DevError, Runner
from .runtime_verify import (
    BleakControlFactory,
    PySerialFactory,
    RuntimeVerificationError,
    SERIAL_PROBE_HARD_TIMEOUT_SECONDS,
    close_serial_transport,
    parse_serial_probe_result,
    run_ble_runtime_bounded,
    serial_probe_argv,
)
from .workspace import Workspace


PIO_ENVIRONMENT = {"PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no"}
TARGET_ENVIRONMENTS = {
    "c3": "litch-esp32c3",
    "s3": "litch-esp32s3",
}
FIRMWARE_VERSION_HEADER = Path("include/firmware/firmware_version.hpp")
BLE_SCAN_SECONDS = 4.0
BLE_SCAN_TIMEOUT_SECONDS = 15.0
LOCAL_STATE_NAME = "dev-local-state.json"
_MAX_SELECTOR_LENGTH = 256
_ANSI_CSI_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")
_S3_ROM_MAC_RE = re.compile(
    r"^MAC:[ \t]*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})[ \t\r]*$",
    re.IGNORECASE | re.MULTILINE,
)
_WRITE_PROGRESS_RE = re.compile(
    r"\bWriting at\b.*?\b(?P<percent>\d{1,3}(?:\.\d+)?)%",
    re.IGNORECASE,
)
_WROTE_SEGMENT_RE = re.compile(
    r"\bWrote\s+.+?\bat\s+(?P<offset>0x[0-9a-f]+)\b",
    re.IGNORECASE,
)
_S3_BOOTLOADER_COMMAND = b"reboot bootloader\n"
_S3_BOOTLOADER_READY = "reboot bootloader ready: state saved; entering ROM"
# Firmware owns up to 10 s persistence + 1 s electrical settle. The remaining
# 2 s bounds console delivery and host reads; no timeout authorizes a reset.
_S3_BOOTLOADER_ADMISSION_SECONDS = 13.0



class BuildTarget(Protocol):
    def __call__(self, target: str) -> None: ...


class CoreDirectory(Protocol):
    def __call__(self) -> Path: ...


@dataclass(frozen=True)
class S3RomEntryAdmission:
    response: str
    teardown_error: str | None = None


def touch_s3_application_bootloader(port: str) -> S3RomEntryAdmission:
    """Request one persistence-governed ROM entry; never drive a CDC reset."""

    transport = PySerialFactory().open(
        port,
        baudrate=115200,
        read_timeout_seconds=0.1,
        dtr=True,
        rts=True,
    )
    response = bytearray()
    admitted = False
    teardown_error = None
    try:
        transport.reset_input_buffer()
        if transport.write(_S3_BOOTLOADER_COMMAND) != len(_S3_BOOTLOADER_COMMAND):
            raise RuntimeVerificationError("S3 governed ROM request was only partially written")
        deadline = time.monotonic() + _S3_BOOTLOADER_ADMISSION_SECONDS
        while time.monotonic() < deadline and len(response) < 8192:
            response.extend(transport.read(min(512, 8192 - len(response))))
            lines = response.decode("utf-8", errors="replace").replace("\r", "\n").splitlines()
            for line in lines:
                if line.startswith(("Unknown command", "unknown command", "ERR ", "error:", "Error:")):
                    raise RuntimeVerificationError("save-aware entry unavailable: " + line)
                if line.startswith(("reboot bootloader rejected:", "reboot bootloader aborted:")):
                    raise RuntimeVerificationError("S3 governed ROM entry refused: " + line)
            if (_S3_BOOTLOADER_READY in lines and "> reboot bootloader" in lines):
                print("S3 USB: " + _S3_BOOTLOADER_READY)
                admitted = True
                break
        if not admitted:
            raise RuntimeVerificationError(
                "S3 governed ROM entry has no durable terminal acknowledgement; "
                "no reset or legacy CDC fallback was attempted. Response: "
                + response.decode("utf-8", errors="replace")
            )
    except BaseException as error:
        notes = list(getattr(error, "__notes__", ()))
        notes.append("Governed ROM response: " + response.decode("utf-8", errors="replace"))
        setattr(error, "__notes__", notes)
        raise
    finally:
        if admitted:
            try:
                close_serial_transport(transport)
            except RuntimeVerificationError as error:
                # The intended detach can invalidate this exact APP endpoint.
                # Retain uncertainty; only the caller's exact ROM identity proof
                # can establish that the admitted transition completed.
                teardown_error = str(error)
        else:
            close_serial_transport(transport, primary_error=sys.exc_info()[1])
    return S3RomEntryAdmission(response.decode("utf-8", errors="replace"), teardown_error)


def enter_s3_rom_from_application(
    inventory: PioUsbInventory,
    *,
    application: UsbParent,
) -> UsbParent:
    """Require one acknowledged request and this exact parent's ROM transition."""

    captured = wait_for_exact_usb_parent(
        inventory,
        identity=application.identity,
        vid=ESPRESSIF_VID,
        pid=LITCH_APP_PID,
        stable_reads=1,
    )
    try:
        admission = touch_s3_application_bootloader(captured.port)
        if isinstance(admission, S3RomEntryAdmission) and admission.teardown_error is not None:
            print("S3 USB: state saved; application USB detached during handoff", flush=True)
        return wait_for_exact_usb_transition(
            inventory, identity=captured.identity, initial_source=captured,
            from_vid=ESPRESSIF_VID, from_pid=LITCH_APP_PID,
            to_vid=ESPRESSIF_VID, to_pid=ESP32S3_ROM_PID,
        )
    except (RuntimeVerificationError, UsbTransitionTimeout, OSError) as error:
        print(f"S3 USB: save-aware entry unavailable ({error}); trying standard USB bootloader entry", flush=True)
    # Older/private firmware may not expose the console command. The standard
    # Arduino 1200-baud touch needs no console permission or readiness proof.
    selected = admit_s3_usb_parent(inventory, serial=captured.identity.serial)
    if selected.identity != captured.identity:
        raise DeviceInventoryError("selected USB identity changed before bootloader entry")
    if selected.pid == ESP32S3_ROM_PID:
        return selected
    touch_error = None
    try:
        transport = PySerialFactory().open(selected.port, baudrate=1200,
                                           read_timeout_seconds=0.1, dtr=True, rts=True)
        try:
            time.sleep(0.1)
        finally:
            close_serial_transport(transport)
    except (RuntimeVerificationError, OSError) as error:
        touch_error = error
    try:
        return wait_for_exact_usb_transition(
            inventory, identity=captured.identity, initial_source=selected,
            from_vid=ESPRESSIF_VID, from_pid=LITCH_APP_PID,
            to_vid=ESPRESSIF_VID, to_pid=ESP32S3_ROM_PID,
        )
    except UsbTransitionTimeout as error:
        raise DeviceInventoryError(
            f"Could not enter the bootloader for USB serial {captured.identity.serial}. "
            "Hold P1-X while reconnecting USB, release it, then retry the same selected board. "
            f"Cause: {touch_error or error}"
        ) from error


@dataclass(frozen=True)
class FlashTarget:
    """A resolved transport target.  It is safe to lock and execute as-is."""

    target: str
    transport: str
    usb_identity: str | None = None
    ble_identity: str | None = None

    def __post_init__(self) -> None:
        if self.target not in TARGET_ENVIRONMENTS:
            raise ValueError(f"unknown flash target {self.target!r}")
        if self.transport not in {"usb", "ble"}:
            raise ValueError(f"unknown flash transport {self.transport!r}")
        if self.transport == "usb" and not self.usb_identity:
            raise ValueError("USB flash target needs a USB identity")
        if self.transport == "ble" and (self.target != "s3" or not self.ble_identity):
            raise ValueError("BLE flash is available only for S3 and needs a BLE identity")


@dataclass(frozen=True)
class FlashCandidate:
    """One currently discoverable board identity, before target selection."""

    transport: str
    identity: str
    display: str
    target_hint: str | None = None
    ports: tuple[str, ...] = ()
    usb_parent: UsbParent | None = None

    def matches(self, selector: str) -> bool:
        if self.transport == "usb":
            if selector.startswith("/"):
                return selector in self.ports
            try:
                return normalize_usb_serial(selector) in normalize_usb_serial(
                    self.identity
                )
            except (TypeError, ValueError):
                return False
        return selector.casefold() in self.identity.casefold()


def _local_state_path(workspace: Workspace) -> Path:
    return workspace.root / ".cache" / LOCAL_STATE_NAME


def _bounded_identity(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    normalized = value.strip()
    if not normalized or len(normalized) > _MAX_SELECTOR_LENGTH:
        return None
    return normalized


def _remembered_flash(path: Path, target: str | None) -> FlashTarget | None:
    state = read_local_state(path)
    if target is None and state is not None:
        target = state.get("last_flash")
    if not isinstance(target, str) or target not in {"c3", "s3"}:
        return None
    value = None if state is None else state.get(f"{target}_firmware")
    if not isinstance(value, dict):
        return None
    try:
        return FlashTarget(
            target,
            value["transport"],
            _bounded_identity(value.get("usb_identity")),
            _bounded_identity(value.get("ble_identity")),
        )
    except (KeyError, TypeError, ValueError):
        return None


def _remember_flash(path: Path, selected: FlashTarget) -> None:
    remember_local_state(
        path,
        {
            f"{selected.target}_firmware": {
                "transport": selected.transport,
                "usb_identity": selected.usb_identity,
                "ble_identity": selected.ble_identity,
            },
            "last_flash": selected.target,
        },
    )


def _usb_flash_candidates(parents: Sequence[UsbParent]) -> tuple[FlashCandidate, ...]:
    return tuple(
        FlashCandidate(
            "usb",
            parent.identity.serial,
            f"USB {parent.identity.serial} at {parent.identity.location} ({parent.vid:04x}:{parent.pid:04x})",
            "s3" if parent.pid == LITCH_APP_PID else None,
            parent.ports,
            parent,
        )
        for parent in _target_usb_parents(parents)
    )


async def _discover_ble_candidates() -> tuple[FlashCandidate, ...]:
    # Keep discovery classification in the established BLE OTA implementation.
    from tools import ble_update_firmware

    candidates = await ble_update_firmware.scan_candidates(BLE_SCAN_SECONDS)
    return tuple(
        FlashCandidate("ble", candidate.address, f"BLE {candidate.display_name}", "s3")
        for candidate in candidates
    )


def _resolve_unique_candidate(
    candidates: Sequence[FlashCandidate], selector: str, *, transport: str
) -> FlashCandidate:
    matches = [candidate for candidate in candidates if candidate.matches(selector)]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        recovery = (
            "; rerun ./litch devices and copy the current PORT path"
            if transport.upper() == "USB" and selector.startswith("/") else ""
        )
        raise DevError(
            f"no {transport.upper()} Litch board matched selector {selector!r}{recovery}",
            exit_code=2,
        )
    listed = "; ".join(candidate.display for candidate in matches)
    raise DevError(
        f"{transport.upper()} selector {selector!r} is ambiguous: {listed}", exit_code=2
    )


def _selection_required(candidates: Sequence[FlashCandidate]) -> DevError:
    options = "\n".join(
        f"  {index}. {candidate.display}" for index, candidate in enumerate(candidates, start=1)
    )
    selectors = []
    if any(candidate.transport == "usb" for candidate in candidates):
        selectors.append("`--device <USB serial or exact port path>`")
    if any(candidate.transport == "ble" for candidate in candidates):
        selectors.append("`--ble-device <unique BLE address>`")
    return DevError(
        "multiple Litch boards are available; select one explicitly:\n"
        f"{options}\n"
        "rerun with " + " or ".join(selectors),
        exit_code=2,
    )


def _validate_flash_request(
    requested_target: str | None,
    usb_selector: str | None,
    ble_selector: str | None,
) -> None:
    for selector, transport in ((usb_selector, "USB"), (ble_selector, "BLE")):
        if selector is not None and _bounded_identity(selector) is None:
            raise DevError(
                f"{transport} selector must be non-empty and at most 256 characters",
                exit_code=2,
            )
    if usb_selector is not None and ble_selector is not None:
        raise DevError(
            "--device and --ble-device are mutually exclusive; choose one transport: "
            "use --device <usb-serial> for USB or --ble-device <ble-address> for BLE",
            exit_code=2,
        )
    if requested_target == "c3" and ble_selector:
        raise DevError("C3 does not support BLE OTA; select a USB board", exit_code=2)


def resolve_flash_target(
    *,
    requested_target: str | None,
    usb_selector: str | None,
    ble_selector: str | None,
    usb_candidates: Sequence[FlashCandidate],
    ble_candidates: Sequence[FlashCandidate],
    remembered: FlashTarget | None,
) -> FlashTarget:
    """Resolve selectors without guessing across boards or unknown USB models."""

    _validate_flash_request(requested_target, usb_selector, ble_selector)

    usb = (
        _resolve_unique_candidate(usb_candidates, usb_selector, transport="USB")
        if usb_selector
        else None
    )
    ble = (
        _resolve_unique_candidate(ble_candidates, ble_selector, transport="BLE")
        if ble_selector
        else None
    )
    selected = usb or ble
    if selected is None and remembered is not None:
        if requested_target is None and remembered.target == "c3":
            raise DevError(
                "the remembered board is a legacy target; name that target explicitly to use it",
                exit_code=2,
            )
        remembered_candidates = (
            usb_candidates if remembered.transport == "usb" else ble_candidates
        )
        remembered_identity = (
            remembered.usb_identity
            if remembered.transport == "usb"
            else remembered.ble_identity
        )
        selected = next(
            (
                candidate
                for candidate in remembered_candidates
                if _candidate_identity_matches(candidate, remembered_identity)
            ),
            None,
        )
        if selected is not None:
            target = requested_target or remembered.target
            if target == "c3" and selected.transport == "ble":
                raise DevError("C3 does not support BLE OTA; select a USB board", exit_code=2)
            return FlashTarget(
                target,
                selected.transport,
                selected.identity if selected.transport == "usb" else None,
                selected.identity if selected.transport == "ble" else None,
            )
        raise DevError("the remembered flash device is no longer available", exit_code=2)

    if selected is None:
        candidates = tuple(usb_candidates) + tuple(ble_candidates)
        if len(candidates) != 1:
            if not candidates:
                raise DevError("no USB or BLE Litch boards are currently available", exit_code=2)
            raise _selection_required(candidates)
        selected = candidates[0]

    target = requested_target or selected.target_hint
    if requested_target is None and target == "c3":
        raise DevError(
            "the USB board is a legacy target; name that target explicitly to use it",
            exit_code=2,
        )
    if target is None:
        raise DevError(
            "the USB board target is not safely identifiable; rerun with an explicit target and exact USB serial",
            exit_code=2,
        )
    if target == "c3" and selected.transport == "ble":
        raise DevError("C3 does not support BLE OTA; select a USB board", exit_code=2)
    return FlashTarget(
        target,
        selected.transport,
        selected.identity if selected.transport == "usb" else None,
        selected.identity if selected.transport == "ble" else None,
    )


class _EsptoolProgress:
    """Render a small stable subset of esptool's captured terminal stream."""

    def __init__(self, output: TextIO) -> None:
        self._output = output
        self._buffer = ""
        self._last_write_percent: float | None = None

    def feed(self, _stream_name: str, chunk: str) -> None:
        self._buffer += chunk
        while True:
            match = re.search(r"[\r\n]", self._buffer)
            if match is None:
                return
            record = self._buffer[: match.start()]
            self._buffer = self._buffer[match.end() :]
            self._render(record)

    def finish(self) -> None:
        if self._buffer:
            self._render(self._buffer)
            self._buffer = ""

    def _render(self, record: str) -> None:
        clean = _ANSI_CSI_RE.sub("", record).strip()
        write = _WRITE_PROGRESS_RE.search(clean)
        if write is not None:
            percent = min(100.0, float(write.group("percent")))
            if (
                self._last_write_percent is None
                or percent >= 100.0
                or percent >= self._last_write_percent + 10.0
            ):
                print(
                    f"S3 flash: writing {percent:.1f}%",
                    file=self._output,
                    flush=True,
                )
                self._last_write_percent = percent
            return
        wrote = _WROTE_SEGMENT_RE.search(clean)
        if wrote is not None:
            print(
                f"S3 flash: wrote segment at {wrote.group('offset')}",
                file=self._output,
                flush=True,
            )
            return


class _BoundCaptureRunner:
    """Adapt the repository runner to the narrow fakeable hardware protocol."""

    def __init__(
        self,
        runner: Runner,
        *,
        cwd: Path,
        verbose: bool = False,
        output: TextIO | None = None,
    ) -> None:
        self._runner = runner
        self._cwd = cwd
        self._verbose = verbose
        self._output = output or sys.stdout
        self._timings: dict[str, tuple[float, int]] = {}

    @contextmanager
    def phase(self, name: str):
        started = time.monotonic()
        try:
            yield
        finally:
            elapsed, count = self._timings.get(name, (0.0, 0))
            self._timings[name] = elapsed + time.monotonic() - started, count + 1

    def report_timings(self, total_seconds: float) -> None:
        parts = [f"{name} {seconds:.2f}s ({count})"
                 for name, (seconds, count) in self._timings.items()]
        print("S3 USB execution timing (selection excluded; phases may overlap): "
              + "; ".join(parts) + f"; elapsed {total_seconds:.2f}s",
              file=self._output, flush=True)

    def run(
        self,
        argv: Sequence[str],
        *,
        timeout_seconds: float,
        open_port_attempts: int | None = None,
    ) -> CompletedCommand:
        normalized = tuple(str(token) for token in argv)
        show_progress = any(
            token in {"write-flash", "write_flash"}
            for token in normalized
        )
        progress = _EsptoolProgress(self._output) if show_progress else None
        extra_env = dict(PIO_ENVIRONMENT)
        if open_port_attempts is not None:
            extra_env["ESPTOOL_OPEN_PORT_ATTEMPTS"] = str(open_port_attempts)
        command = next((token.replace("_", "-") for token in normalized
                        if token in {"read-flash", "read_flash", "write-flash", "write_flash",
                                     "erase-flash", "erase_flash", "read-mac", "read_mac",
                                     "version"}), "discovery")
        try:
            with self.phase(command):
                return self._runner.run(
                    normalized,
                    cwd=self._cwd,
                    timeout_seconds=timeout_seconds,
                    capture=True,
                    check=False,
                    extra_env=extra_env,
                    announce=self._verbose,
                    live_output=progress.feed if progress is not None else None,
                )
        except DevError as error:
            # The narrow seams deliberately model launch/timeout uncertainty as
            # an OS-level failure and never retry a possibly executed reset.
            raise OSError(str(error)) from error
        finally:
            if progress is not None:
                progress.finish()


def _selector_key(selector: str) -> str:
    try:
        normalized = normalize_usb_serial(selector)
    except (TypeError, ValueError) as error:
        raise DeviceInventoryError(f"invalid exact USB serial: {error}") from error
    return normalized


def _candidate_identity_matches(
    candidate: FlashCandidate, identity: str | None
) -> bool:
    if identity is None:
        return False
    if candidate.transport == "usb":
        try:
            return normalize_usb_serial(candidate.identity) == normalize_usb_serial(
                identity
            )
        except (TypeError, ValueError):
            return False
    return candidate.identity.casefold() == identity.casefold()


def select_parent_by_serial(
    parents: Sequence[UsbParent],
    *,
    serial: str,
    vid: int,
    pid: int,
) -> UsbParent:
    """Resolve one serial without an only-device or VID/PID-only fallback."""

    key = _selector_key(serial)
    serial_matches = [parent for parent in parents if parent.identity.serial == key]
    identities = {parent.identity for parent in serial_matches}
    if not serial_matches:
        raise DeviceInventoryError(f"no USB parent exactly matched serial {serial!r}")
    if len(identities) != 1 or len(serial_matches) != 1:
        raise DeviceInventoryError(
            f"USB serial {serial!r} is ambiguous across locations or interfaces"
        )
    parent = serial_matches[0]
    if (parent.vid, parent.pid) != (vid, pid):
        raise DeviceInventoryError(
            f"USB serial {serial!r} is {parent.vid:04x}:{parent.pid:04x}, "
            f"expected {vid:04x}:{pid:04x}"
        )
    return parent


def select_s3_usb_parent(parents: Sequence[UsbParent], *, serial: str) -> UsbParent:
    """Select one exact S3 USB parent in either application or ROM mode."""

    key = _selector_key(serial)
    matches = [
        parent
        for parent in parents
        if parent.identity.serial == key
        and parent.vid == ESPRESSIF_VID
        and parent.pid in {LITCH_APP_PID, ESP32S3_ROM_PID}
    ]
    if len(matches) != 1:
        raise DeviceInventoryError(
            f"S3 USB serial {serial!r} must identify one application or ROM parent"
        )
    return matches[0]


def admit_s3_usb_parent(
    inventory: PioUsbInventory,
    *,
    serial: str,
    timeout_seconds: float = 5.0,
    poll_interval_seconds: float = 0.1,
    monotonic=time.monotonic,
    sleep=time.sleep,
) -> UsbParent:
    """Capture a stable exact S3 parent before build without changing its mode."""

    if timeout_seconds <= 0 or poll_interval_seconds <= 0:
        raise ValueError("S3 USB admission waits must be positive")

    key = _selector_key(serial)
    deadline = monotonic() + timeout_seconds
    first_poll = True
    while True:
        if not first_poll:
            sleep(min(poll_interval_seconds, max(0.0, deadline - monotonic())))
        first_poll = False
        if monotonic() >= deadline:
            break

        parents = tuple(inventory.snapshot())
        serial_matches = [
            parent for parent in parents if parent.identity.serial == key
        ]
        if not serial_matches:
            continue

        # Once the selected serial appears, wrong hardware or multiple parents
        # are fatal. Only a temporary absence is retried.
        parent = select_s3_usb_parent(parents, serial=serial)
        return wait_for_exact_usb_parent(
            inventory,
            identity=parent.identity,
            vid=ESPRESSIF_VID,
            pid=parent.pid,
            timeout_seconds=max(poll_interval_seconds, deadline - monotonic()),
            poll_interval_seconds=poll_interval_seconds,
            monotonic=monotonic,
            sleep=sleep,
        )

    raise DeviceInventoryError(
        f"timed out waiting for S3 USB serial {serial!r} to return stably"
    )


def _target_usb_parents(parents: Sequence[UsbParent]) -> tuple[UsbParent, ...]:
    return tuple(
        parent
        for parent in parents
        if parent.vid == ESPRESSIF_VID
        and parent.pid in {ESP32S3_ROM_PID, LITCH_APP_PID}
    )


def _render_usb_devices(parents: Sequence[UsbParent]) -> list[str]:
    lines = ["USB_SERIAL\tLOCATION\tVID:PID\tPORT"]
    for parent in _target_usb_parents(parents):
        lines.append(
            f"{parent.identity.serial}\t{parent.identity.location}\t"
            f"{parent.vid:04x}:{parent.pid:04x}\t{parent.port}"
        )
    if len(lines) == 1:
        lines.append("(no exact Litch USB identities found)")
    return lines


def _scan_ble(workspace: Workspace, runner: Runner) -> str:
    result = runner.run(
        [
            sys.executable,
            workspace.root / "tools" / "ble_update_firmware.py",
            "scan",
            "--scan-seconds",
            str(BLE_SCAN_SECONDS),
        ],
        cwd=workspace.root,
        timeout_seconds=BLE_SCAN_TIMEOUT_SECONDS,
        capture=True,
        check=False,
    )
    output = "\n".join(
        part.strip() for part in (result.stdout, result.stderr) if part.strip()
    )
    if result.returncode != 0 and output != "No Litch boards found.":
        raise DevError(f"BLE discovery failed: {output or 'no diagnostic'}")
    return output or "No Litch boards found."


def list_devices_for_cli(
    workspace: Workspace,
    runner: Runner,
    *,
    include_ble: bool,
    lock_timeout: float,
) -> str:
    bound = _BoundCaptureRunner(runner, cwd=workspace.root)
    try:
        with LockManager(workspace, argv=sys.argv).lane(
            "platformio", timeout=lock_timeout
        ):
            parents = PioUsbInventory(bound).snapshot()
    except DeviceInventoryError as error:
        raise DevError(str(error)) from error
    lines = _render_usb_devices(parents)
    if include_ble:
        lines.extend(("", "BLE", _scan_ble(workspace, runner)))
    return "\n".join(lines)


def _esptool_argv() -> tuple[str, ...]:
    try:
        argv = resolve_esptool_argv()
    except RuntimeError as error:
        raise DevError(str(error)) from error
    if argv is None:
        raise DevError(f"esptool 4.9+ or 5.x is required; {ESPTOOL_INSTALL_REMEDIATION}")
    return argv


def _probe_s3_rom_entry(
    bound: _BoundCaptureRunner,
    esptool_argv: Sequence[str],
    dialect: EsptoolDialect,
    rom: UsbParent,
) -> None:
    # USB Serial/JTAG can enumerate before its port can open or the ROM
    # loader can answer. Keep esptool's open retries on this read-only probe.
    if sys.platform == "darwin":
        print("S3 USB: allow the USB accessory if macOS prompts.", flush=True)
    read_mac = "read_mac" if dialect.major == 4 else "read-mac"
    result = bound.run(
        (*esptool_argv, "--chip", "esp32s3", "--port", rom.port,
         "--before", dialect.default_reset, "--after", dialect.no_reset,
         "--no-stub", read_mac),
        timeout_seconds=30, open_port_attempts=30,
    )
    output = _ANSI_CSI_RE.sub("", f"{result.stdout}\n{result.stderr}").strip()
    if result.returncode:
        detail = output or "no diagnostic"
        raise DeviceInventoryError(f"S3 ROM probe failed with exit code {result.returncode}: {detail}")
    # esptool can print the base MAC both in its connection summary and
    # in the command reply. Both must describe the same selected chip.
    macs = {normalize_usb_serial(mac) for mac in _S3_ROM_MAC_RE.findall(output)}
    if macs != {rom.identity.serial}:
        raise DeviceInventoryError("S3 ROM probe did not confirm the selected USB serial's MAC")


def _platformio_upload(
    workspace: Workspace,
    runner: Runner,
    *,
    target: str,
    port: str,
) -> None:
    runner.run(
        [
            "pio",
            "run",
            "-e",
            TARGET_ENVIRONMENTS[target],
            "-t",
            "upload",
            "--upload-port",
            port,
        ],
        cwd=workspace.root,
        timeout_seconds=5 * 60,
        extra_env=PIO_ENVIRONMENT,
    )


def _run_serial_version_probe(
    workspace: Workspace,
    runner: Runner,
    *,
    port: str,
    expected_version: str,
    expected_image_sha256: str | None = None,
    assert_dtr: bool = False,
) -> str:
    """Run all sync serial I/O inside a bounded disposable subprocess."""

    argv = serial_probe_argv(
        python_executable=sys.executable,
        port=port,
        expected_version=expected_version,
        expected_image_sha256=expected_image_sha256,
        assert_dtr=assert_dtr,
    )
    try:
        completed = runner.run(
            argv,
            cwd=workspace.root,
            timeout_seconds=SERIAL_PROBE_HARD_TIMEOUT_SECONDS,
            capture=True,
            check=False,
        )
    except DevError as error:
        raise RuntimeVerificationError(
            "serial version probe exceeded its hard process boundary or could not run: "
            f"{error}"
        ) from error
    if completed.returncode != 0:
        diagnostic = "\n".join(
            part.strip()
            for part in (completed.stderr, completed.stdout)
            if part.strip()
        )
        if len(diagnostic) > 2000:
            marker = "first_post_trial_incident="
            incident_at = diagnostic.rfind(marker)
            tail = (
                diagnostic[incident_at:incident_at + 700]
                if incident_at >= 0
                else diagnostic[-700:]
            )
            diagnostic = f"{diagnostic[:1200]}\n...[truncated]...\n{tail}"
        raise RuntimeVerificationError(
            "serial version probe failed: "
            f"{diagnostic if diagnostic else 'no diagnostic'}"
        )
    if completed.stderr.strip():
        raise RuntimeVerificationError(
            "serial version probe emitted unexpected stderr output"
        )
    return parse_serial_probe_result(
        completed.stdout,
        expected_version=expected_version,
    )


@dataclass(frozen=True)
class _C3UploadContext:
    inventory: PioUsbInventory
    identity: PhysicalUsbIdentity
    expected_version: str




def _require_c3_image_fits_partition(build_dir: Path) -> None:
    """Check actual image bytes; the ELF size summary can omit .eh_frame."""

    firmware = build_dir / "firmware.bin"
    partitions = build_dir / "partitions.bin"
    try:
        if not firmware.is_file():
            raise DevError(f"missing C3 firmware image: {firmware}")
        firmware_size = firmware.stat().st_size
        with partitions.open("rb") as source:
            table = source.read(PARTITION_TABLE_BINARY_BYTES + 1)
    except OSError as error:
        raise DevError(f"cannot inspect C3 build artifacts: {error}") from error
    if len(table) != PARTITION_TABLE_BINARY_BYTES:
        raise DevError("C3 partitions.bin must be a 3072-byte partition table")

    app0_sizes: list[int] = []
    for position in range(0, len(table), 32):
        magic, kind, subtype, offset, size, label, _ = struct.unpack_from(
            "<HBBII16sI", table, position,
        )
        if magic in (0xEBEB, 0xFFFF):
            break
        if magic != 0x50AA:
            raise DevError("invalid entry in C3 partitions.bin")
        if label.split(b"\0", 1)[0] == b"app0":
            if (kind, subtype, offset) != (0, 0x10, 0x10000):
                raise DevError("C3 app0 must be ota_0 at offset 0x10000")
            app0_sizes.append(size)
    if len(app0_sizes) != 1 or app0_sizes[0] <= 0:
        raise DevError("C3 partitions.bin must contain one nonempty app0 slot")
    if firmware_size <= 0:
        raise DevError(f"C3 firmware.bin is empty: {firmware}")
    if firmware_size > app0_sizes[0]:
        raise DevError(
            f"C3 firmware.bin is {firmware_size} bytes; exceeds built app0 "
            f"capacity of {app0_sizes[0]} bytes; rebuild before flashing"
        )


def _upload_c3(
    workspace: Workspace,
    runner: Runner,
    inventory: PioUsbInventory,
    *,
    serial: str,
    expected_identity: PhysicalUsbIdentity | None = None,
) -> _C3UploadContext:
    parent = select_parent_by_serial(
        inventory.snapshot(),
        serial=serial,
        vid=ESPRESSIF_VID,
        pid=ESP32S3_ROM_PID,
    )
    if expected_identity is not None and parent.identity != expected_identity:
        raise DeviceInventoryError(
            "selected USB identity changed during build; list devices and select again before flashing"
        )
    _require_c3_image_fits_partition(
        platformio_build_directory(workspace.root, TARGET_ENVIRONMENTS["c3"])
    )
    # Capture once more immediately before the upload; a build must never turn
    # a stale port path into permission to select another board.
    exact = capture_exact_usb_parent(
        inventory,
        identity=parent.identity,
        vid=ESPRESSIF_VID,
        pid=ESP32S3_ROM_PID,
    )
    expected_version = read_firmware_header_version(
        workspace.root / FIRMWARE_VERSION_HEADER
    )
    _platformio_upload(workspace, runner, target="c3", port=exact.port)
    return _C3UploadContext(inventory, exact.identity, expected_version)


def _verify_c3_runtime(
    context: _C3UploadContext,
    *,
    workspace: Workspace,
    runner: Runner,
) -> None:
    application = wait_for_exact_usb_parent(
        context.inventory,
        identity=context.identity,
        vid=ESPRESSIF_VID,
        pid=ESP32S3_ROM_PID,
    )
    actual_version = _run_serial_version_probe(
        workspace,
        runner,
        port=application.port,
        expected_version=context.expected_version,
        assert_dtr=False,
    )
    print(
        f"C3 flash verified for USB serial {application.identity.serial} "
        f"at location {application.identity.location}: version={actual_version}"
    )


def _flash_s3_usb(
    workspace: Workspace, inventory: PioUsbInventory, *, rom: UsbParent,
    bound: _BoundCaptureRunner, esptool_argv: Sequence[str], dialect,
    factory_provision: bool,
) -> None:
    identity = rom.identity
    label = f"USB serial {identity.serial} at location {identity.location}"
    try:
        selected = capture_exact_usb_parent(inventory, identity=identity,
                                            vid=ESPRESSIF_VID, pid=ESP32S3_ROM_PID)
        build_dir = platformio_build_directory(workspace.root, TARGET_ENVIRONMENTS["s3"])
        require_direct_inputs(build_dir, factory=factory_provision)
        if factory_provision:
            print("S3 factory: erase the board, then write the initial firmware", flush=True)
            erase_s3_direct(bound, esptool_argv, dialect, port=selected.port)
            selected = wait_for_exact_usb_parent(inventory, identity=identity,
                                                 vid=ESPRESSIF_VID, pid=ESP32S3_ROM_PID)
        print("S3 USB: release P1-X if held; flashing firmware with esptool", flush=True)
        flash_s3_direct(bound, esptool_argv, dialect, port=selected.port,
                        build_dir=build_dir,
                        factory=factory_provision)
    except (RomFlashError, OSError, DeviceInventoryError, KeyboardInterrupt) as error:
        retry = " --factory" if factory_provision else ""
        guidance = (f"Flash stopped for {label}. Release P1-X if held and reconnect the same board, "
                    f"then retry ./litch flash s3 --device {identity.serial}{retry}. "
                    "If it will not connect, hold P1-X while reconnecting USB, release it, and retry.")
        if isinstance(error, KeyboardInterrupt):
            print("litch: " + guidance, file=sys.stderr, flush=True)
            raise
        raise RomFlashError(f"{guidance} Cause: {error}") from error
    print(f"S3 USB flash complete for {label} (esptool)", flush=True)


def flash_exact_device(
    *, workspace: Workspace, target: str, usb_identity: str, ble_identity: str | None,
    runner: Runner, lock_timeout: float, build_target: BuildTarget,
    platformio_core_dir: CoreDirectory, verbose: bool = False,
    factory_provision: bool = False,
    expected_usb_identity: PhysicalUsbIdentity | None = None,
) -> None:
    if target not in TARGET_ENVIRONMENTS:
        raise DevError(f"unsupported hardware target: {target}", exit_code=2)
    try:
        _selector_key(usb_identity)
    except DeviceInventoryError as error:
        raise DevError(str(error), exit_code=2) from error
    _validate_flash_request(target, usb_identity, ble_identity)
    if factory_provision and target != "s3":
        raise DevError("--factory is only valid for S3 USB flash", exit_code=2)
    platformio_build_directory(workspace.root, TARGET_ENVIRONMENTS[target])
    manager = LockManager(workspace, argv=sys.argv)
    bound = _BoundCaptureRunner(runner, cwd=workspace.root, verbose=verbose)
    started = time.monotonic()
    try:
        if target == "c3":
            PySerialFactory.require_available()
        with manager.device(normalize_device_identity(_selector_key(usb_identity)), timeout=lock_timeout):
            with manager.lane("platformio", timeout=lock_timeout):
                inventory = PioUsbInventory(bound)
                if target == "c3":
                    admitted = select_parent_by_serial(inventory.snapshot(), serial=usb_identity,
                                                       vid=ESPRESSIF_VID, pid=ESP32S3_ROM_PID)
                else:
                    admitted = admit_s3_usb_parent(inventory, serial=usb_identity)
                if expected_usb_identity is not None and admitted.identity != expected_usb_identity:
                    raise DeviceInventoryError("selected USB identity changed; list devices and select again before flashing")
                if target == "s3":
                    esptool_argv = _esptool_argv()
                    dialect = detect_esptool_dialect(bound, esptool_argv)
                with bound.phase("build"):
                    build_target(target)
                if target == "c3":
                    c3_context = _upload_c3(workspace, runner, inventory, serial=usb_identity,
                                             expected_identity=admitted.identity)
                else:
                    retry = f"./litch flash s3 --device {admitted.identity.serial}" + (" --factory" if factory_provision else "")
                    entry_guidance = (f"USB serial {admitted.identity.serial} at location {admitted.identity.location}: "
                                      "release P1-X and reconnect this same board. If needed, hold P1-X while reconnecting "
                                      f"USB, release it, then retry {retry}")
                    try:
                        if admitted.pid == LITCH_APP_PID:
                            with bound.phase("ROM entry"):
                                rom = enter_s3_rom_from_application(inventory, application=admitted)
                        else:
                            with bound.phase("ROM entry"):
                                # A blank chip can briefly disappear after admission.
                                # One exact return lets the MAC probe enter its loader.
                                print(f"S3 USB: waiting for USB serial {admitted.identity.serial} at location "
                                      f"{admitted.identity.location}. Leave this board connected.", flush=True)
                                rom = wait_for_exact_usb_parent(
                                    inventory, identity=admitted.identity, vid=ESPRESSIF_VID,
                                    pid=ESP32S3_ROM_PID, timeout_seconds=5.0, stable_reads=1,
                                )
                                _probe_s3_rom_entry(bound, esptool_argv, dialect, rom)
                    except KeyboardInterrupt:
                        print(f"litch: stopped before flashing. {entry_guidance}", file=sys.stderr, flush=True)
                        raise
                    except (DeviceInventoryError, RuntimeVerificationError, OSError) as error:
                        raise DeviceInventoryError(
                            f"Bootloader entry failed before erase or write. {entry_guidance}. Cause: {error}"
                        ) from error
                    _flash_s3_usb(workspace, inventory, rom=rom, bound=bound,
                                  esptool_argv=esptool_argv, dialect=dialect,
                                  factory_provision=factory_provision)
            if target == "c3":
                _verify_c3_runtime(c3_context, workspace=workspace, runner=runner)
    except (DeviceInventoryError, RomFlashError, RuntimeVerificationError, OSError) as error:
        raise DevError(str(error)) from error
    finally:
        if target == "s3":
            bound.report_timings(time.monotonic() - started)


def _flash_ble_ota(
    *,
    workspace: Workspace,
    target: FlashTarget,
    runner: Runner,
    lock_timeout: float,
    build_target: BuildTarget,
) -> None:
    """Build S3 then delegate OTA transport to the established updater script."""

    from tools.ble_update_firmware import UPDATE_END_UNCERTAIN_EXIT_CODE, UPDATE_TIMEOUT_SECONDS

    assert target.transport == "ble" and target.target == "s3" and target.ble_identity
    firmware = platformio_build_directory(
        workspace.root, TARGET_ENVIRONMENTS["s3"]
    ) / "firmware.bin"
    manager = LockManager(workspace, argv=sys.argv)
    ble_lock = f"BLE:{normalize_device_identity(target.ble_identity)}"
    with manager.device(ble_lock, timeout=lock_timeout):
        with manager.lane("platformio", timeout=lock_timeout):
            build_target("s3")
            expected_version = read_firmware_header_version(
                workspace.root / FIRMWARE_VERSION_HEADER
            )
            firmware_metadata = inspect_firmware_image(firmware)
            update = runner.run(
                [
                    sys.executable,
                    workspace.root / "tools" / "ble_update_firmware.py",
                    "update",
                    firmware,
                    "--device",
                    target.ble_identity,
                ],
                cwd=workspace.root,
                timeout_seconds=UPDATE_TIMEOUT_SECONDS,
                check=False,
            )
            uncertain = update.returncode == UPDATE_END_UNCERTAIN_EXIT_CODE
            if update.returncode not in (0, UPDATE_END_UNCERTAIN_EXIT_CODE):
                raise DevError(f"BLE updater failed (exit {update.returncode}); no automatic retry was made.")
            try:
                verified = run_ble_runtime_bounded(
                    ble_factory=BleakControlFactory(),
                    ble_selector=target.ble_identity,
                    expected_version=expected_version,
                    expected_image_sha256=firmware_metadata.image_sha256,
                    require_ota_accepted=True,
                )
            except RuntimeVerificationError as error:
                prefix = "End delivery is uncertain; verification remains pending: " if uncertain else ""
                raise DevError(prefix + str(error)) from error
            if uncertain:
                raise DevError(
                    "End delivery is uncertain. The selected board currently reports "
                    f"version={verified.version} image_sha256={verified.image_sha256}, "
                    "but this does not prove that this update ran; verification remains pending."
                )
            print(
                "S3 BLE OTA verified: "
                f"version={verified.version} image_sha256={verified.image_sha256}"
            )


def _discover_usb_flash_candidates(
    workspace: Workspace,
    runner: Runner,
    lock_timeout: float,
    *,
    verbose: bool = False,
) -> tuple[FlashCandidate, ...]:
    bound = _BoundCaptureRunner(runner, cwd=workspace.root, verbose=verbose)
    try:
        with LockManager(workspace, argv=sys.argv).lane(
            "platformio", timeout=lock_timeout
        ):
            return _usb_flash_candidates(PioUsbInventory(bound).snapshot())
    except DeviceInventoryError as error:
        raise DevError(str(error)) from error


def discover_possible_usb_boards(
    workspace: Workspace, runner: Runner, lock_timeout: float,
) -> tuple[FlashCandidate, ...]:
    """List supported USB candidates, retaining incomplete identities as hints.

    Informational discovery never opens a port. Only complete parents can be
    selected; the existing exact workflow repeats admission before building.
    """

    bound = _BoundCaptureRunner(runner, cwd=workspace.root)
    try:
        with LockManager(workspace, argv=sys.argv).lane("platformio", timeout=lock_timeout):
            endpoints = PioUsbInventory(bound).endpoints()
        possible = tuple(endpoint for endpoint in endpoints
                         if endpoint.vid == ESPRESSIF_VID
                         and endpoint.pid in {LITCH_APP_PID, ESP32S3_ROM_PID})
        complete = tuple(endpoint for endpoint in possible
                         if endpoint.serial and endpoint.location)
        candidates = list(_usb_flash_candidates(normalize_usb_parents(complete)))
        candidates.extend(
            FlashCandidate(
                "usb", endpoint.serial or "",
                f"{endpoint.port} (USB serial or location unavailable)",
                ports=(endpoint.port,),
            )
            for endpoint in possible if not endpoint.serial or not endpoint.location
        )
        return tuple(candidates)
    except DeviceInventoryError as error:
        raise DevError(str(error)) from error


def _discover_flash_targets(
    *,
    workspace: Workspace,
    runner: Runner,
    lock_timeout: float,
    include_usb: bool,
    include_ble: bool,
    best_effort: bool = False,
    verbose: bool = False,
) -> tuple[tuple[FlashCandidate, ...], tuple[FlashCandidate, ...]]:
    usb_candidates: tuple[FlashCandidate, ...] = ()
    ble_candidates: tuple[FlashCandidate, ...] = ()
    if include_usb:
        try:
            usb_candidates = _discover_usb_flash_candidates(
                workspace,
                runner,
                lock_timeout,
                verbose=verbose,
            )
        except DevError as error:
            if not best_effort:
                raise
            print(f"Warning: USB discovery unavailable ({error}); continuing with available transports.")
    if include_ble:
        discovery = _discover_ble_candidates()
        try:
            ble_candidates = asyncio.run(discovery)
        except SystemExit as error:
            discovery.close()
            if not best_effort:
                raise DevError(f"BLE discovery failed: {error}") from error
            print(f"Warning: BLE discovery unavailable ({error}); continuing with available transports.")
        except Exception as error:
            discovery.close()
            if not best_effort:
                raise DevError(f"BLE discovery failed: {error}") from error
            print(f"Warning: BLE discovery unavailable ({error}); continuing with available transports.")
    return usb_candidates, ble_candidates


def _flash_options(
    usb_candidates: tuple[FlashCandidate, ...],
    ble_candidates: tuple[FlashCandidate, ...],
    requested_target: str | None = None,
) -> list[tuple[FlashTarget, str]]:
    options: list[tuple[FlashTarget, str]] = []
    effective_target = requested_target or "s3"
    for candidate in usb_candidates:
        eligible = (
            candidate.target_hint == "s3"
            if requested_target is None
            else candidate.target_hint in {None, requested_target}
        )
        targets = (effective_target,) if eligible else ()
        options.extend(
            (
                FlashTarget(target, "usb", candidate.identity),
                f"./litch flash {target} --device {candidate.identity}",
            )
            for target in targets
        )
    if effective_target == "s3":
        for candidate in ble_candidates:
            options.append(
                (
                    FlashTarget("s3", "ble", ble_identity=candidate.identity),
                    f"./litch flash s3 --ble-device {candidate.identity}",
                )
            )
    return options


def _explicit_flash_choices(
    usb_candidates: tuple[FlashCandidate, ...],
    ble_candidates: tuple[FlashCandidate, ...],
    requested_target: str | None = None,
    *,
    include_ble: bool = False,
) -> str:
    commands = [
        command
        for _, command in _flash_options(
            usb_candidates,
            ble_candidates,
            requested_target,
        )
    ]
    if not commands:
        if include_ble:
            return (
                "No eligible BLE Litch boards were discovered. Run './litch devices --ble' "
                "and select one explicitly with './litch flash s3 --ble-device <ble-address>'."
            )
        unidentified = tuple(candidate for candidate in usb_candidates if candidate.target_hint is None)
        if unidentified and requested_target is None:
            commands = [f"./litch flash s3 --device {candidate.identity}" for candidate in unidentified]
            return (
                "USB board model is not identified; for an S3 board in ROM mode, "
                "select one explicitly: " + "; ".join(commands)
            )
        return (
            "No eligible USB Litch boards were discovered. Run './litch devices' "
            f"and select one explicitly with './litch flash {requested_target or 's3'} --device <usb-serial>'."
        )
    return "Discovery is informational; choose one explicitly: " + "; ".join(commands)


def _prompt_flash_choice(
    usb_candidates: tuple[FlashCandidate, ...],
    ble_candidates: tuple[FlashCandidate, ...],
    requested_target: str | None = None,
    *,
    include_ble: bool = False,
) -> FlashTarget:
    options = _flash_options(usb_candidates, ble_candidates, requested_target)
    if not options:
        raise DevError(
            _explicit_flash_choices(
                usb_candidates,
                ble_candidates,
                requested_target,
                include_ble=include_ble,
            ),
            exit_code=2,
        )
    print("Choose the exact board to flash:")
    for index, (_, command) in enumerate(options, start=1):
        print(f"  {index}. {command}")
    while True:
        try:
            raw = input("Choice: ").strip()
        except EOFError:
            raise DevError(
                "No board selected; rerun a direct command: "
                + "; ".join(command for _, command in options),
                exit_code=2,
            ) from None
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1][0]
        print(f"Enter a number from 1 to {len(options)}.")


def _choose_factory_usb(
    candidates: tuple[FlashCandidate, ...], *, interactive: bool,
) -> tuple[FlashTarget, PhysicalUsbIdentity]:
    """A destructive implicit request needs a fresh exact board and erase choice."""
    if not candidates:
        raise DevError("No USB boards found. Connect the S3 board by USB and retry ./litch flash --factory.", exit_code=2)
    identities = [candidate.identity for candidate in candidates]
    eligible = tuple(candidate for candidate in candidates
                     if candidate.usb_parent is not None
                     and identities.count(candidate.identity) == 1
                     and candidate.target_hint in (None, "s3"))
    if not eligible:
        raise DevError("USB identities are incomplete or ambiguous; reconnect one S3 board and retry.", exit_code=2)
    commands = [f"./litch flash s3 --device {candidate.identity} --factory" for candidate in eligible]
    if not interactive:
        raise DevError("Factory erase needs an exact USB board. Choose one explicitly: " + "; ".join(commands), exit_code=2)
    print("Factory provisioning erases all settings, saved games and artwork on one board.")
    print("Select only an S3 board. Enter its number to erase it; q cancels:")
    for index, candidate in enumerate(eligible, 1):
        model = "S3" if candidate.target_hint == "s3" else "S3 (model not identified by USB)"
        print(f"  {index}. Erase {model}: {candidate.display}")
    while True:
        try:
            raw = input("Board number [q cancels]: ").strip()
        except EOFError:
            raw = "q"
        if raw.lower() == "q" or not raw:
            raise DevError("Factory provisioning cancelled; nothing was erased.", exit_code=2)
        if raw.isascii() and raw.isdigit() and 1 <= int(raw) <= len(eligible):
            candidate = eligible[int(raw) - 1]
            break
        print(f"Enter a number from 1 to {len(eligible)}, or q to cancel.")
    assert candidate.usb_parent is not None
    return FlashTarget("s3", "usb", candidate.identity), candidate.usb_parent.identity


def flash_selected_target(
    *,
    workspace: Workspace,
    requested_target: str | None,
    usb_selector: str | None,
    ble_selector: str | None,
    runner: Runner,
    lock_timeout: float,
    build_target: BuildTarget,
    platformio_core_dir: CoreDirectory,
    interactive: bool = False,
    verbose: bool = False,
    factory_provision: bool = False,
    expected_usb_identity: PhysicalUsbIdentity | None = None,
) -> None:
    """Select an available board, execute exactly one transport, then remember it."""

    _validate_flash_request(requested_target, usb_selector, ble_selector)
    if factory_provision and ble_selector is not None:
        raise DevError(
            "--factory-provision is only valid for S3 USB flash; connect the board by USB",
            exit_code=2,
        )
    if factory_provision and requested_target not in (None, "s3"):
        raise DevError("--factory-provision is only valid for S3 USB flash", exit_code=2)
    platformio_build_directory(workspace.root, TARGET_ENVIRONMENTS[requested_target or "s3"])
    state_path = _local_state_path(workspace)
    remembered = (
        _remembered_flash(state_path, requested_target or "s3")
        if usb_selector is None and ble_selector is None and not factory_provision
        else None
    )
    had_remembered_target = remembered is not None
    if remembered is not None:
        include_usb = remembered.transport == "usb"
        include_ble = remembered.transport == "ble"
    else:
        include_usb = ble_selector is None
        include_ble = ble_selector is not None
    implicit_request = usb_selector is None and ble_selector is None
    usb_candidates, ble_candidates = _discover_flash_targets(
        workspace=workspace,
        runner=runner,
        lock_timeout=lock_timeout,
        include_usb=include_usb,
        include_ble=include_ble,
        best_effort=implicit_request and not factory_provision,
        verbose=verbose,
    )
    # An invocation that discovers BLE must remain on BLE; USB selection
    # never scans another transport, including stale preferences and prompts.
    usb_candidates = usb_candidates if include_usb else ()
    ble_candidates = ble_candidates if include_ble else ()
    if remembered is not None:
        candidates = usb_candidates if remembered.transport == "usb" else ble_candidates
        identity = remembered.usb_identity if remembered.transport == "usb" else remembered.ble_identity
        remembered = remembered if any(
            _candidate_identity_matches(item, identity)
            for item in candidates
        ) else None
    selected: FlashTarget | None = None
    if factory_provision and implicit_request:
        selected, expected_usb_identity = _choose_factory_usb(usb_candidates, interactive=interactive)
    if implicit_request and remembered is None and had_remembered_target:
        if interactive:
            selected = _prompt_flash_choice(
                usb_candidates, ble_candidates, requested_target, include_ble=include_ble,
            )
        else:
            raise DevError(
                "the remembered flash device is no longer available. "
                + _explicit_flash_choices(usb_candidates, ble_candidates, requested_target, include_ble=include_ble),
                exit_code=2,
            )
    if selected is None:
        try:
            selected = resolve_flash_target(
                requested_target=requested_target,
                usb_selector=usb_selector,
                ble_selector=ble_selector,
                usb_candidates=usb_candidates,
                ble_candidates=ble_candidates,
                remembered=remembered,
            )
        except DevError as error:
            if not implicit_request:
                raise
            if interactive:
                selected = _prompt_flash_choice(
                    usb_candidates, ble_candidates, requested_target, include_ble=include_ble,
                )
            else:
                raise DevError(
                    (f"{error}. " if usb_candidates or ble_candidates else "")
                    + _explicit_flash_choices(usb_candidates, ble_candidates, requested_target, include_ble=include_ble),
                    exit_code=2,
                ) from error
    assert selected is not None
    if expected_usb_identity is not None:
        matches = [candidate for candidate in usb_candidates
                   if _candidate_identity_matches(candidate, selected.usb_identity)]
        if (selected.transport != "usb" or len(matches) != 1
                or matches[0].usb_parent is None
                or matches[0].usb_parent.identity != expected_usb_identity):
            raise DevError("selected USB identity changed; list devices and select again before flashing", exit_code=2)
    if factory_provision and (selected.target != "s3" or selected.transport != "usb"):
        raise DevError("--factory-provision is only valid for S3 USB flash", exit_code=2)
    identity = selected.usb_identity or selected.ble_identity
    transport = selected.transport.upper()
    if implicit_request and remembered is not None:
        selector = "--device <usb-serial>" if selected.transport == "usb" else "--ble-device <ble-address>"
        print(
            f"litch: using remembered {selected.target.upper()} over {transport} -> {identity} "
            f"(override: ./litch flash {selected.target} {selector})",
            flush=True,
        )
    else:
        print(
            f"litch: selected {selected.target.upper()} over {transport} -> {identity}",
            flush=True,
        )
    print(
        f"litch: starting {selected.target.upper()} firmware build before {transport} write",
        flush=True,
    )
    if selected.transport == "usb":
        assert selected.usb_identity is not None
        flash_exact_device(
            workspace=workspace,
            target=selected.target,
            usb_identity=selected.usb_identity,
            ble_identity=selected.ble_identity,
            runner=runner,
            lock_timeout=lock_timeout,
            build_target=build_target,
            platformio_core_dir=platformio_core_dir,
            verbose=verbose,
            factory_provision=factory_provision,
            expected_usb_identity=expected_usb_identity,
        )
    else:
        _flash_ble_ota(
            workspace=workspace,
            target=selected,
            runner=runner,
            lock_timeout=lock_timeout,
            build_target=build_target,
        )
    print(
        f"litch: flash complete: {selected.target.upper()} over {transport} -> {identity}",
        flush=True,
    )
    if factory_provision:
        print(
            "litch: factory validation is now required. V3 starts hardware test "
            "automatically; on V2 run `hwtest start`. Open "
            f"`./litch monitor s3 --device {identity}`, run `hwtest status`, "
            "and do not accept the board until the full-green result is "
            "durably saved and, on V3, provisioning is passed. Use `hwtest "
            "restart` after a "
            "failed or expired run.",
            flush=True,
        )
    _remember_flash(state_path, selected)


def _monitor_options(
    candidates: Sequence[FlashCandidate],
    requested_target: str | None = None,
) -> list[tuple[str, FlashCandidate, str]]:
    options: list[tuple[str, FlashCandidate, str]] = []
    effective_target = requested_target or "s3"
    for candidate in candidates:
        eligible = (
            candidate.target_hint == "s3"
            if requested_target is None
            else candidate.target_hint in {None, requested_target}
        )
        targets = (effective_target,) if eligible else ()
        options.extend(
            (
                target,
                candidate,
                f"./litch monitor {target} --device {candidate.identity}",
            )
            for target in targets
        )
    return options


def _monitor_choices(
    candidates: Sequence[FlashCandidate],
    requested_target: str | None = None,
) -> str:
    commands = [
        command
        for _, _, command in _monitor_options(candidates, requested_target)
    ]
    if not commands:
        return "no eligible USB boards are available; run './litch devices'"
    return "choose one: " + "; ".join(commands)


def monitor_selected_target(
    *,
    workspace: Workspace,
    requested_target: str | None,
    usb_selector: str | None,
    runner: Runner,
    lock_timeout: float,
    interactive: bool,
) -> None:
    """Resolve one exact USB monitor target, reusing a successful flash choice."""

    if usb_selector is not None and _bounded_identity(usb_selector) is None:
        raise DevError("USB selector must be non-empty and at most 256 characters", exit_code=2)
    candidates = _discover_usb_flash_candidates(workspace, runner, lock_timeout)
    remembered = (
        _remembered_flash(
            _local_state_path(workspace), requested_target or "s3"
        )
        if usb_selector is None
        else None
    )
    used_remembered = False
    selected_candidate: FlashCandidate | None = None
    target = requested_target
    if remembered is not None and remembered.transport == "usb":
        selected_candidate = next(
            (
                candidate
                for candidate in candidates
                if _candidate_identity_matches(candidate, remembered.usb_identity)
            ),
            None,
        )
        if selected_candidate is not None:
            target = target or remembered.target
            used_remembered = True
    elif usb_selector is not None:
        selected_candidate = _resolve_unique_candidate(
            candidates,
            usb_selector,
            transport="USB",
        )
        target = target or selected_candidate.target_hint

    if (
        selected_candidate is None
        and target is None
        and len(candidates) == 1
        and candidates[0].target_hint == "s3"
    ):
        selected_candidate = candidates[0]
        target = selected_candidate.target_hint

    if (selected_candidate is None or target not in TARGET_ENVIRONMENTS) and interactive:
        options = _monitor_options(candidates, requested_target)
        if options:
            print("Choose the exact board to monitor:")
            for index, (_, _, command) in enumerate(options, start=1):
                print(f"  {index}. {command}")
            while True:
                raw = input("Choice: ").strip()
                if raw.isdigit() and 1 <= int(raw) <= len(options):
                    target, selected_candidate, _ = options[int(raw) - 1]
                    break
                print(f"Enter a number from 1 to {len(options)}.")
    if selected_candidate is None or target not in TARGET_ENVIRONMENTS:
        raise DevError(_monitor_choices(candidates, requested_target), exit_code=2)
    if used_remembered:
        print(
            f"litch: using remembered {target.upper()} USB board -> {selected_candidate.identity} "
            f"(override: ./litch monitor {target} --device <usb-serial>)",
            flush=True,
        )
    else:
        print(
            f"litch: monitoring {target.upper()} over USB -> {selected_candidate.identity}",
            flush=True,
        )
    monitor_exact_device(
        workspace=workspace,
        target=target,
        usb_identity=selected_candidate.identity,
        runner=runner,
        lock_timeout=lock_timeout,
    )


def monitor_exact_device(
    *,
    workspace: Workspace,
    target: str,
    usb_identity: str,
    runner: Runner,
    lock_timeout: float,
) -> None:
    if target not in TARGET_ENVIRONMENTS:
        raise DevError(f"unsupported hardware target: {target}", exit_code=2)
    try:
        _selector_key(usb_identity)
    except DeviceInventoryError as error:
        raise DevError(str(error), exit_code=2) from error
    expected_pid = ESP32S3_ROM_PID if target == "c3" else LITCH_APP_PID
    # Assert both lines for deployed firmware whose Arduino CDC transport
    # requires them. Current S3 firmware also accepts replies after host RX
    # with inactive lines, but the monitor must remain usable with older images.
    monitor_dtr = "1" if target == "s3" else "0"
    monitor_rts = "1" if target == "s3" else "0"
    manager = LockManager(workspace, argv=sys.argv)
    bound = _BoundCaptureRunner(runner, cwd=workspace.root)
    try:
        with manager.device(usb_identity, timeout=lock_timeout):
            parent = select_parent_by_serial(
                PioUsbInventory(bound).snapshot(),
                serial=usb_identity,
                vid=ESPRESSIF_VID,
                pid=expected_pid,
            )
            runner.run(
                [
                    "pio",
                    "device",
                    "monitor",
                    "--port",
                    parent.port,
                    "--baud",
                    "115200",
                    "--dtr",
                    monitor_dtr,
                    "--rts",
                    monitor_rts,
                    "--filter",
                    "direct",
                    "--no-reconnect",
                ],
                cwd=workspace.root,
                timeout_seconds=None,
                interactive=True,
                extra_env=PIO_ENVIRONMENT,
            )
    except DeviceInventoryError as error:
        raise DevError(str(error)) from error
