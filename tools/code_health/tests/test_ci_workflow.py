from __future__ import annotations

import subprocess
import csv
import os
import shutil
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
RUNNER = REPO_ROOT / "tools" / "code_health" / "run_ci_bundle.sh"


class CodeHealthCiWorkflowTests(unittest.TestCase):
    def test_bundle_records_failed_lane_time_and_still_attempts_later_lanes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            shutil.copyfile(RUNNER, root / "bundle.sh")
            for name, body in (
                ("litch", 'case "$*" in *native-coverage*) exit 7;; esac\nexit 0\n'),
                ("lizard", "exit 0\n"),
            ):
                path = root / name
                path.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8")
                path.chmod(0o755)
            result = subprocess.run(
                ["bash", "bundle.sh"], cwd=root, capture_output=True, text=True,
                env={**os.environ, "PATH": str(root) + os.pathsep + os.environ["PATH"],
                     "CODE_HEALTH_BASE_REF": "HEAD", "GITHUB_SHA": "test-revision"},
                timeout=10,
            )
            with (root / "build/code_health/test-revision/lane-status.tsv").open() as stream:
                rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual([row["lane"] for row in rows], [
            "native-coverage", "flutter-coverage", "complexity", "crap",
            "static-analysis", "firmware-size", "runtime-headroom",
        ])
        self.assertEqual([row["exit_code"] for row in rows], ["7"] + ["0"] * 6)
        self.assertEqual([row["status"] for row in rows], ["failed"] + ["passed"] * 6)
        self.assertTrue(all(row["elapsed_seconds"].isdigit() for row in rows))

    def test_runner_is_valid_shell_and_attempts_every_report_lane(self) -> None:
        subprocess.run(["bash", "-n", str(RUNNER)], check=True)
        content = RUNNER.read_text(encoding="utf-8")

        self.assertIn("CODE_HEALTH_BASE_REF", content)
        self.assertIn("lane-status.tsv", content)
        self.assertIn("overall=1", content)
        for lane in (
            "native-coverage",
            "flutter-coverage",
            "complexity",
            "crap",
            "static-analysis",
            "firmware-size",
            "runtime-headroom",
        ):
            self.assertIn(f"run_lane {lane}", content)

        self.assertIn("--backend gcovr", content)
        self.assertIn("lizard -Ecpre -l cpp --csv", content)
        self.assertIn("--changed-from \"$base_ref\"", content)
        self.assertIn("--report-only", content)
        for forbidden in ("--fail-under", "--minimum", "--threshold"):
            self.assertNotIn(forbidden, content)

if __name__ == "__main__":
    unittest.main()
