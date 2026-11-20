"""Bounded cross-process locks for developer tool and physical-device lanes."""

from __future__ import annotations

from collections.abc import Sequence
from datetime import datetime, timezone
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import time
from types import TracebackType
from typing import Any, Protocol
import unicodedata


_LANE_PATTERN = re.compile(r"[a-z0-9][a-z0-9_.-]{0,127}\Z")
_OWNER_READ_LIMIT = 64 * 1024
_PRIVATE_DIRECTORY_MODE = 0o700
_PRIVATE_FILE_MODE = 0o600
_UNSAFE_WRITE_BITS = stat.S_IWGRP | stat.S_IWOTH


class WorkspaceLike(Protocol):
    """The workspace fields required for lock scope and diagnostics."""

    root: Path
    common_git_dir: Path
    branch: str | None
    head: str


class LockError(RuntimeError):
    """Base class for developer lock failures."""


class LockTimeoutError(TimeoutError, LockError):
    """Raised when a bounded lock acquisition cannot complete."""

    def __init__(
        self,
        *,
        resource: str,
        path: Path,
        timeout: float,
        owner: dict[str, Any] | None,
    ) -> None:
        self.resource = resource
        self.path = path
        self.timeout = timeout
        self.owner = owner

        if owner is None:
            owner_detail = "owner diagnostics unavailable"
        else:
            owner_detail = (
                f"pid={owner.get('pid', 'unknown')} "
                f"checkout={owner.get('checkout', 'unknown')} "
                f"branch={owner.get('branch', 'unknown')} "
                f"head={owner.get('head', 'unknown')} "
                f"acquired={owner.get('acquired_utc', 'unknown')} "
                f"argv={owner.get('argv', 'unknown')}"
            )
        super().__init__(
            f"timed out after {timeout:.3f}s waiting for {resource} lock "
            f"at {path}; {owner_detail}"
        )


def normalize_device_identity(serial: str) -> str:
    """Normalize an exact physical USB serial for a global device lock key.

    Only surrounding whitespace, Unicode compatibility forms, and case differ
    after normalization. Punctuation is retained so unrelated serials are not
    accidentally coalesced.
    """

    if not isinstance(serial, str):
        raise TypeError("device serial must be a string")
    normalized = unicodedata.normalize("NFKC", serial).strip().upper()
    if not normalized:
        raise ValueError("device serial must not be empty")
    if any(not character.isprintable() for character in normalized):
        raise ValueError("device serial must contain printable characters only")
    return normalized


def _canonical_path(path: os.PathLike[str] | str) -> Path:
    return Path(path).expanduser().resolve(strict=False)


def _effective_uid() -> int:
    """Return the ownership boundary used by the per-user lock namespace."""

    return os.geteuid()


def default_state_root() -> Path:
    """Return one machine-local per-user root independent of launch environment."""

    return _canonical_path("/tmp") / f"litch-dev-{_effective_uid()}"


def _validate_test_state_root(path: os.PathLike[str] | str) -> Path:
    """Validate the explicit constructor override reserved for isolated tests."""

    candidate = Path(os.path.normpath(os.fspath(Path(path).expanduser())))
    if not candidate.is_absolute():
        raise ValueError("test state_root override must be an absolute path")
    if candidate == Path(candidate.anchor):
        raise ValueError("test state_root override must not be a filesystem root")
    try:
        canonical_parent = candidate.parent.resolve(strict=True)
    except OSError as error:
        raise ValueError("test state_root parent must already exist") from error
    if not canonical_parent.is_dir():
        raise ValueError("test state_root parent must be a directory")
    # Resolve the trusted fixture parent, but preserve the final component so
    # acquisition can reject a root symlink instead of silently following it.
    return canonical_parent / candidate.name


def _required_open_flag(name: str) -> int:
    value = getattr(os, name, None)
    if not isinstance(value, int):
        raise LockError(
            f"supported hosts must provide {name} for secure workflow locks"
        )
    return value


def _directory_open_flags() -> int:
    return (
        os.O_RDONLY
        | _required_open_flag("O_DIRECTORY")
        | _required_open_flag("O_NOFOLLOW")
        | _required_open_flag("O_CLOEXEC")
    )


def _lock_file_open_flags() -> int:
    return (
        os.O_RDWR
        | _required_open_flag("O_NOFOLLOW")
        | _required_open_flag("O_CLOEXEC")
        | _required_open_flag("O_NONBLOCK")
    )


def _same_inode(left: os.stat_result, right: os.stat_result) -> bool:
    return left.st_dev == right.st_dev and left.st_ino == right.st_ino


def _revalidate_path_descriptor(path: Path, file_descriptor: int) -> None:
    try:
        opened = os.fstat(file_descriptor)
        named = os.stat(path, follow_symlinks=False)
    except OSError as error:
        raise LockError(f"could not revalidate secure lock path {path}: {error}") from error
    if not _same_inode(opened, named):
        raise LockError(f"secure lock path changed during acquisition: {path}")


def _revalidate_directory_entry(
    parent_descriptor: int,
    name: str,
    file_descriptor: int,
    path: Path,
) -> None:
    try:
        opened = os.fstat(file_descriptor)
        named = os.stat(
            name,
            dir_fd=parent_descriptor,
            follow_symlinks=False,
        )
    except OSError as error:
        raise LockError(f"could not revalidate secure lock path {path}: {error}") from error
    if not _same_inode(opened, named):
        raise LockError(f"secure lock path changed during acquisition: {path}")


def _validate_anchor_directory(file_descriptor: int, path: Path) -> None:
    try:
        metadata = os.fstat(file_descriptor)
    except OSError as error:
        raise LockError(f"could not inspect lock-state parent {path}: {error}") from error
    if not stat.S_ISDIR(metadata.st_mode):
        raise LockError(f"lock-state parent is not a directory: {path}")
    mode = stat.S_IMODE(metadata.st_mode)
    if metadata.st_uid not in (0, os.geteuid()):
        raise LockError(
            f"lock-state parent is not owned by root or effective user: {path}"
        )
    if mode & _UNSAFE_WRITE_BITS and not mode & stat.S_ISVTX:
        raise LockError(
            f"group/world-writable lock-state parent lacks the sticky bit: {path}"
        )


def _validate_owned_directory(file_descriptor: int, path: Path) -> None:
    try:
        metadata = os.fstat(file_descriptor)
    except OSError as error:
        raise LockError(f"could not inspect workflow lock directory {path}: {error}") from error
    if not stat.S_ISDIR(metadata.st_mode):
        raise LockError(f"workflow lock path is not a directory: {path}")
    if metadata.st_uid != _effective_uid():
        raise LockError(
            f"workflow lock directory is not owned by the effective user: {path}"
        )
    mode = stat.S_IMODE(metadata.st_mode)
    if mode & _UNSAFE_WRITE_BITS:
        raise LockError(f"workflow lock directory is group/world-writable: {path}")
    try:
        if mode != _PRIVATE_DIRECTORY_MODE:
            os.fchmod(file_descriptor, _PRIVATE_DIRECTORY_MODE)
        private_mode = stat.S_IMODE(os.fstat(file_descriptor).st_mode)
    except OSError as error:
        raise LockError(f"could not make workflow lock directory private: {path}") from error
    if private_mode != _PRIVATE_DIRECTORY_MODE:
        raise LockError(f"workflow lock directory did not become private: {path}")


def _validate_owned_lock_file(file_descriptor: int, path: Path) -> None:
    try:
        metadata = os.fstat(file_descriptor)
    except OSError as error:
        raise LockError(f"could not inspect workflow lock file {path}: {error}") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise LockError(f"workflow lock path is not a regular file: {path}")
    if metadata.st_uid != _effective_uid():
        raise LockError(
            f"workflow lock file is not owned by the effective user: {path}"
        )
    mode = stat.S_IMODE(metadata.st_mode)
    if mode & _UNSAFE_WRITE_BITS:
        raise LockError(f"workflow lock file is group/world-writable: {path}")
    if metadata.st_nlink != 1:
        raise LockError(f"workflow lock file has an unsafe link count: {path}")
    try:
        if mode != _PRIVATE_FILE_MODE:
            os.fchmod(file_descriptor, _PRIVATE_FILE_MODE)
        private_mode = stat.S_IMODE(os.fstat(file_descriptor).st_mode)
    except OSError as error:
        raise LockError(f"could not make workflow lock file private: {path}") from error
    if private_mode != _PRIVATE_FILE_MODE:
        raise LockError(f"workflow lock file did not become private: {path}")


def _open_anchor_directory(path: Path) -> int:
    try:
        file_descriptor = os.open(path, _directory_open_flags())
    except OSError as error:
        raise LockError(f"could not open lock-state parent {path}: {error}") from error
    try:
        _validate_anchor_directory(file_descriptor, path)
        _revalidate_path_descriptor(path, file_descriptor)
        return file_descriptor
    except BaseException:
        os.close(file_descriptor)
        raise


def _open_owned_directory_at(
    parent_descriptor: int,
    name: str,
    path: Path,
) -> int:
    try:
        os.mkdir(
            name,
            mode=_PRIVATE_DIRECTORY_MODE,
            dir_fd=parent_descriptor,
        )
    except FileExistsError:
        pass
    except OSError as error:
        raise LockError(f"could not create workflow lock directory {path}: {error}") from error

    try:
        file_descriptor = os.open(
            name,
            _directory_open_flags(),
            dir_fd=parent_descriptor,
        )
    except OSError as error:
        raise LockError(
            f"could not securely open workflow lock directory {path}: {error}"
        ) from error
    try:
        _validate_owned_directory(file_descriptor, path)
        _revalidate_directory_entry(parent_descriptor, name, file_descriptor, path)
        return file_descriptor
    except BaseException:
        os.close(file_descriptor)
        raise


def _open_owned_lock_file_at(
    parent_descriptor: int,
    name: str,
    path: Path,
) -> int:
    base_flags = _lock_file_open_flags()
    try:
        file_descriptor = os.open(
            name,
            base_flags | os.O_CREAT | os.O_EXCL,
            _PRIVATE_FILE_MODE,
            dir_fd=parent_descriptor,
        )
    except FileExistsError:
        try:
            existing = os.stat(
                name,
                dir_fd=parent_descriptor,
                follow_symlinks=False,
            )
        except OSError as error:
            raise LockError(f"could not inspect workflow lock path {path}: {error}") from error
        if not stat.S_ISREG(existing.st_mode):
            raise LockError(f"workflow lock path is not a regular file: {path}")
        try:
            file_descriptor = os.open(
                name,
                base_flags,
                dir_fd=parent_descriptor,
            )
        except OSError as error:
            raise LockError(
                f"could not securely open workflow lock file {path}: {error}"
            ) from error
    except OSError as error:
        raise LockError(f"could not create workflow lock file {path}: {error}") from error

    try:
        _validate_owned_lock_file(file_descriptor, path)
        _revalidate_directory_entry(parent_descriptor, name, file_descriptor, path)
        return file_descriptor
    except BaseException:
        os.close(file_descriptor)
        raise


def _open_secure_lock_path(
    state_root: Path,
    lock_path: Path,
) -> tuple[int, int, str]:
    try:
        relative = lock_path.relative_to(state_root)
    except ValueError as error:
        raise LockError(f"lock path escapes its state root: {lock_path}") from error
    parts = relative.parts
    if not parts or any(part in ("", ".", "..") or "/" in part for part in parts):
        raise LockError(f"lock path has an unsafe component: {lock_path}")

    anchor_descriptor = _open_anchor_directory(state_root.parent)
    try:
        current_descriptor = _open_owned_directory_at(
            anchor_descriptor,
            state_root.name,
            state_root,
        )
    finally:
        os.close(anchor_descriptor)

    try:
        current_path = state_root
        for component in parts[:-1]:
            next_path = current_path / component
            next_descriptor = _open_owned_directory_at(
                current_descriptor,
                component,
                next_path,
            )
            os.close(current_descriptor)
            current_descriptor = next_descriptor
            current_path = next_path

        file_name = parts[-1]
        file_descriptor = _open_owned_lock_file_at(
            current_descriptor,
            file_name,
            lock_path,
        )
        return file_descriptor, current_descriptor, file_name
    except BaseException:
        os.close(current_descriptor)
        raise


def _repository_key(common_git_dir: Path) -> str:
    canonical = os.fsencode(os.fspath(_canonical_path(common_git_dir)))
    return hashlib.sha256(canonical).hexdigest()


def _device_key(normalized_serial: str) -> str:
    return hashlib.sha256(normalized_serial.encode("utf-8")).hexdigest()


def _validate_timeout(timeout: float) -> float:
    value = float(timeout)
    if not math.isfinite(value) or value < 0.0:
        raise ValueError("lock timeout must be a finite non-negative number")
    return value


def _validate_poll_interval(poll_interval: float) -> float:
    value = float(poll_interval)
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError("lock poll interval must be a finite positive number")
    return value


class LockHandle:
    """One non-reentrant file lock acquired and released as a context manager."""

    def __init__(
        self,
        *,
        state_root: Path,
        path: Path,
        resource: str,
        owner: dict[str, Any],
        timeout: float,
        poll_interval: float,
    ) -> None:
        self.state_root = state_root
        self.path = path
        self.resource = resource
        self.owner = owner
        self.timeout = _validate_timeout(timeout)
        self.poll_interval = _validate_poll_interval(poll_interval)
        self._file_descriptor: int | None = None

    def _read_owner(self, file_descriptor: int) -> dict[str, Any] | None:
        try:
            raw = os.pread(file_descriptor, _OWNER_READ_LIMIT, 0)
            decoded = json.loads(raw.decode("utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
            return None
        return decoded if isinstance(decoded, dict) else None

    def _write_owner(self, file_descriptor: int) -> None:
        encoded = (
            json.dumps(self.owner, sort_keys=True, separators=(",", ":")) + "\n"
        ).encode("utf-8")
        os.ftruncate(file_descriptor, 0)
        offset = 0
        while offset < len(encoded):
            written = os.pwrite(file_descriptor, encoded[offset:], offset)
            if written <= 0:
                raise LockError(f"could not write owner diagnostics to {self.path}")
            offset += written
        os.fsync(file_descriptor)

    def acquire(self) -> "LockHandle":
        """Acquire the lock or raise :class:`LockTimeoutError` on deadline."""

        if self._file_descriptor is not None:
            raise LockError(f"lock handle is already acquired: {self.resource}")

        file_descriptor, parent_descriptor, file_name = _open_secure_lock_path(
            self.state_root,
            self.path,
        )
        deadline = time.monotonic() + self.timeout

        try:
            while True:
                try:
                    fcntl.flock(file_descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except OSError as error:
                    if error.errno not in (errno.EACCES, errno.EAGAIN):
                        raise

                    remaining = deadline - time.monotonic()
                    if remaining <= 0.0:
                        owner = self._read_owner(file_descriptor)
                        raise LockTimeoutError(
                            resource=self.resource,
                            path=self.path,
                            timeout=self.timeout,
                            owner=owner,
                        ) from None
                    time.sleep(min(self.poll_interval, remaining))

            # Ensure the public basename still names the exact inode carrying
            # the lease before owner JSON can be truncated or rewritten.
            _revalidate_directory_entry(
                parent_descriptor,
                file_name,
                file_descriptor,
                self.path,
            )

            # Capture process identity and acquisition time only after this
            # process owns the file. A handle may have waited, been reused, or
            # been created before a fork.
            self.owner["pid"] = os.getpid()
            self.owner["acquired_utc"] = (
                datetime.now(timezone.utc)
                .isoformat(timespec="milliseconds")
                .replace("+00:00", "Z")
            )
            self._write_owner(file_descriptor)
            self._file_descriptor = file_descriptor
            return self
        except BaseException:
            os.close(file_descriptor)
            raise
        finally:
            os.close(parent_descriptor)

    def release(self) -> None:
        """Release this process's lease while retaining its diagnostics file."""

        file_descriptor = self._file_descriptor
        if file_descriptor is None:
            return
        self._file_descriptor = None
        try:
            fcntl.flock(file_descriptor, fcntl.LOCK_UN)
        finally:
            os.close(file_descriptor)

    def __enter__(self) -> "LockHandle":
        return self.acquire()

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        del exception_type, exception, traceback
        self.release()


class LockManager:
    """Create per-effective-UID repository-lane and physical-device locks."""

    def __init__(
        self,
        workspace: WorkspaceLike,
        state_root: os.PathLike[str] | str | None = None,
        argv: Sequence[str] | None = None,
    ) -> None:
        self.workspace = workspace
        self.state_root = (
            _validate_test_state_root(state_root)
            if state_root is not None
            else default_state_root()
        )
        self.repository_key = _repository_key(workspace.common_git_dir)
        self.repo_state_dir = self.state_root / "repos" / self.repository_key
        self.device_state_dir = self.state_root / "devices"
        self.argv = tuple(argv if argv is not None else sys.argv)

    def _owner(self, resource: str) -> dict[str, Any]:
        return {
            "pid": os.getpid(),
            "argv": list(self.argv),
            "checkout": os.fspath(_canonical_path(self.workspace.root)),
            "branch": self.workspace.branch if self.workspace.branch is not None else "detached",
            "head": self.workspace.head,
            "acquired_utc": None,
            "resource": resource,
        }

    def lane(
        self,
        name: str,
        timeout: float = 30.0,
        *,
        poll_interval: float = 0.05,
    ) -> LockHandle:
        """Return a repo-family lock for a shared toolchain lane."""

        normalized_name = name.strip().lower()
        if not _LANE_PATTERN.fullmatch(normalized_name):
            raise ValueError(
                "lane name must contain only lowercase letters, digits, '.', '_', or '-'"
            )
        resource = f"lane:{normalized_name}"
        return LockHandle(
            state_root=self.state_root,
            path=self.repo_state_dir / "lanes" / f"{normalized_name}.lock",
            resource=resource,
            owner=self._owner(resource),
            timeout=timeout,
            poll_interval=poll_interval,
        )

    def device(
        self,
        serial: str,
        timeout: float = 30.0,
        *,
        poll_interval: float = 0.05,
    ) -> LockHandle:
        """Return this UID's lock for one exact normalized physical USB serial."""

        normalized_serial = normalize_device_identity(serial)
        resource = f"device:{normalized_serial}"
        return LockHandle(
            state_root=self.state_root,
            path=self.device_state_dir / f"{_device_key(normalized_serial)}.lock",
            resource=resource,
            owner=self._owner(resource),
            timeout=timeout,
            poll_interval=poll_interval,
        )
