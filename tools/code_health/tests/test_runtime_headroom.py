from __future__ import annotations

import io
import json
import sys
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
FIXTURES = TEST_DIR / "fixtures"
REPO_ROOT = TEST_DIR.parents[2]
sys.path.insert(0, str(TEST_DIR.parent))

import runtime_headroom as runtime  # noqa: E402


def load_json(name: str):
    with (FIXTURES / name).open("r", encoding="utf-8") as stream:
        return json.load(stream)


class RuntimeHeadroomTest(unittest.TestCase):
    def parse_serial(self, fixture: str, *, kind: str = "hardware", target: str = "s3"):
        serial = load_json(fixture)["serial"].encode("utf-8")
        return runtime.parse_stream(
            io.BytesIO(serial),
            evidence_kind=kind,
            target=target,
            source_path=FIXTURES / fixture,
            repo_root=REPO_ROOT,
        )

    def test_crlf_logger_prefixes_and_existing_record_families(self):
        report = self.parse_serial("runtime_hardware_serial.json")
        facts = report["facts"]

        self.assertEqual(report["target"], "s3")
        self.assertEqual(report["evidence"]["kind"], "hardware")
        self.assertEqual(
            report["evidence"]["path"],
            "tools/code_health/tests/fixtures/runtime_hardware_serial.json",
        )
        self.assertTrue(report["evidence"]["present"])
        self.assertEqual(report["hardware_in_loop"]["status"], "measured")
        self.assertTrue(report["hardware_in_loop"]["present"])

        self.assertEqual(facts["runtime.loop.max_delta_ms"]["value"], 17)
        self.assertEqual(facts["runtime.loop.max_delta_ms"]["status"], "measured")
        self.assertFalse(facts["runtime.loop.max_delta_ms"]["diagnostic_only"])
        self.assertEqual(facts["runtime.i2c.errors"]["value"], 2)
        self.assertEqual(facts["memory.heap.free_bytes"]["value"], 207680)
        self.assertEqual(facts["memory.heap_caps.internal.largest_free_block_bytes"]["value"], 90000)
        self.assertEqual(facts["memory.heap_caps.spiram.free_blocks"]["value"], 3)
        self.assertEqual(facts["storage.flash.size_bytes"]["status"], "configured")
        self.assertEqual(facts["storage.sketch.free_bytes"]["value"], 1800000)
        self.assertEqual(facts["memory.psram.free_bytes"]["value"], 7000000)

        self.assertEqual(facts["memory.stack.current_task.high_water_words"]["value"], 321)
        self.assertEqual(facts["memory.stack.current_task.high_water_words"]["status"], "measured")
        self.assertEqual(facts["memory.stack.all_tasks.minimum_high_water_words"]["status"],
                         "unavailable")
        self.assertIsNone(facts["memory.stack.all_tasks.minimum_high_water_words"]["value"])

        self.assertEqual(facts["ble.ota.write.max_ms"]["value"], 6)
        self.assertEqual(facts["ble.ota.write.stage_capacity_bytes"]["status"], "configured")
        self.assertEqual(facts["ble.ota.queue.high_water_chunks"]["value"], 7)
        self.assertEqual(facts["ble.ota.queue.high_water_chunks"]["scope"], "queue_lifetime")
        self.assertEqual(facts["ble.ota.notify.max_ms"]["value"], 4)
        self.assertEqual(facts["ble.bench_notify.size_100.rate_bytes_per_second"]["value"], 398000)

        self.assertEqual(facts["power.model.baseline_ma"]["status"], "configured")
        self.assertEqual(facts["power.model.total_ma"]["status"], "modeled")
        self.assertEqual(facts["power.usb_cc.raw1"]["status"], "measured")
        self.assertEqual(facts["power.usb_cc.monitor_interval_ms"]["status"], "configured")
        self.assertEqual(facts["power.hil.calibration.core_ma"]["value"], 118)
        self.assertEqual(facts["power.hil.calibration.ambient_ma"]["status"], "deferred_hil")
        self.assertEqual(facts["power.model.calibration.red.fullscale_ma"]["status"], "modeled")
        self.assertEqual(facts["power.hil.verify.core_baseline.actual_total_ma"]["value"], 121)
        self.assertEqual(facts["power.hil.verify.neutral_over_limit.actual_total_ma"]["status"],
                         "deferred_hil")

        self.assertIsNone(facts["runtime.frames.dropped"]["value"])
        self.assertEqual(facts["runtime.frames.dropped"]["reported_value"], 0)
        self.assertEqual(facts["runtime.frames.dropped"]["status"], "unavailable")
        self.assertNotIn("verdict", report)
        for fact in facts.values():
            self.assertIn(fact["status"], runtime.FACT_STATUSES)
            self.assertIn("provenance", fact)

    def test_missing_hil_and_expected_target_are_explicit(self):
        report = self.parse_serial("runtime_missing_hil.json", kind="unknown", target="c3")
        facts = report["facts"]

        self.assertFalse(report["evidence"]["present"])
        self.assertEqual(report["hardware_in_loop"]["status"], "deferred_hil")
        self.assertEqual(facts["runtime.loop.max_delta_ms"]["status"], "deferred_hil")
        self.assertEqual(facts["memory.heap.free_bytes"]["status"], "deferred_hil")
        self.assertEqual(facts["power.actual_total_ma"]["status"], "deferred_hil")
        self.assertEqual(facts["ble.runtime.queue.high_water_events"]["status"], "unavailable")

        bundle = runtime.combine_reports([report])
        self.assertEqual(list(bundle["targets"]), ["c3", "s3"])
        self.assertEqual(bundle["targets"]["c3"]["status"], "unavailable")
        self.assertEqual(bundle["targets"]["s3"]["status"], "unavailable")
        self.assertEqual(
            bundle["targets"]["s3"]["hardware_in_loop"]["status"],
            "deferred_hil",
        )
        self.assertEqual(bundle["targets"]["s3"]["reports"], [])

    def test_unwired_frame_stack_and_generic_queue_do_not_become_zero(self):
        report = self.parse_serial("runtime_unwired_zero.json")
        facts = report["facts"]

        self.assertEqual(facts["runtime.loop.count"]["value"], 10)
        self.assertEqual(facts["runtime.frames.dropped"]["reported_value"], 0)
        self.assertIsNone(facts["runtime.frames.dropped"]["value"])
        self.assertEqual(facts["runtime.frames.dropped"]["status"], "unavailable")
        self.assertIsNone(facts["memory.stack.current_task.high_water_words"]["value"])
        self.assertIsNone(facts["memory.stack.all_tasks.minimum_high_water_words"]["value"])
        self.assertIsNone(facts["ble.runtime.queue.high_water_events"]["value"])
        self.assertIsNone(facts["ble.ota.queue.high_water_chunks"]["value"])
        self.assertEqual(facts["ble.ota.queue.high_water_chunks"]["status"], "deferred_hil")

    def test_simulated_observations_keep_non_hardware_provenance(self):
        report = self.parse_serial("runtime_unwired_zero.json", kind="simulated")
        fact = report["facts"]["runtime.loop.max_delta_ms"]

        self.assertEqual(fact["status"], "measured")
        self.assertEqual(fact["value"], 3)
        self.assertEqual(fact["provenance"]["evidence_kind"], "simulated")
        self.assertEqual(report["hardware_in_loop"]["status"], "unavailable")
        self.assertIn("simulated_observations_are_not_hardware_measurements", report["limitations"])

    def test_zero_notify_count_does_not_create_zero_timing_observation(self):
        serial = (
            b"BLE OTA path metrics: status=write_failed enqueue_count=0 enqueue_drop=0 "
            b"queue_highwater=0 drain_cycles=0 drain_count=0 drain_max_chunks=0 "
            b"notify_count=0 notify_avg_ms=0 notify_max_ms=0\r\n"
        )
        report = runtime.parse_stream(io.BytesIO(serial), evidence_kind="hardware", target="s3")
        facts = report["facts"]

        self.assertEqual(facts["ble.ota.queue.high_water_chunks"]["value"], 0)
        self.assertEqual(facts["ble.ota.queue.high_water_chunks"]["status"], "measured")
        self.assertEqual(facts["ble.ota.notify.count"]["value"], 0)
        self.assertIsNone(facts["ble.ota.notify.avg_ms"]["value"])
        self.assertEqual(facts["ble.ota.notify.avg_ms"]["reported_value"], 0)
        self.assertEqual(facts["ble.ota.notify.max_ms"]["status"], "unavailable")

    def test_fake_snapshot_reports_modeled_power_and_ignores_unsupported_zeros(self):
        fixture = FIXTURES / "runtime_fake_snapshot.json"
        report = runtime.parse_file(
            fixture,
            evidence_kind="fake",
            target="c3",
            repository_root=REPO_ROOT,
        )
        facts = report["facts"]

        self.assertEqual(facts["power.fake.estimated_led_ma"]["value"], 820)
        self.assertEqual(facts["power.fake.estimated_led_ma"]["status"], "modeled")
        self.assertEqual(facts["power.fake.limited"]["value"], True)
        self.assertEqual(facts["power.fake.limited"]["provenance"]["method"],
                         "virtual_litch_power_model")
        self.assertEqual(facts["runtime.health.revision"]["value"], 1)
        self.assertEqual(facts["runtime.health.state"]["value"], 2)
        self.assertEqual(facts["runtime.health.causes"]["value"], 4)
        self.assertEqual(facts["runtime.health.state"]["status"], "modeled")
        self.assertIsNone(facts["memory.heap.free_bytes"]["value"])
        self.assertEqual(facts["memory.heap.free_bytes"]["status"], "unavailable")
        self.assertIsNone(facts["memory.stack.current_task.high_water_words"]["value"])
        self.assertIsNone(facts["ble.ota.queue.high_water_chunks"]["value"])
        self.assertIsNone(facts["runtime.frames.dropped"]["reported_value"])
        self.assertIn("unsupported_fake_snapshot_runtime_fields_ignored", report["limitations"])
        self.assertEqual(report["hardware_in_loop"]["status"], "unavailable")
        self.assertEqual(runtime.combine_reports([report])["targets"]["c3"]["status"], "modeled")

    def test_hardware_health_accepts_decimal_and_prefixed_lines(self):
        health_serial = load_json("runtime_hardware_health.json")["serial"]
        existing_serial = load_json("runtime_hardware_serial.json")["serial"]
        report = runtime.parse_stream(
            io.BytesIO((health_serial + existing_serial).encode("utf-8")),
            evidence_kind="hardware",
            target="s3",
            source_path=FIXTURES / "runtime_hardware_health.json",
            repo_root=REPO_ROOT,
        )
        facts = report["facts"]
        self.assertEqual(facts["runtime.loop.max_delta_ms"]["value"], 17)
        self.assertEqual(
            {name: facts[name]["value"] for name in (
                "runtime.health.revision", "runtime.health.state", "runtime.health.causes"
            )},
            {"runtime.health.revision": 1, "runtime.health.state": 2,
             "runtime.health.causes": 4},
        )
        decimal = self.parse_serial("runtime_hardware_health_decimal.json")
        decimal_facts = decimal["facts"]
        self.assertEqual(decimal_facts["runtime.health.revision"]["value"], 17)
        self.assertEqual(decimal_facts["runtime.health.state"]["value"], 255)
        self.assertEqual(decimal_facts["runtime.health.causes"]["value"], 65535)

    def test_missing_hardware_health_stays_deferred(self):
        report = self.parse_serial("runtime_unwired_zero.json")
        for name in ("runtime.health.revision", "runtime.health.state", "runtime.health.causes"):
            self.assertIsNone(report["facts"][name]["value"])
            self.assertEqual(report["facts"][name]["status"], "deferred_hil")

    def test_fake_health_requires_one_complete_non_boolean_bounded_object(self):
        cases = load_json("runtime_fake_health_cases.json")
        for name, snapshot in cases.items():
            with self.subTest(name=name):
                report = runtime.parse_fake_snapshot(snapshot, target="c3")
                facts = report["facts"]
                self.assertEqual(
                    [facts[key]["value"] for key in (
                        "runtime.health.revision", "runtime.health.state", "runtime.health.causes"
                    )],
                    [65535, 255, 65535] if name == "unknown_values" else [None, None, None],
                )
                if name == "unknown_values":
                    self.assertEqual(facts["runtime.health.state"]["status"], "modeled")
                else:
                    self.assertEqual(facts["runtime.health.state"]["status"], "unavailable")

    def test_health_facts_are_deterministic(self):
        fixture = load_json("runtime_fake_snapshot.json")
        first = runtime.parse_fake_snapshot(fixture, target="c3")
        second = runtime.parse_fake_snapshot(fixture, target="c3")
        self.assertEqual(
            json.dumps(first, sort_keys=True, separators=(",", ":")),
            json.dumps(second, sort_keys=True, separators=(",", ":")),
        )

    def test_trace_reports_scenario_counts_but_not_hardware_headroom(self):
        report = runtime.parse_file(
            FIXTURES / "runtime_trace.jsonl",
            evidence_kind="trace",
            target="s3",
            repo_root=REPO_ROOT,
        )
        facts = report["facts"]

        self.assertEqual(facts["trace.format_version"]["status"], "configured")
        self.assertEqual(facts["trace.scenario_time_ms"]["value"], 25)
        self.assertEqual(facts["trace.scenario_time_ms"]["status"], "modeled")
        self.assertEqual(facts["trace.frames_records"]["value"], 1)
        self.assertIsNone(facts["runtime.loop.max_delta_ms"]["value"])
        self.assertEqual(facts["runtime.loop.max_delta_ms"]["status"], "unavailable")
        self.assertIsNone(facts["memory.heap.free_bytes"]["value"])
        self.assertIn("trace_time_is_scenario_time_not_hardware_latency", report["limitations"])

    def test_stream_and_retained_evidence_are_bounded(self):
        line = b"loops=1 last_dt_ms=1 min_dt_ms=1 max_dt_ms=1\n"
        report = runtime.parse_stream(
            io.BytesIO(line * 5),
            evidence_kind="hardware",
            target="s3",
            limits={"max_records": 2},
        )
        evidence = report["evidence"]
        self.assertEqual(evidence["matched_records"], 5)
        self.assertEqual(evidence["records_retained"], 2)
        self.assertEqual(evidence["records_dropped"], 3)

        truncated = runtime.parse_stream(
            io.BytesIO(line * 5),
            evidence_kind="hardware",
            limits={"max_bytes": 20},
        )
        self.assertTrue(truncated["evidence"]["input_truncated"])
        self.assertLessEqual(truncated["evidence"]["bytes_read"], 20)

    def test_truncated_input_retains_facts_as_diagnostic_only(self):
        line = b"loops=1 last_dt_ms=1 min_dt_ms=1 max_dt_ms=1\n"
        report = runtime.parse_stream(
            io.BytesIO(line + b"unread trailing evidence\n"),
            evidence_kind="hardware",
            target="s3",
            limits={"max_bytes": len(line)},
        )

        self.assertEqual(report["status"], "incomplete")
        self.assertFalse(report["measurement_eligible"])
        self.assertEqual(report["evidence"]["status"], "incomplete")
        self.assertEqual(report["evidence"]["incomplete_reasons"], ["input_truncated"])
        self.assertTrue(report["evidence"]["diagnostic_only"])
        self.assertEqual(report["facts"]["runtime.loop.count"]["value"], 1)
        self.assertEqual(report["facts"]["runtime.loop.count"]["status"], "measured")
        self.assertTrue(report["facts"]["runtime.loop.count"]["diagnostic_only"])
        self.assertEqual(report["hardware_in_loop"]["status"], "unavailable")
        self.assertFalse(report["hardware_in_loop"]["present"])

    def test_overlong_line_retains_facts_as_diagnostic_only(self):
        record = b"loops=1 last_dt_ms=1 min_dt_ms=1 max_dt_ms=1"
        report = runtime.parse_stream(
            io.BytesIO(record + b" " * 32 + b"\n"),
            evidence_kind="hardware",
            target="s3",
            limits={"max_line_chars": len(record)},
        )

        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["evidence"]["overlong_lines"], 1)
        self.assertEqual(report["evidence"]["incomplete_reasons"], ["overlong_lines"])
        self.assertEqual(report["facts"]["runtime.loop.max_delta_ms"]["value"], 1)
        self.assertTrue(report["facts"]["runtime.loop.max_delta_ms"]["diagnostic_only"])
        self.assertEqual(report["hardware_in_loop"]["status"], "unavailable")

    def test_overlong_partial_numeric_token_does_not_abort_diagnostic_report(self):
        partial = b"loops=1 last_dt_ms=1 min_dt_ms=1 max_dt_ms=-"
        report = runtime.parse_stream(
            io.BytesIO(partial + b"123456789\n"),
            evidence_kind="hardware",
            target="s3",
            limits={"max_line_chars": len(partial)},
        )

        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["evidence"]["incomplete_reasons"], ["overlong_lines"])
        self.assertEqual(report["evidence"]["matched_records"], 0)
        self.assertEqual(report["facts"]["runtime.loop.count"]["value"], 1)
        self.assertTrue(report["facts"]["runtime.loop.count"]["diagnostic_only"])
        self.assertEqual(report["hardware_in_loop"]["status"], "unavailable")

    def test_incomplete_report_makes_bundle_incomplete_and_target_unavailable(self):
        record = b"loops=1 last_dt_ms=1 min_dt_ms=1 max_dt_ms=1"
        reports = {
            "overlong": runtime.parse_stream(
                io.BytesIO(record + b" trailing data beyond the line limit\n"),
                evidence_kind="hardware",
                target="s3",
                limits={"max_line_chars": len(record)},
            ),
            "truncated": runtime.parse_stream(
                io.BytesIO(record + b"\ntrailing evidence\n"),
                evidence_kind="hardware",
                target="s3",
                limits={"max_bytes": len(record) + 1},
            ),
        }

        for name, report in reports.items():
            with self.subTest(name=name):
                bundle = runtime.combine_reports([report])

                self.assertEqual(bundle["status"], "incomplete")
                self.assertFalse(bundle["measurement_eligible"])
                self.assertEqual(bundle["incomplete_targets"], ["s3"])
                self.assertEqual(bundle["targets"]["s3"]["evidence_status"], "incomplete")
                self.assertEqual(bundle["targets"]["s3"]["status"], "unavailable")
                self.assertEqual(
                    bundle["targets"]["s3"]["hardware_in_loop"]["status"],
                    "unavailable",
                )
                self.assertFalse(bundle["targets"]["s3"]["hardware_in_loop"]["present"])

    def test_combining_order_and_json_are_deterministic(self):
        hardware = self.parse_serial("runtime_unwired_zero.json", target="s3")
        fake = runtime.parse_fake_snapshot(load_json("runtime_fake_snapshot.json"), target="c3")
        first = runtime.combine_reports([hardware, fake])
        second = runtime.combine_reports([fake, hardware])

        self.assertEqual(
            json.dumps(first, sort_keys=True, separators=(",", ":")),
            json.dumps(second, sort_keys=True, separators=(",", ":")),
        )
        self.assertEqual(first["targets"]["s3"]["status"], "measured")
        self.assertEqual(first["targets"]["c3"]["status"], "modeled")
        self.assertEqual(first["status"], "complete")
        self.assertTrue(first["measurement_eligible"])

    def test_path_scrubbing_never_exposes_external_absolute_path(self):
        self.assertEqual(
            runtime.scrub_path("/private/secret/runtime.log", REPO_ROOT),
            "<external>/runtime.log",
        )
        self.assertEqual(runtime.scrub_path("../../runtime.log", REPO_ROOT), "runtime.log")


if __name__ == "__main__":
    unittest.main()
