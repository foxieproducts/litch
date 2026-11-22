#!/usr/bin/env python3
"""Compare a vendored subtree with a pinned, caller-supplied Git checkout.

The tool never clones or fetches. It reads the pinned commit directly from the
supplied checkout's object database and disables Git's partial-clone lazy fetch.
"""

from __future__ import annotations

import argparse
import dataclasses
import difflib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


STATUSES = ("exact", "modified", "local-only", "upstream-only")
SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ino"}
)
HEX_SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
GIT_BLOB_MODES = frozenset({"100644", "100755", "120000"})
SOURCE_TOKEN_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|"
    r"[A-Za-z_]\w*|(?:0[xX][0-9A-Fa-f]+|\d+(?:\.\d*)?(?:[eEpP][+-]?\d+)?[A-Za-z0-9_]*)|"
    r"<=>|>>=|<<=|->\*|\.\.\.|##|::|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||"
    r"\*=|/=|%=|\+=|-=|&=|\^=|\|=|\.\*|<:|:>|<%|%>|%:|[^\s]",
    re.DOTALL,
)


class AuditError(RuntimeError):
    """The manifest or supplied checkout cannot be audited safely."""


@dataclasses.dataclass(frozen=True)
class FileExpectation:
    status: str
    local_blob: Optional[str] = None
    local_mode: Optional[str] = None


@dataclasses.dataclass(frozen=True)
class VendorSpec:
    name: str
    repository: str
    tag: str
    commit: str
    tree: str
    local_path: PurePosixPath
    files: Mapping[str, FileExpectation]


@dataclasses.dataclass(frozen=True)
class CheckoutInfo:
    root: Path
    head: str
    tree: str
    origin: str


@dataclasses.dataclass(frozen=True)
class Inventory:
    exact: Tuple[str, ...]
    modified: Tuple[str, ...]
    local_only: Tuple[str, ...]
    upstream_only: Tuple[str, ...]

    def paths_for(self, status: str) -> Tuple[str, ...]:
        if status == "exact":
            return self.exact
        if status == "modified":
            return self.modified
        if status == "local-only":
            return self.local_only
        if status == "upstream-only":
            return self.upstream_only
        raise KeyError(status)


@dataclasses.dataclass(frozen=True)
class AuditResult:
    spec: VendorSpec
    checkout: CheckoutInfo
    inventory: Inventory
    upstream_blobs: Mapping[str, str]
    upstream_modes: Mapping[str, str]
    local_blobs: Mapping[str, str]
    local_modes: Mapping[str, str]
    drift: Tuple[str, ...]

    @property
    def ok(self) -> bool:
        return not self.drift


def _require_mapping(value: object, context: str) -> Mapping[str, object]:
    if not isinstance(value, dict):
        raise AuditError(f"{context} must be a JSON object")
    return value


def _require_string(mapping: Mapping[str, object], key: str, context: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        raise AuditError(f"{context}.{key} must be a non-empty string")
    return value


def _validate_sha1(value: str, context: str) -> str:
    if not HEX_SHA1_RE.fullmatch(value):
        raise AuditError(f"{context} must be a full lowercase SHA-1 object ID")
    return value


def _validate_relative_path(value: str, context: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or value in ("", ".")
        or path.as_posix() != value
        or any(part in ("", ".", "..") for part in path.parts)
    ):
        raise AuditError(f"{context} must be a normalized relative POSIX path")
    if ".git" in path.parts:
        raise AuditError(f"{context} must not include a .git path component")
    return path


def load_vendor_spec(manifest_path: Path, vendor_name: str) -> VendorSpec:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AuditError(f"cannot read manifest {manifest_path}: {exc}") from exc

    root = _require_mapping(manifest, "manifest")
    if root.get("schema_version") != 1:
        raise AuditError("manifest.schema_version must be 1")
    vendors = _require_mapping(root.get("vendors"), "manifest.vendors")
    if vendor_name not in vendors:
        available = ", ".join(sorted(vendors))
        raise AuditError(f"unknown vendor {vendor_name!r}; available: {available}")

    raw_spec = _require_mapping(vendors[vendor_name], f"manifest.vendors.{vendor_name}")
    repository = _require_string(raw_spec, "repository", vendor_name)
    tag = _require_string(raw_spec, "tag", vendor_name)
    commit = _validate_sha1(_require_string(raw_spec, "commit", vendor_name), f"{vendor_name}.commit")
    tree = _validate_sha1(_require_string(raw_spec, "tree", vendor_name), f"{vendor_name}.tree")
    local_path = _validate_relative_path(
        _require_string(raw_spec, "local_path", vendor_name), f"{vendor_name}.local_path"
    )
    raw_files = _require_mapping(raw_spec.get("files"), f"{vendor_name}.files")

    files: Dict[str, FileExpectation] = {}
    for raw_path, raw_expectation in raw_files.items():
        if not isinstance(raw_path, str):
            raise AuditError(f"{vendor_name}.files keys must be strings")
        path = _validate_relative_path(raw_path, f"{vendor_name}.files[{raw_path!r}]").as_posix()
        expectation = _require_mapping(raw_expectation, f"{vendor_name}.files[{path!r}]")
        status = _require_string(expectation, "status", f"{vendor_name}.files[{path!r}]")
        if status not in STATUSES:
            raise AuditError(f"{vendor_name}.files[{path!r}].status must be one of {', '.join(STATUSES)}")
        local_blob_value = expectation.get("local_blob")
        local_mode_value = expectation.get("local_mode")
        local_blob: Optional[str]
        local_mode: Optional[str]
        if status in ("modified", "local-only"):
            if not isinstance(local_blob_value, str):
                raise AuditError(f"{vendor_name}.files[{path!r}] requires local_blob for status {status}")
            local_blob = _validate_sha1(local_blob_value, f"{vendor_name}.files[{path!r}].local_blob")
            if not isinstance(local_mode_value, str) or local_mode_value not in GIT_BLOB_MODES:
                modes = ", ".join(sorted(GIT_BLOB_MODES))
                raise AuditError(
                    f"{vendor_name}.files[{path!r}].local_mode must be one of {modes} "
                    f"for status {status}"
                )
            local_mode = local_mode_value
        else:
            if local_blob_value is not None:
                raise AuditError(
                    f"{vendor_name}.files[{path!r}] must omit local_blob for status {status}"
                )
            if local_mode_value is not None:
                raise AuditError(
                    f"{vendor_name}.files[{path!r}] must omit local_mode for status {status}"
                )
            local_blob = None
            local_mode = None
        files[path] = FileExpectation(
            status=status,
            local_blob=local_blob,
            local_mode=local_mode,
        )

    if not files:
        raise AuditError(f"{vendor_name}.files must not be empty")
    return VendorSpec(
        name=vendor_name,
        repository=repository,
        tag=tag,
        commit=commit,
        tree=tree,
        local_path=local_path,
        files=files,
    )


def _git_environment() -> Mapping[str, str]:
    environment = os.environ.copy()
    environment.update(
        {
            "GIT_NO_LAZY_FETCH": "1",
            "GIT_NO_REPLACE_OBJECTS": "1",
            "GIT_OPTIONAL_LOCKS": "0",
            "GIT_TERMINAL_PROMPT": "0",
        }
    )
    return environment


def _run_git(checkout: Path, arguments: Sequence[str]) -> bytes:
    command = ["git", "-C", str(checkout), *arguments]
    try:
        completed = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=_git_environment(),
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        if isinstance(exc, subprocess.CalledProcessError):
            detail = exc.stderr.decode("utf-8", errors="replace").strip()
        else:
            detail = str(exc)
        raise AuditError(f"local Git command failed ({' '.join(arguments)}): {detail}") from exc
    return completed.stdout


def _git_text(checkout: Path, arguments: Sequence[str]) -> str:
    return _run_git(checkout, arguments).decode("utf-8", errors="strict").strip()


def validate_checkout(spec: VendorSpec, checkout: Path) -> CheckoutInfo:
    requested_root = checkout.resolve()
    actual_root = Path(_git_text(requested_root, ["rev-parse", "--show-toplevel"])).resolve()
    if actual_root != requested_root:
        raise AuditError(f"upstream checkout must be its Git root: expected {actual_root}, got {requested_root}")

    object_format = _git_text(actual_root, ["rev-parse", "--show-object-format"])
    if object_format != "sha1":
        raise AuditError(f"unsupported Git object format {object_format!r}; manifest schema 1 requires sha1")

    origin = _git_text(actual_root, ["remote", "get-url", "origin"])
    if origin != spec.repository:
        raise AuditError(f"origin URL mismatch: expected {spec.repository}, got {origin}")

    head = _git_text(actual_root, ["rev-parse", "--verify", "HEAD^{commit}"])
    if head != spec.commit:
        raise AuditError(f"HEAD mismatch: expected {spec.commit}, got {head}")

    tag_commit = _git_text(actual_root, ["rev-parse", "--verify", f"refs/tags/{spec.tag}^{{commit}}"])
    if tag_commit != spec.commit:
        raise AuditError(f"tag {spec.tag} mismatch: expected {spec.commit}, got {tag_commit}")

    tree = _git_text(actual_root, ["rev-parse", "--verify", "HEAD^{tree}"])
    if tree != spec.tree:
        raise AuditError(f"tree mismatch: expected {spec.tree}, got {tree}")

    return CheckoutInfo(root=actual_root, head=head, tree=tree, origin=origin)


def read_upstream_files(
    checkout: CheckoutInfo,
) -> Tuple[Mapping[str, str], Mapping[str, str]]:
    output = _run_git(checkout.root, ["ls-tree", "-r", "-z", "--full-tree", checkout.head])
    blobs: Dict[str, str] = {}
    modes: Dict[str, str] = {}
    for record in output.split(b"\0"):
        if not record:
            continue
        try:
            metadata, raw_path = record.split(b"\t", 1)
            raw_mode, object_type, raw_object_id = metadata.split(b" ", 2)
        except ValueError as exc:
            raise AuditError("unexpected git ls-tree record") from exc
        path = raw_path.decode("utf-8", errors="surrogateescape")
        _validate_relative_path(path, f"upstream tree path {path!r}")
        if object_type != b"blob":
            raise AuditError(f"unsupported upstream object type for {path}: {object_type.decode()}")
        mode = raw_mode.decode("ascii")
        if mode not in GIT_BLOB_MODES:
            raise AuditError(f"unsupported upstream Git mode for {path}: {mode}")
        blobs[path] = raw_object_id.decode("ascii")
        modes[path] = mode
    return blobs, modes


def git_blob_id(content: bytes) -> str:
    header = f"blob {len(content)}\0".encode("ascii")
    return hashlib.sha1(header + content).hexdigest()


def _local_file_content(path: Path) -> bytes:
    if path.is_symlink():
        return os.readlink(path).encode("utf-8", errors="surrogateescape")
    if not path.is_file():
        raise AuditError(f"unsupported local filesystem entry: {path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise AuditError(f"cannot read local vendor file {path}: {exc}") from exc


def local_git_mode(path: Path) -> str:
    if path.is_symlink():
        return "120000"
    if not path.is_file():
        raise AuditError(f"unsupported local filesystem entry: {path}")
    try:
        executable = bool(path.stat().st_mode & 0o111)
    except OSError as exc:
        raise AuditError(f"cannot stat local vendor file {path}: {exc}") from exc
    return "100755" if executable else "100644"


def scan_local_files(
    local_root: Path,
) -> Tuple[Mapping[str, str], Mapping[str, str]]:
    if not local_root.is_dir():
        raise AuditError(f"local vendor path is not a directory: {local_root}")

    blobs: Dict[str, str] = {}
    modes: Dict[str, str] = {}
    for current_root, raw_directories, raw_files in os.walk(local_root, followlinks=False):
        current = Path(current_root)
        directories = sorted(raw_directories)
        raw_directories[:] = []
        symlink_directories: List[str] = []
        for directory in directories:
            child = current / directory
            if directory == ".git":
                continue
            if child.is_symlink():
                symlink_directories.append(directory)
            else:
                raw_directories.append(directory)

        for filename in sorted([*raw_files, *symlink_directories]):
            if filename == ".git":
                continue
            path = current / filename
            relative = path.relative_to(local_root).as_posix()
            _validate_relative_path(relative, f"local vendor path {relative!r}")
            blobs[relative] = git_blob_id(_local_file_content(path))
            modes[relative] = local_git_mode(path)
    return blobs, modes


def _classify(
    upstream_blobs: Mapping[str, str],
    upstream_modes: Mapping[str, str],
    local_blobs: Mapping[str, str],
    local_modes: Mapping[str, str],
) -> Tuple[Inventory, Mapping[str, str]]:
    by_status: Dict[str, List[str]] = {status: [] for status in STATUSES}
    actual_status: Dict[str, str] = {}
    for path in sorted(set(upstream_blobs) | set(local_blobs)):
        upstream_blob = upstream_blobs.get(path)
        local_blob = local_blobs.get(path)
        if upstream_blob is None:
            status = "local-only"
        elif local_blob is None:
            status = "upstream-only"
        elif upstream_blob == local_blob and upstream_modes[path] == local_modes[path]:
            status = "exact"
        else:
            status = "modified"
        by_status[status].append(path)
        actual_status[path] = status

    inventory = Inventory(
        exact=tuple(by_status["exact"]),
        modified=tuple(by_status["modified"]),
        local_only=tuple(by_status["local-only"]),
        upstream_only=tuple(by_status["upstream-only"]),
    )
    return inventory, actual_status


def audit_vendor(spec: VendorSpec, checkout_path: Path, repo_root: Path) -> AuditResult:
    checkout = validate_checkout(spec, checkout_path)
    resolved_repo_root = repo_root.resolve()
    local_root = (resolved_repo_root / Path(*spec.local_path.parts)).resolve()
    try:
        local_root.relative_to(resolved_repo_root)
    except ValueError as exc:
        raise AuditError(f"local vendor path escapes repository root: {local_root}") from exc

    upstream_blobs, upstream_modes = read_upstream_files(checkout)
    local_blobs, local_modes = scan_local_files(local_root)
    inventory, actual_status = _classify(
        upstream_blobs,
        upstream_modes,
        local_blobs,
        local_modes,
    )

    drift: List[str] = []
    actual_paths = set(actual_status)
    expected_paths = set(spec.files)
    for path in sorted(actual_paths - expected_paths):
        drift.append(f"unclassified {actual_status[path]} path: {path}")
    for path in sorted(expected_paths - actual_paths):
        drift.append(f"classified path missing from both trees: {path}")
    for path in sorted(actual_paths & expected_paths):
        expectation = spec.files[path]
        actual = actual_status[path]
        if actual != expectation.status:
            drift.append(f"status drift for {path}: expected {expectation.status}, got {actual}")
            continue
        if expectation.local_blob is not None:
            actual_local_blob = local_blobs.get(path)
            if actual_local_blob != expectation.local_blob:
                drift.append(
                    f"local content drift for {path}: expected {expectation.local_blob}, got {actual_local_blob}"
                )
        if expectation.local_mode is not None:
            actual_local_mode = local_modes.get(path)
            if actual_local_mode != expectation.local_mode:
                drift.append(
                    f"local mode drift for {path}: expected {expectation.local_mode}, "
                    f"got {actual_local_mode}"
                )

    return AuditResult(
        spec=spec,
        checkout=checkout,
        inventory=inventory,
        upstream_blobs=upstream_blobs,
        upstream_modes=upstream_modes,
        local_blobs=local_blobs,
        local_modes=local_modes,
        drift=tuple(drift),
    )


def _source_tokens(content: bytes) -> List[str]:
    text = content.decode("utf-8", errors="replace")
    tokens: List[str] = []
    for match in SOURCE_TOKEN_RE.finditer(text):
        token = match.group(0)
        if token.startswith("//") or token.startswith("/*"):
            continue
        tokens.append(token + "\n")
    return tokens


def read_upstream_blob(checkout: CheckoutInfo, object_id: str) -> bytes:
    _validate_sha1(object_id, "upstream blob")
    return _run_git(checkout.root, ["cat-file", "blob", object_id])


def semantic_diff_for_path(result: AuditResult, repo_root: Path, path: str) -> str:
    if path not in result.inventory.modified:
        raise AuditError(f"semantic diff requires a modified path: {path}")
    if PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES:
        raise AuditError(f"semantic diff is supported only for source files: {path}")

    upstream_content = read_upstream_blob(result.checkout, result.upstream_blobs[path])
    local_path = repo_root.resolve() / Path(*result.spec.local_path.parts) / Path(*PurePosixPath(path).parts)
    local_content = _local_file_content(local_path)
    return "".join(
        difflib.unified_diff(
            _source_tokens(upstream_content),
            _source_tokens(local_content),
            fromfile=f"upstream/{path}",
            tofile=f"local/{path}",
            n=3,
        )
    )


def _format_inventory(result: AuditResult) -> Iterable[str]:
    for status in STATUSES:
        paths = result.inventory.paths_for(status)
        yield f"{status} ({len(paths)}):"
        for path in paths:
            yield f"  {path}"


def print_report(result: AuditResult, repo_root: Path, include_semantic_diff: bool) -> None:
    print(f"vendor: {result.spec.name}")
    print(f"repository: {result.spec.repository}")
    print(f"tag: {result.spec.tag}")
    print(f"commit: {result.spec.commit}")
    print(f"tree: {result.spec.tree}")
    print(f"local-path: {result.spec.local_path.as_posix()}")
    for line in _format_inventory(result):
        print(line)

    if include_semantic_diff:
        source_paths = [
            path
            for path in result.inventory.modified
            if PurePosixPath(path).suffix.lower() in SOURCE_SUFFIXES
        ]
        print("semantic-diff:")
        for path in source_paths:
            print(f"  {path}:")
            diff = semantic_diff_for_path(result, repo_root, path)
            if diff:
                for line in diff.rstrip("\n").splitlines():
                    print(f"    {line}")
            else:
                print("    (no non-comment token differences)")

    if result.drift:
        print("drift:")
        for message in result.drift:
            print(f"  {message}")
        print("audit: FAILED")
    else:
        print("audit: ok")


def _build_parser() -> argparse.ArgumentParser:
    default_manifest = Path(__file__).with_name("sources.json")
    default_repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vendor", required=True, help="Vendor key from the manifest")
    parser.add_argument("--upstream", required=True, type=Path, help="Caller-supplied upstream Git checkout root")
    parser.add_argument("--manifest", type=Path, default=default_manifest, help="Pinned vendor manifest")
    parser.add_argument("--repo-root", type=Path, default=default_repo_root, help="Repository containing the local vendor subtree")
    parser.add_argument(
        "--semantic-diff",
        action="store_true",
        help="Show a whitespace/comment-insensitive token diff for modified source files",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        spec = load_vendor_spec(args.manifest, args.vendor)
        result = audit_vendor(spec, args.upstream, args.repo_root)
        print_report(result, args.repo_root, args.semantic_diff)
    except AuditError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
