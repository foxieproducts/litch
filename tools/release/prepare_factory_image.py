#!/usr/bin/env python3
"""Export an offline ESP32-S3 factory image; never access a device or service."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.dev import ota_trial, rom_flash  # noqa: E402
from tools.dev.tool_requirements import resolve_esptool_argv  # noqa: E402


DEFAULT_BUILD_DIR = REPO_ROOT / ".pio/build/litch-esp32s3"
DEFAULT_OUTPUT = REPO_ROOT / "build/factory/litch-esp32s3-factory.bin"
PARTITION_TABLE_OFFSET = 0x8000
OTA_DATA_OFFSET = 0xE000
# Match the current hardware contract and reject old or incompatible builds.
PARTITIONS = (
    (1, 2, 0x9000, 0x5000, b"nvs"),
    (1, 0, OTA_DATA_OFFSET, ota_trial.OTA_DATA_SIZE, b"otadata"),
    (0, 0x10, ota_trial.OTA_SLOT_OFFSETS[0], ota_trial.OTA_SLOT_SIZE, b"ota_0"),
    (0, 0x11, ota_trial.OTA_SLOT_OFFSETS[1], ota_trial.OTA_SLOT_SIZE, b"ota_1"),
    (1, 0x82, 0x670000, 0x190000, b"spiffs"),
)


def validate_partitions(data: bytes) -> None:
    """Require the exact current S3 table, including its checksum and padding."""

    entries = b"".join(
        struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label, 0)
        for kind, subtype, offset, size, label in PARTITIONS
    )
    prefix = entries + b"\xEB\xEB" + b"\xFF" * 14 + hashlib.md5(entries).digest()
    expected = prefix + b"\xFF" * (rom_flash.PARTITION_TABLE_BINARY_BYTES - len(prefix))
    if data != expected:
        raise rom_flash.RomFlashError(
            "partitions.bin does not match the current 8 MiB S3 factory layout; "
            "rebuild with pio run -e litch-esp32s3"
        )


def run_esptool(argv: tuple[str, ...]) -> str:
    result = subprocess.run(
        argv, check=True, capture_output=True, text=True, timeout=60,
    )
    return result.stdout + result.stderr


def prepare_factory_image(build_dir: Path, output: Path) -> str:
    """Snapshot and validate build inputs, merge, verify, then publish locally."""

    build_dir = build_dir.resolve()
    output = output.resolve()
    if output.suffix.lower() != ".bin":
        raise rom_flash.RomFlashError("output must end in .bin (raw binary format)")
    if output.exists():
        raise rom_flash.RomFlashError(
            f"output already exists: {output}; choose another --output filename"
        )
    esptool = resolve_esptool_argv()
    if esptool is None:
        raise rom_flash.RomFlashError(
            "esptool is unavailable; install esptool >=4.9,<6 into this Python"
        )
    dialect = rom_flash.parse_esptool_dialect(run_esptool((*esptool, "version")))

    # Keep merge inputs immutable and publish atomically without overwriting.
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".factory-image-", dir=output.parent) as directory:
        staging = Path(directory)
        for name, limit in (
            ("bootloader.bin", PARTITION_TABLE_OFFSET),
            ("partitions.bin", rom_flash.PARTITION_TABLE_BINARY_BYTES),
            ("firmware.bin", ota_trial.OTA_SLOT_SIZE),
        ):
            source = build_dir / name
            if not source.is_file():
                raise rom_flash.RomFlashError(
                    f"missing build input: {source}; run pio run -e litch-esp32s3 first"
                )
            if not 0 < source.stat().st_size <= limit:
                raise rom_flash.RomFlashError(f"invalid size for {source}: maximum {limit} bytes")
            (staging / name).write_bytes(source.read_bytes())

        bootloader = staging / "bootloader.bin"
        rom_flash.inspect_firmware_image(bootloader)
        # This is the same header used by tools/dev/rom_flash.py. Do not force
        # QIO from platformio.ini: the Arduino S3 loader requires DIO here.
        if bootloader.read_bytes()[2:4] != bytes((2, 0x3F)):
            raise rom_flash.RomFlashError("bootloader must use the DIO / 80 MHz / 8 MiB header")
        partitions = staging / "partitions.bin"
        validate_partitions(partitions.read_bytes())
        firmware = staging / "firmware.bin"
        inspected = rom_flash.inspect_firmware_image_with_policy(firmware)
        rom_flash.require_sdk_trial_boot_policy(inspected.boot_policy)
        metadata = inspected.metadata
        selectors = staging / "otadata.bin"
        selectors.write_bytes(ota_trial.make_dual_slot_bootstrap_otadata(
            bytes.fromhex(metadata.image_sha256)
        ))
        segments = (
            (0, bootloader),
            (PARTITION_TABLE_OFFSET, partitions),
            (OTA_DATA_OFFSET, selectors),
            (ota_trial.OTA_SLOT_OFFSETS[0], firmware),
            (ota_trial.OTA_SLOT_OFFSETS[1], firmware),
        )

        merged = staging / "merged.bin"
        merge_command = "merge_bin" if dialect.major == 4 else "merge-bin"
        padding_option = "--fill-flash-size" if dialect.major == 4 else "--pad-to-size"
        argv = (
            *esptool, "--chip", "esp32s3", merge_command, "-o", str(merged),
            dialect.flash_mode_option, "dio", dialect.flash_freq_option, "80m",
            dialect.flash_size_option, "8MB", padding_option, "8MB",
            *(argument for offset, path in segments for argument in (hex(offset), str(path))),
        )
        run_esptool(argv)

        expected = bytearray(b"\xFF" * rom_flash.FLASH_SIZE_BYTES)
        for offset, path in segments:
            data = path.read_bytes()
            expected[offset:offset + len(data)] = data
        if merged.read_bytes() != expected:
            raise rom_flash.RomFlashError("merged image does not match the complete factory layout")
        digest = hashlib.sha256(expected).hexdigest()
        # The initial existence check alone cannot protect concurrent exports.
        # A same-filesystem hard link publishes the complete file exclusively;
        # staging cleanup removes its temporary name after publication.
        try:
            os.link(merged, output)
        except FileExistsError as error:
            raise rom_flash.RomFlashError(
                f"output already exists: {output}; choose another --output filename"
            ) from error
    return digest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Export a complete ESP32-S3 factory .bin for JLCPCB from an existing "
            "PlatformIO build. Programming address: 0x0. No device access."
        ),
    )
    parser.add_argument(
        "--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
        help="directory containing the finished S3 build (default: .pio/build/litch-esp32s3)",
    )
    parser.add_argument(
        "--output", type=Path, default=DEFAULT_OUTPUT,
        help="new .bin filename (default: build/factory/litch-esp32s3-factory.bin)",
    )
    args = parser.parse_args(argv)
    try:
        digest = prepare_factory_image(args.build_dir, args.output)
    except (OSError, rom_flash.RomFlashError, subprocess.SubprocessError) as error:
        detail = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            detail = (error.stdout or "") + (error.stderr or "") or detail
        print(f"Factory image export failed: {detail.strip()}", file=sys.stderr)
        return 1
    print(f"Factory image: {args.output.resolve()}")
    print(f"Size: {rom_flash.FLASH_SIZE_BYTES} bytes (8 MiB)")
    print("Programming address: 0x0; ESP32-S3, DIO, 80 MHz, 8 MiB flash")
    print(f"SHA-256: {digest}")
    print("Factory programming replaces all flash contents, including settings and saved games.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
