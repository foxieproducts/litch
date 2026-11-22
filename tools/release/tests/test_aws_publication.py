from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import official_candidate as candidate
import publish_aws as aws
from test_official_candidate import CandidateFixture


BUCKET = "firmware-test"
KEY_PREFIX = "fw/"
LOCK_KEY = "locks/test-firmware"


def setUpModule():
    environment = patch.dict(os.environ, {
        "FIRMWARE_AWS_REGION": "us-west-2", "FIRMWARE_S3_BUCKET": BUCKET,
        "FIRMWARE_S3_PREFIX": KEY_PREFIX, "FIRMWARE_S3_LOCK_KEY": LOCK_KEY})
    environment.start()
    unittest.addModuleCleanup(environment.stop)


def s3_error(command, code):
    return subprocess.CompletedProcess(command, 254, "", f"An error occurred ({code}) when calling the operation: raw detail")


class FakeS3:
    """CLI seam only: checks what S3 checks, versions objects and serves the app origin."""

    def __init__(self):
        self.objects = {"fw/manifest_beta.json": (b'{"version":"0.7"}', {"--cache-control": "no-cache"})}
        self.versions = {}
        self.bodies = []
        self.calls = []
        self.fail = {}
        self.lost_reply = set()
        self.contended_lock_error = None
        self.ignore_if_none_match = False
        self.successor_takes_lock = False
        self.next_version = 0
        self.lock_reply: str | None = None
        self.object_reply: str | None = None
        self.delete_reply: str | None = None
        self.missing_public_status = 404

    def store(self, key, data, headers):
        self.next_version += 1
        self.objects[key] = (data, headers)
        self.versions[key] = f"v{self.next_version}"
        return self.versions[key]

    def run(self, command, **kwargs):
        self.calls.append((command, kwargs))
        operation, args = command[2], command[3:]
        key = args[args.index("--key") + 1]
        if key in self.fail:
            return s3_error(command, self.fail[key])
        stdout = "{}"
        if operation == "put-object":
            exclusive = "--if-none-match" in args and not self.ignore_if_none_match
            if exclusive and key in self.objects:
                return s3_error(command, self.contended_lock_error or "PreconditionFailed")
            data = Path(args[args.index("--body") + 1]).read_bytes()
            if args[args.index("--checksum-sha256") + 1] != base64.b64encode(hashlib.sha256(data).digest()).decode():
                return s3_error(command, "BadDigest")
            headers = {flag: args[args.index(flag) + 1] for flag in ("--content-type", "--cache-control") if flag in args}
            self.bodies.append((key, data))
            stdout = json.dumps({"VersionId": self.store(key, data, headers)})
            if key == LOCK_KEY and self.lock_reply is not None:
                stdout = self.lock_reply
            elif key != LOCK_KEY and self.object_reply is not None:
                stdout = self.object_reply
        elif operation == "delete-object":
            version = args[args.index("--version-id") + 1] if "--version-id" in args else None
            if version is None or self.versions.get(key) == version:
                self.objects.pop(key, None)
                self.versions.pop(key, None)
            if version is None:
                self.next_version += 1
                stdout = json.dumps({"DeleteMarker": True, "VersionId": f"v{self.next_version}"})
                if self.delete_reply is not None:
                    stdout = self.delete_reply
            if key == LOCK_KEY and self.successor_takes_lock:
                self.store(key, b"successor", {})
        else:
            raise AssertionError(f"unexpected S3 operation {operation}")
        if key in self.lost_reply:
            return s3_error(command, "RequestTimeout")
        return subprocess.CompletedProcess(command, 0, stdout, "")

    def read(self, name, maximum, missing_ok=False):
        stored = self.objects.get(KEY_PREFIX + name)
        if stored is None:
            if self.missing_public_status != 404:
                raise aws.PublicationError(f"public readback failed (HTTP {self.missing_public_status})")
            if missing_ok:
                return None
            raise aws.PublicationError("public readback failed (HTTP 404)")
        return stored[0]

    def host(self):
        return aws.S3FirmwareHost(self.run)

    def keys_written(self):
        return [c[0][c[0].index("--key") + 1] for c in self.calls if c[0][2] == "put-object"]

    def operations_on(self, key):
        return [c[0][2] for c in self.calls if c[0][c[0].index("--key") + 1] == key]


INVALID_LOCK_REPLIES = (
    ("missing", "{}"),
    ("empty", '{"VersionId":""}'),
    ("blank", '{"VersionId":"  "}'),
    ("null", '{"VersionId":"null"}'),
    ("wrong type", '{"VersionId":5}'),
    ("malformed JSON", "{bad"),
    ("wrong JSON shape", "[]"),
)


DIGEST_BIN = "firmware_0.9.0-1_" + "a" * 64 + ".bin"


class S3FirmwareHostTests(unittest.TestCase):
    def test_replace_sends_exact_bytes_checked_by_sha256_with_app_headers(self):
        s3 = FakeS3()
        s3.host().replace("manifest.json", b'{"version":"0.9.0-1"}')
        self.assertEqual(s3.objects["fw/manifest.json"],
                         (b'{"version":"0.9.0-1"}', {"--content-type": "application/json", "--cache-control": "no-cache"}))
        command, kwargs = s3.calls[-1]
        self.assertEqual(command[:3], ["aws", "s3api", "put-object"])
        self.assertEqual(command[command.index("--bucket") + 1], BUCKET)
        self.assertNotIn("shell", kwargs)

    def test_digest_named_files_are_immutable_and_aliases_revalidate(self):
        s3 = FakeS3()
        host = s3.host()
        compliance = "firmware_compliance_" + "c" * 64 + ".tar.gz"
        for name in (DIGEST_BIN, "firmware.bin", "version_0.9.0-1_" + "b" * 64 + ".md", "version_beta.md", compliance):
            host.replace(name, b"bytes")
        cache = {key: headers["--cache-control"] for key, (_, headers) in s3.objects.items()}
        self.assertEqual(cache["fw/" + DIGEST_BIN], "public, max-age=31536000, immutable")
        self.assertEqual(cache["fw/firmware.bin"], "no-cache")
        self.assertEqual(cache["fw/version_beta.md"], "no-cache")
        self.assertEqual(s3.objects["fw/version_beta.md"][1]["--content-type"], "text/markdown; charset=utf-8")
        self.assertEqual(cache["fw/" + compliance], "public, max-age=31536000, immutable")
        self.assertEqual(s3.objects["fw/" + compliance][1]["--content-type"], "application/gzip")

    def test_unsafe_names_never_reach_aws(self):
        s3 = FakeS3()
        for name in ("../manifest.json", "nested/firmware.bin", ".hidden", "", "manifest.json\n", "firmware+1.bin"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "unsafe"):
                s3.host().replace(name, b"bytes")
        self.assertEqual(s3.calls, [])

    def test_lock_and_conditional_creates_disable_automatic_retries(self):
        s3 = FakeS3()
        first, second = s3.host(), s3.host()
        with patch.dict(os.environ, {"AWS_MAX_ATTEMPTS": "5"}):
            first.lock()
            with self.assertRaisesRegex(aws.LockUnavailable, "inspect any previous interrupted run"):
                second.lock()
            first.replace("manifest.json", b"bytes")
            self.assertTrue(first.create(DIGEST_BIN, b"new image"))
            self.assertFalse(first.create(DIGEST_BIN, b"collision"))
            first.unlock()
            second.lock()
        lock_calls = [c for c in s3.calls if LOCK_KEY in c[0]]
        self.assertEqual(len(lock_calls), 4)
        for command, kwargs in lock_calls:
            self.assertEqual(kwargs["env"]["AWS_MAX_ATTEMPTS"], "1")
            if command[2] == "put-object":
                self.assertEqual(command[command.index("--if-none-match") + 1], "*")
        replace_call = next(c for c in s3.calls if "fw/manifest.json" in c[0])
        self.assertEqual(replace_call[1]["env"]["AWS_MAX_ATTEMPTS"], "5")
        creates = [c for c in s3.calls if "fw/" + DIGEST_BIN in c[0]]
        for command, kwargs in creates:
            self.assertEqual(kwargs["env"]["AWS_MAX_ATTEMPTS"], "1")
            self.assertEqual(command[command.index("--if-none-match") + 1], "*")
        self.assertEqual(s3.objects["fw/" + DIGEST_BIN][0], b"new image")

    def test_unlock_removes_only_this_runs_lock_version(self):
        s3 = FakeS3()
        first, second = s3.host(), s3.host()
        first.lock()
        s3.objects.pop(LOCK_KEY)  # An operator clears the lock while the first run is alive.
        s3.versions.pop(LOCK_KEY)
        second.lock()
        first.unlock()
        self.assertIn(LOCK_KEY, s3.objects)
        second.unlock()
        self.assertNotIn(LOCK_KEY, s3.objects)

    def test_unowned_unlock_never_deletes_another_runs_lock(self):
        for version in (None, "", "  ", "null", 5):
            with self.subTest(version=version):
                s3 = FakeS3()
                s3.store(LOCK_KEY, b"successor", {})
                host = s3.host()
                host.lock_version = version
                with self.assertRaisesRegex(aws.PublicationError, "unique lock version"):
                    host.unlock()
                self.assertEqual(s3.objects[LOCK_KEY], (b"successor", {}))
                self.assertEqual(s3.operations_on(LOCK_KEY), [])

    def test_unexpected_lock_failure_reports_unknown_lock_state(self):
        s3 = FakeS3()
        s3.fail[LOCK_KEY] = "InternalError"
        with self.assertRaisesRegex(ValueError, "lock state unknown") as caught:
            s3.host().lock()
        self.assertNotIsInstance(caught.exception, aws.LockUnavailable)

    def test_errors_report_only_the_s3_code(self):
        s3 = FakeS3()
        s3.fail["fw/manifest.json"] = "AccessDenied"
        with self.assertRaises(aws.PublicationError) as caught:
            s3.host().replace("manifest.json", b"bytes")
        self.assertIn("AccessDenied", str(caught.exception))
        self.assertNotIn("raw detail", str(caught.exception))
        self.assertNotIsInstance(caught.exception, aws.LockUnavailable)

    def test_failure_without_an_s3_code_hides_the_cli_output(self):
        def unreachable(command, **kwargs):
            return subprocess.CompletedProcess(command, 255, "", 'Could not connect to the endpoint URL: "https://private.example"')

        with self.assertRaisesRegex(ValueError, "no S3 error code") as caught:
            aws.S3FirmwareHost(unreachable).replace("manifest.json", b"bytes")
        self.assertNotIn("private.example", str(caught.exception))

    def test_failed_unlock_asks_for_inspection(self):
        s3 = FakeS3()
        host = s3.host()
        host.lock()
        s3.fail[LOCK_KEY] = "InternalError"
        with self.assertRaisesRegex(ValueError, "could not release publication lock"):
            host.unlock()

    def test_missing_cli_and_deadline_fail_closed(self):
        def missing(*args, **kwargs):
            raise FileNotFoundError("aws")

        def slow(command, **kwargs):
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])

        with self.assertRaisesRegex(ValueError, "AWS CLI not found"):
            aws.S3FirmwareHost(missing).replace("manifest.json", b"bytes")
        with self.assertRaisesRegex(ValueError, "deadline"):
            aws.S3FirmwareHost(slow).replace("manifest.json", b"bytes")


class S3PublicationTransactionTests(unittest.TestCase):
    """The unchanged publish() transaction, run through the S3 transport."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.fixture = CandidateFixture(temporary.name)
        f = self.fixture
        build_sha = f.prepare()
        notes = f.base / "reviewed.md"
        notes.write_text("## Improvements\n\n- Better board recovery.\n")
        self.final = f.base / "final"
        self.final_sha = candidate.finalize_notes(f.output, f.source_sha, build_sha, notes, self.final)
        self.s3 = FakeS3()
        self.s3.store("fw/manifest.json", b'{"version":"0.7"}', {})
        self.previous_sha = candidate.sha256(b'{"version":"0.7"}')

    def publish(self):
        aws.publish(self.final, self.fixture.source_sha, self.final_sha,
                    self.previous_sha, self.s3.host, self.s3.read)

    def test_assets_land_before_catalog_and_lock_is_released(self):
        self.publish()
        self.assertEqual(self.s3.objects["fw/manifest.json"][0],
                         (self.final / "catalog/manifest.json").read_bytes())
        writes = self.s3.keys_written()
        self.assertEqual(writes[0], LOCK_KEY)
        self.assertEqual(writes[-2], "fw/manifest.json")
        self.assertTrue(writes[-1].startswith("fw/provenance_"))
        self.assertEqual(len(writes), 9)
        self.assertNotIn(LOCK_KEY, self.s3.objects)
        for path in (self.final / "assets").iterdir():
            data, headers = self.s3.objects["fw/" + path.name]
            self.assertEqual(data, path.read_bytes())
            immutable = len(path.stem) > 64
            self.assertEqual(headers["--cache-control"] == "no-cache", not immutable)

    def test_publication_with_opaque_missing_keys_verifies_all_assets_before_catalog(self):
        self.s3.missing_public_status = 403
        self.publish()
        self.assertEqual(self.s3.objects["fw/manifest.json"][0],
                         (self.final / "catalog/manifest.json").read_bytes())
        for path in (self.final / "assets").iterdir():
            self.assertEqual(self.s3.read(path.name, path.stat().st_size), path.read_bytes())
        self.assertEqual(self.s3.keys_written()[-2], "fw/manifest.json")
        self.assertTrue(self.s3.keys_written()[-1].startswith("fw/provenance_"))
        self.assertNotIn(LOCK_KEY, self.s3.objects)

    def test_immutable_collision_requires_exact_positive_read_without_overwrite(self):
        path = next(p for p in (self.final / "assets").iterdir() if len(p.stem) > 64)
        key = KEY_PREFIX + path.name
        for same in (True, False):
            with self.subTest(matching=same):
                self.s3 = FakeS3()
                self.s3.missing_public_status = 403
                self.s3.store("fw/manifest.json", b'{"version":"0.7"}', {})
                data = path.read_bytes() if same else b"foreign bytes"
                version = self.s3.store(key, data, {})
                if same:
                    self.publish()
                else:
                    with self.assertRaisesRegex(aws.PublicationError, "immutable asset"):
                        self.publish()
                    self.assertEqual(self.s3.objects["fw/manifest.json"][0], b'{"version":"0.7"}')
                self.assertEqual(self.s3.versions[key], version)
                self.assertEqual(self.s3.objects[key][0], data)
                self.assertNotIn(key, [name for name, _ in self.s3.bodies])
                self.assertNotIn(LOCK_KEY, self.s3.objects)

    def test_public403_after_upload_withholds_catalog(self):
        def denied_asset(name, maximum, missing_ok=False):
            if name != "manifest.json":
                raise aws.PublicationError("public readback failed (HTTP 403)")
            return self.s3.read(name, maximum, missing_ok)

        with self.assertRaisesRegex(aws.PublicationError, "HTTP 403"):
            aws.publish(self.final, self.fixture.source_sha, self.final_sha,
                        self.previous_sha, self.s3.host, denied_asset)
        self.assertEqual(self.s3.objects["fw/manifest.json"][0], b'{"version":"0.7"}')
        self.assertNotIn(LOCK_KEY, self.s3.objects)

    def test_lost_immutable_create_reply_recovers_only_after_exact_public_read(self):
        path = next(p for p in (self.final / "assets").iterdir() if len(p.stem) > 64)
        key = KEY_PREFIX + path.name
        self.s3.missing_public_status = 403
        self.s3.lost_reply.add(key)
        with self.assertRaisesRegex(aws.PublicationError, "RequestTimeout"):
            self.publish()
        version = self.s3.versions[key]
        self.assertEqual(self.s3.objects["fw/manifest.json"][0], b'{"version":"0.7"}')
        self.assertNotIn(LOCK_KEY, self.s3.objects)
        self.s3.lost_reply.clear()
        self.publish()
        self.assertEqual(self.s3.versions[key], version)
        self.assertEqual(self.s3.objects["fw/manifest.json"][0],
                         (self.final / "catalog/manifest.json").read_bytes())

    def test_unusable_lock_reply_withholds_all_assets_and_unowned_cleanup(self):
        for name, reply in INVALID_LOCK_REPLIES:
            with self.subTest(response=name):
                self.s3 = FakeS3()
                self.s3.store("fw/manifest.json", b'{"version":"0.7"}', {})
                self.s3.lock_reply = reply
                with self.assertRaisesRegex(aws.PublicationError, "unique lock version") as caught:
                    self.publish()
                self.assertIn(f"s3://{BUCKET}/{LOCK_KEY}", str(caught.exception))
                self.assertIn("inspect", str(caught.exception))
                self.assertEqual(self.s3.keys_written(), [LOCK_KEY])
                self.assertEqual(self.s3.operations_on(LOCK_KEY), ["put-object"])
                self.assertIn(LOCK_KEY, self.s3.objects)
                self.assertEqual(self.s3.objects["fw/manifest.json"][0], b'{"version":"0.7"}')

    def test_held_lock_stops_publication_before_any_upload(self):
        self.s3.store(LOCK_KEY, b"another run", {})
        with self.assertRaises(aws.LockUnavailable):
            self.publish()
        self.assertEqual(self.s3.objects[LOCK_KEY], (b"another run", {}))
        self.assertEqual(self.s3.keys_written(), [LOCK_KEY])

    def test_lost_lock_reply_leaves_the_lock_for_inspection(self):
        self.s3.lost_reply.add(LOCK_KEY)
        with self.assertRaisesRegex(ValueError, "lock state unknown"):
            self.publish()
        self.assertIn(LOCK_KEY, self.s3.objects)
        self.assertEqual(self.s3.operations_on(LOCK_KEY), ["put-object"])
        self.assertEqual(self.s3.keys_written(), [LOCK_KEY])

    def test_failed_asset_upload_withholds_catalog_and_releases_lock(self):
        self.s3.fail["fw/firmware.bin"] = "InternalError"
        with self.assertRaisesRegex(ValueError, "InternalError"):
            self.publish()
        self.assertEqual(self.s3.objects["fw/manifest.json"][0], b'{"version":"0.7"}')
        self.assertNotIn(LOCK_KEY, self.s3.objects)


if __name__ == "__main__":
    unittest.main()
