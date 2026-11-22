#!/usr/bin/env python3
"""Scoped static-analysis execution, normalization, baselines, and suppressions."""

from __future__ import annotations

import datetime as dt
import re
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

from common import (
    CodeHealthError,
    CommandResult,
    DEFAULT_CAPTURE_BYTES,
    normalize_message,
    normalized_repo_path,
    probe_tool,
    read_json_bounded,
    run_bounded,
    scrub_command,
    scrub_text,
    stable_fingerprint,
)


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
CPP_TIDY_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
DART_SUFFIXES = {".dart"}
DEFAULT_MAX_FILES = 50
DEFAULT_MAX_DIAGNOSTICS_PER_TOOL = 5_000
SCHEMA_VERSION = 1


@dataclass(frozen=True)
class Diagnostic:
    tool: str
    path: str
    line: int | None
    column: int | None
    severity: str
    code: str
    message: str

    @property
    def fingerprint(self) -> str:
        """Location-specific identity used by narrow suppressions."""

        return stable_fingerprint(
            self.baseline_fingerprint,
            str(self.line) if self.line is not None else "<no-line>",
            str(self.column) if self.column is not None else "<no-column>",
        )

    @property
    def baseline_fingerprint(self) -> str:
        """Line-stable identity used only to compare analyzer baselines."""

        return stable_fingerprint(
            self.tool,
            self.path,
            self.severity,
            self.code,
            normalize_message(self.message),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "baseline_fingerprint": self.baseline_fingerprint,
            "code": self.code,
            "column": self.column,
            "fingerprint": self.fingerprint,
            "line": self.line,
            "message": normalize_message(self.message),
            "path": self.path,
            "severity": self.severity,
            "tool": self.tool,
        }


def _optional_int(value: str | None) -> int | None:
    if value is None or value == "":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_cppcheck_xml(text: str, repo_root: Path) -> list[Diagnostic]:
    """Parse cppcheck XML v2 without treating malformed output as no findings."""

    start = text.find("<?xml")
    if start < 0:
        start = text.find("<results")
    if start < 0:
        raise CodeHealthError("cppcheck output did not contain an XML results envelope")
    try:
        root = ET.fromstring(text[start:])
    except ET.ParseError as exc:
        raise CodeHealthError(f"invalid cppcheck XML: {exc}") from exc

    diagnostics: list[Diagnostic] = []
    for error in root.findall(".//error"):
        locations = error.findall("location") or [None]
        for location in locations:
            raw_path = location.get("file", "") if location is not None else ""
            path = normalized_repo_path(raw_path or "<unknown>", repo_root)
            diagnostics.append(
                Diagnostic(
                    tool="cppcheck",
                    path=path,
                    line=_optional_int(location.get("line") if location is not None else None),
                    column=_optional_int(location.get("column") if location is not None else None),
                    severity=error.get("severity", "unknown"),
                    code=error.get("id", "unknown"),
                    message=scrub_text(
                        error.get("msg") or error.get("verbose") or "cppcheck finding",
                        repo_root,
                    ),
                )
            )
    return diagnostics


CLANG_DIAGNOSTIC_RE = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<column>\d+):\s+"
    r"(?P<severity>warning|error|note):\s+(?P<message>.*?)"
    r"(?:\s+\[(?P<code>[^\]]+)\])?$"
)


def parse_clang_tidy_output(text: str, repo_root: Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    for raw_line in text.splitlines():
        match = CLANG_DIAGNOSTIC_RE.match(raw_line.strip())
        if not match:
            continue
        diagnostics.append(
            Diagnostic(
                tool="clang-tidy",
                path=normalized_repo_path(match.group("path"), repo_root),
                line=int(match.group("line")),
                column=int(match.group("column")),
                severity=match.group("severity"),
                code=match.group("code") or "clang-diagnostic",
                message=scrub_text(match.group("message"), repo_root),
            )
        )
    return diagnostics


def parse_dart_machine_output(text: str, repo_root: Path, app_root: Path) -> list[Diagnostic]:
    """Parse `dart analyze --format machine` output."""

    diagnostics: list[Diagnostic] = []
    for raw_line in text.splitlines():
        parts = raw_line.split("|", 7)
        if len(parts) != 8 or parts[0] not in {"ERROR", "WARNING", "INFO"}:
            continue
        severity, _kind, code, raw_path, line, column, _length, message = parts
        diagnostics.append(
            Diagnostic(
                tool="dart",
                path=normalized_repo_path(raw_path, repo_root, base_dir=app_root),
                line=_optional_int(line),
                column=_optional_int(column),
                severity=severity.lower(),
                code=code,
                message=scrub_text(message, repo_root),
            )
        )
    return diagnostics


def _load_baseline_fingerprints(path: Path | None) -> Counter[str]:
    if path is None:
        return Counter()
    payload = read_json_bounded(path)
    if (
        not isinstance(payload, dict)
        or payload.get("schema_version") != SCHEMA_VERSION
        or not isinstance(payload.get("diagnostics"), list)
    ):
        raise CodeHealthError("static-analysis baseline must contain a diagnostics list")
    fingerprints: Counter[str] = Counter()
    for item in payload["diagnostics"]:
        if not isinstance(item, dict):
            raise CodeHealthError("static-analysis baseline diagnostics must be objects")
        fingerprint = item.get("baseline_fingerprint")
        if not isinstance(fingerprint, str) or not fingerprint:
            raise CodeHealthError(
                "static-analysis baseline diagnostic lacks baseline_fingerprint"
            )
        fingerprints[fingerprint] += 1
    return fingerprints


def load_suppressions(path: Path | None, evaluation_date: dt.date) -> dict[str, dict[str, Any]]:
    """Load narrow, owned, expiring suppressions keyed by stable fingerprint."""

    if path is None:
        return {}
    payload = read_json_bounded(path)
    if not isinstance(payload, dict) or payload.get("schema_version") != SCHEMA_VERSION:
        raise CodeHealthError("suppression file must use schema_version 1")
    entries = payload.get("suppressions")
    if not isinstance(entries, list):
        raise CodeHealthError("suppression file must contain a suppressions list")

    suppressions: dict[str, dict[str, Any]] = {}
    for index, item in enumerate(entries):
        if not isinstance(item, dict):
            raise CodeHealthError(f"suppression {index} must be an object")
        fingerprint = item.get("fingerprint")
        reason = item.get("reason")
        owner = item.get("owner")
        expires = item.get("expires")
        if not all(isinstance(value, str) and value.strip() for value in (fingerprint, reason, owner, expires)):
            raise CodeHealthError(
                f"suppression {index} requires non-empty fingerprint, reason, owner, and expires"
            )
        try:
            expiry_date = dt.date.fromisoformat(expires)
        except ValueError as exc:
            raise CodeHealthError(f"suppression {index} has invalid ISO expiry: {expires}") from exc
        if fingerprint in suppressions:
            raise CodeHealthError(f"duplicate suppression fingerprint: {fingerprint}")
        suppressions[fingerprint] = {
            "expires": expires,
            "expired": expiry_date < evaluation_date,
            "owner": owner.strip(),
            "reason": reason.strip(),
        }
    return suppressions


def classify_diagnostics(
    diagnostics: Iterable[Diagnostic],
    baseline_fingerprints: Mapping[str, int],
    suppressions: Mapping[str, Mapping[str, Any]],
) -> list[dict[str, Any]]:
    classified: list[dict[str, Any]] = []
    remaining_baseline = Counter(baseline_fingerprints)
    ordered = sorted(
        diagnostics,
        key=lambda item: (
            item.path,
            item.line if item.line is not None else -1,
            item.column if item.column is not None else -1,
            item.tool,
            item.code,
            item.message,
        ),
    )
    for diagnostic in ordered:
        item = diagnostic.to_dict()
        suppression = suppressions.get(diagnostic.fingerprint)
        baseline_matched = remaining_baseline[diagnostic.baseline_fingerprint] > 0
        if baseline_matched:
            remaining_baseline[diagnostic.baseline_fingerprint] -= 1
        if suppression is not None and not suppression.get("expired", False):
            item["state"] = "suppressed"
            item["suppression"] = dict(suppression)
        elif baseline_matched:
            item["state"] = "baseline"
            item["suppression"] = None
        else:
            item["state"] = "new"
            item["suppression"] = dict(suppression) if suppression is not None else None
        classified.append(item)
    return sorted(
        classified,
        key=lambda item: (
            item["path"],
            item["line"] if item["line"] is not None else -1,
            item["column"] if item["column"] is not None else -1,
            item["tool"],
            item["code"],
            item["message"],
        ),
    )


def _result_payload(
    tool: str,
    status: str,
    *,
    command: Sequence[str] | None = None,
    version: str | None = None,
    reason: str | None = None,
    returncode: int | None = None,
    output: str = "",
    truncated: bool = False,
    commands: Sequence[Sequence[str]] | None = None,
    invocations: Sequence[Mapping[str, Any]] | None = None,
    out_of_scope_diagnostics: int = 0,
    timed_out: bool = False,
    repo_root: Path,
) -> dict[str, Any]:
    return {
        "command": scrub_command(command, repo_root) if command else None,
        "commands": (
            [scrub_command(item, repo_root) for item in commands]
            if commands is not None
            else None
        ),
        "invocations": list(invocations) if invocations is not None else None,
        "name": tool,
        "out_of_scope_diagnostics": out_of_scope_diagnostics,
        "output": scrub_text(output, repo_root),
        "output_truncated": truncated,
        "reason": reason,
        "returncode": returncode,
        "status": status,
        "timed_out": timed_out,
        "version": version,
    }


def run_static_analysis(
    *,
    repo_root: Path,
    files: Sequence[str],
    requested_tools: Sequence[str] | None = None,
    compile_commands: Path | None = None,
    baseline: Path | None = None,
    suppressions_path: Path | None = None,
    evaluation_date: dt.date | None = None,
    max_files: int = DEFAULT_MAX_FILES,
    command_runner: Callable[..., CommandResult] = run_bounded,
) -> dict[str, Any]:
    """Run applicable analyzers on an explicit bounded file scope."""

    normalized_files = sorted(set(files))
    if len(normalized_files) > max_files:
        raise CodeHealthError(
            f"analysis scope contains {len(normalized_files)} files; raise --max-files above {max_files} explicitly"
        )
    for path in normalized_files:
        if path.startswith("<external>/") or Path(path).is_absolute():
            raise CodeHealthError(f"analysis scope must be repository-relative: {path}")

    cpp_files = [path for path in normalized_files if Path(path).suffix.lower() in CPP_SUFFIXES]
    tidy_files = [path for path in cpp_files if Path(path).suffix.lower() in CPP_TIDY_SUFFIXES]
    dart_files = [path for path in normalized_files if Path(path).suffix.lower() in DART_SUFFIXES]
    applicable = {
        "clang-tidy": bool(tidy_files),
        "cppcheck": bool(cpp_files),
        "dart": bool(dart_files),
    }
    tools = sorted(set(requested_tools or ("clang-tidy", "cppcheck", "dart")))
    unknown = sorted(set(tools) - set(applicable))
    if unknown:
        raise CodeHealthError(f"unknown analysis tools: {', '.join(unknown)}")

    raw_diagnostics: list[Diagnostic] = []
    tool_results: list[dict[str, Any]] = []
    app_root = repo_root / "app"

    for tool in tools:
        if not applicable[tool]:
            tool_results.append(
                _result_payload(tool, "not_applicable", reason="no matching files in scope", repo_root=repo_root)
            )
            continue
        probe = probe_tool(tool, ("--version",), cwd=repo_root, repo_root=repo_root)
        if probe["status"] != "available":
            tool_results.append(
                _result_payload(
                    tool,
                    probe["status"],
                    version=probe.get("version"),
                    reason=probe.get("reason"),
                    repo_root=repo_root,
                )
            )
            continue

        executable = tool
        executed_commands: list[list[str]] | None = None
        invocation_records: list[dict[str, Any]] | None = None
        invocation_failed = False
        invocation_failure_reason: str | None = None
        diagnostic_limit_exceeded = False
        if tool == "clang-tidy":
            if compile_commands is None or not (compile_commands / "compile_commands.json").is_file():
                tool_results.append(
                    _result_payload(
                        tool,
                        "deferred",
                        version=probe["version"],
                        reason="--compile-commands must name a directory containing compile_commands.json",
                        repo_root=repo_root,
                    )
                )
                continue
            command = [executable, *tidy_files, f"-p={compile_commands}"]
            result = command_runner(command, cwd=repo_root)
            parsed = parse_clang_tidy_output(result.output, repo_root)
            invocation_failed = result.timed_out or result.returncode != 0
            if result.timed_out:
                invocation_failure_reason = "clang-tidy invocation timed out"
        elif tool == "cppcheck":
            command = [
                executable,
                "--xml",
                "--xml-version=2",
                "--enable=warning,performance,portability",
                "--std=c++17",
                "--error-exitcode=1",
                *cpp_files,
            ]
            result = command_runner(command, cwd=repo_root)
            try:
                parsed = parse_cppcheck_xml(result.output, repo_root)
            except CodeHealthError as exc:
                parsed = []
                invocation_failure_reason = str(exc)
            if result.timed_out:
                invocation_failure_reason = "cppcheck invocation timed out"
            invocation_failed = (
                invocation_failure_reason is not None
                or result.timed_out
                or result.returncode not in {0, 1}
                or (result.returncode != 0 and not parsed)
            )
        else:
            parsed = []
            combined_output: list[str] = []
            retained_chars = 0
            returncodes: list[int] = []
            truncated = False
            timed_out = False
            commands: list[list[str]] = []
            invocation_records = []
            for file_path in dart_files:
                app_relative = Path(file_path).relative_to("app").as_posix()
                command = [executable, "analyze", "--format", "machine", app_relative]
                commands.append(command)
                one_result = command_runner(command, cwd=app_root, env={"CI": "true"})
                remaining = DEFAULT_CAPTURE_BYTES - retained_chars
                if remaining > 0:
                    retained = one_result.output[:remaining]
                    combined_output.append(retained)
                    retained_chars += len(retained)
                if len(one_result.output) > remaining:
                    truncated = True
                returncodes.append(one_result.returncode)
                truncated = truncated or one_result.truncated
                timed_out = timed_out or one_result.timed_out
                one_parsed = parse_dart_machine_output(
                    one_result.output,
                    repo_root,
                    app_root,
                )
                remaining_diagnostics = DEFAULT_MAX_DIAGNOSTICS_PER_TOOL - len(parsed)
                if len(one_parsed) > remaining_diagnostics:
                    diagnostic_limit_exceeded = True
                if remaining_diagnostics > 0:
                    parsed.extend(one_parsed[:remaining_diagnostics])
                one_failed = one_result.timed_out or one_result.returncode < 0 or (
                    one_result.returncode != 0 and not one_parsed
                )
                if one_result.timed_out:
                    invocation_failure_reason = (
                        "one or more dart analyze invocations timed out"
                    )
                if one_failed:
                    invocation_failed = True
                invocation_records.append(
                    {
                        "command": scrub_command(command, repo_root),
                        "cwd": "app",
                        "environment": {"CI": "true"},
                        "output_truncated": one_result.truncated,
                        "parsed_diagnostics": len(one_parsed),
                        "returncode": one_result.returncode,
                        "status": "invocation_error"
                        if one_failed
                        else ("completed_with_findings" if one_parsed else "completed"),
                        "timed_out": one_result.timed_out,
                    }
                )
            result = CommandResult(
                tuple(command for commands_item in commands for command in commands_item),
                next((code for code in returncodes if code != 0), 0),
                "\n".join(combined_output),
                truncated,
                timed_out,
            )
            command = ["dart", "analyze", "--format", "machine", "<one-file-at-a-time>"]
            executed_commands = commands

        if len(parsed) > DEFAULT_MAX_DIAGNOSTICS_PER_TOOL:
            parsed = parsed[:DEFAULT_MAX_DIAGNOSTICS_PER_TOOL]
            diagnostic_limit_exceeded = True

        in_scope = [diagnostic for diagnostic in parsed if diagnostic.path in normalized_files]
        out_of_scope_count = len(parsed) - len(in_scope)
        raw_diagnostics.extend(in_scope)
        if result.truncated or diagnostic_limit_exceeded:
            status = "incomplete_output"
            reason = (
                "analyzer output or diagnostic count exceeded its bounded capture; "
                "retained diagnostics are incomplete"
            )
        elif invocation_failed:
            status = "invocation_error"
            reason = invocation_failure_reason or (
                f"one or more analyzer invocations exited unexpectedly ({result.returncode})"
            )
        elif in_scope:
            status = "completed_with_findings"
            reason = None
        elif result.returncode == 0:
            status = "completed"
            reason = None
        else:
            status = "completed"
            reason = None
        tool_results.append(
            _result_payload(
                tool,
                status,
                command=command,
                version=probe["version"],
                reason=reason,
                returncode=result.returncode,
                output=result.output,
                truncated=result.truncated,
                commands=executed_commands,
                invocations=invocation_records,
                out_of_scope_diagnostics=out_of_scope_count,
                timed_out=result.timed_out,
                repo_root=repo_root,
            )
        )

    date = evaluation_date or dt.date.today()
    baseline_fingerprints = _load_baseline_fingerprints(baseline)
    suppressions = load_suppressions(suppressions_path, date)
    diagnostics = classify_diagnostics(raw_diagnostics, baseline_fingerprints, suppressions)
    counts = {
        "baseline": sum(item["state"] == "baseline" for item in diagnostics),
        "new": sum(item["state"] == "new" for item in diagnostics),
        "suppressed": sum(item["state"] == "suppressed" for item in diagnostics),
        "total": len(diagnostics),
    }
    applicable_results = [item for item in tool_results if item["status"] != "not_applicable"]
    available_results = [
        item
        for item in applicable_results
        if item["status"]
        in {"completed", "completed_with_findings", "incomplete_output", "invocation_error"}
    ]
    return {
        "diagnostics": diagnostics,
        "evaluation_date": date.isoformat(),
        "schema_version": SCHEMA_VERSION,
        "scope": {"file_count": len(normalized_files), "files": normalized_files},
        "summary": {
            **counts,
            "all_applicable_tools_unavailable": bool(applicable_results) and not available_results,
            "invocation_errors": sum(item["status"] == "invocation_error" for item in tool_results),
            "incomplete_outputs": sum(
                item["status"] == "incomplete_output" for item in tool_results
            ),
            "out_of_scope_diagnostics": sum(
                int(item.get("out_of_scope_diagnostics", 0)) for item in tool_results
            ),
        },
        "tools": tool_results,
    }


def render_human_summary(report: Mapping[str, Any]) -> str:
    summary = report["summary"]
    lines = [
        "Static analysis",
        f"scope: {report['scope']['file_count']} file(s)",
        (
            "diagnostics: "
            f"{summary['new']} new, {summary['baseline']} baseline, "
            f"{summary['suppressed']} suppressed"
        ),
    ]
    for tool in report["tools"]:
        detail = f" ({tool['reason']})" if tool.get("reason") else ""
        lines.append(f"{tool['name']}: {tool['status']}{detail}")
    return "\n".join(lines)
