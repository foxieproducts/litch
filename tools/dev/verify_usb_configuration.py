#!/usr/bin/env python3
"""Verify active board/USB configuration without scanning historical ledgers.

The older repository-wide verifier also scans portable issue history and
tracked release notes. Those are immutable/historical evidence rather than
active board configuration, so the canonical developer workflow calls the
focused configuration checks here.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.pio import verify_usb_identity  # noqa: E402
from tools.dev.ota_trial import OTA_SLOT_SIZE  # noqa: E402
from tools.release.firmware_artifact import (  # noqa: E402
    ArtifactValidationError,
    inspect_firmware_with_policy,
)


OTA_ROLLBACK_HOOK_SYMBOL = "verifyRollbackLater"
OTA_ROLLBACK_HOOK_OBJECT_SUFFIX = "/src/ota_boot_health.cpp.o"
OTA_ROLLBACK_HOOK_SOURCE = REPO_ROOT / "src" / "ota_boot_health.cpp"
TINYUSB_POWER_HOOK_SYMBOL = "__wrap_tinyusb_init"
TINYUSB_POWER_HOOK_OBJECT_SUFFIX = "/src/usb_device_power_descriptor.cpp.o"
BOOT_POLICY_SYMBOL = "litch_boot_policy_descriptor"
BOOT_POLICY_OBJECT_SUFFIX = "/src/firmware_boot_policy_descriptor.cpp.o"
# Must match the implemented SDK-only authority, not merely parser support.
CURRENT_BOOT_POLICY_EPOCH = 2


def verify_ota_rollback_hook(map_path: Path) -> None:
    if not map_path.is_file():
        raise RuntimeError(f"missing S3 linker map: {map_path}")

    providers = re.findall(
        rf"^{re.escape(OTA_ROLLBACK_HOOK_SYMBOL)}\s+(\S+)\s*$",
        map_path.read_text(encoding="utf-8", errors="replace"),
        flags=re.MULTILINE,
    )
    if len(providers) != 1:
        raise RuntimeError(
            "S3 linker map must identify exactly one verifyRollbackLater provider"
        )

    provider = providers[0].replace("\\", "/")
    if not provider.endswith(OTA_ROLLBACK_HOOK_OBJECT_SUFFIX):
        raise RuntimeError(
            "S3 verifyRollbackLater did not resolve to the Litch OTA health hook: "
            f"{provider}"
        )


def verify_ota_rollback_hook_source(source_path: Path) -> None:
    if not source_path.is_file():
        raise RuntimeError(f"missing S3 rollback-hook source: {source_path}")
    source = source_path.read_text(encoding="utf-8", errors="strict")
    matches = re.findall(
        r'extern\s+"C"\s+bool\s+verifyRollbackLater\s*\(\s*\)\s*\{(.*?)\}',
        source,
        flags=re.DOTALL,
    )
    if len(matches) != 1:
        raise RuntimeError(
            "S3 source must define exactly one verifyRollbackLater hook"
        )
    body = " ".join(matches[0].split())
    if body != "return true;":
        raise RuntimeError(
            "S3 verifyRollbackLater must remain a pure pre-runtime "
            "`return true;` decision"
        )


def verify_tinyusb_power_hook(map_path: Path) -> None:
    if not map_path.is_file():
        raise RuntimeError(f"missing S3 linker map: {map_path}")

    providers = re.findall(
        rf"^{re.escape(TINYUSB_POWER_HOOK_SYMBOL)}\s+(\S+)\s*$",
        map_path.read_text(encoding="utf-8", errors="replace"),
        flags=re.MULTILINE,
    )
    if len(providers) != 1:
        raise RuntimeError(
            "S3 linker map must identify exactly one __wrap_tinyusb_init provider"
        )

    provider = providers[0].replace("\\", "/")
    if not provider.endswith(TINYUSB_POWER_HOOK_OBJECT_SUFFIX):
        raise RuntimeError(
            "S3 TinyUSB initialization did not resolve through the Litch "
            f"power-descriptor hook: {provider}"
        )


def verify_rtc_slow_reservations(map_path: Path) -> None:
    # Normal RTC sections end here in the pinned S3 linker script. The panic
    # capsule begins at 0x50001c00 and P1-X wake code begins at 0x50001f80.
    ends = re.findall(
        r"^[ \t]*(0x[0-9a-fA-F]+)[ \t]+_rtc_force_slow_end = ABSOLUTE \(\.\)[ \t]*$",
        map_path.read_text(encoding="utf-8", errors="replace"), flags=re.MULTILINE,
    )
    if len(ends) != 1 or not 0x50000000 <= int(ends[0], 16) <= 0x50001C00:
        raise RuntimeError("S3 normal RTC slow data must end by the reserved panic capsule at 0x50001c00")


def verify_boot_policy_descriptor(
    map_path: Path,
    firmware_path: Path,
    *,
    expected_epoch: int = CURRENT_BOOT_POLICY_EPOCH,
) -> None:
    """Bind one retained producer to the complete verified image descriptor."""
    if not map_path.is_file():
        raise RuntimeError(f"missing S3 linker map: {map_path}")
    map_text = map_path.read_text(encoding="utf-8", errors="replace")
    providers = re.findall(
        rf"^{re.escape(BOOT_POLICY_SYMBOL)}\s+(\S+)\s*$",
        map_text,
        flags=re.MULTILINE,
    )
    if len(providers) != 1 or not providers[0].replace("\\", "/").endswith(
        BOOT_POLICY_OBJECT_SUFFIX
    ):
        raise RuntimeError("S3 boot-policy descriptor needs exactly one Litch provider")
    sections = re.findall(
        r"^[ \t]*\.rodata_custom_desc(?:\.\S+)?[ \t]*(?:\n[ \t]*)?"
        r"(0x[0-9a-fA-F]+)[ \t]+(0x[0-9a-fA-F]+)[ \t]+(\S+)[ \t]*$",
        map_text,
        flags=re.MULTILINE,
    )
    addresses = re.findall(
        rf"^[ \t]*(0x[0-9a-fA-F]+)[ \t]+{re.escape(BOOT_POLICY_SYMBOL)}[ \t]*$",
        map_text,
        flags=re.MULTILINE,
    )
    if len(sections) != 1 or len(addresses) != 1:
        raise RuntimeError("S3 boot-policy descriptor must have one retained section and symbol")
    address, size, provider = sections[0]
    if int(size, 16) != 16 or not provider.replace("\\", "/").endswith(
        BOOT_POLICY_OBJECT_SUFFIX
    ):
        raise RuntimeError("S3 boot-policy descriptor section has wrong size or provider")
    try:
        inspection = inspect_firmware_with_policy(
            firmware_path, max_size_bytes=OTA_SLOT_SIZE
        )
    except (ArtifactValidationError, OSError) as error:
        raise RuntimeError(f"S3 boot-policy image integrity failed: {error}") from error
    policy = inspection.boot_policy
    if policy.status.value != "known" or policy.epoch != expected_epoch:
        raise RuntimeError(
            "S3 built image must identify the actual current boot-policy epoch "
            f"{expected_epoch}; observed {policy}"
        )
    expected_address = inspection.first_segment_load_address + 256
    if int(address, 16) != expected_address or int(addresses[0], 16) != expected_address:
        raise RuntimeError("S3 boot-policy descriptor must follow the first SDK app descriptor")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify active Litch USB configuration")
    parser.add_argument("--firmware", type=Path, help="optional built S3 ELF")
    args = parser.parse_args()

    verify_usb_identity.verify_platformio_board_selection()
    verify_usb_identity.verify_board_manifests()
    verify_usb_identity.verify_s3_descriptors()
    verify_usb_identity.verify_partition_layout()
    verify_ota_rollback_hook_source(OTA_ROLLBACK_HOOK_SOURCE)
    if args.firmware is not None:
        verify_usb_identity.verify_firmware_artifact(args.firmware)
        map_path = args.firmware.with_suffix(".map")
        verify_ota_rollback_hook(map_path)
        verify_tinyusb_power_hook(map_path)
        verify_rtc_slow_reservations(map_path)
        verify_boot_policy_descriptor(map_path, args.firmware.with_suffix(".bin"))
    print("Active USB configuration verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
