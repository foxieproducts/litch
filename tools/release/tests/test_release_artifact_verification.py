from __future__ import annotations

import sys
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import verify_release_artifacts  # noqa: E402


class ReleaseArtifactVerificationTests(unittest.TestCase):
    def _outputs(self) -> dict[str, bytes]:
        return {
            **{f"docs/release/generated/{name}": b"clean"
               for name in verify_release_artifacts.EXPECTED_GENERATED},
            "THIRD_PARTY_NOTICES.md": b"notices",
            verify_release_artifacts.APP_NATIVE_NOTICES_RELATIVE.as_posix(): b"app notices",
        }

    def _write_outputs(self, root: Path, outputs: dict[str, bytes]) -> None:
        for name, data in outputs.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def test_second_generation_detects_nondeterminism(self) -> None:
        outputs = self._outputs()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_outputs(root, outputs)
            changed = {**outputs, "THIRD_PARTY_NOTICES.md": b"different notices"}
            with patch.object(verify_release_artifacts, "_generate", side_effect=[outputs, changed]), \
                    self.assertRaisesRegex(verify_release_artifacts.VerificationError, "nondeterministic"):
                verify_release_artifacts.verify(root)

    def test_stale_first_generation_does_not_repeat_known_failed_work(self) -> None:
        outputs = self._outputs()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._write_outputs(root, outputs)
            changed = {**outputs, verify_release_artifacts.APP_NATIVE_NOTICES_RELATIVE.as_posix(): b"stale app notice"}
            with patch.object(verify_release_artifacts, "_generate", return_value=changed) as generate:
                with self.assertRaisesRegex(verify_release_artifacts.VerificationError, "stale"):
                    verify_release_artifacts.verify(root)
            self.assertEqual(generate.call_count, 1)

    def test_contained_rejects_escape(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(verify_release_artifacts.VerificationError):
                verify_release_artifacts._contained(root.parent / "outside", root)

    def test_expected_sets_are_explicit(self) -> None:
        self.assertEqual(verify_release_artifacts.EXPECTED_GENERATED, {
            "dependency_inventory.json", "dependency_inventory.md", "release_sbom.json",
        })
        self.assertEqual(verify_release_artifacts.EXPECTED_ROOT, {
            "THIRD_PARTY_NOTICES.md", "app/assets/licenses/native_runtime_notices.txt",
        })

    def test_files_rejects_missing_and_unexpected_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "expected.txt").write_text("ok", encoding="utf-8")
            (root / "extra.txt").write_text("unexpected", encoding="utf-8")
            with self.assertRaisesRegex(verify_release_artifacts.VerificationError, "unexpected"):
                verify_release_artifacts._files(root, frozenset({"expected.txt"}))

    def test_total_deadline_covers_both_actual_generation_phase_budgets(self) -> None:
        outputs = self._outputs()
        deadlines = []

        def generator(command, **kwargs):
            script = Path(command[1]).name
            deadlines.append((script, kwargs["timeout"]))
            directory = Path(command[command.index("--output-dir") + 1])
            self.assertEqual(script, "release_inventory.py")
            for name in ("dependency_inventory.json", "dependency_inventory.md", "release_sbom.json"):
                (directory / name).write_bytes(outputs[f"docs/release/generated/{name}"])
            Path(command[command.index("--notices-path") + 1]).write_bytes(
                outputs["THIRD_PARTY_NOTICES.md"]
            )
            app_notices = Path(command[command.index("--app-notices-path") + 1])
            app_notices.parent.mkdir(parents=True)
            app_notices.write_bytes(outputs[verify_release_artifacts.APP_NATIVE_NOTICES_RELATIVE.as_posix()])
            return subprocess.CompletedProcess(command, 0, "", "")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_outputs(root, outputs)
            with patch.object(verify_release_artifacts.subprocess, "run", side_effect=generator):
                verify_release_artifacts.verify(root)

        self.assertEqual(deadlines, [("release_inventory.py", 120), ("release_inventory.py", 120)])
        self.assertEqual(
            verify_release_artifacts.VERIFICATION_TIMEOUT_SECONDS,
            sum(seconds for _, seconds in deadlines) + 60,
        )

    def test_generator_timeout_fails_with_bounded_diagnostic(self) -> None:
        with patch.object(
            verify_release_artifacts.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(
                cmd=["release_inventory.py"], timeout=60
            ),
        ), self.assertRaisesRegex(
            verify_release_artifacts.VerificationError,
            "release_inventory.py timed out",
        ):
            verify_release_artifacts._run_generator(
                Path("release_inventory.py"),
                Path("."),
                Path("generated"),
            )


if __name__ == "__main__":
    unittest.main()
