import contextlib
import io
import json
import sys
import unittest
from unittest import mock
from decimal import Decimal
from pathlib import Path


TOOLS_STORAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_STORAGE))

from litchcraft_endurance import main, preset, simulate  # noqa: E402


class LitchcraftEnduranceTest(unittest.TestCase):
    def test_current_n8_is_only_an_unqualified_ideal_floor(self):
        report = simulate(preset("current-n8-floor", None))

        self.assertEqual(report.replacements, 73_000)
        self.assertEqual(report.logical_bytes, 21_178_176_000)
        self.assertEqual(report.body_sector_count, 400)
        self.assertEqual(report.maximum_sector_erases, 12_958)
        self.assertIsNone(report.guaranteed_pe_cycles)
        self.assertIsNone(report.endurance_margin)
        self.assertFalse(report.passes_two_x_margin)

    def test_future_plan_rotates_body_and_metadata_without_a_hot_sector(self):
        report = simulate(preset("future-32m-plan", None))

        self.assertEqual(report.body_sector_count, 6_144)
        self.assertEqual(report.metadata_sector_count, 64)
        self.assertEqual(report.maximum_sector_erases, 3_375)
        self.assertEqual(report.p95_sector_erases, 3_375)
        self.assertEqual(report.minimum_sector_erases, 70)
        self.assertGreater(report.endurance_margin, 29.6)
        self.assertTrue(report.passes_two_x_margin)

    def test_worse_measured_amplification_reduces_margin(self):
        report = simulate(
            preset("future-32m-plan", Decimal("8")),
        )

        self.assertEqual(report.maximum_sector_erases, 6_737)
        self.assertGreater(report.endurance_margin, 14.8)
        self.assertLess(report.endurance_margin, 14.9)

    def test_current_n8_can_bound_a_smaller_rotating_pool_without_claiming_endurance(self):
        output = io.StringIO()
        with (
            mock.patch.object(sys, "argv", [
                "litchcraft_endurance.py", "--preset", "current-n8-floor",
                "--rotating-body-bytes", "819200",
            ]),
            contextlib.redirect_stdout(output),
        ):
            self.assertEqual(main(), 0)
        report = json.loads(output.getvalue())
        self.assertEqual(report["body_sector_count"], 200)
        self.assertEqual(report["maximum_sector_erases"], 25_915)
        self.assertIsNone(report["endurance_margin"])


if __name__ == "__main__":
    unittest.main()
