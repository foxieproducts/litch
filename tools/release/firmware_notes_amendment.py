#!/usr/bin/env python3
"""Amend the initial public notes without replacing any frozen release asset."""

from __future__ import annotations

import json
import os
from pathlib import Path
import time

import official_candidate as candidate
from firmware_publication import (
    CATALOG_READBACK_DELAYS_SECONDS, PublicationError, check_previous, public_read,
)


CUSTOMER_BODY = (
    "Initial release\n\n"
    "Play Classic or Duck Chess with lighted move guidance. Play with a friend or "
    "against 11 computer levels. Use the Foxie Litch app to review saved games, "
    "adjust board settings, and create light animations with Litchcraft.\n"
).encode()
SOURCE_GUIDE = (
    "https://github.com/foxieproducts/litch/blob/main/docs/release/ci_foundation.md"
    "#initial-public-firmware-release"
)
RELEASE_BODY = CUSTOMER_BODY + f"\n[Source and licenses]({SOURCE_GUIDE})\n".encode()


def amended_body(release: dict) -> bool:
    return (release.get("body") or "").encode() == RELEASE_BODY


class Amendment:
    """One explicitly reviewed correction, derived from the original final receipt.

    This is deliberately not a same-version promotion escape hatch. Admission
    fixes the source, image, original notes and both original final receipts.
    """

    def __init__(self, promotion):
        self.promotion = promotion
        self.channel = promotion.channel
        self.original = promotion.final
        self.root = promotion.root / "notes-amendment"
        self.root.mkdir()
        proof = promotion.cutover_proof
        if not isinstance(proof, dict):
            raise PublicationError("Notes amendment requires firmware-origin source snapshot evidence")
        final_digest = proof["final_receipts"][self.channel]
        final = candidate.verify(self.original, promotion.source, final_digest)
        self.original_body_sha = proof["customer_body_sha256"]
        self.name = "manifest_beta.json" if self.channel == "beta" else "manifest.json"
        self.alias = "version_beta.md" if self.channel == "beta" else "version.md"
        self.previous = (self.original / "catalog" / self.name).read_bytes()
        self.manifest = json.loads(self.previous)
        if (final.get("build_receipt_sha256") != promotion.digest
                or final.get("version") != "1.0.1-0"):
            raise PublicationError("Notes amendment is restricted to the admitted initial public release")
        self.image = self.manifest["artifact"]["sha256"]
        self.notes = b"Version: 1.0.1-0\n\n" + CUSTOMER_BODY
        names = candidate.artifact.release_names(
            self.channel, "1.0.1-0", self.image, candidate.sha256(self.notes))
        self.notes_name = names["notes_immutable"]
        # Preserve every field other than the notes descriptor, including both
        # firmware digests, its URL, size, channel and compatibility identity.
        self.manifest["notes"] = {
            "url": candidate.PUBLIC_BASE_URL + "/" + self.notes_name,
            "size_bytes": len(self.notes), "sha256": candidate.sha256(self.notes),
        }
        self.catalog = candidate.artifact.render_manifest_json(self.manifest).encode()
        self.catalog_name = self.name[:-5] + "_" + candidate.sha256(self.catalog) + ".json"
        self.assets = {
            self.notes_name: self.notes, self.catalog_name: self.catalog,
        }
        self.receipt = {
            "schema_version": 1, "kind": "initial-release-notes-amendment",
            "repository": candidate.REPOSITORY, "channel": self.channel,
            "version": "1.0.1-0", "source_sha": promotion.source,
            "build_receipt_sha256": promotion.digest,
            "original_final_receipt_sha256": final_digest,
            "previous_manifest_sha256": candidate.sha256(self.previous),
            "manifest_sha256": candidate.sha256(self.catalog),
            "artifact": self.manifest["artifact"],
            "compliance_bundle": final["compliance_bundle"],
            "customer_body_sha256": candidate.sha256(CUSTOMER_BODY),
            "release_body_sha256": candidate.sha256(RELEASE_BODY),
            "source_guide": SOURCE_GUIDE,
            "files": {name: candidate.sha256(data) for name, data in self.assets.items()},
        }
        raw = candidate.artifact.render_manifest_json(self.receipt).encode()
        self.digest = candidate.sha256(raw)
        self.receipt_name = f"notes_amendment_{self.channel}_1.0.1-0_{self.digest}.json"
        self.assets[self.receipt_name] = raw  # Freeze marker is always last.
        for name, data in self.assets.items():
            (self.root / name).write_bytes(data)
        (self.root / "release-body.md").write_bytes(RELEASE_BODY)

    def require_body(self, release: dict, *, finished: bool = False) -> None:
        if amended_body(release):
            return
        if not finished and candidate.sha256((release.get("body") or "").encode()) == self.original_body_sha:
            return
        raise PublicationError("Release description differs from the approved original or amended notes")

    def verify_archive(self, release: dict, destination: str) -> None:
        for name, data in self.assets.items():
            path = self.promotion.download(release, name, self.promotion.root / destination)
            if path.read_bytes() != data:
                raise PublicationError("Notes amendment archive differs: " + name)

    def verify_delivery(self, *, active: bool, read=public_read) -> None:
        # Old immutable notes, the image and corresponding source remain served.
        original_manifest = json.loads(self.previous)
        original_names = {Path(original_manifest[key]["url"]).name for key in ("artifact", "notes")}
        original_names.add(self.receipt["compliance_bundle"]["filename"])
        if active:
            original_names.add("firmware_beta.bin" if self.channel == "beta" else "firmware.bin")
        for name in sorted(original_names):
            data = (self.original / "assets" / name).read_bytes()
            if read(name, len(data)) != data:
                raise PublicationError("Original firmware/source/notes delivery changed: " + name)
        if read(self.notes_name, len(self.notes)) != self.notes:
            raise PublicationError("Notes amendment public readback mismatch")
        if active:
            for name, data in ((self.alias, self.notes), (self.name, self.catalog)):
                if read(name, len(data)) != data:
                    raise PublicationError("Active notes amendment delivery mismatch: " + name)

    def publish(self, gh, *, read=public_read, connect=None) -> None:
        if os.environ.get("NOTES_SHA256") != candidate.sha256(CUSTOMER_BODY):
            raise PublicationError("Supply the approved initial-release customer body SHA-256")
        previous_sha = os.environ.get("PREVIOUS_MANIFEST_SHA", "")
        if not candidate.artifact.SHA256_PATTERN.fullmatch(previous_sha):
            raise PublicationError("Supply the approved current channel manifest SHA-256")
        live = read(self.name, 32768)
        if live != self.catalog:
            check_previous(live, previous_sha)
            if live != self.previous:
                raise PublicationError("The initial release is no longer the active channel")
        release = self.promotion.resolve(self.promotion.releases())
        self.require_body(release)
        # The old public assets are checked before any new archive/public writes.
        self.promotion.verify_immutable_delivery()
        for name, data in self.assets.items():
            record = next((a for a in release["assets"] if a["name"] == name), None)
            if record and record.get("digest") != "sha256:" + candidate.sha256(data):
                raise PublicationError("Existing amendment asset differs; refusing overwrite: " + name)
            if not record:
                gh("release", "upload", release["tag_name"], str(self.root / name),
                   "--repo", candidate.REPOSITORY)
            refreshed = self.promotion.resolve(self.promotion.releases())
            self.require_body(refreshed)
            downloaded = self.promotion.download(
                refreshed, name, self.promotion.root / "amendment-archive-readback")
            if downloaded.read_bytes() != data:
                raise PublicationError("Notes amendment archive readback mismatch: " + name)
        release = self.promotion.resolve(self.promotion.releases())
        self.require_body(release)
        if connect is None:
            from publish_aws import S3FirmwareHost
            connect = S3FirmwareHost
        host = connect()
        locked = False
        try:
            host.lock()
            locked = True
            live = read(self.name, 32768)
            if live != self.catalog:
                check_previous(live, previous_sha)
                if live != self.previous:
                    raise PublicationError("Channel advanced; refusing notes amendment")
            other = "manifest.json" if self.channel == "beta" else "manifest_beta.json"
            counterpart = json.loads(read(other, 32768))
            if (counterpart.get("version") != "1.0.1-0"
                    or counterpart.get("artifact", {}).get("sha256") != self.image):
                raise PublicationError("Both channels must still deliver the admitted 1.0.1-0 image")
            host.create(self.notes_name, self.notes)  # Never overwrite, even on retries.
            if read(self.notes_name, len(self.notes)) != self.notes:
                raise PublicationError("Amendment immutable readback failed; catalog withheld")
            if live != self.catalog:
                host.replace(self.alias, self.notes)
                if read(self.alias, len(self.notes)) != self.notes:
                    raise PublicationError("Amended compatibility notes readback failed; catalog withheld")
                check_previous(read(self.name, 32768), previous_sha)
                host.replace(self.name, self.catalog)
                for delay in CATALOG_READBACK_DELAYS_SECONDS:
                    if delay:
                        time.sleep(delay)
                    delivered = read(self.name, 32768)
                    if delivered == self.catalog:
                        break
                    if delivered != self.previous:
                        raise PublicationError("Unexpected catalog after notes amendment; inspect before retry")
                else:
                    raise PublicationError("Notes catalog readback lagged; retry the exact amendment")
            self.verify_delivery(active=True, read=read)
            release = self.promotion.resolve(self.promotion.releases())
            self.require_body(release)
            if not amended_body(release):
                # Only the description changes: no target, tag, title or frozen
                # asset is replaced. The coordinator freezes editorial writes
                # during this correction; GitHub supplies no established body
                # compare-and-swap here. The exact original body was rechecked
                # immediately above, and the resulting body is checked below.
                payload = self.root / "release-edit.json"
                payload.write_text(json.dumps({"body": RELEASE_BODY.decode()}))
                print("Replacing verified release description SHA-256 "
                      + candidate.sha256(release["body"].encode())
                      + " with " + candidate.sha256(RELEASE_BODY))
                gh("api", "--method", "PATCH", f"repos/{candidate.REPOSITORY}/releases/{release['id']}",
                   "--input", str(payload))
            release = self.promotion.resolve(self.promotion.releases())
            self.require_body(release, finished=True)
            self.verify_archive(release, "amendment-final-readback")
            self.verify_delivery(active=True, read=read)
        finally:
            try:
                if locked:
                    host.unlock()
            finally:
                host.close()
