from __future__ import annotations

import json
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from tools.dev import locks as locks_module
from tools.dev.locks import (
    LockError,
    LockManager,
    LockTimeoutError,
    default_state_root,
    normalize_device_identity,
)


REPO_ROOT = Path(__file__).resolve().parents[3]
HOLDER_PROGRAM = r"""
import os
from pathlib import Path
from types import SimpleNamespace

from tools.dev.locks import LockManager

workspace = SimpleNamespace(
    root=Path(os.environ["TEST_CHECKOUT"]),
    common_git_dir=Path(os.environ["TEST_COMMON_GIT_DIR"]),
    branch=os.environ.get("TEST_BRANCH") or None,
    head=os.environ["TEST_HEAD"],
)
kind = os.environ["TEST_LOCK_KIND"]
identity = os.environ["TEST_LOCK_IDENTITY"]
state_root = os.environ.get("TEST_STATE_ROOT")
manager = LockManager(
    workspace,
    state_root=Path(state_root) if state_root is not None else None,
    argv=["holder", kind, identity],
)
lock = (
    manager.lane(identity, timeout=2.0)
    if kind == "lane"
    else manager.device(identity, timeout=2.0)
)
with lock:
    print("LOCKED", flush=True)
    input()
"""


class LockManagerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.temporary_root = Path(self.temporary_directory.name)
        self.state_root = self.temporary_root / "state"

        self.common_git_dir = self.temporary_root / "repo-a" / ".git"
        self.common_git_dir.mkdir(parents=True)
        self.checkout_a = self.temporary_root / "repo-a" / "checkout-a"
        self.checkout_b = self.temporary_root / "repo-a" / "checkout-b"
        self.checkout_a.mkdir()
        self.checkout_b.mkdir()
        self.workspace_a = self._workspace(self.checkout_a, self.common_git_dir, "main")
        self.workspace_b = self._workspace(self.checkout_b, self.common_git_dir, "feature")

        self.other_common_git_dir = self.temporary_root / "repo-b" / ".git"
        self.other_common_git_dir.mkdir(parents=True)
        self.other_checkout = self.temporary_root / "repo-b" / "checkout"
        self.other_checkout.mkdir()
        self.other_workspace = self._workspace(
            self.other_checkout,
            self.other_common_git_dir,
            "other",
        )
        self.holders: list[subprocess.Popen[str]] = []
        self.addCleanup(self._stop_all_holders)

    @staticmethod
    def _workspace(root: Path, common_git_dir: Path, branch: str | None):
        return SimpleNamespace(
            root=root,
            common_git_dir=common_git_dir,
            branch=branch,
            head="0123456789abcdef0123456789abcdef01234567",
            dirty=False,
        )

    def _start_holder(
        self,
        *,
        kind: str,
        identity: str,
        workspace=None,
        use_default_state_root: bool = False,
        tmpdir: Path | None = None,
        legacy_state_dir: Path | None = None,
    ) -> subprocess.Popen[str]:
        active_workspace = workspace if workspace is not None else self.workspace_a
        environment = os.environ.copy()
        environment.update(
            {
                "PYTHONPATH": str(REPO_ROOT),
                "TEST_CHECKOUT": str(active_workspace.root),
                "TEST_COMMON_GIT_DIR": str(active_workspace.common_git_dir),
                "TEST_BRANCH": active_workspace.branch or "",
                "TEST_HEAD": active_workspace.head,
                "TEST_LOCK_KIND": kind,
                "TEST_LOCK_IDENTITY": identity,
            }
        )
        if use_default_state_root:
            environment.pop("TEST_STATE_ROOT", None)
        else:
            environment["TEST_STATE_ROOT"] = str(self.state_root)
        if tmpdir is not None:
            environment["TMPDIR"] = str(tmpdir)
        if legacy_state_dir is not None:
            environment["LITCH_DEV_STATE_DIR"] = str(legacy_state_dir)
        process = subprocess.Popen(
            [sys.executable, "-c", HOLDER_PROGRAM],
            cwd=REPO_ROOT,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self.holders.append(process)

        assert process.stdout is not None
        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ)
        try:
            ready = selector.select(timeout=5.0)
        finally:
            selector.close()
        if not ready:
            self._stop_holder(process)
            self.fail("subprocess lock holder did not become ready")

        line = process.stdout.readline()
        if line != "LOCKED\n":
            assert process.stderr is not None
            error = process.stderr.read()
            self._stop_holder(process)
            self.fail(f"subprocess lock holder failed: {line!r} {error}")
        return process

    def _stop_holder(self, process: subprocess.Popen[str]) -> None:
        if process.poll() is None:
            try:
                if process.stdin is not None:
                    process.stdin.write("\n")
                    process.stdin.flush()
                process.wait(timeout=2.0)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2.0)
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream is not None and not stream.closed:
                stream.close()

    def _stop_all_holders(self) -> None:
        for process in self.holders:
            self._stop_holder(process)

    def _manager(self, workspace=None) -> LockManager:
        return LockManager(
            workspace if workspace is not None else self.workspace_a,
            state_root=self.state_root,
            argv=["test-dev", "command"],
        )

    def test_state_root_override_and_repository_hash_namespace(self) -> None:
        override = self.temporary_root / "override"
        manager = LockManager(self.workspace_a, state_root=override)

        self.assertEqual(manager.state_root, override.resolve())
        self.assertEqual(manager.repo_state_dir.parent.parent, override.resolve())
        self.assertEqual(manager.repo_state_dir.parent.name, "repos")
        self.assertEqual(len(manager.repository_key), 64)
        self.assertNotIn(str(self.common_git_dir), str(manager.repo_state_dir))

    def test_default_state_root_is_stable_across_tmpdir_environments(self) -> None:
        first_tmpdir = self.temporary_root / "tmp-a"
        second_tmpdir = self.temporary_root / "tmp-b"
        first_tmpdir.mkdir()
        second_tmpdir.mkdir()
        with mock.patch.dict(
            os.environ,
            {
                "TMPDIR": str(first_tmpdir),
                "LITCH_DEV_STATE_DIR": str(self.temporary_root / "legacy-a"),
            },
            clear=False,
        ):
            first = default_state_root()
        with mock.patch.dict(
            os.environ,
            {
                "TMPDIR": str(second_tmpdir),
                "LITCH_DEV_STATE_DIR": str(self.temporary_root / "legacy-b"),
            },
            clear=False,
        ):
            second = default_state_root()

        self.assertEqual(first, second)
        self.assertEqual(first, Path("/tmp").resolve() / f"litch-dev-{os.geteuid()}")

    def test_default_lock_contends_across_different_tmpdir_environments(self) -> None:
        first_tmpdir = self.temporary_root / "tmp-a"
        second_tmpdir = self.temporary_root / "tmp-b"
        first_tmpdir.mkdir()
        second_tmpdir.mkdir()
        holder = self._start_holder(
            kind="lane",
            identity="cross-environment",
            use_default_state_root=True,
            tmpdir=first_tmpdir,
            legacy_state_dir=self.temporary_root / "legacy-a",
        )
        with mock.patch.dict(
            os.environ,
            {
                "TMPDIR": str(second_tmpdir),
                "LITCH_DEV_STATE_DIR": str(self.temporary_root / "legacy-b"),
            },
            clear=False,
        ):
            manager = LockManager(self.workspace_b, argv=["contender"])
            with self.assertRaises(LockTimeoutError) as context:
                with manager.lane(
                    "cross-environment",
                    timeout=0.1,
                    poll_interval=0.01,
                ):
                    self.fail("different TMPDIR bypassed the stable lane lock")
        self.assertEqual(context.exception.owner["pid"], holder.pid)  # type: ignore[index]

    def test_explicit_test_state_root_override_must_be_absolute_and_scoped(self) -> None:
        with self.assertRaisesRegex(ValueError, "absolute"):
            LockManager(self.workspace_a, state_root="relative-state")
        with self.assertRaisesRegex(ValueError, "filesystem root"):
            LockManager(self.workspace_a, state_root=Path("/"))

    def test_state_root_symlink_is_rejected_without_touching_its_target(self) -> None:
        attacker_target = self.temporary_root / "attacker-target"
        attacker_target.mkdir()
        attacker_target.chmod(0o700)
        sentinel = attacker_target / "sentinel.txt"
        sentinel.write_text("DO NOT CHANGE", encoding="utf-8")
        sentinel.chmod(0o600)
        hostile_root = self.temporary_root / "hostile-state"
        hostile_root.symlink_to(attacker_target, target_is_directory=True)

        manager = LockManager(self.workspace_a, state_root=hostile_root)
        with self.assertRaises(LockError):
            with manager.lane("root-symlink", timeout=0.0):
                self.fail("state-root symlink unexpectedly acquired")

        self.assertEqual(sentinel.read_text(encoding="utf-8"), "DO NOT CHANGE")
        self.assertEqual(tuple(attacker_target.iterdir()), (sentinel,))

    def test_state_root_rejects_wrong_owner_unsafe_mode_and_non_directory(self) -> None:
        wrong_owner_root = self.temporary_root / "wrong-owner-state"
        wrong_owner_root.mkdir(mode=0o700)
        actual_euid = os.geteuid()
        with mock.patch.object(
            locks_module,
            "_effective_uid",
            return_value=actual_euid + 1,
        ):
            manager = LockManager(self.workspace_a, state_root=wrong_owner_root)
            with self.assertRaisesRegex(LockError, "effective user"):
                with manager.lane("wrong-owner", timeout=0.0):
                    self.fail("wrong-owner state root unexpectedly acquired")
        self.assertFalse((wrong_owner_root / "repos").exists())

        unsafe_mode_root = self.temporary_root / "unsafe-mode-state"
        unsafe_mode_root.mkdir(mode=0o700)
        unsafe_mode_root.chmod(0o777)
        manager = LockManager(self.workspace_a, state_root=unsafe_mode_root)
        with self.assertRaisesRegex(LockError, "group/world-writable"):
            with manager.lane("unsafe-mode", timeout=0.0):
                self.fail("unsafe-mode state root unexpectedly acquired")
        self.assertFalse((unsafe_mode_root / "repos").exists())

        non_directory_root = self.temporary_root / "file-state"
        non_directory_root.write_text("DO NOT CHANGE", encoding="utf-8")
        manager = LockManager(self.workspace_a, state_root=non_directory_root)
        with self.assertRaises(LockError):
            with manager.lane("file-root", timeout=0.0):
                self.fail("non-directory state root unexpectedly acquired")
        self.assertEqual(
            non_directory_root.read_text(encoding="utf-8"),
            "DO NOT CHANGE",
        )

    def test_intermediate_directory_symlink_is_rejected_without_target_changes(self) -> None:
        state_root = self.temporary_root / "component-state"
        state_root.mkdir(mode=0o700)
        attacker_target = self.temporary_root / "component-target"
        attacker_target.mkdir()
        attacker_target.chmod(0o700)
        sentinel = attacker_target / "sentinel.txt"
        sentinel.write_text("DO NOT CHANGE", encoding="utf-8")
        sentinel.chmod(0o600)
        (state_root / "repos").symlink_to(
            attacker_target,
            target_is_directory=True,
        )

        manager = LockManager(self.workspace_a, state_root=state_root)
        with self.assertRaises(LockError):
            with manager.lane("component-symlink", timeout=0.0):
                self.fail("directory-component symlink unexpectedly acquired")

        self.assertEqual(sentinel.read_text(encoding="utf-8"), "DO NOT CHANGE")
        self.assertEqual(tuple(attacker_target.iterdir()), (sentinel,))

        file_root = self.temporary_root / "component-file-state"
        file_root.mkdir(mode=0o700)
        component_file = file_root / "repos"
        component_file.write_text("DO NOT CHANGE", encoding="utf-8")
        component_file.chmod(0o600)
        manager = LockManager(self.workspace_a, state_root=file_root)
        with self.assertRaises(LockError):
            with manager.lane("component-file", timeout=0.0):
                self.fail("regular-file directory component unexpectedly acquired")
        self.assertEqual(component_file.read_text(encoding="utf-8"), "DO NOT CHANGE")

    def test_lock_file_symlink_is_rejected_without_changing_victim(self) -> None:
        state_root = self.temporary_root / "lock-symlink-state"
        manager = LockManager(self.workspace_a, state_root=state_root)
        with manager.lane("seed", timeout=0.0):
            pass
        lease = manager.lane("lock-symlink", timeout=0.0)
        victim = self.temporary_root / "victim.txt"
        victim.write_text("DO NOT CHANGE", encoding="utf-8")
        victim.chmod(0o600)
        lease.path.symlink_to(victim)

        with self.assertRaises(LockError):
            with lease:
                self.fail("lock-file symlink unexpectedly acquired")
        self.assertEqual(victim.read_text(encoding="utf-8"), "DO NOT CHANGE")

    def test_lock_file_hard_link_is_rejected_without_changing_victim(self) -> None:
        state_root = self.temporary_root / "lock-hardlink-state"
        manager = LockManager(self.workspace_a, state_root=state_root)
        with manager.lane("seed", timeout=0.0):
            pass
        lease = manager.lane("lock-hardlink", timeout=0.0)
        victim = self.temporary_root / "hardlink-victim.txt"
        victim.write_text("DO NOT CHANGE", encoding="utf-8")
        victim.chmod(0o600)
        os.link(victim, lease.path)

        with self.assertRaisesRegex(LockError, "unsafe link count"):
            with lease:
                self.fail("hard-linked lock file unexpectedly acquired")
        self.assertEqual(victim.read_text(encoding="utf-8"), "DO NOT CHANGE")

    def test_lock_file_rejects_unsafe_mode_and_non_regular_type(self) -> None:
        unsafe_root = self.temporary_root / "unsafe-file-state"
        unsafe_manager = LockManager(self.workspace_a, state_root=unsafe_root)
        with unsafe_manager.lane("seed", timeout=0.0):
            pass
        unsafe_lease = unsafe_manager.lane("unsafe-file", timeout=0.0)
        unsafe_lease.path.write_text("DO NOT CHANGE", encoding="utf-8")
        unsafe_lease.path.chmod(0o666)
        with self.assertRaisesRegex(LockError, "group/world-writable"):
            with unsafe_lease:
                self.fail("unsafe-mode lock file unexpectedly acquired")
        self.assertEqual(
            unsafe_lease.path.read_text(encoding="utf-8"),
            "DO NOT CHANGE",
        )

        fifo_root = self.temporary_root / "fifo-state"
        fifo_manager = LockManager(self.workspace_a, state_root=fifo_root)
        with fifo_manager.lane("seed", timeout=0.0):
            pass
        fifo_lease = fifo_manager.lane("fifo", timeout=0.0)
        os.mkfifo(fifo_lease.path, mode=0o600)
        with self.assertRaises(LockError):
            with fifo_lease:
                self.fail("non-regular lock file unexpectedly acquired")

    def test_secure_lock_paths_are_created_with_private_modes(self) -> None:
        state_root = self.temporary_root / "private-state"
        manager = LockManager(self.workspace_a, state_root=state_root)
        with manager.lane("private", timeout=0.0) as lease:
            canonical_root = manager.state_root
            self.assertEqual(canonical_root.stat().st_mode & 0o777, 0o700)
            relative_parent = lease.path.parent.relative_to(canonical_root)
            current = canonical_root
            for component in relative_parent.parts:
                current /= component
                self.assertEqual(current.stat().st_mode & 0o777, 0o700)
            self.assertEqual(lease.path.stat().st_mode & 0o777, 0o600)

    def test_same_lane_contends_across_worktrees_and_reports_owner(self) -> None:
        holder = self._start_holder(kind="lane", identity="pio")
        manager = self._manager(self.workspace_b)

        started = time.monotonic()
        with self.assertRaises(LockTimeoutError) as context:
            with manager.lane("pio", timeout=0.15, poll_interval=0.01):
                self.fail("contended lock unexpectedly acquired")
        elapsed = time.monotonic() - started

        self.assertGreaterEqual(elapsed, 0.10)
        self.assertLess(elapsed, 1.0)
        error = context.exception
        self.assertEqual(error.resource, "lane:pio")
        self.assertIsNotNone(error.owner)
        assert error.owner is not None
        self.assertEqual(error.owner["pid"], holder.pid)
        self.assertEqual(error.owner["checkout"], str(self.checkout_a.resolve()))
        self.assertEqual(error.owner["branch"], "main")
        self.assertEqual(error.owner["head"], self.workspace_a.head)
        self.assertEqual(error.owner["argv"], ["holder", "lane", "pio"])
        self.assertIn("acquired_utc", error.owner)

        self._stop_holder(holder)
        with manager.lane("pio", timeout=0.2) as lease:
            self.assertTrue(lease.path.exists())
        self.assertTrue(lease.path.exists(), "lock diagnostics file must not be deleted")

    def test_pio_and_flutter_lanes_may_overlap(self) -> None:
        self._start_holder(kind="lane", identity="pio")

        with self._manager(self.workspace_b).lane("flutter", timeout=0.0) as lease:
            self.assertEqual(lease.resource, "lane:flutter")

    def test_kernel_releases_lock_after_holder_process_dies(self) -> None:
        holder = self._start_holder(kind="lane", identity="pio")
        manager = self._manager(self.workspace_b)
        lock_path = manager.lane("pio").path

        holder.terminate()
        holder.wait(timeout=2.0)

        with manager.lane("pio", timeout=0.5):
            pass
        self.assertTrue(lock_path.exists(), "stale diagnostics file should be retained")

    def test_device_lock_is_global_across_repositories_by_normalized_serial(self) -> None:
        holder = self._start_holder(
            kind="device",
            identity=" ab-cd ",
            workspace=self.workspace_a,
        )
        other_manager = self._manager(self.other_workspace)

        with self.assertRaises(LockTimeoutError) as context:
            with other_manager.device("AB-CD", timeout=0.1, poll_interval=0.01):
                self.fail("same physical device unexpectedly acquired")
        self.assertEqual(context.exception.owner["pid"], holder.pid)  # type: ignore[index]

        with other_manager.device("AB-CE", timeout=0.0):
            pass

    def test_owner_json_contains_required_diagnostics(self) -> None:
        manager = self._manager()
        with manager.lane("pio", timeout=0.0) as lease:
            owner = json.loads(lease.path.read_text(encoding="utf-8"))

        self.assertEqual(owner["pid"], os.getpid())
        self.assertEqual(owner["argv"], ["test-dev", "command"])
        self.assertEqual(owner["checkout"], str(self.checkout_a.resolve()))
        self.assertEqual(owner["branch"], "main")
        self.assertEqual(owner["head"], self.workspace_a.head)
        self.assertRegex(owner["acquired_utc"], r"Z$")

    def test_device_identity_normalization_is_exact_and_validated(self) -> None:
        self.assertEqual(normalize_device_identity("  aBc-123  "), "ABC-123")
        self.assertEqual(normalize_device_identity("ＡＢＣ"), "ABC")
        self.assertNotEqual(
            normalize_device_identity("ABC-123"),
            normalize_device_identity("ABC123"),
        )
        with self.assertRaises(ValueError):
            normalize_device_identity("  ")
        with self.assertRaises(ValueError):
            normalize_device_identity("ABC\x00")

    def test_invalid_lane_and_timeout_are_rejected_without_creating_state(self) -> None:
        manager = self._manager()
        with self.assertRaises(ValueError):
            manager.lane("pio/native")
        with self.assertRaises(ValueError):
            manager.lane("pio", timeout=-1.0)
        with self.assertRaises(ValueError):
            manager.lane("pio", timeout=float("inf"))
        self.assertFalse(self.state_root.exists())


if __name__ == "__main__":
    unittest.main()
