#!/usr/bin/env python3
"""Prepare/check the shared source identity; never build, allocate remotely or publish."""

from __future__ import annotations

import argparse
import hashlib
from datetime import datetime
import firmware_artifact as artifact
import json
from pathlib import Path
import re
import sys

SERIES = "1.0.2"
BARE_INITIAL_VERSION = "1.0.2"
ZERO_START_SERIES = ("0.9.7", "0.9.8", "0.9.9", "1.0.0", "1.0.1", SERIES)
SUPPORTED_SERIES = ("0.9.0", "0.9.5", *ZERO_START_SERIES)
RESERVED_BETA_VERSIONS = frozenset((
    "0.9.0-2", "0.9.0-3", "0.9.0-4", "0.9.0-5", "0.9.0-6", "0.9.0-7",
    "0.9.5-10", "0.9.5-11", "0.9.5-13", "0.9.7-0", "0.9.7-5", "0.9.7-6", "0.9.7-10",
    "0.9.8-0", "0.9.9-0",
))
PUBLIC_BASE_URL = "https://foxielitch.com/fw"
MAX_PACKAGE_CODE = 9999
VERSION = re.compile(
    rf"(?:({'|'.join(map(re.escape, SUPPORTED_SERIES))})-(0|[1-9][0-9]*)|{re.escape(BARE_INITIAL_VERSION)})\Z"
)
HEADER = "include/firmware/firmware_version.hpp"
PUBSPEC = "app/pubspec.yaml"
APP_IDENTITY = "app/lib/src/models/app_release_identity.dart"
SOURCE_PATHS = (HEADER, PUBSPEC, APP_IDENTITY)
HEADER_VERSION = re.compile(r'(FIRMWARE_VERSION\s*=\s*")([^"\n]+)("\s*;)')
PUBSPEC_VERSION = re.compile(r'(?m)^(version: )([0-9]+\.[0-9]+\.[0-9]+)\+([1-9][0-9]*)(\s*)$')
APP_VERSION = re.compile(
    r"(const AppReleaseIdentity litchAppReleaseIdentity = AppReleaseIdentity\(\n)"
    r"  version: '([^'\n]*)',\n  nativeVersion: '([^'\n]+)',\n"
    r"  buildNumber: '([1-9][0-9]*)',\n(\);)"
)


def canonical_version(version: str) -> str:
    """The first 1.0.2 release and its retained -0 spelling consume one identity."""
    return BARE_INITIAL_VERSION if version == BARE_INITIAL_VERSION + "-0" else version


def sequence(version: str) -> int:
    if version == BARE_INITIAL_VERSION:
        return 0
    match = VERSION.fullmatch(version)
    if (not match or len(version) > 64
            or (match[1] not in ZERO_START_SERIES and match[2] == "0")):
        choices = " or ".join(f"{series}-N" for series in SUPPORTED_SERIES)
        raise ValueError(f"release identity must be {choices}; N starts at 0 for "
                         f"{' and '.join(ZERO_START_SERIES)}, "
                         "at 1 for older series, without leading zeros")
    return int(match[2])


def release_series(version: str) -> str:
    sequence(version)
    return version.split("-", 1)[0]


def release_records(value: list) -> list:
    """Accept the complete GitHub releases response, including --paginate --slurp."""
    if not isinstance(value, list):
        raise ValueError("release history must be a complete GitHub release list")
    records = [r for page in value for r in page] if value and isinstance(value[0], list) else value
    if len(records) > 10000 or any(not isinstance(r, dict) for r in records):
        raise ValueError("invalid or oversized release history")
    return records


def next_beta_version(releases: list, *, series: str = SERIES) -> str:
    # Drafts, candidate tags and local builds do not consume N. A stable-only
    # first 1.0.2 release occupies zero; later stable promotions do not advance N.
    if series not in SUPPORTED_SERIES:
        raise ValueError("unsupported release series")
    numbers = []
    for release in release_records(releases):
        if type(release.get("draft")) is not bool or not isinstance(release.get("tag_name"), str):
            raise ValueError("incomplete GitHub release record")
        if release["draft"]:
            continue
        initial_stable = release["tag_name"] in (
            f"fw-stable-{BARE_INITIAL_VERSION}", f"fw-stable-{BARE_INITIAL_VERSION}-0")
        if not release["tag_name"].startswith("fw-beta-") and not initial_stable:
            continue
        if not release.get("published_at"):
            raise ValueError("published beta is missing its publication time")
        version = release["tag_name"].split("-", 2)[2]
        if any(version.startswith(known) for known in SUPPORTED_SERIES):
            if release_series(version) == series:
                numbers.append(sequence(version))
    return canonical_version(f"{series}-{max(numbers, default=-1 if series in ZERO_START_SERIES else 0) + 1}")


def _unpublished_beta_expectation(version: str, releases: list) -> str:
    """Validate target non-use/history and return the ordinary beta expectation."""
    series = release_series(version)
    records = release_records(releases)
    for record in records:
        if (record.get("draft") is False and isinstance(record.get("tag_name"), str)
                and any(record["tag_name"].startswith(prefix)
                        and canonical_version(record["tag_name"].removeprefix(prefix))
                        == canonical_version(version)
                        for prefix in ("fw-beta-", "fw-stable-"))):
            if not record.get("published_at"):
                raise ValueError("matching release is missing its publication time; cannot prove identity unused")
            raise ValueError("version already published; do not rebuild a used identity")
    return next_beta_version(records, series=series)


def check_next_beta(version: str, releases: list, published_beta: str | None = None) -> None:
    expected = _unpublished_beta_expectation(version, releases)
    # Before the first GitHub release here, the live Beta catalog owns the
    # sequence. It also prevents reusing an identity after an interrupted record.
    if published_beta and not VERSION.fullmatch(published_beta):
        raise ValueError("verify the previous beta release and public manifest before preparation")
    if published_beta and release_series(published_beta) == release_series(version):
        if sequence(version) <= sequence(published_beta):
            raise ValueError("beta version already deployed or superseded")
        if sequence(published_beta) >= sequence(expected):
            expected = f"{release_series(version)}-{sequence(published_beta) + 1}"
    # Some historical beta numbers were reserved before publication.
    # Allow those numbers while still rejecting an already published version.
    if version in RESERVED_BETA_VERSIONS and sequence(expected) <= sequence(version):
        return
    if version != expected:
        raise ValueError(f"next unpublished beta is {expected}; do not renumber a retained candidate")


def check_public_baseline_for_migration(manifest: dict) -> None:
    """Require the deployed Beta catalog and its canonical immutable asset URLs."""
    if (not isinstance(manifest, dict) or manifest.get("schema_version") != 1
            or manifest.get("channel") != "beta" or manifest.get("target") != "esp32s3"
            or manifest.get("chip_id") != 9 or not VERSION.fullmatch(manifest.get("version", ""))):
        raise ValueError("verify the previous beta release and public manifest before preparation")
    image, notes = manifest["artifact"], manifest["notes"]
    names = artifact.release_names("beta", manifest["version"], image["sha256"], notes["sha256"])
    origin = "https://updates.foxieproducts.com/fw/"
    if image["url"] != origin + names["firmware_immutable"] or notes["url"] != origin + names["notes_immutable"]:
        raise ValueError("public bootstrap catalog must use the app's immutable URLs")


def check_public_baseline(releases: list, manifest: dict, recovered_receipt: Path | None = None,
                          recovered_sha256: str | None = None) -> None:
    """A missing/mismatched public catalog needs receipt recovery, not allocation."""
    if recovered_receipt is not None or recovered_sha256 is not None:
        if recovered_receipt is None or recovered_sha256 is None:
            raise ValueError("recovered receipt and independently retained digest are required together")
        check_recovered_publication(manifest, recovered_receipt, recovered_sha256)
    official = [r for r in release_records(releases)
                if r.get("draft") is False and r.get("tag_name", "").startswith("fw-beta-")]
    if not official:
        # The public repository starts after the original published catalogs.
        # Bind the supplied fresh Beta snapshot to the canonical app endpoint.
        check_public_baseline_for_migration(manifest)
        return
    if any(not r.get("published_at") for r in official):
        raise ValueError("verify the previous beta release and public manifest before preparation")
    latest = max(official, key=lambda r: r["published_at"])
    if (not isinstance(manifest, dict) or manifest.get("schema_version") != 1
            or manifest.get("channel") != "beta" or manifest.get("target") != "esp32s3"
            or manifest.get("version") != latest["tag_name"].removeprefix("fw-beta-")):
        if recovered_receipt is None or recovered_sha256 is None:
            raise ValueError("public beta and release history disagree; recover the exact publication receipt first")
        receipt = json.loads(recovered_receipt.read_bytes())
        try:
            recovered_at = datetime.fromisoformat(receipt["utc"].replace("Z", "+00:00"))
            published_at = datetime.fromisoformat(latest["published_at"].replace("Z", "+00:00"))
            if recovered_at.tzinfo is None or published_at.tzinfo is None or recovered_at <= published_at:
                raise ValueError("recovered publication is stale relative to official history")
        except (KeyError, TypeError, AttributeError) as error:
            raise ValueError("recovered publication/history timestamps are invalid") from error



def check_recovered_publication(manifest: dict, receipt_path: Path, expected_sha256: str) -> None:
    """Reconcile a pinned manual exposure without claiming an official CI release."""
    if (not artifact.SHA256_PATTERN.fullmatch(expected_sha256) or receipt_path.is_symlink()
            or receipt_path.stat().st_size > 65536):
        raise ValueError("invalid recovered publication receipt")
    raw = receipt_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError("recovered publication receipt digest mismatch")
    receipt = json.loads(raw)
    if (not isinstance(receipt, dict) or receipt.get("official_ci_release") is not False
            or receipt.get("status") != "TEMPORARY_MANUAL_PUBLICATION_VERIFIED"
            or receipt.get("target") != PUBLIC_BASE_URL
            or not re.fullmatch(r"[0-9a-f]{40}", receipt.get("source", ""))
            or manifest.get("schema_version") != 1 or manifest.get("channel") != "beta"
            or manifest.get("target") != "esp32s3" or manifest.get("chip_id") != 9
            or receipt.get("version") != manifest.get("version")):
        raise ValueError("recovered publication identity mismatch")
    payloads = receipt.get("payloads", {})
    events = receipt.get("events", [])
    if not isinstance(payloads, dict) or not isinstance(events, list):
        raise ValueError("incomplete recovered publication")
    image, notes = manifest.get("artifact", {}), manifest.get("notes", {})
    expected = {}
    for channel in ("stable", "beta"):
        names = artifact.release_names(channel, manifest["version"], image["sha256"], notes["sha256"])
        for field in ("firmware_alias", "firmware_immutable"):
            expected[names[field]] = {"bytes": image["size_bytes"], "sha256": image["sha256"]}
        for field in ("notes_alias", "notes_immutable"):
            expected[names[field]] = {"bytes": notes["size_bytes"], "sha256": notes["sha256"]}
    raw_manifest = artifact.render_manifest_json(manifest).encode()
    expected["manifest_beta.json"] = {"bytes": len(raw_manifest), "sha256": hashlib.sha256(raw_manifest).hexdigest()}
    # Stable catalog has its own channel/URL bytes; the pinned receipt binds that distinct payload.
    stable = payloads.get("manifest.json", {})
    if (not isinstance(stable, dict) or type(stable.get("bytes")) is not int or stable["bytes"] <= 0
            or not artifact.SHA256_PATTERN.fullmatch(stable.get("sha256", ""))):
        raise ValueError("invalid recovered stable catalog evidence")
    expected["manifest.json"] = stable
    if (payloads != expected or len(events) != 10
            or any(not isinstance(e, dict) or e.get("status") != "HTTPS_VERIFIED" for e in events)
            or {e.get("name") for e in events} != set(expected)
            or image.get("url") != f"{PUBLIC_BASE_URL}/" + artifact.release_names("beta", manifest["version"], image["sha256"], notes["sha256"])["firmware_immutable"]
            or notes.get("url") != f"{PUBLIC_BASE_URL}/" + artifact.release_names("beta", manifest["version"], image["sha256"], notes["sha256"])["notes_immutable"]):
        raise ValueError("recovered publication assets/catalog or complete verification mismatch")


def single(pattern: re.Pattern, text: str, label: str) -> re.Match:
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ValueError(f"source must declare exactly one {label}")
    return matches[0]


def valid_package_code(value: int) -> bool:
    return type(value) is int and 1 <= value <= MAX_PACKAGE_CODE


def source_identity(texts: dict[str, str]) -> dict:
    firmware = single(HEADER_VERSION, texts[HEADER], "firmware version")[2]
    pubspec = single(PUBSPEC_VERSION, texts[PUBSPEC], "native app version/build")
    app = single(APP_VERSION, texts[APP_IDENTITY], "app release identity")
    series = release_series(firmware)
    if (app[2] != firmware or app[3] != series or pubspec[2] != series
            or app[4] != pubspec[3]):
        raise ValueError("app and firmware release identities/native package inputs disagree")
    code = int(app[4])
    if not valid_package_code(code):
        raise ValueError("native package code must be an integer in 1..9999")
    return {"version": firmware, "native_version": series, "package_code": code}


def read_sources(root: Path) -> dict[str, str]:
    return {name: (root / name).read_text() for name in SOURCE_PATHS}


def check(root: Path) -> dict:
    return source_identity(read_sources(root))


def prepare(root: Path, version: str, package_code: int, previous_package_code: int,
            releases: list, manifest: dict, recovered_receipt: Path | None = None,
            recovered_sha256: str | None = None) -> dict:
    """Set release versions using the supplied publication history and package-code floor.

    Reconcile incomplete publication receipts with the release history before calling.
    Reusing the same inputs does not consume a version number.
    """
    texts = read_sources(root)
    header = single(HEADER_VERSION, texts[HEADER], "firmware version")
    pubspec = single(PUBSPEC_VERSION, texts[PUBSPEC], "native app version/build")
    single(APP_VERSION, texts[APP_IDENTITY], "app release identity")
    series = release_series(version)
    current = (source_identity(texts)
               if any(header[2].startswith(known) for known in SUPPORTED_SERIES) else None)
    if current is not None and current["version"] == version and version in RESERVED_BETA_VERSIONS:
        # Recheck a reserved, unused version against later publications.
        _unpublished_beta_expectation(version, releases)
    else:
        has_beta = any(r.get("draft") is False and r.get("tag_name", "").startswith("fw-beta-")
                       for r in release_records(releases))
        floor = None if has_beta or not isinstance(manifest, dict) else manifest.get("version")
        check_next_beta(version, releases, floor)
    check_public_baseline(releases, manifest, recovered_receipt, recovered_sha256)
    if recovered_receipt is not None:
        receipt = json.loads(recovered_receipt.read_bytes())
        if canonical_version(receipt.get("version")) == canonical_version(version):
            raise ValueError("version already used by the recovered publication")
    if (current is not None
            and tuple(map(int, series.split("."))) < tuple(map(int, current["native_version"].split(".")))):
        raise ValueError("release series cannot go backwards from coherent current source")
    current_code = int(pubspec[3])
    if type(package_code) is not int or type(previous_package_code) is not int:
        raise ValueError("native package codes must be integers")
    if (not valid_package_code(package_code) or previous_package_code < 0
            or package_code <= previous_package_code
            or package_code < current_code
            or (canonical_version(header[2]) != canonical_version(version) and package_code == current_code)):
        raise ValueError("native package code must be 1..9999 and advance past prior uploaded packages; never derive it from N")
    updates = dict(texts)
    updates[HEADER] = HEADER_VERSION.sub(lambda m: m[1] + version + m[3], texts[HEADER])
    updates[PUBSPEC] = PUBSPEC_VERSION.sub(
        lambda m: f"{m[1]}{series}+{package_code}{m[4]}", texts[PUBSPEC])
    updates[APP_IDENTITY] = APP_VERSION.sub(
        lambda m: (f"{m[1]}  version: '{version}',\n  nativeVersion: '{series}',\n"
                   f"  buildNumber: '{package_code}',\n{m[5]}"), texts[APP_IDENTITY])
    identity = source_identity(updates)
    # Validate everything before writing; admission rejects an interrupted partial update.
    for name, data in updates.items():
        if data != texts[name]:
            (root / name).write_text(data)
    return identity


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("check", "prepare"):
        command = commands.add_parser(name)
        command.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    prepare_parser = commands.choices["prepare"]
    prepare_parser.add_argument("--version", required=True)
    prepare_parser.add_argument("--package-code", type=int, required=True)
    prepare_parser.add_argument("--previous-package-code", type=int, required=True,
                                help="verified maximum code previously uploaded/distributed on either app platform")
    prepare_parser.add_argument("--public-beta-manifest", type=Path, required=True)
    prepare_parser.add_argument("--recovered-publication-receipt", type=Path)
    prepare_parser.add_argument("--recovered-receipt-sha256",
                                help="independently retained digest of the exact recovered manual receipt")
    next_parser = commands.add_parser("check-next-beta")
    next_parser.add_argument("--version", required=True)
    next_parser.add_argument("--public-beta-manifest", type=Path,
                             help="fresh live catalog; required before this repository has a published Beta")
    for command in (prepare_parser, next_parser):
        command.add_argument("--releases", type=Path, required=True,
                             help="fresh complete GitHub releases JSON, including all pages")
    args = parser.parse_args()
    try:
        if args.command == "check":
            print(json.dumps(check(args.root), sort_keys=True))
        else:
            releases = json.loads(args.releases.read_text())
            if args.command == "check-next-beta":
                records = release_records(releases)
                has_beta = any(not r["draft"] and r["tag_name"].startswith("fw-beta-") for r in records)
                floor = None
                if args.public_beta_manifest:
                    manifest = json.loads(args.public_beta_manifest.read_text())
                    check_public_baseline(records, manifest)
                    if not has_beta:
                        floor = manifest["version"]
                elif not has_beta:
                    raise ValueError("first beta requires the deployed --public-beta-manifest")
                check_next_beta(args.version, releases, floor)
            else:
                manifest = json.loads(args.public_beta_manifest.read_text())
                if bool(args.recovered_publication_receipt) != bool(args.recovered_receipt_sha256):
                    raise ValueError("recovered receipt and independently retained digest are required together")
                if args.recovered_publication_receipt and args.public_beta_manifest.read_bytes() != artifact.render_manifest_json(manifest).encode():
                    raise ValueError("public beta manifest bytes differ from the recovered canonical catalog")
                print(json.dumps(prepare(args.root, args.version, args.package_code,
                                         args.previous_package_code, releases, manifest,
                                         args.recovered_publication_receipt, args.recovered_receipt_sha256), sort_keys=True))
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"release identity rejected: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
