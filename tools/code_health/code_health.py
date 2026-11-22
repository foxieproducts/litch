#!/usr/bin/env python3
"""Top-level deterministic code-health reporting command family."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import re
import sys
from pathlib import Path
from typing import Any, Sequence

from common import (
    CodeHealthError,
    expand_repo_paths,
    find_repo_root,
    normalize_explicit_paths,
    normalized_repo_path,
    read_json_bounded,
    read_text_bounded,
    revision_info,
    revision_output_root,
    run_bounded,
    scrub_command,
    scrub_text,
    select_changed_files,
    sha256_file,
    write_json,
    write_text,
)
from crap_report import (
    build_crap_report,
    load_complexity,
    load_lcov_line_hits,
    render_human_summary as render_crap_summary,
)
from firmware_size import (
    SUPPORTED_TARGETS,
    compare_firmware_size_snapshots,
    create_firmware_size_snapshot,
    validate_firmware_size_snapshot,
    validate_measured_linked_capacities,
    validate_repository_size_configuration,
)
from host_contract import UnsupportedHostError, require_supported_host
from static_analysis import (
    CPP_SUFFIXES,
    DART_SUFFIXES,
    render_human_summary as render_analysis_summary,
    run_static_analysis,
)


SCHEMA_VERSION = 1
TARGET_SHORT_NAMES = {
    "s3": "litch-esp32s3",
    "c3": "litch-esp32c3",
}
DEFAULT_TARGET_SHORT_NAMES = {"s3": "litch-esp32s3"}
DEFAULT_RUNTIME_TARGETS = ("s3",)
LEGACY_RUNTIME_TARGETS = ("s3", "c3")


def _repo_root(args: argparse.Namespace) -> Path:
    return find_repo_root(Path(args.repo_root) if args.repo_root else None)


def _output_dir(
    args: argparse.Namespace,
    repo_root: Path,
    lane: str,
    repository: dict[str, Any] | None = None,
) -> Path:
    if not args.output_dir:
        return revision_output_root(repo_root, repository) / lane
    output = Path(args.output_dir)
    resolved = output.resolve() if output.is_absolute() else (repo_root / output).resolve()
    try:
        resolved.relative_to(repo_root.resolve())
    except ValueError as exc:
        raise CodeHealthError("output directory must remain inside the repository") from exc
    return resolved


def _input_path(repo_root: Path, value: str | None) -> Path | None:
    if value is None:
        return None
    path = Path(value)
    return path.resolve() if path.is_absolute() else (repo_root / path).resolve()


def _add_common_output(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--repo-root", help="repository root (auto-detected by default)")
    parser.add_argument("--output-dir", help="override the ignored revision-scoped output directory")


def _add_scope(parser: argparse.ArgumentParser) -> None:
    scope = parser.add_mutually_exclusive_group(required=True)
    scope.add_argument("--changed-from", metavar="REF", help="scope to REF...HEAD changed files")
    scope.add_argument(
        "--path",
        action="append",
        dest="paths",
        metavar="PATH",
        help="explicit repository file/directory scope (repeatable)",
    )


def _scope_files(
    args: argparse.Namespace,
    repo_root: Path,
    *,
    suffixes: set[str] | None = None,
) -> tuple[list[str], list[str], str]:
    if args.changed_from:
        files = select_changed_files(repo_root, args.changed_from, args.max_files)
        if suffixes is not None:
            files = [path for path in files if Path(path).suffix.lower() in suffixes]
        return files, files, "changed"
    selectors = normalize_explicit_paths(repo_root, args.paths or [])
    files = expand_repo_paths(
        repo_root,
        selectors,
        max_files=args.max_files,
        suffixes=suffixes,
    )
    return files, selectors, "explicit"


def _attach_repository(
    report: dict[str, Any],
    repository: dict[str, Any],
) -> dict[str, Any]:
    report["repository"] = repository
    return report


def _assert_repository_state_unchanged(
    repo_root: Path,
    expected: dict[str, Any],
    *,
    invalidate_paths: Sequence[Path] = (),
) -> None:
    observed = revision_info(repo_root)
    if observed != expected:
        for path in invalidate_paths:
            try:
                path.unlink(missing_ok=True)
            except OSError as exc:
                raise CodeHealthError(
                    "repository state changed during measurement and stale artifact "
                    f"invalidation failed: {normalized_repo_path(path, repo_root)}: {exc}"
                ) from exc
        raise CodeHealthError(
            "repository state changed during measurement; discard the artifact and rerun"
        )


def _display_path(path: Path, repo_root: Path) -> str:
    return normalized_repo_path(path, repo_root)


def _input_provenance(path: Path | None, repo_root: Path) -> dict[str, Any] | None:
    if path is None:
        return None
    if not path.is_file():
        return {
            "path": normalized_repo_path(path, repo_root),
            "sha256": None,
            "size_bytes": None,
            "status": "unavailable",
        }
    return {
        "path": normalized_repo_path(path, repo_root),
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
        "status": "measured",
    }


def _version_file_provenance(
    path: Path | None,
    repo_root: Path,
) -> dict[str, Any] | None:
    provenance = _input_provenance(path, repo_root)
    if provenance is None:
        return None
    if provenance["status"] != "measured" or path is None:
        return {**provenance, "version": None}
    text = scrub_text(read_text_bounded(path, max_bytes=16_384), repo_root)
    version = next((line.strip() for line in text.splitlines() if line.strip()), None)
    return {**provenance, "version": version}


def command_crap(args: argparse.Namespace) -> int:
    repo_root = _repo_root(args)
    repository = revision_info(repo_root)
    output_dir = _output_dir(args, repo_root, "crap", repository)
    scope_files, selectors, scope_mode = _scope_files(args, repo_root)
    complexity_paths = [_input_path(repo_root, path) for path in args.complexity]
    complexity_version_path = _input_path(
        repo_root, getattr(args, "complexity_version", None)
    )
    coverage_paths = [_input_path(repo_root, path) for path in args.coverage]
    baseline_path = _input_path(repo_root, args.baseline)
    functions = load_complexity(complexity_paths, repo_root)  # type: ignore[arg-type]
    coverage = load_lcov_line_hits(coverage_paths, repo_root)  # type: ignore[arg-type]
    report = build_crap_report(
        functions=functions,
        line_hits=coverage,
        scope_selectors=selectors,
        scope_files=scope_files,
        baseline_path=baseline_path,
        max_functions=args.max_functions,
        hotspot_limit=args.limit,
    )
    report["scope"]["mode"] = scope_mode
    if args.changed_from:
        report["scope"]["changed_from"] = args.changed_from
    report["inputs"] = {
        "baseline": _input_provenance(baseline_path, repo_root),
        "complexity": [
            _input_provenance(path, repo_root) for path in complexity_paths
        ],
        "complexity_producer_version": _version_file_provenance(
            complexity_version_path, repo_root
        ),
        "coverage": [_input_provenance(path, repo_root) for path in coverage_paths],
    }
    _attach_repository(report, repository)
    summary = render_crap_summary(report)
    _assert_repository_state_unchanged(repo_root, repository)
    write_json(output_dir / "report.json", report)
    write_text(output_dir / "summary.txt", summary)
    print(summary)
    print(f"report: {_display_path(output_dir / 'report.json', repo_root)}")
    return 0


def command_analyze(args: argparse.Namespace) -> int:
    repo_root = _repo_root(args)
    repository = revision_info(repo_root)
    output_dir = _output_dir(args, repo_root, "analysis", repository)
    files, selectors, scope_mode = _scope_files(
        args,
        repo_root,
        suffixes=CPP_SUFFIXES | DART_SUFFIXES,
    )
    evaluation_date = (
        dt.date.fromisoformat(args.evaluation_date) if args.evaluation_date else dt.date.today()
    )
    compile_commands = _input_path(repo_root, args.compile_commands)
    baseline_path = _input_path(repo_root, args.baseline)
    suppressions_path = _input_path(repo_root, args.suppressions)
    report = run_static_analysis(
        repo_root=repo_root,
        files=files,
        requested_tools=args.tool,
        compile_commands=compile_commands,
        baseline=baseline_path,
        suppressions_path=suppressions_path,
        evaluation_date=evaluation_date,
        max_files=args.max_files,
    )
    report["scope"]["mode"] = scope_mode
    report["scope"]["selectors"] = selectors
    if args.changed_from:
        report["scope"]["changed_from"] = args.changed_from
    report["inputs"] = {
        "baseline": _input_provenance(baseline_path, repo_root),
        "compile_commands": _input_provenance(
            compile_commands / "compile_commands.json" if compile_commands else None,
            repo_root,
        ),
        "suppressions": _input_provenance(suppressions_path, repo_root),
    }
    _attach_repository(report, repository)
    summary = render_analysis_summary(report)
    _assert_repository_state_unchanged(repo_root, repository)
    write_json(output_dir / "report.json", report)
    write_text(output_dir / "summary.txt", summary)
    print(summary)
    print(f"report: {_display_path(output_dir / 'report.json', repo_root)}")
    if args.report_only:
        return 0
    if report["summary"]["invocation_errors"] or report["summary"]["incomplete_outputs"]:
        return 1
    if report["summary"]["all_applicable_tools_unavailable"]:
        return 2
    return 1 if report["summary"]["new"] else 0


def _platformio_version(repo_root: Path) -> dict[str, Any]:
    try:
        result = run_bounded(
            ["pio", "--version"],
            cwd=repo_root,
            env={"PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no"},
            max_capture_bytes=16_384,
        )
    except CodeHealthError as exc:
        return {"status": "unavailable", "version": None, "reason": str(exc)}
    output = scrub_text(result.output.strip(), repo_root)
    available = not result.timed_out and not result.truncated and result.returncode == 0
    if result.timed_out:
        reason = "PlatformIO version probe timed out"
    elif result.truncated:
        reason = "PlatformIO version probe exceeded its capture limit"
    elif result.returncode != 0:
        reason = output or f"exit {result.returncode}"
    else:
        reason = None
    return {
        "status": "available" if available else "probe_failed",
        "version": output.splitlines()[0] if output and available else None,
        "reason": reason,
    }


def _run_firmware_builds(
    repo_root: Path,
    output_dir: Path,
    pio_core_dir: Path | None,
    target_short_names: dict[str, str] | None = None,
) -> tuple[dict[str, Path], dict[str, Path | None], list[dict[str, Any]]]:
    build_dir = output_dir / "pio-build"
    logs: dict[str, Path | None] = {}
    artifacts: dict[str, Path] = {}
    results: list[dict[str, Any]] = []
    env = {
        "PLATFORMIO_BUILD_DIR": str(build_dir),
        "PLATFORMIO_SETTING_ENABLE_TELEMETRY": "no",
    }
    if pio_core_dir is not None:
        env["PLATFORMIO_CORE_DIR"] = str(pio_core_dir)
    selected_targets = target_short_names or DEFAULT_TARGET_SHORT_NAMES
    for short_name, target in selected_targets.items():
        artifact_path = build_dir / target / "firmware.bin"
        if artifact_path.is_file():
            artifact_path.unlink()
        command = ["pio", "run", "-e", target]
        result = run_bounded(command, cwd=repo_root, env=env, max_capture_bytes=4_000_000)
        log_path = output_dir / f"{short_name}.platformio.log"
        write_text(log_path, scrub_text(result.output, repo_root))
        failure_reasons: list[str] = []
        if result.returncode != 0:
            failure_reasons.append(f"build command exited {result.returncode}")
        if result.truncated:
            failure_reasons.append("build output exceeded capture limit")
        if result.timed_out:
            failure_reasons.append("build command timed out")
        if not artifact_path.is_file():
            failure_reasons.append("expected firmware.bin was not produced")
        elif not failure_reasons:
            try:
                sha256_file(artifact_path)
            except OSError:
                failure_reasons.append("expected firmware.bin is not readable")
        build_complete = not failure_reasons
        if not build_complete and artifact_path.is_file():
            artifact_path.unlink()
        logs[short_name] = log_path if build_complete else None
        artifacts[short_name] = artifact_path
        results.append(
            {
                "command": scrub_command(command, repo_root),
                "failure_reason": (
                    "; ".join(failure_reasons) if failure_reasons else None
                ),
                "output_truncated": result.truncated,
                "status": "completed" if build_complete else "incomplete",
                "returncode": result.returncode,
                "target": target,
                "timed_out": result.timed_out,
            }
        )
    return artifacts, logs, results


def _safe_output_component(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-._") or "snapshot"
    digest = hashlib.sha256(value.encode("utf-8")).hexdigest()[:8]
    return f"{cleaned[:40]}-{digest}"


def _size_snapshot_identity(
    args: argparse.Namespace,
    repository: dict[str, Any],
) -> tuple[str, str, str]:
    if args.build:
        if args.role != "current":
            raise CodeHealthError("--build can only create a current snapshot")
        if any(
            value
            for value in (args.s3_artifact, args.c3_artifact, args.s3_log, args.c3_log)
        ):
            raise CodeHealthError("--build cannot be combined with imported artifact or log paths")
        if args.revision and args.revision != repository["revision"]:
            raise CodeHealthError("--build revision must match the checked-out HEAD")
        revision = str(repository["revision"])
        revision_provenance = (
            "verified_checked_out_worktree_build"
            if repository.get("dirty")
            else "verified_checked_out_commit_build"
        )
    else:
        if not args.revision:
            raise CodeHealthError("imported artifacts require an explicit --revision")
        if args.role == "baseline" and not args.s3_artifact:
            raise CodeHealthError("a baseline snapshot requires explicit --s3-artifact")
        if (
            args.role == "baseline"
            and getattr(args, "legacy_c3", False)
            and not args.c3_artifact
        ):
            raise CodeHealthError(
                "a legacy C3 baseline snapshot requires explicit --c3-artifact"
            )
        if args.pio_core_dir:
            raise CodeHealthError("--pio-core-dir only applies with --build")
        revision = args.revision
        revision_provenance = "declared_for_imported_artifacts"
    label = args.label or f"{args.role}:{revision[:12]}"
    return revision, revision_provenance, label


def _configuration_file(repo_root: Path, relative_path: str) -> dict[str, Any]:
    path = repo_root / relative_path
    if not path.is_file():
        return {"path": relative_path, "sha256": None, "status": "unavailable"}
    return {"path": relative_path, "sha256": sha256_file(path), "status": "measured"}


def _build_configuration_provenance(
    repo_root: Path,
    revision_provenance: str,
) -> dict[str, Any]:
    paths = (
        "platformio.ini",
        "boards/litch_esp32c3.json",
        "boards/litch_esp32s3.json",
        "tools/partitions/partitions-8MB-ota-spiffs.csv",
    )
    try:
        capacity_validation = validate_repository_size_configuration(repo_root)
    except ValueError as exc:
        raise CodeHealthError(str(exc)) from exc
    return {
        "mode": "fixed_repository_platformio_environments",
        "provenance": (
            "verified_build_checkout"
            if revision_provenance
            in {
                "verified_checked_out_commit_build",
                "verified_checked_out_worktree_build",
            }
            else "reporting_checkout_not_verified_artifact_build"
        ),
        "files": [_configuration_file(repo_root, path) for path in paths],
        "capacity_validation": capacity_validation,
    }


def command_size_snapshot(args: argparse.Namespace) -> int:
    repo_root = _repo_root(args)
    info = revision_info(repo_root)
    revision, revision_provenance, label = _size_snapshot_identity(args, info)
    output_dir = (
        _output_dir(args, repo_root, "size", info)
        if args.output_dir
        else revision_output_root(repo_root, info)
        / "size"
        / "snapshots"
        / f"{args.role}-{_safe_output_component(label)}"
    )
    include_legacy_c3 = getattr(args, "legacy_c3", False)
    if not include_legacy_c3 and (args.c3_artifact or args.c3_log):
        raise CodeHealthError(
            "legacy C3 artifact inputs require the explicit --legacy-c3 option"
        )
    selected_targets = (
        TARGET_SHORT_NAMES if include_legacy_c3 else DEFAULT_TARGET_SHORT_NAMES
    )
    artifacts = {
        short_name: _input_path(repo_root, getattr(args, f"{short_name}_artifact"))
        for short_name in selected_targets
    }
    logs: dict[str, Path | None] = {
        short_name: _input_path(repo_root, getattr(args, f"{short_name}_log"))
        for short_name in selected_targets
    }
    builds: list[dict[str, Any]] = []
    if args.build:
        built_artifacts, built_logs, builds = _run_firmware_builds(
            repo_root,
            output_dir,
            _input_path(repo_root, args.pio_core_dir),
            selected_targets,
        )
        artifacts.update(built_artifacts)
        logs.update(built_logs)
        _assert_repository_state_unchanged(repo_root, info)

    targets: dict[str, Any] = {}
    for short_name, target in selected_targets.items():
        targets[target] = create_firmware_size_snapshot(
            target=target,
            role=args.role,
            artifact_path=artifacts[short_name],
            repository_root=repo_root,
            platformio_log_path=logs[short_name],
            revision=revision,
        )
    if args.build:
        try:
            for snapshot in targets.values():
                validate_measured_linked_capacities(snapshot)
        except ValueError as exc:
            raise CodeHealthError(str(exc)) from exc
    report = {
        "profile": {
            "label": args.profile_label,
            "affects_build": False,
            "note": "descriptive comparison dimension; builds use fixed repository environments",
        },
        "build_configuration": _build_configuration_provenance(
            repo_root,
            revision_provenance,
        ),
        "builds": builds,
        "kind": "firmware_size_bundle",
        "policy_thresholds": None,
        "repository": info,
        "role": args.role,
        "label": label,
        "revision": revision,
        "revision_provenance": revision_provenance,
        "schema_version": SCHEMA_VERSION,
        "targets": targets,
        "tools": {
            "platformio": {
                **_platformio_version(repo_root),
                "provenance": "reporting_host",
            }
        },
    }
    _assert_repository_state_unchanged(repo_root, info)
    write_json(output_dir / "snapshot.json", report)
    lines = [
        f"Firmware size snapshot ({args.role}, label={label}, "
        f"profile_label={args.profile_label})"
    ]
    unavailable = False
    for target in selected_targets.values():
        artifact = targets[target]["artifact"]
        linked = targets[target]["linked_usage"]
        if artifact["size_bytes"] is None:
            unavailable = True
            lines.append(f"{target}: artifact unavailable ({artifact['unavailable_reason']})")
        else:
            lines.append(
                f"{target}: artifact={artifact['size_bytes']} bytes, "
                f"slot_remaining={artifact['application_slot_remaining_bytes']} bytes, "
                f"linked_ram={_usage_text(linked['ram'])}, "
                f"linked_flash={_usage_text(linked['flash'])}"
            )
    lines.append("thresholds: none (per-target trend report only)")
    summary = "\n".join(lines)
    write_text(output_dir / "summary.txt", summary)
    print(summary)
    print(f"report: {_display_path(output_dir / 'snapshot.json', repo_root)}")
    if builds and any(item["status"] != "completed" for item in builds):
        return 1
    return 2 if unavailable and args.require_artifacts else 0


def _validated_bundle_role(payload: dict[str, Any]) -> str:
    if type(payload.get("schema_version")) is not int or payload.get(
        "schema_version"
    ) != SCHEMA_VERSION:
        raise CodeHealthError("firmware_size_bundle has an unsupported schema version")
    role = payload.get("role")
    if role not in {"baseline", "current"}:
        raise CodeHealthError("firmware_size_bundle must declare baseline/current role")
    if not isinstance(payload.get("targets"), dict):
        raise CodeHealthError("firmware_size_bundle targets must be an object")
    return role


def _target_snapshots(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise CodeHealthError("size input must be a JSON object")
    if payload.get("kind") == "firmware_size_bundle" and isinstance(payload.get("targets"), dict):
        _validated_bundle_role(payload)
        targets = payload["targets"]
        if not targets:
            raise CodeHealthError("firmware_size_bundle contains no targets")
        unknown = sorted(set(targets) - set(SUPPORTED_TARGETS))
        if unknown:
            raise CodeHealthError(
                f"firmware_size_bundle contains unsupported targets: {', '.join(unknown)}"
            )
        validated: dict[str, Any] = {}
        for target in SUPPORTED_TARGETS:
            if target not in targets:
                continue
            snapshot = targets[target]
            if not isinstance(snapshot, dict):
                raise CodeHealthError(f"firmware size target {target} must be an object")
            try:
                snapshot_target = validate_firmware_size_snapshot(snapshot)
            except ValueError as exc:
                raise CodeHealthError(f"invalid {target} snapshot: {exc}") from exc
            if snapshot_target != target:
                raise CodeHealthError(
                    f"firmware size bundle key {target} disagrees with snapshot target "
                    f"{snapshot_target}"
                )
            validated[target] = snapshot
        return validated
    if payload.get("kind") == "firmware_size_snapshot" and isinstance(payload.get("target"), str):
        try:
            target = validate_firmware_size_snapshot(payload)
        except ValueError as exc:
            raise CodeHealthError(f"invalid firmware size snapshot: {exc}") from exc
        return {target: payload}
    raise CodeHealthError("size input is neither a firmware_size_bundle nor firmware_size_snapshot")


def _snapshot_role(payload: Any) -> str | None:
    if not isinstance(payload, dict):
        return None
    role = payload.get("role")
    if payload.get("kind") == "firmware_size_bundle" and isinstance(payload.get("targets"), dict):
        role = _validated_bundle_role(payload)
        roles = {
            item.get("role")
            for item in payload["targets"].values()
            if isinstance(item, dict)
        }
        if any(candidate != role for candidate in roles):
            raise CodeHealthError("firmware size bundle role disagrees with a target snapshot")
        return role
    return role if isinstance(role, str) else None


def _usage_text(metric: dict[str, Any]) -> str:
    value = metric.get("used_bytes")
    if value is not None:
        return str(value)
    return f"unavailable ({metric.get('unavailable_reason') or 'not reported'})"


def _delta_text(metric: dict[str, Any]) -> str:
    value = metric.get("delta_bytes")
    if value is not None:
        return str(value)
    return f"unavailable ({metric.get('unavailable_reason') or 'not comparable'})"


def _profile_label(profile: Any) -> str | None:
    if isinstance(profile, dict) and isinstance(profile.get("label"), str):
        return profile["label"]
    return None


def command_size_compare(args: argparse.Namespace) -> int:
    repo_root = _repo_root(args)
    repository = revision_info(repo_root)
    output_dir = _output_dir(args, repo_root, "size", repository)
    baseline_payload = read_json_bounded(_input_path(repo_root, args.baseline))  # type: ignore[arg-type]
    current_payload = read_json_bounded(_input_path(repo_root, args.current))  # type: ignore[arg-type]
    if _snapshot_role(baseline_payload) != "baseline":
        raise CodeHealthError("--baseline input must declare role=baseline")
    if _snapshot_role(current_payload) != "current":
        raise CodeHealthError("--current input must declare role=current")
    baseline = _target_snapshots(baseline_payload)
    current = _target_snapshots(current_payload)
    targets = sorted(set(baseline) | set(current))
    comparisons: dict[str, Any] = {}
    unavailable: list[str] = []
    for target in targets:
        if target not in baseline or target not in current:
            unavailable.append(target)
            comparisons[target] = {
                "kind": "firmware_size_comparison",
                "target": target,
                "status": "unavailable",
                "reason": "target missing from baseline" if target not in baseline else "target missing from current",
            }
            continue
        comparisons[target] = compare_firmware_size_snapshots(baseline[target], current[target])
        useful_deltas = (
            comparisons[target]["deltas"][name]["delta_bytes"]
            for name in (
                "artifact_size_bytes",
                "linked_ram_used_bytes",
                "linked_flash_used_bytes",
            )
        )
        if any(value is not None for value in useful_deltas):
            comparisons[target]["status"] = "measured"
        else:
            comparisons[target]["status"] = "unavailable"
            comparisons[target]["reason"] = "no comparable artifact or linked-use metrics"
            unavailable.append(target)
    report = {
        "comparisons": comparisons,
        "kind": "firmware_size_comparison_bundle",
        "labels": {
            "baseline": baseline_payload.get("label") if isinstance(baseline_payload, dict) else None,
            "current": current_payload.get("label") if isinstance(current_payload, dict) else None,
        },
        "profiles": {
            "baseline": baseline_payload.get("profile")
            if isinstance(baseline_payload, dict)
            else None,
            "current": current_payload.get("profile")
            if isinstance(current_payload, dict)
            else None,
        },
        "policy_thresholds": None,
        "repository": repository,
        "schema_version": SCHEMA_VERSION,
    }
    _assert_repository_state_unchanged(repo_root, repository)
    write_json(output_dir / "comparison.json", report)
    lines = [
        "Firmware size comparison",
        f"baseline: label={report['labels']['baseline']} "
        f"profile_label={_profile_label(report['profiles']['baseline'])}",
        f"current: label={report['labels']['current']} "
        f"profile_label={_profile_label(report['profiles']['current'])}",
    ]
    for target, comparison in comparisons.items():
        if comparison["status"] != "measured":
            lines.append(f"{target}: unavailable ({comparison['reason']})")
            continue
        artifact_delta = comparison["deltas"]["artifact_size_bytes"]
        linked_ram = comparison["deltas"]["linked_ram_used_bytes"]
        linked_flash = comparison["deltas"]["linked_flash_used_bytes"]
        lines.append(
            f"{target}: artifact_delta={_delta_text(artifact_delta)}, "
            f"linked_ram_delta={_delta_text(linked_ram)}, "
            f"linked_flash_delta={_delta_text(linked_flash)} bytes"
        )
    lines.append("thresholds: none (review deltas in context)")
    summary = "\n".join(lines)
    write_text(output_dir / "comparison_summary.txt", summary)
    print(summary)
    print(f"report: {_display_path(output_dir / 'comparison.json', repo_root)}")
    return 2 if unavailable else 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Deterministic, bounded Litch code-health reporting",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    native = subparsers.add_parser("native-coverage", help="run native aggregate with coverage")
    _add_common_output(native)
    native.add_argument("--backend", choices=("auto", "llvm", "gcovr"), default="auto")
    native.add_argument("--pio-core-dir")
    native.set_defaults(handler=command_native_coverage)

    flutter = subparsers.add_parser("flutter-coverage", help="run Flutter tests with LCOV")
    _add_common_output(flutter)
    flutter.set_defaults(handler=command_flutter_coverage)

    crap = subparsers.add_parser("crap", help="calculate explicitly scoped CRAP hotspots")
    _add_common_output(crap)
    _add_scope(crap)
    crap.add_argument("--coverage", action="append", required=True, help="LCOV input (repeatable)")
    crap.add_argument(
        "--complexity",
        action="append",
        required=True,
        help="normalized JSON or lizard CSV complexity input (repeatable)",
    )
    crap.add_argument(
        "--complexity-version",
        help="bounded file containing the complexity producer's version",
    )
    crap.add_argument("--baseline", help="prior CRAP report for deltas")
    crap.add_argument("--limit", type=int, default=25, help="human hotspot count (default: 25)")
    crap.add_argument("--max-files", type=int, default=200)
    crap.add_argument("--max-functions", type=int, default=1000)
    crap.set_defaults(handler=command_crap)

    analyze = subparsers.add_parser("analyze", help="run scoped static analysis")
    _add_common_output(analyze)
    _add_scope(analyze)
    analyze.add_argument(
        "--tool",
        action="append",
        choices=("clang-tidy", "cppcheck", "dart"),
        help="requested tool (repeatable; defaults to all applicable tools)",
    )
    analyze.add_argument("--compile-commands", help="directory containing compile_commands.json")
    analyze.add_argument("--baseline", help="prior analysis report")
    analyze.add_argument("--suppressions", help="owned, expiring suppression JSON")
    analyze.add_argument("--evaluation-date", help="ISO date for deterministic suppression expiry")
    analyze.add_argument("--max-files", type=int, default=50)
    analyze.add_argument("--report-only", action="store_true", help="always exit zero after writing report")
    analyze.set_defaults(handler=command_analyze)

    snapshot = subparsers.add_parser(
        "size-snapshot", help="snapshot firmware binary and linked size"
    )
    _add_common_output(snapshot)
    snapshot.add_argument("--role", choices=("baseline", "current"), default="current")
    snapshot.add_argument(
        "--revision",
        help="required declared revision for imports; --build binds to checked-out HEAD",
    )
    snapshot.add_argument(
        "--profile-label",
        default="repository-default",
        help="descriptive comparison label; does not change the fixed PlatformIO environments",
    )
    snapshot.add_argument("--label", help="feature/baseline label stored with the snapshot")
    snapshot.add_argument("--s3-artifact")
    snapshot.add_argument("--c3-artifact", help=argparse.SUPPRESS)
    snapshot.add_argument("--s3-log")
    snapshot.add_argument("--c3-log", help=argparse.SUPPRESS)
    snapshot.add_argument(
        "--legacy-c3", action="store_true", help=argparse.SUPPRESS
    )
    snapshot.add_argument(
        "--build", action="store_true", help="build S3 before snapshotting"
    )
    snapshot.add_argument("--pio-core-dir")
    snapshot.add_argument("--require-artifacts", action="store_true")
    snapshot.set_defaults(handler=command_size_snapshot)

    compare = subparsers.add_parser("size-compare", help="compare baseline/current size snapshots")
    _add_common_output(compare)
    compare.add_argument("--baseline", required=True)
    compare.add_argument("--current", required=True)
    compare.set_defaults(handler=command_size_compare)

    runtime = subparsers.add_parser(
        "runtime-headroom",
        help="normalize bounded runtime/support evidence without inventing HIL data",
    )
    _add_common_output(runtime)
    runtime.add_argument(
        "--evidence",
        action="append",
        metavar="TARGET:KIND=PATH",
        help="e.g. s3:hardware=logs/s3.log (repeatable)",
    )
    runtime.add_argument(
        "--report-only",
        action="store_true",
        help="write incomplete diagnostic evidence without failing the command",
    )
    runtime.add_argument(
        "--legacy-c3", action="store_true", help=argparse.SUPPRESS
    )
    runtime.set_defaults(handler=command_runtime_headroom)

    return parser


def command_native_coverage(args: argparse.Namespace) -> int:
    # Imported lazily so parser/unit-test commands remain usable when optional
    # coverage tools are absent.
    from coverage_runner import run_native_coverage

    repo_root = _repo_root(args)
    repository = revision_info(repo_root)
    output_dir = _output_dir(args, repo_root, "native", repository)
    result = run_native_coverage(
        repo_root=repo_root,
        output_dir=output_dir,
        revision=repository["revision"],
        worktree_fingerprint=repository.get("worktree_fingerprint"),
        backend=args.backend,
        pio_core_dir=Path(args.pio_core_dir) if args.pio_core_dir else None,
    )
    _assert_repository_state_unchanged(
        repo_root,
        repository,
        invalidate_paths=(
            output_dir / "coverage.info",
            output_dir / "coverage.json",
            output_dir / "run.json",
            output_dir / "summary.txt",
        ),
    )
    summary = _coverage_summary(result.to_dict())
    write_text(output_dir / "summary.txt", summary)
    print(summary)
    return 0 if result.status == "ok" else (2 if result.status == "unavailable" else 1)


def command_flutter_coverage(args: argparse.Namespace) -> int:
    from coverage_runner import run_flutter_coverage

    repo_root = _repo_root(args)
    repository = revision_info(repo_root)
    output_dir = _output_dir(args, repo_root, "flutter", repository)
    result = run_flutter_coverage(
        repo_root=repo_root,
        output_dir=output_dir,
        revision=repository["revision"],
        worktree_fingerprint=repository.get("worktree_fingerprint"),
    )
    _assert_repository_state_unchanged(
        repo_root,
        repository,
        invalidate_paths=(
            output_dir / "coverage.info",
            output_dir / "coverage.json",
            output_dir / "run.json",
            output_dir / "summary.txt",
        ),
    )
    summary = _coverage_summary(result.to_dict())
    write_text(output_dir / "summary.txt", summary)
    print(summary)
    return 0 if result.status == "ok" else (2 if result.status == "unavailable" else 1)


def _coverage_summary(payload: dict[str, Any]) -> str:
    lines = [f"{payload['kind'].capitalize()} coverage: {payload['status']}"]
    coverage = payload.get("coverage")
    if isinstance(coverage, dict) and isinstance(coverage.get("summary"), dict):
        for metric_name in ("lines", "functions", "branches"):
            metric = coverage["summary"].get(metric_name)
            if not isinstance(metric, dict) or not metric.get("available"):
                lines.append(f"{metric_name}: unavailable")
                continue
            lines.append(
                f"{metric_name}: {metric['covered']}/{metric['total']} "
                f"({metric['percent']:.2f}%)"
            )
    for diagnostic in payload.get("diagnostics", []):
        lines.append(f"diagnostic: {diagnostic}")
    lines.append("thresholds: none (measurement artifact only)")
    return "\n".join(lines)


def _parse_evidence_argument(value: str) -> tuple[str, str, Path]:
    if "=" not in value or ":" not in value.split("=", 1)[0]:
        raise CodeHealthError("evidence must use TARGET:KIND=PATH")
    descriptor, raw_path = value.split("=", 1)
    target, evidence_kind = descriptor.split(":", 1)
    target = target.lower()
    if target.startswith("litch-esp32"):
        target = target.removeprefix("litch-esp32")
    if target not in LEGACY_RUNTIME_TARGETS:
        raise CodeHealthError(f"unsupported evidence target: {target}")
    if not raw_path:
        raise CodeHealthError("evidence path must not be empty")
    return target, evidence_kind, Path(raw_path)


def command_runtime_headroom(args: argparse.Namespace) -> int:
    from runtime_headroom import combine_reports, parse_file

    repo_root = _repo_root(args)
    repository = revision_info(repo_root)
    output_dir = _output_dir(args, repo_root, "support", repository)
    include_legacy_c3 = getattr(args, "legacy_c3", False)
    expected_targets = (
        LEGACY_RUNTIME_TARGETS if include_legacy_c3 else DEFAULT_RUNTIME_TARGETS
    )
    reports = []
    for value in args.evidence or []:
        target, evidence_kind, path = _parse_evidence_argument(value)
        if target == "c3" and not include_legacy_c3:
            raise CodeHealthError(
                "legacy C3 evidence requires the explicit --legacy-c3 option"
            )
        evidence_path = path.resolve() if path.is_absolute() else (repo_root / path).resolve()
        reports.append(
            parse_file(
                evidence_path,
                target=target,
                evidence_kind=evidence_kind,
                repo_root=repo_root,
            )
        )
    bundle = combine_reports(reports, expected_targets=expected_targets)
    _attach_repository(bundle, repository)
    _assert_repository_state_unchanged(repo_root, repository)
    write_json(output_dir / "report.json", bundle)
    lines = [f"Runtime headroom evidence: {bundle.get('status', 'unavailable')}"]
    for target in expected_targets:
        target_report = bundle["targets"][target]
        lines.append(
            f"{target}: {target_report['status']}, "
            f"hil={target_report['hardware_in_loop']['status']}"
        )
    lines.append("thresholds: none; missing signals remain unavailable/deferred HIL")
    summary = "\n".join(lines)
    write_text(output_dir / "summary.txt", summary)
    print(summary)
    print(f"report: {_display_path(output_dir / 'report.json', repo_root)}")
    if bundle.get("status") == "incomplete" and not getattr(
        args, "report_only", False
    ):
        return 1
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    try:
        require_supported_host()
    except UnsupportedHostError as exc:
        print(f"code-health: {exc}", file=sys.stderr)
        return 2
    args = _parser().parse_args(argv)
    try:
        return int(args.handler(args))
    except (CodeHealthError, ValueError, OSError) as exc:
        print(f"code-health: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
