#!/usr/bin/env python3
"""Validate and describe publishable Litch ESP32-S3 firmware artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Sequence
from urllib.parse import unquote, urlsplit


ESP_IMAGE_MAGIC = 0xE9
ESP32S3_CHIP_ID = 9
ESP_IMAGE_HEADER_SIZE = 24
ESP_SEGMENT_HEADER_SIZE = 8
ESP_IMAGE_MAX_SEGMENTS = 16
ESP_IMAGE_HASH_SIZE = 32
MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE = 2 * 1024 * 1024
MAX_NOTES_SIZE = 256 * 1024
ESP_CHECKSUM_MAGIC = 0xEF
ESP_APP_DESCRIPTOR_MAGIC = 0xABCD5432
ESP_APP_DESCRIPTOR_SIZE = 256
BOOT_POLICY_MAGIC = b"FXBT"
BOOT_POLICY_SCHEMA = 1
BOOT_POLICY_SIZE = 16
KNOWN_BOOT_POLICY_EPOCHS = frozenset((1, 2))
VERSION_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


class ArtifactValidationError(ValueError):
    """Raised when bytes cannot be admitted as Litch S3 firmware."""


@dataclass(frozen=True)
class FirmwareArtifact:
    size_bytes: int
    sha256: str
    image_sha256: str
    chip_id: int
    segment_count: int


class FirmwareBootPolicyStatus(str, Enum):
    UNAVAILABLE = "unavailable"
    ABSENT = "absent"
    KNOWN = "known"
    UNSUPPORTED = "unsupported"
    MALFORMED = "malformed"


@dataclass(frozen=True)
class FirmwareBootPolicy:
    status: FirmwareBootPolicyStatus
    schema: int = 0
    size: int = 0
    epoch: int = 0


@dataclass(frozen=True)
class FirmwareInspection:
    metadata: FirmwareArtifact
    boot_policy: FirmwareBootPolicy
    first_segment_load_address: int
    first_segment_size_bytes: int


@dataclass(frozen=True)
class ReleaseNotesArtifact:
    size_bytes: int
    sha256: str


def _fail(message: str) -> None:
    raise ArtifactValidationError(message)


def _check_firmware_size(size: int, maximum: int) -> None:
    if maximum <= 0:
        raise ValueError("firmware inspection size limit must be positive")
    if size > maximum:
        limit = (
            "2 MiB official publication limit"
            if maximum == MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE
            else f"{maximum // 1024} KiB admission limit"
        )
        _fail(f"ESP32-S3 firmware exceeds the {limit}")


def _inspect_boot_policy(first_segment: memoryview) -> FirmwareBootPolicy:
    if (len(first_segment) < ESP_APP_DESCRIPTOR_SIZE or
            struct.unpack_from("<I", first_segment)[0] != ESP_APP_DESCRIPTOR_MAGIC):
        return FirmwareBootPolicy(FirmwareBootPolicyStatus.UNAVAILABLE)
    descriptor = first_segment[ESP_APP_DESCRIPTOR_SIZE:]
    if descriptor[:4] != BOOT_POLICY_MAGIC:
        return FirmwareBootPolicy(FirmwareBootPolicyStatus.ABSENT)
    schema, size = struct.unpack_from("<HH", descriptor, 4) if len(descriptor) >= 8 else (0, 0)
    if len(descriptor) < BOOT_POLICY_SIZE:
        return FirmwareBootPolicy(FirmwareBootPolicyStatus.MALFORMED, schema, size)
    if schema != BOOT_POLICY_SCHEMA:
        return FirmwareBootPolicy(FirmwareBootPolicyStatus.UNSUPPORTED, schema, size)
    epoch, reserved = struct.unpack_from("<II", descriptor, 8)
    if size != BOOT_POLICY_SIZE or reserved != 0:
        return FirmwareBootPolicy(FirmwareBootPolicyStatus.MALFORMED, schema, size)
    status = (
        FirmwareBootPolicyStatus.KNOWN
        if epoch in KNOWN_BOOT_POLICY_EPOCHS else FirmwareBootPolicyStatus.UNSUPPORTED
    )
    return FirmwareBootPolicy(status, schema, size, epoch)


def inspect_firmware_bytes_with_policy(
    data: bytes,
    *,
    max_size_bytes: int = MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE,
) -> FirmwareInspection:
    """Verify one immutable image before exposing its bounded capability.

    A caller's size override changes this inspection bound, not publication or
    activation policy. Unknown, absent or malformed capability is not admission.
    """
    _check_firmware_size(len(data), max_size_bytes)
    data = bytes(data)
    _check_firmware_size(len(data), max_size_bytes)
    if len(data) < ESP_IMAGE_HEADER_SIZE + ESP_SEGMENT_HEADER_SIZE + 1 + ESP_IMAGE_HASH_SIZE:
        _fail("firmware is too short to contain a complete ESP image")
    if data[0] != ESP_IMAGE_MAGIC:
        _fail("firmware magic must be 0xe9")

    segment_count = data[1]
    if not 1 <= segment_count <= ESP_IMAGE_MAX_SEGMENTS:
        _fail("firmware segment count must be between 1 and 16")

    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id != ESP32S3_CHIP_ID:
        _fail("firmware chip ID must be ESP32-S3 (9)")
    if data[23] != 1:
        _fail("firmware must contain an appended image SHA-256")

    offset = ESP_IMAGE_HEADER_SIZE
    checksum = ESP_CHECKSUM_MAGIC
    first_segment_offset = first_segment_size = first_segment_address = 0
    for index in range(segment_count):
        if offset + ESP_SEGMENT_HEADER_SIZE > len(data):
            _fail("firmware segment header is truncated")
        segment_address, segment_size = struct.unpack_from("<II", data, offset)
        offset += ESP_SEGMENT_HEADER_SIZE
        if segment_size == 0 or segment_size % 4 != 0:
            _fail("firmware segment sizes must be non-zero multiples of four")
        segment_end = offset + segment_size
        if segment_end > len(data):
            _fail("firmware segment data is truncated")
        if index == 0:
            first_segment_offset = offset
            first_segment_size = segment_size
            first_segment_address = segment_address
        for value in data[offset:segment_end]:
            checksum ^= value
        offset = segment_end

    checksum_offset = offset + ((15 - offset) % 16)
    prefix_end = checksum_offset + 1
    if len(data) != prefix_end + ESP_IMAGE_HASH_SIZE:
        _fail("firmware exact size does not match its parsed segments and image hash")
    if any(data[offset:checksum_offset]):
        _fail("firmware checksum padding must be zero")
    if data[checksum_offset] != checksum:
        _fail("firmware segment checksum does not match")

    image_digest = hashlib.sha256(data[:prefix_end]).digest()
    if data[prefix_end:] != image_digest:
        _fail("firmware appended image SHA-256 does not match its prefix")

    metadata = FirmwareArtifact(
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
        image_sha256=image_digest.hex(),
        chip_id=chip_id,
        segment_count=segment_count,
    )
    return FirmwareInspection(
        metadata=metadata,
        boot_policy=_inspect_boot_policy(
            memoryview(data)[first_segment_offset:first_segment_offset + first_segment_size]
        ),
        first_segment_load_address=first_segment_address,
        first_segment_size_bytes=first_segment_size,
    )


def inspect_firmware_bytes(data: bytes) -> FirmwareArtifact:
    """Return the stable five-field metadata for a publishable S3 image."""
    return inspect_firmware_bytes_with_policy(data).metadata


def inspect_firmware_with_policy(
    path: Path | str,
    *,
    max_size_bytes: int = MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE,
) -> FirmwareInspection:
    """Read once and verify metadata/capability from those same image bytes."""
    firmware_path = Path(path)
    _check_firmware_size(firmware_path.stat().st_size, max_size_bytes)
    return inspect_firmware_bytes_with_policy(
        firmware_path.read_bytes(), max_size_bytes=max_size_bytes,
    )


def inspect_firmware(path: Path | str) -> FirmwareArtifact:
    """Read and verify one publishable firmware file."""
    return inspect_firmware_with_policy(path).metadata


def inspect_release_notes(path: Path | str) -> ReleaseNotesArtifact:
    """Read and verify one publishable release-notes file."""
    notes_path = Path(path)
    if not 0 < notes_path.stat().st_size <= MAX_NOTES_SIZE:
        _fail("release notes size is outside the 256 KiB admission limit")
    data = notes_path.read_bytes()
    if not data or len(data) > MAX_NOTES_SIZE:
        _fail("release notes size is outside the 256 KiB admission limit")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        _fail("release notes must be valid UTF-8")
    return ReleaseNotesArtifact(
        size_bytes=len(data),
        sha256=hashlib.sha256(data).hexdigest(),
    )


def _validated_https_url(value: str, label: str):
    parsed = urlsplit(value)
    try:
        parsed.port
    except ValueError:
        _fail(f"{label} has an invalid port")
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or any(ord(character) < 33 for character in value)
        or "\\" in value
    ):
        _fail(f"{label} must be a plain HTTPS URL")
    return parsed


def _origin(parsed) -> tuple[str, str, int]:
    return parsed.scheme, parsed.hostname or "", parsed.port or 443


def build_manifest(
    *,
    artifact: FirmwareArtifact,
    channel: str,
    version: str,
    manifest_url: str,
    firmware_url: str,
    notes_url: str,
    notes: ReleaseNotesArtifact,
) -> dict[str, object]:
    """Build schema 1 metadata for one immutable, same-origin artifact."""
    if channel not in {"stable", "beta"}:
        _fail("manifest channel must be stable or beta")
    if not VERSION_PATTERN.fullmatch(version):
        _fail("manifest version contains unsafe characters")
    if artifact.chip_id != ESP32S3_CHIP_ID:
        _fail("manifest artifact must target ESP32-S3")
    if not 0 < artifact.size_bytes <= MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE:
        _fail("manifest artifact size is outside the admission limit")
    if not SHA256_PATTERN.fullmatch(artifact.sha256):
        _fail("manifest full-file SHA-256 is invalid")
    if not SHA256_PATTERN.fullmatch(artifact.image_sha256):
        _fail("manifest image SHA-256 is invalid")
    if not 0 < notes.size_bytes <= MAX_NOTES_SIZE:
        _fail("manifest notes size is outside the admission limit")
    if not SHA256_PATTERN.fullmatch(notes.sha256):
        _fail("manifest notes SHA-256 is invalid")

    manifest_uri = _validated_https_url(manifest_url, "manifest URL")
    firmware_uri = _validated_https_url(firmware_url, "firmware URL")
    notes_uri = _validated_https_url(notes_url, "notes URL")
    if _origin(firmware_uri) != _origin(manifest_uri):
        _fail("firmware URL must use the manifest origin")
    if _origin(notes_uri) != _origin(manifest_uri):
        _fail("notes URL must use the manifest origin")

    firmware_name = unquote(firmware_uri.path.rsplit("/", 1)[-1])
    notes_name = unquote(notes_uri.path.rsplit("/", 1)[-1])
    aliases = {"firmware.bin", "firmware_beta.bin"}
    if (
        firmware_name in aliases
        or not firmware_name.endswith(".bin")
        or artifact.sha256 not in firmware_name
        or "/" in firmware_name
        or "\\" in firmware_name
        or any(ord(character) < 33 for character in firmware_name)
    ):
        _fail("firmware URL must name an immutable SHA-addressed .bin")
    expected_notes_sha256 = notes.sha256
    if (
        not notes_name.endswith(".md")
        or expected_notes_sha256 not in notes_name
        or "/" in notes_name
        or "\\" in notes_name
        or any(ord(character) < 33 for character in notes_name)
    ):
        _fail("notes URL must name immutable SHA-addressed markdown")

    return {
        "schema_version": 1,
        "channel": channel,
        "version": version,
        "target": "esp32s3",
        "chip_id": artifact.chip_id,
        "artifact": {
            "url": firmware_url,
            "size_bytes": artifact.size_bytes,
            "sha256": artifact.sha256,
            "image_sha256": artifact.image_sha256,
        },
        "notes": {
            "url": notes_url,
            "size_bytes": notes.size_bytes,
            "sha256": notes.sha256,
        },
    }


def render_manifest_json(manifest: dict[str, object]) -> str:
    """Return canonical human-readable JSON with no host-specific metadata."""
    return json.dumps(
        manifest,
        indent=2,
        sort_keys=True,
        separators=(",", ": "),
    ) + "\n"


def release_names(
    channel: str,
    version: str,
    sha256: str,
    notes_sha256: str,
) -> dict[str, str]:
    """Return stable aliases and immutable names for one release artifact."""
    if channel not in {"stable", "beta"}:
        _fail("release channel must be stable or beta")
    if not VERSION_PATTERN.fullmatch(version):
        _fail("release version contains unsafe characters")
    if not SHA256_PATTERN.fullmatch(sha256):
        _fail("release SHA-256 must be 64 lowercase hexadecimal characters")
    if not SHA256_PATTERN.fullmatch(notes_sha256):
        _fail("release notes SHA-256 must be 64 lowercase hexadecimal characters")

    if channel == "stable":
        prefix = ""
        manifest_name = "manifest.json"
    else:
        prefix = "_beta"
        manifest_name = "manifest_beta.json"
    return {
        "firmware_alias": f"firmware{prefix}.bin",
        "notes_alias": f"version{prefix}.md",
        "manifest": manifest_name,
        "firmware_immutable": f"firmware{prefix}_{version}_{sha256}.bin",
        "notes_immutable": f"version{prefix}_{version}_{notes_sha256}.md",
    }


def create_manifest(
    *,
    firmware_path: Path,
    notes_path: Path,
    channel: str,
    version: str,
    public_base_url: str,
    output_path: Path,
) -> dict[str, object]:
    """Validate firmware and write its deterministic schema-1 manifest."""
    artifact = inspect_firmware(firmware_path)
    notes = inspect_release_notes(notes_path)
    names = release_names(
        channel,
        version,
        artifact.sha256,
        notes.sha256,
    )
    base_url = public_base_url.rstrip("/")
    _validated_https_url(base_url, "public base URL")
    manifest = build_manifest(
        artifact=artifact,
        channel=channel,
        version=version,
        manifest_url=f"{base_url}/{names['manifest']}",
        firmware_url=f"{base_url}/{names['firmware_immutable']}",
        notes_url=f"{base_url}/{names['notes_immutable']}",
        notes=notes,
    )
    temporary_path = output_path.with_name(f"{output_path.name}.tmp")
    try:
        temporary_path.write_text(render_manifest_json(manifest), encoding="utf-8")
        os.replace(temporary_path, output_path)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()
    return manifest


def _read_manifest(path: Path) -> dict[str, object]:
    decoded = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(decoded, dict):
        _fail("manifest root must be an object")
    return decoded


def verify_release(
    *,
    firmware_path: Path,
    notes_path: Path,
    manifest_path: Path,
    manifest_url: str,
) -> dict[str, object]:
    """Verify that a local manifest exactly describes a local S3 image."""
    manifest = _read_manifest(manifest_path)
    try:
        artifact_data = manifest["artifact"]
        if not isinstance(artifact_data, dict):
            _fail("manifest artifact must be an object")
        notes = inspect_release_notes(notes_path)
        notes_data = manifest["notes"]
        if not isinstance(notes_data, dict):
            _fail("manifest notes must be an object")
        notes_url = str(notes_data["url"])
        expected = build_manifest(
            artifact=inspect_firmware(firmware_path),
            channel=str(manifest["channel"]),
            version=str(manifest["version"]),
            manifest_url=manifest_url,
            firmware_url=str(artifact_data["url"]),
            notes_url=notes_url,
            notes=notes,
        )
    except KeyError as error:
        _fail(f"manifest is missing required field {error}")
    if render_manifest_json(manifest) != render_manifest_json(expected):
        _fail("manifest fields do not exactly describe the verified firmware and notes")
    return manifest


def _manifest_field(manifest: dict[str, object], field: str) -> str:
    artifact = manifest.get("artifact")
    notes = manifest.get("notes")
    if not isinstance(artifact, dict):
        _fail("manifest artifact must be an object")
    if not isinstance(notes, dict):
        _fail("manifest notes must be an object")
    values: dict[str, object] = {
        "channel": manifest.get("channel"),
        "version": manifest.get("version"),
        "artifact.url": artifact.get("url"),
        "artifact.sha256": artifact.get("sha256"),
        "artifact.image_sha256": artifact.get("image_sha256"),
        "artifact.size_bytes": artifact.get("size_bytes"),
        "notes.url": notes.get("url"),
        "notes.sha256": notes.get("sha256"),
        "notes.size_bytes": notes.get("size_bytes"),
    }
    if field == "artifact_name":
        value = unquote(urlsplit(str(artifact.get("url", ""))).path.rsplit("/", 1)[-1])
    elif field == "notes_name":
        value = unquote(urlsplit(str(notes.get("url", ""))).path.rsplit("/", 1)[-1])
    elif field in values:
        value = values[field]
    else:
        _fail(f"unsupported manifest field: {field}")
    if not isinstance(value, (str, int)) or "\n" in str(value):
        _fail(f"manifest field {field} is missing or invalid")
    return str(value)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate and describe Litch ESP32-S3 firmware artifacts."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect")
    inspect_parser.add_argument("--firmware", required=True, type=Path)
    inspect_parser.add_argument(
        "--field",
        required=True,
        choices=("size_bytes", "sha256", "image_sha256", "chip_id", "segment_count"),
    )

    inspect_notes_parser = subparsers.add_parser("inspect-notes")
    inspect_notes_parser.add_argument("--notes", required=True, type=Path)
    inspect_notes_parser.add_argument(
        "--field",
        required=True,
        choices=("size_bytes", "sha256"),
    )

    name_parser = subparsers.add_parser("name")
    name_parser.add_argument("--channel", required=True, choices=("stable", "beta"))
    name_parser.add_argument("--version", required=True)
    name_parser.add_argument("--sha256", required=True)
    name_parser.add_argument("--notes-sha256", required=True)
    name_parser.add_argument(
        "--field",
        required=True,
        choices=(
            "firmware_alias",
            "notes_alias",
            "manifest",
            "firmware_immutable",
            "notes_immutable",
        ),
    )

    create_parser = subparsers.add_parser("create-manifest")
    create_parser.add_argument("--firmware", required=True, type=Path)
    create_parser.add_argument("--notes", required=True, type=Path)
    create_parser.add_argument("--channel", required=True, choices=("stable", "beta"))
    create_parser.add_argument("--version", required=True)
    create_parser.add_argument("--public-base-url", required=True)
    create_parser.add_argument("--output", required=True, type=Path)

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--firmware", required=True, type=Path)
    verify_parser.add_argument("--notes", required=True, type=Path)
    verify_parser.add_argument("--manifest", required=True, type=Path)
    verify_parser.add_argument("--manifest-url", required=True)

    field_parser = subparsers.add_parser("manifest-field")
    field_parser.add_argument("--manifest", required=True, type=Path)
    field_parser.add_argument("--field", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.command == "inspect":
            print(getattr(inspect_firmware(args.firmware), args.field))
        elif args.command == "inspect-notes":
            print(getattr(inspect_release_notes(args.notes), args.field))
        elif args.command == "name":
            print(
                release_names(
                    args.channel,
                    args.version,
                    args.sha256,
                    args.notes_sha256,
                )[args.field]
            )
        elif args.command == "create-manifest":
            create_manifest(
                firmware_path=args.firmware,
                notes_path=args.notes,
                channel=args.channel,
                version=args.version,
                public_base_url=args.public_base_url,
                output_path=args.output,
            )
        elif args.command == "verify":
            verify_release(
                firmware_path=args.firmware,
                notes_path=args.notes,
                manifest_path=args.manifest,
                manifest_url=args.manifest_url,
            )
        elif args.command == "manifest-field":
            print(_manifest_field(_read_manifest(args.manifest), args.field))
        else:  # pragma: no cover - argparse prevents this branch.
            _fail("unsupported command")
    except (ArtifactValidationError, OSError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
