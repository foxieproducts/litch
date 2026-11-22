#!/usr/bin/env python3
"""Verify the complete pinned Litch-Max source snapshot before compiling it."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import stat
import subprocess


REPOSITORY = "https://github.com/foxieproducts/litch-max.git"
SUBMODULE = "lib/litch-max"
INIT_HELP = f"run git submodule update --init --recursive -- {SUBMODULE}"
FORBIDDEN_NAMES = {
    ".git", ".pio", ".cache", "__pycache__", ".pytest_cache",
    ".mypy_cache", ".ruff_cache", ".dart_tool", ".ds_store",
    "build", "cache", "cmakefiles", "cmakecache.txt",
    ".sconsign.dblite", ".ninja_log", ".ninja_deps",
}
HEADER_NAMES = ("fixed_string.hpp", "format.hpp")


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(f"Litch-Max snapshot: {message}")


def _relative_path(value: object) -> tuple[str, ...]:
    _require(isinstance(value, str) and bool(value), "file path must be nonempty text")
    _require("\\" not in value and not any(ord(c) < 32 for c in value),
             f"invalid file path {value!r}")
    path = PurePosixPath(value)
    _require(not path.is_absolute() and not PureWindowsPath(value).drive and
             path.as_posix() == value and bool(path.parts) and
             all(part not in (".", "..") for part in path.parts),
             f"noncanonical relative file path {value!r}")
    _require(not any(part.casefold() in FORBIDDEN_NAMES for part in path.parts) and
             not value.casefold().endswith((".pyc", ".pyo")),
             f"repository metadata or cache path {value!r}")
    try:
        value.encode("utf-8")
    except UnicodeEncodeError as error:
        raise ValueError("Litch-Max snapshot: filenames must be valid UTF-8") from error
    return path.parts


def _ordinary_path(root: Path, relative: str, *, directory: bool = False) -> Path:
    """Check every descendant before reading; never follow a source symlink."""
    path = root
    parts = PurePosixPath(relative).parts
    for index, part in enumerate(parts):
        path = path / part
        mode = path.lstat().st_mode
        _require(not stat.S_ISLNK(mode), f"symlink is forbidden: {path}")
        expected_directory = index < len(parts) - 1 or directory
        _require(stat.S_ISDIR(mode) if expected_directory else stat.S_ISREG(mode),
                 f"expected {'directory' if expected_directory else 'regular file'}: {path}")
    return path


def _snapshot_files(root: Path) -> dict[str, tuple[Path, str]]:
    files = {}
    pending = [root]
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                path = Path(entry.path)
                relative = path.relative_to(root).as_posix()
                if relative == ".git":  # An initialized submodule owns this one marker.
                    continue
                _relative_path(relative)
                mode = entry.stat(follow_symlinks=False).st_mode
                _require(not stat.S_ISLNK(mode), f"symlink is forbidden: {relative}")
                if stat.S_ISDIR(mode):
                    pending.append(path)
                else:
                    _require(stat.S_ISREG(mode), f"nonregular file: {relative}")
                    files[relative] = (path, "100755" if mode & stat.S_IXUSR else "100644")
    return files


def _git(root: Path, *args: str) -> bytes:
    # A partial clone must not fetch missing objects during a build/evidence check.
    env = {**os.environ, "GIT_NO_LAZY_FETCH": "1", "GIT_ALLOW_PROTOCOL": ""}
    result = subprocess.run(["git", "--no-replace-objects", "-C", os.fspath(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    _require(result.returncode == 0, result.stderr.decode(errors="replace").strip() or
             f"git {args[0]} failed in {root}")
    return result.stdout


def _checkout_root(path: Path) -> bool:
    top = os.fsdecode(_git(path, "rev-parse", "--show-toplevel").removesuffix(b"\n"))
    return Path(top).resolve() == path.resolve()


def _tree_entries(root: Path, *args: str) -> dict[str, tuple[str, str, str]]:
    entries = {}
    for record in _git(root, "ls-tree", "--full-tree", "-z", *args).split(b"\0"):
        if record:
            info, raw_path = record.split(b"\t", 1)
            name = raw_path.decode("utf-8")
            _relative_path(name)
            mode, kind, oid = info.decode("ascii").split(" ")
            entries[name] = (mode, kind, oid)
    return entries


def _selected_pin(root: Path, revision: str) -> str:
    entries = _tree_entries(root, revision, "--", ".gitmodules", SUBMODULE)
    _require(entries.get(".gitmodules", ())[:2] in (("100644", "blob"), ("100755", "blob")),
             "selected firmware commit must contain a regular .gitmodules")
    link = entries.get(SUBMODULE, ())
    _require(link[:2] == ("160000", "commit"), f"selected firmware commit must pin {SUBMODULE} as a gitlink")
    config = _git(root, "config", "--no-includes", "--null", "--blob", f"{revision}:.gitmodules", "--list")
    settings = {}
    for entry in config.split(b"\0"):
        if entry:
            key, _, value = entry.partition(b"\n")
            settings.setdefault(key, []).append(value)
    path = SUBMODULE.encode()
    keys = [key for key, values in settings.items()
            if key.startswith(b"submodule.") and key.endswith(b".path") and path in values]
    _require(len(keys) == 1 and settings[keys[0]] == [path], "invalid or ambiguous submodule path in .gitmodules")
    _require(settings.get(keys[0][:-len(b"path")] + b"url") == [REPOSITORY.encode()],
             "unexpected submodule repository in committed .gitmodules")
    return link[2]


def verify_snapshot(project_root: str | Path, source_sha: str = "HEAD") -> dict:
    """Derive the file receipt from a selected firmware gitlink and its local checkout."""
    root = Path(project_root).absolute()
    _require(stat.S_ISDIR(root.lstat().st_mode) and not root.is_symlink(),
             "project root must be an ordinary directory")
    _require(_checkout_root(root), "project root must be the firmware Git checkout")
    revision = _git(root, "rev-parse", "--verify", "--end-of-options", f"{source_sha}^{{commit}}").decode().strip()
    commit = _selected_pin(root, revision)
    try:
        engine = _ordinary_path(root, SUBMODULE, directory=True)
        marker = (engine / ".git").lstat().st_mode
    except FileNotFoundError:
        raise ValueError(f"Litch-Max submodule is not initialized; {INIT_HELP}") from None
    _require(stat.S_ISREG(marker) or stat.S_ISDIR(marker), f"invalid submodule .git marker; {INIT_HELP}")
    _require(_checkout_root(engine), f"not an initialized engine checkout; {INIT_HELP}")
    head = _git(engine, "rev-parse", "--verify", "HEAD").decode().strip()
    _require(head == commit, f"submodule HEAD {head} differs from pinned {commit}; {INIT_HELP}")
    tree = _git(engine, "rev-parse", "--verify", f"{commit}^{{tree}}").decode().strip()
    entries = _tree_entries(engine, "-r", tree)
    _require(bool(entries), "engine commit has no source files")
    files = _snapshot_files(engine)
    missing = sorted(entries.keys() - files.keys())
    extra = sorted(files.keys() - entries.keys())
    _require(not missing and not extra, f"file set differs; missing={missing[:5]!r}, extra={extra[:5]!r}")
    records = {}
    for relative, (mode, kind, oid) in entries.items():
        path, actual_mode = files[relative]
        _require(kind == "blob" and mode == actual_mode, f"Git mode or file type differs: {relative}")
        content = path.read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(content)).encode("ascii") + b"\0" + content).hexdigest()
        _require(blob == oid, f"bytes differ from pinned Git blob: {relative}")
        records[relative] = {"mode": mode, "sha256": hashlib.sha256(content).hexdigest()}
    shared_headers = [f"lib/foxie_embedded/include/foxie_embedded/{name}" for name in HEADER_NAMES]
    selected_headers = _tree_entries(root, revision, "--", *shared_headers)
    for name, shared_path in zip(HEADER_NAMES, shared_headers):
        relative = f"third_party/foxie_embedded/include/foxie_embedded/{name}"
        _require(relative in records, f"missing bundled utility header: {name}")
        selected = selected_headers.get(shared_path, ())
        _require(selected[:2] in (("100644", "blob"), ("100755", "blob")) and
                 selected[2] == entries[relative][2], f"bundled utility header differs from selected firmware: {name}")
        original = _ordinary_path(root, shared_path)
        _require(hashlib.sha256(original.read_bytes()).hexdigest() == records[relative]["sha256"],
                 f"bundled utility header differs from firmware copy: {name}")
    return {"schema": 1, "repository": REPOSITORY, "commit": commit, "tree": tree, "files": records}


def install(env) -> None:
    root = Path(env.subst("$PROJECT_DIR"))
    try:
        verify_snapshot(root)
    except (OSError, ValueError) as error:
        from SCons.Errors import UserError
        raise UserError(str(error)) from error
    env.PrependUnique(CPPPATH=[str(root / "lib/foxie_embedded/include")])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project_root", nargs="?", type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument("--source-sha", default="HEAD", help="superproject commit containing the engine gitlink")
    args = parser.parse_args()
    try:
        pin = verify_snapshot(args.project_root, args.source_sha)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Litch-Max {pin['commit']} verified: {len(pin['files'])} files, tree {pin['tree']}")
    return 0


if "Import" in globals():
    Import("env")  # type: ignore[name-defined]  # Injected only by SCons.
    install(env)  # type: ignore[name-defined]
elif __name__ == "__main__":
    raise SystemExit(main())
