#!/usr/bin/env python3
"""Resume the GitHub portion of exact-byte firmware promotion; never build or test."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from time import sleep
from urllib.parse import quote

import official_candidate as candidate
from firmware_publication import PublicationError, check_previous, public_read


# These two release records retain the original build target. Only their Git
# tags move to the parentless public source snapshot.
CUTOVER_SOURCE = "4d2a261e64ed8822020dea260617cff94fbab016"
CUTOVER_ORIGINAL_ROOT = "ba554604bfccdb766af09143d8ade4bb5e327cb3"
CUTOVER_RECEIPT = "8f08a4f37f0d0cdd7f8edbb28c8219175427ad15c0ce8be7ccfa4dbb51f5423b"
CUTOVER_RELEASE_IDS = {"fw-beta-1.0.1-0": 452329516, "fw-stable-1.0.1-0": 452393690}
CUTOVER_BODY_SHA256 = "9730f3a2558e486798bcfef1ec57d55957896b049df135eb87f7b560970c66d9"
CUTOVER_FINAL_RECEIPTS = {
    "beta": "739b14c1e010c30e249dfc8b71d3078addbadd2bcf52478458c971fdb07299ab",
    "stable": "da46de49a5830a0b16dd4212eb0a19af6eef02d8799307e3d12baaa790fe6d26",
}


def public_cutover_root() -> str | None:
    if (os.environ.get("OPERATION") not in ("verify", "verify-archive")
            or os.environ["EXPECTED_SHA"] != CUTOVER_SOURCE
            or os.environ["RECEIPT_SHA"] != CUTOVER_RECEIPT
            or os.environ["CANDIDATE_RUN_ID"] != "44684256095"
            or os.environ["CANDIDATE_RUN_ATTEMPT"] != "1"):
        return None
    if candidate.git(Path.cwd(), "rev-parse", "--is-shallow-repository").strip() != b"false":
        raise PublicationError("Public cutover verification requires a complete checkout")
    root = candidate.git(Path.cwd(), "rev-list", "--max-parents=0", "HEAD").decode().strip()
    if not candidate.SOURCE_PATTERN.fullmatch(root):
        raise PublicationError("Cannot establish a unique public source root")
    return None if root == CUTOVER_ORIGINAL_ROOT else root


def gh(*args: str) -> str:
    result = subprocess.run(["gh", *args], capture_output=True, text=True, timeout=120)
    if result.returncode:
        # gh diagnostics do not contain credentials; keep the useful HTTP status.
        raise PublicationError(result.stderr.strip() or "GitHub request failed")
    return result.stdout


def api(path: str) -> dict:
    return json.loads(gh("api", f"repos/{candidate.REPOSITORY}/{path}"))


def require_tag_source(tag: str, source: str) -> None:
    ref = api("git/ref/tags/" + tag)["object"]
    # Annotated tags are supported, with a bounded dereference chain.
    for _ in range(4):
        if ref["type"] == "commit":
            break
        if ref["type"] != "tag":
            raise PublicationError("Release tag does not resolve to a commit")
        ref = api("git/tags/" + ref["sha"])["object"]
    if ref["type"] != "commit" or ref["sha"] != source:
        raise PublicationError("Release tag does not point to the approved source")


def summary(message: str) -> None:
    print(message)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as output:
            output.write(message + "\n\n")


def preflight(source: str, branch: str) -> None:
    """GITHUB_TOKEN cannot modify releases whose target changes workflows."""
    current = api("branches/" + quote(branch, safe=""))["commit"]["sha"]
    tree = current
    for name in (".github", "workflows"):
        entries = api("git/trees/" + tree)
        if entries.get("truncated"):
            raise PublicationError("Cannot establish the default branch workflow tree")
        matches = [item for item in entries["tree"]
                   if item["path"] == name and item["type"] == "tree"]
        if len(matches) != 1:
            raise PublicationError("Cannot establish the default branch workflow tree")
        tree = matches[0]["sha"]
    expected = candidate.git(Path.cwd(), "rev-parse", source + ":.github/workflows").decode().strip()
    if tree != expected:
        raise PublicationError(
            "Owner action required before promotion: source/default workflow trees differ. "
            "GITHUB_TOKEN cannot create or update this release (Workflows write is unavailable). "
            "Keep the retained bytes and coordinate recovery "
            "of the exact release record with the owner; do not rebuild, retag, or add credentials. "
            "An already-complete release can still use operation=verify.")


def admit_build() -> None:
    """Accept only this repository's successful manual candidate job on main."""
    run_id = os.environ["CANDIDATE_RUN_ID"]
    attempt = os.environ["CANDIDATE_RUN_ATTEMPT"]
    source = os.environ["EXPECTED_SHA"]
    digest = os.environ["RECEIPT_SHA"]
    if (not re.fullmatch(r"[1-9][0-9]*", run_id) or not re.fullmatch(r"[1-9][0-9]*", attempt)
            or not candidate.SOURCE_PATTERN.fullmatch(source)
            or not candidate.artifact.SHA256_PATTERN.fullmatch(digest)):
        raise PublicationError("A full source SHA, receipt SHA-256, run ID and attempt are required")
    run = api(f"actions/runs/{run_id}/attempts/{attempt}")
    path = ".github/workflows/firmware_candidate.yml"
    expected = {"event": "workflow_dispatch",
                "status": "completed", "conclusion": "success", "head_sha": source,
                "head_branch": "main", "run_attempt": int(attempt), "id": int(run_id)}
    if (run.get("path") not in {path, path + "@main", path + "@refs/heads/main"}
            or any(run.get(key) != value for key, value in expected.items())
            or run["repository"]["full_name"] != candidate.REPOSITORY
            or run["head_repository"]["full_name"] != candidate.REPOSITORY):
        raise PublicationError("Candidate does not match the approved successful build")
    jobs = api(f"actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100")["jobs"]
    builds = [job for job in jobs if job["name"] == "candidate"]
    if len(builds) != 1 or any(builds[0].get(key) != value for key, value in {
            "status": "completed", "conclusion": "success", "head_sha": source,
            "run_id": int(run_id)}.items()):
        raise PublicationError("Original attempt did not complete the candidate job")


class Promotion:
    def __init__(self) -> None:
        self.root = Path(os.environ["RUNNER_TEMP"])
        self.original = self.root / "firmware-candidate"
        self.final = self.root / "firmware-final"
        self.source = os.environ["EXPECTED_SHA"]
        self.digest = os.environ["RECEIPT_SHA"]
        self.channel = os.environ["RELEASE_CHANNEL"]
        self.branch = os.environ["DEFAULT_BRANCH"]
        self.cutover_root = public_cutover_root()
        read_only = os.environ.get("OPERATION") in ("verify", "verify-archive")
        if self.cutover_root:
            _, _, self.build = self.archived_build_receipt()
        else:
            if read_only and not self.original.exists():
                self.restore_archived_build()
            self.build = candidate.verify(self.original, self.source, self.digest)
        execution = self.build.get("build_execution")
        if execution is not None and execution != {
                "run_id": os.environ["CANDIDATE_RUN_ID"],
                "run_attempt": os.environ["CANDIDATE_RUN_ATTEMPT"]}:
            raise PublicationError("Supplied run and attempt do not match the approved build execution")
        if read_only and execution is None:
            summary("Historical receipt binds source and approved build digest, but does not bind "
                    "an exact GitHub run/attempt. Successful-run admission remains required.")
        self.draft_tag = (f"candidate-{self.channel}-{os.environ['CANDIDATE_RUN_ID']}-"
                          f"{os.environ['CANDIDATE_RUN_ATTEMPT']}")
        self.final_tag = f"fw-{self.channel}-{self.build['version']}"
        self.notes = self.root / "reviewed-notes.md"
        self.context = self.root / "notes-context.json"

    def archived_build_receipt(self) -> tuple[dict, bytes, dict]:
        """Read the authentic build receipt from the selected published channel."""
        if (not candidate.SOURCE_PATTERN.fullmatch(self.source)
                or not candidate.artifact.SHA256_PATTERN.fullmatch(self.digest)
                or self.channel not in ("beta", "stable")):
            raise PublicationError("A full source SHA, build receipt SHA-256 and channel are required")
        releases = self.releases()
        matches = [release for release in releases
                   if release.get("draft") is False
                   and release.get("target_commitish") == self.source
                   and release.get("tag_name", "").startswith(f"fw-{self.channel}-")
                   and any(asset.get("name") == "build-candidate.json"
                           and asset.get("digest") == "sha256:" + self.digest
                           for asset in release.get("assets", []))]
        if len(matches) != 1:
            raise PublicationError("Require one public channel release bound to the approved build receipt")
        release = matches[0]
        inputs = self.root / "archived-build-inputs"
        build_path = self.download(release, "build-candidate.json", inputs)
        raw = build_path.read_bytes()
        if len(raw) > 32768 or candidate.sha256(raw) != self.digest:
            raise PublicationError("Archived build receipt does not match the approved original")
        build = json.loads(raw)
        if (not isinstance(build, dict) or build.get("source_sha") != self.source
                or build.get("repository") != candidate.REPOSITORY
                or build.get("target") != "esp32s3" or build.get("notes_status") != "draft"
                or build.get("channel") not in ("beta", "stable")):
            raise PublicationError("Archived build receipt has the wrong identity or status")
        if release["tag_name"] != f"fw-{self.channel}-{build['version']}":
            raise PublicationError("Archived release tag does not match the approved build version")
        return release, raw, build

    def archived_build_payloads(self, release: dict, build: dict) -> tuple[bytes, Path | None]:
        inputs = self.root / "archived-build-inputs"
        alias = "firmware_beta.bin" if build["channel"] == "beta" else "firmware.bin"
        image_sha = build["files"]["assets/" + alias]
        name = candidate.artifact.release_names(
            self.channel, build["version"], image_sha, "0" * 64)["firmware_immutable"]
        firmware = self.download(release, name, inputs).read_bytes()
        if candidate.sha256(firmware) != image_sha:
            raise PublicationError("Archived firmware differs from the approved original build")
        compliance = build.get("compliance_bundle")
        bundle = None
        if compliance is not None:
            if (not isinstance(compliance, dict)
                    or not re.fullmatch(r"firmware_compliance_[0-9a-f]{64}\.tar\.gz",
                                        compliance.get("filename", ""))):
                raise PublicationError("Invalid archived compliance filename")
            bundle = self.download(release, compliance["filename"], inputs)
        return firmware, bundle

    def restore_archived_build(self) -> None:
        release, raw, build = self.archived_build_receipt()
        firmware, bundle = self.archived_build_payloads(release, build)
        notes = candidate.draft_notes(Path.cwd(), self.source, build["notes_base_sha"])
        self.original.mkdir()
        candidate.write_payloads(self.original, firmware, notes, build["channel"],
                                 build["version"], build.get("compliance_bundle"), bundle)
        (self.original / "candidate.json").write_bytes(raw)

    def verify_cutover_release(self, release: dict) -> str:
        """Verify delivered bytes and original metadata without recovering internal draft text."""
        digest = CUTOVER_FINAL_RECEIPTS[self.channel]
        receipt = self.download(release, "candidate.json", self.final).read_bytes()
        if candidate.sha256(receipt) != digest:
            raise PublicationError("Cutover release differs from its approved final receipt")
        body = release["body"].encode()
        if candidate.sha256(body) != CUTOVER_BODY_SHA256:
            raise PublicationError("Cutover release body differs from the approved customer notes")
        firmware, bundle = self.archived_build_payloads(release, self.build)
        candidate.write_payloads(self.final, firmware, body, self.channel,
                                 self.build["version"], self.build["compliance_bundle"], bundle)
        final = candidate.verify(self.final, self.source, digest)
        # Channel, status and published files may change. All build inputs remain bound.
        expected = dict(self.build, channel=self.channel, notes_status="reviewed",
                        files=final["files"], build_receipt_sha256=self.digest)
        if final != expected:
            raise PublicationError("Finalized metadata differs from its authenticated original build")
        context = {"schema_version": 1, "source_sha": self.source, "channel": self.channel,
                   "build_receipt_sha256": self.digest, "version": self.build["version"],
                   "notes_base_sha": self.build["notes_base_sha"]}
        saved = self.download(release, "notes-context.json", self.root)
        if saved.read_bytes() != candidate.artifact.render_manifest_json(context).encode():
            raise PublicationError("Archived notes context differs from its original build and channel")
        self.compare_assets(release)
        summary("The original build receipt records the unshipped draft digest. Its text was not recovered.")
        return digest

    def releases(self) -> list:
        pages = json.loads(gh("api", "--paginate", "--slurp",
                              f"repos/{candidate.REPOSITORY}/releases?per_page=100"))
        return candidate.release_identity.release_records(pages)

    def resolve(self, releases: list) -> dict | None:
        matches = [release for release in releases
                   if release["tag_name"] in (self.final_tag, self.draft_tag)]
        if len(matches) > 1:
            raise PublicationError("Both candidate and final release records exist; reconcile before retry")
        if not matches:
            return None
        release = matches[0]
        if (release["target_commitish"] != self.source
                or (not release["draft"] and release["tag_name"] != self.final_tag)):
            raise PublicationError("Release source/status does not match the retained build")
        if not release["draft"]:
            if release["prerelease"] != (self.channel == "beta"):
                raise PublicationError("Public release has the wrong channel status")
            if self.cutover_root and release["id"] != CUTOVER_RELEASE_IDS[self.final_tag]:
                raise PublicationError("Public cutover release record differs from the original")
            require_tag_source(self.final_tag, self.cutover_root or self.source)
        return release

    def download(self, release: dict, name: str, destination: Path) -> Path:
        destination.mkdir(parents=True, exist_ok=True)
        path = destination / name
        if path.exists() or path.is_symlink():
            raise PublicationError("Readback output already exists; use a fresh run directory")
        record = next((item for item in release["assets"] if item["name"] == name), None)
        if record is None:
            raise PublicationError("GitHub asset is missing from release metadata: " + name)
        expected_size, expected_digest = record["size"], record.get("digest")
        maximum = (candidate.firmware_compliance.MAX_BUNDLE_SIZE
                   if re.fullmatch(r"firmware_compliance_[0-9a-f]{64}\.tar\.gz", name)
                   else candidate.artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE)
        if (type(expected_size) is not int or not 0 < expected_size <= maximum
                or not isinstance(expected_digest, str)
                or not re.fullmatch(r"sha256:[0-9a-f]{64}", expected_digest)):
            raise PublicationError("GitHub asset metadata exceeds its admission bounds: " + name)
        # Keep the original release/name/digest throughout this read. A private
        # directory confines cleanup to partial bytes created by this download.
        with tempfile.TemporaryDirectory(prefix=".readback-", dir=destination) as temporary:
            partial = Path(temporary) / name
            for attempt in range(3):
                try:
                    gh("release", "download", release["tag_name"], "--repo", candidate.REPOSITORY,
                       "--pattern", name, "--dir", temporary)
                    break
                except (PublicationError, subprocess.TimeoutExpired) as error:
                    transient = isinstance(error, subprocess.TimeoutExpired) or re.search(
                        r"\bHTTP (?:408|429|500|502|503|504)\b", str(error))
                    if not transient or attempt == 2:
                        raise
                    partial.unlink(missing_ok=True)
                    summary(f"Retrying transient GitHub asset readback for {name} (attempt {attempt + 2}/3).")
                    sleep(attempt + 1)
            if partial.is_symlink() or partial.stat().st_size != expected_size:
                raise PublicationError("GitHub asset size/readback mismatch: " + name)
            data = partial.read_bytes()
            if len(data) != expected_size or expected_digest != "sha256:" + candidate.sha256(data):
                raise PublicationError("GitHub asset digest/readback mismatch: " + name)
            partial.rename(path)
        return path

    def freeze(self, release: dict, archived: bool, *, read_only: bool = False) -> str:
        if not any(item["name"] == "notes-context.json" for item in release["assets"]):
            raise PublicationError("Draft has no notes context; resume prepare-notes before promotion")
        context = self.download(release, "notes-context.json", self.root)
        self.notes.write_bytes((release["body"] or "").encode("utf-8"))
        if os.environ.get("OPERATION") == "promote":
            approved = os.environ.get("NOTES_SHA256", "")
            if candidate.sha256(self.notes.read_bytes()) != approved:
                raise PublicationError("Draft notes differ from the approved notes SHA-256")
        digest = candidate.finalize_notes(self.original, self.source, self.digest,
                                          self.notes, self.final, context,
                                          historical_read_only=read_only and archived and not release["draft"])
        final = candidate.verify(self.final, self.source, digest)
        if final["channel"] != self.channel:
            raise PublicationError("Reviewed context belongs to a different channel")
        if archived:
            saved = self.download(release, "candidate.json", self.root / "archive")
            if saved.read_bytes() != (self.final / "candidate.json").read_bytes():
                raise PublicationError("Frozen release changed; reconcile archived receipt/notes before retry")
            build = self.download(release, "build-candidate.json", self.root / "archive")
            if build.read_bytes() != (self.original / "candidate.json").read_bytes():
                raise PublicationError("Archived build receipt does not match the approved original")
            # Public records are immutable recovery inputs. Missing/changed assets
            # are a real failure, never an invitation to overwrite a publication.
            if not release["draft"]:
                self.compare_assets(release)
        return digest

    def payloads(self) -> list[Path]:
        return [*(self.final / "assets").iterdir(), *(self.final / "catalog").iterdir()]

    def compare_assets(self, release: dict) -> None:
        names = {item["name"] for item in release["assets"]}
        for path in self.payloads():
            if path.name not in names:
                raise PublicationError("Release archive is incomplete: " + path.name)
            remote = self.download(release, path.name, self.root / "release-readback")
            if remote.read_bytes() != path.read_bytes():
                raise PublicationError("Release archive differs from frozen bytes: " + path.name)

    def delivered(self) -> bool:
        catalog = next((self.final / "catalog").iterdir())
        if public_read(catalog.name, 32768, missing_ok=True) != catalog.read_bytes():
            return False
        for path in self.payloads():
            if public_read(path.name, path.stat().st_size) != path.read_bytes():
                raise PublicationError("Published candidate asset readback mismatch: " + path.name)
        return True

    def verify_immutable_delivery(self) -> None:
        # A later channel publication legitimately replaces the catalog and
        # compatibility aliases. Historical integrity belongs to immutable URLs.
        catalog = next((self.final / "catalog").iterdir())
        manifest = json.loads(catalog.read_bytes())
        names = {Path(manifest[key]["url"]).name for key in ("artifact", "notes")}
        compliance = self.build.get("compliance_bundle")
        if compliance is not None:
            names.add(compliance["filename"])
        for name in sorted(names):
            path = self.final / "assets" / name
            if public_read(name, path.stat().st_size) != path.read_bytes():
                raise PublicationError("Published immutable release asset readback mismatch: " + name)

    def save(self, release: dict, digest: str, complete: bool) -> None:
        state = {"tag": release["tag_name"], "public": not release["draft"],
                 "receipt_sha256": digest, "complete": complete}
        (self.root / "promotion-state.json").write_text(json.dumps(state))
        with open(os.environ["GITHUB_ENV"], "a") as output:
            output.write(f"FINAL_RECEIPT_SHA={digest}\n")
            output.write(f"PROMOTION_COMPLETE={'true' if complete else 'false'}\n")

    def prepare(self, operation: str) -> None:
        read_only = operation in ("verify", "verify-archive")
        if not read_only:
            candidate.require_compliance(self.build)
        releases = self.releases()
        release = self.resolve(releases)
        archived = release and any(item["name"] == "candidate.json" for item in release["assets"])
        if read_only and (not release or release["draft"] or not archived):
            raise PublicationError("Read-only verification requires the exact public release and frozen archive")
        if release and not release["draft"] and not archived:
            raise PublicationError("Public release has no frozen archive; reconcile without rewriting it")
        if archived:
            if self.cutover_root:
                digest = self.verify_cutover_release(release)
            else:
                digest = self.freeze(release, True, read_only=read_only)
            if read_only:
                if operation == "verify":
                    if not self.delivered():
                        raise PublicationError("Read-only verification found incomplete active app delivery; nothing was changed")
                    summary(f"Verified active {self.final_tag}: exact source, original/final receipts, "
                            "notes, GitHub assets, channel catalog, aliases and app HTTPS bytes. "
                            "No writes, builds or tests.")
                    return
                self.verify_immutable_delivery()
                summary(f"Verified archived {self.final_tag}: exact source, original/final receipts, "
                        "notes, GitHub assets and immutable app HTTPS bytes. "
                        "Current channel catalog and compatibility aliases were not checked. "
                        "No writes, builds or tests.")
                return
            complete = self.delivered()
            if complete and not release["draft"]:
                self.save(release, digest, True)
                summary(f"Verified existing {self.final_tag}: exact source, original/final receipts, "
                        "notes, GitHub assets and anonymous app HTTPS bytes. No writes, builds or tests.")
                return
            if operation == "prepare-notes":
                summary("Frozen release retained; notes cannot be regenerated. Resume promote with the original build identity.")
                return
            if release["draft"]:
                preflight(self.source, self.branch)
            # Exclude this final record when reconciling a retry's original
            # baseline. A later channel publication must not be rolled back.
            frozen = json.loads(self.context.read_bytes())["notes_base_sha"]
            prior = [item for item in releases if item["tag_name"] != self.final_tag]
            if candidate.resolve_notes_base(Path.cwd(), prior, self.channel, frozen) != frozen:
                raise PublicationError("Channel baseline advanced beyond this frozen promotion; refusing rollback")
            self.save(release, digest, False)
            summary("Resuming frozen promotion; original notes baseline and immutable bytes retained.")
            return
        if read_only:
            raise PublicationError("No completed publication to verify; nothing was changed")
        preflight(self.source, self.branch)
        other = "stable" if self.channel == "beta" else "beta"
        other_tag = f"fw-{other}-{self.build['version']}"
        counterpart = next((record for record in releases
                            if record["tag_name"] == other_tag and not record["draft"]), None)
        if counterpart:
            original = next((asset for asset in counterpart["assets"]
                             if asset["name"] == "build-candidate.json"), None)
            if not original or original.get("digest") != "sha256:" + self.digest:
                raise PublicationError("Use the original build already released in the other channel")
        # A fresh identity obeys the deployed Beta allocation floor, even if
        # first delivered to Release. A subsequent channel adopts the same build.
        if self.channel == "beta" or not counterpart:
            live = json.loads(public_read("manifest_beta.json", 32768))
            candidate.release_identity.check_public_baseline_for_migration(live)
            beta_history = [record for record in releases if record["tag_name"] != other_tag]
            candidate.release_identity.check_next_beta(self.build["version"], beta_history, live["version"])
        # An existing tag is authoritative even while its release is a draft.
        # Do not discover an unrelated tag target after the app catalog changes.
        if subprocess.run(['git', 'show-ref', '--verify', '--quiet', 'refs/tags/' + self.final_tag],
                          timeout=15, check=False).returncode == 0:
            raise PublicationError('Channel/version tag already exists without a frozen archive; reconcile before publication')
        base = candidate.resolve_notes_base(Path.cwd(), releases, self.channel,
                                             os.environ.get("BOOTSTRAP_BASE", ""))
        if release:
            if not any(item["name"] == "notes-context.json" for item in release["assets"]):
                if operation != "prepare-notes":
                    raise PublicationError("Draft context upload was interrupted; resume prepare-notes")
            else:
                context = self.download(release, "notes-context.json", self.root)
                review = json.loads(context.read_bytes())
                if (review.get("notes_base_sha") != base or review.get("channel") != self.channel
                        or review.get("build_receipt_sha256") != self.digest
                        or review.get("source_sha") != self.source
                        or review.get("version") != self.build["version"]):
                    raise PublicationError("Draft context/baseline changed; review the correct notes range")
                if operation == "prepare-notes":
                    summary("Editable draft: " + release["html_url"] + " — notes SHA-256: "
                            + candidate.sha256((release["body"] or "").encode()))
                    return
                # freeze downloads context once; the validated copy is reused.
                context.unlink()
                digest = self.freeze(release, False)
                self.save(release, digest, False)
                summary("Owner notes frozen; draft archive/readback precedes app publication.")
                return
        if operation != "prepare-notes":
            raise PublicationError("No reviewed draft; run prepare-notes using this original build identity")
        output = self.root / "channel-notes"
        candidate.prepare_notes(Path.cwd(), self.original, self.source, self.digest,
                                self.channel, base, output)
        if not release:
            gh("release", "create", self.draft_tag, "--repo", candidate.REPOSITORY,
               "--draft", "--target", self.source,
               "--title", f"Review firmware {self.build['version']} for {self.channel}",
               "--notes-file", str(output / "notes.md"))
        gh("release", "upload", self.draft_tag, str(output / "notes-context.json"),
           "--repo", candidate.REPOSITORY)
        saved = self.resolve(self.releases())
        summary("Editable draft: " + saved["html_url"] + " — notes SHA-256: "
                + candidate.sha256((saved["body"] or "").encode()))

    def archive(self) -> None:
        candidate.require_compliance(self.build)
        state = json.loads((self.root / "promotion-state.json").read_text())
        candidate.require_compliance(candidate.verify(self.final, self.source, state["receipt_sha256"]))
        if state["complete"] or state["public"]:
            return
        preflight(self.source, self.branch)
        release = self.resolve(self.releases())
        self.require_body(release)
        build = self.root / "build-candidate.json"
        build.write_bytes((self.original / "candidate.json").read_bytes())
        # Receipt is the freeze marker: upload it last, after all payloads and
        # the independently pinned build receipt have been read back exactly.
        paths = [*self.payloads(), build]
        present = {item["name"]: item for item in release["assets"]}
        for path in [*paths, self.final / "candidate.json"]:
            item = present.get(path.name)
            if item and item.get("digest") != "sha256:" + candidate.sha256(path.read_bytes()):
                raise PublicationError("Existing archive asset differs; refusing overwrite: " + path.name)
            if not item:
                gh("release", "upload", release["tag_name"], str(path), "--repo", candidate.REPOSITORY)
            refreshed = self.resolve(self.releases())
            remote = self.download(refreshed, path.name, self.root / "archive-readback")
            if remote.read_bytes() != path.read_bytes():
                raise PublicationError("Draft archive readback mismatch: " + path.name)
        self.require_body(self.resolve(self.releases()))
        summary("Frozen receipt and all assets archived/read back before app publication.")

    def require_body(self, release: dict) -> None:
        if (release["body"] or "").encode("utf-8") != self.notes.read_bytes():
            raise PublicationError("Owner notes changed after freezing; reconcile without overwriting the edit")

    def ready(self) -> None:
        candidate.require_compliance(self.build)
        state = json.loads((self.root / "promotion-state.json").read_text())
        candidate.require_compliance(candidate.verify(self.final, self.source, state["receipt_sha256"]))
        release = self.resolve(self.releases())
        self.require_body(release)
        if release["draft"]:
            preflight(self.source, self.branch)
        name = "manifest_beta.json" if self.channel == "beta" else "manifest.json"
        previous = public_read(name, 32768)
        if previous == (self.final / "catalog" / name).read_bytes():
            return  # The publisher verifies all assets before completing this retry.
        check_previous(previous, os.environ["PREVIOUS_MANIFEST_SHA"])
        catalog = json.loads(previous)
        if (catalog["channel"] != self.channel or catalog["schema_version"] != 1
                or catalog["target"] != "esp32s3" or catalog["chip_id"] != 9):
            raise PublicationError("Unexpected live firmware catalog")
        version = catalog["version"]
        identity = candidate.release_identity
        def order(value: str) -> tuple:
            return (*map(int, identity.release_series(value).split(".")), identity.sequence(value))
        if order(version) >= order(self.build["version"]):
            raise PublicationError("Channel version already deployed or superseded")

    def finish(self) -> None:
        candidate.require_compliance(self.build)
        state = json.loads((self.root / "promotion-state.json").read_text())
        candidate.require_compliance(candidate.verify(self.final, self.source, state["receipt_sha256"]))
        if not self.delivered():
            raise PublicationError("App delivery incomplete; official record was not finalized")
        release = self.resolve(self.releases())
        self.require_body(release)
        if release["draft"]:
            preflight(self.source, self.branch)
            gh("release", "edit", release["tag_name"], "--repo", candidate.REPOSITORY,
               "--tag", self.final_tag, "--target", self.source,
               "--title", f"Firmware {self.build['version']} ({self.channel})",
               "--prerelease=" + ("true" if self.channel == "beta" else "false"),
               "--draft=false")
            release = self.resolve(self.releases())
            self.require_body(release)
        if release["draft"]:
            raise PublicationError("Official record remains a draft; resume the retained promotion")
        summary(f"Completed {self.final_tag}: app delivery and official record verified. "
                f"Final receipt {state['receipt_sha256']}. No builds or tests ran.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("admit-build", "prepare", "archive", "ready", "finish"))
    args = parser.parse_args()
    try:
        if args.command == "admit-build":
            admit_build()
        else:
            promotion = Promotion()
            if args.command == "prepare":
                promotion.prepare(os.environ["OPERATION"])
            else:
                getattr(promotion, args.command)()
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        summary("Promotion not complete: " + str(exc) +
                " Retain the original build identity and frozen promotion; retry after resolving this boundary. "
                "No failed history was changed.")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
