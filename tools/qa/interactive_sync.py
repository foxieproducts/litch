#!/usr/bin/env python3
"""Run the bounded interactive app/board synchronization journeys."""

from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time
from typing import Iterable

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.dev.cli import COMMAND_TIMEOUT_SECONDS
from tools.dev.runner import Runner


FAST_NATIVE_TESTS = (
    "test_firmware_render_latency",
    "test_litchcraft_persistence",
    "test_virtual_litch_session",
)

FAST_FLUTTER_TESTS = (
    "test/litchcraft_concurrent_paint_test.dart",
    "test/litchcraft_stroke_batch_benchmark_test.dart",
    "test/settings_screen_test.dart",
    "test/app_console_screen_test.dart",
)

BRIDGE_FLUTTER_TESTS = (
    "test/virtual_litch_interactive_sync_journey_test.dart",
    "test/virtual_litch_mode_latency_test.dart",
    "test/virtual_litch_bootstrap_journey_test.dart",
    "test/virtual_litch_coaching_journey_test.dart",
    "test/virtual_litch_clock_setup_journey_test.dart",
    "test/virtual_litch_cpu_duck_journey_test.dart",
    "test/virtual_litch_cold_attach_journey_test.dart",
    "test/virtual_litch_native_cleanup_journey_test.dart",
    "test/connection_session_received_head_test.dart",
    "test/litchcraft_native_interop_test.dart",
    "test/game_history_native_rebase_test.dart",
)

MIXED_WORKLOAD_NAME = "seeded mixed Craft workload preserves order across physical Undo and reconnect"
COLD_ATTACH_TEST = "test/virtual_litch_cold_attach_journey_test.dart"
REQUIRED_FLUTTER_CASES = {
    "test/virtual_litch_native_cleanup_journey_test.dart": (
        "Object timeout renews the peripheral lifetime after local GATT cleanup "
        "(variant: TargetPlatform.android)",
    ),
    COLD_ATTACH_TEST: tuple(f"{MIXED_WORKLOAD_NAME} (variant: TargetPlatform.{platform})"
                            for platform in ("iOS", "android")) + (
        "closed native Store capacity keeps rapid Craft edits and Exit recoverable "
        "(variant: TargetPlatform.android)",
        "passive native mode Craft precedes local draft and orientation "
        "(variant: TargetPlatform.android)",
        "passive native mode Play precedes local draft and orientation "
        "(variant: TargetPlatform.android)",
    ) + tuple(
        "app background link loss then raw physical Exit with 21 completed strokes: "
        "Craft Play navigation keeps board controls available "
        f"(variant: TargetPlatform.{platform})" for platform in ("iOS", "android")
    ),
    "test/connection_session_received_head_test.dart": (
        "Store submission observes acceptance after native return with six grants left",
        "Object observation distinguishes a genuine grant wait and retires exactly once",
    ),
}
FLUTTER_BACKENDS = {test: ("LITCH_VIRTUAL_BRIDGE",) for test in BRIDGE_FLUTTER_TESTS
                    if test.startswith("test/virtual_litch_")}
FLUTTER_BACKENDS["test/litchcraft_native_interop_test.dart"] = ("LITCHCRAFT_NATIVE_RUNNER",)
FLUTTER_BACKENDS["test/game_history_native_rebase_test.dart"] = ("LITCH_HISTORY_REBASE_WIRE",)

SOFTWARE_PROFILES = ("focused", "fast", "bridge", "all", "full")
PHYSICAL_CHECKS = ("drawing", "frame_edit", "playback", "board_input")
MAX_PHYSICAL_INPUT_BYTES = 1024 * 1024
MAX_FAILURE_LABELS = 64
MAX_FLUTTER_RECORD_CHARS = 1024 * 1024
FLUTTER_DIAGNOSTIC_CHARS = 16 * 1024
FLUTTER_PROGRESS_SECONDS = 30
FULL_FLUTTER_TIMEOUT_SECONDS = 15 * 60
# Shards retain the full gate's safety bound; 360s is a reported stage target.
SHARD_FLUTTER_TIMEOUT_SECONDS = FULL_FLUTTER_TIMEOUT_SECONDS
FLUTTER_TOTAL_SHARDS = 12
SHARD_SCHEMA = 1
SHARD_BACKEND_KEYS = ("LITCH_VIRTUAL_BRIDGE", "LITCHCRAFT_NATIVE_RUNNER",
                      "LITCH_HISTORY_REBASE_WIRE")
TOOLCHAIN_FIELDS = ("frameworkVersion", "frameworkRevision", "engineRevision", "dartSdkVersion")
MAX_SHARD_RECEIPT_BYTES = 4 * 1024 * 1024


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _integer(record: dict, key: str, minimum: int = 0) -> int:
    value = record.get(key)
    _require(type(value) is int and value >= minimum, f"invalid {key}")
    return value


def _distribution(values: list[float]) -> dict:
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    return {
        "n": len(values),
        "mean": statistics.mean(values),
        "median": statistics.median(values),
        "p95": ordered[math.ceil(len(values) * 0.95) - 1],
        "max": max(values),
    }


def _utc(value: str) -> datetime:
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    _require(parsed.utcoffset() is not None, "UTC workload bounds must include timezone")
    return parsed


def _object_timing(records: list[dict], upload: dict, used: set[int],
                   transfers: set[tuple[int, int]]) -> dict:
    """Join one successful upload to its session-clock submission observations."""
    mapping = next((record for record in records
                    if record["seq"] == upload["seq"] - 1), {})
    _require(mapping.get("event") == "litchcraft.object_transfer" and
             mapping.get("accepted") is True and not mapping.get("fields_omitted"),
             "missing consecutive Object transfer mapping")
    life, request = _integer(mapping, "life", 1), _integer(mapping, "request", 1)
    _require(request <= 0xffff, "invalid Object transfer request")
    _require(_integer(mapping, "session", 1) == upload["session"] and
             _integer(mapping, "generation", 1) == upload["generation"],
             "Object transfer mapping authority mismatch")
    key = (life, request)
    _require(key not in transfers, "reused Object transfer evidence")
    transfers.add(key)
    rows = [record for record in records
            if record.get("event") == "litchcraft.object_submission" and
            (record.get("life"), record.get("request")) == key]
    _require(bool(rows), "missing Object submission evidence for mapped lifetime/request")
    _require(len(rows) == _integer(mapping, "submissions", 1),
             "missing or extra Object submission rows")
    samples, gaps = [], []
    previous_done = previous_last = None
    for row in rows:
        _require((_integer(row, "life", 1), _integer(row, "request", 1)) == key,
                 "invalid Object submission authority")
        _require(row["seq"] < mapping["seq"] and row["seq"] not in used,
                 "late or reused Object submission evidence")
        _require(not row.get("fields_omitted") and row.get("outcome") == "accepted",
                 "failed or incomplete Object submission")
        first, last, count = (_integer(row, field, 1) for field in ("first", "last", "count"))
        _require(last - first + 1 == count, "invalid atomic Object sequence interval")
        queued, sent, returned, accepted, done = (
            _integer(row, field) for field in
            ("queued_us", "first_send_us", "last_return_us", "accepted_us", "done_us"))
        grant, native = (_integer(row, field) for field in ("grant_wait_us", "native_us"))
        _require(queued <= sent <= returned <= done and sent <= accepted <= done,
                 "invalid Object submission endpoint order")
        _require(grant <= done - queued and native <= done - queued,
                 "invalid Object submission duration")
        if previous_done is not None:
            _require(queued >= previous_done and first > previous_last,
                     "overlapping or reordered Object submissions")
            # Both values belong to the exact same activation life and request.
            gaps.append(queued - previous_done)
        samples.append({"native_us": native, "grant_wait_us": grant,
                        "acceptance_tail_us": max(0, accepted - returned), "objects": count})
        previous_done, previous_last = done, last
        used.add(row["seq"])
    _require(sum(sample["objects"] for sample in samples) == _integer(mapping, "objects", 1),
             "Object submission total does not match completed transfer")
    return {
        "life": life, "request": request, "submissions": len(rows),
        "objects": sum(sample["objects"] for sample in samples),
        "native_us": _distribution([sample["native_us"] for sample in samples]),
        "grant_wait_us": _distribution([sample["grant_wait_us"] for sample in samples]),
        "acceptance_tail_us": _distribution([sample["acceptance_tail_us"] for sample in samples]),
        "next_invocation_gap_us": _distribution(gaps),
    }


def analyze_physical32(snapshot: dict, observations: dict) -> dict:
    """Validate captured release evidence; never operate a phone or board."""
    copied_files = snapshot.get("source_kind") == "release_file_copy"
    if not copied_files:
        _require(snapshot.get("available") is True, "AppTrace unavailable")
        _require(snapshot.get("failure") is None, "AppTrace storage failure")
        _require(snapshot.get("dropped_events") == 0, "AppTrace dropped events")
    _require(len(snapshot["jsonl"].encode("utf-8")) <= MAX_PHYSICAL_INPUT_BYTES,
             "physical trace exceeds one MiB")
    records = [json.loads(line) for line in snapshot["jsonl"].splitlines() if line]
    start_utc, end_utc = _utc(observations["start_utc"]), _utc(observations["end_utc"])
    _require(start_utc <= end_utc, "invalid UTC workload bounds")
    records = [record for record in records if
               start_utc <= _utc(record["ts"]) <= end_utc]
    if copied_files:
        # Slot numbering rotates; file-copy order is not emission order.
        records.sort(key=lambda record: (_utc(record["ts"]), record["seq"]))
    indexed = {}
    previous_seq = previous_mono = -1
    for record in records:
        _require(record.get("type") == "app_trace" and record.get("schema") == 1,
                 "invalid AppTrace record")
        seq = _integer(record, "seq", 1)
        mono = _integer(record, "mono_ms")
        _require(seq > previous_seq and mono >= previous_mono,
                 "AppTrace reordered, duplicated or spans process restarts")
        indexed[seq] = record
        previous_seq, previous_mono = seq, mono

    failures = []
    for key in ("app_candidate", "firmware_candidate", "phone", "operator", "usb_evidence"):
        if not isinstance(observations.get(key), str) or not observations[key].strip():
            failures.append(f"missing operator evidence: {key}")
    if observations.get("app_build_mode") != "release":
        failures.append("ordinary release app required")
    if observations.get("board") != "V3.3":
        failures.append("exact V3.3 target required")
    if observations.get("trace_extraction_verified") is not True:
        failures.append("release-phone AppTrace extraction remains unverified")
    elapsed = observations.get("workload_elapsed_ms")
    if type(elapsed) is not int or elapsed <= 0:
        failures.append("missing operator stopwatch workload_elapsed_ms")
    first = _integer(observations, "start_seq", 1)
    last = _integer(observations, "end_seq", first)
    _require(first in indexed and last in indexed, "workload trace bounds missing")
    workload = [record for record in records if first <= record["seq"] <= last]
    for check in PHYSICAL_CHECKS:
        evidence = observations.get("checks", {}).get(check, {})
        detail = evidence.get("evidence")
        if evidence.get("passed") is not True or not isinstance(detail, str) or not detail.strip():
            failures.append(f"missing/pending/failed operator check: {check}")
    operator_failures = observations.get("failures")
    _require(isinstance(operator_failures, list), "explicit operator failures list required")
    for failure in operator_failures:
        failures.append(f"operator failure: {failure}")

    rounds = observations.get("roundtrips", [])
    if len(rounds) != 3:
        failures.append("exactly three completed roundtrips required")
    samples = []
    identities = set()
    used_sequences = set()
    used_object_rows: set[int] = set()
    object_transfers: set[tuple[int, int]] = set()
    for number, trip in enumerate(rounds, 1):
        sample = {"roundtrip": number}
        try:
            _require(trip.get("adopted") is True and isinstance(trip.get("evidence"), str) and
                     bool(trip["evidence"].strip()), "operator must confirm completed adoption")
            sequences = [_integer(trip, key, 1) for key in
                         ("upload_seq", "timing_seq", "download_seq")]
            _require(not used_sequences.intersection(sequences), "reused trace evidence")
            _require(sequences[0] < sequences[1] < sequences[2], "invalid roundtrip order")
            _require(sequences[1] == sequences[0] + 1,
                     "upload timing must be the consecutive producer event")
            _require(first <= sequences[0] and sequences[2] <= last,
                     "roundtrip outside workload trace bounds")
            used_sequences.update(sequences)
            upload, timing, download = [indexed[seq] for seq in sequences]
            for record in (upload, timing, download):
                _require(not record.get("fields_omitted"), "AppTrace fields omitted")
                _require(_integer(record, "frames") == 32, "32 frames required each way")
                _integer(record, "session", 1)
                _integer(record, "generation", 1)
            _require(upload.get("event") == "litchcraft.frame_store_delivery" and
                     upload.get("accepted") is True, "upload did not complete")
            _require(upload.get("resident_hit") is False and
                     upload.get("resident_cache_hit") is False,
                     "resident selection substituted for upload body")
            _require(timing.get("event") == "litchcraft.frame_store_timing",
                     "missing upload timing")
            _require(timing.get("session") == upload.get("session") and
                     timing.get("generation") == upload.get("generation"),
                     "upload timing authority mismatch")
            _require(download.get("event") == "litchcraft.frame_store_snapshot_read" and
                     download.get("read_kind") == "saved_body" and
                     download.get("cache_hits") == 0,
                     "metadata/cache/partial read substituted for download body")
            identity = (_integer(upload, "canonical_bytes", 1),
                        _integer(upload, "canonical_crc32"))
            _require(identity == (_integer(download, "body_bytes", 1),
                                  _integer(download, "body_crc32")),
                     "roundtrip content identity mismatch")
            _require(identity not in identities, "three distinct body identities required")
            identities.add(identity)
            upload_bytes = _integer(upload, "body_bytes", 1)
            upload_chunks = _integer(upload, "chunks", 1)
            download_chunks = _integer(download, "chunks", 1)
            upload_ms = _integer(timing, "transfer_ms", 1)
            download_ms = _integer(download, "elapsed_ms", 1)
            object_timing = _object_timing(workload, upload, used_object_rows, object_transfers)
            sample.update({
                "object_timing": object_timing,
                "canonical_bytes": identity[0], "canonical_crc32": identity[1],
                "upload_bytes": upload_bytes, "download_bytes": identity[0],
                "upload_chunks": upload_chunks, "download_chunks": download_chunks,
                "upload_ms": upload_ms, "download_ms": download_ms,
                "upload_goodput_Bps": upload_bytes * 1000 / upload_ms,
                "download_goodput_Bps": identity[0] * 1000 / download_ms,
                "cache_hits": 0,
            })
        except (KeyError, TypeError, ValueError) as error:
            sample["failure"] = str(error)
            failures.append(f"roundtrip {number}: {error}")
        samples.append(sample)
    for record in workload:
        if record.get("event", "").endswith(("_failed", "_deferred")):
            failures.append(f"trace failure: seq {record['seq']} {record['event']}")
    uploads = {record["seq"] for record in workload if
               record.get("event") == "litchcraft.frame_store_delivery" and
               record.get("frames") == 32 and record.get("body_bytes", 0) > 0 and
               record.get("resident_hit") is False}
    downloads = {record["seq"] for record in workload if
                 record.get("event") == "litchcraft.frame_store_snapshot_read" and
                 record.get("read_kind") == "saved_body" and record.get("frames") == 32}
    if uploads != {trip.get("upload_seq") for trip in rounds} or downloads != {
        trip.get("download_seq") for trip in rounds
    }:
        failures.append("workload must contain exactly the three declared genuine bodies each way")
    if {record["seq"] for record in workload
        if record.get("event") == "litchcraft.object_submission"} != used_object_rows:
        failures.append("unmapped or cross-lifetime Object submission evidence")
    phases = {}
    for record in workload:
        if record.get("event") == "litchcraft.store_entry_phase" and record.get("stage") == "complete":
            key = f"{record.get('entry_kind')}.{record.get('phase')}"
            phases.setdefault(key, []).append(_integer(record, "elapsed_ms"))
    valid = [sample for sample in samples if "failure" not in sample]
    return {
        "profile": "physical32", "passed": not failures, "failures": failures,
        "trace_health": ("release file copy: ring drops and pending flush unknown"
                         if copied_files else "snapshot available; zero reported drops"),
        "workload_elapsed_ms": elapsed,
        "over_60s": type(elapsed) is int and elapsed > 60000,
        "samples": samples,
        "statistics": {key: _distribution([sample[key] for sample in valid]) for key in
                       ("upload_ms", "download_ms", "upload_goodput_Bps", "download_goodput_Bps")},
        "body_bytes": {direction: sum(sample[f"{direction}_bytes"] for sample in valid)
                       for direction in ("upload", "download")},
        "cache_hits": sum(record.get("cache_hits", 0) for record in records
                          if first <= record["seq"] <= last and
                          record.get("event") == "litchcraft.frame_store_snapshot_read"),
        "entry_phase_ms": {key: _distribution(values) for key, values in phases.items()},
        "p95_note": "With three samples nearest-rank p95 is max; no statistical guarantee.",
        "clock_note": ("Transfer durations use the phone monotonic clock. Object endpoints/gaps use "
                       "only the mapped activation's session clock; native_us is call duration, "
                       "acceptance is not durability. USB/native clocks are never subtracted. "
                       "LED submission is not photons."),
    }


def run_physical32(trace_paths: list[Path], observations_path: Path) -> bool:
    _require(1 <= len(trace_paths) <= 7, "one snapshot or at most seven raw segments required")
    _require(sum(path.stat().st_size for path in trace_paths) + observations_path.stat().st_size <=
             MAX_PHYSICAL_INPUT_BYTES, "physical evidence exceeds 1 MiB")
    first_text = trace_paths[0].read_text()
    try:
        snapshot = json.loads(first_text)
    except ValueError:
        snapshot = None
    if isinstance(snapshot, dict) and "jsonl" in snapshot:
        _require(len(trace_paths) == 1, "snapshot cannot be mixed with raw segments")
    else:
        lines = []
        for path in trace_paths:
            _require(path.stat().st_size <= 64 * 1024, "raw AppTrace segment exceeds 64 KiB")
            segment = [json.loads(line) for line in path.read_text().splitlines() if line]
            _require(segment and segment[0].get("type") == "app_trace_segment" and
                     segment[0].get("schema") == 1, "raw AppTrace segment header missing")
            lines.extend(json.dumps(record) for record in segment[1:])
        snapshot = {"source_kind": "release_file_copy", "jsonl": "\n".join(lines)}
    result = analyze_physical32(snapshot, json.loads(observations_path.read_text()))
    print(json.dumps(result, indent=2))
    return result["passed"]


def _display(command: Iterable[str], cwd: Path) -> None:
    print(f"[{cwd}] {' '.join(command)}", flush=True)


def _run(command: list[str], *, cwd: Path, env: dict[str, str], dry_run: bool) -> None:
    _display(command, cwd)
    if env.get("LITCH_NATIVE_TEST_FILTER"):
        print(f"native suite: {env['LITCH_NATIVE_TEST_FILTER']}", flush=True)
    if not dry_run:
        subprocess.run(command, cwd=cwd, env=env, check=True)


def _native_command(tests: Iterable[str], *, build_only: bool = False) -> list[str]:
    command = ["nice", "-n", "15", "pio", "test", "-e", "native"]
    for test in tests:
        command.extend(("-f", test))
    if build_only:
        command.append("--without-testing")
    return command


def _flutter_command(tests: Iterable[str], *, full: bool = False,
                     plain_names: Iterable[str] = (), shard_index: int | None = None,
                     total_shards: int | None = None) -> list[str]:
    plain_names = tuple(plain_names)
    selection = []
    if len(plain_names) == 1:
        selection = ["--plain-name", plain_names[0]]
    elif plain_names:
        # package:test intersects repeated filters. One escaped alternation
        # selects the union of our literal requested names instead.
        selection = ["--name", "|".join(re.escape(name) for name in plain_names)]
    return [
        "nice",
        "-n",
        "15",
        "flutter",
        "test",
        "--no-pub",
        "--concurrency=4" if full else "--concurrency=1",
        *(() if full else tests),
        *selection,
        *([] if shard_index is None else
          ["--total-shards", str(total_shards), "--shard-index", str(shard_index)]),
        "--reporter",
        "json",
    ]


class FlutterResults:
    """Count completed bodies, not loader tests or a successful process exit."""

    def __init__(self, tests: Iterable[str], app: Path, *, focused: bool = False,
                 plain_names: Iterable[str] = (), sharded: bool = False) -> None:
        self.app = app.resolve()
        self.focused = focused
        self.sharded = sharded
        self.groups: dict[int, dict] = {}
        self.body_rows: dict[int, dict] = {}
        self.plain_names = tuple(plain_names)
        self.required = {str((self.app / test).resolve()): test for test in tests}
        self.suites: dict[int, str] = {}
        self.tests: dict[int, dict] = {}
        self.completed: set[int] = set()
        self.passed = {test: [] for test in self.required.values()}
        self.errors: list[str] = []
        self.failed_test_ids: set[int | None] = set()
        self.terminal: dict | None = None
        self.started_ms: dict[int, int] = {}
        self.timed_bodies = 0
        self.suite_body_ms: dict[str, int] = {}
        self.slowest_bodies: list[dict] = []
        self.body_output: dict[int, deque[str]] = {}

    def fail(self, message: str) -> None:
        if len(self.errors) < 8:
            self.errors.append(message[:600])

    def body_label(self, identity: int | None) -> str:
        test = self.tests.get(identity, {})
        path = self.suites.get(test.get("suiteID"), "<unknown suite>")
        suite = self.required.get(path, path)
        return f"{suite[:180]} :: {str(test.get('name', '<unknown body>'))[:160]}"

    def report_body_output(self, identity: int | None) -> None:
        for message in self.body_output.pop(identity, ()):
            self.fail(f"Flutter output: {self.body_label(identity)}: {message}")

    def record_failed_test(self, identity: int | None) -> None:
        # Reuse the started-test registry; unidentified errors share one label.
        self.failed_test_ids.add(identity if identity in self.tests else None)

    def failure_summary(self) -> dict:
        failed = set()
        for identity in self.failed_test_ids:
            test = self.tests.get(identity, {})
            path = self.suites.get(test.get("suiteID"), "<unknown suite>")
            failed.add((self.required.get(path, path),
                        str(test.get("name", "<unknown body>"))))
        ordered = sorted(failed)
        suite_counts = {}
        for suite, _ in ordered:
            suite_counts[suite] = suite_counts.get(suite, 0) + 1
        retained = ordered[:MAX_FAILURE_LABELS]
        suites = list(suite_counts.items())[:MAX_FAILURE_LABELS]
        return {"total": len(ordered),
                "labels": [f"{suite[:180]} :: {name[:512]}" for suite, name in retained],
                "omitted": len(ordered) - len(retained),
                "truncated_labels": sum(len(suite) > 180 or len(name) > 512
                                        for suite, name in retained),
                "suite_counts": [{"suite": suite[:180], "failed": count}
                                 for suite, count in suites],
                "truncated_suites": sum(len(suite) > 180 for suite, _ in suites),
                "suites_omitted": len(suite_counts) - len(suites)}

    def accept(self, event: dict) -> None:
        kind = event.get("type")
        if self.terminal is not None:
            self.fail("machine events after terminal result")
            return
        if kind == "suite":
            suite = event["suite"]
            path = str((self.app / Path(suite["path"])).resolve())
            if self.sharded:
                _require(suite["id"] not in self.suites and path not in self.suites.values(),
                         "duplicate Flutter suite")
                _require(path in self.required, "suite outside shard manifest")
            self.suites[suite["id"]] = path
        elif kind == "group" and self.sharded:
            group = event["group"]
            _integer(group, "id")
            _integer(group, "testCount")
            _require(group["id"] not in self.groups and group["suiteID"] in self.suites,
                     "duplicate group or unknown group suite")
            _require("parentID" in group and (group["parentID"] is None or
                     group["parentID"] in self.groups), "unknown parent group")
            self.groups[group["id"]] = group
        elif kind == "testStart":
            test = event["test"]
            if self.sharded:
                _integer(test, "id")
                _require(test.get("suiteID") in self.suites and
                         isinstance(test.get("name"), str) and bool(test["name"]) and
                         isinstance(test.get("groupIDs"), list) and
                         all(type(group) is int for group in test["groupIDs"]),
                         "invalid shard test start")
            if test["id"] in self.tests:
                self.fail("duplicate test start")
            self.tests[test["id"]] = test
            timestamp = event.get("time")
            if type(timestamp) is int and timestamp >= 0:
                self.started_ms[test["id"]] = timestamp
        elif kind == "print":
            identity = event.get("testID")
            if identity in self.tests and identity not in self.completed:
                self.body_output.setdefault(identity, deque(maxlen=2)).append(
                    str(event.get("message", ""))[:1000])
                if len(self.body_output) > 8:
                    self.body_output.pop(next(iter(self.body_output)))
        elif kind == "error":
            self.record_failed_test(event.get("testID"))
            error = str(event.get("error"))[:120]
            stack = str(event.get("stackTrace") or "")[:100]
            self.fail(f"Flutter error: {self.body_label(event.get('testID'))}: {error}\n{stack}")
            self.report_body_output(event.get("testID"))
        elif kind == "testDone":
            identity = event["testID"]
            if event.get("result") != "success":
                self.record_failed_test(identity)
            test = self.tests.get(identity)
            if test is None or identity in self.completed:
                self.fail("missing start or duplicate test completion")
                return
            self.completed.add(identity)
            if event.get("result") != "success":
                self.fail(f"failed test: {self.body_label(identity)}")
                self.report_body_output(identity)
            self.body_output.pop(identity, None)
            if event.get("hidden") is True:
                if self.sharded and event.get("skipped") is not False:
                    self.fail("shard loader or lifecycle test skipped")
                return
            if event.get("hidden") is not False:
                self.fail("unclassified test body")
                return
            path = self.suites.get(test["suiteID"])
            relative = self.required.get(path)
            if self.sharded:
                _require(relative is not None, "body outside shard manifest")
                self.body_rows[identity] = {"name": test["name"],
                                            "skipped": event.get("skipped")}
            if self.focused and (relative is None or (self.plain_names and not any(
                    name in test["name"] for name in self.plain_names))):
                self.fail(f"body outside requested selection: {self.body_label(identity)}")
            if event.get("skipped") is not False:
                self.fail(f"required body skipped: {self.body_label(identity)}")
            elif relative is not None and event.get("result") == "success":
                self.passed[relative].append(test["name"])
                started, ended = self.started_ms.get(identity), event.get("time")
                if started is not None and type(ended) is int and ended >= started:
                    elapsed = ended - started
                    self.timed_bodies += 1
                    self.suite_body_ms[relative] = self.suite_body_ms.get(relative, 0) + elapsed
                    self.slowest_bodies.append({"file": relative, "test": test["name"],
                                                "elapsed_ms": elapsed})
                    self.slowest_bodies.sort(key=lambda body: (-body["elapsed_ms"],
                                                              body["file"], body["test"]))
                    del self.slowest_bodies[10:]
        elif kind == "done":
            self.terminal = event

    def finish(self) -> dict:
        _require(self.terminal is not None, "Flutter run has no terminal result")
        _require(self.terminal.get("success") is True, "Flutter terminal success is false")
        _require(self.completed == set(self.tests), "unfinished Flutter tests")
        _require(not self.errors, "; ".join(self.errors))
        if self.sharded:
            return {"passed": True,
                    "bodies": {test: len(names) for test, names in self.passed.items()},
                    "declared_optional_skips": 0,
                    "terminal_success": True, "started_tests": len(self.tests),
                    "completed_tests": len(self.completed), **self.shard_coverage(),
                    **self.timing_summary()}
        for test, names in self.passed.items():
            _require(bool(names), f"no executed bodies for selected suite: {test}")
            for name in REQUIRED_FLUTTER_CASES.get(test, ()):
                if self.plain_names and not any(part in name for part in self.plain_names):
                    continue
                _require(name in names, f"required named body not executed: {name}")
        for part in self.plain_names:
            _require(any(part in name for names in self.passed.values() for name in names),
                     f"requested name not executed: {part}")
        result = {"passed": True, "bodies": {test: len(names) for test, names in self.passed.items()},
                  "declared_optional_skips": 0}
        return {**result, **self.timing_summary()}

    def shard_coverage(self) -> dict:
        """Keep post-shard root counts and body order, including duplicate names."""
        _require(set(self.suites.values()) == set(self.required),
                 "shard did not discover every manifest suite")
        rows, counts, loaders = {}, {}, {}
        for path, relative in self.required.items():
            bodies = [identity for identity, test in self.tests.items()
                      if self.suites.get(test["suiteID"]) == path and identity in self.body_rows]
            loaders[relative] = sum(self.suites.get(test["suiteID"]) == path and
                                    identity not in self.body_rows and test.get("groupIDs") == []
                                    for identity, test in self.tests.items())
            _require(loaders[relative] == 1, f"missing or duplicate shard loader: {relative}")
            roots = [group for group in self.groups.values()
                     if self.suites[group["suiteID"]] == path and group["parentID"] is None]
            _require(len(roots) == (1 if bodies else 0),
                     f"missing or ambiguous shard root group: {relative}")
            count = roots[0]["testCount"] if roots else 0
            _require(count == len(bodies), f"shard group/body count mismatch: {relative}")
            for identity in bodies:
                groups = self.tests[identity].get("groupIDs", [])
                _require(groups and groups[0] == roots[0]["id"] and
                         all(group in self.groups and
                             self.groups[group]["suiteID"] == self.tests[identity]["suiteID"] and
                             self.groups[group]["parentID"] == (groups[offset - 1] if offset else None)
                             for offset, group in enumerate(groups)),
                         f"body outside reported root group: {relative}")
            rows[relative] = [self.body_rows[identity] for identity in bodies]
            counts[relative] = count
        return {"suite_bodies": rows, "suite_root_counts": counts, "suite_loaders": loaders}

    def timing_summary(self) -> dict:
        if self.timed_bodies:
            return {"body_timings": {
                "basis": "passing-body testStart/testDone timestamps; sums can overlap across workers",
                "observed_bodies": self.timed_bodies,
                "unavailable_bodies": sum(map(len, self.passed.values())) - self.timed_bodies,
                "suite_body_ms": dict(sorted(self.suite_body_ms.items())),
                "slowest_bodies": self.slowest_bodies,
            }}
        return {}


def _backend(env: dict[str, str], key: str) -> Path:
    recovery = ("Prepare the candidate's backends with successful native/bridge commands: "
                "tools/qa/README.md#interactive-sync-backend-preparation")
    value = env.get(key)
    _require(bool(value), f"required backend environment missing: {key}. {recovery}")
    path = Path(value).resolve()
    executable = key != "LITCH_HISTORY_REBASE_WIRE"
    _require(path.is_file() and (not executable or os.access(path, os.X_OK)),
             f"required backend is not a usable {'executable' if executable else 'file'}: "
             f"{key}={path}. {recovery}")
    return path


class _FlutterMachineStream:
    """Bound the stream adapter while Runner owns process lifetime and cleanup."""

    def __init__(self, results: FlutterResults, started: float) -> None:
        self.results = results
        self.started = self.last_progress = started
        self.pending = {"stdout": "", "stderr": ""}
        self.discarding = {"stdout": False, "stderr": False}
        self.diagnostic_tail = ""

    def active_labels(self, limit: int) -> list[str]:
        labels = []
        for identity in self.results.tests:
            if identity not in self.results.completed:
                labels.append(self.results.body_label(identity))
                if len(labels) == limit:
                    break
        return labels

    def _record(self, line: str) -> None:
        prior_failures = len(self.results.failed_test_ids)
        prior_errors = len(self.results.errors)
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            if line.lstrip().startswith("{"):
                self.results.fail("invalid Flutter machine JSON")
            return
        try:
            _require(isinstance(event, dict), "machine event must be an object")
            self.results.accept(event)
        except (KeyError, TypeError, ValueError):
            self.results.fail("invalid Flutter machine event")
        failures = len(self.results.failed_test_ids)
        if prior_failures < failures <= MAX_FAILURE_LABELS:
            print(json.dumps({"event": "flutter.failure", "failed_tests": failures,
                              "test": self.results.body_label(event.get("testID")),
                              "error": self.results.errors[-1] if len(self.results.errors) > prior_errors
                              else str(event.get("error") or "test failed")[:600]}),
                  flush=True)

    def feed(self, stream: str, chunk: str) -> None:
        self.diagnostic_tail = (self.diagnostic_tail + chunk)[-FLUTTER_DIAGNOSTIC_CHARS:]
        parts = chunk.split("\n")
        for index, part in enumerate(parts):
            if not self.discarding[stream]:
                self.pending[stream] += part
                if len(self.pending[stream]) >= MAX_FLUTTER_RECORD_CHARS:
                    self.results.fail("oversized Flutter machine record")
                    self.pending[stream] = ""
                    self.discarding[stream] = True
            if index + 1 < len(parts):
                if not self.discarding[stream]:
                    self._record(self.pending[stream])
                self.pending[stream] = ""
                self.discarding[stream] = False
        now = time.monotonic()
        if now - self.last_progress >= FLUTTER_PROGRESS_SECONDS:
            print(json.dumps({"event": "flutter.progress", "elapsed_s": round(now - self.started, 1),
                              "completed_tests": len(self.results.completed),
                              "failed_tests": len(self.results.failed_test_ids),
                              "active_tests": self.active_labels(4)}), flush=True)
            self.last_progress = now

    def finish_records(self) -> None:
        for stream, pending in self.pending.items():
            if pending and not self.discarding[stream]:
                self._record(pending)
            self.pending[stream] = ""

    def failure_details(self, *, include_tail: bool) -> dict:
        details = {"incomplete_tests": self.active_labels(MAX_FAILURE_LABELS)}
        if include_tail:
            details["diagnostic_tail"] = self.diagnostic_tail
        return details


def _run_flutter(tests: Iterable[str], root: Path, env: dict[str, str], dry_run: bool,
                 *, full: bool = False, focused: bool = False,
                 plain_names: Iterable[str] = (), shard_index: int | None = None,
                 total_shards: int | None = None) -> dict | None:
    tests = tuple(tests)
    app = root / "app"
    plain_names = tuple(plain_names)
    shard_options = ({} if shard_index is None else
                     {"shard_index": shard_index, "total_shards": total_shards})
    command = _flutter_command(tests, full=full, plain_names=plain_names, **shard_options)
    _display(command, app)
    if dry_run:
        return
    for test in tests:
        _require((app / test).is_file(), f"selected Flutter suite missing: {test}")
    backends = {key: str(_backend(env, key)) for key in
                sorted({key for test in tests for key in FLUTTER_BACKENDS.get(test, ())})}
    results = FlutterResults(tests, app, focused=focused, plain_names=plain_names,
                             sharded=shard_index is not None)
    deadline = (SHARD_FLUTTER_TIMEOUT_SECONDS if shard_index is not None else
                FULL_FLUTTER_TIMEOUT_SECONDS if full else COMMAND_TIMEOUT_SECONDS["flutter-test"])
    started = time.monotonic()
    stream = _FlutterMachineStream(results, started)
    print(json.dumps({"event": "flutter.start", "deadline_s": deadline}), flush=True)
    try:
        completed = Runner(announce_commands=False).run(
            command, cwd=app, timeout_seconds=deadline,
            check=False, extra_env=env, live_output=stream.feed)
    except BaseException as error:
        stream.finish_records()
        print(json.dumps({"passed": False, "runner_error": str(error)[:600],
                          "errors": results.errors, "failed_tests": results.failure_summary(),
                          **results.timing_summary(),
                          **stream.failure_details(include_tail=True)}), flush=True)
        raise
    stream.finish_records()
    code = completed.returncode
    evidence = {}
    validation_error = None
    try:
        evidence = results.finish()
    except ValueError as error:
        validation_error = error
    if code or validation_error is not None:
        print(json.dumps({"passed": False, "exit_code": code, "errors": results.errors,
                          "failed_tests": results.failure_summary(),
                          **results.timing_summary(),
                          **stream.failure_details(include_tail=not results.errors),
                          "validation_error": str(validation_error)[:600]
                          if validation_error is not None else None}), flush=True)
        if code:
            raise subprocess.CalledProcessError(code, command)
        assert validation_error is not None
        raise validation_error
    summary = {**evidence, "backends": backends, "command": command,
               "scope": ("full-shard" if shard_index is not None else
                         "focused" if focused else "full" if full else "profile"),
               "deadline_s": deadline, "elapsed_s": round(time.monotonic() - started, 3)}
    print(json.dumps(summary), flush=True)
    return summary


def run_focused(root: Path, env: dict[str, str], dry_run: bool,
                tests: Iterable[str], plain_names: Iterable[str] = ()) -> None:
    """Execute only explicitly selected app bodies, without building backends."""
    tests = tuple(dict.fromkeys(tests))
    plain_names = tuple(dict.fromkeys(plain_names))
    _require(bool(tests), "focused requires at least one --test")
    _require(all(name.strip() for name in plain_names), "empty --plain-name")
    for name in tests:
        path = Path(name)
        _require(path.parts and path.parts[0] == "test" and name.endswith("_test.dart")
                 and (root / "app" / name).resolve().is_relative_to(
                     (root / "app/test").resolve()),
                 f"select a test/*_test.dart inside this app: {name}")
    _run_flutter(tests, root, env, dry_run, focused=True, plain_names=plain_names)


def _full_tests(root: Path) -> list[str]:
    tests = {path.relative_to(root / "app").as_posix()
             for path in (root / "app" / "test").rglob("*_test.dart")}
    # Keep named contract journeys mandatory even if a file disappears.
    return sorted(tests | set(FAST_FLUTTER_TESTS) | set(BRIDGE_FLUTTER_TESTS))


def _test_hashes(root: Path) -> dict[str, str]:
    hashes = {}
    for test in _full_tests(root):
        path = root / "app" / test
        _require(path.is_file(), f"selected Flutter suite missing: {test}")
        hashes[test] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def _source_sha(root: Path) -> str:
    status = subprocess.check_output(
        ["git", "status", "--porcelain", "--untracked-files=all"], cwd=root, text=True)
    _require(not status.strip(), "shard qualification requires a clean source checkout")
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()


def _run_identity(root: Path, env: dict[str, str]) -> dict:
    for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"):
        _require(bool(re.fullmatch(r"[1-9][0-9]*", env.get(key, ""))),
                 f"shard qualification requires {key}")
    return {"source_sha": _source_sha(root), "run_id": env["GITHUB_RUN_ID"],
            "run_attempt": env["GITHUB_RUN_ATTEMPT"]}


def _toolchain_identity(root: Path, env: dict[str, str]) -> dict:
    output = subprocess.check_output(["flutter", "--version", "--machine"],
                                     cwd=root / "app", env=env, text=True, timeout=30)
    version = json.loads(output)
    _require(isinstance(version, dict) and all(isinstance(version.get(key), str) and
             bool(version[key]) for key in TOOLCHAIN_FIELDS), "incomplete Flutter SDK identity")
    return {key: version[key] for key in TOOLCHAIN_FIELDS}


def _backend_hashes(env: dict[str, str]) -> dict[str, str]:
    return {key: hashlib.sha256(_backend(env, key).read_bytes()).hexdigest()
            for key in SHARD_BACKEND_KEYS}


def _manifest_digest(manifest: dict) -> str:
    content = {key: value for key, value in manifest.items() if key != "manifest_sha256"}
    return hashlib.sha256(json.dumps(content, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()


def _new_output(path: Path) -> None:
    _require(not path.exists() and not path.is_symlink(), f"output already exists: {path}")


def _write_shard_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x") as output:
        json.dump(value, output, sort_keys=True, separators=(",", ":"))
        output.write("\n")


def _read_shard_json(path: Path) -> dict:
    _require(path.is_file() and not path.is_symlink() and
             path.stat().st_size <= MAX_SHARD_RECEIPT_BYTES, f"invalid shard evidence: {path}")
    record = json.loads(path.read_text())
    _require(isinstance(record, dict), "shard evidence must be an object")
    return record


def _validate_manifest(manifest: dict, root: Path, env: dict[str, str],
                       *, live_inputs: bool) -> None:
    _require(type(manifest.get("schema")) is int and manifest["schema"] == SHARD_SCHEMA and
             manifest.get("kind") == "flutter-shard-manifest", "incompatible shard manifest schema")
    _require(manifest.get("manifest_sha256") == _manifest_digest(manifest),
             "shard manifest identity mismatch")
    _require(manifest.get("total_shards") == FLUTTER_TOTAL_SHARDS,
             f"qualification requires {FLUTTER_TOTAL_SHARDS} native shards")
    _require(all(manifest.get(key) == value for key, value in _run_identity(root, env).items()),
             "shard source/run/attempt mismatch")
    _require(manifest.get("test_files") == _test_hashes(root), "shard test content mismatch")
    _require(isinstance(manifest.get("toolchain"), dict) and
             set(manifest["toolchain"]) == set(TOOLCHAIN_FIELDS) and
             all(isinstance(value, str) and value for value in manifest["toolchain"].values()),
             "incomplete manifest SDK identity")
    backends = manifest.get("backends")
    _require(isinstance(backends, dict) and set(backends) == set(SHARD_BACKEND_KEYS) and
             all(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value)
                 for value in backends.values()), "incomplete manifest backend identity")
    stage = manifest.get("stage_started_epoch")
    _require(type(stage) in (int, float) and math.isfinite(stage) and
             946684800 <= stage <= time.time(), "invalid app stage start")
    if live_inputs:
        _require(manifest["toolchain"] == _toolchain_identity(root, env), "shard SDK mismatch")
        _require(backends == _backend_hashes(env), "shard backend content mismatch")


def prepare_shards(root: Path, env: dict[str, str], manifest_path: Path,
                   total_shards: int = FLUTTER_TOTAL_SHARDS) -> dict:
    """Bind the unfiltered corpus, SDK and backend bytes before native sharding."""
    _new_output(manifest_path)
    _require(type(total_shards) is int and total_shards == FLUTTER_TOTAL_SHARDS,
             f"qualification requires {FLUTTER_TOTAL_SHARDS} native shards")
    _require(bool(env.get("LITCH_APP_STAGE_STARTED_EPOCH")),
             "prepare-shards requires LITCH_APP_STAGE_STARTED_EPOCH from app stage start")
    stage = float(env["LITCH_APP_STAGE_STARTED_EPOCH"])
    _require(math.isfinite(stage) and 946684800 <= stage <= time.time(),
             "invalid app stage start")
    manifest = {"schema": SHARD_SCHEMA, "kind": "flutter-shard-manifest",
                **_run_identity(root, env), "total_shards": total_shards,
                "stage_started_epoch": stage, "test_files": _test_hashes(root),
                "toolchain": _toolchain_identity(root, env), "backends": _backend_hashes(env)}
    manifest["manifest_sha256"] = _manifest_digest(manifest)
    _validate_manifest(manifest, root, env, live_inputs=False)
    _write_shard_json(manifest_path, manifest)
    print(json.dumps(manifest), flush=True)
    return manifest


def run_full(root: Path, env: dict[str, str], dry_run: bool, *,
             shard_index: int | None = None, total_shards: int | None = None,
             shard_manifest: Path | None = None, shard_receipt: Path | None = None) -> dict | None:
    """Validate the unfiltered gate, or one source-bound native partition of it."""
    if all(value is None for value in
           (shard_index, total_shards, shard_manifest, shard_receipt)):
        return _run_flutter(_full_tests(root), root, env, dry_run, full=True)
    _require(not dry_run and type(shard_index) is int and
             type(total_shards) is int and total_shards == FLUTTER_TOTAL_SHARDS and
             0 <= shard_index < total_shards and shard_manifest is not None and
             shard_receipt is not None, "full shard requires manifest, new receipt and valid index/total")
    _new_output(shard_receipt)
    manifest = _read_shard_json(shard_manifest)
    _validate_manifest(manifest, root, env, live_inputs=True)
    evidence = _run_flutter(_full_tests(root), root, env, False, full=True,
                            shard_index=shard_index, total_shards=total_shards)
    _validate_manifest(manifest, root, env, live_inputs=True)
    assert evidence is not None
    receipt = {**evidence, "schema": SHARD_SCHEMA, "kind": "flutter-shard-receipt",
               **{key: manifest[key] for key in
                  ("source_sha", "run_id", "run_attempt", "total_shards", "manifest_sha256")},
               "shard_index": shard_index,
               "runtime": {"compiler_cache_hit": env.get("LITCH_FLUTTER_COMPILER_CACHE_HIT", ""),
                           "compiler_cache_matched_key":
                           env.get("LITCH_FLUTTER_COMPILER_CACHE_MATCHED_KEY", "")[:512]}}
    _write_shard_json(shard_receipt, receipt)
    return receipt


def _shard_bounds(count: int, index: int, total: int) -> tuple[int, int]:
    # Match Dart's positive double.round(), not Python's ties-to-even round().
    return ((2 * count * index + total) // (2 * total),
            (2 * count * (index + 1) + total) // (2 * total))


def merge_shards(root: Path, env: dict[str, str], manifest_path: Path,
                 receipts_dir: Path, receipt_path: Path) -> dict:
    """Accept full coverage only from every checked native partition of this run."""
    _new_output(receipt_path)
    manifest = _read_shard_json(manifest_path)
    _validate_manifest(manifest, root, env, live_inputs=False)
    paths = sorted(receipts_dir.rglob("*.json"))
    total = manifest["total_shards"]
    _require(len(paths) == total, "missing or extra shard receipts")
    receipts = {}
    files = set(manifest["test_files"])
    for path in paths:
        record = _read_shard_json(path)
        index = _integer(record, "shard_index")
        _require(index < total and index not in receipts, "duplicate or invalid shard index")
        _require(type(record.get("schema")) is int and record["schema"] == SHARD_SCHEMA and
                 record.get("kind") == "flutter-shard-receipt" and
                 record.get("scope") == "full-shard" and record.get("passed") is True,
                 "incompatible or unsuccessful shard receipt")
        _require(all(record.get(key) == manifest[key] for key in
                     ("source_sha", "run_id", "run_attempt", "total_shards", "manifest_sha256")),
                 "shard receipt source/run/manifest mismatch")
        _require(record.get("command") == _flutter_command((), full=True, shard_index=index,
                                                           total_shards=total),
                 "receipt did not run the declared native shard command")
        _require(record.get("terminal_success") is True and
                 _integer(record, "started_tests", 1) == _integer(record, "completed_tests", 1),
                 "shard terminal or completion evidence missing")
        _require(record.get("deadline_s") == SHARD_FLUTTER_TIMEOUT_SECONDS,
                 "shard command deadline mismatch")
        _require(isinstance(record.get("suite_bodies"), dict) and
                 set(record["suite_bodies"]) == files and
                 isinstance(record.get("suite_root_counts"), dict) and
                 set(record["suite_root_counts"]) == files and
                 isinstance(record.get("bodies"), dict) and set(record["bodies"]) == files and
                 isinstance(record.get("suite_loaders"), dict) and
                 set(record["suite_loaders"]) == files and
                 all(type(count) is int and count == 1 for count in record["suite_loaders"].values()),
                 "shard suite discovery or loader evidence is incomplete")
        for test in files:
            rows = record["suite_bodies"][test]
            _require(isinstance(rows, list) and
                     _integer(record["suite_root_counts"], test) == len(rows),
                     f"shard group/body count mismatch: {test}")
            _require(all(isinstance(row, dict) and isinstance(row.get("name"), str) and
                         bool(row["name"]) and type(row.get("skipped")) is bool for row in rows),
                     "invalid shard body identity")
            _require(all(not row["skipped"] for row in rows), "required body skipped")
            _require(_integer(record["bodies"], test) == len(rows),
                     "shard passed-body count mismatch")
        _require(_integer(record, "declared_optional_skips") == 0, "unexpected skips in shard receipt")
        _require(record["started_tests"] >= len(files) +
                 sum(len(rows) for rows in record["suite_bodies"].values()),
                 "shard loader/body completion evidence missing")
        receipts[index] = record
    _require(set(receipts) == set(range(total)), "incomplete shard indices")
    combined = {}
    for test in sorted(files):
        count = sum(record["suite_root_counts"][test] for record in receipts.values())
        _require(count > 0, f"no registered bodies for selected suite: {test}")
        rows = []
        for index in range(total):
            start, end = _shard_bounds(count, index, total)
            partition = receipts[index]["suite_bodies"][test]
            _require(len(partition) == end - start,
                     f"native shard interval coverage mismatch: {test} index {index}")
            rows.extend(partition)
        names = [row["name"] for row in rows]
        _require(names, f"no executed bodies for selected suite: {test}")
        for name in REQUIRED_FLUTTER_CASES.get(test, ()):
            _require(name in names, f"required named body not executed: {name}")
        combined[test] = len(names)
    _validate_manifest(manifest, root, env, live_inputs=False)
    elapsed = time.time() - manifest["stage_started_epoch"]
    summary = {"schema": SHARD_SCHEMA, "kind": "flutter-shard-summary", "scope": "full",
               "passed": True, **{key: manifest[key] for key in
                  ("source_sha", "run_id", "run_attempt", "total_shards", "manifest_sha256",
                   "toolchain", "backends", "stage_started_epoch")},
               "bodies": combined, "executed_bodies": sum(combined.values()),
               "total_bodies": sum(combined.values()),
               "declared_optional_skips": 0, "stage_elapsed_s": elapsed,
               "stage_target_s": 360, "stage_target_met": elapsed <= 360,
               "shard_runtime": {str(index): {"elapsed_s": record.get("elapsed_s"),
                                              **record.get("runtime", {})}
                                 for index, record in sorted(receipts.items())}}
    _write_shard_json(receipt_path, summary)
    print(json.dumps(summary), flush=True)
    return summary


def run_fast(root: Path, env: dict[str, str], dry_run: bool) -> None:
    if env.get("LITCHCRAFT_NATIVE_RUNNER"):
        program = Path(env["LITCHCRAFT_NATIVE_RUNNER"])
        if not dry_run:
            program = _backend(env, "LITCHCRAFT_NATIVE_RUNNER")
        for test in FAST_NATIVE_TESTS:
            _run([str(program)], cwd=root, env=dict(env, LITCH_NATIVE_TEST_FILTER=test),
                 dry_run=dry_run)
    else:
        _run(_native_command(FAST_NATIVE_TESTS), cwd=root, env=env, dry_run=dry_run)
    _run_flutter(FAST_FLUTTER_TESTS, root, env, dry_run)


def run_bridge(root: Path, env: dict[str, str], dry_run: bool, skip_build: bool) -> None:
    build = root / ".pio" / "interactive-sync" / "build"
    bridge_env = dict(env, PLATFORMIO_BUILD_DIR=str(build), PLATFORMIO_RUN_JOBS="1")
    supplied = env.get("LITCH_VIRTUAL_BRIDGE")
    if not dry_run:
        for key in sorted({key for test in BRIDGE_FLUTTER_TESTS
                           for key in FLUTTER_BACKENDS.get(test, ())} - {"LITCH_VIRTUAL_BRIDGE"}):
            _backend(bridge_env, key)
    if not supplied and not skip_build:
        _run(_native_command(("test_virtual_litch_app_bridge_manual",), build_only=True),
             cwd=root, env=bridge_env, dry_run=dry_run)
    program = Path(supplied) if supplied else build / "native" / (
        "program.exe" if os.name == "nt" else "program")
    bridge_env["LITCH_VIRTUAL_BRIDGE"] = str(program)
    _run_flutter(BRIDGE_FLUTTER_TESTS, root, bridge_env, dry_run)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--profile",
        choices=(*SOFTWARE_PROFILES, "prepare-shards", "merge-shards", "physical32"),
        default="focused",
        help="all runs focused software profiles; full runs unfiltered Flutter once; "
             "physical32 validates opt-in captured release evidence",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--test", action="append", default=[],
                        help="focused only: app-relative test file; repeat for several files")
    parser.add_argument("--plain-name", action="append", default=[],
                        help="focused only: literal name substring; every supplied name must execute")
    parser.add_argument("--shard-index", type=int)
    parser.add_argument("--total-shards", type=int)
    parser.add_argument("--shard-manifest", type=Path)
    parser.add_argument("--shard-receipt", type=Path)
    parser.add_argument("--shard-receipts", type=Path)
    parser.add_argument("--skip-bridge-build", action="store_true")
    parser.add_argument("--app-trace", type=Path, nargs="+")
    parser.add_argument("--observations", type=Path)
    args = parser.parse_args()
    if args.profile != "focused" and (args.test or args.plain_name):
        parser.error("--test/--plain-name require --profile focused; full stays unfiltered")
    if args.profile == "focused" and not args.test:
        parser.error("focused requires at least one --test")

    shard_arguments = any(value is not None for value in
                          (args.shard_index, args.total_shards, args.shard_manifest,
                           args.shard_receipt, args.shard_receipts))
    if args.profile == "prepare-shards":
        if (args.shard_manifest is None or args.shard_index is not None or
                args.shard_receipt is not None or args.shard_receipts is not None or args.dry_run):
            parser.error("prepare-shards requires only a new --shard-manifest and --total-shards")
    elif args.profile == "merge-shards":
        if (args.shard_manifest is None or args.shard_receipts is None or
                args.shard_receipt is None or args.shard_index is not None or
                args.total_shards is not None or args.dry_run):
            parser.error("merge-shards requires --shard-manifest, --shard-receipts and new --shard-receipt")
    elif shard_arguments:
        if (args.profile != "full" or args.shard_index is None or args.total_shards is None or
                args.shard_manifest is None or args.shard_receipt is None or
                args.shard_receipts is not None or args.dry_run):
            parser.error("full shard requires --shard-index, --total-shards, --shard-manifest and --shard-receipt")

    if args.profile == "physical32":
        if args.app_trace is None or args.observations is None:
            parser.error("physical32 requires --app-trace and --observations")
        try:
            passed = run_physical32(args.app_trace, args.observations)
        except (OSError, KeyError, TypeError, ValueError) as error:
            print(json.dumps({"profile": "physical32", "passed": False, "failures": [str(error)]}))
            raise SystemExit(1) from error
        raise SystemExit(0 if passed else 1)

    root = Path(__file__).resolve().parents[2]
    env = dict(os.environ, PLATFORMIO_RUN_JOBS="1")
    if args.profile == "prepare-shards":
        prepare_shards(root, env, args.shard_manifest,
                       args.total_shards if args.total_shards is not None else FLUTTER_TOTAL_SHARDS)
        return
    if args.profile == "merge-shards":
        merge_shards(root, env, args.shard_manifest, args.shard_receipts, args.shard_receipt)
        return
    if args.profile == "focused":
        run_focused(root, env, args.dry_run, args.test, args.plain_name)
        return
    if not args.dry_run and not (root / "app" / ".dart_tool" / "package_config.json").is_file():
        _run(
            ["nice", "-n", "15", "flutter", "pub", "get"],
            cwd=root / "app",
            env=env,
            dry_run=False,
        )
    if args.profile in ("fast", "all"):
        run_fast(root, env, args.dry_run)
    if args.profile in ("bridge", "all"):
        run_bridge(root, env, args.dry_run, args.skip_bridge_build)
    if args.profile == "full":
        run_full(root, env, args.dry_run, shard_index=args.shard_index,
                 total_shards=args.total_shards, shard_manifest=args.shard_manifest,
                 shard_receipt=args.shard_receipt)


if __name__ == "__main__":
    main()
