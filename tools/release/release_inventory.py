#!/usr/bin/env python3
"""Generate deterministic public-release inventory and notice artifacts."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import importlib.util
import json
import re
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path
from urllib.parse import quote
from xml.etree import ElementTree


TOOL_VERSION = "1.9.0"
INVENTORY_SCHEMA = "litch-release-inventory-v1"
SBOM_SCHEMA = "litch-release-sbom-v1"
DEPENDENCY_EVIDENCE_SCHEMA = "litch-dependency-license-evidence-v1"
DEPENDENCY_EVIDENCE_RELATIVE = Path(
    "tools/release/dependency_license_evidence.json"
)
LICENSE_EVIDENCE_ROOT_RELATIVE = Path("tools/release/license_evidence")
ANDROID_RUNTIME_RELATIVE = Path("tools/release/android_runtime_inventory.json")
APP_NATIVE_NOTICES_RELATIVE = Path("app/assets/licenses/native_runtime_notices.txt")
FIRMWARE_LICENSE_MANIFEST_RELATIVE = Path("tools/release/firmware_license_manifest.json")
LITCH_MAX_RELATIVE = Path("lib/litch-max")
ROOT_LICENSE_RELATIVE = Path("LICENSE")
ROOT_LICENSE_EXPRESSION = "MPL-2.0"
ROOT_LICENSE_SHA256 = "56960b346a7d727b846e9cd8e0234fbf18ab442c4e5432ced60f5d3517d5e348"
HARDWARE_LICENSE_RELATIVE = Path("LICENSE-HARDWARE")
HARDWARE_LICENSE_EXPRESSION = "CERN-OHL-S-2.0"
HARDWARE_LICENSE_SHA256 = "6dd37fc3173b383f4b6faed21ca7613b3ceb94df71a76cf50d7f934c2a03854e"
DOCUMENTATION_LICENSE_RELATIVE = Path("LICENSE-DOCUMENTATION")
DOCUMENTATION_LICENSE_EXPRESSION = "CC-BY-SA-4.0"
DOCUMENTATION_LICENSE_SHA256 = "c8d5b2e0183cbe206426174d8e905281324fbf51ecc2ecec22b0aa3bb8694452"
LITCH_SOURCE_LOCATION = "https://github.com/foxieproducts/litch"
ANDROID_RUNTIME_CONFIGURATION = ":app:releaseRuntimeClasspath"
ANDROID_RUNTIME_PUB_OVERRIDES = "app/pubspec_overrides.yaml"
ANDROID_RUNTIME_INPUTS = (
    "app/android/app/build.gradle.kts",
    "app/android/build.gradle.kts",
    "app/android/gradle.properties",
    "app/android/gradle/wrapper/gradle-wrapper.properties",
    "app/android/settings.gradle.kts",
    "app/pubspec.lock",
    "app/pubspec.yaml",
    "app/third_party/flutter_reactive_ble/pubspec.yaml",
    "app/third_party/reactive_ble_mobile/android/build.gradle",
    "app/third_party/reactive_ble_mobile/pubspec.yaml",
)
REPO_ROOT = Path(__file__).resolve().parents[2].resolve()
SUPPORTED_DECLARED_SPDX_EXPRESSIONS = frozenset(
    {"Apache-2.0", "BSD-3-Clause", "CC0-1.0", "MIT"}
)


@dataclass(frozen=True)
class EvidenceFile:
    path: str
    sha256: str
    role: str
    origin: str
    text: str | None = field(default=None, repr=False)


@dataclass(frozen=True)
class Component:
    ecosystem: str
    name: str
    version: str
    relationship: str
    source_type: str
    source_reference: str
    evidence_paths: tuple[str, ...]
    license_expression: str
    license_status: str
    notice_disposition: str
    provenance_notes: tuple[str, ...] = ()
    package_url: str | None = None
    notice_text: str | None = None
    notice_text_source: str | None = None
    lock_digest_algorithm: str | None = None
    lock_digest: str | None = None
    license_evidence: tuple[EvidenceFile, ...] = ()
    runtime_artifacts: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class Inventory:
    tool_version: str
    inventory_schema: str
    sbom_schema: str
    components: tuple[Component, ...]
    unresolved_questions: tuple[str, ...] = field(default_factory=tuple)
    board_notices: tuple[tuple[str, str], ...] = ()
    project_license: EvidenceFile | None = None
    hardware_license: EvidenceFile | None = None
    documentation_license: EvidenceFile | None = None


def repo_relative(path: Path, repo_root: Path) -> str:
    resolved_path = path.resolve()
    return resolved_path.relative_to(repo_root.resolve()).as_posix()


def _read_vendor_records(repo_root: Path) -> dict[str, dict[str, object]]:
    path = repo_root / "tools" / "vendor" / "sources.json"
    if not path.is_file():
        return {}
    payload = json.loads(path.read_text(encoding="utf-8"))
    vendors = payload.get("vendors", {})
    if not isinstance(vendors, dict):
        return {}
    return {
        str(name): record
        for name, record in vendors.items()
        if isinstance(record, dict)
    }


def _vendor_source_reference(
    record: dict[str, object], fallback: str
) -> str:
    repository = str(record.get("repository", fallback))
    tag = record.get("tag")
    commit = record.get("commit")
    if tag and commit:
        return f"{repository} tag {tag}, commit {commit}"
    return repository


def _tracked_evidence(repo_root: Path, *relative_paths: str) -> tuple[str, ...]:
    return tuple(
        relative_path
        for relative_path in relative_paths
        if (repo_root / relative_path).is_file()
    )


def _library_properties_version(path: Path) -> str | None:
    if not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip() or None
    return None


def exact_flutter_sdk_version(repo_root: Path) -> str | None:
    """Read the exact Flutter distribution pin used by local tools and CI."""
    pubspec = repo_root / "app/pubspec.yaml"
    if not pubspec.is_file():
        return None
    match = re.search(
        r"(?ms)^environment:\s*$.*?^\s{2}flutter:\s*['\"]?(\d+\.\d+\.\d+)['\"]?\s*$",
        pubspec.read_text(encoding="utf-8"),
    )
    return match.group(1) if match else None


def parse_pubspec_lock(path: Path, repo_root: Path) -> list[Component]:
    components: list[Component] = []
    package_name: str | None = None
    package_data: dict[str, str] = {}
    description_name: str | None = None
    description_url: str | None = None

    def flush() -> None:
        nonlocal package_name, package_data, description_name, description_url
        if package_name is None:
            return
        name = description_name or package_name
        source = package_data.get("source", "unknown")
        version = package_data.get("version", "unresolved")
        dependency = package_data.get("dependency", "transitive")
        source_reference = source
        if source == "hosted" and description_url:
            source_reference = (
                f"{description_url.rstrip('/')}/packages/{name}/versions/{version}"
            )
        elif source == "sdk":
            exact_sdk_version = exact_flutter_sdk_version(repo_root)
            if exact_sdk_version is not None:
                version = exact_sdk_version
                source_reference = (
                    f"https://github.com/flutter/flutter/tree/{exact_sdk_version}"
                )
            else:
                source_reference = "Flutter SDK"
        lock_digest = package_data.get("sha256") if source == "hosted" else None
        components.append(
            Component(
                ecosystem="dart-pub",
                name=name,
                version=version,
                relationship=dependency,
                source_type=source,
                source_reference=source_reference,
                evidence_paths=(repo_relative(path, repo_root),),
                license_expression="UNKNOWN",
                license_status="unresolved",
                notice_disposition="verify upstream license before publication",
                provenance_notes=(
                    (
                        "Tracked app/pubspec.yaml pins the exact Flutter SDK distribution used by CI."
                        if source == "sdk" and version != "0.0.0"
                        else "Tracked pubspec.lock records name, version, source, and hosted checksum/url only."
                    ),
                ),
                package_url=(
                    f"pkg:pub/{name}@{version}"
                    if source == "hosted" and version != "unresolved"
                    else None
                ),
                lock_digest_algorithm="sha256" if lock_digest else None,
                lock_digest=lock_digest,
            )
        )
        package_name = None
        package_data = {}
        description_name = None
        description_url = None

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if raw_line.startswith("  ") and not raw_line.startswith("    ") and raw_line.rstrip().endswith(":"):
            flush()
            package_name = raw_line.strip()[:-1]
            continue
        if package_name is None:
            continue
        stripped = raw_line.strip()
        if stripped.startswith("dependency:"):
            package_data["dependency"] = stripped.split(":", 1)[1].strip().strip('"')
        elif stripped.startswith("source:"):
            package_data["source"] = stripped.split(":", 1)[1].strip().strip('"')
        elif stripped.startswith("version:"):
            package_data["version"] = stripped.split(":", 1)[1].strip().strip('"')
        elif stripped.startswith("sha256:"):
            package_data["sha256"] = stripped.split(":", 1)[1].strip().strip('"')
        elif stripped.startswith("name:"):
            description_name = stripped.split(":", 1)[1].strip().strip('"')
        elif stripped.startswith("url:"):
            description_url = stripped.split(":", 1)[1].strip().strip('"')
    flush()
    return sorted(components, key=_component_sort_key)


def parse_podfile_lock(path: Path, repo_root: Path) -> list[Component]:
    components: list[Component] = []
    pods: dict[str, str] = {}
    direct_dependencies: set[str] = set()
    external_sources: dict[str, str | None] = {}
    external_source_name: str | None = None
    spec_checksums: dict[str, str] = {}
    section = ""

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if not raw_line:
            continue
        if not raw_line.startswith(" "):
            section = raw_line.rstrip(":")
            external_source_name = None
            continue
        stripped = raw_line.strip()
        if section == "PODS" and raw_line.startswith("  - "):
            entry = stripped[2:]
            match = re.match(r"([^ (]+) \(([^)]+)\)", entry)
            if match:
                pods[match.group(1)] = match.group(2)
        elif section == "DEPENDENCIES" and stripped.startswith("- "):
            direct_dependencies.add(stripped[2:].split(" ", 1)[0])
        elif section == "EXTERNAL SOURCES" and raw_line.startswith("  "):
            if not raw_line.startswith("    ") and stripped.endswith(":"):
                external_source_name = stripped[:-1]
                external_sources[external_source_name] = None
            elif (
                raw_line.startswith("    ")
                and external_source_name is not None
                and stripped.startswith(":path:")
            ):
                external_sources[external_source_name] = (
                    stripped[len(":path:") :].strip().strip('"').strip("'")
                )
        elif section == "SPEC CHECKSUMS" and ":" in stripped:
            name, checksum = stripped.split(":", 1)
            spec_checksums[name] = checksum.strip()

    for name, version in sorted(pods.items()):
        source_type = "path" if name in external_sources else "spec-repo"
        if name in external_sources:
            external_path = external_sources[name]
            if not external_path:
                raise ValueError(f"Podfile.lock external source has no path for {name}")
            source_reference = f"Podfile.lock :path: {external_path}"
        else:
            source_reference = "CocoaPods trunk"
        relationship = "direct" if name in direct_dependencies else "transitive"
        package_url = f"pkg:cocoapods/{name}@{version}"
        components.append(
            Component(
                ecosystem="cocoapods",
                name=name,
                version=version,
                relationship=relationship,
                source_type=source_type,
                source_reference=source_reference,
                evidence_paths=(repo_relative(path, repo_root),),
                license_expression="UNKNOWN",
                license_status="unresolved",
                notice_disposition="verify pod license from upstream podspec/source before publication",
                provenance_notes=(
                    "Tracked Podfile.lock records resolved pod versions and path/spec-repo origin, not license terms.",
                ),
                package_url=package_url,
                lock_digest_algorithm=(
                    "sha1" if spec_checksums.get(name) else None
                ),
                lock_digest=spec_checksums.get(name),
            )
        )
    return components


def parse_platformio(path: Path, repo_root: Path) -> list[Component]:
    parser = configparser.ConfigParser(inline_comment_prefixes=(";",))
    parser.read_string(path.read_text(encoding="utf-8"))
    aggregated: dict[tuple[str, str], dict[str, object]] = {}

    for section in parser.sections():
        if not section.startswith("env:"):
            continue
        env_name = section.split(":", 1)[1]
        relationship = f"platformio:{env_name}"
        platform = parser.get(section, "platform", fallback="").strip()
        if platform:
            name, version = _split_platformio_pin(platform)
            _merge_platformio_component(
                aggregated,
                key=("platformio-platform", name),
                component=Component(
                    ecosystem="platformio-platform",
                    name=name,
                    version=version,
                    relationship=relationship,
                    source_type="manifest-pin",
                    source_reference=platform,
                    evidence_paths=(repo_relative(path, repo_root),),
                    license_expression="UNKNOWN",
                    license_status="unresolved",
                    notice_disposition="verify upstream platform/framework licenses before publication",
                    provenance_notes=("Tracked platformio.ini pins the PlatformIO platform only.",),
                    package_url=(
                        f"pkg:generic/{name}@{version}"
                        if version != "unresolved"
                        else None
                    ),
                ),
            )
        framework = parser.get(section, "framework", fallback="").strip()
        if framework:
            framework_reference = framework
            framework_version = "unresolved"
            framework_source_type = "manifest-name"
            framework_package_name: str | None = None
            expected_framework_package = {
                "arduino": "framework-arduinoespressif32",
            }.get(framework)
            for entry in _iter_platformio_list(
                parser.get(section, "platform_packages", fallback="")
            ):
                package_name, package_version = _split_platformio_pin(entry)
                if (
                    expected_framework_package is not None
                    and package_name.split("/")[-1] == expected_framework_package
                ):
                    framework_reference = entry
                    framework_version = package_version
                    framework_source_type = "manifest-pin"
                    framework_package_name = package_name
            _merge_platformio_component(
                aggregated,
                key=("framework", framework),
                component=Component(
                    ecosystem="platformio-framework",
                    name=framework,
                    version=framework_version,
                    relationship=relationship,
                    source_type=framework_source_type,
                    source_reference=framework_reference,
                    evidence_paths=(repo_relative(path, repo_root),),
                    license_expression="UNKNOWN",
                    license_status="unresolved",
                    notice_disposition="carry the exact framework's complete mixed-license evidence before publication",
                    provenance_notes=(
                        (
                            "Tracked platformio.ini pins the exact PlatformIO framework package."
                            if framework_version != "unresolved"
                            else "Tracked platformio.ini names the framework but does not pin its version."
                        ),
                    ),
                    package_url=(
                        f"pkg:generic/{framework_package_name}@{framework_version}"
                        if framework_package_name is not None
                        and framework_version != "unresolved"
                        else None
                    ),
                ),
            )
        deps = parser.get(section, "lib_deps", fallback="")
        for entry in _iter_platformio_list(deps):
            lib_name, version = _split_platformio_pin(entry)
            _merge_platformio_component(
                aggregated,
                key=("platformio-lib", lib_name),
                component=Component(
                    ecosystem="platformio-lib",
                    name=lib_name,
                    version=version,
                    relationship=relationship,
                    source_type="manifest-pin",
                    source_reference=entry,
                    evidence_paths=(repo_relative(path, repo_root),),
                    license_expression="UNKNOWN",
                    license_status="unresolved",
                    notice_disposition="verify upstream library license from package metadata/source before publication",
                    provenance_notes=("Tracked platformio.ini is the only evidence used here.",),
                    package_url=(
                        f"pkg:generic/{lib_name}@{version}"
                        if version != "unresolved"
                        else None
                    ),
                ),
            )
    return sorted(
        [
            Component(
                ecosystem=value["ecosystem"],
                name=value["name"],
                version=value["version"],
                relationship=",".join(sorted(value["relationships"])),
                source_type=value["source_type"],
                source_reference=value["source_reference"],
                evidence_paths=tuple(sorted(value["evidence_paths"])),
                license_expression=value["license_expression"],
                license_status=value["license_status"],
                notice_disposition=value["notice_disposition"],
                provenance_notes=tuple(value["provenance_notes"]),
                package_url=value["package_url"],
            )
            for value in aggregated.values()
        ],
        key=_component_sort_key,
    )


def apply_dependency_license_evidence(
    components: list[Component], repo_root: Path
) -> list[Component]:
    """Apply exact tracked evidence while failing closed on stale records."""
    evidence_index = repo_root / DEPENDENCY_EVIDENCE_RELATIVE
    if not evidence_index.is_file():
        return sorted(components, key=_component_sort_key)

    try:
        payload = json.loads(evidence_index.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError(
            f"Invalid {repo_relative(evidence_index, repo_root)}: {error}"
        ) from error
    if payload.get("schema") != DEPENDENCY_EVIDENCE_SCHEMA:
        raise ValueError(
            f"Expected dependency evidence schema {DEPENDENCY_EVIDENCE_SCHEMA}"
        )
    records = payload.get("records")
    if not isinstance(records, list):
        raise ValueError("Dependency evidence records must be a list")

    components_by_key = {
        (component.ecosystem, component.name, component.version): component
        for component in components
    }
    if len(components_by_key) != len(components):
        raise ValueError("Dependency components must have unique ecosystem/name/version keys")

    resolved: dict[tuple[str, str, str], Component] = {}
    referenced_evidence_paths: set[str] = set()
    for record_number, record in enumerate(records, start=1):
        if not isinstance(record, dict):
            raise ValueError(f"Dependency evidence record {record_number} is not an object")
        key = tuple(record.get(field) for field in ("ecosystem", "name", "version"))
        if len(key) != 3 or not all(isinstance(value, str) and value for value in key):
            raise ValueError(
                f"Dependency evidence record {record_number} has an invalid key"
            )
        if key in resolved:
            raise ValueError(f"Duplicate dependency evidence record: {'/'.join(key)}")
        component = components_by_key.get(key)
        if component is None:
            raise ValueError(
                "Stale or orphan dependency evidence record: " + "/".join(key)
            )
        if component.version in {"unresolved", "0.0.0"}:
            raise ValueError(
                "Resolved dependency evidence requires an exact version: "
                + "/".join(key)
            )

        expected_source_type = record.get("expected_source_type")
        if expected_source_type != component.source_type:
            raise ValueError(
                f"Dependency evidence source type mismatch for {'/'.join(key)}: "
                f"expected {component.source_type}, got {expected_source_type}"
            )

        if component.ecosystem == "maven" and record.get("runtime_artifacts") != dict(component.runtime_artifacts):
            raise ValueError(f"Dependency evidence runtime artifact mismatch for {'/'.join(key)}")

        recorded_lock_digest = record.get("lock_digest")
        if component.lock_digest is None:
            if recorded_lock_digest is not None:
                raise ValueError(
                    f"Dependency evidence invents a lock digest for {'/'.join(key)}"
                )
        else:
            expected_digest = {
                "algorithm": component.lock_digest_algorithm,
                "value": component.lock_digest,
            }
            if recorded_lock_digest != expected_digest:
                raise ValueError(
                    f"Dependency evidence lock digest mismatch for {'/'.join(key)}"
                )

        raw_files = record.get("evidence_files")
        if not isinstance(raw_files, list) or not raw_files:
            raise ValueError(
                f"Dependency evidence files are missing for {'/'.join(key)}"
            )
        evidence_files: list[EvidenceFile] = []
        evidence_bytes_by_path: dict[str, bytes] = {}
        for raw_file in raw_files:
            if not isinstance(raw_file, dict):
                raise ValueError(
                    f"Dependency evidence file is invalid for {'/'.join(key)}"
                )
            relative_path = raw_file.get("path")
            expected_sha256 = raw_file.get("sha256")
            role = raw_file.get("role")
            origin = raw_file.get("origin")
            if not isinstance(relative_path, str) or not relative_path:
                raise ValueError(
                    f"Dependency evidence path is invalid for {'/'.join(key)}"
                )
            if not isinstance(expected_sha256, str) or not re.fullmatch(
                r"[0-9a-f]{64}", expected_sha256
            ):
                raise ValueError(
                    f"Dependency evidence SHA-256 is invalid for {relative_path}"
                )
            if role not in {"license-text", "notice-text", "provenance-metadata"}:
                raise ValueError(f"Dependency evidence role is invalid for {relative_path}")
            if not isinstance(origin, str) or not origin:
                raise ValueError(
                    f"Dependency evidence origin is missing for {relative_path}"
                )
            evidence_path = _contained_license_evidence_path(
                repo_root, relative_path
            )
            referenced_evidence_paths.add(relative_path)
            evidence_bytes = evidence_path.read_bytes()
            evidence_bytes_by_path[relative_path] = evidence_bytes
            actual_sha256 = hashlib.sha256(evidence_bytes).hexdigest()
            if actual_sha256 != expected_sha256:
                raise ValueError(
                    f"Dependency evidence SHA-256 mismatch for {relative_path}: "
                    f"expected {expected_sha256}, got {actual_sha256}"
                )
            text = None
            if role in {"license-text", "notice-text"}:
                try:
                    text = evidence_bytes.decode("utf-8")
                except UnicodeDecodeError as error:
                    raise ValueError(
                        f"Dependency license evidence is not UTF-8: {relative_path}"
                    ) from error
            evidence_files.append(
                EvidenceFile(
                    path=relative_path,
                    sha256=expected_sha256,
                    role=role,
                    origin=origin,
                    text=text,
                )
            )

        license_expression = record.get("license_expression")
        license_status = record.get("license_status")
        source_reference = record.get("source_reference")
        notice_disposition = record.get("notice_disposition")
        if not isinstance(license_expression, str) or not license_expression:
            raise ValueError(
                f"Dependency license expression is missing for {'/'.join(key)}"
            )
        if not isinstance(license_status, str) or not license_status.startswith(
            "resolved-"
        ):
            raise ValueError(
                f"Dependency license status is not resolved for {'/'.join(key)}"
            )
        has_license_text = any(
            evidence.role == "license-text" for evidence in evidence_files
        )
        if license_expression == "UNKNOWN":
            raise ValueError(
                "Resolved dependency evidence cannot use UNKNOWN for " + "/".join(key)
            )
        if license_expression == "NOASSERTION":
            if not has_license_text:
                raise ValueError(
                    "NOASSERTION dependency evidence must carry exact license text for "
                    + "/".join(key)
                )
            if license_status != "resolved-exact-license-text":
                raise ValueError(
                    "Dependency license status/text mismatch for " + "/".join(key)
                )
            if record.get("license_declaration") is not None:
                raise ValueError(
                    "NOASSERTION dependency evidence cannot declare SPDX metadata for "
                    + "/".join(key)
                )
        else:
            if license_expression not in SUPPORTED_DECLARED_SPDX_EXPRESSIONS:
                raise ValueError(
                    f"Unsupported SPDX dependency expression {license_expression!r} "
                    f"for {'/'.join(key)}"
                )
            expected_status = (
                "resolved-declared-spdx-and-carried-text"
                if has_license_text
                else "resolved-declared-spdx"
            )
            if license_status != expected_status:
                raise ValueError(
                    "Dependency license status/text mismatch for " + "/".join(key)
                )
            declaration = record.get("license_declaration")
            if not isinstance(declaration, dict):
                raise ValueError(
                    "SPDX dependency evidence must identify an exact declaration for "
                    + "/".join(key)
                )
            declaration_path = declaration.get("path")
            declaration_format = declaration.get("format")
            declaration_field = declaration.get("field")
            if (
                not isinstance(declaration_path, str)
                or not isinstance(declaration_format, str)
                or not isinstance(declaration_field, str)
                or declaration_path not in evidence_bytes_by_path
            ):
                raise ValueError(
                    f"SPDX declaration metadata is invalid for {'/'.join(key)}"
                )
            declaration_evidence = next(
                evidence
                for evidence in evidence_files
                if evidence.path == declaration_path
            )
            if declaration_evidence.role != "provenance-metadata":
                raise ValueError(
                    f"SPDX declaration is not provenance metadata for {'/'.join(key)}"
                )
            if declaration_format == "maven-pom":
                if component.ecosystem != "maven" or declaration_field != "licenses":
                    raise ValueError(f"Invalid Maven POM license declaration for {'/'.join(key)}")
                parent_path = declaration.get("parent_path")
                if parent_path is not None and not any(
                    evidence.path == parent_path and evidence.role == "provenance-metadata"
                    for evidence in evidence_files
                ):
                    raise ValueError(f"Maven POM license parent is missing for {'/'.join(key)}")
                declared_license = _read_maven_license_declaration(
                    component, declaration_path, parent_path, evidence_bytes_by_path
                )
            else:
                if component.ecosystem == "maven":
                    raise ValueError(f"Maven SPDX declaration requires a POM for {'/'.join(key)}")
                declared_license = _read_declared_license(
                    evidence_bytes_by_path[declaration_path],
                    declaration_format,
                    declaration_field,
                    declaration_path,
                )
            if declared_license != license_expression:
                raise ValueError(
                    f"SPDX declaration mismatch for {'/'.join(key)}: expected "
                    f"{license_expression}, got {declared_license}"
                )
        if source_reference != component.source_reference:
            raise ValueError(
                f"Dependency source reference mismatch for {'/'.join(key)}"
            )
        if not isinstance(notice_disposition, str) or not notice_disposition:
            raise ValueError(
                f"Dependency notice disposition is missing for {'/'.join(key)}"
            )
        provenance_notes = record.get("provenance_notes", [])
        if not isinstance(provenance_notes, list) or not all(
            isinstance(note, str) and note for note in provenance_notes
        ):
            raise ValueError(
                f"Dependency provenance notes are invalid for {'/'.join(key)}"
            )
        package_url = record.get("package_url", component.package_url)
        if package_url != component.package_url:
            raise ValueError(f"Dependency package URL mismatch for {'/'.join(key)}")

        resolved[key] = replace(
            component,
            source_reference=source_reference,
            evidence_paths=tuple(
                sorted(
                    set(component.evidence_paths)
                    | {evidence.path for evidence in evidence_files}
                    | {repo_relative(evidence_index, repo_root)}
                )
            ),
            license_expression=license_expression,
            license_status=license_status,
            notice_disposition=notice_disposition,
            provenance_notes=tuple(component.provenance_notes)
            + tuple(provenance_notes),
            package_url=package_url,
            license_evidence=tuple(evidence_files),
        )

    evidence_root = repo_root / LICENSE_EVIDENCE_ROOT_RELATIVE
    actual_evidence_paths = {
        repo_relative(path, repo_root)
        for path in evidence_root.rglob("*")
        if path.is_file() and path.name != ".gitattributes"
    }
    orphan_paths = sorted(actual_evidence_paths - referenced_evidence_paths)
    if orphan_paths:
        raise ValueError(
            "Unreferenced dependency evidence files: " + ", ".join(orphan_paths)
        )

    return sorted(
        [
            resolved.get(
                (component.ecosystem, component.name, component.version), component
            )
            for component in components
        ],
        key=_component_sort_key,
    )


def _contained_license_evidence_path(repo_root: Path, relative_path: str) -> Path:
    candidate_relative = Path(relative_path)
    if candidate_relative.is_absolute():
        raise ValueError(f"Dependency evidence path must be relative: {relative_path}")
    evidence_root = (repo_root / LICENSE_EVIDENCE_ROOT_RELATIVE).resolve()
    candidate = (repo_root / candidate_relative).resolve()
    try:
        candidate.relative_to(evidence_root)
    except ValueError as error:
        raise ValueError(
            f"Dependency evidence path escapes {LICENSE_EVIDENCE_ROOT_RELATIVE}: "
            f"{relative_path}"
        ) from error
    if not candidate.is_file():
        raise ValueError(f"Dependency evidence file is missing: {relative_path}")
    return candidate


def _read_declared_license(
    evidence_bytes: bytes, evidence_format: str, field_name: str, path: str
) -> str:
    try:
        evidence_text = evidence_bytes.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError(f"SPDX declaration metadata is not UTF-8: {path}") from error
    if evidence_format == "json":
        try:
            value = json.loads(evidence_text).get(field_name)
        except (ValueError, AttributeError) as error:
            raise ValueError(f"SPDX declaration JSON is invalid: {path}") from error
    elif evidence_format == "properties":
        properties = {
            key.strip(): value.strip()
            for key, value in (
                line.split("=", 1)
                for line in evidence_text.splitlines()
                if "=" in line and not line.lstrip().startswith(("#", ";"))
            )
        }
        value = properties.get(field_name)
    else:
        raise ValueError(f"Unsupported SPDX declaration format {evidence_format}: {path}")
    if not isinstance(value, str) or not value:
        raise ValueError(f"SPDX declaration field {field_name} is invalid: {path}")
    return value


@dataclass(frozen=True)
class MavenPom:
    coordinate: tuple[str, str, str]
    parent: tuple[str, str, str] | None
    license_expression: str | None


def parse_maven_pom(data: bytes, path: str) -> MavenPom:
    try:
        project = ElementTree.fromstring(data)
    except ElementTree.ParseError as error:
        raise ValueError(f"Invalid Maven POM {path}: {error}") from error
    if project.tag not in {"project", "{http://maven.apache.org/POM/4.0.0}project"}:
        raise ValueError(f"Invalid Maven POM root in {path}")

    def value(node: ElementTree.Element, name: str) -> str:
        return (node.findtext(f"{{*}}{name}") or "").strip()

    parent = project.find("{*}parent")
    parent_coordinate = None if parent is None else (
        value(parent, "groupId"), value(parent, "artifactId"), value(parent, "version"),
    )
    parent_group, _, parent_version = parent_coordinate or ("", "", "")
    coordinate = (
        value(project, "groupId") or parent_group,
        value(project, "artifactId"),
        value(project, "version") or parent_version,
    )
    if not all(
        re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*", part)
        for part in coordinate + (parent_coordinate or ())
    ):
        raise ValueError(f"Invalid Maven POM coordinates in {path}")
    licenses = project.findall("{*}licenses/{*}license")
    if not licenses:
        return MavenPom(coordinate, parent_coordinate, None)
    if len(licenses) != 1:
        raise ValueError(f"Maven POM must declare a single reviewed license in {path}")
    name, url = value(licenses[0], "name"), value(licenses[0], "url")
    if name in {
        "Apache-2.0", "Apache License Version 2.0", "The Apache License, Version 2.0",
        "The Apache Software License, Version 2.0",
    } and url in {
        "http://www.apache.org/licenses/LICENSE-2.0.txt",
        "https://www.apache.org/licenses/LICENSE-2.0.txt",
    }:
        expression = "Apache-2.0"
    elif (name, url) == ("BSD-3-Clause", "https://opensource.org/licenses/BSD-3-Clause"):
        expression = "BSD-3-Clause"
    elif (name, url) == ("CC0", "http://creativecommons.org/publicdomain/zero/1.0/"):
        expression = "CC0-1.0"
    else:
        raise ValueError(f"Unsupported Maven POM license declaration in {path}: {name!r}, {url!r}")
    return MavenPom(coordinate, parent_coordinate, expression)


def _read_maven_license_declaration(
    component: Component,
    path: str,
    parent_path: str | None,
    evidence_bytes_by_path: dict[str, bytes],
) -> str | None:
    pom = parse_maven_pom(evidence_bytes_by_path[path], path)
    if pom.coordinate != (*component.name.split(":"), component.version):
        raise ValueError(f"Maven POM coordinate mismatch for {component.name}")
    if parent_path is None:
        return pom.license_expression
    parent = parse_maven_pom(evidence_bytes_by_path[parent_path], parent_path)
    if pom.license_expression is not None or parent.coordinate != pom.parent:
        raise ValueError(f"Maven POM license parent mismatch for {component.name}")
    return parent.license_expression


def _read_mit_notice(
    repo_root: Path,
    relative_path: str,
    *,
    expected_sha256: str | None = None,
) -> str:
    """Return exact checked-in MIT text; fail closed on absent or changed evidence."""
    license_path = repo_root / relative_path
    notice_bytes = license_path.read_bytes()
    if expected_sha256 is not None:
        actual_sha256 = hashlib.sha256(notice_bytes).hexdigest()
        if actual_sha256 != expected_sha256:
            raise ValueError(
                f"Expected {repo_relative(license_path, repo_root)} SHA-256 "
                f"{expected_sha256}, got {actual_sha256}"
            )
    notice_text = notice_bytes.decode("utf-8")
    if not notice_text.startswith("MIT License\n"):
        raise ValueError(f"Expected MIT notice text in {repo_relative(license_path, repo_root)}")
    return notice_text


def collect_litch_max_component(repo_root: Path) -> Component | None:
    package_path = repo_root / LITCH_MAX_RELATIVE / "library.json"
    modules_path = repo_root / ".gitmodules"
    if (not package_path.exists() and not package_path.is_symlink()
            and not modules_path.exists() and not modules_path.is_symlink()
            and not (repo_root / LITCH_MAX_RELATIVE / "src").exists()):
        return None
    verifier_path = Path(__file__).resolve().parents[1] / "pio/verify_litch_max.py"
    spec = importlib.util.spec_from_file_location("litch_max_submodule", verifier_path)
    if spec is None or spec.loader is None:
        raise ValueError("Litch-Max submodule verifier is unavailable")
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    snapshot = verifier.verify_snapshot(repo_root)
    package = json.loads(package_path.read_text(encoding="utf-8"))
    if (package.get("name") != "litch-max" or not isinstance(package.get("version"), str)
            or package.get("license") != "MPL-2.0 AND MIT AND LicenseRef-micro-Max-Public-Domain"):
        raise ValueError("Litch-Max package identity or license scope differs from reviewed metadata")
    source = snapshot["repository"].removesuffix(".git") + "/tree/" + snapshot["commit"]
    license_path = LITCH_MAX_RELATIVE / "LICENSE"
    license_evidence = _read_approved_license(
        repo_root, license_path, snapshot["files"]["LICENSE"]["sha256"], "Litch-Max",
        "Exact MPL 2.0 grant from the pinned Litch-Max Git submodule",
    )
    if license_evidence is None:
        raise ValueError("The pinned Litch-Max license is missing")
    return Component(
        ecosystem="first-party-library", name="Litch-Max", version=package["version"],
        relationship="submodule-internal", source_type="git-submodule",
        source_reference=source,
        evidence_paths=_tracked_evidence(
            repo_root, ".gitmodules",
            (LITCH_MAX_RELATIVE / "library.json").as_posix(), license_path.as_posix(),
            (LITCH_MAX_RELATIVE / "README.md").as_posix(),
        ),
        license_expression="MPL-2.0", license_status="resolved-pinned-source-license",
        notice_disposition=(
            "Preserve the Litch-Max MPL grant and source access; nested mcu-max/micro-Max "
            "and portable foxie_embedded inputs retain their separately recorded terms."
        ),
        provenance_notes=(
            f"Every submodule file and mode matches the parent gitlink commit {snapshot['commit']} "
            f"and tree {snapshot['tree']}.",
            "The two bundled foxie_embedded headers are checked byte-for-byte against "
            "the parent project's corresponding MPL-covered headers.",
        ),
        package_url=f"pkg:github/foxieproducts/litch-max@{snapshot['commit']}",
        lock_digest_algorithm="git-sha1", lock_digest=snapshot["commit"],
        license_evidence=(license_evidence,),
    )


def collect_vendored_components(
    repo_root: Path, project_license: EvidenceFile | None = None,
) -> list[Component]:
    records = _read_vendor_records(repo_root)
    mcu_max_record = records.get("mcu-max", {})
    pca95x5_record = records.get("pca95x5", {})
    sparkfun_record = records.get("sparkfun-veml6030", {})
    mcu_review_path = "lib/litch-max/docs/mcu_max_provenance.md"
    sparkfun_review_path = "docs/vendor/sparkfun_veml6030.md"
    mcu_grant = None
    if (repo_root / mcu_review_path).is_file():
        review = (repo_root / mcu_review_path).read_text(encoding="utf-8")
        mcu_grant = re.search(r"(?m)^> (Micro-Max is public domain, so you can use it for whatever purpose you like\.)$", review)
        if mcu_grant is None:
            raise ValueError("Reviewed micro-Max author grant is missing from vendor evidence")
    sparkfun_notice = None
    if (repo_root / sparkfun_review_path).is_file():
        review = (repo_root / sparkfun_review_path).read_text(encoding="utf-8")
        section = review.split("## License and attribution disposition", 1)[-1].split("## Complete diff classification", 1)[0]
        excerpts = re.findall(r"(?ms)^```text\n(.*?)\n```$", section)
        if len(excerpts) != 3:
            raise ValueError("Reviewed SparkFun README, implementation and example terms are required")
        sparkfun_notice = "\n\n".join([
            "SparkFun Electronics and Elias Santistevan, July 2019.",
            *excerpts,
        ])
    pca95x5_license_sha256 = pca95x5_record.get("license_sha256")
    if not isinstance(pca95x5_license_sha256, str):
        raise ValueError("PCA95x5 vendor evidence must record license_sha256")
    mcu_max_version = str(
        mcu_max_record.get("tag")
        or _library_properties_version(
            repo_root / "lib" / "litch-max" / "vendor" / "mcu-max" / "library.properties"
        )
        or "unresolved"
    )
    components = [
        Component(
            ecosystem="vendored-cpp",
            name="PCA95x5",
            version=str(pca95x5_record.get("tag") or "unresolved"),
            relationship="vendored-direct",
            source_type="checked-in-source",
            source_reference=_vendor_source_reference(
                pca95x5_record,
                "lib/PCA95x5/PCA95x5.h (upstream baseline unresolved)",
            ),
            evidence_paths=_tracked_evidence(
                repo_root,
                "lib/PCA95x5/PCA95x5.h",
                "lib/PCA95x5/LICENSE",
                "tools/vendor/sources.json",
                "docs/vendor/pca95x5.md",
            ),
            license_expression="MIT",
            license_status="tracked-mit-notice-carried",
            notice_disposition=(
                "include the tracked upstream MIT notice with PCA95x5 source "
                "and binary distributions"
            ),
            provenance_notes=(
                "v0.1.3 is the selected exact comparison baseline; v0.1.2 "
                "has the same imported header, so the historical import tag "
                "remains ambiguous.",
                "The tracked license is byte-identical to the pinned v0.1.3 "
                "LICENSE blob (SHA-256 "
                "e38a878eb6e9a21413bca755242c8a239e70a96e96c0e978009a381cdabadeef).",
            ),
            package_url=None,
            notice_text=_read_mit_notice(
                repo_root,
                "lib/PCA95x5/LICENSE",
                expected_sha256=pca95x5_license_sha256,
            ),
        ),
        Component(
            ecosystem="vendored-cpp",
            name="SparkFun Ambient Light Sensor Arduino Library",
            version=str(sparkfun_record.get("tag") or "unresolved"),
            relationship="vendored-direct",
            source_type="checked-in-source",
            source_reference=_vendor_source_reference(
                sparkfun_record,
                "lib/SparkFun_Ambient_Light_Sensor_Arduino_Library",
            ),
            evidence_paths=_tracked_evidence(
                repo_root,
                "lib/SparkFun_Ambient_Light_Sensor_Arduino_Library/library.properties",
                "lib/SparkFun_Ambient_Light_Sensor_Arduino_Library/README.MD",
                "tools/vendor/sources.json",
                sparkfun_review_path,
            ),
            license_expression="LicenseRef-SparkFun-VEML6030" if sparkfun_notice else "NOASSERTION",
            license_status="resolved-reviewed-custom-terms" if sparkfun_notice else "non-standard-file-scoped-terms-review",
            notice_disposition=(
                "preserve SparkFun Electronics and Elias Santistevan attribution "
                "and the exact README and file-scoped terms with derivative library files"
            ),
            provenance_notes=(
                "The pinned upstream 1.0.4 baseline is retained with reviewed "
                "local read-validity changes in the C++ source and header; the "
                "other eight vendor files, including README and attribution, "
                "remain byte-identical to that baseline.",
                "README and source headers mix Beerware, attribution/share-alike "
                "requests, and public-domain wording; no standalone SPDX-style "
                "license file exists.",
            ),
            package_url=None,
            notice_text=sparkfun_notice,
            notice_text_source=sparkfun_review_path if sparkfun_notice else None,
        ),
        Component(
            ecosystem="vendored-c",
            name="mcu-max",
            version=mcu_max_version,
            relationship="vendored-transitive",
            source_type="checked-in-source",
            source_reference=_vendor_source_reference(
                mcu_max_record,
                "https://github.com/Gissio/mcu-max",
            ),
            evidence_paths=_tracked_evidence(
                repo_root,
                "lib/litch-max/vendor/mcu-max/library.properties",
                "lib/litch-max/vendor/mcu-max/library.json",
                "lib/litch-max/vendor/mcu-max/LICENSE",
                "tools/vendor/sources.json",
                mcu_review_path,
            ),
            license_expression="MIT AND LicenseRef-micro-Max-Public-Domain" if mcu_grant else "MIT",
            license_status="resolved-reviewed-transitive-terms" if mcu_grant else "direct-mit-transitive-chain-review",
            notice_disposition=(
                "include the tracked MIT notice, H.G. Muller's original grant and mcu-max/micro-Max attribution"
                if mcu_grant else "include the tracked MIT notice and review the underlying micro-Max grant"
            ),
            provenance_notes=(
                "Tracked 1.0.7 license and package metadata agree on MIT for mcu-max.",
                "This upstream-derived source is nested in the pinned Litch-Max library.",
            ) + ((
                f'H.G. Muller wrote on August 22, 2026, "{mcu_grant[1]}" '
                "https://www.talkchess.com/forum/viewtopic.php?p=996683#p996683",
                "The reviewed family-wide grant applies to the separately identified micro-Max 4.8 lineage. "
                "This scope is an inference from the author's version-unrestricted statement.",
            ) if mcu_grant else ("The authoritative grant for the underlying micro-Max 4.8 source remains unresolved.",)),
            package_url=(
                f"pkg:generic/mcu-max@{mcu_max_version}"
                if mcu_max_version != "unresolved"
                else None
            ),
            notice_text=_read_mit_notice(
                repo_root, "lib/litch-max/vendor/mcu-max/LICENSE",
                expected_sha256="bd896154097fe85796b8ff31771ba315be734fe9e9d30b80a72e5e52ad64ea84" if mcu_grant else None,
            ).strip(),
        ),
        Component(
            ecosystem="first-party-library",
            name="foxie_embedded",
            version="0.1",
            relationship="vendored-internal",
            source_type="checked-in-source",
            source_reference="lib/foxie_embedded",
            evidence_paths=_tracked_evidence(
                repo_root,
                "lib/foxie_embedded/README.md",
                "lib/foxie_embedded/version.md",
            ) + ((project_license.path,) if project_license else ()),
            license_expression=ROOT_LICENSE_EXPRESSION if project_license else "PROJECT-LICENSE-TBD",
            license_status="resolved-approved-root-license" if project_license else "blocked-on-root-license",
            notice_disposition=(
                "The canonical original-software LICENSE explicitly covers foxie_embedded."
                if project_license else "First-party component pending the root project licensing decision."
            ),
            provenance_notes=("Tracked files establish this as a project-owned reusable library.",),
            package_url=None,
            license_evidence=(project_license,) if project_license else (),
        ),
    ]
    litch_max = collect_litch_max_component(repo_root)
    if litch_max is not None:
        components.append(litch_max)
    return sorted(components, key=_component_sort_key)


def parse_android_runtime_snapshot(repo_root: Path) -> list[Component]:
    path = repo_root / ANDROID_RUNTIME_RELATIVE
    if not path.is_file():
        if (repo_root / "app/android").is_dir():
            raise ValueError(f"Missing Android release runtime snapshot {ANDROID_RUNTIME_RELATIVE}")
        return []
    if (repo_root / ANDROID_RUNTIME_PUB_OVERRIDES).exists():
        raise ValueError(f"Android release runtime snapshot requires no local {ANDROID_RUNTIME_PUB_OVERRIDES}")

    payload = json.loads(path.read_text(encoding="utf-8"))
    if (
        not isinstance(payload, dict)
        or payload.get("schema") != "litch-android-runtime-v1"
        or payload.get("configuration") != ANDROID_RUNTIME_CONFIGURATION
    ):
        raise ValueError("Invalid Android release runtime snapshot schema or classpath")
    gradle_version = payload.get("gradle_version")
    flutter_version = exact_flutter_sdk_version(repo_root)
    if (
        not isinstance(gradle_version, str)
        or not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", gradle_version)
        or not flutter_version
        or payload.get("flutter_version") != flutter_version
    ):
        raise ValueError("Android release runtime snapshot has a stale or invalid toolchain")

    inputs = payload.get("inputs")
    if not isinstance(inputs, dict) or inputs.keys() != set(ANDROID_RUNTIME_INPUTS):
        raise ValueError("Android release runtime snapshot has an invalid input set")
    for relative in ANDROID_RUNTIME_INPUTS:
        source = repo_root / relative
        if not source.is_file():
            raise ValueError(f"Missing Android release runtime input {relative}")
        content = source.read_text(encoding="utf-8")
        if relative == "app/pubspec.yaml":
            content = re.sub(r"(?m)^version:[^\n]*\n?", "", content, count=1)
        if hashlib.sha256(content.encode("utf-8")).hexdigest() != inputs[relative]:
            raise ValueError(f"Stale Android release runtime snapshot for {relative}")

    modules = payload.get("modules")
    if not isinstance(modules, list) or not modules:
        raise ValueError("Android release runtime snapshot has no Maven modules")
    components: list[Component] = []
    coordinates: set[str] = set()
    children: dict[str, set[str]] = {}
    for module in modules:
        coordinate = module.get("coordinate") if isinstance(module, dict) else None
        if not isinstance(coordinate, str) or not re.fullmatch(
            r"[\w.-]+:[\w.-]+:[\w.+-]+", coordinate, flags=re.ASCII
        ):
            raise ValueError(f"Invalid Maven coordinate in Android release runtime: {coordinate!r}")
        if coordinate in coordinates:
            raise ValueError(f"Duplicate Maven coordinate in Android release runtime: {coordinate}")
        coordinates.add(coordinate)
        group, name, version = coordinate.split(":")

        parents = module.get("parents")
        if not isinstance(parents, list) or not parents or not all(
            isinstance(parent, str) and parent for parent in parents
        ):
            raise ValueError(f"Invalid Android release dependency graph for {coordinate}")
        for parent in parents:
            children.setdefault(parent, set()).add(coordinate)
        artifacts = module.get("artifacts")
        if not isinstance(artifacts, dict) or not all(
            re.fullmatch(r"[\w.+-]+", name, flags=re.ASCII)
            and isinstance(digest, str)
            and re.fullmatch(r"[0-9a-f]{64}", digest)
            for name, digest in artifacts.items()
        ):
            raise ValueError(f"Invalid Android release artifact digests for {coordinate}")

        components.append(
            Component(
                ecosystem="maven",
                name=f"{group}:{name}",
                version=version,
                relationship="android release runtime" if artifacts else "android release runtime metadata",
                source_type="gradle-runtime-snapshot",
                source_reference=ANDROID_RUNTIME_CONFIGURATION,
                evidence_paths=(repo_relative(path, repo_root),),
                license_expression="UNKNOWN",
                license_status="unresolved",
                notice_disposition="review exact license and notice evidence before publication",
                provenance_notes=(
                    f"Selected by Gradle {gradle_version} with Flutter {flutter_version}.",
                    f"Graph parents include {', '.join(sorted(set(parents)))}.",
                ) + tuple(f"SHA-256 {digest} for {filename}." for filename, digest in sorted(artifacts.items())),
                package_url=f"pkg:maven/{group}/{name}@{quote(version, safe='')}",
                runtime_artifacts=tuple(sorted(artifacts.items())),
            )
        )
    project_roots = {
        parent for parent in children
        if re.fullmatch(r"project :[\w.-]+(?::[\w.-]+)*", parent, flags=re.ASCII)
    }
    if unknown_parents := children.keys() - coordinates - project_roots:
        raise ValueError("Unknown Maven parents in Android release runtime: " + ", ".join(sorted(unknown_parents)))
    reachable = project_roots.copy()
    pending = list(project_roots)
    while pending:
        for child in children.get(pending.pop(), ()):
            if child not in reachable:
                reachable.add(child)
                pending.append(child)
    if disconnected := coordinates - reachable:
        raise ValueError("Unreachable Maven modules in Android release runtime: " + ", ".join(sorted(disconnected)))
    return sorted(components, key=_component_sort_key)


def collect_flutter_assets(repo_root: Path) -> list[Component]:
    pubspec = repo_root / "app/pubspec.yaml"
    if not pubspec.is_file() or not re.search(
        r"(?m)^  uses-material-design: true[ \t]*$", pubspec.read_text(encoding="utf-8")
    ):
        return []
    sdk_version = exact_flutter_sdk_version(repo_root)
    if sdk_version is None:
        raise ValueError("MaterialIcons evidence requires an exact Flutter SDK pin")
    pin_path = LICENSE_EVIDENCE_ROOT_RELATIVE / "flutter-sdk" / sdk_version / "material-icons/material_fonts.version"
    archive = (repo_root / pin_path).read_text(encoding="utf-8").strip()
    match = re.fullmatch(r"flutter_infra_release/flutter/fonts/([0-9a-f]{40})/fonts\.zip", archive)
    if match is None:
        raise ValueError("Invalid Flutter MaterialIcons font archive pin")
    version = match[1]
    return [Component(
        ecosystem="flutter-asset",
        name="MaterialIcons",
        version=version,
        relationship="app runtime asset",
        source_type="sdk-asset",
        source_reference=f"https://storage.googleapis.com/{archive}",
        evidence_paths=("app/pubspec.yaml", pin_path.as_posix()),
        license_expression="UNKNOWN",
        license_status="unresolved",
        notice_disposition="review the exact Flutter-distributed font license and attribution",
    )]


def collect_s3_runtime(repo_root: Path) -> list[Component]:
    manifest = repo_root / "platformio.ini"
    config = configparser.ConfigParser(interpolation=None)
    config.read(manifest, encoding="utf-8")
    section = "env:litch-esp32s3"
    if not config.has_section(section):
        return []
    platform, version = _split_platformio_pin(config.get(section, "platform"))
    if platform != "platformio/espressif32" or not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError("S3 firmware runtime requires an exact Espressif32 platform pin")
    metadata_path = (
        LICENSE_EVIDENCE_ROOT_RELATIVE / "platformio-platform"
        / f"platformio__espressif32-{version}" / "platform.json"
    )
    platform_metadata = json.loads((repo_root / metadata_path).read_text(encoding="utf-8"))
    toolchain = "toolchain-xtensa-esp32s3"
    package = platform_metadata.get("packages", {}).get(toolchain, {})
    runtime_version = package.get("version")
    if (
        platform_metadata.get("name") != "espressif32"
        or platform_metadata.get("version") != version
        or package.get("type") != "toolchain"
        or package.get("owner") != "espressif"
        or not isinstance(runtime_version, str)
        or not re.fullmatch(r"\d+\.\d+\.\d+\+[A-Za-z0-9.-]+", runtime_version)
    ):
        raise ValueError("Selected Espressif32 platform has no exact S3 runtime provider")

    name = f"espressif/{toolchain}"
    source = f"{name}@{runtime_version}"
    for override in _iter_platformio_list(config.get(section, "platform_packages", fallback="")):
        package_name, _ = _split_platformio_pin(override)
        if package_name.rsplit("/", 1)[-1] == toolchain and override != source:
            raise ValueError("S3 runtime provider differs from the tracked platform selection")
    return [Component(
        ecosystem="platformio-runtime",
        name=name,
        version=runtime_version,
        relationship="firmware runtime linked by platformio:litch-esp32s3",
        source_type="platformio-runtime-selection",
        source_reference=source,
        evidence_paths=("platformio.ini", metadata_path.as_posix()),
        license_expression="UNKNOWN",
        license_status="unresolved",
        notice_disposition="review the runtime archives linked into the selected S3 image",
        provenance_notes=(
            f"Tracked {platform} {version} metadata identifies the selected S3 toolchain package.",
            "This component covers selected runtime objects linked into the firmware image.",
        ),
        package_url=f"pkg:generic/{name}@{quote(runtime_version, safe='')}",
    )]


def _read_approved_license(
    repo_root: Path, relative: Path, expected_sha256: str, scope: str, origin: str,
) -> EvidenceFile | None:
    path = repo_root / relative
    if path.is_symlink():
        raise ValueError(f"{relative} must be a regular file")
    if not path.exists():
        return None
    if not path.is_file():
        raise ValueError(f"{relative} must be a regular file")
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != expected_sha256:
        raise ValueError(f"{relative} differs from the pinned {scope} terms")
    return EvidenceFile(
        path=relative.as_posix(), sha256=digest, role="license-text",
        origin=origin, text=data.decode("utf-8"),
    )


def read_project_license(repo_root: Path) -> EvidenceFile | None:
    return _read_approved_license(
        repo_root, ROOT_LICENSE_RELATIVE, ROOT_LICENSE_SHA256, "original-software",
        "Foxie Products, LLC original-software scope and unmodified MPL 2.0",
    )


def read_hardware_license(repo_root: Path) -> EvidenceFile | None:
    return _read_approved_license(
        repo_root, HARDWARE_LICENSE_RELATIVE, HARDWARE_LICENSE_SHA256, "hardware-design",
        "Foxie Products, LLC hardware-design scope and unmodified CERN-OHL-S-2.0",
    )


def read_documentation_license(repo_root: Path) -> EvidenceFile | None:
    return _read_approved_license(
        repo_root, DOCUMENTATION_LICENSE_RELATIVE, DOCUMENTATION_LICENSE_SHA256, "documentation-and-image",
        "Foxie Products, LLC original prose, listed photographs and documentation previews "
        "with unmodified CC BY-SA 4.0",
    )


def collect_board_notices(repo_root: Path, *, required: bool = False) -> tuple[tuple[str, str], ...]:
    manifest = repo_root / FIRMWARE_LICENSE_MANIFEST_RELATIVE
    if not manifest.is_file():
        if required:
            raise ValueError("Missing license manifest for the selected S3 firmware")
        return ()
    legal = json.loads(manifest.read_text(encoding="utf-8"))
    if legal.get("schema_version") != 1 or legal.get("target") != "litch-esp32s3":
        raise ValueError("Board notices require the selected S3 license manifest")

    vendor_sections = {
        "lib/litch-max/docs/mcu_max_provenance.md": "## License and attribution",
        "docs/vendor/sparkfun_veml6030.md": "## License and attribution disposition",
    }
    notices = []
    seen = set()
    for item in legal["legal_files"]:
        role = item["role"]
        relative = item["path"]
        path = repo_root / relative
        if (
            role not in {"license-text", "notice-text", "provenance-metadata"}
            or relative in seen
            or path.is_symlink()
            or repo_relative(path, repo_root) != relative
        ):
            raise ValueError(f"Invalid board notice evidence path or role for {relative}")
        seen.add(relative)
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != item["sha256"]:
            raise ValueError(f"Board notice evidence bytes changed for {relative}")

        if role == "provenance-metadata":
            continue
        text = raw.decode("utf-8")
        if section := vendor_sections.get(relative):
            text = text.replace("\r\n", "\n").replace("\r", "\n")
            _, heading, text = text.partition(f"\n{section}\n")
            if not heading:
                raise ValueError(f"Missing vendor license section in {relative}")
            text = text.partition("\n## ")[0]
        component = item["component"]
        if not isinstance(component, str) or not component.strip() or component.splitlines() != [component]:
            raise ValueError(f"Invalid board notice component for {relative}")
        notices.append((f"Board firmware — {component}", text))
    return tuple(notices)


def build_inventory(repo_root: Path) -> Inventory:
    s3_runtime = collect_s3_runtime(repo_root)
    dependency_components = (
        parse_pubspec_lock(repo_root / "app" / "pubspec.lock", repo_root)
        + parse_podfile_lock(repo_root / "app" / "ios" / "Podfile.lock", repo_root)
        + parse_android_runtime_snapshot(repo_root)
        + parse_platformio(repo_root / "platformio.ini", repo_root)
        + collect_flutter_assets(repo_root)
        + s3_runtime
    )
    dependency_components = apply_dependency_license_evidence(
        dependency_components, repo_root
    )
    project_license = read_project_license(repo_root)
    vendors = collect_vendored_components(repo_root, project_license)
    components = dependency_components + vendors
    pending_project = () if project_license else (
        "Root project license remains an explicit human/legal decision and is not chosen by this tooling.",
        "foxie_embedded is first-party code with no standalone tracked outbound license file.",
    )
    unresolved_questions = pending_project + (
        "PCA95x5 carries the pinned v0.1.3 MIT notice, but the exact historical "
        "import tag remains ambiguous because v0.1.2 has the same imported header.",
    ) + tuple(
        f"{component.name} terms remain unresolved pending exact vendor review."
        for component in vendors if component.license_status in {
            "direct-mit-transitive-chain-review", "non-standard-file-scoped-terms-review"
        }
    ) + _dependency_unresolved_questions(dependency_components)
    return Inventory(
        tool_version=TOOL_VERSION,
        inventory_schema=INVENTORY_SCHEMA,
        sbom_schema=SBOM_SCHEMA,
        components=tuple(sorted(components, key=_component_sort_key)),
        unresolved_questions=unresolved_questions,
        board_notices=collect_board_notices(repo_root, required=bool(s3_runtime)),
        project_license=project_license,
        hardware_license=read_hardware_license(repo_root),
        documentation_license=read_documentation_license(repo_root),
    )


def _dependency_unresolved_questions(
    components: list[Component],
) -> tuple[str, ...]:
    unresolved = {
        ecosystem: {
            component.name
            for component in components
            if component.ecosystem == ecosystem
            and not component.license_status.startswith("resolved-")
        }
        for ecosystem in {
            "dart-pub",
            "cocoapods",
            "flutter-asset",
            "maven",
            "platformio-framework",
            "platformio-lib",
            "platformio-platform",
            "platformio-runtime",
        }
    }
    questions: list[str] = []
    dart_names = unresolved["dart-pub"]
    if dart_names == {"flutter", "flutter_test", "flutter_web_plugins", "sky_engine"}:
        questions.append(
            "Flutter SDK packages remain unresolved because the release path does "
            "not lock an exact Flutter SDK distribution."
        )
    elif dart_names == {"sky_engine"}:
        questions.append(
            "The pinned Flutter SDK's sky_engine package remains unresolved until its "
            "exact mixed engine license bundle is carried without reducing it to the "
            "Flutter framework's root BSD text."
        )
    elif dart_names:
        entry_label = "entry" if len(dart_names) == 1 else "entries"
        questions.append(
            f"{len(dart_names)} Dart package {entry_label} remains unresolved because no "
            "matching exact tracked evidence record was applied; see component records."
        )

    pod_names = unresolved["cocoapods"]
    if pod_names == {"Flutter", "SwiftProtobuf"}:
        questions.extend(
            [
                "The Flutter CocoaPod remains unresolved because the release path "
                "does not lock an exact Flutter SDK distribution.",
                "SwiftProtobuf 1.35.1 remains unresolved: its exact podspec declares "
                "the free-form type 'Apache 2.0' and references LICENSE.txt, but no "
                "exact license text is carried and this tooling does not guess an SPDX alias.",
            ]
        )
    elif pod_names:
        entry_label = "entry" if len(pod_names) == 1 else "entries"
        questions.append(
            f"{len(pod_names)} CocoaPods {entry_label} remains unresolved because no "
            "matching exact tracked evidence record was applied; see component records."
        )

    maven_names = unresolved["maven"]
    if maven_names:
        questions.append(
            f"Android release runtime license evidence remains unresolved for {len(maven_names)} Maven components. "
            "The Gradle snapshot establishes selected coordinates and artifact digests. "
            "Review the exact licenses and notices before publication."
        )

    if unresolved["flutter-asset"]:
        questions.append("Flutter MaterialIcons license and attribution evidence remains unresolved.")

    platformio_names = set().union(
        unresolved["platformio-framework"],
        unresolved["platformio-lib"],
        unresolved["platformio-platform"],
    )
    if "arduino" in unresolved["platformio-framework"]:
        questions.append(
            "The exact Arduino framework contains mixed-license source and precompiled SDK content. "
            "The selected firmware still needs a reviewed license bundle and corresponding source and relink delivery evidence."
        )
        platformio_names.discard("arduino")
    if unresolved["platformio-runtime"]:
        questions.append("The linked S3 GCC and Newlib runtime needs exact license and archive selection evidence.")
    if platformio_names:
        entry_label = "entry" if len(platformio_names) == 1 else "entries"
        questions.append(
            f"{len(platformio_names)} PlatformIO {entry_label} remains unresolved because no "
            "matching exact tracked evidence record was applied or no exact version is pinned; "
            "see component records."
        )
    return tuple(questions)


def _approved_license_metadata(
    evidence: EvidenceFile, expression: str, status: str,
) -> dict[str, object]:
    return {
        "license_expression": expression,
        "license_status": status,
        "evidence": {
            "path": evidence.path,
            "sha256": evidence.sha256,
            "role": evidence.role,
            "origin": evidence.origin,
        },
    }


def _separate_license_metadata(
    evidence: EvidenceFile, expression: str, scope: str,
) -> dict[str, object]:
    metadata = _approved_license_metadata(evidence, expression, f"resolved-approved-{scope}-license")
    metadata["source_location"] = LITCH_SOURCE_LOCATION
    return metadata


def inventory_to_json(inventory: Inventory) -> str:
    payload = {
        "tool_version": inventory.tool_version,
        "inventory_schema": inventory.inventory_schema,
        "sbom_schema": inventory.sbom_schema,
        "components": [_component_to_dict(component) for component in inventory.components],
        "unresolved_questions": list(inventory.unresolved_questions),
    }
    if inventory.project_license:
        payload["project_license"] = _approved_license_metadata(
            inventory.project_license, ROOT_LICENSE_EXPRESSION, "resolved-approved-root-license",
        )
    if inventory.hardware_license:
        payload["hardware_license"] = _separate_license_metadata(
            inventory.hardware_license, HARDWARE_LICENSE_EXPRESSION, "hardware",
        )
    if inventory.documentation_license:
        payload["documentation_license"] = _separate_license_metadata(
            inventory.documentation_license, DOCUMENTATION_LICENSE_EXPRESSION, "documentation",
        )
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def _component_to_dict(component: Component) -> dict[str, object]:
    payload = asdict(component)
    artifacts = payload.pop("runtime_artifacts")
    if artifacts:
        payload["runtime_artifacts"] = dict(artifacts)
    if component.notice_text_source is None:
        payload.pop("notice_text_source")
    for evidence in payload["license_evidence"]:
        evidence.pop("text", None)
    return payload


def sbom_to_json(inventory: Inventory) -> str:
    packages = []
    for component in inventory.components:
        properties = [
            {"name": "litch:ecosystem", "value": component.ecosystem},
            {"name": "litch:relationship", "value": component.relationship},
            {"name": "litch:source_type", "value": component.source_type},
            {"name": "litch:source_reference", "value": component.source_reference},
            {"name": "litch:license_status", "value": component.license_status},
            {
                "name": "litch:license_expression",
                "value": component.license_expression,
            },
            {"name": "litch:notice_disposition", "value": component.notice_disposition},
            {
                "name": "litch:evidence_paths",
                "value": ",".join(component.evidence_paths),
            },
        ]
        if component.lock_digest:
            properties.append(
                {
                    "name": "litch:lock_digest",
                    "value": (
                        f"{component.lock_digest_algorithm}:{component.lock_digest}"
                    ),
                }
            )
        if component.license_evidence:
            properties.append(
                {
                    "name": "litch:license_evidence",
                    "value": json.dumps(
                        [
                            {
                                "origin": evidence.origin,
                                "path": evidence.path,
                                "role": evidence.role,
                                "sha256": evidence.sha256,
                            }
                            for evidence in component.license_evidence
                        ],
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                }
            )
        if component.provenance_notes:
            properties.append(
                {
                    "name": "litch:provenance_notes",
                    "value": json.dumps(
                        list(component.provenance_notes), separators=(",", ":")
                    ),
                }
            )
        packages.append(
            {
                "bom-ref": f"{component.ecosystem}:{component.name}:{component.version}",
                "name": component.name,
                "version": component.version,
                "type": _sbom_type(component),
                "purl": component.package_url,
                "properties": properties,
            }
        )
    project_component: dict[str, object] = {"name": "litch", "type": "application", "version": "unreleased"}
    if inventory.project_license:
        project_component["license"] = _approved_license_metadata(
            inventory.project_license, ROOT_LICENSE_EXPRESSION, "resolved-approved-root-license",
        )
    metadata = {
        "tools": [{"name": "tools/release/release_inventory.py", "version": inventory.tool_version}],
        "component": project_component,
    }
    if inventory.hardware_license:
        metadata["hardware_license"] = _separate_license_metadata(
            inventory.hardware_license, HARDWARE_LICENSE_EXPRESSION, "hardware",
        )
    if inventory.documentation_license:
        metadata["documentation_license"] = _separate_license_metadata(
            inventory.documentation_license, DOCUMENTATION_LICENSE_EXPRESSION, "documentation",
        )
    payload = {
        "bomFormat": "Litch-SBOM",
        "specVersion": inventory.sbom_schema,
        "metadata": metadata,
        "components": packages,
        "annotations": [{"text": question} for question in inventory.unresolved_questions],
    }
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def notices_to_markdown(inventory: Inventory) -> str:
    sections = [
        "# Third-Party Notices",
        "",
        "This file is generated from tracked repository evidence by `tools/release/release_inventory.py`.",
        "It records what the repository can currently support and keeps unresolved items explicit.",
        "",
    ]
    if inventory.project_license:
        sections.extend([
            "## Original Litch software",
            "",
            "Foxie Products, LLC licenses its original app, firmware, tools and foxie_embedded "
            "under MPL 2.0. Third-party material retains its own terms. Hardware designs, artwork, "
            "branding, datasheets, general documentation and release notes have separate scope. "
            "[Canonical license and full scope](LICENSE)",
            "",
            f"- Exact license SHA-256 `{inventory.project_license.sha256}`",
            "",
        ])
        litch_max = next((component for component in inventory.components
                          if component.name == "Litch-Max"
                          and component.ecosystem == "first-party-library"), None)
        if litch_max is not None:
            source = litch_max.source_reference.replace("/tree/", "/blob/")
            sections.extend([
                "Litch-Max's original software is covered by its separate "
                f"[MPL 2.0 license and scope]({source}/LICENSE). "
                "Its nested upstream inputs retain their "
                f"[license]({source}/vendor/mcu-max/LICENSE) and "
                f"[reviewed attribution]({source}/docs/mcu_max_provenance.md#license-and-attribution).",
                "",
            ])
    if inventory.hardware_license:
        sections.extend([
            "## Original Litch hardware designs",
            "",
            "Foxie Products, LLC licenses the original functional hardware designs identified "
            "in the PCB and manufacturing documentation under CERN-OHL-S-2.0. "
            "[Canonical hardware license and full scope](LICENSE-HARDWARE)",
            "",
            f"- [Canonical source location]({LITCH_SOURCE_LOCATION})",
            f"- Exact license SHA-256 `{inventory.hardware_license.sha256}`",
            "",
        ])
    if inventory.documentation_license:
        sections.extend([
            "## Original Litch documentation and selected images",
            "",
            "Foxie Products, LLC licenses the original prose documentation and individually listed "
            "photographs and documentation previews identified in the scope notice under CC BY-SA 4.0. "
            "[Canonical documentation license and full scope](LICENSE-DOCUMENTATION)",
            "",
            f"- Exact license SHA-256 `{inventory.documentation_license.sha256}`",
            "",
        ])
    carried_texts: dict[str, dict[str, object]] = {}
    for component in inventory.components:
        if component.relationship.startswith(("vendored-internal", "submodule-internal")):
            continue
        sections.append(f"## {component.name}")
        sections.append("")
        sections.append(f"- Ecosystem: `{component.ecosystem}`")
        sections.append(f"- Version: `{component.version}`")
        sections.append(f"- Source evidence: `{', '.join(component.evidence_paths)}`")
        sections.append(f"- License status: `{component.license_status}`")
        sections.append(f"- Notice disposition: {component.notice_disposition}")
        if component.license_expression != "UNKNOWN":
            sections.append(f"- Tracked license expression/text status: `{component.license_expression}`")
        if component.lock_digest:
            sections.append(
                "- Locked package digest: "
                f"`{component.lock_digest_algorithm}:{component.lock_digest}`"
            )
        for evidence in component.license_evidence:
            sections.append(
                f"- {evidence.role.replace('-', ' ').title()} evidence: "
                f"`{evidence.path}` (SHA-256 `{evidence.sha256}`; "
                f"origin: {evidence.origin})"
            )
            if evidence.text is not None:
                group = carried_texts.setdefault(
                    evidence.sha256,
                    {
                        "text": evidence.text,
                        "components": set(),
                        "paths": set(),
                        "origins": set(),
                        "roles": set(),
                    },
                )
                if group["text"] != evidence.text:
                    raise ValueError(
                        f"Conflicting dependency evidence text for SHA-256 {evidence.sha256}"
                    )
                group["components"].add(
                    f"{component.ecosystem}:{component.name}@{component.version}"
                )
                group["paths"].add(evidence.path)
                group["origins"].add(evidence.origin)
                group["roles"].add(evidence.role)
        for note in component.provenance_notes:
            sections.append(f"- Note: {note}")
        if component.notice_text:
            license_evidence = component.notice_text_source or next(
                (
                    path
                    for path in component.evidence_paths
                    if Path(path).name.upper() == "LICENSE"
                ),
                None,
            )
            if license_evidence is None:
                raise ValueError(
                    f"{component.name} includes notice text without a tracked LICENSE evidence path"
                )
            provenance = (
                f"Reviewed license excerpts from `{license_evidence}`."
                if component.notice_text_source else
                "Generator provenance: extracted verbatim from the tracked "
                f"`{license_evidence}` evidence path."
            )
            sections.extend(
                [
                    "",
                    "### Included License Text",
                    "",
                    provenance,
                    *([] if component.notice_text_source else [
                        "Test provenance: `tools/release/tests/test_release_inventory.py` verifies this text is emitted unchanged."
                    ]),
                    "",
                    component.notice_text,
                ]
            )
        sections.append("")
    if carried_texts:
        sections.extend(
            [
                "## Carried Dependency License And Notice Texts",
                "",
                "Identical raw evidence is emitted once and linked to every applicable component above.",
                "This Markdown display normalizes line endings and trailing whitespace; "
                "the linked evidence paths preserve the exact hashed source bytes.",
                "",
            ]
        )
        for evidence_sha256, group in sorted(carried_texts.items()):
            sections.append(f"### SHA-256 `{evidence_sha256}`")
            sections.append("")
            sections.append(
                "- Applies to: "
                + ", ".join(f"`{name}`" for name in sorted(group["components"]))
            )
            sections.append(
                "- Evidence files: "
                + ", ".join(f"`{path}`" for path in sorted(group["paths"]))
            )
            sections.append(
                "- Roles: "
                + ", ".join(f"`{role}`" for role in sorted(group["roles"]))
            )
            for origin in sorted(group["origins"]):
                sections.append(f"- Origin: {origin}")
            sections.extend(["", _markdown_display_text(group["text"]), ""])
    sections.append("## Unresolved Review Items")
    sections.append("")
    for question in inventory.unresolved_questions:
        sections.append(f"- {question}")
    sections.append("")
    return "\n".join(sections)


def _markdown_display_text(text: str) -> str:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = []
    for line in normalized.split("\n"):
        content = line.lstrip(" \t")
        indent = line[:len(line) - len(content)]
        if " \t" in indent:
            line = indent.expandtabs(4) + content
        lines.append(line.rstrip(" \t"))
    return "\n".join(lines)


def app_native_notices_to_text(inventory: Inventory) -> str:
    dart_digests = {
        component.name: {evidence.sha256 for evidence in component.license_evidence if evidence.text is not None}
        for component in inventory.components if component.ecosystem == "dart-pub"
    }
    texts: dict[str, set[str]] = {}
    separator = "\n" + "-" * 80 + "\n"

    def add(label: str, body: str, source: str) -> None:
        text = body.replace("\r\n", "\n").replace("\r", "\n").strip("\n")
        if not text.strip() or separator in text:
            raise ValueError(f"Invalid Flutter notice block in {source}")
        texts.setdefault(text, set()).add(label)

    for component in inventory.components:
        if component.ecosystem not in {"cocoapods", "flutter-asset", "maven", "platformio-platform"}:
            continue
        if component.ecosystem == "maven" and not component.runtime_artifacts:
            continue
        already_carried = set()
        if component.ecosystem == "cocoapods":
            already_carried = dart_digests.get(component.name, set())
            if component.name == "Flutter":
                already_carried = dart_digests.get("flutter", set())
        elif component.ecosystem == "maven" and component.name.startswith("io.flutter:"):
            already_carried = dart_digests.get("sky_engine", set())
        label = f"{component.name} {component.version}"
        if component.ecosystem == "platformio-platform":
            label = f"Build tools — {label}"
        for evidence in component.license_evidence:
            if evidence.text is not None and evidence.sha256 not in already_carried:
                add(label, evidence.text, evidence.path)
    for label, text in inventory.board_notices:
        add(label, text, label)
    if inventory.board_notices:
        add(
            "Litch licensing overview",
            f"Canonical source location\n{LITCH_SOURCE_LOCATION}\n\n" +
            ("Original software\n"
             "The Litch entry contains the canonical MPL 2.0 grant and scope for Foxie Products, LLC’s "
             "original software.\n\n" if inventory.project_license else "") +
            ("Hardware designs\n"
             "The Litch hardware designs entry contains CERN-OHL-S-2.0 for the original functional "
             "PCB and mechanical designs and their identified manufacturing exports.\n\n"
             if inventory.hardware_license else "") +
            ("Documentation and images\n"
             "The Litch documentation and images entry contains CC BY-SA 4.0 for the original prose "
             "identified in its scope notice and individually listed photographs and documentation previews. "
             "The original hardware design files are covered by the separate hardware license.\n\n"
             if inventory.documentation_license else "") +
            "Mobile app\n"
            "Flutter’s package and SDK notice bundles include Apache, BSD, MIT, the Mozilla Public License 2.0, "
            "Unicode and ICU terms, Boost Software License 1.0, zlib, and the LLVM exception. "
            "The SDK notices cover multiple platforms. SwiftProtobuf’s runtime exception and "
            "Material Icons’ CC BY 4.0 terms appear under their own entries.\n\n"
            "Board firmware\n"
            "Board firmware entries carry the notices for the selected ESP32-S3 image, including Arduino’s "
            "LGPL 2.1 or later, NeoPixelBus’s LGPL 3.0 or later and accompanying GPL text, "
            "Newlib and GCC runtime terms, and the retained micro-Max and SparkFun terms.\n\n"
            "Build tools\n"
            "Build tools entries identify the PlatformIO platforms used to build and test Litch. "
            "Each entry gives the applicable text and attribution for its scope.",
            "Litch licensing overview",
        )
    blocks = ["\n".join(sorted(labels)) + "\n\n" + text for text, labels in texts.items()]
    for label, evidence in (
        ("Litch", inventory.project_license),
        ("Litch hardware designs", inventory.hardware_license),
        ("Litch documentation and images", inventory.documentation_license),
    ):
        if evidence is None:
            continue
        if evidence.text is None or separator in evidence.text:
            raise ValueError(f"Invalid Flutter notice block in {evidence.path}")
        blocks.append(label + "\n\n" + evidence.text)
    return separator.join(sorted(blocks)) + ("\n" if blocks else "")


def inventory_to_markdown(inventory: Inventory) -> str:
    lines = [
        "# Public Release Inventory",
        "",
        f"- Tool version: `{inventory.tool_version}`",
        f"- Inventory schema: `{inventory.inventory_schema}`",
        f"- SBOM schema: `{inventory.sbom_schema}`",
    ]
    if inventory.project_license:
        lines.append(
            f"- Original software license `{ROOT_LICENSE_EXPRESSION}` "
            f"([canonical LICENSE](../../../LICENSE), SHA-256 `{inventory.project_license.sha256}`)"
        )
    if inventory.hardware_license:
        lines.append(
            f"- Original hardware design license `{HARDWARE_LICENSE_EXPRESSION}` "
            f"([canonical LICENSE-HARDWARE](../../../LICENSE-HARDWARE), SHA-256 `{inventory.hardware_license.sha256}`)"
        )
    if inventory.documentation_license:
        lines.append(
            f"- Original documentation and listed-image license `{DOCUMENTATION_LICENSE_EXPRESSION}` "
            f"([canonical LICENSE-DOCUMENTATION](../../../LICENSE-DOCUMENTATION), "
            f"SHA-256 `{inventory.documentation_license.sha256}`)"
        )
    lines.extend([
        "",
        "| Ecosystem | Name | Version | Relationship | License status | Evidence |",
        "| --- | --- | --- | --- | --- | --- |",
    ])
    for component in inventory.components:
        lines.append(
            "| {ecosystem} | {name} | {version} | {relationship} | {license_status} | {evidence} |".format(
                ecosystem=component.ecosystem,
                name=component.name,
                version=component.version,
                relationship=component.relationship,
                license_status=component.license_status,
                evidence=", ".join(component.evidence_paths),
            )
        )
    lines.extend(["", "## Unresolved Questions", ""])
    for question in inventory.unresolved_questions:
        lines.append(f"- {question}")
    lines.append("")
    return "\n".join(lines)


def write_outputs(
    repo_root: Path, output_dir: Path, notices_path: Path | None = None,
    app_notices_path: Path | None = None,
) -> list[Path]:
    inventory = build_inventory(repo_root)
    if notices_path is None and output_dir != repo_root / "docs/release/generated":
        notices_path = output_dir / "THIRD_PARTY_NOTICES.md"
    if app_notices_path is None:
        if notices_path is not None:
            app_notices_path = notices_path.with_name(APP_NATIVE_NOTICES_RELATIVE.name)
        elif output_dir != repo_root / "docs/release/generated":
            app_notices_path = output_dir / APP_NATIVE_NOTICES_RELATIVE.name
        else:
            app_notices_path = repo_root / APP_NATIVE_NOTICES_RELATIVE
    outputs = {
        output_dir / "dependency_inventory.json": inventory_to_json(inventory),
        output_dir / "dependency_inventory.md": inventory_to_markdown(inventory),
        output_dir / "release_sbom.json": sbom_to_json(inventory),
        (notices_path or repo_root / "THIRD_PARTY_NOTICES.md"): notices_to_markdown(
            inventory
        ),
        app_notices_path: app_native_notices_to_text(inventory),
    }
    for path, content in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    return sorted(outputs, key=lambda path: path.as_posix())


def _iter_platformio_list(raw_value: str) -> list[str]:
    return [line.strip() for line in raw_value.splitlines() if line.strip()]


def _split_platformio_pin(entry: str) -> tuple[str, str]:
    if "@" in entry:
        name, version = entry.split("@", 1)
        return name.strip(), version.strip()
    return entry.strip(), "unresolved"


def _merge_platformio_component(
    aggregated: dict[tuple[str, str], dict[str, object]],
    *,
    key: tuple[str, str],
    component: Component,
) -> None:
    existing = aggregated.get(key)
    if existing is None:
        aggregated[key] = {
            "ecosystem": component.ecosystem,
            "name": component.name,
            "version": component.version,
            "relationships": {component.relationship},
            "source_type": component.source_type,
            "source_reference": component.source_reference,
            "evidence_paths": set(component.evidence_paths),
            "license_expression": component.license_expression,
            "license_status": component.license_status,
            "notice_disposition": component.notice_disposition,
            "provenance_notes": list(component.provenance_notes),
            "package_url": component.package_url,
        }
        return
    immutable_fields = (
        "ecosystem",
        "name",
        "version",
        "source_type",
        "source_reference",
        "license_expression",
        "license_status",
        "notice_disposition",
        "package_url",
    )
    conflicts = [
        field_name
        for field_name in immutable_fields
        if existing[field_name] != getattr(component, field_name)
    ]
    if conflicts:
        raise ValueError(
            f"Conflicting PlatformIO declarations for {component.name}: "
            + ", ".join(conflicts)
        )
    existing["relationships"].add(component.relationship)
    existing["evidence_paths"].update(component.evidence_paths)


def _sbom_type(component: Component) -> str:
    if component.ecosystem.startswith("first-party"):
        return "library"
    if component.ecosystem in {"dart-pub", "cocoapods", "maven"}:
        return "library"
    if component.ecosystem == "flutter-asset":
        return "file"
    return "firmware"


def _component_sort_key(component: Component) -> tuple[str, str, str, str]:
    return (
        component.ecosystem,
        component.name.lower(),
        component.version,
        component.relationship,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        default=None,
        help=(
            "Directory for generated release inventory artifacts "
            "(defaults under --repo-root)."
        ),
    )
    parser.add_argument(
        "--repo-root",
        default=str(REPO_ROOT),
        help="Repository root containing tracked manifests and vendored sources.",
    )
    parser.add_argument(
        "--notices-path",
        default=None,
        help="Explicit path for THIRD_PARTY_NOTICES.md (defaults under --repo-root).",
    )
    parser.add_argument("--app-notices-path", help="Explicit Flutter native runtime notice output path")
    args = parser.parse_args()
    repo_root = Path(args.repo_root).resolve()
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir is not None
        else repo_root / "docs" / "release" / "generated"
    )
    notices_path = Path(args.notices_path).resolve() if args.notices_path else None
    if notices_path is None and args.output_dir:
        notices_path = output_dir / "THIRD_PARTY_NOTICES.md"
    app_notices_path = Path(args.app_notices_path).resolve() if args.app_notices_path else None
    if app_notices_path is None and args.output_dir:
        app_notices_path = output_dir / APP_NATIVE_NOTICES_RELATIVE.name
    for path in write_outputs(repo_root, output_dir, notices_path, app_notices_path):
        print(_display_output_path(path, repo_root))
    return 0


def _display_output_path(path: Path, repo_root: Path) -> str:
    """Render a useful path even when an explicitly requested output is external."""
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


if __name__ == "__main__":
    raise SystemExit(main())
