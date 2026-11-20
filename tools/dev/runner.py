"""Bounded argv-only subprocess execution for the developer workflow."""

from __future__ import annotations

import ctypes
import codecs
import heapq
import itertools
import math
import os
import selectors
import shlex
import signal
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence, TextIO


class DevError(RuntimeError):
    """A user-facing workflow failure."""

    def __init__(self, message: str, *, exit_code: int = 1) -> None:
        super().__init__(message)
        self.exit_code = exit_code


class _SupervisorSignal(BaseException):
    """Deferred default signal delivery after protected-child cleanup."""

    def __init__(self, signum: signal.Signals) -> None:
        super().__init__(int(signum))
        self.signum = signum


class _SupervisorSignalHandlers:
    """Defer ordinary supervisor signals until protected cleanup can run."""

    _MANAGED_SIGNALS = (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)

    def __init__(self, *, enabled: bool) -> None:
        self._enabled = enabled
        self._previous: dict[signal.Signals, object] = {}
        self._received: signal.Signals | None = None
        self._armed = False

    @property
    def received_signal(self) -> signal.Signals | None:
        return self._received

    def install(self) -> None:
        # Python permits handler changes only on the main thread. A Runner used
        # from a worker cannot own process-wide termination semantics, so leave
        # its inherited handlers untouched.
        if not self._enabled or threading.current_thread() is not threading.main_thread():
            return
        for signum in self._MANAGED_SIGNALS:
            previous = signal.getsignal(signum)
            # Preserve an embedding application's custom or ignored handler.
            # The CLI supervisor uses the ordinary default disposition, which
            # is the unsafe path this guard needs to defer.
            ordinary_sigint = (
                signum == signal.SIGINT
                and previous == signal.default_int_handler
            )
            if previous != signal.SIG_DFL and not ordinary_sigint:
                continue
            self._previous[signum] = previous
            signal.signal(signum, self._handle)

    def arm(self) -> None:
        """Permit delivery exceptions once the child has an exact identity."""

        self._armed = True
        if self._received is not None:
            self._raise_received()

    def begin_finalization(self) -> None:
        """Record new signals without unwinding across the finalization edge."""

        self._armed = False

    def restore(self) -> None:
        self.finalize(())

    def finalize(
        self,
        fds_to_close: Sequence[int | None],
    ) -> signal.Signals | None:
        """Close launch FDs and restore dispositions without a signal gap."""

        if not self._previous:
            for fd in fds_to_close:
                _close_fd(fd)
            return self._received

        # Acquire the mask in a retry loop: a signal can arrive immediately
        # before the first pthread_sigmask call. The handler records it, and
        # subsequent delivery is coalesced while this loop finishes blocking.
        pthread_sigmask = getattr(signal, "pthread_sigmask", None)
        previous_mask: set[signal.Signals] | None = None
        if callable(pthread_sigmask):
            while previous_mask is None:
                try:
                    previous_mask = pthread_sigmask(
                        signal.SIG_BLOCK,
                        self._MANAGED_SIGNALS,
                    )
                except _SupervisorSignal:
                    continue
                except KeyboardInterrupt:
                    if self._received != signal.SIGINT:
                        raise
                    continue
        try:
            for fd in fds_to_close:
                _close_fd(fd)
            for signum, previous in reversed(tuple(self._previous.items())):
                signal.signal(signum, previous)
            self._previous.clear()
        finally:
            if previous_mask is not None:
                pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        return self._received

    def _handle(self, raw_signum: int, _frame: object) -> None:
        # Once cleanup starts, coalesce additional TERM/HUP/INT deliveries so they
        # cannot interrupt the bounded TERM -> KILL escalation halfway through.
        if self._received is not None:
            return
        self._received = signal.Signals(raw_signum)
        # Popen can be interrupted after the OS child exists but before it has
        # returned the Popen handle. Defer unwinding until Runner has recorded
        # both that handle and its birth identity.
        if self._armed:
            self._raise_received()

    def _raise_received(self) -> None:
        if self._received == signal.SIGINT:
            raise KeyboardInterrupt
        if self._received is not None:
            raise _SupervisorSignal(self._received)


def _redeliver_default_signal(signum: signal.Signals) -> None:
    """Restore ordinary supervisor exit semantics after child cleanup."""

    os.kill(os.getpid(), signum)
    # A default, unblocked terminating signal does not return. Keep a bounded
    # fallback for unusual masks instead of continuing as though the command
    # completed successfully.
    raise SystemExit(128 + int(signum))


@dataclass(frozen=True)
class CompletedCommand:
    """Stable subset of subprocess completion data used by tests and adapters."""

    argv: tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str


@dataclass(frozen=True)
class _ProcessIdentity:
    """Process-tree entry carrying a birth token for PID-reuse checks."""

    pid: int
    ppid: int
    pgid: int
    birth_token: str


class _DarwinBSDInfo(ctypes.Structure):
    """Darwin ``struct proc_bsdinfo`` fields used by the cleanup path."""

    _fields_ = [
        ("pbi_flags", ctypes.c_uint32),
        ("pbi_status", ctypes.c_uint32),
        ("pbi_xstatus", ctypes.c_uint32),
        ("pbi_pid", ctypes.c_uint32),
        ("pbi_ppid", ctypes.c_uint32),
        ("pbi_uid", ctypes.c_uint32),
        ("pbi_gid", ctypes.c_uint32),
        ("pbi_ruid", ctypes.c_uint32),
        ("pbi_rgid", ctypes.c_uint32),
        ("pbi_svuid", ctypes.c_uint32),
        ("pbi_svgid", ctypes.c_uint32),
        ("rfu_1", ctypes.c_uint32),
        ("pbi_comm", ctypes.c_char * 16),
        ("pbi_name", ctypes.c_char * 32),
        ("pbi_nfiles", ctypes.c_uint32),
        ("pbi_pgid", ctypes.c_uint32),
        ("pbi_pjobc", ctypes.c_uint32),
        ("e_tdev", ctypes.c_uint32),
        ("e_tpgid", ctypes.c_uint32),
        ("pbi_nice", ctypes.c_int32),
        ("pbi_start_tvsec", ctypes.c_uint64),
        ("pbi_start_tvusec", ctypes.c_uint64),
    ]


_DARWIN_LIBPROC: ctypes.CDLL | None | bool = None
_MAX_TRACKED_DESCENDANTS = 4096
_MAX_TRACKED_FDS = 4096
_TREE_SNAPSHOT_SECONDS = 0.20
_PROCESS_PROBE_SECONDS = 0.10
_GATED_EXEC_CODE = """\
import os
import sys

gate_fd = int(sys.argv[1])
error_fd = int(sys.argv[2])
argv = sys.argv[3:]
try:
    token = os.read(gate_fd, 1)
finally:
    os.close(gate_fd)
if token != b"1":
    os.close(error_fd)
    raise SystemExit(125)
try:
    os.set_inheritable(error_fd, False)
    os.execvpe(argv[0], argv, os.environ)
except OSError as error:
    os.write(error_fd, str(error.errno or 0).encode("ascii"))
    os.close(error_fd)
    raise SystemExit(127 if error.errno == 2 else 126)
"""


class _DarwinFDInfo(ctypes.Structure):
    """Darwin ``struct proc_fdinfo`` used to enumerate pipe descriptors."""

    _fields_ = [
        ("proc_fd", ctypes.c_int32),
        ("proc_fdtype", ctypes.c_uint32),
    ]


def display_argv(argv: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(token)) for token in argv)


def _spawn_gated_noninteractive(
    argv: Sequence[str],
    *,
    cwd: Path,
    env: Mapping[str, str],
    capture: bool,
) -> tuple[subprocess.Popen[str], int, int]:
    """Spawn a blocked wrapper that cannot exec ``argv`` until released."""

    gate_read, gate_write = os.pipe()
    error_read, error_write = os.pipe()
    try:
        process = subprocess.Popen(
            (
                sys.executable,
                "-c",
                _GATED_EXEC_CODE,
                str(gate_read),
                str(error_write),
                *argv,
            ),
            cwd=cwd,
            env=env,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
            start_new_session=True,
            pass_fds=(gate_read, error_write),
        )
    except BaseException:
        for fd in (gate_read, gate_write, error_read, error_write):
            os.close(fd)
        raise
    os.close(gate_read)
    os.close(error_write)
    return process, gate_write, error_read


def _close_fd(fd: int | None) -> None:
    if fd is None:
        return
    try:
        os.close(fd)
    except OSError:
        pass


def _read_exec_errno(fd: int) -> int | None:
    try:
        payload = os.read(fd, 32)
    except OSError:
        return 0
    if not payload:
        return None
    try:
        return int(payload.decode("ascii"))
    except ValueError:
        return 0


def _communicate_with_live_output(
    process: subprocess.Popen[str],
    *,
    timeout_seconds: float,
    callback: Callable[[str, str], None],
    retain_output: bool,
) -> tuple[str, str]:
    """Drain pipes live, retaining complete text only for captured callers."""

    if process.stdout is None or process.stderr is None:
        raise DevError("live output requires captured stdout and stderr")
    deadline = time.monotonic() + timeout_seconds
    selector = selectors.DefaultSelector()
    streams = {
        "stdout": process.stdout,
        "stderr": process.stderr,
    }
    chunks: dict[str, list[str]] = {"stdout": [], "stderr": []}
    decoders = {}
    try:
        for stream_name, stream in streams.items():
            encoding = stream.encoding or "utf-8"
            decoders[stream_name] = codecs.getincrementaldecoder(encoding)(errors="replace")
            selector.register(stream.fileno(), selectors.EVENT_READ, stream_name)

        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(process.args, timeout_seconds)
            for key, _ in selector.select(remaining):
                stream_name = key.data
                payload = os.read(key.fd, 65536)
                if payload:
                    text = decoders[stream_name].decode(payload)
                    if text:
                        if retain_output:
                            chunks[stream_name].append(text)
                        callback(stream_name, text)
                    continue
                selector.unregister(key.fd)
                final_text = decoders[stream_name].decode(b"", final=True)
                if final_text:
                    if retain_output:
                        chunks[stream_name].append(final_text)
                    callback(stream_name, final_text)
                streams[stream_name].close()

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(process.args, timeout_seconds)
        process.wait(timeout=remaining)
    finally:
        selector.close()
    return "".join(chunks["stdout"]), "".join(chunks["stderr"])


class Runner:
    """Execute commands without a shell and with explicit time bounds."""

    def __init__(
        self,
        *,
        output: TextIO | None = None,
        announce_commands: bool = True,
    ) -> None:
        self.output = output or sys.stderr
        self.announce_commands = announce_commands

    @staticmethod
    def environment(extra: Mapping[str, str] | None = None) -> dict[str, str]:
        env = dict(os.environ)
        # PlatformIO otherwise persists telemetry at interpreter shutdown, which
        # is noise in CI and can make read-only diagnostics appear to fail.
        env.setdefault("PLATFORMIO_SETTING_ENABLE_TELEMETRY", "no")
        env.setdefault("PYTHONUNBUFFERED", "1")
        if extra:
            env.update({str(key): str(value) for key, value in extra.items()})
        return env

    def run(
        self,
        argv: Sequence[str | Path],
        *,
        cwd: Path,
        timeout_seconds: float | None,
        capture: bool = False,
        check: bool = True,
        extra_env: Mapping[str, str] | None = None,
        interactive: bool = False,
        announce: bool | None = None,
        live_output: Callable[[str, str], None] | None = None,
    ) -> CompletedCommand:
        normalized = tuple(str(token) for token in argv)
        if not normalized or not normalized[0]:
            raise DevError("refusing to execute an empty command")
        if interactive and capture:
            raise DevError("interactive commands cannot capture output")
        if interactive and live_output is not None:
            raise DevError("interactive commands cannot stream output")
        if not interactive and (
            timeout_seconds is None
            or not math.isfinite(timeout_seconds)
            or timeout_seconds <= 0
        ):
            raise DevError("non-interactive commands require a finite positive timeout")

        should_announce = self.announce_commands if announce is None else announce
        if should_announce:
            print(f"[{cwd}] $ {display_argv(normalized)}", file=self.output, flush=True)
        handlers = _SupervisorSignalHandlers(enabled=not interactive)
        process: subprocess.Popen[str] | None = None
        leader_identity: _ProcessIdentity | None = None
        preidentity_retained: tuple[_ProcessIdentity, ...] = ()
        gate_release_fd: int | None = None
        exec_error_fd: int | None = None
        exec_errno: int | None = None
        interrupted_signal: signal.Signals | None = None
        cleanup_finished = False
        started_at = time.monotonic()

        def cleanup_noninteractive() -> None:
            nonlocal cleanup_finished, leader_identity, preidentity_retained
            if process is None or interactive or cleanup_finished:
                return
            if leader_identity is None:
                leader_identity, newly_retained = _record_leader_identity(
                    process,
                    deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
                )
                preidentity_retained = _merge_process_identities(
                    preidentity_retained,
                    newly_retained,
                )
            if leader_identity is None:
                _terminate_unidentified_process_tree(
                    process,
                    initial_retained=preidentity_retained,
                )
            else:
                _terminate_process_group(
                    process,
                    leader_identity=leader_identity,
                    initial_retained=preidentity_retained,
                )
            cleanup_finished = True

        try:
            try:
                handlers.install()
                try:
                    command_environment = self.environment(extra_env)
                    if interactive:
                        process = subprocess.Popen(
                            normalized,
                            cwd=cwd,
                            env=command_environment,
                            text=True,
                        )
                    else:
                        process, gate_release_fd, exec_error_fd = (
                            _spawn_gated_noninteractive(
                                normalized,
                                cwd=cwd,
                                env=command_environment,
                                capture=capture or live_output is not None,
                            )
                        )
                except FileNotFoundError as error:
                    raise DevError(
                        f"required command not found: {normalized[0]}; "
                        "run ./litch doctor for setup guidance",
                        exit_code=127,
                    ) from error
                if interactive:
                    returncode = process.wait()
                    stdout = ""
                    stderr = ""
                else:
                    leader_identity, preidentity_retained = _record_leader_identity(
                        process,
                        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
                    )
                    if leader_identity is None and process.returncode is None:
                        raise DevError(
                            "could not establish an exact identity for the spawned command; "
                            "the command was stopped before workflow locks were released"
                        )
                    handlers.arm()
                    if gate_release_fd is not None:
                        try:
                            os.write(gate_release_fd, b"1")
                        except OSError as error:
                            raise DevError(
                                "could not release the contained command after "
                                "recording its process identity"
                            ) from error
                        finally:
                            _close_fd(gate_release_fd)
                            gate_release_fd = None
                    if live_output is None:
                        stdout, stderr = process.communicate(timeout=timeout_seconds)
                    else:
                        assert timeout_seconds is not None
                        stdout, stderr = _communicate_with_live_output(
                            process,
                            timeout_seconds=timeout_seconds,
                            callback=live_output,
                            retain_output=capture,
                        )
                    returncode = process.returncode
                    if exec_error_fd is not None:
                        exec_errno = _read_exec_errno(exec_error_fd)
                        _close_fd(exec_error_fd)
                        exec_error_fd = None
                handlers.begin_finalization()
            except subprocess.TimeoutExpired as error:
                try:
                    cleanup_noninteractive()
                finally:
                    handlers.begin_finalization()
                raise DevError(
                    f"command exceeded {timeout_seconds:g}s "
                    f"(elapsed {time.monotonic() - started_at:.2f}s including cleanup): "
                    f"{display_argv(normalized)}"
                ) from error
            except BaseException:
                try:
                    cleanup_noninteractive()
                finally:
                    handlers.begin_finalization()
                raise
        except _SupervisorSignal as error:
            # A signal can arrive while timeout cleanup itself is running. In
            # that case retry the idempotent, exact-PID cleanup before releasing
            # the surrounding lane/device lock and replaying default delivery.
            cleanup_noninteractive()
            handlers.begin_finalization()
            interrupted_signal = error.signum
        except BaseException:
            # If Popen failed after an unarmed handler recorded termination,
            # default signal semantics take precedence over the spawn error.
            # Any returned process is still cleaned before replaying the signal.
            if handlers.received_signal is None:
                raise
            cleanup_noninteractive()
            handlers.begin_finalization()
            if handlers.received_signal == signal.SIGINT:
                raise KeyboardInterrupt
            interrupted_signal = handlers.received_signal
        finally:
            finalized_signal = handlers.finalize(
                (gate_release_fd, exec_error_fd),
            )
            if interrupted_signal is None and finalized_signal is not None:
                if finalized_signal == signal.SIGINT:
                    raise KeyboardInterrupt
                _redeliver_default_signal(finalized_signal)

        if interrupted_signal is not None:
            _redeliver_default_signal(interrupted_signal)

        if exec_errno is not None:
            if exec_errno == 2:
                raise DevError(
                    f"required command not found: {normalized[0]}; "
                    "run ./litch doctor for setup guidance",
                    exit_code=127,
                )
            raise DevError(
                f"could not execute command: {display_argv(normalized)}",
                exit_code=126,
            )

        completed = CompletedCommand(
            argv=normalized,
            returncode=returncode,
            stdout=stdout or "",
            stderr=stderr or "",
        )
        if should_announce:
            print(
                f"[completed in {time.monotonic() - started_at:.2f}s; exit {returncode}]",
                file=self.output,
                flush=True,
            )
        if check and completed.returncode != 0:
            detail = ""
            if capture:
                combined = "\n".join(
                    part.strip() for part in (completed.stdout, completed.stderr) if part.strip()
                )
                if combined:
                    detail = f"\n{combined}"
            raise DevError(
                f"command failed with exit {completed.returncode}: {display_argv(normalized)}{detail}",
                exit_code=completed.returncode or 1,
            )
        return completed


def _record_leader_identity(
    process: subprocess.Popen[str],
    *,
    deadline: float,
) -> tuple[_ProcessIdentity | None, tuple[_ProcessIdentity, ...]]:
    """Obtain the leader identity while retaining exact early descendants."""

    retained: tuple[_ProcessIdentity, ...] = ()
    while True:
        identity = _process_identity(process.pid, deadline=deadline)
        if identity is not None:
            return identity, retained
        # Do not poll/reap here. Popen's unreaped child is the ownership token
        # that makes its numeric PID/PGID un-recyclable while exact group and
        # ancestry candidates are captured. This also retains a child that
        # detaches immediately before the leader exits.
        snapshot_deadline = min(deadline, time.monotonic() + 0.02)
        retained = _merge_process_identities(
            retained,
            _snapshot_group_members(process.pid, deadline=snapshot_deadline),
            _snapshot_descendants(process.pid, deadline=snapshot_deadline),
        )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None, retained
        time.sleep(min(0.005, remaining))


def _snapshot_unidentified_owned_tree(
    process: subprocess.Popen[str],
    *,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    """Snapshot exact candidates while Popen still owns the group leader."""

    # A live, unreaped child created with start_new_session owns both this PID
    # and PGID. That authorizes bounded candidate discovery without signalling
    # the raw group; every discovered PID still carries a birth token.
    if process.returncode is not None:
        return _snapshot_capture_pipe_holders(process, deadline=deadline)
    return _merge_process_identities(
        _snapshot_group_members(process.pid, deadline=deadline),
        _snapshot_descendants(process.pid, deadline=deadline),
        _snapshot_capture_pipe_holders(process, deadline=deadline),
    )


def _terminate_unidentified_process_tree(
    process: subprocess.Popen[str],
    *,
    initial_retained: Sequence[_ProcessIdentity] = (),
    grace_seconds: float = 0.25,
) -> None:
    """Fail closed when the leader's first-class birth probe is unavailable."""

    retained = _merge_process_identities(
        initial_retained,
        _snapshot_unidentified_owned_tree(
            process,
            deadline=time.monotonic() + _TREE_SNAPSHOT_SECONDS,
        ),
    )
    _signal_retained_processes(
        retained,
        signum=signal.SIGTERM,
        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
    )

    try:
        process.terminate()
    except (ProcessLookupError, PermissionError, OSError):
        pass

    term_deadline = time.monotonic() + grace_seconds
    _communicate_until(process, deadline=term_deadline)
    term_quiet = _wait_for_cleanup_exit(
        process,
        retained=retained,
        deadline=term_deadline,
    )
    retained = _merge_process_identities(
        retained,
        _snapshot_unidentified_owned_tree(
            process,
            deadline=time.monotonic() + _TREE_SNAPSHOT_SECONDS,
        ),
    )
    if term_quiet and not _retained_process_alive(
        retained,
        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
    ):
        return

    _signal_retained_processes(
        retained,
        signum=signal.SIGKILL,
        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
    )
    try:
        process.kill()
    except (ProcessLookupError, PermissionError, OSError):
        pass
    kill_deadline = time.monotonic() + grace_seconds
    _communicate_until(process, deadline=kill_deadline)
    _wait_for_cleanup_exit(
        process,
        retained=retained,
        deadline=kill_deadline,
    )
    for stream in (process.stdout, process.stderr):
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass


def _terminate_process_group(
    process: subprocess.Popen[str],
    *,
    leader_identity: _ProcessIdentity | None = None,
    initial_retained: Sequence[_ProcessIdentity] = (),
    grace_seconds: float = 0.25,
) -> None:
    """Boundedly terminate a command and its retained, discoverable descendants."""

    # A child can outlive and reparent away from the direct leader, or call
    # setsid(2) and leave the initial process group. Retain every process that
    # is still tied to this command by ancestry, its original process group, or
    # one of the command's private capture pipes. Every PID is paired with a
    # birth token and revalidated immediately before signalling, rejecting
    # stale identities instead of trusting a recycled numeric PID or PGID.
    retained = _merge_process_identities(
        initial_retained,
        _snapshot_cleanup_processes(
            process,
            leader_identity=leader_identity,
            deadline=time.monotonic() + _TREE_SNAPSHOT_SECONDS,
        ),
    )
    _signal_retained_processes(
        retained,
        signum=signal.SIGTERM,
        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
    )

    term_deadline = time.monotonic() + grace_seconds
    _communicate_until(process, deadline=term_deadline)
    term_quiet = _wait_for_cleanup_exit(
        process,
        retained=retained,
        deadline=term_deadline,
    )

    # Always refresh before accepting TERM completion: a TERM handler can spawn
    # and detach a new child after the initial snapshot while that child keeps
    # one of the command's capture pipes open.
    retained = _merge_process_identities(
        retained,
        _snapshot_cleanup_processes(
            process,
            leader_identity=leader_identity,
            deadline=time.monotonic() + _TREE_SNAPSHOT_SECONDS,
        ),
    )
    if term_quiet and not _retained_process_alive(
        retained,
        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
    ):
        return

    _signal_retained_processes(
        retained,
        signum=signal.SIGKILL,
        deadline=time.monotonic() + _PROCESS_PROBE_SECONDS,
    )

    kill_deadline = time.monotonic() + grace_seconds
    _communicate_until(process, deadline=kill_deadline)
    _wait_for_cleanup_exit(
        process,
        retained=retained,
        deadline=kill_deadline,
    )
    # SIGKILL is final; do not turn bounded workflow cleanup into another
    # unbounded wait if the OS reports an uninterruptible process.
    for stream in (process.stdout, process.stderr):
        if stream is not None:
            try:
                stream.close()
            except OSError:
                pass


def _communicate_until(
    process: subprocess.Popen[str],
    *,
    deadline: float,
) -> None:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        process.poll()
        return
    try:
        process.communicate(timeout=remaining)
    except (subprocess.TimeoutExpired, OSError, ValueError):
        pass


def _wait_for_cleanup_exit(
    process: subprocess.Popen[str],
    *,
    retained: Sequence[_ProcessIdentity],
    deadline: float,
) -> bool:
    """Reap the leader and wait boundedly for retained exact processes."""

    while True:
        process.poll()
        retained_alive = _retained_process_alive(
            retained,
            deadline=deadline,
        )
        if process.returncode is not None and not retained_alive:
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(0.02, remaining))


def _snapshot_cleanup_processes(
    process: subprocess.Popen[str],
    *,
    leader_identity: _ProcessIdentity | None,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    """Find every bounded, kernel-evidenced process owned by ``process``."""

    retained: tuple[_ProcessIdentity, ...] = (
        (leader_identity,) if leader_identity is not None else ()
    )
    leader_is_current = False
    if leader_identity is not None:
        current = _current_process_identity(leader_identity, deadline=deadline)
        leader_is_current = current is not None and _same_process(
            leader_identity,
            current,
        )
    leader_bound_processes: tuple[_ProcessIdentity, ...] = ()
    if leader_is_current:
        # Numeric ancestry and process-group identifiers are authoritative only
        # while the recorded leader birth identity still owns that PID.
        leader_bound_processes = _merge_process_identities(
            _snapshot_group_members(process.pid, deadline=deadline),
            _snapshot_descendants(process.pid, deadline=deadline),
        )
    retained = _merge_process_identities(
        retained,
        leader_bound_processes,
        _snapshot_capture_pipe_holders(process, deadline=deadline),
    )
    return retained


def _merge_process_identities(
    *groups: Sequence[_ProcessIdentity],
) -> tuple[_ProcessIdentity, ...]:
    merged: dict[tuple[int, str], _ProcessIdentity] = {}
    for group in groups:
        for identity in group:
            if identity.pid != os.getpid():
                merged[(identity.pid, identity.birth_token)] = identity
    return tuple(merged.values())[:_MAX_TRACKED_DESCENDANTS]


def _snapshot_group_members(
    pgid: int,
    *,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    """Snapshot exact members of the private group created for a command."""

    if sys.platform == "darwin":
        libproc = _darwin_libproc()
        if libproc is not None:
            estimated = libproc.proc_listpgrppids(pgid, None, 0)
            if estimated >= 0:
                capacity = min(
                    max(int(estimated) + 32, 64),
                    _MAX_TRACKED_DESCENDANTS + 1,
                )
                buffer = (ctypes.c_int * capacity)()
                count = libproc.proc_listpgrppids(
                    pgid,
                    buffer,
                    ctypes.sizeof(buffer),
                )
                if count >= 0:
                    members: list[_ProcessIdentity] = []
                    for pid in buffer[: min(int(count), capacity)]:
                        if time.monotonic() >= deadline:
                            break
                        identity = _native_process_identity(int(pid))
                        if identity is not None and identity.pgid == pgid:
                            members.append(identity)
                    return tuple(members)

    if sys.platform.startswith("linux"):
        members = []
        for pid in _linux_pids_descending(deadline=deadline):
            if time.monotonic() >= deadline:
                break
            identity = _native_process_identity(pid)
            if identity is not None and identity.pgid == pgid:
                members.append(identity)
                if len(members) >= _MAX_TRACKED_DESCENDANTS:
                    break
        return tuple(members)

    table = _ps_process_table(deadline=deadline)
    members = []
    for candidate in table.values():
        if time.monotonic() >= deadline:
            break
        if candidate.pgid != pgid:
            continue
        identity = _native_process_identity(candidate.pid)
        if identity is not None and identity.pgid == pgid:
            members.append(identity)
    return tuple(members)[:_MAX_TRACKED_DESCENDANTS]


def _snapshot_capture_pipe_holders(
    process: subprocess.Popen[str],
    *,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    """Find children retaining this command's private stdout/stderr pipes."""

    pipe_fds: list[int] = []
    for stream in (process.stdout, process.stderr):
        if stream is None:
            continue
        try:
            pipe_fds.append(stream.fileno())
        except (OSError, ValueError):
            continue
    if not pipe_fds or time.monotonic() >= deadline:
        return ()
    if sys.platform == "darwin":
        return _darwin_capture_pipe_holders(pipe_fds, deadline=deadline)
    if sys.platform.startswith("linux"):
        return _linux_capture_pipe_holders(pipe_fds, deadline=deadline)
    return ()


def _darwin_capture_pipe_holders(
    pipe_fds: Sequence[int],
    *,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    libproc = _darwin_libproc()
    if libproc is None:
        return ()
    pipe_keys = {
        key
        for fd in pipe_fds
        if (key := _darwin_pipe_key(os.getpid(), fd)) is not None
    }
    if not pipe_keys:
        return ()
    pids = _darwin_uid_pids()
    fd_buffer = (_DarwinFDInfo * _MAX_TRACKED_FDS)()
    holders: list[_ProcessIdentity] = []
    for pid in pids:
        if time.monotonic() >= deadline:
            break
        if pid == os.getpid():
            continue
        before = _native_process_identity(pid)
        if before is None:
            continue
        returned = libproc.proc_pidinfo(
            pid,
            1,  # PROC_PIDLISTFDS
            0,
            fd_buffer,
            ctypes.sizeof(fd_buffer),
        )
        if returned <= 0:
            continue
        entry_count = min(
            int(returned) // ctypes.sizeof(_DarwinFDInfo),
            _MAX_TRACKED_FDS,
        )
        owns_pipe = False
        for entry in fd_buffer[:entry_count]:
            if time.monotonic() >= deadline:
                break
            if int(entry.proc_fdtype) != 6:  # PROX_FDTYPE_PIPE
                continue
            key = _darwin_pipe_key(pid, int(entry.proc_fd))
            if key in pipe_keys:
                owns_pipe = True
                break
        if not owns_pipe:
            continue
        after = _native_process_identity(pid)
        if after is not None and _same_process(before, after):
            holders.append(before)
            if len(holders) >= _MAX_TRACKED_DESCENDANTS:
                break
    return tuple(holders)


def _darwin_pipe_key(pid: int, fd: int) -> tuple[int, int] | None:
    """Return an order-independent kernel identity for one Darwin pipe."""

    libproc = _darwin_libproc()
    if libproc is None:
        return None
    # Darwin's stable pipe_fdinfo layout is 184 bytes; pipe_handle and
    # pipe_peerhandle are uint64 fields at byte offsets 160 and 168.
    buffer = ctypes.create_string_buffer(184)
    returned = libproc.proc_pidfdinfo(
        pid,
        fd,
        6,  # PROC_PIDFDPIPEINFO
        buffer,
        ctypes.sizeof(buffer),
    )
    if returned != ctypes.sizeof(buffer):
        return None
    handle, peer_handle = struct.unpack_from("@QQ", buffer.raw, 160)
    if handle == 0 and peer_handle == 0:
        return None
    return tuple(sorted((int(handle), int(peer_handle))))


def _darwin_uid_pids() -> tuple[int, ...]:
    libproc = _darwin_libproc()
    if libproc is None:
        return ()
    effective_uid = os.geteuid()
    estimated_bytes = libproc.proc_listpids(4, effective_uid, None, 0)  # PROC_UID_ONLY
    if estimated_bytes < 0:
        return ()
    capacity = min(
        max(int(estimated_bytes) // ctypes.sizeof(ctypes.c_int) + 32, 64),
        _MAX_TRACKED_DESCENDANTS + 1,
    )
    buffer = (ctypes.c_int * capacity)()
    returned_bytes = libproc.proc_listpids(
        4,
        effective_uid,
        buffer,
        ctypes.sizeof(buffer),
    )
    if returned_bytes < 0:
        return ()
    count = min(
        int(returned_bytes) // ctypes.sizeof(ctypes.c_int),
        capacity,
    )
    return tuple(int(pid) for pid in buffer[:count] if pid > 0)


def _linux_capture_pipe_holders(
    pipe_fds: Sequence[int],
    *,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    pipe_names: set[str] = set()
    for fd in pipe_fds:
        try:
            target = os.readlink(f"/proc/{os.getpid()}/fd/{fd}")
        except (FileNotFoundError, PermissionError, OSError):
            continue
        if target.startswith("pipe:["):
            pipe_names.add(target)
    if not pipe_names:
        return ()

    holders: list[_ProcessIdentity] = []
    for pid in _linux_pids_descending(deadline=deadline):
        if time.monotonic() >= deadline:
            break
        if pid == os.getpid():
            continue
        before = _native_process_identity(pid)
        if before is None:
            continue
        try:
            with os.scandir(f"/proc/{pid}/fd") as entries:
                fd_entries = tuple(itertools.islice(entries, _MAX_TRACKED_FDS))
        except (FileNotFoundError, PermissionError, OSError):
            continue
        owns_pipe = False
        for entry in fd_entries:
            if time.monotonic() >= deadline:
                break
            try:
                if os.readlink(entry.path) in pipe_names:
                    owns_pipe = True
                    break
            except (FileNotFoundError, PermissionError, OSError):
                continue
        if not owns_pipe:
            continue
        after = _native_process_identity(pid)
        if after is not None and _same_process(before, after):
            holders.append(before)
            if len(holders) >= _MAX_TRACKED_DESCENDANTS:
                break
    return tuple(holders)


def _linux_pids_descending(*, deadline: float) -> tuple[int, ...]:
    newest_pids: list[int] = []
    try:
        with os.scandir("/proc") as entries:
            for entry in entries:
                if time.monotonic() >= deadline:
                    break
                if entry.name.isdigit():
                    pid = int(entry.name)
                    if len(newest_pids) < _MAX_TRACKED_DESCENDANTS:
                        heapq.heappush(newest_pids, pid)
                    elif pid > newest_pids[0]:
                        heapq.heapreplace(newest_pids, pid)
    except (FileNotFoundError, PermissionError, OSError):
        return ()
    return tuple(sorted(newest_pids, reverse=True))


def _snapshot_descendants(
    root_pid: int,
    *,
    deadline: float,
) -> tuple[_ProcessIdentity, ...]:
    """Snapshot a bounded, exact ancestry tree below ``root_pid``."""

    descendants: list[_ProcessIdentity] = []
    pending = [root_pid]
    seen = {root_pid}
    native_supported = sys.platform.startswith("linux") or sys.platform == "darwin"

    while pending and len(descendants) < _MAX_TRACKED_DESCENDANTS:
        if time.monotonic() >= deadline:
            break
        parent_pid = pending.pop(0)
        child_pids = _native_child_pids(parent_pid, deadline=deadline)
        if child_pids is None:
            native_supported = False
            break
        for child_pid in child_pids:
            if child_pid <= 0 or child_pid in seen:
                continue
            identity = _native_process_identity(child_pid)
            # The child can exit or be reparented between the two kernel calls.
            # Only retain an entry that is still an exact child of this node.
            if identity is None or identity.ppid != parent_pid:
                continue
            seen.add(child_pid)
            descendants.append(identity)
            pending.append(child_pid)
            if len(descendants) >= _MAX_TRACKED_DESCENDANTS:
                break

    if native_supported:
        return tuple(descendants)

    table = _ps_process_table(deadline=deadline)
    fallback_descendants = _descendants_from_table(root_pid, table)
    combined: dict[int, _ProcessIdentity] = {}
    for candidate in fallback_descendants:
        if time.monotonic() >= deadline:
            break
        identity = _native_process_identity(candidate.pid)
        if identity is not None:
            combined[identity.pid] = identity
    combined.update({identity.pid: identity for identity in descendants})
    return tuple(combined.values())[:_MAX_TRACKED_DESCENDANTS]


def _native_child_pids(
    parent_pid: int,
    *,
    deadline: float,
) -> tuple[int, ...] | None:
    if sys.platform.startswith("linux"):
        children: set[int] = set()
        try:
            with os.scandir(f"/proc/{parent_pid}/task") as entries:
                task_dirs = tuple(
                    itertools.islice(entries, _MAX_TRACKED_DESCENDANTS)
                )
        except FileNotFoundError:
            return ()
        except (PermissionError, OSError):
            return None
        for task_dir in task_dirs[:_MAX_TRACKED_DESCENDANTS]:
            if time.monotonic() >= deadline:
                break
            try:
                content = Path(task_dir.path, "children").read_text(encoding="ascii")
                children.update(int(token) for token in content.split())
            except FileNotFoundError:
                continue
            except (PermissionError, OSError, ValueError):
                return None
        return tuple(children)
    if sys.platform == "darwin":
        libproc = _darwin_libproc()
        if libproc is None:
            return None
        estimated = libproc.proc_listchildpids(parent_pid, None, 0)
        if estimated < 0:
            return None
        capacity = min(
            max(int(estimated) + 32, 64),
            _MAX_TRACKED_DESCENDANTS + 1,
        )
        buffer = (ctypes.c_int * capacity)()
        count = libproc.proc_listchildpids(
            parent_pid,
            buffer,
            ctypes.sizeof(buffer),
        )
        if count < 0:
            return None
        return tuple(int(pid) for pid in buffer[: min(int(count), capacity)] if pid > 0)
    return None


def _native_process_identity(pid: int) -> _ProcessIdentity | None:
    if sys.platform.startswith("linux"):
        try:
            stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        except (FileNotFoundError, PermissionError, OSError):
            return None
        closing_paren = stat.rfind(")")
        if closing_paren < 0:
            return None
        fields = stat[closing_paren + 1 :].split()
        if len(fields) < 20:
            return None
        try:
            return _ProcessIdentity(
                pid=pid,
                ppid=int(fields[1]),
                pgid=int(fields[2]),
                birth_token=f"linux:{fields[19]}",
            )
        except ValueError:
            return None
    if sys.platform == "darwin":
        libproc = _darwin_libproc()
        if libproc is None:
            return None
        info = _DarwinBSDInfo()
        returned = libproc.proc_pidinfo(
            pid,
            3,  # PROC_PIDTBSDINFO
            0,
            ctypes.byref(info),
            ctypes.sizeof(info),
        )
        if returned != ctypes.sizeof(info) or int(info.pbi_pid) != pid:
            return None
        return _ProcessIdentity(
            pid=pid,
            ppid=int(info.pbi_ppid),
            pgid=int(info.pbi_pgid),
            birth_token=(
                f"darwin:{int(info.pbi_start_tvsec)}:"
                f"{int(info.pbi_start_tvusec)}"
            ),
        )
    return None


def _darwin_libproc() -> ctypes.CDLL | None:
    global _DARWIN_LIBPROC

    if _DARWIN_LIBPROC is False:
        return None
    if isinstance(_DARWIN_LIBPROC, ctypes.CDLL):
        return _DARWIN_LIBPROC
    try:
        libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    except OSError:
        _DARWIN_LIBPROC = False
        return None
    libproc.proc_listchildpids.argtypes = [
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_listchildpids.restype = ctypes.c_int
    libproc.proc_listpgrppids.argtypes = [
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_listpgrppids.restype = ctypes.c_int
    libproc.proc_listpids.argtypes = [
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_listpids.restype = ctypes.c_int
    libproc.proc_pidinfo.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_pidinfo.restype = ctypes.c_int
    libproc.proc_pidfdinfo.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_pidfdinfo.restype = ctypes.c_int
    _DARWIN_LIBPROC = libproc
    return libproc


def _ps_process_table(*, deadline: float) -> dict[int, _ProcessIdentity]:
    """Bounded POSIX fallback used only when native discovery is unavailable."""

    remaining = min(_PROCESS_PROBE_SECONDS, deadline - time.monotonic())
    if remaining <= 0:
        return {}
    try:
        completed = subprocess.run(
            ("ps", "-axo", "pid=,ppid=,pgid=,lstart="),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=remaining,
            check=False,
            start_new_session=True,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return {}
    table: dict[int, _ProcessIdentity] = {}
    for line in completed.stdout.splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) != 4:
            continue
        try:
            pid = int(fields[0])
            table[pid] = _ProcessIdentity(
                pid=pid,
                ppid=int(fields[1]),
                pgid=int(fields[2]),
                birth_token=f"ps:{fields[3]}",
            )
        except ValueError:
            continue
    return table


def _descendants_from_table(
    root_pid: int,
    table: Mapping[int, _ProcessIdentity],
) -> tuple[_ProcessIdentity, ...]:
    children: dict[int, list[_ProcessIdentity]] = {}
    for identity in table.values():
        children.setdefault(identity.ppid, []).append(identity)
    descendants: list[_ProcessIdentity] = []
    pending = [root_pid]
    seen = {root_pid}
    while pending and len(descendants) < _MAX_TRACKED_DESCENDANTS:
        parent_pid = pending.pop(0)
        for identity in children.get(parent_pid, ()):
            if identity.pid in seen:
                continue
            seen.add(identity.pid)
            descendants.append(identity)
            pending.append(identity.pid)
            if len(descendants) >= _MAX_TRACKED_DESCENDANTS:
                break
    return tuple(descendants)


def _current_process_identity(
    expected: _ProcessIdentity,
    *,
    deadline: float,
) -> _ProcessIdentity | None:
    # ``ps`` start times have only second-level resolution and are suitable for
    # candidate discovery, never for authorizing a signal after PID reuse.
    if expected.birth_token.startswith("ps:"):
        return None
    identity = _native_process_identity(expected.pid)
    if identity is not None:
        return identity
    return None


def _process_identity(pid: int, *, deadline: float) -> _ProcessIdentity | None:
    del deadline
    return _native_process_identity(pid)


def _same_process(left: _ProcessIdentity, right: _ProcessIdentity) -> bool:
    return left.pid == right.pid and left.birth_token == right.birth_token


def _signal_retained_processes(
    retained: Sequence[_ProcessIdentity],
    *,
    signum: signal.Signals,
    deadline: float,
) -> None:
    """Signal exact retained PIDs after validating every birth token."""

    for expected in reversed(retained):
        if time.monotonic() >= deadline:
            return
        current = _current_process_identity(expected, deadline=deadline)
        if current is None or not _same_process(expected, current):
            continue
        if _signal_with_linux_pidfd(expected, signum=signum):
            continue
        try:
            os.kill(current.pid, signum)
        except (ProcessLookupError, PermissionError):
            pass


def _signal_with_linux_pidfd(
    expected: _ProcessIdentity,
    *,
    signum: signal.Signals,
) -> bool:
    """Atomically signal a Linux process when the runtime exposes pidfds."""

    if not sys.platform.startswith("linux"):
        return False
    pidfd_open = getattr(os, "pidfd_open", None)
    pidfd_send_signal = getattr(signal, "pidfd_send_signal", None)
    if not callable(pidfd_open) or not callable(pidfd_send_signal):
        return False
    try:
        pidfd = pidfd_open(expected.pid, 0)
    except ProcessLookupError:
        return True
    except (PermissionError, OSError):
        return False
    try:
        # Opening the pidfd and then checking birth identity ensures the handle
        # cannot have bound to a process that recycled ``expected.pid``.
        bound = _native_process_identity(expected.pid)
        if bound is None or not _same_process(expected, bound):
            return True
        try:
            pidfd_send_signal(pidfd, signum)
        except (ProcessLookupError, PermissionError, OSError):
            pass
        return True
    finally:
        os.close(pidfd)


def _retained_process_alive(
    retained: Sequence[_ProcessIdentity],
    *,
    deadline: float,
) -> bool:
    for expected in retained:
        if time.monotonic() >= deadline:
            return True
        current = _current_process_identity(expected, deadline=deadline)
        if current is not None and _same_process(expected, current):
            return True
    return False
