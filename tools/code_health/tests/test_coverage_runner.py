from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CODE_HEALTH_DIR = Path(__file__).resolve().parents[1]
FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(CODE_HEALTH_DIR))

import coverage_runner  # noqa: E402


class FakeTools:
    def __init__(self, repo_root: Path, available: set[str]) -> None:
        self.repo_root = repo_root
        self.available = available
        self.calls: list[tuple[tuple[str, ...], Path, dict[str, str]]] = []
        self.paths = {name: f"/tools/{name}" for name in available}
        self.lcov_native = (
            "SF:src/native.cpp\n"
            "FN:4,native\n"
            "FNDA:1,native\n"
            "DA:4,1\n"
            "BRDA:5,0,0,0\n"
            "end_of_record\n"
            "SF:test/test_native/test_main.cpp\nDA:1,1\nend_of_record\n"
        )
        self.lcov_flutter = (FIXTURES_DIR / "coverage_flutter.info").read_text(
            encoding="utf-8"
        )
        self.native_program_returncode = 0
        self.flutter_test_returncode = 0

    def resolve(self, name: str) -> str | None:
        return self.paths.get(name)

    def execute(
        self, argv: tuple[str, ...], cwd: Path, environment: dict[str, str]
    ) -> subprocess.CompletedProcess[str]:
        self.calls.append((tuple(argv), cwd, dict(environment)))
        executable = Path(argv[0]).name
        if len(argv) >= 2 and argv[1] == "--version":
            return self.result(argv, stdout=f"{executable} version 1.2.3\n")
        if executable == "xcrun" and argv[1:3] == ("--find", "llvm-profdata"):
            return self.result(argv, stdout="/xcode/llvm-profdata\n")
        if executable == "xcrun" and argv[1:3] == ("--find", "llvm-cov"):
            return self.result(argv, stdout="/xcode/llvm-cov\n")
        if executable == "pio" and "--without-testing" in argv:
            build_dir = Path(environment["PLATFORMIO_BUILD_DIR"])
            program = build_dir / "native" / "program"
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_text("fake executable", encoding="utf-8")
            return self.result(argv, stdout="native build ok\n")
        if executable in {"program", "program.exe"}:
            if "LLVM_PROFILE_FILE" in environment:
                profile = Path(environment["LLVM_PROFILE_FILE"].replace("%p", "123"))
                profile.parent.mkdir(parents=True, exist_ok=True)
                profile.write_bytes(b"profile")
            else:
                gcda = Path(argv[0]).parent / "src" / "native.gcda"
                gcda.parent.mkdir(parents=True, exist_ok=True)
                gcda.write_bytes(b"gcda")
            output = (
                "[ALL_NATIVE] FAIL: test_example (rc=1)\n"
                if self.native_program_returncode
                else "native tests ok\n"
            )
            return self.result(
                argv, returncode=self.native_program_returncode, stdout=output
            )
        if executable == "llvm-profdata" and "merge" in argv:
            output = Path(argv[argv.index("-o") + 1])
            output.write_bytes(b"merged")
            return self.result(argv)
        if executable == "llvm-cov" and "export" in argv:
            return self.result(argv, stdout=self.lcov_native)
        if executable == "gcovr":
            output = Path(argv[argv.index("--output") + 1])
            output.write_text(self.lcov_native, encoding="utf-8")
            return self.result(argv)
        if executable == "flutter" and "test" in argv:
            coverage_arg = Path(argv[argv.index("--coverage-path") + 1])
            coverage_path = (cwd / coverage_arg).resolve()
            coverage_path.parent.mkdir(parents=True, exist_ok=True)
            coverage_path.write_text(self.lcov_flutter, encoding="utf-8")
            return self.result(
                argv,
                returncode=self.flutter_test_returncode,
                stdout='{"type":"done","success":true}\n',
            )
        return self.result(argv, returncode=2, stderr=f"unexpected command: {argv}\n")

    @staticmethod
    def result(
        argv: tuple[str, ...],
        *,
        returncode: int = 0,
        stdout: str = "",
        stderr: str = "",
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(
            args=list(argv), returncode=returncode, stdout=stdout, stderr=stderr
        )


class CoverageRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repo_root = Path(self.temporary_directory.name) / "repo"
        (self.repo_root / "app").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_native_plan_appends_flags_and_scopes_build_by_revision(self) -> None:
        first = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            worktree_fingerprint="1" * 64,
            existing_build_flags="-D CALLER_FLAG=1",
        )
        second = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="def456",
        )

        self.assertNotEqual(first.build_dir, second.build_dir)
        self.assertIn("abc123", first.build_dir.as_posix())
        self.assertIn("dirty-111111111111", first.build_dir.as_posix())
        self.assertEqual(first.to_dict()["worktree_fingerprint"], "1" * 64)
        self.assertEqual(first.build_command[-1], "--without-testing")
        flags = first.build_environment["PLATFORMIO_BUILD_FLAGS"]
        self.assertIn("-D CALLER_FLAG=1", flags)
        self.assertIn("-fprofile-instr-generate", flags)
        self.assertEqual(
            first.build_environment["CODE_HEALTH_LINK_FLAGS"],
            "-fprofile-instr-generate",
        )
        self.assertEqual(first.build_environment["CODE_HEALTH_CC"], "clang")
        self.assertEqual(first.build_environment["CODE_HEALTH_CXX"], "clang++")
        self.assertIn(
            coverage_runner.PIO_COVERAGE_POST_SCRIPT,
            first.build_environment["PLATFORMIO_EXTRA_SCRIPTS"],
        )
        self.assertNotIn("LITCH_NATIVE", flags)
        self.assertEqual(first.to_dict()["build_flags_mode"], "append")
        self.assertEqual(first.to_dict()["link_flags_mode"], "post-script append")
        self.assertEqual(
            first.build_environment["PLATFORMIO_SETTING_ENABLE_TELEMETRY"], "no"
        )
        self.assertTrue(
            first.build_environment["PLATFORMIO_BUILD_DIR"].startswith(
                str((self.repo_root / "build/code_health/native/pio").resolve())
            )
        )

    def test_native_plan_records_optional_platformio_core_without_leaking_path(self) -> None:
        external_core = Path(self.temporary_directory.name) / "pio-core"
        plan = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            pio_core_dir=external_core,
        )

        self.assertEqual(
            plan.build_environment["PLATFORMIO_CORE_DIR"],
            str(external_core.resolve()),
        )
        self.assertEqual(plan.to_dict()["pio_core_dir"], "pio-core")

    def test_revision_key_distinguishes_dirty_worktrees(self) -> None:
        first = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            worktree_fingerprint="a" * 64,
        )
        second = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            worktree_fingerprint="b" * 64,
        )

        self.assertNotEqual(first.revision_key, second.revision_key)

    def test_native_plan_preserves_caller_extra_scripts(self) -> None:
        plan = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            existing_extra_scripts="pre:caller.py\npost:caller-post.py",
        )

        scripts = plan.build_environment["PLATFORMIO_EXTRA_SCRIPTS"].splitlines()
        self.assertEqual(
            scripts,
            [
                "pre:caller.py",
                "post:caller-post.py",
                coverage_runner.PIO_COVERAGE_POST_SCRIPT,
            ],
        )

    def test_native_suite_manifest_records_inclusions_and_exclusion_reasons(self) -> None:
        for relative in (
            "test/test_fast/test_main.cpp",
            "test/test_slow_long/test_main.cpp",
            "test/test_manual_manual/test_main.cpp",
            "test/test_opt_in/test_main.cpp",
            "test/test_missing_main/README.md",
            "lib/foxie_embedded/tests/test_foxie/test_main.cpp",
            "tools/pio/generate_test_all_native.py",
            "tools/pio/generate_test_all_native_foxie.py",
            "test/test_all_native/test_main.cpp",
            "test/test_all_native_foxie/test_main.cpp",
        ):
            path = self.repo_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture\n", encoding="utf-8")
        (self.repo_root / "test/test_opt_in/ALL_NATIVE_OPT_IN").write_text(
            "", encoding="utf-8"
        )

        plan = coverage_runner.plan_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
        )

        manifest = plan.to_dict()["suite_manifest"]
        self.assertEqual(manifest["status"], "measured")
        primary, foxie = manifest["aggregates"]
        self.assertEqual(
            primary["included_suites"], ["test_all_native_foxie", "test_fast"]
        )
        self.assertEqual(primary["included_count"], 2)
        self.assertEqual(
            {item["suite"]: item["reason"] for item in primary["excluded_suites"]},
            {
                "test_manual_manual": "name_ends_with__manual",
                "test_missing_main": "missing_test_main.cpp",
                "test_opt_in": "ALL_NATIVE_OPT_IN_marker",
                "test_slow_long": "name_ends_with__long",
            },
        )
        self.assertEqual(foxie["included_suites"], ["test_foxie"])
        self.assertRegex(primary["generator"]["sha256"], r"^[0-9a-f]{64}$")

    def test_failure_diagnostic_prefers_compiler_error_over_suite_footer(self) -> None:
        outcome = coverage_runner.CommandOutcome(
            args=("pio", "test"),
            returncode=1,
            stdout="Building...\nclang++: error: linker command failed\n",
            stderr="native:test_all_native [ERRORED]\n",
        )

        diagnostic = coverage_runner._failure_diagnostic(
            "native build", outcome, self.repo_root
        )

        self.assertIn("clang++: error: linker command failed", diagnostic)

    def test_runs_llvm_native_build_program_merge_export_and_normalization(self) -> None:
        tools = FakeTools(
            self.repo_root,
            {"pio", "clang", "clang++", "llvm-profdata", "llvm-cov"},
        )

        result = coverage_runner.run_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            executor=tools.execute,
            resolver=tools.resolve,
            inherited_environment={"PLATFORMIO_BUILD_FLAGS": "-D EXISTING=1"},
        )

        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(result.backend, "llvm")
        self.assertEqual(result.report.line_hits_by_path(), {"src/native.cpp": {4: 1}})
        phases = [command.phase for command in result.commands]
        self.assertIn("native-build", phases)
        self.assertIn("native-test", phases)
        self.assertIn("native-profile-merge", phases)
        self.assertIn("native-coverage-export", phases)
        build_command = next(
            command for command in result.commands if command.phase == "native-build"
        )
        self.assertIn("-D EXISTING=1", dict(build_command.environment)["PLATFORMIO_BUILD_FLAGS"])
        self.assertIn(
            "-fprofile-instr-generate",
            dict(build_command.environment)["PLATFORMIO_BUILD_FLAGS"],
        )
        serialized = json.dumps(result.to_dict(), sort_keys=True)
        self.assertNotIn(str(self.repo_root), serialized)
        coverage_json = self.repo_root / result.artifacts["coverage_json"]
        self.assertTrue(coverage_json.is_file())
        self.assertEqual(json.loads(coverage_json.read_text())["schema_version"], 1)
        normalized_lcov = (self.repo_root / result.artifacts["lcov"]).read_text(
            encoding="utf-8"
        )
        self.assertIn("SF:src/native.cpp", normalized_lcov)
        self.assertIn("FNDA:1,native", normalized_lcov)
        self.assertIn("BRDA:5,0,0,0", normalized_lcov)
        self.assertNotIn("SF:test/", normalized_lcov)
        self.assertNotIn(str(self.repo_root), normalized_lcov)
        run_json = self.repo_root / result.artifacts["run_json"]
        self.assertEqual(
            json.loads(run_json.read_text(encoding="utf-8")),
            result.to_dict(),
        )

    def test_forced_backend_does_not_fall_back(self) -> None:
        tools = FakeTools(self.repo_root, {"pio", "gcc", "g++", "gcov", "gcovr"})

        result = coverage_runner.run_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="forced",
            backend="llvm",
            executor=tools.execute,
            resolver=tools.resolve,
            inherited_environment={},
        )

        self.assertEqual(result.status, "unavailable")
        self.assertIn("forced LLVM", result.diagnostics[0])
        self.assertFalse(any(tool.name == "gcovr" for tool in result.tools))

    def test_failed_native_test_still_exports_profile_as_failed_evidence(self) -> None:
        tools = FakeTools(
            self.repo_root,
            {"pio", "clang", "clang++", "llvm-profdata", "llvm-cov"},
        )
        tools.native_program_returncode = 1

        result = coverage_runner.run_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            executor=tools.execute,
            resolver=tools.resolve,
            inherited_environment={},
        )

        self.assertEqual(result.status, "failed")
        self.assertIsNotNone(result.report)
        self.assertIn("coverage_json", result.artifacts)
        self.assertIn("test_example", result.diagnostics[0])

    def test_uses_xcrun_fallback_for_llvm_tools(self) -> None:
        tools = FakeTools(self.repo_root, {"pio", "clang", "clang++", "xcrun"})

        result = coverage_runner.run_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            executor=tools.execute,
            resolver=tools.resolve,
            inherited_environment={},
        )

        self.assertTrue(result.ok, result.diagnostics)
        self.assertTrue(
            any(command.argv[1:3] == ("--find", "llvm-cov") for command in result.commands)
        )
        self.assertEqual(
            next(tool for tool in result.tools if tool.name == "llvm-cov").version,
            "llvm-cov version 1.2.3",
        )

    def test_uses_gcovr_only_when_complete_gcc_fallback_is_available(self) -> None:
        tools = FakeTools(self.repo_root, {"pio", "gcc", "g++", "gcov", "gcovr"})

        result = coverage_runner.run_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="fallback",
            executor=tools.execute,
            resolver=tools.resolve,
            inherited_environment={},
        )

        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(result.backend, "gcovr")
        build_command = next(
            command for command in result.commands if command.phase == "native-build"
        )
        environment = dict(build_command.environment)
        self.assertEqual(environment["CC"], "gcc")
        self.assertEqual(environment["CXX"], "g++")
        self.assertIn("--coverage", environment["PLATFORMIO_BUILD_FLAGS"])

    def test_reports_native_unavailable_without_downloading_tools(self) -> None:
        tools = FakeTools(self.repo_root, {"pio", "clang", "clang++"})

        result = coverage_runner.run_native_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/native",
            revision="abc123",
            executor=tools.execute,
            resolver=tools.resolve,
            inherited_environment={},
        )

        self.assertEqual(result.status, "unavailable")
        self.assertIsNone(result.backend)
        self.assertIn("complete clang/LLVM", result.diagnostics[0])
        self.assertFalse(any("install" in " ".join(call[0]) for call in tools.calls))
        missing = {tool.name for tool in result.tools if not tool.available}
        self.assertIn("llvm-cov", missing)
        self.assertIn("gcovr", missing)

    def test_flutter_plan_uses_branch_coverage_path_and_json_reporter(self) -> None:
        plan = coverage_runner.plan_flutter_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/flutter",
        )

        self.assertEqual(
            plan.test_command[:5],
            ("flutter", "test", "--no-pub", "--coverage", "--branch-coverage"),
        )
        self.assertIn("--coverage-path", plan.test_command)
        self.assertIn("--reporter=json", plan.test_command)
        coverage_argument = plan.test_command[
            plan.test_command.index("--coverage-path") + 1
        ]
        self.assertFalse(Path(coverage_argument).is_absolute())

    def test_runs_flutter_and_writes_json_events_and_normalized_report(self) -> None:
        tools = FakeTools(self.repo_root, {"flutter"})

        result = coverage_runner.run_flutter_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/flutter",
            executor=tools.execute,
            resolver=tools.resolve,
        )

        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(result.backend, "flutter")
        self.assertEqual(
            sorted(result.report.line_hits_by_path()),
            ["app/lib/a_widget.dart", "app/lib/src/z_service.dart"],
        )
        event_path = self.repo_root / result.artifacts["raw_test_events"]
        self.assertEqual(
            json.loads(event_path.read_text(encoding="utf-8"))["type"], "done"
        )
        flutter_command = next(
            command for command in result.commands if command.phase == "flutter-test"
        )
        self.assertEqual(flutter_command.cwd, "app")
        self.assertIn("--reporter=json", flutter_command.argv)
        self.assertIn("--no-pub", flutter_command.argv)
        self.assertEqual(dict(flutter_command.environment)["CI"], "true")
        self.assertEqual(
            dict(flutter_command.environment)["FLUTTER_SUPPRESS_ANALYTICS"],
            "true",
        )
        self.assertEqual(flutter_command.returncode, 0)

    def test_failed_flutter_test_retains_parseable_coverage(self) -> None:
        tools = FakeTools(self.repo_root, {"flutter"})
        tools.flutter_test_returncode = 1

        result = coverage_runner.run_flutter_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/flutter-failed",
            executor=tools.execute,
            resolver=tools.resolve,
        )

        self.assertEqual(result.status, "failed")
        self.assertIsNotNone(result.report)
        self.assertIn("coverage_json", result.artifacts)
        self.assertEqual(
            next(command for command in result.commands if command.phase == "flutter-test").returncode,
            1,
        )

    def test_timeout_with_zero_returncode_is_still_failed(self) -> None:
        tools = FakeTools(self.repo_root, {"flutter"})

        class TimedOutResult:
            args = ["/tools/flutter", "test"]
            returncode = 0
            stdout = '{"type":"done","success":false}\n'
            stderr = ""
            stdout_truncated = False
            stderr_truncated = False
            timed_out = True

        def execute(argv, cwd, environment):
            if Path(argv[0]).name == "flutter" and "test" in argv:
                coverage_argument = Path(argv[argv.index("--coverage-path") + 1])
                coverage_path = (cwd / coverage_argument).resolve()
                coverage_path.parent.mkdir(parents=True, exist_ok=True)
                coverage_path.write_text(tools.lcov_flutter, encoding="utf-8")
                return TimedOutResult()
            return tools.execute(argv, cwd, environment)

        result = coverage_runner.run_flutter_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/flutter-timeout",
            executor=execute,
            resolver=tools.resolve,
        )

        self.assertEqual(result.status, "failed")
        self.assertIsNotNone(result.report)
        command = next(item for item in result.commands if item.phase == "flutter-test")
        self.assertTrue(command.timed_out)
        self.assertEqual(command.returncode, 0)

    def test_reports_flutter_tool_absence(self) -> None:
        tools = FakeTools(self.repo_root, set())

        result = coverage_runner.run_flutter_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/flutter",
            executor=tools.execute,
            resolver=tools.resolve,
        )

        self.assertEqual(result.status, "unavailable")
        self.assertIn("not found", result.diagnostics[0])
        self.assertEqual(result.commands, ())
        self.assertTrue((self.repo_root / result.artifacts["run_json"]).is_file())

    def test_default_executor_caps_and_marks_both_output_streams(self) -> None:
        original_limit = coverage_runner.MAX_CAPTURE_BYTES
        coverage_runner.MAX_CAPTURE_BYTES = 32
        try:
            outcome = coverage_runner._default_executor(
                (
                    sys.executable,
                    "-c",
                    "import sys; print('x' * 100); print('y' * 100, file=sys.stderr)",
                ),
                self.repo_root,
                {},
            )
        finally:
            coverage_runner.MAX_CAPTURE_BYTES = original_limit

        self.assertEqual(outcome.returncode, 0)
        self.assertLessEqual(len(outcome.stdout.encode()), 32)
        self.assertLessEqual(len(outcome.stderr.encode()), 32)
        self.assertTrue(outcome.stdout_truncated)
        self.assertTrue(outcome.stderr_truncated)

    def test_default_executor_times_out_and_records_it(self) -> None:
        original_timeout = coverage_runner.MAX_COMMAND_SECONDS
        coverage_runner.MAX_COMMAND_SECONDS = 0.05
        try:
            outcome = coverage_runner._default_executor(
                (sys.executable, "-c", "import time; time.sleep(5)"),
                self.repo_root,
                {},
            )
        finally:
            coverage_runner.MAX_COMMAND_SECONDS = original_timeout

        self.assertTrue(outcome.timed_out)
        self.assertNotEqual(outcome.returncode, 0)

    def test_default_executor_rejects_inherited_pipe_that_outlives_parent(self) -> None:
        parent = (
            "import subprocess, sys; "
            "subprocess.Popen([sys.executable, '-c', "
            "'import time; time.sleep(0.5)'], stdout=sys.stdout, stderr=sys.stderr); "
            "print('parent complete', flush=True)"
        )
        original_join = coverage_runner.PIPE_DRAIN_JOIN_SECONDS
        coverage_runner.PIPE_DRAIN_JOIN_SECONDS = 0.02
        try:
            outcome = coverage_runner._default_executor(
                (sys.executable, "-c", parent),
                self.repo_root,
                {},
            )
        finally:
            coverage_runner.PIPE_DRAIN_JOIN_SECONDS = original_join

        self.assertTrue(outcome.stdout_truncated or outcome.stderr_truncated)
        self.assertNotEqual(outcome.returncode, 0)

    def test_recorded_command_exposes_capture_truncation(self) -> None:
        class TruncatedResult:
            args = ["/tools/flutter", "--version"]
            returncode = 0
            stdout = "flutter version\n"
            stderr = ""
            stdout_truncated = True
            stderr_truncated = False

        result = coverage_runner.run_flutter_coverage(
            repo_root=self.repo_root,
            output_dir="build/code_health/flutter",
            executor=lambda argv, cwd, environment: TruncatedResult(),
            resolver=lambda name: "/tools/flutter" if name == "flutter" else None,
        )

        self.assertEqual(result.status, "unavailable")
        self.assertTrue(result.commands[0].stdout_truncated)

    def test_output_must_remain_inside_repository(self) -> None:
        with self.assertRaisesRegex(ValueError, "inside the repository"):
            coverage_runner.plan_native_coverage(
                repo_root=self.repo_root,
                output_dir=Path(self.temporary_directory.name) / "outside",
                revision="abc123",
            )


if __name__ == "__main__":
    unittest.main()
