#!/usr/bin/env python3
"""Rebuild the supplied Arduino libraries and relink one supplied S3 object set.

This utility deliberately does not upload, sign, or publish firmware.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import firmware_artifact


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def remove_private_debug(path: Path, toolchain: Path) -> None:
    # Xtensa ld uses .debug_frame CFI to retain correct allocated .eh_frame
    # references. It contains addresses, not source paths. Removing it changes
    # the image, so retain it while removing the source/type/path DWARF data.
    subprocess.run([str(toolchain / "bin/xtensa-esp32s3-elf-objcopy"),
                    "--remove-section=.debug*", "--remove-section=.zdebug*",
                    "--keep-section=.debug_frame", str(path)],
                   check=True, capture_output=True, timeout=30)


def verify_original_image(image: bytes, rebuilt_elf_sha256: str,
                          original_elf_sha256: str, expected_image_sha256: str) -> bytes:
    """Admit original bytes despite the removed, non-loadable debug metadata.

    esptool embeds the complete ELF digest at offset 0xb0. Relocating/stripping
    debug data changes that digest without changing executable behavior. Restore
    only this admitted digest and its two derived checksums, and accept only if
    the resulting *entire* image has the original, externally bound SHA-256.
    A modified library must use its fresh digest and must not use this mode.
    """
    firmware_artifact.inspect_firmware_bytes(image)
    for value in (rebuilt_elf_sha256, original_elf_sha256, expected_image_sha256):
        if not firmware_artifact.SHA256_PATTERN.fullmatch(value):
            raise ValueError("invalid image verification identity")
    if (len(image) < 0xd0 or struct.unpack_from("<I", image, 32)[0] != 0xabcd5432
            or image[0xb0:0xd0].hex() != rebuilt_elf_sha256):
        raise ValueError("image does not carry the rebuilt ELF's descriptor digest")
    normalized = bytearray(image)
    normalized[0xb0:0xd0] = bytes.fromhex(original_elf_sha256)
    offset = 24
    checksum = 0xef
    for _ in range(normalized[1]):
        size = struct.unpack_from("<I", normalized, offset + 4)[0]
        offset += 8
        for value in normalized[offset:offset + size]:
            checksum ^= value
        offset += size
    checksum_offset = offset + ((15 - offset) % 16)
    normalized[checksum_offset] = checksum
    normalized[checksum_offset + 1:] = hashlib.sha256(normalized[:checksum_offset + 1]).digest()
    result = bytes(normalized)
    firmware_artifact.inspect_firmware_bytes(result)
    if sha256(result) != expected_image_sha256:
        raise ValueError("relinked image differs beyond the admitted ELF descriptor digest")
    return result


def _run(bundle: Path, output: Path, toolchain: Path, esptool: Path,
         modified: bool = False) -> Path:
    bundle, output = bundle.resolve(), output.resolve()
    if output.exists():
        raise ValueError("relink output must be a new directory")
    recipe = json.loads((bundle / "relink.json").read_text())
    if recipe.get("schema_version") != 1 or recipe.get("target") != "esp32s3":
        raise ValueError("unsupported relink recipe")
    package = json.loads((toolchain / "package.json").read_text())
    if (package.get("name") != "toolchain-xtensa-esp32s3"
            or package.get("version") != "8.4.0+2021r2-patch5"
            or package.get("system") != recipe["toolchain_system"]):
        raise ValueError("relink requires the recorded producing toolchain host package")
    compiler = toolchain / "bin/xtensa-esp32s3-elf-gcc"
    identity = subprocess.run([str(compiler), "--version"], check=True, capture_output=True,
                              text=True, timeout=10).stdout.splitlines()[0]
    if "8.4.0" not in identity or "esp-2021r2-patch5" not in identity:
        raise ValueError("relink requires the pinned Espressif GCC 8.4.0 toolchain")
    version = subprocess.run([sys.executable, str(esptool), "version"], check=True,
                             capture_output=True, text=True, timeout=10).stdout
    if not re.search(r"(?:^|\n)4\.9\.0(?:\n|$)", version):
        raise ValueError("relink requires esptool 4.9.0")
    for relative, expected in recipe["toolchain_files"].items():
        path = (toolchain / relative).resolve()
        if not path.is_relative_to(toolchain.resolve()) or sha256(path.read_bytes()) != expected:
            raise ValueError("toolchain runtime/header does not match the supplied recipe")
    output.mkdir(parents=True)
    shutil.copytree(bundle / "objects", output / "objects")
    substitutions = {
        "{bundle}": str(bundle), "{work}": str(output), "{toolchain}": str(toolchain.resolve()),
        "{esptool}": str(esptool.resolve()), "{python}": sys.executable,
    }

    def expanded_command(argv: list[str]) -> list[str]:
        allowed_tools = {"{toolchain}/bin/xtensa-esp32s3-elf-gcc",
                         "{toolchain}/bin/xtensa-esp32s3-elf-g++",
                         "{toolchain}/bin/xtensa-esp32s3-elf-gcc-ar",
                         "{toolchain}/bin/xtensa-esp32s3-elf-gcc-ranlib", "{python}"}
        if (not argv or argv[0] not in allowed_tools
                or (argv[0] == "{python}" and (len(argv) < 2 or argv[1] != "{esptool}"))):
            raise ValueError("unexpected relink program")
        expanded = []
        for token in argv:
            if (token.startswith(("/", "@")) or "../" in token or "plugin" in token.lower()
                    or "@" in token or "\x00" in token):
                raise ValueError("uncontained relink command operand")
            for before, after in substitutions.items():
                token = token.replace(before, after)
            if "{" in token or "}" in token or token.startswith("@"):
                raise ValueError("unresolved relink command operand")
            expanded.append(token)
        return expanded

    def command(argv: list[str]) -> None:
        subprocess.run(expanded_command(argv), cwd=output, check=True, timeout=180, stdout=sys.stderr)

    # Reject the complete command graph before spending time recompiling it.
    commands = [item["argv"] for item in recipe["compile"]]
    commands.extend(argv for item in recipe["archives"] for argv in item["commands"])
    commands.extend((recipe["link"], recipe["image"]))
    for argv in commands:
        expanded_command(argv)

    for compilation in recipe["compile"]:
        relative = PurePosixPath(compilation["object"])
        if (relative.is_absolute() or ".." in relative.parts
                or not str(relative).startswith("objects/") or relative.suffix != ".o"):
            raise ValueError("invalid compilation output")
        target = output / compilation["object"]
        target.parent.mkdir(parents=True, exist_ok=True)
        command(compilation["argv"])
        # Xtensa's instruction relaxation can differ when -ggdb is omitted.
        # Preserve captured code-generation flags, then remove only debug data
        # from the completed private copy before linking the distributable image.
        remove_private_debug(target, toolchain)
    for archive in recipe["archives"]:
        relative = PurePosixPath(archive["archive"])
        if (relative.is_absolute() or ".." in relative.parts
                or not str(relative).startswith("objects/") or relative.suffix != ".a"):
            raise ValueError("invalid archive output")
        target = output / archive["archive"]
        target.unlink(missing_ok=True)
        for argv in archive["commands"]:
            command(argv)
    command(recipe["link"])
    command(recipe["image"])
    elf = output / "firmware.elf"
    image = output / "firmware.bin"
    image_data = image.read_bytes()
    firmware_artifact.inspect_firmware_bytes(image_data)
    if modified:
        if image_data[0xb0:0xd0].hex() != sha256(elf.read_bytes()):
            raise ValueError("modified image lost its fresh ELF identity")
    else:
        image_data = verify_original_image(image_data, sha256(elf.read_bytes()),
            recipe["original_elf_sha256"], recipe["firmware_sha256"])
        image.write_bytes(image_data)
    print(f"{'modified-image' if modified else 'original-image-verified'} sha256={sha256(image_data)}")
    return image


def run(bundle: Path, output: Path, toolchain: Path, esptool: Path,
        modified: bool = False) -> Path:
    output = output.resolve()
    if output.exists():
        raise ValueError("relink output must be a new directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".relink-", dir=output.parent) as temporary:
        staged = Path(temporary) / "verified"
        _run(bundle, staged, toolchain, esptool, modified)
        if output.exists():
            raise ValueError("relink output appeared during verification")
        os.rename(staged, output)
    return output / "firmware.bin"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--esptool", type=Path, required=True)
    parser.add_argument("--modified", action="store_true",
                        help="retain fresh ELF identity after modifying Arduino source")
    args = parser.parse_args()
    try:
        run(args.bundle, args.output, args.toolchain, args.esptool, args.modified)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"relink: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
