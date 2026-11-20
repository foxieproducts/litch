#!/usr/bin/env python3
"""Capture exact dependency license evidence from already-populated local caches.

The Python helper performs no fetches, but CocoaPods evaluates each selected
local Ruby podspec. Run this maintenance operation in a network-denied
environment when network isolation is required. Its output is a reviewable
tracked input to release_inventory.py, not part of normal release generation.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path


RELEASE_TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(RELEASE_TOOLS))

import release_inventory  # noqa: E402


COCOAPODS_PATH_PODS = (
    ("file_selector_ios", "0.0.1", "file_selector_ios", "0.5.3+6", "ios/file_selector_ios.podspec"),
    ("package_info_plus", "0.4.5", "package_info_plus", "8.3.1", "ios/package_info_plus.podspec"),
    ("permission_handler_apple", "9.3.0", "permission_handler_apple", "9.4.7", "ios/permission_handler_apple.podspec"),
    ("reactive_ble_mobile", "0.0.1", "reactive_ble_mobile", "5.4.0", "darwin/reactive_ble_mobile.podspec"),
    ("sensors_plus", "0.0.1", "sensors_plus", "6.1.2", "ios/sensors_plus.podspec"),
    ("share_plus", "0.0.1", "share_plus", "11.1.0", "ios/share_plus.podspec"),
    ("shared_preferences_foundation", "0.0.1", "shared_preferences_foundation", "2.5.6", "darwin/shared_preferences_foundation.podspec"),
)

PLATFORMIO_PACKAGES = (
    {
        "ecosystem": "platformio-platform",
        "name": "platformio/espressif32",
        "version": "6.12.0",
        "owner": "platformio",
        "directory": "espressif32",
        "repository_url": "https://github.com/platformio/platform-espressif32.git",
        "license_expression": "Apache-2.0",
        "files": (
            (".piopm", "piopm.json", "provenance-metadata"),
            ("platform.json", "platform.json", "provenance-metadata"),
            ("LICENSE", "LICENSE", "license-text"),
        ),
    },
    {
        "ecosystem": "platformio-platform",
        "name": "platformio/native",
        "version": "1.2.1",
        "owner": "platformio",
        "directory": "native",
        "repository_url": "https://github.com/platformio/platform-native.git",
        "license_expression": "Apache-2.0",
        "files": (
            (".piopm", "piopm.json", "provenance-metadata"),
            ("platform.json", "platform.json", "provenance-metadata"),
            ("LICENSE", "LICENSE", "license-text"),
        ),
    },
    {
        "ecosystem": "platformio-lib",
        "name": "bblanchon/ArduinoJson",
        "version": "6.21.5",
        "owner": "bblanchon",
        "directory": "ArduinoJson",
        "repository_url": "https://github.com/bblanchon/ArduinoJson.git",
        "license_expression": "MIT",
        "files": (
            (".piopm", "piopm.json", "provenance-metadata"),
            ("library.json", "library.json", "provenance-metadata"),
            ("library.properties", "library.properties", "provenance-metadata"),
            ("LICENSE.txt", "LICENSE.txt", "license-text"),
        ),
    },
    {
        "ecosystem": "platformio-lib",
        "name": "makuna/NeoPixelBus",
        "version": "2.8.4",
        "owner": "makuna",
        "directory": "NeoPixelBus",
        "repository_url": "https://github.com/Makuna/NeoPixelBus",
        "license_expression": "NOASSERTION",
        "files": (
            (".piopm", "piopm.json", "provenance-metadata"),
            ("library.json", "library.json", "provenance-metadata"),
            ("library.properties", "library.properties", "provenance-metadata"),
            ("src/NeoPixelBus.h", "src/NeoPixelBus.h", "provenance-metadata"),
            ("COPYING", "COPYING", "license-text"),
        ),
    },
    {
        "ecosystem": "platformio-lib",
        "name": "h2zero/NimBLE-Arduino",
        "version": "2.3.7",
        "owner": "h2zero",
        "directory": "NimBLE-Arduino",
        "repository_url": "https://github.com/h2zero/NimBLE-Arduino",
        "license_expression": "NOASSERTION",
        "files": (
            (".piopm", "piopm.json", "provenance-metadata"),
            ("library.properties", "library.properties", "provenance-metadata"),
            ("LICENSE", "LICENSE", "license-text"),
            ("NOTICE", "NOTICE", "notice-text"),
            ("src/nimble/LICENSE", "src/nimble/LICENSE", "license-text"),
            ("src/nimble/NOTICE", "src/nimble/NOTICE", "notice-text"),
            (
                "src/nimble/ext/tinycrypt/LICENSE",
                "src/nimble/ext/tinycrypt/LICENSE",
                "license-text",
            ),
        ),
    },
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _safe_name(value: str) -> str:
    return value.replace("/", "__").replace("+", "_")


def _write_exact(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() != data:
        raise ValueError(f"Conflicting captured evidence bytes: {path}")
    path.write_bytes(data)


def _capture_file(
    source: Path,
    destination_relative: Path,
    *,
    role: str,
    origin: str,
    evidence_root: Path,
) -> dict[str, str]:
    return _capture_bytes(source.read_bytes(), destination_relative, role, origin, evidence_root)


def _capture_bytes(
    data: bytes,
    destination_relative: Path,
    role: str,
    origin: str,
    evidence_root: Path,
) -> dict[str, str]:
    destination = evidence_root / destination_relative
    _write_exact(destination, data)
    return {
        "path": (
            release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE
            / destination_relative
        ).as_posix(),
        "sha256": _sha256(data),
        "role": role,
        "origin": origin,
    }


def _pubspec_lock_path_sources(path: Path) -> dict[str, str]:
    package_name: str | None = None
    paths: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        package_match = re.fullmatch(r"  ([^ ]+):", raw_line)
        if package_match is not None:
            package_name = package_match.group(1)
            continue
        if package_name is None:
            continue
        path_match = re.fullmatch(r"      path:\s*['\"]?(.+?)['\"]?", raw_line)
        if path_match is not None:
            paths[package_name] = path_match.group(1)
    return paths


def _read_pubspec_identity(path: Path) -> tuple[str, str]:
    pubspec_text = path.read_text(encoding="utf-8")
    name_match = re.search(r"(?m)^name:\s*['\"]?([^'\"\s#]+)", pubspec_text)
    version_match = re.search(
        r"(?m)^version:\s*['\"]?([^'\"\s#]+)", pubspec_text
    )
    if name_match is None or version_match is None:
        raise ValueError(f"Package pubspec has no exact name/version: {path}")
    return name_match.group(1), version_match.group(1)


def _read_pubspec_name(path: Path) -> str:
    pubspec_text = path.read_text(encoding="utf-8")
    name_match = re.search(r"(?m)^name:\s*['\"]?([^'\"\s#]+)", pubspec_text)
    if name_match is None:
        raise ValueError(f"Package pubspec has no name: {path}")
    return name_match.group(1)


def _capture_flutter_sdk(
    repo_root: Path,
    flutter_sdk: Path,
    evidence_root: Path,
) -> tuple[list[dict[str, object]], list[dict[str, str]], str]:
    version = release_inventory.exact_flutter_sdk_version(repo_root)
    if version is None:
        raise ValueError("app/pubspec.yaml does not pin an exact Flutter SDK")
    version_file = flutter_sdk / "bin/cache/flutter.version.json"
    version_payload = json.loads(version_file.read_text(encoding="utf-8"))
    if (
        version_payload.get("frameworkVersion") != version
        or version_payload.get("flutterVersion") != version
        or version_payload.get("channel") != "stable"
        or version_payload.get("repositoryUrl")
        != "https://github.com/flutter/flutter.git"
    ):
        raise ValueError(f"Installed Flutter SDK does not match pinned {version}")
    framework_revision = version_payload.get("frameworkRevision")
    if not isinstance(framework_revision, str) or not re.fullmatch(
        r"[0-9a-f]{40}", framework_revision
    ):
        raise ValueError("Installed Flutter SDK has no exact framework revision")

    sdk_relative = Path("flutter-sdk") / version
    normalized_version_bytes = (
        json.dumps(version_payload, indent=2, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    normalized_version_relative = sdk_relative / "flutter.version.json"
    _write_exact(
        evidence_root / normalized_version_relative,
        normalized_version_bytes,
    )
    normalized_version_evidence = {
        "path": (
            release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE
            / normalized_version_relative
        ).as_posix(),
        "sha256": _sha256(normalized_version_bytes),
        "role": "provenance-metadata",
        "origin": (
            f"Normalized metadata observation from the locally installed Flutter "
            f"{version} stable SDK used to verify the tracked pin"
        ),
    }
    shared_evidence = [
        normalized_version_evidence,
        _capture_file(
            flutter_sdk / "LICENSE",
            sdk_relative / "LICENSE",
            role="license-text",
            origin=f"Exact LICENSE from the locally installed Flutter {version} stable SDK",
            evidence_root=evidence_root,
        ),
    ]
    components = release_inventory.parse_pubspec_lock(
        repo_root / "app/pubspec.lock", repo_root
    )
    records: list[dict[str, object]] = []
    for component in components:
        if component.source_type != "sdk":
            continue
        if component.name == "sky_engine":
            package_pubspec = flutter_sdk / "bin/cache/pkg/sky_engine/pubspec.yaml"
            package_shared = [
                normalized_version_evidence,
                _capture_sky_engine_license(flutter_sdk, version, version_payload["engineRevision"], evidence_root),
            ]
            license_scope = "the complete sky_engine mixed engine license collection"
        else:
            package_pubspec = flutter_sdk / "packages" / component.name / "pubspec.yaml"
            package_shared = shared_evidence
            license_scope = "the distribution root LICENSE"
        if _read_pubspec_name(package_pubspec) != component.name:
            raise ValueError(f"Flutter SDK package identity mismatch for {component.name}")
        package_evidence = _capture_file(
            package_pubspec,
            sdk_relative / component.name / "pubspec.yaml",
            role="provenance-metadata",
            origin=(
                f"Exact {component.name} package pubspec from the locally installed "
                f"Flutter {version} stable SDK, framework revision {framework_revision}"
            ),
            evidence_root=evidence_root,
        )
        records.append(
            {
                "ecosystem": component.ecosystem,
                "name": component.name,
                "version": component.version,
                "expected_source_type": component.source_type,
                "source_reference": component.source_reference,
                "package_url": component.package_url,
                "lock_digest": None,
                "license_expression": "NOASSERTION",
                "license_status": "resolved-exact-license-text",
                "notice_disposition": (
                    "include the exact carried license text from the pinned Flutter "
                    "SDK distribution"
                ),
                "provenance_notes": [
                    f"app/pubspec.yaml pins Flutter {version}; installed stable SDK "
                    f"metadata records framework revision {framework_revision}; the "
                    f"exact {component.name} pubspec and {license_scope} are carried."
                ],
                "evidence_files": [*package_shared, package_evidence],
            }
        )
    return records, shared_evidence, version


def _capture_sky_engine_license(flutter_sdk: Path, version: str, engine: str, evidence_root: Path) -> dict[str, str]:
    return _capture_file(
        flutter_sdk / "bin/cache/pkg/sky_engine/LICENSE",
        Path("flutter-sdk") / version / "sky_engine/LICENSE",
        role="license-text",
        origin=f"Exact sky_engine LICENSE from locally installed Flutter {version} stable SDK, engine revision {engine}",
        evidence_root=evidence_root,
    )


def _capture_path_dart_package(
    component: release_inventory.Component,
    package_root: Path,
    repo_root: Path,
    evidence_root: Path,
) -> dict[str, object]:
    resolved_package_root = package_root.resolve()
    try:
        package_source = resolved_package_root.relative_to(repo_root.resolve())
    except ValueError as error:
        raise ValueError(
            f"Path package is outside the tracked repository: {component.name}"
        ) from error
    pubspec = resolved_package_root / "pubspec.yaml"
    actual_name, actual_version = _read_pubspec_identity(pubspec)
    if (actual_name, actual_version) != (component.name, component.version):
        raise ValueError(
            f"Checked-in path package identity mismatch for "
            f"{component.name}@{component.version}"
        )
    license_path = resolved_package_root / "LICENSE"
    license_sha256 = _sha256(license_path.read_bytes())
    package_relative = (
        Path("dart-pub")
        / f"{_safe_name(component.name)}-{_safe_name(component.version)}"
    )
    origin = (
        f"Exact carried copy of {package_source.as_posix()} from the checked-in "
        "path package; files are hashed directly because path dependencies have "
        "no Pub archive checksum"
    )
    evidence_files = [
        _capture_file(
            pubspec,
            package_relative / "pubspec.yaml",
            role="provenance-metadata",
            origin=f"{origin}; package identity metadata",
            evidence_root=evidence_root,
        ),
        _capture_file(
            license_path,
            Path("texts") / f"{license_sha256}.txt",
            role="license-text",
            origin=f"{origin}; package LICENSE",
            evidence_root=evidence_root,
        ),
    ]
    patch_notes = resolved_package_root / "LITCH_PATCH.md"
    if patch_notes.is_file():
        evidence_files.append(
            _capture_file(
                patch_notes,
                package_relative / "LITCH_PATCH.md",
                role="provenance-metadata",
                origin=f"{origin}; local patch provenance",
                evidence_root=evidence_root,
            )
        )
    return {
        "ecosystem": component.ecosystem,
        "name": component.name,
        "version": component.version,
        "expected_source_type": component.source_type,
        "source_reference": component.source_reference,
        "package_url": component.package_url,
        "lock_digest": None,
        "license_expression": "NOASSERTION",
        "license_status": "resolved-exact-license-text",
        "notice_disposition": "include the exact checked-in package LICENSE",
        "provenance_notes": [
            f"Checked-in path package {component.name} {component.version}; "
            "the exact pubspec, license, and optional local patch notes are carried."
        ],
        "evidence_files": evidence_files,
    }


def _capture_dart(
    repo_root: Path, pub_cache: Path, evidence_root: Path
) -> tuple[
    list[dict[str, object]],
    dict[tuple[str, str], dict[str, object]],
    dict[tuple[str, str], Path],
]:
    records: list[dict[str, object]] = []
    by_package: dict[tuple[str, str], dict[str, object]] = {}
    roots_by_package: dict[tuple[str, str], Path] = {}
    lock_path = repo_root / "app/pubspec.lock"
    path_sources = _pubspec_lock_path_sources(lock_path)
    components = release_inventory.parse_pubspec_lock(
        lock_path, repo_root
    )
    for component in components:
        key = (component.name, component.version)
        if component.source_type == "path":
            relative_source = path_sources.get(component.name)
            if relative_source is None:
                raise ValueError(
                    f"Path source is missing from app/pubspec.lock for {component.name}"
                )
            package_dir = repo_root / "app" / relative_source
            record = _capture_path_dart_package(
                component, package_dir, repo_root, evidence_root
            )
            records.append(record)
            by_package[key] = record
            roots_by_package[key] = package_dir.resolve()
            continue
        if component.source_type != "hosted":
            continue
        package_dir = (
            pub_cache / "hosted" / "pub.dev" / f"{component.name}-{component.version}"
        )
        cache_hash_path = (
            pub_cache
            / "hosted-hashes"
            / "pub.dev"
            / f"{component.name}-{component.version}.sha256"
        )
        cache_hash = cache_hash_path.read_text(encoding="utf-8").strip()
        if cache_hash != component.lock_digest:
            raise ValueError(
                f"Pub cache hash does not match app/pubspec.lock for "
                f"{component.name}@{component.version}"
            )
        pubspec = package_dir / "pubspec.yaml"
        if _read_pubspec_identity(pubspec) != (component.name, component.version):
            raise ValueError(
                f"Cached pubspec identity mismatch for {component.name}@{component.version}"
            )
        license_path = package_dir / "LICENSE"
        license_bytes = license_path.read_bytes()
        license_sha256 = _sha256(license_bytes)
        license_relative = Path("texts") / f"{license_sha256}.txt"
        package_relative = (
            Path("dart-pub")
            / f"{_safe_name(component.name)}-{_safe_name(component.version)}"
        )
        origin_prefix = (
            f"Local Pub cache observation for {component.name} {component.version}; "
            f"app/pubspec.lock and Pub's separate hash ledger both record archive "
            f"SHA-256 {component.lock_digest}"
        )
        evidence_files = [
            _capture_file(
                pubspec,
                package_relative / "pubspec.yaml",
                role="provenance-metadata",
                origin=f"{origin_prefix}; observed pubspec.yaml",
                evidence_root=evidence_root,
            ),
            _capture_file(
                license_path,
                license_relative,
                role="license-text",
                origin=f"{origin_prefix}; observed top-level LICENSE",
                evidence_root=evidence_root,
            ),
        ]
        record: dict[str, object] = {
            "ecosystem": component.ecosystem,
            "name": component.name,
            "version": component.version,
            "expected_source_type": component.source_type,
            "source_reference": component.source_reference,
            "package_url": component.package_url,
            "lock_digest": {
                "algorithm": component.lock_digest_algorithm,
                "value": component.lock_digest,
            },
            "license_expression": "NOASSERTION",
            "license_status": "resolved-exact-license-text",
            "notice_disposition": (
                "include the exact carried top-level LICENSE observed in the matching "
                "name/version local Pub cache"
            ),
            "provenance_notes": [
                "The tracked hosted-package SHA-256 is the Pub archive checksum; the "
                "carried pubspec and LICENSE are separately hashed local-cache observations. "
                "The archive is not carried, so it does not cryptographically bind those files."
            ],
            "evidence_files": evidence_files,
        }
        records.append(record)
        by_package[key] = record
        roots_by_package[key] = package_dir
    return records, by_package, roots_by_package


def _podfile_cocoapods_version(path: Path) -> str:
    match = re.search(r"(?m)^COCOAPODS:\s*(\S+)$", path.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError("Podfile.lock has no CocoaPods version")
    return match.group(1)


def _pod_ipc_spec(pod: str, spec: Path, expected_version: str) -> bytes:
    actual_version = subprocess.run(
        [pod, "--version"], capture_output=True, check=True, timeout=30
    ).stdout.decode("utf-8").strip()
    if actual_version != expected_version:
        raise ValueError(
            f"CocoaPods version mismatch: expected {expected_version}, got {actual_version}"
        )
    return subprocess.run(
        [pod, "ipc", "spec", str(spec)],
        capture_output=True,
        check=True,
        timeout=30,
    ).stdout


def _installed_cocoapods_package(
    cocoapods_cache: Path,
    name: str,
    version: str,
    lock_digest: str | None,
) -> Path:
    if lock_digest is None:
        raise ValueError(f"CocoaPods package has no lock digest: {name}")
    candidates = sorted(
        (cocoapods_cache / name).glob(f"{version}-{lock_digest[:5]}*")
    )
    if len(candidates) != 1 or not candidates[0].is_dir():
        raise ValueError(
            f"Expected one spec-prefix-associated CocoaPods cache package for "
            f"{name}@{version}, found {len(candidates)}"
        )
    return candidates[0]


def _capture_flutter_pod(
    repo_root: Path,
    component: release_inventory.Component,
    pod: str,
    cocoapods_version: str,
    flutter_version: str,
    flutter_evidence: list[dict[str, str]],
    evidence_root: Path,
) -> dict[str, object]:
    spec_path = repo_root / "app/ios/Flutter/Flutter.podspec"
    spec_bytes = _pod_ipc_spec(pod, spec_path, cocoapods_version)
    if hashlib.sha1(spec_bytes).hexdigest() != component.lock_digest:
        raise ValueError("Normalized Flutter podspec checksum mismatch")
    spec_json = json.loads(spec_bytes)
    if (
        spec_json.get("name") != component.name
        or str(spec_json.get("version")) != component.version
        or spec_json.get("license", {}).get("type") != "BSD"
    ):
        raise ValueError("Normalized Flutter podspec identity/license mismatch")
    sdk_relative = Path("flutter-sdk") / flutter_version
    source_evidence = _capture_file(
        spec_path,
        sdk_relative / "Flutter.podspec.rb",
        role="provenance-metadata",
        origin=(
            "Observed local Flutter placeholder podspec source; its normalized "
            "form is checksum-matched to Podfile.lock"
        ),
        evidence_root=evidence_root,
    )
    normalized_path = sdk_relative / "Flutter.podspec.json"
    normalized_destination = evidence_root / normalized_path
    _write_exact(normalized_destination, spec_bytes)
    normalized_evidence = {
        "path": (
            release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE / normalized_path
        ).as_posix(),
        "sha256": _sha256(spec_bytes),
        "role": "provenance-metadata",
        "origin": (
            f"CocoaPods {cocoapods_version} normalized local Flutter placeholder "
            "podspec; SHA-1 matches Podfile.lock"
        ),
    }
    return {
        "ecosystem": component.ecosystem,
        "name": component.name,
        "version": component.version,
        "expected_source_type": component.source_type,
        "source_reference": component.source_reference,
        "package_url": component.package_url,
        "lock_digest": {
            "algorithm": component.lock_digest_algorithm,
            "value": component.lock_digest,
        },
        "license_expression": "NOASSERTION",
        "license_status": "resolved-exact-license-text",
        "notice_disposition": (
            "include the exact carried Flutter SDK license text for the "
            "checksum-bound generated podspec"
        ),
        "provenance_notes": [
            "The observed local Flutter placeholder podspec is checksum-matched "
            "to Podfile.lock. Independently captured metadata and license evidence "
            f"identify the installed Flutter {flutter_version} SDK; the placeholder "
            "podspec itself does not encode that SDK identity."
        ],
        "evidence_files": [source_evidence, normalized_evidence, *flutter_evidence],
    }


def _require_component_coverage(
    ecosystem: str,
    components: list[release_inventory.Component],
    records: list[dict[str, object]],
    *,
    allowed_unresolved: set[tuple[str, str]],
) -> None:
    expected = {
        (component.name, component.version)
        for component in components
        if component.ecosystem == ecosystem
    } - allowed_unresolved
    actual = {
        (str(record["name"]), str(record["version"]))
        for record in records
        if record["ecosystem"] == ecosystem
    }
    if expected != actual:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError(
            f"{ecosystem} capture coverage mismatch; missing={missing}, extra={extra}"
        )


def _require_license_evidence_matches(
    declared_license: Path,
    license_evidence: list[dict[str, object]],
    package_name: str,
) -> None:
    if (
        not license_evidence
        or _sha256(declared_license.read_bytes())
        != license_evidence[0]["sha256"]
    ):
        raise ValueError(
            f"Podspec license file does not match carried Dart evidence for "
            f"{package_name}"
        )


def _capture_cocoapods(
    repo_root: Path,
    cocoapods_trunk: Path,
    cocoapods_cache: Path,
    pod: str,
    evidence_root: Path,
    dart_records: dict[tuple[str, str], dict[str, object]],
    dart_package_roots: dict[tuple[str, str], Path],
    flutter_evidence: list[dict[str, str]],
    flutter_version: str,
) -> list[dict[str, object]]:
    parsed_components = release_inventory.parse_podfile_lock(
        repo_root / "app/ios/Podfile.lock", repo_root
    )
    components = {
        (component.name, component.version): component
        for component in parsed_components
    }
    cocoapods_version = _podfile_cocoapods_version(
        repo_root / "app/ios/Podfile.lock"
    )
    records: list[dict[str, object]] = [
        _capture_flutter_pod(
            repo_root,
            components[("Flutter", "1.0.0")],
            pod,
            cocoapods_version,
            flutter_version,
            flutter_evidence,
            evidence_root,
        )
    ]
    for pod_name, pod_version, pub_name, pub_version, spec_relative in COCOAPODS_PATH_PODS:
        component = components[(pod_name, pod_version)]
        dart_key = (pub_name, pub_version)
        spec_path = dart_package_roots[dart_key] / spec_relative
        dart_record = dart_records[dart_key]
        is_checked_in_path = dart_record["expected_source_type"] == "path"
        dart_source_description = (
            f"checked-in Dart path package {pub_name} {pub_version}"
            if is_checked_in_path
            else f"local Pub cache {pub_name} {pub_version}"
        )
        spec_bytes = _pod_ipc_spec(pod, spec_path, cocoapods_version)
        if hashlib.sha1(spec_bytes).hexdigest() != component.lock_digest:
            raise ValueError(f"Normalized podspec checksum mismatch for {pod_name}")
        spec_json = json.loads(spec_bytes)
        if spec_json.get("name") != pod_name or str(spec_json.get("version")) != pod_version:
            raise ValueError(f"Normalized podspec identity mismatch for {pod_name}")
        license_declaration = spec_json.get("license")
        if (
            not isinstance(license_declaration, dict)
            or license_declaration.get("file") != "../LICENSE"
        ):
            raise ValueError(
                f"Normalized podspec does not reference ../LICENSE for {pod_name}"
            )
        destination_relative = (
            Path("cocoapods")
            / f"{_safe_name(pod_name)}-{_safe_name(pod_version)}"
            / "podspec.json"
        )
        podspec_evidence = _capture_file(
            spec_path,
            destination_relative.with_name("podspec-source.rb"),
            role="provenance-metadata",
            origin=(
                f"Podspec source observed in {dart_source_description}; "
                f"CocoaPods {cocoapods_version} normalized SHA-1 "
                f"matched app/ios/Podfile.lock and declared ../LICENSE"
            ),
            evidence_root=evidence_root,
        )
        normalized_destination = evidence_root / destination_relative
        _write_exact(normalized_destination, spec_bytes)
        normalized_evidence = {
            "path": (
                release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE
                / destination_relative
            ).as_posix(),
            "sha256": _sha256(spec_bytes),
            "role": "provenance-metadata",
            "origin": (
                f"CocoaPods {cocoapods_version} normalized exact {pod_name} "
                f"podspec; SHA-1 matched app/ios/Podfile.lock"
            ),
        }
        license_evidence = [
            dict(evidence)
            for evidence in dart_record["evidence_files"]
            if evidence["role"] == "license-text"
        ]
        declared_license = (spec_path.parent / "../LICENSE").resolve()
        _require_license_evidence_matches(
            declared_license,
            license_evidence,
            pod_name,
        )
        records.append(
            {
                "ecosystem": component.ecosystem,
                "name": component.name,
                "version": component.version,
                "expected_source_type": component.source_type,
                "source_reference": component.source_reference,
                "package_url": component.package_url,
                "lock_digest": {
                    "algorithm": component.lock_digest_algorithm,
                    "value": component.lock_digest,
                },
                "license_expression": "NOASSERTION",
                "license_status": "resolved-exact-license-text",
                "notice_disposition": (
                    "include the exact carried ../LICENSE text observed beside this "
                    "checksum-bound path-pod spec"
                ),
                "provenance_notes": [
                    f"Pod version {pod_version} is supplied by locked Dart package "
                    f"{pub_name} {pub_version}; normalized podspec SHA-1 matches "
                    "Podfile.lock and points to ../LICENSE. "
                    + (
                        "The referenced license is carried directly from the checked-in "
                        "path package."
                        if is_checked_in_path
                        else "The license bytes are a separately hashed local-cache "
                        "observation because the Pub archive is not carried."
                    )
                ],
                "evidence_files": [
                    podspec_evidence,
                    normalized_evidence,
                    *license_evidence,
                ],
            }
        )

    protobuf = components[("Protobuf", "3.29.6")]
    protobuf_spec = (
        cocoapods_trunk
        / "Specs/e/c/d/Protobuf/3.29.6/Protobuf.podspec.json"
    )
    protobuf_bytes = protobuf_spec.read_bytes()
    if hashlib.sha1(protobuf_bytes).hexdigest() != protobuf.lock_digest:
        raise ValueError("Protobuf podspec checksum does not match Podfile.lock")
    protobuf_payload = json.loads(protobuf_bytes)
    protobuf_source = {
        "git": "https://github.com/protocolbuffers/protobuf.git",
        "tag": "v3.29.6-objectivec",
    }
    if (
        protobuf_payload.get("name") != protobuf.name
        or protobuf_payload.get("version") != protobuf.version
        or protobuf_payload.get("license") != "BSD-3-Clause"
        or protobuf_payload.get("source") != protobuf_source
    ):
        raise ValueError(
            "Protobuf podspec identity, source, or license no longer matches evidence"
        )
    protobuf_destination = Path("cocoapods/Protobuf-3.29.6/podspec.json")
    protobuf_package = _installed_cocoapods_package(
        cocoapods_cache,
        protobuf.name,
        protobuf.version,
        protobuf.lock_digest,
    )
    protobuf_license = protobuf_package / "LICENSE"
    protobuf_evidence = [
        _capture_file(
            protobuf_spec,
            protobuf_destination,
            role="provenance-metadata",
            origin=(
                "Exact CocoaPods trunk Protobuf 3.29.6 spec; raw SHA-1 "
                "matches app/ios/Podfile.lock"
            ),
            evidence_root=evidence_root,
        ),
        _capture_file(
            protobuf_license,
            protobuf_destination.with_name("LICENSE"),
            role="license-text",
            origin=(
                "Separately hashed LICENSE observation from the local CocoaPods "
                "Protobuf 3.29.6 cache directory associated with the locked spec"
            ),
            evidence_root=evidence_root,
        ),
    ]
    records.append(
        {
            "ecosystem": protobuf.ecosystem,
            "name": protobuf.name,
            "version": protobuf.version,
            "expected_source_type": protobuf.source_type,
            "source_reference": protobuf.source_reference,
            "package_url": protobuf.package_url,
            "lock_digest": {
                "algorithm": protobuf.lock_digest_algorithm,
                "value": protobuf.lock_digest,
            },
            "license_expression": "BSD-3-Clause",
            "license_status": "resolved-declared-spdx-and-carried-text",
            "license_declaration": {
                "path": (
                    release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE
                    / protobuf_destination
                ).as_posix(),
                "format": "json",
                "field": "license",
            },
            "notice_disposition": (
                "include the exact carried BSD-3-Clause license text from the "
                "observed local package cache"
            ),
            "provenance_notes": [
                "The carried CocoaPods trunk spec bytes have the exact SHA-1 recorded "
                "by Podfile.lock, directly declare BSD-3-Clause, and identify source "
                f"{protobuf_source['git']} tag {protobuf_source['tag']}; the installed "
                "cache contains the separately hashed carried LICENSE. The cache "
                "directory name is associated with the spec checksum but no retained "
                "archive or tree digest cryptographically binds those source bytes."
            ],
            "evidence_files": protobuf_evidence,
        }
    )

    swift = components[("SwiftProtobuf", "1.35.1")]
    swift_specs = sorted(
        cocoapods_trunk.glob(
            "Specs/*/*/*/SwiftProtobuf/1.35.1/SwiftProtobuf.podspec.json"
        )
    )
    if len(swift_specs) != 1:
        raise ValueError(
            f"Expected one SwiftProtobuf trunk spec, found {len(swift_specs)}"
        )
    swift_spec = swift_specs[0]
    swift_bytes = swift_spec.read_bytes()
    if hashlib.sha1(swift_bytes).hexdigest() != swift.lock_digest:
        raise ValueError("SwiftProtobuf podspec checksum does not match Podfile.lock")
    swift_payload = json.loads(swift_bytes)
    if (
        swift_payload.get("name") != swift.name
        or swift_payload.get("version") != swift.version
        or swift_payload.get("license")
        != {"type": "Apache 2.0", "file": "LICENSE.txt"}
        or swift_payload.get("source")
        != {
            "git": "https://github.com/apple/swift-protobuf.git",
            "tag": "1.35.1",
        }
    ):
        raise ValueError(
            "SwiftProtobuf podspec identity, source, or license no longer matches evidence"
        )
    swift_package = _installed_cocoapods_package(
        cocoapods_cache,
        swift.name,
        swift.version,
        swift.lock_digest,
    )
    swift_license = swift_package / "LICENSE.txt"
    swift_license_text = swift_license.read_text(encoding="utf-8")
    if "Apache License" not in swift_license_text or "Runtime Library Exception" not in swift_license_text:
        raise ValueError("SwiftProtobuf LICENSE.txt no longer has the reviewed terms")
    swift_destination = Path("cocoapods/SwiftProtobuf-1.35.1")
    records.append(
        {
            "ecosystem": swift.ecosystem,
            "name": swift.name,
            "version": swift.version,
            "expected_source_type": swift.source_type,
            "source_reference": swift.source_reference,
            "package_url": swift.package_url,
            "lock_digest": {
                "algorithm": swift.lock_digest_algorithm,
                "value": swift.lock_digest,
            },
            "license_expression": "NOASSERTION",
            "license_status": "resolved-exact-license-text",
            "notice_disposition": (
                "include the exact carried Apache 2.0 text and Runtime Library "
                "Exception without guessing an SPDX expression"
            ),
            "provenance_notes": [
                "The checksum-bound podspec identifies the SwiftProtobuf 1.35.1 "
                "Git tag and LICENSE.txt; the local cache observation includes the "
                "Runtime Library Exception and is carried without normalization. "
                "The cache directory name is associated with the spec checksum but "
                "no retained archive or tree digest cryptographically binds those "
                "source bytes."
            ],
            "evidence_files": [
                _capture_file(
                    swift_spec,
                    swift_destination / "podspec.json",
                    role="provenance-metadata",
                    origin=(
                        "Exact CocoaPods trunk SwiftProtobuf 1.35.1 spec; raw "
                        "SHA-1 matches app/ios/Podfile.lock"
                    ),
                    evidence_root=evidence_root,
                ),
                _capture_file(
                    swift_license,
                    swift_destination / "LICENSE.txt",
                    role="license-text",
                    origin=(
                        "Separately hashed LICENSE.txt observation from the local "
                        "CocoaPods SwiftProtobuf 1.35.1 cache directory associated "
                        "with the locked spec"
                    ),
                    evidence_root=evidence_root,
                ),
            ],
        }
    )
    _require_component_coverage(
        "cocoapods", parsed_components, records, allowed_unresolved=set()
    )
    return records


def _capture_platformio(
    repo_root: Path,
    platformio_home: Path,
    platformio_libdeps_root: Path,
    evidence_root: Path,
) -> list[dict[str, object]]:
    components = {
        (component.ecosystem, component.name, component.version): component
        for component in release_inventory.parse_platformio(
            repo_root / "platformio.ini", repo_root
        )
    }
    records: list[dict[str, object]] = []
    for package in PLATFORMIO_PACKAGES:
        key = (package["ecosystem"], package["name"], package["version"])
        component = components[key]
        package_root = (
            platformio_home / "platforms" / package["directory"]
            if package["ecosystem"] == "platformio-platform"
            else platformio_libdeps_root / package["directory"]
        )
        piopm = json.loads((package_root / ".piopm").read_text(encoding="utf-8"))
        if (
            piopm.get("type")
            != ("platform" if package["ecosystem"] == "platformio-platform" else "library")
            or piopm.get("name") != package["directory"]
            or piopm.get("version") != package["version"]
            or piopm.get("spec", {}).get("owner") != package["owner"]
        ):
            raise ValueError(
                f"PlatformIO package identity mismatch for {package['name']}"
            )
        if package["ecosystem"] == "platformio-platform":
            platform_metadata = json.loads(
                (package_root / "platform.json").read_text(encoding="utf-8")
            )
            if (
                platform_metadata.get("name") != package["directory"]
                or platform_metadata.get("version") != package["version"]
                or platform_metadata.get("license") != package["license_expression"]
                or platform_metadata.get("repository", {}).get("url")
                != package["repository_url"]
            ):
                raise ValueError(f"{package['name']} metadata no longer matches evidence")
        elif package["name"] == "bblanchon/ArduinoJson":
            properties = {
                key: value
                for key, value in (
                    line.split("=", 1)
                    for line in (package_root / "library.properties")
                    .read_text(encoding="utf-8")
                    .splitlines()
                    if "=" in line
                )
            }
            library_metadata = json.loads(
                (package_root / "library.json").read_text(encoding="utf-8")
            )
            if (
                properties.get("version") != package["version"]
                or properties.get("license") != package["license_expression"]
                or library_metadata.get("name") != "ArduinoJson"
                or library_metadata.get("version") != package["version"]
                or library_metadata.get("repository", {}).get("url")
                != package["repository_url"]
            ):
                raise ValueError("ArduinoJson package metadata no longer matches evidence")
        elif package["name"] == "makuna/NeoPixelBus":
            library_metadata = json.loads(
                (package_root / "library.json").read_text(encoding="utf-8")
            )
            if (
                library_metadata.get("name") != "NeoPixelBus"
                or library_metadata.get("version") != package["version"]
                or library_metadata.get("repository")
                != {
                    "type": "git",
                    "url": package["repository_url"],
                }
            ):
                raise ValueError("NeoPixelBus package metadata no longer matches evidence")
        elif package["name"] == "h2zero/NimBLE-Arduino":
            properties = {
                key: value
                for key, value in (
                    line.split("=", 1)
                    for line in (package_root / "library.properties")
                    .read_text(encoding="utf-8")
                    .splitlines()
                    if "=" in line
                )
            }
            if (
                properties.get("name") != "NimBLE-Arduino"
                or properties.get("version") != package["version"]
                or properties.get("url") != package["repository_url"]
            ):
                raise ValueError("NimBLE package metadata no longer matches evidence")
        package_relative = (
            Path(package["ecosystem"])
            / f"{_safe_name(package['name'])}-{_safe_name(package['version'])}"
        )
        origin = (
            f"Exact local PlatformIO package {package['name']} {package['version']} "
            "selected by tracked platformio.ini; .piopm owner/name/version checked"
        )
        evidence_files = [
            _capture_file(
                package_root / source_relative,
                package_relative / destination_relative,
                role=role,
                origin=f"{origin}; extracted {source_relative}",
                evidence_root=evidence_root,
            )
            for source_relative, destination_relative, role in package["files"]
        ]
        record = {
            "ecosystem": component.ecosystem,
            "name": component.name,
            "version": component.version,
            "expected_source_type": component.source_type,
            "source_reference": component.source_reference,
            "package_url": component.package_url,
            "lock_digest": None,
            "license_expression": package["license_expression"],
            "license_status": (
                "resolved-declared-spdx-and-carried-text"
                if package["license_expression"] != "NOASSERTION"
                else "resolved-exact-license-text"
            ),
            "notice_disposition": (
                "include every carried license and notice text listed for this "
                "exact PlatformIO package"
            ),
            "provenance_notes": [
                "Only exact platform/library pins are resolved. PlatformIO cache "
                "observations for unpinned framework-provided components are excluded.",
                "The carried metadata, license, and notice files are separately hashed "
                "local PlatformIO cache observations. No package archive or tree digest "
                "is carried to prove their upstream package contents.",
            ],
            "evidence_files": evidence_files,
        }
        if package["license_expression"] != "NOASSERTION":
            declaration_filename = (
                "platform.json"
                if package["ecosystem"] == "platformio-platform"
                else "library.properties"
            )
            record["license_declaration"] = {
                "path": next(
                    evidence["path"]
                    for evidence in evidence_files
                    if Path(evidence["path"]).name == declaration_filename
                ),
                "format": (
                    "json"
                    if declaration_filename.endswith(".json")
                    else "properties"
                ),
                "field": "license",
            }
        records.append(record)
    return records


def _gradle_cached_file(
    cache: Path, coordinate: tuple[str, str, str], filename: str, sha256: str | None = None,
) -> Path:
    candidates = []
    for path in cache.joinpath(*coordinate).glob(f"*/{filename}"):
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256" if sha256 else "sha1").hexdigest()
        # Gradle's SHA-1 cache directory omits leading zeroes.
        if digest == (sha256 or path.parent.name.zfill(40)):
            candidates.append(path)
    if len(candidates) != 1:
        raise ValueError(f"Expected one matching Gradle cache file for {':'.join(coordinate)} {filename}")
    return candidates[0]


def _capture_maven_archive_terms(path: Path, evidence_root: Path) -> list[dict[str, str]]:
    evidence = []

    def collect(archive: zipfile.ZipFile, prefix: str) -> None:
        for member in sorted(archive.namelist()):
            basename = member.rsplit("/", 1)[-1]
            match = re.fullmatch(r"(LICENSE|NOTICE|COPYING)(?:\.(?:TXT|MD))?", basename, flags=re.I)
            if match:
                data = archive.read(member)
                data.decode("utf-8")
                role = "notice-text" if match[1].upper() == "NOTICE" else "license-text"
                evidence.append(_capture_bytes(
                    data, Path("texts") / f"{_sha256(data)}.txt", role,
                    f"Exact member {prefix}{member} of SHA-256-matched Gradle runtime artifact {path.name}",
                    evidence_root,
                ))

    with zipfile.ZipFile(path) as archive:
        collect(archive, "")
        if path.suffix == ".aar":
            for member in sorted(name for name in archive.namelist() if name.endswith(".jar")):
                with zipfile.ZipFile(io.BytesIO(archive.read(member))) as nested:
                    collect(nested, f"{member}!/")
    return evidence


def _capture_flutter_maven_terms(
    component: release_inventory.Component, cache: Path, flutter_sdk: Path, evidence_root: Path,
) -> list[dict[str, str]]:
    metadata = json.loads((flutter_sdk / "bin/cache/flutter.version.json").read_text(encoding="utf-8"))
    version = metadata["frameworkVersion"]
    engine = metadata["engineRevision"]
    group, name = component.name.split(":")
    sdk_artifacts = {
        "arm64_v8a_release": ("android-arm64-release", "lib/arm64-v8a/libflutter.so"),
        "armeabi_v7a_release": ("android-arm-release", "lib/armeabi-v7a/libflutter.so"),
        "x86_64_release": ("android-x64-release", "lib/x86_64/libflutter.so"),
        "flutter_embedding_release": ("android-arm64-release", None),
    }
    if component.version != f"1.0.0-{engine}" or name not in sdk_artifacts or len(component.runtime_artifacts) != 1:
        raise ValueError(f"Flutter Maven runtime does not match installed engine for {component.name}")
    sdk_directory, native_member = sdk_artifacts[name]
    sdk_release = flutter_sdk / "bin/cache/artifacts/engine" / sdk_directory
    filename, digest = component.runtime_artifacts[0]
    runtime_path = _gradle_cached_file(cache, (group, name, component.version), filename, digest)

    with zipfile.ZipFile(sdk_release / "flutter.jar") as sdk, zipfile.ZipFile(runtime_path) as maven:
        sdk_files = {info.filename for info in sdk.infolist() if not info.is_dir()}
        maven_files = {info.filename for info in maven.infolist() if not info.is_dir()}
        if native_member is not None:
            expected = {native_member}
        else:
            expected = {entry for entry in sdk_files if not entry.endswith("/libflutter.so")}
            maven_files.discard("META-INF/MANIFEST.MF")
            maven_files = {
                entry for entry in maven_files
                if not (re.fullmatch(r"tmp[\w]+/\.empty", entry) and maven.getinfo(entry).file_size == 0)
            }
        if expected != maven_files or not expected.issubset(sdk_files):
            raise ValueError(f"Flutter SDK/Maven archive member mismatch for {component.name}")
        for entry in sorted(expected):
            with sdk.open(entry) as sdk_file, maven.open(entry) as maven_file:
                if hashlib.file_digest(sdk_file, "sha256").digest() != hashlib.file_digest(maven_file, "sha256").digest():
                    raise ValueError(f"Flutter SDK/Maven archive content mismatch for {component.name} {entry}")

    pointer = sdk_release / "LICENSE.flutter_jar_zip.md"
    pointer_bytes = pointer.read_bytes()
    if f"/flutter/{engine}/sky_engine.zip".encode() not in pointer_bytes:
        raise ValueError(f"Flutter SDK Android license pointer does not identify engine {engine}")
    return [
        _capture_bytes(
            pointer_bytes, Path("flutter-sdk") / version / "android-release/LICENSE.flutter_jar_zip.md",
            "provenance-metadata", f"Exact Flutter {version} Android release SDK license pointer for engine {engine}",
            evidence_root,
        ),
        _capture_sky_engine_license(flutter_sdk, version, engine, evidence_root),
    ]


def _capture_maven(
    components: list[release_inventory.Component], cache: Path, flutter_sdk: Path, evidence_root: Path,
    reviewed_root: Path,
) -> list[dict[str, object]]:
    def pom_for(coordinate: tuple[str, str, str]) -> tuple[release_inventory.MavenPom, dict[str, str]]:
        _, name, version = coordinate
        filename = f"{name}-{version}.pom"
        path = _gradle_cached_file(cache, coordinate, filename)
        data = path.read_bytes()
        pom = release_inventory.parse_maven_pom(data, filename)
        if pom.coordinate != coordinate:
            raise ValueError(f"Gradle cache POM identity mismatch for {':'.join(coordinate)}")
        evidence = _capture_bytes(
            data, Path("maven").joinpath(*coordinate, filename), "provenance-metadata",
            f"Exact published Maven POM for {':'.join(coordinate)} from the checksum-identified Gradle cache",
            evidence_root,
        )
        return pom, evidence

    records = []
    for component in components:
        group, name = component.name.split(":")
        coordinate = (group, name, component.version)
        pom, pom_evidence = pom_for(coordinate)
        evidence = [pom_evidence]
        if group == "io.flutter":
            evidence.extend(_capture_flutter_maven_terms(component, cache, flutter_sdk, evidence_root))
            records.append({
                "ecosystem": component.ecosystem,
                "name": component.name,
                "version": component.version,
                "expected_source_type": component.source_type,
                "source_reference": component.source_reference,
                "package_url": component.package_url,
                "lock_digest": None,
                "runtime_artifacts": dict(component.runtime_artifacts),
                "license_expression": "NOASSERTION",
                "license_status": "resolved-exact-license-text",
                "notice_disposition": "carry the complete mixed Flutter engine license collection and its attributions",
                "provenance_notes": [
                    "The SHA-256-matched Maven runtime payload matches the installed Flutter SDK Android release JAR member by member. "
                    "For the Java embedding, the archive manifest and empty packaging placeholder are outside that payload comparison.",
                    "The SDK Android license pointer names the engine revision and directs consumers to the carried sky_engine license collection.",
                ],
                "evidence_files": evidence,
            })
            continue
        declaration = {"path": pom_evidence["path"], "format": "maven-pom", "field": "licenses"}
        expression = pom.license_expression
        if expression is None and pom.parent is not None:
            parent, parent_evidence = pom_for(pom.parent)
            expression = parent.license_expression
            evidence.append(parent_evidence)
            declaration["parent_path"] = parent_evidence["path"]
        if expression is None:
            raise ValueError(f"No reviewed Maven POM license for {':'.join(coordinate)}")

        for filename, digest in component.runtime_artifacts:
            artifact = _gradle_cached_file(cache, coordinate, filename, digest)
            evidence.extend(_capture_maven_archive_terms(artifact, evidence_root))
        evidence.extend(_capture_reviewed_maven_terms(coordinate, expression, reviewed_root, evidence_root))
        has_license = any(item["role"] == "license-text" for item in evidence)
        notes = [
            "The exact-coordinate POM declares the package license directly or via the captured parent."
        ]
        if component.runtime_artifacts:
            notes.append(
                "Selected AAR or JAR bytes were matched to the Gradle runtime snapshot by SHA-256. "
                "The separate POM declares the package license. Shaded code and upstream notices require their own review."
            )
        records.append({
            "ecosystem": component.ecosystem,
            "name": component.name,
            "version": component.version,
            "expected_source_type": component.source_type,
            "source_reference": component.source_reference,
            "package_url": component.package_url,
            "lock_digest": None,
            "runtime_artifacts": dict(component.runtime_artifacts),
            "license_expression": expression,
            "license_status": "resolved-declared-spdx-and-carried-text" if has_license else "resolved-declared-spdx",
            "license_declaration": declaration,
            "notice_disposition": "carry applicable license texts and any upstream notices in the distribution",
            "provenance_notes": notes,
            "evidence_files": evidence,
        })
    return records


def _capture_reviewed_maven_terms(
    coordinate: tuple[str, str, str], expression: str, reviewed_root: Path, evidence_root: Path,
) -> list[dict[str, str]]:
    files: list[tuple[str, str, str]] = []
    if expression == "Apache-2.0":
        files.append(("Apache-2.0.txt", "license-text", "Canonical Apache License 2.0 text named by the POM at https://www.apache.org/licenses/LICENSE-2.0.txt"))
    elif expression == "CC0-1.0":
        files.append(("CC0-1.0.txt", "license-text", "Creative Commons CC0 1.0 legal code named by the POM at https://creativecommons.org/publicdomain/zero/1.0/legalcode.txt"))

    if coordinate == ("com.google.protobuf", "protobuf-javalite", "3.25.3"):
        files.append(("protobuf-LICENSE.txt", "license-text", "Exact Protobuf v25.3 LICENSE at https://github.com/protocolbuffers/protobuf/blob/v25.3/LICENSE"))
    elif coordinate == ("androidx.datastore", "datastore-preferences-external-protobuf", "1.1.7"):
        files.extend([
            ("protobuf-LICENSE.txt", "license-text", "Exact Protobuf v28.2 LICENSE at https://github.com/protocolbuffers/protobuf/blob/v28.2/LICENSE, byte-identical to v25.3"),
            ("datastore-shadow-NOTICE.txt", "notice-text", "Reviewed attribution for the relocated Protobuf 4.28.2 payload, identified by the AndroidX DataStore 1.1.7 build and matching relocated class inventory"),
        ])
    elif coordinate == ("com.polidea.rxandroidble2", "dagger-library-shadow", "1.16.0"):
        files.extend([
            ("dagger-2.42-LICENSE.txt", "license-text", "Exact Dagger 2.42 LICENSE.txt at https://github.com/google/dagger/blob/dagger-2.42/LICENSE.txt"),
            ("dagger-shadow-NOTICE.txt", "notice-text", "Reviewed attribution from the exact Dagger 2.42 and javax.inject 1 source distributions identified by the RxAndroidBle release build and matching relocated class inventory"),
        ])
    return [
        _capture_file(
            reviewed_root / relative, Path("maven/terms") / relative,
            role=role, origin=origin, evidence_root=evidence_root,
        )
        for relative, role, origin in files
    ]


def capture(args: argparse.Namespace) -> None:
    repo_root = Path(args.repo_root).resolve()
    evidence_root = Path(args.output_dir).resolve()
    manifest_path = Path(args.manifest).resolve()
    if evidence_root.exists() or manifest_path.exists():
        raise ValueError(
            "Refusing to overwrite tracked evidence; capture into a fresh path for review"
        )
    _write_exact(evidence_root / ".gitattributes", b"** -text -whitespace\n")
    dart_records, dart_by_package, dart_package_roots = _capture_dart(
        repo_root, Path(args.pub_cache).resolve(), evidence_root
    )
    flutter_records, flutter_evidence, flutter_version = _capture_flutter_sdk(
        repo_root,
        Path(args.flutter_sdk).resolve(),
        evidence_root,
    )
    dart_records.extend(flutter_records)
    dart_components = release_inventory.parse_pubspec_lock(
        repo_root / "app/pubspec.lock", repo_root
    )
    _require_component_coverage(
        "dart-pub",
        dart_components,
        dart_records,
        allowed_unresolved=set(),
    )
    records = dart_records
    records.extend(
        _capture_cocoapods(
            repo_root,
            Path(args.cocoapods_trunk).resolve(),
            Path(args.cocoapods_cache).resolve(),
            args.pod,
            evidence_root,
            dart_by_package,
            dart_package_roots,
            flutter_evidence,
            flutter_version,
        )
    )
    records.extend(
        _capture_platformio(
            repo_root,
            Path(args.platformio_home).resolve(),
            Path(args.platformio_libdeps_root).resolve(),
            evidence_root,
        )
    )
    records.extend(_capture_maven(
        release_inventory.parse_android_runtime_snapshot(repo_root), Path(args.gradle_cache).resolve(),
        Path(args.flutter_sdk).resolve(), evidence_root,
        repo_root / release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE / "maven/terms",
    ))
    records.sort(
        key=lambda record: (
            record["ecosystem"],
            str(record["name"]).lower(),
            record["version"],
        )
    )
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(
            {
                "schema": release_inventory.DEPENDENCY_EVIDENCE_SCHEMA,
                "records": records,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=str(release_inventory.REPO_ROOT))
    parser.add_argument("--pub-cache", required=True)
    parser.add_argument("--flutter-sdk", required=True)
    parser.add_argument("--cocoapods-trunk", required=True)
    parser.add_argument("--cocoapods-cache", required=True)
    parser.add_argument("--pod", default="pod")
    parser.add_argument("--platformio-home", required=True)
    parser.add_argument("--platformio-libdeps-root", required=True)
    parser.add_argument("--gradle-cache", required=True, help="Gradle modules-2/files-2.1 cache directory")
    parser.add_argument(
        "--output-dir",
        default=str(
            release_inventory.REPO_ROOT
            / release_inventory.LICENSE_EVIDENCE_ROOT_RELATIVE
        ),
    )
    parser.add_argument(
        "--manifest",
        default=str(
            release_inventory.REPO_ROOT
            / release_inventory.DEPENDENCY_EVIDENCE_RELATIVE
        ),
    )
    args = parser.parse_args()
    try:
        capture(args)
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        print(f"dependency evidence capture failed: {error}", file=sys.stderr)
        return 1
    print(
        "dependency evidence capture: OK "
        f"({args.manifest}, {args.output_dir})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
