from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CODE_HEALTH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CODE_HEALTH_DIR))

import common  # noqa: E402


class RevisionIdentityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repo = Path(self.temporary_directory.name) / "repo"
        self.repo.mkdir()
        self._git("init", "-q")
        self._git("config", "user.name", "Code Health Test")
        self._git("config", "user.email", "code-health@example.invalid")
        (self.repo / ".gitignore").write_text("build/\n", encoding="utf-8")
        (self.repo / "tracked.txt").write_text("one\n", encoding="utf-8")
        self._git("add", ".gitignore", "tracked.txt")
        self._git("commit", "-q", "-m", "fixture")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def _git(self, *args: str) -> str:
        completed = subprocess.run(
            ["git", *args],
            cwd=self.repo,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return completed.stdout.strip()

    def test_clean_revision_has_no_worktree_fingerprint(self) -> None:
        info = common.revision_info(self.repo)

        self.assertFalse(info["dirty"])
        self.assertIsNone(info["worktree_fingerprint"])
        self.assertEqual(
            common.revision_output_root(self.repo).name,
            info["revision"][:12],
        )

    def test_dirty_output_identity_changes_with_tracked_content(self) -> None:
        (self.repo / "tracked.txt").write_text("two\n", encoding="utf-8")
        first = common.revision_info(self.repo)
        first_output = common.revision_output_root(self.repo).name
        (self.repo / "tracked.txt").write_text("three\n", encoding="utf-8")
        second = common.revision_info(self.repo)

        self.assertTrue(first["dirty"])
        self.assertNotEqual(first["worktree_fingerprint"], second["worktree_fingerprint"])
        self.assertIn(str(first["worktree_fingerprint"])[:12], first_output)

    def test_untracked_content_is_bound_but_ignored_output_is_not(self) -> None:
        (self.repo / "new.txt").write_text("alpha\n", encoding="utf-8")
        first = common.revision_info(self.repo)
        (self.repo / "new.txt").write_text("beta\n", encoding="utf-8")
        second = common.revision_info(self.repo)
        ignored = self.repo / "build" / "code_health" / "artifact.json"
        ignored.parent.mkdir(parents=True)
        ignored.write_text("ignored\n", encoding="utf-8")
        third = common.revision_info(self.repo)

        self.assertNotEqual(first["worktree_fingerprint"], second["worktree_fingerprint"])
        self.assertEqual(second, third)

    def test_untracked_executable_mode_is_bound(self) -> None:
        path = self.repo / "new-tool"
        path.write_text("#!/bin/sh\n", encoding="utf-8")
        path.chmod(0o644)
        ordinary = common.revision_info(self.repo)
        path.chmod(0o755)
        executable = common.revision_info(self.repo)

        self.assertNotEqual(
            ordinary["worktree_fingerprint"], executable["worktree_fingerprint"]
        )

    def test_output_root_can_reuse_captured_identity(self) -> None:
        captured = common.revision_info(self.repo)
        (self.repo / "tracked.txt").write_text("changed later\n", encoding="utf-8")

        self.assertEqual(
            common.revision_output_root(self.repo, captured).name,
            captured["revision"][:12],
        )


if __name__ == "__main__":
    unittest.main()
