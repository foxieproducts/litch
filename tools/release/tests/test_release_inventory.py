from __future__ import annotations

from dataclasses import replace
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import release_inventory  # noqa: E402


class ReleaseInventoryTests(unittest.TestCase):
    def _evidence_fixture(
        self,
        repo_root: Path,
        component: release_inventory.Component,
        *,
        evidence_bytes: bytes = b"Exact carried license terms.\n",
        role: str = "license-text",
    ) -> dict[str, object]:
        evidence_path = (
            repo_root
            / "tools"
            / "release"
            / "license_evidence"
            / "fixture-license.txt"
        )
        evidence_path.parent.mkdir(parents=True)
        evidence_path.write_bytes(evidence_bytes)
        return {
            "ecosystem": component.ecosystem,
            "name": component.name,
            "version": component.version,
            "expected_source_type": component.source_type,
            "source_reference": component.source_reference,
            "package_url": component.package_url,
            "lock_digest": {
                "algorithm": component.lock_digest_algorithm,
                "value": component.lock_digest,
            },
            "license_expression": "NOASSERTION",
            "license_status": "resolved-exact-license-text",
            "notice_disposition": "include carried exact text",
            "provenance_notes": ["Fixture provenance is exact."],
            "evidence_files": [
                {
                    "path": "tools/release/license_evidence/fixture-license.txt",
                    "sha256": hashlib.sha256(evidence_bytes).hexdigest(),
                    "role": role,
                    "origin": "Exact fixture distribution LICENSE",
                }
            ],
        }

    def _write_evidence_index(
        self, repo_root: Path, records: list[dict[str, object]]
    ) -> None:
        index = repo_root / release_inventory.DEPENDENCY_EVIDENCE_RELATIVE
        index.parent.mkdir(parents=True, exist_ok=True)
        index.write_text(
            json.dumps(
                {
                    "schema": release_inventory.DEPENDENCY_EVIDENCE_SCHEMA,
                    "records": records,
                }
            ),
            encoding="utf-8",
        )

    @staticmethod
    def _unresolved_fixture_component() -> release_inventory.Component:
        return release_inventory.Component(
            ecosystem="dart-pub",
            name="alpha",
            version="1.2.3",
            relationship="direct main",
            source_type="hosted",
            source_reference="https://pub.dev",
            evidence_paths=("app/pubspec.lock",),
            license_expression="UNKNOWN",
            license_status="unresolved",
            notice_disposition="verify upstream",
            lock_digest_algorithm="sha256",
            lock_digest="1" * 64,
        )

    def test_pubspec_lock_parser_keeps_stable_order(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            lockfile = repo_root / "app" / "pubspec.lock"
            lockfile.parent.mkdir(parents=True)
            lockfile.write_text(
                "\n".join(
                    [
                        "packages:",
                        "  zebra:",
                        "    dependency: transitive",
                        "    description:",
                        "      name: zebra",
                        '      url: "https://pub.dev"',
                        "    source: hosted",
                        '    version: "1.0.0"',
                        "  alpha:",
                        '    dependency: "direct main"',
                        "    description:",
                        "      name: alpha",
                        '      url: "https://pub.dev"',
                        "    source: hosted",
                        '    version: "2.0.0"',
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            names = [
                component.name
                for component in release_inventory.parse_pubspec_lock(lockfile, repo_root)
            ]

        self.assertEqual(names, ["alpha", "zebra"])

    def test_pubspec_lock_parser_keeps_hosted_digest_and_exact_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            lockfile = repo_root / "app" / "pubspec.lock"
            lockfile.parent.mkdir(parents=True)
            lockfile.write_text(
                "\n".join(
                    [
                        "packages:",
                        "  alpha:",
                        "    dependency: transitive",
                        "    description:",
                        "      name: alpha",
                        f'      sha256: "{"a" * 64}"',
                        '      url: "https://pub.dev"',
                        "    source: hosted",
                        '    version: "2.0.0"',
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            component = release_inventory.parse_pubspec_lock(lockfile, repo_root)[0]

        self.assertEqual(component.lock_digest_algorithm, "sha256")
        self.assertEqual(component.lock_digest, "a" * 64)
        self.assertEqual(
            component.source_reference,
            "https://pub.dev/packages/alpha/versions/2.0.0",
        )

    def test_pubspec_lock_uses_exact_flutter_distribution_pin_for_sdk_packages(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            app = repo_root / "app"
            app.mkdir()
            (app / "pubspec.yaml").write_text(
                "environment:\n  sdk: ^3.11.0\n  flutter: 3.41.2\n",
                encoding="utf-8",
            )
            lockfile = app / "pubspec.lock"
            lockfile.write_text(
                "packages:\n"
                "  flutter:\n"
                '    dependency: "direct main"\n'
                "    description: flutter\n"
                "    source: sdk\n"
                '    version: "0.0.0"\n',
                encoding="utf-8",
            )

            component = release_inventory.parse_pubspec_lock(lockfile, repo_root)[0]

        self.assertEqual(component.version, "3.41.2")
        self.assertEqual(
            component.source_reference,
            "https://github.com/flutter/flutter/tree/3.41.2",
        )
        self.assertIn("exact Flutter SDK distribution", component.provenance_notes[0])

    def test_exact_dependency_evidence_resolves_and_carries_text(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            self._write_evidence_index(repo_root, [record])

            resolved = release_inventory.apply_dependency_license_evidence(
                [component], repo_root
            )[0]
            inventory = release_inventory.Inventory(
                tool_version="test",
                inventory_schema="inventory",
                sbom_schema="sbom",
                components=(resolved,),
            )

        self.assertEqual(resolved.license_status, "resolved-exact-license-text")
        self.assertEqual(resolved.license_expression, "NOASSERTION")
        self.assertEqual(len(resolved.license_evidence), 1)
        self.assertIn("Exact carried license terms.", release_inventory.notices_to_markdown(inventory))
        self.assertNotIn(
            "Exact carried license terms.", release_inventory.inventory_to_json(inventory)
        )

    def test_missing_dependency_evidence_remains_honestly_unresolved(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            self._write_evidence_index(repo_root, [])

            result = release_inventory.apply_dependency_license_evidence(
                [component], repo_root
            )[0]

        self.assertEqual(result.license_expression, "UNKNOWN")
        self.assertEqual(result.license_status, "unresolved")
        self.assertEqual(result.notice_disposition, "verify upstream")

    def test_dependency_evidence_rejects_stale_lock_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            record["lock_digest"] = {"algorithm": "sha256", "value": "2" * 64}
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "lock digest mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_source_reference_override(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            record["source_reference"] = "https://invented.invalid/source"
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "source reference mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_package_url_override(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            record["package_url"] = "pkg:generic/invented@9.9.9"
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "package URL mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_changed_evidence_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            evidence_path = (
                repo_root
                / "tools"
                / "release"
                / "license_evidence"
                / "fixture-license.txt"
            )
            evidence_path.write_bytes(b"Changed terms.\n")
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_orphan_evidence_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            orphan = (
                repo_root
                / "tools"
                / "release"
                / "license_evidence"
                / "orphan.txt"
            )
            orphan.write_bytes(b"Unreferenced terms.\n")
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "Unreferenced"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_path_escape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            outside = repo_root / "tools" / "release" / "outside-license.txt"
            outside.write_bytes(b"Outside terms.\n")
            record["evidence_files"][0] = {
                "path": "tools/release/outside-license.txt",
                "sha256": hashlib.sha256(outside.read_bytes()).hexdigest(),
                "role": "license-text",
                "origin": "Outside fixture",
            }
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "path escapes"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_stale_component_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            record["version"] = "1.2.4"
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "Stale or orphan"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_noassertion_evidence_requires_exact_license_text(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(
                repo_root, component, role="provenance-metadata"
            )
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "must carry exact license text"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_spdx_evidence_requires_exact_declaration_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(
                repo_root,
                component,
                evidence_bytes=b'{"license":"MIT"}\n',
                role="provenance-metadata",
            )
            record.update(
                {
                    "license_expression": "MIT",
                    "license_status": "resolved-declared-spdx",
                }
            )
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "exact declaration"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_spdx_evidence_rejects_declaration_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(
                repo_root,
                component,
                evidence_bytes=b'{"license":"MIT"}\n',
                role="provenance-metadata",
            )
            record.update(
                {
                    "license_expression": "Apache-2.0",
                    "license_status": "resolved-declared-spdx",
                    "license_declaration": {
                        "path": (
                            "tools/release/license_evidence/fixture-license.txt"
                        ),
                        "format": "json",
                        "field": "license",
                    },
                }
            )
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "SPDX declaration mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_free_form_spdx_label(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            record.update(
                {
                    "license_expression": "Apache 2.0",
                    "license_status": "resolved-declared-spdx-and-carried-text",
                }
            )
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "Unsupported SPDX"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_dependency_evidence_rejects_status_text_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            component = self._unresolved_fixture_component()
            record = self._evidence_fixture(repo_root, component)
            record["license_status"] = "resolved-invented-status"
            self._write_evidence_index(repo_root, [record])

            with self.assertRaisesRegex(ValueError, "status/text mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [component], repo_root
                )

    def test_unresolved_summary_does_not_assume_repository_exact_gap(self) -> None:
        questions = release_inventory._dependency_unresolved_questions(
            [self._unresolved_fixture_component()]
        )

        self.assertIn("1 Dart package entry remains unresolved", questions[0])
        self.assertNotIn("Flutter SDK packages", questions[0])

    def test_notice_display_normalizes_only_rendering_whitespace(self) -> None:
        self.assertEqual(
            release_inventory._markdown_display_text("terms  \r\nnext\t\r\n"),
            "terms\nnext\n",
        )

    def test_write_outputs_uses_relative_paths_and_deterministic_json(self) -> None:
        inventory = release_inventory.Inventory(
            tool_version="1.0.0",
            inventory_schema="schema-a",
            sbom_schema="schema-b",
            components=(
                release_inventory.Component(
                    ecosystem="vendored-c",
                    name="mcu-max",
                    version="1.0.6",
                    relationship="vendored-direct",
                    source_type="checked-in-source",
                    source_reference="https://example.invalid",
                    evidence_paths=("lib/litch-max/vendor/mcu-max/LICENSE",),
                    license_expression="MIT",
                    license_status="resolved",
                    notice_disposition="include notice",
                    provenance_notes=("Tracked license is present.",),
                    package_url="pkg:generic/mcu-max@1.0.6",
                ),
            ),
            unresolved_questions=("Question A",),
        )

        rendered = release_inventory.inventory_to_json(inventory)
        payload = json.loads(rendered)

        self.assertEqual(payload["components"][0]["evidence_paths"], ["lib/litch-max/vendor/mcu-max/LICENSE"])
        self.assertNotIn(str(Path.cwd()), rendered)
        self.assertEqual(rendered, release_inventory.inventory_to_json(inventory))

    def test_sbom_keeps_license_expression_and_status(self) -> None:
        inventory = release_inventory.Inventory(
            tool_version="1.1.0",
            inventory_schema="schema-a",
            sbom_schema="schema-b",
            components=(
                release_inventory.Component(
                    ecosystem="vendored-cpp",
                    name="example",
                    version="1.0",
                    relationship="vendored-direct",
                    source_type="checked-in-source",
                    source_reference="example.invalid",
                    evidence_paths=("lib/example/LICENSE",),
                    license_expression="MIT",
                    license_status="notice-missing",
                    notice_disposition="hold",
                ),
            ),
        )

        properties = {
            item["name"]: item["value"]
            for item in json.loads(release_inventory.sbom_to_json(inventory))[
                "components"
            ][0]["properties"]
        }

        self.assertEqual(properties["litch:license_expression"], "MIT")
        self.assertEqual(properties["litch:license_status"], "notice-missing")

    def test_sbom_keeps_license_evidence_origin_and_provenance(self) -> None:
        evidence = release_inventory.EvidenceFile(
            path="tools/release/license_evidence/example/LICENSE",
            sha256="a" * 64,
            role="license-text",
            origin="Separately hashed local cache observation.",
            text="terms\n",
        )
        inventory = release_inventory.Inventory(
            tool_version="1.3.0",
            inventory_schema="schema-a",
            sbom_schema="schema-b",
            components=(
                release_inventory.Component(
                    ecosystem="dart-pub",
                    name="example",
                    version="1.2.3",
                    relationship="direct",
                    source_type="hosted",
                    source_reference="https://pub.dev/packages/example/versions/1.2.3",
                    evidence_paths=(evidence.path,),
                    license_expression="NOASSERTION",
                    license_status="resolved-exact-license-text",
                    notice_disposition="include exact carried text",
                    provenance_notes=("Archive-to-file binding is unavailable.",),
                    license_evidence=(evidence,),
                ),
            ),
        )

        properties = {
            item["name"]: item["value"]
            for item in json.loads(release_inventory.sbom_to_json(inventory))[
                "components"
            ][0]["properties"]
        }

        license_evidence = json.loads(properties["litch:license_evidence"])
        self.assertEqual(license_evidence[0]["origin"], evidence.origin)
        self.assertEqual(
            json.loads(properties["litch:provenance_notes"]),
            ["Archive-to-file binding is unavailable."],
        )

    def test_notice_text_group_uses_cross_ecosystem_component_identity(self) -> None:
        evidence = release_inventory.EvidenceFile(
            path="tools/release/license_evidence/texts/shared.txt",
            sha256="b" * 64,
            role="license-text",
            origin="Shared exact text.",
            text="shared terms\n",
        )
        components = tuple(
            release_inventory.Component(
                ecosystem=ecosystem,
                name="example",
                version="1.2.3",
                relationship="direct",
                source_type="fixture",
                source_reference="fixture",
                evidence_paths=(evidence.path,),
                license_expression="NOASSERTION",
                license_status="resolved-exact-license-text",
                notice_disposition="include exact carried text",
                license_evidence=(evidence,),
            )
            for ecosystem in ("cocoapods", "dart-pub")
        )
        inventory = release_inventory.Inventory(
            tool_version="1.3.0",
            inventory_schema="schema-a",
            sbom_schema="schema-b",
            components=components,
        )

        rendered = release_inventory.notices_to_markdown(inventory)

        self.assertIn("`cocoapods:example@1.2.3`", rendered)
        self.assertIn("`dart-pub:example@1.2.3`", rendered)

    def test_repository_vendor_records_feed_release_inventory(self) -> None:
        components = {
            component.name: component
            for component in release_inventory.build_inventory(
                release_inventory.REPO_ROOT
            ).components
        }

        gitlink = subprocess.check_output(
            ["git", "ls-tree", "HEAD", "lib/litch-max"],
            cwd=release_inventory.REPO_ROOT, text=True,
        ).split()
        self.assertEqual(gitlink[:2], ["160000", "commit"])
        litch_max = components["Litch-Max"]
        self.assertEqual(litch_max.license_expression, "MPL-2.0")
        self.assertEqual(litch_max.lock_digest, gitlink[2])
        self.assertEqual(litch_max.source_type, "git-submodule")
        self.assertIn(".gitmodules", litch_max.evidence_paths)
        self.assertEqual(litch_max.source_reference,
            "https://github.com/foxieproducts/litch-max/tree/" + gitlink[2])
        self.assertEqual(litch_max.license_evidence[0].path, "lib/litch-max/LICENSE")
        notices = release_inventory.notices_to_markdown(
            release_inventory.build_inventory(release_inventory.REPO_ROOT)
        )
        source = "https://github.com/foxieproducts/litch-max/blob/" + gitlink[2]
        for path in ("LICENSE", "vendor/mcu-max/LICENSE",
                     "docs/mcu_max_provenance.md#license-and-attribution"):
            self.assertIn(f"]({source}/{path})", notices)
        self.assertNotIn("## Litch-Max\n", notices)
        self.assertEqual(components["mcu-max"].version, "1.0.7")
        self.assertEqual(components["mcu-max"].relationship, "vendored-transitive")
        self.assertEqual(
            components["mcu-max"].license_status,
            "resolved-reviewed-transitive-terms",
        )
        self.assertIn("LicenseRef-micro-Max-Public-Domain", components["mcu-max"].license_expression)
        self.assertIn("lib/litch-max/docs/mcu_max_provenance.md", components["mcu-max"].evidence_paths)
        self.assertEqual(components["PCA95x5"].version, "v0.1.3")
        self.assertEqual(
            components["PCA95x5"].license_status,
            "tracked-mit-notice-carried",
        )
        self.assertIn("lib/PCA95x5/LICENSE", components["PCA95x5"].evidence_paths)
        pca_license = release_inventory.REPO_ROOT / "lib" / "PCA95x5" / "LICENSE"
        self.assertEqual(
            components["PCA95x5"].notice_text,
            pca_license.read_text(encoding="utf-8"),
        )
        self.assertEqual(
            hashlib.sha256(pca_license.read_bytes()).hexdigest(),
            "e38a878eb6e9a21413bca755242c8a239e70a96e96c0e978009a381cdabadeef",
        )
        self.assertEqual(
            components["SparkFun Ambient Light Sensor Arduino Library"].version,
            "1.0.4",
        )
        self.assertEqual(components["SparkFun Ambient Light Sensor Arduino Library"].license_expression, "LicenseRef-SparkFun-VEML6030")
        self.assertIn("release anything derivative under the same license", components["SparkFun Ambient Light Sensor Arduino Library"].notice_text)

    def test_repository_dependency_evidence_keeps_exact_resolved_counts(self) -> None:
        inventory = release_inventory.build_inventory(release_inventory.REPO_ROOT)
        expected = {
            "dart-pub": (103, 103),
            "cocoapods": (10, 10),
            "flutter-asset": (1, 1),
            "maven": (96, 96),
            "platformio-platform": (2, 2),
            "platformio-framework": (1, 1),
            "platformio-lib": (3, 3),
            "platformio-runtime": (1, 1),
        }

        # Preserve exact checks for SDK, hosted, and checked-in path sources
        # rather than accepting dependency growth as a larger unresolved count.
        components = {
            component.name: component
            for component in inventory.components
            if component.ecosystem == "dart-pub"
        }
        self.assertEqual(components["flutter_localizations"].version, "3.41.2")
        self.assertEqual(components["flutter_localizations"].source_type, "sdk")
        # Approved replay dependency adds one exactly resolved component.
        self.assertEqual(components["chess"].version, "0.8.1")
        self.assertEqual(components["chess"].source_type, "hosted")
        self.assertEqual(
            components["chess"].lock_digest,
            "6e5a8222931bfb5cd14a4bb9b030c24e257e23957ec3c533fb0ff74a48984ddb",
        )
        self.assertEqual(
            components["chess"].license_status, "resolved-exact-license-text"
        )
        self.assertEqual(components["flutter_reactive_ble"].source_type, "path")
        self.assertEqual(
            components["flutter_reactive_ble"].license_status,
            "resolved-exact-license-text",
        )
        self.assertEqual(components["share_plus"].version, "11.1.0")
        self.assertEqual(
            components["share_plus"].license_status,
            "resolved-exact-license-text",
        )
        self.assertEqual(components["intl"].version, "0.20.2")
        self.assertEqual(
            components["intl"].lock_digest,
            "3df61194eb431efc39c4ceba583b95633a403f46c9fd341e550ce0bfa50e9aa5",
        )
        for name in ("flutter_localizations", "intl"):
            self.assertEqual(components[name].license_status, "resolved-exact-license-text")
            self.assertTrue(any(
                evidence.role == "license-text" and evidence.text
                for evidence in components[name].license_evidence
            ))

        media_dependencies = {
            "archive": "4.3.0",
            "file_selector": "1.1.0",
            "file_selector_android": "0.5.2+6",
            "file_selector_ios": "0.5.3+6",
            "file_selector_linux": "0.9.4+1",
            "file_selector_macos": "0.9.5+1",
            "file_selector_platform_interface": "2.7.0",
            "file_selector_web": "0.9.5",
            "file_selector_windows": "0.9.3+6",
            "image": "4.10.1",
            "posix": "6.5.2",
        }
        for name, version in media_dependencies.items():
            with self.subTest(package=name):
                component = components[name]
                self.assertEqual(component.version, version)
                self.assertEqual(component.source_type, "hosted")
                self.assertEqual(component.license_status, "resolved-exact-license-text")
                self.assertTrue(any(
                    evidence.role == "license-text" and evidence.text
                    for evidence in component.license_evidence
                ))

        for name in ("flutter_reactive_ble", "reactive_ble_mobile"):
            with self.subTest(path_package=name):
                patch = next(
                    evidence
                    for evidence in components[name].license_evidence
                    if Path(evidence.path).name == "LITCH_PATCH.md"
                )
                source = release_inventory.REPO_ROOT / "app/third_party" / name / "LITCH_PATCH.md"
                self.assertEqual(patch.sha256, hashlib.sha256(source.read_bytes()).hexdigest())

        actual = {}
        for ecosystem, (_, total) in expected.items():
            components = [
                component
                for component in inventory.components
                if component.ecosystem == ecosystem
            ]
            actual[ecosystem] = (
                sum(
                    component.license_status.startswith("resolved-")
                    for component in components
                ),
                len(components),
            )
            self.assertEqual(len(components), total)

        self.assertEqual(actual, expected)
        s3_components = {
            component.ecosystem: component for component in inventory.components
            if component.ecosystem in {"platformio-framework", "platformio-runtime"}
        }
        for ecosystem, component in s3_components.items():
            with self.subTest(ecosystem=ecosystem):
                self.assertEqual(component.license_expression, "NOASSERTION")
                self.assertTrue(any(path.endswith("/selection.json") for path in component.evidence_paths))
        cocoa_components = {
            component.name: component
            for component in inventory.components
            if component.ecosystem == "cocoapods"
        }
        selector_pod = cocoa_components["file_selector_ios"]
        self.assertEqual(selector_pod.version, "0.0.1")
        self.assertEqual(selector_pod.lock_digest, "ec57ec07954363dd730b642e765e58f199bb621a")
        self.assertEqual(selector_pod.license_status, "resolved-exact-license-text")
        self.assertEqual(cocoa_components["share_plus"].version, "0.0.1")
        self.assertEqual(
            cocoa_components["share_plus"].license_status,
            "resolved-exact-license-text",
        )
        reactive_pod = cocoa_components["reactive_ble_mobile"]
        reactive_provenance = " ".join(
            (*reactive_pod.provenance_notes,)
            + tuple(evidence.origin for evidence in reactive_pod.license_evidence)
        )
        self.assertIn("checked-in", reactive_provenance)
        self.assertNotIn("local Pub cache", reactive_provenance)
        self.assertNotIn("Pub archive is not carried", reactive_provenance)
        for name in ("Protobuf", "SwiftProtobuf"):
            provenance = " ".join(cocoa_components[name].provenance_notes)
            self.assertIn("no retained archive or tree digest", provenance)
            self.assertNotIn("checksum-matched package", provenance)
        flutter_pod_provenance = " ".join(
            cocoa_components["Flutter"].provenance_notes
        )
        self.assertIn("does not encode that SDK identity", flutter_pod_provenance)
        self.assertNotIn("generated by pinned Flutter", flutter_pod_provenance)
        unresolved = {
            component.name
            for component in inventory.components
            if component.ecosystem
            in {
                "dart-pub",
                "cocoapods",
                "platformio-platform",
                "platformio-framework",
                "platformio-lib",
                "platformio-runtime",
            }
            and not component.license_status.startswith("resolved-")
        }
        self.assertEqual(unresolved, set())

    def test_android_release_runtime_reaches_sbom_and_notices(self) -> None:
        inventory = release_inventory.build_inventory(release_inventory.REPO_ROOT)
        maven = {
            component.name: component
            for component in inventory.components
            if component.ecosystem == "maven"
        }
        expected_versions = {
            "com.jakewharton.rxrelay2:rxrelay": "2.1.1",
            "com.polidea.rxandroidble2:rxandroidble": "1.16.0",
            "com.polidea.rxandroidble2:dagger-library-shadow": "1.16.0",
            "com.google.protobuf:protobuf-javalite": "3.25.3",
            "io.reactivex.rxjava2:rxandroid": "2.1.1",
            "io.reactivex.rxjava2:rxjava": "2.2.17",
            "io.reactivex.rxjava2:rxkotlin": "2.4.0",
            "org.reactivestreams:reactive-streams": "1.0.3",
        }
        for name, version in expected_versions.items():
            with self.subTest(module=name):
                self.assertIn(name, maven)
                self.assertEqual(maven[name].version, version)
                self.assertEqual(maven[name].license_status, "resolved-declared-spdx-and-carried-text")
        self.assertIn("io.flutter:flutter_embedding_release", maven)
        self.assertNotIn("junit:junit", maven)
        self.assertNotIn("com.android.tools.build:gradle", maven)

        sbom = json.loads(release_inventory.sbom_to_json(inventory))
        ble = next(
            package for package in sbom["components"]
            if package["bom-ref"] == "maven:com.polidea.rxandroidble2:rxandroidble:1.16.0"
        )
        self.assertEqual(ble["type"], "library")
        self.assertEqual(ble["purl"], "pkg:maven/com.polidea.rxandroidble2/rxandroidble@1.16.0")
        properties = {prop["name"]: prop["value"] for prop in ble["properties"]}
        self.assertEqual(properties["litch:license_expression"], "Apache-2.0")
        self.assertEqual(properties["litch:source_reference"], ":app:releaseRuntimeClasspath")
        self.assertIn("project :reactive_ble_mobile", properties["litch:provenance_notes"])
        self.assertRegex(properties["litch:provenance_notes"], r"SHA-256 [0-9a-f]{64} for rxandroidble-1\.16\.0\.aar")
        notices = release_inventory.notices_to_markdown(inventory)
        self.assertIn("## com.polidea.rxandroidble2:rxandroidble\n", notices)
        self.assertNotIn("Android release runtime license evidence remains unresolved", notices)
        app_notices = release_inventory.app_native_notices_to_text(inventory)
        self.assertIn("com.polidea.rxandroidble2:rxandroidble 1.16.0\n", app_notices)
        self.assertIn("Copyright (C) 2009 The JSR-330 Expert Group", app_notices)
        self.assertIn("MaterialIcons ", app_notices)
        self.assertIn("SwiftProtobuf ", app_notices)
        self.assertNotIn("io.flutter:flutter_embedding_release", app_notices)

    def test_selected_project_license_is_single_exact_about_block(self) -> None:
        root = release_inventory.REPO_ROOT
        data = (root / release_inventory.ROOT_LICENSE_RELATIVE).read_bytes()
        inventory = release_inventory.build_inventory(root)
        records = json.loads(release_inventory.inventory_to_json(inventory))
        sbom = json.loads(release_inventory.sbom_to_json(inventory))
        license_record = records["project_license"]
        self.assertEqual(license_record["license_expression"], "MPL-2.0")
        self.assertEqual(license_record["evidence"]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(sbom["metadata"]["component"]["license"], license_record)
        foxie = next(component for component in inventory.components if component.name == "foxie_embedded")
        self.assertEqual(foxie.license_expression, "MPL-2.0")
        self.assertEqual(foxie.license_evidence[0].path, "LICENSE")
        self.assertIn("[Canonical license and full scope](LICENSE)", release_inventory.notices_to_markdown(inventory))
        app_blocks = release_inventory.app_native_notices_to_text(inventory).split("\n" + "-" * 80 + "\n")
        project_bodies = [
            body for labels, _, body in (block.partition("\n\n") for block in app_blocks)
            if labels.splitlines() == ["Litch"]
        ]
        self.assertEqual(project_bodies, [data.decode("utf-8")])

    def test_selected_hardware_license_is_separate_exact_about_block(self) -> None:
        root = release_inventory.REPO_ROOT
        data = (root / release_inventory.HARDWARE_LICENSE_RELATIVE).read_bytes()
        inventory = release_inventory.build_inventory(root)
        records = json.loads(release_inventory.inventory_to_json(inventory))
        sbom = json.loads(release_inventory.sbom_to_json(inventory))["metadata"]
        hardware = records["hardware_license"]
        self.assertEqual(hardware["license_expression"], "CERN-OHL-S-2.0")
        self.assertEqual(hardware["evidence"]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(hardware["source_location"], "https://github.com/foxieproducts/litch")
        self.assertEqual(sbom["hardware_license"], hardware)
        self.assertEqual(sbom["component"]["license"]["license_expression"], "MPL-2.0")
        blocks = release_inventory.app_native_notices_to_text(inventory).split("\n" + "-" * 80 + "\n")
        hardware_bodies = [
            body for labels, _, body in (block.partition("\n\n") for block in blocks)
            if labels.splitlines() == ["Litch hardware designs"]
        ]
        self.assertEqual(hardware_bodies, [data.decode("utf-8")])
        self.assertTrue(any(
            block.startswith("Litch licensing overview\n\n") and hardware["source_location"] in block
            for block in blocks
        ))
        self.assertIn(
            "[Canonical hardware license and full scope](LICENSE-HARDWARE)",
            release_inventory.notices_to_markdown(inventory),
        )

    def test_selected_documentation_license_is_separate_exact_about_block(self) -> None:
        root = release_inventory.REPO_ROOT
        data = (root / release_inventory.DOCUMENTATION_LICENSE_RELATIVE).read_bytes()
        self.assertIn(b"app/assets/images/foxie_planet.jpeg", data)
        self.assertIn(b"docs/images/litch-v3.31/foxie_planet.jpeg", data)
        self.assertIn(b"cover crop and a black overlay", data)
        inventory = release_inventory.build_inventory(root)
        records = json.loads(release_inventory.inventory_to_json(inventory))
        sbom = json.loads(release_inventory.sbom_to_json(inventory))["metadata"]
        documentation = records["documentation_license"]
        self.assertEqual(documentation["license_expression"], "CC-BY-SA-4.0")
        self.assertEqual(documentation["evidence"]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(sbom["documentation_license"], documentation)
        self.assertEqual(sbom["component"]["license"]["license_expression"], "MPL-2.0")
        self.assertEqual(sbom["hardware_license"]["license_expression"], "CERN-OHL-S-2.0")
        blocks = release_inventory.app_native_notices_to_text(inventory).split("\n" + "-" * 80 + "\n")
        documentation_bodies = [
            body for labels, _, body in (block.partition("\n\n") for block in blocks)
            if labels.splitlines() == ["Litch documentation and images"]
        ]
        self.assertEqual(documentation_bodies, [data.decode("utf-8")])
        self.assertIn("photographs and documentation previews", release_inventory.notices_to_markdown(inventory))

    def test_unapproved_root_license_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            self.assertIsNone(release_inventory.read_project_license(repo))
            (repo / release_inventory.ROOT_LICENSE_RELATIVE).write_text("Unreviewed terms.\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differs from the pinned"):
                release_inventory.read_project_license(repo)
            self.assertIsNone(release_inventory.read_hardware_license(repo))
            (repo / release_inventory.HARDWARE_LICENSE_RELATIVE).write_text("Unreviewed hardware terms.\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "differs from the pinned hardware-design"):
                release_inventory.read_hardware_license(repo)
            self.assertIsNone(release_inventory.read_documentation_license(repo))
            (repo / release_inventory.DOCUMENTATION_LICENSE_RELATIVE).write_text(
                "Unreviewed documentation terms.\n", encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "differs from the pinned documentation-and-image"):
                release_inventory.read_documentation_license(repo)

    def test_selected_board_license_texts_reach_app_notices(self) -> None:
        root = release_inventory.REPO_ROOT
        manifest = json.loads((root / release_inventory.FIRMWARE_LICENSE_MANIFEST_RELATIVE).read_text(encoding="utf-8"))
        inventory = release_inventory.build_inventory(root)
        app = release_inventory.app_native_notices_to_text(inventory)
        license_rows = [row for row in manifest["legal_files"] if row["role"] in {"license-text", "notice-text"}]
        self.assertEqual(len(inventory.board_notices), len(license_rows))
        labels = {
            label for block in app.split("\n" + "-" * 80 + "\n")
            for label in block.partition("\n\n")[0].splitlines()
        }
        self.assertEqual(
            {label for label in labels if label.startswith("Board firmware — ")},
            {f"Board firmware — {row['component']}" for row in license_rows},
        )
        self.assertIn("Litch licensing overview", labels)
        self.assertIn("Build tools — platformio/native 1.2.1", labels)
        self.assertIn("GCC Runtime Library Exception", app)
        self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", app)
        self.assertIn("Micro-Max is public domain, so you can use it for whatever purpose you like.", app)
        self.assertIn("release anything derivative under the same license", app)
        self.assertNotIn("## Complete diff classification", app)

    def test_board_notice_rejects_changed_manifest_text(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            evidence = repo / "fixture-license.txt"
            evidence.write_bytes(b"Exact board license.\n")
            manifest = repo / release_inventory.FIRMWARE_LICENSE_MANIFEST_RELATIVE
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({
                "schema_version": 1,
                "target": "litch-esp32s3",
                "legal_files": [{
                    "path": evidence.name,
                    "role": "license-text",
                    "component": "Fixture board library",
                    "sha256": hashlib.sha256(evidence.read_bytes()).hexdigest(),
                }],
            }), encoding="utf-8")
            self.assertEqual(release_inventory.collect_board_notices(repo), (
                ("Board firmware — Fixture board library", "Exact board license.\n"),
            ))
            evidence.write_bytes(b"Changed board license.\n")
            with self.assertRaisesRegex(ValueError, "Board notice evidence bytes changed"):
                release_inventory.collect_board_notices(repo)

    def test_maven_evidence_rejects_changed_runtime_bytes_at_same_coordinate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            component = replace(
                self._unresolved_fixture_component(), ecosystem="maven", name="example:library",
                lock_digest=None, lock_digest_algorithm=None,
                runtime_artifacts=(("library-1.2.3.jar", "2" * 64),),
            )
            record = self._evidence_fixture(root, component)
            record["lock_digest"] = None
            record["runtime_artifacts"] = dict(component.runtime_artifacts)
            self._write_evidence_index(root, [record])
            self.assertEqual(release_inventory.apply_dependency_license_evidence([component], root)[0].license_status,
                             "resolved-exact-license-text")
            changed = replace(component, runtime_artifacts=(("library-1.2.3.jar", "3" * 64),))
            with self.assertRaisesRegex(ValueError, "runtime artifact mismatch"):
                release_inventory.apply_dependency_license_evidence([changed], root)

    def test_maven_pom_rejects_unreviewed_license_url(self) -> None:
        pom = b"""<project><groupId>example</groupId><artifactId>library</artifactId><version>1.2.3</version>
            <licenses><license><name>Apache-2.0</name><url>https://example.invalid/license</url></license></licenses>
            </project>"""
        with self.assertRaisesRegex(ValueError, "Unsupported Maven POM license declaration"):
            release_inventory.parse_maven_pom(pom, "fixture.pom")

    def _copy_android_release_runtime_fixture(self, fixture: Path) -> None:
        source = release_inventory.REPO_ROOT
        for relative in (
            *release_inventory.ANDROID_RUNTIME_INPUTS,
            release_inventory.ANDROID_RUNTIME_RELATIVE,
        ):
            target = fixture / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((source / relative).read_bytes())

    def test_android_release_runtime_snapshot_rejects_gradle_input_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            self._copy_android_release_runtime_fixture(fixture)
            self.assertTrue(release_inventory.parse_android_runtime_snapshot(fixture))
            gradle = fixture / "app/android/app/build.gradle.kts"
            gradle.write_text(gradle.read_text(encoding="utf-8") + "\n// input changed\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Stale Android release runtime snapshot for app/android/app/build.gradle.kts"):
                release_inventory.parse_android_runtime_snapshot(fixture)

    def test_android_release_runtime_snapshot_binds_pub_dependencies_without_native_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            self._copy_android_release_runtime_fixture(fixture)
            pubspec = fixture / "app/pubspec.yaml"
            content = pubspec.read_text(encoding="utf-8")
            version_changed = re.sub(r"(?m)^version:.*$", "version: 99.0.0+100", content)
            self.assertNotEqual(content, version_changed)
            pubspec.write_text(version_changed, encoding="utf-8")
            self.assertTrue(release_inventory.parse_android_runtime_snapshot(fixture))

            pubspec.write_text(
                version_changed.replace("dependencies:\n", "dependencies:\n  fixture_dependency: 1.0.0\n", 1),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "Stale Android release runtime snapshot for app/pubspec.yaml"):
                release_inventory.parse_android_runtime_snapshot(fixture)

            pubspec.write_text(content, encoding="utf-8")
            (fixture / "app/pubspec_overrides.yaml").write_text("dependency_overrides: {}\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "local app/pubspec_overrides.yaml"):
                release_inventory.parse_android_runtime_snapshot(fixture)

    def test_android_release_runtime_snapshot_rejects_disconnected_maven_cycle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            self._copy_android_release_runtime_fixture(fixture)
            snapshot = fixture / release_inventory.ANDROID_RUNTIME_RELATIVE
            payload = json.loads(snapshot.read_text(encoding="utf-8"))
            module = payload["modules"][0]
            module["parents"].append(module["coordinate"])
            snapshot.write_text(json.dumps(payload), encoding="utf-8")
            self.assertTrue(release_inventory.parse_android_runtime_snapshot(fixture))

            module["parents"] = [module["coordinate"]]
            snapshot.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Unreachable Maven modules in Android release runtime"):
                release_inventory.parse_android_runtime_snapshot(fixture)

    def test_podfile_lock_prefers_top_level_resolved_versions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            lockfile = repo_root / "app" / "ios" / "Podfile.lock"
            lockfile.parent.mkdir(parents=True)
            lockfile.write_text(
                "\n".join(
                    [
                        "PODS:",
                        "  - Protobuf (3.29.6)",
                        "  - reactive_ble_mobile (0.0.1):",
                        "    - Protobuf (~> 3.5)",
                        "",
                        "DEPENDENCIES:",
                        "  - reactive_ble_mobile (from `.symlinks/plugins/reactive_ble_mobile/darwin`)",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            components = release_inventory.parse_podfile_lock(lockfile, repo_root)

        self.assertEqual(
            {component.name: component.version for component in components},
            {"Protobuf": "3.29.6", "reactive_ble_mobile": "0.0.1"},
        )

    def test_podfile_lock_parser_keeps_spec_checksums(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            lockfile = repo_root / "app" / "ios" / "Podfile.lock"
            lockfile.parent.mkdir(parents=True)
            lockfile.write_text(
                "\n".join(
                    [
                        "PODS:",
                        "  - Protobuf (3.29.6)",
                        "",
                        "SPEC CHECKSUMS:",
                        "  Protobuf: 8d5596f7468be5400861a6bbfb2dfe9d0d1cde34",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            component = release_inventory.parse_podfile_lock(lockfile, repo_root)[0]

        self.assertEqual(component.lock_digest_algorithm, "sha1")
        self.assertEqual(
            component.lock_digest, "8d5596f7468be5400861a6bbfb2dfe9d0d1cde34"
        )

    def test_podfile_lock_path_drift_stales_dependency_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            lockfile = repo_root / "app" / "ios" / "Podfile.lock"
            lockfile.parent.mkdir(parents=True)

            def write_lock(source_path: str) -> None:
                lockfile.write_text(
                    "\n".join(
                        [
                            "PODS:",
                            "  - example (1.2.3)",
                            "",
                            "EXTERNAL SOURCES:",
                            "  example:",
                            f'    :path: "{source_path}"',
                            "",
                            "SPEC CHECKSUMS:",
                            f"  example: {'1' * 40}",
                        ]
                    )
                    + "\n",
                    encoding="utf-8",
                )

            write_lock(".symlinks/plugins/example/ios")
            original = release_inventory.parse_podfile_lock(lockfile, repo_root)[0]
            record = self._evidence_fixture(repo_root, original)
            self._write_evidence_index(repo_root, [record])
            write_lock(".symlinks/plugins/example/replaced")
            changed = release_inventory.parse_podfile_lock(lockfile, repo_root)[0]

            with self.assertRaisesRegex(ValueError, "source reference mismatch"):
                release_inventory.apply_dependency_license_evidence(
                    [changed], repo_root
                )

    def test_s3_runtime_rejects_provider_override_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            source = release_inventory.REPO_ROOT
            runtime, = release_inventory.collect_s3_runtime(source)
            self.assertEqual(runtime.source_reference, "espressif/toolchain-xtensa-esp32s3@8.4.0+2021r2-patch5")
            self.assertEqual(runtime.ecosystem, "platformio-runtime")
            for relative in runtime.evidence_paths:
                destination = repo / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((source / relative).read_bytes())
            manifest = repo / "platformio.ini"
            content = manifest.read_text(encoding="utf-8")
            start = content.index("[env:litch-esp32s3]")
            before, s3_and_later = content[:start], content[start:]
            changed = s3_and_later.replace(
                "platform_packages =\n",
                "platform_packages =\n  espressif/toolchain-xtensa-esp32s3@9.1.0+2023r1\n",
                1,
            )
            manifest.write_text(before + changed, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "S3 runtime provider differs"):
                release_inventory.collect_s3_runtime(repo)

    def test_platformio_inventory_merges_shared_pins_across_envs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            manifest = repo_root / "platformio.ini"
            manifest.write_text(
                "\n".join(
                    [
                        "[env:first]",
                        "platform = platformio/espressif32@6.12.0",
                        "framework = arduino",
                        "lib_deps =",
                        "  bblanchon/ArduinoJson@6.21.5",
                        "",
                        "[env:second]",
                        "platform = platformio/espressif32@6.12.0",
                        "framework = arduino",
                        "lib_deps =",
                        "  bblanchon/ArduinoJson@6.21.5",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            components = release_inventory.parse_platformio(manifest, repo_root)

        relationships = {
            component.name: component.relationship for component in components
        }
        self.assertEqual(
            relationships["bblanchon/ArduinoJson"],
            "platformio:first,platformio:second",
        )

    def test_platformio_inventory_records_exact_framework_override(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            manifest = repo_root / "platformio.ini"
            manifest.write_text(
                "\n".join(
                    [
                        "[env:esp32]",
                        "platform = platformio/espressif32@6.12.0",
                        "framework = arduino",
                        "platform_packages =",
                        "  platformio/framework-arduinoespressif32@3.20017.241212+sha.dcc1105b",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            components = release_inventory.parse_platformio(manifest, repo_root)

        framework = next(
            component for component in components if component.name == "arduino"
        )
        self.assertEqual(framework.version, "3.20017.241212+sha.dcc1105b")
        self.assertEqual(framework.source_type, "manifest-pin")
        self.assertEqual(
            framework.source_reference,
            "platformio/framework-arduinoespressif32@3.20017.241212+sha.dcc1105b",
        )
        self.assertEqual(
            framework.package_url,
            "pkg:generic/platformio/framework-arduinoespressif32@3.20017.241212+sha.dcc1105b",
        )

    def test_platformio_inventory_rejects_conflicting_pins_across_envs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            manifest = repo_root / "platformio.ini"
            manifest.write_text(
                "\n".join(
                    [
                        "[env:first]",
                        "lib_deps = bblanchon/ArduinoJson@6.21.5",
                        "",
                        "[env:second]",
                        "lib_deps = bblanchon/ArduinoJson@7.0.0",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "Conflicting PlatformIO"):
                release_inventory.parse_platformio(manifest, repo_root)

    def test_notices_do_not_overclaim_unknown_licenses(self) -> None:
        inventory = release_inventory.Inventory(
            tool_version="1.0.0",
            inventory_schema="schema-a",
            sbom_schema="schema-b",
            components=(
                release_inventory.Component(
                    ecosystem="vendored-cpp",
                    name="PCA95x5",
                    version="unresolved",
                    relationship="vendored-direct",
                    source_type="checked-in-source",
                    source_reference="lib/PCA95x5/PCA95x5.h",
                    evidence_paths=("lib/PCA95x5/PCA95x5.h",),
                    license_expression="UNKNOWN",
                    license_status="unresolved",
                    notice_disposition="hold for legal review before publication",
                    provenance_notes=("No tracked license file exists.",),
                    package_url=None,
                ),
            ),
            unresolved_questions=(),
        )

        rendered = release_inventory.notices_to_markdown(inventory)

        self.assertIn("hold for legal review before publication", rendered)
        self.assertNotIn("permission is hereby granted", rendered.lower())

    def test_notices_include_mcu_max_mit_text_with_provenance(self) -> None:
        mit_text = "MIT License\n\nCopyright (c) Example\n\nPermission is hereby granted"
        inventory = release_inventory.Inventory(
            tool_version="1.0.0",
            inventory_schema="schema-a",
            sbom_schema="schema-b",
            components=(
                release_inventory.Component(
                    ecosystem="vendored-c",
                    name="mcu-max",
                    version="1.0.6",
                    relationship="vendored-direct",
                    source_type="checked-in-source",
                    source_reference="https://example.invalid",
                    evidence_paths=("lib/litch-max/vendor/mcu-max/LICENSE",),
                    license_expression="MIT",
                    license_status="resolved-from-tracked-license",
                    notice_disposition="include MIT notice text",
                    notice_text=mit_text,
                ),
            ),
        )

        rendered = release_inventory.notices_to_markdown(inventory)

        self.assertIn(mit_text, rendered)
        self.assertIn(
            "Generator provenance: extracted verbatim from the tracked "
            "`lib/litch-max/vendor/mcu-max/LICENSE` evidence path.",
            rendered,
        )
        self.assertIn("Test provenance:", rendered)

    def test_mit_notice_rejects_recorded_sha256_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory)
            license_path = repo_root / "lib" / "PCA95x5" / "LICENSE"
            license_path.parent.mkdir(parents=True)
            license_path.write_text("MIT License\n\nchanged\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "SHA-256"):
                release_inventory._read_mit_notice(
                    repo_root,
                    "lib/PCA95x5/LICENSE",
                    expected_sha256="0" * 64,
                )

    def test_custom_repo_root_owns_default_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory) / "repository"
            (repo_root / "app" / "ios").mkdir(parents=True)
            (repo_root / "lib" / "litch-max" / "vendor" / "mcu-max").mkdir(parents=True)
            (repo_root / "lib" / "PCA95x5").mkdir(parents=True)
            (repo_root / "tools" / "vendor").mkdir(parents=True)
            (repo_root / "app" / "pubspec.lock").write_text(
                "packages:\n", encoding="utf-8"
            )
            (repo_root / "app" / "ios" / "Podfile.lock").write_text(
                "PODS:\n", encoding="utf-8"
            )
            (repo_root / "platformio.ini").write_text(
                "[env:native]\nplatform = native\n", encoding="utf-8"
            )
            (repo_root / "lib" / "litch-max" / "vendor" / "mcu-max" / "LICENSE").write_text(
                "MIT License\n\nCopyright (c) Example\n", encoding="utf-8"
            )
            pca_notice = "MIT License\n\nCopyright (c) Example\n"
            (repo_root / "lib" / "PCA95x5" / "LICENSE").write_text(
                pca_notice, encoding="utf-8"
            )
            (repo_root / "tools" / "vendor" / "sources.json").write_text(
                json.dumps(
                    {
                        "vendors": {
                            "pca95x5": {
                                "license_sha256": hashlib.sha256(
                                    pca_notice.encode("utf-8")
                                ).hexdigest()
                            }
                        }
                    }
                ),
                encoding="utf-8",
            )

            subprocess.run(
                [
                    sys.executable,
                    str(RELEASE_TOOLS / "release_inventory.py"),
                    "--repo-root",
                    str(repo_root),
                ],
                capture_output=True,
                text=True,
                check=True,
            )

            self.assertTrue(
                (repo_root / "docs" / "release" / "generated" /
                 "dependency_inventory.json").is_file()
            )
            self.assertTrue((repo_root / "THIRD_PARTY_NOTICES.md").is_file())
            app_notice = repo_root / release_inventory.APP_NATIVE_NOTICES_RELATIVE
            app_notice.write_bytes(b"checked-in app notice")
            root_notice = repo_root / "THIRD_PARTY_NOTICES.md"
            root_notice.write_bytes(b"checked-in project notice")
            isolated = Path(temporary_directory) / "isolated"
            subprocess.run([
                sys.executable, str(RELEASE_TOOLS / "release_inventory.py"), "--repo-root", str(repo_root),
                "--output-dir", str(isolated),
            ], capture_output=True, text=True, check=True)
            self.assertEqual(app_notice.read_bytes(), b"checked-in app notice")
            self.assertEqual(root_notice.read_bytes(), b"checked-in project notice")
            self.assertTrue((isolated / release_inventory.APP_NATIVE_NOTICES_RELATIVE.name).is_file())
            self.assertTrue((isolated / root_notice.name).is_file())


if __name__ == "__main__":
    unittest.main()
