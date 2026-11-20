"""Git-worktree discovery for the repository's developer command surface.

The helpers in this module deliberately invoke Git with argument vectors only.
They do not depend on the caller's editor, shell configuration, or current
checkout once a repository root has been supplied.
"""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
from typing import Final, Union


PathLike = Union[str, os.PathLike[str]]
MODULE_REPO_ROOT: Final[Path] = Path(__file__).resolve().parents[2]


class WorkspaceError(RuntimeError):
    """Base class for workspace discovery failures."""


class WorkspaceNotFoundError(WorkspaceError):
    """Raised when a selector does not identify a registered worktree."""


class WorkspaceAmbiguityError(WorkspaceError):
    """Raised when a short selector identifies more than one worktree."""


class OutsideRepositoryError(WorkspaceError):
    """Raised when an explicit path belongs to another repository."""


@dataclass(frozen=True)
class Workspace:
    """A verified Git worktree and the state useful to command diagnostics."""

    root: Path
    common_git_dir: Path
    branch: str | None
    head: str
    dirty: bool

    @property
    def branch_name(self) -> str:
        """Return the short branch name, or ``detached`` for detached HEADs."""

        return self.branch if self.branch is not None else "detached"

    def describe(self) -> str:
        """Return a compact, stable human-readable workspace description."""

        state = "dirty" if self.dirty else "clean"
        return f"{self.branch_name} {self.head[:12]} {state} {self.root}"

    @classmethod
    def discover(
        cls,
        repo_root: PathLike | None = None,
        selector: PathLike | None = None,
    ) -> "Workspace":
        """Resolve ``selector`` within the repository containing ``repo_root``.

        A selector may be an exact worktree path, a worktree basename, its
        parent-directory basename, or its short branch name. Short selectors
        must identify exactly one registered worktree. An explicit path into a
        different repository is rejected rather than treated as a generic Git
        checkout.
        """

        return resolve_workspace(repo_root=repo_root, selector=selector)


def repo_root_from_module() -> Path:
    """Return the checkout root containing this module."""

    return MODULE_REPO_ROOT


def _run_git(
    cwd: Path,
    *arguments: str,
    allowed_returncodes: tuple[int, ...] = (0,),
) -> subprocess.CompletedProcess[str]:
    command = ["git", "-C", str(cwd), *arguments]
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as error:
        raise WorkspaceError("git was not found in PATH") from error

    if result.returncode not in allowed_returncodes:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown Git error"
        raise WorkspaceError(f"Git command failed ({' '.join(arguments)}): {detail}")
    return result


def _canonical_existing_directory(path: PathLike) -> Path:
    candidate = Path(path).expanduser()
    try:
        resolved = candidate.resolve(strict=True)
    except FileNotFoundError as error:
        raise WorkspaceNotFoundError(f"workspace path does not exist: {candidate}") from error
    if not resolved.is_dir():
        raise WorkspaceNotFoundError(f"workspace path is not a directory: {resolved}")
    return resolved


def _git_toplevel(path: PathLike) -> Path:
    directory = _canonical_existing_directory(path)
    result = _run_git(directory, "rev-parse", "--show-toplevel")
    return Path(result.stdout.strip()).resolve(strict=True)


def _git_common_dir(worktree_root: Path) -> Path:
    result = _run_git(worktree_root, "rev-parse", "--git-common-dir")
    raw_path = Path(result.stdout.strip())
    if not raw_path.is_absolute():
        raw_path = worktree_root / raw_path
    return raw_path.resolve(strict=True)


def _inspect_workspace(worktree_root: Path, expected_common_dir: Path) -> Workspace:
    root = _git_toplevel(worktree_root)
    if root != worktree_root.resolve(strict=True):
        raise WorkspaceNotFoundError(
            f"selector must identify a worktree root, not a subdirectory: {worktree_root}"
        )

    common_git_dir = _git_common_dir(root)
    if common_git_dir != expected_common_dir:
        raise OutsideRepositoryError(
            f"worktree belongs to another repository: {root} "
            f"(common Git directory {common_git_dir})"
        )

    head = _run_git(root, "rev-parse", "--verify", "HEAD").stdout.strip()
    branch_result = _run_git(
        root,
        "symbolic-ref",
        "--quiet",
        "--short",
        "HEAD",
        allowed_returncodes=(0, 1),
    )
    branch = branch_result.stdout.strip() if branch_result.returncode == 0 else None
    status = _run_git(
        root,
        "status",
        "--porcelain=v1",
        "--untracked-files=normal",
    ).stdout
    return Workspace(
        root=root,
        common_git_dir=common_git_dir,
        branch=branch,
        head=head,
        dirty=bool(status),
    )


def _registered_worktree_roots(anchor: Path) -> tuple[Path, ...]:
    result = _run_git(anchor, "worktree", "list", "--porcelain", "-z")
    roots: list[Path] = []
    for field in result.stdout.split("\0"):
        if field.startswith("worktree "):
            registered = Path(field.removeprefix("worktree "))
            try:
                root = registered.resolve(strict=True)
            except FileNotFoundError:
                # Git retains prunable registrations after temporary evidence
                # worktrees are removed. They must not make every unrelated
                # build, monitor, or guarded flash command unusable.
                continue
            if not (root / ".git").exists() or root in roots:
                continue
            roots.append(root)
    if not roots:
        raise WorkspaceError(f"Git reported no worktrees for {anchor}")
    return tuple(roots)


def list_workspaces(repo_root: PathLike | None = None) -> tuple[Workspace, ...]:
    """Return every registered worktree in deterministic path order."""

    anchor_root = _git_toplevel(repo_root if repo_root is not None else MODULE_REPO_ROOT)
    common_git_dir = _git_common_dir(anchor_root)
    workspaces = [
        _inspect_workspace(root, common_git_dir)
        for root in _registered_worktree_roots(anchor_root)
    ]
    workspaces.sort(key=lambda workspace: os.fspath(workspace.root))
    return tuple(workspaces)


def accepted_workspace_selectors(
    workspace: Workspace,
    all_workspaces: tuple[Workspace, ...],
) -> tuple[str, ...]:
    """Return the exact path plus every unambiguous accepted short selector."""

    if workspace not in all_workspaces:
        raise WorkspaceNotFoundError("workspace is not in the registered worktree set")
    candidates: list[str] = [workspace.root.name, workspace.root.parent.name]
    if workspace.branch is not None:
        candidates.append(workspace.branch)

    accepted = [os.fspath(workspace.root)]
    for candidate in candidates:
        if candidate in accepted:
            continue
        matches = [
            item
            for item in all_workspaces
            if candidate in {item.root.name, item.root.parent.name, item.branch}
        ]
        if matches == [workspace]:
            accepted.append(candidate)
    return tuple(accepted)


def _looks_like_explicit_path(selector: PathLike) -> bool:
    if isinstance(selector, os.PathLike):
        return True
    value = os.fspath(selector)
    path = Path(value).expanduser()
    return (
        path.is_absolute()
        or value.startswith(".")
        or os.sep in value
        or (os.altsep is not None and os.altsep in value)
    )


def _resolve_explicit_path(selector: PathLike, anchor_root: Path) -> Path:
    path = Path(selector).expanduser()
    if not path.is_absolute():
        path = anchor_root / path
    return _canonical_existing_directory(path)


def resolve_workspace(
    repo_root: PathLike | None = None,
    selector: PathLike | None = None,
) -> Workspace:
    """Resolve one same-repository workspace from an exact or short selector."""

    anchor_root = _git_toplevel(repo_root if repo_root is not None else MODULE_REPO_ROOT)
    common_git_dir = _git_common_dir(anchor_root)
    if selector is None:
        return _inspect_workspace(anchor_root, common_git_dir)

    selector_text = os.fspath(selector)
    if not selector_text:
        raise WorkspaceNotFoundError("workspace selector must not be empty")

    workspaces = list_workspaces(anchor_root)

    # An absolute path is the strongest selector and never competes with short
    # aliases that happen to contain the same text.
    selector_path = Path(selector_text).expanduser()
    if selector_path.is_absolute():
        explicit_root = _resolve_explicit_path(selector_path, anchor_root)
        explicit_toplevel = _git_toplevel(explicit_root)
        explicit_common_dir = _git_common_dir(explicit_toplevel)
        if explicit_common_dir != common_git_dir:
            raise OutsideRepositoryError(
                f"workspace path belongs to another repository: {explicit_root}"
            )
        if explicit_root != explicit_toplevel:
            raise WorkspaceNotFoundError(
                f"selector must identify a worktree root, not a subdirectory: {explicit_root}"
            )
        for workspace in workspaces:
            if workspace.root == explicit_root:
                return workspace
        raise WorkspaceNotFoundError(f"path is not a registered worktree: {explicit_root}")

    matches = [
        workspace
        for workspace in workspaces
        if selector_text
        in {
            workspace.root.name,
            workspace.root.parent.name,
            workspace.branch,
        }
    ]
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        descriptions = "; ".join(workspace.describe() for workspace in matches)
        raise WorkspaceAmbiguityError(
            f"ambiguous workspace selector {selector_text!r}: {descriptions}"
        )

    # Path objects and dot-relative strings explicitly request path semantics.
    # Branch names containing slashes remain valid because branch matching ran
    # first.
    if _looks_like_explicit_path(selector):
        explicit_root = _resolve_explicit_path(selector, anchor_root)
        explicit_toplevel = _git_toplevel(explicit_root)
        explicit_common_dir = _git_common_dir(explicit_toplevel)
        if explicit_common_dir != common_git_dir:
            raise OutsideRepositoryError(
                f"workspace path belongs to another repository: {explicit_root}"
            )
        if explicit_root != explicit_toplevel:
            raise WorkspaceNotFoundError(
                f"selector must identify a worktree root, not a subdirectory: {explicit_root}"
            )
        for workspace in workspaces:
            if workspace.root == explicit_root:
                return workspace

    raise WorkspaceNotFoundError(
        f"unknown workspace selector {selector_text!r}; choose an exact path, "
        "worktree basename, parent basename, or branch"
    )
