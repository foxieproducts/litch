from __future__ import annotations

import io
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

from tools.dev import runner as runner_module
from tools.dev.runner import DevError, Runner, display_argv


class RunnerTests(unittest.TestCase):
    def _wait_for_path(self, path: Path, *, timeout_seconds: float = 3.0) -> None:
        deadline = time.monotonic() + timeout_seconds
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(path.exists(), f"timed out waiting for {path}")

    def _assert_signal_cleanup_precedes_lock_release(
        self,
        signum: signal.Signals,
    ) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        child = (
            "import os,pathlib,signal,sys,time; "
            "os.setsid(); "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            "signal.signal(signal.SIGHUP, signal.SIG_IGN); "
            "pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); "
            "time.sleep(3)"
        )
        leader = (
            "import subprocess,sys,time; "
            f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1]], "
            "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
            "time.sleep(20)"
        )
        supervisor = (
            "import fcntl,io,pathlib,sys; "
            "from tools.dev.runner import Runner; "
            "lock=open(sys.argv[1], 'a+'); "
            "fcntl.flock(lock, fcntl.LOCK_EX); "
            "Runner(output=io.StringIO()).run("
            "[sys.executable, '-c', sys.argv[4], sys.argv[2]], "
            "cwd=pathlib.Path(sys.argv[3]), timeout_seconds=30)"
        )
        contender = (
            "import fcntl,os,pathlib,sys; "
            "lock=open(sys.argv[1], 'a+'); "
            "fcntl.flock(lock, fcntl.LOCK_EX); "
            "pid=int(pathlib.Path(sys.argv[2]).read_text()); "
            "alive=True; "
            "\ntry: os.kill(pid, 0)"
            "\nexcept ProcessLookupError: alive=False"
            "\npathlib.Path(sys.argv[3]).write_text('alive' if alive else 'dead')"
        )
        environment = dict(os.environ)
        existing_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (str(repository_root), existing_pythonpath)
            if part
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = root / "lane.lock"
            child_pid_path = root / "child.pid"
            contender_result = root / "contender-result"
            supervisor_process = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    supervisor,
                    str(lock_path),
                    str(child_pid_path),
                    str(root),
                    leader,
                ],
                cwd=root,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            contender_process: subprocess.Popen[str] | None = None
            try:
                self._wait_for_path(child_pid_path)
                contender_process = subprocess.Popen(
                    [
                        sys.executable,
                        "-c",
                        contender,
                        str(lock_path),
                        str(child_pid_path),
                        str(contender_result),
                    ],
                    cwd=root,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                os.kill(supervisor_process.pid, signum)
                contender_process.wait(timeout=5)
                supervisor_process.wait(timeout=5)
                self._wait_for_path(contender_result)

                self.assertEqual(supervisor_process.returncode, -int(signum))
                self.assertEqual(contender_result.read_text(), "dead")
            finally:
                for process in (contender_process, supervisor_process):
                    if process is not None and process.poll() is None:
                        process.kill()
                        process.wait(timeout=2)

    def test_sigterm_cleanup_precedes_lock_release(self) -> None:
        self._assert_signal_cleanup_precedes_lock_release(signal.SIGTERM)

    def test_sighup_cleanup_precedes_lock_release(self) -> None:
        self._assert_signal_cleanup_precedes_lock_release(signal.SIGHUP)

    def test_sigint_preserves_cleanup_before_lock_release(self) -> None:
        self._assert_signal_cleanup_precedes_lock_release(signal.SIGINT)

    def test_signal_during_popen_is_deferred_until_child_identity_is_recorded(self) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        supervisor = (
            "import fcntl,io,os,pathlib,sys\n"
            "from tools.dev import runner as runner_module\n"
            "real_subprocess=runner_module.subprocess\n"
            "real_process_identity=runner_module._process_identity\n"
            "identity_probes=0\n"
            "def initially_unavailable_identity(pid, *, deadline):\n"
            " global identity_probes\n"
            " identity_probes+=1\n"
            " if identity_probes == 1:\n"
            "  return None\n"
            " return real_process_identity(pid, deadline=deadline)\n"
            "class SignalDuringPopen:\n"
            " def Popen(self, *args, **kwargs):\n"
            "  process=real_subprocess.Popen(*args, **kwargs)\n"
            "  pathlib.Path(sys.argv[2]).write_text(str(process.pid))\n"
            "  os.kill(os.getpid(), int(sys.argv[4]))\n"
            "  return process\n"
            " def __getattr__(self, name):\n"
            "  return getattr(real_subprocess, name)\n"
            "lock=open(sys.argv[1], 'a+')\n"
            "fcntl.flock(lock, fcntl.LOCK_EX)\n"
            "runner_module.subprocess=SignalDuringPopen()\n"
            "runner_module._process_identity=initially_unavailable_identity\n"
            "runner_module.Runner(output=io.StringIO()).run(\n"
            " [sys.executable, '-c', 'import time; time.sleep(3)'],\n"
            " cwd=pathlib.Path(sys.argv[3]), timeout_seconds=30)\n"
        )
        contender = (
            "import fcntl,os,pathlib,sys; "
            "lock=open(sys.argv[1], 'a+'); "
            "fcntl.flock(lock, fcntl.LOCK_EX); "
            "pid=int(pathlib.Path(sys.argv[2]).read_text()); "
            "alive=True; "
            "\ntry: os.kill(pid, 0)"
            "\nexcept ProcessLookupError: alive=False"
            "\npathlib.Path(sys.argv[3]).write_text('alive' if alive else 'dead')"
        )
        environment = dict(os.environ)
        existing_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (str(repository_root), existing_pythonpath)
            if part
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = root / "lane.lock"
            child_pid_path = root / "spawned-child.pid"
            contender_result = root / "spawn-contender-result"
            supervisor_process = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    supervisor,
                    str(lock_path),
                    str(child_pid_path),
                    str(root),
                    str(int(signal.SIGTERM)),
                ],
                cwd=root,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            contender_process: subprocess.Popen[str] | None = None
            try:
                self._wait_for_path(child_pid_path)
                contender_process = subprocess.Popen(
                    [
                        sys.executable,
                        "-c",
                        contender,
                        str(lock_path),
                        str(child_pid_path),
                        str(contender_result),
                    ],
                    cwd=root,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    text=True,
                )
                contender_process.wait(timeout=5)
                supervisor_process.wait(timeout=5)
                self._wait_for_path(contender_result)

                self.assertEqual(supervisor_process.returncode, -int(signal.SIGTERM))
                self.assertEqual(contender_result.read_text(), "dead")
            finally:
                for process in (contender_process, supervisor_process):
                    if process is not None and process.poll() is None:
                        process.kill()
                        process.wait(timeout=2)

    def test_persistent_identity_failure_never_executes_the_real_command(self) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        supervisor = (
            "import io,os,pathlib,sys\n"
            "from tools.dev import runner as runner_module\n"
            "real_subprocess=runner_module.subprocess\n"
            "class SignalDuringPopen:\n"
            " def Popen(self, *args, **kwargs):\n"
            "  process=real_subprocess.Popen(*args, **kwargs)\n"
            "  os.kill(os.getpid(), int(sys.argv[3]))\n"
            "  return process\n"
            " def __getattr__(self, name):\n"
            "  return getattr(real_subprocess, name)\n"
            "runner_module.subprocess=SignalDuringPopen()\n"
            "runner_module._process_identity=lambda pid, deadline: None\n"
            "runner_module.Runner(output=io.StringIO()).run(\n"
            " [sys.executable, '-c', "
            "  'import pathlib,sys; pathlib.Path(sys.argv[1]).write_text(\"started\")', "
            "  sys.argv[1]],\n"
            " cwd=pathlib.Path(sys.argv[2]), timeout_seconds=30)\n"
        )
        environment = dict(os.environ)
        existing_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (str(repository_root), existing_pythonpath)
            if part
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            command_started = root / "real-command-started"
            supervisor_process = subprocess.Popen(
                [
                    sys.executable,
                    "-c",
                    supervisor,
                    str(command_started),
                    str(root),
                    str(int(signal.SIGTERM)),
                ],
                cwd=root,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            supervisor_process.wait(timeout=5)
            self.assertEqual(supervisor_process.returncode, -int(signal.SIGTERM))
            self.assertFalse(command_started.exists())

    def test_signal_during_handler_restoration_keeps_default_exit_semantics(self) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        supervisor = (
            "import io,os,pathlib,signal,sys\n"
            "from tools.dev import runner as runner_module\n"
            "real_signal=signal.signal\n"
            "injected=False\n"
            "def signal_during_restore(signum, handler):\n"
            " global injected\n"
            " result=real_signal(signum, handler)\n"
            " if signum == signal.SIGHUP and handler == signal.SIG_DFL and not injected:\n"
            "  injected=True\n"
            "  os.kill(os.getpid(), signal.SIGTERM)\n"
            " return result\n"
            "runner_module.signal.signal=signal_during_restore\n"
            "runner_module.Runner(output=io.StringIO()).run(\n"
            " [sys.executable, '-c', 'pass'],\n"
            " cwd=pathlib.Path(sys.argv[1]), timeout_seconds=5)\n"
            "pathlib.Path(sys.argv[2]).write_text('returned')\n"
        )
        environment = dict(os.environ)
        existing_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (str(repository_root), existing_pythonpath)
            if part
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            returned = root / "runner-returned"
            supervisor_process = subprocess.Popen(
                [sys.executable, "-c", supervisor, str(root), str(returned)],
                cwd=root,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            supervisor_process.wait(timeout=5)
            self.assertEqual(supervisor_process.returncode, -int(signal.SIGTERM))
            self.assertFalse(returned.exists())

    def test_signal_immediately_before_restore_keeps_default_exit_semantics(self) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        supervisor = (
            "import io,os,pathlib,sys\n"
            "from tools.dev import runner as runner_module\n"
            "real_finalize=runner_module._SupervisorSignalHandlers.finalize\n"
            "injected=False\n"
            "def signal_before_restore(self, fds_to_close):\n"
            " global injected\n"
            " if not injected:\n"
            "  injected=True\n"
            "  os.kill(os.getpid(), runner_module.signal.SIGTERM)\n"
            " return real_finalize(self, fds_to_close)\n"
            "runner_module._SupervisorSignalHandlers.finalize=signal_before_restore\n"
            "runner_module.Runner(output=io.StringIO()).run(\n"
            " [sys.executable, '-c', 'pass'],\n"
            " cwd=pathlib.Path(sys.argv[1]), timeout_seconds=5)\n"
            "pathlib.Path(sys.argv[2]).write_text('returned')\n"
        )
        environment = dict(os.environ)
        existing_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (str(repository_root), existing_pythonpath)
            if part
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            returned = root / "runner-returned-before-restore"
            supervisor_process = subprocess.Popen(
                [sys.executable, "-c", supervisor, str(root), str(returned)],
                cwd=root,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            supervisor_process.wait(timeout=5)
            self.assertEqual(supervisor_process.returncode, -int(signal.SIGTERM))
            self.assertFalse(returned.exists())

    def test_unidentified_fallback_contains_detached_descendants(self) -> None:
        child = (
            "import os,pathlib,signal,sys,time; "
            "os.setsid(); "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
            "pathlib.Path(sys.argv[1]).write_text('ready'); "
            "time.sleep(0.8); pathlib.Path(sys.argv[2]).write_text('survived')"
        )
        leader = (
            "import pathlib,subprocess,sys,time; "
            f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1], sys.argv[2]], "
            "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
            "ready=pathlib.Path(sys.argv[1]); "
            "deadline=time.monotonic()+1; "
            "\nwhile not ready.exists() and time.monotonic() < deadline: time.sleep(0.005); "
            "\ntime.sleep(10)"
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ready = root / "unidentified-descendant-ready"
            marker = root / "unidentified-descendant-survived"
            process = subprocess.Popen(
                [sys.executable, "-c", leader, str(ready), str(marker)],
                cwd=root,
                start_new_session=True,
            )
            try:
                self._wait_for_path(ready)
                runner_module._terminate_unidentified_process_tree(process)
                self.assertIsNotNone(process.poll())
                time.sleep(0.9)
                self.assertFalse(marker.exists())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=2)

    def test_cli_ctrl_c_is_calm_exit_130_after_runner_cleanup(self) -> None:
        repository_root = Path(__file__).resolve().parents[3]
        child = (
            "import pathlib,sys,time; "
            "pathlib.Path(sys.argv[1]).write_text('ready'); "
            "time.sleep(3)"
        )
        supervisor = (
            "import io,pathlib,sys\n"
            "from tools.dev import cli\n"
            "from tools.dev.runner import Runner\n"
            f"child={child!r}\n"
            "def run_with_child(_args):\n"
            " Runner(output=io.StringIO()).run(\n"
            "  [sys.executable, '-c', child, sys.argv[1]],\n"
            "  cwd=pathlib.Path(sys.argv[2]), timeout_seconds=30)\n"
            " return 0\n"
            "cli.run=run_with_child\n"
            "raise SystemExit(cli.main(['worktrees']))\n"
        )
        environment = dict(os.environ)
        existing_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = os.pathsep.join(
            part
            for part in (str(repository_root), existing_pythonpath)
            if part
        )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ready = root / "cli-child-ready"
            supervisor_process = subprocess.Popen(
                [sys.executable, "-c", supervisor, str(ready), str(root)],
                cwd=root,
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            try:
                self._wait_for_path(ready)
                os.kill(supervisor_process.pid, signal.SIGINT)
                _stdout, stderr = supervisor_process.communicate(timeout=5)
                self.assertEqual(supervisor_process.returncode, 130)
                self.assertEqual(stderr.strip().splitlines()[-1], "litch: interrupted")
            finally:
                if supervisor_process.poll() is None:
                    supervisor_process.kill()
                    supervisor_process.wait(timeout=2)

    def test_custom_and_ignored_supervisor_handlers_are_not_replaced(self) -> None:
        def custom_handler(_signum: int, _frame: object) -> None:
            pass

        handlers = runner_module._SupervisorSignalHandlers(enabled=True)
        with (
            mock.patch.object(
                runner_module.signal,
                "getsignal",
                side_effect=(custom_handler, signal.SIG_IGN, custom_handler),
            ),
            mock.patch.object(runner_module.signal, "signal") as install,
        ):
            handlers.install()
            handlers.restore()
        install.assert_not_called()

    def test_run_restores_supervisor_signal_handlers(self) -> None:
        previous = {
            signum: signal.getsignal(signum)
            for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)
        }
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            runner.run(
                [sys.executable, "-c", "pass"],
                cwd=Path(temporary),
                timeout_seconds=5,
            )
        self.assertEqual(
            {
                signum: signal.getsignal(signum)
                for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT)
            },
            previous,
        )

    def test_capture_uses_argv_without_shell_expansion(self) -> None:
        output = io.StringIO()
        runner = Runner(output=output)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = runner.run(
                [sys.executable, "-c", "import sys; print(sys.argv[1])", "$(not-a-shell)"],
                cwd=root,
                timeout_seconds=5,
                capture=True,
            )
        self.assertEqual(result.stdout.strip(), "$(not-a-shell)")
        self.assertIn("'$(not-a-shell)'", output.getvalue())
        self.assertRegex(output.getvalue(), r"\[completed in \d+\.\d{2}s; exit 0\]")

    def test_failed_command_reports_elapsed_time_and_keeps_exit_status(self) -> None:
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(DevError) as error:
                Runner(output=output).run(
                    [sys.executable, "-c", "raise SystemExit(7)"],
                    cwd=Path(temporary),
                    timeout_seconds=5,
                    capture=True,
                )
        self.assertEqual(error.exception.exit_code, 7)
        self.assertRegex(output.getvalue(), r"\[completed in \d+\.\d{2}s; exit 7\]")

    def test_command_announcement_can_be_suppressed_without_losing_capture(self) -> None:
        output = io.StringIO()
        runner = Runner(output=output, announce_commands=False)
        with tempfile.TemporaryDirectory() as temporary:
            result = runner.run(
                [sys.executable, "-c", "print('captured')"],
                cwd=Path(temporary),
                timeout_seconds=5,
                capture=True,
            )

        self.assertEqual(result.stdout.strip(), "captured")
        self.assertEqual(output.getvalue(), "")

    def test_live_capture_is_forwarded_before_the_child_exits_and_retained(self) -> None:
        output = io.StringIO()
        runner = Runner(output=output, announce_commands=False)
        received: list[tuple[str, str]] = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            release = root / "release"
            child = (
                "import pathlib,sys,time; "
                "print('half', flush=True); "
                "release=pathlib.Path(sys.argv[1]); "
                "deadline=time.monotonic()+2; "
                "\nwhile not release.exists() and time.monotonic() < deadline: time.sleep(0.01); "
                "\nif not release.exists(): raise SystemExit(7); "
                "\nprint('done', flush=True)"
            )

            def forward(stream_name: str, chunk: str) -> None:
                received.append((stream_name, chunk))
                if stream_name == "stdout" and "half" in chunk:
                    release.write_text("continue")

            for capture in (True, False):
                with self.subTest(capture=capture):
                    release.unlink(missing_ok=True)
                    received.clear()
                    result = runner.run(
                        [sys.executable, "-c", child, str(release)],
                        cwd=root,
                        timeout_seconds=5,
                        capture=capture,
                        live_output=forward,
                    )
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(result.stdout.splitlines(), ["half", "done"] if capture else [])
                    self.assertEqual(result.stderr, "")
                    self.assertTrue(any(name == "stdout" and "half" in chunk for name, chunk in received))
            with self.assertRaisesRegex(DevError, "interactive"):
                runner.run([sys.executable, "-c", child, str(release)], cwd=root,
                           timeout_seconds=5, interactive=True, live_output=forward)

    def test_timeout_is_bounded_and_actionable(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(
                DevError, r"exceeded 0.05s \(elapsed \d+\.\d{2}s including cleanup\)"
            ):
                runner.run(
                    [sys.executable, "-c", "import time; time.sleep(2)"],
                    cwd=Path(temporary),
                    timeout_seconds=0.05,
                    capture=True,
                )

    def test_timeout_terminates_descendant_process_group(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker = root / "descendant-survived"
            child = (
                "import pathlib,sys,time; "
                "time.sleep(0.8); pathlib.Path(sys.argv[1]).write_text('survived')"
            )
            parent = (
                "import subprocess,sys,time; "
                f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1]]); "
                "time.sleep(10)"
            )
            with self.assertRaisesRegex(DevError, "exceeded"):
                runner.run(
                    [sys.executable, "-c", parent, str(marker)],
                    cwd=root,
                    timeout_seconds=0.2,
                    capture=True,
                )
            time.sleep(0.9)
            self.assertFalse(marker.exists())

    def test_timeout_terminates_descendant_after_group_leader_exits(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            marker = root / "descendant-survived"
            child = (
                "import pathlib,sys,time; "
                "time.sleep(0.8); pathlib.Path(sys.argv[1]).write_text('survived')"
            )
            parent = (
                "import os,subprocess,sys; "
                f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1]]); "
                "os._exit(0)"
            )
            with self.assertRaisesRegex(DevError, "exceeded"):
                runner.run(
                    [sys.executable, "-c", parent, str(marker)],
                    cwd=root,
                    timeout_seconds=0.2,
                    capture=True,
                )
            time.sleep(0.9)
            self.assertFalse(marker.exists())

    def test_timeout_force_kills_term_resistant_descendant(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ready = root / "descendant-ready"
            marker = root / "descendant-survived"
            child = (
                "import pathlib,signal,sys,time; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "pathlib.Path(sys.argv[1]).write_text('ready'); "
                "time.sleep(0.8); pathlib.Path(sys.argv[2]).write_text('survived')"
            )
            parent = (
                "import pathlib,subprocess,sys,time; "
                f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1], sys.argv[2]], "
                "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
                "ready=pathlib.Path(sys.argv[1]); "
                "deadline=time.monotonic()+1; "
                "\nwhile not ready.exists() and time.monotonic() < deadline: time.sleep(0.005); "
                "\ntime.sleep(10)"
            )
            with self.assertRaisesRegex(DevError, "exceeded"):
                runner.run(
                    [sys.executable, "-c", parent, str(ready), str(marker)],
                    cwd=root,
                    timeout_seconds=0.3,
                )
            self.assertTrue(ready.exists())
            time.sleep(0.9)
            self.assertFalse(marker.exists())

    def test_timeout_force_kills_term_resistant_setsid_descendant(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ready = root / "escaped-descendant-ready"
            marker = root / "escaped-descendant-survived"
            child = (
                "import os,pathlib,signal,sys,time; "
                "os.setsid(); "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "pathlib.Path(sys.argv[1]).write_text('ready'); "
                "time.sleep(0.8); pathlib.Path(sys.argv[2]).write_text('survived')"
            )
            parent = (
                "import pathlib,subprocess,sys,time; "
                f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1], sys.argv[2]], "
                "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); "
                "ready=pathlib.Path(sys.argv[1]); "
                "deadline=time.monotonic()+1; "
                "\nwhile not ready.exists() and time.monotonic() < deadline: time.sleep(0.005); "
                "\ntime.sleep(10)"
            )
            with self.assertRaisesRegex(DevError, "exceeded"):
                runner.run(
                    [sys.executable, "-c", parent, str(ready), str(marker)],
                    cwd=root,
                    timeout_seconds=0.3,
                )
            self.assertTrue(ready.exists())
            time.sleep(0.9)
            self.assertFalse(marker.exists())

    def test_timeout_kills_reparented_setsid_child_holding_capture_pipe(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            ready = root / "reparented-descendant-ready"
            marker = root / "reparented-descendant-survived"
            child = (
                "import os,pathlib,signal,sys,time; "
                "os.setsid(); "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "pathlib.Path(sys.argv[1]).write_text('ready'); "
                "time.sleep(0.8); pathlib.Path(sys.argv[2]).write_text('survived')"
            )
            parent = (
                "import os,pathlib,subprocess,sys,time; "
                f"child_process=subprocess.Popen([sys.executable, '-c', {child!r}, sys.argv[1], sys.argv[2]]); "
                "ready=pathlib.Path(sys.argv[1]); "
                "deadline=time.monotonic()+1; "
                "\nwhile not ready.exists() and time.monotonic() < deadline: time.sleep(0.005); "
                "\nos._exit(0)"
            )
            with self.assertRaisesRegex(DevError, "exceeded"):
                runner.run(
                    [sys.executable, "-c", parent, str(ready), str(marker)],
                    cwd=root,
                    timeout_seconds=0.3,
                    capture=True,
                )
            self.assertTrue(ready.exists())
            time.sleep(0.9)
            self.assertFalse(marker.exists())

    def test_term_handler_cannot_escape_a_new_detached_capture_holder(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child_ready = root / "term-child-ready"
            marker = root / "term-child-survived"
            child = (
                "import os,pathlib,signal,sys,time; "
                "os.setsid(); "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                "pathlib.Path(sys.argv[1]).write_text('ready'); "
                "time.sleep(0.8); pathlib.Path(sys.argv[2]).write_text('survived')"
            )
            parent = (
                "import os,pathlib,signal,subprocess,sys,time; "
                "ready=pathlib.Path(sys.argv[1]); "
                f"child={child!r}\n"
                "def handle(signum, frame):\n"
                " child_process=subprocess.Popen([sys.executable, '-c', child, sys.argv[1], sys.argv[2]])\n"
                " deadline=time.monotonic()+1\n"
                " while not ready.exists() and time.monotonic() < deadline: time.sleep(0.005)\n"
                " os._exit(0)\n"
                "signal.signal(signal.SIGTERM, handle); "
                "time.sleep(10)"
            )
            with self.assertRaisesRegex(DevError, "exceeded"):
                runner.run(
                    [
                        sys.executable,
                        "-c",
                        parent,
                        str(child_ready),
                        str(marker),
                    ],
                    cwd=root,
                    timeout_seconds=0.3,
                    capture=True,
                )
            self.assertTrue(child_ready.exists())
            time.sleep(0.9)
            self.assertFalse(marker.exists())

    def test_timeout_cleanup_never_signals_a_raw_process_group(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            with (
                mock.patch.object(
                    runner_module.os,
                    "killpg",
                    side_effect=AssertionError("unsafe raw group signal"),
                ) as killpg,
                self.assertRaisesRegex(DevError, "exceeded"),
            ):
                runner.run(
                    [sys.executable, "-c", "import time; time.sleep(2)"],
                    cwd=Path(temporary),
                    timeout_seconds=0.05,
                    capture=True,
                )
        killpg.assert_not_called()

    def test_cleanup_does_not_signal_a_recycled_descendant_pid(self) -> None:
        expected = runner_module._ProcessIdentity(
            pid=1234,
            ppid=1233,
            pgid=1234,
            birth_token="darwin:100:1",
        )
        recycled = runner_module._ProcessIdentity(
            pid=1234,
            ppid=1,
            pgid=1234,
            birth_token="darwin:101:2",
        )
        with (
            mock.patch.object(
                runner_module,
                "_current_process_identity",
                return_value=recycled,
            ),
            mock.patch.object(runner_module.os, "kill") as kill,
        ):
            runner_module._signal_retained_processes(
                (expected,),
                signum=runner_module.signal.SIGKILL,
                deadline=time.monotonic() + 1,
            )
        kill.assert_not_called()

    def test_reaped_leader_pid_reuse_cannot_adopt_a_new_group_or_tree(self) -> None:
        original = runner_module._ProcessIdentity(
            pid=1234,
            ppid=1200,
            pgid=1234,
            birth_token="darwin:100:1",
        )
        recycled = runner_module._ProcessIdentity(
            pid=1234,
            ppid=1,
            pgid=1234,
            birth_token="darwin:101:2",
        )
        process = mock.Mock(pid=1234, stdout=None, stderr=None)
        with (
            mock.patch.object(
                runner_module,
                "_current_process_identity",
                return_value=recycled,
            ),
            mock.patch.object(
                runner_module,
                "_snapshot_group_members",
            ) as snapshot_group,
            mock.patch.object(
                runner_module,
                "_snapshot_descendants",
            ) as snapshot_descendants,
            mock.patch.object(
                runner_module,
                "_snapshot_capture_pipe_holders",
                return_value=(),
            ),
        ):
            retained = runner_module._snapshot_cleanup_processes(
                process,
                leader_identity=original,
                deadline=time.monotonic() + 1,
            )
        self.assertEqual(retained, (original,))
        snapshot_group.assert_not_called()
        snapshot_descendants.assert_not_called()

    def test_linux_pidfd_revalidates_birth_after_open_before_signal(self) -> None:
        expected = runner_module._ProcessIdentity(
            pid=1234,
            ppid=1200,
            pgid=1234,
            birth_token="linux:100",
        )
        recycled = runner_module._ProcessIdentity(
            pid=1234,
            ppid=1,
            pgid=1234,
            birth_token="linux:101",
        )
        with (
            mock.patch.object(runner_module.sys, "platform", "linux"),
            mock.patch.object(
                runner_module.os,
                "pidfd_open",
                return_value=77,
                create=True,
            ),
            mock.patch.object(
                runner_module.signal,
                "pidfd_send_signal",
                create=True,
            ) as pidfd_send_signal,
            mock.patch.object(
                runner_module,
                "_native_process_identity",
                return_value=recycled,
            ),
            mock.patch.object(runner_module.os, "close") as close,
        ):
            handled = runner_module._signal_with_linux_pidfd(
                expected,
                signum=runner_module.signal.SIGKILL,
            )
        self.assertTrue(handled)
        pidfd_send_signal.assert_not_called()
        close.assert_called_once_with(77)

    def test_noninteractive_timeout_must_be_finite_and_positive(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for timeout in (None, 0, float("inf"), float("nan")):
                with self.subTest(timeout=timeout):
                    with self.assertRaisesRegex(DevError, "finite positive"):
                        runner.run(
                            [sys.executable, "-c", "pass"],
                            cwd=root,
                            timeout_seconds=timeout,
                        )

    def test_interactive_run_does_not_require_process_identity_containment(self) -> None:
        runner = Runner(output=io.StringIO())
        with (
            tempfile.TemporaryDirectory() as temporary,
            mock.patch.object(
                runner_module,
                "_record_leader_identity",
                side_effect=AssertionError(
                    "interactive commands must not enter identity containment"
                ),
            ) as record_identity,
        ):
            result = runner.run(
                [sys.executable, "-c", "pass"],
                cwd=Path(temporary),
                timeout_seconds=None,
                interactive=True,
            )
        self.assertEqual(result.returncode, 0)
        record_identity.assert_not_called()

    def test_nonzero_capture_includes_diagnostics(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(DevError, "specific failure"):
                runner.run(
                    [sys.executable, "-c", "import sys; print('specific failure', file=sys.stderr); sys.exit(7)"],
                    cwd=Path(temporary),
                    timeout_seconds=5,
                    capture=True,
                )

    def test_gated_exec_preserves_missing_command_diagnostic(self) -> None:
        runner = Runner(output=io.StringIO())
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(
                DevError,
                "required command not found: definitely-not-a-real-litch-tool",
            ) as caught:
                runner.run(
                    ["definitely-not-a-real-litch-tool"],
                    cwd=Path(temporary),
                    timeout_seconds=5,
                    capture=True,
                )
        self.assertEqual(caught.exception.exit_code, 127)

    def test_display_argv_quotes_tokens(self) -> None:
        self.assertEqual(display_argv(("tool", "two words")), "tool 'two words'")


if __name__ == "__main__":
    unittest.main()
