from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError

RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import official_candidate as candidate
import firmware_publication as publisher
from test_official_candidate import CandidateFixture


class FakeHost:
    """Publication seam; the AWS adapter has its own S3 contract tests."""

    def __init__(self):
        self.files = {"manifest.json": b'{"version":"0.7"}', "manifest_beta.json": b'{"version":"0.7"}'}
        self.events = []
        self.fail_asset = None
        self.fail_replace = None
        self.changed_before_promotion = False
        self.catalog_readbacks = []
        self.locked = False

    def connect(self):
        self.events.append(("connect",))
        return self

    def lock(self):
        if self.locked:
            raise publisher.PublicationError("lock unavailable")
        self.locked = True
        self.events.append(("lock",))

    def unlock(self):
        self.locked = False
        self.events.append(("unlock",))

    def close(self):
        self.events.append(("close",))

    def replace(self, name, data):
        self.events.append(("replace", name))
        if self.fail_replace == name:
            raise publisher.PublicationError("controlled replacement failure")
        self.files[name] = data

    def create(self, name, data):
        if name in self.files:
            return False
        self.replace(name, data)
        return True

    def read(self, name, maximum, missing_ok=False):
        self.events.append(("read", name))
        if self.changed_before_promotion and name == "manifest.json" and any(e[0] == "replace" for e in self.events):
            return b'{"version":"another deployment"}'
        if self.fail_asset == name and ("replace", name) in self.events:
            return b"wrong or truncated served bytes"
        data = self.files.get(name)
        if (name == "manifest.json" and ("replace", name) in self.events
                and self.catalog_readbacks):
            assert self.locked, "the publisher must verify the catalog before unlocking"
            data = self.catalog_readbacks.pop(0)
        if data is None:
            if missing_ok:
                return None
            raise publisher.PublicationError("HTTP 404")
        if len(data) > maximum:
            raise publisher.PublicationError("public readback exceeded its size limit")
        return data


class FirmwarePublicationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.fixture = CandidateFixture(temporary.name)
        f = self.fixture
        self.build_sha = f.prepare()
        notes = f.base / "reviewed.md"
        notes.write_text("## Improvements\n\n- Better board recovery.\n")
        self.final = f.base / "final"
        self.final_sha = candidate.finalize_notes(f.output, f.source_sha, self.build_sha, notes, self.final)
        self.host = FakeHost()
        self.previous_sha = candidate.sha256(self.host.files["manifest.json"])

    def publish(self):
        publisher.publish(self.final, self.fixture.source_sha, self.final_sha,
                          self.previous_sha, self.host.connect, self.host.read)

    def test_full_production_transaction_assets_read_back_before_catalog(self):
        self.publish()
        expected = (self.final / "catalog/manifest.json").read_bytes()
        self.assertEqual(self.host.files["manifest.json"], expected)
        writes = [e[1] for e in self.host.events if e[0] == "replace"]
        self.assertEqual(writes[-1], "manifest.json")
        self.assertEqual(len(writes), 6)
        self.assertNotIn(writes[0], ("firmware.bin", "version.md"))
        manifest_write = self.host.events.index(("replace", "manifest.json"))
        for name in writes[:-1]:
            write = self.host.events.index(("replace", name))
            readback = self.host.events.index(("read", name), write + 1)
            self.assertLess(readback, manifest_write)
        self.assertEqual(self.host.events[-3:], [("read", "manifest.json"), ("unlock",), ("close",)])

    def test_beta_promotion_leaves_stable_catalog_and_aliases_untouched(self):
        f = self.fixture
        f.output = f.base / "beta-build"
        build_sha = f.prepare("beta")
        final = f.base / "beta-final"
        final_sha = candidate.finalize_notes(f.output, f.source_sha, build_sha,
                                             f.base / "reviewed.md", final)
        stable = dict(self.host.files)
        publisher.publish(final, f.source_sha, final_sha, candidate.sha256(stable["manifest_beta.json"]),
                          self.host.connect, self.host.read)
        self.assertEqual(self.host.files["manifest.json"], stable["manifest.json"])
        self.assertEqual(self.host.files["manifest_beta.json"],
                         (final / "catalog/manifest_beta.json").read_bytes())
        writes = [event[1] for event in self.host.events if event[0] == "replace"]
        self.assertEqual(writes[-1], "manifest_beta.json")
        self.assertNotIn("firmware.bin", writes)
        self.assertNotIn("version.md", writes)

    def test_corrupt_local_candidate_or_draft_never_connects(self):
        with self.assertRaisesRegex(ValueError, "still a draft"):
            publisher.publish(self.fixture.output, self.fixture.source_sha,
                              self.build_sha, self.previous_sha, self.host.connect, self.host.read)
        (self.final / "assets/firmware.bin").write_bytes(b"bad")
        with self.assertRaises(ValueError):
            self.publish()
        self.assertEqual(self.host.events, [])

    def test_historical_candidate_is_rejected_before_any_external_read_or_connection(self):
        digest = self.fixture.historical()
        with self.assertRaisesRegex(ValueError, "historical candidates are read-only"):
            publisher.publish(self.fixture.output, self.fixture.source_sha, digest,
                              self.previous_sha, self.host.connect, self.host.read)
        self.assertEqual(self.host.events, [])

    def test_compliance_bundle_collision_is_immutable_and_exact_existing_bytes_are_reused(self):
        receipt = candidate.verify(self.final, self.fixture.source_sha, self.final_sha)
        name = receipt["compliance_bundle"]["filename"]
        self.host.files[name] = b"foreign bundle"
        with self.assertRaisesRegex(ValueError, "immutable asset"):
            self.publish()
        self.assertEqual(self.host.files[name], b"foreign bundle")
        self.assertNotIn(("replace", name), self.host.events)
        self.assertNotIn(("replace", "manifest.json"), self.host.events)
        self.host = FakeHost()
        self.host.files[name] = (self.final / "assets" / name).read_bytes()
        self.publish()
        self.assertNotIn(("replace", name), self.host.events)
        self.assertLess(self.host.events.index(("read", name)),
                        self.host.events.index(("replace", "manifest.json")))

    def test_each_asset_readback_failure_withholds_manifest(self):
        old = self.host.files["manifest.json"]
        for path in (self.final / "assets").iterdir():
            with self.subTest(asset=path.name):
                self.host = FakeHost()
                self.host.fail_asset = path.name
                with self.assertRaises(publisher.PublicationError):
                    self.publish()
                self.assertEqual(self.host.files["manifest.json"], old)
                self.assertNotIn(("replace", "manifest.json"), self.host.events)
                self.assertFalse(self.host.locked)

    def test_each_failed_upload_withholds_catalog(self):
        for path in (self.final / "assets").iterdir():
            with self.subTest(asset=path.name):
                self.host = FakeHost()
                self.host.fail_replace = path.name
                with self.assertRaises(publisher.PublicationError):
                    self.publish()
                self.assertNotIn(("replace", "manifest.json"), self.host.events)

    def test_stale_approval_or_concurrent_catalog_change_is_rejected(self):
        self.previous_sha = "0" * 64
        with self.assertRaisesRegex(ValueError, "changed since approval"):
            self.publish()
        self.assertNotIn(("connect",), self.host.events)
        self.previous_sha = candidate.sha256(self.host.files["manifest.json"])
        self.host.changed_before_promotion = True
        with self.assertRaisesRegex(ValueError, "changed since approval"):
            self.publish()
        self.assertNotIn(("replace", "manifest.json"), self.host.events)

    def test_immutable_collision_and_reused_version_fail_closed(self):
        name = next(p.name for p in (self.final / "assets").iterdir() if len(p.name) > 64)
        self.host.files[name] = b"another image"
        with self.assertRaisesRegex(ValueError, "immutable asset"):
            self.publish()
        self.host = FakeHost()
        self.host.files["manifest.json"] = b'{"version":"0.9.0-9"}'
        self.previous_sha = candidate.sha256(self.host.files["manifest.json"])
        with self.assertRaisesRegex(ValueError, "version already published"):
            self.publish()
        self.assertNotIn(("connect",), self.host.events)

        for counterpart in (None, b'{"version":"0.9.0-9","artifact":{"sha256":"different"}}'):
            self.host = FakeHost()
            self.previous_sha = candidate.sha256(self.host.files["manifest.json"])
            if counterpart:
                self.host.files["manifest_beta.json"] = counterpart
            else:
                del self.host.files["manifest_beta.json"]
            with self.subTest(counterpart=counterpart), self.assertRaises(ValueError):
                self.publish()
            self.assertFalse(any(event[0] == "replace" for event in self.host.events))
            self.assertFalse(self.host.locked)
        self.host = FakeHost()
        self.host.files["manifest.json"] = b'{"version":"0.9.0-10"}'
        self.previous_sha = candidate.sha256(self.host.files["manifest.json"])
        with self.assertRaisesRegex(ValueError, "refusing rollback"):
            self.publish()
        self.assertNotIn(("connect",), self.host.events)

    def test_first_manifest_refuses_new_bytes_under_existing_legacy_version(self):
        self.host.files = {"version.md": b"Version: 0.9.0-9\n", "firmware.bin": b"old binary"}
        self.previous_sha = "missing"
        with self.assertRaisesRegex(ValueError, "legacy version already"):
            self.publish()
        self.assertNotIn(("connect",), self.host.events)

    def test_previous_catalog_can_temporarily_remain_visible_after_replacement(self):
        expected = (self.final / "catalog/manifest.json").read_bytes()
        previous = self.host.files["manifest.json"]
        # The prior response can be shorter or longer than the new catalog.
        for old in (previous, previous + b" " * len(expected), None):
            with self.subTest(previous_size=len(old) if old is not None else "absent"):
                self.host = FakeHost()
                if old is None:
                    del self.host.files["manifest.json"]
                    self.previous_sha = "missing"
                else:
                    self.host.files["manifest.json"] = old
                    self.previous_sha = candidate.sha256(old)
                self.host.catalog_readbacks = [old, old]

                with patch.object(publisher.time, "sleep") as sleep:
                    self.publish()

                self.assertEqual(self.host.files["manifest.json"], expected)
                self.assertEqual(sleep.call_count, 2)
                self.assertEqual(self.host.events.count(("replace", "manifest.json")), 1)
                manifest_write = self.host.events.index(("replace", "manifest.json"))
                self.assertEqual(self.host.events[manifest_write + 1:],
                                 [("read", "manifest.json")] * 3 + [("unlock",), ("close",)])
                self.assertFalse(self.host.locked)

    def test_catalog_that_stays_on_previous_bytes_is_unverified(self):
        self.host.catalog_readbacks = [self.host.files["manifest.json"]] * 5
        with patch.object(publisher.time, "sleep") as sleep:
            with self.assertRaisesRegex(ValueError, "publication may have occurred"):
                self.publish()

        self.assertFalse(self.host.catalog_readbacks)
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [1, 2, 4, 8])
        self.assertEqual(self.host.events.count(("replace", "manifest.json")), 1)
        self.assertEqual(self.host.events[-2:], [("unlock",), ("close",)])
        self.assertFalse(self.host.locked)

    def test_failed_final_readback_is_not_success_and_exact_retry_recovers(self):
        expected = (self.final / "catalog/manifest.json").read_bytes()
        self.host.catalog_readbacks = [b"unexpected bytes", expected]
        with patch.object(publisher.time, "sleep") as sleep:
            with self.assertRaisesRegex(ValueError, "publication may have occurred"):
                self.publish()
        sleep.assert_not_called()
        self.assertEqual(self.host.catalog_readbacks, [expected])
        self.assertEqual(self.host.events.count(("replace", "manifest.json")), 1)
        self.assertEqual(self.host.events[-2:], [("unlock",), ("close",)])
        self.assertFalse(self.host.locked)

        self.host.catalog_readbacks.clear()
        self.host.events.clear()
        self.publish()
        self.assertFalse(any(e[0] == "replace" for e in self.host.events))
        self.assertNotIn(("connect",), self.host.events)

    def test_existing_lock_is_not_removed_by_another_publisher(self):
        self.host.locked = True
        with self.assertRaisesRegex(ValueError, "lock unavailable"):
            self.publish()
        self.assertTrue(self.host.locked)
        self.assertNotIn(("unlock",), self.host.events)

    def test_public_http_redirect_auth_failure_and_oversize_are_rejected(self):
        for status in (301, 302, 401, 403, 500):
            opener = Mock()
            opener.open.side_effect = HTTPError("https://foxielitch.com/fw/file", status, "untrusted", {}, None)
            with self.subTest(status=status), patch.object(publisher.request, "build_opener", return_value=opener):
                with self.assertRaises(publisher.PublicationError):
                    publisher.public_read("file", 10, True)
                req = opener.open.call_args.args[0]
                self.assertEqual(req.full_url, "https://updates.foxieproducts.com/fw/file")
                self.assertNotIn("Authorization", req.headers)
        response = Mock(status=200, headers={})
        response.read1.return_value = b"too many bytes"
        response.__enter__ = Mock(return_value=response)
        response.__exit__ = Mock(return_value=False)
        opener.open.side_effect = None
        opener.open.return_value = response
        with patch.object(publisher.request, "build_opener", return_value=opener), self.assertRaisesRegex(ValueError, "size limit"):
            publisher.public_read("file", 1)
        self.assertIsNone(publisher.NoRedirect().redirect_request(None, None, None, None, None, None))

    def test_only_explicit_http_404_can_mean_no_prior_release(self):
        opener = Mock()
        opener.open.side_effect = HTTPError("https://foxielitch.com/fw/file", 404, "missing", {}, None)
        with patch.object(publisher.request, "build_opener", return_value=opener):
            self.assertIsNone(publisher.public_read("file", 10, True))
            with self.assertRaises(publisher.PublicationError):
                publisher.public_read("file", 10)




if __name__ == "__main__":
    unittest.main()
