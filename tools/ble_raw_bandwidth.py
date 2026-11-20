#!/usr/bin/env python3

from __future__ import annotations

import argparse
import asyncio
import json
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Sequence

from ble_update_firmware import (
    BASE_SERVICE_UUID,
    COMMAND_CHARACTERISTIC_UUID,
    CONSOLE_STREAM_CHARACTERISTIC_UUID,
    DEFAULT_BURST_LIMIT,
    DEFAULT_FAST_DATA_LEN_OCTETS,
    DEFAULT_SCAN_SECONDS,
    DEFAULT_SIMULATION_BYTES,
    DEVICE_CACHE_PATH,
    LITCH_NAME_PREFIX,
    OTA_CONTROL_CHARACTERISTIC_UUID,
    OTA_DATA_CHARACTERISTIC_UUID,
    CandidateDevice,
    classify_candidate_device,
    decode_ota_status,
    describe_unsupported_command,
    load_cached_candidate,
    normalize_uuid,
    require_bleak,
    safe_int,
    save_cached_candidate,
    scan_candidates,
    select_candidate,
)

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOCAL_FIRMWARE_BIN = REPO_ROOT / ".pio" / "build" / "litch-esp32s3" / "firmware.bin"
DEFAULT_MAX_ATT_PAYLOAD_BYTES = 512
DEFAULT_RECOMMENDED_OTA_CHUNK_BYTES = DEFAULT_FAST_DATA_LEN_OCTETS - 7


@dataclass
class DownlinkStats:
    host_bytes: int = 0
    first_packet_at: float = 0.0
    last_packet_at: float = 0.0
    packet_count: int = 0

    def note(self, payload: bytes) -> None:
        now = time.monotonic()
        if self.first_packet_at == 0.0:
            self.first_packet_at = now
        self.last_packet_at = now
        self.host_bytes += len(payload)
        self.packet_count += 1

    @property
    def elapsed_seconds(self) -> float:
        if self.first_packet_at == 0.0 or self.last_packet_at == 0.0:
            return 0.0
        return max(self.last_packet_at - self.first_packet_at, 0.0)


class RawBenchmarkSession:
    def __init__(self, client: Any, candidate: CandidateDevice) -> None:
        self._client = client
        self._candidate = candidate
        self._disconnect_event = asyncio.Event()
        self._ota_queue: "asyncio.Queue[Dict[str, Any]]" = asyncio.Queue()
        self._downlink_stats = DownlinkStats()
        self._capture_binary_console = False

        self._command_char = self._resolve_characteristic(BASE_SERVICE_UUID, COMMAND_CHARACTERISTIC_UUID)
        self._console_char = self._resolve_characteristic(BASE_SERVICE_UUID, CONSOLE_STREAM_CHARACTERISTIC_UUID)
        self._ota_control_char = self._resolve_characteristic(BASE_SERVICE_UUID, OTA_CONTROL_CHARACTERISTIC_UUID)
        self._ota_data_char = self._resolve_characteristic(BASE_SERVICE_UUID, OTA_DATA_CHARACTERISTIC_UUID)

    @property
    def mtu_size(self) -> int:
        value = int(getattr(self._client, "mtu_size", 23) or 23)
        return value if value > 0 else 23

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
        raise RuntimeError(f"Characteristic {characteristic_uuid} not found.")

    async def start(self) -> None:
        await self._client.start_notify(self._ota_control_char, self._handle_ota_notification)
        await self._client.start_notify(self._console_char, self._handle_console_notification)

    async def stop(self) -> None:
        try:
            await self._client.stop_notify(self._ota_control_char)
        except Exception:
            pass
        try:
            await self._client.stop_notify(self._console_char)
        except Exception:
            pass

    def _handle_ota_notification(self, _: Any, payload: bytearray) -> None:
        self._ota_queue.put_nowait(decode_ota_status(bytes(payload)))

    def _handle_console_notification(self, _: Any, payload: bytearray) -> None:
        if self._capture_binary_console:
            self._downlink_stats.note(bytes(payload))

    async def write_command_line(self, line: str) -> None:
        await self._client.write_gatt_char(
            self._command_char,
            (line.rstrip() + "\n").encode("utf-8"),
            response=True,
        )

    async def write_ota_control_json(self, payload: Dict[str, Any], *, response: bool = True) -> None:
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        await self._client.write_gatt_char(self._ota_control_char, encoded, response=response)

    async def wait_for_ota_event(self, timeout_seconds: float, expected_states: Sequence[str]) -> Dict[str, Any]:
        deadline = time.monotonic() + timeout_seconds
        expected = set(expected_states)
        while True:
            if self._disconnect_event.is_set():
                return {"state": "disconnected"}
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return {"state": "timeout"}
            try:
                event = await asyncio.wait_for(self._ota_queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                return {"state": "timeout"}
            if str(event.get("state")) in expected:
                return event

    async def fetch_fw_version(self) -> Optional[str]:
        await self.write_ota_control_json({"cmd": "fw_version"})
        event = await self.wait_for_ota_event(3.0, ["version", "error", "timeout"])
        if event.get("state") == "version":
            return str(event.get("detail") or "").strip() or None
        return None

    async def run_downlink_notify_test(self, packet_size: int, packet_count: int, delay_ms: int) -> Dict[str, Any]:
        self._downlink_stats = DownlinkStats()
        self._capture_binary_console = True
        board_bytes = 0
        board_elapsed_ms = 0
        board_rate = 0
        try:
            await self.write_ota_control_json(
                {"cmd": "notify_test", "size": packet_size, "count": packet_count, "delay_ms": delay_ms}
            )
            expected_seconds = (packet_size * packet_count) / 32768.0
            deadline = time.monotonic() + max(20.0, expected_seconds + 15.0)
            idle_gap_seconds = 1.0
            while True:
                if self._disconnect_event.is_set():
                    raise RuntimeError("notify_test failed (disconnected)")

                while True:
                    try:
                        event = self._ota_queue.get_nowait()
                    except asyncio.QueueEmpty:
                        break

                    state = str(event.get("state") or "")
                    if state == "error":
                        detail = event.get("detail") or "unknown"
                        if detail == "unknown_cmd":
                            detail = describe_unsupported_command("notify_test")
                        raise RuntimeError(f"notify_test failed ({detail})")
                    if state == "bench_notify":
                        detail = str(event.get("detail") or "")
                        parts = {
                            item.split("=", 1)[0]: item.split("=", 1)[1]
                            for item in detail.split()
                            if "=" in item
                        }
                        board_bytes = safe_int(parts.get("size")) * safe_int(parts.get("ok"))
                        board_elapsed_ms = max(safe_int(parts.get("elapsed_ms")), 1)
                        board_rate = safe_int(parts.get("rate_Bps"))

                now = time.monotonic()
                if self._downlink_stats.packet_count > 0 and self._downlink_stats.last_packet_at > 0.0:
                    if (now - self._downlink_stats.last_packet_at) >= idle_gap_seconds:
                        break

                if now >= deadline:
                    break

                await asyncio.sleep(0.05)
        finally:
            self._capture_binary_console = False

        if self._downlink_stats.packet_count == 0:
            raise RuntimeError("notify_test failed (no host packets received)")

        host_elapsed = self._downlink_stats.elapsed_seconds
        host_elapsed_ms = max(int(round(host_elapsed * 1000.0)), 1) if host_elapsed > 0.0 else 0
        host_rate = int(round(self._downlink_stats.host_bytes / host_elapsed)) if host_elapsed > 0.0 else 0

        return {
            "packet_size": packet_size,
            "packet_count": packet_count,
            "delay_ms": delay_ms,
            "board_bytes": board_bytes,
            "board_elapsed_ms": board_elapsed_ms,
            "board_rate_Bps": board_rate,
            "host_bytes": self._downlink_stats.host_bytes,
            "host_elapsed_ms": host_elapsed_ms,
            "host_rate_Bps": host_rate,
            "host_packets": self._downlink_stats.packet_count,
        }

    def derive_auto_chunk_size(self) -> int:
        mtu_payload = max(20, min(self.mtu_size - 3, DEFAULT_MAX_ATT_PAYLOAD_BYTES))
        recommended_payload = min(mtu_payload, DEFAULT_RECOMMENDED_OTA_CHUNK_BYTES)
        max_write = getattr(self._ota_data_char, "max_write_without_response_size", None)
        if max_write is not None:
            try:
                value = int(max_write)
                if value > 0:
                    return max(20, min(value, recommended_payload))
            except Exception:
                pass
        return recommended_payload

    async def run_uplink_sim_test(self, total_bytes: int, chunk_size: Optional[int], burst: int) -> Dict[str, Any]:
        if total_bytes <= 0:
            raise RuntimeError("bytes must be positive")
        effective_chunk = chunk_size or self.derive_auto_chunk_size()
        effective_burst = max(1, burst)
        rolling_crc = 0
        payload_seed = bytes((index % 251 for index in range(effective_chunk)))

        await self.write_ota_control_json(
            {"cmd": "ota_sim_begin", "size": total_bytes, "chunk": effective_chunk, "burst": effective_burst}
        )
        begin = await self.wait_for_ota_event(4.0, ["begin", "error", "timeout", "disconnected"])
        if begin.get("state") == "error":
            raise RuntimeError(f"ota_sim_begin rejected ({begin.get('detail')})")
        if begin.get("state") != "begin":
            raise RuntimeError(f"ota_sim_begin failed ({begin.get('state')})")

        sent = 0
        burst_count = 0
        started_at = time.monotonic()
        while sent < total_bytes:
            emit = min(total_bytes - sent, effective_chunk)
            chunk = payload_seed[:emit]
            rolling_crc = zlib.crc32(chunk, rolling_crc)
            response = burst_count >= effective_burst
            await self._client.write_gatt_char(self._ota_data_char, chunk, response=response)
            sent += emit
            if response:
                burst_count = 0
            else:
                burst_count += 1

        await self.write_ota_control_json(
            {"cmd": "ota_end", "size": total_bytes, "crc32": f"{rolling_crc & 0xFFFFFFFF:08x}"}
        )
        terminal = await self.wait_for_ota_event(8.0, ["done", "error", "timeout", "disconnected"])
        elapsed = max(time.monotonic() - started_at, 0.001)
        if terminal.get("state") != "done":
            raise RuntimeError(f"simulated upload failed ({terminal.get('detail') or terminal.get('state')})")
        return {
            "bytes": sent,
            "seconds": elapsed,
            "rate_Bps": int(round(sent / elapsed)),
            "chunk_size": effective_chunk,
            "burst": effective_burst,
        }


async def resolve_candidate(selector: Optional[str], scan_seconds: float, include_name_match: bool) -> CandidateDevice:
    cached = load_cached_candidate(selector)
    if cached is not None:
        return cached
    candidates = await scan_candidates(scan_seconds, include_name_match=include_name_match)
    candidate = select_candidate(candidates, selector)
    save_cached_candidate(candidate)
    return candidate


async def open_session(candidate: CandidateDevice) -> RawBenchmarkSession:
    BleakClient, _, BleakError = require_bleak()
    disconnected = asyncio.Event()

    def on_disconnect(_: Any) -> None:
        disconnected.set()

    client = BleakClient(candidate.address, disconnected_callback=on_disconnect)
    try:
        await client.connect()
    except BleakError as exc:
        raise SystemExit(f"Failed to connect to {candidate.name} ({candidate.address}): {exc}") from exc

    session = RawBenchmarkSession(client, candidate)
    session._disconnect_event = disconnected
    try:
        await session.start()
    except Exception:
        await client.disconnect()
        raise
    return session


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run stripped-down BLE bandwidth benchmarks without the updater progress/UI path.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_common(subparser: argparse.ArgumentParser) -> None:
        subparser.add_argument("--device", help="Board name/address substring to auto-select")
        subparser.add_argument("--scan-seconds", type=float, default=DEFAULT_SCAN_SECONDS)
        subparser.add_argument("--include-name-match", action="store_true")

    downlink = subparsers.add_parser("downlink", help="Measure raw board-to-host notify throughput")
    add_common(downlink)
    downlink.add_argument("--size", type=int, default=DEFAULT_MAX_ATT_PAYLOAD_BYTES)
    downlink.add_argument("--count", type=int, default=4000)
    downlink.add_argument("--delay-ms", type=int, default=0)

    uplink = subparsers.add_parser("uplink", help="Measure stripped simulated host-to-board write throughput")
    add_common(uplink)
    uplink.add_argument("--bytes", type=int, default=DEFAULT_SIMULATION_BYTES)
    uplink.add_argument("--chunk-size", default="auto")
    uplink.add_argument("--burst", type=int, default=DEFAULT_BURST_LIMIT)

    combo = subparsers.add_parser("both", help="Run both stripped bandwidth probes")
    add_common(combo)
    combo.add_argument("--size", type=int, default=DEFAULT_MAX_ATT_PAYLOAD_BYTES)
    combo.add_argument("--count", type=int, default=4000)
    combo.add_argument("--delay-ms", type=int, default=0)
    combo.add_argument("--bytes", type=int, default=DEFAULT_SIMULATION_BYTES)
    combo.add_argument("--chunk-size", default="auto")
    combo.add_argument("--burst", type=int, default=DEFAULT_BURST_LIMIT)

    return parser


async def async_main(args: argparse.Namespace) -> int:
    candidate = await resolve_candidate(args.device, args.scan_seconds, args.include_name_match)
    session = await open_session(candidate)
    try:
        version = await session.fetch_fw_version()
        print(f"Board: {candidate.name} ({candidate.address})")
        print(f"Firmware: {version or 'unknown'}")
        print(f"MTU: {session.mtu_size} auto_chunk={session.derive_auto_chunk_size()}")

        if args.command in {"downlink", "both"}:
            downlink_result = await session.run_downlink_notify_test(args.size, args.count, args.delay_ms)
            board_summary = "board_rate=unavailable board_bytes=unavailable"
            if downlink_result["board_rate_Bps"] > 0 and downlink_result["board_bytes"] > 0:
                board_summary = (
                    f"board_rate={downlink_result['board_rate_Bps']} B/s "
                    f"board_bytes={downlink_result['board_bytes']}"
                )
            print(
                "Downlink raw: "
                f"{board_summary} "
                f"host_rate={downlink_result['host_rate_Bps']} B/s "
                f"host_bytes={downlink_result['host_bytes']} "
                f"host_packets={downlink_result['host_packets']}"
            )

        if args.command in {"uplink", "both"}:
            chunk_size = None if args.chunk_size == "auto" else int(args.chunk_size)
            uplink_result = await session.run_uplink_sim_test(args.bytes, chunk_size, args.burst)
            print(
                "Uplink raw sim: "
                f"rate={uplink_result['rate_Bps']} B/s "
                f"bytes={uplink_result['bytes']} "
                f"chunk={uplink_result['chunk_size']} burst={uplink_result['burst']} "
                f"seconds={uplink_result['seconds']:.2f}"
            )
        return 0
    finally:
        await session.stop()
        await session._client.disconnect()


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return asyncio.run(async_main(args))


if __name__ == "__main__":
    raise SystemExit(main())
