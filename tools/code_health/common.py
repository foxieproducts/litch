#!/usr/bin/env python3
"""Shared utilities for bounded code-health reports."""

from __future__ import annotations

import hashlib
import json
import os
import re
import signal
import shutil
import stat
import subprocess
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


DEFAULT_CAPTURE_BYTES = 1_000_000
SCHEMA_VERSION = 1
MAX_WORKTREE_DIFF_BYTES = 64 * 1024 * 1024
MAX_UNTRACKED_LIST_BYTES = 8 * 1024 * 1024
MAX_UNTRACKED_FILES = 10_000
MAX_UNTRACKED_CONTENT_BYTES = 256 * 1024 * 1024
PIPE_DRAIN_JOIN_SECONDS = 5.0


class CodeHealthError(RuntimeError):
    """A user-facing, deterministic code-health failure."""


@dataclass(frozen=True)
class CommandResult:
    """Bounded subprocess result suitable for a machine report."""

    command: tuple[str, ...]
    returncode: int
    output: str
    truncated: bool
    timed_out: bool = False


def find_repo_root(start: Path | None = None) -> Path:
    """Find the repository root without relying on the caller's cwd."""

    current = (start or Path.cwd()).resolve()
    if current.is_file():
        current = current.parent
    for candidate in (current, *current.parents):
        if (candidate / "platformio.ini").is_file() and (candidate / "app").is_dir():
            return candidate
    raise CodeHealthError("repository root not found (expected platformio.ini and app/)")


def _git(repo_root: Path, *args: str) -> str:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=repo_root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=60,
        )
    except subprocess.TimeoutExpired as exc:
        raise CodeHealthError("git command timed out after 60 seconds") from exc
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or "git failed"
        raise CodeHealthError(detail)
    return completed.stdout.strip()


def revision_info(repo_root: Path) -> dict[str, Any]:
    """Return revision metadata with a content-bound dirty-worktree identity.

    Ignored output is excluded by Git.  Tracked diffs and every untracked file
    are bounded and hashed, so different source states cannot share a generic
    ``HEAD-dirty`` artifact directory.
    """

    revision = _git(repo_root, "rev-parse", "HEAD")
    fingerprint = _worktree_fingerprint(repo_root)
    return {
        "revision": revision,
        "dirty": fingerprint is not None,
        "worktree_fingerprint": fingerprint,
    }


def _worktree_fingerprint(repo_root: Path) -> str | None:
    tracked = run_bounded(
        ["git", "diff", "--binary", "--no-ext-diff", "HEAD", "--"],
        cwd=repo_root,
        max_capture_bytes=MAX_WORKTREE_DIFF_BYTES,
        timeout_seconds=60,
    )
    if tracked.timed_out:
        raise CodeHealthError("worktree diff timed out after 60 seconds")
    if tracked.truncated:
        raise CodeHealthError(
            f"worktree diff exceeds the {MAX_WORKTREE_DIFF_BYTES}-byte fingerprint limit"
        )
    if tracked.returncode != 0:
        raise CodeHealthError(tracked.output.strip() or "git diff failed")

    untracked = run_bounded(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=repo_root,
        max_capture_bytes=MAX_UNTRACKED_LIST_BYTES,
        timeout_seconds=60,
    )
    if untracked.timed_out:
        raise CodeHealthError("untracked-file selection timed out after 60 seconds")
    if untracked.truncated:
        raise CodeHealthError(
            "untracked-file list exceeds the bounded worktree fingerprint limit"
        )
    if untracked.returncode != 0:
        raise CodeHealthError(untracked.output.strip() or "git ls-files failed")

    untracked_paths = sorted(path for path in untracked.output.split("\0") if path)
    if len(untracked_paths) > MAX_UNTRACKED_FILES:
        raise CodeHealthError(
            f"worktree has {len(untracked_paths)} untracked files; limit is {MAX_UNTRACKED_FILES}"
        )
    if not tracked.output and not untracked_paths:
        return None

    digest = hashlib.sha256()
    digest.update(b"tracked-diff\0")
    digest.update(tracked.output.encode("utf-8", errors="surrogateescape"))
    total_untracked_bytes = 0
    root = repo_root.resolve()
    for relative in untracked_paths:
        candidate = repo_root / relative
        try:
            candidate.parent.resolve().relative_to(root)
        except ValueError as exc:
            raise CodeHealthError(
                f"untracked path escapes repository during fingerprinting: {relative}"
            ) from exc
        try:
            metadata = candidate.lstat()
        except OSError as exc:
            raise CodeHealthError(
                f"cannot inspect untracked path during fingerprinting: {relative}: {exc}"
            ) from exc
        digest.update(b"untracked\0")
        digest.update(relative.encode("utf-8", errors="surrogateescape"))
        digest.update(b"\0")
        if stat.S_ISLNK(metadata.st_mode):
            payload = os.readlink(candidate).encode("utf-8", errors="surrogateescape")
            total_untracked_bytes += len(payload)
            digest.update(b"symlink\0")
            digest.update(payload)
        elif stat.S_ISREG(metadata.st_mode):
            total_untracked_bytes += metadata.st_size
            if total_untracked_bytes > MAX_UNTRACKED_CONTENT_BYTES:
                raise CodeHealthError(
                    "untracked content exceeds the bounded worktree fingerprint limit"
                )
            digest.update(b"file\0")
            digest.update(
                b"executable\0" if metadata.st_mode & 0o111 else b"not-executable\0"
            )
            with candidate.open("rb") as handle:
                for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(chunk)
        else:
            raise CodeHealthError(
                f"unsupported untracked filesystem entry during fingerprinting: {relative}"
            )
        digest.update(b"\0")
    return digest.hexdigest()


def revision_output_root(
    repo_root: Path,
    repository: Mapping[str, Any] | None = None,
) -> Path:
    """Return the output root for one already-captured repository identity."""

    info = revision_info(repo_root) if repository is None else repository
    suffix = (
        f"-dirty-{str(info['worktree_fingerprint'])[:12]}" if info["dirty"] else ""
    )
    return repo_root / "build" / "code_health" / f"{info['revision'][:12]}{suffix}"


def normalized_repo_path(path: str | Path, repo_root: Path, base_dir: Path | None = None) -> str:
    """Normalize a source path to a host-independent repository-relative path."""

    raw = Path(path)
    if not raw.is_absolute():
        raw = (base_dir or repo_root) / raw
    resolved = raw.resolve(strict=False)
    try:
        return resolved.relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return f"<external>/{resolved.name}"


def scrub_text(text: str, repo_root: Path) -> str:
    """Remove host-specific repository and home prefixes from captured output."""

    scrubbed = text.replace(str(repo_root.resolve()), ".")
    user_home = str(Path.home().resolve())
    if user_home and user_home != "/":
        scrubbed = scrubbed.replace(user_home, "<home>")
    posix_path = re.compile(
        r"(?<![:/A-Za-z0-9_])/(?:[^\s/:]+/)+[^\s:;,)}\]]+"
    )
    windows_path = re.compile(
        r"(?<![A-Za-z0-9_])[A-Za-z]:\\(?:[^\s\\:]+\\)+[^\s:;,)}\]]+"
    )
    scrubbed = posix_path.sub(
        lambda match: f"<absolute>/{Path(match.group(0)).name}",
        scrubbed,
    )
    scrubbed = windows_path.sub(
        lambda match: "<absolute>/"
        + match.group(0).replace("\\", "/").rsplit("/", 1)[-1],
        scrubbed,
    )
    return scrubbed


def scrub_command(command: Sequence[str], repo_root: Path) -> list[str]:
    return [scrub_text(str(arg), repo_root) for arg in command]


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, allow_nan=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def write_text(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value.rstrip() + "\n", encoding="utf-8")


def read_json_bounded(path: Path, max_bytes: int = 16_000_000) -> Any:
    if not path.is_file():
        raise CodeHealthError(f"input does not exist: {path}")
    size = path.stat().st_size
    if size > max_bytes:
        raise CodeHealthError(f"input exceeds {max_bytes} byte limit: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CodeHealthError(f"invalid JSON in {path}: {exc}") from exc


def read_text_bounded(path: Path, max_bytes: int = 16_000_000) -> str:
    if not path.is_file():
        raise CodeHealthError(f"input does not exist: {path}")
    if path.stat().st_size > max_bytes:
        raise CodeHealthError(f"input exceeds {max_bytes} byte limit: {path}")
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise CodeHealthError(f"cannot read {path}: {exc}") from exc


def stable_fingerprint(*parts: str) -> str:
    digest = hashlib.sha256()
    for part in parts:
        digest.update(part.encode("utf-8"))
        digest.update(b"\0")
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_bounded(
    command: Sequence[str],
    *,
    cwd: Path,
    env: Mapping[str, str] | None = None,
    max_capture_bytes: int = DEFAULT_CAPTURE_BYTES,
    timeout_seconds: float = 600,
) -> CommandResult:
    """Run a time-bounded command while retaining bounded combined output."""

    if timeout_seconds <= 0:
        raise CodeHealthError("command timeout must be positive")

    merged_env = os.environ.copy()
    if env:
        merged_env.update({key: str(value) for key, value in env.items()})
    try:
        process = subprocess.Popen(
            [str(arg) for arg in command],
            cwd=cwd,
            env=merged_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    except OSError as exc:
        raise CodeHealthError(f"cannot run {command[0]}: {exc}") from exc

    retained = bytearray()
    truncated = False
    assert process.stdout is not None

    def drain() -> None:
        nonlocal truncated
        try:
            for chunk in iter(lambda: process.stdout.read(65536), b""):
                remaining = max_capture_bytes - len(retained)
                if remaining > 0:
                    retained.extend(chunk[:remaining])
                if len(chunk) > remaining:
                    truncated = True
        finally:
            process.stdout.close()

    reader = threading.Thread(target=drain, name="code-health-output", daemon=True)
    reader.start()
    timed_out = False
    try:
        returncode = process.wait(timeout=timeout_seconds)
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
    reader.join(timeout=PIPE_DRAIN_JOIN_SECONDS)
    if reader.is_alive():
        truncated = True
        if returncode == 0:
            returncode = 125
    output = retained.decode("utf-8", errors="replace")
    return CommandResult(
        tuple(str(arg) for arg in command),
        returncode,
        output,
        truncated,
        timed_out,
    )


def resolve_tool(name: str) -> str | None:
    return shutil.which(name)


def probe_tool(
    name: str,
    version_args: Sequence[str],
    *,
    cwd: Path,
    repo_root: Path,
) -> dict[str, Any]:
    executable = resolve_tool(name)
    if executable is None:
        return {
            "name": name,
            "status": "unavailable",
            "reason": f"{name} is not on PATH",
            "version": None,
        }
    try:
        result = run_bounded(
            [executable, *version_args],
            cwd=cwd,
            max_capture_bytes=16_384,
            timeout_seconds=30,
        )
    except CodeHealthError as exc:
        return {
            "name": name,
            "status": "probe_failed",
            "reason": str(exc),
            "version": None,
        }
    version_output = scrub_text(result.output.strip(), repo_root)
    if result.truncated:
        return {
            "name": name,
            "status": "probe_failed",
            "reason": f"version probe exceeded the {16_384}-byte capture cap",
            "version": None,
        }
    if result.timed_out:
        return {
            "name": name,
            "status": "probe_failed",
            "reason": "version probe timed out after 30 seconds",
            "version": None,
        }
    if result.returncode != 0:
        return {
            "name": name,
            "status": "probe_failed",
            "reason": version_output or f"version probe exited {result.returncode}",
            "version": None,
        }
    return {
        "name": name,
        "status": "available",
        "reason": None,
        "version": version_output.splitlines()[0] if version_output else "unknown",
    }


def select_changed_files(repo_root: Path, changed_from: str, max_files: int) -> list[str]:
    """Select changed files without shell interpolation or implicit whole-repo scope."""

    result = run_bounded(
        [
            "git",
            "diff",
            "--name-only",
            "--diff-filter=ACMR",
            "-z",
            f"{changed_from}...HEAD",
            "--",
        ],
        cwd=repo_root,
        max_capture_bytes=max(65_536, (max_files + 1) * 4096),
        timeout_seconds=60,
    )
    if result.timed_out:
        raise CodeHealthError("changed-file selection timed out after 60 seconds")
    if result.returncode != 0:
        raise CodeHealthError(result.output.strip() or "git diff failed")
    if result.truncated:
        raise CodeHealthError("changed-file selection exceeded its bounded output")
    output = result.output
    files = sorted({item for item in output.split("\0") if item})
    if len(files) > max_files:
        raise CodeHealthError(
            f"changed scope contains {len(files)} files; raise --max-files above {max_files} explicitly"
        )
    return files


def normalize_explicit_paths(repo_root: Path, paths: Iterable[str]) -> list[str]:
    normalized: set[str] = set()
    for value in paths:
        path = normalized_repo_path(value, repo_root)
        if path.startswith("<external>/"):
            raise CodeHealthError(f"scope path is outside the repository: {value}")
        normalized.add(path.rstrip("/"))
    return sorted(normalized)


def expand_repo_paths(
    repo_root: Path,
    selectors: Sequence[str],
    *,
    max_files: int,
    suffixes: set[str] | None = None,
) -> list[str]:
    """Expand explicit repository files/directories with a hard, visible bound."""

    ignored_parts = {".git", ".pio", ".dart_tool", "build", "__pycache__"}
    files: set[str] = set()
    for selector in selectors:
        candidate = repo_root / selector
        if not candidate.exists():
            raise CodeHealthError(f"scope path does not exist: {selector}")
        candidates = [candidate] if candidate.is_file() else candidate.rglob("*")
        for path in candidates:
            if not path.is_file():
                continue
            try:
                path.resolve().relative_to(repo_root.resolve())
            except ValueError as exc:
                raise CodeHealthError(
                    f"scope path resolves outside the repository: {path}"
                ) from exc
            relative = path.relative_to(repo_root).as_posix()
            if any(part in ignored_parts for part in Path(relative).parts):
                continue
            if suffixes is not None and path.suffix.lower() not in suffixes:
                continue
            files.add(relative)
            if len(files) > max_files:
                raise CodeHealthError(
                    f"explicit scope contains more than {max_files} files; raise --max-files explicitly"
                )
    return sorted(files)


def path_in_scope(path: str, selectors: Sequence[str]) -> bool:
    return any(path == selector or path.startswith(f"{selector}/") for selector in selectors)


def normalize_message(message: str) -> str:
    return re.sub(r"\s+", " ", message.strip())
