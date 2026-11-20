"""Bounded parsers for existing firmware runtime-headroom evidence.

The parser intentionally reports observations, provenance, and instrumentation
gaps.  It does not apply budgets, thresholds, or pass/fail policy.  A missing or
structurally unwired metric remains null even when a firmware placeholder printed
zero.
"""

from __future__ import annotations

import codecs
import io
import json
import os
import re
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import Any, BinaryIO, TextIO


SCHEMA_VERSION = 1

EVIDENCE_KINDS = ("hardware", "simulated", "fake", "trace", "unknown")
FACT_STATUSES = ("measured", "modeled", "configured", "unavailable", "deferred_hil")

DEFAULT_LIMITS = {
    "max_bytes": 1024 * 1024,
    "max_line_chars": 4096,
    "max_records": 128,
    "chunk_bytes": 4096,
}

_LOGGER_PREFIXES = (
    re.compile(r"^\d+\.\d{3}\s+\[(?:TRACE|DEBUG|INFO|WARN|ERROR)\]\s+"),
    re.compile(r"^[VDIWE]\s+\(\d+\)\s+[^:]+:\s*"),
    re.compile(r"^\[[^\]]*(?:TRACE|DEBUG|INFO|WARN|ERROR)[^\]]*\]\s+", re.IGNORECASE),
)
_ANSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_ASSIGNMENT_RE = re.compile(r"(?:^|\s)([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
_HEALTH_LINE_RE = re.compile(
    r"health:\s+revision=(0[xX][0-9A-Fa-f]+|[0-9]+)"
    r"\s+state=(0[xX][0-9A-Fa-f]+|[0-9]+)"
    r"\s+causes=(0[xX][0-9A-Fa-f]+|[0-9]+)"
)


def scrub_path(path: str | os.PathLike[str] | None,
               repo_root: str | os.PathLike[str] | None = None) -> str | None:
    """Return a stable relative path without exposing an absolute host path."""
    if path is None:
        return None
    raw = os.fspath(path)
    if not raw:
        return None

    normalized = os.path.normpath(raw)
    if os.path.isabs(normalized):
        if repo_root is not None:
            root = os.path.abspath(os.fspath(repo_root))
            candidate = os.path.abspath(normalized)
            try:
                relative = os.path.relpath(candidate, root)
            except ValueError:
                relative = ".."
            if relative != ".." and not relative.startswith(".." + os.sep):
                return Path(relative).as_posix()
        return "<external>/" + os.path.basename(normalized)

    parts = Path(normalized).parts
    if ".." in parts:
        return os.path.basename(normalized)
    result = Path(normalized).as_posix()
    if result == ".":
        return None
    return result[2:] if result.startswith("./") else result


def _clean_target(target: str | None) -> str:
    if not target:
        return "unspecified"
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(target)).strip("._-")
    return cleaned or "unspecified"


def _limits(overrides: Mapping[str, int] | None) -> dict[str, int]:
    result = dict(DEFAULT_LIMITS)
    if overrides:
        unknown = set(overrides) - set(result)
        if unknown:
            raise ValueError("unknown runtime evidence limit(s): " + ", ".join(sorted(unknown)))
        for key, value in overrides.items():
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                raise ValueError(f"{key} must be a positive integer")
            result[key] = value
    return result


def _normalize_line(line: str) -> str:
    value = _ANSI_RE.sub("", line).strip()
    changed = True
    while changed and value:
        changed = False
        prompt = re.sub(r"^(?:>\s*)+", "", value)
        if prompt != value:
            value = prompt.lstrip()
            changed = True
        for pattern in _LOGGER_PREFIXES:
            stripped = pattern.sub("", value, count=1)
            if stripped != value:
                value = stripped.lstrip()
                changed = True
                break
    return value


def _assignments(line: str) -> dict[str, str]:
    return {match.group(1): match.group(2).rstrip(",") for match in _ASSIGNMENT_RE.finditer(line)}


def _integer(value: str) -> int:
    return int(value, 10)


def _decimal(value: str) -> float:
    return float(value)


def _health_integer(value: str) -> int:
    return int(value, 16 if value.lower().startswith("0x") else 10)


def _valid_health_value(value: Any, upper_bound: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= upper_bound


def _bounded_lines(stream: BinaryIO | TextIO,
                   limits: Mapping[str, int],
                   stats: dict[str, Any]) -> Iterable[tuple[int, str, bool]]:
    """Yield CR, LF, or CRLF-delimited lines without buffering the input."""
    decoder = codecs.getincrementaldecoder("utf-8")("replace")
    remaining = limits["max_bytes"]
    current: list[str] = []
    current_chars = 0
    overlong = False
    previous_was_cr = False
    line_number = 0
    binary_mode: bool | None = None
    stopped = False

    def process_text(text: str) -> Iterable[tuple[int, str, bool]]:
        nonlocal current, current_chars, overlong, previous_was_cr, line_number
        for character in text:
            if character == "\n" and previous_was_cr:
                previous_was_cr = False
                continue
            if character == "\r" or character == "\n":
                line_number += 1
                stats["lines_seen"] = line_number
                yield line_number, "".join(current), overlong
                current = []
                current_chars = 0
                overlong = False
                previous_was_cr = character == "\r"
                continue
            previous_was_cr = False
            if current_chars < limits["max_line_chars"]:
                current.append(character)
                current_chars += 1
            elif not overlong:
                overlong = True
                stats["overlong_lines"] += 1

    while remaining > 0:
        request_size = min(limits["chunk_bytes"], remaining + 1)
        chunk = stream.read(request_size)
        if chunk == b"" or chunk == "":
            break
        if not isinstance(chunk, (bytes, str)):
            raise TypeError("runtime evidence stream.read() must return bytes or str")
        if binary_mode is None:
            binary_mode = isinstance(chunk, bytes)
        elif binary_mode != isinstance(chunk, bytes):
            raise TypeError("runtime evidence stream changed between binary and text chunks")

        if isinstance(chunk, bytes):
            if len(chunk) > remaining:
                chunk = chunk[:remaining]
                stats["input_truncated"] = True
                stopped = True
            remaining -= len(chunk)
            stats["bytes_read"] += len(chunk)
            decoded = decoder.decode(chunk, final=False)
            yield from process_text(decoded)
        else:
            accepted: list[str] = []
            for character in chunk:
                width = len(character.encode("utf-8"))
                if width > remaining:
                    stats["input_truncated"] = True
                    stopped = True
                    break
                accepted.append(character)
                remaining -= width
                stats["bytes_read"] += width
            if len(accepted) != len(chunk):
                stopped = True
            yield from process_text("".join(accepted))

        if stopped:
            break

    if remaining == 0 and not stats["input_truncated"]:
        extra = stream.read(1)
        if extra not in (b"", ""):
            stats["input_truncated"] = True

    if binary_mode:
        yield from process_text(decoder.decode(b"", final=True))
    if current or overlong:
        line_number += 1
        stats["lines_seen"] = line_number
        yield line_number, "".join(current), overlong


class _ReportBuilder:
    def __init__(self,
                 evidence_kind: str,
                 target: str,
                 source_path: str | None,
                 limits: Mapping[str, int]) -> None:
        self.evidence_kind = evidence_kind
        self.target = target
        self.source_path = source_path
        self.limits = limits
        self.facts: dict[str, dict[str, Any]] = {}
        self.records: list[dict[str, Any]] = []
        self.records_dropped = 0
        self.limitations: set[str] = {
            "all_task_stack_high_water_not_in_current_records",
            "frame_drop_counter_has_no_production_call_site",
            "non_ota_queue_high_water_not_in_current_records",
        }
        self.matched_records = 0
        self._verify_recorded_entries: int | None = None
        self._install_defaults()

    def _install_defaults(self) -> None:
        missing_status = (
            "deferred_hil" if self.evidence_kind in ("hardware", "unknown") else "unavailable"
        )
        missing_note = (
            "no matching record in supplied hardware evidence"
            if missing_status == "deferred_hil"
            else f"{self.evidence_kind} evidence does not supply this hardware metric"
        )
        for name, unit in (
            ("runtime.loop.count", "count"),
            ("runtime.loop.last_delta_ms", "ms"),
            ("runtime.loop.min_delta_ms", "ms"),
            ("runtime.loop.max_delta_ms", "ms"),
            ("runtime.i2c.errors", "count"),
            ("memory.heap.free_bytes", "bytes"),
            ("memory.heap.min_free_bytes", "bytes"),
            ("storage.flash.size_bytes", "bytes"),
            ("storage.sketch.free_bytes", "bytes"),
            ("memory.psram.free_bytes", "bytes"),
            ("ble.ota.queue.high_water_chunks", "chunks"),
            ("ble.ota.notify.max_ms", "ms"),
            ("ble.ota.write.max_ms", "ms"),
        ):
            self.set_fact(name, missing_status, None, unit, "capture", "not_in_evidence", None, None,
                          missing_note)

        for name in ("runtime.health.revision", "runtime.health.state", "runtime.health.causes"):
            self.set_fact(name, missing_status, None, None, "runtime_health", "not_in_evidence",
                          None, None, missing_note)

        self.set_fact("runtime.frames.dropped", "unavailable", None, "frames", "firmware_lifetime",
                      "unwired_firmware_counter", None, None,
                      "the production firmware has no caller for the frame-drop counter")
        self.set_fact("memory.stack.current_task.high_water_words", "unavailable", None, "words",
                      "current_task_at_report", "unwired_crash_report", None, None,
                      "the crash-report output is not wired into the current production runtime")
        self.set_fact("memory.stack.all_tasks.minimum_high_water_words", "unavailable", None, "words",
                      "all_tasks", "not_in_instrumentation", None, None,
                      "no current record reports stack headroom for all tasks")
        self.set_fact("ble.runtime.queue.high_water_events", "unavailable", None, "events",
                      "service_lifetime", "not_in_instrumentation", None, None,
                      "the generic BLE queue reports drops but not a high-water mark")
        self.set_fact("power.actual_total_ma", "deferred_hil", None, "mA", "device",
                      "operator_meter_not_in_evidence", None, None,
                      "actual current requires an inline-meter HIL capture")

        if self.evidence_kind == "fake":
            self.limitations.add("virtual_litch_runtime_metrics_unsupported")
        elif self.evidence_kind == "trace":
            self.limitations.add("trace_time_is_scenario_time_not_hardware_latency")
            self.limitations.add("trace_runtime_headroom_metrics_unsupported")
        elif self.evidence_kind == "simulated":
            self.limitations.add("simulated_observations_are_not_hardware_measurements")

    def set_fact(self,
                 name: str,
                 status: str,
                 value: Any,
                 unit: str | None,
                 scope: str,
                 method: str,
                 record_family: str | None,
                 line: int | None,
                 note: str | None = None,
                 reported_value: Any = None) -> None:
        if status not in FACT_STATUSES:
            raise ValueError(f"unsupported fact status: {status}")
        self.facts[name] = {
            "status": status,
            "value": value,
            "reported_value": reported_value,
            "unit": unit,
            "scope": scope,
            "provenance": {
                "evidence_kind": self.evidence_kind,
                "method": method,
                "source_path": self.source_path,
                "record_family": record_family,
                "line": line,
            },
            "note": note,
        }

    def retain(self, family: str, line_number: int, line: str) -> None:
        self.matched_records += 1
        if len(self.records) < self.limits["max_records"]:
            self.records.append({"family": family, "line": line_number, "text": line})
        else:
            self.records_dropped += 1

    def consume(self, line: str, line_number: int) -> str | None:
        if not line:
            return None
        if self.evidence_kind == "fake":
            return None
        if self.evidence_kind == "trace":
            return self._consume_trace(line, line_number)

        parsers = (
            self._consume_health,
            self._consume_perf,
            self._consume_heap,
            self._consume_crash,
            self._consume_ota,
            self._consume_bench_notify,
            self._consume_ledpower,
            self._consume_amptest,
        )
        for parser in parsers:
            family = parser(line, line_number)
            if family is not None:
                return family
        return None

    def _consume_health(self, line: str, line_number: int) -> str | None:
        if self.evidence_kind != "hardware":
            return None
        match = _HEALTH_LINE_RE.fullmatch(line)
        if match is None:
            return None
        revision, state, causes = (_health_integer(value) for value in match.groups())
        if not (_valid_health_value(revision, 0xFFFF)
                and _valid_health_value(state, 0xFF)
                and _valid_health_value(causes, 0xFFFF)):
            return None
        for fact, value in (
            ("runtime.health.revision", revision),
            ("runtime.health.state", state),
            ("runtime.health.causes", causes),
        ):
            self.set_fact(fact, "measured", value, None, "runtime_health",
                          "device_runtime_health_snapshot", "runtime_health", line_number)
        return "runtime_health"

    def _consume_perf(self, line: str, line_number: int) -> str | None:
        values = _assignments(line)
        if line.startswith("loops=") and {"loops", "last_dt_ms", "min_dt_ms", "max_dt_ms"} <= values.keys():
            mapping = (
                ("runtime.loop.count", "loops", "count"),
                ("runtime.loop.last_delta_ms", "last_dt_ms", "ms"),
                ("runtime.loop.min_delta_ms", "min_dt_ms", "ms"),
                ("runtime.loop.max_delta_ms", "max_dt_ms", "ms"),
            )
            for fact, key, unit in mapping:
                self.set_fact(fact, "measured", _integer(values[key]), unit, "firmware_lifetime",
                              "device_loop_telemetry", "perf_loop", line_number)
            return "perf_loop"

        required = {"dropped_frames", "i2c_errors", "min_free_heap"}
        if line.startswith("dropped_frames=") and required <= values.keys():
            dropped = _integer(values["dropped_frames"])
            self.set_fact("runtime.frames.dropped", "unavailable", None, "frames", "firmware_lifetime",
                          "unwired_firmware_counter", "perf_counters", line_number,
                          "reported placeholder retained separately; production never increments it",
                          reported_value=dropped)
            self.set_fact("runtime.i2c.errors", "measured", _integer(values["i2c_errors"]), "count",
                          "firmware_lifetime", "device_i2c_error_counter", "perf_counters", line_number)
            self.set_fact("memory.heap.min_free_bytes", "measured", _integer(values["min_free_heap"]),
                          "bytes", "firmware_lifetime", "device_min_free_heap", "perf_counters",
                          line_number)
            return "perf_counters"
        return None

    def _consume_heap(self, line: str, line_number: int) -> str | None:
        if line == "heap: unsupported":
            for fact in ("memory.heap.total_bytes", "memory.heap.used_bytes",
                         "memory.heap.free_bytes", "memory.heap.min_free_bytes"):
                self.set_fact(fact, "unavailable", None, "bytes", "capture",
                              "heap_command_unsupported", "heap_unsupported", line_number,
                              "this runtime explicitly reported heap statistics unsupported")
            return "heap_unsupported"

        values = _assignments(line)
        if line.startswith("heap: ") and {"total", "used", "free", "min_free"} <= values.keys():
            for fact, key in (
                ("memory.heap.total_bytes", "total"),
                ("memory.heap.used_bytes", "used"),
                ("memory.heap.free_bytes", "free"),
                ("memory.heap.min_free_bytes", "min_free"),
            ):
                self.set_fact(fact, "measured", _integer(values[key]), "bytes", "capture",
                              "device_heap_snapshot", "heap", line_number)
            return "heap"

        if line.startswith("hw: model=") and {"model", "rev", "cores", "cpu_mhz"} <= values.keys():
            for fact, key, unit, convert in (
                ("hardware.model", "model", None, str),
                ("hardware.revision", "rev", None, _integer),
                ("hardware.cores", "cores", "count", _integer),
                ("hardware.cpu_mhz", "cpu_mhz", "MHz", _integer),
            ):
                self.set_fact(fact, "configured", convert(values[key]), unit, "device",
                              "device_reported_configuration", "hardware", line_number)
            return "hardware"
        if line.startswith("hw: sdk=") and "sdk" in values:
            self.set_fact("hardware.sdk", "configured", values["sdk"], None, "firmware_build",
                          "device_reported_configuration", "hardware_sdk", line_number)
            return "hardware_sdk"

        if line.startswith("flash: ") and {"size", "speed_hz", "mode"} <= values.keys():
            for fact, key, unit in (
                ("storage.flash.size_bytes", "size", "bytes"),
                ("storage.flash.speed_hz", "speed_hz", "Hz"),
                ("storage.flash.mode", "mode", None),
            ):
                self.set_fact(fact, "configured", _integer(values[key]), unit, "device",
                              "device_reported_configuration", "flash", line_number)
            return "flash"
        if line.startswith("sketch: ") and {"size", "free_space"} <= values.keys():
            self.set_fact("storage.sketch.size_bytes", "measured", _integer(values["size"]), "bytes",
                          "firmware_image", "device_sketch_snapshot", "sketch", line_number)
            self.set_fact("storage.sketch.free_bytes", "measured", _integer(values["free_space"]),
                          "bytes", "firmware_partition", "device_sketch_snapshot", "sketch", line_number)
            return "sketch"
        if line.startswith("psram: ") and {"size", "free"} <= values.keys():
            self.set_fact("memory.psram.size_bytes", "configured", _integer(values["size"]), "bytes",
                          "device", "device_reported_configuration", "psram", line_number)
            self.set_fact("memory.psram.free_bytes", "measured", _integer(values["free"]), "bytes",
                          "capture", "device_psram_snapshot", "psram", line_number)
            return "psram"

        for prefix, scope in (("heap_caps_internal: ", "internal"),
                              ("heap_caps_spiram: ", "spiram")):
            if line.startswith(prefix):
                detailed = {"free", "alloc", "min_free", "largest"}
                blocks = {"free_blocks", "alloc_blocks", "total_blocks"}
                if detailed <= values.keys():
                    for suffix, key in (("free_bytes", "free"), ("allocated_bytes", "alloc"),
                                        ("min_free_bytes", "min_free"),
                                        ("largest_free_block_bytes", "largest")):
                        self.set_fact(f"memory.heap_caps.{scope}.{suffix}", "measured",
                                      _integer(values[key]), "bytes", "capture",
                                      "device_heap_caps_snapshot", f"heap_caps_{scope}", line_number)
                    return f"heap_caps_{scope}"
                if blocks <= values.keys():
                    for suffix, key in (("free_blocks", "free_blocks"),
                                        ("allocated_blocks", "alloc_blocks"),
                                        ("total_blocks", "total_blocks")):
                        self.set_fact(f"memory.heap_caps.{scope}.{suffix}", "measured",
                                      _integer(values[key]), "blocks", "capture",
                                      "device_heap_caps_snapshot", f"heap_caps_{scope}_blocks",
                                      line_number)
                    return f"heap_caps_{scope}_blocks"
        return None

    def _consume_crash(self, line: str, line_number: int) -> str | None:
        values = _assignments(line)
        reset = re.fullmatch(r"crash: reset_reason=([^\s]+) \((\d+)\)", line)
        if reset:
            self.set_fact("crash.reset_reason", "measured", reset.group(1), None, "latest_capture",
                          "device_crash_snapshot", "crash_reset", line_number)
            self.set_fact("crash.reset_reason_code", "measured", _integer(reset.group(2)), None,
                          "latest_capture", "device_crash_snapshot", "crash_reset", line_number)
            return "crash_reset"
        if line.startswith("crash: heap_free=") and {"heap_free", "min_free", "largest_free"} <= values.keys():
            for fact, key in (("crash.heap.free_bytes", "heap_free"),
                              ("crash.heap.min_free_bytes", "min_free"),
                              ("crash.heap.largest_free_block_bytes", "largest_free")):
                self.set_fact(fact, "measured", _integer(values[key]), "bytes", "report_invocation",
                              "device_crash_snapshot", "crash_heap", line_number,
                              "captured when the report is printed, not persisted at fault time")
            return "crash_heap"
        if line.startswith("crash: stack_high_water_words=") and "stack_high_water_words" in values:
            self.set_fact("memory.stack.current_task.high_water_words", "measured",
                          _integer(values["stack_high_water_words"]), "words", "current_task_at_report",
                          "device_crash_snapshot", "crash_stack", line_number,
                          "current task only; this is not an all-task minimum")
            return "crash_stack"
        if line.startswith("crash: breadcrumb_seq=") and "breadcrumb_seq" in values:
            self.set_fact("crash.breadcrumb.sequence", "measured", _integer(values["breadcrumb_seq"]),
                          "count", "report_invocation", "device_crash_snapshot", "crash_breadcrumb",
                          line_number)
            breadcrumb = line.split(" breadcrumb=", 1)
            if len(breadcrumb) == 2:
                self.set_fact("crash.breadcrumb.value", "measured", breadcrumb[1], None,
                              "report_invocation", "device_crash_snapshot", "crash_breadcrumb",
                              line_number)
            return "crash_breadcrumb"
        return None

    def _consume_ota(self, line: str, line_number: int) -> str | None:
        values = _assignments(line)
        write_required = {"status", "calls", "bytes", "total_ms", "avg_ms", "max_ms",
                          "slow_count", "stage"}
        if line.startswith("BLE OTA write metrics: ") and write_required <= values.keys():
            self.set_fact("ble.ota.write.status", "measured", values["status"], None, "ota_transfer",
                          "device_ota_write_timing", "ble_ota_write", line_number)
            for fact, key, unit in (
                ("ble.ota.write.calls", "calls", "count"),
                ("ble.ota.write.bytes", "bytes", "bytes"),
                ("ble.ota.write.total_ms", "total_ms", "ms"),
                ("ble.ota.write.avg_ms", "avg_ms", "ms"),
                ("ble.ota.write.max_ms", "max_ms", "ms"),
                ("ble.ota.write.slow_count", "slow_count", "count"),
            ):
                self.set_fact(fact, "measured", _integer(values[key]), unit, "ota_transfer",
                              "device_ota_write_timing", "ble_ota_write", line_number)
            self.set_fact("ble.ota.write.stage_capacity_bytes", "configured", _integer(values["stage"]),
                          "bytes", "firmware_build", "firmware_configuration", "ble_ota_write",
                          line_number)
            return "ble_ota_write"

        path_required = {"status", "enqueue_count", "enqueue_drop", "queue_highwater",
                         "drain_cycles", "drain_count", "drain_max_chunks", "notify_count",
                         "notify_avg_ms", "notify_max_ms"}
        if line.startswith("BLE OTA path metrics: ") and path_required <= values.keys():
            self.set_fact("ble.ota.path.status", "measured", values["status"], None,
                          "service_epoch_or_since_abort", "device_ota_path_counter", "ble_ota_path",
                          line_number)
            for fact, key, unit, scope in (
                ("ble.ota.queue.enqueue_count", "enqueue_count", "count", "service_epoch_or_since_abort"),
                ("ble.ota.queue.enqueue_drops", "enqueue_drop", "count", "service_epoch_or_since_abort"),
                ("ble.ota.queue.high_water_chunks", "queue_highwater", "chunks", "queue_lifetime"),
                ("ble.ota.drain.cycles", "drain_cycles", "count", "service_epoch_or_since_abort"),
                ("ble.ota.drain.chunks", "drain_count", "chunks", "service_epoch_or_since_abort"),
                ("ble.ota.drain.max_chunks_per_cycle", "drain_max_chunks", "chunks", "service_epoch_or_since_abort"),
                ("ble.ota.notify.count", "notify_count", "count", "service_epoch_or_since_abort"),
            ):
                self.set_fact(fact, "measured", _integer(values[key]), unit, scope,
                              "device_ota_path_counter", "ble_ota_path", line_number)
            notify_count = _integer(values["notify_count"])
            for fact, key in (("ble.ota.notify.avg_ms", "notify_avg_ms"),
                              ("ble.ota.notify.max_ms", "notify_max_ms")):
                reported = _integer(values[key])
                if notify_count == 0:
                    self.set_fact(fact, "unavailable", None, "ms", "service_epoch_or_since_abort",
                                  "device_ota_notify_timing", "ble_ota_path", line_number,
                                  "no notify observations; firmware emitted a zero placeholder",
                                  reported_value=reported)
                else:
                    self.set_fact(fact, "measured", reported, "ms",
                                  "service_epoch_or_since_abort", "device_ota_notify_timing",
                                  "ble_ota_path", line_number,
                                  "successful-transfer output precedes the final done notification")
            return "ble_ota_path"
        return None

    def _record_bench_notify(self,
                             detail: str,
                             line_number: int,
                             family: str) -> str | None:
        match = re.fullmatch(
            r"size=(\d+) ok=(\d+) fail=(\d+) elapsed_ms=(\d+) rate_Bps=(\d+)", detail.strip()
        )
        if not match:
            return None
        size, ok, failed, elapsed, rate = (_integer(item) for item in match.groups())
        prefix = f"ble.bench_notify.size_{size}"
        for fact, value, unit in (
            (f"{prefix}.packet_size_bytes", size, "bytes"),
            (f"{prefix}.ok_count", ok, "count"),
            (f"{prefix}.failure_count", failed, "count"),
            (f"{prefix}.elapsed_ms", elapsed, "ms"),
            (f"{prefix}.rate_bytes_per_second", rate, "bytes/s"),
        ):
            self.set_fact(fact, "measured", value, unit, "notify_test",
                          "device_ble_notify_benchmark", family, line_number,
                          "elapsed_ms is clamped to at least one millisecond by firmware")
        return family

    def _consume_bench_notify(self, line: str, line_number: int) -> str | None:
        if line.startswith("{"):
            try:
                record = json.loads(line)
            except (TypeError, ValueError):
                record = None
            if isinstance(record, Mapping) and record.get("st") == "bench_notify":
                detail = record.get("detail")
                if isinstance(detail, str):
                    return self._record_bench_notify(detail, line_number, "bench_notify")
        direct = re.fullmatch(r"bench_notify:?\s+(.+)", line)
        if direct:
            return self._record_bench_notify(direct.group(1), line_number, "bench_notify")
        return None

    def _consume_ledpower(self, line: str, line_number: int) -> str | None:
        if not line.startswith("ledpower: "):
            return None
        values = _assignments(line)
        if {"brownout_latched", "led_limit_ma", "total_limit_ma", "baseline_ma"} <= values.keys():
            for fact, key, unit in (
                ("power.policy.brownout_latched", "brownout_latched", None),
                ("power.policy.led_limit_ma", "led_limit_ma", "mA"),
                ("power.policy.total_limit_ma", "total_limit_ma", "mA"),
                ("power.model.baseline_ma", "baseline_ma", "mA"),
            ):
                self.set_fact(fact, "configured", _integer(values[key]), unit, "current_policy",
                              "device_reported_power_configuration", "ledpower_policy", line_number,
                              "baseline is a configured/calibrated model input, not a live current reading")
            return "ledpower_policy"
        modeled = {"last_modeled_requested_ma", "last_modeled_applied_ma",
                   "last_modeled_total_ma", "last_scale_pct"}
        if modeled <= values.keys():
            for fact, key, unit in (
                ("power.model.requested_led_ma", "last_modeled_requested_ma", "mA"),
                ("power.model.applied_led_ma", "last_modeled_applied_ma", "mA"),
                ("power.model.total_ma", "last_modeled_total_ma", "mA"),
                ("power.model.scale_percent", "last_scale_pct", "%"),
            ):
                self.set_fact(fact, "modeled", _integer(values[key]), unit, "latest_led_frame",
                              "firmware_power_model", "ledpower_model", line_number)
            return "ledpower_model"
        cc = {"cc_raw1", "cc_raw2", "advertised", "strong_samples", "confirmed",
              "unstable_latched"}
        if cc <= values.keys():
            self.set_fact("power.usb_cc.raw1", "measured", _integer(values["cc_raw1"]), "ADC count",
                          "latest_poll", "device_adc_snapshot", "ledpower_cc", line_number)
            self.set_fact("power.usb_cc.raw2", "measured", _integer(values["cc_raw2"]), "ADC count",
                          "latest_poll", "device_adc_snapshot", "ledpower_cc", line_number)
            sample_parts = values["strong_samples"].split("/", 1)
            if len(sample_parts) == 2:
                self.set_fact("power.usb_cc.strong_samples", "measured", _integer(sample_parts[0]),
                              "count", "current_confirmation", "firmware_power_policy", "ledpower_cc",
                              line_number)
                self.set_fact("power.usb_cc.strong_samples_required", "configured",
                              _integer(sample_parts[1]), "count", "firmware_build",
                              "firmware_configuration", "ledpower_cc", line_number)
            for fact, key in (("power.usb_cc.advertised", "advertised"),
                              ("power.usb_cc.confirmed", "confirmed"),
                              ("power.usb_cc.unstable_latched", "unstable_latched")):
                converter = str if key == "advertised" else _integer
                self.set_fact(fact, "modeled", converter(values[key]), None, "current_policy",
                              "firmware_power_policy", "ledpower_cc", line_number)
            return "ledpower_cc"
        monitor = {"cc_active_pin", "cc_monitor_raw", "downgrade_pending", "pair_recheck_pending",
                   "monitor_interval_ms", "debounce_ms"}
        if monitor <= values.keys():
            self.set_fact("power.usb_cc.active_pin", "modeled", values["cc_active_pin"], None,
                          "current_policy", "firmware_power_policy", "ledpower_cc_monitor", line_number)
            self.set_fact("power.usb_cc.monitor_raw", "measured", _integer(values["cc_monitor_raw"]),
                          "ADC count", "latest_poll", "device_adc_snapshot", "ledpower_cc_monitor",
                          line_number)
            for fact, key in (("power.usb_cc.downgrade_pending", "downgrade_pending"),
                              ("power.usb_cc.pair_recheck_pending", "pair_recheck_pending")):
                self.set_fact(fact, "modeled", _integer(values[key]), None, "current_policy",
                              "firmware_power_policy", "ledpower_cc_monitor", line_number)
            for fact, key in (("power.usb_cc.monitor_interval_ms", "monitor_interval_ms"),
                              ("power.usb_cc.debounce_ms", "debounce_ms")):
                self.set_fact(fact, "configured", _integer(values[key]), "ms", "firmware_build",
                              "firmware_configuration", "ledpower_cc_monitor", line_number)
            return "ledpower_cc_monitor"
        thresholds = {"cc_thresh_500", "cc_thresh_1500", "cc_thresh_3000", "strong_enter_raw",
                      "entry_candidate"}
        if thresholds <= values.keys():
            for fact, key in (
                ("power.usb_cc.threshold_500_raw", "cc_thresh_500"),
                ("power.usb_cc.threshold_1500_raw", "cc_thresh_1500"),
                ("power.usb_cc.threshold_3000_raw", "cc_thresh_3000"),
                ("power.usb_cc.strong_enter_raw", "strong_enter_raw"),
            ):
                self.set_fact(fact, "configured", _integer(values[key]), "ADC count", "current_policy",
                              "firmware_configuration", "ledpower_cc_thresholds", line_number)
            self.set_fact("power.usb_cc.entry_candidate", "modeled", _integer(values["entry_candidate"]),
                          None, "current_policy", "firmware_power_policy", "ledpower_cc_thresholds",
                          line_number)
            return "ledpower_cc_thresholds"
        return None

    def _consume_amptest(self, line: str, line_number: int) -> str | None:
        if line.startswith("amptest verify: "):
            values = _assignments(line)
            if line.startswith("amptest verify: recorded_entries=") and "recorded_entries" in values:
                count = _integer(values["recorded_entries"])
                self._verify_recorded_entries = count
                self.set_fact("power.hil.verify.recorded_entries", "measured", count, "count",
                              "verification_session", "operator_meter_session", "amptest_verify_count",
                              line_number)
                return "amptest_verify_count"
            summary = re.fullmatch(
                r"amptest verify: ([a-z_]+) predicted_total=(\d+) mA actual=(pending|\d+ mA(?: delta=-?\d+ mA)?)",
                line,
            )
            if summary:
                step, predicted, actual = summary.groups()
                prefix = f"power.hil.verify.{step}"
                self.set_fact(f"{prefix}.predicted_total_ma", "modeled", _integer(predicted), "mA",
                              "verification_step", "firmware_power_model", "amptest_verify_step",
                              line_number)
                if actual == "pending":
                    self.set_fact(f"{prefix}.actual_total_ma", "deferred_hil", None, "mA",
                                  "verification_step", "operator_meter_not_recorded",
                                  "amptest_verify_step", line_number,
                                  "operator has not recorded an inline-meter value")
                else:
                    actual_match = re.fullmatch(r"(\d+) mA delta=(-?\d+) mA", actual)
                    if actual_match:
                        actual_ma, delta_ma = (_integer(item) for item in actual_match.groups())
                        self.set_fact(f"{prefix}.actual_total_ma", "measured", actual_ma, "mA",
                                      "verification_step", "operator_inline_meter",
                                      "amptest_verify_step", line_number)
                        self.set_fact(f"{prefix}.delta_ma", "measured", delta_ma, "mA",
                                      "verification_step", "meter_minus_power_model",
                                      "amptest_verify_step", line_number)
                        self.set_fact("power.actual_total_ma", "measured", actual_ma, "mA",
                                      f"verification_step:{step}", "operator_inline_meter",
                                      "amptest_verify_step", line_number)
                return "amptest_verify_step"
            if line.startswith("amptest verify: max_abs_delta=") and "max_abs_delta" in values:
                reported = _integer(values["max_abs_delta"])
                if self._verify_recorded_entries == 0:
                    self.set_fact("power.hil.verify.max_abs_delta_ma", "deferred_hil", None, "mA",
                                  "verification_session", "operator_meter_not_recorded",
                                  "amptest_verify_max_delta", line_number,
                                  "firmware emitted zero with no recorded measurements",
                                  reported_value=reported)
                else:
                    self.set_fact("power.hil.verify.max_abs_delta_ma", "measured", reported, "mA",
                                  "verification_session", "meter_minus_power_model",
                                  "amptest_verify_max_delta", line_number)
                return "amptest_verify_max_delta"
            return None

        if not line.startswith("amptest: "):
            return None
        values = _assignments(line)
        if line.startswith("amptest: recorded_entries=") and "recorded_entries" in values:
            self.set_fact("power.hil.calibration.recorded_entries", "measured",
                          _integer(values["recorded_entries"]), "count", "calibration_session",
                          "operator_meter_session", "amptest_count", line_number)
            return "amptest_count"
        baseline = re.fullmatch(r"amptest: (core|ambient|rail)=(pending|\d+ mA)", line)
        if baseline:
            label, value = baseline.groups()
            fact = f"power.hil.calibration.{label}_ma"
            if value == "pending":
                self.set_fact(fact, "deferred_hil", None, "mA", "calibration_session",
                              "operator_meter_not_recorded", "amptest_baseline", line_number)
            else:
                measured = _integer(value.split()[0])
                self.set_fact(fact, "measured", measured, "mA", "calibration_session",
                              "operator_inline_meter", "amptest_baseline", line_number)
                self.set_fact("power.actual_total_ma", "measured", measured, "mA",
                              f"calibration_baseline:{label}", "operator_inline_meter",
                              "amptest_baseline", line_number)
            return "amptest_baseline"
        delta = re.fullmatch(r"amptest: (ambient_delta_vs_core|rail_delta_vs_ambient)=(-?\d+) mA", line)
        if delta:
            self.set_fact(f"power.hil.calibration.{delta.group(1)}_ma", "measured",
                          _integer(delta.group(2)), "mA", "calibration_session",
                          "operator_meter_derived", "amptest_delta", line_number)
            return "amptest_delta"
        if re.fullmatch(r"amptest: (red|green|blue|white)=missing_led_baseline", line):
            label = line.split(": ", 1)[1].split("=", 1)[0]
            self.set_fact(f"power.model.calibration.{label}.fullscale_ma", "deferred_hil", None, "mA",
                          "calibration_session", "operator_meter_not_recorded", "amptest_fit",
                          line_number, "LED baseline required before fitting")
            return "amptest_fit"
        fit = re.fullmatch(
            r"amptest: (red|green|blue|white) samples=(\d+) fullscale=(\d+) mA "
            r"delta_vs_([a-z]+)=(-?\d+) mA per_led=(\d+\.\d+) mA "
            r"per_channel=(\d+\.\d+) mA residual_max=(\d+) mA",
            line,
        )
        if fit:
            label, samples, fullscale, baseline_label, delta_ma, per_led, per_channel, residual = fit.groups()
            prefix = f"power.model.calibration.{label}"
            for fact, value, unit in (
                (f"{prefix}.samples", _integer(samples), "count"),
                (f"{prefix}.fullscale_ma", _integer(fullscale), "mA"),
                (f"{prefix}.delta_ma", _integer(delta_ma), "mA"),
                (f"{prefix}.per_led_ma", _decimal(per_led), "mA"),
                (f"{prefix}.per_channel_ma", _decimal(per_channel), "mA"),
                (f"{prefix}.residual_max_ma", _integer(residual), "mA"),
            ):
                self.set_fact(fact, "modeled", value, unit, "calibration_session",
                              "interpolated_inline_meter_model", "amptest_fit", line_number,
                              f"fit is relative to {baseline_label}")
            return "amptest_fit"
        derived = re.fullmatch(
            r"amptest: (rgb|rgbw)_derived_fullscale=(\d+) mA delta_vs_([a-z]+)=(-?\d+) mA "
            r"per_led=(\d+\.\d+) mA per_channel=(\d+\.\d+) mA",
            line,
        )
        if derived:
            label, total, baseline_label, delta_ma, per_led, per_channel = derived.groups()
            prefix = f"power.model.calibration.{label}"
            for fact, value, unit in (
                (f"{prefix}.fullscale_ma", _integer(total), "mA"),
                (f"{prefix}.delta_ma", _integer(delta_ma), "mA"),
                (f"{prefix}.per_led_ma", _decimal(per_led), "mA"),
                (f"{prefix}.per_channel_ma", _decimal(per_channel), "mA"),
            ):
                self.set_fact(fact, "modeled", value, unit, "calibration_session",
                              "derived_inline_meter_model", "amptest_derived", line_number,
                              f"model is relative to {baseline_label}")
            return "amptest_derived"
        return None

    def _consume_trace(self, line: str, line_number: int) -> str | None:
        if not line.startswith("{"):
            return None
        try:
            record = json.loads(line)
        except (TypeError, ValueError):
            return None
        if not isinstance(record, Mapping):
            return None
        record_type = record.get("type")
        if record_type == "header":
            for fact, key in (("trace.format_version", "ver"), ("trace.build", "build"),
                              ("trace.board", "board"), ("trace.rng_seed", "rng_seed")):
                if key in record and isinstance(record[key], (str, int)) and not isinstance(record[key], bool):
                    self.set_fact(fact, "configured", record[key], None, "trace_scenario",
                                  "trace_header", "trace_header", line_number)
            return "trace_header"
        if record_type == "end":
            t_ms = record.get("t_ms")
            if isinstance(t_ms, int) and not isinstance(t_ms, bool):
                self.set_fact("trace.scenario_time_ms", "modeled", t_ms, "ms", "trace_scenario",
                              "trace_scenario_clock", "trace_end", line_number,
                              "scenario time is not wall-clock or hardware loop timing")
            stats = record.get("stats")
            if isinstance(stats, Mapping):
                for key in ("events", "frames", "engine"):
                    value = stats.get(key)
                    if isinstance(value, int) and not isinstance(value, bool):
                        self.set_fact(f"trace.{key}_records", "modeled", value, "count",
                                      "trace_scenario", "trace_recorder_count", "trace_end",
                                      line_number)
            return "trace_end"
        return None

    def consume_fake_snapshot(self, snapshot: Mapping[str, Any]) -> None:
        frame: Mapping[str, Any] = snapshot
        for key in ("physical_frame", "latest_frame", "frame"):
            candidate = snapshot.get(key)
            if isinstance(candidate, Mapping):
                frame = candidate
                break
        supported = (
            ("estimated_led_current_ma", "power.fake.estimated_led_ma", "mA", int),
            ("applied_led_current_ma", "power.fake.applied_led_ma", "mA", int),
            ("applied_brightness_scale_65535", "power.fake.applied_scale_65535", None, int),
            ("power_limited", "power.fake.limited", None, bool),
        )
        consumed = 0
        for key, fact, unit, expected_type in supported:
            value = frame.get(key)
            valid = isinstance(value, expected_type)
            if expected_type is int and isinstance(value, bool):
                valid = False
            if valid:
                self.set_fact(fact, "modeled", value, unit, "latest_fake_physical_frame",
                              "virtual_litch_power_model", "fake_snapshot", None,
                              "VirtualLitch physical output is production-model output, not measured current")
                consumed += 1
        if consumed:
            self.retain("fake_snapshot", 0, f"supported_fields={consumed}")
        ignored = sorted(set(frame) & {
            "min_free_heap", "heap_free", "stack_high_water_words", "queue_highwater",
            "dropped_frames", "loop_delta_ms",
        })
        if ignored:
            self.limitations.add("unsupported_fake_snapshot_runtime_fields_ignored")

        runtime_health = snapshot.get("runtime_health")
        health_valid = (
            isinstance(runtime_health, Mapping)
            and _valid_health_value(runtime_health.get("revision"), 0xFFFF)
            and _valid_health_value(runtime_health.get("state"), 0xFF)
            and _valid_health_value(runtime_health.get("causes"), 0xFFFF)
        )
        if health_valid:
            for fact, value in (
                ("runtime.health.revision", runtime_health["revision"]),
                ("runtime.health.state", runtime_health["state"]),
                ("runtime.health.causes", runtime_health["causes"]),
            ):
                self.set_fact(fact, "modeled", value, None, "runtime_health",
                              "fake_runtime_health_snapshot", "runtime_health", None,
                              "VirtualLitch runtime health is simulated evidence")
            self.retain("fake_runtime_health", 0, "runtime_health=complete")
        elif runtime_health is not None:
            self.limitations.add("invalid_fake_runtime_health_ignored")

    def finish(self, stats: Mapping[str, Any]) -> dict[str, Any]:
        present = self.matched_records > 0
        incomplete_reasons = []
        if stats["input_truncated"]:
            incomplete_reasons.append("input_truncated")
        if stats["overlong_lines"]:
            incomplete_reasons.append("overlong_lines")
        incomplete = bool(incomplete_reasons)

        if incomplete:
            hil_status = "unavailable"
            hil_present = False
            self.limitations.add("incomplete_evidence_is_diagnostic_only")
        elif self.evidence_kind == "hardware" and present:
            hil_status = "measured"
            hil_present = True
        elif self.evidence_kind in ("hardware", "unknown"):
            hil_status = "deferred_hil"
            hil_present = False
            self.limitations.add("hardware_in_loop_evidence_not_present")
        else:
            hil_status = "unavailable"
            hil_present = False
            self.limitations.add("hardware_in_loop_evidence_not_present")

        for fact in self.facts.values():
            provenance = fact.get("provenance")
            fact["diagnostic_only"] = bool(
                incomplete
                and (
                    fact.get("value") is not None
                    or fact.get("reported_value") is not None
                    or (
                        isinstance(provenance, Mapping)
                        and provenance.get("record_family") is not None
                    )
                )
            )

        evidence = {
            "kind": self.evidence_kind,
            "path": self.source_path,
            "present": present,
            "status": "incomplete" if incomplete else "complete",
            "diagnostic_only": incomplete,
            "incomplete_reasons": incomplete_reasons,
            "bytes_read": stats["bytes_read"],
            "input_truncated": stats["input_truncated"],
            "lines_seen": stats["lines_seen"],
            "overlong_lines": stats["overlong_lines"],
            "matched_records": self.matched_records,
            "records_retained": len(self.records),
            "records_dropped": self.records_dropped,
            "limits": {key: self.limits[key] for key in sorted(self.limits)},
            "records": list(self.records),
        }
        return {
            "schema_version": SCHEMA_VERSION,
            "kind": "runtime_headroom",
            "status": "incomplete" if incomplete else "complete",
            "measurement_eligible": not incomplete,
            "target": self.target,
            "hardware_in_loop": {
                "status": hil_status,
                "present": hil_present,
                "provenance": {
                    "evidence_kind": self.evidence_kind,
                    "source_path": self.source_path,
                },
            },
            "evidence": evidence,
            "facts": {key: self.facts[key] for key in sorted(self.facts)},
            "limitations": sorted(self.limitations),
        }


def parse_stream(stream: BinaryIO | TextIO,
                 *,
                 evidence_kind: str = "unknown",
                 target: str = "unspecified",
                 source_path: str | os.PathLike[str] | None = None,
                 repo_root: str | os.PathLike[str] | None = None,
                 fake_snapshot: Mapping[str, Any] | None = None,
                 limits: Mapping[str, int] | None = None) -> dict[str, Any]:
    """Parse a serial/trace stream into a deterministic JSON-ready report."""
    if evidence_kind not in EVIDENCE_KINDS:
        raise ValueError("evidence_kind must be one of: " + ", ".join(EVIDENCE_KINDS))
    if fake_snapshot is not None and evidence_kind != "fake":
        raise ValueError("fake_snapshot requires evidence_kind='fake'")
    active_limits = _limits(limits)
    stats: dict[str, Any] = {
        "bytes_read": 0,
        "input_truncated": False,
        "lines_seen": 0,
        "overlong_lines": 0,
    }
    builder = _ReportBuilder(evidence_kind, _clean_target(target),
                             scrub_path(source_path, repo_root), active_limits)
    for line_number, raw_line, overlong in _bounded_lines(stream, active_limits, stats):
        line = _normalize_line(raw_line)
        try:
            family = builder.consume(line, line_number)
        except (OverflowError, ValueError):
            if not overlong and not stats["input_truncated"]:
                raise
            family = None
        if family is not None:
            builder.retain(family, line_number, line)
    if fake_snapshot is not None:
        builder.consume_fake_snapshot(fake_snapshot)
    return builder.finish(stats)


def parse_file(path: str | os.PathLike[str],
               *,
               evidence_kind: str = "unknown",
               target: str = "unspecified",
               repo_root: str | os.PathLike[str] | None = None,
               repository_root: str | os.PathLike[str] | None = None,
               limits: Mapping[str, int] | None = None) -> dict[str, Any]:
    """Open and parse a bounded evidence file without loading it wholesale."""
    if repo_root is not None and repository_root is not None:
        if os.path.abspath(os.fspath(repo_root)) != os.path.abspath(os.fspath(repository_root)):
            raise ValueError("repo_root and repository_root disagree")
    if repo_root is None:
        repo_root = repository_root
    if evidence_kind == "fake":
        active_limits = _limits(limits)
        with open(path, "rb") as stream:
            payload = stream.read(active_limits["max_bytes"] + 1)
        if len(payload) > active_limits["max_bytes"]:
            raise ValueError("fake snapshot exceeds max_bytes")
        try:
            snapshot = json.loads(payload)
        except (UnicodeDecodeError, ValueError) as error:
            raise ValueError("fake snapshot must be bounded UTF-8 JSON") from error
        if not isinstance(snapshot, Mapping):
            raise ValueError("fake snapshot JSON must contain an object")
        return parse_stream(io.BytesIO(payload), evidence_kind="fake", target=target,
                            source_path=path, repo_root=repo_root,
                            fake_snapshot=snapshot, limits=active_limits)
    with open(path, "rb") as stream:
        return parse_stream(stream, evidence_kind=evidence_kind, target=target,
                            source_path=path, repo_root=repo_root, limits=limits)


def parse_fake_snapshot(snapshot: Mapping[str, Any],
                        *,
                        target: str = "unspecified",
                        source_path: str | os.PathLike[str] | None = None,
                        repo_root: str | os.PathLike[str] | None = None,
                        limits: Mapping[str, int] | None = None) -> dict[str, Any]:
    """Report the modeled fields, and explicit gaps, in a VirtualLitch snapshot."""
    if not isinstance(snapshot, Mapping):
        raise TypeError("fake snapshot must be a mapping")
    return parse_stream(io.BytesIO(), evidence_kind="fake", target=target,
                        source_path=source_path, repo_root=repo_root,
                        fake_snapshot=snapshot, limits=limits)


def combine_reports(reports: Iterable[Mapping[str, Any]],
                    expected_targets: Iterable[str] = ("s3", "c3")) -> dict[str, Any]:
    """Nest reports by target while retaining unavailable expected targets."""
    grouped: dict[str, list[Mapping[str, Any]]] = {}
    for report in reports:
        if not isinstance(report, Mapping) or report.get("kind") != "runtime_headroom":
            raise ValueError("combine_reports accepts runtime_headroom report mappings")
        target = _clean_target(str(report.get("target") or "unspecified"))
        grouped.setdefault(target, []).append(report)

    targets = {_clean_target(str(target)) for target in expected_targets}
    targets.update(grouped)
    status_rank = {"unavailable": 0, "deferred_hil": 1, "configured": 2,
                   "modeled": 3, "measured": 4}
    combined: dict[str, Any] = {}
    incomplete_targets: list[str] = []
    for target in sorted(targets):
        target_reports = list(grouped.get(target, ()))
        target_reports.sort(key=lambda item: json.dumps(item, sort_keys=True, separators=(",", ":")))
        status = "unavailable"
        hil_status = "deferred_hil"
        hil_present = False
        target_incomplete = False
        for report in target_reports:
            evidence = report.get("evidence")
            if report.get("status") == "incomplete":
                target_incomplete = True
            elif isinstance(evidence, Mapping):
                target_incomplete = target_incomplete or bool(
                    evidence.get("input_truncated") or evidence.get("overlong_lines")
                )
        if target_incomplete:
            incomplete_targets.append(target)
            hil_status = "unavailable"
        elif target_reports:
            candidate_statuses: list[str] = []
            hil_statuses: list[str] = []
            for report in target_reports:
                hil = report.get("hardware_in_loop")
                if isinstance(hil, Mapping) and isinstance(hil.get("status"), str):
                    hil_statuses.append(str(hil["status"]))
                    hil_present = hil_present or bool(hil.get("present"))
                evidence = report.get("evidence")
                if not isinstance(evidence, Mapping) or not evidence.get("present"):
                    continue
                facts = report.get("facts")
                if isinstance(facts, Mapping):
                    for fact in facts.values():
                        if (isinstance(fact, Mapping) and fact.get("value") is not None and
                                fact.get("status") in status_rank):
                            candidate_statuses.append(str(fact["status"]))
            if candidate_statuses:
                status = max(candidate_statuses, key=status_rank.__getitem__)
            if hil_statuses:
                hil_status = max(hil_statuses, key=status_rank.__getitem__)
        combined[target] = {
            "status": status,
            "evidence_status": (
                "incomplete"
                if target_incomplete
                else "complete"
                if target_reports
                else "unavailable"
            ),
            "hardware_in_loop": {
                "status": hil_status,
                "present": hil_present,
                "note": (
                    "incomplete evidence is retained for diagnostics only"
                    if target_incomplete
                    else None
                    if hil_present
                    else "no measured hardware-in-loop evidence supplied for this target"
                ),
            },
            "reports": target_reports,
        }

    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "runtime_headroom_bundle",
        "status": "incomplete" if incomplete_targets else "complete",
        "measurement_eligible": not incomplete_targets,
        "incomplete_targets": incomplete_targets,
        "targets": combined,
    }


# Verbose aliases make call sites self-documenting without changing the compact API.
parse_runtime_stream = parse_stream
parse_runtime_file = parse_file
