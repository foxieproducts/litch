from __future__ import annotations

import contextlib
import dataclasses
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


VENDOR_TOOL_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VENDOR_TOOL_DIR))

import compare_vendor  # noqa: E402


class VendorFixture:
    repository = "https://example.invalid/demo.git"
    tag = "v1"

    def __init__(self, temporary_root: Path) -> None:
        self.upstream = temporary_root / "upstream"
        self.repo_root = temporary_root / "project"
        self.local = self.repo_root / "lib" / "demo"
        self.manifest = temporary_root / "sources.json"
        self.upstream.mkdir()
        self.local.mkdir(parents=True)

        self.git("init", "-q")
        self.git("config", "user.name", "Vendor Test")
        self.git("config", "user.email", "vendor-test@example.invalid")
        self.git("remote", "add", "origin", self.repository)

        upstream_files = {
            "a-exact.txt": b"same a\n",
            "mode-sensitive.txt": b"target",
            "modified.c": b"int answer = 1; // upstream comment\n",
            "upstream-only.txt": b"only upstream\n",
            "z-exact.txt": b"same z\n",
        }
        for relative, content in upstream_files.items():
            self.write(self.upstream / relative, content)
        self.git("add", ".")
        self.git("commit", "-q", "-m", "fixture baseline")
        self.git("tag", self.tag)

        self.commit = self.git_text("rev-parse", "HEAD")
        self.tree = self.git_text("rev-parse", "HEAD^{tree}")

        self.write(self.local / "a-exact.txt", upstream_files["a-exact.txt"])
        self.write(self.local / "mode-sensitive.txt", upstream_files["mode-sensitive.txt"])
        self.write(self.local / "modified.c", b"int   answer=1; /* local comment */\n")
        self.write(self.local / "local-only.txt", b"only local\n")
        self.write(self.local / "z-exact.txt", upstream_files["z-exact.txt"])
        self.write(self.local / ".git" / "ignored", b"never inventory metadata\n")
        self.write(self.local / "nested" / ".git", b"also ignored\n")
        self.write_manifest()

    @staticmethod
    def write(path: Path, content: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def git(self, *arguments: str) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(
            ["git", "-C", str(self.upstream), *arguments],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def git_text(self, *arguments: str) -> str:
        return self.git(*arguments).stdout.decode("utf-8").strip()

    def manifest_data(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "vendors": {
                "demo": {
                    "repository": self.repository,
                    "tag": self.tag,
                    "commit": self.commit,
                    "tree": self.tree,
                    "local_path": "lib/demo",
                    "files": {
                        "a-exact.txt": {"status": "exact"},
                        "local-only.txt": {
                            "status": "local-only",
                            "local_blob": compare_vendor.git_blob_id(
                                (self.local / "local-only.txt").read_bytes()
                            ),
                            "local_mode": compare_vendor.local_git_mode(
                                self.local / "local-only.txt"
                            ),
                        },
                        "mode-sensitive.txt": {
                            "status": "exact",
                        },
                        "modified.c": {
                            "status": "modified",
                            "local_blob": compare_vendor.git_blob_id(
                                (self.local / "modified.c").read_bytes()
                            ),
                            "local_mode": compare_vendor.local_git_mode(
                                self.local / "modified.c"
                            ),
                        },
                        "upstream-only.txt": {"status": "upstream-only"},
                        "z-exact.txt": {"status": "exact"},
                    },
                }
            },
        }

    def write_manifest(self) -> None:
        self.manifest.write_text(
            json.dumps(self.manifest_data(), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def spec(self) -> compare_vendor.VendorSpec:
        return compare_vendor.load_vendor_spec(self.manifest, "demo")

    def audit(self, spec: compare_vendor.VendorSpec | None = None) -> compare_vendor.AuditResult:
        return compare_vendor.audit_vendor(spec or self.spec(), self.upstream, self.repo_root)


class CompareVendorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.fixture = VendorFixture(Path(self.temporary_directory.name))

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_inventory_is_sorted_and_git_metadata_is_excluded(self) -> None:
        result = self.fixture.audit()

        self.assertTrue(result.ok, result.drift)
        self.assertEqual(
            result.inventory.exact,
            ("a-exact.txt", "mode-sensitive.txt", "z-exact.txt"),
        )
        self.assertEqual(result.inventory.modified, ("modified.c",))
        self.assertEqual(result.inventory.local_only, ("local-only.txt",))
        self.assertEqual(result.inventory.upstream_only, ("upstream-only.txt",))
        self.assertFalse(any(".git" in path.split("/") for path in result.local_blobs))

    def test_whitespace_and_comments_are_ignored_by_semantic_diff(self) -> None:
        result = self.fixture.audit()

        semantic_diff = compare_vendor.semantic_diff_for_path(
            result, self.fixture.repo_root, "modified.c"
        )

        self.assertEqual(semantic_diff, "")

    def test_semantic_diff_reports_token_changes(self) -> None:
        changed_content = b"int answer = 2;\n"
        self.fixture.write(self.fixture.local / "modified.c", changed_content)
        spec = self.fixture.spec()
        files = dict(spec.files)
        files["modified.c"] = compare_vendor.FileExpectation(
            status="modified",
            local_blob=compare_vendor.git_blob_id(changed_content),
            local_mode=compare_vendor.local_git_mode(self.fixture.local / "modified.c"),
        )
        result = self.fixture.audit(dataclasses.replace(spec, files=files))

        semantic_diff = compare_vendor.semantic_diff_for_path(
            result, self.fixture.repo_root, "modified.c"
        )

        self.assertIn("-1", semantic_diff)
        self.assertIn("+2", semantic_diff)
        self.assertNotEqual(
            compare_vendor._source_tokens(b"if (left == right) {}\n"),
            compare_vendor._source_tokens(b"if (left = = right) {}\n"),
        )

    def test_executable_bit_drift_changes_exact_status(self) -> None:
        os.chmod(self.fixture.local / "a-exact.txt", 0o755)

        result = self.fixture.audit()

        self.assertEqual(result.local_modes["a-exact.txt"], "100755")
        self.assertIn("a-exact.txt", result.inventory.modified)
        self.assertIn(
            "status drift for a-exact.txt: expected exact, got modified",
            result.drift,
        )

    def test_modified_file_mode_is_pinned(self) -> None:
        os.chmod(self.fixture.local / "modified.c", 0o755)

        result = self.fixture.audit()

        self.assertIn("modified.c", result.inventory.modified)
        self.assertIn(
            "local mode drift for modified.c: expected 100644, got 100755",
            result.drift,
        )

    def test_manifest_requires_mode_for_non_upstream_identical_files(self) -> None:
        manifest = self.fixture.manifest_data()
        modified = manifest["vendors"]["demo"]["files"]["modified.c"]
        del modified["local_mode"]
        self.fixture.manifest.write_text(
            json.dumps(manifest),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(compare_vendor.AuditError, "local_mode must be one of"):
            self.fixture.spec()

    def test_symlink_and_regular_file_with_same_blob_are_not_exact(self) -> None:
        local_path = self.fixture.local / "mode-sensitive.txt"
        local_path.unlink()
        local_path.symlink_to("target")

        result = self.fixture.audit()

        self.assertEqual(
            result.upstream_blobs["mode-sensitive.txt"],
            result.local_blobs["mode-sensitive.txt"],
        )
        self.assertEqual(result.upstream_modes["mode-sensitive.txt"], "100644")
        self.assertEqual(result.local_modes["mode-sensitive.txt"], "120000")
        self.assertIn("mode-sensitive.txt", result.inventory.modified)
        self.assertIn(
            "status drift for mode-sensitive.txt: expected exact, got modified",
            result.drift,
        )

    def test_unclassified_and_classified_content_drift_fail_audit(self) -> None:
        self.fixture.write(self.fixture.local / "new-file.txt", b"unclassified\n")
        self.fixture.write(self.fixture.local / "local-only.txt", b"changed\n")
        changed_blob = compare_vendor.git_blob_id(b"changed\n")

        result = self.fixture.audit()

        self.assertFalse(result.ok)
        self.assertEqual(
            result.drift,
            (
                "unclassified local-only path: new-file.txt",
                "local content drift for local-only.txt: expected "
                f"{self.fixture.spec().files['local-only.txt'].local_blob}, got "
                f"{changed_blob}",
            ),
        )

    def test_checkout_rejects_wrong_origin_head_tree_and_tag(self) -> None:
        spec = self.fixture.spec()
        cases = {
            "origin": (
                dataclasses.replace(spec, repository="https://example.invalid/wrong.git"),
                "origin URL mismatch",
            ),
            "head": (dataclasses.replace(spec, commit="0" * 40), "HEAD mismatch"),
            "tree": (dataclasses.replace(spec, tree="0" * 40), "tree mismatch"),
        }
        for label, (invalid_spec, message) in cases.items():
            with self.subTest(label=label):
                with self.assertRaisesRegex(compare_vendor.AuditError, message):
                    compare_vendor.validate_checkout(invalid_spec, self.fixture.upstream)

        self.fixture.write(self.fixture.upstream / "second.txt", b"second\n")
        self.fixture.git("add", "second.txt")
        self.fixture.git("commit", "-q", "-m", "second fixture commit")
        self.fixture.git("tag", "wrong-tag")
        self.fixture.git("checkout", "-q", self.fixture.commit)
        with self.assertRaisesRegex(compare_vendor.AuditError, "tag wrong-tag mismatch"):
            compare_vendor.validate_checkout(
                dataclasses.replace(spec, tag="wrong-tag"), self.fixture.upstream
            )

    def test_cli_returns_one_and_prints_drift(self) -> None:
        self.fixture.write(self.fixture.local / "unclassified.txt", b"drift\n")
        stdout = io.StringIO()
        stderr = io.StringIO()

        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            exit_code = compare_vendor.main(
                [
                    "--vendor",
                    "demo",
                    "--manifest",
                    str(self.fixture.manifest),
                    "--upstream",
                    str(self.fixture.upstream),
                    "--repo-root",
                    str(self.fixture.repo_root),
                ]
            )

        self.assertEqual(exit_code, 1)
        self.assertIn("unclassified local-only path: unclassified.txt", stdout.getvalue())
        self.assertIn("audit: FAILED", stdout.getvalue())
        self.assertEqual(stderr.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
