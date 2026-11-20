from __future__ import annotations

import importlib.util
import copy
import json
import os
import re
import tempfile
import contextlib
import io
import subprocess
import sys
import time
from unittest import mock
from pathlib import Path
import unittest

from tools.dev.runner import DevError


ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = ROOT / "tools" / "qa" / "interactive_sync.py"
SPEC = importlib.util.spec_from_file_location("interactive_sync", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
interactive_sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(interactive_sync)


class InteractiveSyncManifestTest(unittest.TestCase):
    def test_physical_work_is_excluded_from_all_software_profiles(self) -> None:
        self.assertNotIn("physical32", interactive_sync.SOFTWARE_PROFILES)

    def test_fast_profile_covers_each_interactive_surface(self) -> None:
        flutter = set(interactive_sync.FAST_FLUTTER_TESTS)
        native = set(interactive_sync.FAST_NATIVE_TESTS)
        self.assertIn("test/litchcraft_stroke_batch_benchmark_test.dart", flutter)
        self.assertIn("test/settings_screen_test.dart", flutter)
        self.assertIn("test/app_console_screen_test.dart", flutter)
        self.assertIn("test_firmware_render_latency", native)
        self.assertIn("test_virtual_litch_session", native)

    def test_bridge_profile_cannot_silently_skip_production_bridge(self) -> None:
        tests = set(interactive_sync.BRIDGE_FLUTTER_TESTS)
        self.assertIn("test/virtual_litch_interactive_sync_journey_test.dart", tests)
        self.assertIn("test/virtual_litch_mode_latency_test.dart", tests)
        self.assertIn("test/virtual_litch_coaching_journey_test.dart", tests)
        self.assertIn("test/virtual_litch_cold_attach_journey_test.dart", tests)
        self.assertIn("test/connection_session_received_head_test.dart", tests)
        self.assertIn("test/litchcraft_native_interop_test.dart", tests)
        self.assertIn("test/game_history_native_rebase_test.dart", tests)
        command = interactive_sync._native_command(
            ("test_virtual_litch_app_bridge_manual",),
            build_only=True,
        )
        self.assertIn("test_virtual_litch_app_bridge_manual", command)
        self.assertIn("--without-testing", command)


class FlutterExecutionEvidenceTest(unittest.TestCase):
    """Controlled child output at the actual software runner boundary."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.bridge = self.root / ".pio/interactive-sync/build/native/program"
        self.bridge.parent.mkdir(parents=True)
        self.bridge.write_text("controlled backend; never executed")
        self.bridge.chmod(0o755)
        self.env = {"LITCH_VIRTUAL_BRIDGE": str(self.bridge),
                    "LITCHCRAFT_NATIVE_RUNNER": str(self.bridge),
                    "LITCH_HISTORY_REBASE_WIRE": str(self.bridge)}
        self.tests = interactive_sync.BRIDGE_FLUTTER_TESTS
        for name in self.tests:
            path = self.root / "app" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("controlled selected suite")

    def events(self) -> list[dict]:
        records = []
        for number, name in enumerate(self.tests):
            records.append({"type": "suite", "suite": {"id": number,
                            "path": str(self.root / "app" / name)}})
            names = getattr(interactive_sync, "REQUIRED_FLUTTER_CASES", {}).get(name, ("required body",))
            for offset, body in enumerate(names):
                identity = number * 10 + offset + 100
                records.extend([
                    {"type": "testStart", "test": {"id": identity,
                     "suiteID": number, "name": body}},
                    {"type": "testDone", "testID": identity,
                     "result": "success", "skipped": False, "hidden": False},
                ])
        return [*records, {"type": "done", "success": True}]

    def run_child(self, events: list[dict], *, exit_code: int = 0,
                  output: io.StringIO | None = None) -> None:
        script = ("import sys; sys.stdout.write(" +
                  repr("\n".join(json.dumps(event) for event in events) + "\n") +
                  f"); sys.exit({exit_code})")
        with mock.patch.object(interactive_sync, "_flutter_command",
                               return_value=[sys.executable, "-c", script]):
            with contextlib.redirect_stdout(output if output is not None else io.StringIO()):
                interactive_sync.run_bridge(self.root, self.env, False, True)

    def test_native_cleanup_journey_is_selected_and_named_body_required(self) -> None:
        test = "test/virtual_litch_native_cleanup_journey_test.dart"
        body = ("Object timeout renews the peripheral lifetime after local GATT cleanup "
                "(variant: TargetPlatform.android)")
        self.assertIn(test, interactive_sync.BRIDGE_FLUTTER_TESTS)
        self.assertEqual(interactive_sync.FLUTTER_BACKENDS[test], ("LITCH_VIRTUAL_BRIDGE",))
        self.assertEqual(interactive_sync.REQUIRED_FLUTTER_CASES[test], (body,))
        with mock.patch.object(interactive_sync, "_run_flutter") as run_flutter:
            interactive_sync.run_bridge(self.root, self.env, False, True)
        self.assertIn(test, run_flutter.call_args.args[0])
        self.run_child(self.events())
        events = self.events()
        for event in events:
            if event["type"] == "testStart" and event["test"]["name"] == body:
                event["test"]["name"] = "another successful body"
        with self.assertRaisesRegex(ValueError, "required named body not executed"):
            self.run_child(events)

    def test_real_child_success_covers_every_selected_suite(self) -> None:
        self.run_child(self.events())

    def test_cold_attach_boundaries_require_their_named_bodies(self) -> None:
        test = interactive_sync.COLD_ATTACH_TEST
        bodies = [
            "closed native Store capacity keeps rapid Craft edits and Exit recoverable "
            "(variant: TargetPlatform.android)",
            *(f"passive native mode {mode} precedes local draft and orientation "
              "(variant: TargetPlatform.android)" for mode in ("Craft", "Play")),
            *("app background link loss then raw physical Exit with 21 completed strokes: "
              "Craft Play navigation keeps board controls available "
              f"(variant: TargetPlatform.{platform})" for platform in ("iOS", "android")),
        ]
        self.assertEqual(interactive_sync.FLUTTER_BACKENDS[test], ("LITCH_VIRTUAL_BRIDGE",))
        self.run_child(self.events())
        for body in bodies:
            with self.subTest(body=body):
                self.assertIn(body, interactive_sync.REQUIRED_FLUTTER_CASES[test])
                events = self.events()
                for event in events:
                    if event["type"] == "testStart" and event["test"]["name"] == body:
                        event["test"]["name"] = "another successful body"
                with self.assertRaisesRegex(ValueError, "required named body not executed"):
                    self.run_child(events)

    def test_existing_machine_stream_reports_slowest_bodies_without_loaders(self) -> None:
        events = self.events()
        identities = []
        for event in events:
            if event["type"] == "testStart":
                identity = event["test"]["id"]
                identities.append(identity)
                event["time"] = identity * 1000
            elif event["type"] == "testDone":
                identity = event["testID"]
                event["time"] = identity * 1000 + identity
        events[-1:-1] = [
            {"type": "testStart", "time": 0,
             "test": {"id": 999, "suiteID": 0, "name": "hidden loader"}},
            {"type": "testDone", "time": 9999999, "testID": 999,
             "result": "success", "skipped": False, "hidden": True},
        ]
        script = "import sys; sys.stdout.write(" + repr(
            "\n".join(json.dumps(event) for event in events) + "\n"
        ) + ")"
        output = io.StringIO()
        with mock.patch.object(interactive_sync, "_flutter_command",
                               return_value=[sys.executable, "-c", script]), \
                contextlib.redirect_stdout(output):
            interactive_sync.run_bridge(self.root, self.env, False, True)
        result = json.loads(output.getvalue().splitlines()[-1])
        timings = result["body_timings"]
        self.assertEqual(timings["observed_bodies"], len(identities))
        self.assertEqual(timings["unavailable_bodies"], 0)
        self.assertEqual(sum(timings["suite_body_ms"].values()), sum(identities))
        self.assertEqual([body["elapsed_ms"] for body in timings["slowest_bodies"]],
                         sorted(identities, reverse=True)[:10])
        self.assertNotIn("hidden loader", str(timings))

        fragmented = copy.deepcopy(events)
        fragmented.insert(2, {"type": "print", "testID": identities[0], "message": "snow \u2603"})
        data = "\n".join(json.dumps(event, ensure_ascii=False) for event in fragmented).encode()
        split = data.index("\u2603".encode()) + 1
        script = ("import os,time; data=" + repr(data) +
                  f"; os.write(1,data[:{split}]); time.sleep(0.01); os.write(1,data[{split}:])")
        output = io.StringIO()
        with mock.patch.object(interactive_sync, "_flutter_command",
                               return_value=[sys.executable, "-c", script]), \
                mock.patch.object(interactive_sync, "FLUTTER_PROGRESS_SECONDS", 0), \
                contextlib.redirect_stdout(output):
            interactive_sync.run_bridge(self.root, self.env, False, True)
        reports = [json.loads(line) for line in output.getvalue().splitlines() if line.startswith("{")]
        self.assertTrue(reports[-1]["passed"])
        self.assertTrue(any(report.get("event") == "flutter.progress" for report in reports))

        script = ("import sys; sys.stdout.write('{'+'x'*" +
                  str(interactive_sync.MAX_FLUTTER_RECORD_CHARS) + "+'\\n'); sys.stdout.write(" +
                  repr(data.decode()) + ")")
        with mock.patch.object(interactive_sync, "_flutter_command",
                               return_value=[sys.executable, "-c", script]), \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "oversized"):
                interactive_sync.run_bridge(self.root, self.env, False, True)

        marker, ready = self.root / "descendant-survived", self.root / "descendant-ready"
        descendant = "import time; from pathlib import Path; time.sleep(1); Path(" + repr(str(marker)) + ").touch()"
        prefix = "\n".join(json.dumps(event) for event in events[:2]) + "\n"
        script = ("import subprocess,sys; from pathlib import Path; "
                  f"child=subprocess.Popen([sys.executable,'-c',{descendant!r}]); "
                  f"Path({str(ready)!r}).write_text(str(child.pid)); "
                  f"sys.stdout.write({prefix!r}); sys.stdout.flush(); "
                  "sys.stderr.write('x'*20000+'controlled-stderr\\n'); sys.stderr.flush()")
        output = io.StringIO()
        with mock.patch.object(interactive_sync, "_flutter_command",
                               return_value=[sys.executable, "-c", script]), \
                mock.patch.dict(interactive_sync.COMMAND_TIMEOUT_SECONDS, {"flutter-test": 0.5}), \
                contextlib.redirect_stdout(output):
            with self.assertRaisesRegex(DevError, "exceeded 0.5s"):
                interactive_sync.run_bridge(self.root, self.env, False, True)
        self.assertTrue(ready.is_file(), "controlled descendant actually inherited the output pipe")
        failure = json.loads(output.getvalue().splitlines()[-1])
        self.assertFalse(failure["passed"])
        self.assertIn(events[1]["test"]["name"], str(failure["incomplete_tests"]))
        self.assertIn("controlled-stderr", failure["diagnostic_tail"])
        self.assertLessEqual(len(failure["diagnostic_tail"]), interactive_sync.FLUTTER_DIAGNOSTIC_CHARS)
        time.sleep(1.1)
        self.assertFalse(marker.exists(), "timeout stopped the descendant after its leader exited")

    def test_exit_zero_with_required_body_skipped_cannot_pass(self) -> None:
        events = self.events()
        events[2]["skipped"] = True
        with self.assertRaisesRegex(ValueError, "skip"):
            self.run_child(events)

    def test_exit_zero_without_executed_bodies_cannot_pass(self) -> None:
        events = [event for event in self.events()
                  if event["type"] not in ("testStart", "testDone")]
        with self.assertRaisesRegex(ValueError, "executed|bodies"):
            self.run_child(events)

    def test_exit_zero_with_missing_selected_suite_cannot_pass(self) -> None:
        events = self.events()[3:]
        with self.assertRaisesRegex(ValueError, "suite|executed"):
            self.run_child(events)

    def test_exit_zero_without_terminal_event_cannot_pass(self) -> None:
        with self.assertRaisesRegex(ValueError, "finish|terminal"):
            self.run_child(self.events()[:-1])

    def test_exit_zero_with_unfinished_body_cannot_pass(self) -> None:
        events = self.events()
        del events[2]
        with self.assertRaisesRegex(ValueError, "finish|incomplete"):
            self.run_child(events)

    def test_exit_zero_false_success_event_cannot_pass(self) -> None:
        events = self.events()
        events[-1]["success"] = False
        with self.assertRaisesRegex(ValueError, "success|failed"):
            self.run_child(events)

    def test_error_event_cannot_be_hidden_by_successful_done(self) -> None:
        events = self.events()
        events.insert(-1, {"type": "error", "testID": 100,
                           "error": "controlled failure"})
        with self.assertRaisesRegex(ValueError, "error|failed"):
            self.run_child(events)

    def test_invalid_supplied_backend_fails_before_child(self) -> None:
        self.env["LITCH_VIRTUAL_BRIDGE"] = str(self.root / "missing")
        with self.assertRaisesRegex(ValueError, "backend|executable") as failure:
            self.run_child(self.events())
        self.assertIn("tools/qa/README.md#interactive-sync-backend-preparation", str(failure.exception))
        self.assertIn("candidate", str(failure.exception))


    def test_missing_native_and_history_inputs_cannot_hide_behind_bridge(self) -> None:
        for key in ("LITCHCRAFT_NATIVE_RUNNER", "LITCH_HISTORY_REBASE_WIRE"):
            with self.subTest(key=key):
                value = self.env.pop(key)
                try:
                    with self.assertRaisesRegex(ValueError, key) as failure:
                        self.run_child(self.events())
                    self.assertIn("tools/qa/README.md#interactive-sync-backend-preparation",
                                  str(failure.exception))
                    self.assertIn("successful", str(failure.exception))
                finally:
                    self.env[key] = value

    def test_unrelated_skipped_body_is_rejected(self) -> None:
        events = self.events()
        events[-1:-1] = [
            {"type": "suite", "suite": {"id": 99,
             "path": str(self.root / "app/test/other_test.dart")}},
            {"type": "testStart", "test": {"id": 999, "suiteID": 99, "name": "hidden gate"}},
            {"type": "testDone", "testID": 999, "result": "success",
             "skipped": True, "hidden": False},
        ]
        with self.assertRaisesRegex(ValueError, "skipped"):
            self.run_child(events)

    def test_only_loaders_cannot_count_as_executed_bodies(self) -> None:
        events = self.events()
        for event in events:
            if event["type"] == "testDone":
                event["hidden"] = True
        with self.assertRaisesRegex(ValueError, "executed bodies"):
            self.run_child(events)

    def test_mixed_workload_must_execute_both_named_platform_cases(self) -> None:
        for platform in ("iOS", "android"):
            with self.subTest(platform=platform):
                events = self.events()
                target = next(event["test"]["id"] for event in events
                              if event["type"] == "testStart" and
                              event["test"]["name"].endswith(f"TargetPlatform.{platform})"))
                events = [event for event in events if not (
                    (event["type"] == "testStart" and event["test"]["id"] == target) or
                    (event["type"] == "testDone" and event["testID"] == target))]
                with self.assertRaisesRegex(ValueError, "required named body"):
                    self.run_child(events)

    def test_supplied_bridge_is_reused_without_build_even_without_skip_option(self) -> None:
        with mock.patch.object(interactive_sync, "_run") as build:
            with mock.patch.object(interactive_sync, "_run_flutter") as flutter:
                interactive_sync.run_bridge(self.root, self.env, False, False)
        build.assert_not_called()
        self.assertEqual(flutter.call_args.args[2]["LITCH_VIRTUAL_BRIDGE"], str(self.bridge))

    def test_supplied_native_fast_runner_is_not_rebuilt(self) -> None:
        with mock.patch.object(interactive_sync, "_run") as native:
            with mock.patch.object(interactive_sync, "_run_flutter"):
                interactive_sync.run_fast(self.root, self.env, False)
        self.assertEqual(native.call_count, len(interactive_sync.FAST_NATIVE_TESTS))
        for call, test in zip(native.call_args_list, interactive_sync.FAST_NATIVE_TESTS):
            self.assertEqual(call.args[0], [str(self.bridge)])
            self.assertEqual(call.kwargs["env"]["LITCH_NATIVE_TEST_FILTER"], test)

    def test_nonzero_child_exit_cannot_pass(self) -> None:
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_child(self.events(), exit_code=2)

    def test_failed_child_reports_named_body_error_and_stack_before_raising(self) -> None:
        events = self.events()
        body = events[1]["test"]
        body["name"] = "controlled failing body"
        events[2]["result"] = "failure"
        events.insert(2, {"type": "error", "testID": body["id"],
                          "error": "controlled assertion reason " + "detail" * 1000,
                          "stackTrace": "test/controlled_test.dart 37:9 " + "frame" * 1000})
        events[-1]["success"] = False
        output = io.StringIO()
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            self.run_child(events, exit_code=1, output=output)
        self.assertEqual(failure.exception.returncode, 1)
        result = json.loads(output.getvalue().splitlines()[-1])
        self.assertFalse(result["passed"])
        self.assertEqual(result["exit_code"], 1)
        diagnostic = "\n".join(result["errors"])
        self.assertIn(self.tests[0], diagnostic)
        self.assertIn("controlled failing body", diagnostic)
        self.assertIn("controlled assertion reason", diagnostic)
        self.assertIn("test/controlled_test.dart 37:9", diagnostic)
        self.assertLess(len(json.dumps(result)), 6000)

        late = copy.deepcopy(body)
        late.update(id=9999, name="late failing body")
        events[2:2] = [
            *({"type": "error", "testID": body["id"], "error": "earlier assertion"}
              for _ in range(8)),
            {"type": "testStart", "test": late},
            {"type": "error", "testID": late["id"], "error": "late assertion"},
            {"type": "testDone", "testID": late["id"], "result": "failure",
             "hidden": False, "skipped": False},
        ]
        output = io.StringIO()
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_child(events, exit_code=1, output=output)
        announced = [json.loads(line) for line in output.getvalue().splitlines()
                     if line.startswith("{")]
        late_failure = next(record for record in announced
                            if record.get("event") == "flutter.failure" and
                            "late failing body" in record["test"])
        self.assertEqual(late_failure["error"], "late assertion")

    def test_exit_zero_validator_failure_is_reported_before_raising(self) -> None:
        output = io.StringIO()
        with self.assertRaisesRegex(ValueError, "terminal"):
            self.run_child(self.events()[:-1], output=output)
        result = json.loads(output.getvalue().splitlines()[-1])
        self.assertFalse(result["passed"])
        self.assertEqual(result["exit_code"], 0)
        self.assertIn("terminal", result["validation_error"])

    def test_widget_print_assertion_survives_generic_error_event(self) -> None:
        events = self.events()
        body = events[1]["test"]
        events[2]["result"] = "failure"
        events[2:2] = [
            {"type": "print", "testID": body["id"],
             "message": "The following TestFailure: Expected false, Actual true at timeout.dart:42"},
            {"type": "error", "testID": body["id"],
             "error": "Test failed. See exception logs above."},
        ]
        events[-1]["success"] = False
        output = io.StringIO()
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_child(events, exit_code=1, output=output)
        result = json.loads(output.getvalue().splitlines()[-1])
        self.assertIn("Expected false, Actual true", str(result["errors"]))
        self.assertIn("timeout.dart:42", str(result["errors"]))

    def failed_events(self, count: int, *, suites: int = 3) -> list[dict]:
        events = []
        for number in range(suites):
            path = (self.tests[number] if number < len(self.tests) else
                    f"test/failure_suite_{number}.dart")
            events.append({"type": "suite", "suite": {"id": number,
                           "path": str(self.root / "app" / path)}})
        for number in range(count):
            identity = number + 1000
            events.extend([
                {"type": "testStart", "test": {"id": identity,
                 "suiteID": number % suites, "name": f"failure family {number:02}"}},
                {"type": "print", "testID": identity, "message": "assertion witness"},
                *({"type": "error", "testID": identity, "error": "repeated assertion"}
                  for _ in range(3)),
                {"type": "testDone", "testID": identity, "result": "failure",
                 "hidden": False, "skipped": False},
            ])
        return [*events, {"type": "done", "success": False}]

    def failure_report(self, events: list[dict]) -> dict:
        output = io.StringIO()
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_child(events, exit_code=1, output=output)
        return json.loads(output.getvalue().splitlines()[-1])

    def test_failure_summary_retains_every_family_after_witness_limit(self) -> None:
        events = self.failed_events(12)
        # The same body can emit errors and a failed completion more than once.
        events.insert(-1, {"type": "error", "testID": 1000, "error": "late error"})
        result = self.failure_report(events)
        self.assertEqual(len(result["errors"]), 8)
        summary = result["failed_tests"]
        self.assertEqual(summary["total"], 12)
        self.assertEqual(summary["omitted"], 0)
        self.assertEqual(len(summary["labels"]), 12)
        for number in range(12):
            self.assertEqual(sum(f"failure family {number:02}" in label
                                 for label in summary["labels"]), 1)
        self.assertEqual({row["suite"]: row["failed"] for row in summary["suite_counts"]},
                         {name: 4 for name in self.tests[:3]})
        self.assertEqual(summary["suites_omitted"], 0)

    def test_failure_summary_bounds_names_and_reports_omissions(self) -> None:
        result = self.failure_report(self.failed_events(70))
        summary = result["failed_tests"]
        self.assertEqual(summary["total"], 70)
        self.assertEqual(len(summary["labels"]), 64)
        self.assertEqual(summary["omitted"], 6)
        self.assertEqual(sum(row["failed"] for row in summary["suite_counts"]), 70)
        self.assertEqual(summary["suites_omitted"], 0)
        self.assertLess(len(json.dumps(result)), 35000)

    def test_failure_summary_deduplicates_labels_and_includes_both_event_kinds(self) -> None:
        events = self.failed_events(3, suites=1)
        events = [event for event in events if not (
            event["type"] == "error" and event.get("testID") == 1001)]
        # Error-only bodies must remain visible even when testDone says success.
        next(event for event in events if event["type"] == "testDone" and
             event["testID"] == 1000)["result"] = "success"
        next(event["test"] for event in events if event["type"] == "testStart" and
             event["test"]["id"] == 1002)["name"] = "failure family 00"
        events.insert(-1, {"type": "error", "error": "no test attribution"})
        summary = self.failure_report(events)["failed_tests"]
        self.assertEqual(summary["total"], 3)
        self.assertEqual(len(summary["labels"]), 3)
        self.assertTrue(any("<unknown body>" in label for label in summary["labels"]))
        self.assertEqual(sum(row["failed"] for row in summary["suite_counts"]), 3)

    def test_failure_summary_caps_suites_and_preserves_long_name_counts(self) -> None:
        events = self.failed_events(70, suites=70)
        for event in events:
            if event["type"] == "suite":
                event["suite"]["path"] = "x" * 200 + str(event["suite"]["id"])
            elif event["type"] == "testStart":
                event["test"]["name"] = "y" * 600 + str(event["test"]["id"])
        summary = self.failure_report(events)["failed_tests"]
        self.assertEqual(summary["total"], 70)
        self.assertEqual(summary["omitted"], 6)
        self.assertEqual(len(summary["suite_counts"]), 64)
        self.assertEqual(summary["suites_omitted"], 6)
        self.assertEqual(sum(row["failed"] for row in summary["suite_counts"]), 64)
        self.assertEqual(summary["truncated_labels"], 64)
        self.assertEqual(summary["truncated_suites"], 64)
        self.assertLessEqual(max(map(len, summary["labels"])), 696)

    def test_success_json_does_not_include_failure_summary(self) -> None:
        output = io.StringIO()
        self.run_child(self.events(), output=output)
        result = json.loads(output.getvalue().splitlines()[-1])
        self.assertTrue(result["passed"])
        self.assertNotIn("failed_tests", result)


class FocusedFlutterExecutionEvidenceTest(unittest.TestCase):
    """A selected run must execute its requested bodies and only their backends."""

    def setUp(self):
        FlutterExecutionEvidenceTest.setUp(self)
        self.tests = (interactive_sync.BRIDGE_FLUTTER_TESTS[0],)
        self.env = {"LITCH_VIRTUAL_BRIDGE": str(self.bridge)}

    def run_selected(self, names, *, requested=("first operation", "second operation"),
                     skipped=False):
        events = [{"type": "suite", "suite": {"id": 1,
                   "path": str(self.root / "app" / self.tests[0])}}]
        for identity, name in enumerate(names):
            events.extend([
                {"type": "testStart", "time": identity * 20,
                 "test": {"id": identity, "suiteID": 1, "name": name}},
                {"type": "testDone", "time": identity * 20 + 10, "testID": identity,
                 "result": "success", "hidden": False, "skipped": skipped},
            ])
        events.append({"type": "done", "success": True})
        script = "import sys; sys.stdout.write(" + repr(
            "\n".join(json.dumps(event) for event in events) + "\n") + ")"
        output = io.StringIO()
        with mock.patch.object(interactive_sync, "_flutter_command",
                               return_value=[sys.executable, "-c", script]), \
                contextlib.redirect_stdout(output):
            interactive_sync.run_focused(self.root, self.env, False, self.tests, requested)
        return json.loads(output.getvalue().splitlines()[-1])

    def test_one_selected_stream_needs_only_its_backend_and_retains_timings(self):
        result = self.run_selected(["first operation", "second operation"])
        self.assertEqual(result["scope"], "focused")
        self.assertEqual(result["bodies"], {self.tests[0]: 2})
        self.assertEqual(set(result["backends"]), {"LITCH_VIRTUAL_BRIDGE"})
        self.assertEqual(result["body_timings"]["observed_bodies"], 2)
        names = ("first [frame].*", "second (receipt)+?")
        command = interactive_sync._flutter_command(self.tests, plain_names=names)
        self.assertEqual(command.count("--name"), 1)
        self.assertNotIn("--plain-name", command)
        pattern = command[command.index("--name") + 1]
        for name in names:
            self.assertIsNotNone(re.search(pattern, name))
        self.assertIsNone(re.search(pattern, "first frame must remain excluded"))
        single = interactive_sync._flutter_command(self.tests, plain_names=names[:1])
        self.assertEqual(single[single.index("--plain-name"):],
                         ["--plain-name", names[0], "--reporter", "json"])

    def test_missing_requested_name_unrelated_body_and_skip_cannot_pass(self):
        for names, skipped, error in ((["first operation"], False, "requested name"),
                                      (["unrelated"], False, "outside requested"),
                                      (["first operation"], True, "skipped")):
            with self.subTest(names=names, skipped=skipped), self.assertRaisesRegex(ValueError, error):
                self.run_selected(names, skipped=skipped)

    def test_missing_backend_fails_before_child(self):
        self.env.clear()
        with mock.patch.object(interactive_sync.subprocess, "Popen") as child:
            with self.assertRaisesRegex(ValueError, "backend environment missing"):
                interactive_sync.run_focused(self.root, self.env, False, self.tests)
            child.assert_not_called()

    def test_focused_cli_cannot_filter_full_or_escape_app_tests(self):
        for arguments in ((), ("--profile", "full", "--test", self.tests[0]),
                          ("--profile", "focused"),
                          ("--profile", "focused", "--test", "test/../../outside_test.dart"),
                          ("--profile", "full", "--shard-index", "0"),
                          ("--profile", "prepare-shards"),
                          ("--profile", "merge-shards", "--shard-manifest", "manifest.json"),
                          ("--profile", "focused", "--test", self.tests[0], "--total-shards", "12")):
            with self.subTest(arguments=arguments):
                result = subprocess.run([sys.executable, str(MODULE_PATH), *arguments, "--dry-run"],
                                        capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0, result.stdout)
        for profile, options, function in (
                ("prepare-shards", ["--shard-manifest", "manifest.json", "--total-shards", "12"],
                 "prepare_shards"),
                ("merge-shards", ["--shard-manifest", "manifest.json", "--shard-receipts", "receipts",
                                  "--shard-receipt", "summary.json"], "merge_shards")):
            with self.subTest(dispatch=profile):
                with mock.patch.object(sys, "argv", [str(MODULE_PATH), "--profile", profile, *options]), \
                        mock.patch.object(interactive_sync, function) as dispatch, \
                        mock.patch.object(interactive_sync, "_run") as bootstrap:
                    interactive_sync.main()
                dispatch.assert_called_once()
                bootstrap.assert_not_called()


class FullFlutterExecutionEvidenceTest(unittest.TestCase):
    """The final gate invokes unfiltered Flutter once, with the same strict checks."""

    events = FlutterExecutionEvidenceTest.events

    def setUp(self) -> None:
        FlutterExecutionEvidenceTest.setUp(self)
        self.extra = "test/nested/new_feature_test.dart"
        self.tests = (*interactive_sync.FAST_FLUTTER_TESTS, *self.tests, self.extra)
        for name in self.tests:
            path = self.root / "app" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("controlled selected suite")
        self.calls = self.root / "calls.jsonl"
        self.output = self.root / "events.jsonl"
        binary = self.root / "bin/flutter"
        binary.parent.mkdir()
        binary.write_text(
            f"#!{sys.executable}\n"
            "import os, sys\nfrom pathlib import Path\nimport json\n"
            "with Path(os.environ['CHILD_CALLS']).open('a') as calls:\n"
            " calls.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "if sys.argv[1:] == ['--version', '--machine']:\n"
            " print(os.environ['CHILD_TOOLCHAIN']); sys.exit(0)\n"
            "sys.stdout.write(Path(os.environ['CHILD_EVENTS']).read_text())\n"
            "for key in ('CHILD_MUTATE_BACKEND_AFTER', 'CHILD_MUTATE_SOURCE_AFTER'):\n"
            " if os.environ.get(key): Path(os.environ[key]).write_text('changed during child')\n"
            "sys.exit(int(os.environ.get('CHILD_EXIT', '0')))\n")
        binary.chmod(0o755)
        self.env.update(PATH=str(binary.parent) + os.pathsep + os.defpath,
                        CHILD_CALLS=str(self.calls), CHILD_EVENTS=str(self.output),
                        CHILD_TOOLCHAIN=json.dumps({key: f"controlled-{key}" for key in
                                                    interactive_sync.TOOLCHAIN_FIELDS}),
                        GITHUB_RUN_ID="12345", GITHUB_RUN_ATTEMPT="1",
                        LITCH_APP_STAGE_STARTED_EPOCH=str(time.time() - 10))

    def prepare_shards(self) -> dict:
        source = mock.patch.object(interactive_sync, "_source_sha", return_value="a" * 40)
        source.start()
        self.addCleanup(source.stop)
        self.manifest_path = self.root / "manifest.json"
        self.receipts_dir = self.root / "receipts"
        self.summary_path = self.root / "summary.json"
        with contextlib.redirect_stdout(io.StringIO()):
            self.manifest = interactive_sync.prepare_shards(self.root, self.env, self.manifest_path)
        return self.manifest

    def shard_events(self, index: int) -> list[dict]:
        baseline = self.events()
        starts = {event["test"]["id"]: event["test"] for event in baseline
                  if event["type"] == "testStart"}
        rows = {test: [] for test in self.tests}
        for event in baseline:
            if event["type"] == "testDone":
                body = starts[event["testID"]]
                rows[self.tests[body["suiteID"]]].append(
                    {"name": body["name"], "skipped": event["skipped"]})
        rows[self.extra] = [{"name": "duplicate body" if number < 2 else f"extra {number}",
                             "skipped": False} for number in range(18)]
        rows[self.tests[0]].extend({"name": f"half boundary {number}", "skipped": False}
                                   for number in range(5))
        events = []
        for suite, test in enumerate(self.tests):
            start, end = interactive_sync._shard_bounds(len(rows[test]), index, 12)
            selected = rows[test][start:end]
            loader, group = 100000 + suite * 1000, 200000 + suite
            events.extend([
                {"type": "suite", "suite": {"id": suite,
                 "path": str(self.root / "app" / test)}},
                {"type": "testStart", "test": {"id": loader, "suiteID": suite,
                 "name": f"loading {test}", "groupIDs": []}},
                {"type": "testDone", "testID": loader, "result": "success",
                 "skipped": False, "hidden": True},
            ])
            if selected:
                events.append({"type": "group", "group": {"id": group,
                               "suiteID": suite, "parentID": None, "testCount": len(selected)}})
            for number, row in enumerate(selected):
                identity = loader + number + 1
                events.extend([
                    {"type": "testStart", "test": {"id": identity, "suiteID": suite,
                     "name": row["name"], "groupIDs": [group]}},
                    {"type": "testDone", "testID": identity, "result": "success",
                     "skipped": row["skipped"], "hidden": False},
                ])
        return [*events, {"type": "done", "success": True}]

    def run_shard(self, index: int, events: list[dict] | None = None,
                  receipt: Path | None = None) -> dict:
        self.output.write_text("\n".join(json.dumps(event) for event in
                                       (events if events is not None else self.shard_events(index))) + "\n")
        with contextlib.redirect_stdout(io.StringIO()):
            return interactive_sync.run_full(
                self.root, self.env, False, shard_index=index, total_shards=12,
                shard_manifest=self.manifest_path,
                shard_receipt=receipt or self.receipts_dir / f"{index}.json")

    def run_shards(self) -> list[dict]:
        return [self.run_shard(index) for index in range(12)]

    def merge_shards(self, *, env: dict | None = None) -> dict:
        with contextlib.redirect_stdout(io.StringIO()):
            return interactive_sync.merge_shards(self.root, env if env is not None else self.env,
                                                 self.manifest_path, self.receipts_dir,
                                                 self.summary_path)

    def run_child(self, events: list[dict]) -> dict:
        self.output.write_text("\n".join(json.dumps(event) for event in events) + "\n")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            interactive_sync.run_full(self.root, self.env, False)
        return json.loads(output.getvalue().splitlines()[-1])

    def test_one_unfiltered_child_covers_new_suites(self) -> None:
        result = self.run_child(self.events())
        self.assertEqual(result["deadline_s"], 900)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertEqual(calls, [["test", "--no-pub", "--concurrency=4", "--reporter", "json"]])
        self.assertEqual(result["bodies"][self.extra], 1)
        self.assertEqual(result["declared_optional_skips"], 0)
        self.assertEqual(set(result["backends"]), set(self.env) & {
            "LITCH_VIRTUAL_BRIDGE", "LITCHCRAFT_NATIVE_RUNNER", "LITCH_HISTORY_REBASE_WIRE"})
        manifest = self.prepare_shards()
        receipts = self.run_shards()
        summary = self.merge_shards(env={key: self.env[key] for key in
                                        ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT")})
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["scope"], "full")
        self.assertEqual(summary["bodies"][self.extra], 18)
        self.assertEqual(summary["declared_optional_skips"], 0)
        self.assertEqual(summary["total_bodies"], summary["executed_bodies"])
        self.assertEqual(summary["manifest_sha256"], manifest["manifest_sha256"])
        self.assertTrue(summary["stage_target_met"])
        self.assertLess(len(json.dumps(summary)), 32 * 1024)
        self.assertEqual(sum(row["name"] == "duplicate body" for receipt in receipts
                             for row in receipt["suite_bodies"][self.extra]), 2)
        for index, receipt in enumerate(receipts):
            self.assertEqual(receipt["scope"], "full-shard")
            self.assertEqual(receipt["deadline_s"], 900)
            self.assertEqual(receipt["command"], interactive_sync._flutter_command(
                (), full=True, shard_index=index, total_shards=12))
            self.assertEqual(set(receipt["suite_bodies"]), set(self.tests))
        self.assertEqual(interactive_sync._shard_bounds(6, 0, 12), (0, 1))
        self.assertEqual(interactive_sync._shard_bounds(18, 0, 12), (0, 2))
        self.assertEqual(interactive_sync._shard_bounds(1, 5, 12), (0, 1))
        self.assertEqual(sum(interactive_sync._shard_bounds(18, index, 12)[1] -
                             interactive_sync._shard_bounds(18, index, 12)[0]
                             for index in range(12)), 18)
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.run_shard(0)
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.merge_shards()
        for elapsed in (360, 360.0004, 400):
            with self.subTest(stage_elapsed_s=elapsed):
                self.summary_path.unlink()
                with mock.patch.object(interactive_sync.time, "time",
                                       return_value=manifest["stage_started_epoch"] + elapsed):
                    measured = self.merge_shards()
                self.assertTrue(measured["passed"], "Complete coverage qualifies above the optimization target")
                self.assertEqual(measured["stage_target_met"], elapsed <= 360)
                self.assertAlmostEqual(measured["stage_elapsed_s"], elapsed, places=5)

    def test_full_discovery_cannot_omit_a_new_suite(self) -> None:
        events = self.events()
        suite = next(event["suite"]["id"] for event in events
                     if event["type"] == "suite" and event["suite"]["path"].endswith(self.extra))
        identities = {event["test"]["id"] for event in events
                      if event["type"] == "testStart" and event["test"]["suiteID"] == suite}
        events = [event for event in events if not (
            event.get("testID") in identities or
            (event["type"] == "testStart" and event["test"]["id"] in identities))]
        with self.assertRaisesRegex(ValueError, "no executed bodies"):
            self.run_child(events)
        self.prepare_shards()
        self.run_shards()
        saved = {path: path.read_text() for path in self.receipts_dir.glob("*.json")}
        for fault in ("missing", "extra", "duplicate_index", "run", "manifest", "schema",
                      "command", "terminal", "unfinished", "suite", "loader", "root_count", "interval",
                      "missing_named", "body_skipped", "skip_count"):
            with self.subTest(shard_fault=fault):
                records = {path: json.loads(text) for path, text in saved.items()}
                first = records[self.receipts_dir / "0.json"]
                second = records[self.receipts_dir / "1.json"]
                if fault == "missing":
                    (self.receipts_dir / "0.json").unlink()
                    records.pop(self.receipts_dir / "0.json")
                elif fault == "extra":
                    records[self.receipts_dir / "extra.json"] = copy.deepcopy(first)
                elif fault == "duplicate_index":
                    second["shard_index"] = 0
                elif fault == "run":
                    first["run_attempt"] = "2"
                elif fault == "manifest":
                    first["manifest_sha256"] = "b" * 64
                elif fault == "schema":
                    first["schema"] = 2
                elif fault == "command":
                    first["command"].extend(["--plain-name", "a filtered subset"])
                elif fault == "terminal":
                    first["terminal_success"] = False
                elif fault == "unfinished":
                    first["started_tests"] += 1
                elif fault == "suite":
                    first["suite_bodies"].pop(self.extra)
                elif fault == "loader":
                    first["suite_loaders"][self.extra] = 0
                elif fault == "root_count":
                    first["suite_root_counts"][self.extra] += 1
                elif fault == "interval":
                    moved = first["suite_bodies"][self.extra].pop()
                    first["suite_root_counts"][self.extra] -= 1
                    first["bodies"][self.extra] -= 1
                    second["suite_bodies"][self.extra].append(moved)
                    second["suite_root_counts"][self.extra] += 1
                    second["bodies"][self.extra] += 1
                    second["started_tests"] += 1
                    second["completed_tests"] += 1
                elif fault == "missing_named":
                    next(row for record in records.values()
                         for row in record["suite_bodies"][interactive_sync.COLD_ATTACH_TEST]
                         if row["name"].startswith(interactive_sync.MIXED_WORKLOAD_NAME))["name"] = "other"
                elif fault == "body_skipped":
                    first["suite_bodies"][self.extra][0]["skipped"] = True
                    first["bodies"][self.extra] -= 1
                elif fault == "skip_count":
                    first["declared_optional_skips"] = 1
                try:
                    for path, record in records.items():
                        path.write_text(json.dumps(record))
                    with self.assertRaises(ValueError):
                        self.merge_shards()
                    self.assertFalse(self.summary_path.exists())
                finally:
                    for path in self.receipts_dir.glob("*.json"):
                        path.unlink()
                    for path, text in saved.items():
                        path.write_text(text)

    def test_full_gate_retains_required_body_and_terminal_failures(self) -> None:
        for fault in ("required_skip", "missing_named", "missing_terminal", "error", "exit"):
            with self.subTest(fault=fault):
                events = self.events()
                self.env["CHILD_EXIT"] = "0"
                if fault == "required_skip":
                    events[2]["skipped"] = True
                elif fault == "missing_named":
                    next(event["test"] for event in events if event["type"] == "testStart" and
                         event["test"]["name"].startswith(interactive_sync.MIXED_WORKLOAD_NAME))["name"] = "other body"
                elif fault == "missing_terminal":
                    events.pop()
                elif fault == "error":
                    events.insert(-1, {"type": "error", "error": "controlled full-run failure"})
                else:
                    self.env["CHILD_EXIT"] = "1"
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    self.run_child(events)
        self.env["CHILD_EXIT"] = "0"
        self.prepare_shards()
        for fault in ("required_skip", "missing_terminal", "unfinished", "root_count",
                      "missing_group", "duplicate_root", "missing_suite", "missing_loader", "duplicate_start",
                      "error", "exit", "empty_exit79"):
            with self.subTest(shard_fault=fault):
                self.env["CHILD_EXIT"] = "0"
                events = self.shard_events(0)
                body = next(event for event in events if event["type"] == "testDone"
                            and event["hidden"] is False)
                if fault == "required_skip":
                    body["skipped"] = True
                elif fault == "missing_terminal":
                    events.pop()
                elif fault == "unfinished":
                    events.remove(body)
                elif fault == "root_count":
                    next(event["group"] for event in events if event["type"] == "group")["testCount"] += 1
                elif fault in ("missing_group", "duplicate_root"):
                    group = next(event for event in events if event["type"] == "group")
                    if fault == "missing_group":
                        events.remove(group)
                    else:
                        duplicate = copy.deepcopy(group)
                        duplicate["group"]["id"] += 99999
                        events.insert(events.index(group) + 1, duplicate)
                elif fault == "missing_suite":
                    identities = {event["test"]["id"] for event in events
                                  if event["type"] == "testStart" and event["test"]["suiteID"] == 0}
                    events = [event for event in events if not (
                        (event["type"] == "suite" and event["suite"]["id"] == 0) or
                        (event["type"] == "group" and event["group"]["suiteID"] == 0) or
                        (event["type"] == "testStart" and event["test"]["id"] in identities) or
                        event.get("testID") in identities)]
                elif fault == "missing_loader":
                    loader = events[1]["test"]["id"]
                    events = [event for event in events if not (
                              (event["type"] == "testStart" and event["test"]["id"] == loader) or
                              event.get("testID") == loader)]
                elif fault == "duplicate_start":
                    events.insert(2, copy.deepcopy(events[1]))
                elif fault == "error":
                    events.insert(-1, {"type": "error", "testID": body["testID"],
                                       "error": "controlled shard error"})
                elif fault == "exit":
                    self.env["CHILD_EXIT"] = "1"
                elif fault == "empty_exit79":
                    identities = {event["testID"] for event in events if
                                  event["type"] == "testDone" and event["hidden"] is False}
                    events = [event for event in events if not (event["type"] == "group" or
                              (event["type"] == "testStart" and event["test"]["id"] in identities) or
                              event.get("testID") in identities)]
                    self.env["CHILD_EXIT"] = "79"
                receipt = self.root / f"failed-{fault}.json"
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    self.run_shard(0, events, receipt)
                self.assertFalse(receipt.exists())

    def test_missing_backend_or_required_file_rejects_before_child(self) -> None:
        self.env.pop("LITCH_HISTORY_REBASE_WIRE")
        with self.assertRaisesRegex(ValueError, "LITCH_HISTORY_REBASE_WIRE"):
            self.run_child(self.events())
        self.assertFalse(self.calls.exists())
        self.env["LITCH_HISTORY_REBASE_WIRE"] = str(self.bridge)
        (self.root / "app" / interactive_sync.COLD_ATTACH_TEST).unlink()
        with self.assertRaisesRegex(ValueError, "selected Flutter suite missing"):
            self.run_child(self.events())
        self.assertFalse(self.calls.exists())
        missing = self.root / "app" / interactive_sync.COLD_ATTACH_TEST
        missing.write_text("controlled selected suite")
        with mock.patch.object(interactive_sync.subprocess, "check_output", return_value=" M tracked.py\n"):
            with self.assertRaisesRegex(ValueError, "clean source"):
                interactive_sync._source_sha(self.root)
        stage = self.env.pop("LITCH_APP_STAGE_STARTED_EPOCH")
        with self.assertRaisesRegex(ValueError, "LITCH_APP_STAGE_STARTED_EPOCH"):
            interactive_sync.prepare_shards(self.root, self.env, self.root / "no-stage.json")
        self.assertFalse((self.root / "no-stage.json").exists())
        for index, value in enumerate(("nan", "inf", "0", str(time.time() + 1000))):
            self.env["LITCH_APP_STAGE_STARTED_EPOCH"] = value
            rejected = self.root / f"invalid-stage-{index}.json"
            with self.assertRaisesRegex(ValueError, "invalid app stage start"):
                interactive_sync.prepare_shards(self.root, self.env, rejected)
            self.assertFalse(rejected.exists())
        self.env["LITCH_APP_STAGE_STARTED_EPOCH"] = stage
        self.prepare_shards()
        backend_source = self.bridge.read_text()
        suite = self.root / "app" / self.extra
        suite_source = suite.read_text()
        for fault in ("backend_before", "source_before", "new_suite", "head", "run", "sdk", "stale"):
            with self.subTest(shard_fault=fault):
                receipt = self.root / f"rejected-{fault}.json"
                added = self.root / "app/test/added_test.dart"
                if fault == "backend_before":
                    self.bridge.write_text("changed before child")
                elif fault == "source_before":
                    suite.write_text("changed before child")
                elif fault == "new_suite":
                    added.write_text("new suite")
                elif fault == "run":
                    self.env["GITHUB_RUN_ATTEMPT"] = "2"
                elif fault == "sdk":
                    self.env["CHILD_TOOLCHAIN"] = json.dumps({key: "different" for key in
                                                            interactive_sync.TOOLCHAIN_FIELDS})
                elif fault == "stale":
                    receipt.write_text("stale receipt")
                before = self.calls.read_text().count('["test",')
                try:
                    head = "b" * 40 if fault == "head" else "a" * 40
                    with mock.patch.object(interactive_sync, "_source_sha", return_value=head):
                        with self.assertRaises(ValueError):
                            self.run_shard(0, receipt=receipt)
                    self.assertEqual(self.calls.read_text().count('["test",'), before)
                    self.assertEqual(receipt.exists(), fault == "stale")
                finally:
                    self.bridge.write_text(backend_source)
                    suite.write_text(suite_source)
                    added.unlink(missing_ok=True)
                    self.env["GITHUB_RUN_ATTEMPT"] = "1"
                    self.env["CHILD_TOOLCHAIN"] = json.dumps({key: f"controlled-{key}" for key in
                                                            interactive_sync.TOOLCHAIN_FIELDS})
        for key, target in (("CHILD_MUTATE_BACKEND_AFTER", self.bridge),
                            ("CHILD_MUTATE_SOURCE_AFTER", suite)):
            receipt = self.root / f"rejected-{key}.json"
            self.env[key] = str(target)
            with self.assertRaisesRegex(ValueError, "content mismatch"):
                self.run_shard(0, receipt=receipt)
            self.assertFalse(receipt.exists())
            self.env.pop(key)
            self.bridge.write_text(backend_source)
            suite.write_text(suite_source)


class Physical32EvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.records = []
        rounds = []
        for index in range(3):
            seq = index * 3 + 1
            identity = {"canonical_bytes": 9000 + index, "canonical_crc32": 100 + index}
            common = {"schema": 1, "type": "app_trace", "frames": 32,
                      "session": 7, "generation": 10 + index}
            self.records.extend([
                {**common, **identity, "seq": seq, "mono_ms": seq * 1000,
                 "event": "litchcraft.frame_store_delivery", "accepted": True,
                 "resident_hit": False, "resident_cache_hit": False,
                 "body_bytes": 5000, "chunks": 24},
                {**common, "seq": seq + 1, "mono_ms": (seq + 1) * 1000,
                 "event": "litchcraft.frame_store_timing", "transfer_ms": 2000 + index},
                {**common, "seq": seq + 2, "mono_ms": (seq + 2) * 1000,
                 "event": "litchcraft.frame_store_snapshot_read", "read_kind": "saved_body",
                 "body_bytes": identity["canonical_bytes"],
                 "body_crc32": identity["canonical_crc32"], "chunks": 42,
                 "cache_hits": 0, "elapsed_ms": 3000 + index},
            ])
            for record in self.records[-3:]:
                record["ts"] = f"2026-10-23T12:00:{record['seq']:02}Z"
            rounds.append({"upload_seq": seq, "timing_seq": seq + 1, "download_seq": seq + 2,
                           "adopted": True, "evidence": "Use Litch completed with matching artwork"})
        self.observations = {
            "app_candidate": "candidate-a", "firmware_candidate": "candidate-b",
            "phone": "iPhone release", "operator": "engineer", "board": "V3.3",
            "app_build_mode": "release", "trace_extraction_verified": True,
            "usb_evidence": "bounded USB capture reviewed",
            "workload_elapsed_ms": 75000, "roundtrips": rounds,
            "start_seq": 1, "end_seq": 9,
            "start_utc": "2026-10-23T12:00:01Z", "end_utc": "2026-10-23T12:00:09Z",
            "checks": {key: {"passed": True, "evidence": "observed actual device"}
                       for key in interactive_sync.PHYSICAL_CHECKS},
            "failures": [],
        }

    def capture(self) -> tuple[list[dict], dict]:
        # Keep the original identity/cache/failure mutations at their old seam;
        # expand only the fixture's trace with the now-required producer mapping.
        records, observations = copy.deepcopy(self.records), copy.deepcopy(self.observations)
        output = []
        transfer = 0
        for record in records:
            record["seq"] *= 10
            if record.get("event") == "litchcraft.frame_store_delivery" and record.get("accepted"):
                transfer += 1
                common = {key: record[key] for key in ("schema", "type", "ts", "mono_ms")}
                life, request = 100 + transfer, transfer
                output.extend([
                    {**common, "seq": record["seq"] - 2,
                     "event": "litchcraft.object_submission", "life": life, "request": request,
                     "first": 1, "last": 48, "count": 48, "queued_us": 0,
                     "first_send_us": 10, "last_return_us": 100, "accepted_us": 9100,
                     "done_us": 9100, "grant_wait_us": 0, "native_us": 90,
                     "outcome": "accepted"},
                    {**common, "seq": record["seq"] - 1,
                     "event": "litchcraft.object_transfer", "life": life, "request": request,
                     "session": record.get("session"), "generation": record.get("generation"),
                     "accepted": True, "submissions": 1, "objects": 48},
                ])
            output.append(record)
        for trip in observations["roundtrips"]:
            for key in ("upload_seq", "timing_seq", "download_seq"):
                trip[key] *= 10
        # Delivery and timing are consecutive producer records, with a mapping
        # immediately before delivery; scale the fixture's other sequence IDs.
        renumber = {record["seq"]: index for index, record in enumerate(output, 1)}
        for trip in observations["roundtrips"]:
            for key in ("upload_seq", "timing_seq", "download_seq"):
                trip[key] = renumber[trip[key]]
        observations["start_seq"] = renumber[observations["start_seq"] * 10 - 2]
        observations["end_seq"] = renumber[observations["end_seq"] * 10]
        for record in output:
            record["seq"] = renumber[record["seq"]]
        return output, observations

    def analyze(self) -> dict:
        records, observations = self.capture()
        return interactive_sync.analyze_physical32(
            {"available": True, "failure": None, "dropped_events": 0,
             "jsonl": "\n".join(json.dumps(record) for record in records)}, observations)

    def test_complete_workload_reports_all_three_even_when_over_sixty_seconds(self) -> None:
        result = self.analyze()
        self.assertTrue(result["passed"], result["failures"])
        self.assertTrue(result["over_60s"])
        self.assertEqual(len(result["samples"]), 3)
        self.assertEqual(result["statistics"]["download_ms"],
                         {"n": 3, "mean": 3001, "median": 3001, "p95": 3002, "max": 3002})
        self.assertEqual(result["body_bytes"], {"upload": 15000, "download": 27003})
        self.assertEqual(result["cache_hits"], 0)

    def test_resident_upload_and_cached_or_partial_download_cannot_pass(self) -> None:
        original = copy.deepcopy(self.records)
        for field, value in (("resident_hit", True), ("body_bytes", 0), ("chunks", 0)):
            with self.subTest(field=field):
                self.records = copy.deepcopy(original)
                self.records[0][field] = value
                self.assertFalse(self.analyze()["passed"])
        for kind in ("entry_metadata", "resident_cache", "verified_cache", "live_frames"):
            with self.subTest(kind=kind):
                self.records = copy.deepcopy(original)
                self.records[2]["read_kind"] = kind
                self.records[2]["body_bytes"] = 0
                self.assertFalse(self.analyze()["passed"])

    def test_repeated_or_mismatched_content_and_incomplete_workload_fail(self) -> None:
        original = copy.deepcopy(self.records)
        self.records[5]["body_crc32"] += 1
        self.assertFalse(self.analyze()["passed"])
        self.records = copy.deepcopy(original)
        for target, source in ((self.records[3], self.records[0]),
                               (self.records[5], self.records[2])):
            for key in ("canonical_bytes", "canonical_crc32", "body_bytes", "body_crc32"):
                if key in source:
                    target[key] = source[key]
        self.assertFalse(self.analyze()["passed"])
        self.records = original
        self.observations["roundtrips"].pop()
        self.assertFalse(self.analyze()["passed"])

    def test_failed_workload_or_missing_real_input_evidence_cannot_pass(self) -> None:
        self.observations["roundtrips"][0]["adopted"] = False
        self.assertFalse(self.analyze()["passed"])
        self.observations["roundtrips"][0]["adopted"] = True
        self.observations["checks"]["board_input"]["passed"] = False
        self.assertFalse(self.analyze()["passed"])
        self.observations["checks"]["board_input"]["passed"] = True
        self.records.insert(2, {"schema": 1, "type": "app_trace", "seq": 3,
                               "mono_ms": 2500, "ts": "2026-10-23T12:00:02.500000Z",
                               "event": "litchcraft.transport_failed"})
        for record in self.records[3:]:
            record["seq"] += 1
        for trip in self.observations["roundtrips"]:
            for key in ("upload_seq", "timing_seq", "download_seq"):
                if trip[key] >= 3:
                    trip[key] += 1
        self.observations["end_seq"] += 1
        result = self.analyze()
        self.assertFalse(result["passed"])
        self.assertTrue(any("trace failure" in failure for failure in result["failures"]))

    def test_wrong_timing_authority_and_missing_release_observability_fail(self) -> None:
        self.records[1]["generation"] += 1
        self.assertFalse(self.analyze()["passed"])
        self.records[1]["generation"] -= 1
        self.observations["trace_extraction_verified"] = False
        self.assertFalse(self.analyze()["passed"])

    def test_missing_or_nonpositive_authority_cannot_match(self) -> None:
        original = copy.deepcopy(self.records)
        for field in ("session", "generation"):
            for value in (None, 0, -1, True, 7.0, "7"):
                with self.subTest(field=field, value=value):
                    self.records = copy.deepcopy(original)
                    for record in self.records[:2]:
                        if value is None:
                            record.pop(field)
                        else:
                            record[field] = value
                    self.assertFalse(self.analyze()["passed"])
            self.records = copy.deepcopy(original)
            self.records[2].pop(field)
            self.assertFalse(self.analyze()["passed"])

    def test_later_matching_timing_cannot_replace_consecutive_upload_timing(self) -> None:
        later = {**self.records[1], "seq": 3, "mono_ms": 2500,
                 "ts": "2026-10-23T12:00:02.500000Z", "transfer_ms": 1}
        self.records.insert(2, later)
        for record in self.records[3:]:
            record["seq"] += 1
        for trip in self.observations["roundtrips"]:
            for key in ("upload_seq", "timing_seq", "download_seq"):
                if trip[key] >= 3:
                    trip[key] += 1
        self.observations["end_seq"] += 1
        self.assertTrue(self.analyze()["passed"])
        self.observations["roundtrips"][0]["timing_seq"] = later["seq"]
        result = self.analyze()
        self.assertFalse(result["passed"])
        self.assertIn("consecutive", result["samples"][0]["failure"])

    def test_physical_check_evidence_requires_nonempty_trimmed_text(self) -> None:
        for check in interactive_sync.PHYSICAL_CHECKS:
            for value in (True, 1, [], "", " \t\n"):
                with self.subTest(check=check, value=value):
                    self.observations["checks"][check]["evidence"] = value
                    self.assertFalse(self.analyze()["passed"])
            self.observations["checks"][check]["evidence"] = "observed actual device"

    def test_reconnect_can_change_root_generation_but_not_content_identity(self) -> None:
        self.records[2]["generation"] += 100
        self.records[2]["session"] += 100
        self.assertTrue(self.analyze()["passed"])

    def test_previous_process_failures_and_cache_hits_do_not_contaminate_workload(self) -> None:
        self.records.insert(0, {"schema": 1, "type": "app_trace", "seq": 1,
                               "mono_ms": 99999, "ts": "2026-10-23T11:00:00Z",
                               "event": "litchcraft.transport_failed", "cache_hits": 32})
        self.assertTrue(self.analyze()["passed"])

    def test_raw_release_segments_report_unknown_health_without_fabricated_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "segment.jsonl"
            observations = Path(directory) / "observations.json"
            records, observation_values = self.capture()
            trace.write_text(json.dumps({"schema": 1, "type": "app_trace_segment", "bucket": 1}) +
                             "\n" + "\n".join(json.dumps(record) for record in records))
            observations.write_text(json.dumps(observation_values))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertTrue(interactive_sync.run_physical32([trace], observations))
            self.assertIn("unknown", json.loads(output.getvalue())["trace_health"])


class ObjectSubmissionEvidenceTest(unittest.TestCase):
    def setUp(self) -> None:
        fixture = Physical32EvidenceTest()
        fixture.setUp()
        self.records, self.observations = fixture.capture()

    def analyze(self) -> dict:
        return interactive_sync.analyze_physical32(
            {"available": True, "failure": None, "dropped_events": 0,
             "jsonl": "\n".join(json.dumps(record) for record in self.records)},
            self.observations)

    def add_second_batch(self) -> dict:
        row = {**self.records[0], "seq": 2, "first": 49, "last": 54, "count": 6,
               "queued_us": 25000, "first_send_us": 25100, "last_return_us": 25200,
               "accepted_us": 25300, "done_us": 25400, "native_us": 100}
        for record in self.records[1:]:
            record["seq"] += 1
        for trip in self.observations["roundtrips"]:
            for key in ("upload_seq", "timing_seq", "download_seq"):
                if trip[key] >= 2:
                    trip[key] += 1
        self.observations["end_seq"] += 1
        self.records.insert(1, row)
        self.records[2].update(submissions=2, objects=54)
        return row

    def test_observed_acceptance_tail_and_next_invocation_gap_are_distinct_from_native(self) -> None:
        self.add_second_batch()
        result = self.analyze()
        self.assertTrue(result["passed"], result["failures"])
        timing = result["samples"][0]["object_timing"]
        self.assertEqual(timing["objects"], 54)
        self.assertEqual(timing["submissions"], 2)
        self.assertEqual(timing["native_us"]["max"], 100)
        self.assertEqual(timing["acceptance_tail_us"]["max"], 9000)
        self.assertEqual(timing["next_invocation_gap_us"]["max"], 15900)

    def test_missing_one_of_multiple_submission_rows_cannot_pass(self) -> None:
        self.add_second_batch()
        del self.records[1]
        self.assertFalse(self.analyze()["passed"])

    def test_missing_mapping_or_submission_cannot_pass(self) -> None:
        original = copy.deepcopy(self.records)
        for kind in ("litchcraft.object_transfer", "litchcraft.object_submission"):
            with self.subTest(kind=kind):
                self.records = [record for record in copy.deepcopy(original)
                                if not (record["event"] == kind and record["seq"] in (1, 2))]
                # Keep the selected bounds valid so missing evidence owns failure.
                self.observations["start_seq"] = self.records[0]["seq"]
                self.assertFalse(self.analyze()["passed"])

    def test_cross_lifetime_and_mapping_authority_cannot_match(self) -> None:
        original = copy.deepcopy(self.records)
        for index, key in ((0, "life"), (0, "request"), (1, "session"), (1, "generation"),
                           (1, "submissions"), (1, "objects")):
            with self.subTest(index=index, key=key):
                self.records = copy.deepcopy(original)
                self.records[index][key] += 1
                self.assertFalse(self.analyze()["passed"])

    def test_absent_endpoints_failed_or_omitted_rows_do_not_invent_timings(self) -> None:
        original = copy.deepcopy(self.records)
        mutations = [(key, None) for key in
                     ("queued_us", "first_send_us", "last_return_us", "accepted_us", "done_us")]
        mutations += [("outcome", "failed"), ("fields_omitted", True),
                      ("count", 47), ("native_us", -1), ("grant_wait_us", 100000)]
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                self.records = copy.deepcopy(original)
                self.records[0][key] = value
                self.assertFalse(self.analyze()["passed"])

    def test_overlapping_batch_or_sequence_evidence_fails(self) -> None:
        row = self.add_second_batch()
        row["queued_us"] = 9099
        self.assertFalse(self.analyze()["passed"])
        row["queued_us"] = 25000
        row.update(first=48, last=53)
        self.assertFalse(self.analyze()["passed"])

    def test_actual_retained_row_allows_acceptance_before_native_return(self) -> None:
        self.records[0].update(life=2138150201925734693, request=11, first=15, last=33, count=19,
                               queued_us=117207, first_send_us=117313, last_return_us=134849,
                               accepted_us=134841, done_us=134854, grant_wait_us=0, native_us=17536)
        self.records[1].update(life=2138150201925734693, request=11, objects=19)
        result = self.analyze()
        self.assertTrue(result["passed"], result["failures"])
        self.assertEqual(result["samples"][0]["object_timing"]["acceptance_tail_us"]["max"], 0)

    def test_request_reuse_in_new_lifetime_never_subtracts_old_clock(self) -> None:
        self.records[5]["request"] = self.records[0]["request"]
        self.records[6]["request"] = self.records[0]["request"]
        self.records[5].update(queued_us=0, first_send_us=1, last_return_us=2,
                               accepted_us=3, done_us=4, native_us=1)
        result = self.analyze()
        self.assertTrue(result["passed"], result["failures"])
        self.assertEqual(result["samples"][1]["object_timing"]["next_invocation_gap_us"], {"n": 0})

    def test_unmapped_submission_and_wrong_typed_identity_fail(self) -> None:
        row = self.add_second_batch()
        row["life"] += 1
        self.assertFalse(self.analyze()["passed"])
        self.records[0]["life"] = self.records[2]["life"] = 1
        row["life"] = True
        self.assertFalse(self.analyze()["passed"])

    def test_direct_analyzer_keeps_one_MiB_input_bound(self) -> None:
        self.records[0]["padding"] = "x" * (1024 * 1024)
        with self.assertRaisesRegex(ValueError, "one MiB"):
            self.analyze()


if __name__ == "__main__":
    unittest.main()
