#!/usr/bin/env python3
"""Deterministic LCOV parsing and repository-relative coverage summaries.

The parser deliberately keeps "not recorded" distinct from a recorded zero.
For example, a file with no ``BRDA`` records has unavailable branch coverage;
it does not have zero-percent branch coverage.
"""

from __future__ import annotations

import dataclasses
import os
from pathlib import Path, PurePosixPath
import re
from typing import Iterable, Mapping, Sequence


class CoverageParseError(ValueError):
    """Raised when an LCOV record cannot be interpreted safely."""


@dataclasses.dataclass(frozen=True, order=True)
class LineCoverage:
    line: int
    count: int
    checksum: str | None = None

    @property
    def covered(self) -> bool:
        return self.count > 0

    def to_dict(self) -> dict[str, object]:
        result: dict[str, object] = {
            "line": self.line,
            "count": self.count,
            "covered": self.covered,
        }
        if self.checksum is not None:
            result["checksum"] = self.checksum
        return result


@dataclasses.dataclass(frozen=True)
class FunctionCoverage:
    name: str
    line: int | None
    end_line: int | None
    count: int | None

    @property
    def covered(self) -> bool:
        return self.count is not None and self.count > 0

    def sort_key(self) -> tuple[int, int, str]:
        return (
            self.line if self.line is not None else -1,
            self.end_line if self.end_line is not None else -1,
            self.name,
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "line": self.line,
            "end_line": self.end_line,
            "count": self.count,
            "covered": self.covered,
        }


@dataclasses.dataclass(frozen=True)
class BranchCoverage:
    line: int
    block: str
    branch: str
    taken: int | None

    @property
    def covered(self) -> bool:
        return self.taken is not None and self.taken > 0

    def sort_key(self) -> tuple[int, tuple[int, int | str], tuple[int, int | str]]:
        return self.line, _identifier_sort_key(self.block), _identifier_sort_key(self.branch)

    def to_dict(self) -> dict[str, object]:
        return {
            "line": self.line,
            "block": self.block,
            "branch": self.branch,
            "taken": self.taken,
            "covered": self.covered,
        }


@dataclasses.dataclass(frozen=True)
class MetricSummary:
    available: bool
    covered: int | None
    total: int | None
    percent: float | None

    @classmethod
    def unavailable(cls) -> "MetricSummary":
        return cls(available=False, covered=None, total=None, percent=None)

    @classmethod
    def from_coverage(cls, covered: int, total: int) -> "MetricSummary":
        if total <= 0:
            return cls.unavailable()
        return cls(
            available=True,
            covered=covered,
            total=total,
            percent=round((covered * 100.0) / total, 6),
        )

    def to_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


@dataclasses.dataclass(frozen=True)
class CoverageSummary:
    lines: MetricSummary
    functions: MetricSummary
    branches: MetricSummary

    def to_dict(self) -> dict[str, object]:
        return {
            "lines": self.lines.to_dict(),
            "functions": self.functions.to_dict(),
            "branches": self.branches.to_dict(),
        }


@dataclasses.dataclass(frozen=True)
class FileCoverage:
    path: str
    lines: tuple[LineCoverage, ...] | None
    functions: tuple[FunctionCoverage, ...] | None
    branches: tuple[BranchCoverage, ...] | None

    @property
    def summary(self) -> CoverageSummary:
        return CoverageSummary(
            lines=_summarize_records(self.lines),
            functions=_summarize_records(self.functions),
            branches=_summarize_records(self.branches),
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "path": self.path,
            "summary": self.summary.to_dict(),
            "lines": None if self.lines is None else [item.to_dict() for item in self.lines],
            "functions": (
                None
                if self.functions is None
                else [item.to_dict() for item in self.functions]
            ),
            "branches": (
                None
                if self.branches is None
                else [item.to_dict() for item in self.branches]
            ),
        }


@dataclasses.dataclass(frozen=True)
class CoverageReport:
    files: tuple[FileCoverage, ...]
    source_scopes: tuple[str, ...] | None
    dropped_source_count: int = 0

    @property
    def summary(self) -> CoverageSummary:
        return CoverageSummary(
            lines=_summarize_groups(item.lines for item in self.files),
            functions=_summarize_groups(item.functions for item in self.files),
            branches=_summarize_groups(item.branches for item in self.files),
        )

    def file(self, path: str) -> FileCoverage | None:
        normalized = PurePosixPath(path).as_posix()
        return next((item for item in self.files if item.path == normalized), None)

    def line_hits_by_path(self) -> dict[str, dict[int, int]]:
        """Return stable hit maps for files with measured line records.

        Files whose LCOV records contain no ``DA`` entries are omitted, keeping
        unavailable line measurement distinct from an empty/zero hit map.
        """

        return {
            item.path: {line.line: line.count for line in item.lines}
            for item in self.files
            if item.lines is not None
        }

    def to_dict(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "source_scopes": (
                None if self.source_scopes is None else list(self.source_scopes)
            ),
            "dropped_source_count": self.dropped_source_count,
            "file_count": len(self.files),
            "summary": self.summary.to_dict(),
            "files": [item.to_dict() for item in self.files],
        }


def merge_coverage_reports(reports: Iterable[CoverageReport]) -> CoverageReport:
    """Merge compatible normalized reports by summing recorded hit counts."""

    report_list = list(reports)
    merged: dict[str, _MutableSource] = {}
    for report in report_list:
        for source in report.files:
            target = merged.setdefault(source.path, _MutableSource(path=source.path))
            for line in source.lines or ():
                target.add_line(line.line, line.count, line.checksum)
            for function in source.functions or ():
                target.add_function(
                    name=function.name,
                    line=function.line,
                    end_line=function.end_line,
                    count=function.count,
                )
            for branch in source.branches or ():
                target.add_branch(
                    line=branch.line,
                    block=branch.block,
                    branch=branch.branch,
                    taken=branch.taken,
                )

    scope_values = {report.source_scopes for report in report_list}
    if not scope_values:
        scopes: tuple[str, ...] | None = None
    elif len(scope_values) == 1:
        scopes = next(iter(scope_values))
    elif None in scope_values:
        scopes = None
    else:
        scopes = tuple(sorted({scope for value in scope_values for scope in value or ()}))

    return CoverageReport(
        files=tuple(source.freeze() for _, source in sorted(merged.items())),
        source_scopes=scopes,
        dropped_source_count=sum(report.dropped_source_count for report in report_list),
    )


@dataclasses.dataclass
class _MutableSource:
    path: str
    lines: dict[int, tuple[int, str | None]] = dataclasses.field(default_factory=dict)
    functions: dict[tuple[int | None, int | None, str], int | None] = dataclasses.field(
        default_factory=dict
    )
    branches: dict[tuple[int, str, str], int | None] = dataclasses.field(
        default_factory=dict
    )

    def add_line(self, line: int, count: int, checksum: str | None) -> None:
        old_count, old_checksum = self.lines.get(line, (0, None))
        if old_checksum is not None and checksum is not None and old_checksum != checksum:
            raise CoverageParseError(
                f"conflicting checksums for {self.path}:{line}: "
                f"{old_checksum!r} != {checksum!r}"
            )
        self.lines[line] = (old_count + count, old_checksum or checksum)

    def add_function(
        self,
        *,
        name: str,
        line: int | None,
        end_line: int | None,
        count: int | None,
    ) -> None:
        key = (line, end_line, name)
        self.functions[key] = _merge_optional_count(self.functions.get(key), count)

    def add_branch(
        self,
        *,
        line: int,
        block: str,
        branch: str,
        taken: int | None,
    ) -> None:
        key = (line, block, branch)
        self.branches[key] = _merge_optional_count(self.branches.get(key), taken)

    def freeze(self) -> FileCoverage:
        lines: tuple[LineCoverage, ...] | None
        functions: tuple[FunctionCoverage, ...] | None
        branches: tuple[BranchCoverage, ...] | None

        if self.lines:
            lines = tuple(
                LineCoverage(line=line, count=value[0], checksum=value[1])
                for line, value in sorted(self.lines.items())
            )
        else:
            lines = None

        if self.functions:
            functions = tuple(
                sorted(
                    (
                        FunctionCoverage(
                            name=key[2],
                            line=key[0],
                            end_line=key[1],
                            count=count,
                        )
                        for key, count in self.functions.items()
                    ),
                    key=FunctionCoverage.sort_key,
                )
            )
        else:
            functions = None

        if self.branches:
            branches = tuple(
                sorted(
                    (
                        BranchCoverage(
                            line=key[0],
                            block=key[1],
                            branch=key[2],
                            taken=taken,
                        )
                        for key, taken in self.branches.items()
                    ),
                    key=BranchCoverage.sort_key,
                )
            )
        else:
            branches = None

        return FileCoverage(
            path=self.path,
            lines=lines,
            functions=functions,
            branches=branches,
        )


@dataclasses.dataclass
class _PendingRecord:
    raw_source: str | None = None
    lines: list[tuple[int, int, str | None]] = dataclasses.field(default_factory=list)
    function_definitions: list[tuple[int, int | None, str]] = dataclasses.field(
        default_factory=list
    )
    function_counts: dict[str, int] = dataclasses.field(default_factory=dict)
    branches: list[tuple[int, str, str, int | None]] = dataclasses.field(
        default_factory=list
    )

    @property
    def has_data(self) -> bool:
        return bool(
            self.raw_source
            or self.lines
            or self.function_definitions
            or self.function_counts
            or self.branches
        )


def parse_lcov(
    text: str,
    *,
    repo_root: Path | str,
    source_base: Path | str | None = None,
    source_scopes: Sequence[str] | None = None,
) -> CoverageReport:
    """Parse LCOV text into a stable, repository-relative report.

    ``source_scopes`` are repository-relative prefix paths. ``None`` keeps all
    in-repository sources. Absolute sources outside ``repo_root`` and relative
    paths that escape it are always dropped so reports never leak host paths.
    """

    root = Path(repo_root).resolve()
    base = Path(source_base).resolve() if source_base is not None else root
    normalized_scopes = _normalize_scopes(source_scopes)
    sources: dict[str, _MutableSource] = {}
    dropped_sources: set[str] = set()
    pending = _PendingRecord()

    def flush(record_line: int) -> None:
        nonlocal pending
        if not pending.has_data:
            pending = _PendingRecord()
            return
        if pending.raw_source is None:
            raise CoverageParseError(
                f"LCOV record ending near line {record_line} has data but no SF entry"
            )

        normalized_path = normalize_source_path(
            pending.raw_source,
            repo_root=root,
            source_base=base,
        )
        if normalized_path is None or not _path_in_scopes(
            normalized_path, normalized_scopes
        ):
            dropped_sources.add(pending.raw_source)
            pending = _PendingRecord()
            return

        target = sources.setdefault(
            normalized_path, _MutableSource(path=normalized_path)
        )
        for line, count, checksum in pending.lines:
            target.add_line(line, count, checksum)
        for line, end_line, name in pending.function_definitions:
            target.add_function(
                name=name,
                line=line,
                end_line=end_line,
                count=pending.function_counts.get(name),
            )
        defined_names = {name for _, _, name in pending.function_definitions}
        for name, count in pending.function_counts.items():
            if name not in defined_names:
                target.add_function(
                    name=name,
                    line=None,
                    end_line=None,
                    count=count,
                )
        for line, block, branch, taken in pending.branches:
            target.add_branch(line=line, block=block, branch=branch, taken=taken)
        pending = _PendingRecord()

    for input_line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.rstrip("\r")
        if not line or line.startswith("TN:"):
            continue
        if line == "end_of_record":
            flush(input_line_number)
            continue
        if line.startswith("SF:"):
            if pending.raw_source is not None:
                raise CoverageParseError(
                    f"line {input_line_number}: duplicate SF before end_of_record"
                )
            pending.raw_source = line[3:]
            if not pending.raw_source:
                raise CoverageParseError(f"line {input_line_number}: SF path is empty")
            continue
        if line.startswith("DA:"):
            parts = line[3:].split(",", 2)
            if len(parts) < 2:
                raise CoverageParseError(f"line {input_line_number}: malformed DA entry")
            line_number = _parse_nonnegative_int(parts[0], input_line_number, "DA line")
            count = _parse_nonnegative_int(parts[1], input_line_number, "DA count")
            if line_number == 0:
                raise CoverageParseError(f"line {input_line_number}: DA line must be positive")
            checksum = parts[2] or None if len(parts) == 3 else None
            pending.lines.append((line_number, count, checksum))
            continue
        if line.startswith("FN:"):
            start, end, name = _parse_function_definition(line[3:], input_line_number)
            pending.function_definitions.append((start, end, name))
            continue
        if line.startswith("FNDA:"):
            parts = line[5:].split(",", 1)
            if len(parts) != 2 or not parts[1]:
                raise CoverageParseError(f"line {input_line_number}: malformed FNDA entry")
            count = _parse_nonnegative_int(parts[0], input_line_number, "FNDA count")
            pending.function_counts[parts[1]] = (
                pending.function_counts.get(parts[1], 0) + count
            )
            continue
        if line.startswith("BRDA:"):
            parts = line[5:].split(",", 3)
            if len(parts) != 4 or not parts[1] or not parts[2]:
                raise CoverageParseError(f"line {input_line_number}: malformed BRDA entry")
            line_number = _parse_nonnegative_int(parts[0], input_line_number, "BRDA line")
            if line_number == 0:
                raise CoverageParseError(f"line {input_line_number}: BRDA line must be positive")
            taken = (
                None
                if parts[3] == "-"
                else _parse_nonnegative_int(parts[3], input_line_number, "BRDA taken")
            )
            pending.branches.append((line_number, parts[1], parts[2], taken))
            continue
        # LCOV aggregate fields (LF/LH/FNF/FNH/BRF/BRH) and extensions are
        # intentionally ignored. Summaries are recomputed from raw records.

    flush(len(text.splitlines()) + 1)

    return CoverageReport(
        files=tuple(source.freeze() for _, source in sorted(sources.items())),
        source_scopes=normalized_scopes,
        dropped_source_count=len(dropped_sources),
    )


def render_lcov(report: CoverageReport) -> str:
    """Render normalized, repository-relative LCOV in deterministic order."""

    lines: list[str] = ["TN:code-health-normalized"]
    for source in report.files:
        lines.append(f"SF:{source.path}")
        for function in source.functions or ():
            if function.line is not None:
                if function.end_line is None:
                    lines.append(f"FN:{function.line},{function.name}")
                else:
                    lines.append(
                        f"FN:{function.line},{function.end_line},{function.name}"
                    )
        for function in source.functions or ():
            if function.count is not None:
                lines.append(f"FNDA:{function.count},{function.name}")
        if source.functions is not None:
            lines.append(f"FNF:{len(source.functions)}")
            lines.append(
                f"FNH:{sum(1 for function in source.functions if function.covered)}"
            )
        for line in source.lines or ():
            rendered = f"DA:{line.line},{line.count}"
            if line.checksum is not None:
                rendered += f",{line.checksum}"
            lines.append(rendered)
        if source.lines is not None:
            lines.append(f"LF:{len(source.lines)}")
            lines.append(f"LH:{sum(1 for line in source.lines if line.covered)}")
        for branch in source.branches or ():
            taken = "-" if branch.taken is None else str(branch.taken)
            lines.append(
                f"BRDA:{branch.line},{branch.block},{branch.branch},{taken}"
            )
        if source.branches is not None:
            lines.append(f"BRF:{len(source.branches)}")
            lines.append(
                f"BRH:{sum(1 for branch in source.branches if branch.covered)}"
            )
        lines.append("end_of_record")
    return "\n".join(lines) + "\n"


def normalize_source_path(
    raw_path: str,
    *,
    repo_root: Path | str,
    source_base: Path | str | None = None,
) -> str | None:
    """Return a normalized repository-relative POSIX path, or ``None``.

    Returning ``None`` for out-of-repository sources keeps host SDK and package
    paths out of deterministic reports.
    """

    if not raw_path or "\x00" in raw_path:
        return None
    root = Path(repo_root).resolve()
    base = Path(source_base).resolve() if source_base is not None else root
    platform_path = raw_path.replace("\\", os.sep)
    candidate = Path(platform_path)
    if not candidate.is_absolute():
        candidate = base / candidate
    try:
        relative = candidate.resolve().relative_to(root)
    except (OSError, ValueError):
        return None
    normalized = PurePosixPath(relative.as_posix()).as_posix()
    if normalized in ("", ".") or any(
        part in ("", ".", "..") for part in PurePosixPath(normalized).parts
    ):
        return None
    return normalized


def _normalize_scopes(scopes: Sequence[str] | None) -> tuple[str, ...] | None:
    if scopes is None:
        return None
    normalized: set[str] = set()
    for raw_scope in scopes:
        scope = raw_scope.replace("\\", "/").strip("/")
        path = PurePosixPath(scope)
        if (
            not scope
            or path.is_absolute()
            or any(part in ("", ".", "..") for part in path.parts)
        ):
            raise ValueError(f"invalid repository-relative source scope: {raw_scope!r}")
        normalized.add(path.as_posix())
    return tuple(sorted(normalized))


def _path_in_scopes(path: str, scopes: tuple[str, ...] | None) -> bool:
    if scopes is None:
        return True
    return any(path == scope or path.startswith(scope + "/") for scope in scopes)


def _parse_function_definition(
    value: str, input_line_number: int
) -> tuple[int, int | None, str]:
    parts = value.split(",", 2)
    if len(parts) < 2:
        raise CoverageParseError(f"line {input_line_number}: malformed FN entry")
    start = _parse_nonnegative_int(parts[0], input_line_number, "FN line")
    if start == 0:
        raise CoverageParseError(f"line {input_line_number}: FN line must be positive")

    end: int | None = None
    if len(parts) == 3 and re.fullmatch(r"\d+", parts[1]):
        end = _parse_nonnegative_int(parts[1], input_line_number, "FN end line")
        name = parts[2]
        if end < start:
            raise CoverageParseError(
                f"line {input_line_number}: FN end line precedes start line"
            )
    else:
        name = value.split(",", 1)[1]
    if not name:
        raise CoverageParseError(f"line {input_line_number}: FN name is empty")
    return start, end, name


def _parse_nonnegative_int(value: str, line: int, label: str) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as exc:
        raise CoverageParseError(f"line {line}: {label} is not an integer") from exc
    if parsed < 0:
        raise CoverageParseError(f"line {line}: {label} must be nonnegative")
    return parsed


def _merge_optional_count(old: int | None, new: int | None) -> int | None:
    if old is None:
        return new
    if new is None:
        return old
    return old + new


def _identifier_sort_key(identifier: str) -> tuple[int, int | str]:
    if re.fullmatch(r"\d+", identifier):
        return 0, int(identifier)
    return 1, identifier


def _summarize_records(records: Sequence[object] | None) -> MetricSummary:
    if records is None or not records:
        return MetricSummary.unavailable()
    covered = sum(1 for record in records if bool(getattr(record, "covered")))
    return MetricSummary.from_coverage(covered, len(records))


def _summarize_groups(groups: Iterable[Sequence[object] | None]) -> MetricSummary:
    available_groups = [group for group in groups if group]
    if not available_groups:
        return MetricSummary.unavailable()
    total = sum(len(group) for group in available_groups)
    covered = sum(
        1
        for group in available_groups
        for record in group
        if bool(getattr(record, "covered"))
    )
    return MetricSummary.from_coverage(covered, total)


def coverage_report_from_dict(data: Mapping[str, object]) -> CoverageReport:
    """Rehydrate the normalized schema used by code-health JSON artifacts.

    This deliberately validates only the stable public fields needed by CRAP
    and comparison tools; raw LCOV should be reparsed for forensic detail.
    """

    if data.get("schema_version") != 1:
        raise CoverageParseError("coverage JSON schema_version must be 1")
    raw_files = data.get("files")
    if not isinstance(raw_files, list):
        raise CoverageParseError("coverage JSON files must be an array")
    raw_scopes = data.get("source_scopes")
    if raw_scopes is not None and not (
        isinstance(raw_scopes, list) and all(isinstance(item, str) for item in raw_scopes)
    ):
        raise CoverageParseError("coverage JSON source_scopes must be null or strings")

    files: list[FileCoverage] = []
    for raw_file in raw_files:
        if not isinstance(raw_file, dict) or not isinstance(raw_file.get("path"), str):
            raise CoverageParseError("coverage JSON file entries require a path")
        path = raw_file["path"]
        lines = _lines_from_json(raw_file.get("lines"))
        functions = _functions_from_json(raw_file.get("functions"))
        branches = _branches_from_json(raw_file.get("branches"))
        files.append(FileCoverage(path=path, lines=lines, functions=functions, branches=branches))

    dropped = data.get("dropped_source_count", 0)
    if not isinstance(dropped, int) or dropped < 0:
        raise CoverageParseError("coverage JSON dropped_source_count must be nonnegative")
    return CoverageReport(
        files=tuple(sorted(files, key=lambda item: item.path)),
        source_scopes=None if raw_scopes is None else tuple(sorted(set(raw_scopes))),
        dropped_source_count=dropped,
    )


def _lines_from_json(raw: object) -> tuple[LineCoverage, ...] | None:
    if raw is None:
        return None
    if not isinstance(raw, list):
        raise CoverageParseError("coverage JSON lines must be null or an array")
    result: list[LineCoverage] = []
    for item in raw:
        if not isinstance(item, dict):
            raise CoverageParseError("coverage JSON line entry must be an object")
        line, count = item.get("line"), item.get("count")
        checksum = item.get("checksum")
        if not isinstance(line, int) or line <= 0 or not isinstance(count, int) or count < 0:
            raise CoverageParseError("coverage JSON line/count is invalid")
        if checksum is not None and not isinstance(checksum, str):
            raise CoverageParseError("coverage JSON checksum must be a string")
        result.append(LineCoverage(line=line, count=count, checksum=checksum))
    return tuple(sorted(result)) if result else None


def _functions_from_json(raw: object) -> tuple[FunctionCoverage, ...] | None:
    if raw is None:
        return None
    if not isinstance(raw, list):
        raise CoverageParseError("coverage JSON functions must be null or an array")
    result: list[FunctionCoverage] = []
    for item in raw:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            raise CoverageParseError("coverage JSON function entry is invalid")
        line, end_line, count = item.get("line"), item.get("end_line"), item.get("count")
        if line is not None and (not isinstance(line, int) or line <= 0):
            raise CoverageParseError("coverage JSON function line is invalid")
        if end_line is not None and (not isinstance(end_line, int) or end_line <= 0):
            raise CoverageParseError("coverage JSON function end_line is invalid")
        if count is not None and (not isinstance(count, int) or count < 0):
            raise CoverageParseError("coverage JSON function count is invalid")
        result.append(
            FunctionCoverage(
                name=item["name"],
                line=line,
                end_line=end_line,
                count=count,
            )
        )
    return tuple(sorted(result, key=FunctionCoverage.sort_key)) if result else None


def _branches_from_json(raw: object) -> tuple[BranchCoverage, ...] | None:
    if raw is None:
        return None
    if not isinstance(raw, list):
        raise CoverageParseError("coverage JSON branches must be null or an array")
    result: list[BranchCoverage] = []
    for item in raw:
        if not isinstance(item, dict):
            raise CoverageParseError("coverage JSON branch entry is invalid")
        line, block, branch, taken = (
            item.get("line"),
            item.get("block"),
            item.get("branch"),
            item.get("taken"),
        )
        if not isinstance(line, int) or line <= 0:
            raise CoverageParseError("coverage JSON branch line is invalid")
        if not isinstance(block, str) or not isinstance(branch, str):
            raise CoverageParseError("coverage JSON branch identifiers are invalid")
        if taken is not None and (not isinstance(taken, int) or taken < 0):
            raise CoverageParseError("coverage JSON branch taken count is invalid")
        result.append(
            BranchCoverage(line=line, block=block, branch=branch, taken=taken)
        )
    return tuple(sorted(result, key=BranchCoverage.sort_key)) if result else None
