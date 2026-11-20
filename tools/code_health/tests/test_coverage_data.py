from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest


CODE_HEALTH_DIR = Path(__file__).resolve().parents[1]
FIXTURES_DIR = Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(CODE_HEALTH_DIR))

import coverage_data  # noqa: E402


class CoverageDataTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repo_root = Path(self.temporary_directory.name) / "repo"
        self.repo_root.mkdir()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def fixture(self, name: str) -> str:
        text = (FIXTURES_DIR / name).read_text(encoding="utf-8")
        return text.replace(
            "__EXTERNAL_ROOT__",
            (Path(self.temporary_directory.name) / "external").as_posix(),
        )

    def test_parses_and_merges_line_function_and_branch_records(self) -> None:
        report = coverage_data.parse_lcov(
            self.fixture("coverage_mixed.info"),
            repo_root=self.repo_root,
            source_scopes=("src", "include"),
        )

        self.assertEqual(
            [item.path for item in report.files],
            ["include/only_lines.hpp", "src/alpha.cpp", "src/zeta.cpp"],
        )
        alpha = report.file("src/alpha.cpp")
        self.assertIsNotNone(alpha)
        assert alpha is not None
        self.assertEqual(
            [(line.line, line.count) for line in alpha.lines or ()],
            [(5, 9), (6, 1), (12, 1)],
        )
        self.assertEqual(
            [(function.name, function.line, function.end_line, function.count)
             for function in alpha.functions or ()],
            [("alpha", 5, None, 9), ("beta", 12, 18, 1)],
        )
        self.assertEqual(
            [(branch.block, branch.branch, branch.taken)
             for branch in alpha.branches or ()],
            [("1", "0", 4), ("1", "1", 2)],
        )
        zeta = report.file("src/zeta.cpp")
        assert zeta is not None
        self.assertEqual(zeta.functions[0].name, "zeta(int, int)")
        self.assertIsNone(zeta.branches[0].taken)

    def test_absent_record_kinds_are_unavailable_not_zero(self) -> None:
        report = coverage_data.parse_lcov(
            self.fixture("coverage_mixed.info"),
            repo_root=self.repo_root,
            source_scopes=("include",),
        )

        header = report.file("include/only_lines.hpp")
        assert header is not None
        self.assertTrue(header.summary.lines.available)
        self.assertEqual(header.summary.lines.percent, 100.0)
        self.assertIsNone(header.functions)
        self.assertIsNone(header.branches)
        self.assertEqual(
            header.summary.functions,
            coverage_data.MetricSummary.unavailable(),
        )
        self.assertEqual(
            header.summary.branches,
            coverage_data.MetricSummary.unavailable(),
        )

    def test_recorded_zero_is_available_zero_percent(self) -> None:
        report = coverage_data.parse_lcov(
            "SF:src/zero.cpp\nDA:4,0\nFN:4,zero\nFNDA:0,zero\n"
            "BRDA:4,0,0,-\nend_of_record\n",
            repo_root=self.repo_root,
        )

        source = report.files[0]
        self.assertEqual(source.summary.lines.percent, 0.0)
        self.assertEqual(source.summary.functions.percent, 0.0)
        self.assertEqual(source.summary.branches.percent, 0.0)
        self.assertTrue(source.summary.branches.available)
        self.assertIsNone(source.branches[0].taken)

    def test_scrubs_paths_filters_scopes_and_counts_dropped_sources(self) -> None:
        absolute_owned = self.repo_root / "src" / "owned.cpp"
        external = Path(self.temporary_directory.name) / "sdk" / "external.cpp"
        text = (
            f"SF:{absolute_owned}\nDA:2,1\nend_of_record\n"
            "SF:./include\\owned.hpp\nDA:3,1\nend_of_record\n"
            "SF:test/test_owned.cpp\nDA:1,1\nend_of_record\n"
            f"SF:{external}\nDA:1,1\nend_of_record\n"
        )

        report = coverage_data.parse_lcov(
            text,
            repo_root=self.repo_root,
            source_scopes=("src", "include"),
        )

        self.assertEqual(
            [item.path for item in report.files],
            ["include/owned.hpp", "src/owned.cpp"],
        )
        rendered = json.dumps(report.to_dict(), sort_keys=True)
        self.assertNotIn(str(self.repo_root), rendered)
        self.assertNotIn(str(external), rendered)
        self.assertEqual(report.dropped_source_count, 2)

    def test_flutter_relative_sources_use_app_as_source_base(self) -> None:
        app_root = self.repo_root / "app"
        app_root.mkdir()

        report = coverage_data.parse_lcov(
            self.fixture("coverage_flutter.info"),
            repo_root=self.repo_root,
            source_base=app_root,
            source_scopes=("app/lib",),
        )

        self.assertEqual(
            [item.path for item in report.files],
            ["app/lib/a_widget.dart", "app/lib/src/z_service.dart"],
        )
        self.assertEqual(report.summary.lines.covered, 2)
        self.assertEqual(report.summary.lines.total, 3)
        self.assertEqual(report.summary.branches.percent, 50.0)

    def test_output_is_deterministic_and_rehydrates(self) -> None:
        report = coverage_data.parse_lcov(
            self.fixture("coverage_mixed.info"),
            repo_root=self.repo_root,
            source_scopes=("src", "include"),
        )

        serialized = json.dumps(report.to_dict(), indent=2, sort_keys=True) + "\n"
        restored = coverage_data.coverage_report_from_dict(json.loads(serialized))

        self.assertEqual(restored.to_dict(), report.to_dict())
        self.assertEqual(
            report.line_hits_by_path()["src/alpha.cpp"],
            {5: 9, 6: 1, 12: 1},
        )

        normalized_lcov = coverage_data.render_lcov(report)
        self.assertNotIn(str(self.repo_root), normalized_lcov)
        self.assertIn("SF:src/alpha.cpp", normalized_lcov)
        self.assertIn("FN:12,18,beta", normalized_lcov)
        self.assertIn("FNDA:1,beta", normalized_lcov)
        self.assertIn("BRDA:6,1,0,4", normalized_lcov)
        reparsed = coverage_data.parse_lcov(
            normalized_lcov,
            repo_root=self.repo_root,
            source_scopes=("src", "include"),
        )
        self.assertEqual(reparsed.files, report.files)
        self.assertEqual(reparsed.summary, report.summary)
        self.assertEqual(reparsed.source_scopes, report.source_scopes)

    def test_merges_reports_without_converting_missing_metrics_to_zero(self) -> None:
        first = coverage_data.parse_lcov(
            "SF:src/a.cpp\nDA:2,1\nend_of_record\n",
            repo_root=self.repo_root,
            source_scopes=("src",),
        )
        second = coverage_data.parse_lcov(
            "SF:src/a.cpp\nDA:2,2\nBRDA:2,0,0,0\nend_of_record\n",
            repo_root=self.repo_root,
            source_scopes=("src",),
        )

        merged = coverage_data.merge_coverage_reports([first, second])

        source = merged.files[0]
        self.assertEqual(source.lines[0].count, 3)
        self.assertIsNone(source.functions)
        self.assertEqual(source.summary.branches.percent, 0.0)

    def test_rejects_malformed_records_and_escaping_scopes(self) -> None:
        with self.assertRaisesRegex(coverage_data.CoverageParseError, "malformed DA"):
            coverage_data.parse_lcov(
                "SF:src/a.cpp\nDA:1\nend_of_record\n",
                repo_root=self.repo_root,
            )
        with self.assertRaisesRegex(ValueError, "source scope"):
            coverage_data.parse_lcov(
                "",
                repo_root=self.repo_root,
                source_scopes=("../outside",),
            )


if __name__ == "__main__":
    unittest.main()
