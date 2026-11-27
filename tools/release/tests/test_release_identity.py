from __future__ import annotations

import json
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import release_identity as identity


def published(version, *, channel="beta", draft=False, date="2026-09-07T12:00:00Z"):
    return {"tag_name": f"fw-{channel}-{version}", "draft": draft,
            "published_at": None if draft else date}


def manifest(version):
    return {"schema_version": 1, "channel": "beta", "target": "esp32s3", "version": version}


class ReleaseIdentityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in identity.SOURCE_PATHS:
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(RELEASE_TOOLS.parents[1] / name, target)
        # Fixed pre-policy inputs; tests remain meaningful after real preparation.
        (self.root / identity.HEADER).write_text('const char* FIRMWARE_VERSION = "0.8";\n')
        (self.root / identity.PUBSPEC).write_text("version: 0.6.0+2\n")
        app = self.root / identity.APP_IDENTITY
        app.write_text(identity.APP_VERSION.sub(
            lambda m: (m[1] + "  version: '',\n  nativeVersion: '0.6.0',\n"
                       "  buildNumber: '2',\n" + m[5]), app.read_text()))

    def prepare(self, version="0.9.0-2", code=3, previous=2, releases=None, current=None):
        return identity.prepare(self.root, version, code, previous,
                                releases if releases is not None else [published("0.8")],
                                current if current is not None else manifest("0.8"))

    def test_no_identity_is_inferred_from_unprepared_source(self):
        with self.assertRaisesRegex(ValueError, "0.9.0-N"):
            identity.check(self.root)

    def test_preparation_sets_all_sources_and_native_code_is_independent_of_n(self):
        prepared = self.prepare()
        self.assertEqual(prepared, {"version": "0.9.0-2", "native_version": "0.9.0", "package_code": 3})
        self.assertEqual(identity.check(self.root), prepared)
        self.assertIn("version: 0.9.0+3\n", (self.root / identity.PUBSPEC).read_text())
        self.assertIn("version: '0.9.0-2'", (self.root / identity.APP_IDENTITY).read_text())

    def test_local_build_retry_preparation_is_byte_identical_and_does_not_write(self):
        self.prepare()
        before = {name: ((self.root / name).read_bytes(), (self.root / name).stat().st_mtime_ns)
                  for name in identity.SOURCE_PATHS}
        self.prepare()
        self.assertEqual(before, {name: ((self.root / name).read_bytes(), (self.root / name).stat().st_mtime_ns)
                                  for name in identity.SOURCE_PATHS})

    def test_native_code_cannot_go_backwards_or_reuse_uploaded_code_even_for_n1(self):
        for code, floor in ((1, 2), (2, 2), (3, 3), (4, 5), (10000, 2)):
            with self.subTest(code=code, floor=floor), self.assertRaisesRegex(ValueError, "native package code"):
                self.prepare(code=code, previous=floor)
        self.prepare(code=8, previous=7)
        with self.assertRaisesRegex(ValueError, "native package code"):
            self.prepare(code=7, previous=2)

    def test_only_new_published_betas_advance_numeric_sequence_across_9_and_10(self):
        releases = [published("0.8"), published("0.9.0-9"),
                    published("0.9.0-77", draft=True), published("0.9.0-88", channel="stable"),
                    {"tag_name": "candidate-beta-local-build-999", "draft": False, "published_at": "2026"}]
        self.assertEqual(identity.next_beta_version(releases, series="0.9.0"), "0.9.0-10")
        identity.check_next_beta("0.9.0-10", releases)
        identity.check_next_beta("0.9.0-10", releases)  # A retry consumes nothing.
        releases.insert(0, published("0.9.0-10"))
        self.assertEqual(identity.next_beta_version([releases[:2], releases[2:]], series="0.9.0"), "0.9.0-11")
        with self.assertRaisesRegex(ValueError, "already published"):
            identity.check_next_beta("0.9.0-10", releases)

    def test_reserved_unused_second_beta_does_not_invent_a_first_release(self):
        history = [published("0.8.3-1")]
        identity.check_next_beta("0.9.0-2", history)
        self.assertEqual(identity.next_beta_version(history, series="0.9.0"), "0.9.0-1")
        self.assertEqual(history, [published("0.8.3-1")])
        for channel in ("beta", "stable"):
            with self.subTest(channel=channel), self.assertRaisesRegex(ValueError, "already published"):
                identity.check_next_beta("0.9.0-2", history + [published("0.9.0-2", channel=channel)])

    def test_reserved_third_identity_after_second_stable_preserves_history_guards(self):
        history = [published("0.8.3-1"), published("0.9.0-2", channel="stable")]
        before = json.loads(json.dumps(history))
        identity.check_next_beta("0.9.0-3", history)
        self.assertEqual(identity.next_beta_version(history, series="0.9.0"), "0.9.0-1")
        self.assertEqual(history, before)
        for channel in ("beta", "stable"):
            with self.subTest(channel=channel), self.assertRaisesRegex(ValueError, "already published"):
                identity.check_next_beta("0.9.0-3", history + [published("0.9.0-3", channel=channel)])
        for version, records in (("0.9.0-8", history), ("0.9.0-3", [published("0.9.0-4")])):
            with self.subTest(version=version, records=records), self.assertRaisesRegex(ValueError, "next unpublished beta"):
                identity.check_next_beta(version, records)
        self.prepare()
        with self.assertRaisesRegex(ValueError, "native package code"):
            self.prepare(version="0.9.0-3", code=3, previous=3,
                         releases=history, current=manifest("0.8.3-1"))
        prepared = self.prepare(version="0.9.0-3", code=4, previous=3,
                                releases=history, current=manifest("0.8.3-1"))
        self.assertEqual(prepared, {"version": "0.9.0-3", "native_version": "0.9.0", "package_code": 4})

    def test_selected_stable_four_and_beta_five_keep_used_version_guards(self):
        history = [published("0.9.0-3")]
        identity.check_next_beta("0.9.0-4", history)
        history.append(published("0.9.0-4", channel="stable"))
        identity.check_next_beta("0.9.0-5", history)
        self.assertEqual(identity.next_beta_version(history, series="0.9.0"), "0.9.0-4")
        for version in ("0.9.0-4", "0.9.0-5"):
            for channel in ("stable", "beta"):
                with self.subTest(version=version, channel=channel), self.assertRaisesRegex(ValueError, "already published"):
                    identity.check_next_beta(version, history + [published(version, channel=channel)])

    def test_selected_six_seven_pair_keeps_history_and_native_code_guards(self):
        history = [published("0.9.0-5")]
        identity.check_next_beta("0.9.0-6", history)
        history.append(published("0.9.0-6", channel="stable"))
        before = json.loads(json.dumps(history))
        identity.check_next_beta("0.9.0-7", history)
        self.assertEqual(identity.next_beta_version(history, series="0.9.0"), "0.9.0-6")
        self.assertEqual(history, before)
        for version in ("0.9.0-6", "0.9.0-7"):
            for channel in ("stable", "beta"):
                with self.subTest(version=version, channel=channel):
                    with self.assertRaisesRegex(ValueError, "already published"):
                        identity.check_next_beta(
                            version, [published(version, channel=channel)],
                        )
        with self.assertRaisesRegex(ValueError, "next unpublished beta"):
            identity.check_next_beta("0.9.0-7", [published("0.9.0-8")])
        self.prepare(version="0.9.0-6", code=12, previous=11,
                     releases=[published("0.9.0-5")], current=manifest("0.9.0-5"))
        with self.assertRaisesRegex(ValueError, "native package code"):
            self.prepare(version="0.9.0-7", code=12, previous=11,
                         releases=history, current=manifest("0.9.0-5"))
        self.assertEqual(
            self.prepare(version="0.9.0-7", code=13, previous=11,
                         releases=history, current=manifest("0.9.0-5")),
            {"version": "0.9.0-7", "native_version": "0.9.0", "package_code": 13},
        )

    def recovered_manual_publication(self):
        current = {**manifest("0.8.9-1"), "chip_id": 9,
                   "artifact": {"sha256": "a" * 64, "image_sha256": "b" * 64,
                                "size_bytes": 128, "url": identity.PUBLIC_BASE_URL + "/firmware_beta_0.8.9-1_" + "a" * 64 + ".bin"},
                   "notes": {"sha256": "c" * 64, "size_bytes": 64,
                             "url": identity.PUBLIC_BASE_URL + "/version_beta_0.8.9-1_" + "c" * 64 + ".md"}}
        payloads = {}
        for channel in ("stable", "beta"):
            names = identity.artifact.release_names(channel, current["version"], "a" * 64, "c" * 64)
            for kind in ("firmware_alias", "firmware_immutable"):
                payloads[names[kind]] = {"bytes": 128, "sha256": "a" * 64}
            for kind in ("notes_alias", "notes_immutable"):
                payloads[names[kind]] = {"bytes": 64, "sha256": "c" * 64}
            catalog = {**current, "channel": channel}
            data = identity.artifact.render_manifest_json(catalog).encode()
            payloads[names["manifest"]] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        receipt = {"utc": "2026-10-11T05:24:55+00:00", "official_ci_release": False, "status": "TEMPORARY_MANUAL_PUBLICATION_VERIFIED",
                   "target": identity.PUBLIC_BASE_URL, "version": current["version"],
                   "source": "d" * 40, "payloads": payloads,
                   "events": [{"name": name, "status": "HTTPS_VERIFIED"} for name in payloads]}
        path = self.root / "manual-receipt.json"
        data = json.dumps(receipt, indent=2).encode()
        path.write_bytes(data)
        return current, path, hashlib.sha256(data).hexdigest()

    def test_exact_recovered_manual_receipt_allows_selected_identity_without_official_history_rewrite(self):
        current, path, digest = self.recovered_manual_publication()
        history = [published("0.8.3-1"), published("0.8")]
        before = json.dumps(history)
        prepared = identity.prepare(self.root, "0.9.0-2", 3, 2, history, current, path, digest)
        self.assertEqual(prepared, {"version": "0.9.0-2", "native_version": "0.9.0", "package_code": 3})
        self.assertEqual(json.dumps(history), before)

    def test_later_official_publication_makes_recovered_manual_snapshot_stale(self):
        current, path, digest = self.recovered_manual_publication()
        before = identity.read_sources(self.root)
        with self.assertRaisesRegex(ValueError, "stale"):
            identity.prepare(self.root, "0.9.0-2", 3, 2,
                             [published("0.8.9-2", date="2026-10-12T12:00:00Z")], current, path, digest)
        self.assertEqual(identity.read_sources(self.root), before)

    def test_recovered_receipt_mismatch_cannot_write_any_source(self):
        current, path, digest = self.recovered_manual_publication()
        before = identity.read_sources(self.root)
        variants = [({**current, "version": "0.8.9-2"}, digest),
                    ({**current, "artifact": {**current["artifact"], "sha256": "e" * 64}}, digest),
                    (current, "f" * 64)]
        for catalog, pin in variants:
            with self.subTest(catalog=catalog), self.assertRaises(ValueError):
                identity.prepare(self.root, "0.9.0-2", 3, 2, [published("0.8.3-1")], catalog, path, pin)
            self.assertEqual(identity.read_sources(self.root), before)
        receipt = json.loads(path.read_text())
        for edit in (lambda r: r.update(official_ci_release=True),
                     lambda r: r["events"].pop(),
                     lambda r: r["payloads"]["firmware.bin"].update(sha256="e" * 64)):
            invalid = json.loads(json.dumps(receipt)); edit(invalid)
            data = json.dumps(invalid).encode(); path.write_bytes(data)
            with self.assertRaises(ValueError):
                identity.prepare(self.root, "0.9.0-2", 3, 2, [published("0.8.3-1")], current,
                                 path, hashlib.sha256(data).hexdigest())
            self.assertEqual(identity.read_sources(self.root), before)

    def test_partial_publication_or_missing_history_requires_receipt_recovery_before_writes(self):
        before = identity.read_sources(self.root)
        for releases, current in (([published("0.8")], manifest("0.9.0-1")),
                                  ([published("0.9.0-1")], manifest("0.8")),
                                  ([], manifest("0.8"))):
            with self.subTest(releases=releases), self.assertRaisesRegex(ValueError, "receipt|previous beta"):
                self.prepare(releases=releases, current=current,
                             version="0.9.0-2")
            self.assertEqual(identity.read_sources(self.root), before)

        live = manifest("1.0.0-1")
        names = identity.artifact.release_names("beta", live["version"], "a" * 64, "b" * 64)
        live.update(chip_id=9, artifact={"sha256": "a" * 64, "url": "https://updates.foxieproducts.com/fw/" + names["firmware_immutable"]},
                    notes={"sha256": "b" * 64, "url": "https://updates.foxieproducts.com/fw/" + names["notes_immutable"]})
        for version in ("1.0.0-0", "1.0.0-1", "1.0.0-3"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.prepare(version=version, code=47, previous=46, releases=[], current=live)
        prepared = self.prepare(version="1.0.0-2", code=47, previous=46, releases=[], current=live)
        self.assertEqual(prepared["package_code"], 47)
        self.assertEqual(prepared["version"], "1.0.0-2")

    def test_app_firmware_and_native_source_mismatches_fail(self):
        self.prepare()
        good = identity.read_sources(self.root)
        cases = ((identity.HEADER, '"0.9.0-2"', '"0.9.0-3"'),
                 (identity.PUBSPEC, "0.9.0+3", "0.9.0+4"),
                 (identity.PUBSPEC, "0.9.0+3", "0.8.2+3"),
                 (identity.APP_IDENTITY, "version: '0.9.0-2'", "version: '0.9.0-10'"))
        for name, old, new in cases:
            with self.subTest(name=name):
                source = {**good, name: good[name].replace(old, new)}
                with self.assertRaisesRegex(ValueError, "disagree"):
                    identity.source_identity(source)

    def test_bad_sequence_and_missing_public_evidence_never_stamp_any_source(self):
        before = identity.read_sources(self.root)
        for version in ("0.8", "0.9.0", "0.9.0-0", "0.9.0-01", "0.9.0-1-beta", "0.9.0-8"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.prepare(version=version)
            self.assertEqual(identity.read_sources(self.root), before)
        for current in ({}, {**manifest("0.8"), "channel": "stable"}):
            with self.assertRaises(ValueError):
                self.prepare(current=current)
            self.assertEqual(identity.read_sources(self.root), before)

    def test_cli_requires_verified_inputs_and_check_does_not_allocate(self):
        command = [sys.executable, str(RELEASE_TOOLS / "release_identity.py")]
        before = identity.read_sources(self.root)
        result = subprocess.run(command + ["prepare", "--root", str(self.root), "--version", "0.9.0-1"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("required", result.stderr)
        self.assertEqual(identity.read_sources(self.root), before)
        self.prepare()
        result = subprocess.run(command + ["check", "--root", str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), identity.check(self.root))


    def test_095_selected_pair_stamps_matching_triplet_without_inventing_history(self):
        history = [published("0.9.0-5")]
        self.prepare(version="0.9.0-7", code=13, previous=12,
                     releases=history, current=manifest("0.9.0-5"))
        before_history = json.dumps(history)
        prepared = self.prepare(version="0.9.5-10", code=14, previous=13,
                                releases=history, current=manifest("0.9.0-5"))
        self.assertEqual(prepared, {"version": "0.9.5-10", "native_version": "0.9.5", "package_code": 14})
        self.assertEqual(identity.check(self.root), prepared)
        self.assertEqual(json.dumps(history), before_history)
        before = identity.read_sources(self.root)
        self.prepare(version="0.9.5-10", code=14, previous=13,
                     releases=history, current=manifest("0.9.0-5"))
        self.assertEqual(identity.read_sources(self.root), before)
        history.append(published("0.9.5-10", channel="stable"))
        prepared = self.prepare(version="0.9.5-11", code=15, previous=14,
                                releases=history, current=manifest("0.9.0-5"))
        self.assertEqual(prepared, {"version": "0.9.5-11", "native_version": "0.9.5", "package_code": 15})
        self.assertEqual(identity.next_beta_version(history, series="0.9.5"), "0.9.5-1")
        self.assertEqual(identity.next_beta_version(history + [published("0.9.5-11")], series="0.9.5"), "0.9.5-12")
        for version in ("0.9.5-0", "0.9.5-01", "0.9.6-10"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                identity.sequence(version)
        with self.assertRaises(ValueError):
            identity.next_beta_version(history, series="0.9.6")
        good = identity.read_sources(self.root)
        for name, old, wrong in (
                (identity.PUBSPEC, "0.9.5+15", "0.9.0+15"),
                (identity.APP_IDENTITY, "nativeVersion: '0.9.5'", "nativeVersion: '0.9.0'"),
                (identity.APP_IDENTITY, "buildNumber: '15'", "buildNumber: '14'")):
            with self.subTest(path=name), self.assertRaisesRegex(ValueError, "disagree"):
                identity.source_identity({**good, name: good[name].replace(old, wrong)})

    def test_095_pair_rejects_used_versions_native_code_reuse_and_unselected_gap(self):
        history = [published("0.9.0-5")]
        self.prepare(version="0.9.0-7", code=13, previous=12,
                     releases=history, current=manifest("0.9.0-5"))
        before = identity.read_sources(self.root)
        for version in ("0.9.5-10", "0.9.5-11"):
            for channel in ("beta", "stable"):
                with self.subTest(version=version, channel=channel), self.assertRaisesRegex(ValueError, "already published"):
                    self.prepare(version=version, code=15, previous=13,
                                 releases=history + [published(version, channel=channel)],
                                 current=manifest("0.9.0-5"))
                self.assertEqual(identity.read_sources(self.root), before)
        for code, floor in ((10, 13), (11, 13), (13, 12), (14, 14)):
            with self.subTest(code=code, floor=floor), self.assertRaisesRegex(ValueError, "native package code"):
                self.prepare(version="0.9.5-10", code=code, previous=floor,
                             releases=history, current=manifest("0.9.0-5"))
            self.assertEqual(identity.read_sources(self.root), before)
        for version, releases in (("0.9.5-9", history),
                                  ("0.9.5-11", history + [published("0.9.5-12")])):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "next unpublished beta"):
                identity.check_next_beta(version, releases)
        with self.assertRaisesRegex(ValueError, "disagree|receipt"):
            self.prepare(version="0.9.5-10", code=14, previous=13,
                         releases=history, current=manifest("0.9.5-10"))
        self.assertEqual(identity.read_sources(self.root), before)

    def test_095_prepare_cannot_silently_repair_tampered_historical_app_inputs(self):
        history = [published("0.9.0-5")]
        self.prepare(version="0.9.0-7", code=13, previous=12,
                     releases=history, current=manifest("0.9.0-5"))
        path = self.root / identity.APP_IDENTITY
        path.write_text(path.read_text().replace("nativeVersion: '0.9.0'", "nativeVersion: '0.9.5'"))
        before = identity.read_sources(self.root)
        with self.assertRaisesRegex(ValueError, "disagree"):
            self.prepare(version="0.9.5-10", code=14, previous=13,
                         releases=history, current=manifest("0.9.0-5"))
        self.assertEqual(identity.read_sources(self.root), before)


    def prepare_retained_current_source(self):
        history = [published("0.9.0-7", date="2026-10-19T12:00:00Z")]
        self.prepare(version="0.9.5-10", code=19, previous=18,
                     releases=history, current=manifest("0.9.0-7"))
        return history + [published("0.9.5-11", date="2026-10-24T12:00:00Z")]

    def test_retained_current_selected_identity_advances_after_later_beta(self):
        history = self.prepare_retained_current_source()
        before_history = json.dumps(history)
        prepared = self.prepare(version="0.9.5-10", code=21, previous=20,
                                releases=history, current=manifest("0.9.5-11"))
        self.assertEqual(prepared, {"version": "0.9.5-10", "native_version": "0.9.5", "package_code": 21})
        self.assertEqual(identity.check(self.root), prepared)
        self.assertEqual(json.dumps(history), before_history)
        with self.assertRaisesRegex(ValueError, "next unpublished beta is 0.9.5-12"):
            identity.check_next_beta("0.9.5-10", history)
        identity.check_next_beta("0.9.5-12", history)
        before = identity.read_sources(self.root)
        with patch.object(Path, "write_text", autospec=True) as writes:
            self.prepare(version="0.9.5-10", code=21, previous=20,
                         releases=history, current=manifest("0.9.5-11"))
            writes.assert_not_called()
        self.assertEqual(identity.read_sources(self.root), before)

    def test_next_device_update_uses_selected_13_without_inventing_published_12(self):
        history = [published("0.9.5-11")]
        self.prepare(version="0.9.5-12", code=24, previous=23,
                     releases=history, current=manifest("0.9.5-11"))
        before_history = json.dumps(history)
        result = self.prepare(version="0.9.5-13", code=25, previous=24,
                              releases=history, current=manifest("0.9.5-11"))
        self.assertEqual(result, {"version": "0.9.5-13", "native_version": "0.9.5", "package_code": 25})
        self.assertEqual(json.dumps(history), before_history)
        self.assertEqual(identity.next_beta_version(history, series="0.9.5"), "0.9.5-12")
        with self.assertRaisesRegex(ValueError, "next unpublished beta"):
            identity.check_next_beta("0.9.5-14", history)
        for channel in ("beta", "stable"):
            with self.subTest(channel=channel), self.assertRaisesRegex(ValueError, "already published"):
                identity.check_next_beta("0.9.5-13", history + [published("0.9.5-13", channel=channel)])

    def test_zero_start_series_prepare_check_and_retry_without_consuming_identity(self):
        history = [published("0.9.7-6")]
        self.prepare(version="0.9.7-10", code=32, previous=31,
                     releases=history, current=manifest("0.9.7-6"))
        for series, code, prior in (
            ("0.9.8", 33, "0.9.7-6"),
            ("0.9.9", 34, "0.9.8-0"),
            ("1.0.0", 45, "0.9.9-7"),
            ("1.0.1", 49, "1.0.0-1"),
            ("1.0.2", 50, "1.0.1-0"),
            ("1.0.3", 52, "1.0.2"),
            ("1.0.4", 55, "1.0.3"),
        ):
            with self.subTest(series=series):
                history = [published(prior)]
                if series == "1.0.4":
                    history = [[published("0.8"), published("1.0.2"),
                                published("1.0.2", channel="stable")],
                               [published(prior, date="2026-11-23T12:00:00Z"),
                                published(prior, channel="stable", date="2026-11-23T13:00:00Z"),
                                published(series, draft=True)]]
                before_history = json.dumps(history)
                version = identity.canonical_version(f"{series}-0")
                prepared = self.prepare(version=version, code=code, previous=code-1,
                                        releases=history, current=manifest(prior))
                self.assertEqual(prepared, {"version": version, "native_version": series, "package_code": code})
                self.assertEqual(identity.check(self.root), prepared)
                self.assertIn(f"version: {series}+{code}\n", (self.root / identity.PUBSPEC).read_text())
                self.assertEqual(identity.next_beta_version(history, series=series), version)
                self.assertEqual(json.dumps(history), before_history)
                before = identity.read_sources(self.root)
                with patch.object(Path, "write_text", autospec=True) as writes:
                    self.prepare(version=version, code=code, previous=code-1,
                                 releases=history, current=manifest(prior))
                    writes.assert_not_called()
                self.assertEqual(identity.read_sources(self.root), before)
                if series == "1.0.4":
                    with self.assertRaisesRegex(ValueError, "disagree|receipt"):
                        self.prepare(version=version, code=code, previous=code-1,
                                     releases=history, current=manifest("1.0.2"))
                    self.assertEqual(identity.read_sources(self.root), before)
        self.assertEqual(identity.next_beta_version(history), "1.0.4")
        for series, prior in (("1.0.2", "1.0.1-0"), ("1.0.3", "1.0.2"),
                              ("1.0.4", "1.0.3")):
            history = [published(prior)]
            spellings = (series, f"{series}-0")
            for spelling in spellings:
                for channel in ("beta", "stable"):
                    records = history + [published(spelling, channel=channel)]
                    self.assertEqual(identity.next_beta_version(records, series=series), f"{series}-1")
                    identity.check_next_beta(f"{series}-1", records)
                    for reused in spellings:
                        with self.assertRaisesRegex(ValueError, "already published"):
                            identity.check_next_beta(reused, records)
            with self.assertRaisesRegex(ValueError, "next unpublished beta"):
                identity.check_next_beta(f"{series}-0", history)
            for live in spellings:
                with self.assertRaisesRegex(ValueError, "deployed|superseded"):
                    identity.check_next_beta(series, history, published_beta=live)

    def test_097_published_zero_advances_numerically_and_cannot_be_rebuilt(self):
        history = [published("0.9.5-11"), published("0.9.7-99", draft=True),
                   published("0.9.7-8", channel="stable")]
        self.assertEqual(identity.next_beta_version(history, series="0.9.7"), "0.9.7-0")
        after_zero = history + [published("0.9.7-0")]
        self.assertEqual(identity.next_beta_version(after_zero, series="0.9.7"), "0.9.7-1")
        identity.check_next_beta("0.9.7-1", after_zero)
        self.assertEqual(identity.next_beta_version(
            after_zero + [published("0.9.7-9"), published("0.9.7-10")], series="0.9.7"), "0.9.7-11")
        for version, code, later in (("0.9.7-5", 28, "0.9.7-6"), ("0.9.7-6", 29, "0.9.7-7")):
            prepared = self.prepare(version=version, code=code, previous=code-1,
                                    releases=[published("0.9.7-0")], current=manifest("0.9.7-0"))
            self.assertEqual(prepared, {"version": version, "native_version": "0.9.7", "package_code": code})
            self.assertEqual(identity.check(self.root), prepared)
            self.assertEqual(identity.next_beta_version(after_zero, series="0.9.7"), "0.9.7-1")
            with self.assertRaisesRegex(ValueError, "next unpublished beta"):
                identity.check_next_beta(version, after_zero + [published(later)])
        before = identity.read_sources(self.root)
        for version in ("0.9.7-0", "0.9.8-0", "0.9.9-0", "0.9.7-6"):
            for channel in ("beta", "stable"):
                with self.subTest(version=version, channel=channel), patch.object(Path, "write_text", autospec=True) as writes:
                    with self.assertRaisesRegex(ValueError, "already published"):
                        self.prepare(version=version, code=28, previous=27,
                                     releases=history + [published(version, channel=channel)],
                                     current=manifest("0.9.5-11"))
                    writes.assert_not_called()
        self.assertEqual(identity.read_sources(self.root), before)

    def test_098_zero_retains_legacy_syntax_native_floor_and_public_baseline_guards(self):
        for version in ("0.9.0-0", "0.9.5-0", "0.9.8-00", "0.9.8-01",
                        "0.9.8--1", "0.9.8-0-beta", "0.9.9-00", "0.9.9-01", "0.9.6-0",
                        "1.0.4-00", "1.0.4-01", "1.0.4--1", "1.0.4-0-beta"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                identity.sequence(version)
        history = [published("0.9.7-6")]
        self.prepare(version="0.9.7-10", code=32, previous=31,
                     releases=history, current=manifest("0.9.7-6"))
        before = identity.read_sources(self.root)
        for code, floor in ((32, 32), (32, 31), (33, 33), (True, 32)):
            with self.subTest(code=code, floor=floor), patch.object(Path, "write_text", autospec=True) as writes:
                with self.assertRaisesRegex(ValueError, "native package code"):
                    self.prepare(version="0.9.8-0", code=code, previous=floor,
                                 releases=history, current=manifest("0.9.7-6"))
                writes.assert_not_called()
        for catalog in ({}, manifest("0.9.7-5")):
            with self.subTest(catalog=catalog), patch.object(Path, "write_text", autospec=True) as writes:
                with self.assertRaisesRegex(ValueError, "disagree|receipt"):
                    self.prepare(version="0.9.8-0", code=33, previous=32,
                                 releases=history, current=catalog)
                writes.assert_not_called()
        self.assertEqual(identity.read_sources(self.root), before)

    def test_retained_current_identity_requires_unused_valid_publication_evidence(self):
        history = self.prepare_retained_current_source()
        cases = [
            (history + [published("0.9.5-10", channel=channel)], manifest("0.9.5-11"), "already published")
            for channel in ("beta", "stable")
        ]
        cases += [
            (history + [published("0.9.5-10", channel="stable", date=None)], manifest("0.9.5-11"), "publication time"),
            (history + [{"tag_name": "fw-beta-0.9.5-12", "draft": False}], manifest("0.9.5-11"), "publication time"),
            (history + [{"tag_name": "fw-stable-other", "draft": "false"}], manifest("0.9.5-11"), "incomplete"),
            (history, manifest("0.9.0-7"), "disagree|receipt"),
        ]
        before = identity.read_sources(self.root)
        for records, catalog, error in cases:
            with self.subTest(records=records[-1], catalog=catalog), patch.object(Path, "write_text", autospec=True) as writes:
                with self.assertRaisesRegex(ValueError, error):
                    self.prepare(version="0.9.5-10", code=21, previous=20,
                                 releases=records, current=catalog)
                writes.assert_not_called()
            self.assertEqual(identity.read_sources(self.root), before)

    def test_retained_identity_cannot_be_invented_or_taken_from_other_current_source(self):
        history = self.prepare_retained_current_source()
        for current, target in (("0.9.5-10", "0.9.5-9"), ("0.9.5-9", "0.9.5-9"),
                                ("0.9.5-11", "0.9.5-10")):
            texts = identity.read_sources(self.root)
            old = identity.source_identity(texts)["version"]
            for name in (identity.HEADER, identity.APP_IDENTITY):
                (self.root / name).write_text(texts[name].replace(old, current))
            before = identity.read_sources(self.root)
            with self.subTest(current=current, target=target), patch.object(Path, "write_text", autospec=True) as writes:
                with self.assertRaisesRegex(ValueError, "next unpublished beta"):
                    self.prepare(version=target, code=21, previous=20,
                                 releases=history, current=manifest("0.9.5-11"))
                writes.assert_not_called()
            self.assertEqual(identity.read_sources(self.root), before)

    def test_retained_identity_keeps_current_coherence_and_native_code_floors(self):
        history = self.prepare_retained_current_source()
        original = identity.read_sources(self.root)
        for name, old, wrong in ((identity.PUBSPEC, "0.9.5+19", "0.9.0+19"),
                                 (identity.APP_IDENTITY, "buildNumber: '19'", "buildNumber: '18'")):
            (self.root / name).write_text(original[name].replace(old, wrong))
            before = identity.read_sources(self.root)
            with self.subTest(source=name), patch.object(Path, "write_text", autospec=True) as writes:
                with self.assertRaisesRegex(ValueError, "disagree"):
                    self.prepare(version="0.9.5-10", code=21, previous=20,
                                 releases=history, current=manifest("0.9.5-11"))
                writes.assert_not_called()
            self.assertEqual(identity.read_sources(self.root), before)
            (self.root / name).write_text(original[name])
        for code, previous in ((19, 20), (20, 20), (18, 17), (21, 21), (True, 20)):
            with self.subTest(code=code, previous=previous), patch.object(Path, "write_text", autospec=True) as writes:
                with self.assertRaisesRegex(ValueError, "native package code"):
                    self.prepare(version="0.9.5-10", code=code, previous=previous,
                                 releases=history, current=manifest("0.9.5-11"))
                writes.assert_not_called()
            self.assertEqual(identity.read_sources(self.root), original)

    def test_coherent_source_cannot_prepare_a_backwards_series_or_write_any_file(self):
        history = [published("0.9.0-7", date="2026-10-19T12:00:00Z")]
        self.prepare(version="0.9.5-11", code=15, previous=14,
                     releases=history, current=manifest("0.9.0-7"))
        history.append(published("0.9.5-11", date="2026-10-24T12:00:00Z"))
        before = identity.read_sources(self.root)
        self.assertEqual(identity.check(self.root),
                         {"version": "0.9.5-11", "native_version": "0.9.5", "package_code": 15})
        # Intercept every source mutation so even the rejected old behavior
        # cannot actually replace the coherent fixture's three version files.
        with patch.object(Path, "write_text", autospec=True) as writes:
            try:
                with self.assertRaisesRegex(ValueError, "backwards"):
                    self.prepare(version="0.9.0-8", code=16, previous=15,
                                 releases=history, current=manifest("0.9.5-11"))
            finally:
                writes.assert_not_called()
        self.assertEqual(identity.read_sources(self.root), before)


if __name__ == "__main__":
    unittest.main()
