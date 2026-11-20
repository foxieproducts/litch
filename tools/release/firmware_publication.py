#!/usr/bin/env python3
"""Admit and publish exact firmware assets with public HTTPS readback."""

from __future__ import annotations

import json
from pathlib import Path
import re
import tempfile
import time
from urllib import error, request

import official_candidate as candidate


IO_TIMEOUT = 15
TRANSFER_TIMEOUT = 60
MAX_CATALOG_BYTES = 32768
CATALOG_READBACK_DELAYS_SECONDS = (0, 1, 2, 4, 8)


class PublicationError(ValueError):
    pass


class NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def public_read(name: str, maximum: int, missing_ok: bool = False) -> bytes | None:
    """Read exactly the unauthenticated, nonredirected origin used by the app."""
    opener = request.build_opener(NoRedirect())
    started = time.monotonic()
    req = request.Request(f"{candidate.PUBLIC_BASE_URL}/{name}", headers={
        "Cache-Control": "no-cache", "Accept-Encoding": "identity",
        "User-Agent": "Litch-release-verifier/1",
    })
    try:
        with opener.open(req, timeout=IO_TIMEOUT) as response:
            if response.status != 200:
                raise PublicationError("public readback requires HTTP 200")
            if response.headers.get("Content-Encoding", "identity") != "identity":
                raise PublicationError("public readback used unexpected content encoding")
            data = bytearray()
            while True:
                if time.monotonic() - started > TRANSFER_TIMEOUT:
                    raise PublicationError("public readback exceeded its deadline")
                block = response.read1(min(65536, maximum + 1 - len(data)))
                if not block:
                    return bytes(data)
                data.extend(block)
                if len(data) > maximum:
                    raise PublicationError("public readback exceeded its size limit")
    except error.HTTPError as exc:
        if exc.code == 404 and missing_ok:
            return None
        raise PublicationError(f"public readback failed (HTTP {exc.code}); redirects are refused") from None
    except (OSError, error.URLError) as exc:
        raise PublicationError("public HTTPS readback failed; catalog delivery is unverified") from None




def check_previous(previous: bytes | None, expected: str) -> None:
    actual = "missing" if previous is None else candidate.sha256(previous)
    if actual != expected:
        raise PublicationError("live catalog changed since approval; inspect and approve its exact digest")


def publish(directory: Path, source_sha: str, receipt_sha: str, previous_sha: str,
            connect, read=public_read) -> None:
    """Keep the old catalog until verified public assets are all available."""
    receipt = candidate.verify(directory, source_sha, receipt_sha)
    compliance = candidate.require_compliance(receipt)
    if receipt["notes_status"] != "reviewed":
        raise PublicationError("release notes are still a draft; review and finalize them before publication")
    if previous_sha != "missing" and not candidate.artifact.SHA256_PATTERN.fullmatch(previous_sha):
        raise PublicationError("previous catalog must be its approved SHA-256 or 'missing'")
    payloads = {path: (directory / path).read_bytes() for path in receipt["files"]}
    if any(candidate.sha256(data) != receipt["files"][path] for path, data in payloads.items()):
        raise PublicationError("candidate changed after admission")
    manifest_path = next(path for path in payloads if path.startswith("catalog/"))
    name = Path(manifest_path).name
    manifest_bytes = payloads[manifest_path]
    manifest = json.loads(manifest_bytes)
    previous = read(name, MAX_CATALOG_BYTES, True)
    if previous == manifest_bytes:
        # Safe retry after a lost final response: success requires all assets too.
        for path, data in payloads.items():
            if path.startswith("assets/") and read(Path(path).name, len(data)) != data:
                raise PublicationError("published candidate asset readback mismatch")
        return
    check_previous(previous, previous_sha)
    if previous is not None:
        old = json.loads(previous)
        if candidate.release_identity.VERSION.fullmatch(old.get("version", "")):
            def order(version):
                series = candidate.release_identity.release_series(version)
                return (*map(int, series.split(".")), candidate.release_identity.sequence(version))
            if order(old["version"]) > order(receipt["version"]):
                raise PublicationError("A newer app catalog exists; refusing rollback")
        if old.get("version") == receipt["version"]:
            raise PublicationError("version already published with different metadata; commit a new version")
    else:
        # First-manifest migration must not reuse a legacy version for new bytes:
        # old boards can verify only the version after reboot.
        suffix = "_beta" if receipt["channel"] == "beta" else ""
        old_notes = read(f"version{suffix}.md", candidate.artifact.MAX_NOTES_SIZE, True)
        if old_notes is not None:
            versions = re.findall(rb"(?m)^Version: ([^\r\n]+)$", old_notes)
            if len(versions) != 1:
                raise PublicationError("legacy version is ambiguous; resolve migration before publication")
            if versions[0].decode("utf-8") == receipt["version"]:
                old_binary = read(
                    f"firmware{suffix}.bin",
                    candidate.artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE,
                )
                if candidate.sha256(old_binary) != manifest["artifact"]["sha256"]:
                    raise PublicationError("legacy version already names different firmware; commit a new version")

    host = connect()  # All local and prior-state checks precede any storage mutation.
    locked = False
    try:
        host.lock()
        locked = True
        check_previous(read(name, MAX_CATALOG_BYTES, True), previous_sha)
        # Both channels share this lock. During migration a deployed version
        # may have no release record in this repository.
        other = "manifest.json" if receipt["channel"] == "beta" else "manifest_beta.json"
        other_catalog = json.loads(read(other, MAX_CATALOG_BYTES))
        if (other_catalog["version"] == receipt["version"]
                and other_catalog["artifact"]["sha256"] != manifest["artifact"]["sha256"]):
            raise PublicationError("This version names different firmware in the other channel")
        with tempfile.TemporaryDirectory(prefix="litch-firmware-publication-") as temporary:
            readback_dir = Path(temporary) / "readback"
            readback_dir.mkdir()
            immutable_names = {Path(manifest[key]["url"]).name for key in ("artifact", "notes")}
            immutable_names.add(compliance["filename"])
            assets = [path for path in payloads if path.startswith("assets/")]
            assets.sort(key=lambda path: (Path(path).name not in immutable_names, path))
            for path in assets:
                asset_name, data = Path(path).name, payloads[path]
                immutable = asset_name in immutable_names
                created = host.create(asset_name, data) if immutable else False
                if not immutable:
                    host.replace(asset_name, data)
                delivered = read(asset_name, len(data))
                if delivered != data:
                    if immutable and not created:
                        raise PublicationError("immutable asset already exists with different bytes")
                    raise PublicationError("public asset readback mismatch; catalog withheld")
                (readback_dir / asset_name).write_bytes(delivered)
            candidate.stage_catalog(directory, source_sha, receipt_sha,
                                    readback_dir, Path(temporary) / "catalog")
            check_previous(read(name, MAX_CATALOG_BYTES, True), previous_sha)
            host.replace(name, manifest_bytes)
            # The public endpoint can briefly lag behind the acknowledged PUT.
            # Poll only while it still serves the exact approved prior state.
            for delay in CATALOG_READBACK_DELAYS_SECONDS:
                if delay:
                    time.sleep(delay)
                delivered = read(name, MAX_CATALOG_BYTES, previous is None)
                if delivered == manifest_bytes:
                    return
                if delivered != previous:
                    break
            raise PublicationError("catalog readback mismatch; publication may have occurred, inspect before retry")
    finally:
        try:
            if locked:
                host.unlock()
        finally:
            host.close()
