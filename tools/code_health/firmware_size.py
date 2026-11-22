#!/usr/bin/env python3
"""Deterministic firmware artifact size snapshots and comparisons.

This module deliberately reports measurements; it does not define or enforce
size thresholds.  Artifact byte size and linked ELF usage are kept separate
because PlatformIO derives the latter from sections in ``firmware.elf`` while
the former is the exact update image delivered to a device.
"""

from __future__ import annotations

import copy
import configparser
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any, Mapping


SCHEMA_VERSION = 1
_HASH_CHUNK_BYTES = 1024 * 1024
_MAX_PLATFORMIO_LOG_BYTES = 16_000_000

_ANSI_ESCAPE_RE = re.compile(
    r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))"
)
_PROCESSING_RE = re.compile(r"^\s*Processing\s+([A-Za-z0-9_.-]+)\s+\(")
_SIZE_RE = re.compile(
    r"\b(?P<kind>RAM|Flash)\s*:\s*.*?"
    r"\bused\s+(?P<used>[0-9][0-9,]*)\s+bytes\s+from\s+"
    r"(?P<capacity>[0-9][0-9,]*)\s+bytes\b",
    flags=re.IGNORECASE,
)
_PLATFORM_METADATA_RE = re.compile(
    r"^\s*PLATFORM:\s*(?P<name>.*?)\s+\((?P<version>[^)]+)\)(?:\s*>.*)?$",
    flags=re.IGNORECASE,
)
_PACKAGE_RE = re.compile(
    r"^\s*-\s+(?P<name>[A-Za-z0-9_.-]+)\s+@\s+(?P<version>.+?)\s*$"
)
_WINDOWS_ABSOLUTE_RE = re.compile(r"^[A-Za-z]:[\\/]")


_TARGET_CONFIGURATIONS: dict[str, dict[str, Any]] = {
    "litch-esp32c3": {
        "environment": "litch-esp32c3",
        "chip": "esp32c3",
        "framework": "arduino",
        "platform": "platformio/espressif32@6.12.0",
        "platformio_section": "env:litch-esp32c3",
        "physical_flash_bytes": 4 * 1024 * 1024,
        "physical_flash_provenance": {
            "source": "boards/litch_esp32c3.json",
            "field": "upload.flash_size",
        },
        "application_slot_bytes": 0x140000,
        "application_slot_provenance": {
            "source": "platformio.ini",
            "field": "env:litch-esp32c3 board_build.partitions",
            "selector": "implicit Arduino default.csv app0/ota_0",
            "platform": "platformio/espressif32@6.12.0",
        },
        "static_ram_bytes": 327680,
        "static_ram_provenance": {
            "source": "boards/litch_esp32c3.json",
            "field": "upload.maximum_ram_size",
        },
    },
    "litch-esp32s3": {
        "environment": "litch-esp32s3",
        "chip": "esp32s3",
        "framework": "arduino",
        "platform": "platformio/espressif32@6.12.0",
        "platformio_section": "env:litch-esp32s3",
        "physical_flash_bytes": 8 * 1024 * 1024,
        "physical_flash_provenance": {
            "source": "boards/litch_esp32s3.json",
            "field": "upload.flash_size",
        },
        "application_slot_bytes": 0x330000,
        "application_slot_provenance": {
            "source": "tools/partitions/partitions-8MB-ota-spiffs.csv",
            "field": "ota_0.size",
            "selector": "ota_0",
        },
        "static_ram_bytes": 327680,
        "static_ram_provenance": {
            "source": "boards/litch_esp32s3.json",
            "field": "upload.maximum_ram_size",
        },
    },
}

_TARGET_ALIASES = {
    "c3": "litch-esp32c3",
    "esp32c3": "litch-esp32c3",
    "s3": "litch-esp32s3",
    "esp32s3": "litch-esp32s3",
}

SUPPORTED_TARGETS = tuple(sorted(_TARGET_CONFIGURATIONS))


def validate_repository_size_configuration(
    repository_root: str | Path,
) -> dict[str, Any]:
    """Fail closed when capacity constants drift from authoritative repo files."""

    root = Path(repository_root).resolve()
    ini_path = root / "platformio.ini"
    try:
        ini_text = ini_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"cannot read platformio.ini for size validation: {exc}") from exc
    if len(ini_text.encode("utf-8")) > 1_000_000:
        raise ValueError("platformio.ini exceeds size-validation read limit")
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        parser.read_string(ini_text)
    except configparser.Error as exc:
        raise ValueError(f"cannot parse platformio.ini for size validation: {exc}") from exc

    checks: list[dict[str, Any]] = []

    def require_equal(target: str, field: str, observed: Any, expected: Any) -> None:
        if observed != expected:
            raise ValueError(
                f"size configuration drift for {target} {field}: "
                f"observed {observed!r}, expected {expected!r}"
            )
        checks.append(
            {
                "target": target,
                "field": field,
                "observed": observed,
                "expected": expected,
                "status": "verified",
            }
        )

    for target in SUPPORTED_TARGETS:
        configuration = _TARGET_CONFIGURATIONS[target]
        section = str(configuration["platformio_section"])
        if not parser.has_section(section):
            raise ValueError(f"platformio.ini is missing [{section}]")
        require_equal(
            target,
            "platformio.platform",
            parser.get(section, "platform", fallback="").strip(),
            configuration["platform"],
        )
        expected_board = "litch_esp32c3" if target.endswith("c3") else "litch_esp32s3"
        require_equal(
            target,
            "platformio.board",
            parser.get(section, "board", fallback="").strip(),
            expected_board,
        )
        board_path = root / "boards" / f"{expected_board}.json"
        try:
            board_bytes = board_path.read_bytes()
            if len(board_bytes) > 1_000_000:
                raise ValueError(f"{board_path.name} exceeds size-validation read limit")
            board = json.loads(board_bytes)
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise ValueError(f"cannot parse {board_path.name}: {exc}") from exc
        upload = board.get("upload")
        if not isinstance(upload, dict):
            raise ValueError(f"{board_path.name} has no upload object")
        require_equal(
            target,
            "board.upload.flash_size",
            _parse_capacity_bytes(upload.get("flash_size")),
            configuration["physical_flash_bytes"],
        )
        require_equal(
            target,
            "board.upload.maximum_ram_size",
            _parse_capacity_bytes(upload.get("maximum_ram_size")),
            configuration["static_ram_bytes"],
        )

    c3 = _TARGET_CONFIGURATIONS["litch-esp32c3"]
    c3_section = str(c3["platformio_section"])
    c3_partition = parser.get(c3_section, "board_build.partitions", fallback="").strip()
    require_equal(
        "litch-esp32c3",
        "platformio.board_build.partitions",
        c3_partition or None,
        None,
    )
    require_equal(
        "litch-esp32c3",
        "pinned_platform_default_app_slot_bytes",
        c3["application_slot_bytes"],
        0x140000,
    )

    s3 = _TARGET_CONFIGURATIONS["litch-esp32s3"]
    s3_section = str(s3["platformio_section"])
    partition_relative = parser.get(
        s3_section, "board_build.partitions", fallback=""
    ).strip()
    require_equal(
        "litch-esp32s3",
        "platformio.board_build.partitions",
        partition_relative,
        "tools/partitions/partitions-8MB-ota-spiffs.csv",
    )
    partition_path = root / partition_relative
    try:
        partition_text = partition_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"cannot read {partition_relative}: {exc}") from exc
    if len(partition_text.encode("utf-8")) > 1_000_000:
        raise ValueError("partition table exceeds size-validation read limit")
    partition_sizes: dict[str, int] = {}
    for raw_line in partition_text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        columns = [column.strip() for column in line.split(",")]
        if len(columns) < 5 or not columns[0]:
            continue
        parsed_size = _parse_capacity_bytes(columns[4])
        if parsed_size is not None:
            partition_sizes[columns[0]] = parsed_size
    require_equal(
        "litch-esp32s3",
        "partition.ota_0.size",
        partition_sizes.get("ota_0"),
        s3["application_slot_bytes"],
    )
    require_equal(
        "litch-esp32s3",
        "partition.ota_1.size",
        partition_sizes.get("ota_1"),
        s3["application_slot_bytes"],
    )
    return {"status": "verified", "checks": checks}


def _parse_capacity_bytes(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        return None
    text = value.strip()
    match = re.fullmatch(r"(?i)(0x[0-9a-f]+|[0-9]+)([km])?b?", text)
    if match is None:
        return None
    number = int(match.group(1), 0)
    suffix = match.group(2).lower() if match.group(2) else None
    multiplier = {None: 1, "k": 1024, "m": 1024 * 1024}[suffix]
    return number * multiplier


def canonical_target(target: str) -> str:
    """Return the canonical PlatformIO environment name for a supported target."""

    normalized = target.strip().lower()
    canonical = _TARGET_ALIASES.get(normalized, normalized)
    if canonical not in _TARGET_CONFIGURATIONS:
        supported = ", ".join(SUPPORTED_TARGETS)
        raise ValueError(f"unsupported firmware target {target!r}; expected one of {supported}")
    return canonical


def target_configuration(target: str) -> dict[str, Any]:
    """Return a caller-owned copy of deterministic configured capacities."""

    return copy.deepcopy(_TARGET_CONFIGURATIONS[canonical_target(target)])


def scrub_path(
    path: str | Path,
    *,
    repository_root: str | Path | None = None,
) -> str:
    """Return a stable path without leaking an external host directory.

    Paths below ``repository_root`` become normalized repository-relative POSIX
    paths.  Absolute external paths (and relative paths containing ``..``) are
    reduced to their basename.
    """

    raw_text = str(path)
    portable_text = raw_text.replace("\\", "/")
    native_path = Path(path)

    if repository_root is not None:
        root = Path(repository_root).resolve()
        candidate = native_path if native_path.is_absolute() else root / native_path
        try:
            relative = candidate.resolve(strict=False).relative_to(root)
        except ValueError:
            pass
        else:
            return relative.as_posix()

    looks_absolute = (
        native_path.is_absolute()
        or _WINDOWS_ABSOLUTE_RE.match(raw_text) is not None
        or portable_text.startswith("//")
    )
    portable_path = PurePosixPath(portable_text)
    if looks_absolute or ".." in portable_path.parts:
        return portable_path.name

    normalized_parts = tuple(part for part in portable_path.parts if part not in ("", "."))
    if not normalized_parts:
        return portable_path.name or "."
    return PurePosixPath(*normalized_parts).as_posix()


def _unavailable_usage(reason: str) -> dict[str, int | str | None]:
    return {
        "used_bytes": None,
        "capacity_bytes": None,
        "remaining_bytes": None,
        "unavailable_reason": reason,
    }


def _measured_usage(used: int, capacity: int) -> dict[str, int | str | None]:
    return {
        "used_bytes": used,
        "capacity_bytes": capacity,
        "remaining_bytes": capacity - used,
        "unavailable_reason": None,
    }


def _clean_platformio_output(text: str) -> str:
    return _ANSI_ESCAPE_RE.sub("", text).replace("\r", "\n")


def parse_platformio_size_log(
    text: str,
    *,
    target: str | None = None,
) -> dict[str, Any]:
    """Parse exact linked RAM/flash usage from PlatformIO console output.

    A combined log may contain multiple ``Processing <environment>`` sections.
    Passing ``target`` prevents a metric from another environment being
    silently attributed to the requested firmware.
    """

    canonical = canonical_target(target) if target is not None else None
    records: dict[str | None, dict[str, tuple[int, int]]] = {}
    current_environment: str | None = None

    for line in _clean_platformio_output(text).splitlines():
        processing_match = _PROCESSING_RE.search(line)
        if processing_match is not None:
            current_environment = processing_match.group(1).lower()
            records.setdefault(current_environment, {})
            continue

        size_match = _SIZE_RE.search(line)
        if size_match is None:
            continue
        kind = size_match.group("kind").lower()
        used = int(size_match.group("used").replace(",", ""))
        capacity = int(size_match.group("capacity").replace(",", ""))
        environment_records = records.setdefault(current_environment, {})
        if kind in environment_records and environment_records[kind] != (used, capacity):
            subject = current_environment or "unscoped output"
            raise ValueError(
                f"PlatformIO log contains conflicting {kind.upper()} summaries for {subject}"
            )
        environment_records[kind] = (used, capacity)

    populated = {environment: values for environment, values in records.items() if values}
    selected_environment: str | None
    selected: dict[str, tuple[int, int]]
    if canonical is not None and canonical in populated:
        selected_environment = canonical
        selected = populated[canonical]
    elif canonical is not None and None in populated and len(populated) == 1:
        selected_environment = None
        selected = populated[None]
    elif canonical is None and len(populated) == 1:
        selected_environment, selected = next(iter(populated.items()))
    elif canonical is None and len(populated) > 1:
        raise ValueError("PlatformIO log contains multiple environments; target is required")
    else:
        selected_environment = canonical
        selected = {}

    subject = canonical or selected_environment or "requested target"
    result: dict[str, Any] = {"environment": selected_environment}
    for kind, display_name in (("ram", "RAM"), ("flash", "Flash")):
        if kind in selected:
            result[kind] = _measured_usage(*selected[kind])
        else:
            result[kind] = _unavailable_usage(
                f"PlatformIO log did not contain a {display_name} summary for {subject}"
            )
    return result


def parse_platformio_toolchain_log(
    text: str,
    *,
    target: str,
) -> dict[str, Any]:
    """Extract target-scoped platform/framework/toolchain package versions."""

    canonical = canonical_target(target)
    records: dict[str, dict[str, Any]] = {}
    current_environment: str | None = None
    for line in _clean_platformio_output(text).splitlines():
        processing_match = _PROCESSING_RE.search(line)
        if processing_match is not None:
            current_environment = processing_match.group(1).lower()
            records.setdefault(current_environment, {"platform": None, "packages": {}})
            continue
        if current_environment is None:
            continue
        platform_match = _PLATFORM_METADATA_RE.match(line)
        if platform_match is not None:
            records[current_environment]["platform"] = {
                "name": platform_match.group("name").strip(),
                "version": platform_match.group("version").strip(),
            }
            continue
        package_match = _PACKAGE_RE.match(line)
        if package_match is not None:
            records[current_environment]["packages"][package_match.group("name")] = (
                package_match.group("version").strip()
            )

    selected = records.get(canonical)
    if selected is None:
        return {
            "status": "unavailable",
            "platform": None,
            "framework_packages": [],
            "toolchain_packages": [],
            "packages": [],
            "unavailable_reason": (
                f"PlatformIO log did not contain toolchain metadata for {canonical}"
            ),
        }
    packages = [
        {"name": name, "version": version}
        for name, version in sorted(selected["packages"].items())
    ]
    present = selected["platform"] is not None or bool(packages)
    return {
        "status": "measured" if present else "unavailable",
        "platform": selected["platform"],
        "framework_packages": [
            item for item in packages if item["name"].startswith("framework-")
        ],
        "toolchain_packages": [
            item for item in packages if item["name"].startswith("toolchain-")
        ],
        "packages": packages,
        "unavailable_reason": (
            None
            if present
            else f"PlatformIO log contained no toolchain metadata for {canonical}"
        ),
    }


def _unavailable_toolchain(reason: str) -> dict[str, Any]:
    return {
        "status": "unavailable",
        "platform": None,
        "framework_packages": [],
        "toolchain_packages": [],
        "packages": [],
        "unavailable_reason": reason,
    }


def _artifact_details(
    artifact_path: str | Path | None,
    *,
    repository_root: str | Path | None,
    application_slot_bytes: int,
) -> dict[str, Any]:
    if artifact_path is None:
        return {
            "path": None,
            "size_bytes": None,
            "sha256": None,
            "application_slot_remaining_bytes": None,
            "unavailable_reason": "firmware artifact not supplied",
        }

    candidate = Path(artifact_path)
    if candidate.is_dir():
        candidate = candidate / "firmware.bin"
    stable_path = scrub_path(candidate, repository_root=repository_root)

    def unavailable(reason: str) -> dict[str, Any]:
        return {
            "path": stable_path,
            "size_bytes": None,
            "sha256": None,
            "application_slot_remaining_bytes": None,
            "unavailable_reason": reason,
        }

    open_flags = os.O_RDONLY
    open_flags |= getattr(os, "O_CLOEXEC", 0)
    open_flags |= getattr(os, "O_NONBLOCK", 0)
    descriptor = None
    try:
        try:
            descriptor = os.open(candidate, open_flags)
        except FileNotFoundError:
            return unavailable("firmware artifact not found")
        except OSError:
            return unavailable("firmware artifact could not be read")

        try:
            opened_stat = os.fstat(descriptor)
        except OSError:
            return unavailable("firmware artifact could not be inspected")
        if not stat.S_ISREG(opened_stat.st_mode):
            return unavailable("firmware artifact is not a regular file")

        declared_size_bytes = opened_stat.st_size
        if declared_size_bytes > application_slot_bytes:
            raise ValueError(
                "firmware artifact exceeds application slot capacity: "
                f"{declared_size_bytes} > {application_slot_bytes} bytes"
            )

        digest = hashlib.sha256()
        size_bytes = 0
        try:
            while True:
                remaining_bytes = application_slot_bytes - size_bytes
                chunk = os.read(
                    descriptor,
                    min(_HASH_CHUNK_BYTES, remaining_bytes + 1),
                )
                if not chunk:
                    break
                next_size_bytes = size_bytes + len(chunk)
                if next_size_bytes > application_slot_bytes:
                    raise ValueError(
                        "firmware artifact exceeds application slot capacity: "
                        f"{next_size_bytes} > {application_slot_bytes} bytes"
                    )
                digest.update(chunk)
                size_bytes = next_size_bytes
        except OSError:
            return unavailable("firmware artifact could not be read")
        try:
            final_stat = os.fstat(descriptor)
        except OSError:
            return unavailable("firmware artifact could not be inspected")

        stable_fields = (
            "st_dev",
            "st_ino",
            "st_mode",
            "st_nlink",
            "st_size",
            "st_mtime_ns",
            "st_ctime_ns",
        )
        opened_identity = tuple(
            getattr(opened_stat, field, None) for field in stable_fields
        )
        final_identity = tuple(
            getattr(final_stat, field, None) for field in stable_fields
        )
        if size_bytes != declared_size_bytes or final_identity != opened_identity:
            raise ValueError("firmware artifact changed while being read")
    finally:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass

    return {
        "path": stable_path,
        "size_bytes": size_bytes,
        "sha256": digest.hexdigest(),
        "application_slot_remaining_bytes": application_slot_bytes - size_bytes,
        "unavailable_reason": None,
    }


def _linked_usage(
    *,
    target: str,
    configuration: Mapping[str, Any],
    platformio_log_text: str | None,
    platformio_log_path: str | Path | None,
    repository_root: str | Path | None,
) -> dict[str, Any]:
    if platformio_log_text is not None and platformio_log_path is not None:
        raise ValueError("provide PlatformIO log text or a log path, not both")

    source_path: str | None = None
    unavailable_reason: str | None = None
    log_text = platformio_log_text
    if platformio_log_path is not None:
        source_path = scrub_path(platformio_log_path, repository_root=repository_root)
        log_candidate = Path(platformio_log_path)
        if not log_candidate.is_file():
            unavailable_reason = "PlatformIO log not found"
        else:
            try:
                if log_candidate.stat().st_size > _MAX_PLATFORMIO_LOG_BYTES:
                    unavailable_reason = (
                        f"PlatformIO log exceeds {_MAX_PLATFORMIO_LOG_BYTES} byte limit"
                    )
                else:
                    log_text = log_candidate.read_text(encoding="utf-8", errors="replace")
            except OSError:
                unavailable_reason = "PlatformIO log could not be read"
    elif platformio_log_text is not None:
        source_path = None
    else:
        unavailable_reason = "PlatformIO log not supplied"

    if log_text is None:
        parsed = {
            "environment": target,
            "ram": _unavailable_usage(unavailable_reason or "PlatformIO log unavailable"),
            "flash": _unavailable_usage(unavailable_reason or "PlatformIO log unavailable"),
        }
        toolchain = _unavailable_toolchain(
            unavailable_reason or "PlatformIO log unavailable"
        )
    else:
        parsed = parse_platformio_size_log(log_text, target=target)
        toolchain = parse_platformio_toolchain_log(log_text, target=target)

    capacity_fields = {
        "ram": int(configuration["static_ram_bytes"]),
        "flash": int(configuration["application_slot_bytes"]),
    }
    for kind, configured_capacity in capacity_fields.items():
        metric = parsed[kind]
        metric["configured_capacity_bytes"] = configured_capacity
        observed_capacity = metric["capacity_bytes"]
        metric["capacity_matches_configuration"] = (
            observed_capacity == configured_capacity if observed_capacity is not None else None
        )

    return {
        "source": {
            "kind": "platformio_log",
            "path": source_path,
        },
        "environment": parsed["environment"],
        "ram": parsed["ram"],
        "flash": parsed["flash"],
        "toolchain": toolchain,
    }


def create_firmware_size_snapshot(
    *,
    target: str,
    role: str,
    artifact_path: str | Path | None,
    repository_root: str | Path | None = None,
    platformio_log_text: str | None = None,
    platformio_log_path: str | Path | None = None,
    revision: str | None = None,
) -> dict[str, Any]:
    """Create one deterministic current or baseline firmware size snapshot."""

    if role not in {"baseline", "current"}:
        raise ValueError("firmware size snapshot role must be 'baseline' or 'current'")
    canonical = canonical_target(target)
    configuration = target_configuration(canonical)
    artifact = _artifact_details(
        artifact_path,
        repository_root=repository_root,
        application_slot_bytes=int(configuration["application_slot_bytes"]),
    )
    linked_usage = _linked_usage(
        target=canonical,
        configuration=configuration,
        platformio_log_text=platformio_log_text,
        platformio_log_path=platformio_log_path,
        repository_root=repository_root,
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "firmware_size_snapshot",
        "target": canonical,
        "role": role,
        "revision": revision,
        "configuration": configuration,
        "artifact": artifact,
        "linked_usage": linked_usage,
    }


def validate_measured_linked_capacities(snapshot: Mapping[str, Any]) -> None:
    """Reject a measured linker capacity that disagrees with configuration.

    Missing linked-use summaries remain unavailable evidence.  This validator
    is intended for freshly built snapshots, where a measured-but-different
    capacity would make the reported remaining-byte margin stale.
    """

    target = str(snapshot.get("target") or "unknown target")
    linked_usage = snapshot.get("linked_usage")
    if not isinstance(linked_usage, Mapping):
        raise ValueError(f"{target} snapshot lacks linked usage")
    for kind in ("ram", "flash"):
        metric = linked_usage.get(kind)
        if not isinstance(metric, Mapping):
            raise ValueError(f"{target} snapshot lacks linked {kind} usage")
        observed = metric.get("capacity_bytes")
        configured = metric.get("configured_capacity_bytes")
        if observed is not None and observed != configured:
            raise ValueError(
                f"{target} linked {kind} capacity mismatch: observed {observed} bytes, "
                f"configured {configured} bytes"
            )


def _required_mapping(value: Any, path: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ValueError(f"firmware size snapshot {path} must be an object")
    return value


def _byte_integer(value: Any, path: str, *, allow_negative: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"firmware size snapshot {path} must be an integer")
    if not allow_negative and value < 0:
        raise ValueError(f"firmware size snapshot {path} must be nonnegative")
    return value


def validate_firmware_size_snapshot(
    snapshot: Mapping[str, Any],
    *,
    expected_role: str | None = None,
) -> str:
    """Validate the complete comparison-facing snapshot schema.

    Unavailable measurements are allowed only as an explicit all-null metric
    with a reason.  Byte counts never accept booleans, which Python otherwise
    treats as integers.
    """

    if snapshot.get("kind") != "firmware_size_snapshot":
        raise ValueError("input is not a firmware size snapshot")
    if type(snapshot.get("schema_version")) is not int or snapshot.get(
        "schema_version"
    ) != SCHEMA_VERSION:
        raise ValueError("firmware size snapshot has an unsupported schema version")
    role = snapshot.get("role")
    if role not in {"baseline", "current"}:
        raise ValueError("firmware size snapshot has an invalid role")
    if expected_role is not None and role != expected_role:
        raise ValueError(f"firmware size snapshot must declare role={expected_role}")
    target = canonical_target(str(snapshot.get("target", "")))

    configuration = _required_mapping(snapshot.get("configuration"), "configuration")
    slot_capacity = _byte_integer(
        configuration.get("application_slot_bytes"),
        "configuration.application_slot_bytes",
    )
    ram_capacity = _byte_integer(
        configuration.get("static_ram_bytes"),
        "configuration.static_ram_bytes",
    )
    physical_flash_capacity = _byte_integer(
        configuration.get("physical_flash_bytes"),
        "configuration.physical_flash_bytes",
    )
    if min(slot_capacity, ram_capacity, physical_flash_capacity) <= 0:
        raise ValueError("firmware size snapshot capacities must be positive")

    artifact = _required_mapping(snapshot.get("artifact"), "artifact")
    artifact_size = artifact.get("size_bytes")
    artifact_remaining = artifact.get("application_slot_remaining_bytes")
    artifact_hash = artifact.get("sha256")
    artifact_reason = artifact.get("unavailable_reason")
    if artifact_size is None:
        if artifact_remaining is not None or artifact_hash is not None:
            raise ValueError("unavailable firmware artifact has measured fields")
        if not isinstance(artifact_reason, str) or not artifact_reason.strip():
            raise ValueError("unavailable firmware artifact requires a reason")
    else:
        measured_size = _byte_integer(artifact_size, "artifact.size_bytes")
        if measured_size > slot_capacity:
            raise ValueError(
                "firmware artifact exceeds application slot capacity: "
                f"{measured_size} > {slot_capacity} bytes"
            )
        measured_remaining = _byte_integer(
            artifact_remaining,
            "artifact.application_slot_remaining_bytes",
            allow_negative=True,
        )
        if measured_remaining != slot_capacity - measured_size:
            raise ValueError("firmware artifact slot margin is inconsistent")
        if not isinstance(artifact_hash, str) or re.fullmatch(
            r"[0-9a-f]{64}", artifact_hash
        ) is None:
            raise ValueError("measured firmware artifact requires a SHA-256 hash")
        if artifact_reason is not None:
            raise ValueError("measured firmware artifact cannot have an unavailable reason")

    linked_usage = _required_mapping(snapshot.get("linked_usage"), "linked_usage")
    _required_mapping(linked_usage.get("source"), "linked_usage.source")
    _required_mapping(linked_usage.get("toolchain"), "linked_usage.toolchain")
    if linked_usage.get("environment") not in {None, target}:
        raise ValueError("linked usage environment disagrees with snapshot target")
    for kind, configured_capacity in (("ram", ram_capacity), ("flash", slot_capacity)):
        metric = _required_mapping(linked_usage.get(kind), f"linked_usage.{kind}")
        reported_configured = _byte_integer(
            metric.get("configured_capacity_bytes"),
            f"linked_usage.{kind}.configured_capacity_bytes",
        )
        if reported_configured != configured_capacity:
            raise ValueError(f"linked {kind} configured capacity is inconsistent")
        values = (
            metric.get("used_bytes"),
            metric.get("capacity_bytes"),
            metric.get("remaining_bytes"),
        )
        if all(value is None for value in values):
            if metric.get("capacity_matches_configuration") is not None:
                raise ValueError(f"unavailable linked {kind} has a capacity verdict")
            reason = metric.get("unavailable_reason")
            if not isinstance(reason, str) or not reason.strip():
                raise ValueError(f"unavailable linked {kind} requires a reason")
            continue
        if any(value is None for value in values):
            raise ValueError(f"linked {kind} measurement is incomplete")
        used = _byte_integer(values[0], f"linked_usage.{kind}.used_bytes")
        capacity = _byte_integer(values[1], f"linked_usage.{kind}.capacity_bytes")
        if capacity <= 0:
            raise ValueError(f"linked {kind} capacity must be positive")
        remaining = _byte_integer(
            values[2],
            f"linked_usage.{kind}.remaining_bytes",
            allow_negative=True,
        )
        if remaining != capacity - used:
            raise ValueError(f"linked {kind} remaining-byte metric is inconsistent")
        verdict = metric.get("capacity_matches_configuration")
        if not isinstance(verdict, bool) or verdict != (capacity == reported_configured):
            raise ValueError(f"linked {kind} capacity verdict is inconsistent")
        if metric.get("unavailable_reason") is not None:
            raise ValueError(f"measured linked {kind} cannot have an unavailable reason")
    return target


def _nested_value(data: Mapping[str, Any], path: tuple[str, ...]) -> Any:
    value: Any = data
    for part in path:
        if not isinstance(value, Mapping) or part not in value:
            return None
        value = value[part]
    return value


def _delta(
    baseline: Mapping[str, Any],
    current: Mapping[str, Any],
    *,
    value_path: tuple[str, ...],
    reason_path: tuple[str, ...] | None = None,
) -> dict[str, Any]:
    baseline_value = _nested_value(baseline, value_path)
    current_value = _nested_value(current, value_path)
    metric_name = ".".join(value_path)
    for label, value in (("baseline", baseline_value), ("current", current_value)):
        if value is not None and (isinstance(value, bool) or not isinstance(value, int)):
            raise ValueError(f"{label} {metric_name} must be an integer or null")
    reasons: list[str] = []
    if baseline_value is None:
        baseline_reason = _nested_value(baseline, reason_path) if reason_path else None
        reasons.append(f"baseline unavailable: {baseline_reason or 'metric not reported'}")
    if current_value is None:
        current_reason = _nested_value(current, reason_path) if reason_path else None
        reasons.append(f"current unavailable: {current_reason or 'metric not reported'}")
    return {
        "baseline_bytes": baseline_value,
        "current_bytes": current_value,
        "delta_bytes": (
            current_value - baseline_value
            if baseline_value is not None and current_value is not None
            else None
        ),
        "unavailable_reason": "; ".join(reasons) if reasons else None,
    }


def compare_firmware_size_snapshots(
    baseline: Mapping[str, Any],
    current: Mapping[str, Any],
) -> dict[str, Any]:
    """Compare two snapshots of the same target without applying a gate."""

    baseline_target = validate_firmware_size_snapshot(
        baseline, expected_role="baseline"
    )
    current_target = validate_firmware_size_snapshot(current, expected_role="current")
    if baseline_target != current_target:
        raise ValueError(
            f"cannot compare different firmware targets: {baseline_target} and {current_target}"
        )

    artifact_reason = ("artifact", "unavailable_reason")
    ram_reason = ("linked_usage", "ram", "unavailable_reason")
    flash_reason = ("linked_usage", "flash", "unavailable_reason")
    metric_paths = {
        "application_slot_capacity_bytes": (
            ("configuration", "application_slot_bytes"),
            None,
        ),
        "application_slot_remaining_bytes": (
            ("artifact", "application_slot_remaining_bytes"),
            artifact_reason,
        ),
        "artifact_size_bytes": (("artifact", "size_bytes"), artifact_reason),
        "linked_flash_capacity_bytes": (
            ("linked_usage", "flash", "capacity_bytes"),
            flash_reason,
        ),
        "linked_flash_remaining_bytes": (
            ("linked_usage", "flash", "remaining_bytes"),
            flash_reason,
        ),
        "linked_flash_used_bytes": (
            ("linked_usage", "flash", "used_bytes"),
            flash_reason,
        ),
        "linked_ram_capacity_bytes": (
            ("linked_usage", "ram", "capacity_bytes"),
            ram_reason,
        ),
        "linked_ram_remaining_bytes": (
            ("linked_usage", "ram", "remaining_bytes"),
            ram_reason,
        ),
        "linked_ram_used_bytes": (
            ("linked_usage", "ram", "used_bytes"),
            ram_reason,
        ),
        "physical_flash_capacity_bytes": (
            ("configuration", "physical_flash_bytes"),
            None,
        ),
        "static_ram_capacity_bytes": (
            ("configuration", "static_ram_bytes"),
            None,
        ),
    }
    deltas = {
        name: _delta(
            baseline,
            current,
            value_path=value_path,
            reason_path=reason_path,
        )
        for name, (value_path, reason_path) in metric_paths.items()
    }

    baseline_hash = _nested_value(baseline, ("artifact", "sha256"))
    current_hash = _nested_value(current, ("artifact", "sha256"))
    hash_reasons: list[str] = []
    if baseline_hash is None:
        hash_reasons.append("baseline artifact hash unavailable")
    if current_hash is None:
        hash_reasons.append("current artifact hash unavailable")

    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "firmware_size_comparison",
        "target": baseline_target,
        "baseline": {
            "revision": baseline.get("revision"),
            "artifact_path": _nested_value(baseline, ("artifact", "path")),
        },
        "current": {
            "revision": current.get("revision"),
            "artifact_path": _nested_value(current, ("artifact", "path")),
        },
        "artifact_hash": {
            "baseline_sha256": baseline_hash,
            "current_sha256": current_hash,
            "changed": (
                baseline_hash != current_hash
                if baseline_hash is not None and current_hash is not None
                else None
            ),
            "unavailable_reason": "; ".join(hash_reasons) if hash_reasons else None,
        },
        "deltas": deltas,
    }


def render_json(value: Mapping[str, Any]) -> str:
    """Render stable, human-readable JSON suitable for an artifact file."""

    return json.dumps(
        value,
        indent=2,
        sort_keys=True,
        separators=(",", ": "),
        ensure_ascii=False,
    ) + "\n"
