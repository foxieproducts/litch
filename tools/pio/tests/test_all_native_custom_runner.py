import importlib.util
import io
import os
import re
import runpy
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from platformio.test.exception import UnitTestError
from platformio.test.result import TestStatus, TestSuite


RUNNER_PATH = (
    Path(__file__).resolve().parents[3]
    / "test"
    / "test_custom_runner.py"
)
GENERATOR_PATH = RUNNER_PATH.parents[1] / "tools/pio/generate_test_all_native.py"


def load_runner_module():
    spec = importlib.util.spec_from_file_location(
        "litch_all_native_custom_runner", RUNNER_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load custom runner from {RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runner_class():
    return load_runner_module().CustomTestRunner


class AllNativeCustomRunnerTest(unittest.TestCase):
    def make_runner(self):
        runner = object.__new__(load_runner_class())
        runner.options = types.SimpleNamespace(verbose=0, without_testing=False, program_args=None)
        runner.test_suite = TestSuite("native", "test_all_native")
        runner.test_suite.on_start()
        runner._testing_output_buffer = ""
        return runner

    def test_nested_unity_summary_does_not_finish_aggregate(self):
        runner = self.make_runner()

        runner.on_testing_line_output(
            "nested/test_main.cpp:12:TestNested:PASS\n"
        )
        runner.on_testing_line_output("1 Tests 0 Failures 0 Ignored\n")

        self.assertFalse(runner.test_suite.is_finished())
        self.assertEqual(1, len(runner.test_suite.cases))

    def test_aggregate_success_marker_does_not_preempt_process_exit(self):
        runner = self.make_runner()

        runner.on_testing_line_output("[ALL_NATIVE] ok\n")

        self.assertFalse(runner.test_suite.is_finished())

    def test_aggregate_failure_marker_does_not_preempt_process_exit(self):
        runner = self.make_runner()

        runner.on_testing_line_output("[ALL_NATIVE] failures=1\n")

        self.assertFalse(runner.test_suite.is_finished())

    def test_nonverbose_custom_failures_remain_visible(self):
        for line in (
            "[FAIL] empty square retained a piece light\n",
            "[ALL_NATIVE] FAIL: test_empty_piece_led_virtual_litch_journey\n",
            "[ALL_NATIVE] failures=1\n",
        ):
            with self.subTest(line=line):
                runner = self.make_runner()
                output = io.StringIO()
                with redirect_stdout(output):
                    runner.on_testing_line_output(line)
                self.assertEqual(line, output.getvalue())
                self.assertFalse(runner.test_suite.is_finished())
                self.assertEqual(0, len(runner.test_suite.cases))

    def test_verbose_custom_failures_are_printed_once(self):
        runner = self.make_runner()
        runner.options.verbose = 1
        line = "[ALL_NATIVE] FAIL: test_example\n"
        output = io.StringIO()
        with redirect_stdout(output):
            runner.on_testing_line_output(line)
        self.assertEqual(line, output.getvalue())
        self.assertFalse(runner.test_suite.is_finished())

    def test_nonverbose_ordinary_output_stays_quiet(self):
        runner = self.make_runner()
        output = io.StringIO()
        with redirect_stdout(output):
            runner.on_testing_line_output("[ALL_NATIVE] start\n")
            runner.on_testing_line_output("[ALL_NATIVE] ok\n")
            runner.on_testing_line_output("expected fault log contains [FAIL]\n")
        self.assertEqual("", output.getvalue())
        self.assertFalse(runner.test_suite.is_finished())

    def test_nonverbose_suite_timing_is_visible_without_finishing_aggregate(self):
        runner = self.make_runner()
        output = io.StringIO()
        lines = (
            "[ALL_NATIVE] launch test_example\n",
            "[ALL_NATIVE] begin test_example\n",
            "[ALL_NATIVE] suite test_example elapsed_ms=120 rc=0\n",
            "[ALL_NATIVE] TIMEOUT: test_stuck (timed out after 120s)\n",
        )
        with redirect_stdout(output):
            for line in lines:
                runner.on_testing_line_output(line)
        self.assertEqual(output.getvalue(), "".join(lines))
        self.assertFalse(runner.test_suite.is_finished())
        self.assertEqual(len(runner.test_suite.cases), 0)


class NativeSuiteTimingProcessTest(unittest.TestCase):
    def test_actual_runner_reports_each_executed_suite_and_retains_filter_failures(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler, "Native runner regression requires the host C++ compiler")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = RUNNER_PATH.parent / "test_all_native/test_main.cpp"
            (root / "test_main.cpp").write_bytes(source.read_bytes())
            (root / "generated").mkdir()
            suites = ["test_ok", "test_bad", "test_signal", "test_hang", "test_partial", "test_after"]
            (root / "generated/all_native_registry.hpp").write_text(
                "#define ALL_NATIVE_TEST_SUITES(X) " + " ".join(f"X({name})" for name in suites) + "\n"
            )
            (root / "foxie_embedded").mkdir()
            (root / "foxie_embedded/millis.hpp").write_text(
                "namespace fxe { inline unsigned ManualMillisProvider() { return 0; } "
                "inline void SetMillisProviderForTests(unsigned (*)()) {} "
                "struct ManualMillis { static void SetNowMs(unsigned) {} }; }\n"
            )
            (root / "suites.cpp").write_text(
                '#include <csignal>\n#include <cstdio>\n#include <cstdlib>\n#include <unistd.h>\n'
                'struct BeforeMain { BeforeMain() { if (std::getenv("TEST_LISTING_HANG")) '
                'for (;;) pause(); } } beforeMain;\n'
                'int test_ok_main() { return 0; }\nint test_bad_main() { return 1; }\n'
                'int test_signal_main() { std::fprintf(stderr,"discarded-stderr-prefix\\n"); '
                'for (int i=0; i<256; ++i) std::fprintf(stderr,"diagnostic padding %0256d\\n", i); '
                'std::fprintf(stderr,"reason: controlled signal failure\\n"); '
                'std::raise(SIGTERM); return 0; }\n'
                'int test_hang_main() { std::signal(SIGCHLD, SIG_IGN); '
                'std::signal(SIGTERM, SIG_IGN); const auto child = fork(); '
                'if (child < 0) return 1; if (child == 0) { std::signal(SIGTERM, SIG_DFL); '
                'for (;;) pause(); } std::printf("hang_pgid=%ld child_pid=%ld\\n", '
                '(long)getpid(), (long)child); std::printf("partial-without-newline"); '
                'std::fflush(stdout); for (;;) pause(); }\n'
                'int test_partial_main() { std::printf("nested.cpp:1:Partial:FAIL:unterminated"); '
                'std::fflush(stdout); std::_Exit(0); }\n'
                'int test_after_main() { return 0; }\n'
            )
            program = root / "program"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-I", str(root), str(root / "test_main.cpp"),
                 str(root / "suites.cpp"), "-o", str(program)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            listing = subprocess.run(
                [str(program), "--list"],
                env={**os.environ, "LITCH_NATIVE_TEST_FILTER": "test_hang"},
                capture_output=True, timeout=5,
            )
            self.assertEqual(listing.returncode, 0)
            module = load_runner_module()
            self.assertEqual(module.NativeAggregateReader._suite_names(listing.stdout), suites)
            for selected, code, expected_suites in (("test_ok", 0, ["test_ok"]),
                                                    ("missing", 1, [])):
                with self.subTest(selected=selected):
                    result = subprocess.run(
                        [str(program)], env={**os.environ, "LITCH_NATIVE_TEST_FILTER": selected},
                        capture_output=True, text=True, timeout=5,
                    )
                    self.assertEqual(result.returncode, code, result.stdout)
                    timings = re.findall(
                        r"^\[ALL_NATIVE\] suite (\w+) elapsed_ms=(\d+) rc=([01])$",
                        result.stdout, re.MULTILINE,
                    )
                    self.assertEqual([row[0] for row in timings], expected_suites)
                    begins = re.findall(
                        r"^\[ALL_NATIVE\] begin (\w+)$", result.stdout, re.MULTILINE
                    )
                    self.assertEqual(begins, expected_suites)
                    for suite in expected_suites:
                        self.assertLess(
                            result.stdout.index(f"[ALL_NATIVE] begin {suite}\n"),
                            result.stdout.index(f"[ALL_NATIVE] suite {suite} elapsed_ms="),
                        )

            runner = AllNativeCustomRunnerTest().make_runner()
            runner.project_config = types.SimpleNamespace(
                get=lambda section, key: [str(program)] if key == "test_testing_command" else ""
            )
            reader = module.NativeAggregateReader(runner)
            reader.suite_timeout, reader.termination_grace = 0.15, 0.1
            output = io.StringIO()
            prior_term = signal.getsignal(signal.SIGTERM)
            started = time.monotonic()
            with mock.patch.dict(os.environ, {"LITCH_NATIVE_TEST_FILTER": ""}), redirect_stdout(output):
                reader.begin()
            self.assertLess(time.monotonic() - started, 5)
            self.assertEqual(signal.getsignal(signal.SIGTERM), prior_term)
            native_cases = [case for case in runner.test_suite.cases if case.name in suites]
            self.assertEqual([case.name for case in native_cases], suites)
            self.assertEqual([case.status for case in native_cases], [
                TestStatus.PASSED, TestStatus.FAILED, TestStatus.ERRORED,
                TestStatus.ERRORED, TestStatus.FAILED, TestStatus.PASSED,
            ])
            self.assertEqual(re.findall(r"^\[ALL_NATIVE\] launch (\w+)$", output.getvalue(), re.M), suites)
            self.assertIn("[ALL_NATIVE] TIMEOUT: test_hang", output.getvalue())
            self.assertIn("reason: controlled signal failure", output.getvalue())
            self.assertNotIn("discarded-stderr-prefix", output.getvalue())
            signal_output = output.getvalue().split("[ALL_NATIVE] diagnostics test_signal:", 1)[1]
            signal_output = signal_output.split("[ALL_NATIVE] launch test_hang", 1)[0]
            self.assertLess(len(signal_output), reader.diagnostic_limit + 200)
            self.assertIn("partial-without-newline\n[ALL_NATIVE] TIMEOUT", output.getvalue())
            self.assertEqual(runner._testing_output_buffer, "")
            group = int(re.search(r"hang_pgid=(\d+)", output.getvalue()).group(1))
            with self.assertRaises(ProcessLookupError):
                os.killpg(group, 0)
            with mock.patch.dict(os.environ, {"LITCH_NATIVE_TEST_FILTER": "missing"}):
                with self.assertRaisesRegex(UnitTestError, "No native suite matched"):
                    reader.begin()
            for invalid in (b"", b"[ALL_NATIVE] suites:\n", b"bad\n",
                            b"[ALL_NATIVE] suites:\n[ALL_NATIVE]  - test_ok\nbad\n",
                            b"[ALL_NATIVE] suites:\n[ALL_NATIVE]  - test_ok\n[ALL_NATIVE]  - test_ok\n"):
                with self.subTest(listing=invalid), self.assertRaises(UnitTestError):
                    reader._suite_names(invalid)
            original_get = runner.project_config.get
            try:
                runner.project_config.get = lambda section, key: [str(program), "--list"]
                with self.assertRaisesRegex(UnitTestError, "List-only"):
                    reader.begin()
                fake_listing = "print('[ALL_NATIVE] suites:\\n[ALL_NATIVE]  - test_not_executed')"
                runner.project_config.get = lambda section, key: [sys.executable, "-c", fake_listing]
                with mock.patch.dict(os.environ, {"LITCH_NATIVE_TEST_FILTER": ""}), redirect_stdout(io.StringIO()):
                    reader.begin()
                never_entered = runner.test_suite.cases[-1]
                self.assertEqual(never_entered.name, "test_not_executed")
                self.assertEqual(never_entered.status, TestStatus.ERRORED)
                self.assertIn("did not enter", never_entered.message)
            finally:
                runner.project_config.get = original_get
            reader.listing_timeout = 0.1
            with mock.patch.dict(os.environ, {"TEST_LISTING_HANG": "1"}):
                with self.assertRaisesRegex(UnitTestError, "listing timed out"):
                    reader.begin()
            with self.assertRaisesRegex(UnitTestError, "exceeds its bound"):
                reader._run([sys.executable, "-c", "print('x' * 140000)"],
                            dict(os.environ), 5, capture=True)

            # Interrupt inside Popen after the real child exists but before
            # its owner is returned; both signals must still reap that group.
            real_popen = subprocess.Popen
            spawned = []
            for sig, exception in ((signal.SIGTERM, SystemExit), (signal.SIGINT, KeyboardInterrupt)):
                def cancel_during_spawn(command, **kwargs):
                    process = real_popen(command, **kwargs)
                    if "--list" not in command:
                        spawned.append(process)
                        signal.raise_signal(sig)
                    return process

                try:
                    with mock.patch.dict(os.environ, {"LITCH_NATIVE_TEST_FILTER": "test_hang"}), \
                         mock.patch.object(module.subprocess, "Popen", side_effect=cancel_during_spawn), \
                         redirect_stdout(io.StringIO()), self.assertRaises(exception):
                        reader.begin()
                    self.assertTrue(spawned)
                    with self.assertRaises(ProcessLookupError):
                        os.killpg(spawned[-1].pid, 0)
                    self.assertEqual(signal.getsignal(signal.SIGTERM), prior_term)
                finally:
                    for process in spawned:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        process.wait(timeout=5)
                    spawned.clear()

            # An external cancellation must reap the hanging suite group too.
            driver = root / "drive.py"
            driver.write_text(
                'import importlib.util, types\n'
                'from platformio.test.result import TestSuite\n'
                f'spec=importlib.util.spec_from_file_location("runner", {str(RUNNER_PATH)!r})\n'
                'module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)\n'
                'runner=object.__new__(module.CustomTestRunner)\n'
                'runner.options=types.SimpleNamespace(verbose=1,program_args=None,without_testing=False)\n'
                'runner._testing_output_buffer=""\n'
                'runner.test_suite=TestSuite("native","test_all_native"); runner.test_suite.on_start()\n'
                'runner.project_config=types.SimpleNamespace(get=lambda section,key: '
                f'[{str(program)!r}] if key=="test_testing_command" else "")\n'
                'reader=module.NativeAggregateReader(runner); reader.termination_grace=0.1\n'
                'reader.begin()\n'
            )
            supervisor = subprocess.Popen(
                [sys.executable, "-u", str(driver)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env={**os.environ, "LITCH_NATIVE_TEST_FILTER": "test_hang"},
            )
            captured = bytearray()
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(supervisor.stdout, selectors.EVENT_READ)
                    deadline = time.monotonic() + 5
                    while b"hang_pgid=" not in captured:
                        self.assertLess(time.monotonic(), deadline)
                        for key, _ in selector.select(0.1):
                            data = os.read(key.fd, 4096)
                            self.assertTrue(data, captured.decode())
                            captured.extend(data)
                supervisor.send_signal(signal.SIGTERM)
                tail, _ = supervisor.communicate(timeout=5)
                captured.extend(tail)
                self.assertEqual(supervisor.returncode, 143, captured.decode())
                group = int(re.search(rb"hang_pgid=(\d+)", captured).group(1))
                with self.assertRaises(ProcessLookupError):
                    os.killpg(group, 0)
            finally:
                if supervisor.poll() is None:
                    supervisor.terminate()
                    try:
                        supervisor.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        supervisor.kill()
                        supervisor.wait(timeout=5)
                supervisor.stdout.close()

            with mock.patch.object(module.UnityTestRunner, "stage_testing", return_value="delegated") as delegated:
                standalone = AllNativeCustomRunnerTest().make_runner()
                standalone.test_suite.test_name = "test_standalone"
                self.assertEqual(standalone.stage_testing(), "delegated")
                delegated.assert_called_once_with()
                self.assertEqual(standalone.test_suite.status, TestStatus.PASSED)

    def test_shared_allocation_probe_links_once_standalone_and_aggregate(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        repository = RUNNER_PATH.parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tests = root / "test"
            async_suite = tests / "test_async_engine_ownership"
            probe_suite = tests / "test_litchcraft_persistence"
            async_suite.mkdir(parents=True)
            probe_suite.mkdir()
            for name in ("allocation_probe.cpp", "allocation_probe.hpp"):
                shutil.copyfile(repository / "test/test_litchcraft_persistence" / name,
                                probe_suite / name)
            shutil.copyfile(repository / "test/test_async_engine_ownership/allocation_probe.cpp",
                            async_suite / "allocation_probe.cpp")
            (async_suite / "test_main.cpp").write_text(
                '#include "../test_litchcraft_persistence/allocation_probe.hpp"\n'
                '#include <new>\n'
                'int main() { litchcraft_allocation_probe::calls = 0; '
                'void* p = ::operator new(16, std::nothrow); '
                'const bool okay = p && litchcraft_allocation_probe::calls == 1; '
                '::operator delete(p); return okay ? 0 : 1; }\n'
            )
            (probe_suite / "test_main.cpp").write_text('int main() { return 0; }\n')
            environment = types.SimpleNamespace(
                subst=lambda _: str(root), GetProjectOption=lambda _, default: default)
            with redirect_stdout(io.StringIO()):
                runpy.run_path(str(GENERATOR_PATH),
                               init_globals={"Import": lambda _: None, "env": environment})
            generated = tests / "test_all_native/generated"
            aggregate_main = root / "aggregate_main.cpp"
            aggregate_main.write_text(
                'int test_async_engine_ownership_main();\n'
                'int test_litchcraft_persistence_main();\n'
                'int main() { return test_async_engine_ownership_main() || '
                'test_litchcraft_persistence_main(); }\n'
            )
            cases = {
                "standalone": list(async_suite.glob("*.cpp")),
                "aggregate": [aggregate_main, *generated.glob("*.cpp")],
            }
            for mode, sources in cases.items():
                with self.subTest(mode=mode):
                    program = root / mode
                    compiled = subprocess.run(
                        [compiler, "-std=c++17", *map(str, sources), "-o", str(program)],
                        capture_output=True, text=True, timeout=30,
                    )
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    executed = subprocess.run([str(program)], timeout=5)
                    self.assertEqual(executed.returncode, 0)


if __name__ == "__main__":
    unittest.main()
