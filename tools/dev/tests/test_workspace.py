from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools.dev.workspace import (
    MODULE_REPO_ROOT,
    OutsideRepositoryError,
    Workspace,
    WorkspaceAmbiguityError,
    WorkspaceNotFoundError,
    accepted_workspace_selectors,
    list_workspaces,
    repo_root_from_module,
)


class WorkspaceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.temporary_root = Path(self.temporary_directory.name)
        self.repo = self.temporary_root / "main-checkout"
        self._create_repository(self.repo)

        self.worktree_a = self.temporary_root / "slot-a" / "checkout"
        self.worktree_b = self.temporary_root / "slot-b" / "checkout"
        self.detached_worktree = self.temporary_root / "slot-detached" / "detached"
        self._add_worktree(self.worktree_a, branch="feature/alpha")
        self._add_worktree(self.worktree_b, branch="feature/beta")
        self._add_detached_worktree(self.detached_worktree)

    def _git(self, cwd: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(cwd), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )

    def _create_repository(self, path: Path) -> None:
        path.mkdir(parents=True)
        subprocess.run(
            ["git", "init", "-q", "-b", "main", str(path)],
            check=True,
            capture_output=True,
            text=True,
        )
        self._git(path, "config", "user.email", "dev-tests@example.invalid")
        self._git(path, "config", "user.name", "Dev Tests")
        (path / "README.md").write_text("workspace fixture\n", encoding="utf-8")
        self._git(path, "add", "README.md")
        self._git(path, "commit", "-q", "-m", "fixture")

    def _add_worktree(self, path: Path, *, branch: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._git(self.repo, "worktree", "add", "-q", "-b", branch, str(path))

    def _add_detached_worktree(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._git(self.repo, "worktree", "add", "-q", "--detach", str(path), "HEAD")

    def test_module_root_is_derived_from_workspace_module(self) -> None:
        self.assertEqual(repo_root_from_module(), MODULE_REPO_ROOT)
        self.assertEqual(MODULE_REPO_ROOT, Path(__file__).resolve().parents[3])

    def test_discover_current_workspace_reports_clean_branch_and_head(self) -> None:
        workspace = Workspace.discover(self.repo)

        expected_head = self._git(self.repo, "rev-parse", "HEAD").stdout.strip()
        self.assertEqual(workspace.root, self.repo.resolve())
        self.assertEqual(workspace.branch, "main")
        self.assertEqual(workspace.branch_name, "main")
        self.assertEqual(workspace.head, expected_head)
        self.assertFalse(workspace.dirty)
        self.assertIn(expected_head[:12], workspace.describe())
        self.assertIn("clean", workspace.describe())

    def test_selectors_support_exact_path_parent_basename_and_branch(self) -> None:
        exact = Workspace.discover(self.repo, self.worktree_a.resolve())
        by_parent = Workspace.discover(self.repo, "slot-a")
        by_branch = Workspace.discover(self.repo, "feature/alpha")

        self.assertEqual(exact.root, self.worktree_a.resolve())
        self.assertEqual(by_parent, exact)
        self.assertEqual(by_branch, exact)
        self.assertEqual(exact.common_git_dir, Workspace.discover(self.repo).common_git_dir)

    def test_duplicate_worktree_basename_is_ambiguous(self) -> None:
        with self.assertRaises(WorkspaceAmbiguityError) as context:
            Workspace.discover(self.repo, "checkout")

        message = str(context.exception)
        self.assertIn(str(self.worktree_a), message)
        self.assertIn(str(self.worktree_b), message)

    def test_detached_workspace_is_reported_explicitly(self) -> None:
        workspace = Workspace.discover(self.repo, "slot-detached")

        self.assertIsNone(workspace.branch)
        self.assertEqual(workspace.branch_name, "detached")
        self.assertIn("detached", workspace.describe())

    def test_dirty_workspace_is_reported(self) -> None:
        (self.worktree_a / "untracked.txt").write_text("dirty\n", encoding="utf-8")

        workspace = Workspace.discover(self.repo, "feature/alpha")

        self.assertTrue(workspace.dirty)
        self.assertIn("dirty", workspace.describe())

    def test_absolute_path_in_another_repository_is_rejected(self) -> None:
        other_repo = self.temporary_root / "other-repo"
        self._create_repository(other_repo)

        with self.assertRaises(OutsideRepositoryError):
            Workspace.discover(self.repo, other_repo.resolve())

    def test_subdirectory_is_not_accepted_as_an_exact_worktree_path(self) -> None:
        subdirectory = self.worktree_a / "nested"
        subdirectory.mkdir()

        with self.assertRaises(WorkspaceNotFoundError):
            Workspace.discover(self.repo, subdirectory.resolve())

    def test_unknown_and_empty_selectors_are_rejected(self) -> None:
        with self.assertRaises(WorkspaceNotFoundError):
            Workspace.discover(self.repo, "does-not-exist")
        with self.assertRaises(WorkspaceNotFoundError):
            Workspace.discover(self.repo, "")

    def test_list_workspaces_is_sorted_and_reports_all_registered_roots(self) -> None:
        workspaces = list_workspaces(self.repo)

        roots = [workspace.root for workspace in workspaces]
        self.assertEqual(roots, sorted(roots, key=str))
        self.assertEqual(
            set(roots),
            {
                self.repo.resolve(),
                self.worktree_a.resolve(),
                self.worktree_b.resolve(),
                self.detached_worktree.resolve(),
            },
        )

    def test_list_workspaces_ignores_removed_prunable_registration(self) -> None:
        removed = self.temporary_root / "removed-evidence-worktree"
        self._add_detached_worktree(removed)
        shutil.rmtree(removed)

        roots = [workspace.root for workspace in list_workspaces(self.repo)]

        self.assertNotIn(removed.resolve(), roots)
        self.assertIn(self.repo.resolve(), roots)

    def test_accepted_selectors_lead_with_exact_path_and_omit_ambiguous_aliases(self) -> None:
        workspaces = list_workspaces(self.repo)
        selected = next(item for item in workspaces if item.root == self.worktree_a.resolve())
        selectors = accepted_workspace_selectors(selected, workspaces)

        self.assertEqual(selectors[0], str(self.worktree_a.resolve()))
        self.assertIn("slot-a", selectors)
        self.assertIn("feature/alpha", selectors)
        self.assertNotIn("checkout", selectors)


if __name__ == "__main__":
    unittest.main()
