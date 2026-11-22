#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path


TOOL_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_ROOT))

from common import CodeHealthError  # noqa: E402
from crap_report import (  # noqa: E402
    build_crap_report,
    calculate_crap,
    parse_complexity_json,
    parse_lcov_line_hits,
    parse_lizard_csv,
    load_complexity,
)


FIXTURES = Path(__file__).parent / "fixtures"


class CrapReportTest(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Path("/workspace/litch").resolve()
        self.functions = parse_complexity_json(
            json.loads((FIXTURES / "crap_complexity.json").read_text(encoding="utf-8")),
            self.repo,
        )
        self.coverage = parse_lcov_line_hits(
            (FIXTURES / "crap_coverage.lcov").read_text(encoding="utf-8"),
            self.repo,
        )

    def test_formula(self) -> None:
        self.assertAlmostEqual(calculate_crap(5, 0.5), 8.125)

    def test_invalid_or_incomplete_lcov_is_rejected(self) -> None:
        for invalid in ("", "<html>not lcov</html>", "SF:src/a.cpp\nDA:1,1\n"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(CodeHealthError):
                    parse_lcov_line_hits(invalid, self.repo)

    def test_report_is_explicitly_scoped_and_top_sorted(self) -> None:
        report = build_crap_report(
            functions=self.functions,
            line_hits=self.coverage,
            scope_selectors=["src/demo.cpp"],
            scope_files=["src/demo.cpp"],
            hotspot_limit=1,
        )
        self.assertEqual(report["summary"]["function_count"], 2)
        self.assertEqual(len(report["hotspots"]), 1)
        self.assertEqual(report["hotspots"][0]["long_name"], "demo(int)")
        self.assertEqual(report["hotspots"][0]["crap"], 8.125)
        self.assertIsNone(report["policy_thresholds"])

    def test_missing_executable_lines_are_unavailable_not_zero_coverage(self) -> None:
        report = build_crap_report(
            functions=self.functions,
            line_hits=self.coverage,
            scope_selectors=["src/outside.cpp"],
        )
        row = report["functions"][0]
        self.assertEqual(row["status"], "coverage_unavailable")
        self.assertIsNone(row["coverage"]["fraction"])
        self.assertIsNone(row["crap"])

    def test_baseline_delta_uses_stable_fingerprint(self) -> None:
        initial = build_crap_report(
            functions=self.functions,
            line_hits=self.coverage,
            scope_selectors=["src/demo.cpp"],
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            baseline = Path(temp_dir) / "baseline.json"
            baseline.write_text(json.dumps(initial), encoding="utf-8")
            changed_coverage = {"src/demo.cpp": {10: 1, 11: 1, 12: 1, 13: 1, 20: 1, 21: 1}}
            report = build_crap_report(
                functions=self.functions,
                line_hits=changed_coverage,
                scope_selectors=["src/demo.cpp"],
                baseline_path=baseline,
            )
        demo = next(row for row in report["functions"] if row["name"] == "demo")
        self.assertEqual(demo["crap"], 5.0)
        self.assertEqual(demo["crap_delta"], -3.125)

    def test_lizard_csv_adapter(self) -> None:
        functions = parse_lizard_csv(
            (FIXTURES / "crap_lizard.csv").read_text(encoding="utf-8"), self.repo
        )
        self.assertEqual(functions[0].cyclomatic_complexity, 5)
        self.assertEqual(functions[0].start_line, 10)

    def test_lizard_native_headerless_csv_adapter(self) -> None:
        functions = parse_lizard_csv(
            '5,5,30,1,5,"demo@10-14@src/demo.cpp",'
            '"src/demo.cpp","demo","demo(int)",10,14\n',
            self.repo,
        )
        self.assertEqual(functions[0].path, "src/demo.cpp")
        self.assertEqual(functions[0].long_name, "demo(int)")
        self.assertEqual(functions[0].cyclomatic_complexity, 5)
        self.assertEqual(functions[0].start_line, 10)

    def test_unscoped_report_is_rejected(self) -> None:
        with self.assertRaises(CodeHealthError):
            build_crap_report(functions=self.functions, line_hits=self.coverage, scope_selectors=[])

    def test_malformed_complexity_entry_is_not_silently_dropped(self) -> None:
        with self.assertRaisesRegex(CodeHealthError, "must be an object"):
            parse_complexity_json({"schema_version": 1, "functions": [None]}, self.repo)

    def test_boolean_complexity_and_fractional_lines_are_rejected(self) -> None:
        base = {
            "path": "src/a.cpp",
            "name": "a",
            "long_name": "a()",
            "start_line": 1,
            "end_line": 2,
            "cyclomatic_complexity": 1,
        }
        for update in (
            {"start_line": 1.5},
            {"cyclomatic_complexity": True},
        ):
            payload = {"schema_version": 1, "functions": [{**base, **update}]}
            with self.subTest(update=update):
                with self.assertRaises(CodeHealthError):
                    parse_complexity_json(payload, self.repo)

    def test_non_finite_baseline_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            baseline = Path(temp_dir) / "baseline.json"
            baseline.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "functions": [{"fingerprint": "x", "crap": float("nan")}],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(CodeHealthError, "non-finite"):
                build_crap_report(
                    functions=self.functions,
                    line_hits=self.coverage,
                    scope_selectors=["src/demo.cpp"],
                    baseline_path=baseline,
                )

    def test_duplicate_report_fingerprint_is_rejected_before_reporting(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            first = Path(temp_dir) / "one.json"
            second = Path(temp_dir) / "two.json"
            template = {
                "schema_version": 1,
                "functions": [
                    {
                        "path": "src/demo.cpp",
                        "name": "demo",
                        "long_name": "demo(int)",
                        "start_line": 10,
                        "end_line": 20,
                        "cyclomatic_complexity": 2,
                    }
                ],
            }
            first.write_text(json.dumps(template), encoding="utf-8")
            template["functions"][0]["start_line"] = 30
            template["functions"][0]["end_line"] = 40
            second.write_text(json.dumps(template), encoding="utf-8")
            with self.assertRaisesRegex(CodeHealthError, "duplicate complexity function"):
                load_complexity([first, second], self.repo)


if __name__ == "__main__":
    unittest.main()
