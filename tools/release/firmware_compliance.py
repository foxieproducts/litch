#!/usr/bin/env python3
"""Prepare and admit the bounded MPL/LGPL source/relink asset for one S3 image."""

from __future__ import annotations

import gzip
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zlib

import firmware_artifact
import relink_firmware


MAX_BUNDLE_SIZE = 64 * 1024 * 1024
MAX_EXPANDED_SIZE = 256 * 1024 * 1024
MAX_MEMBERS = 12000
MAX_MANIFEST_SIZE = 4 * 1024 * 1024
MAX_LICENSE_SIZE = 256 * 1024
MAX_TAR_SIZE = MAX_EXPANDED_SIZE + MAX_MEMBERS * 1024 + 10240
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
SOURCE_SHA = re.compile(r"[0-9a-f]{40}\Z")
PRIVATE_KEY = re.compile(rb"-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\r?\n[A-Za-z0-9]")
PEM_BLOCK = re.compile(rb"-----BEGIN ((?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY)-----\r?\n.+?-----END \1-----", re.DOTALL)
# Exact public certs.c fixtures from Mbed TLS 2b8e772fc1cb0732cda3bae7d1e9d6f4cfaf63d9.
# Source SHA-256: a11c0dc43dea7addc8efa255bcfebedd2d5f33a78c83342a182598ab8b14f291.
MBEDTLS_ARCHIVE_SHA256 = "3d56277224b066118b6c48a973ae18fdf8078448bf54b482ac5557699e52f223"
PUBLIC_TEST_KEY_SHA256 = frozenset({
    "660d13287799f69599adf0aff1db46ee42b4c492b0f153f620f89a639dcb8f7d",
    "c14642ce0c12abd0f9d0b2cb92010abca23bd8f8ee674163750f3c5179c6bf6a",
    "74d956f51bb9765928ee0b0e72eca06b536edf1c8544e6c77d1d776a6aea286e",
    "a0b6fd2ac7e0b3946df9c6a24353cc4da5a8e9f40c8feea428773604231e6b7a",
    "145e105c1386fe6404119b0073f1fc9e5df78dbf0d10a447b710ed94b8cd99be",
    "f305b97578b0afe7afa39be4f2e9cec557a22d4fa28b70561f1f60086cd2a1d0",
})
SOURCE_POLICY_PATH = Path(__file__).with_name("firmware_relink_sources.json")
LICENSE_POLICY_PATH = Path(__file__).with_name("firmware_license_manifest.json")
LITCH_MAX_LEGAL_PATHS = frozenset({
    "lib/litch-max/LICENSE", "lib/litch-max/vendor/mcu-max/LICENSE",
    "lib/litch-max/docs/mcu_max_provenance.md",
})
REQUIRED_FILES = frozenset({
    "README.md", "relink_firmware.py", "firmware_artifact.py", "relink.json",
    "notices/NOTICE.md", "notices/LGPL-2.1.txt", "notices/LGPL-3.0.txt", "notices/GPL-3.0.txt",
    "notices/LITCH-LICENSE.txt",
    "notices/LITCH-MAX-LICENSE.txt",
    "notices/mcu_max_provenance.md",
    "notices/litch-max.snapshot.json",
    "notices/opening_book_baseline.json",
    "INSTALL.md", "installation/otadata.bin",
})


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def reject_private_material(name: str, data: bytes) -> None:
    if name == "objects/sdk/lib/libmbedx509.a":
        for match in PEM_BLOCK.finditer(data):
            if digest(match[0].replace(b"\r\n", b"\n")) not in PUBLIC_TEST_KEY_SHA256:
                raise ValueError("unknown private key in SDK input")
        data = PEM_BLOCK.sub(b"", data)
    if (PRIVATE_KEY.search(data) or b"/Users/" in data or b"/home/" in data
            or re.search(rb"AKIA[0-9A-Z]{16}", data)):
        raise ValueError("private key or host-path material in compliance input")


def safe_member(name: str) -> bool:
    path = PurePosixPath(name)
    contained = (bool(name) and not path.is_absolute() and path.as_posix() == name
            and all(part not in (".", "..", ".git", ".aws", ".env") for part in path.parts)
            and "\\" not in name and not any(ord(c) < 32 or ord(c) == 127 for c in name))
    if not contained:
        return False
    if name in REQUIRED_FILES | {"bundle.json"}:
        return True
    if name.startswith("objects/"):
        return path.suffix in (".o", ".a")
    if name.startswith("notices/"):
        return path.suffix in (".md", ".txt", ".json")
    if name.startswith("sdk/"):
        return path.suffix in (".h", ".hpp", ".inc", ".ld")
    if name == "source/project/variants/litch_esp32s3/pins_arduino.h":
        return True
    if name.startswith("source/project/"):
        policy = json.loads(SOURCE_POLICY_PATH.read_text())
        return name.removeprefix("source/project/") in policy["first_party_files"] + policy["vendor_files"]
    if name.startswith("source/opening_book_baseline/"):
        policy = json.loads(SOURCE_POLICY_PATH.read_text())
        return name.removeprefix("source/opening_book_baseline/") in policy["opening_book_baseline"]["files"]
    if name.startswith(("source/libraries/ArduinoJson/src/", "source/libraries/NimBLE-Arduino/src/")):
        return path.suffix in (".h", ".hpp", ".inc")
    source_roots = ("source/arduino/cores/esp32/", "source/libraries/NeoPixelBus/src/", *(
        f"source/arduino/libraries/{library}/src/"
        for library in ("FS", "Wire", "SPI", "Update", "LittleFS")))
    return name.startswith(source_roots) and (path.suffix in (
        ".h", ".hpp", ".c", ".cpp", ".S", ".s", ".inc") or path.name == "LICENSE")


def canonical_json(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def litch_max_legal_records() -> dict[str, dict]:
    legal = json.loads(LICENSE_POLICY_PATH.read_text())
    records = {row["path"]: row for row in legal["legal_files"]
               if row["path"] in LITCH_MAX_LEGAL_PATHS}
    if records.keys() != LITCH_MAX_LEGAL_PATHS:
        raise ValueError("trusted Litch-Max license policy is incomplete")
    return records


def validate_license_text(data: bytes) -> None:
    if not 0 < len(data) <= MAX_LICENSE_SIZE:
        raise ValueError("root LICENSE must contain bounded UTF-8 text")
    try:
        value = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("root LICENSE must contain UTF-8 text") from error
    if not value.strip() or "\x00" in value:
        raise ValueError("root LICENSE must contain nonempty text")


def source_owner(root: Path, source_sha: str, relative: str) -> tuple[Path, str, str]:
    """Resolve engine blobs through the selected superproject gitlink."""
    if relative.startswith("lib/litch-max/"):
        record = subprocess.run(["git", "ls-tree", "-z", source_sha, "--", "lib/litch-max"],
                                cwd=root, check=True, capture_output=True).stdout
        fields = record.removesuffix(b"\x00").split(b"\t")
        metadata = fields[0].split()
        if (len(fields) == 2 and fields[1] == b"lib/litch-max" and len(metadata) == 3
                and metadata[:2] == [b"160000", b"commit"]):
            owner = root / "lib/litch-max"
            if owner.is_symlink() or not owner.is_dir():
                raise ValueError("selected Litch-Max submodule is unavailable")
            return owner, metadata[2].decode("ascii"), relative.removeprefix("lib/litch-max/")
    return root, source_sha, relative


def source_bytes(root: Path, source_sha: str, relative: str) -> bytes:
    owner, revision, name = source_owner(root, source_sha, relative)
    return subprocess.run(["git", "show", f"{revision}:{name}"], cwd=owner,
                          check=True, capture_output=True).stdout


def source_license(root: Path, source_sha: str, relative: str = "LICENSE") -> bytes:
    """Return the exact candidate's regular license, never an uncommitted grant."""
    if relative not in ("LICENSE", "lib/litch-max/LICENSE"):
        raise ValueError("unapproved first-party license path")
    label = "root LICENSE" if relative == "LICENSE" else "Litch-Max LICENSE"
    if not SOURCE_SHA.fullmatch(source_sha):
        raise ValueError(f"{label} requires a complete source commit identity")
    path = root / relative
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"source-bound {label} is required before firmware packaging")
    with path.open("rb") as stream:
        working = stream.read(MAX_LICENSE_SIZE + 1)
    validate_license_text(working)
    owner, revision, name = source_owner(root, source_sha, relative)
    record = subprocess.run(["git", "ls-tree", "-z", revision, "--", name],
                            cwd=owner, check=True, capture_output=True).stdout
    fields = record.removesuffix(b"\x00").split(b"\t")
    metadata = fields[0].split()
    if (len(fields) != 2 or fields[1] != name.encode() or len(metadata) != 3
            or metadata[0] not in (b"100644", b"100755") or metadata[1] != b"blob"):
        raise ValueError(f"{label} must be a regular file in the selected source commit")
    blob = metadata[2].decode("ascii")
    size = int(subprocess.run(["git", "cat-file", "-s", blob], cwd=owner, check=True,
                              capture_output=True).stdout)
    if not 0 < size <= MAX_LICENSE_SIZE:
        raise ValueError(f"committed {label} exceeds its text bound")
    original = subprocess.run(["git", "cat-file", "blob", blob], cwd=owner, check=True,
                              capture_output=True).stdout
    validate_license_text(original)
    if original != working:
        raise ValueError(f"{label} differs from the selected Git source")
    return original


def source_snapshot(root: Path, source_sha: str) -> bytes:
    root = root.resolve()
    if not SOURCE_SHA.fullmatch(source_sha):
        raise ValueError("Litch-Max snapshot requires a complete source commit identity")
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pio"))
    from verify_litch_max import verify_snapshot
    data = canonical_json(verify_snapshot(root, source_sha))
    if not 0 < len(data) <= MAX_MANIFEST_SIZE:
        raise ValueError("Litch-Max snapshot exceeds its admission bound")
    return data


def installation_selector() -> bytes:
    # Use the same selector owner as tools/dev/rom_flash.py, with no device I/O.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dev"))
    import ota_trial
    return (ota_trial.make_selector_sector(1, ota_trial.ESP_OTA_IMG_VALID)
            + b"\xff" * ota_trial.OTA_DATA_SECTOR_SIZE)


def opening_book_inputs(root: Path, source_sha: str) -> dict[str, bytes]:
    """Supply only the reviewed historical inputs of the authored opening table."""
    baseline = json.loads(SOURCE_POLICY_PATH.read_text())["opening_book_baseline"]
    if not SOURCE_SHA.fullmatch(source_sha):
        raise ValueError("historical source requires the admitted source commit")

    def admitted(relative: str) -> bytes:
        path = root / relative
        if (path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root)
                or path.stat().st_size > MAX_MANIFEST_SIZE):
            raise ValueError("invalid historical source input")
        data = path.read_bytes()
        original = source_bytes(root, source_sha, relative)
        if data != original:
            raise ValueError("historical source differs from admitted Git source")
        return data

    manifest = json.loads(admitted(baseline["manifest"]))
    if (set(manifest) != {"head", "files"} or manifest["head"] != baseline["source_sha"]
            or not isinstance(manifest["files"], dict)
            or set(manifest["files"]) != set(baseline["files"])
            or digest(canonical_json(manifest)) != baseline["manifest_sha256"]):
        raise ValueError("historical source manifest differs from reviewed baseline")
    files = {}
    hashes = {}
    for relative in baseline["files"]:
        name = "source/opening_book_baseline/" + relative
        if not safe_member(name):
            raise ValueError("unapproved opening-book baseline source")
        data = admitted(baseline["local_root"] + "/" + relative)
        if digest(data) != manifest["files"][relative]:
            raise ValueError("historical source bytes differ from reviewed baseline")
        reject_private_material(name, data)
        files[name] = data
        hashes[relative] = digest(data)
    files["notices/opening_book_baseline.json"] = canonical_json({
        "head": baseline["source_sha"], "files": hashes})
    return files


def legal_inputs(root: Path, manifest: Path, evidence_root: Path) -> tuple[dict, dict[str, bytes]]:
    legal = json.loads(manifest.read_text())
    if legal.get("schema_version") != 1 or legal.get("target") != "litch-esp32s3":
        raise ValueError("license manifest must describe the shipping S3 schema")
    for required in litch_max_legal_records().values():
        if required not in legal["legal_files"]:
            raise ValueError("license manifest omits or changes required Litch-Max terms")
    contents = {}
    for item in legal["legal_files"]:
        relative = PurePosixPath(item["path"])
        if relative.is_absolute() or ".." in relative.parts or str(relative) in contents:
            raise ValueError("invalid or repeated license evidence path")
        path = evidence_root / relative
        if not path.exists() and evidence_root != root:
            path = root / relative
        if (path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_MANIFEST_SIZE
                or not (path.resolve().is_relative_to(root) or path.resolve().is_relative_to(evidence_root))):
            raise ValueError("license evidence path escapes its owner")
        data = path.read_bytes()
        if digest(data) != item["sha256"]:
            raise ValueError(f"license evidence bytes changed for {relative}")
        contents[str(relative)] = data
    for field in ("framework_selection", "runtime_selection"):
        if legal[field] not in contents:
            raise ValueError("selected archive evidence is not hash bound")
    return legal, contents


def runtime_archives(selection: dict, installed: dict, contents: dict[str, bytes]) -> list[dict]:
    if (selection.get("package") != "espressif/toolchain-xtensa-esp32s3"
            or selection.get("version") != "8.4.0+2021r2-patch5"
            or installed.get("name") != "toolchain-xtensa-esp32s3"
            or installed.get("version") != selection["version"]):
        raise ValueError("runtime toolchain package identity differs from reviewed evidence")
    system = installed.get("system")
    if not isinstance(system, str) or system not in selection["variants"]:
        raise ValueError("runtime toolchain host variant is not reviewed")
    variant = selection["variants"][system]
    if (variant["package_metadata"] not in contents
            or ("distribution_provenance" in variant and variant["distribution_provenance"] not in contents)):
        raise ValueError("runtime host provenance is not hash bound")
    metadata = json.loads(contents[variant["package_metadata"]])
    if any(installed.get(key) != metadata.get(key) for key in ("name", "version", "system")):
        raise ValueError("runtime host metadata does not match the installed package")
    return variant["archives"]


def write_bundle(files: dict[str, bytes], source_sha: str, firmware_sha256: str,
                 original_elf_sha256: str, output: Path) -> Path:
    """Write deterministic regular-file members; never archive a directory tree."""
    if "bundle.json" in files or any(not safe_member(name) for name in files):
        raise ValueError("invalid compliance member name")
    manifest = {
        "schema_version": 1, "target": "esp32s3", "source_sha": source_sha,
        "firmware_sha256": firmware_sha256, "original_elf_sha256": original_elf_sha256,
        "files": {name: {"sha256": digest(data), "size_bytes": len(data)}
                  for name, data in sorted(files.items())},
    }
    members = {**files, "bundle.json": canonical_json(manifest)}
    if len(members) > MAX_MEMBERS or sum(map(len, members.values())) > MAX_EXPANDED_SIZE:
        raise ValueError("compliance bundle exceeds expanded bounds")
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", filename="", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for name, data in sorted(members.items()):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o644
                archive.addfile(member, io.BytesIO(data))
    data = buffer.getvalue()
    if len(data) > MAX_BUNDLE_SIZE:
        raise ValueError("compliance bundle exceeds compressed bound")
    output.mkdir(parents=True, exist_ok=True)
    path = output / f"firmware_compliance_{digest(data)}.tar.gz"
    with tempfile.TemporaryDirectory(prefix=".compliance-", dir=output) as temporary:
        staged = Path(temporary) / path.name
        staged.write_bytes(data)
        inspect_bundle(staged, firmware_sha256, source_sha)
        # No overwrite, even if another process creates this final name.
        os.link(staged, path)
    return path


def inspect_bundle(path: Path, expected_firmware_sha256: str,
                   expected_source_sha: str, *,
                   expected_license_sha256: str | None = None,
                   expected_litch_max_license_sha256: str | None = None,
                   expected_litch_max_snapshot_sha256: str | None = None) -> dict[str, object]:
    """Validate exact bounded asset identity, paths, inventory and every member."""
    path = Path(path)
    if not SHA256.fullmatch(expected_firmware_sha256) or not SOURCE_SHA.fullmatch(expected_source_sha):
        raise ValueError("invalid compliance source or firmware identity")
    if expected_license_sha256 is not None and not SHA256.fullmatch(expected_license_sha256):
        raise ValueError("invalid expected root LICENSE identity")
    if (expected_litch_max_license_sha256 is not None
            and not SHA256.fullmatch(expected_litch_max_license_sha256)):
        raise ValueError("invalid expected Litch-Max LICENSE identity")
    if (expected_litch_max_snapshot_sha256 is not None
            and not SHA256.fullmatch(expected_litch_max_snapshot_sha256)):
        raise ValueError("invalid expected Litch-Max snapshot identity")
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_BUNDLE_SIZE:
        raise ValueError("compliance bundle must be a bounded regular file")
    data = path.read_bytes()
    asset_sha = digest(data)
    if path.name != f"firmware_compliance_{asset_sha}.tar.gz":
        raise ValueError("compliance filename must match its SHA-256")
    observed = {}
    selector = None
    opening_manifest = None
    library_snapshot = None
    manifest = None
    expanded = 0
    try:
        if data[:10] != b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff":
            raise ValueError("compliance gzip header must not carry ancillary metadata")
        compressed = zlib.decompressobj(wbits=31)
        tar_data = compressed.decompress(data, MAX_TAR_SIZE + 1)
        if (not compressed.eof or compressed.unused_data or compressed.unconsumed_tail
                or len(tar_data) > MAX_TAR_SIZE or len(tar_data) % 512):
            raise ValueError("compliance archive exceeds total expanded bounds or is truncated")
        offset = 0
        while offset + 512 <= len(tar_data):
            header = tar_data[offset:offset + 512]
            if not any(header):
                if len(tar_data) - offset < 1024 or any(tar_data[offset:]):
                    raise ValueError("compliance archive has hidden trailing content")
                break
            if header[257:265] != b"ustar\x0000" or header[156:157] != b"0":
                raise ValueError("only ordinary USTAR files are allowed")
            member = tarfile.TarInfo.frombuf(header, "utf-8", "strict")
            if member.tobuf(format=tarfile.USTAR_FORMAT, encoding="utf-8", errors="strict") != header:
                raise ValueError("noncanonical compliance header metadata")
            offset += 512
            if (not member.isfile() or not safe_member(member.name)
                    or member.name in observed or len(observed) >= MAX_MEMBERS
                    or member.linkname or member.uid or member.gid or member.mtime
                    or member.uname or member.gname):
                raise ValueError("invalid or repeated compliance archive member")
            expanded += member.size
            if member.size < 0 or expanded > MAX_EXPANDED_SIZE:
                raise ValueError("compliance archive exceeds expanded bound")
            if member.name == "bundle.json" and member.size > MAX_MANIFEST_SIZE:
                raise ValueError("compliance manifest is too large")
            end = offset + member.size
            padded_end = end + (-member.size % 512)
            if padded_end > len(tar_data) or any(tar_data[end:padded_end]):
                raise ValueError("truncated compliance member or nonzero padding")
            content = tar_data[offset:end]
            offset = padded_end
            reject_private_material(member.name, content)
            observed[member.name] = {"sha256": digest(content), "size_bytes": len(content)}
            if member.name == "bundle.json":
                manifest = json.loads(content)
                if canonical_json(manifest) != content:
                    raise ValueError("compliance manifest must be canonical JSON")
            elif member.name == "installation/otadata.bin":
                selector = content
            elif member.name == "notices/opening_book_baseline.json":
                opening_manifest = json.loads(content)
            elif member.name == "notices/litch-max.snapshot.json":
                if (len(content) > MAX_MANIFEST_SIZE or
                        (expected_litch_max_snapshot_sha256 is not None
                         and digest(content) != expected_litch_max_snapshot_sha256)):
                    raise ValueError("bundle Litch-Max snapshot differs from the selected Git source")
                library_snapshot = json.loads(content)
            elif member.name == "notices/LITCH-LICENSE.txt":
                validate_license_text(content)
                if expected_license_sha256 is not None and digest(content) != expected_license_sha256:
                    raise ValueError("bundle root LICENSE differs from the selected Git source")
            elif member.name == "notices/LITCH-MAX-LICENSE.txt":
                validate_license_text(content)
                if (expected_litch_max_license_sha256 is not None
                        and digest(content) != expected_litch_max_license_sha256):
                    raise ValueError("bundle Litch-Max LICENSE differs from the selected Git source")
        else:
            raise ValueError("compliance archive lacks its end markers")
    except (tarfile.TarError, zlib.error, OSError, EOFError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("malformed compliance archive") from error
    if (not isinstance(manifest, dict) or set(manifest) != {
            "schema_version", "target", "source_sha", "firmware_sha256",
            "original_elf_sha256", "files"}
            or manifest["schema_version"] != 1 or manifest["target"] != "esp32s3"
            or manifest["source_sha"] != expected_source_sha
            or manifest["firmware_sha256"] != expected_firmware_sha256
            or not isinstance(manifest["original_elf_sha256"], str)
            or not SHA256.fullmatch(manifest["original_elf_sha256"])):
        raise ValueError("compliance manifest does not match the admitted S3 source/image")
    observed.pop("bundle.json", None)
    required_notices = litch_max_legal_records()
    for relative in LITCH_MAX_LEGAL_PATHS - {"lib/litch-max/LICENSE"}:
        expected_hash = required_notices[relative]["sha256"]
        names = ["notices/" + expected_hash + ".txt"]
        if relative.endswith("mcu_max_provenance.md"):
            names.append("notices/mcu_max_provenance.md")
        if any(observed.get(name, {}).get("sha256") != expected_hash for name in names):
            raise ValueError("bundle omits or changes required Litch-Max vendor notices")
    library_hash = observed.get("notices/LITCH-MAX-LICENSE.txt", {}).get("sha256")
    if (not library_hash or observed.get("notices/" + library_hash + ".txt", {}).get("sha256")
            != library_hash):
        raise ValueError("bundle Litch-Max license notice differs from its source grant")
    policy = json.loads(SOURCE_POLICY_PATH.read_text())
    if (not isinstance(library_snapshot, dict) or library_snapshot.get("schema") != 1
            or not isinstance(library_snapshot.get("files"), dict)):
        raise ValueError("bundle lacks a valid Litch-Max source snapshot")
    library_files = library_snapshot["files"]
    for relative in policy["first_party_files"] + policy["vendor_files"]:
        if relative.startswith("lib/litch-max/"):
            record = library_files.get(relative.removeprefix("lib/litch-max/"), {})
            expected_hash = record.get("sha256") if isinstance(record, dict) else None
            if (not isinstance(expected_hash, str) or not SHA256.fullmatch(expected_hash)
                    or observed.get("source/project/" + relative, {}).get("sha256") != expected_hash):
                raise ValueError("bundle Litch-Max source differs from its pinned snapshot")
    expected_project = {"source/project/" + name
                        for name in policy["first_party_files"] + policy["vendor_files"]}
    observed_project = {name for name in observed if name.startswith("source/project/")}
    expected_baseline = {"source/opening_book_baseline/" + name
                         for name in policy["opening_book_baseline"]["files"]}
    observed_baseline = {name for name in observed if name.startswith("source/opening_book_baseline/")}
    expected_opening_manifest = {"head": policy["opening_book_baseline"]["source_sha"],
        "files": {name.removeprefix("source/opening_book_baseline/"): observed[name]["sha256"]
                  for name in observed_baseline}}
    if (observed != manifest["files"] or not REQUIRED_FILES <= observed.keys()
            or not any(name.startswith("source/arduino/") for name in observed)
            or "source/libraries/NeoPixelBus/src/NeoPixelBus.h" not in observed
            or expected_project != observed_project or expected_baseline != observed_baseline
            or opening_manifest != expected_opening_manifest
            or digest(canonical_json(opening_manifest)) != policy["opening_book_baseline"]["manifest_sha256"]
            or selector != installation_selector()
            or not any(name.startswith("objects/") for name in observed)):
        raise ValueError("compliance members do not match the complete declared inventory")
    return {"filename": path.name, "size_bytes": len(data), "sha256": asset_sha,
            "firmware_sha256": expected_firmware_sha256, "source_sha": expected_source_sha}


class BundleInputs:
    """Translate this one S3 command graph into a relocatable public input set."""

    def __init__(self, root: Path, build: Path, metadata: dict, stage: Path):
        self.root, self.build, self.metadata, self.stage = root.resolve(), build.resolve(), metadata, stage
        self.framework = Path(metadata["framework_dir"]).resolve()
        self.libdeps = self.root / ".pio/libdeps/litch-esp32s3"
        self.sdk = self.framework / "tools/sdk/esp32s3"
        self.toolchain = Path(metadata["link"]["argv"][0]).resolve().parent.parent
        self.toolchain_identity = json.loads((self.toolchain / "package.json").read_text())
        self.esptool = Path(metadata["elf_to_bin"]["argv"][1]).resolve()
        self.toolchain_files = {}
        self.files = {}
        self.records = {item["path"]: item for item in metadata["inputs"]}
        text = Path(metadata["linker_map"]["path"]).read_text()
        # Omit archives with no selected member rather than distributing
        # unrelated optional SDK components merely mentioned by the link command.
        self.used_archives = {self.absolute(path) for path in re.findall(
            r"([^\s()]+\.a)\(", text.split("Discarded input sections", 1)[0])}
        self.framework_archives = {Path(item["path"]).resolve()
                                   for item in metadata["framework_archives"]}
        self.link_archives = {Path(path).resolve() for path in metadata["link"]["inputs"]
                              if path.endswith(".a")}
        self.selected_libraries = {path.name[3:-2] for path in
            self.used_archives | self.framework_archives}

    def complete_dependencies(self, temporary: Path) -> None:
        # -MMD intentionally drops system-marked headers and their transitive
        # includes. Ask the same production compiler for the complete closure;
        # preprocessing performs no code generation or build-product mutation.
        sys.path.insert(0, str(self.root / "tools/pio"))
        from capture_firmware_relink import complete_compile_dependencies
        records = [obj for archive in self.metadata["framework_archives"]
                   for obj in archive["objects"]] + self.metadata["application_rebuild"]
        if not 1 <= len(records) <= 512:
            raise ValueError("relink compile closure exceeds its bound")
        for record in records:
            record["dependencies"] = complete_compile_dependencies(record, self.root, temporary)
        print(f"Resolved complete compiler dependencies for {len(records)} translation units", file=sys.stderr)

    def validate_project_sources(self, source_sha: str) -> None:
        policy = json.loads(SOURCE_POLICY_PATH.read_text())
        expected = set(policy["first_party_files"] + policy["vendor_files"])
        actual = {Path(value).relative_to(self.root).as_posix()
                  for item in self.metadata["application_rebuild"] for value in item["dependencies"]
                  if Path(value).is_relative_to(self.root) and not Path(value).is_relative_to(self.libdeps)}
        delivery = self.metadata["project_sources"]
        actual.update(Path(value).relative_to(self.root).as_posix()
                      for item in delivery for value in item["dependencies"])
        for generated, preferred in policy["generated_sources"].items():
            if generated not in actual:
                raise ValueError("generated source is absent from the compiler input closure")
            actual.update(preferred)
        sources = {Path(item["source"]).relative_to(self.root).as_posix()
                   for item in self.metadata["application_rebuild"]}
        delivered_sources = {Path(item["source"]).relative_to(self.root).as_posix()
                             for item in delivery}
        if policy.get("schema_version") != 1 or policy.get("target") != "litch-esp32s3":
            raise ValueError("invalid shipping S3 source allowlist")
        differences = []
        for group, observed, reviewed in (
            ("files", actual, expected),
            ("translation_units", sources, set(policy["translation_units"])),
            ("source_translation_units", delivered_sources, set(policy["source_translation_units"])),
        ):
            for label, paths in (("absent from allowlist", observed - reviewed),
                                 ("absent from build", reviewed - observed)):
                if paths:
                    differences.append(f"{group} {label}: {', '.join(sorted(paths))}")
        if differences:
            raise ValueError("application source closure differs from the reviewed allowlist\n" + "\n".join(differences))
        for relative in sorted(expected):
            original = source_bytes(self.root, source_sha, relative)
            if self.checked(self.root / relative) != original:
                raise ValueError(f"public source differs from admitted Git source: {relative}")
        self.project_sources = expected

    def validate_licenses(self, legal: dict, contents: dict[str, bytes]) -> None:
        for field, package in (("framework_selection", self.framework),
                               ("runtime_selection", self.toolchain)):
            selection = json.loads(contents[legal[field]])
            if selection.get("target") != "litch-esp32s3":
                raise ValueError("archive license selection targets another board")
            observed = {path.relative_to(package).as_posix(): path
                        for path in self.used_archives if path.is_relative_to(package)}
            selected = (runtime_archives(selection, self.toolchain_identity, contents)
                        if field == "runtime_selection" else selection["archives"])
            records = {item["package_path"]: item for item in selected}
            if len(records) != len(selected) or observed.keys() != records.keys():
                raise ValueError("selected archive closure differs from reviewed license evidence")
            for relative, path in observed.items():
                record = records[relative]
                data = self.checked(path)
                if len(data) != record["bytes"] or digest(data) != record["sha256"]:
                    raise ValueError("selected archive bytes differ from reviewed license evidence")
                if not record["license_paths"] or any(name not in contents for name in record["license_paths"]):
                    raise ValueError("selected archive lacks its complete license evidence")

    def absolute(self, value: str) -> Path:
        return (self.root / value).resolve()

    def checked(self, path: Path) -> bytes:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_EXPANDED_SIZE:
            raise ValueError(f"invalid compliance input: {path.name}")
        data = path.read_bytes()
        record = self.records.get(str(path.resolve()))
        if record and (len(data) != record["size"] or digest(data) != record["sha256"]):
            raise ValueError(f"captured build input changed: {path.name}")
        return data

    def location(self, path: Path) -> str:
        path = path.resolve()
        if path.is_relative_to(self.toolchain):
            return "{toolchain}/" + path.relative_to(self.toolchain).as_posix()
        if path == self.esptool:
            return "{esptool}"
        if path.is_relative_to(self.build):
            relative = path.relative_to(self.build).as_posix()
            if relative in ("firmware.elf", "firmware.bin", "firmware.map"):
                return "{work}/" + relative
            return "{work}/objects/build/" + relative
        if path.is_relative_to(self.sdk):
            relative = path.relative_to(self.sdk).as_posix()
            if path.suffix == ".a" or relative.split("/", 1)[0] in ("lib",):
                return "{work}/objects/sdk/" + relative
            # ld and memory-profile directories hold both scripts and archives.
            return "{bundle}/sdk/" + relative
        if path.is_relative_to(self.framework):
            relative = path.relative_to(self.framework).as_posix()
            return "{bundle}/source/arduino" + ("/" + relative if relative != "." else "")
        if path == self.libdeps or path.is_relative_to(self.libdeps):
            relative = path.relative_to(self.libdeps).as_posix()
            return "{bundle}/source/libraries" + ("/" + relative if relative != "." else "")
        if path == self.root or path.is_relative_to(self.root):
            relative = path.relative_to(self.root).as_posix()
            return "{bundle}/source/project" + ("/" + relative if relative != "." else "")
        raise ValueError(f"unapproved relink path: {path.name}")

    def add(self, path: Path) -> None:
        path = path.resolve()
        data = self.checked(path)
        if path.is_relative_to(self.toolchain):
            self.toolchain_files[path.relative_to(self.toolchain).as_posix()] = digest(data)
            return
        location = self.location(path)
        name = location.removeprefix("{bundle}/").removeprefix("{work}/")
        if not safe_member(name):
            raise ValueError(f"input has no approved public role: {name}")
        target = self.stage / name
        if name in self.files:
            return
        if name == "objects/sdk/lib/libmbedx509.a" and digest(data) != MBEDTLS_ARCHIVE_SHA256:
            raise ValueError("public test-key exception requires the exact reviewed SDK archive")
        if name == "sdk/qio_qspi/sections.ld":
            # Espressif's generated provenance comment contains its CI runner
            # workspace. Preserve the upstream-relative provenance, not that
            # build host path; linker directives are unchanged.
            data = data.replace(
                b"/* Generated from: /home/runner/work/esp32-arduino-lib-builder/esp32-arduino-lib-builder/",
                b"/* Generated from: esp32-arduino-lib-builder/")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        if path.suffix in (".o", ".a"):
            relink_firmware.remove_private_debug(target, self.toolchain)
            data = target.read_bytes()
        reject_private_material(name, data)
        self.files[name] = data

    def command(self, argv: list[str], *, compile_command: bool = False) -> list[str]:
        output = []
        index = 0
        while index < len(argv):
            token = argv[index]
            index += 1
            if token.startswith("@") or "plugin" in token.lower():
                raise ValueError("response files and compiler plugins are not admitted")
            if index == 1:
                output.append("{python}" if Path(token).name.startswith("python")
                              else self.location(Path(token)))
            elif token.startswith("-ffile-prefix-map="):
                source, replacement = token[len("-ffile-prefix-map="):].rsplit("=", 1)
                output.append("-ffile-prefix-map=" + self.location(Path(source)) + "=" + replacement)
            elif token.startswith("-I"):
                path = self.absolute(token[2:])
                # Keep exactly the source/header closure; no application header
                # or unrelated SDK include search directory is shipped.
                if path in self.include_directories:
                    output.append("-I" + self.location(path))
            elif token.startswith("-L"):
                path = self.absolute(token[2:])
                if path.is_relative_to(self.sdk):
                    output.append("-L{work}/objects/sdk/" + path.relative_to(self.sdk).as_posix())
                    output.append("-L{bundle}/sdk/" + path.relative_to(self.sdk).as_posix())
                else:
                    output.append("-L" + self.location(path))
            elif token.startswith("-l") and len(token) > 2:
                if token[2:] in self.selected_libraries:
                    output.append(token)
            elif token.startswith("-Wl,-Map="):
                output.append("-Wl,-Map={work}/firmware.map")
            elif token.startswith("/") or self.absolute(token).is_file() or token.startswith(".pio/"):
                output.append(self.location(self.absolute(token)))
            else:
                output.append(token)
        if compile_command:
            # The original project operands (-Iinclude, src/foo.cpp, local
            # library headers) are relative. Keep those __FILE__ spellings
            # when the standalone command uses an absolute bundle path.
            output.append("-ffile-prefix-map={bundle}/source/project/=")
            output.append("-ffile-prefix-map={bundle}/source/libraries/=.pio/libdeps/litch-esp32s3/")
            output.append("-ffile-prefix-map={bundle}/sdk/=arduino/tools/sdk/esp32s3/")
        return output

    def collect(self) -> dict:
        for relative in sorted(self.project_sources):
            self.add(self.root / relative)
        self.framework_dependencies = {dependency for archive in self.metadata["framework_archives"]
                                       for obj in archive["objects"] for dependency in obj["dependencies"]}
        self.framework_dependencies.update(dependency for obj in self.metadata.get("application_rebuild", [])
                                           for dependency in obj["dependencies"])
        self.include_directories = {parent for dependency in self.framework_dependencies
                                    for parent in Path(dependency).parents}
        for dependency in sorted(self.framework_dependencies):
            self.add(Path(dependency))
        for directory in [self.framework / "cores/esp32", self.libdeps / "NeoPixelBus/src", *(
                self.framework / "libraries" / name / "src"
                for name in ("FS", "Wire", "SPI", "Update", "LittleFS"))]:
            for path in sorted(directory.rglob("*")):
                if path.is_file() and safe_member(self.location(path).removeprefix("{bundle}/")):
                    self.add(path)
        link_argv = self.metadata["link"]["argv"]
        link_dirs = [self.absolute(value[2:]) for value in link_argv if value.startswith("-L")]
        scripts = [link_argv[index + 1] for index, value in enumerate(link_argv) if value == "-T"]
        seen_scripts = set()
        while scripts:
            name = scripts.pop()
            if name in seen_scripts or len(seen_scripts) > 64:
                raise ValueError("repeated or excessive linker script includes")
            seen_scripts.add(name)
            path = next((directory / name for directory in link_dirs
                         if (directory / name).is_file()), None)
            if path is None:
                raise ValueError(f"missing selected linker script: {name}")
            self.add(path)
            scripts.extend(re.findall(r'^\s*INCLUDE\s+[\"\']?([^\s\"\']+)', path.read_text(), re.MULTILINE))
        for value in self.metadata["link"]["inputs"]:
            path = Path(value).resolve()
            if path.suffix == ".o" or path in self.used_archives | self.framework_archives:
                self.add(path)
        compiles, archives = [], []
        for archive in self.metadata["framework_archives"]:
            for obj in archive["objects"]:
                compiles.append({"source": self.location(Path(obj["source"])).removeprefix("{bundle}/"),
                                 "object": self.location(Path(obj["object"])).removeprefix("{work}/"),
                                 "argv": self.command(obj["argv"], compile_command=True)})
            archives.append({"archive": self.location(Path(archive["path"])).removeprefix("{work}/"),
                             "commands": [self.command(argv) for argv in archive["commands"]]})
        for obj in self.metadata.get("application_rebuild", []):
            compiles.append({"source": self.location(Path(obj["source"])).removeprefix("{bundle}/"),
                             "object": self.location(Path(obj["object"])).removeprefix("{work}/"),
                             "argv": self.command(obj["argv"], compile_command=True)})
        return {"schema_version": 1, "target": "esp32s3", "compile": compiles,
                "archives": archives, "link": self.command(self.metadata["link"]["argv"]),
                "image": self.command(self.metadata["elf_to_bin"]["argv"]),
                "toolchain_files": self.toolchain_files,
                "toolchain_system": self.toolchain_identity["system"],
                "firmware_sha256": self.metadata["artifacts"]["bin"]["sha256"],
                "original_elf_sha256": self.metadata["artifacts"]["elf"]["sha256"]}


def prepare(root: Path, build: Path, source_sha: str, output: Path,
            license_manifest: Path | None = None, license_root: Path | None = None) -> Path:
    root, build = root.resolve(), build.resolve()
    if not SOURCE_SHA.fullmatch(source_sha):
        raise ValueError("source SHA must be a complete commit identity")
    actual = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                            capture_output=True, text=True).stdout.strip()
    if actual != source_sha:
        raise ValueError("source SHA does not match this checkout")
    first_party_license = source_license(root, source_sha)
    litch_max_license = source_license(root, source_sha, "lib/litch-max/LICENSE")
    litch_max_snapshot = source_snapshot(root, source_sha)
    capture = build / "firmware_relink.json"
    if not capture.is_file() or capture.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("run the shipping S3 relink-info capture before packaging")
    metadata = json.loads(capture.read_text())
    if (metadata.get("schema_version") != 1 or metadata.get("environment") != "litch-esp32s3"
            or Path(metadata["project_dir"]).resolve() != root
            or Path(metadata["build_dir"]).resolve() != build):
        raise ValueError("relink capture belongs to another source/build owner")
    if "project_sources" not in metadata:
        raise ValueError("refresh shipping S3 relink-info to capture complete original sources")
    for record in metadata["artifacts"].values():
        path = Path(record["path"])
        if path.stat().st_size != record["size"] or digest(path.read_bytes()) != record["sha256"]:
            raise ValueError("captured firmware artifact changed")
    firmware_artifact.inspect_firmware(build / "firmware.bin")
    legal, legal_contents = legal_inputs(root,
        license_manifest or root / "tools/release/firmware_license_manifest.json",
        (license_root or root).resolve())
    if legal_contents["lib/litch-max/LICENSE"] != litch_max_license:
        raise ValueError("Litch-Max legal evidence differs from the selected source grant")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".firmware-compliance-", dir=output.parent) as temporary:
        stage = Path(temporary) / "source and relink"
        stage.mkdir()
        inputs = BundleInputs(root, build, metadata, stage)
        inputs.validate_licenses(legal, legal_contents)
        inputs.complete_dependencies(Path(temporary))
        inputs.validate_project_sources(source_sha)
        recipe = inputs.collect()
        files = inputs.files
        files.update(opening_book_inputs(root, source_sha))
        files["notices/LITCH-LICENSE.txt"] = first_party_license
        files["notices/LITCH-MAX-LICENSE.txt"] = litch_max_license
        files["notices/litch-max.snapshot.json"] = litch_max_snapshot
        notices = ["# Litch firmware notices\n",
                   "The original Litch software license is in LITCH-LICENSE.txt, copied exactly "
                   "from the selected source commit. Third-party terms follow.\n\n",
                   "Arduino-ESP32 is used under GNU LGPL version 2.1 or later. "
                   "NeoPixelBus 2.8.4 is used under GNU LGPL version 3 or later. "
                   "The GNU GPL version 3 companion text is included.\n\n"
                   "See source/arduino and source/libraries/NeoPixelBus for corresponding "
                   "library sources, README.md for recombination, and INSTALL.md for S3 installation.\n\n"
                   "Litch-Max original software is under MPL 2.0; its exact grant is in "
                   "LITCH-MAX-LICENSE.txt. Its nested mcu-max/micro-Max grant and attribution "
                   "are in mcu_max_provenance.md, "
                   "License and attribution; SparkFun VEML6030 attribution is in "
                   "sparkfun_veml6030.md, License and attribution disposition.\n"]
        for item in legal["legal_files"]:
            data = legal_contents[item["path"]]
            name = "notices/" + item["sha256"] + ".txt"
            files[name] = data
            notices.append(f"\n## {item['component']}\n\n{item['license']}\n\n"
                           f"Source: {item['source_url']}\n\nSee {Path(name).name}.\n")
            if item["component"] == "Arduino core and source libraries":
                files["notices/LGPL-2.1.txt"] = data
            if "NeoPixelBus" in item["component"]:
                files["notices/LGPL-3.0.txt"] = data
            if item["path"].endswith("/gcc/COPYING3"):
                files["notices/GPL-3.0.txt"] = data
            if item["path"] in ("lib/litch-max/docs/mcu_max_provenance.md", "docs/vendor/sparkfun_veml6030.md"):
                files["notices/" + Path(item["path"]).name] = data
        files["notices/NOTICE.md"] = "".join(notices).encode()
        files["notices/provenance.json"] = canonical_json(legal)
        files["relink.json"] = canonical_json(recipe)
        for name in ("relink_firmware.py", "firmware_artifact.py"):
            files[name] = (root / "tools/release" / name).read_bytes()
        files["README.md"] = (
            "# Litch S3 firmware source and relink materials\n\n"
            f"These materials correspond to firmware SHA-256 `{recipe['firmware_sha256']}` "
            f"and source commit `{source_sha}`. The original firmware source and required project "
            "headers are supplied in source/project under MPL 2.0. The Litch grant is in "
            "notices/LITCH-LICENSE.txt; the separate Litch-Max grant covering its original library "
            "source is in notices/LITCH-MAX-LICENSE.txt. Third-party files, including nested "
            "mcu-max/micro-Max, retain their own notices and terms.\n\n"
            "The historical Foxie-owned opening-book inputs in source/opening_book_baseline "
            "also use that MPL grant; the mcu-max files retain their vendor terms in notices/NOTICE.md.\n\n"
            "This bundle supplies the Arduino and NeoPixelBus sources used by this firmware, linkable application "
            "objects, selected SDK inputs, and exact third-party notices. See notices/NOTICE.md "
            "and notices/LITCH-LICENSE.txt for the original Litch software terms.\n\n"
            "Prerequisites: Python 3, Espressif xtensa-esp32s3-elf GCC 8.4.0 esp-2021r2-patch5, "
            "and esptool.py 4.9.0. Obtain the pinned toolchain through PlatformIO; the package "
            "identity is espressif/toolchain-xtensa-esp32s3@8.4.0+2021r2-patch5. "
            f"This bundle requires its producing host package, `{inputs.toolchain_identity['system']}`. "
            "Use that host or a compatible environment with those exact package bytes; a different "
            "host package with the same version is insufficient. Other host variants are unverified. "
            "The recipe validates selected runtime/header bytes.\n\n"
            "Run `python3 relink_firmware.py --output /new/output --toolchain /path/to/toolchain "
            "--esptool /path/to/esptool.py`. This rebuilds the included Arduino and NeoPixelBus libraries "
            "and the application translation units that use NeoPixelBus headers. "
            "For a modified library, edit source/arduino or source/libraries/NeoPixelBus and add `--modified`. "
            "The source/project directory also supplies the remaining original firmware and "
            "Litch-Max and foxie_embedded source, including Litch-Max's nested upstream C inputs. "
            "Those remaining application objects stay linkable in objects/; "
            "this library-recombination command rebuilds only the selected library consumers. "
            "Neither mode uploads, signs, or publishes firmware.\n\n"
            "Original-image verification restores the admitted ELF descriptor digest only after "
            "checking the rebuilt image, then requires the exact original whole-image SHA-256. "
            "This compensates only for removed debug metadata; any other byte change fails. "
            "Modified mode keeps its fresh ELF digest and produces a new image identity.\n\n"
            "Copied object/archive DWARF source, type and path data is removed; address-only "
            ".debug_frame CFI is retained because the Xtensa linker needs it for correct unwind data. "
            "Original compilation flags are preserved before that removal. Espressif's generated sections.ld "
            "provenance comment uses an upstream-relative path; linker directives are unchanged. "
            "SDK libmbedx509.a contains only the exact public Mbed TLS certs.c test keys recorded "
            "by the pinned dependency, not device or distribution credentials.\n\n"
            "Install a rebuilt image using INSTALL.md. The public toolchain and esptool prerequisites "
            "do not require this project's private repository, signing keys, or services.\n\n"
            "## Generated engine tables\n\n"
            "The supplied .inc files preserve the exact firmware inputs; regenerating tables is optional "
            "and is separate from the library relink command. Preferred inputs are in "
            "source/project/lib/litch-max/tools/engine_knowledge. For KPK, compile generate_kpk.cpp with a host "
            "C++17 compiler (`c++ -std=c++17 -O2 source/project/lib/litch-max/tools/engine_knowledge/generate_kpk.cpp "
            "-o /tmp/litch-generate-kpk`), then run `/tmp/litch-generate-kpk "
            "--check source/project/lib/litch-max/src/classic_chess_kpk_data.inc`, or use its "
            "`--write /path/to/table.inc` option after modifying the generator.\n\n"
            "The current opening table retains its original generator in the historical subdirectory; "
            "the standalone library's newer generation mode has a different baseline purpose. "
            "Historical opening-book generation needs Python 3 with python-chess, CMake 3.20 or newer, and host "
            "C11/C++17 compilers. Copy notices/opening_book_baseline.json to a temporary file and add "
            "a `root` property containing the absolute path to source/opening_book_baseline; preserve "
            "its `head` and `files` properties. From the extracted bundle run:\n\n"
            "```sh\npython3 source/project/lib/litch-max/tools/engine_knowledge/historical/generate_opening_book.py "
            "--baseline-manifest /path/to/local-baseline.json --artifact-dir /new/temporary/campaign "
            "--output /path/to/table.inc --jobs 2\n```\n\n"
            "This runs 408 complete Full5 searches. The original location-dependent baseline manifest "
            "was not retained. A local manifest changes the generated provenance digest, so this is "
            "a usable modification path, not a claim of byte-identical table regeneration. The supplied "
            "historical source bytes are pinned to the commit recorded in the baseline JSON; no Git "
            "checkout, private service, stored search results or external engine dataset is required.\n"
        ).encode()
        files["installation/otadata.bin"] = installation_selector()
        files["INSTALL.md"] = (
            "# Install a rebuilt image on Litch ESP32-S3\n\n"
            "These steps apply to an already provisioned Litch V2, V3, or V3.3 ESP32-S3 board "
            "with 8 MiB flash and OTA0 at 0x10000. They preserve saved settings "
            "and filesystem data. They replace OTA0 and its boot selector. Do not use them for "
            "a C3 board or factory provisioning.\n\n"
            "Use Python 3 and public esptool 4.9.0 (`python3 -m pip install esptool==4.9.0`). "
            "The relinker only prepares firmware.bin; installation is a separate action on the "
            "board you select. This firmware build uses no secure-boot signing or flash encryption. "
            "A separately secured board needs its owner's applicable authorization material.\n\n"
            "1. Finish the rebuild and keep its firmware.bin. For these existing-board steps, "
            "keep the image at or below 2 MiB, which fits supported existing OTA0 layouts. "
            "Do not erase flash or overwrite the bootloader, partition table, NVS, or filesystem.\n"
            "2. Disconnect USB. Hold P1-X while inserting USB to enter the ESP32-S3 ROM loader. "
            "Select this board's newly enumerated ROM serial port; release P1-X before writing.\n"
            "3. From the extracted bundle, run the command below, replacing PORT and the image path:\n\n"
            "```sh\npython3 -m esptool --chip esp32s3 --port \"PORT\" --baud 921600 "
            "--before no_reset --after watchdog_reset write_flash -z --flash_mode dio "
            "--flash_freq 80m --flash_size 8MB 0x10000 \"/path/to/firmware.bin\" "
            "0xe000 installation/otadata.bin\n```\n\n"
            "The supplied 8 KiB selector marks OTA0 valid using the same selector generation "
            "as the project ROM recovery tool. Wait for esptool's successful transfer/hash check. "
            "The board should return to its ordinary startup. If it does not, release P1-X and "
            "cold replug USB. After an interrupted write, repeat the ROM procedure on the same "
            "board with the same complete image and selector; do not factory erase. "
            "Successful transfer verifies bytes, while the behavior of your modified library "
            "must be checked on the board.\n"
        ).encode()
        for name, data in files.items():
            if not safe_member(name):
                raise ValueError("unapproved source/relink member")
            reject_private_material(name, data)
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        relink_firmware.run(stage, Path(temporary) / "verified", inputs.toolchain, inputs.esptool)
        return write_bundle(files, source_sha, recipe["firmware_sha256"],
                            recipe["original_elf_sha256"], output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("prepare")
    command.add_argument("--root", type=Path, required=True)
    command.add_argument("--build-dir", type=Path, required=True)
    command.add_argument("--source-sha", required=True)
    command.add_argument("--output", type=Path, required=True)
    command.add_argument("--license-manifest", type=Path)
    command.add_argument("--license-root", type=Path)
    args = parser.parse_args()
    try:
        # Build/verification diagnostics belong on stderr; stdout is one asset path.
        from contextlib import redirect_stdout
        with redirect_stdout(sys.stderr):
            path = prepare(args.root, args.build_dir, args.source_sha, args.output,
                           args.license_manifest, args.license_root)
        print(path)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"firmware-compliance: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
