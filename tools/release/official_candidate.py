#!/usr/bin/env python3
"""Prepare and verify a CI candidate; never upload or publish to a service."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import firmware_artifact as artifact
import firmware_compliance
import release_identity


PUBLIC_BASE_URL = "https://updates.foxieproducts.com/fw"
REPOSITORY = "foxieproducts/litch"
SOURCE_PATTERN = re.compile(r"^[0-9a-f]{40}$")
VERSION_HEADER = "include/firmware/firmware_version.hpp"
INITIAL_NOTES = "docs/release/initial_1.0.2.txt"
HISTORY_NOTES = "docs/release/firmware_notes.md"
BUILD_INPUTS = (
    "LICENSE",
    "lib/litch-max/LICENSE",
    "lib/litch-max.snapshot.json",
    "platformio.ini",
    VERSION_HEADER,
    INITIAL_NOTES,
    "boards/litch_esp32s3.json",
    "tools/release/release_identity.py",
    ".github/workflows/firmware_candidate.yml",
    "tools/release/official_candidate.py",
    "tools/release/github_promotion.py",
    "tools/release/firmware_artifact.py",
    "tools/release/firmware_compliance.py",
    "tools/release/firmware_license_manifest.json",
    "tools/release/firmware_relink_sources.json",
    "tools/release/relink_firmware.py",
    "tools/pio/capture_firmware_relink.py",
    "tools/pio/verify_litch_max.py",
    "tools/release/firmware_publication.py",
    "tools/release/firmware_provenance.py",
    ".github/workflows/firmware_provenance.yml",
    "tools/release/firmware_notes_amendment.py",
    "tools/release/publish_aws.py",
    ".github/workflows/firmware_host_check.yml",
    "tools/release/verify_release_artifacts.py",
    "tools/release/scan_tracked_secrets.py",
)

def fail(message: str) -> None:
    raise artifact.ArtifactValidationError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(root: Path, *args: str) -> bytes:
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, capture_output=True,
        timeout=15,
    ).stdout


def check_source(root: Path, expected_sha: str) -> None:
    if not SOURCE_PATTERN.fullmatch(expected_sha):
        fail("source must be a full lowercase commit SHA")
    if git(root, "rev-parse", "HEAD").decode().strip() != expected_sha:
        fail("checkout does not match the approved source SHA")
    if git(root, "status", "--porcelain", "--untracked-files=normal"):
        fail("candidate requires a clean checkout, including untracked source")


def validate_evidence_ref(ref: str) -> None:
    if not ref.strip() or len(ref.encode()) > 2048 or any(ord(ch) < 32 or ord(ch) == 127 for ch in ref):
        fail("invalid local validation reference")


def history_version_key(version: str) -> tuple[int, int, int, int]:
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9]+)?", version):
        fail("invalid firmware notes version")
    series, _, sequence = version.partition("-")
    try:
        major, minor, patch = map(int, series.split("."))
        return major, minor, patch, int(sequence) if sequence else 0
    except ValueError:
        fail("invalid firmware notes version")


def history_sections(data: bytes, version: str) -> list[bytes]:
    """Validate a customer history while keeping its section bytes intact."""
    if not data or len(data) > artifact.MAX_NOTES_SIZE:
        fail("firmware notes history size is outside the admission limit")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        fail("firmware notes history must be UTF-8")
    matches = list(re.finditer(rb"(?m)^## ([0-9]+\.[0-9]+\.[0-9]+(?:-[0-9]+)?)\n", data))
    if not matches or matches[0].start() != 0:
        fail("firmware notes history must start with a version heading")
    versions = [m.group(1).decode("ascii") for m in matches]
    keys = [history_version_key(v) for v in versions]
    if (versions[0] != version or any(a <= b for a, b in zip(keys, keys[1:]))):
        fail("firmware notes must start with the current version and be newest first")
    sections = [data[m.start():matches[i + 1].start() if i + 1 < len(matches) else len(data)]
                for i, m in enumerate(matches)]
    if any(not section.split(b"\n", 1)[1].strip() for section in sections):
        fail("firmware notes sections must not be empty")
    return sections


def validate_reviewed_history(data: bytes, original: bytes, version: str) -> None:
    sections = history_sections(data, version)
    prior = history_sections(original, version)
    if sections[1:] != prior[1:]:
        fail("reviewed notes must preserve earlier release sections")


def source_firmware_version(root: Path, commit: str) -> str:
    return release_identity.firmware_version(git(root, "show", f"{commit}:{VERSION_HEADER}").decode())


def series_tuple(version: str) -> tuple[int, ...]:
    return tuple(map(int, release_identity.release_series(version).split(".")))


def draft_notes(root: Path, source_sha: str, base_sha: str) -> bytes:
    if not SOURCE_PATTERN.fullmatch(base_sha):
        fail("notes base must be the full source SHA of the last official release")
    # The reviewed public launch is one new root, not a changelog range over
    # the retained old release tags. Subsequent commits keep normal ancestry.
    version = source_firmware_version(root, source_sha)
    if series_tuple(version) >= (1, 0, 3):
        if base_sha == source_sha:
            fail("no commits since the selected release; choose a newer candidate")
        git(root, "merge-base", "--is-ancestor", base_sha, source_sha)
        notes = git(root, "show", f"{source_sha}:{HISTORY_NOTES}")
        sections = history_sections(notes, version)
        initial = git(root, "show", f"{source_sha}:{INITIAL_NOTES}")
        if sections[-1] != b"## 1.0.2\n\n" + initial:
            fail("firmware notes must retain the complete original 1.0.2 text")
        return notes
    parents = git(root, "rev-list", "--parents", "-n", "1", source_sha).decode().split()
    if version == "1.0.2" and len(parents) == 1:
        git(root, "rev-parse", "--verify", base_sha + "^{commit}")
        notes = git(root, "show", f"{source_sha}:{INITIAL_NOTES}")
        notes.decode("utf-8")
        if not notes.strip() or len(notes) > artifact.MAX_NOTES_SIZE:
            fail("initial release notes must be nonempty and within the admission limit")
        if re.search(rb"(?m)^Version:", notes):
            fail("initial notes contain customer text only; the version is added from the build")
        return notes
    git(root, "merge-base", "--is-ancestor", base_sha, source_sha)
    subjects = git(root, "log", "--reverse", "--no-merges", "--format=%s",
                   f"{base_sha}..{source_sha}").decode("utf-8").splitlines()
    if not subjects:
        fail("no commits since the selected release; choose a newer candidate")
    notes = "## Changes\n\n" + "\n".join(f"- {subject}" for subject in subjects) + "\n"
    if len(notes.encode()) > artifact.MAX_NOTES_SIZE:
        fail("generated notes exceed the admission limit")
    return notes.encode()


def resolve_notes_base(root: Path, releases: list, channel: str, bootstrap: str) -> str:
    if channel not in ("beta", "stable"):
        fail("invalid notes channel")
    releases = release_identity.release_records(releases)
    official = sorted((r for r in releases if not r["draft"]
                       and r["tag_name"].startswith(f"fw-{channel}-")),
                      key=lambda r: r["published_at"], reverse=True)
    if official:
        base = git(root, "rev-parse", "--verify", official[0]["tag_name"] + "^{commit}").decode().strip()
        if bootstrap and bootstrap != base:
            fail("notes base must match the last official release")
    else:
        roots = git(root, "rev-list", "--max-parents=0", "HEAD").decode().splitlines()
        if len(roots) != 1:
            fail("cannot establish a single public source root for the first notes range")
        base = roots[0]
        if bootstrap and bootstrap != base:
            fail("first notes range must start at the public source root")
    if not SOURCE_PATTERN.fullmatch(base):
        fail("notes baseline must resolve to a full commit SHA")
    return base


def prepare_notes(root: Path, directory: Path, expected_sha: str, receipt_sha: str,
                  channel: str, base_sha: str, output: Path) -> None:
    receipt = verify(directory, expected_sha, receipt_sha)
    require_compliance(receipt)
    # Channel is distribution metadata. The compiled version is never rewritten.
    artifact.release_names(channel, receipt["version"], "0" * 64, "0" * 64)
    context = {
        "schema_version": 1, "source_sha": expected_sha,
        "build_receipt_sha256": receipt_sha, "channel": channel,
        "notes_base_sha": base_sha, "version": receipt["version"],
    }
    _write_notes_draft(root, expected_sha, base_sha, output, context)
    footer = source_footer(receipt)
    if footer:
        notes = output / "notes.md"
        notes.write_bytes(notes.read_bytes() + footer)


def prepare_source_notes(root: Path, expected_sha: str, channel: str,
                         base_sha: str, output: Path) -> None:
    """Prepare editable notes bound to reviewed source before compilation."""
    check_source(root, expected_sha)
    version = source_firmware_version(root, expected_sha)
    artifact.release_names(channel, version, "0" * 64, "0" * 64)
    _write_notes_draft(root, expected_sha, base_sha, output, {
        "schema_version": 2, "source_sha": expected_sha, "channel": channel,
        "notes_base_sha": base_sha, "version": version,
    })


def _write_notes_draft(root: Path, expected_sha: str, base_sha: str,
                       output: Path, context: dict) -> None:
    body = draft_notes(root, expected_sha, base_sha)
    if output.exists() or output.is_symlink():
        fail("notes draft already exists; do not overwrite owner edits")
    output.mkdir(parents=True)
    (output / "notes.md").write_bytes(body)
    (output / "notes-context.json").write_text(artifact.render_manifest_json(context))


def compliance_notice(descriptor: dict) -> bytes:
    return (
        "\n\n## Firmware source and relinking\n\n"
        "This firmware includes the Arduino-ESP32 core under LGPL-2.1-or-later "
        "and NeoPixelBus under LGPL-3.0-or-later. "
        "Corresponding source, license notices and materials for relinking with "
        "modified versions of these libraries are available in the "
        f"[firmware compliance bundle]({PUBLIC_BASE_URL}/{descriptor['filename']}).\n"
    ).encode()


def require_compliance(receipt: dict) -> dict:
    descriptor = receipt.get("compliance_bundle")
    if receipt.get("schema_version") not in (2, 3) or not isinstance(descriptor, dict):
        fail("publication requires a firmware compliance bundle; historical candidates are read-only")
    return descriptor


def separate_source_notice(receipt: dict) -> bool:
    presentation = receipt.get("notes_presentation")
    if presentation not in (None, "customer-text-with-source-asset",
                            "customer-text-with-manifest-source"):
        fail("unsupported release notes presentation")
    return presentation is not None


def manifest_source_notice(receipt: dict) -> bool:
    return receipt.get("notes_presentation") == "customer-text-with-manifest-source"


def source_notice_name(descriptor: dict) -> str:
    return "firmware_notices_" + sha256(compliance_notice(descriptor)) + ".md"


def source_footer(receipt: dict) -> bytes:
    if not separate_source_notice(receipt) or manifest_source_notice(receipt):
        return b""
    name = source_notice_name(require_compliance(receipt))
    return f"\n\n[Source and licenses]({PUBLIC_BASE_URL}/{name})\n".encode()


def write_payloads(stage: Path, firmware_bytes: bytes, notes_bytes: bytes,
                   channel: str, version: str, compliance: dict | None = None,
                   bundle: Path | None = None, *, separate_notice: bool = False,
                   manifest_source: bool = False) -> None:
    if manifest_source and (not separate_notice or compliance is None):
        fail("manifest source link requires separate admitted source materials")
    info = artifact.inspect_firmware_bytes(firmware_bytes)
    if compliance is not None:
        notice = compliance_notice(compliance)
        if notes_bytes.endswith(notice):
            notes_bytes = notes_bytes[:-len(notice)]
        if not separate_notice:
            notes_bytes += notice
    # Older deployed clients extract this line from version[_beta].md.
    notes_bytes = f"Version: {version}\n\n".encode() + notes_bytes
    names = artifact.release_names(channel, version, info.sha256, sha256(notes_bytes))
    (stage / "assets").mkdir()
    (stage / "catalog").mkdir()
    for key in ("firmware_alias", "firmware_immutable"):
        (stage / "assets" / names[key]).write_bytes(firmware_bytes)
    for key in ("notes_alias", "notes_immutable"):
        (stage / "assets" / names[key]).write_bytes(notes_bytes)
    if compliance is not None:
        shutil.copyfile(bundle, stage / "assets" / compliance["filename"])
        if separate_notice:
            (stage / "assets" / source_notice_name(compliance)).write_bytes(notice)
    artifact.create_manifest(
        firmware_path=stage / "assets" / names["firmware_immutable"],
        notes_path=stage / "assets" / names["notes_immutable"],
        channel=channel, version=version, public_base_url=PUBLIC_BASE_URL,
        output_path=stage / "catalog" / names["manifest"],
        source_path=(stage / "assets" / source_notice_name(compliance)
                     if manifest_source else None),
    )


def write_receipt(stage: Path, receipt: dict) -> str:
    receipt["files"] = {
        path.relative_to(stage).as_posix(): sha256(path.read_bytes())
        for folder in ("assets", "catalog")
        for path in sorted((stage / folder).iterdir())
    }
    data = artifact.render_manifest_json(receipt).encode()
    (stage / "candidate.json").write_bytes(data)
    digest = sha256(data)
    verify(stage, receipt["source_sha"], digest)
    return digest


def prepare(root: Path, firmware: Path, channel: str,
            expected_sha: str, base_sha: str, output: Path,
            compliance_bundle: Path | None = None, local_validation_ref: str | None = None) -> str:
    """Build an immutable directory only after all local bytes pass admission.

    A clean, admitted CI build establishes source-to-binary attribution. This
    tool validates bytes, not an arbitrary binary's claimed source history.
    """
    check_source(root, expected_sha)
    license_bytes = firmware_compliance.source_license(root, expected_sha)
    litch_max_license = firmware_compliance.source_license(root, expected_sha, "lib/litch-max/LICENSE")
    litch_max_snapshot = firmware_compliance.source_snapshot(root, expected_sha)
    version = source_firmware_version(root, expected_sha)
    series = series_tuple(version)
    firmware_info = artifact.inspect_firmware(firmware)
    firmware_bytes = firmware.read_bytes()
    if sha256(firmware_bytes) != firmware_info.sha256:
        fail("firmware changed during admission")
    if compliance_bundle is None:
        fail("new candidates require --compliance-bundle")
    compliance = firmware_compliance.inspect_bundle(
        compliance_bundle, firmware_info.sha256, expected_sha,
        expected_license_sha256=sha256(license_bytes),
        expected_litch_max_license_sha256=sha256(litch_max_license),
        expected_litch_max_snapshot_sha256=sha256(litch_max_snapshot))
    notes_bytes = draft_notes(root, expected_sha, base_sha)
    manifest_source = series >= (1, 0, 2)
    if output.exists() or output.is_symlink():
        fail("candidate output already exists; use a new run directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".candidate-", dir=output.parent) as temporary:
        stage = Path(temporary) / "candidate"
        stage.mkdir()
        write_payloads(stage, firmware_bytes, notes_bytes, channel, version,
                       compliance, compliance_bundle, separate_notice=True,
                       manifest_source=manifest_source)
        inputs = BUILD_INPUTS
        if series >= (1, 0, 3):
            inputs = (*inputs, HISTORY_NOTES)
        receipt = {
            "schema_version": 3,
            "repository": REPOSITORY,
            "source_sha": expected_sha,
            "channel": channel,
            "version": version,
            "target": "esp32s3",
            "notes_base_sha": base_sha,
            "notes_status": "draft",
            "notes_presentation": ("customer-text-with-manifest-source" if manifest_source
                                   else "customer-text-with-source-asset"),
            "compliance_bundle": compliance,
            "input_sha256": {
                path: sha256(litch_max_snapshot if path == "lib/litch-max.snapshot.json"
                             else litch_max_license if path == "lib/litch-max/LICENSE"
                             else git(root, "show", f"{expected_sha}:{path}"))
                for path in inputs
            },
        }
        execution = {"run_id": os.environ.get("GITHUB_RUN_ID"),
                     "run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT")}
        if any(value is not None for value in execution.values()):
            if any(not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]*", value)
                   for value in execution.values()):
                fail("GitHub build requires a positive run ID and attempt together")
            receipt["build_execution"] = execution
        if local_validation_ref is not None:
            validate_evidence_ref(local_validation_ref)
            receipt["local_validation"] = {
                "status": "attested_by_dispatcher", "source_sha": expected_sha,
                "evidence_ref": local_validation_ref.strip(),
            }
            receipt["validation"] = {"product_tests_and_analysis": "not_run_in_ci",
                                     "canonical_s3_build": "passed", "release_artifacts": "passed",
                                     "tracked_secret_scan_high_confidence": "clear"}
        receipt_sha = write_receipt(stage, receipt)
        check_source(root, expected_sha)
        # Output is new and immutable. No prior candidate/catalog is replaced.
        os.rename(stage, output)
    return receipt_sha


def finalize_notes(directory: Path, expected_sha: str, receipt_sha: str,
                   notes: Path, output: Path, context: Path | None = None,
                   notes_sha256: str | None = None,
                   notes_context_sha256: str | None = None, *,
                   historical_read_only: bool = False) -> str:
    """Freeze owner-edited notes, preserving the verified firmware bytes."""
    receipt = verify(directory, expected_sha, receipt_sha)
    if not historical_read_only:
        require_compliance(receipt)
    original_channel = receipt["channel"]
    if context is not None:
        if context.is_symlink() or context.stat().st_size > 4096:
            fail("invalid notes context")
        context_data = context.read_bytes()
        if notes_context_sha256 is not None and (
                not artifact.SHA256_PATTERN.fullmatch(notes_context_sha256)
                or sha256(context_data) != notes_context_sha256):
            fail("reviewed notes context changed after approval")
        review = json.loads(context_data)
        build_bound = (review.get("schema_version") == 1
                       and review.get("build_receipt_sha256") == receipt_sha)
        source_bound = (review.get("schema_version") == 2
                        and set(review) == {"schema_version", "source_sha", "channel",
                                            "version", "notes_base_sha"})
        if (not (build_bound or source_bound) or review.get("source_sha") != expected_sha
                or review.get("version") != receipt["version"]
                or review.get("channel") not in ("beta", "stable")
                or not SOURCE_PATTERN.fullmatch(review.get("notes_base_sha", ""))):
            fail("notes context does not match the admitted build")
        if source_bound and (notes_sha256 is None
                             or not artifact.SHA256_PATTERN.fullmatch(notes_sha256)):
            fail("source-bound notes require the approved notes SHA-256")
        if source_bound and notes_context_sha256 is None:
            fail("source-bound notes require the approved context SHA-256")
        receipt["channel"] = review["channel"]
        receipt["notes_base_sha"] = review["notes_base_sha"]
    elif notes_context_sha256 is not None:
        fail("approved notes context SHA-256 requires a notes context")
    artifact.inspect_release_notes(notes)
    data = notes.read_bytes()
    if notes_sha256 is not None and (not artifact.SHA256_PATTERN.fullmatch(notes_sha256)
                                     or sha256(data) != notes_sha256):
        fail("reviewed notes changed after approval")
    if not data.strip():
        fail("reviewed notes must not be blank")
    if re.search(rb"(?m)^Version:", data):
        fail("edit customer notes only; the version header is added from the verified build")
    footer = source_footer(receipt)
    if footer and data.endswith(footer):
        data = data[:-len(footer)]
    if not data.strip():
        fail("reviewed customer notes must not be blank")
    if HISTORY_NOTES in receipt.get("input_sha256", {}):
        original_notes = (directory / "assets" / ("version_beta.md" if original_channel == "beta" else "version.md")).read_bytes()
        prefix = f"Version: {receipt['version']}\n\n".encode()
        if not original_notes.startswith(prefix):
            fail("candidate notes lack their version header")
        validate_reviewed_history(data, original_notes[len(prefix):], receipt["version"])
    if output.exists() or output.is_symlink():
        fail("final candidate already exists; use a new output directory")
    alias = "firmware_beta.bin" if original_channel == "beta" else "firmware.bin"
    firmware = (directory / "assets" / alias).read_bytes()
    if sha256(firmware) != receipt["files"][f"assets/{alias}"]:
        fail("firmware changed after admission")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".final-notes-", dir=output.parent) as temporary:
        stage = Path(temporary) / "candidate"
        stage.mkdir()
        compliance = receipt.get("compliance_bundle")
        bundle = directory / "assets" / compliance["filename"] if compliance else None
        write_payloads(stage, firmware, data, receipt["channel"], receipt["version"],
                       compliance, bundle, separate_notice=separate_source_notice(receipt),
                       manifest_source=manifest_source_notice(receipt))
        receipt["notes_status"] = "reviewed"
        receipt.pop("notes_review_sha256", None)
        receipt.pop("notes_review_context_sha256", None)
        if notes_sha256 is not None:
            receipt["notes_review_sha256"] = notes_sha256
        if notes_context_sha256 is not None:
            receipt["notes_review_context_sha256"] = notes_context_sha256
        receipt["build_receipt_sha256"] = receipt_sha
        final_sha = write_receipt(stage, receipt)
        os.rename(stage, output)
    return final_sha


def verify(candidate: Path, expected_sha: str, receipt_sha: str, *,
           expected_repository: str = REPOSITORY) -> dict:
    """Require the independently retained CI receipt digest and source SHA."""
    if not SOURCE_PATTERN.fullmatch(expected_sha):
        fail("source must be a full lowercase commit SHA")
    if not artifact.SHA256_PATTERN.fullmatch(receipt_sha):
        fail("receipt digest must be lowercase SHA-256")
    receipt_path = candidate / "candidate.json"
    if receipt_path.is_symlink() or receipt_path.stat().st_size > 32768:
        fail("invalid candidate receipt")
    receipt_bytes = receipt_path.read_bytes()
    if sha256(receipt_bytes) != receipt_sha:
        fail("candidate receipt does not match the approved digest")
    receipt = json.loads(receipt_bytes)
    if (not isinstance(receipt, dict) or receipt.get("schema_version") not in (1, 2, 3)
            or receipt.get("repository") != expected_repository
            or receipt.get("source_sha") != expected_sha
            or receipt.get("target") != "esp32s3"
            or receipt.get("notes_status") not in ("draft", "reviewed")):
        fail("candidate receipt has the wrong source, repository, schema or target")
    execution = receipt.get("build_execution")
    if execution is not None and (
            not isinstance(execution, dict) or set(execution) != {"run_id", "run_attempt"}
            or any(not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]*", value)
                   for value in execution.values())):
        fail("candidate receipt has an invalid GitHub build execution")
    # Retained pre-policy receipts remain readable, but cannot be newly published.
    if receipt["schema_version"] == 3:
        release_identity.release_series(receipt["version"])
    elif any(receipt.get("version", "").startswith(series)
             for series in release_identity.SUPPORTED_SERIES):
        series = release_identity.release_series(receipt["version"])
        app_identity = receipt.get("app_identity", {})
        if (app_identity.get("version") != receipt["version"]
                or app_identity.get("native_version") != series
                or not release_identity.valid_package_code(app_identity.get("package_code"))):
            fail("candidate app identity does not match the firmware version/native package")
    channel = receipt.get("channel")
    if channel not in ("stable", "beta"):
        fail("candidate channel must be stable or beta")
    manifest_name = "manifest.json" if channel == "stable" else "manifest_beta.json"
    manifest_path = candidate / "catalog" / manifest_name
    if manifest_path.is_symlink() or manifest_path.stat().st_size > 32768:
        fail("invalid candidate manifest")
    manifest = json.loads(manifest_path.read_text())
    names = artifact.release_names(
        channel, receipt["version"], manifest["artifact"]["sha256"], manifest["notes"]["sha256"],
    )
    expected_files = {"candidate.json", f"catalog/{manifest_name}"}
    expected_files.update(f"assets/{names[key]}" for key in (
        "firmware_alias", "firmware_immutable", "notes_alias", "notes_immutable",
    ))
    compliance = receipt.get("compliance_bundle")
    separate_notice = separate_source_notice(receipt)
    if separate_notice and receipt["schema_version"] == 1:
        fail("separate source notices require a bound compliance bundle")
    bundle_relative = None
    license_sha = None
    litch_max_license_sha = None
    litch_max_snapshot_sha = None
    if receipt["schema_version"] >= 2:
        require_compliance(receipt)
        inputs = receipt.get("input_sha256")
        if (not isinstance(inputs, dict) or not isinstance(inputs.get("LICENSE"), str)
                or not artifact.SHA256_PATTERN.fullmatch(inputs["LICENSE"])):
            fail("candidate receipt must bind the source LICENSE hash")
        license_sha = inputs["LICENSE"]
        litch_max_license_sha = inputs.get("lib/litch-max/LICENSE")
        if (not isinstance(litch_max_license_sha, str)
                or not artifact.SHA256_PATTERN.fullmatch(litch_max_license_sha)):
            fail("unsupported pre-extraction or incomplete candidate: Litch-Max LICENSE hash "
                 "is required; create a fresh qualified build")
        litch_max_snapshot_sha = inputs.get("lib/litch-max.snapshot.json")
        if (not isinstance(litch_max_snapshot_sha, str)
                or not artifact.SHA256_PATTERN.fullmatch(litch_max_snapshot_sha)):
            fail("unsupported pre-extraction or incomplete candidate: Litch-Max snapshot hash "
                 "is required; create a fresh qualified build")
        if (set(compliance) != {"filename", "size_bytes", "sha256", "firmware_sha256", "source_sha"}
                or not isinstance(compliance.get("sha256"), str)
                or not artifact.SHA256_PATTERN.fullmatch(compliance["sha256"])
                or compliance.get("filename") != f"firmware_compliance_{compliance['sha256']}.tar.gz"
                or type(compliance.get("size_bytes")) is not int
                or not 0 < compliance["size_bytes"] <= firmware_compliance.MAX_BUNDLE_SIZE
                or compliance.get("source_sha") != expected_sha
                or compliance.get("firmware_sha256") != manifest["artifact"]["sha256"]):
            fail("invalid firmware compliance descriptor or source/firmware binding")
        bundle_relative = f"assets/{compliance['filename']}"
        expected_files.add(bundle_relative)
        if separate_notice:
            expected_files.add("assets/" + source_notice_name(compliance))
    elif compliance is not None:
        fail("historical receipt cannot add a firmware compliance descriptor")
    paths = list(candidate.rglob("*"))
    if any(path.is_symlink() for path in paths):
        fail("candidate must not contain symlinks")
    if {p.relative_to(candidate).as_posix() for p in paths if not p.is_dir()} != expected_files:
        fail("candidate contains missing or unexpected files")
    if set(receipt.get("files", {})) != expected_files - {"candidate.json"}:
        fail("receipt must describe exactly the publication files")
    for relative, digest in receipt["files"].items():
        path = candidate / relative
        if (
            not path.is_file()
            or path.stat().st_size > (firmware_compliance.MAX_BUNDLE_SIZE
                                     if relative == bundle_relative
                                     else artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE)
        ):
            fail("candidate file is not a bounded regular file")
        if sha256(path.read_bytes()) != digest:
            fail(f"candidate bytes do not match the receipt: {relative}")
    expected_manifest = artifact.verify_release(
        firmware_path=candidate / "assets" / names["firmware_immutable"],
        notes_path=candidate / "assets" / names["notes_immutable"],
        manifest_path=manifest_path, manifest_url=f"{PUBLIC_BASE_URL}/{manifest_name}",
        source_path=(candidate / "assets" / source_notice_name(compliance)
                     if manifest_source_notice(receipt) else None),
    )
    if expected_manifest["channel"] != channel or expected_manifest["version"] != receipt["version"]:
        fail("manifest identity does not match the candidate")
    for key, object_name in (("firmware_immutable", "artifact"), ("notes_immutable", "notes")):
        if expected_manifest[object_name]["url"] != f"{PUBLIC_BASE_URL}/{names[key]}":
            fail("manifest must name the exact app delivery path")
    for alias, immutable in (("firmware_alias", "firmware_immutable"), ("notes_alias", "notes_immutable")):
        if (candidate / "assets" / names[alias]).read_bytes() != (candidate / "assets" / names[immutable]).read_bytes():
            fail("compatibility alias does not match immutable bytes")
    if compliance is not None:
        inspected = firmware_compliance.inspect_bundle(
            candidate / bundle_relative, manifest["artifact"]["sha256"], expected_sha,
            expected_license_sha256=license_sha,
            expected_litch_max_license_sha256=litch_max_license_sha,
            expected_litch_max_snapshot_sha256=litch_max_snapshot_sha)
        if inspected != compliance:
            fail("firmware compliance bundle does not match its receipt")
        if separate_notice:
            if (candidate / "assets" / source_notice_name(compliance)).read_bytes() != compliance_notice(compliance):
                fail("source notice must retain the canonical public firmware compliance link")
        elif not (candidate / "assets" / names["notes_immutable"]).read_bytes().endswith(compliance_notice(compliance)):
            fail("release notes must include the canonical public firmware compliance link")
    return receipt


def stage_catalog(candidate: Path, expected_sha: str, receipt_sha: str,
                  readback: Path, output: Path) -> None:
    """Stage a local catalog pointer after comparing every downloaded asset.

    The host adapter must supply fresh, successful HTTPS readbacks,
    serialize promotion, atomically publish this file, and verify it remotely.
    This function alone does not establish remote delivery or publish anything.
    """
    receipt = verify(candidate, expected_sha, receipt_sha)
    require_compliance(receipt)
    for relative, digest in receipt["files"].items():
        if not relative.startswith("assets/"):
            continue
        remote = readback / Path(relative).name
        local = candidate / relative
        if (remote.is_symlink() or not remote.is_file()
                or remote.stat().st_size != local.stat().st_size
                or sha256(remote.read_bytes()) != digest):
            fail(f"asset readback mismatch; catalog withheld: {Path(relative).name}")
    name = "manifest.json" if receipt["channel"] == "stable" else "manifest_beta.json"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output, prefix=".catalog-", delete=False) as handle:
        temporary = Path(handle.name)
        handle.write((candidate / "catalog" / name).read_bytes())
    try:
        os.replace(temporary, output / name)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    prepare_parser.add_argument("--local-validation-ref")
    prepare_parser.add_argument("--firmware", type=Path, required=True)
    prepare_parser.add_argument("--compliance-bundle", type=Path, required=True)
    prepare_parser.add_argument("--notes-base-sha", required=True)
    prepare_parser.add_argument("--channel", choices=("stable", "beta"), required=True)
    prepare_parser.add_argument("--output", type=Path, required=True)
    source_notes = commands.add_parser("prepare-source-notes")
    source_notes.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    source_notes.add_argument("--channel", choices=("stable", "beta"), required=True)
    source_notes.add_argument("--notes-base-sha", required=True)
    source_notes.add_argument("--output", type=Path, required=True)
    for name in ("verify", "stage-catalog", "finalize-notes", "prepare-notes"):
        command = commands.add_parser(name)
        command.add_argument("--candidate", type=Path, required=True)
        command.add_argument("--receipt-sha256", required=True)
        if name == "stage-catalog":
            command.add_argument("--readback", type=Path, required=True)
            command.add_argument("--output", type=Path, required=True)
        if name == "finalize-notes":
            command.add_argument("--notes", type=Path, required=True)
            command.add_argument("--notes-context", type=Path)
            command.add_argument("--notes-sha256")
            command.add_argument("--notes-context-sha256")
            command.add_argument("--output", type=Path, required=True)
        if name == "prepare-notes":
            command.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
            command.add_argument("--channel", choices=("stable", "beta"), required=True)
            command.add_argument("--notes-base-sha", required=True)
            command.add_argument("--output", type=Path, required=True)
    for command in commands.choices.values():
        command.add_argument("--source-sha", required=True)
    base_parser = commands.add_parser("notes-base")
    base_parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    base_parser.add_argument("--releases", type=Path, required=True)
    base_parser.add_argument("--channel", choices=("stable", "beta"), required=True)
    base_parser.add_argument("--bootstrap", default="")
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            print(prepare(args.root, args.firmware, args.channel, args.source_sha,
                          args.notes_base_sha, args.output, args.compliance_bundle, args.local_validation_ref))
        elif args.command == "verify":
            verify(args.candidate, args.source_sha, args.receipt_sha256)
        elif args.command == "finalize-notes":
            print(finalize_notes(args.candidate, args.source_sha, args.receipt_sha256, args.notes, args.output,
                                 args.notes_context, args.notes_sha256, args.notes_context_sha256))
        elif args.command == "prepare-source-notes":
            prepare_source_notes(args.root, args.source_sha, args.channel, args.notes_base_sha, args.output)
        elif args.command == "prepare-notes":
            prepare_notes(args.root, args.candidate, args.source_sha, args.receipt_sha256,
                          args.channel, args.notes_base_sha, args.output)
        elif args.command == "notes-base":
            print(resolve_notes_base(args.root, json.loads(args.releases.read_text()),
                                     args.channel, args.bootstrap))
        else:
            stage_catalog(args.candidate, args.source_sha, args.receipt_sha256, args.readback, args.output)
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
        # Never echo subprocess output: source notes and local details stay private.
        print(f"candidate rejected: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
