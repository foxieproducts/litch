from __future__ import annotations

import json
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import scan_tracked_secrets  # noqa: E402


class IncrementalHistoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="litch-history-fixture-")
        self.addCleanup(self.directory.cleanup)
        self.temporary = Path(self.directory.name)
        self.root = self.temporary / "repo"
        self.root.mkdir()
        self.environment = mock.patch.dict(os.environ, {"GIT_ATTR_NOSYSTEM": "1"})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.git("init", "-q")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")
        self.cache = self.temporary / "history.json"
        self.secret = "pass" + "word='synthetic-" + "value'\n"
        (self.root / "tracked.txt").write_text(self.secret)
        self.commit("initial signature")
        self.initial = self.git("rev-parse", "HEAD")
        self.branch = self.git("branch", "--show-current")

    def git(self, *arguments: str) -> str:
        return subprocess.run(["git", *arguments], cwd=self.root, check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-qm", message)

    def make_cache(self) -> str:
        report = scan_tracked_secrets.build_report(self.root)
        scan_tracked_secrets.write_history_cache(self.root, self.cache, report)
        return hashlib.sha256(self.cache.read_bytes()).hexdigest()

    def pin_cache(self, cache: object) -> str:
        self.cache.write_text(json.dumps(cache))
        return hashlib.sha256(self.cache.read_bytes()).hexdigest()

    def assert_parity(self, digest: str) -> None:
        full = scan_tracked_secrets.build_report(self.root)
        with mock.patch.object(scan_tracked_secrets, "scan_history_patch",
                               wraps=scan_tracked_secrets.scan_history_patch) as scan:
            incremental = scan_tracked_secrets.build_report(self.root, self.cache, digest)
        scan.assert_called_once_with(self.root, self.initial)
        self.assertEqual(scan_tracked_secrets.report_to_json(incremental),
                         scan_tracked_secrets.report_to_json(full))
        self.assertEqual(scan_tracked_secrets.report_to_markdown(incremental),
                         scan_tracked_secrets.report_to_markdown(full))

    def test_exact_report_parity_for_linear_deletion_reintroduction_and_branch_merge(self) -> None:
        digest = self.make_cache()
        self.assert_parity(digest)
        (self.root / "tracked.txt").write_text(self.secret + "safe addition\n")
        self.commit("linear edit")
        self.assert_parity(digest)
        (self.root / "tracked.txt").unlink()
        self.commit("delete signature file")
        self.assert_parity(digest)
        (self.root / "tracked.txt").write_text(self.secret)
        self.commit("reintroduce signature")
        self.assert_parity(digest)
        self.git("switch", "-qc", "merge-side", self.initial)
        (self.root / "branch.txt").write_text("ghp_" + "a" * 24 + "\n")
        self.commit("signature on side branch")
        self.git("switch", "-q", self.branch)
        self.git("merge", "--no-ff", "-qm", "merge signatures", "merge-side")
        self.assert_parity(digest)
        # Cached history never substitutes for the actual working tree.
        (self.root / "tracked.txt").write_text("Bearer " + "b" * 24 + "\n")
        self.assert_parity(digest)
        report = scan_tracked_secrets.build_report(self.root, self.cache, digest)
        self.assertIn("bearer-token", {finding["rule_id"] for finding in report["tree_findings"]})

    def test_narrow_git_range_excludes_cached_ancestors_and_keeps_merge_parent(self) -> None:
        digest = self.make_cache()
        (self.root / "tracked.txt").write_text("safe\n")
        self.commit("remove signature")
        real_popen = subprocess.Popen
        with mock.patch.object(scan_tracked_secrets.subprocess, "Popen", wraps=real_popen) as popen:
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        commands = [call.args[0] for call in popen.call_args_list]
        self.assertIn(["git", "log", "--format=commit:%H", "-p", "HEAD", "^" + self.initial, "--"], commands)
        self.assertNotIn(["git", "log", "--format=commit:%H", "-p", "HEAD", "--"], commands)

    def test_merge_can_make_older_side_branch_newly_reachable_after_cache_base(self) -> None:
        fork = self.initial
        (self.root / "main.txt").write_text(self.secret)
        self.commit("official ancestor after fork")
        self.initial = self.git("rev-parse", "HEAD")
        digest = self.make_cache()
        self.git("switch", "-qc", "older-side", fork)
        (self.root / "side.txt").write_text("ghp_" + "c" * 24 + "\n")
        self.commit("older side signature newly reachable through merge")
        side = self.git("rev-parse", "HEAD")
        self.git("switch", "-q", self.branch)
        self.git("merge", "--no-ff", "-qm", "new merge", "older-side")
        self.assert_parity(digest)
        report = scan_tracked_secrets.build_report(self.root, self.cache, digest)
        self.assertIn(side, {finding["commit"] for finding in report["history_findings"]})

    def test_cli_round_trip_emits_new_pinnable_cache_without_changing_report_bytes(self) -> None:
        output = self.temporary / "reports"
        script = str(RELEASE_TOOLS / "scan_tracked_secrets.py")
        subprocess.run([sys.executable, script, "--repo-root", str(self.root),
                        "--output-dir", str(output), "--write-history-cache", str(self.cache)],
                       capture_output=True, text=True, check=True)
        digest = hashlib.sha256(self.cache.read_bytes()).hexdigest()
        (self.root / "tracked.txt").write_text("safe\n")
        self.commit("delete signature through real CLI seam")
        next_cache = self.temporary / "next-history.json"
        subprocess.run([sys.executable, script, "--repo-root", str(self.root),
                        "--output-dir", str(output), "--history-cache", str(self.cache),
                        "--history-cache-sha256", digest, "--write-history-cache", str(next_cache)],
                       capture_output=True, text=True, check=True)
        full = scan_tracked_secrets.build_report(self.root)
        self.assertEqual((output / "tracked_secret_scan.json").read_text(),
                         scan_tracked_secrets.report_to_json(full))
        self.assertEqual((output / "tracked_secret_scan.md").read_text(),
                         scan_tracked_secrets.report_to_markdown(full))
        new_digest = hashlib.sha256(next_cache.read_bytes()).hexdigest()
        head, findings = scan_tracked_secrets.load_history_cache(self.root, next_cache, new_digest)
        self.assertEqual(head, self.git("rev-parse", "HEAD"))
        self.assertEqual(len(findings), len(full["history_findings"]))

    def test_pin_is_required_and_corruption_or_malformed_cache_cannot_fall_back(self) -> None:
        digest = self.make_cache()
        for path, pin in ((self.cache, None), (None, digest), (self.cache, "")):
            with self.subTest(path=path, pin=pin), self.assertRaises(scan_tracked_secrets.HistoryCacheError):
                scan_tracked_secrets.build_report(self.root, path, pin)
        self.cache.write_bytes(self.cache.read_bytes() + b" ")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "digest mismatch"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        self.cache.write_text("{malformed")
        digest = hashlib.sha256(self.cache.read_bytes()).hexdigest()
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "invalid JSON"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)

    def test_changed_rules_scanner_or_effective_git_identity_rejects_reuse(self) -> None:
        from dataclasses import replace

        digest = self.make_cache()
        changed = (replace(scan_tracked_secrets.RULES[0], label="controlled rule change"),
                   *scan_tracked_secrets.RULES[1:])
        with mock.patch.object(scan_tracked_secrets, "RULES", changed), \
                self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "identity mismatch"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        changed_scanner = self.temporary / "scanner.py"
        changed_scanner.write_bytes(Path(scan_tracked_secrets.__file__).read_bytes() + b"\n# changed scanner\n")
        with mock.patch.object(scan_tracked_secrets, "__file__", str(changed_scanner)), \
                self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "identity mismatch"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        self.git("config", "diff.renames", "false")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "identity mismatch"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)

    def test_checkout_transport_identity_does_not_invalidate_history_cache(self) -> None:
        digest = self.make_cache()
        self.git("config", "http.https://example.invalid/.extraheader", "synthetic authorization")
        self.git("config", "safe.directory", str(self.root))
        self.git("config", "user.name", "Different release runner")
        self.assert_parity(digest)

    def test_nonancestor_full_commit_and_finding_commit_validation(self) -> None:
        (self.root / "tracked.txt").write_text("safe\n")
        self.commit("future ancestor cache")
        digest = self.make_cache()
        self.git("switch", "-qc", "unrelated-descendant", self.initial)
        (self.root / "other.txt").write_text("safe\n")
        self.commit("other branch")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "not an ancestor"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        cache = json.loads(self.cache.read_bytes())
        cache["head"] = "HEAD"
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "full commit"):
            scan_tracked_secrets.build_report(self.root, self.cache, self.pin_cache(cache))
        cache["head"] = self.git("rev-parse", "HEAD")
        cache["history_findings"][0]["commit"] = self.git("rev-parse", self.branch)
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "not an ancestor"):
            scan_tracked_secrets.build_report(self.root, self.cache, self.pin_cache(cache))

    def test_cache_does_not_contain_raw_secrets_and_rejects_unredacted_findings(self) -> None:
        self.make_cache()
        data = self.cache.read_bytes()
        self.assertNotIn(b"synthetic-value", data)
        cache = json.loads(data)
        cache["history_findings"][0]["summary"] = self.secret
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "not redacted"):
            scan_tracked_secrets.build_report(self.root, self.cache, self.pin_cache(cache))

    def test_changed_attributes_and_external_helpers_reject_incomplete_identity(self) -> None:
        digest = self.make_cache()
        attributes = self.root / ".git/info/attributes"
        attributes.write_text("*.txt -diff\n")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "identity mismatch"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        attributes.unlink()
        self.git("config", "diff.custom.textconv", "arbitrary external helper")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "external diff"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)

    def test_untracked_and_ignored_attributes_cannot_silently_change_cached_patches(self) -> None:
        scratch = self.root / "scratch"
        scratch.mkdir()
        historical = scratch / "historical.txt"
        historical.write_text(self.secret)
        self.commit("historical path affected by nested attributes")
        historical.unlink()
        self.commit("delete historical path before ignored attributes appear")
        digest = self.make_cache()
        attributes = self.root / ".gitattributes"
        attributes.write_text("*.txt -diff\n")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "untracked or ignored"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        attributes.unlink()
        (self.root / ".gitignore").write_text("scratch/\n")
        (scratch / ".gitattributes").write_text("*.txt -diff\n")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "untracked or ignored"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)

    def test_ignored_build_dependency_attributes_without_history_allow_exact_cache_export(self) -> None:
        (self.root / ".gitignore").write_text(".pio/\n")
        self.commit("ignore ordinary build dependencies")
        self.initial = self.git("rev-parse", "HEAD")
        digest = self.make_cache()
        dependency = self.root / ".pio/libdeps/litch-esp32s3/NeoPixelBus"
        dependency.mkdir(parents=True)
        attributes = dependency / ".gitattributes"
        attributes.write_text("*.txt -diff\n")
        (dependency / "untracked-source.txt").write_text(self.secret)
        next_cache = self.temporary / "next-dependency-history.json"
        output = self.temporary / "dependency-reports"
        real_read_bytes = Path.read_bytes

        def read_tracked_only(path):
            if path == attributes:
                self.fail("ignored dependency attributes must be proven irrelevant without opening them")
            return real_read_bytes(path)

        with mock.patch.object(Path, "read_bytes", read_tracked_only):
            # Both cold production and warm production must export a usable
            # cache after CI has installed ordinary ignored dependencies.
            scan_tracked_secrets.write_outputs(self.root, output, history_cache_output=next_cache)
            scan_tracked_secrets.write_outputs(self.root, output, self.cache, digest, next_cache)
            self.assert_parity(digest)
        full = scan_tracked_secrets.build_report(self.root)
        self.assertEqual((output / "tracked_secret_scan.json").read_text(),
                         scan_tracked_secrets.report_to_json(full))
        self.assertEqual((output / "tracked_secret_scan.md").read_text(),
                         scan_tracked_secrets.report_to_markdown(full))
        head, _findings = scan_tracked_secrets.load_history_cache(
            self.root, next_cache, hashlib.sha256(next_cache.read_bytes()).hexdigest())
        self.assertEqual(head, self.initial)

    def test_merge_config_is_bound_and_remerge_or_external_merge_drivers_are_rejected(self) -> None:
        digest = self.make_cache()
        self.git("config", "merge.conflictStyle", "diff3")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "identity mismatch"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        self.git("config", "--unset", "merge.conflictStyle")
        self.git("config", "log.diffMerges", "remerge")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "remerge"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        self.git("config", "--unset", "log.diffMerges")
        self.git("config", "merge.custom.driver", "arbitrary external merge helper")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "external diff/merge"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)

    def test_overridden_git_worktree_cannot_bypass_attribute_identity(self) -> None:
        digest = self.make_cache()
        alternate = self.temporary / "other-worktree"
        alternate.mkdir()
        with mock.patch.dict(os.environ, {"GIT_WORK_TREE": str(alternate)}), \
                self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "effective Git worktree"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)

    def test_cache_input_and_export_are_bounded_regular_files_and_json_depth_is_rejected(self) -> None:
        digest = self.make_cache()
        symlink = self.temporary / "cache-link.json"
        symlink.symlink_to(self.cache)
        for path in (symlink, self.temporary):
            with self.subTest(path=path), self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "regular file"):
                scan_tracked_secrets.pinned_history_cache(path, digest)
        with self.cache.open("wb") as oversized:
            oversized.truncate(scan_tracked_secrets.MAX_HISTORY_CACHE_BYTES + 1)
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "size limit"):
            scan_tracked_secrets.pinned_history_cache(self.cache, digest)
        self.cache.write_bytes(b"[" * 2000 + b"0" + b"]" * 2000)
        digest = hashlib.sha256(self.cache.read_bytes()).hexdigest()
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "invalid JSON"):
            scan_tracked_secrets.load_history_cache(self.root, self.cache, digest)
        exported = self.temporary / "oversized-export.json"
        report = scan_tracked_secrets.build_report(self.root)
        with mock.patch.object(scan_tracked_secrets, "MAX_HISTORY_CACHE_BYTES", 16), \
                self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "size limit"):
            scan_tracked_secrets.write_history_cache(self.root, exported, report)
        self.assertFalse(exported.exists())

    def test_shallow_and_replacement_history_cannot_reuse_complete_history_cache(self) -> None:
        digest = self.make_cache()
        shallow = self.root / ".git/shallow"
        shallow.write_text(self.initial + "\n")
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "non-shallow"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)
        shallow.unlink()
        self.git("replace", self.initial, self.initial)
        with self.assertRaisesRegex(scan_tracked_secrets.HistoryCacheError, "replacement"):
            scan_tracked_secrets.build_report(self.root, self.cache, digest)


class ScanTrackedSecretsTests(unittest.TestCase):

    def test_sparse_long_line_avoids_repeated_unanchored_regex_work(self) -> None:
        from dataclasses import replace

        class MeasuredPattern:
            def __init__(self, pattern):
                self.pattern = pattern
                self.unanchored_characters = 0
                self.anchored_calls = 0

            def search(self, text):
                self.unanchored_characters += len(text)
                return self.pattern.search(text)

            def finditer(self, text):
                self.unanchored_characters += len(text)
                return self.pattern.finditer(text)

            def sub(self, replacement, text):
                self.unanchored_characters += len(text)
                return self.pattern.sub(replacement, text)

            def match(self, text, start):
                self.anchored_calls += 1
                return self.pattern.match(text, start)

        measured = [replace(rule, pattern=MeasuredPattern(rule.pattern))
                    if rule.rule_id in ('github-token', 'aws-access-key', 'generic-secret-assignment')
                    else rule for rule in scan_tracked_secrets.RULES]
        # Large escaped history records contain many JSON delimiters and domain
        # words, while almost all positions cannot begin a configured signature.
        text = '{"note":"history metadata"}' * 65536
        with mock.patch.object(scan_tracked_secrets, 'RULES', tuple(measured)):
            self.assertEqual(scan_tracked_secrets.scan_text('history', 'docs/history.jsonl', text,
                                                         commit='1'*40), [])
        self.assertEqual(sum(getattr(rule.pattern, 'unanchored_characters', 0)
                             for rule in measured), 0,
                         'Sparse large history lines must not repeat full regex-VM searches')
        self.assertEqual(sum(getattr(rule.pattern, 'anchored_calls', 0)
                             for rule in measured), 0,
                         'Quoted JSON keys cannot start unquoted secret assignments')

    def test_optimized_matches_and_redaction_equal_original_patterns(self) -> None:
        from itertools import product

        token = 'ghp_' + 'a'*24
        aws = 'AKIA' + '0'*16
        keys = ('token', 'SECRET', 'PaSsWoRd', 'passwd', 'api_key', 'api-key', 'apikey',
                'pa\u017f\u017fword', 'ap\u0131_key', 'AP\u0130_KEY', 'api_\u212aey', '\u017fecret')
        samples = ['plain', token, 'x'+token, '_'+token, '\u00e9'+token, '-'+token,
                   token+'_', token+'\u00e9', aws, 'x'+aws, aws+'_',
                   'Bearer\t'+'b'*20, 'Bearer\u2003'+'b'*20,
                   '-----BEGIN RSA ' + 'PRIVATE KEY-----',
                   'first '+token+' second '+aws+' '+token,
                   'word='+'"token='+'\'synthetic-value\'"']
        for key, before, after, opening, closing in product(
                keys, ('', 'word', '\u00e9', '-', '\U0001f98a'),
                ('', ' ', '\t', '\u00a0', '\u2003', '\x1c'), ("'", '"'), ("'", '"')):
            samples.append(before+key+after+':'+after+opening+'synthetic-value'+closing)
        samples.extend(['token'+' '*20000+'='+"'synthetic-value'",
                        'token='+"'short'", 'secret='+"'first-value'"+' token='+"'second-value'"])
        for text in samples:
            for rule in scan_tracked_secrets.RULES:
                with self.subTest(rule=rule.rule_id, size=len(text)):
                    self.assertEqual([m.span() for m in scan_tracked_secrets._iter_matches(rule, text)],
                                     [m.span() for m in rule.pattern.finditer(text)])
            expected = text
            for rule in scan_tracked_secrets.RULES:
                expected = rule.pattern.sub(scan_tracked_secrets._replace_with_redacted, expected)
            self.assertEqual(scan_tracked_secrets.redact(text), expected)

    def test_full_findings_and_reports_equal_original_search_and_substitution(self) -> None:
        from dataclasses import asdict

        def original_redact(text):
            for rule in scan_tracked_secrets.RULES:
                text = rule.pattern.sub(scan_tracked_secrets._replace_with_redacted, text)
            return text

        def original_scan(scope, path, text, commit=None):
            return [scan_tracked_secrets.Finding(scope, rule.rule_id, rule.confidence,
                        path, index, commit, original_redact(line.strip()))
                    for index, line in enumerate(text.splitlines(), 1)
                    for rule in scan_tracked_secrets.RULES if rule.pattern.search(line)]

        key, value = 'pass'+'word', 'synthetic-'+'value'
        token, aws = 'ghp_'+'a'*24, 'AKIA'+'0'*16
        content = (f"prefix\u2028{key}='{value}'\r\n"+token+' '+aws+'\v'+
                   'Bearer\t'+'b'*20+'\n'+f"{key}='{value}' {key}='{value}'\n")
        expected = original_scan('history', 'unicode-\U0001f98a.txt', content, '1'*40)
        self.assertEqual([asdict(x) for x in scan_tracked_secrets.scan_text(
                            'history', 'unicode-\U0001f98a.txt', content, '1'*40)],
                         [asdict(x) for x in expected])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.run(['git', *args], cwd=root, check=True,
                                      capture_output=True, text=True)
            git('init', '-q')
            git('config', 'user.name', 'Release Test')
            git('config', 'user.email', 'release-test@example.invalid')
            (root/'first.txt').write_text(content, encoding='utf-8')
            git('add', 'first.txt')
            git('commit', '-qm', 'initial signatures')
            git('mv', 'first.txt', 'renamed.txt')
            (root/'renamed.txt').write_text(content+'safe\n', encoding='utf-8')
            git('add', 'renamed.txt')
            git('commit', '-qm', 'rename with edit')
            (root/'renamed.txt').write_text('safe\n', encoding='utf-8')
            git('add', 'renamed.txt')
            git('commit', '-qm', 'remove signatures')
            with mock.patch.object(scan_tracked_secrets, 'scan_text', side_effect=original_scan), \
                    mock.patch.object(scan_tracked_secrets, 'redact', side_effect=original_redact):
                original = scan_tracked_secrets.build_report(root)
            actual = scan_tracked_secrets.build_report(root)
            self.assertEqual(actual, original)
            self.assertEqual(scan_tracked_secrets.report_to_json(actual),
                             scan_tracked_secrets.report_to_json(original))
            self.assertEqual(scan_tracked_secrets.report_to_markdown(actual),
                             scan_tracked_secrets.report_to_markdown(original))

    def test_multiple_rules_share_one_large_redacted_summary(self) -> None:
        token, aws = 'ghp_'+'a'*24, 'AKIA'+'0'*16
        key, value = 'pass'+'word', 'synthetic-'+'value'
        text = 'history '*131072 + f"{key}='{value}' "+token+' '+aws
        findings = scan_tracked_secrets.scan_text('history', 'large.jsonl', text, '1'*40)
        self.assertEqual(len(findings), 3)
        self.assertTrue(all(f.summary is findings[0].summary for f in findings),
                        'One immutable summary per physical line bounds retained large matches')
        self.assertTrue(all(x not in findings[0].summary for x in (value, token, aws)))

    def _scan_history_child(self, payload: bytes, exit_code: int = 0,
                            pause_seconds: int = 0):
        children = []
        real_popen = subprocess.Popen
        command = ["git", "log", "--format=commit:%H", "-p", "HEAD", "--"]
        program = (
            "import sys,time; "
            f"sys.stdout.buffer.write({payload!r}); sys.stdout.flush(); "
            "sys.stderr.write('controlled history diagnostic\\n'); sys.stderr.flush(); "
            f"time.sleep({pause_seconds}); sys.exit({exit_code})"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def revision_only(repo_root, args):
                self.assertEqual(repo_root, root)
                if args == ["rev-parse", "--verify", "HEAD"]:
                    return "1" * 40 + "\n"
                self.fail("history patch must stream rather than capture complete stdout")

            def start_child(args, **kwargs):
                self.assertEqual(args, command)
                self.assertEqual(kwargs["cwd"], root)
                self.assertEqual(kwargs["stdout"], subprocess.PIPE)
                self.assertTrue(kwargs["text"])
                child = real_popen([sys.executable, "-c", program], **kwargs)
                children.append(child)
                return child

            try:
                with mock.patch.object(scan_tracked_secrets, "run_git", side_effect=revision_only), \
                        mock.patch.object(scan_tracked_secrets.subprocess, "Popen", side_effect=start_child):
                    return scan_tracked_secrets.scan_history_patch(root)
            finally:
                for child in children:
                    exited = child.poll() is not None
                    if not exited:
                        child.kill()
                        child.wait()
                    self.assertTrue(exited, "history child is reaped before returning or raising")
                    self.assertTrue(child.stdout.closed, "history stdout is closed")
                    if pause_seconds:
                        self.assertNotEqual(child.returncode, exit_code,
                                            "decoder failure terminates the child rather than waiting for completion")

    def test_history_stream_preserves_patch_state_and_unicode_separators(self) -> None:
        first, second = "1" * 40, "2" * 40
        token = "ghp_" + "a" * 24
        name, value = "pass" + "word", "synthetic-" + "value"
        api_name, api_value = "api_" + "key", "synthetic-" + "key"
        bearer = "Bearer " + "b" * 20
        patch = (
            f"commit:{first}\n+{name}=\"{value}\"\n"
            "--- a/deleted.txt\n+++ b/renamed.txt\n"
            f"-token=\"{token}\"\r\n+{name}=\"{value}\"\n"
            f"+prefix\u2028+Authorization: {bearer}\n"
            f"+emoji-\U0001f98a\n+safe\v+{api_name}=\"{api_value}\"\rcommit:{second}\n"
            f"+{name}=\"{value}\"\n--- a/removal.txt\n-{name}=\"{value}\""
        )
        findings = self._scan_history_child(patch.encode())
        actual = sorted((f.rule_id, f.path, f.commit, f.line) for f in findings)
        expected = sorted([
            ("github-token", "renamed.txt", first, 1),
            ("generic-secret-assignment", "renamed.txt", first, 1),
            ("generic-secret-assignment", "renamed.txt", first, 1),
            ("bearer-token", "renamed.txt", first, 1),
            ("generic-secret-assignment", "renamed.txt", first, 1),
            ("generic-secret-assignment", "removal.txt", second, 1),
        ])
        self.assertEqual(actual, expected)
        self.assertTrue(all("[REDACTED]" in f.summary for f in findings))
        self.assertTrue(all(f.scope == "history" for f in findings))
        self.assertTrue(all(token not in f.summary and value not in f.summary for f in findings))

    def test_history_nonzero_exit_cannot_return_partial_findings(self) -> None:
        patch = ("commit:" + "1" * 40 + "\n+++ b/test.txt\n+safe\n").encode()
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            self._scan_history_child(patch, exit_code=7)
        self.assertEqual(raised.exception.returncode, 7)
        self.assertIn("controlled history diagnostic", raised.exception.stderr)

    def test_history_decode_failure_terminates_and_reaps_child(self) -> None:
        with self.assertRaises(UnicodeDecodeError):
            self._scan_history_child(b"\xff", exit_code=7, pause_seconds=10)

    def test_redact_removes_secret_values(self) -> None:
        sample = "token=ghp_" + "abcdefghijklmnopqrstuvwxyz123456"

        redacted = scan_tracked_secrets.redact(sample)

        self.assertNotIn("abcdefghijklmnopqrstuvwxyz", redacted)
        self.assertIn("[REDACTED]", redacted)

    def test_scan_text_marks_high_and_low_confidence(self) -> None:
        bearer_line = "Authorization: Bearer " + "abcdefghijklmnop"
        secret_name = "pass" + "word"
        secret_value = "super" + "secretvalue"
        findings = scan_tracked_secrets.scan_text(
            "content",
            "demo.txt",
            "\n".join(
                [
                    bearer_line,
                    f"{secret_name} = '{secret_value}'",
                ]
            ),
        )

        self.assertEqual([finding.confidence for finding in findings], ["high", "low"])
        self.assertTrue(all("[REDACTED]" in finding.summary for finding in findings))

    def test_report_json_is_deterministic(self) -> None:
        report = {
            "tool_version": "1.0.0",
            "report_schema": "schema-a",
            "history_scope": "HEAD ancestry",
            "tree_findings": [],
            "history_findings": [],
            "findings": [],
            "summary": {
                "high_confidence_count": 0,
                "low_confidence_count": 0,
                "tracked_file_count": 10,
                "limitations": ["tracked only"],
            },
        }

        rendered = scan_tracked_secrets.report_to_json(report)

        self.assertEqual(rendered, scan_tracked_secrets.report_to_json(report))
        self.assertEqual(json.loads(rendered)["summary"]["tracked_file_count"], 10)
        self.assertEqual(json.loads(rendered)["history_scope"], "HEAD ancestry")

    def test_unrelated_local_ref_does_not_change_release_history_report(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory) / "repository"
            repo_root.mkdir()
            subprocess.run(["git", "init", "-q"], cwd=repo_root, check=True)
            subprocess.run(
                ["git", "config", "user.name", "Release Test"],
                cwd=repo_root,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.email", "release-test@example.invalid"],
                cwd=repo_root,
                check=True,
            )
            tracked = repo_root / "tracked.txt"
            tracked.write_text("safe\n", encoding="utf-8")
            subprocess.run(["git", "add", "tracked.txt"], cwd=repo_root, check=True)
            subprocess.run(
                ["git", "commit", "-q", "-m", "release head"],
                cwd=repo_root,
                check=True,
            )
            release_branch = scan_tracked_secrets.run_git(
                repo_root, ["branch", "--show-current"]
            ).strip()
            before = scan_tracked_secrets.build_report(repo_root)

            subprocess.run(
                ["git", "switch", "-q", "-c", "unrelated-local-ref"],
                cwd=repo_root,
                check=True,
            )
            token = "ghp_" + "a" * 24
            tracked.write_text(f"token = '{token}'\n", encoding="utf-8")
            subprocess.run(["git", "add", "tracked.txt"], cwd=repo_root, check=True)
            subprocess.run(
                ["git", "commit", "-q", "-m", "unrelated secret"],
                cwd=repo_root,
                check=True,
            )
            unrelated_commit = scan_tracked_secrets.run_git(
                repo_root, ["rev-parse", "HEAD"]
            ).strip()
            subprocess.run(
                ["git", "switch", "-q", release_branch],
                cwd=repo_root,
                check=True,
            )

            after = scan_tracked_secrets.build_report(repo_root)

            self.assertEqual(before, after)
            self.assertNotIn(
                unrelated_commit,
                {finding["commit"] for finding in after["history_findings"]},
            )

    def test_external_output_dir_and_custom_repo_root_do_not_crash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_root = Path(temporary_directory)
            repo_root = temporary_root / "repository"
            output_dir = temporary_root / "evidence-outside-repository"
            repo_root.mkdir()
            secret_name = "pass" + "word"
            secret_value = "super" + "secretvalue"
            (repo_root / "tracked.txt").write_text(
                f"{secret_name} = '{secret_value}'\n", encoding="utf-8"
            )
            subprocess.run(["git", "init", "-q"], cwd=repo_root, check=True)
            subprocess.run(["git", "add", "tracked.txt"], cwd=repo_root, check=True)
            result = subprocess.run(
                [
                    sys.executable,
                    str(RELEASE_TOOLS / "scan_tracked_secrets.py"),
                    "--repo-root",
                    str(repo_root),
                    "--output-dir",
                    str(output_dir),
                ],
                capture_output=True,
                text=True,
                check=True,
            )

            report = (output_dir / "tracked_secret_scan.json").read_text(encoding="utf-8")
            self.assertIn("[REDACTED]", report)
            self.assertNotIn("supersecretvalue", report)
            self.assertIn(str(output_dir / "tracked_secret_scan.json"), result.stdout)

    def test_custom_repo_root_owns_default_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            repo_root = Path(temporary_directory) / "repository"
            repo_root.mkdir()
            (repo_root / "tracked.txt").write_text("safe\n", encoding="utf-8")
            subprocess.run(["git", "init", "-q"], cwd=repo_root, check=True)
            subprocess.run(["git", "add", "tracked.txt"], cwd=repo_root, check=True)

            subprocess.run(
                [
                    sys.executable,
                    str(RELEASE_TOOLS / "scan_tracked_secrets.py"),
                    "--repo-root",
                    str(repo_root),
                ],
                capture_output=True,
                text=True,
                check=True,
            )

            self.assertTrue(
                (repo_root / "docs" / "release" / "generated" /
                 "tracked_secret_scan.json").is_file()
            )


if __name__ == "__main__":
    unittest.main()
