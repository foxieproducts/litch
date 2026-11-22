#!/usr/bin/env python3
"""Publish an approved candidate to the S3 bucket behind the app's HTTPS origin."""

from __future__ import annotations

import argparse
import base64
from collections.abc import Callable
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from firmware_publication import IO_TIMEOUT, TRANSFER_TIMEOUT, MAX_CATALOG_BYTES, PublicationError, publish


IMMUTABLE_NAME = re.compile(r"_[0-9a-f]{64}\.(?:[A-Za-z0-9]+|tar\.gz)$")
CONTENT_TYPES = {
    ".bin": "application/octet-stream",
    ".gz": "application/gzip",
    ".json": "application/json",
    ".md": "text/markdown; charset=utf-8",
    ".txt": "text/plain; charset=utf-8",
}


class S3Error(PublicationError):
    def __init__(self, operation: str, code: str | None):
        super().__init__(f"S3 {operation} failed ({code or 'no S3 error code'})")
        self.code = code


class LockUnavailable(PublicationError):
    pass


def _unique_version(version: object) -> bool:
    return (isinstance(version, str) and bool(version)
            and version.strip() == version and version != "null")


class S3FirmwareHost:
    """Firmware publication storage backed by the AWS CLI with workflow OIDC credentials.

    S3 replaces an object atomically and rejects a body whose SHA-256 differs
    from the one sent with it, so no temporary name or rename is needed.
    """

    def __init__(self, run: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run):
        self.bucket = os.environ["FIRMWARE_S3_BUCKET"]
        self.region = os.environ["FIRMWARE_AWS_REGION"]
        self.prefix = os.environ["FIRMWARE_S3_PREFIX"]
        self.lock_key = os.environ["FIRMWARE_S3_LOCK_KEY"]
        if (not self.bucket or not self.region or not self.lock_key
                or not self.prefix.endswith("/") or self.lock_key.startswith(self.prefix)):
            raise PublicationError("configure the firmware bucket, region, asset prefix and separate lock key")
        self.run = run
        self.lock_version: str | None = None

    def _s3api(self, operation: str, *args: str, retry: bool = False) -> dict:
        env = dict(os.environ)
        if not retry:
            # A retried lock or unlock after a lost reply could act on a successor's lock.
            env["AWS_MAX_ATTEMPTS"] = "1"
        command = ["aws", "s3api", operation, "--bucket", self.bucket, *args, "--region", self.region,
                   "--cli-connect-timeout", str(IO_TIMEOUT), "--cli-read-timeout", str(IO_TIMEOUT),
                   "--output", "json"]
        try:
            result = self.run(command, capture_output=True, encoding="utf-8", errors="replace",
                              timeout=TRANSFER_TIMEOUT, env=env, check=False)
        except FileNotFoundError:
            raise PublicationError("AWS CLI not found; install it before publishing") from None
        except subprocess.TimeoutExpired:
            raise PublicationError(f"S3 {operation} exceeded its deadline; inspect the publication state") from None
        if result.returncode != 0:
            # Report only the S3 error code, never the raw CLI output.
            code = re.search(r"An error occurred \(([A-Za-z0-9]+)\)", result.stderr or "")
            raise S3Error(operation, code.group(1) if code else None)
        try:
            return json.loads(result.stdout or "{}")
        except ValueError:
            return {}

    def _put(self, key: str, data: bytes, *args: str, retry: bool = False) -> dict:
        digest = base64.b64encode(hashlib.sha256(data).digest()).decode("ascii")
        with tempfile.TemporaryDirectory(prefix="litch-s3-") as temporary:
            body = Path(temporary) / "body"
            body.write_bytes(data)
            return self._s3api("put-object", "--key", key, "--body", str(body),
                               "--checksum-sha256", digest, *args, retry=retry)

    def lock(self) -> None:
        holder = (f"Litch firmware publication lock\n"
                  f"run: {os.environ.get('GITHUB_RUN_ID', 'local')} "
                  f"attempt {os.environ.get('GITHUB_RUN_ATTEMPT', '-')}\n").encode()
        try:
            created = self._put(self.lock_key, holder, "--if-none-match", "*")
        except S3Error as exc:
            if exc.code == "PreconditionFailed":
                raise LockUnavailable(f"publication lock s3://{self.bucket}/{self.lock_key} unavailable; "
                                      "inspect any previous interrupted run") from None
            raise PublicationError(f"publication lock state unknown ({exc.code or 'no S3 error code'}); "
                                   f"inspect s3://{self.bucket}/{self.lock_key} before another run") from None
        version = created.get("VersionId") if isinstance(created, dict) else None
        if not _unique_version(version):
            # The PUT may have succeeded, but key-only cleanup could erase a successor.
            raise PublicationError(f"no unique lock version; inspect s3://{self.bucket}/{self.lock_key} "
                                   "and enabled bucket versioning before another run")
        # Unlocking this exact version can never remove a lock another run takes later.
        self.lock_version = version

    def unlock(self) -> None:
        if not _unique_version(self.lock_version):
            raise PublicationError(f"no unique lock version; inspect s3://{self.bucket}/{self.lock_key} "
                                   "before any lock cleanup")
        try:
            self._s3api("delete-object", "--key", self.lock_key, "--version-id", self.lock_version)
        except PublicationError:
            raise PublicationError("could not release publication lock; inspect it before another run") from None
        self.lock_version = None

    def create(self, name: str, data: bytes) -> bool:
        """Create without overwriting; only an acknowledged version proves ownership."""
        key = self._key(name)
        try:
            created = self._put(key, data, *self._object_headers(name), "--if-none-match", "*")
        except S3Error as exc:
            if exc.code == "PreconditionFailed":
                return False
            raise PublicationError(f"object creation unverified ({exc.code or 'no S3 error code'}); "
                                   "inspect the requested object before retry or cleanup") from None
        version = created.get("VersionId") if isinstance(created, dict) else None
        if not _unique_version(version):
            raise PublicationError("no unique object version; inspect the requested object "
                                   "before any fixture cleanup")
        return True

    def catalog_history(self, channel: str):
        """Read only versioned copies of the selected known publication catalog."""
        from firmware_provenance import catalog_name

        key = self._key(catalog_name(channel))
        markers = []
        for _ in range(8):
            page = self._s3api("list-object-versions", "--prefix", key,
                               "--max-keys", "32", "--no-paginate", *markers)
            if (not isinstance(page, dict) or type(page.get("IsTruncated")) is not bool
                    or not isinstance(page.get("Versions", []), list)):
                raise PublicationError("Historical catalog listing is incomplete or malformed")
            for record in page.get("Versions", []):
                if not isinstance(record, dict):
                    raise PublicationError("Historical catalog version is malformed")
                if record.get("Key") != key:
                    continue
                version = record.get("VersionId")
                size = record.get("Size")
                if not _unique_version(version) or type(size) is not int or not 0 < size <= MAX_CATALOG_BYTES:
                    raise PublicationError("Historical catalog exceeds admission bounds")
                with tempfile.TemporaryDirectory(prefix="litch-catalog-") as temporary:
                    path = Path(temporary) / "catalog.json"
                    self._s3api("get-object", "--key", key, "--version-id", version,
                                "--range", f"bytes=0-{MAX_CATALOG_BYTES}", str(path))
                    raw = path.read_bytes()
                    if len(raw) != size:
                        raise PublicationError("Historical catalog readback size differs")
                yield raw
            if not page.get("IsTruncated"):
                return
            next_key = page.get("NextKeyMarker")
            next_version = page.get("NextVersionIdMarker")
            if isinstance(next_key, str) and next_key.startswith(key) and next_key > key:
                return  # Sorted listing has exhausted every version of the exact catalog key.
            if next_key != key or not _unique_version(next_version):
                raise PublicationError("Historical catalog pagination is invalid")
            markers = ["--key-marker", key, "--version-id-marker", next_version]
        raise PublicationError("Historical catalog exceeds the bounded inspection window")

    def replace(self, name: str, data: bytes) -> None:
        self._put(self._key(name), data, *self._object_headers(name), retry=True)

    @staticmethod
    def _object_headers(name: str) -> tuple[str, ...]:
        content_type = CONTENT_TYPES.get(Path(name).suffix, "application/octet-stream")
        cache = "public, max-age=31536000, immutable" if IMMUTABLE_NAME.search(name) else "no-cache"
        return ("--content-type", content_type, "--cache-control", cache)

    def close(self) -> None:
        pass  # Each operation is a self-contained CLI call; nothing stays open.

    def _key(self, name: str) -> str:
        # No "+": S3 reads an unencoded "+" in a URL path as a space.
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name):
            raise PublicationError("unsafe publication filename")
        return self.prefix + name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--receipt-sha256", required=True)
    parser.add_argument("--previous-manifest-sha256", required=True)
    args = parser.parse_args()
    try:
        publish(args.candidate, args.source_sha, args.receipt_sha256,
                args.previous_manifest_sha256, S3FirmwareHost)
    except (ValueError, OSError, KeyError, TypeError) as exc:
        detail = str(exc) if isinstance(exc, PublicationError) else "candidate/config validation failed"
        print(f"Publication not confirmed: {detail}", file=sys.stderr)
        return 2
    print("Published exact candidate assets and catalog verified through the app HTTPS origin.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
