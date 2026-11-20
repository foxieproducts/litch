#!/usr/bin/env python3
"""Verify the complete pinned Litch-Max source snapshot before compiling it."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import stat


REPOSITORY = "https://github.com/foxieproducts/litch-max.git"
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


def _unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        _require(key not in result, f"duplicate JSON key {key!r}")
        result[key] = value
    return result


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
                _relative_path(relative)
                mode = entry.stat(follow_symlinks=False).st_mode
                _require(not stat.S_ISLNK(mode), f"symlink is forbidden: {relative}")
                if stat.S_ISDIR(mode):
                    pending.append(path)
                else:
                    _require(stat.S_ISREG(mode), f"nonregular file: {relative}")
                    files[relative] = (path, "100755" if mode & stat.S_IXUSR else "100644")
    return files


def _git_object(kind: bytes, content: bytes) -> bytes:
    return hashlib.sha1(kind + b" " + str(len(content)).encode("ascii") + b"\0" + content).digest()


def _tree_hash(tree: dict) -> bytes:
    # Git compares a directory as if its name ends in '/'. Ordinary empty
    # directories are absent: only file paths create nodes in this tree.
    names = sorted(tree, key=lambda name: name.encode("utf-8") +
                   (b"/" if isinstance(tree[name], dict) else b""))
    entries = []
    for name in names:
        item = tree[name]
        mode, digest = ("40000", _tree_hash(item)) if isinstance(item, dict) else item
        entries.append(mode.encode("ascii") + b" " + name.encode("utf-8") + b"\0" + digest)
    return _git_object(b"tree", b"".join(entries))


def verify_snapshot(project_root: str | Path) -> dict:
    """Return the admitted pin, or raise ValueError/OSError on any mismatch.

    This verifies source bytes and their Git tree identity. It does not fetch
    or independently authenticate the recorded remote commit.
    """
    root = Path(project_root).absolute()
    _require(stat.S_ISDIR(root.lstat().st_mode) and not root.is_symlink(),
             "project root must be an ordinary directory")
    pin_path = _ordinary_path(root, "lib/litch-max.snapshot.json")
    pin = json.loads(pin_path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    _require(isinstance(pin, dict) and
             set(pin) == {"schema", "repository", "commit", "tree", "files"},
             "pin must contain schema, repository, commit, tree and files")
    _require(type(pin["schema"]) is int and pin["schema"] == 1, "unsupported pin schema")
    _require(pin["repository"] == REPOSITORY, "unexpected repository")
    for field in ("commit", "tree"):
        _require(isinstance(pin[field], str) and
                 re.fullmatch(r"[0-9a-fA-F]{40}", pin[field]) is not None,
                 f"{field} must be a 40-digit Git SHA-1")
    records = pin["files"]
    _require(isinstance(records, dict) and bool(records), "files must be a nonempty object")
    for relative, record in records.items():
        _relative_path(relative)
        _require(isinstance(record, dict) and set(record) == {"sha256", "mode"},
                 f"invalid file record: {relative}")
        _require(isinstance(record["sha256"], str) and
                 re.fullmatch(r"[0-9a-fA-F]{64}", record["sha256"]) is not None,
                 f"invalid SHA256: {relative}")
        _require(record["mode"] in ("100644", "100755"), f"invalid Git mode: {relative}")

    snapshot = _ordinary_path(root, "lib/litch-max", directory=True)
    files = _snapshot_files(snapshot)
    missing = sorted(set(records) - set(files))
    extra = sorted(set(files) - set(records))
    _require(not missing and not extra,
             f"file set differs; missing={missing[:5]!r}, extra={extra[:5]!r}")
    tree = {}
    header_bytes = {}
    for relative, record in records.items():
        path, mode = files[relative]
        _require(mode == record["mode"], f"Git mode differs: {relative}")
        content = path.read_bytes()
        _require(hashlib.sha256(content).hexdigest() == record["sha256"].lower(),
                 f"SHA256 differs: {relative}")
        node = tree
        parts = _relative_path(relative)
        for part in parts[:-1]:
            node = node.setdefault(part, {})
            _require(isinstance(node, dict), f"file/directory conflict: {relative}")
        _require(parts[-1] not in node, f"file/directory conflict: {relative}")
        node[parts[-1]] = (mode, _git_object(b"blob", content))
        if relative.startswith("third_party/foxie_embedded/include/foxie_embedded/"):
            header_bytes[relative] = content
    _require(_tree_hash(tree).hex() == pin["tree"].lower(), "Git tree differs from pinned tree")
    for name in HEADER_NAMES:
        relative = f"third_party/foxie_embedded/include/foxie_embedded/{name}"
        _require(relative in header_bytes, f"missing bundled utility header: {name}")
        original = _ordinary_path(root, f"lib/foxie_embedded/include/foxie_embedded/{name}")
        _require(original.read_bytes() == header_bytes[relative],
                 f"bundled utility header differs from firmware copy: {name}")
    return pin


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
    args = parser.parse_args()
    try:
        pin = verify_snapshot(args.project_root)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Litch-Max {pin['commit']} verified: {len(pin['files'])} files, tree {pin['tree']}")
    return 0


if "Import" in globals():
    Import("env")  # type: ignore[name-defined]  # Injected only by SCons.
    install(env)  # type: ignore[name-defined]
elif __name__ == "__main__":
    raise SystemExit(main())
