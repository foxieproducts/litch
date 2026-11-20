#!/usr/bin/env python3
"""Scoped CRAP hotspot calculation from LCOV and normalized complexity data."""

from __future__ import annotations

import csv
import io
import itertools
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

from common import (
    CodeHealthError,
    normalized_repo_path,
    path_in_scope,
    read_json_bounded,
    read_text_bounded,
    stable_fingerprint,
)


SCHEMA_VERSION = 1
DEFAULT_MAX_FUNCTIONS = 1_000
DEFAULT_HOTSPOT_LIMIT = 25
MAX_INPUT_FILES = 8
MAX_COMPLEXITY_INPUT_BYTES = 32_000_000
MAX_COVERAGE_INPUT_BYTES = 128_000_000


@dataclass(frozen=True)
class FunctionComplexity:
    path: str
    name: str
    long_name: str
    start_line: int
    end_line: int
    cyclomatic_complexity: float

    @property
    def fingerprint(self) -> str:
        # Long names normally include overload signatures and survive line motion.
        return stable_fingerprint(self.path, self.long_name or self.name)


def calculate_crap(cyclomatic_complexity: float, coverage_fraction: float) -> float:
    """Calculate CRAP = CC^2 * (1 - coverage)^3 + CC."""

    if not math.isfinite(cyclomatic_complexity) or cyclomatic_complexity < 1:
        raise CodeHealthError("cyclomatic complexity must be finite and at least 1")
    if not math.isfinite(coverage_fraction) or not 0.0 <= coverage_fraction <= 1.0:
        raise CodeHealthError("coverage fraction must be between 0 and 1")
    return cyclomatic_complexity**2 * (1.0 - coverage_fraction) ** 3 + cyclomatic_complexity


def _positive_int(value: Any, field: str) -> int:
    if isinstance(value, bool):
        raise CodeHealthError(f"{field} must be an integer")
    if isinstance(value, int):
        parsed = value
    elif isinstance(value, str) and re.fullmatch(r"[0-9]+", value.strip()):
        parsed = int(value)
    else:
        raise CodeHealthError(f"{field} must be an integer")
    if parsed < 1:
        raise CodeHealthError(f"{field} must be at least 1")
    return parsed


def _complexity(value: Any) -> float:
    if isinstance(value, bool):
        raise CodeHealthError("cyclomatic_complexity must be numeric")
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise CodeHealthError("cyclomatic_complexity must be numeric") from exc
    if not math.isfinite(parsed) or parsed < 1:
        raise CodeHealthError("cyclomatic_complexity must be finite and at least 1")
    return parsed


def _function_from_mapping(item: Mapping[str, Any], repo_root: Path) -> FunctionComplexity:
    raw_path = item.get("path") or item.get("file")
    name = item.get("name") or item.get("function")
    long_name = item.get("long_name") or name
    if not isinstance(raw_path, str) or not raw_path:
        raise CodeHealthError("complexity function requires path")
    if not isinstance(name, str) or not name:
        raise CodeHealthError("complexity function requires name")
    if not isinstance(long_name, str) or not long_name:
        raise CodeHealthError("complexity function requires long_name or name")
    start_line = _positive_int(item.get("start_line", item.get("start")), "start_line")
    end_line = _positive_int(item.get("end_line", item.get("end", start_line)), "end_line")
    if end_line < start_line:
        raise CodeHealthError("function end_line precedes start_line")
    path = normalized_repo_path(raw_path, repo_root)
    if path.startswith("<external>/"):
        raise CodeHealthError(f"complexity path is outside repository: {raw_path}")
    return FunctionComplexity(
        path=path,
        name=name.strip(),
        long_name=long_name.strip(),
        start_line=start_line,
        end_line=end_line,
        cyclomatic_complexity=_complexity(
            item.get("cyclomatic_complexity", item.get("ccn", item.get("CCN")))
        ),
    )


def parse_complexity_json(payload: Any, repo_root: Path) -> list[FunctionComplexity]:
    if not isinstance(payload, dict) or payload.get("schema_version") != SCHEMA_VERSION:
        raise CodeHealthError("complexity JSON must use schema_version 1")
    functions = payload.get("functions")
    if not isinstance(functions, list):
        raise CodeHealthError("complexity JSON must contain a functions list")
    parsed: list[FunctionComplexity] = []
    for index, item in enumerate(functions):
        if not isinstance(item, dict):
            raise CodeHealthError(f"complexity function {index} must be an object")
        parsed.append(_function_from_mapping(item, repo_root))
    return parsed


def _validate_input_set(
    paths: Sequence[Path],
    *,
    label: str,
    max_total_bytes: int,
) -> None:
    if not paths:
        raise CodeHealthError(f"at least one {label} input is required")
    if len(paths) > MAX_INPUT_FILES:
        raise CodeHealthError(
            f"{label} input count {len(paths)} exceeds the {MAX_INPUT_FILES}-file limit"
        )
    total_bytes = 0
    for path in paths:
        if not path.is_file():
            raise CodeHealthError(f"input does not exist: {path}")
        total_bytes += path.stat().st_size
        if total_bytes > max_total_bytes:
            raise CodeHealthError(
                f"aggregate {label} input exceeds {max_total_bytes} byte limit"
            )


def parse_lizard_csv(text: str, repo_root: Path) -> list[FunctionComplexity]:
    """Parse lizard CSV with either an explicit header or Lizard's native layout."""

    native_fields = (
        "nloc",
        "ccn",
        "token",
        "param",
        "length",
        "location",
        "file",
        "function",
        "long_name",
        "start",
        "end",
    )
    reader = csv.reader(io.StringIO(text))
    first_row = next(reader, None)
    if first_row is None:
        raise CodeHealthError("complexity CSV is empty")

    normalized_first = tuple(field.strip().lower() for field in first_row)
    if "ccn" in normalized_first or "cyclomatic_complexity" in normalized_first:
        fields = normalized_first
        rows = reader
        first_data_row = 2
    else:
        fields = native_fields
        rows = itertools.chain((first_row,), reader)
        first_data_row = 1

    functions: list[FunctionComplexity] = []
    for row_index, row in enumerate(rows, start=first_data_row):
        if len(row) != len(fields):
            raise CodeHealthError(
                f"invalid complexity CSV row {row_index}: "
                f"expected {len(fields)} fields, found {len(row)}"
            )
        lowered = dict(zip(fields, row))
        try:
            functions.append(
                _function_from_mapping(
                    {
                        "path": lowered.get("file") or lowered.get("path"),
                        "name": lowered.get("function") or lowered.get("name"),
                        "long_name": lowered.get("long_name") or lowered.get("function"),
                        "start_line": lowered.get("start") or lowered.get("start_line"),
                        "end_line": lowered.get("end") or lowered.get("end_line"),
                        "cyclomatic_complexity": lowered.get("ccn")
                        or lowered.get("cyclomatic_complexity"),
                    },
                    repo_root,
                )
            )
        except CodeHealthError as exc:
            raise CodeHealthError(f"invalid complexity CSV row {row_index}: {exc}") from exc
    return functions


def load_complexity(paths: Sequence[Path], repo_root: Path) -> list[FunctionComplexity]:
    _validate_input_set(
        paths,
        label="complexity",
        max_total_bytes=MAX_COMPLEXITY_INPUT_BYTES,
    )
    functions: list[FunctionComplexity] = []
    for path in paths:
        if path.suffix.lower() == ".json":
            functions.extend(parse_complexity_json(read_json_bounded(path), repo_root))
        elif path.suffix.lower() == ".csv":
            functions.extend(parse_lizard_csv(read_text_bounded(path), repo_root))
        else:
            raise CodeHealthError(f"unsupported complexity format (use .json or .csv): {path}")
    identities: set[tuple[str, str]] = set()
    for function in functions:
        identity = (function.path, function.long_name)
        if identity in identities:
            raise CodeHealthError(
                f"duplicate complexity function: {function.path}:{function.start_line} {function.long_name}"
            )
        identities.add(identity)
    return functions


def parse_lcov_line_hits(text: str, repo_root: Path) -> dict[str, dict[int, int]]:
    """Read the executable-line subset of LCOV needed for function CRAP."""

    files: dict[str, dict[int, int]] = {}
    current_path: str | None = None
    in_record = False
    saw_source = False
    saw_end = False
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("SF:"):
            if in_record:
                raise CodeHealthError("LCOV source record started before prior end_of_record")
            if not line[3:]:
                raise CodeHealthError("LCOV SF record has an empty source path")
            in_record = True
            saw_source = True
            path = normalized_repo_path(line[3:], repo_root)
            current_path = None if path.startswith("<external>/") else path
            if current_path is not None:
                files.setdefault(current_path, {})
        elif line.startswith("DA:"):
            if not in_record:
                raise CodeHealthError("LCOV DA record appears outside a source record")
            if current_path is None:
                continue
            fields = line[3:].split(",")
            if len(fields) < 2:
                raise CodeHealthError(f"invalid LCOV DA record: {line}")
            try:
                line_number = int(fields[0])
                hit_count = int(fields[1])
            except ValueError as exc:
                raise CodeHealthError(f"invalid LCOV DA record: {line}") from exc
            if line_number < 1 or hit_count < 0:
                raise CodeHealthError(f"invalid LCOV DA values: {line}")
            files[current_path][line_number] = max(
                hit_count, files[current_path].get(line_number, 0)
            )
        elif line == "end_of_record":
            if not in_record:
                raise CodeHealthError("LCOV end_of_record appears without an SF record")
            saw_end = True
            in_record = False
            current_path = None
    if in_record:
        raise CodeHealthError("LCOV source record is missing end_of_record")
    if not saw_source or not saw_end:
        raise CodeHealthError("coverage input contains no complete LCOV source record")
    return files


def load_lcov_line_hits(paths: Sequence[Path], repo_root: Path) -> dict[str, dict[int, int]]:
    _validate_input_set(
        paths,
        label="coverage",
        max_total_bytes=MAX_COVERAGE_INPUT_BYTES,
    )
    merged: dict[str, dict[int, int]] = {}
    source_for_path: dict[str, Path] = {}
    for path in paths:
        parsed = parse_lcov_line_hits(read_text_bounded(path, max_bytes=64_000_000), repo_root)
        for source_path, lines in parsed.items():
            if source_path in merged:
                raise CodeHealthError(
                    f"coverage for {source_path} appears in both {source_for_path[source_path]} and {path}"
                )
            merged[source_path] = lines
            source_for_path[source_path] = path
    return merged


def _load_baseline(path: Path | None) -> dict[str, float]:
    if path is None:
        return {}
    payload = read_json_bounded(path)
    if (
        not isinstance(payload, dict)
        or payload.get("schema_version") != SCHEMA_VERSION
        or not isinstance(payload.get("functions"), list)
    ):
        raise CodeHealthError("CRAP baseline must use schema_version 1 and contain a functions list")
    baseline: dict[str, float] = {}
    for index, item in enumerate(payload["functions"]):
        if not isinstance(item, dict):
            raise CodeHealthError(f"CRAP baseline function {index} must be an object")
        fingerprint = item.get("fingerprint")
        crap_value = item.get("crap")
        if not isinstance(fingerprint, str) or not fingerprint:
            raise CodeHealthError(f"CRAP baseline function {index} lacks a fingerprint")
        if crap_value is None:
            continue
        if isinstance(crap_value, bool) or not isinstance(crap_value, (int, float)):
            raise CodeHealthError(f"CRAP baseline function {index} has a non-numeric CRAP value")
        parsed = float(crap_value)
        if not math.isfinite(parsed):
            raise CodeHealthError(f"CRAP baseline function {index} has a non-finite CRAP value")
        if fingerprint in baseline:
            raise CodeHealthError(f"duplicate CRAP baseline fingerprint: {fingerprint}")
        baseline[fingerprint] = parsed
    return baseline


def build_crap_report(
    *,
    functions: Sequence[FunctionComplexity],
    line_hits: Mapping[str, Mapping[int, int]],
    scope_selectors: Sequence[str],
    scope_files: Sequence[str] | None = None,
    baseline_path: Path | None = None,
    max_functions: int = DEFAULT_MAX_FUNCTIONS,
    hotspot_limit: int = DEFAULT_HOTSPOT_LIMIT,
) -> dict[str, Any]:
    if not scope_selectors:
        raise CodeHealthError("CRAP requires --changed-from or at least one explicit --path")
    if max_functions < 1 or hotspot_limit < 1:
        raise CodeHealthError("function and hotspot limits must be positive")
    selected = [function for function in functions if path_in_scope(function.path, scope_selectors)]
    if len(selected) > max_functions:
        raise CodeHealthError(
            f"CRAP scope contains {len(selected)} functions; raise --max-functions above {max_functions} explicitly"
        )

    baseline = _load_baseline(baseline_path)
    rows: list[dict[str, Any]] = []
    for function in sorted(selected, key=lambda item: (item.path, item.start_line, item.long_name)):
        file_lines = line_hits.get(function.path)
        covered_lines = 0
        executable_lines = 0
        if file_lines is not None:
            for line_number, hits in file_lines.items():
                if function.start_line <= line_number <= function.end_line:
                    executable_lines += 1
                    covered_lines += hits > 0

        if executable_lines == 0:
            coverage_fraction = None
            crap_value = None
            status = "coverage_unavailable"
        else:
            coverage_fraction = covered_lines / executable_lines
            crap_value = calculate_crap(function.cyclomatic_complexity, coverage_fraction)
            status = "measured"
        baseline_crap = baseline.get(function.fingerprint)
        crap_delta = (
            crap_value - baseline_crap
            if crap_value is not None and baseline_crap is not None
            else None
        )
        rows.append(
            {
                "baseline_crap": round(baseline_crap, 6) if baseline_crap is not None else None,
                "coverage": {
                    "covered_lines": covered_lines if executable_lines else None,
                    "executable_lines": executable_lines if executable_lines else None,
                    "fraction": round(coverage_fraction, 6) if coverage_fraction is not None else None,
                    "status": status,
                },
                "crap": round(crap_value, 6) if crap_value is not None else None,
                "crap_delta": round(crap_delta, 6) if crap_delta is not None else None,
                "cyclomatic_complexity": function.cyclomatic_complexity,
                "end_line": function.end_line,
                "fingerprint": function.fingerprint,
                "long_name": function.long_name,
                "name": function.name,
                "path": function.path,
                "start_line": function.start_line,
                "status": status,
            }
        )

    hotspots = sorted(
        (row for row in rows if row["crap"] is not None),
        key=lambda row: (-row["crap"], row["path"], row["start_line"], row["long_name"]),
    )[:hotspot_limit]
    selected_paths = sorted({function.path for function in selected})
    explicit_scope_files = sorted(set(scope_files or selected_paths))
    files_without_complexity = sorted(set(explicit_scope_files) - set(selected_paths))
    return {
        "formula": "CC^2 * (1 - line_coverage_fraction)^3 + CC",
        "functions": rows,
        "hotspots": hotspots,
        "policy_thresholds": None,
        "schema_version": SCHEMA_VERSION,
        "scope": {
            "files": explicit_scope_files,
            "files_without_complexity": files_without_complexity,
            "selectors": sorted(scope_selectors),
        },
        "summary": {
            "baseline_matches": sum(row["baseline_crap"] is not None for row in rows),
            "coverage_unavailable": sum(row["status"] == "coverage_unavailable" for row in rows),
            "function_count": len(rows),
            "hotspot_limit": hotspot_limit,
            "measured": sum(row["status"] == "measured" for row in rows),
        },
    }


def render_human_summary(report: Mapping[str, Any]) -> str:
    summary = report["summary"]
    lines = [
        "Scoped CRAP hotspots",
        (
            f"functions: {summary['function_count']} scoped, {summary['measured']} measured, "
            f"{summary['coverage_unavailable']} without executable-line coverage"
        ),
        "thresholds: none (trend/review report only)",
    ]
    for row in report["hotspots"]:
        delta = ""
        if row["crap_delta"] is not None:
            delta = f" delta={row['crap_delta']:+.3f}"
        lines.append(
            f"{row['crap']:.3f}{delta} CC={row['cyclomatic_complexity']:g} "
            f"coverage={row['coverage']['fraction']:.1%} "
            f"{row['path']}:{row['start_line']} {row['long_name']}"
        )
    return "\n".join(lines)
