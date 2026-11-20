#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
EXPECTED_BOARDS = {
    "litch-esp32c3": "litch_esp32c3",
    "litch-esp32s3": "litch_esp32s3",
}
EXPECTED_S3_PARTITIONS = "tools/partitions/partitions-8MB-ota-spiffs.csv"
EXPECTED_S3_HWIDS = {("0x303a", "0x4001"), ("0x303a", "0x1001")}
EXPECTED_S3_PARTITION_ROWS = [
    ["nvs", "data", "nvs", "0x9000", "20k", ""],
    ["otadata", "data", "ota", "0xe000", "8k", ""],
    ["ota_0", "0", "ota_0", "0x10000", "3264k", ""],
    ["ota_1", "0", "ota_1", "0x340000", "3264k", ""],
    ["spiffs", "data", "spiffs", "0x670000", "1600k", ""],
]
PROHIBITED_BRANDING = (
    "ada" + "fruit",
    "qt" + "py",
    "qt" + " py",
    "qt" + "-py",
    "qt" + "_py",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def repository_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
    )
    return [REPO_ROOT / entry.decode() for entry in result.stdout.split(b"\0") if entry]


def verify_tracked_branding() -> None:
    violations: list[str] = []
    for path in repository_files():
        if not path.is_file():
            continue
        content = path.read_bytes()
        if b"\0" in content:
            continue
        lowered = content.decode("utf-8", errors="ignore").lower()
        if any(token in lowered for token in PROHIBITED_BRANDING):
            violations.append(str(path.relative_to(REPO_ROOT)))
    require(not violations, f"legacy board branding remains in: {', '.join(violations)}")


def environment_body(config: str, environment: str) -> str:
    match = re.search(
        rf"^\[env:{re.escape(environment)}\]\n(.*?)(?=^\[|\Z)",
        config,
        flags=re.MULTILINE | re.DOTALL,
    )
    require(match is not None, f"missing PlatformIO environment: {environment}")
    return match.group(1)


def verify_platformio_board_selection() -> None:
    config = (REPO_ROOT / "platformio.ini").read_text(encoding="utf-8")
    for environment, board in EXPECTED_BOARDS.items():
        body = environment_body(config, environment)
        require(
            re.search(rf"^board\s*=\s*{re.escape(board)}\s*$", body, re.MULTILINE) is not None,
            f"{environment} must select board {board}",
        )

    s3_body = environment_body(config, "litch-esp32s3")
    require(
        re.search(
            rf"^board_build\.partitions\s*=\s*{re.escape(EXPECTED_S3_PARTITIONS)}\s*$",
            s3_body,
            re.MULTILINE,
        )
        is not None,
        f"litch-esp32s3 must select {EXPECTED_S3_PARTITIONS}",
    )


def normalized_hwids(board: dict[str, object]) -> set[tuple[str, str]]:
    raw_hwids = board["build"]["hwids"]
    return {(vid.lower(), pid.lower()) for vid, pid in raw_hwids}


def verify_board_manifests() -> None:
    for board_name in EXPECTED_BOARDS.values():
        path = REPO_ROOT / "boards" / f"{board_name}.json"
        require(path.exists(), f"missing custom board manifest: {path.relative_to(REPO_ROOT)}")
        board = json.loads(path.read_text(encoding="utf-8"))
        require(board["vendor"] == "Foxie Products", f"wrong vendor in {path.name}")
        require(board["name"].startswith("Litch "), f"wrong product name in {path.name}")
        require(board["build"]["variant"] == board_name, f"wrong variant in {path.name}")
        require(board["build"]["variants_dir"] == "variants", f"wrong variants_dir in {path.name}")

    s3_path = REPO_ROOT / "boards" / "litch_esp32s3.json"
    s3_board = json.loads(s3_path.read_text(encoding="utf-8"))
    require(normalized_hwids(s3_board) == EXPECTED_S3_HWIDS, "unexpected S3 USB IDs")
    require("arduino" not in s3_board["upload"], "S3 manifest must not inject recovery images")
    arduino_build = s3_board["build"].get("arduino", {})
    require(
        not isinstance(arduino_build, dict) or "custom_bootloader" not in arduino_build,
        "S3 manifest must not select a custom bootloader",
    )


def verify_s3_descriptors() -> None:
    header = (REPO_ROOT / "variants" / "litch_esp32s3" / "pins_arduino.h").read_text(
        encoding="utf-8"
    )
    expected_lines = (
        '#define USB_VID 0x303A',
        '#define USB_PID 0x4001',
        '#define USB_MANUFACTURER "Foxie Products"',
        '#define USB_PRODUCT "Litch"',
        '#define USB_SERIAL "__MAC__"',
    )
    for line in expected_lines:
        require(line in header, f"missing S3 USB descriptor setting: {line}")


def verify_partition_layout() -> None:
    path = REPO_ROOT / EXPECTED_S3_PARTITIONS
    require(path.exists(), f"missing partition layout: {path.relative_to(REPO_ROOT)}")
    content = path.read_text(encoding="utf-8").lower()
    rows = [
        [field.strip() for field in line.split(",")]
        for line in content.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    require(rows == EXPECTED_S3_PARTITION_ROWS, "unexpected S3 partition offsets or sizes")


def verify_firmware_artifact(path: Path) -> None:
    # Branding is carried by strings, not arbitrary instruction/debug bytes.
    # A raw case-insensitive substring can match unrelated machine opcodes.
    content = b"\0".join(re.findall(rb"[\x20-\x7e]{4,}\x00", path.read_bytes())).lower()
    require(b"litch" in content, f"Litch USB product string missing from {path}")
    require(b"foxie products" in content, f"USB manufacturer string missing from {path}")
    for token in PROHIBITED_BRANDING:
        pattern = rb"(?<![a-z0-9_])" + re.escape(token.encode()) + rb"(?![a-z0-9_])"
        require(re.search(pattern, content) is None, f"legacy board branding remains in {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify Litch board and USB identity configuration")
    parser.add_argument("--firmware", type=Path, help="optional built firmware ELF or binary")
    args = parser.parse_args()

    verify_platformio_board_selection()
    verify_board_manifests()
    verify_s3_descriptors()
    verify_partition_layout()
    verify_tracked_branding()
    if args.firmware is not None:
        verify_firmware_artifact(args.firmware)

    print("USB identity verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
