"""Build once, then supervise every registered native suite independently.

Nested Unity summaries are case data, not the end of the outer qualification.
A failed, signaled or stalled suite cannot hide the remaining registered suites.
"""

import os
import re
import selectors
import signal
import subprocess
import time

import click

from platformio.test.exception import UnitTestError
from platformio.test.result import TestCase, TestStatus
from platformio.test.runners.readers.native import NativeTestOutputReader
from platformio.test.runners.unity import UnityTestRunner
from platformio.util import strip_ansi_codes


class NativeAggregateReader(NativeTestOutputReader):
    """Reuse one build while bounding each registered suite's process lifetime."""

    suite_timeout = 120.0
    listing_timeout = 10.0
    listing_limit = 128 * 1024
    diagnostic_limit = 16 * 1024
    termination_grace = 1.0
    reap_timeout = 5.0

    def __init__(self, test_runner):
        # Only get_testing_command is inherited. This reader owns no async loop.
        self.test_runner = test_runner

    def _stop(self, process):
        # A descendant may retain the pipe after its leader has already exited.
        previous = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            for sig in previous:
                signal.signal(sig, signal.SIG_IGN)
            for sig, timeout in ((signal.SIGTERM, self.termination_grace),
                                 (signal.SIGKILL, self.reap_timeout)):
                try:
                    os.killpg(process.pid, sig)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    if sig == signal.SIGKILL:
                        raise UnitTestError("Could not reap native suite process")
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)

    def _run(self, command, environment, timeout, *, capture=False):
        process = None
        handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
        pending = []

        def defer(signum, _frame):
            if signum not in pending:
                pending.append(signum)

        deadline = time.monotonic() + timeout
        output = bytearray()
        try:
            # Popen may be interrupted after fork but before it returns its
            # owner. Defer cancellation until that owner is inside cleanup.
            for sig in handlers:
                signal.signal(sig, defer)
            process = subprocess.Popen(
                command, env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, start_new_session=True,
            )
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            for sig in pending:
                handler = handlers[sig]
                if callable(handler):
                    handler(sig, None)
                elif handler != signal.SIG_IGN:
                    raise SystemExit(128 + sig)
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(command, timeout)
                    for key, _ in selector.select(min(remaining, 0.2)):
                        data = os.read(key.fd, 4096)
                        if not data:
                            selector.unregister(key.fileobj)
                        elif capture:
                            if len(output) + len(data) > self.listing_limit:
                                raise UnitTestError("Native suite listing exceeds its bound")
                            output.extend(data)
                        else:
                            output.extend(data)
                            del output[:-self.diagnostic_limit]
                            self.test_runner.on_testing_data_output(data)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(command, timeout)
            return process.wait(timeout=remaining), bytes(output)
        except BaseException as error:
            if isinstance(error, subprocess.TimeoutExpired):
                error.output = bytes(output)
            if process is not None:
                self._stop(process)
            raise
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            if process is not None:
                process.stdout.close()

    @staticmethod
    def _suite_names(listing):
        try:
            lines = listing.decode("utf-8").splitlines()
        except UnicodeDecodeError as error:
            raise UnitTestError("Invalid native suite listing encoding") from error
        if not lines or lines[0] != "[ALL_NATIVE] suites:":
            raise UnitTestError("Missing native suite registry")
        names = []
        for line in lines[1:]:
            match = re.fullmatch(r"\[ALL_NATIVE\]  - (test_[A-Za-z0-9_]+)", line)
            if match is None:
                raise UnitTestError("Malformed native suite registry")
            names.append(match.group(1))
        if not names or len(set(names)) != len(names):
            raise UnitTestError("Empty or duplicate native suite registry")
        return names

    def begin(self):
        if os.name != "posix":
            raise UnitTestError("Native aggregate supervision requires macOS or Linux")
        previous_term = signal.getsignal(signal.SIGTERM)

        def interrupted(signum, _frame):
            raise SystemExit(128 + signum)

        signal.signal(signal.SIGTERM, interrupted)
        try:
            command = self.get_testing_command()
            if "--list" in command[1:]:
                raise UnitTestError("List-only native execution cannot qualify suites")
            environment = dict(os.environ)
            try:
                code, listing = self._run(
                    [*command, "--list"], environment, self.listing_timeout, capture=True,
                )
            except subprocess.TimeoutExpired as error:
                raise UnitTestError("Native suite listing timed out") from error
            if code != 0:
                raise UnitTestError(f"Native suite listing exited with {code}")
            names = self._suite_names(listing)
            selected = environment.get("LITCH_NATIVE_TEST_FILTER", "")
            if selected:
                if selected not in names:
                    raise UnitTestError(f"No native suite matched {selected!r}")
                names = [selected]
            failures = 0
            for name in names:
                self.test_runner.on_testing_line_output(f"[ALL_NATIVE] launch {name}\n")
                started = time.monotonic()
                first_case = len(self.test_runner.test_suite.cases)
                self.test_runner._native_begun = None
                status, message, error = TestStatus.PASSED, None, None
                code = None
                output = b""
                try:
                    code, output = self._run(
                        command, {**environment, "LITCH_NATIVE_TEST_FILTER": name},
                        self.suite_timeout,
                    )
                except subprocess.TimeoutExpired as timeout:
                    status = TestStatus.ERRORED
                    message = f"timed out after {self.suite_timeout:g}s"
                    error = UnitTestError(message)
                    output = timeout.output or b""
                finally:
                    if self.test_runner._testing_output_buffer:
                        self.test_runner.on_testing_data_output("\n")
                if status == TestStatus.PASSED:
                    if code < 0:
                        status = TestStatus.ERRORED
                        message = f"terminated by signal {-code}"
                        error = UnitTestError(message)
                    elif code != 0:
                        status, message = TestStatus.FAILED, f"exited with {code}"
                    elif self.test_runner._native_begun != name:
                        status = TestStatus.ERRORED
                        message = "did not enter its registered body"
                        error = UnitTestError(message)
                    elif any(case.status in (TestStatus.FAILED, TestStatus.ERRORED)
                             for case in self.test_runner.test_suite.cases[first_case:]):
                        status, message = TestStatus.FAILED, "reported failed assertions"
                if status != TestStatus.PASSED:
                    failures += 1
                    if output and not self.test_runner.options.verbose:
                        click.echo(
                            f"[ALL_NATIVE] diagnostics {name}: last {len(output)} output bytes"
                        )
                        click.echo(strip_ansi_codes(output.decode("utf-8", errors="replace")),
                                   nl=False)
                        if not output.endswith(b"\n"):
                            click.echo()
                    prefix = "TIMEOUT" if message.startswith("timed out") else "FAIL"
                    self.test_runner.on_testing_line_output(
                        f"[ALL_NATIVE] {prefix}: {name} ({message})\n"
                    )
                self.test_runner.test_suite.add_case(TestCase(
                    name=name, status=status, message=message, exception=error,
                    duration=time.monotonic() - started,
                ))
            self.test_runner.on_testing_line_output(
                f"[ALL_NATIVE] failures={failures}\n" if failures else "[ALL_NATIVE] ok\n"
            )
        finally:
            signal.signal(signal.SIGTERM, previous_term)


class CustomTestRunner(UnityTestRunner):
    """Retain Unity case parsing without accepting nested end markers."""

    def stage_testing(self):
        if self.options.without_testing:
            return None
        if self.test_suite.env_name == "native" and self.test_suite.test_name == "test_all_native":
            click.secho("Testing registered suites in isolated processes...", bold=True)
            return NativeAggregateReader(self).begin()
        result = super().stage_testing()
        if not self.test_suite.cases:
            self.test_suite.add_case(TestCase(
                name=self.test_suite.test_name, status=TestStatus.PASSED,
            ))
        return result

    def on_testing_line_output(self, line):
        if self.options.verbose:
            click.echo(line, nl=False)

        normalized = strip_ansi_codes(line or "").strip()
        if not normalized:
            return
        if normalized.startswith("[ALL_NATIVE] begin "):
            self._native_begun = normalized[len("[ALL_NATIVE] begin "):]

        if not self.options.verbose and normalized.startswith(
            (
                "[FAIL]",
                "[ALL_NATIVE] FAIL:",
                "[ALL_NATIVE] failures=",
                "[ALL_NATIVE] begin ",
                "[ALL_NATIVE] launch ",
                "[ALL_NATIVE] TIMEOUT:",
                "[ALL_NATIVE] suite ",
            )
        ):
            # Custom suites do not emit Unity cases. Keep failures and suite
            # progress visible without claiming the process has finished.
            click.echo(line, nl=False)

        test_case = self.parse_test_case(normalized)
        if test_case:
            self.test_suite.add_case(test_case)
            if not self.options.verbose:
                click.echo(test_case.humanize())
