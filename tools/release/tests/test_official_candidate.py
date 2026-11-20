from __future__ import annotations

import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch

RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import official_candidate as candidate
from test_firmware_artifact import make_esp32s3_image
from test_firmware_compliance import FIXTURE_LICENSE, make_bundle_fixture


class CandidateFixture:
    def __init__(self, directory: str):
        self.base = Path(directory)
        self.root = self.base / "source"
        self.root.mkdir()
        for name in candidate.BUILD_INPUTS:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture build input\n")
        (self.root / "LICENSE").write_bytes(FIXTURE_LICENSE)
        (self.root / "lib/litch-max/LICENSE").write_bytes(FIXTURE_LICENSE)
        (self.root / "lib/litch-max.snapshot.json").write_bytes(
            (RELEASE_TOOLS.parents[1] / "lib/litch-max.snapshot.json").read_bytes())
        (self.root / candidate.VERSION_HEADER).write_text('const char* FIRMWARE_VERSION = "0.9.0-9";\n')
        (self.root / "app/pubspec.yaml").write_text("version: 0.9.0+11\n")
        identity = (RELEASE_TOOLS.parents[1] / candidate.release_identity.APP_IDENTITY).read_text()
        identity = candidate.release_identity.APP_VERSION.sub(
            lambda m: (m[1] + "  version: '0.9.0-9',\n  nativeVersion: '0.9.0',\n"
                       "  buildNumber: '11',\n" + m[5]), identity)
        (self.root / candidate.release_identity.APP_IDENTITY).write_text(identity)
        (self.root / "notes.md").write_text("Improves board recovery.\n")
        self.git("init", "--quiet")
        self.git("add", ".")
        # Only the disposable fixture repository gets a synthetic commit.
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "--quiet", "-m", "fixture")
        self.base_sha = self.git("rev-parse", "HEAD").decode().strip()
        (self.root / "notes.md").write_text("Changed fixture\n")
        self.git("add", "notes.md")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "--quiet", "-m", "Improves board recovery.")
        self.source_sha = self.git("rev-parse", "HEAD").decode().strip()
        self.firmware = self.base / "firmware.bin"
        self.firmware.write_bytes(make_esp32s3_image())
        self.output = self.base / "candidate"

    def git(self, *args):
        return candidate.git(self.root, *args)

    def prepare(self, channel="stable"):
        self.bundle_index = getattr(self, "bundle_index", 0) + 1
        self.compliance = make_bundle_fixture(
            self.base / f"compliance-{self.bundle_index}", candidate.sha256(self.firmware.read_bytes()), self.source_sha)
        return candidate.prepare(self.root, self.firmware, channel,
                                 self.source_sha, self.base_sha, self.output, self.compliance)

    def historical(self):
        """Reconstruct the old receipt format without granting new publication."""
        receipt = json.loads((self.output / "candidate.json").read_text())
        receipt["schema_version"] = 1
        receipt.pop("compliance_bundle")
        receipt.pop("build_execution", None)
        self.output = self.base / "historical-candidate"
        self.output.mkdir()
        notes = candidate.draft_notes(self.root, self.source_sha, self.base_sha)
        candidate.write_payloads(self.output, self.firmware.read_bytes(), notes,
                                 receipt["channel"], receipt["version"])
        return candidate.write_receipt(self.output, receipt)


class OfficialCandidateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.fixture = CandidateFixture(self.temporary.name)

    def test_new_candidates_require_compliance_bound_to_exact_source_and_firmware(self):
        f = self.fixture
        with self.assertRaisesRegex(ValueError, "require --compliance-bundle"):
            candidate.prepare(f.root, f.firmware, "stable", f.source_sha, f.base_sha, f.output)
        self.assertFalse(f.output.exists())
        for source, firmware, license_text, library_license in (
                ("0" * 40, candidate.sha256(f.firmware.read_bytes()), FIXTURE_LICENSE, FIXTURE_LICENSE),
                (f.source_sha, "0" * 64, FIXTURE_LICENSE, FIXTURE_LICENSE),
                (f.source_sha, candidate.sha256(f.firmware.read_bytes()), b"Unbound fixture grant.\n", FIXTURE_LICENSE),
                (f.source_sha, candidate.sha256(f.firmware.read_bytes()), FIXTURE_LICENSE, b"Unbound library grant.\n")):
            with self.subTest(source=source, firmware=firmware, license_text=license_text,
                              library_license=library_license):
                bundle = make_bundle_fixture(f.base / "wrong-compliance", firmware, source,
                                             license_text, library_license)
                with self.assertRaises(ValueError):
                    candidate.prepare(f.root, f.firmware, "stable", f.source_sha,
                                      f.base_sha, f.output, bundle)
                self.assertFalse(f.output.exists())

    def test_compliance_descriptor_and_archive_are_strictly_bounded(self):
        f = self.fixture
        digest = f.prepare()
        receipt = candidate.verify(f.output, f.source_sha, digest)
        self.assertEqual(receipt["schema_version"], 2)
        original = (f.output / "candidate.json").read_bytes()
        descriptor = receipt["compliance_bundle"]
        for changes in ({"source_sha": "0" * 40}, {"firmware_sha256": "0" * 64},
                        {"size_bytes": True}, {"size_bytes": descriptor["size_bytes"] + 1},
                        {"filename": "../bundle.tar.gz"}, {"extra": "unexpected"},
                        {"sha256": "0" * 64}):
            with self.subTest(changes=changes):
                changed = {**receipt, "compliance_bundle": {**descriptor, **changes}}
                data = candidate.artifact.render_manifest_json(changed).encode()
                (f.output / "candidate.json").write_bytes(data)
                with self.assertRaises(ValueError):
                    candidate.verify(f.output, f.source_sha, candidate.sha256(data))
        for license_name, license_hash in ((name, value) for name in (
                "LICENSE", "lib/litch-max/LICENSE", "lib/litch-max.snapshot.json")
                                         for value in (None, "bad", "0" * 64)):
            with self.subTest(license_name=license_name, license_hash=license_hash):
                inputs = dict(receipt["input_sha256"])
                if license_hash is None:
                    inputs.pop(license_name)
                else:
                    inputs[license_name] = license_hash
                data = candidate.artifact.render_manifest_json({**receipt, "input_sha256": inputs}).encode()
                (f.output / "candidate.json").write_bytes(data)
                with self.assertRaisesRegex(ValueError, "license|LICENSE|snapshot"):
                    candidate.verify(f.output, f.source_sha, candidate.sha256(data))
        (f.output / "candidate.json").write_bytes(original)
        with patch.object(candidate.firmware_compliance, "MAX_BUNDLE_SIZE", descriptor["size_bytes"] - 1):
            with self.assertRaisesRegex(ValueError, "descriptor"):
                candidate.verify(f.output, f.source_sha, digest)

    def test_historical_receipt_is_readable_but_cannot_be_finalized_for_publication(self):
        f = self.fixture
        f.prepare()
        digest = f.historical()
        self.assertEqual(candidate.verify(f.output, f.source_sha, digest)["schema_version"], 1)
        notes = f.base / "reviewed.md"
        notes.write_text("Reviewed notes.\n")
        with self.assertRaisesRegex(ValueError, "historical candidates are read-only"):
            candidate.finalize_notes(f.output, f.source_sha, digest, notes, f.base / "final")
        with self.assertRaisesRegex(ValueError, "historical candidates are read-only"):
            candidate.prepare_notes(f.root, f.output, f.source_sha, digest,
                                    "stable", f.base_sha, f.base / "review")

    def test_source_bound_notes_for_both_channels_precede_build_and_preserve_one_image(self):
        f = self.fixture
        beta_base = f.source_sha
        (f.root / "notes.md").write_text("Latest reviewed source\n")
        f.git("add", "notes.md")
        f.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
              "commit", "--quiet", "-m", "Refines recovery guidance.")
        f.source_sha = f.git("rev-parse", "HEAD").decode().strip()
        reviews = {}
        for channel, base in (("beta", beta_base), ("stable", f.base_sha)):
            review = f.base / (channel + "-source-notes")
            candidate.prepare_source_notes(f.root, f.source_sha, channel, base, review)
            context = json.loads((review / "notes-context.json").read_text())
            self.assertEqual(context["source_sha"], f.source_sha)
            self.assertEqual(context["notes_base_sha"], base)
            self.assertNotIn("build_receipt_sha256", context)
            notes = review / "notes.md"
            notes.write_text(f"## Changes\n\n- Reviewed {channel} recovery notes.\n")
            reviews[channel] = (review, candidate.sha256(notes.read_bytes()),
                                candidate.sha256((review / "notes-context.json").read_bytes()))
        self.assertFalse(f.output.exists(), "Note approval does not require a prior firmware build")
        digest = f.prepare("beta")
        for channel, (review, approved_sha, approved_context_sha) in reviews.items():
            final = f.base / (channel + "-final")
            final_sha = candidate.finalize_notes(
                f.output, f.source_sha, digest, review / "notes.md", final,
                review / "notes-context.json", approved_sha, approved_context_sha)
            receipt = candidate.verify(final, f.source_sha, final_sha)
            alias = "firmware_beta.bin" if channel == "beta" else "firmware.bin"
            self.assertEqual((final / "assets" / alias).read_bytes(), f.firmware.read_bytes())
            self.assertEqual(receipt["channel"], channel)
            self.assertEqual(receipt["notes_review_sha256"], approved_sha)
            self.assertEqual(receipt["notes_review_context_sha256"], approved_context_sha)
            self.assertEqual(receipt["notes_status"], "reviewed")

    def test_source_bound_notes_require_exact_source_and_explicit_body_approval(self):
        f = self.fixture
        review = f.base / "source-notes"
        candidate.prepare_source_notes(f.root, f.source_sha, "stable", f.base_sha, review)
        digest = f.prepare("beta")
        context_path = review / "notes-context.json"
        context = json.loads(context_path.read_text())
        notes = review / "notes.md"
        approved_sha = candidate.sha256(notes.read_bytes())
        approved_context_sha = candidate.sha256(context_path.read_bytes())

        def context_digest(record):
            return candidate.sha256(candidate.artifact.render_manifest_json(record).encode())

        invalid_contexts = [
            {**context, "source_sha": "0" * 40},
            {**context, "version": "0.9.0-10"},
            {**context, "channel": "other"},
            {**context, "build_receipt_sha256": digest},
        ]
        cases = [
            (context, None, approved_context_sha),
            (context, approved_sha, None),
            ({**context, "channel": "beta"}, approved_sha, approved_context_sha),
            ({**context, "notes_base_sha": f.source_sha}, approved_sha, approved_context_sha),
            (context, "0" * 64, approved_context_sha),
            (context, approved_sha, "0" * 64),
        ] + [(record, approved_sha, context_digest(record)) for record in invalid_contexts]
        for index, (record, approval, context_approval) in enumerate(cases):
            with self.subTest(case=index):
                context_path.write_text(candidate.artifact.render_manifest_json(record))
                output = f.base / f"rejected-notes-{index}"
                with self.assertRaises(ValueError):
                    candidate.finalize_notes(f.output, f.source_sha, digest, notes, output,
                                             context_path, approval, context_approval)
                self.assertFalse(output.exists())
        context_path.write_text(candidate.artifact.render_manifest_json(context))
        notes.write_text("Changed after approval.\n")
        with self.assertRaisesRegex(ValueError, "changed after approval"):
            candidate.finalize_notes(f.output, f.source_sha, digest, notes,
                                     f.base / "edited-notes", context_path, approved_sha,
                                     approved_context_sha)

    def test_refinalizing_notes_does_not_inherit_previous_body_or_context_approval(self):
        f = self.fixture
        review = f.base / "source-notes"
        candidate.prepare_source_notes(f.root, f.source_sha, "stable", f.base_sha, review)
        build_sha = f.prepare("beta")
        notes, context = review / "notes.md", review / "notes-context.json"
        first = f.base / "first-final"
        first_sha = candidate.finalize_notes(
            f.output, f.source_sha, build_sha, notes, first, context,
            candidate.sha256(notes.read_bytes()), candidate.sha256(context.read_bytes()))
        first_receipt = candidate.verify(first, f.source_sha, first_sha)
        self.assertIn("notes_review_sha256", first_receipt)
        self.assertIn("notes_review_context_sha256", first_receipt)
        notes.write_text("## Changes\n\n- A newly reviewed recovery explanation.\n")
        second = f.base / "second-final"
        second_sha = candidate.finalize_notes(first, f.source_sha, first_sha, notes, second)
        receipt = candidate.verify(second, f.source_sha, second_sha)
        self.assertNotIn("notes_review_sha256", receipt)
        self.assertNotIn("notes_review_context_sha256", receipt)
        self.assertEqual((second / "assets/firmware.bin").read_bytes(), f.firmware.read_bytes())
        self.assertEqual((second / "assets/version.md").read_bytes(),
                         b"Version: 0.9.0-9\n\n" + notes.read_bytes()
                         + candidate.compliance_notice(receipt["compliance_bundle"]))

    def test_exact_source_version_target_digests_and_curated_notes(self):
        f = self.fixture
        receipt_sha = f.prepare()
        receipt = candidate.verify(f.output, f.source_sha, receipt_sha)
        manifest = json.loads((f.output / "catalog/manifest.json").read_text())
        self.assertEqual(receipt["source_sha"], f.source_sha)
        self.assertEqual(receipt["version"], "0.9.0-9")
        self.assertEqual(manifest["target"], "esp32s3")
        self.assertEqual(manifest["artifact"]["url"],
                         "https://updates.foxieproducts.com/fw/firmware_0.9.0-9_"
                         + manifest["artifact"]["sha256"] + ".bin")
        self.assertEqual(manifest["notes"]["url"],
                         "https://updates.foxieproducts.com/fw/version_0.9.0-9_"
                         + manifest["notes"]["sha256"] + ".md")
        self.assertEqual(manifest["artifact"]["sha256"], candidate.sha256(f.firmware.read_bytes()))
        self.assertNotEqual(manifest["artifact"]["sha256"], manifest["artifact"]["image_sha256"])
        self.assertEqual(set(receipt["input_sha256"]), set(candidate.BUILD_INPUTS))
        self.assertEqual(receipt["input_sha256"]["LICENSE"], candidate.sha256(FIXTURE_LICENSE))
        self.assertEqual(receipt["input_sha256"]["lib/litch-max/LICENSE"], candidate.sha256(FIXTURE_LICENSE))
        self.assertTrue({
            "tools/release/firmware_publication.py",
            "tools/release/publish_aws.py",
            ".github/workflows/firmware_host_check.yml",
        }.issubset(receipt["input_sha256"]))
        notes = (f.output / "assets/version.md").read_text()
        self.assertEqual(receipt["notes_base_sha"], f.base_sha)
        self.assertIn("Improves board recovery.", notes)
        self.assertIn("Arduino-ESP32 core under LGPL-2.1-or-later", notes)
        self.assertIn("NeoPixelBus under LGPL-3.0-or-later", notes)
        self.assertNotIn("fixture build input", notes)
        self.assertEqual(len(receipt["files"]), 6)

    def test_beta_uses_existing_app_channel_and_exact_endpoints(self):
        f = self.fixture
        receipt_sha = f.prepare("beta")
        candidate.verify(f.output, f.source_sha, receipt_sha)
        manifest = json.loads((f.output / "catalog/manifest_beta.json").read_text())
        self.assertEqual(manifest["channel"], "beta")
        self.assertEqual(manifest["artifact"]["url"],
                         "https://updates.foxieproducts.com/fw/firmware_beta_0.9.0-9_"
                         + manifest["artifact"]["sha256"] + ".bin")
        self.assertEqual(manifest["notes"]["url"],
                         "https://updates.foxieproducts.com/fw/version_beta_0.9.0-9_"
                         + manifest["notes"]["sha256"] + ".md")

    def test_changed_source_or_wrong_sha_cannot_prepare_a_catalog(self):
        f = self.fixture
        for wrong_sha in ("bad", "0" * 40):
            with self.subTest(sha=wrong_sha), self.assertRaises(ValueError):
                candidate.prepare(f.root, f.firmware, "stable", wrong_sha, f.base_sha, f.output)
        (f.root / "unexpected.cpp").write_text("untracked production input")
        with self.assertRaisesRegex(ValueError, "clean checkout"):
            f.prepare()
        self.assertFalse(f.output.exists())

    def test_product_changes_admit_without_manual_touch_and_keep_notes_base_guards(self):
        f = self.fixture
        screen = f.root / "app/lib/src/screens/example.dart"
        screen.parent.mkdir(parents=True, exist_ok=True)
        screen.write_text("// New user-facing behavior.\n")
        f.git("add", str(screen.relative_to(f.root)))
        f.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
              "commit", "--quiet", "-m", "Adds a visible app behavior.")
        f.source_sha = f.git("rev-parse", "HEAD").decode().strip()
        receipt = f.prepare()
        candidate.verify(f.output, f.source_sha, receipt)
        for base in ("bad", "0" * 40):
            rejected = f.output.parent / "bad-base-candidate"
            with self.subTest(base=base), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                candidate.prepare(f.root, f.firmware, "stable", f.source_sha, base, rejected, f.compliance)
            self.assertFalse(rejected.exists())

    def test_version_override_is_not_exposed_and_dirty_header_is_rejected(self):
        f = self.fixture
        (f.root / candidate.VERSION_HEADER).write_text('const char* FIRMWARE_VERSION = "9.9";')
        with self.assertRaisesRegex(ValueError, "clean checkout"):
            f.prepare()
        self.assertFalse(f.output.exists())

    def test_incomplete_or_wrong_target_bytes_never_create_candidate(self):
        f = self.fixture
        original = f.firmware.read_bytes()
        wrong_chip = bytearray(original)
        wrong_chip[12] = 0
        for data in (original[:-1], bytes(wrong_chip), original + b"extra"):
            f.firmware.write_bytes(data)
            with self.assertRaises(ValueError):
                f.prepare()
            self.assertFalse(f.output.exists())

    def test_existing_candidate_cannot_be_overwritten(self):
        f = self.fixture
        receipt_sha = f.prepare()
        with self.assertRaisesRegex(ValueError, "already exists"):
            f.prepare()
        candidate.verify(f.output, f.source_sha, receipt_sha)

    def test_unapproved_receipt_and_source_are_rejected(self):
        f = self.fixture
        receipt_sha = f.prepare()
        with self.assertRaisesRegex(ValueError, "approved digest"):
            candidate.verify(f.output, f.source_sha, "0" * 64)
        with self.assertRaisesRegex(ValueError, "wrong source"):
            candidate.verify(f.output, "0" * 40, receipt_sha)

    def test_every_missing_corrupt_or_extra_publication_file_fails_admission(self):
        f = self.fixture
        receipt_sha = f.prepare()
        receipt = candidate.verify(f.output, f.source_sha, receipt_sha)
        for relative in receipt["files"]:
            path = f.output / relative
            original = path.read_bytes()
            with self.subTest(file=relative, fault="corrupt"):
                path.write_bytes(original[:-1])
                with self.assertRaises((ValueError, OSError)):
                    candidate.verify(f.output, f.source_sha, receipt_sha)
            path.write_bytes(original)
            with self.subTest(file=relative, fault="missing"):
                path.unlink()
                with self.assertRaises((ValueError, OSError)):
                    candidate.verify(f.output, f.source_sha, receipt_sha)
            path.write_bytes(original)
        (f.output / "private-source.txt").write_text("must not be published")
        with self.assertRaisesRegex(ValueError, "unexpected"):
            candidate.verify(f.output, f.source_sha, receipt_sha)

    def test_symlink_cannot_escape_admission(self):
        f = self.fixture
        receipt_sha = f.prepare()
        path = f.output / "assets/firmware.bin"
        path.unlink()
        path.symlink_to(f.firmware)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            candidate.verify(f.output, f.source_sha, receipt_sha)

    def test_missing_or_wrong_readback_preserves_last_catalog(self):
        f = self.fixture
        receipt_sha = f.prepare()
        readback = f.base / "readback"
        shutil.copytree(f.output / "assets", readback)
        destination = f.base / "catalog"
        destination.mkdir()
        pointer = destination / "manifest.json"
        pointer.write_bytes(b"last known good")
        for asset in list(readback.iterdir()):
            original = asset.read_bytes()
            for fault in ("missing", "corrupt"):
                with self.subTest(file=asset.name, fault=fault):
                    asset.unlink(missing_ok=True)
                    if fault == "corrupt":
                        asset.write_bytes(b"wrong")
                    with self.assertRaisesRegex(ValueError, "catalog withheld"):
                        candidate.stage_catalog(f.output, f.source_sha, receipt_sha, readback, destination)
                    self.assertEqual(pointer.read_bytes(), b"last known good")
            asset.write_bytes(original)
        candidate.stage_catalog(f.output, f.source_sha, receipt_sha, readback, destination)
        self.assertEqual(pointer.read_bytes(), (f.output / "catalog/manifest.json").read_bytes())
        self.assertEqual(list(destination.iterdir()), [pointer])

    def test_cli_rejects_bad_receipt_with_no_partial_success(self):
        f = self.fixture
        f.prepare()
        result = subprocess.run([
            sys.executable, str(RELEASE_TOOLS / "official_candidate.py"), "verify",
            "--candidate", str(f.output), "--source-sha", f.source_sha,
            "--receipt-sha256", "0" * 64,
        ], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("candidate rejected", result.stderr)

    def test_editable_notes_freeze_without_rebuilding_or_changing_firmware(self):
        f = self.fixture
        original_sha = f.prepare()
        notes = f.base / "reviewed.md"
        notes.write_text("## Improvements\n\n- Board updates recover reliably after a dropped connection.\n")
        final = f.base / "final"
        final_sha = candidate.finalize_notes(f.output, f.source_sha, original_sha, notes, final)
        receipt = candidate.verify(final, f.source_sha, final_sha)
        self.assertEqual(receipt["notes_status"], "reviewed")
        self.assertEqual(receipt["build_receipt_sha256"], original_sha)
        self.assertEqual((final / "assets/firmware.bin").read_bytes(), f.firmware.read_bytes())
        self.assertEqual((final / "assets/version.md").read_bytes(), b"Version: 0.9.0-9\n\n"
                         + notes.read_bytes() + candidate.compliance_notice(receipt["compliance_bundle"]))
        original_manifest = json.loads((f.output / "catalog/manifest.json").read_text())
        final_manifest = json.loads((final / "catalog/manifest.json").read_text())
        self.assertEqual(original_manifest["artifact"], final_manifest["artifact"])
        self.assertNotEqual(original_manifest["notes"], final_manifest["notes"])
        notes.write_text("A later edit must not change the frozen app release.")
        candidate.verify(final, f.source_sha, final_sha)
        self.assertNotEqual((final / "assets/version.md").read_bytes(), notes.read_bytes())
        candidate.verify(f.output, f.source_sha, original_sha)

    def test_retained_beta_binary_gets_stable_range_notes_without_version_or_byte_changes(self):
        f = self.fixture
        stable_base, beta_base = f.base_sha, f.source_sha
        (f.root / "notes.md").write_text("Stable follow-up\n")
        f.git("add", "notes.md")
        f.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
              "commit", "--quiet", "-m", "Adds a useful board setting.")
        f.source_sha = f.git("rev-parse", "HEAD").decode().strip()
        f.base_sha = beta_base
        build_sha = f.prepare("beta")
        review = f.base / "stable-review"
        candidate.prepare_notes(f.root, f.output, f.source_sha, build_sha,
                                "stable", stable_base, review)
        generated = (review / "notes.md").read_text()
        self.assertIn("Improves board recovery.", generated)
        self.assertIn("Adds a useful board setting.", generated)
        beta_notes = (f.output / "assets/version_beta.md").read_text()
        self.assertNotIn("Improves board recovery.", beta_notes)
        self.assertIn("Adds a useful board setting.", beta_notes)
        (review / "notes.md").write_text("## Improvements\n\n- Customer-edited stable changes.\n")
        final = f.base / "stable-final"
        digest = candidate.finalize_notes(f.output, f.source_sha, build_sha,
                                         review / "notes.md", final, review / "notes-context.json")
        receipt = candidate.verify(final, f.source_sha, digest)
        self.assertEqual(receipt["channel"], "stable")
        self.assertEqual(receipt["notes_base_sha"], stable_base)
        self.assertEqual(receipt["build_receipt_sha256"], build_sha)
        self.assertEqual(receipt["version"], "0.9.0-9")
        compliance = receipt["compliance_bundle"]
        self.assertEqual(compliance, json.loads((f.output / "candidate.json").read_text())["compliance_bundle"])
        self.assertEqual((final / "assets" / compliance["filename"]).read_bytes(), f.compliance.read_bytes())
        self.assertTrue((final / "assets/version.md").read_bytes().endswith(candidate.compliance_notice(compliance)))
        self.assertEqual((final / "assets/firmware.bin").read_bytes(),
                         (f.output / "assets/firmware_beta.bin").read_bytes())
        before = json.loads((f.output / "catalog/manifest_beta.json").read_text())
        after = json.loads((final / "catalog/manifest.json").read_text())
        for key in ("sha256", "image_sha256", "size_bytes"):
            self.assertEqual(before["artifact"][key], after["artifact"][key])
        with self.assertRaisesRegex(ValueError, "overwrite owner edits"):
            candidate.prepare_notes(f.root, f.output, f.source_sha, build_sha,
                                    "stable", stable_base, review)

    def test_notes_context_cannot_substitute_another_source_receipt_or_version(self):
        f = self.fixture
        build_sha = f.prepare("beta")
        review = f.base / "stable-review"
        candidate.prepare_notes(f.root, f.output, f.source_sha, build_sha,
                                "stable", f.base_sha, review)
        context = review / "notes-context.json"
        original = json.loads(context.read_text())
        for key, value in (("source_sha", "0" * 40), ("build_receipt_sha256", "0" * 64),
                           ("version", "9.9"), ("channel", "preview"), ("notes_base_sha", "bad")):
            with self.subTest(key=key):
                context.write_text(json.dumps({**original, key: value}))
                with self.assertRaisesRegex(ValueError, "does not match"):
                    candidate.finalize_notes(f.output, f.source_sha, build_sha,
                                             review / "notes.md", f.base / "bad-final", context)
                self.assertFalse((f.base / "bad-final").exists())

    def test_notes_baseline_uses_latest_published_release_in_selected_channel(self):
        f = self.fixture
        f.git("tag", "fw-stable-0.7", f.base_sha)
        f.git("tag", "fw-beta-0.9.0-9", f.source_sha)
        releases = [
            {"draft": False, "tag_name": "fw-stable-0.7", "published_at": "2026-08-01"},
            {"draft": False, "tag_name": "fw-beta-0.9.0-9", "published_at": "2026-09-01"},
            {"draft": True, "tag_name": "fw-stable-future", "published_at": None},
        ]
        self.assertEqual(candidate.resolve_notes_base(f.root, releases, "stable", ""), f.base_sha)
        self.assertEqual(candidate.resolve_notes_base(f.root, releases, "beta", ""), f.source_sha)
        with self.assertRaisesRegex(ValueError, "must match"):
            candidate.resolve_notes_base(f.root, releases, "stable", f.source_sha)
        self.assertEqual(candidate.resolve_notes_base(f.root, [], "beta", f.base_sha), f.base_sha)
        self.assertEqual(candidate.resolve_notes_base(f.root, [], "beta", ""), f.base_sha)
        with self.assertRaisesRegex(ValueError, "public source root"):
            candidate.resolve_notes_base(f.root, [], "beta", "f" * 40)

    def test_workflow_admission_rejects_wrong_run_source_event_status_and_repository(self):
        import github_promotion as promotion
        repo = {"full_name": candidate.REPOSITORY}
        good = {"id": 123, "event": "workflow_dispatch", "status": "completed", "conclusion": "success",
                "path": ".github/workflows/firmware_candidate.yml", "head_sha": "a" * 40,
                "head_branch": "main", "run_attempt": 2, "repository": repo, "head_repository": repo}
        env = {"EXPECTED_SHA": "a" * 40, "RECEIPT_SHA": "b" * 64, "CANDIDATE_RUN_ATTEMPT": "2",
               "CANDIDATE_RUN_ID": "123"}
        job = {"name": "candidate", "status": "completed", "conclusion": "success", "head_sha": "a" * 40,
               "run_id": 123}
        with patch.dict(os.environ, env):
            for suffix in ("", "@main", "@refs/heads/main"):
                run = {**good, "path": good["path"] + suffix}
                with patch.object(promotion, "api", side_effect=[run, {"jobs": [job]}]):
                    promotion.admit_build()
            for key, value in (("event", "pull_request"), ("head_sha", "b" * 40), ("head_branch", "feature"),
                               ("run_attempt", 1), ("path", "other.yml"), ("conclusion", "failure"),
                               ("repository", {"full_name": "other/fork"}), ("head_repository", {"full_name": "other/fork"})):
                with self.subTest(key=key), patch.object(promotion, "api", return_value={**good, key: value}):
                    with self.assertRaises(ValueError):
                        promotion.admit_build()
            for key, value in (("name", "draft"), ("run_id", 1), ("head_sha", "b" * 40), ("conclusion", "failure")):
                with self.subTest(job=key), patch.object(promotion, "api", side_effect=[good, {"jobs": [{**job, key: value}]}]):
                    with self.assertRaises(ValueError):
                        promotion.admit_build()

    def test_empty_notes_cannot_finalize_and_wrong_base_cannot_generate(self):
        f = self.fixture
        digest = f.prepare()
        notes = f.base / "blank.md"
        notes.write_text("  \n")
        with self.assertRaisesRegex(ValueError, "blank"):
            candidate.finalize_notes(f.output, f.source_sha, digest, notes, f.base / "final")
        with self.assertRaisesRegex(ValueError, "no commits"):
            candidate.draft_notes(f.root, f.source_sha, f.source_sha)
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            candidate.draft_notes(f.root, f.source_sha, "0" * 40)

    def test_local_checks_keep_hosted_build_and_release_admission_mandatory(self):
        source = (RELEASE_TOOLS.parents[1] / ".github/workflows/firmware_candidate.yml").read_text()
        jobs = dict(re.findall(r"(?ms)^  ([a-z][a-z_-]*):\n(.*?)(?=^  [a-z][a-z_-]*:|\Z)",
                               source.split("\njobs:\n", 1)[1]))
        self.assertEqual(set(jobs), {"candidate", "admit", "draft", "promote"})
        self.assertIn("./litch build s3 --compile-only", jobs["candidate"])
        self.assertIn("tools/release/verify_release_artifacts.py", jobs["candidate"])
        self.assertIn("--local-validation-ref", jobs["candidate"])
        self.assertIn("github_promotion.py admit-build", jobs["admit"])
        for name in ("candidate", "admit"):
            self.assertNotIn("contents: write", jobs[name])
            self.assertNotIn("id-token: write", jobs[name])
        admit_steps = re.split(r"(?m)^      - ", jobs["admit"])[1:]
        retained_steps = [step for step in admit_steps
                          if step.startswith("uses: actions/download-artifact@")
                          or step.startswith("name: Verify the retained bytes\n")]
        self.assertEqual(len(retained_steps), 2)
        for step in retained_steps:
            self.assertIn("if: inputs.operation != 'verify' && inputs.operation != 'verify-archive'", step)
        for name in ("draft", "promote"):
            self.assertIn("needs: admit", jobs[name])
            self.assertIn("environment: firmware-publishing", jobs[name])
        self.assertNotIn("id-token: write", jobs["draft"])
        self.assertIn("id-token: write", jobs["promote"])
        self.assertNotIn("pull_request", source)

    def test_workflow_installs_the_canonical_esptool_requirement(self):
        source = (
            RELEASE_TOOLS.parents[1]
            / ".github/workflows/firmware_candidate.yml"
        ).read_text()
        self.assertIn(
            "from tools.dev.tool_requirements import ESPTOOL_REQUIREMENT",
            source,
        )
        self.assertNotIn("esptool_install_argv", source)



def workflow_step(name):
    source = (RELEASE_TOOLS.parents[1] / ".github/workflows/firmware_candidate.yml").read_text()
    block = source.split(f"      - name: {name}\n", 1)[1].split("\n      - ", 1)[0]
    block = re.split(r"\n  [a-z][a-z_-]*:\n", block, maxsplit=1)[0]
    return textwrap.dedent(block.split("        run: |\n", 1)[1])


class WorkflowRecoveryTests(unittest.TestCase):
    """Run real workflow commands with local GitHub/artifact service seams."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.f = CandidateFixture(temporary.name)
        with patch.dict(os.environ, {"GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1"}):
            self.receipt_sha = self.f.prepare("beta")
        self.runner = self.f.base / "runner"
        self.runner.mkdir()
        self.bin = self.f.base / "bin"
        self.bin.mkdir()
        (self.bin / "python").symlink_to(sys.executable)
        self.state = self.f.base / "github.json"
        self.save({"releases": {}, "calls": []})
        stub = self.bin / "gh"
        stub.write_text("#!" + sys.executable + "\n" + textwrap.dedent('''
            import base64, hashlib, json, os, sys
            from pathlib import Path
            path = Path(os.environ['FAKE_GITHUB_STATE'])
            state = json.loads(path.read_text())
            args = sys.argv[1:]
            state['calls'].append(args)
            def save(): path.write_text(json.dumps(state))
            def option(name): return args[args.index(name) + 1]
            if args[0] == 'api':
                endpoint = next(value for value in args if value.startswith('repos/'))
                if endpoint.endswith('/releases?per_page=100'):
                    releases = []
                    for tag, release in state['releases'].items():
                        assets = [{'name': name, 'size': len(base64.b64decode(data)),
                                   'digest': 'sha256:' + hashlib.sha256(base64.b64decode(data)).hexdigest()}
                                  for name, data in release['assets'].items()]
                        releases.append({'tag_name': tag, 'draft': release['isDraft'],
                                         'prerelease': release.get('prerelease', False),
                                         'target_commitish': release['targetCommitish'],
                                         'body': release['body'], 'assets': assets,
                                         'published_at': release.get('published_at'),
                                         'html_url': 'https://example.invalid/release'})
                    print(json.dumps([releases]))
                elif '/branches/' in endpoint:
                    print(json.dumps({'commit': {'sha': os.environ['GITHUB_SHA']}}))
                elif '/git/trees/' in endpoint:
                    tree = ([{'path': '.github', 'type': 'tree', 'sha': 'github-tree'}]
                            if endpoint.endswith(os.environ['GITHUB_SHA']) else
                            [{'path': 'workflows', 'type': 'tree', 'sha': os.environ['FAKE_WORKFLOW_TREE']}])
                    print(json.dumps({'tree': tree}))
                elif '/git/ref/tags/' in endpoint:
                    print(json.dumps({'object': {'type': 'commit', 'sha': os.environ['GITHUB_SHA']}}))
                elif '/attempts/' in endpoint:
                    expected = '/attempts/' + os.environ['CANDIDATE_RUN_ATTEMPT'] + '/jobs?per_page=100'
                    assert endpoint.endswith(expected)
                    print(json.dumps(state['jobs']))
                else: print(json.dumps(state['run']))
                save(); raise SystemExit(0)
            if args[0] != 'release': raise SystemExit('Unexpected GitHub command')
            verb, tag = args[1:3]
            release = state['releases'].get(tag)
            if verb == 'create':
                if state.pop('fail_create', False):
                    save(); raise SystemExit('Injected draft creation failure')
                if release: save(); raise SystemExit('Draft already exists')
                release = {'isDraft': True, 'targetCommitish': option('--target'),
                           'body': Path(option('--notes-file')).read_text(), 'assets': {}}
                state['releases'][tag] = release
                for asset in args[3:]:
                    if Path(asset).is_file():
                        release['assets'][Path(asset).name] = base64.b64encode(Path(asset).read_bytes()).decode()
            elif not release:
                save(); raise SystemExit('Release not found')
            elif verb == 'view':
                result = {key: release.get(key) for key in option('--json').split(',')}
                if '--jq' in args: print('https://example.invalid/release')
                else: print(json.dumps(result))
            elif verb == 'upload':
                for asset in args[3:]:
                    if Path(asset).is_file():
                        release['assets'][Path(asset).name] = base64.b64encode(Path(asset).read_bytes()).decode()
                if state.get('edit_during_upload'):
                    release['body'] = state.pop('edit_during_upload')
            elif verb == 'download':
                name = option('--pattern'); destination = Path(option('--dir'))
                destination.mkdir(parents=True, exist_ok=True)
                target = destination / name
                if target.exists():
                    save(); raise SystemExit('Readback file already exists')
                failures = state.get('download_failures', {}).get(name, [])
                if failures:
                    failure = failures.pop(0)
                    target.write_bytes(b'partial download')
                    save(); raise SystemExit(failure)
                data = base64.b64decode(release['assets'][name])
                if state.get('corrupt_download') == name:
                    data = bytes([data[0] ^ 1]) + data[1:]
                target.write_bytes(data)
            elif verb == 'edit':
                if '--notes-file' in args: release['body'] = Path(option('--notes-file')).read_text()
                if state.get('edit_during_publish'):
                    release['body'] = state.pop('edit_during_publish')
                release['isDraft'] = False
                release['prerelease'] = '--prerelease=true' in args
                state['releases'][option('--tag')] = state['releases'].pop(tag)
            else: raise SystemExit('Unexpected release operation')
            save()
        '''))
        stub.chmod(0o755)
        self.env = {**os.environ, "PATH": str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
                    "PYTHONPATH": str(RELEASE_TOOLS),
                    "FAKE_GITHUB_STATE": str(self.state), "RUNNER_TEMP": str(self.runner),
                    "GITHUB_REPOSITORY": candidate.REPOSITORY, "GITHUB_RUN_ID": "123",
                    "GITHUB_RUN_ATTEMPT": "1", "GITHUB_SHA": self.f.source_sha,
                    "BUILD_ATTEMPT": "1", "RELEASE_CHANNEL": "beta", "RECEIPT_SHA": self.receipt_sha,
                    "EXPECTED_SHA": self.f.source_sha, "DRAFT_TAG": "candidate-beta-123-1",
                    "CANDIDATE_RUN_ID": "123", "CANDIDATE_RUN_ATTEMPT": "1",
                    "DEFAULT_BRANCH": "main", "BOOTSTRAP_BASE": self.f.base_sha,
                    "OPERATION": "promote", "NOTES_SHA256": candidate.sha256(b"Reviewed customer notes.\n"), "GITHUB_ENV": str(self.runner / "env"),
                    "FAKE_WORKFLOW_TREE": self.f.git('rev-parse', self.f.source_sha + ':.github/workflows').decode().strip(),
                    "GITHUB_STEP_SUMMARY": str(self.runner / "summary.md")}

    def save(self, value):
        self.state.write_text(json.dumps(value))

    def load(self):
        return json.loads(self.state.read_text())

    def previous_beta(self):
        data = json.loads((self.f.output / "catalog/manifest_beta.json").read_bytes())
        data["version"] = "0.9.0-8"
        for key in ("artifact", "notes"):
            data[key]["url"] = data[key]["url"].replace("0.9.0-9", data["version"])
        return json.dumps(data).encode()

    def execute(self, name, public_read=None):
        if name in ('Resolve reviewed notes or recover the exact frozen promotion',
                    'Archive frozen assets before app publication',
                    'Record the official release after verified app delivery'):
            import io
            from contextlib import redirect_stdout, redirect_stderr
            import github_promotion

            command = workflow_step(name).strip().split()[-1]
            output, errors = io.StringIO(), io.StringIO()
            def read(name, maximum, missing_ok=False):
                paths = list((self.runner / 'firmware-final').glob('*/' + name))
                if paths:
                    return paths[0].read_bytes()
                return self.previous_beta() if name == 'manifest_beta.json' else None
            previous = Path.cwd()
            try:
                os.chdir(self.f.root)
                with patch.dict(os.environ, self.env), patch.object(sys, 'argv', ['github_promotion.py', command]), \
                        patch.object(github_promotion, 'public_read', side_effect=public_read or read), \
                        redirect_stdout(output), redirect_stderr(errors):
                    code = github_promotion.main()
            finally:
                os.chdir(previous)
            return subprocess.CompletedProcess(command, code, output.getvalue(), errors.getvalue())
        return subprocess.run(["bash", "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", workflow_step(name)],
                              cwd=self.f.base, env=self.env, text=True, capture_output=True, timeout=20)

    def setup_recording(self):
        shutil.copytree(self.f.output, self.runner / "firmware-candidate")
        notes = self.runner / "reviewed-notes.md"
        notes.write_text("Reviewed customer notes.\n")
        context = {'schema_version': 1, 'source_sha': self.f.source_sha, 'channel': 'beta',
                   'notes_base_sha': self.f.base_sha, 'version': '0.9.0-9',
                   'build_receipt_sha256': self.receipt_sha}
        import base64
        state = self.load()
        # This transaction now enters through version/baseline admission rather
        # than bypassing it at the final recording shell.
        self.f.git('tag', 'fw-beta-0.9.0-8', self.f.base_sha)
        state['releases']['fw-beta-0.9.0-8'] = {
            'isDraft': False, 'targetCommitish': self.f.base_sha,
            'body': 'Previous official beta', 'assets': {}, 'prerelease': True,
            'published_at': '2026-08-01T00:00:00Z',
        }
        state['releases'][self.env['DRAFT_TAG']] = {
            'isDraft': True, 'targetCommitish': self.f.source_sha,
            'body': notes.read_text(), 'assets': {'notes-context.json': base64.b64encode(json.dumps(context).encode()).decode()},
        }
        self.save(state)
        prepared = self.execute('Resolve reviewed notes or recover the exact frozen promotion')
        self.assertEqual(prepared.returncode, 0, prepared.stdout + prepared.stderr)

    def test_recording_publishes_matching_notes_and_verified_assets(self):
        self.setup_recording()
        archived = self.execute('Archive frozen assets before app publication')
        self.assertEqual(archived.returncode, 0, archived.stdout + archived.stderr)
        result = self.execute("Record the official release after verified app delivery")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        release = self.load()['releases']['fw-beta-0.9.0-9']
        self.assertFalse(release['isDraft'])
        self.assertEqual(release['body'], (self.runner / 'reviewed-notes.md').read_text())
        self.assertIn('candidate.json', release['assets'])
        self.assertIn('manifest_beta.json', release['assets'])
        receipt = json.loads((self.runner / 'firmware-final/candidate.json').read_text())
        self.assertIn(receipt['compliance_bundle']['filename'], release['assets'])
        # Expired Actions bytes and a later channel publication must not prevent
        # verification of this archived identity or permit service mutations.
        import base64
        assets = {name: base64.b64decode(data) for name, data in release['assets'].items()}
        manifest = json.loads(assets['manifest_beta.json'])
        immutable = {Path(manifest[key]['url']).name for key in ('artifact', 'notes')}
        immutable.add(receipt['compliance_bundle']['filename'])
        cases = (
            ('verify', self.receipt_sha, ('123', '1'), None, None),
            ('verify', self.receipt_sha, ('123', '1'), 'manifest_beta.json', 'incomplete active app delivery'),
            ('verify', self.receipt_sha, ('123', '1'), 'missing catalog', 'incomplete active app delivery'),
            ('verify', self.receipt_sha, ('123', '1'), 'firmware_beta.bin', 'asset readback mismatch'),
            ('verify-archive', self.receipt_sha, ('123', '1'), None, None),
            ('verify-archive', '0' * 64, ('123', '1'), None, 'bound to the approved build receipt'),
            ('verify-archive', self.receipt_sha, ('456', '1'), None, 'approved build execution'),
            ('verify-archive', self.receipt_sha, ('123', '2'), None, 'approved build execution'),
        )
        for operation, digest, execution, corrupt, error in cases:
            with self.subTest(operation=operation, digest=digest, execution=execution, corrupt=corrupt):
                if operation == 'verify-archive':
                    state = self.load()
                    state['releases']['fw-beta-0.9.0-10'] = {
                        'isDraft': False, 'targetCommitish': 'b' * 40, 'body': 'Later beta',
                        'assets': {}, 'prerelease': True, 'published_at': '2026-09-01T00:00:00Z',
                    }
                    self.save(state)
                for path in ('firmware-candidate', 'firmware-final', 'archive', 'archive-readback',
                             'release-readback', 'archived-build-inputs'):
                    shutil.rmtree(self.runner / path, ignore_errors=True)
                for path in ('notes-context.json', 'reviewed-notes.md'):
                    (self.runner / path).unlink(missing_ok=True)
                self.assertFalse((self.runner / 'firmware-candidate').exists())
                self.env.update(OPERATION=operation, RECEIPT_SHA=digest,
                                CANDIDATE_RUN_ID=execution[0], CANDIDATE_RUN_ATTEMPT=execution[1])
                reads = []
                def read(name, maximum, missing_ok=False):
                    # Current catalogs/aliases can now point elsewhere. Only
                    # verify-archive may ignore their current state.
                    self.assertIn(name, immutable if operation == 'verify-archive' else assets)
                    reads.append(name)
                    if corrupt == 'missing catalog' and name == 'manifest_beta.json':
                        self.assertTrue(missing_ok)
                        return None
                    if name == corrupt:
                        return b'changed public bytes'
                    self.assertLessEqual(len(assets[name]), maximum)
                    return assets[name]
                before = len(self.load()['calls'])
                recovered = self.execute('Resolve reviewed notes or recover the exact frozen promotion', read)
                self.assertEqual(recovered.returncode, 2 if error else 0, recovered.stdout + recovered.stderr)
                if not error:
                    expected = immutable if operation == 'verify-archive' else {
                        name for name in assets if name not in ('candidate.json', 'build-candidate.json', 'notes-context.json')}
                    self.assertEqual(set(reads), expected)
                    self.assertEqual((self.runner / 'firmware-candidate/candidate.json').read_bytes(),
                                     assets['build-candidate.json'])
                else:
                    self.assertIn(error, recovered.stdout)
                    if corrupt is None:
                        self.assertEqual(reads, [])
                self.assertFalse(any(call[:2] in (['release', 'upload'], ['release', 'edit'], ['release', 'create'])
                                     for call in self.load()['calls'][before:]))

    def test_historical_public_release_verifies_read_only_and_all_mutating_entries_refuse(self):
        import base64
        import github_promotion

        self.receipt_sha = self.f.historical()
        self.env['RECEIPT_SHA'] = self.receipt_sha
        notes = self.f.base / 'historical-reviewed.md'
        notes.write_text('Historical reviewed customer notes.\n')
        context = self.f.base / 'historical-context.json'
        context.write_text(json.dumps({
            'schema_version': 1, 'source_sha': self.f.source_sha, 'channel': 'beta',
            'notes_base_sha': self.f.base_sha, 'version': '0.9.0-9',
            'build_receipt_sha256': self.receipt_sha,
        }))
        saved = self.f.base / 'historical-final'
        candidate.finalize_notes(self.f.output, self.f.source_sha, self.receipt_sha,
                                 notes, saved, context, historical_read_only=True)
        paths = {path.name: path.read_bytes() for path in saved.rglob('*') if path.is_file()}
        paths['notes-context.json'] = context.read_bytes()
        paths['build-candidate.json'] = (self.f.output / 'candidate.json').read_bytes()
        state = self.load()
        state['releases']['fw-beta-0.9.0-9'] = {
            'isDraft': False, 'targetCommitish': self.f.source_sha,
            'body': notes.read_text(), 'prerelease': True,
            'published_at': '2026-08-01T00:00:00Z',
            'assets': {name: base64.b64encode(data).decode() for name, data in paths.items()},
        }
        self.save(state)
        self.env['OPERATION'] = 'verify'
        self.assertFalse((self.runner / 'firmware-candidate').exists())
        result = self.execute('Resolve reviewed notes or recover the exact frozen promotion')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('does not bind an exact GitHub run/attempt', result.stdout)
        self.assertEqual((self.runner / 'firmware-candidate/candidate.json').read_bytes(),
                         paths['build-candidate.json'])
        self.assertFalse(any(call[:2] in (['release', 'upload'], ['release', 'edit'], ['release', 'create'])
                             for call in self.load()['calls']))
        before = len(self.load()['calls'])
        with patch.dict(os.environ, self.env):
            for method in ('archive', 'ready', 'finish'):
                with self.subTest(method=method), self.assertRaisesRegex(ValueError, 'historical candidates are read-only'):
                    getattr(github_promotion.Promotion(), method)()
            for operation in ('promote', 'prepare-notes'):
                with self.subTest(operation=operation), self.assertRaisesRegex(ValueError, 'historical candidates are read-only'):
                    github_promotion.Promotion().prepare(operation)
        self.assertEqual(len(self.load()['calls']), before)

    def test_archive_retries_transient_asset_read_without_reuploading(self):
        self.setup_recording()
        state = self.load()
        name = 'firmware_beta.bin'
        state['download_failures'] = {name: ['HTTP 500: Internal Server Error']}
        self.save(state)
        with patch('github_promotion.sleep') as delay:
            result = self.execute('Archive frozen assets before app publication')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        state = self.load()
        downloads = [call for call in state['calls'] if call[:2] == ['release', 'download']
                     and call[call.index('--pattern') + 1] == name]
        self.assertEqual(len(downloads), 2)
        self.assertEqual(downloads[0], downloads[1])
        self.assertEqual(sum(call[:2] == ['release', 'upload'] and any(Path(arg).name == name for arg in call)
                             for call in state['calls']), 1)
        delay.assert_called_once_with(1)
        self.assertEqual((self.runner / 'archive-readback' / name).read_bytes(),
                         (self.runner / 'firmware-final/assets' / name).read_bytes())
        self.assertIn('candidate.json', state['releases'][self.env['DRAFT_TAG']]['assets'])

    def test_asset_readback_exhaustion_and_permanent_failures_stop_without_partial_output(self):
        import github_promotion
        self.setup_recording()
        name = 'notes-context.json'
        cases = (
            (['HTTP 503: Service Unavailable'] * 3, False, 3),
            (['HTTP 401: Bad credentials'], False, 1),
            (['HTTP 403: Forbidden'], False, 1),
            (['HTTP 404: Not Found'], False, 1),
            ([], True, 1),
        )
        with patch.dict(os.environ, self.env), patch('github_promotion.sleep'):
            promotion = github_promotion.Promotion()
            release = promotion.resolve(promotion.releases())
            for index, (failures, corrupt, expected_calls) in enumerate(cases):
                with self.subTest(failures=failures, corrupt=corrupt):
                    state = self.load()
                    before = len(state['calls'])
                    state['download_failures'] = {name: list(failures)}
                    state['corrupt_download'] = name if corrupt else None
                    self.save(state)
                    destination = self.runner / f'failed-readback-{index}'
                    destination.mkdir()
                    protected = destination / 'existing.txt'
                    protected.write_bytes(b'preserve')
                    with self.assertRaises(github_promotion.PublicationError):
                        promotion.download(release, name, destination)
                    calls = self.load()['calls'][before:]
                    self.assertEqual(len(calls), expected_calls)
                    self.assertTrue(all(call[:2] == ['release', 'download'] for call in calls))
                    self.assertEqual(list(destination.iterdir()), [protected])
                    self.assertEqual(protected.read_bytes(), b'preserve')
            destination = self.runner / 'timed-out-readback'
            with patch.object(github_promotion, 'gh', side_effect=subprocess.TimeoutExpired('gh', 120)) as download:
                with self.assertRaises(subprocess.TimeoutExpired):
                    promotion.download(release, name, destination)
            self.assertEqual(download.call_count, 3)
            self.assertEqual(list(destination.iterdir()), [])
            destination = self.runner / 'missing-metadata-readback'
            before = len(self.load()['calls'])
            with self.assertRaisesRegex(github_promotion.PublicationError, 'missing from release metadata'):
                promotion.download({**release, 'assets': []}, name, destination)
            self.assertEqual(len(self.load()['calls']), before)
            self.assertEqual(list(destination.iterdir()), [])

    def test_owner_edit_during_actual_asset_upload_survives_incomplete_recording(self):
        self.setup_recording()
        state = self.load()
        state['edit_during_upload'] = 'Owner edit made while release assets upload.\n'
        self.save(state)
        result = self.execute('Archive frozen assets before app publication')
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        release = self.load()['releases'][self.env['DRAFT_TAG']]
        self.assertEqual(release['body'], state['edit_during_upload'])
        self.assertTrue(release['isDraft'])
        self.assertIn('Owner notes changed', result.stdout)
        self.assertFalse(any(call[1] == 'edit' for call in self.load()['calls']))

    def test_last_moment_edit_is_preserved_and_detected_after_record_publication(self):
        self.setup_recording()
        archived = self.execute('Archive frozen assets before app publication')
        self.assertEqual(archived.returncode, 0, archived.stdout + archived.stderr)
        state = self.load()
        state['edit_during_publish'] = 'Owner edit during the final API request.\n'
        self.save(state)
        result = self.execute("Record the official release after verified app delivery")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.load()['releases']['fw-beta-0.9.0-9']['body'], state['edit_during_publish'])
        self.assertFalse(any('--notes-file' in call for call in self.load()['calls'] if call[1] == 'edit'))

    def test_successful_build_failed_draft_and_failed_job_retry_keep_original_identity(self):
        shutil.copytree(self.f.output, self.runner / "firmware-candidate")
        self.env["OPERATION"] = "prepare-notes"
        state = self.load()
        state["fail_create"] = True
        self.save(state)
        # The fixture uses 0.9.0-9. The deployed 0.9.0-8 catalog is the only
        # version authority during first-repository bootstrap.
        import github_promotion
        live = self.previous_beta()
        previous = Path.cwd()
        try:
            os.chdir(self.f.root)
            with patch.dict(os.environ, self.env), patch.object(github_promotion, "public_read", return_value=live):
                first = github_promotion.Promotion()
                with self.assertRaisesRegex(ValueError, "draft creation failure"):
                    first.prepare("prepare-notes")
                shutil.rmtree(self.runner / "channel-notes")
                github_promotion.Promotion().prepare("prepare-notes")
                state = self.load()
                release = state["releases"][self.env["DRAFT_TAG"]]
                release["body"] = "Owner edits."
                self.save(state)
                # Stable-first adoption must use the very same original receipt.
                import base64
                state["releases"]["fw-stable-0.9.0-9"] = {
                    **release, "isDraft": False, "published_at": "2026-08-01T00:00:00Z",
                    "assets": {"build-candidate.json": base64.b64encode(b"foreign build").decode()}}
                self.save(state)
                with self.assertRaisesRegex(ValueError, "original build"):
                    github_promotion.Promotion().prepare("prepare-notes")
                original = (self.f.output / "candidate.json").read_bytes()
                state["releases"]["fw-stable-0.9.0-9"]["assets"]["build-candidate.json"] = base64.b64encode(original).decode()
                self.save(state)
                github_promotion.Promotion().prepare("prepare-notes")
                self.assertEqual(self.load()["releases"][self.env["DRAFT_TAG"]]["body"], "Owner edits.")
        finally:
            os.chdir(previous)


if __name__ == "__main__":
    unittest.main()
