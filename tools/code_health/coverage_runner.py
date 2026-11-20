#!/usr/bin/env python3
"""Tool-neutral native and Flutter coverage orchestration.

The command builders keep generated state under a revision-scoped output
directory, append instrumentation through PlatformIO's environment override,
and never require a coverage-only ``platformio.ini`` environment.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import threading
from typing import Callable, Mapping, Optional, Sequence

try:  # Supports both direct scripts/tests and package-style imports.
    from .coverage_data import CoverageParseError, CoverageReport, parse_lcov, render_lcov
except ImportError:  # pragma: no cover - exercised by direct unittest discovery.
    from coverage_data import CoverageParseError, CoverageReport, parse_lcov, render_lcov


NATIVE_SOURCE_SCOPES = (
    "include",
    "lib/foxie_embedded/include",
    "lib/foxie_embedded/src",
    "src",
)
FLUTTER_SOURCE_SCOPES = ("app/lib",)
DEFAULT_NATIVE_FILTER = "test_all_native"
NATIVE_AGGREGATE_EXCLUSIONS = (
    "directories ending in _long",
    "directories ending in _manual",
    "directories containing ALL_NATIVE_OPT_IN",
)
MAX_DIAGNOSTIC_CHARS = 4000
MAX_CAPTURE_BYTES = 4 * 1024 * 1024
MAX_LCOV_BYTES = 64 * 1024 * 1024
MAX_COMMAND_SECONDS = 1_200
PIPE_DRAIN_JOIN_SECONDS = 5.0
PIO_COVERAGE_POST_SCRIPT = "post:tools/code_health/pio_coverage_toolchain.py"


CommandExecutor = Callable[
    [Sequence[str], Path, Mapping[str, str]], object
]
ToolResolver = Callable[[str], Optional[str]]


@dataclasses.dataclass(frozen=True)
class ToolProbe:
    name: str
    available: bool
    path: str | None
    version: str | None
    diagnostic: str | None = None

    def to_dict(self) -> dict[str, object]:
        return {
            "name": self.name,
            "available": self.available,
            "executable": None if self.path is None else Path(self.path).name,
            "version": self.version,
            "diagnostic": self.diagnostic,
        }


@dataclasses.dataclass(frozen=True)
class RecordedCommand:
    phase: str
    argv: tuple[str, ...]
    cwd: str
    environment: tuple[tuple[str, str], ...]
    returncode: int
    stdout_truncated: bool = False
    stderr_truncated: bool = False
    timed_out: bool = False

    def to_dict(self) -> dict[str, object]:
        return {
            "phase": self.phase,
            "argv": list(self.argv),
            "cwd": self.cwd,
            "environment": dict(self.environment),
            "returncode": self.returncode,
            "stdout_truncated": self.stdout_truncated,
            "stderr_truncated": self.stderr_truncated,
            "timed_out": self.timed_out,
        }


@dataclasses.dataclass(frozen=True)
class CommandOutcome:
    args: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str
    stdout_truncated: bool = False
    stderr_truncated: bool = False
    timed_out: bool = False


class _BoundedCapture:
    """Drain a subprocess pipe while retaining at most ``limit`` bytes."""

    def __init__(self, limit: int) -> None:
        self._limit = limit
        self._buffer = bytearray()
        self.truncated = False

    def drain(self, stream: object) -> None:
        reader = getattr(stream, "read")
        while True:
            chunk = reader(64 * 1024)
            if not chunk:
                break
            if not isinstance(chunk, bytes):
                chunk = str(chunk).encode("utf-8", errors="replace")
            remaining = self._limit - len(self._buffer)
            if remaining > 0:
                self._buffer.extend(chunk[:remaining])
            if len(chunk) > remaining:
                self.truncated = True
        close = getattr(stream, "close", None)
        if close is not None:
            close()

    def text(self) -> str:
        return bytes(self._buffer).decode("utf-8", errors="replace")


@dataclasses.dataclass(frozen=True)
class NativeCoveragePlan:
    repo_root: Path
    output_dir: Path
    revision: str
    worktree_fingerprint: str | None
    revision_key: str
    backend: str
    requested_backend: str
    suite_filter: str
    build_dir: Path
    profile_dir: Path
    merged_profile: Path
    lcov_path: Path
    json_path: Path
    appended_build_flags: tuple[str, ...]
    appended_link_flags: tuple[str, ...]
    build_environment: Mapping[str, str]
    pio_core_dir: Path | None = None
    suite_manifest: Mapping[str, object] = dataclasses.field(default_factory=dict)

    @property
    def build_command(self) -> tuple[str, ...]:
        return (
            "pio",
            "test",
            "-e",
            "native",
            "-f",
            self.suite_filter,
            "--without-testing",
        )

    def program_candidates(self) -> tuple[Path, ...]:
        environment_dir = self.build_dir / "native"
        return environment_dir / "program", environment_dir / "program.exe"

    def to_dict(self) -> dict[str, object]:
        return {
            "kind": "native",
            "revision": self.revision,
            "worktree_fingerprint": self.worktree_fingerprint,
            "revision_key": self.revision_key,
            "backend": self.backend,
            "requested_backend": self.requested_backend,
            "suite_filter": self.suite_filter,
            "aggregate_exclusions": list(NATIVE_AGGREGATE_EXCLUSIONS),
            "includes_foxie_aggregate": self.suite_filter == DEFAULT_NATIVE_FILTER,
            "build_dir": _relative_display(self.build_dir, self.repo_root),
            "output_dir": _relative_display(self.output_dir, self.repo_root),
            "build_flags_mode": "append",
            "appended_build_flags": list(self.appended_build_flags),
            "link_flags_mode": "post-script append",
            "appended_link_flags": list(self.appended_link_flags),
            "toolchain_post_script": PIO_COVERAGE_POST_SCRIPT,
            "suite_manifest": dict(self.suite_manifest),
            "build_command": list(self.build_command),
            "pio_core_dir": (
                None
                if self.pio_core_dir is None
                else _relative_display(self.pio_core_dir, self.repo_root)
            ),
        }


@dataclasses.dataclass(frozen=True)
class FlutterCoveragePlan:
    repo_root: Path
    app_root: Path
    output_dir: Path
    lcov_path: Path
    json_path: Path
    test_events_path: Path
    revision: str | None = None
    worktree_fingerprint: str | None = None

    @property
    def test_command(self) -> tuple[str, ...]:
        coverage_path = Path(os.path.relpath(self.lcov_path, self.app_root)).as_posix()
        return (
            "flutter",
            "test",
            "--no-pub",
            "--coverage",
            "--branch-coverage",
            "--coverage-path",
            coverage_path,
            "--reporter=json",
        )

    def to_dict(self) -> dict[str, object]:
        return {
            "kind": "flutter",
            "revision": self.revision,
            "worktree_fingerprint": self.worktree_fingerprint,
            "source_scope": list(FLUTTER_SOURCE_SCOPES),
            "output_dir": _relative_display(self.output_dir, self.repo_root),
            "test_command": list(self.test_command),
            "raw_json_reporter": _relative_display(self.test_events_path, self.repo_root),
            "raw_json_reporter_note": "diagnostic-only; host paths, timing, and process IDs may vary",
        }


@dataclasses.dataclass(frozen=True)
class CoverageRunResult:
    kind: str
    status: str
    backend: str | None
    selection: Mapping[str, object]
    tools: tuple[ToolProbe, ...]
    commands: tuple[RecordedCommand, ...]
    diagnostics: tuple[str, ...]
    artifacts: Mapping[str, str]
    report: CoverageReport | None = None

    def __post_init__(self) -> None:
        if self.status not in {"ok", "unavailable", "failed"}:
            raise ValueError(f"invalid coverage run status: {self.status!r}")

    @property
    def ok(self) -> bool:
        return self.status == "ok"

    def to_dict(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "kind": self.kind,
            "status": self.status,
            "backend": self.backend,
            "selection": dict(self.selection),
            "tools": [tool.to_dict() for tool in self.tools],
            "commands": [command.to_dict() for command in self.commands],
            "diagnostics": list(self.diagnostics),
            "artifacts": dict(sorted(self.artifacts.items())),
            "coverage": None if self.report is None else self.report.to_dict(),
        }


def plan_native_coverage(
    *,
    repo_root: Path | str,
    output_dir: Path | str,
    revision: str,
    worktree_fingerprint: str | None = None,
    backend: str = "llvm",
    requested_backend: str | None = None,
    suite_filter: str = DEFAULT_NATIVE_FILTER,
    cc: str = "clang",
    cxx: str = "clang++",
    existing_build_flags: str = "",
    existing_extra_scripts: str = "",
    pio_core_dir: Path | str | None = None,
) -> NativeCoveragePlan:
    """Create a deterministic native coverage plan without running tools."""

    root = Path(repo_root).resolve()
    output = _resolve_output(root, output_dir)
    if backend not in {"llvm", "gcovr"}:
        raise ValueError("native coverage backend must be llvm or gcovr")
    if not revision.strip():
        raise ValueError("native coverage requires a non-empty revision")
    if not re.fullmatch(r"[A-Za-z0-9_.+*/:-]+", suite_filter):
        raise ValueError(f"unsafe PlatformIO test filter: {suite_filter!r}")

    revision_key = _revision_key(revision, backend, worktree_fingerprint)
    build_dir = output / "pio" / revision_key
    profile_dir = output / "profiles" / revision_key
    if backend == "llvm":
        flags = (
            "-fprofile-instr-generate",
            "-fcoverage-mapping",
            "-fcoverage-compilation-dir=.",
        )
        link_flags = ("-fprofile-instr-generate",)
    else:
        flags = ("--coverage", "-O0", "-g")
        link_flags = ("--coverage",)

    combined_flags = " ".join(
        part for part in (existing_build_flags.strip(), " ".join(flags)) if part
    )
    combined_extra_scripts = "\n".join(
        part
        for part in (existing_extra_scripts.strip(), PIO_COVERAGE_POST_SCRIPT)
        if part
    )
    environment = {
        "CC": cc,
        "CXX": cxx,
        # The native platform discards shell CC/CXX and reloads gcc/g++.  The
        # owned post-script reapplies the locally-probed compiler after that.
        "CODE_HEALTH_CC": cc,
        "CODE_HEALTH_CXX": cxx,
        "CODE_HEALTH_LINK_FLAGS": " ".join(link_flags),
        # PlatformIO appends this system override to platformio.ini build_flags.
        # It does not replace -std=gnu++17 or LITCH_NATIVE in env:native.
        "PLATFORMIO_BUILD_FLAGS": combined_flags,
        "PLATFORMIO_BUILD_DIR": str(build_dir),
        # PlatformIO appends this multi-value override to configured scripts,
        # preserving both native aggregate generators from platformio.ini.
        "PLATFORMIO_EXTRA_SCRIPTS": combined_extra_scripts,
        "PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no",
    }
    resolved_core_dir: Path | None = None
    if pio_core_dir is not None:
        resolved_core_dir = Path(pio_core_dir)
        if not resolved_core_dir.is_absolute():
            resolved_core_dir = root / resolved_core_dir
        resolved_core_dir = resolved_core_dir.resolve()
        environment["PLATFORMIO_CORE_DIR"] = str(resolved_core_dir)
    return NativeCoveragePlan(
        repo_root=root,
        output_dir=output,
        revision=revision,
        worktree_fingerprint=worktree_fingerprint,
        revision_key=revision_key,
        backend=backend,
        requested_backend=requested_backend or backend,
        suite_filter=suite_filter,
        build_dir=build_dir,
        profile_dir=profile_dir,
        merged_profile=profile_dir / "coverage.profdata",
        lcov_path=output / "coverage.info",
        json_path=output / "coverage.json",
        appended_build_flags=flags,
        appended_link_flags=link_flags,
        build_environment=environment,
        pio_core_dir=resolved_core_dir,
        suite_manifest=_native_suite_manifest(root, suite_filter),
    )


def plan_flutter_coverage(
    *,
    repo_root: Path | str,
    output_dir: Path | str,
    revision: str | None = None,
    worktree_fingerprint: str | None = None,
) -> FlutterCoveragePlan:
    """Create the Flutter test/coverage plan without invoking Flutter."""

    root = Path(repo_root).resolve()
    output = _resolve_output(root, output_dir)
    return FlutterCoveragePlan(
        repo_root=root,
        app_root=root / "app",
        output_dir=output,
        lcov_path=output / "coverage.info",
        json_path=output / "coverage.json",
        test_events_path=output / "test-events.raw.jsonl",
        revision=revision,
        worktree_fingerprint=worktree_fingerprint,
    )


def run_native_coverage(
    *,
    repo_root: Path | str,
    output_dir: Path | str,
    revision: str,
    worktree_fingerprint: str | None = None,
    suite_filter: str = DEFAULT_NATIVE_FILTER,
    backend: str = "auto",
    pio_core_dir: Path | str | None = None,
    executor: CommandExecutor | None = None,
    resolver: ToolResolver | None = None,
    inherited_environment: Mapping[str, str] | None = None,
) -> CoverageRunResult:
    """Build and execute instrumented native coverage, preferring LLVM.

    A gcovr/GCC backend is selected only when the complete LLVM toolchain is
    unavailable and gcovr, gcc, g++, and gcov are all locally available.
    The runner never invokes an installer itself. PlatformIO can still resolve
    missing declared packages unless the calling environment pre-provisions
    them; that package-manager behavior is recorded as a build failure if it
    cannot complete.
    """

    if backend not in {"auto", "llvm", "gcovr"}:
        raise ValueError("native coverage backend must be auto, llvm, or gcovr")
    root = Path(repo_root).resolve()
    output = _resolve_output(root, output_dir)
    output.mkdir(parents=True, exist_ok=True)
    _remove_generated_files(
        output,
        ("coverage.info", "coverage.json", "run.json"),
    )
    run = executor or _default_executor
    which = resolver or shutil.which
    inherited = dict(os.environ if inherited_environment is None else inherited_environment)
    commands: list[RecordedCommand] = []
    tools: list[ToolProbe] = []
    diagnostics: list[str] = []
    probe_environment = {"PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no"}
    resolved_core_dir: Path | None = None
    if pio_core_dir is not None:
        resolved_core_dir = Path(pio_core_dir)
        if not resolved_core_dir.is_absolute():
            resolved_core_dir = root / resolved_core_dir
        resolved_core_dir = resolved_core_dir.resolve()
        probe_environment["PLATFORMIO_CORE_DIR"] = str(resolved_core_dir)

    pio = _probe_tool(
        "pio", run, which, root, commands, environment=probe_environment
    )
    tools.append(pio)
    if not pio.available:
        return _unavailable_native(
            root,
            output,
            revision,
            worktree_fingerprint,
            suite_filter,
            backend,
            tools,
            commands,
            [pio.diagnostic or "pio unavailable"],
        )

    clang = ToolProbe("clang", False, None, None, "clang probe not requested")
    clangxx = ToolProbe("clang++", False, None, None, "clang++ probe not requested")
    llvm_profdata = ToolProbe(
        "llvm-profdata", False, None, None, "llvm-profdata probe not requested"
    )
    llvm_cov = ToolProbe("llvm-cov", False, None, None, "llvm-cov probe not requested")
    llvm_complete = False
    if backend in {"auto", "llvm"}:
        clang = _probe_tool("clang", run, which, root, commands)
        clangxx = _probe_tool("clang++", run, which, root, commands)
        llvm_profdata = _probe_tool(
            "llvm-profdata", run, which, root, commands, allow_xcrun=True
        )
        llvm_cov = _probe_tool(
            "llvm-cov", run, which, root, commands, allow_xcrun=True
        )
        tools.extend((clang, clangxx, llvm_profdata, llvm_cov))
        llvm_complete = all(
            tool.available for tool in (clang, clangxx, llvm_profdata, llvm_cov)
        )

    gcc = ToolProbe("gcc", False, None, None, "gcc probe not requested")
    gxx = ToolProbe("g++", False, None, None, "g++ probe not requested")
    gcov = ToolProbe("gcov", False, None, None, "gcov probe not requested")
    gcovr = ToolProbe("gcovr", False, None, None, "gcovr probe not requested")
    gcc_complete = False
    if backend == "gcovr" or (backend == "auto" and not llvm_complete):
        gcc = _probe_tool("gcc", run, which, root, commands)
        gxx = _probe_tool("g++", run, which, root, commands)
        gcov = _probe_tool("gcov", run, which, root, commands)
        gcovr = _probe_tool("gcovr", run, which, root, commands)
        tools.extend((gcc, gxx, gcov, gcovr))
        gcc_complete = all(tool.available for tool in (gcc, gxx, gcov, gcovr))

    requested_backend = backend
    if backend in {"auto", "llvm"} and llvm_complete:
        selected_backend = "llvm"
        cc = clang.path or "clang"
        cxx = clangxx.path or "clang++"
    elif backend in {"auto", "gcovr"} and gcc_complete:
        selected_backend = "gcovr"
        cc = gcc.path or "gcc"
        cxx = gxx.path or "g++"
    else:
        if backend == "llvm":
            diagnostics.append(
                "native coverage unavailable: forced LLVM backend is incomplete; missing "
                + ", ".join(
                    sorted(
                        tool.name
                        for tool in (clang, clangxx, llvm_profdata, llvm_cov)
                        if not tool.available
                    )
                )
            )
        elif backend == "gcovr":
            diagnostics.append(
                "native coverage unavailable: forced gcovr backend is incomplete; missing "
                + ", ".join(
                    sorted(
                        tool.name
                        for tool in (gcc, gxx, gcov, gcovr)
                        if not tool.available
                    )
                )
            )
        else:
            missing = sorted(
                tool.name
                for tool in (clang, clangxx, llvm_profdata, llvm_cov, gcc, gxx, gcov, gcovr)
                if not tool.available
            )
            diagnostics.append(
                "native coverage unavailable: neither a complete clang/LLVM "
                "toolchain nor a complete GCC/gcovr fallback is installed; missing "
                + ", ".join(missing)
            )
        return _unavailable_native(
            root,
            output,
            revision,
            worktree_fingerprint,
            suite_filter,
            requested_backend,
            tools,
            commands,
            diagnostics,
        )

    plan = plan_native_coverage(
        repo_root=root,
        output_dir=output,
        revision=revision,
        worktree_fingerprint=worktree_fingerprint,
        backend=selected_backend,
        requested_backend=requested_backend,
        suite_filter=suite_filter,
        cc=cc,
        cxx=cxx,
        existing_build_flags=inherited.get("PLATFORMIO_BUILD_FLAGS", ""),
        existing_extra_scripts=inherited.get("PLATFORMIO_EXTRA_SCRIPTS", ""),
        pio_core_dir=resolved_core_dir,
    )
    plan.output_dir.mkdir(parents=True, exist_ok=True)
    plan.profile_dir.mkdir(parents=True, exist_ok=True)
    _remove_generated_profiles(plan.profile_dir)

    build_argv = (pio.path or "pio",) + plan.build_command[1:]
    build = _invoke(
        "native-build",
        build_argv,
        root,
        plan.build_environment,
        run,
        root,
        commands,
    )
    if (
        build.timed_out
        or build.returncode != 0
        or build.stdout_truncated
        or build.stderr_truncated
    ):
        diagnostics.append(_failure_diagnostic("native build", build, root))
        if build.stdout_truncated or build.stderr_truncated:
            diagnostics.append(
                f"native build output exceeded the {MAX_CAPTURE_BYTES}-byte capture cap"
            )
        return _native_result(plan, "failed", tools, commands, diagnostics)

    try:
        program = _find_native_program(plan)
    except FileNotFoundError as exc:
        diagnostics.append(str(exc))
        return _native_result(plan, "failed", tools, commands, diagnostics)

    if selected_backend == "llvm":
        profile_pattern = plan.profile_dir / "%p.profraw"
        program_environment = {"LLVM_PROFILE_FILE": str(profile_pattern)}
    else:
        program_environment = {}
    execution = _invoke(
        "native-test",
        (str(program),),
        root,
        program_environment,
        run,
        root,
        commands,
    )
    execution_failed = (
        execution.timed_out
        or execution.returncode != 0
        or execution.stdout_truncated
        or execution.stderr_truncated
    )
    if execution.timed_out or execution.returncode != 0:
        diagnostics.append(_failure_diagnostic("native test program", execution, root))
    if execution.stdout_truncated or execution.stderr_truncated:
        diagnostics.append(
            f"native test output exceeded the {MAX_CAPTURE_BYTES}-byte capture cap"
        )

    if selected_backend == "llvm":
        raw_profiles = tuple(sorted(plan.profile_dir.glob("*.profraw")))
        if not raw_profiles:
            diagnostics.append(
                "native coverage failed: instrumented test emitted no .profraw files"
            )
            return _native_result(plan, "failed", tools, commands, diagnostics)
        merge_argv = (
            llvm_profdata.path or "llvm-profdata",
            "merge",
            "-sparse",
            *(str(path) for path in raw_profiles),
            "-o",
            str(plan.merged_profile),
        )
        merge = _invoke(
            "native-profile-merge",
            merge_argv,
            root,
            {},
            run,
            root,
            commands,
        )
        if merge.timed_out or merge.returncode != 0:
            diagnostics.append(_failure_diagnostic("llvm-profdata merge", merge, root))
            return _native_result(plan, "failed", tools, commands, diagnostics)

        export_argv = (
            llvm_cov.path or "llvm-cov",
            "export",
            str(program),
            f"-instr-profile={plan.merged_profile}",
            "-format=lcov",
        )
        exported = _invoke(
            "native-coverage-export",
            export_argv,
            root,
            {},
            run,
            root,
            commands,
        )
        if (
            exported.timed_out
            or exported.returncode != 0
            or exported.stdout_truncated
            or exported.stderr_truncated
            or not exported.stdout.strip()
        ):
            if exported.stdout_truncated:
                diagnostics.append(
                    f"llvm-cov export exceeded the {MAX_CAPTURE_BYTES}-byte stdout cap"
                )
            diagnostics.append(_failure_diagnostic("llvm-cov export", exported, root))
            return _native_result(plan, "failed", tools, commands, diagnostics)
        plan.lcov_path.write_text(exported.stdout, encoding="utf-8")
    else:
        gcda_files = tuple(sorted(plan.build_dir.rglob("*.gcda")))
        if not gcda_files:
            diagnostics.append(
                "native coverage failed: GCC-instrumented test emitted no .gcda files"
            )
            return _native_result(plan, "failed", tools, commands, diagnostics)
        gcov = next(tool for tool in tools if tool.name == "gcov")
        gcovr = next(tool for tool in tools if tool.name == "gcovr")
        gcovr_argv = (
            gcovr.path or "gcovr",
            "--root",
            ".",
            "--object-directory",
            str(plan.build_dir / "native"),
            "--gcov-executable",
            gcov.path or "gcov",
            "--lcov",
            "--output",
            str(plan.lcov_path),
        )
        generated = _invoke(
            "native-coverage-export",
            gcovr_argv,
            root,
            {},
            run,
            root,
            commands,
        )
        if (
            generated.timed_out
            or generated.returncode != 0
            or generated.stdout_truncated
            or generated.stderr_truncated
            or not plan.lcov_path.is_file()
        ):
            diagnostics.append(_failure_diagnostic("gcovr export", generated, root))
            return _native_result(plan, "failed", tools, commands, diagnostics)

    try:
        report = parse_lcov(
            _read_lcov(plan.lcov_path),
            repo_root=root,
            source_scopes=NATIVE_SOURCE_SCOPES,
        )
    except (OSError, CoverageParseError) as exc:
        diagnostics.append(f"native LCOV parse failed: {_scrub(str(exc), root)}")
        return _native_result(plan, "failed", tools, commands, diagnostics)

    plan.lcov_path.write_text(render_lcov(report), encoding="utf-8")
    _write_coverage_json(plan.json_path, report)
    if not report.summary.lines.available:
        diagnostics.append("native coverage contains no in-scope line records")
    return _native_result(
        plan,
        "failed" if execution_failed else "ok",
        tools,
        commands,
        diagnostics,
        report=report,
    )


def run_flutter_coverage(
    *,
    repo_root: Path | str,
    output_dir: Path | str,
    revision: str | None = None,
    worktree_fingerprint: str | None = None,
    executor: CommandExecutor | None = None,
    resolver: ToolResolver | None = None,
) -> CoverageRunResult:
    """Run Flutter branch/line coverage and normalize its LCOV report."""

    root = Path(repo_root).resolve()
    plan = plan_flutter_coverage(
        repo_root=root,
        output_dir=output_dir,
        revision=revision,
        worktree_fingerprint=worktree_fingerprint,
    )
    plan.output_dir.mkdir(parents=True, exist_ok=True)
    _remove_generated_files(
        plan.output_dir,
        ("coverage.info", "coverage.json", "run.json", "test-events.raw.jsonl"),
    )
    run = executor or _default_executor
    which = resolver or shutil.which
    commands: list[RecordedCommand] = []
    diagnostics: list[str] = []
    flutter_environment = {
        "CI": "true",
        "FLUTTER_SUPPRESS_ANALYTICS": "true",
    }
    flutter = _probe_tool(
        "flutter",
        run,
        which,
        root,
        commands,
        environment=flutter_environment,
    )
    if not flutter.available:
        return _flutter_result(
            plan,
            "unavailable",
            flutter,
            commands,
            (flutter.diagnostic or "flutter unavailable",),
        )

    plan.output_dir.mkdir(parents=True, exist_ok=True)
    command = (flutter.path or "flutter",) + plan.test_command[1:]
    completed = _invoke(
        "flutter-test",
        command,
        plan.app_root,
        flutter_environment,
        run,
        root,
        commands,
    )
    plan.test_events_path.write_text(completed.stdout, encoding="utf-8")
    artifacts = {
        "raw_test_events": _relative_display(plan.test_events_path, root),
    }
    execution_failed = (
        completed.timed_out
        or completed.returncode != 0
        or completed.stdout_truncated
        or completed.stderr_truncated
    )
    if completed.stdout_truncated or completed.stderr_truncated:
        diagnostics.append(
            f"flutter test output exceeded the {MAX_CAPTURE_BYTES}-byte capture cap; "
            "the JSON event stream is incomplete"
        )
    if completed.timed_out or completed.returncode != 0:
        diagnostics.append(_failure_diagnostic("flutter test", completed, root))
    if not plan.lcov_path.is_file():
        diagnostics.append(
            "flutter coverage failed: flutter test did not create coverage.info"
        )
        return _flutter_result(
            plan, "failed", flutter, commands, diagnostics, artifacts=artifacts
        )

    try:
        report = parse_lcov(
            _read_lcov(plan.lcov_path),
            repo_root=root,
            source_base=plan.app_root,
            source_scopes=FLUTTER_SOURCE_SCOPES,
        )
    except (OSError, CoverageParseError) as exc:
        diagnostics.append(f"Flutter LCOV parse failed: {_scrub(str(exc), root)}")
        return _flutter_result(
            plan, "failed", flutter, commands, diagnostics, artifacts=artifacts
        )

    plan.lcov_path.write_text(render_lcov(report), encoding="utf-8")
    _write_coverage_json(plan.json_path, report)
    artifacts.update(
        {
            "lcov": _relative_display(plan.lcov_path, root),
            "coverage_json": _relative_display(plan.json_path, root),
        }
    )
    if not report.summary.branches.available:
        diagnostics.append(
            "Flutter branch coverage unavailable: LCOV contains no BRDA records"
        )
    return _flutter_result(
        plan,
        "failed" if execution_failed else "ok",
        flutter,
        commands,
        diagnostics,
        artifacts=artifacts,
        report=report,
    )


def _probe_tool(
    name: str,
    executor: CommandExecutor,
    resolver: ToolResolver,
    repo_root: Path,
    commands: list[RecordedCommand],
    *,
    allow_xcrun: bool = False,
    environment: Mapping[str, str] | None = None,
) -> ToolProbe:
    path = resolver(name)
    diagnostic: str | None = None
    if path is None and allow_xcrun:
        xcrun = resolver("xcrun")
        if xcrun is not None:
            found = _invoke(
                f"probe-{name}",
                (xcrun, "--find", name),
                repo_root,
                {},
                executor,
                repo_root,
                commands,
            )
            candidate = found.stdout.strip().splitlines()[0] if found.stdout.strip() else ""
            if (
                not found.timed_out
                and not found.stdout_truncated
                and not found.stderr_truncated
                and found.returncode == 0
                and candidate
            ):
                path = candidate
            else:
                diagnostic = _failure_diagnostic(f"xcrun --find {name}", found, repo_root)
    if path is None:
        return ToolProbe(
            name=name,
            available=False,
            path=None,
            version=None,
            diagnostic=diagnostic or f"{name} not found on PATH",
        )

    version_result = _invoke(
        f"probe-{name}-version",
        (path, "--version"),
        repo_root,
        environment or {},
        executor,
        repo_root,
        commands,
    )
    version = _first_nonempty_line(version_result.stdout, version_result.stderr)
    if version_result.timed_out:
        diagnostic = f"{name} version probe timed out"
    elif version_result.stdout_truncated or version_result.stderr_truncated:
        diagnostic = (
            f"{name} version probe exceeded the {MAX_CAPTURE_BYTES}-byte capture cap"
        )
    elif version_result.returncode != 0:
        diagnostic = _failure_diagnostic(f"{name} --version", version_result, repo_root)
    elif version is None:
        diagnostic = f"{name} version probe returned no version text"
    return ToolProbe(
        name=name,
        available=(
            not version_result.timed_out
            and not version_result.stdout_truncated
            and not version_result.stderr_truncated
            and version_result.returncode == 0
            and version is not None
        ),
        path=path,
        version=version,
        diagnostic=diagnostic,
    )


def _invoke(
    phase: str,
    argv: Sequence[str],
    cwd: Path,
    environment: Mapping[str, str],
    executor: CommandExecutor,
    repo_root: Path,
    commands: list[RecordedCommand],
) -> CommandOutcome:
    try:
        completed = executor(tuple(argv), cwd, dict(environment))
    except OSError as exc:
        outcome = CommandOutcome(
            args=tuple(argv),
            returncode=127,
            stdout="",
            stderr=str(exc),
            timed_out=False,
        )
    else:
        stdout, stdout_limited = _bounded_text(getattr(completed, "stdout", ""))
        stderr, stderr_limited = _bounded_text(getattr(completed, "stderr", ""))
        outcome = CommandOutcome(
            args=tuple(str(item) for item in getattr(completed, "args", argv)),
            returncode=int(getattr(completed, "returncode")),
            stdout=stdout,
            stderr=stderr,
            stdout_truncated=(
                bool(getattr(completed, "stdout_truncated", False)) or stdout_limited
            ),
            stderr_truncated=(
                bool(getattr(completed, "stderr_truncated", False)) or stderr_limited
            ),
            timed_out=bool(getattr(completed, "timed_out", False)),
        )
    commands.append(
        _record_command(
            phase,
            argv,
            cwd,
            environment,
            repo_root,
            returncode=outcome.returncode,
            stdout_truncated=outcome.stdout_truncated,
            stderr_truncated=outcome.stderr_truncated,
            timed_out=outcome.timed_out,
        )
    )
    return outcome


def _default_executor(
    argv: Sequence[str], cwd: Path, environment: Mapping[str, str]
) -> CommandOutcome:
    merged_environment = os.environ.copy()
    merged_environment.update(environment)
    process = subprocess.Popen(
        list(argv),
        cwd=cwd,
        env=merged_environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    assert process.stdout is not None
    assert process.stderr is not None
    stdout_capture = _BoundedCapture(MAX_CAPTURE_BYTES)
    stderr_capture = _BoundedCapture(MAX_CAPTURE_BYTES)
    stdout_thread = threading.Thread(
        target=stdout_capture.drain,
        args=(process.stdout,),
        name="coverage-stdout",
        daemon=True,
    )
    stderr_thread = threading.Thread(
        target=stderr_capture.drain,
        args=(process.stderr,),
        name="coverage-stderr",
        daemon=True,
    )
    stdout_thread.start()
    stderr_thread.start()
    timed_out = False
    try:
        returncode = process.wait(timeout=MAX_COMMAND_SECONDS)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            returncode = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                returncode = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                returncode = 124
    stdout_thread.join(timeout=PIPE_DRAIN_JOIN_SECONDS)
    stderr_thread.join(timeout=PIPE_DRAIN_JOIN_SECONDS)
    stdout_drain_incomplete = stdout_thread.is_alive()
    stderr_drain_incomplete = stderr_thread.is_alive()
    if stdout_drain_incomplete:
        stdout_capture.truncated = True
    if stderr_drain_incomplete:
        stderr_capture.truncated = True
    if (stdout_drain_incomplete or stderr_drain_incomplete) and returncode == 0:
        returncode = 125
    return CommandOutcome(
        args=tuple(str(item) for item in argv),
        returncode=returncode,
        stdout=stdout_capture.text(),
        stderr=stderr_capture.text(),
        stdout_truncated=stdout_capture.truncated,
        stderr_truncated=stderr_capture.truncated,
        timed_out=timed_out,
    )


def _record_command(
    phase: str,
    argv: Sequence[str],
    cwd: Path,
    environment: Mapping[str, str],
    repo_root: Path,
    *,
    returncode: int,
    stdout_truncated: bool = False,
    stderr_truncated: bool = False,
    timed_out: bool = False,
) -> RecordedCommand:
    display_argv: list[str] = []
    for index, argument in enumerate(argv):
        value = _scrub_argument(str(argument), repo_root)
        if Path(value).is_absolute():
            value = Path(value).name
        display_argv.append(value)
    display_environment = tuple(
        sorted(
            (
                key,
                _scrub_environment_value(key, str(value), repo_root),
            )
            for key, value in environment.items()
        )
    )
    return RecordedCommand(
        phase=phase,
        argv=tuple(display_argv),
        cwd=_relative_display(cwd, repo_root),
        environment=display_environment,
        returncode=returncode,
        stdout_truncated=stdout_truncated,
        stderr_truncated=stderr_truncated,
        timed_out=timed_out,
    )


def _scrub_argument(value: str, repo_root: Path) -> str:
    root_text = str(repo_root)
    if value == root_text:
        return "."
    if value.startswith(root_text + os.sep):
        return Path(value).relative_to(repo_root).as_posix()
    marker = "=" + root_text + os.sep
    if marker in value:
        return value.replace(marker, "=", 1)
    return value


def _scrub_environment_value(key: str, value: str, repo_root: Path) -> str:
    scrubbed = value.replace(str(repo_root), ".")
    if key in {
        "CC",
        "CXX",
        "CODE_HEALTH_CC",
        "CODE_HEALTH_CXX",
        "GCOV",
        "PLATFORMIO_CORE_DIR",
    } and Path(
        scrubbed
    ).is_absolute():
        return Path(scrubbed).name
    return scrubbed


def _find_native_program(plan: NativeCoveragePlan) -> Path:
    direct = [candidate for candidate in plan.program_candidates() if candidate.is_file()]
    if len(direct) == 1:
        return direct[0]
    discovered = sorted(
        {
            path
            for pattern in ("program", "program.exe")
            for path in plan.build_dir.rglob(pattern)
            if path.is_file()
        },
        key=lambda path: (len(path.parts), path.as_posix()),
    )
    if len(discovered) == 1:
        return discovered[0]
    if not discovered:
        raise FileNotFoundError(
            "native coverage failed: PlatformIO build produced no native test program"
        )
    raise FileNotFoundError(
        "native coverage failed: PlatformIO build produced multiple candidate test programs"
    )


def _native_suite_manifest(repo_root: Path, suite_filter: str) -> dict[str, object]:
    """Mirror the aggregate generators' bounded, deterministic suite selection."""

    if suite_filter != DEFAULT_NATIVE_FILTER:
        return {
            "status": "explicit_filter",
            "filter": suite_filter,
            "aggregates": [],
        }
    aggregate_specs = (
        {
            "name": "test_all_native",
            "source_root": repo_root / "test",
            "aggregate_dir_name": "test_all_native",
            "generator": repo_root / "tools/pio/generate_test_all_native.py",
            "entrypoint": repo_root / "test/test_all_native/test_main.cpp",
            "exclude_long_manual": True,
        },
        {
            "name": "test_all_native_foxie",
            "source_root": repo_root / "lib/foxie_embedded/tests",
            "aggregate_dir_name": "test_all_native",
            "generator": repo_root / "tools/pio/generate_test_all_native_foxie.py",
            "entrypoint": repo_root / "test/test_all_native_foxie/test_main.cpp",
            "exclude_long_manual": False,
        },
    )
    aggregates: list[dict[str, object]] = []
    for spec in aggregate_specs:
        source_root = Path(spec["source_root"])
        included: list[str] = []
        excluded: list[dict[str, str]] = []
        if source_root.is_dir():
            entries = sorted(
                (entry for entry in source_root.iterdir() if entry.is_dir()),
                key=lambda entry: entry.name,
            )
            for entry in entries:
                name = entry.name
                if not name.startswith("test_") or name == spec["aggregate_dir_name"]:
                    continue
                reason: str | None = None
                if bool(spec["exclude_long_manual"]) and name.endswith("_long"):
                    reason = "name_ends_with__long"
                elif bool(spec["exclude_long_manual"]) and name.endswith("_manual"):
                    reason = "name_ends_with__manual"
                elif (entry / "ALL_NATIVE_OPT_IN").exists():
                    reason = "ALL_NATIVE_OPT_IN_marker"
                elif not (entry / "test_main.cpp").is_file():
                    reason = "missing_test_main.cpp"
                if reason is None:
                    included.append(name)
                else:
                    excluded.append({"suite": name, "reason": reason})
        aggregates.append(
            {
                "name": str(spec["name"]),
                "status": "measured" if source_root.is_dir() else "unavailable",
                "source_root": _relative_display(source_root, repo_root),
                "included_suites": included,
                "included_count": len(included),
                "excluded_suites": excluded,
                "generator": _file_identity(Path(spec["generator"]), repo_root),
                "entrypoint": _file_identity(Path(spec["entrypoint"]), repo_root),
            }
        )
    return {
        "status": (
            "measured"
            if all(item["status"] == "measured" for item in aggregates)
            else "incomplete"
        ),
        "filter": suite_filter,
        "aggregates": aggregates,
    }


def _file_identity(path: Path, repo_root: Path) -> dict[str, object]:
    if not path.is_file():
        return {
            "path": _relative_display(path, repo_root),
            "sha256": None,
            "status": "unavailable",
        }
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "path": _relative_display(path, repo_root),
        "sha256": digest.hexdigest(),
        "status": "measured",
    }


def _native_result(
    plan: NativeCoveragePlan,
    status: str,
    tools: Sequence[ToolProbe],
    commands: Sequence[RecordedCommand],
    diagnostics: Sequence[str],
    *,
    report: CoverageReport | None = None,
) -> CoverageRunResult:
    artifacts: dict[str, str] = {}
    if plan.lcov_path.is_file():
        artifacts["lcov"] = _relative_display(plan.lcov_path, plan.repo_root)
    if plan.json_path.is_file():
        artifacts["coverage_json"] = _relative_display(plan.json_path, plan.repo_root)
    if plan.merged_profile.is_file():
        artifacts["merged_profile"] = _relative_display(
            plan.merged_profile, plan.repo_root
        )
    result = CoverageRunResult(
        kind="native",
        status=status,
        backend=plan.backend,
        selection=plan.to_dict(),
        tools=tuple(tools),
        commands=tuple(commands),
        diagnostics=tuple(diagnostics),
        artifacts=artifacts,
        report=report,
    )
    return _persist_result(result, plan.output_dir / "run.json", plan.repo_root)


def _flutter_result(
    plan: FlutterCoveragePlan,
    status: str,
    flutter: ToolProbe,
    commands: Sequence[RecordedCommand],
    diagnostics: Sequence[str],
    *,
    artifacts: Mapping[str, str] | None = None,
    report: CoverageReport | None = None,
) -> CoverageRunResult:
    result = CoverageRunResult(
        kind="flutter",
        status=status,
        backend=None if status == "unavailable" else "flutter",
        selection=plan.to_dict(),
        tools=(flutter,),
        commands=tuple(commands),
        diagnostics=tuple(diagnostics),
        artifacts={} if artifacts is None else dict(artifacts),
        report=report,
    )
    return _persist_result(result, plan.output_dir / "run.json", plan.repo_root)


def _unavailable_native(
    repo_root: Path,
    output_dir: Path,
    revision: str,
    worktree_fingerprint: str | None,
    suite_filter: str,
    requested_backend: str,
    tools: Sequence[ToolProbe],
    commands: Sequence[RecordedCommand],
    diagnostics: Sequence[str],
) -> CoverageRunResult:
    result = CoverageRunResult(
        kind="native",
        status="unavailable",
        backend=None,
        selection={
            "revision": revision,
            "worktree_fingerprint": worktree_fingerprint,
            "requested_backend": requested_backend,
            "suite_filter": suite_filter,
            "aggregate_exclusions": list(NATIVE_AGGREGATE_EXCLUSIONS),
            "includes_foxie_aggregate": suite_filter == DEFAULT_NATIVE_FILTER,
        },
        tools=tuple(tools),
        commands=tuple(commands),
        diagnostics=tuple(diagnostics),
        artifacts={},
    )
    return _persist_result(result, output_dir / "run.json", repo_root)


def _write_coverage_json(path: Path, report: CoverageReport) -> None:
    path.write_text(
        json.dumps(report.to_dict(), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _persist_result(
    result: CoverageRunResult, path: Path, repo_root: Path
) -> CoverageRunResult:
    path.parent.mkdir(parents=True, exist_ok=True)
    artifacts = dict(result.artifacts)
    artifacts["run_json"] = _relative_display(path, repo_root)
    persisted = dataclasses.replace(result, artifacts=artifacts)
    path.write_text(
        json.dumps(persisted.to_dict(), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return persisted


def _remove_generated_profiles(profile_dir: Path) -> None:
    for pattern in ("*.profraw", "*.profdata"):
        for path in profile_dir.glob(pattern):
            if path.is_file():
                path.unlink()


def _remove_generated_files(directory: Path, names: Sequence[str]) -> None:
    for name in names:
        path = directory / name
        if path.is_file():
            path.unlink()


def _failure_diagnostic(
    label: str, completed: CommandOutcome, repo_root: Path
) -> str:
    if completed.timed_out:
        return f"{label} timed out"
    detail = _most_useful_failure_line(completed.stderr, completed.stdout) or "no output"
    return (
        f"{label} failed with exit {completed.returncode}: "
        f"{_scrub(detail, repo_root)}"
    )[:MAX_DIAGNOSTIC_CHARS]


def _most_useful_failure_line(*values: object) -> str | None:
    lines = [
        line.strip()
        for value in values
        for line in _as_text(value).splitlines()
        if line.strip()
    ]
    for pattern in (
        r"\[ALL_NATIVE\]\s+FAIL:",
        r"\[FAIL\]",
        r"\bfailures=[1-9][0-9]*\b",
        r"\bfatal error\b",
        r"\berror:",
        r"undefined (?:reference|symbol)",
        r"\[ERRORED\]",
        r"\bfailed\b",
    ):
        match = next((line for line in lines if re.search(pattern, line, re.I)), None)
        if match is not None:
            return match
    return lines[0] if lines else None


def _read_lcov(path: Path) -> str:
    size = path.stat().st_size
    if size > MAX_LCOV_BYTES:
        raise CoverageParseError(
            f"LCOV artifact exceeds the {MAX_LCOV_BYTES}-byte read limit"
        )
    return path.read_text(encoding="utf-8")


def _first_nonempty_line(*values: object) -> str | None:
    for value in values:
        text = _as_text(value)
        for line in text.splitlines():
            if line.strip():
                return line.strip()
    return None


def _as_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def _bounded_text(value: object) -> tuple[str, bool]:
    if value is None:
        return "", False
    if isinstance(value, bytes):
        encoded = value
    else:
        encoded = str(value).encode("utf-8", errors="replace")
    if len(encoded) <= MAX_CAPTURE_BYTES:
        return encoded.decode("utf-8", errors="replace"), False
    return (
        encoded[:MAX_CAPTURE_BYTES].decode("utf-8", errors="replace"),
        True,
    )


def _scrub(value: str, repo_root: Path) -> str:
    return value.replace(str(repo_root), ".")


def _relative_display(path: Path, repo_root: Path) -> str:
    try:
        relative = path.resolve().relative_to(repo_root.resolve())
    except ValueError:
        return Path(path).name
    text = relative.as_posix()
    return "." if text in ("", ".") else text


def _resolve_output(repo_root: Path, output_dir: Path | str) -> Path:
    output = Path(output_dir)
    if not output.is_absolute():
        output = repo_root / output
    output = output.resolve()
    try:
        output.relative_to(repo_root)
    except ValueError as exc:
        raise ValueError("coverage output directory must be inside the repository") from exc
    return output


def _revision_key(
    revision: str,
    backend: str,
    worktree_fingerprint: str | None = None,
) -> str:
    label = re.sub(r"[^A-Za-z0-9._-]+", "-", revision.strip()).strip("-._")
    if not label:
        label = "revision"
    identity = revision
    dirty_label = ""
    if worktree_fingerprint:
        identity += f"\0{worktree_fingerprint}"
        dirty_label = f"-dirty-{worktree_fingerprint[:12]}"
    digest = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:12]
    return f"{label[:32]}{dirty_label}-{digest}-{backend}"
