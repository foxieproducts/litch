from __future__ import annotations

from pathlib import Path
import gzip
import io
import json
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import firmware_compliance as compliance
import relink_firmware
from test_firmware_artifact import make_esp32s3_image

FIXTURE_LICENSE = b"Synthetic admission fixture; not a license grant.\n"


def make_bundle_fixture(directory: Path, firmware_sha256: str, source_sha: str,
                        license_text: bytes | None = FIXTURE_LICENSE,
                        litch_max_license_text: bytes | None = FIXTURE_LICENSE) -> Path:
    """Small synthetic content for asset-integrity tests, never a legal clearance."""
    files = {name: f"fixture {name}\n".encode() for name in compliance.REQUIRED_FILES}
    files["source/arduino/cores/esp32/main.cpp"] = b"void fixture() {}\n"
    files["source/libraries/NeoPixelBus/src/NeoPixelBus.h"] = b"// synthetic header\n"
    policy = json.loads(compliance.SOURCE_POLICY_PATH.read_text())
    source_root = Path(__file__).resolve().parents[3]
    for name in policy["first_party_files"] + policy["vendor_files"]:
        files["source/project/" + name] = ((source_root / name).read_bytes()
            if name.startswith("lib/litch-max/") else b"// synthetic source\n")
    for name in policy["opening_book_baseline"]["files"]:
        baseline_root = Path(__file__).resolve().parents[3] / policy["opening_book_baseline"]["local_root"]
        files["source/opening_book_baseline/" + name] = (baseline_root / name).read_bytes()
    files["notices/opening_book_baseline.json"] = compliance.canonical_json({
        "head": policy["opening_book_baseline"]["source_sha"],
        "files": {name: compliance.digest(files["source/opening_book_baseline/" + name])
                  for name in policy["opening_book_baseline"]["files"]}})
    files["installation/otadata.bin"] = compliance.installation_selector()
    files["objects/application.o"] = b"synthetic object fixture\n"
    files["notices/litch-max.snapshot.json"] = (source_root / "lib/litch-max.snapshot.json").read_bytes()
    for relative, row in compliance.litch_max_legal_records().items():
        if relative == "lib/litch-max/LICENSE":
            continue
        content = (source_root / relative).read_bytes()
        files["notices/" + row["sha256"] + ".txt"] = content
        if relative.endswith("mcu_max_provenance.md"):
            files["notices/mcu_max_provenance.md"] = content
    if license_text is None:
        files.pop("notices/LITCH-LICENSE.txt", None)
    else:
        files["notices/LITCH-LICENSE.txt"] = license_text
    if litch_max_license_text is None:
        files.pop("notices/LITCH-MAX-LICENSE.txt", None)
    else:
        files["notices/LITCH-MAX-LICENSE.txt"] = litch_max_license_text
        files["notices/" + compliance.digest(litch_max_license_text) + ".txt"] = litch_max_license_text
    return compliance.write_bundle(files, source_sha, firmware_sha256, "e" * 64, directory)


class FirmwareComplianceTests(unittest.TestCase):
    def test_original_source_delivery_binds_capture_and_git_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source_names = ["src/neopixel_led_renderer.cpp", "lib/litch-max/src/classic_game.cpp"]
            files = source_names + ["include/protocol.hpp"]
            for name in files:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(f"// source fixture {name}\n")
            historical = root / "historical/baseline/src/search.cpp"
            historical.parent.mkdir(parents=True)
            historical.write_bytes(b"// pinned historical source\n")
            manifest = root / "historical/baseline.json"
            manifest.write_text(json.dumps({"head": "a" * 40,
                "files": {"src/search.cpp": compliance.digest(historical.read_bytes())}}))
            def git(*args):
                return subprocess.check_output(["git", "-c", "user.name=Fixture",
                    "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                    "-c", "core.hooksPath=/dev/null", *args], cwd=root)
            git("init", "--quiet")
            git("add", *files, "historical")
            git("commit", "--quiet", "-m", "source fixture")
            source_sha = git("rev-parse", "HEAD").decode().strip()
            policy = root / "policy.json"
            policy.write_text(json.dumps({"schema_version": 1, "target": "litch-esp32s3",
                "translation_units": source_names[:1], "source_translation_units": source_names,
                "first_party_files": files, "vendor_files": [], "generated_sources": {},
                "opening_book_baseline": {"source_sha": "a" * 40, "files": ["src/search.cpp"],
                    "local_root": "historical/baseline", "manifest": "historical/baseline.json",
                    "manifest_sha256": compliance.digest(compliance.canonical_json(json.loads(manifest.read_text())))}}))
            inputs = compliance.BundleInputs.__new__(compliance.BundleInputs)
            inputs.root, inputs.libdeps = root, root / ".pio/libdeps/litch-esp32s3"
            inputs.records = {str(root / name): {"size": (root / name).stat().st_size,
                "sha256": compliance.digest((root / name).read_bytes())} for name in files}
            delivery = [{"source": str(root / name), "dependencies": [str(root / name)]}
                        for name in source_names]
            delivery[1]["dependencies"].append(str(root / "include/protocol.hpp"))
            inputs.metadata = {"application_rebuild": delivery[:1], "project_sources": delivery}
            with mock.patch.object(compliance, "SOURCE_POLICY_PATH", policy):
                inputs.validate_project_sources(source_sha)
                self.assertEqual(inputs.project_sources, set(files))
                delivered_history = compliance.opening_book_inputs(root, source_sha)
                self.assertEqual(delivered_history["source/opening_book_baseline/src/search.cpp"],
                                 historical.read_bytes())
                historical.write_bytes(b"// changed historical source\n")
                with self.assertRaisesRegex(ValueError, "Git source"):
                    compliance.opening_book_inputs(root, source_sha)
                inputs.metadata["project_sources"] = delivery[:1]
                with self.assertRaisesRegex(ValueError, "source closure"):
                    inputs.validate_project_sources(source_sha)
                inputs.metadata["project_sources"] = delivery
                changed = root / source_names[1]
                changed.write_text("// changed after capture\n")
                with self.assertRaisesRegex(ValueError, "captured build input changed"):
                    inputs.validate_project_sources(source_sha)
                inputs.records[str(changed)] = {"size": changed.stat().st_size,
                                               "sha256": compliance.digest(changed.read_bytes())}
                with self.assertRaisesRegex(ValueError, "Git source"):
                    inputs.validate_project_sources(source_sha)

    def test_bundle_supplies_original_sources_outside_lgpl_rebuild(self):
        # These actual linked sources were previously delivered only as objects.
        required = ("source/project/src/ble_ota_transaction.cpp",
                    "source/project/lib/foxie_embedded/src/settings_store_load.cpp",
                    "source/project/lib/litch-max/src/classic_game.cpp",
                    "source/project/lib/litch-max/vendor/mcu-max/src/mcu-max-position.c",
                    "source/project/lib/litch-max/tools/engine_knowledge/generate_kpk.cpp",
                    "source/project/lib/litch-max/tools/engine_knowledge/historical/generate_opening_book.py",
                    "source/opening_book_baseline/src/classic_chess_search.cpp")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = make_bundle_fixture(root / "complete", "a" * 64, "b" * 40)
            with tarfile.open(bundle, "r:gz") as archive:
                files = {item.name: archive.extractfile(item).read()
                         for item in archive if item.name != "bundle.json"}
            for index, member in enumerate(required):
                self.assertTrue(member in files, f"missing corresponding source: {member}")
                omitted = dict(files)
                omitted.pop(member)
                destination = root / f"omitted-{index}"
                with self.assertRaisesRegex(ValueError, "complete declared inventory|pinned snapshot"):
                    compliance.write_bundle(omitted, "b" * 40, "a" * 64, "e" * 64, destination)
                self.assertEqual(list(destination.iterdir()), [])
            for index, member in enumerate(("notices/mcu_max_provenance.md",
                    "notices/" + compliance.litch_max_legal_records()[
                        "lib/litch-max/vendor/mcu-max/LICENSE"]["sha256"] + ".txt")):
                omitted = dict(files)
                omitted.pop(member)
                with self.assertRaisesRegex(ValueError, "vendor notices"):
                    compliance.write_bundle(omitted, "b" * 40, "a" * 64, "e" * 64,
                                            root / f"omitted-notice-{index}")
            changed = dict(files)
            changed["source/project/lib/litch-max/src/classic_game.cpp"] += b"// changed\n"
            with self.assertRaisesRegex(ValueError, "pinned snapshot"):
                compliance.write_bundle(changed, "b" * 40, "a" * 64, "e" * 64, root / "changed-source")
            snapshot = json.loads(changed["notices/litch-max.snapshot.json"])
            snapshot["files"]["src/classic_game.cpp"]["sha256"] = compliance.digest(
                changed["source/project/lib/litch-max/src/classic_game.cpp"])
            changed["notices/litch-max.snapshot.json"] = compliance.canonical_json(snapshot)
            substituted = compliance.write_bundle(changed, "b" * 40, "a" * 64, "e" * 64,
                                                  root / "substituted-snapshot")
            with self.assertRaisesRegex(ValueError, "snapshot differs from the selected Git source"):
                compliance.inspect_bundle(substituted, "a" * 64, "b" * 40,
                    expected_litch_max_snapshot_sha256=compliance.digest(files["notices/litch-max.snapshot.json"]))
            files["notices/opening_book_baseline.json"] = b'{}\n'
            with self.assertRaisesRegex(ValueError, "complete declared inventory"):
                compliance.write_bundle(files, "b" * 40, "a" * 64, "e" * 64, root / "wrong-baseline")

    def test_prepare_requires_bound_root_license_before_build_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def git(*args):
                return subprocess.check_output(["git", "-c", "user.name=Fixture",
                    "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                    "-c", "core.hooksPath=/dev/null", *args], cwd=root)
            git("init", "--quiet")
            # Commits exist only in this disposable fixture repository.
            git("commit", "--quiet", "--allow-empty", "-m", "license fixture")
            source = git("rev-parse", "HEAD").decode().strip()
            with mock.patch.object(compliance.BundleInputs, "complete_dependencies") as dependencies:
                with self.assertRaisesRegex(ValueError, "root LICENSE"):
                    compliance.prepare(root, root / "missing-build", source, root / "output")
                (root / "LICENSE").write_text("Synthetic license fixture; not a grant.\n")
                with self.assertRaisesRegex(ValueError, "root LICENSE"):
                    compliance.prepare(root, root / "missing-build", source, root / "output")
                git("add", "LICENSE")
                git("commit", "--quiet", "-m", "bind fixture license")
                source = git("rev-parse", "HEAD").decode().strip()
                expected = (root / "LICENSE").read_bytes()
                self.assertEqual(compliance.source_license(root, source), expected)
                with self.assertRaisesRegex(ValueError, "Litch-Max LICENSE"):
                    compliance.prepare(root, root / "missing-build", source, root / "output")
                library_license = root / "lib/litch-max/LICENSE"
                library_license.parent.mkdir(parents=True)
                library_license.write_bytes(FIXTURE_LICENSE)
                (root / "lib/litch-max.snapshot.json").write_text('{}\n')
                git("add", "lib/litch-max/LICENSE", "lib/litch-max.snapshot.json")
                git("commit", "--quiet", "-m", "bind library fixture license")
                source = git("rev-parse", "HEAD").decode().strip()
                with self.assertRaisesRegex(ValueError, "relink-info"):
                    compliance.prepare(root, root / "missing-build", source, root / "output")
                (root / "LICENSE").write_bytes(expected + b"Uncommitted change\n")
                with self.assertRaisesRegex(ValueError, "root LICENSE"):
                    compliance.prepare(root, root / "missing-build", source, root / "output")
                dependencies.assert_not_called()
                self.assertFalse((root / "output").exists())

    def test_archive_requires_bounded_utf8_firstparty_license(self):
        with tempfile.TemporaryDirectory() as temporary:
            for index, invalid in enumerate((None, b"", b" \n\t", b"\xff", b"x" * (256 * 1024 + 1))):
                with self.subTest(case=index), self.assertRaises(ValueError):
                    make_bundle_fixture(Path(temporary) / str(index), "a" * 64, "b" * 40, invalid)
                with self.subTest(library_case=index), self.assertRaises(ValueError):
                    make_bundle_fixture(Path(temporary) / f"library-{index}", "a" * 64, "b" * 40,
                                        litch_max_license_text=invalid)
            expected = "Synthetic UTF-8 fixture — not a grant.\n".encode()
            bundle = make_bundle_fixture(Path(temporary) / "valid", "a" * 64, "b" * 40, expected)
            compliance.inspect_bundle(bundle, "a" * 64, "b" * 40)
            compliance.inspect_bundle(bundle, "a" * 64, "b" * 40,
                                      expected_license_sha256=compliance.digest(expected),
                                      expected_litch_max_license_sha256=compliance.digest(FIXTURE_LICENSE))
            with self.assertRaisesRegex(ValueError, "LICENSE differs"):
                compliance.inspect_bundle(bundle, "a" * 64, "b" * 40,
                                          expected_license_sha256="f" * 64)
            with self.assertRaisesRegex(ValueError, "Litch-Max LICENSE differs"):
                compliance.inspect_bundle(bundle, "a" * 64, "b" * 40,
                                          expected_litch_max_license_sha256="f" * 64)
            with tarfile.open(bundle, "r:gz") as archive:
                self.assertEqual(archive.extractfile("notices/LITCH-LICENSE.txt").read(), expected)

    def test_exact_descriptor_and_wrong_candidate_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = make_bundle_fixture(Path(temporary), "a" * 64, "b" * 40)
            result = compliance.inspect_bundle(bundle, "a" * 64, "b" * 40)
            self.assertEqual(result["filename"], bundle.name)
            self.assertEqual(result["size_bytes"], bundle.stat().st_size)
            with self.assertRaises(ValueError):
                compliance.inspect_bundle(bundle, "c" * 64, "b" * 40)
            with self.assertRaises(ValueError):
                compliance.inspect_bundle(bundle, "a" * 64, "c" * 40)

    def test_deterministic_bundle_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = make_bundle_fixture(root / "first", "a" * 64, "b" * 40)
            second = make_bundle_fixture(root / "second", "a" * 64, "b" * 40)
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_member_paths(self):
        for name in ("../escape", "/absolute", "a/../b", "a//b", "a/.git/config", "a\\b",
                     "private/app_source.cpp", "signing-key.pem", "objects/key.pem"):
            self.assertFalse(compliance.safe_member(name), name)

    def test_failed_admission_leaves_no_final_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(ValueError):
                compliance.write_bundle({"README.md": b"incomplete\n"},
                                        "b" * 40, "a" * 64, "e" * 64, root)
            self.assertEqual(list(root.iterdir()), [])

    def test_hidden_archive_content_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = make_bundle_fixture(root, "a" * 64, "b" * 40).read_bytes()
            expanded = gzip.decompress(original)
            cases = [original + gzip.compress(b"hidden", mtime=0)]
            for payload in (expanded + b"hidden" + bytes(506), expanded[:-512] + b"x" + bytes(511)):
                stream = io.BytesIO()
                with gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressed:
                    compressed.write(payload)
                cases.append(stream.getvalue())
            for data in cases:
                path = root / f"firmware_compliance_{compliance.digest(data)}.tar.gz"
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    compliance.inspect_bundle(path, "a" * 64, "b" * 40)
            with mock.patch.object(compliance, "MAX_TAR_SIZE", 512):
                path = root / f"firmware_compliance_{compliance.digest(original)}.tar.gz"
                with self.assertRaises(ValueError):
                    compliance.inspect_bundle(path, "a" * 64, "b" * 40)

    def test_original_image_verification_only_admits_descriptor_digest(self):
        segment = bytearray(256)
        struct.pack_into("<I", segment, 0, 0xabcd5432)
        segment[0x90:0xb0] = bytes.fromhex("a" * 64)
        original = make_esp32s3_image(segment)
        segment[0x90:0xb0] = bytes.fromhex("b" * 64)
        rebuilt = make_esp32s3_image(segment)
        self.assertEqual(relink_firmware.verify_original_image(
            rebuilt, "b" * 64, "a" * 64, compliance.digest(original)), original)
        segment[0xe0] ^= 1  # Integrity remains valid; actual loadable content differs.
        modified = make_esp32s3_image(segment)
        with self.assertRaisesRegex(ValueError, "differs beyond"):
            relink_firmware.verify_original_image(
                modified, "b" * 64, "a" * 64, compliance.digest(original))

    def test_failed_relink_never_exposes_unverified_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "result"
            def failed_run(_bundle, stage, *_args):
                stage.mkdir()
                (stage / "firmware.bin").write_bytes(b"unverified")
                raise ValueError("image differs")
            with mock.patch.object(relink_firmware, "_run", side_effect=failed_run):
                with self.assertRaises(ValueError):
                    relink_firmware.run(Path(temporary), output, Path(temporary), Path(temporary))
            self.assertFalse(output.exists())
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_unknown_private_key_is_rejected_even_in_vendor_archive(self):
        for label in (b"RSA PRIVATE KEY", b"ENCRYPTED PRIVATE KEY"):
            data = b"-----BEGIN " + label + b"-----\n" + b"A" * 100 + b"\n-----END " + label + b"-----"
            with self.assertRaisesRegex(ValueError, "unknown private key"):
                compliance.reject_private_material("objects/sdk/lib/libmbedx509.a", data)
            with self.assertRaises(ValueError):
                compliance.reject_private_material("objects/application.o", data)

    def test_runtime_archive_admission_binds_each_reviewed_host(self):
        root = Path(__file__).resolve().parents[3]
        legal, contents = compliance.legal_inputs(root,
            root / "tools/release/firmware_license_manifest.json", root)
        with tempfile.TemporaryDirectory() as directory:
            for relative in compliance.LITCH_MAX_LEGAL_PATHS:
                changed = {**legal, "legal_files": [row for row in legal["legal_files"]
                                                   if row["path"] != relative]}
                manifest = Path(directory) / "legal.json"
                manifest.write_text(json.dumps(changed))
                with self.assertRaisesRegex(ValueError, "required Litch-Max terms"):
                    compliance.legal_inputs(root, manifest, root)
        selection = json.loads(contents[legal["runtime_selection"]])
        hosts = {}
        for system in ("darwin_arm64", "linux_x86_64"):
            package = json.loads(contents[selection["variants"][system]["package_metadata"]])
            hosts[system] = compliance.runtime_archives(selection, package, contents)
            self.assertEqual(len(hosts[system]), 4)
        self.assertNotEqual([item["sha256"] for item in hosts["darwin_arm64"]],
                            [item["sha256"] for item in hosts["linux_x86_64"]])
        package["system"] = "unreviewed_host"
        with self.assertRaisesRegex(ValueError, "host variant"):
            compliance.runtime_archives(selection, package, contents)
        package["version"] = "changed"
        with self.assertRaisesRegex(ValueError, "identity"):
            compliance.runtime_archives(selection, package, contents)


if __name__ == "__main__":
    unittest.main()
