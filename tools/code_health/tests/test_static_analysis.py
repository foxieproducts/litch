#!/usr/bin/env python3

from __future__ import annotations

import datetime as dt
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


TOOL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_ROOT))

from common import CodeHealthError, CommandResult  # noqa: E402
import static_analysis  # noqa: E402
from static_analysis import (  # noqa: E402
    Diagnostic,
    classify_diagnostics,
    load_suppressions,
    parse_cppcheck_xml,
    parse_dart_machine_output,
    run_static_analysis,
)


FIXTURES = Path(__file__).parent / "fixtures"


class StaticAnalysisTest(unittest.TestCase):
    def test_cppcheck_parser_scrubs_repository_path(self) -> None:
        repo = Path("/workspace/litch").resolve()
        text = (FIXTURES / "static_cppcheck.xml").read_text(encoding="utf-8").replace(
            "REPO", str(repo)
        )
        diagnostics = parse_cppcheck_xml(text, repo)
        self.assertEqual(len(diagnostics), 1)
        self.assertEqual(diagnostics[0].path, "src/example.cpp")
        self.assertEqual(diagnostics[0].code, "knownConditionTrueFalse")

    def test_cppcheck_blank_output_is_not_a_clean_result(self) -> None:
        with self.assertRaisesRegex(CodeHealthError, "XML results envelope"):
            parse_cppcheck_xml("", Path("/workspace/litch"))

        with patch.object(
            static_analysis,
            "probe_tool",
            return_value={"status": "available", "version": "cppcheck 2", "reason": None},
        ):
            report = run_static_analysis(
                repo_root=Path("/workspace/litch"),
                files=["src/a.cpp"],
                requested_tools=["cppcheck"],
                command_runner=lambda command, **kwargs: CommandResult(
                    tuple(command), 0, "", False
                ),
            )
        self.assertEqual(report["tools"][0]["status"], "invocation_error")

    def test_dart_machine_parser_uses_app_relative_paths(self) -> None:
        repo = Path("/workspace/litch").resolve()
        diagnostics = parse_dart_machine_output(
            (FIXTURES / "static_dart.txt").read_text(encoding="utf-8"),
            repo,
            repo / "app",
        )
        self.assertEqual(diagnostics[0].path, "app/lib/example.dart")
        self.assertEqual(diagnostics[0].line, 8)

    def test_baseline_and_unexpired_suppression_are_distinct(self) -> None:
        baseline = Diagnostic("cppcheck", "src/a.cpp", 10, 2, "warning", "A", "old")
        suppressed = Diagnostic("cppcheck", "src/b.cpp", 11, 2, "warning", "B", "waived")
        new = Diagnostic("dart", "app/lib/c.dart", 3, 1, "warning", "C", "new")
        result = classify_diagnostics(
            [new, suppressed, baseline],
            {baseline.baseline_fingerprint},
            {
                suppressed.fingerprint: {
                    "owner": "team@example.com",
                    "reason": "upstream issue",
                    "expires": "2027-01-01",
                    "expired": False,
                }
            },
        )
        states = {item["path"]: item["state"] for item in result}
        self.assertEqual(states["src/a.cpp"], "baseline")
        self.assertEqual(states["src/b.cpp"], "suppressed")
        self.assertEqual(states["app/lib/c.dart"], "new")

    def test_suppressions_require_reason_owner_and_expiry(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "suppressions.json"
            path.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "suppressions": [
                            {"fingerprint": "abc", "reason": "", "owner": "team", "expires": "2027-01-01"}
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaises(CodeHealthError):
                load_suppressions(path, dt.date(2026, 8, 30))

    def test_duplicate_suppression_fingerprint_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "suppressions.json"
            item = {
                "fingerprint": "abc",
                "reason": "temporary",
                "owner": "team",
                "expires": "2027-01-01",
            }
            path.write_text(
                json.dumps({"schema_version": 1, "suppressions": [item, item]}),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(CodeHealthError, "duplicate suppression"):
                load_suppressions(path, dt.date(2026, 8, 30))

    def test_expired_suppression_does_not_hide_diagnostic(self) -> None:
        diagnostic = Diagnostic("cppcheck", "src/a.cpp", 4, 1, "warning", "A", "message")
        result = classify_diagnostics(
            [diagnostic],
            set(),
            {
                diagnostic.fingerprint: {
                    "owner": "team",
                    "reason": "temporary",
                    "expires": "2025-01-01",
                    "expired": True,
                }
            },
        )
        self.assertEqual(result[0]["state"], "new")
        self.assertTrue(result[0]["suppression"]["expired"])

    def test_suppression_identity_is_narrow_but_baseline_identity_survives_line_motion(self) -> None:
        first = Diagnostic("cppcheck", "src/a.cpp", 4, 1, "warning", "A", "same")
        second = Diagnostic("cppcheck", "src/a.cpp", 9, 1, "warning", "A", "same")

        self.assertNotEqual(first.fingerprint, second.fingerprint)
        self.assertEqual(first.baseline_fingerprint, second.baseline_fingerprint)

    def test_baseline_cardinality_does_not_hide_a_new_duplicate(self) -> None:
        old = Diagnostic("cppcheck", "src/a.cpp", 4, 1, "warning", "A", "same")
        added = Diagnostic("cppcheck", "src/a.cpp", 9, 1, "warning", "A", "same")

        result = classify_diagnostics(
            [added, old],
            {old.baseline_fingerprint: 1},
            {},
        )

        self.assertEqual([item["state"] for item in result], ["baseline", "new"])

    def test_suppressed_old_occurrence_still_consumes_baseline_cardinality(self) -> None:
        old = Diagnostic("cppcheck", "src/a.cpp", 4, 1, "warning", "A", "same")
        added = Diagnostic("cppcheck", "src/a.cpp", 9, 1, "warning", "A", "same")
        result = classify_diagnostics(
            [added, old],
            {old.baseline_fingerprint: 1},
            {
                old.fingerprint: {
                    "owner": "team",
                    "reason": "temporary",
                    "expires": "2027-01-01",
                    "expired": False,
                }
            },
        )

        self.assertEqual([item["state"] for item in result], ["suppressed", "new"])

    def test_truncated_output_is_incomplete_and_out_of_scope_findings_are_counted(self) -> None:
        repo = Path("/workspace/litch").resolve()
        output = (
            "<?xml version='1.0'?><results><errors>"
            "<error id='one' severity='warning' msg='one'>"
            "<location file='/workspace/litch/src/in.cpp' line='2'/></error>"
            "<error id='two' severity='warning' msg='two'>"
            "<location file='/workspace/litch/src/out.cpp' line='3'/></error>"
            "</errors></results>"
        )
        with patch.object(
            static_analysis,
            "probe_tool",
            return_value={"status": "available", "version": "cppcheck 2", "reason": None},
        ):
            report = run_static_analysis(
                repo_root=repo,
                files=["src/in.cpp"],
                requested_tools=["cppcheck", "cppcheck"],
                command_runner=lambda command, **kwargs: CommandResult(
                    tuple(command), 0, output, True
                ),
            )

        self.assertEqual(len(report["tools"]), 1)
        self.assertEqual(report["tools"][0]["status"], "incomplete_output")
        self.assertEqual(report["summary"]["incomplete_outputs"], 1)
        self.assertEqual(report["summary"]["out_of_scope_diagnostics"], 1)
        self.assertEqual([item["path"] for item in report["diagnostics"]], ["src/in.cpp"])

    def test_one_failed_dart_invocation_cannot_be_hidden_by_another_finding(self) -> None:
        repo = Path("/workspace/litch").resolve()
        calls = 0

        def run(command, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 1:
                return CommandResult(
                    tuple(command),
                    3,
                    "WARNING|STATIC_WARNING|CODE|lib/a.dart|1|1|1|message\n",
                    False,
                )
            return CommandResult(tuple(command), -9, "", False)

        with patch.object(
            static_analysis,
            "probe_tool",
            return_value={"status": "available", "version": "dart 3", "reason": None},
        ):
            report = run_static_analysis(
                repo_root=repo,
                files=["app/lib/a.dart", "app/lib/b.dart"],
                requested_tools=["dart"],
                command_runner=run,
            )

        self.assertEqual(report["tools"][0]["status"], "invocation_error")
        self.assertEqual(report["summary"]["invocation_errors"], 1)
        self.assertEqual(report["summary"]["new"], 1)
        self.assertEqual(len(report["tools"][0]["commands"]), 2)

    def test_timeout_with_zero_returncode_is_an_invocation_error(self) -> None:
        repo = Path("/workspace/litch").resolve()
        cppcheck_xml = "<?xml version='1.0'?><results><errors/></results>"
        with patch.object(
            static_analysis,
            "probe_tool",
            return_value={"status": "available", "version": "tool 1", "reason": None},
        ):
            cppcheck = run_static_analysis(
                repo_root=repo,
                files=["src/a.cpp"],
                requested_tools=["cppcheck"],
                command_runner=lambda command, **kwargs: CommandResult(
                    tuple(command), 0, cppcheck_xml, False, True
                ),
            )
            dart = run_static_analysis(
                repo_root=repo,
                files=["app/lib/a.dart"],
                requested_tools=["dart"],
                command_runner=lambda command, **kwargs: CommandResult(
                    tuple(command), 0, "", False, True
                ),
            )

        self.assertEqual(cppcheck["tools"][0]["status"], "invocation_error")
        self.assertIn("timed out", cppcheck["tools"][0]["reason"])
        self.assertEqual(dart["tools"][0]["status"], "invocation_error")
        self.assertIn("timed out", dart["tools"][0]["reason"])


if __name__ == "__main__":
    unittest.main()
