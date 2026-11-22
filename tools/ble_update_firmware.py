#!/usr/bin/env python3

from __future__ import annotations

import argparse
import asyncio
import datetime
import json
import platform
import re
import sys
import time
import zlib
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


LITCH_NAME_PREFIX = "Foxie_Litch"
BASE_SERVICE_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0100"
COMMAND_CHARACTERISTIC_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0101"
CONSOLE_STREAM_CHARACTERISTIC_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0102"
SETTINGS_SERVICE_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0101"
OTA_CONTROL_CHARACTERISTIC_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0104"
OTA_DATA_CHARACTERISTIC_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0105"

DEFAULT_TIMEOUT_SECONDS = 8.0
CONNECT_CLEANUP_TIMEOUT_SECONDS = 2.0
UPDATE_TIMEOUT_SECONDS = 15 * 60
UPDATE_END_UNCERTAIN_EXIT_CODE = 75
DEFAULT_SCAN_SECONDS = 1.0
DEFAULT_BURST_LIMIT = 48
DEFAULT_BEGIN_ATTEMPTS = 12
DEFAULT_RETRY_DELAY_SECONDS = 0.25
DEFAULT_PRE_BEGIN_DELAY_SECONDS = 0.9
DEFAULT_FINAL_SETTLE_SECONDS = 0.09
DEFAULT_SIMULATION_BYTES = 512 * 1024
DEFAULT_MAX_ATT_PAYLOAD_BYTES = 512
DEFAULT_FAST_DATA_LEN_OCTETS = 251
DEFAULT_RECOMMENDED_OTA_CHUNK_BYTES = DEFAULT_FAST_DATA_LEN_OCTETS - 7
DEFAULT_BENCH_DELAY_MS = 0
DEFAULT_BENCH_CHUNK_SIZES = (64, 96, 128, 160, 180, 200, 244, 384, 512)
DEFAULT_BENCH_BURSTS = (1, 4, 8, 12, 16, 24, 32, 48)
DEFAULT_NOTIFY_TEST_SIZES = (20, 64, 100, 180, 244, 384, 512)
DEFAULT_NOTIFY_TEST_PACKET_COUNT = 200
DEFAULT_CONSOLE_BLE_TEST_BUCKETS = 8
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCAL_FIRMWARE_BIN = REPO_ROOT / ".pio" / "build" / "litch-esp32s3" / "firmware.bin"
DEVICE_CACHE_PATH = REPO_ROOT / ".cache" / "ble_update_firmware_device.json"

NOTIFY_TEST_DETAIL_RE = re.compile(
    r"size=(?P<size>\d+) ok=(?P<ok>\d+) fail=(?P<fail>\d+) elapsed_ms=(?P<elapsed>\d+) rate_Bps=(?P<bps>\d+)"
)
CONSOLE_BLE_TEST_AGGREGATE_RE = re.compile(
    r"ble test: aggregate bytes=(?P<bytes>\d+) elapsed_ms=(?P<elapsed>\d+) aggregate_Bps=(?P<bps>\d+)",
    re.IGNORECASE,
)
CONSOLE_BLE_TEST_AGGREGATE_TAIL_RE = re.compile(
    r"(?:aggregate_Bps)?=(?P<bps>\d+)$",
    re.IGNORECASE,
)


def describe_unsupported_command(command_name: str) -> str:
    return (
        f"firmware does not support {command_name}; install newer firmware from this repo first"
    )


def require_bleak() -> Tuple[Any, Any, Any]:
    try:
        from bleak import BleakClient, BleakScanner  # type: ignore
        from bleak.exc import BleakError  # type: ignore
    except ImportError as exc:
        raise SystemExit(
            "bleak is required. Install it with: .venv/bin/python -m pip install bleak"
        ) from exc
    return BleakClient, BleakScanner, BleakError


def format_seconds(seconds: float) -> str:
    if seconds < 1.0:
        return f"{seconds * 1000.0:.0f}ms"
    return f"{seconds:.2f}s"


def format_timestamp_now() -> str:
    now = datetime.datetime.now()
    return now.strftime("%H:%M:%S.") + f"{int(now.microsecond / 1000):03d}"


def format_hms(total_seconds: Optional[int]) -> str:
    if total_seconds is None:
        return "--:--"
    clamped = max(0, int(total_seconds))
    minutes, seconds = divmod(clamped, 60)
    hours, minutes = divmod(minutes, 60)
    if hours > 0:
        return f"{hours:02d}:{minutes:02d}:{seconds:02d}"
    return f"{minutes:02d}:{seconds:02d}"


def safe_int(value: Any) -> int:
    try:
        return int(value)
    except Exception:
        return 0


def normalize_uuid(value: str) -> str:
    return str(value).lower()


def host_default_chunk_size() -> int:
    return DEFAULT_RECOMMENDED_OTA_CHUNK_BYTES


@dataclass(frozen=True)
class CandidateDevice:
    name: str
    address: str
    rssi: int
    service_uuids: Tuple[str, ...]
    match_reason: str

    @property
    def display_name(self) -> str:
        short_id = self.address[-5:] if len(self.address) > 5 else self.address
        return f"{self.name} • {short_id}"


def load_cached_candidate(selector: Optional[str]) -> Optional[CandidateDevice]:
    try:
        raw = json.loads(DEVICE_CACHE_PATH.read_text())
    except Exception:
        return None
    if not isinstance(raw, dict):
        return None

    name = str(raw.get("name") or LITCH_NAME_PREFIX)
    address = str(raw.get("address") or "").strip()
    if not address:
        return None
    if selector:
        lowered = selector.lower()
        if lowered not in address.lower() and lowered not in name.lower():
            return None

    service_uuids = raw.get("service_uuids") or []
    if not isinstance(service_uuids, list):
        service_uuids = []
    return CandidateDevice(
        name=name,
        address=address,
        rssi=0,
        service_uuids=tuple(str(item) for item in service_uuids),
        match_reason="cache",
    )


def save_cached_candidate(candidate: CandidateDevice) -> None:
    payload = {
        "name": candidate.name,
        "address": candidate.address,
        "service_uuids": list(candidate.service_uuids),
    }
    try:
        DEVICE_CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
        DEVICE_CACHE_PATH.write_text(json.dumps(payload, separators=(",", ":")))
    except Exception:
        pass


@dataclass(frozen=True)
class TransferConfig:
    chunk_size: Optional[int]
    mode: str
    burst_limit: int
    begin_attempts: int
    retry_delay_seconds: float
    pre_begin_delay_seconds: float
    final_settle_seconds: float
    terminal_timeout_seconds: float


class OtaOutcomeUnknown(RuntimeError):
    """A sent side effect has no confirmed terminal outcome."""


@dataclass
class HostTransferProfile:
    data_write_calls: int = 0
    data_write_no_response_calls: int = 0
    data_write_response_calls: int = 0
    data_write_total_ms: float = 0.0
    data_write_max_ms: float = 0.0
    control_write_calls: int = 0
    control_write_total_ms: float = 0.0
    control_write_max_ms: float = 0.0
    wait_for_ota_event_calls: int = 0
    wait_for_ota_event_total_ms: float = 0.0
    wait_for_ota_event_max_ms: float = 0.0
    wait_for_board_rx_calls: int = 0
    wait_for_board_rx_total_ms: float = 0.0
    wait_for_board_rx_max_ms: float = 0.0
    wait_for_terminal_calls: int = 0
    wait_for_terminal_total_ms: float = 0.0
    wait_for_terminal_max_ms: float = 0.0
    sleep_pre_begin_ms: float = 0.0
    sleep_final_settle_ms: float = 0.0
    progress_notify_count: int = 0
    progress_notify_total_advance: int = 0
    progress_notify_max_gap_ms: float = 0.0
    _last_progress_notify_at: float = 0.0
    _last_progress_rx: int = 0

    def note_control_write(self, elapsed_ms: float) -> None:
        self.control_write_calls += 1
        self.control_write_total_ms += elapsed_ms
        self.control_write_max_ms = max(self.control_write_max_ms, elapsed_ms)

    def note_data_write(self, elapsed_ms: float, response: bool) -> None:
        self.data_write_calls += 1
        if response:
            self.data_write_response_calls += 1
        else:
            self.data_write_no_response_calls += 1
        self.data_write_total_ms += elapsed_ms
        self.data_write_max_ms = max(self.data_write_max_ms, elapsed_ms)

    def note_wait_for_ota_event(self, elapsed_ms: float) -> None:
        self.wait_for_ota_event_calls += 1
        self.wait_for_ota_event_total_ms += elapsed_ms
        self.wait_for_ota_event_max_ms = max(self.wait_for_ota_event_max_ms, elapsed_ms)

    def note_wait_for_board_rx(self, elapsed_ms: float) -> None:
        self.wait_for_board_rx_calls += 1
        self.wait_for_board_rx_total_ms += elapsed_ms
        self.wait_for_board_rx_max_ms = max(self.wait_for_board_rx_max_ms, elapsed_ms)

    def note_wait_for_terminal(self, elapsed_ms: float) -> None:
        self.wait_for_terminal_calls += 1
        self.wait_for_terminal_total_ms += elapsed_ms
        self.wait_for_terminal_max_ms = max(self.wait_for_terminal_max_ms, elapsed_ms)

    def note_progress_notify(self, rx: int) -> None:
        now = time.monotonic()
        if self._last_progress_notify_at > 0.0:
            gap_ms = (now - self._last_progress_notify_at) * 1000.0
            self.progress_notify_max_gap_ms = max(self.progress_notify_max_gap_ms, gap_ms)
        self._last_progress_notify_at = now
        if rx > self._last_progress_rx:
            self.progress_notify_total_advance += rx - self._last_progress_rx
            self._last_progress_rx = rx
        self.progress_notify_count += 1


class ConsoleLineBuffer:
    def __init__(self) -> None:
        self._partial = ""

    def feed(self, payload: bytes) -> List[str]:
        if likely_binary_payload(payload):
            self._partial = ""
            return []
        text = payload.decode("utf-8", errors="replace")
        text = self._partial + text
        normalized = text.replace("\r\n", "\n").replace("\r", "\n")
        parts = normalized.split("\n")
        self._partial = parts.pop() if parts else ""
        return [line.strip() for line in parts if line.strip()]


class LitchBleSession:
    def __init__(self, client: Any, device: CandidateDevice) -> None:
        self._client = client
        self._device = device
        self._ota_queue: "asyncio.Queue[Dict[str, Any]]" = asyncio.Queue()
        self._console_queue: "asyncio.Queue[str]" = asyncio.Queue()
        self._console_buffer = ConsoleLineBuffer()
        self._disconnect_event = asyncio.Event()
        self._subscribed_console = False
        self._subscribed_ota = False
        self._firmware_version: Optional[str] = None
        self._last_ota_state = "idle"
        self._last_ota_progress_percent = 0
        self._last_ota_rx = 0
        self._last_ota_total = 0
        self._session_started_at = time.monotonic()
        self._active_transfer_profile: Optional[HostTransferProfile] = None
        self._ota_abort_task: Optional[asyncio.Task[bool]] = None
        self._command_char = self._resolve_characteristic(
            BASE_SERVICE_UUID,
            COMMAND_CHARACTERISTIC_UUID,
        )
        self._console_char = self._resolve_characteristic(
            BASE_SERVICE_UUID,
            CONSOLE_STREAM_CHARACTERISTIC_UUID,
        )
        self._ota_control_char = self._resolve_characteristic(
            BASE_SERVICE_UUID,
            OTA_CONTROL_CHARACTERISTIC_UUID,
        )
        self._ota_data_char = self._resolve_characteristic(
            BASE_SERVICE_UUID,
            OTA_DATA_CHARACTERISTIC_UUID,
        )

    @property
    def device(self) -> CandidateDevice:
        return self._device

    @property
    def mtu_size(self) -> int:
        mtu = getattr(self._client, "mtu_size", 23)
        try:
            value = int(mtu)
        except Exception:
            return 23
        return value if value > 0 else 23

    def _log(self, message: str) -> None:
        elapsed = time.monotonic() - self._session_started_at
        print(f"[{format_timestamp_now()} +{elapsed:7.3f}s] {message}", file=sys.stderr, flush=True)

    async def start_notifications(self) -> None:
        if not self._subscribed_console:
            await self._client.start_notify(
                self._console_char,
                self._handle_console_notification,
            )
            self._subscribed_console = True
        if not self._subscribed_ota:
            await self._client.start_notify(
                self._ota_control_char,
                self._handle_ota_notification,
            )
            self._subscribed_ota = True

    async def stop_notifications(self) -> None:
        if self._subscribed_console:
            try:
                await self._client.stop_notify(self._console_char)
            except Exception:
                pass
            self._subscribed_console = False
        if self._subscribed_ota:
            try:
                await self._client.stop_notify(self._ota_control_char)
            except Exception:
                pass
            self._subscribed_ota = False

    def _handle_console_notification(self, _: Any, payload: bytearray) -> None:
        for line in self._console_buffer.feed(bytes(payload)):
            self._console_queue.put_nowait(line)

    def _handle_ota_notification(self, _: Any, payload: bytearray) -> None:
        event = decode_ota_status(bytes(payload))
        state = str(event.get("state") or "raw")
        total = safe_int(event.get("total"))
        if total <= 0:
            total = self._last_ota_total
        rx = safe_int(event.get("rx"))
        progress_percent = safe_int(event.get("progress_percent"))
        self._last_ota_state = state
        if state == "begin":
            self._last_ota_progress_percent = 0
        elif state == "progress":
            self._last_ota_progress_percent = max(self._last_ota_progress_percent, progress_percent)
        elif state == "done":
            self._last_ota_progress_percent = 100
        if state == "begin":
            self._last_ota_rx = rx
            self._last_ota_total = total
        else:
            self._last_ota_rx = max(self._last_ota_rx, rx)
            self._last_ota_total = max(self._last_ota_total, total)
        if state != "progress":
            raw_payload = event.get("raw")
            if state == "raw":
                self._log(f"ota notify state=raw payload={raw_payload!r}")
            else:
                self._log(f"ota notify state={state} detail={event.get('detail')} rx={self._last_ota_rx} total={self._last_ota_total}")
        elif self._active_transfer_profile is not None:
            self._active_transfer_profile.note_progress_notify(self._last_ota_rx)
        self._ota_queue.put_nowait(event)

    async def write_command_line(self, line: str) -> None:
        self._log(f"console tx line={line}")
        await self._client.write_gatt_char(
            self._command_char,
            (line.rstrip() + "\n").encode("utf-8"),
            response=True,
        )

    async def write_ota_control_json(
        self,
        payload: Dict[str, Any],
        *,
        attempts: int,
        retry_delay_seconds: float,
        allow_no_response: bool,
    ) -> bool:
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self._log(f"ota control tx cmd={payload.get('cmd')} bytes={len(encoded)} attempts={attempts}")
        for attempt in range(1, attempts + 1):
            try:
                started_at = time.monotonic()
                await self._client.write_gatt_char(
                    self._ota_control_char,
                    encoded,
                    response=True,
                )
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_control_write((time.monotonic() - started_at) * 1000.0)
                return True
            except Exception:
                if allow_no_response:
                    try:
                        started_at = time.monotonic()
                        await self._client.write_gatt_char(
                            self._ota_control_char,
                            encoded,
                            response=False,
                        )
                        if self._active_transfer_profile is not None:
                            self._active_transfer_profile.note_control_write((time.monotonic() - started_at) * 1000.0)
                        return True
                    except Exception:
                        pass
            if attempt < attempts:
                await asyncio.sleep(retry_delay_seconds)
        return False

    async def wait_for_ota_event(
        self,
        *,
        timeout_seconds: float,
        predicate: Optional[Any] = None,
    ) -> Dict[str, Any]:
        started_at = time.monotonic()
        deadline = time.monotonic() + timeout_seconds
        while True:
            if self._disconnect_event.is_set():
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_wait_for_ota_event((time.monotonic() - started_at) * 1000.0)
                return {"state": "disconnected"}
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                synthetic = self._snapshot_matching_ota_event(predicate)
                if synthetic is not None:
                    self._log(f"ota wait timeout recovered via snapshot state={synthetic.get('state')}")
                    if self._active_transfer_profile is not None:
                        self._active_transfer_profile.note_wait_for_ota_event((time.monotonic() - started_at) * 1000.0)
                    return synthetic
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_wait_for_ota_event((time.monotonic() - started_at) * 1000.0)
                return {"state": "timeout"}
            try:
                event = await asyncio.wait_for(self._ota_queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                synthetic = self._snapshot_matching_ota_event(predicate)
                if synthetic is not None:
                    self._log(f"ota queue timeout recovered via snapshot state={synthetic.get('state')}")
                    if self._active_transfer_profile is not None:
                        self._active_transfer_profile.note_wait_for_ota_event((time.monotonic() - started_at) * 1000.0)
                    return synthetic
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_wait_for_ota_event((time.monotonic() - started_at) * 1000.0)
                return {"state": "timeout"}
            if predicate is None or predicate(event):
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_wait_for_ota_event((time.monotonic() - started_at) * 1000.0)
                return event

    def _snapshot_matching_ota_event(self, predicate: Optional[Any]) -> Optional[Dict[str, Any]]:
        if self._last_ota_state == "idle":
            return None
        event = {
            "state": self._last_ota_state,
            "detail": None,
            "progress_percent": self._last_ota_progress_percent,
            "rx": self._last_ota_rx,
            "total": self._last_ota_total,
        }
        if predicate is None or predicate(event):
            return event
        return None

    async def wait_for_console_line(
        self,
        *,
        timeout_seconds: float,
        predicate: Optional[Any] = None,
    ) -> Optional[str]:
        deadline = time.monotonic() + timeout_seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                line = await asyncio.wait_for(self._console_queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                return None
            if predicate is None or predicate(line):
                return line

    def clear_queues(self) -> None:
        drain_queue(self._ota_queue)
        drain_queue(self._console_queue)
        self._last_ota_state = "idle"
        self._last_ota_progress_percent = 0
        self._last_ota_rx = 0
        self._last_ota_total = 0

    async def wait_for_board_rx(self, expected_rx: int, timeout_seconds: float) -> Dict[str, Any]:
        started_at = time.monotonic()
        if self._last_ota_rx >= expected_rx:
            if self._active_transfer_profile is not None:
                self._active_transfer_profile.note_wait_for_board_rx((time.monotonic() - started_at) * 1000.0)
            return {"state": "progress", "rx": self._last_ota_rx, "total": self._last_ota_total}

        deadline = time.monotonic() + timeout_seconds
        while self._last_ota_rx < expected_rx:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_wait_for_board_rx((time.monotonic() - started_at) * 1000.0)
                return {"state": "timeout", "rx": self._last_ota_rx, "total": self._last_ota_total}
            event = await self.wait_for_ota_event(
                timeout_seconds=remaining,
                predicate=lambda item: (
                    item.get("state")
                    in {"progress", "error", "aborted", "done", "timeout", "disconnected"}
                    or safe_int(item.get("rx")) >= expected_rx
                ),
            )
            if event.get("state") in {"error", "aborted", "done", "timeout", "disconnected"}:
                if self._active_transfer_profile is not None:
                    self._active_transfer_profile.note_wait_for_board_rx((time.monotonic() - started_at) * 1000.0)
                return event

        if self._active_transfer_profile is not None:
            self._active_transfer_profile.note_wait_for_board_rx((time.monotonic() - started_at) * 1000.0)
        return {"state": "progress", "rx": self._last_ota_rx, "total": self._last_ota_total}

    async def wait_for_progress_target(self, target_percent: int, timeout_seconds: float) -> Dict[str, Any]:
        clamped_target = max(0, min(target_percent, 100))
        if self._last_ota_progress_percent >= clamped_target:
            return {"state": "progress", "progress_percent": self._last_ota_progress_percent}

        deadline = time.monotonic() + timeout_seconds
        while self._last_ota_progress_percent < clamped_target:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return {"state": "timeout", "progress_percent": self._last_ota_progress_percent}
            event = await self.wait_for_ota_event(
                timeout_seconds=remaining,
                predicate=lambda item: item.get("state")
                in {"progress", "done", "error", "aborted", "timeout", "disconnected"},
            )
            if event.get("state") == "progress" and self._last_ota_progress_percent >= clamped_target:
                return event
            if event.get("state") in {"done", "error", "aborted", "timeout", "disconnected"}:
                return event

        return {"state": "progress", "progress_percent": self._last_ota_progress_percent}

    def derive_auto_chunk_size(self) -> int:
        mtu_payload = max(20, min(self.mtu_size - 3, DEFAULT_MAX_ATT_PAYLOAD_BYTES))
        recommended_payload = min(mtu_payload, DEFAULT_RECOMMENDED_OTA_CHUNK_BYTES)
        max_write = getattr(self._ota_data_char, "max_write_without_response_size", None)
        try:
            if max_write is not None:
                max_payload = int(max_write)
                if max_payload > 0:
                    return max(20, min(max_payload, recommended_payload))
        except Exception:
            pass
        if mtu_payload > 20:
            return recommended_payload
        return min(mtu_payload, host_default_chunk_size())

    def _resolve_characteristic(self, service_uuid: str, characteristic_uuid: str) -> Any:
        services = getattr(self._client, "services", None)
        if services is None:
            raise RuntimeError("BLE services have not been discovered yet.")

        target_service = normalize_uuid(service_uuid)
        target_characteristic = normalize_uuid(characteristic_uuid)
        for service in services:
            if normalize_uuid(service.uuid) != target_service:
                continue
            for characteristic in getattr(service, "characteristics", []):
                if normalize_uuid(characteristic.uuid) == target_characteristic:
                    return characteristic

        raise RuntimeError(
            f"Characteristic {characteristic_uuid} in service {service_uuid} was not found."
        )

    async def fetch_fw_version(self) -> Optional[str]:
        self.clear_queues()
        ok = await self.write_ota_control_json(
            {"cmd": "fw_version"},
            attempts=3,
            retry_delay_seconds=DEFAULT_RETRY_DELAY_SECONDS,
            allow_no_response=True,
        )
        if not ok:
            return None
        event = await self.wait_for_ota_event(
            timeout_seconds=3.0,
            predicate=lambda item: item.get("state") in {"version", "error", "timeout"},
        )
        if event.get("state") == "version":
            self._firmware_version = str(event.get("detail") or "").strip() or None
            return self._firmware_version
        return None

    async def require_current_ota_profile(
        self, *, mode: str, size: int, config: TransferConfig,
    ) -> Tuple[TransferConfig, int]:
        # Info is the same compact pre-session capability record as the app.
        info = None
        for attempt in range(3):
            self.clear_queues()
            sent = await self.write_ota_control_json(
                {"cmd": "info"}, attempts=1, retry_delay_seconds=0,
                allow_no_response=False,
            )
            if sent:
                event = await self.wait_for_ota_event(
                    timeout_seconds=1.2,
                    predicate=lambda item: (
                        isinstance(item.get("raw"), dict) and "i" in item["raw"]
                    ) or item.get("state") in {"error", "disconnected"},
                )
                raw = event.get("raw")
                values = raw.get("i") if isinstance(raw, dict) else None
                if (isinstance(values, list) and len(values) in (3, 4)
                        and all(type(value) is int for value in values)
                        and values[0] == 3 and 0 <= values[1] <= 255
                        and 0 <= values[2] <= 0xffff
                        and (len(values) == 3 or values[3] in (0, 1))
                        and (len(values) == 3 or values[3] != 1
                             or not values[2] & ~(0x08 | 0x10))):
                    info = values
                    break
            if attempt < 2:
                await asyncio.sleep(0.18)
        if info is None or not (info[2] & 0x10):
            raise RuntimeError("Current firmware-update capability could not be verified; no transfer started.")

        self.clear_queues()
        sent = await self.write_ota_control_json(
            {"cmd": "ota_probe", "mode": mode, "size": size},
            attempts=1, retry_delay_seconds=0, allow_no_response=False,
        )
        if not sent:
            raise RuntimeError("OTA protocol probe failed; no transfer started.")
        event = await self.wait_for_ota_event(
            timeout_seconds=1.5,
            predicate=lambda item: item.get("state") in {"probe", "error", "timeout", "disconnected"},
        )
        probe = event.get("raw")
        if event.get("state") in {"timeout", "error", "disconnected"} and info[2] & 0x200:
            raise RuntimeError(
                "The board requires owner admission; this maintenance client cannot establish it. "
                "Use the companion app or USB recovery; no transfer started."
            )
        fields = ("proto", "size", "max_chunk", "chunk", "burst", "window")
        if (not isinstance(probe, dict) or any(type(probe.get(key)) is not int for key in fields)
                or probe["proto"] != 2 or probe.get("mode") != mode or probe["size"] != size
                or not 20 <= probe["chunk"] <= probe["max_chunk"] <= 512
                or not 1 <= probe["burst"] <= 48
                or not 1 <= probe["window"] <= probe["chunk"] * 48
                or (mode == "sim" and probe.get("supports_sim") != 1)):
            raise RuntimeError("Current OTA protocol and receive window could not be verified; no transfer started.")

        chunk = config.chunk_size if config.chunk_size is not None else min(
            self.derive_auto_chunk_size(), probe["chunk"])
        if not 20 <= chunk <= probe["max_chunk"] or not 1 <= config.burst_limit <= 48:
            raise RuntimeError("Requested OTA chunk or burst exceeds the current profile; no transfer started.")
        capacity = min(chunk * 48, 15616)
        window = min(probe["window"], capacity)
        if window < min(1024, capacity):
            raise RuntimeError("Current OTA receive window is too small; no transfer started.")
        effective = replace(config, chunk_size=chunk, mode="fast" if config.mode == "auto" else config.mode)
        return effective, window

    async def upload_file(
        self, firmware_path: Path, config: TransferConfig,
    ) -> Dict[str, Any]:
        self._require_ota_cleanup_complete()
        total_bytes = firmware_path.stat().st_size
        if total_bytes <= 0:
            raise RuntimeError("Firmware file is empty.")
        config, window_bytes = await self.require_current_ota_profile(
            mode="flash", size=total_bytes, config=config)
        with firmware_path.open("rb") as handle:
            return await self._run_ota_transfer(
                payload_chunks=iter(lambda: handle.read(window_bytes), b""),
                total_bytes=total_bytes, config=config, window_bytes=window_bytes,
                begin_command="ota_begin", label=f"firmware update ({firmware_path.name})",
            )

    async def run_simulated_upload_benchmark(
        self, total_bytes: int, config: TransferConfig,
    ) -> Dict[str, Any]:
        self._require_ota_cleanup_complete()
        if total_bytes <= 0:
            raise RuntimeError("Simulation byte count must be positive.")
        config, window_bytes = await self.require_current_ota_profile(
            mode="sim", size=total_bytes, config=config)
        chunk_pattern = bytes((index % 251 for index in range(244)))

        def chunk_iter() -> Iterable[bytes]:
            remaining = total_bytes
            while remaining > 0:
                emit = min(remaining, window_bytes)
                full_repeats, tail = divmod(emit, len(chunk_pattern))
                yield (chunk_pattern * full_repeats) + chunk_pattern[:tail]
                remaining -= emit

        result = await self._run_ota_transfer(
            payload_chunks=chunk_iter(), total_bytes=total_bytes, config=config,
            window_bytes=window_bytes, begin_command="ota_sim_begin",
            label=f"simulated OTA ({total_bytes} bytes)",
        )
        if result["terminal_state"] == "pending_verification":
            raise OtaOutcomeUnknown("Simulated OTA End delivery is uncertain; no further benchmark was started.")
        return result

    async def run_timed_uplink_benchmark(
        self,
        duration_seconds: float,
        config: TransferConfig,
    ) -> Dict[str, Any]:
        deadline = time.monotonic() + max(duration_seconds, 0.1)
        batch_bytes = 128 * 1024
        total_bytes = 0
        total_seconds = 0.0
        runs = 0
        last_result: Optional[Dict[str, Any]] = None

        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining < 0.5 and runs > 0:
                break
            result = await self.run_simulated_upload_benchmark(batch_bytes, config)
            total_bytes += int(result["bytes"])
            total_seconds += float(result["seconds"])
            runs += 1
            last_result = result

        if runs == 0 or last_result is None:
            raise RuntimeError("timed uplink test did not complete a batch")

        return {
            "bytes": total_bytes,
            "seconds": total_seconds,
            "bytes_per_second": total_bytes / max(total_seconds, 0.001),
            "chunk_size": last_result["chunk_size"],
            "burst_limit": last_result["burst_limit"],
            "mode": last_result["mode"],
            "runs": runs,
        }

    async def run_timed_downlink_benchmark(
        self,
        duration_seconds: float,
        delay_ms: int,
    ) -> Dict[str, Any]:
        self.clear_queues()
        whole_seconds = max(1, int(round(duration_seconds)))
        await self.write_command_line(f"ble test {whole_seconds} {delay_ms}")

        total_timeout_seconds = max(
            (whole_seconds * DEFAULT_CONSOLE_BLE_TEST_BUCKETS) + 12,
            whole_seconds + 10,
            15,
        )
        deadline = time.monotonic() + total_timeout_seconds
        fallback_bps: Optional[int] = None
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            line = await self.wait_for_console_line(timeout_seconds=remaining)
            if line is None:
                break

            full_match = CONSOLE_BLE_TEST_AGGREGATE_RE.search(line)
            if full_match is not None:
                return {
                    "bytes": int(full_match.group("bytes")),
                    "elapsed_ms": int(full_match.group("elapsed")),
                    "bytes_per_second": int(full_match.group("bps")),
                    "runs": 1,
                }

            tail_match = CONSOLE_BLE_TEST_AGGREGATE_TAIL_RE.search(line)
            if tail_match is not None:
                fallback_bps = int(tail_match.group("bps"))

        if fallback_bps is not None:
            estimated_elapsed_ms = whole_seconds * DEFAULT_CONSOLE_BLE_TEST_BUCKETS * 1000
            estimated_bytes = (fallback_bps * estimated_elapsed_ms) // 1000
            return {
                "bytes": estimated_bytes,
                "elapsed_ms": estimated_elapsed_ms,
                "bytes_per_second": fallback_bps,
                "runs": 1,
            }

        raise RuntimeError("console ble test timed out")

    def _require_ota_cleanup_complete(self) -> None:
        if self._ota_abort_task is not None and not self._ota_abort_task.done():
            raise RuntimeError("Previous OTA abort is still pending; reconnect before retrying.")

    async def _abort_ota_best_effort(self) -> None:
        task = asyncio.create_task(self.write_ota_control_json(
            {"cmd": "ota_abort"}, attempts=1, retry_delay_seconds=0,
            allow_no_response=False,
        ))
        self._ota_abort_task = task

        def consume_result(completed: asyncio.Task[bool]) -> None:
            try:
                completed.result()
            except BaseException:
                pass

        task.add_done_callback(consume_result)
        try:
            # A native write may ignore cancellation. Retain its owner and
            # forbid another transfer on this session until it has settled.
            await asyncio.wait({task}, timeout=DEFAULT_TIMEOUT_SECONDS)
        finally:
            if not task.done():
                task.cancel()

    async def _run_ota_transfer(
        self,
        *,
        payload_chunks: Iterable[bytes],
        total_bytes: int,
        config: TransferConfig,
        begin_command: str,
        label: str,
        window_bytes: int,
    ) -> Dict[str, Any]:
        self._require_ota_cleanup_complete()
        self.clear_queues()
        self._log(f"starting {label} total_bytes={total_bytes} mode={config.mode}")
        profile = HostTransferProfile()
        self._active_transfer_profile = profile

        sent_bytes = 0
        chunk_size = config.chunk_size or self.derive_auto_chunk_size()
        burst_limit = max(1, config.burst_limit)
        use_fast_mode = config.mode != "reliable"
        burst_count = 0
        rolling_crc = 0
        transfer_start = time.monotonic()
        progress = TransferProgress(label=label, total_bytes=total_bytes)
        terminal: Dict[str, Any] = {"state": "unknown"}
        begin_attempted = False
        end_issued = False

        try:
            await asyncio.sleep(config.pre_begin_delay_seconds)
            profile.sleep_pre_begin_ms += config.pre_begin_delay_seconds * 1000.0
            begin_attempted = True
            begin_ok = await self.write_ota_control_json(
                {"cmd": begin_command, "size": total_bytes, "chunk": chunk_size,
                 "burst": burst_limit, "window": window_bytes},
                attempts=config.begin_attempts,
                retry_delay_seconds=config.retry_delay_seconds,
                allow_no_response=True,
            )
            if not begin_ok:
                raise RuntimeError(f"{label}: failed to send begin command.")

            self._log("waiting for begin confirmation timeout=4.0s")
            begin_event = await self.wait_for_ota_event(
                timeout_seconds=4.0,
                predicate=lambda item: item.get("state") in {"begin", "error", "timeout", "disconnected"},
            )
            self._log(f"begin confirmation state={begin_event.get('state')} rx={begin_event.get('rx')} total={begin_event.get('total')}")
            if begin_event.get("state") == "error":
                detail = begin_event.get("detail") or begin_event.get("error") or "unknown"
                if detail == "unknown_cmd" and begin_command == "ota_sim_begin":
                    detail = describe_unsupported_command(begin_command)
                raise RuntimeError(f"{label}: begin rejected ({detail}).")
            if begin_event.get("state") != "begin":
                raise RuntimeError(f"{label}: did not receive begin confirmation ({begin_event.get('state')}).")

            accepted = begin_event.get("raw")
            expected = {"proto": 2, "sim": int(begin_command == "ota_sim_begin"),
                        "rx": 0, "total": total_bytes, "chunk": chunk_size,
                        "burst": burst_limit, "window": window_bytes}
            if (not isinstance(accepted, dict) or any(
                    type(accepted.get(key)) is not int or accepted[key] != value
                    for key, value in expected.items())):
                raise RuntimeError(f"{label}: begin protocol/window verification failed.")
            self._last_ota_rx = 0
            self._last_ota_total = total_bytes

            for source_chunk in payload_chunks:
                if not source_chunk:
                    continue
                offset = 0
                while offset < len(source_chunk):
                    chunk = source_chunk[offset : offset + chunk_size]
                    required_rx = max(0, sent_bytes + len(chunk) - window_bytes)
                    if self._last_ota_rx < required_rx:
                        event = await self.wait_for_board_rx(required_rx, timeout_seconds=4.0)
                        if (event.get("state") in {"error", "aborted", "done", "disconnected", "timeout"}
                                or self._last_ota_rx < required_rx):
                            raise RuntimeError(f"{label}: board did not acknowledge the OTA receive window.")
                    response = True
                    if use_fast_mode:
                        response = burst_count >= burst_limit
                    try:
                        write_started_at = time.monotonic()
                        await self._client.write_gatt_char(
                            self._ota_data_char,
                            chunk,
                            response=response,
                        )
                        write_ms = (time.monotonic() - write_started_at) * 1000.0
                        profile.note_data_write(write_ms, response)
                    except Exception as exc:
                        raise RuntimeError(f"{label}: data write failed ({exc}).") from exc

                    sent_bytes += len(chunk)
                    rolling_crc = zlib.crc32(chunk, rolling_crc)
                    offset += len(chunk)
                    if use_fast_mode:
                        if response:
                            burst_count = 0
                        else:
                            burst_count += 1
                    else:
                        burst_count = 0

                    progress.render(sent_bytes=sent_bytes, acked_bytes=self._last_ota_rx, force=sent_bytes >= total_bytes)

            final_settle_seconds = config.final_settle_seconds
            if use_fast_mode and final_settle_seconds > 0:
                await asyncio.sleep(final_settle_seconds)
                profile.sleep_final_settle_ms += final_settle_seconds * 1000.0

            drain_timeout_seconds = max(3.0, min(20.0, total_bytes / (64.0 * 1024.0)))
            drain_event = await self.wait_for_board_rx(sent_bytes, timeout_seconds=drain_timeout_seconds)
            progress.render(sent_bytes=sent_bytes, acked_bytes=self._last_ota_rx, force=True)
            if drain_event.get("state") in {"error", "aborted", "disconnected"}:
                progress.finish()
                detail = drain_event.get("detail") or drain_event.get("error") or drain_event.get("state")
                raise RuntimeError(f"{label}: board failed before ota_end ({detail}).")
            if self._last_ota_rx < sent_bytes:
                progress.finish()
                raise RuntimeError(
                    f"{label}: board only acknowledged {self._last_ota_rx}/{sent_bytes} bytes before ota_end timeout."
                )

            final_crc = f"{rolling_crc & 0xFFFFFFFF:08x}"
            end_payload = json.dumps(
                {"cmd": "ota_end", "size": total_bytes, "crc32": final_crc},
                separators=(",", ":"),
            ).encode("utf-8")
            # A failed native write can still have reached the board. Never
            # retry End or abort once this side effect has been issued.
            end_issued = True
            try:
                started_at = time.monotonic()
                await self._client.write_gatt_char(self._ota_control_char, end_payload, response=True)
                profile.note_control_write((time.monotonic() - started_at) * 1000.0)
            except Exception as error:
                self._log(f"ota end delivery uncertain: {error}")
                terminal = {"state": "pending_verification"}
            else:
                terminal_started_at = time.monotonic()
                terminal = await self.wait_for_ota_event(
                    timeout_seconds=config.terminal_timeout_seconds,
                    predicate=lambda item: item.get("state")
                    in {"done", "error", "aborted", "timeout", "disconnected"},
                )
                profile.note_wait_for_terminal((time.monotonic() - terminal_started_at) * 1000.0)
                if terminal.get("state") in {"timeout", "disconnected"}:
                    terminal = {"state": "pending_verification"}
                elif terminal.get("state") != "done":
                    detail = terminal.get("detail") or terminal.get("error") or terminal.get("state")
                    raise RuntimeError(f"{label}: terminal state {detail}.")

            elapsed_seconds = max(time.monotonic() - transfer_start, 0.001)
            bytes_per_second = sent_bytes / elapsed_seconds
            return {
                "bytes": sent_bytes,
                "seconds": elapsed_seconds,
                "bytes_per_second": bytes_per_second,
                "chunk_size": chunk_size,
                "burst_limit": burst_limit,
                "mode": "fast" if use_fast_mode else "reliable",
                "terminal_state": terminal.get("state"),
            }
        except BaseException:
            if begin_attempted and not end_issued:
                try:
                    await self._abort_ota_best_effort()
                except BaseException:
                    # Cleanup must retain the original failure or cancellation.
                    pass
            raise
        finally:
            progress.finish()
            elapsed_seconds = max(time.monotonic() - transfer_start, 0.001)
            self._log_transfer_profile(label, profile, elapsed_seconds)
            self._active_transfer_profile = None

    def _log_transfer_profile(self, label: str, profile: HostTransferProfile, elapsed_seconds: float) -> None:
        self._log(
            f"host transfer profile label={label} elapsed_ms={elapsed_seconds * 1000.0:.1f} "
            f"data_writes={profile.data_write_calls} data_write_ms={profile.data_write_total_ms:.1f} "
            f"data_write_max_ms={profile.data_write_max_ms:.1f} no_rsp={profile.data_write_no_response_calls} rsp={profile.data_write_response_calls} "
            f"control_writes={profile.control_write_calls} control_write_ms={profile.control_write_total_ms:.1f} control_write_max_ms={profile.control_write_max_ms:.1f} "
            f"wait_ota_calls={profile.wait_for_ota_event_calls} wait_ota_ms={profile.wait_for_ota_event_total_ms:.1f} wait_ota_max_ms={profile.wait_for_ota_event_max_ms:.1f} "
            f"wait_rx_calls={profile.wait_for_board_rx_calls} wait_rx_ms={profile.wait_for_board_rx_total_ms:.1f} wait_rx_max_ms={profile.wait_for_board_rx_max_ms:.1f} "
            f"wait_terminal_calls={profile.wait_for_terminal_calls} wait_terminal_ms={profile.wait_for_terminal_total_ms:.1f} wait_terminal_max_ms={profile.wait_for_terminal_max_ms:.1f} "
            f"sleep_pre_ms={profile.sleep_pre_begin_ms:.1f} sleep_final_ms={profile.sleep_final_settle_ms:.1f} "
            f"progress_notify_count={profile.progress_notify_count} progress_advance={profile.progress_notify_total_advance} progress_gap_max_ms={profile.progress_notify_max_gap_ms:.1f}"
        )

    async def run_downlink_benchmark(
        self,
        packet_sizes: Sequence[int],
        packet_count: int,
        delay_ms: int,
    ) -> Dict[str, Any]:
        results: List[Dict[str, Any]] = []
        for packet_size in packet_sizes:
            self.clear_queues()
            started_at = time.monotonic()
            sent = await self.write_ota_control_json(
                {
                    "cmd": "notify_test",
                    "size": packet_size,
                    "count": packet_count,
                    "delay_ms": delay_ms,
                },
                attempts=4,
                retry_delay_seconds=DEFAULT_RETRY_DELAY_SECONDS,
                allow_no_response=True,
            )
            if not sent:
                raise RuntimeError(f"Failed to start downlink notify test for size {packet_size}.")

            event = await self.wait_for_ota_event(
                timeout_seconds=max(10.0, (packet_count * max(delay_ms, 1)) / 1000.0 + 5.0),
                predicate=lambda item: item.get("state") in {"bench_notify", "error", "timeout", "disconnected"},
            )
            if event.get("state") != "bench_notify":
                detail = event.get("detail") or event.get("state") or "unknown"
                if detail == "unknown_cmd":
                    detail = describe_unsupported_command("notify_test")
                raise RuntimeError(f"Downlink notify test failed for size {packet_size} ({detail}).")

            detail = str(event.get("detail") or "")
            match = NOTIFY_TEST_DETAIL_RE.search(detail)
            if match is None:
                raise RuntimeError(f"Malformed downlink benchmark detail: {detail}")

            results.append(
                {
                    "size": int(match.group("size")),
                    "ok": int(match.group("ok")),
                    "fail": int(match.group("fail")),
                    "elapsed_ms": int(match.group("elapsed")),
                    "rate_bps": int(match.group("bps")),
                    "wall_elapsed_ms": int((time.monotonic() - started_at) * 1000.0),
                }
            )

        return {"results": results}


class TransferProgress:
    def __init__(self, label: str, total_bytes: int) -> None:
        self._label = label
        self._total_bytes = max(total_bytes, 1)
        self._start_time = time.monotonic()
        self._last_emit = 0.0
        self._active = False

    def render(self, *, sent_bytes: int, acked_bytes: int, force: bool = False) -> None:
        now = time.monotonic()
        if not force and (now - self._last_emit) < 0.5:
            return
        self._last_emit = now
        self._active = True

        progress_fraction = max(0.0, min(sent_bytes / self._total_bytes, 1.0))
        percent = progress_fraction * 100.0
        elapsed_runtime = max(now - self._start_time, 0.001)
        kib_per_second = (sent_bytes / 1024.0) / elapsed_runtime
        eta_seconds = None
        if sent_bytes > 0 and kib_per_second > 0.01:
            remaining_bytes = max(self._total_bytes - sent_bytes, 0)
            eta_seconds = int(round(remaining_bytes / (kib_per_second * 1024.0)))

        eta_text = format_hms(eta_seconds)
        line = f"\r{percent:5.1f}% {kib_per_second:7.1f} KiB/s eta {eta_text}"
        sys.stdout.write(line)
        sys.stdout.flush()

    def finish(self) -> None:
        if self._active:
            sys.stdout.write("\n")
            sys.stdout.flush()
            self._active = False


def decode_ota_status(payload: bytes) -> Dict[str, Any]:
    text = payload.decode("utf-8", errors="replace").strip()
    try:
        decoded = json.loads(text)
    except json.JSONDecodeError:
        lowered = text.lower()
        if '"st":"begin"' in lowered:
            return {"state": "begin", "detail": None, "rx": 0, "total": 0, "raw": text}
        if '"st":"probe"' in lowered:
            return {"state": "probe", "detail": None, "rx": 0, "total": 0, "raw": text}
        if '"st":"done"' in lowered:
            return {"state": "done", "detail": None, "rx": 0, "total": 0, "raw": text}
        if '"st":"aborted"' in lowered:
            return {"state": "aborted", "detail": None, "rx": 0, "total": 0, "raw": text}
        if '"st":"progress"' in lowered or '"p":' in lowered:
            return {"state": "progress", "detail": None, "progress_percent": 0, "rx": 0, "total": 0, "raw": text}
        if '"e":' in lowered or '"st":"error"' in lowered:
            return {"state": "error", "detail": text, "raw": text}
        return {"state": "raw", "raw": text}

    if not isinstance(decoded, dict):
        return {"state": "raw", "raw": text}

    if safe_int(decoded.get("b")) != 0:
        return {"state": "begin", "detail": None, "rx": 0, "total": 0, "raw": decoded}

    if safe_int(decoded.get("proto")) >= 2 and "st" not in decoded and "s" not in decoded:
        return {"state": "probe", "detail": None, "rx": 0, "total": 0, "raw": decoded}

    if "v" in decoded:
        return {"state": "version", "detail": str(decoded.get("v", ""))}
    if "e" in decoded:
        return {"state": "error", "detail": str(decoded.get("e", ""))}
    if "p" in decoded:
        return {
            "state": "progress",
            "detail": None,
            "progress_percent": safe_int(decoded.get("p")),
            "rx": decoded.get("rx", decoded.get("r", 0)),
            "total": decoded.get("total", decoded.get("t", 0)),
            "raw": decoded,
        }

    state = decoded.get("st") or decoded.get("s")
    if state in {"d", "done"}:
        state = "done"
    elif state in {"a", "aborted"}:
        state = "aborted"
    elif state in {"b", "begin"}:
        state = "begin"
    elif state in {"p", "progress"}:
        state = "progress"
    elif state in {"err", "error"}:
        state = "error"

    detail = decoded.get("detail")
    if detail is None:
        detail = decoded.get("d")
    return {
        "state": state or "raw",
        "detail": detail,
        "progress_percent": decoded.get("p"),
        "rx": decoded.get("rx", decoded.get("r", 0)),
        "total": decoded.get("total", decoded.get("t", 0)),
        "raw": decoded,
    }


def likely_binary_payload(payload: bytes) -> bool:
    if not payload:
        return False
    if b"\n" in payload or b"\r" in payload:
        return False
    non_text_count = 0
    for byte in payload:
        if byte in (9, 32):
            continue
        if 33 <= byte <= 126:
            continue
        non_text_count += 1
    return non_text_count > (len(payload) // 2)


def drain_queue(queue: "asyncio.Queue[Any]") -> None:
    while True:
        try:
            queue.get_nowait()
        except asyncio.QueueEmpty:
            return


def classify_candidate_device(name: str, service_uuids: Sequence[str]) -> Optional[str]:
    normalized_name = name.strip().lower()
    normalized_services = {normalize_uuid(item) for item in service_uuids}
    if BASE_SERVICE_UUID in normalized_services or SETTINGS_SERVICE_UUID in normalized_services:
        return "service"
    if LITCH_NAME_PREFIX.lower() in normalized_name:
        return "name"
    return None


async def scan_candidates(scan_seconds: float, include_name_match: bool = False) -> List[CandidateDevice]:
    _, BleakScanner, _ = require_bleak()
    discovered = await BleakScanner.discover(timeout=scan_seconds, return_adv=True)
    items: Iterable[Any]
    if isinstance(discovered, dict):
        items = discovered.values()
    else:
        items = discovered

    service_candidates: List[CandidateDevice] = []
    name_candidates: List[CandidateDevice] = []
    for item in items:
        device = item
        advertisement = None
        if isinstance(item, tuple) and len(item) == 2:
            device, advertisement = item

        name = (getattr(device, "name", None) or "").strip()
        if not name and advertisement is not None:
            name = (getattr(advertisement, "local_name", None) or "").strip()
        if not name:
            name = LITCH_NAME_PREFIX

        service_uuids: List[str] = []
        if advertisement is not None:
            service_uuids.extend(list(getattr(advertisement, "service_uuids", []) or []))
        metadata = getattr(device, "metadata", {}) or {}
        service_uuids.extend(list(metadata.get("uuids", []) or []))

        match_reason = classify_candidate_device(name, service_uuids)
        if match_reason is None:
            continue

        candidate = CandidateDevice(
            name=name,
            address=str(getattr(device, "address", "")),
            rssi=int(getattr(device, "rssi", 0) or 0),
            service_uuids=tuple(sorted({normalize_uuid(item) for item in service_uuids})),
            match_reason=match_reason,
        )
        if match_reason == "service":
            service_candidates.append(candidate)
        else:
            name_candidates.append(candidate)

    candidates = service_candidates
    if not candidates and include_name_match:
        candidates = name_candidates

    candidates.sort(key=lambda item: (item.name.lower(), item.address.lower()))
    deduped: Dict[str, CandidateDevice] = {}
    for candidate in candidates:
        deduped[candidate.address] = candidate
    return list(deduped.values())


def select_candidate(candidates: Sequence[CandidateDevice], selector: Optional[str]) -> CandidateDevice:
    if not candidates:
        raise SystemExit("No Litch boards found.")

    if selector:
        lowered = selector.lower()
        matches = [
            item
            for item in candidates
            if lowered in item.address.lower() or lowered in item.name.lower()
        ]
        if not matches:
            raise SystemExit(f"No Litch board matched selector '{selector}'.")
        if len(matches) == 1:
            return matches[0]
        candidates = matches

    if len(candidates) == 1:
        candidate = candidates[0]
        print(f"Selected {candidate.display_name} (RSSI {candidate.rssi} dBm)")
        return candidate

    print("Multiple Litch boards found:")
    for index, candidate in enumerate(candidates, start=1):
        services = ",".join(candidate.service_uuids) if candidate.service_uuids else f"({candidate.match_reason}-match)"
        print(
            f"  {index}. {candidate.display_name} RSSI={candidate.rssi} address={candidate.address} services={services}"
        )

    if not sys.stdin.isatty():
        raise SystemExit("Multiple boards found. Re-run with --device to pick one non-interactively.")

    while True:
        raw = input(f"Select board [1-{len(candidates)}]: ").strip()
        if not raw:
            continue
        try:
            index = int(raw)
        except ValueError:
            continue
        if 1 <= index <= len(candidates):
            return candidates[index - 1]


async def connect_session(candidate: CandidateDevice) -> LitchBleSession:
    BleakClient, _, BleakError = require_bleak()
    disconnect_event = asyncio.Event()

    def handle_disconnect(_: Any) -> None:
        disconnect_event.set()

    client = BleakClient(candidate.address, disconnected_callback=handle_disconnect)
    try:
        try:
            await client.connect()
        except BleakError as exc:
            raise SystemExit(f"Failed to connect to {candidate.display_name}: {exc}") from exc
        session = LitchBleSession(client, candidate)
        session._disconnect_event = disconnect_event
        await session.start_notifications()
        return session
    except BaseException as error:
        # The caller cannot release a client until acquisition returns. This
        # owner also covers cancellation and partial notification startup.
        try:
            await asyncio.wait_for(client.disconnect(), timeout=CONNECT_CLEANUP_TIMEOUT_SECONDS)
        except BaseException as cleanup_error:
            notes = list(getattr(error, "__notes__", ()))
            notes.append(f"BLE acquisition cleanup failed: {cleanup_error!r}")
            setattr(error, "__notes__", notes)
        raise


async def try_connect_cached_session(selector: Optional[str]) -> Optional[LitchBleSession]:
    candidate = load_cached_candidate(selector)
    if candidate is None:
        return None
    try:
        session = await connect_session(candidate)
    except SystemExit:
        return None
    print(f"Using cached board {candidate.display_name}")
    return session


def print_session_link_profile(session: LitchBleSession) -> None:
    auto_chunk = session.derive_auto_chunk_size()
    print(
        f"Connected to {session.device.display_name}: mtu={session.mtu_size} auto_chunk={auto_chunk} host={platform.system()}"
    )


async def run_scan_command(args: argparse.Namespace) -> int:
    candidates = await scan_candidates(args.scan_seconds, include_name_match=args.include_name_match)
    if not candidates:
        print("No Litch boards found.")
        return 1
    for index, candidate in enumerate(candidates, start=1):
        services = ",".join(candidate.service_uuids) if candidate.service_uuids else f"({candidate.match_reason}-match)"
        print(
            f"{index}. {candidate.display_name} RSSI={candidate.rssi} address={candidate.address} services={services}"
        )
    return 0


def build_transfer_config(args: argparse.Namespace) -> TransferConfig:
    chunk_size = None if args.chunk_size == "auto" else int(args.chunk_size)
    return TransferConfig(
        chunk_size=chunk_size,
        mode=args.mode,
        burst_limit=args.burst,
        begin_attempts=args.begin_attempts,
        retry_delay_seconds=args.retry_delay_seconds,
        pre_begin_delay_seconds=args.pre_begin_delay_seconds,
        final_settle_seconds=args.final_settle_seconds,
        terminal_timeout_seconds=args.terminal_timeout_seconds,
    )


async def with_selected_session(
    args: argparse.Namespace, *, cache_on_connect: bool = True
) -> LitchBleSession:
    session = None
    if not args.include_name_match:
        session = await try_connect_cached_session(args.device)

    if session is None:
        candidates = await scan_candidates(args.scan_seconds, include_name_match=args.include_name_match)
        candidate = select_candidate(candidates, args.device)
        session = await connect_session(candidate)

    print_session_link_profile(session)
    version = await session.fetch_fw_version()
    if version:
        print(f"Firmware version: {version}")
    else:
        print("Firmware version: unavailable")
    if cache_on_connect:
        save_cached_candidate(session.device)
    return session


def resolve_firmware_bin_path(raw_path: Optional[str]) -> Path:
    firmware_bin_path = DEFAULT_LOCAL_FIRMWARE_BIN if not raw_path else Path(raw_path).expanduser().resolve()
    if not firmware_bin_path.exists():
        if raw_path:
            raise SystemExit(f"Firmware .bin file not found: {firmware_bin_path}")
        raise SystemExit(
            "Default firmware .bin file not found: "
            f"{firmware_bin_path}. Build it first with: pio run -e litch-esp32s3"
        )
    if firmware_bin_path.suffix.lower() != ".bin":
        raise SystemExit(f"Expected a firmware .bin file, got: {firmware_bin_path}")
    return firmware_bin_path


async def run_update_command(args: argparse.Namespace) -> int:
    firmware_path = resolve_firmware_bin_path(args.firmware_bin)

    session = await with_selected_session(args, cache_on_connect=False)
    try:
        config = build_transfer_config(args)
        result = await session.upload_file(firmware_path, config)
        if result.get("terminal_state") not in {"done", "pending_verification"}:
            raise RuntimeError(
                "firmware update did not receive the board's terminal success acknowledgement"
            )
        try:
            await session.stop_notifications()
            await session._client.disconnect()
        except Exception as error:
            result["terminal_state"] = "pending_verification"
            print(f"BLE cleanup failed after End: {error}", file=sys.stderr)
        session = None
        outcome = "Transfer finalized" if result["terminal_state"] == "done" else "End delivery or completion uncertain"
        print(
            f"{outcome}; post-reboot verification pending: bytes={result['bytes']} seconds={result['seconds']:.2f} rate={result['bytes_per_second']:.0f} B/s mode={result['mode']} chunk={result['chunk_size']} burst={result['burst_limit']} terminal={result['terminal_state']}"
        )
        return UPDATE_END_UNCERTAIN_EXIT_CODE if result["terminal_state"] == "pending_verification" else 0
    finally:
        if session is not None:
            await session.stop_notifications()
            await session._client.disconnect()


def parse_int_list(raw: str) -> List[int]:
    values: List[int] = []
    for token in raw.split(","):
        stripped = token.strip()
        if not stripped:
            continue
        values.append(int(stripped))
    return values


async def run_uplink_benchmark(args: argparse.Namespace) -> int:
    session = await with_selected_session(args)
    try:
        config = build_transfer_config(args)
        result = await session.run_simulated_upload_benchmark(args.bytes, config)
        print(
            f"Simulated OTA upload: bytes={result['bytes']} seconds={result['seconds']:.2f} rate={result['bytes_per_second']:.0f} B/s mode={result['mode']} chunk={result['chunk_size']} burst={result['burst_limit']} terminal={result['terminal_state']}"
        )
        return 0
    finally:
        await session.stop_notifications()
        await session._client.disconnect()


async def run_downlink_benchmark(args: argparse.Namespace) -> int:
    session = await with_selected_session(args)
    try:
        packet_sizes = [value for value in parse_int_list(args.sizes) if value > 0]
        result = await session.run_downlink_benchmark(packet_sizes, args.packet_count, args.delay_ms)
        for item in result["results"]:
            print(
                f"Notify test size={item['size']} ok={item['ok']} fail={item['fail']} elapsed_ms={item['elapsed_ms']} rate={item['rate_bps']} B/s wall_ms={item['wall_elapsed_ms']}"
            )
        return 0
    finally:
        await session.stop_notifications()
        await session._client.disconnect()


async def run_combined_benchmark(args: argparse.Namespace) -> int:
    session = await with_selected_session(args)
    try:
        base_config = build_transfer_config(args)
        notify_sizes = [value for value in parse_int_list(args.notify_sizes) if value > 0]
        downlink = await session.run_downlink_benchmark(notify_sizes, args.packet_count, args.delay_ms)
        for item in downlink["results"]:
            print(
                f"Downlink size={item['size']} rate={item['rate_bps']} B/s ok={item['ok']} fail={item['fail']}"
            )

        chunk_sizes = [value for value in parse_int_list(args.chunk_sizes) if value > 0]
        bursts = [value for value in parse_int_list(args.bursts) if value > 0]
        if base_config.mode == "reliable":
            bursts = [1]
        auto_cap = session.derive_auto_chunk_size()
        chunk_sizes = [min(value, auto_cap) for value in chunk_sizes]
        chunk_sizes = sorted(set(chunk_sizes))

        results: List[Dict[str, Any]] = []
        for chunk_size in chunk_sizes:
            for burst in bursts:
                config = TransferConfig(
                    chunk_size=chunk_size,
                    mode=base_config.mode,
                    burst_limit=burst,
                    begin_attempts=base_config.begin_attempts,
                    retry_delay_seconds=base_config.retry_delay_seconds,
                    pre_begin_delay_seconds=base_config.pre_begin_delay_seconds,
                    final_settle_seconds=base_config.final_settle_seconds,
                    terminal_timeout_seconds=base_config.terminal_timeout_seconds,
                )
                print(f"Running simulated OTA sweep chunk={chunk_size} burst={burst}...")
                try:
                    result = await session.run_simulated_upload_benchmark(args.bytes, config)
                    results.append(result)
                    print(
                        f"  rate={result['bytes_per_second']:.0f} B/s seconds={result['seconds']:.2f} terminal={result['terminal_state']}"
                    )
                except OtaOutcomeUnknown:
                    raise
                except RuntimeError as exc:
                    print(f"  failed: {exc}")

        if not results:
            raise SystemExit("No uplink benchmark combinations succeeded.")

        best = max(results, key=lambda item: item["bytes_per_second"])
        print(
            f"Best simulated OTA: rate={best['bytes_per_second']:.0f} B/s chunk={best['chunk_size']} burst={best['burst_limit']} mode={best['mode']}"
        )
        return 0
    finally:
        await session.stop_notifications()
        await session._client.disconnect()


async def run_quick_test_command(args: argparse.Namespace) -> int:
    session = await with_selected_session(args)
    try:
        config = build_transfer_config(args)
        requested_seconds = max(2.0, float(args.seconds))
        uplink_budget_seconds = requested_seconds / 2.0
        downlink_budget_seconds = requested_seconds - uplink_budget_seconds

        uplink_error: Optional[str] = None
        downlink_error: Optional[str] = None
        uplink_result: Optional[Dict[str, Any]] = None
        downlink_result: Optional[Dict[str, Any]] = None

        try:
            uplink_result = await session.run_timed_uplink_benchmark(uplink_budget_seconds, config)
        except RuntimeError as exc:
            uplink_error = str(exc)

        try:
            downlink_result = await session.run_timed_downlink_benchmark(downlink_budget_seconds, args.delay_ms)
        except RuntimeError as exc:
            downlink_error = str(exc)

        if uplink_result is not None:
            print(
                "Test uplink: "
                f"rate={uplink_result['bytes_per_second']:.0f} B/s mode={uplink_result['mode']} "
                f"chunk={uplink_result['chunk_size']} burst={uplink_result['burst_limit']} runs={uplink_result['runs']}"
            )
        else:
            print(f"Test uplink: unavailable ({uplink_error})")

        if downlink_result is not None:
            print(
                "Test downlink: "
                f"rate={downlink_result['bytes_per_second']:.0f} B/s bytes={downlink_result['bytes']} "
                f"elapsed_ms={downlink_result['elapsed_ms']} runs={downlink_result['runs']}"
            )
        else:
            print(f"Test downlink: unavailable ({downlink_error})")

        if uplink_result is None and downlink_result is None:
            return 1
        return 0
    finally:
        await session.stop_notifications()
        await session._client.disconnect()


def add_device_selection_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--device", help="Board name/address substring to auto-select")
    parser.add_argument(
        "--scan-seconds",
        type=float,
        default=DEFAULT_SCAN_SECONDS,
        help=f"BLE scan time before selection (default: {DEFAULT_SCAN_SECONDS})",
    )
    parser.add_argument(
        "--include-name-match",
        action="store_true",
        help="Include name-only matches when no live service-advertising board is found; mainly useful on macOS for debugging cached devices",
    )


def add_transfer_tuning_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--chunk-size",
        default="auto",
        help="OTA data chunk size in bytes, or 'auto' to use platform/write-limit defaults",
    )
    parser.add_argument(
        "--mode",
        choices=("auto", "fast", "reliable"),
        default="auto",
        help="All modes require the current OTA profile; auto/fast prefer write-without-response, reliable uses write-with-response",
    )
    parser.add_argument("--burst", type=int, default=DEFAULT_BURST_LIMIT, help="No-response burst limit before a response barrier")
    parser.add_argument("--begin-attempts", type=int, default=DEFAULT_BEGIN_ATTEMPTS)
    parser.add_argument("--retry-delay-seconds", type=float, default=DEFAULT_RETRY_DELAY_SECONDS)
    parser.add_argument("--pre-begin-delay-seconds", type=float, default=DEFAULT_PRE_BEGIN_DELAY_SECONDS)
    parser.add_argument("--final-settle-seconds", type=float, default=DEFAULT_FINAL_SETTLE_SECONDS)
    parser.add_argument("--terminal-timeout-seconds", type=float, default=DEFAULT_TIMEOUT_SECONDS)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Discover current Litch boards, update firmware over BLE, and run raw BLE bandwidth checks."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser("scan", help="List discoverable Litch boards")
    scan_parser.add_argument("--scan-seconds", type=float, default=DEFAULT_SCAN_SECONDS)
    scan_parser.add_argument(
        "--include-name-match",
        action="store_true",
        help="Include name-only cached matches when no service-advertising board is visible",
    )

    update_parser = subparsers.add_parser("update", help="Upload firmware over BLE using the current OTA protocol")
    update_parser.add_argument(
        "firmware_bin",
        nargs="?",
        help=(
            "Path to the firmware .bin file. Defaults to "
            f"{DEFAULT_LOCAL_FIRMWARE_BIN.relative_to(REPO_ROOT)}"
        ),
    )
    add_device_selection_args(update_parser)
    add_transfer_tuning_args(update_parser)

    uplink_parser = subparsers.add_parser(
        "bench-uplink",
        help="Benchmark the OTA upload path without flashing using ota_sim_begin",
    )
    uplink_parser.add_argument("--bytes", type=int, default=DEFAULT_SIMULATION_BYTES)
    add_device_selection_args(uplink_parser)
    add_transfer_tuning_args(uplink_parser)

    downlink_parser = subparsers.add_parser(
        "bench-downlink",
        help="Run the firmware's structured raw notify benchmark over OTA control + console notify",
    )
    downlink_parser.add_argument(
        "--sizes",
        default=",".join(str(item) for item in DEFAULT_NOTIFY_TEST_SIZES),
        help="Comma-separated notify packet sizes to sweep",
    )
    downlink_parser.add_argument("--packet-count", type=int, default=DEFAULT_NOTIFY_TEST_PACKET_COUNT)
    downlink_parser.add_argument("--delay-ms", type=int, default=DEFAULT_BENCH_DELAY_MS)
    add_device_selection_args(downlink_parser)

    combined_parser = subparsers.add_parser(
        "bench",
        help="Run downlink notify benchmark plus simulated OTA upload sweeps",
    )
    combined_parser.add_argument("--bytes", type=int, default=DEFAULT_SIMULATION_BYTES)
    combined_parser.add_argument(
        "--notify-sizes",
        default=",".join(str(item) for item in DEFAULT_NOTIFY_TEST_SIZES),
        help="Comma-separated notify packet sizes for the downlink sweep",
    )
    combined_parser.add_argument("--packet-count", type=int, default=DEFAULT_NOTIFY_TEST_PACKET_COUNT)
    combined_parser.add_argument("--delay-ms", type=int, default=DEFAULT_BENCH_DELAY_MS)
    combined_parser.add_argument(
        "--chunk-sizes",
        default=",".join(str(item) for item in DEFAULT_BENCH_CHUNK_SIZES),
        help="Comma-separated simulated OTA chunk sizes to sweep",
    )
    combined_parser.add_argument(
        "--bursts",
        default=",".join(str(item) for item in DEFAULT_BENCH_BURSTS),
        help="Comma-separated no-response burst counts to sweep",
    )
    add_device_selection_args(combined_parser)
    add_transfer_tuning_args(combined_parser)

    test_parser = subparsers.add_parser(
        "test",
        help="Run a quick bandwidth check in about 10 seconds total and summarize the results",
    )
    test_parser.add_argument("--seconds", type=float, default=10.0, help="Total test duration budget in seconds")
    test_parser.add_argument("--delay-ms", type=int, default=DEFAULT_BENCH_DELAY_MS)
    add_device_selection_args(test_parser)
    add_transfer_tuning_args(test_parser)

    return parser


async def async_main(args: argparse.Namespace) -> int:
    if args.command == "scan":
        return await run_scan_command(args)
    if args.command == "update":
        return await run_update_command(args)
    if args.command == "bench-uplink":
        return await run_uplink_benchmark(args)
    if args.command == "bench-downlink":
        return await run_downlink_benchmark(args)
    if args.command == "bench":
        return await run_combined_benchmark(args)
    if args.command == "test":
        return await run_quick_test_command(args)
    raise SystemExit(f"Unknown command: {args.command}")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        if args.command == "update":
            # Direct script execution starts with tools/ on sys.path. Reuse
            # the CLI verifier's existing owner for bounded loop retirement.
            if str(REPO_ROOT) not in sys.path:
                sys.path.insert(0, str(REPO_ROOT))
            from tools.dev.runtime_verify import _run_coroutine_in_owned_loop

            return _run_coroutine_in_owned_loop(
                async_main(args), timeout_seconds=UPDATE_TIMEOUT_SECONDS,
                cleanup_timeout_seconds=DEFAULT_TIMEOUT_SECONDS,
                label="BLE firmware update",
            )
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        print("Interrupted.")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
