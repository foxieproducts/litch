#!/usr/bin/env python3
"""Recognize images proved by the protected update server's publication catalogs."""

from __future__ import annotations

import argparse
import json

import official_candidate as candidate


MAX_RECORD_BYTES = 1024


def catalog_name(channel: str) -> str:
    if channel not in ("stable", "beta"):
        raise ValueError("Select a published firmware channel")
    return "manifest_beta.json" if channel == "beta" else "manifest.json"


def recognition(digest: str) -> tuple[str, bytes]:
    if not isinstance(digest, str) or not candidate.artifact.SHA256_PATTERN.fullmatch(digest):
        raise ValueError("Invalid admitted image digest")
    record = {"schema_version": 1, "image_sha256": digest, "status": "official"}
    return f"provenance_{digest}.json", candidate.artifact.render_manifest_json(record).encode()


def admit_server_image(manifest: dict, read) -> str:
    """Validate and recompute identity from the server's immutable firmware bytes."""
    from firmware_publication import PublicationError

    if (not isinstance(manifest, dict) or manifest.get("schema_version") != 1
            or manifest.get("target") != "esp32s3" or manifest.get("chip_id") != 9):
        raise PublicationError("Unsupported server publication catalog")
    channel = manifest.get("channel")
    catalog_name(channel)
    version = manifest.get("version")
    if not isinstance(version, str) or not candidate.release_identity.VERSION.fullmatch(version):
        raise PublicationError("Invalid published firmware version")
    descriptor = manifest.get("artifact")
    if not isinstance(descriptor, dict):
        raise PublicationError("Server catalog has no firmware descriptor")
    size = descriptor.get("size_bytes")
    if type(size) is not int or not 0 < size <= candidate.artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE:
        raise PublicationError("Server firmware exceeds its admission bounds")
    name = candidate.artifact.release_names(
        channel, version, descriptor["sha256"], "0" * 64)["firmware_immutable"]
    if descriptor.get("url") != candidate.PUBLIC_BASE_URL + "/" + name:
        raise PublicationError("Server firmware must use its canonical immutable origin URL")
    inspected = candidate.artifact.inspect_firmware_bytes(read(name, size))
    if any(getattr(inspected, key) != descriptor.get(key)
           for key in ("size_bytes", "sha256", "image_sha256")):
        raise PublicationError("Server firmware does not match its published catalog")
    return inspected.image_sha256


def register_channel(channel: str, version: str | None, connect, read) -> str:
    """Select authority only from fixed live catalogs or protected catalog history."""
    from firmware_publication import MAX_CATALOG_BYTES, PublicationError, register_provenance

    name = catalog_name(channel)
    current = read(name, MAX_CATALOG_BYTES)
    manifest = json.loads(current)
    if not isinstance(manifest, dict) or manifest.get("channel") != channel:
        raise PublicationError("Published catalog belongs to a different channel")
    host = connect()
    locked = False
    try:
        try:
            host.lock()
        except PublicationError:
            raise PublicationError("Shared publication lock unavailable or ownership uncertain; inspect the lock before retrying") from None
        locked = True
        historical = version is not None and manifest.get("version") != version
        if historical:
            matches = []
            for raw in host.catalog_history(channel):
                saved = json.loads(raw)
                if isinstance(saved, dict) and saved.get("version") == version:
                    if saved.get("channel") != channel:
                        raise PublicationError("Historical catalog belongs to a different channel")
                    matches.append(saved)
            if not matches:
                raise PublicationError("Historical publication authority unavailable; recognition remains pending")
            manifest = matches[0]
            if any(saved.get("artifact") != manifest.get("artifact") for saved in matches[1:]):
                raise PublicationError("Historical version has conflicting image identities")
        if not historical and read(name, MAX_CATALOG_BYTES) != current:
            raise PublicationError("Published channel changed; retry recognition from the current catalog")
        register_provenance(host, manifest, read)
        return manifest["version"]
    finally:
        try:
            if locked:
                try:
                    host.unlock()
                except PublicationError:
                    raise PublicationError("Shared publication lock release unconfirmed; inspect the lock before retrying") from None
        finally:
            host.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("stable", "beta"), default="stable")
    parser.add_argument("--version", help="published version; omit for the selected current channel")
    args = parser.parse_args()
    from firmware_publication import PublicationError, public_read
    from publish_aws import S3FirmwareHost
    try:
        version = register_channel(args.channel, args.version, S3FirmwareHost, public_read)
    except PublicationError as error:
        print("Recognition pending: " + str(error))
        return 2
    except (ValueError, OSError, KeyError, TypeError):
        # Storage errors can carry digest-derived object keys. Do not emit them.
        print("Recognition pending: server publication evidence or protected access could not be verified.")
        return 2
    print(f"Official recognition verified for {args.channel} firmware {version}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
