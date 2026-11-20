from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path


RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import capture_dependency_license_evidence as capture  # noqa: E402


class DependencyEvidenceCaptureTests(unittest.TestCase):
    def test_external_review_capture_keeps_tracked_logical_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            source = temporary_root / "source-license.txt"
            source.write_bytes(b"Exact terms.\n")
            output = temporary_root / "review" / "license_evidence"

            evidence = capture._capture_file(
                source,
                Path("dart-pub/alpha-1.0.0/LICENSE"),
                role="license-text",
                origin="Exact fixture archive",
                evidence_root=output,
            )

            self.assertEqual(
                evidence["path"],
                "tools/release/license_evidence/dart-pub/alpha-1.0.0/LICENSE",
            )
            self.assertEqual(
                evidence["sha256"], hashlib.sha256(source.read_bytes()).hexdigest()
            )
            self.assertEqual(
                (output / "dart-pub/alpha-1.0.0/LICENSE").read_bytes(),
                source.read_bytes(),
            )

    def test_dart_capture_includes_checked_in_path_package(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            package_root = repo_root / "app/third_party/example_plugin"
            package_root.mkdir(parents=True)
            (repo_root / "app/pubspec.lock").write_text(
                "\n".join(
                    [
                        "packages:",
                        "  example_plugin:",
                        '    dependency: "direct main"',
                        "    description:",
                        '      path: "third_party/example_plugin"',
                        "      relative: true",
                        "    source: path",
                        '    version: "1.2.3"',
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            (package_root / "pubspec.yaml").write_text(
                "name: example_plugin\nversion: 1.2.3\n",
                encoding="utf-8",
            )
            (package_root / "LICENSE").write_text(
                "Example terms.\n",
                encoding="utf-8",
            )
            (package_root / "LITCH_PATCH.md").write_text(
                "Local patch notes.\n",
                encoding="utf-8",
            )
            output = repo_root / "review/license_evidence"

            records, records_by_package, roots_by_package = capture._capture_dart(
                repo_root,
                repo_root / "empty-pub-cache",
                output,
            )

            self.assertEqual(len(records), 1)
            record = records[0]
            self.assertEqual(record["name"], "example_plugin")
            self.assertEqual(record["expected_source_type"], "path")
            self.assertIsNone(record["lock_digest"])
            self.assertEqual(
                roots_by_package[("example_plugin", "1.2.3")],
                package_root.resolve(),
            )
            self.assertIs(records_by_package[("example_plugin", "1.2.3")], record)
            self.assertEqual(
                {item["role"] for item in record["evidence_files"]},
                {"provenance-metadata", "license-text"},
            )
            self.assertTrue(
                any(
                    Path(item["path"]).name == "LITCH_PATCH.md"
                    for item in record["evidence_files"]
                )
            )

    def test_current_path_pod_capture_table_covers_podfile_lock(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        path_pods = {
            component.name
            for component in capture.release_inventory.parse_podfile_lock(
                repo_root / "app/ios/Podfile.lock",
                repo_root,
            )
            if component.source_type == "path" and component.name != "Flutter"
        }
        configured_path_pods = {entry[0] for entry in capture.COCOAPODS_PATH_PODS}

        self.assertEqual(configured_path_pods, path_pods)

    def test_flutter_sdk_capture_rejects_wrong_installed_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            (root / "app").mkdir()
            (root / "app/pubspec.yaml").write_text(
                "environment:\n  sdk: ^3.11.0\n  flutter: 3.41.2\n",
                encoding="utf-8",
            )
            sdk = root / "flutter-sdk"
            (sdk / "bin/cache").mkdir(parents=True)
            (sdk / "bin/cache/flutter.version.json").write_text(
                json.dumps(
                    {
                        "frameworkVersion": "3.40.0",
                        "flutterVersion": "3.40.0",
                        "channel": "stable",
                        "repositoryUrl": "https://github.com/flutter/flutter.git",
                        "frameworkRevision": "a" * 40,
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "does not match pinned 3.41.2"):
                capture._capture_flutter_sdk(
                    root,
                    sdk,
                    root / "review/license_evidence",
                )

    def test_cocoapods_cache_selection_rejects_ambiguous_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            cache = Path(temporary_directory)
            (cache / "Protobuf/3.29.6-abcde-one").mkdir(parents=True)
            (cache / "Protobuf/3.29.6-abcde-two").mkdir(parents=True)

            with self.assertRaisesRegex(ValueError, "found 2"):
                capture._installed_cocoapods_package(
                    cache,
                    "Protobuf",
                    "3.29.6",
                    "abcde" + "0" * 35,
                )

    def test_component_coverage_fails_closed_on_missing_handler(self) -> None:
        component = capture.release_inventory.Component(
            ecosystem="cocoapods",
            name="new_path_pod",
            version="0.0.1",
            relationship="direct",
            source_type="path",
            source_reference="Podfile.lock :path: plugin",
            evidence_paths=("app/ios/Podfile.lock",),
            license_expression="UNKNOWN",
            license_status="unresolved",
            notice_disposition="verify",
        )

        with self.assertRaisesRegex(ValueError, "new_path_pod"):
            capture._require_component_coverage(
                "cocoapods",
                [component],
                [],
                allowed_unresolved=set(),
            )

    def test_path_pod_rejects_license_bytes_that_do_not_match_dart_evidence(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            license_path = Path(temporary_directory) / "LICENSE"
            license_path.write_text("Observed terms.\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "example_plugin"):
                capture._require_license_evidence_matches(
                    license_path,
                    [{"sha256": "0" * 64}],
                    "example_plugin",
                )


if __name__ == "__main__":
    unittest.main()
