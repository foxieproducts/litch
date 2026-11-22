from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch


CODE_HEALTH_DIR = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(CODE_HEALTH_DIR))

import firmware_size  # noqa: E402


class PlatformioSizeParserTests(unittest.TestCase):
    def test_ansi_log_is_scrubbed_and_metrics_are_target_scoped(self) -> None:
        fixture = (FIXTURES / "size_pio_ansi.txt").read_text(encoding="utf-8")
        ansi_log = fixture.replace("{ESC}", "\x1b")

        c3 = firmware_size.parse_platformio_size_log(ansi_log, target="c3")
        s3 = firmware_size.parse_platformio_size_log(ansi_log, target="s3")

        self.assertEqual(c3["environment"], "litch-esp32c3")
        self.assertEqual(c3["ram"]["used_bytes"], 163840)
        self.assertEqual(c3["flash"]["used_bytes"], 786432)
        self.assertEqual(c3["flash"]["capacity_bytes"], 1310720)
        self.assertEqual(s3["ram"]["used_bytes"], 181212)
        self.assertEqual(s3["ram"]["remaining_bytes"], 146468)
        self.assertEqual(s3["flash"]["used_bytes"], 988745)
        self.assertEqual(s3["flash"]["remaining_bytes"], 2353591)

    def test_absent_metric_is_null_with_a_specific_reason(self) -> None:
        fixture = (FIXTURES / "size_pio_flash_only.txt").read_text(encoding="utf-8")

        parsed = firmware_size.parse_platformio_size_log(fixture, target="litch-esp32s3")

        self.assertIsNone(parsed["ram"]["used_bytes"])
        self.assertIsNone(parsed["ram"]["capacity_bytes"])
        self.assertIn("RAM summary", parsed["ram"]["unavailable_reason"])
        self.assertEqual(parsed["flash"]["used_bytes"], 988745)
        self.assertIsNone(parsed["flash"]["unavailable_reason"])

    def test_unscoped_single_target_log_is_accepted_but_ambiguous_log_is_not(self) -> None:
        unscoped = "RAM: (used 10 bytes from 20 bytes)\nFlash: (used 30 bytes from 40 bytes)\n"
        parsed = firmware_size.parse_platformio_size_log(unscoped, target="s3")
        self.assertEqual(parsed["ram"]["used_bytes"], 10)

        combined = (FIXTURES / "size_pio_ansi.txt").read_text(encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "multiple environments"):
            firmware_size.parse_platformio_size_log(combined.replace("{ESC}", "\x1b"))

    def test_unscoped_metrics_are_not_borrowed_when_another_target_is_named(self) -> None:
        mixed = (
            "RAM: (used 10 bytes from 20 bytes)\n"
            "Processing unrelated-env (platform: example)\n"
            "Flash: (used 30 bytes from 40 bytes)\n"
        )

        parsed = firmware_size.parse_platformio_size_log(mixed, target="s3")

        self.assertIsNone(parsed["ram"]["used_bytes"])
        self.assertIsNone(parsed["flash"]["used_bytes"])

    def test_conflicting_repeated_unscoped_metrics_are_rejected(self) -> None:
        combined = (
            "RAM: (used 10 bytes from 20 bytes)\n"
            "RAM: (used 11 bytes from 20 bytes)\n"
        )
        with self.assertRaisesRegex(ValueError, "conflicting RAM"):
            firmware_size.parse_platformio_size_log(combined, target="s3")


class FirmwareSizeSnapshotTests(unittest.TestCase):
    def test_target_configuration_records_per_target_capacity_provenance(self) -> None:
        c3 = firmware_size.target_configuration("c3")
        s3 = firmware_size.target_configuration("s3")

        self.assertEqual(c3["physical_flash_bytes"], 4194304)
        self.assertEqual(c3["application_slot_bytes"], 1310720)
        self.assertEqual(c3["static_ram_bytes"], 327680)
        self.assertIn("implicit Arduino", c3["application_slot_provenance"]["selector"])
        self.assertEqual(s3["physical_flash_bytes"], 8388608)
        self.assertEqual(s3["application_slot_bytes"], 0x330000)
        self.assertEqual(s3["framework"], "arduino")
        self.assertEqual(s3["platform"], "platformio/espressif32@6.12.0")
        self.assertEqual(
            s3["application_slot_provenance"]["source"],
            "tools/partitions/partitions-8MB-ota-spiffs.csv",
        )

    def test_repository_capacity_constants_match_authoritative_files(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]

        validation = firmware_size.validate_repository_size_configuration(repo_root)

        self.assertEqual(validation["status"], "verified")
        self.assertTrue(validation["checks"])

    def test_s3_partition_map_is_aligned_contiguous_and_fills_flash(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        rows = {}
        for line in (
            repo_root / "tools/partitions/partitions-8MB-ota-spiffs.csv"
        ).read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            columns = [column.strip() for column in line.split(",")]
            rows[columns[0]] = (
                int(columns[3], 0),
                firmware_size._parse_capacity_bytes(columns[4]),
            )

        self.assertEqual(rows["ota_0"], (0x10000, 0x330000))
        self.assertEqual(rows["ota_1"], (0x340000, 0x330000))
        self.assertEqual(rows["spiffs"], (0x670000, 0x190000))
        for offset, size in rows.values():
            self.assertEqual(offset % 0x1000, 0)
            self.assertEqual(size % 0x1000, 0)
        self.assertEqual(rows["ota_0"][0] + rows["ota_0"][1], rows["ota_1"][0])
        self.assertEqual(rows["ota_1"][0] + rows["ota_1"][1], rows["spiffs"][0])
        self.assertEqual(rows["spiffs"][0] + rows["spiffs"][1], 0x800000)

    def test_repository_capacity_validation_fails_on_partition_drift(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory)
            (copied / "boards").mkdir()
            (copied / "tools/partitions").mkdir(parents=True)
            shutil.copy2(repo_root / "platformio.ini", copied / "platformio.ini")
            for name in ("litch_esp32c3.json", "litch_esp32s3.json"):
                shutil.copy2(repo_root / "boards" / name, copied / "boards" / name)
            source_partition = (
                repo_root / "tools/partitions/partitions-8MB-ota-spiffs.csv"
            )
            partition = copied / "tools/partitions/partitions-8MB-ota-spiffs.csv"
            partition.write_text(
                source_partition.read_text(encoding="utf-8").replace(
                    "ota_0,    0,    ota_0,   0x10000,  3264K,",
                    "ota_0,0,ota_0,0x10000,1024K,",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "partition.ota_0.size"):
                firmware_size.validate_repository_size_configuration(copied)

    def test_snapshot_hashes_exact_artifact_and_keeps_linked_usage_separate(self) -> None:
        artifact = FIXTURES / "size_current.bin"
        log = (FIXTURES / "size_pio_ansi.txt").read_text(encoding="utf-8").replace(
            "{ESC}", "\x1b"
        )

        snapshot = firmware_size.create_firmware_size_snapshot(
            target="s3",
            role="current",
            artifact_path=artifact,
            repository_root=CODE_HEALTH_DIR.parents[1],
            platformio_log_text=log,
            revision="abc123",
        )

        artifact_bytes = artifact.read_bytes()
        self.assertEqual(snapshot["artifact"]["size_bytes"], len(artifact_bytes))
        self.assertEqual(
            snapshot["artifact"]["sha256"], hashlib.sha256(artifact_bytes).hexdigest()
        )
        self.assertEqual(
            snapshot["artifact"]["application_slot_remaining_bytes"],
            0x330000 - len(artifact_bytes),
        )
        self.assertEqual(snapshot["linked_usage"]["flash"]["used_bytes"], 988745)
        self.assertEqual(
            snapshot["linked_usage"]["toolchain"]["platform"]["version"],
            "6.12.0",
        )
        self.assertEqual(
            snapshot["linked_usage"]["toolchain"]["toolchain_packages"][0]["name"],
            "toolchain-xtensa-esp32s3",
        )
        self.assertTrue(
            snapshot["linked_usage"]["flash"]["capacity_matches_configuration"]
        )
        self.assertEqual(snapshot["revision"], "abc123")

    def test_oversized_sparse_artifact_is_rejected_before_reading_or_hashing(
        self,
    ) -> None:
        slot_bytes = firmware_size.target_configuration("c3")[
            "application_slot_bytes"
        ]
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "oversized-firmware.bin"
            with artifact.open("wb") as stream:
                stream.truncate(slot_bytes + 1)

            with patch.object(
                firmware_size.os,
                "read",
                side_effect=AssertionError("oversized artifact must not be read"),
            ), patch.object(
                firmware_size.hashlib,
                "sha256",
                side_effect=AssertionError("oversized artifact must not be hashed"),
            ):
                with self.assertRaisesRegex(
                    ValueError,
                    rf"exceeds application slot capacity.*{slot_bytes + 1}.*{slot_bytes}",
                ):
                    firmware_size.create_firmware_size_snapshot(
                        target="c3",
                        role="baseline",
                        artifact_path=artifact,
                    )

    def test_artifact_growth_during_hashing_is_rejected(self) -> None:
        slot_bytes = firmware_size.target_configuration("c3")[
            "application_slot_bytes"
        ]
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "growing-firmware.bin"
            artifact.write_bytes(b"initial")
            real_sha256 = firmware_size.hashlib.sha256
            real_read = firmware_size.os.read
            read_sizes = []

            class RecordingDigest:
                def __init__(self) -> None:
                    self.inner = real_sha256()
                    self.bytes_hashed = 0
                    self.hexdigest_called = False

                def update(self, chunk) -> None:
                    self.bytes_hashed += len(chunk)
                    self.inner.update(chunk)

                def hexdigest(self) -> str:
                    self.hexdigest_called = True
                    return self.inner.hexdigest()

            digest = RecordingDigest()

            def grow_then_hash():
                with artifact.open("r+b") as stream:
                    stream.truncate(slot_bytes + 1)
                return digest

            def track_read(descriptor, size):
                read_sizes.append(size)
                return real_read(descriptor, size)

            with patch.object(
                firmware_size.os, "read", side_effect=track_read
            ), patch.object(
                firmware_size.hashlib, "sha256", side_effect=grow_then_hash
            ):
                with self.assertRaisesRegex(
                    ValueError,
                    rf"exceeds application slot capacity.*{slot_bytes + 1}.*"
                    rf"{slot_bytes}",
                ):
                    firmware_size.create_firmware_size_snapshot(
                        target="c3",
                        role="baseline",
                        artifact_path=artifact,
                    )

            self.assertLessEqual(digest.bytes_hashed, slot_bytes)
            self.assertFalse(digest.hexdigest_called)
            self.assertEqual(sum(read_sizes), slot_bytes + 1)

    def test_artifact_change_within_slot_is_rejected_before_digest_acceptance(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "changing-firmware.bin"
            artifact.write_bytes(b"initial")
            real_sha256 = firmware_size.hashlib.sha256

            class RecordingDigest:
                def __init__(self) -> None:
                    self.inner = real_sha256()
                    self.hexdigest_called = False

                def update(self, chunk) -> None:
                    self.inner.update(chunk)

                def hexdigest(self) -> str:
                    self.hexdigest_called = True
                    return self.inner.hexdigest()

            digest = RecordingDigest()

            def shrink_then_hash():
                artifact.write_bytes(b"x")
                return digest

            with patch.object(
                firmware_size.hashlib, "sha256", side_effect=shrink_then_hash
            ):
                with self.assertRaisesRegex(
                    ValueError, "firmware artifact changed while being read"
                ):
                    firmware_size.create_firmware_size_snapshot(
                        target="c3",
                        role="baseline",
                        artifact_path=artifact,
                    )

            self.assertFalse(digest.hexdigest_called)

    def test_non_regular_opened_descriptor_is_not_measured(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "firmware.pipe"
            os.mkfifo(artifact)

            with patch.object(
                firmware_size.hashlib,
                "sha256",
                side_effect=AssertionError("non-regular artifact must not be hashed"),
            ):
                snapshot = firmware_size.create_firmware_size_snapshot(
                    target="c3",
                    role="baseline",
                    artifact_path=artifact,
                )

            self.assertIsNone(snapshot["artifact"]["size_bytes"])
            self.assertIsNone(snapshot["artifact"]["sha256"])
            self.assertEqual(
                snapshot["artifact"]["unavailable_reason"],
                "firmware artifact is not a regular file",
            )

    def test_missing_log_and_artifact_are_reported_without_zero_substitution(self) -> None:
        snapshot = firmware_size.create_firmware_size_snapshot(
            target="c3",
            role="baseline",
            artifact_path=FIXTURES / "size_missing.bin",
        )

        self.assertIsNone(snapshot["artifact"]["size_bytes"])
        self.assertEqual(snapshot["artifact"]["unavailable_reason"], "firmware artifact not found")
        self.assertIsNone(snapshot["linked_usage"]["ram"]["used_bytes"])
        self.assertEqual(
            snapshot["linked_usage"]["ram"]["unavailable_reason"],
            "PlatformIO log not supplied",
        )
        self.assertEqual(snapshot["linked_usage"]["toolchain"]["status"], "unavailable")

    def test_strict_snapshot_rejects_measured_linked_capacity_mismatch(self) -> None:
        snapshot = firmware_size.create_firmware_size_snapshot(
            target="s3",
            role="current",
            artifact_path=FIXTURES / "size_current.bin",
            platformio_log_text=(
                "RAM: (used 100 bytes from 327680 bytes)\n"
                "Flash: (used 1000 bytes from 2000000 bytes)\n"
            ),
        )

        with self.assertRaisesRegex(ValueError, "linked flash capacity"):
            firmware_size.validate_measured_linked_capacities(snapshot)

    def test_strict_snapshot_rejects_measured_ram_capacity_mismatch(self) -> None:
        snapshot = firmware_size.create_firmware_size_snapshot(
            target="c3",
            role="current",
            artifact_path=FIXTURES / "size_current.bin",
            platformio_log_text=(
                "RAM: (used 100 bytes from 300000 bytes)\n"
                "Flash: (used 1000 bytes from 1310720 bytes)\n"
            ),
        )

        with self.assertRaisesRegex(ValueError, "linked ram capacity"):
            firmware_size.validate_measured_linked_capacities(snapshot)

    def test_strict_snapshot_accepts_matching_measured_capacities(self) -> None:
        snapshot = firmware_size.create_firmware_size_snapshot(
            target="c3",
            role="current",
            artifact_path=FIXTURES / "size_current.bin",
            platformio_log_text=(
                "RAM: (used 100 bytes from 327680 bytes)\n"
                "Flash: (used 1000 bytes from 1310720 bytes)\n"
            ),
        )

        firmware_size.validate_measured_linked_capacities(snapshot)

    def test_non_strict_import_retains_capacity_mismatch_as_evidence(self) -> None:
        snapshot = firmware_size.create_firmware_size_snapshot(
            target="s3",
            role="current",
            artifact_path=FIXTURES / "size_current.bin",
            platformio_log_text=(
                "RAM: (used 100 bytes from 327680 bytes)\n"
                "Flash: (used 1000 bytes from 2000000 bytes)\n"
            ),
        )

        self.assertFalse(
            snapshot["linked_usage"]["flash"]["capacity_matches_configuration"]
        )

    def test_paths_are_normalized_and_external_host_directories_are_scrubbed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repository = Path(temporary_directory) / "checkout"
            artifact = repository / ".pio" / "build" / "litch-esp32s3" / "firmware.bin"
            artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b"firmware")
            external_log = Path(temporary_directory) / "private" / "build-output.log"
            external_log.parent.mkdir()
            external_log.write_text(
                "RAM: (used 1 bytes from 327680 bytes)\n"
                "Flash: (used 2 bytes from 3342336 bytes)\n",
                encoding="utf-8",
            )

            snapshot = firmware_size.create_firmware_size_snapshot(
                target="s3",
                role="current",
                artifact_path=artifact,
                repository_root=repository,
                platformio_log_path=external_log,
            )
            rendered = firmware_size.render_json(snapshot)

        self.assertEqual(
            snapshot["artifact"]["path"],
            ".pio/build/litch-esp32s3/firmware.bin",
        )
        self.assertEqual(snapshot["linked_usage"]["source"]["path"], "build-output.log")
        self.assertNotIn(temporary_directory, rendered)
        self.assertEqual(
            firmware_size.scrub_path(r"C:\\Users\\person\\firmware.bin"),
            "firmware.bin",
        )

    def test_rendered_json_is_deterministic_and_sorted(self) -> None:
        snapshot = firmware_size.create_firmware_size_snapshot(
            target="s3",
            role="baseline",
            artifact_path=FIXTURES / "size_baseline.bin",
        )

        first = firmware_size.render_json(snapshot)
        second = firmware_size.render_json(snapshot)

        self.assertEqual(first, second)
        self.assertTrue(first.endswith("\n"))
        self.assertEqual(json.loads(first), snapshot)


class FirmwareSizeComparisonTests(unittest.TestCase):
    def _snapshot(self, target: str, role: str, artifact: str, log: str | None):
        return firmware_size.create_firmware_size_snapshot(
            target=target,
            role=role,
            artifact_path=FIXTURES / artifact,
            repository_root=CODE_HEALTH_DIR.parents[1],
            platformio_log_text=log,
            revision=f"{role}-{target}",
        )

    def test_per_target_delta_math_uses_current_minus_baseline(self) -> None:
        baseline_log = (
            "RAM: (used 100 bytes from 327680 bytes)\n"
            "Flash: (used 1000 bytes from {flash_capacity} bytes)\n"
        )
        current_log = (
            "RAM: (used 125 bytes from 327680 bytes)\n"
            "Flash: (used 1040 bytes from {flash_capacity} bytes)\n"
        )

        for target, flash_capacity in (("c3", 1310720), ("s3", 0x330000)):
            with self.subTest(target=target):
                baseline = self._snapshot(
                    target,
                    "baseline",
                    "size_baseline.bin",
                    baseline_log.format(flash_capacity=flash_capacity),
                )
                current = self._snapshot(
                    target,
                    "current",
                    "size_current.bin",
                    current_log.format(flash_capacity=flash_capacity),
                )

                comparison = firmware_size.compare_firmware_size_snapshots(baseline, current)
                artifact_delta = (
                    len((FIXTURES / "size_current.bin").read_bytes())
                    - len((FIXTURES / "size_baseline.bin").read_bytes())
                )

                self.assertEqual(comparison["target"], f"litch-esp32{target}")
                self.assertEqual(
                    comparison["deltas"]["artifact_size_bytes"]["delta_bytes"],
                    artifact_delta,
                )
                self.assertEqual(
                    comparison["deltas"]["linked_ram_used_bytes"]["delta_bytes"], 25
                )
                self.assertEqual(
                    comparison["deltas"]["linked_flash_used_bytes"]["delta_bytes"], 40
                )
                self.assertEqual(
                    comparison["deltas"]["application_slot_remaining_bytes"]["delta_bytes"],
                    -artifact_delta,
                )
                self.assertIsNone(
                    comparison["deltas"]["linked_flash_used_bytes"]["unavailable_reason"]
                )

    def test_absent_baseline_metric_produces_null_delta_and_reason(self) -> None:
        baseline = self._snapshot("s3", "baseline", "size_baseline.bin", None)
        current = self._snapshot(
            "s3",
            "current",
            "size_current.bin",
            "RAM: (used 1 bytes from 327680 bytes)\n",
        )

        comparison = firmware_size.compare_firmware_size_snapshots(baseline, current)
        delta = comparison["deltas"]["linked_ram_used_bytes"]

        self.assertIsNone(delta["baseline_bytes"])
        self.assertEqual(delta["current_bytes"], 1)
        self.assertIsNone(delta["delta_bytes"])
        self.assertIn("baseline unavailable", delta["unavailable_reason"])

    def test_different_targets_cannot_be_compared(self) -> None:
        baseline = self._snapshot("c3", "baseline", "size_baseline.bin", None)
        current = self._snapshot("s3", "current", "size_current.bin", None)

        with self.assertRaisesRegex(ValueError, "different firmware targets"):
            firmware_size.compare_firmware_size_snapshots(baseline, current)

    def test_boolean_byte_metric_is_rejected(self) -> None:
        baseline = self._snapshot("c3", "baseline", "size_baseline.bin", None)
        current = self._snapshot("c3", "current", "size_current.bin", None)
        baseline["artifact"]["size_bytes"] = True

        with self.assertRaisesRegex(ValueError, "must be an integer"):
            firmware_size.compare_firmware_size_snapshots(baseline, current)

    def test_truncated_snapshot_shape_is_rejected(self) -> None:
        baseline = self._snapshot("c3", "baseline", "size_baseline.bin", None)
        current = self._snapshot(
            "c3",
            "current",
            "size_current.bin",
            "RAM: (used 100 bytes from 327680 bytes)\n"
            "Flash: (used 1000 bytes from 1310720 bytes)\n",
        )
        del current["linked_usage"]["flash"]["remaining_bytes"]

        with self.assertRaisesRegex(ValueError, "measurement is incomplete"):
            firmware_size.compare_firmware_size_snapshots(baseline, current)

    def test_measured_artifact_larger_than_slot_is_rejected(self) -> None:
        snapshot = self._snapshot(
            "c3", "baseline", "size_baseline.bin", "size_pio_c3.txt"
        )
        slot_bytes = snapshot["configuration"]["application_slot_bytes"]
        snapshot["artifact"]["size_bytes"] = slot_bytes + 1
        snapshot["artifact"]["application_slot_remaining_bytes"] = -1

        with self.assertRaisesRegex(ValueError, "exceeds application slot capacity"):
            firmware_size.validate_firmware_size_snapshot(snapshot)


if __name__ == "__main__":
    unittest.main()
