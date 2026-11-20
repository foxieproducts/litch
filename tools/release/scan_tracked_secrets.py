#!/usr/bin/env python3
"""Scan tracked paths and history for likely secrets without reading ignored files."""

from __future__ import annotations

import argparse
import hashlib
import json
import locale
import os
import re
import shutil
import stat
import subprocess
import tempfile
from collections.abc import Iterator
from dataclasses import asdict, dataclass
from pathlib import Path


TOOL_VERSION = "1.1.0"
REPORT_SCHEMA = "litch-tracked-secret-scan-v1"
HISTORY_SCOPE = "HEAD ancestry"
HISTORY_CACHE_SCHEMA = "litch-tracked-secret-history-cache-v1"
MAX_HISTORY_CACHE_BYTES = 8 * 1024 * 1024
REPO_ROOT = Path(__file__).resolve().parents[2].resolve()


class HistoryCacheError(ValueError):
    """An explicitly supplied history cache cannot be safely reused."""


@dataclass(frozen=True)
class Rule:
    rule_id: str
    label: str
    confidence: str
    pattern: re.Pattern[str]


@dataclass(frozen=True)
class Finding:
    scope: str
    rule_id: str
    confidence: str
    path: str
    line: int | None
    commit: str | None
    summary: str


RULES = (
    Rule(
        rule_id="pem-private-key",
        label="PEM private key block",
        confidence="high",
        pattern=re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----"),
    ),
    Rule(
        rule_id="github-token",
        label="GitHub token",
        confidence="high",
        pattern=re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b"),
    ),
    Rule(
        rule_id="aws-access-key",
        label="AWS access key",
        confidence="high",
        pattern=re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    ),
    Rule(
        rule_id="bearer-token",
        label="Bearer token",
        confidence="high",
        pattern=re.compile(r"Bearer\s+[A-Za-z0-9._-]{16,}"),
    ),
    Rule(
        rule_id="generic-secret-assignment",
        label="Generic secret assignment",
        confidence="low",
        pattern=re.compile(
            r"(?i)\b(?:token|secret|password|passwd|api[_-]?key)\b\s*[:=]\s*['\"][^'\"]{8,}['\"]"
        ),
    ),
)



# Literal prefixes avoid a regex-VM word-boundary check at every character of
# giant history records. The original patterns remain the matching authority.
_GITHUB_CANDIDATE = re.compile(r"gh[pousr]_")
_AWS_CANDIDATE = re.compile(r"AKIA")
_ASSIGNMENT_VALUE = re.compile(r"""[:=]\s*['\"]""")


def _assignment_starts(text: str) -> Iterator[int]:
    for value in _ASSIGNMENT_VALUE.finditer(text):
        key_end = value.start()
        while key_end and text[key_end - 1].isspace():
            key_end -= 1
        # All configured unquoted keys end in n/t/d/y. JSON's quoted keys
        # cannot match; avoid four failed regex calls for every string value.
        if not key_end or text[key_end - 1] not in "nNtTdDyY":
            continue
        # The configured keys span token (5) through password (8). Check the
        # original pattern on the original string, preserving Unicode \b/\s/i.
        for length in (8, 7, 6, 5):
            if key_end >= length:
                yield key_end - length


def _iter_matches(rule: Rule, text: str) -> Iterator[re.Match[str]]:
    if rule.rule_id == "github-token":
        starts = (candidate.start() for candidate in _GITHUB_CANDIDATE.finditer(text))
    elif rule.rule_id == "aws-access-key":
        starts = (candidate.start() for candidate in _AWS_CANDIDATE.finditer(text))
    elif rule.rule_id == "generic-secret-assignment":
        starts = _assignment_starts(text)
    else:
        yield from rule.pattern.finditer(text)
        return
    end = 0
    for start in starts:
        if start < end:
            continue
        match = rule.pattern.match(text, start)
        if match is not None:
            yield match
            end = match.end()


def repo_relative(path: Path, repo_root: Path) -> str:
    resolved_path = path.resolve()
    return resolved_path.relative_to(repo_root.resolve()).as_posix()


def run_git(repo_root: Path, args: list[str]) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def tracked_files(repo_root: Path) -> list[Path]:
    output = run_git(repo_root, ["ls-files", "-z"])
    return sorted(
        [repo_root / item for item in output.split("\0") if item],
        key=lambda path: repo_relative(path, repo_root),
    )


def redact(text: str) -> str:
    redacted = text
    for rule in RULES:
        parts: list[str] = []
        end = 0
        for match in _iter_matches(rule, redacted):
            parts.extend((redacted[end:match.start()], _replace_with_redacted(match)))
            end = match.end()
        if parts:
            parts.append(redacted[end:])
            redacted = "".join(parts)
    return redacted


def scan_filename(path: str) -> list[Finding]:
    findings: list[Finding] = []
    summary: str | None = None
    for rule in RULES:
        if next(_iter_matches(rule, path), None) is not None:
            if summary is None:
                summary = redact(path)
            findings.append(
                Finding(
                    scope="filename",
                    rule_id=rule.rule_id,
                    confidence=rule.confidence,
                    path=path,
                    line=None,
                    commit=None,
                    summary=summary,
                )
            )
    return findings


def scan_text(scope: str, path: str, text: str, commit: str | None = None) -> list[Finding]:
    findings: list[Finding] = []
    for index, line in enumerate(text.splitlines(), start=1):
        summary: str | None = None
        for rule in RULES:
            if next(_iter_matches(rule, line), None) is not None:
                if summary is None:
                    summary = redact(line.strip())
                findings.append(
                    Finding(
                        scope=scope,
                        rule_id=rule.rule_id,
                        confidence=rule.confidence,
                        path=path,
                        line=index,
                        commit=commit,
                        summary=summary,
                    )
                )
    return findings


def scan_tracked_tree(repo_root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for path in tracked_files(repo_root):
        relative = repo_relative(path, repo_root)
        findings.extend(scan_filename(relative))
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data:
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        findings.extend(scan_text("content", relative, text))
    return sorted(findings, key=_finding_sort_key)


def scan_history_patch(repo_root: Path, ancestor: str | None = None) -> list[Finding]:
    try:
        run_git(repo_root, ["rev-parse", "--verify", "HEAD"])
    except subprocess.CalledProcessError:
        return []
    command = ["git", "log", "--format=commit:%H", "-p", "HEAD", "--"]
    if ancestor is not None:
        command.insert(-1, "^" + ancestor)
    findings: list[Finding] = []
    current_commit: str | None = None
    current_path: str | None = None
    # Complete history can be many GiB. Retain only one physical line plus
    # findings; split each line as before to preserve Unicode separator rules.
    # A file-backed diagnostic stream cannot block a full stderr pipe.
    with tempfile.TemporaryFile(mode="w+t") as errors:
        with subprocess.Popen(command, cwd=repo_root, stdout=subprocess.PIPE,
                              stderr=errors, text=True) as process:
            try:
                for physical_line in process.stdout:
                    for line in physical_line.splitlines():
                        if line.startswith("commit:"):
                            current_commit = line.split(":", 1)[1]
                            current_path = None
                            continue
                        if line.startswith("+++ b/") or line.startswith("--- a/"):
                            current_path = line[6:]
                            continue
                        if not line or line[0] not in "+-":
                            continue
                        if line.startswith("+++") or line.startswith("---"):
                            continue
                        if current_commit is None or current_path is None:
                            continue
                        findings.extend(
                            scan_text(
                                "history",
                                current_path,
                                line[1:],
                                commit=current_commit,
                            )
                        )
            except BaseException:
                process.kill()
                raise
            returncode = process.wait()
            if returncode:
                errors.seek(0)
                raise subprocess.CalledProcessError(returncode, command,
                                                    stderr=errors.read())
    return sorted(findings, key=_finding_sort_key)


def _history_identity(repo_root: Path) -> dict[str, object]:
    if Path(run_git(repo_root, ["rev-parse", "--show-toplevel"]).strip()).resolve() != repo_root.resolve():
        raise HistoryCacheError("history cache requires the effective Git worktree to match the repository root")
    if os.environ.get("GIT_ATTR_SOURCE") or os.environ.get("GIT_EXTERNAL_DIFF"):
        raise HistoryCacheError("history cache does not support external diff or attribute sources")
    # Config values can contain credentials. Bind their bytes without retaining
    # them in the cache or diagnostic output. Attribute files also affect diffs.
    config = run_git(repo_root, ["config", "--null", "--list"])
    # Checkout authentication, remote URLs, safe.directory, and user identity do
    # not affect patches and routinely change between trusted CI runs.
    diff_config = "\0".join(
        entry for entry in config.split("\0")
        if entry.partition("\n")[0].lower().startswith(
            ("core.", "diff.", "log.", "merge.", "format.", "color.", "i18n.", "pager.", "submodule.", "extensions.")
        )
    )
    for entry in diff_config.split("\0"):
        key, _separator, value = entry.partition("\n")
        key = key.lower()
        if (key in {"diff.external", "diff.orderfile"}
                or key.startswith("diff.") and key.endswith((".textconv", ".command"))
                or key.startswith("merge.") and key.endswith(".driver")
                or key == "log.diffmerges" and value.strip().lower() in {"remerge", "r"}):
            raise HistoryCacheError("history cache does not support external diff/merge helpers, remerge, or ordering files")
    # Git applies working-tree attributes even if they are untracked/ignored.
    # Dependency attributes below a directory absent from every reachable tree
    # cannot affect a history patch. Prove that without opening ignored files.
    untracked_attributes = run_git(repo_root, ["ls-files", "--others", "-z", "--", ".gitattributes", "**/.gitattributes"])
    attribute_parents = {Path(name).parent.as_posix() for name in untracked_attributes.split("\0") if name}
    for parent in sorted(attribute_parents):
        if parent == "." or run_git(repo_root, [
            "--literal-pathspecs", "rev-list", "--full-history", "-1", "HEAD", "--", parent + "/"
        ]).strip():
            raise HistoryCacheError("history cache does not support untracked or ignored .gitattributes affecting history")
    attribute_paths = {
        repo_relative(path, repo_root): path
        for path in tracked_files(repo_root) if path.name == ".gitattributes"
    }
    info_attributes = Path(run_git(repo_root, ["rev-parse", "--git-path", "info/attributes"]).strip())
    attribute_paths["git-info-attributes"] = repo_root / info_attributes
    configured_attributes = run_git(
        repo_root, ["config", "--path", "--get-all", "core.attributesfile"]
    ) if "core.attributesfile\n" in config.lower() else ""
    if configured_attributes:
        for index, name in enumerate(configured_attributes.splitlines()):
            attribute_paths[f"global-attributes-{index}"] = repo_root / Path(name).expanduser()
    else:
        config_home = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
        attribute_paths["global-attributes"] = config_home / "git/attributes"
    # Git's system attributes are not exposed through rev-parse. A caller that
    # enables system attributes must use a cold scan instead of an incomplete
    # identity. Official CI sets GIT_ATTR_NOSYSTEM=1 explicitly.
    if os.environ.get("GIT_ATTR_NOSYSTEM") != "1":
        raise HistoryCacheError("history cache requires GIT_ATTR_NOSYSTEM=1")
    attributes = {
        name: hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None
        for name, path in sorted(attribute_paths.items())
    }
    git_path = shutil.which("git")
    if git_path is None:
        raise HistoryCacheError("Git executable is unavailable")
    environment = {
        name: value for name, value in sorted(os.environ.items())
        if name in {"GIT_DIFF_OPTS", "GIT_EXTERNAL_DIFF", "GIT_ATTR_NOSYSTEM", "GIT_NO_REPLACE_OBJECTS"}
    }
    rules = [(rule.rule_id, rule.label, rule.confidence, rule.pattern.pattern, rule.pattern.flags)
             for rule in RULES]
    return {
        "scanner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "rules_sha256": hashlib.sha256(json.dumps(rules).encode()).hexdigest(),
        "git_sha256": hashlib.sha256(Path(git_path).read_bytes()).hexdigest(),
        "git_version": run_git(repo_root, ["--version"]).strip(),
        "git_config_sha256": hashlib.sha256(diff_config.encode()).hexdigest(),
        "git_environment_sha256": hashlib.sha256(json.dumps(environment).encode()).hexdigest(),
        "attributes": attributes,
        "encoding": locale.getpreferredencoding(False),
        "object_format": run_git(repo_root, ["rev-parse", "--show-object-format"]).strip(),
    }


def _complete_history(repo_root: Path) -> None:
    if run_git(repo_root, ["rev-parse", "--is-shallow-repository"]).strip() != "false":
        raise HistoryCacheError("history cache requires a complete non-shallow checkout")
    grafts = Path(run_git(repo_root, ["rev-parse", "--git-path", "info/grafts"]).strip())
    if (os.environ.get("GIT_GRAFT_FILE") or (repo_root / grafts).exists()
            or run_git(repo_root, ["replace", "--list"]).strip()):
        raise HistoryCacheError("history cache does not support grafts or replacement objects")


def _require_commit(repo_root: Path, revision: object, object_format: str) -> str:
    length = {"sha1": 40, "sha256": 64}.get(object_format)
    if not isinstance(revision, str) or length is None or re.fullmatch(f"[0-9a-f]{{{length}}}", revision) is None:
        raise HistoryCacheError("history cache requires a full commit object ID")
    try:
        actual = run_git(repo_root, ["rev-parse", "--verify", revision + "^{commit}"]).strip()
    except subprocess.CalledProcessError as error:
        raise HistoryCacheError("history cache commit is unavailable") from error
    if actual != revision:
        raise HistoryCacheError("history cache object is not a commit")
    return revision


def _require_ancestor(repo_root: Path, ancestor: str, descendant: str) -> None:
    try:
        run_git(repo_root, ["merge-base", "--is-ancestor", ancestor, descendant])
    except subprocess.CalledProcessError as error:
        raise HistoryCacheError("history cache commit is not an ancestor of the scanned commit") from error


def pinned_history_cache(path: Path, sha256: str) -> bytes:
    """Read a cache only against a digest supplied by a trusted external receipt."""
    if not isinstance(sha256, str) or re.fullmatch(r"[0-9a-f]{64}", sha256) is None:
        raise HistoryCacheError("history cache requires a pinned SHA-256 digest")
    metadata = path.lstat()
    if not stat.S_ISREG(metadata.st_mode):
        raise HistoryCacheError("history cache must be a regular file, not a symlink or special file")
    if metadata.st_size > MAX_HISTORY_CACHE_BYTES:
        raise HistoryCacheError("history cache exceeds the 8 MiB size limit")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    with os.fdopen(os.open(path, flags), "rb") as source:
        opened = os.fstat(source.fileno())
        if (not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (metadata.st_dev, metadata.st_ino)):
            raise HistoryCacheError("history cache file changed while opening")
        data = source.read(MAX_HISTORY_CACHE_BYTES + 1)
    if len(data) > MAX_HISTORY_CACHE_BYTES:
        raise HistoryCacheError("history cache exceeds the 8 MiB size limit")
    if hashlib.sha256(data).hexdigest() != sha256:
        raise HistoryCacheError("history cache SHA-256 digest mismatch")
    return data


def load_history_cache(repo_root: Path, path: Path, sha256: str) -> tuple[str, list[Finding]]:
    """Validate a pinned cache without scanning patches; return its base and findings."""
    data = pinned_history_cache(path, sha256)
    try:
        cache = json.loads(data)
    except (ValueError, UnicodeDecodeError, RecursionError) as error:
        raise HistoryCacheError("history cache is invalid JSON") from error
    if not isinstance(cache, dict) or cache.get("cache_schema") != HISTORY_CACHE_SCHEMA:
        raise HistoryCacheError("history cache schema mismatch")
    _complete_history(repo_root)
    identity = _history_identity(repo_root)
    if (cache.get("identity") != identity or cache.get("tool_version") != TOOL_VERSION
            or cache.get("report_schema") != REPORT_SCHEMA or cache.get("history_scope") != HISTORY_SCOPE):
        raise HistoryCacheError("history cache scanner, rules, or effective Git identity mismatch")
    ancestor = _require_commit(repo_root, cache.get("head"), identity["object_format"])
    _require_ancestor(repo_root, ancestor, "HEAD")
    raw_findings = cache.get("history_findings")
    if not isinstance(raw_findings, list):
        raise HistoryCacheError("history cache findings is not a list")
    findings: list[Finding] = []
    commits: set[str] = set()
    rules = {rule.rule_id: rule.confidence for rule in RULES}
    for raw in raw_findings:
        try:
            finding = Finding(**raw)
        except (TypeError, ValueError) as error:
            raise HistoryCacheError("history cache finding is malformed") from error
        if (finding.scope != "history" or not isinstance(finding.rule_id, str)
                or rules.get(finding.rule_id) != finding.confidence
                or not isinstance(finding.path, str) or type(finding.line) is not int or finding.line < 1
                or not isinstance(finding.summary, str) or redact(finding.summary) != finding.summary):
            raise HistoryCacheError("history cache finding is invalid or not redacted")
        commits.add(_require_commit(repo_root, finding.commit, identity["object_format"]))
        findings.append(finding)
    for commit in commits:
        _require_ancestor(repo_root, commit, ancestor)
    return ancestor, findings


def write_history_cache(repo_root: Path, path: Path, report: dict[str, object]) -> None:
    _complete_history(repo_root)
    identity = _history_identity(repo_root)
    head = _require_commit(repo_root, run_git(repo_root, ["rev-parse", "HEAD"]).strip(), identity["object_format"])
    cache = {
        "cache_schema": HISTORY_CACHE_SCHEMA,
        "tool_version": TOOL_VERSION,
        "report_schema": REPORT_SCHEMA,
        "history_scope": HISTORY_SCOPE,
        "identity": identity,
        "head": head,
        "history_findings": report["history_findings"],
    }
    data = (json.dumps(cache, sort_keys=True, indent=2) + "\n").encode("utf-8")
    if len(data) > MAX_HISTORY_CACHE_BYTES:
        raise HistoryCacheError("history cache exceeds the 8 MiB size limit")
    path.write_bytes(data)


def build_report(repo_root: Path, history_cache: Path | None = None,
                 history_cache_sha256: str | None = None) -> dict[str, object]:
    if (history_cache is None) != (history_cache_sha256 is None):
        raise HistoryCacheError("history cache path and pinned SHA-256 digest must be supplied together")
    ancestor, cached = load_history_cache(repo_root, history_cache, history_cache_sha256) if history_cache else (None, [])
    tree_findings = scan_tracked_tree(repo_root)
    history_findings = sorted(cached + scan_history_patch(repo_root, ancestor), key=_finding_sort_key)
    findings = sorted(tree_findings + history_findings, key=_finding_sort_key)
    return {
        "tool_version": TOOL_VERSION,
        "report_schema": REPORT_SCHEMA,
        "history_scope": HISTORY_SCOPE,
        "tree_findings": [asdict(finding) for finding in tree_findings],
        "history_findings": [asdict(finding) for finding in history_findings],
        "findings": [asdict(finding) for finding in findings],
        "summary": {
            "high_confidence_count": sum(f.confidence == "high" for f in findings),
            "low_confidence_count": sum(f.confidence == "low" for f in findings),
            "tracked_file_count": len(tracked_files(repo_root)),
            "limitations": [
                "Only Git-tracked filenames, tracked file contents, and history diffs reachable from HEAD are scanned; unrelated local refs are excluded.",
                "Ignored local credential files such as tools/release/site_upload.local.env are not opened by this tool.",
                "Binary blobs are skipped, so embedded secrets inside binary assets may require separate review.",
            ],
        },
    }


def report_to_json(report: dict[str, object]) -> str:
    return json.dumps(report, indent=2, sort_keys=True) + "\n"


def report_to_markdown(report: dict[str, object]) -> str:
    summary = report["summary"]
    lines = [
        "# Tracked Secret Scan",
        "",
        f"- Tool version: `{report['tool_version']}`",
        f"- Report schema: `{report['report_schema']}`",
        "- Git history scope: `HEAD` and its reachable ancestors "
        "(unrelated local refs excluded).",
        f"- Tracked files scanned: `{summary['tracked_file_count']}`",
        f"- High-confidence findings: `{summary['high_confidence_count']}`",
        f"- Low-confidence findings: `{summary['low_confidence_count']}`",
        "",
        "## Findings",
        "",
    ]
    findings = report["findings"]
    if findings:
        for finding in findings:
            location = f"`{finding['path']}`"
            if finding["line"] is not None:
                location += f":{finding['line']}"
            if finding["commit"]:
                location += f" @ `{finding['commit']}`"
            lines.append(
                f"- `{finding['confidence']}` `{finding['rule_id']}` at {location}: {finding['summary']}"
            )
    else:
        lines.append("- No tracked-tree or Git-history matches were found for the configured signatures.")
    lines.extend(["", "## Limitations", ""])
    for item in summary["limitations"]:
        lines.append(f"- {item}")
    lines.append("")
    return "\n".join(lines)


def write_outputs(repo_root: Path, output_dir: Path, history_cache: Path | None = None,
                  history_cache_sha256: str | None = None,
                  history_cache_output: Path | None = None) -> list[Path]:
    report = build_report(repo_root, history_cache, history_cache_sha256)
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = {
        output_dir / "tracked_secret_scan.json": report_to_json(report),
        output_dir / "tracked_secret_scan.md": report_to_markdown(report),
    }
    for path, content in outputs.items():
        path.write_text(content, encoding="utf-8")
    if history_cache_output is not None:
        write_history_cache(repo_root, history_cache_output, report)
    return sorted(outputs, key=lambda path: path.as_posix())


def _finding_sort_key(finding: Finding) -> tuple[str, str, str, int, str]:
    return (
        finding.scope,
        finding.path,
        finding.commit or "",
        finding.line or 0,
        finding.rule_id,
    )


def _replace_with_redacted(match: re.Match[str]) -> str:
    matched = match.group(0)
    if matched.startswith("Bearer "):
        return "Bearer [REDACTED]"
    if matched.startswith("-----BEGIN "):
        return matched.replace("PRIVATE KEY", "[REDACTED]")
    separator = "=" if "=" in matched else ":"
    if separator in matched and any(
        token in matched.lower()
        for token in ("token", "secret", "password", "passwd", "api_key", "apikey", "api-key")
    ):
        key, _value = matched.split(separator, 1)
        return f"{key}{separator} [REDACTED]"
    return "[REDACTED]"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="Repository root to scan.",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Directory for generated scan reports (defaults under --repo-root).",
    )
    parser.add_argument("--history-cache", type=Path, help="Trusted ancestor history cache; requires its externally pinned digest.")
    parser.add_argument("--history-cache-sha256", help="SHA-256 from the trusted cache producer's receipt.")
    parser.add_argument("--write-history-cache", type=Path, help="Write history-only cache alongside this scan for a trusted receipt to pin.")
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir is not None
        else repo_root / "docs" / "release" / "generated"
    )
    try:
        outputs = write_outputs(repo_root, output_dir, args.history_cache,
                                args.history_cache_sha256, args.write_history_cache)
    except (HistoryCacheError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"tracked secret scan failed: {error}\n")
    for path in outputs:
        print(_display_output_path(path, repo_root))
    return 0


def _display_output_path(path: Path, repo_root: Path) -> str:
    """Render a useful path even when an explicitly requested output is external."""
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


if __name__ == "__main__":
    raise SystemExit(main())
