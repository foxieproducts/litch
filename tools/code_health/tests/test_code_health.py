from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


CODE_HEALTH_DIR = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
sys.path.insert(0, str(CODE_HEALTH_DIR))

import code_health  # noqa: E402
from common import (  # noqa: E402
    CodeHealthError,
    CommandResult,
    expand_repo_paths,
    run_bounded,
    scrub_text,
)


class CodeHealthCliTests(unittest.TestCase):
    def test_entrypoint_rejects_unsupported_host_before_parsing_or_work(self) -> None:
        error = code_health.UnsupportedHostError(
            "unsupported host (windows): code-health supports only native macOS and native Linux"
        )
        stderr = io.StringIO()
        with patch.object(
            code_health, "require_supported_host", side_effect=error
        ), patch.object(code_health, "_parser") as parser, redirect_stderr(stderr):
            returncode = code_health.main(["runtime-headroom"])

        self.assertEqual(returncode, 2)
        parser.assert_not_called()
        self.assertEqual(
            stderr.getvalue(),
            "code-health: unsupported host (windows): code-health supports only "
            "native macOS and native Linux\n",
        )

    def test_crap_parser_accepts_explicit_complexity_version_artifact(self) -> None:
        args = code_health._parser().parse_args(
            [
                "crap",
                "--coverage",
                "coverage.info",
                "--complexity",
                "complexity.csv",
                "--complexity-version",
                "lizard.version.txt",
                "--path",
                "src/example.cpp",
            ]
        )

        self.assertEqual(args.complexity_version, "lizard.version.txt")

    def _snapshot_args(self, **overrides):
        values = {
            "build": False,
            "role": "baseline",
            "revision": "base123",
            "s3_artifact": str(FIXTURES / "size_baseline.bin"),
            "c3_artifact": str(FIXTURES / "size_baseline.bin"),
            "s3_log": None,
            "c3_log": None,
            "legacy_c3": True,
            "pio_core_dir": None,
            "label": "main-before-feature",
            "profile_label": "repository-default",
            "repo_root": None,
            "output_dir": None,
            "require_artifacts": False,
        }
        values.update(overrides)
        return argparse.Namespace(**values)

    def test_imported_legacy_baseline_requires_declared_revision_and_both_artifacts(self) -> None:
        repository = {"revision": "head123", "dirty": False}
        with self.assertRaisesRegex(CodeHealthError, "explicit --revision"):
            code_health._size_snapshot_identity(
                self._snapshot_args(revision=None), repository
            )
        with self.assertRaisesRegex(CodeHealthError, "explicit --c3-artifact"):
            code_health._size_snapshot_identity(
                self._snapshot_args(c3_artifact=None), repository
            )

        revision, provenance, label = code_health._size_snapshot_identity(
            self._snapshot_args(), repository
        )
        self.assertEqual(revision, "base123")
        self.assertEqual(provenance, "declared_for_imported_artifacts")
        self.assertEqual(label, "main-before-feature")

    def test_build_cannot_claim_another_revision_or_baseline_role(self) -> None:
        repository = {"revision": "head123", "dirty": False}
        with self.assertRaisesRegex(CodeHealthError, "current snapshot"):
            code_health._size_snapshot_identity(
                self._snapshot_args(
                    build=True,
                    role="baseline",
                    revision=None,
                    s3_artifact=None,
                    c3_artifact=None,
                    s3_log=None,
                    c3_log=None,
                ),
                repository,
            )
        with self.assertRaisesRegex(CodeHealthError, "match the checked-out HEAD"):
            code_health._size_snapshot_identity(
                self._snapshot_args(
                    build=True,
                    role="current",
                    revision="other",
                    s3_artifact=None,
                    c3_artifact=None,
                    s3_log=None,
                    c3_log=None,
                ),
                repository,
            )

    def test_failed_build_removes_stale_revision_scoped_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            output = repo / "build/code_health/size"
            for target in code_health.TARGET_SHORT_NAMES.values():
                stale = output / "pio-build" / target / "firmware.bin"
                stale.parent.mkdir(parents=True, exist_ok=True)
                stale.write_bytes(b"stale")

            with patch.object(
                code_health,
                "run_bounded",
                return_value=CommandResult(("pio", "run"), 1, "failed", False),
            ):
                artifacts, logs, results = code_health._run_firmware_builds(
                    repo, output, None
                )

            self.assertTrue(all(not path.exists() for path in artifacts.values()))
            self.assertTrue(all(path is None for path in logs.values()))
            self.assertTrue(all(item["status"] == "incomplete" for item in results))

    def test_timed_out_zero_exit_build_is_incomplete_and_discards_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            output = repo / "build/code_health/size"

            def timed_out_build(command, **_kwargs):
                artifact = output / "pio-build" / command[-1] / "firmware.bin"
                artifact.parent.mkdir(parents=True, exist_ok=True)
                artifact.write_bytes(b"partial")
                return CommandResult(tuple(command), 0, "terminated after timeout", False, True)

            with patch.object(code_health, "run_bounded", side_effect=timed_out_build):
                artifacts, logs, results = code_health._run_firmware_builds(
                    repo, output, None
                )

            self.assertTrue(all(not path.exists() for path in artifacts.values()))
            self.assertTrue(all(path is None for path in logs.values()))
            self.assertTrue(all(item["status"] == "incomplete" for item in results))
            self.assertTrue(all(item["timed_out"] for item in results))

    def test_zero_exit_build_without_expected_artifact_is_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            output = repo / "build/code_health/size"

            with patch.object(
                code_health,
                "run_bounded",
                return_value=CommandResult(("pio", "run"), 0, "success", False),
            ):
                artifacts, logs, results = code_health._run_firmware_builds(
                    repo, output, None
                )

            self.assertTrue(all(not path.exists() for path in artifacts.values()))
            self.assertTrue(all(path is None for path in logs.values()))
            self.assertTrue(all(item["status"] == "incomplete" for item in results))

    def test_zero_exit_build_with_unreadable_artifact_is_incomplete(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            output = repo / "build/code_health/size"
            original_open = Path.open

            def successful_build(command, **_kwargs):
                artifact = output / "pio-build" / command[-1] / "firmware.bin"
                artifact.parent.mkdir(parents=True, exist_ok=True)
                artifact.write_bytes(b"firmware")
                return CommandResult(tuple(command), 0, "success", False)

            def deny_artifact_read(path, *args, **kwargs):
                mode = args[0] if args else kwargs.get("mode", "r")
                if path.name == "firmware.bin" and mode == "rb":
                    raise PermissionError("fixture denies firmware read")
                return original_open(path, *args, **kwargs)

            with patch.object(
                code_health, "run_bounded", side_effect=successful_build
            ), patch.object(Path, "open", deny_artifact_read):
                artifacts, logs, results = code_health._run_firmware_builds(
                    repo, output, None
                )

            self.assertTrue(all(not path.exists() for path in artifacts.values()))
            self.assertTrue(all(path is None for path in logs.values()))
            self.assertTrue(all(item["status"] == "incomplete" for item in results))

    def test_bundle_role_must_match_target_roles(self) -> None:
        payload = {
            "schema_version": 1,
            "kind": "firmware_size_bundle",
            "role": "baseline",
            "targets": {"litch-esp32s3": {"role": "current"}},
        }
        with self.assertRaisesRegex(CodeHealthError, "disagrees"):
            code_health._snapshot_role(payload)

    def test_empty_size_bundle_is_rejected(self) -> None:
        with self.assertRaisesRegex(CodeHealthError, "contains no targets"):
            code_health._target_snapshots(
                {
                    "schema_version": 1,
                    "kind": "firmware_size_bundle",
                    "role": "baseline",
                    "targets": {},
                }
            )

    def test_partial_size_bundle_and_single_snapshot_are_supported(self) -> None:
        snapshot = code_health.create_firmware_size_snapshot(
            target="c3",
            role="baseline",
            artifact_path=FIXTURES / "size_baseline.bin",
        )
        partial = {
            "schema_version": 1,
            "kind": "firmware_size_bundle",
            "role": "baseline",
            "targets": {"litch-esp32c3": snapshot},
        }

        self.assertEqual(
            set(code_health._target_snapshots(partial)),
            {"litch-esp32c3"},
        )
        self.assertEqual(
            set(code_health._target_snapshots(snapshot)),
            {"litch-esp32c3"},
        )

    def test_size_and_runtime_help_hide_legacy_c3_options(self) -> None:
        parser = code_health._parser()
        self.assertFalse(
            parser.parse_args(["size-snapshot", "--build"]).legacy_c3
        )
        self.assertFalse(
            parser.parse_args(["runtime-headroom"]).legacy_c3
        )

        for command in ("size-snapshot", "runtime-headroom"):
            output = io.StringIO()
            with self.assertRaises(SystemExit):
                with redirect_stdout(output):
                    parser.parse_args([command, "--help"])
            self.assertNotIn("c3", output.getvalue().casefold())

    def test_default_firmware_size_build_targets_only_s3(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            output = repo / "build/code_health/size"
            repo.mkdir()
            commands = []

            def failed_build(command, **_kwargs):
                commands.append(tuple(command))
                return CommandResult(tuple(command), 1, "failed", False)

            with patch.object(code_health, "run_bounded", side_effect=failed_build):
                artifacts, _, _ = code_health._run_firmware_builds(
                    repo, output, None
                )

            self.assertEqual(set(artifacts), {"s3"})
            self.assertEqual(commands, [("pio", "run", "-e", "litch-esp32s3")])

    def test_runtime_c3_evidence_requires_explicit_legacy_option(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            repo.mkdir()
            evidence = repo / "c3.log"
            evidence.write_text("loop=ok\n", encoding="utf-8")
            args = SimpleNamespace(
                repo_root=str(repo),
                output_dir=str(repo / "out"),
                evidence=[f"c3:hardware={evidence}"],
                report_only=True,
                legacy_c3=False,
            )
            with patch.object(
                code_health, "_repo_root", return_value=repo
            ), patch.object(
                code_health,
                "revision_info",
                return_value={"revision": "a" * 40, "dirty": False},
            ):
                with self.assertRaisesRegex(CodeHealthError, "explicit --legacy-c3"):
                    code_health.command_runtime_headroom(args)

    def test_size_bundle_requires_schema_and_explicit_role(self) -> None:
        snapshot = code_health.create_firmware_size_snapshot(
            target="c3",
            role="baseline",
            artifact_path=FIXTURES / "size_baseline.bin",
        )
        other = code_health.create_firmware_size_snapshot(
            target="s3",
            role="baseline",
            artifact_path=FIXTURES / "size_baseline.bin",
        )
        bundle = {
            "kind": "firmware_size_bundle",
            "targets": {
                "litch-esp32c3": snapshot,
                "litch-esp32s3": other,
            },
        }

        with self.assertRaisesRegex(CodeHealthError, "schema version"):
            code_health._target_snapshots(bundle)
        bundle["schema_version"] = 1
        with self.assertRaisesRegex(CodeHealthError, "declare.*role"):
            code_health._target_snapshots(bundle)

    def test_output_override_must_stay_in_repository(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            repo.mkdir()
            args = SimpleNamespace(output_dir=str(Path(temp_dir) / "outside"))
            with self.assertRaisesRegex(CodeHealthError, "inside the repository"):
                code_health._output_dir(args, repo, "analysis")

    def test_runtime_output_reuses_one_captured_repository_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            (repo / "app").mkdir(parents=True)
            (repo / "platformio.ini").write_text("[platformio]\n", encoding="utf-8")
            repository = {
                "revision": "a" * 40,
                "dirty": True,
                "worktree_fingerprint": "b" * 64,
            }
            args = SimpleNamespace(
                repo_root=str(repo),
                output_dir=None,
                evidence=None,
            )
            with patch.object(
                code_health,
                "revision_info",
                side_effect=[repository, repository],
            ) as revision:
                self.assertEqual(code_health.command_runtime_headroom(args), 0)

            self.assertEqual(revision.call_count, 2)
            report_path = (
                repo
                / "build/code_health"
                / "aaaaaaaaaaaa-dirty-bbbbbbbbbbbb"
                / "support/report.json"
            )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["repository"], repository)

    def test_runtime_rejects_repository_change_before_persisting_report(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            (repo / "app").mkdir(parents=True)
            (repo / "platformio.ini").write_text("[platformio]\n", encoding="utf-8")
            output = repo / "build/code_health/runtime-race"
            first = {
                "revision": "a" * 40,
                "dirty": False,
                "worktree_fingerprint": None,
            }
            second = {
                "revision": "a" * 40,
                "dirty": True,
                "worktree_fingerprint": "c" * 64,
            }
            args = SimpleNamespace(
                repo_root=str(repo),
                output_dir=str(output),
                evidence=None,
            )
            with patch.object(
                code_health,
                "revision_info",
                side_effect=[first, second],
            ):
                with self.assertRaisesRegex(CodeHealthError, "state changed"):
                    code_health.command_runtime_headroom(args)

            self.assertFalse((output / "report.json").exists())

    def test_runtime_cli_fails_incomplete_evidence_unless_report_only(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            repo.mkdir()
            evidence = repo / "overlong.log"
            evidence.write_text(
                "loops=1 last_dt_ms=1 min_dt_ms=1 max_dt_ms=1"
                + " " * 5000
                + "\n",
                encoding="utf-8",
            )
            repository = {
                "revision": "a" * 40,
                "dirty": False,
                "worktree_fingerprint": None,
            }

            def invoke(output_name, report_only):
                arguments = [
                    "runtime-headroom",
                    "--evidence",
                    f"s3:hardware={evidence}",
                    "--output-dir",
                    str(repo / output_name),
                ]
                if report_only:
                    arguments.append("--report-only")
                with patch.object(
                    code_health, "_repo_root", return_value=repo
                ), patch.object(
                    code_health, "revision_info", return_value=repository
                ), redirect_stdout(io.StringIO()):
                    return code_health.main(arguments)

            self.assertEqual(invoke("strict", False), 1)
            self.assertEqual(invoke("report-only", True), 0)
            report = json.loads(
                (repo / "strict/report.json").read_text(encoding="utf-8")
            )
            self.assertEqual(report["status"], "incomplete")
            self.assertEqual(report["targets"]["s3"]["status"], "unavailable")
            self.assertEqual(
                report["targets"]["s3"]["hardware_in_loop"]["status"],
                "unavailable",
            )

    def test_repository_drift_invalidates_previously_persisted_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            output = repo / "build/code_health/native"
            output.mkdir(parents=True)
            stale_paths = tuple(
                output / name
                for name in ("coverage.info", "coverage.json", "run.json", "summary.txt")
            )
            for path in stale_paths:
                path.write_text("stale ok evidence\n", encoding="utf-8")
            raw_profile = output / "profiles/123.profraw"
            raw_profile.parent.mkdir()
            raw_profile.write_bytes(b"diagnostic profile")
            expected = {
                "revision": "a" * 40,
                "dirty": False,
                "worktree_fingerprint": None,
            }
            observed = {
                "revision": "a" * 40,
                "dirty": True,
                "worktree_fingerprint": "d" * 64,
            }

            with patch.object(code_health, "revision_info", return_value=observed):
                with self.assertRaisesRegex(CodeHealthError, "state changed"):
                    code_health._assert_repository_state_unchanged(
                        repo,
                        expected,
                        invalidate_paths=stale_paths,
                    )

            self.assertTrue(all(not path.exists() for path in stale_paths))
            self.assertTrue(raw_profile.is_file())

    def test_bounded_command_times_out_and_terminates(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            result = run_bounded(
                [sys.executable, "-c", "import time; time.sleep(5)"],
                cwd=Path(temp_dir),
                timeout_seconds=0.05,
            )
        self.assertTrue(result.timed_out)
        self.assertNotEqual(result.returncode, 0)

    def test_bounded_command_rejects_inherited_pipe_that_outlives_parent(self) -> None:
        parent = (
            "import subprocess, sys; "
            "subprocess.Popen([sys.executable, '-c', "
            "'import time; time.sleep(0.5)'], stdout=sys.stdout, stderr=sys.stderr); "
            "print('parent complete', flush=True)"
        )
        with tempfile.TemporaryDirectory() as temp_dir, patch(
            "common.PIPE_DRAIN_JOIN_SECONDS", 0.02
        ):
            result = run_bounded(
                [sys.executable, "-c", parent],
                cwd=Path(temp_dir),
                timeout_seconds=2,
            )

        self.assertTrue(result.truncated)
        self.assertNotEqual(result.returncode, 0)

    def test_scrubbing_removes_unrelated_absolute_paths(self) -> None:
        scrubbed = scrub_text(
            "SDK failed at /private/tmp/sdk/include/header.h:7",
            Path("/workspace/litch"),
        )
        self.assertNotIn("/private/tmp", scrubbed)
        self.assertIn("<absolute>/header.h", scrubbed)

    def test_explicit_scope_rejects_symlinked_external_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            repo = root / "repo"
            scope = repo / "scope"
            scope.mkdir(parents=True)
            external = root / "external.cpp"
            external.write_text("int external;\n", encoding="utf-8")
            (scope / "external.cpp").symlink_to(external)
            with self.assertRaisesRegex(CodeHealthError, "outside the repository"):
                expand_repo_paths(repo, ["scope"], max_files=5, suffixes={".cpp"})

    def test_size_snapshot_writes_machine_bundle_with_declared_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            (repo / "app").mkdir(parents=True)
            (repo / "platformio.ini").write_text("[platformio]\n", encoding="utf-8")
            output = repo / "build/code_health/test-size"
            args = self._snapshot_args(repo_root=str(repo), output_dir=str(output))
            with patch.object(
                code_health,
                "revision_info",
                return_value={"revision": "head123", "dirty": False},
            ), patch.object(
                code_health,
                "_platformio_version",
                return_value={"status": "unavailable", "version": None, "reason": "test"},
            ), patch.object(
                code_health,
                "validate_repository_size_configuration",
                return_value={"status": "verified", "checks": []},
            ):
                returncode = code_health.command_size_snapshot(args)

            report = json.loads((output / "snapshot.json").read_text(encoding="utf-8"))
            self.assertEqual(returncode, 0)
            self.assertEqual(report["schema_version"], 1)
            self.assertEqual(report["role"], "baseline")
            self.assertEqual(report["revision"], "base123")
            self.assertEqual(
                report["revision_provenance"], "declared_for_imported_artifacts"
            )
            self.assertEqual(report["profile"]["label"], "repository-default")
            self.assertFalse(report["profile"]["affects_build"])
            self.assertEqual(
                report["build_configuration"]["provenance"],
                "reporting_checkout_not_verified_artifact_build",
            )
            self.assertEqual(report["tools"]["platformio"]["provenance"], "reporting_host")

    def test_current_build_snapshot_rejects_measured_capacity_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir) / "repo"
            (repo / "app").mkdir(parents=True)
            (repo / "platformio.ini").write_text("[platformio]\n", encoding="utf-8")
            output = repo / "build/code_health/test-size"
            s3_log = repo / "s3.log"
            c3_log = repo / "c3.log"
            s3_log.write_text(
                "RAM: (used 100 bytes from 327680 bytes)\n"
                "Flash: (used 1000 bytes from 2000000 bytes)\n",
                encoding="utf-8",
            )
            c3_log.write_text(
                "RAM: (used 100 bytes from 327680 bytes)\n"
                "Flash: (used 1000 bytes from 1310720 bytes)\n",
                encoding="utf-8",
            )
            args = self._snapshot_args(
                build=True,
                role="current",
                revision=None,
                s3_artifact=None,
                c3_artifact=None,
                s3_log=None,
                c3_log=None,
                repo_root=str(repo),
                output_dir=str(output),
            )
            artifacts = {
                "s3": FIXTURES / "size_current.bin",
                "c3": FIXTURES / "size_current.bin",
            }
            logs = {"s3": s3_log, "c3": c3_log}
            builds = [
                {"status": "completed", "target": target}
                for target in code_health.TARGET_SHORT_NAMES.values()
            ]
            with patch.object(
                code_health,
                "revision_info",
                return_value={"revision": "head123", "dirty": False},
            ), patch.object(
                code_health,
                "_run_firmware_builds",
                return_value=(artifacts, logs, builds),
            ), patch.object(
                code_health,
                "_assert_repository_state_unchanged",
            ), patch.object(
                code_health,
                "_build_configuration_provenance",
                return_value={"capacity_validation": {"status": "verified"}},
            ), patch.object(
                code_health,
                "_platformio_version",
                return_value={"status": "available", "version": "test", "reason": None},
            ):
                with self.assertRaisesRegex(CodeHealthError, "linked flash capacity"):
                    code_health.command_size_snapshot(args)
            self.assertFalse((output / "snapshot.json").exists())


if __name__ == "__main__":
    unittest.main()
