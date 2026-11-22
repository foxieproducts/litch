#!/usr/bin/env python3
"""Verify checked-in release evidence is complete, current, and reproducible."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

if __package__:
    from .release_inventory import APP_NATIVE_NOTICES_RELATIVE
else:
    from release_inventory import APP_NATIVE_NOTICES_RELATIVE


REPO_ROOT = Path(__file__).resolve().parents[2].resolve()
GENERATED_RELATIVE = Path("docs/release/generated")
EXPECTED_GENERATED = frozenset(
    {
        "dependency_inventory.json",
        "dependency_inventory.md",
        "release_sbom.json",
    }
)
EXPECTED_ROOT = frozenset({"THIRD_PARTY_NOTICES.md", APP_NATIVE_NOTICES_RELATIVE.as_posix()})
GENERATOR_TIMEOUT_SECONDS = 120
VERIFICATION_TIMEOUT_SECONDS = 2 * GENERATOR_TIMEOUT_SECONDS + 60


class VerificationError(RuntimeError):
    """A release artifact verification check failed."""


def _contained(path: Path, root: Path) -> Path:
    resolved = path.resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise VerificationError(f"temporary output escapes isolated root: {resolved}") from error
    return resolved


def _run_generator(script: Path, repo_root: Path, output_dir: Path, *extra: str) -> None:
    command = [
        sys.executable,
        str(script),
        "--repo-root",
        str(repo_root),
        "--output-dir",
        str(output_dir),
        *extra,
    ]
    try:
        result = subprocess.run(
            command,
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
            timeout=GENERATOR_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        raise VerificationError(
            f"{script.name} timed out after {GENERATOR_TIMEOUT_SECONDS} seconds"
        ) from error
    if result.returncode:
        detail = (result.stderr or result.stdout).strip().splitlines()
        message = detail[-1] if detail else "no diagnostic output"
        raise VerificationError(f"{script.name} failed ({result.returncode}): {message}")


def _files(root: Path, expected: frozenset[str]) -> dict[str, bytes]:
    actual = {path.name for path in root.iterdir()} if root.is_dir() else set()
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        details = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if unexpected:
            details.append(f"unexpected {', '.join(unexpected)}")
        raise VerificationError(f"generated file set mismatch in {root}: {'; '.join(details)}")
    return {name: (root / name).read_bytes() for name in sorted(expected)}


def _generate(repo_root: Path, temporary_root: Path, suffix: str) -> dict[str, bytes]:
    generated = _contained(temporary_root / suffix / GENERATED_RELATIVE, temporary_root)
    generated.mkdir(parents=True)
    notices = _contained(temporary_root / suffix / "THIRD_PARTY_NOTICES.md", temporary_root)
    app_notices = _contained(temporary_root / suffix / APP_NATIVE_NOTICES_RELATIVE, temporary_root)
    _run_generator(
        repo_root / "tools/release/release_inventory.py",
        repo_root,
        generated,
        "--notices-path",
        str(notices),
        "--app-notices-path",
        str(app_notices),
    )
    return preflight(temporary_root / suffix)


def preflight(repo_root: Path) -> dict[str, bytes]:
    """Read the complete set of public release artifacts."""
    generated = _files(repo_root / GENERATED_RELATIVE, EXPECTED_GENERATED)
    return {
        **{f"{GENERATED_RELATIVE.as_posix()}/{name}": data for name, data in generated.items()},
        **{name: (repo_root / name).read_bytes() for name in sorted(EXPECTED_ROOT)},
    }


def verify(repo_root: Path) -> None:
    tracked = preflight(repo_root)
    with tempfile.TemporaryDirectory(prefix="litch-release-verify-") as directory:
        temporary_root = Path(directory).resolve()
        first = _generate(repo_root, temporary_root, "first")
        stale = sorted(name for name in tracked if tracked[name] != first[name])
        if stale:
            raise VerificationError("tracked release artifacts are stale: " + ", ".join(stale))
        second = _generate(repo_root, temporary_root, "second")
        if first != second:
            differing = sorted(name for name in first if first[name] != second[name])
            raise VerificationError(
                "generators are nondeterministic: " + ", ".join(differing)
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=str(REPO_ROOT))
    parser.add_argument("--preflight-only", action="store_true",
                        help="check that all required release artifacts exist")
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    try:
        if args.preflight_only:
            preflight(repo_root)
        else:
            verify(repo_root)
    except (OSError, VerificationError) as error:
        print(f"release artifact verification failed: {error}", file=sys.stderr)
        return 1
    print("release artifact preflight: OK (required files present)"
          if args.preflight_only else
          f"release artifact verification: OK ({len(EXPECTED_GENERATED) + len(EXPECTED_ROOT)} artifacts, 2 deterministic generations)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
