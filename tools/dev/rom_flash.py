"""Direct ESP32-S3 esptool flashing and shared offline artifact readers.

CLI flashing delegates transfers, their normal hash checks, retries and reset to
esptool. Canonical image inspection below belongs to offline release artifacts.
"""
from __future__ import annotations

import re
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol, Sequence

if not __package__:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.release import firmware_artifact
try:
    from . import ota_trial
except ImportError:
    import ota_trial


ESP32S3_CHIP_ID = firmware_artifact.ESP32S3_CHIP_ID

ESP_IMAGE_MAGIC = firmware_artifact.ESP_IMAGE_MAGIC

ESP_IMAGE_HEADER_SIZE = firmware_artifact.ESP_IMAGE_HEADER_SIZE

ESP_SEGMENT_HEADER_SIZE = firmware_artifact.ESP_SEGMENT_HEADER_SIZE

ESP_IMAGE_HASH_SIZE = firmware_artifact.ESP_IMAGE_HASH_SIZE

ESP_IMAGE_MAX_SEGMENTS = firmware_artifact.ESP_IMAGE_MAX_SEGMENTS

ESP_CHECKSUM_MAGIC = firmware_artifact.ESP_CHECKSUM_MAGIC

MAX_ESP32S3_FIRMWARE_SIZE = 0x330000

FLASH_SIZE_BYTES = 8 * 1024 * 1024

PARTITION_TABLE_BINARY_BYTES = 0xC00

class RomFlashError(RuntimeError):
    """An esptool operation or offline artifact could not be processed."""

@dataclass(frozen=True)
class CommandResult:
    argv: tuple[str, ...]
    returncode: int
    stdout: str = ""
    stderr: str = ""

class CommandResultLike(Protocol):
    returncode: int
    stdout: str
    stderr: str

class Runner(Protocol):
    def run(
        self,
        argv: Sequence[str],
        *,
        timeout_seconds: float,
    ) -> CommandResultLike: ...

@dataclass(frozen=True)
class FlashSegment:
    label: str
    offset: int
    path: Path

@dataclass(frozen=True)
class FirmwareImageMetadata:
    size_bytes: int
    sha256: str
    image_sha256: str
    chip_id: int
    segment_count: int

@dataclass(frozen=True)
class FirmwareImageInspection:
    metadata: FirmwareImageMetadata
    boot_policy: firmware_artifact.FirmwareBootPolicy
    first_segment_load_address: int
    first_segment_size_bytes: int

_SAFE_VERSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$")

_HEADER_VERSION_RE = re.compile(
    r'^\s*(?:static\s+constexpr\s+const\s+char\s*\*\s*)?'
    r'FIRMWARE_VERSION\s*=\s*"(?P<version>[^"]+)"\s*;\s*$',
    re.MULTILINE,
)

def inspect_firmware_image_with_policy(path: Path | str) -> FirmwareImageInspection:
    """Verify one ROM-sized image and its capability from a single file read."""

    firmware_path = Path(path)
    try:
        if not firmware_path.is_file():
            raise RomFlashError(f"missing firmware image: {firmware_path}")
        inspected = firmware_artifact.inspect_firmware_with_policy(
            firmware_path, max_size_bytes=MAX_ESP32S3_FIRMWARE_SIZE,
        )
    except firmware_artifact.ArtifactValidationError as exc:
        raise RomFlashError(str(exc)) from exc
    except OSError as exc:
        raise RomFlashError(f"cannot read firmware image {firmware_path}: {exc}") from exc

    metadata = inspected.metadata
    return FirmwareImageInspection(
        metadata=FirmwareImageMetadata(
            size_bytes=metadata.size_bytes,
            sha256=metadata.sha256,
            image_sha256=metadata.image_sha256,
            chip_id=metadata.chip_id,
            segment_count=metadata.segment_count,
        ),
        boot_policy=inspected.boot_policy,
        first_segment_load_address=inspected.first_segment_load_address,
        first_segment_size_bytes=inspected.first_segment_size_bytes,
    )

def inspect_firmware_image(path: Path | str) -> FirmwareImageMetadata:
    """Return the stable five-field metadata under the ROM image-size bound."""
    return inspect_firmware_image_with_policy(path).metadata

def require_sdk_trial_boot_policy(policy: firmware_artifact.FirmwareBootPolicy) -> None:
    """Admit NEW images without restricting integrity-valid old VALID fallbacks."""
    if (policy.status != firmware_artifact.FirmwareBootPolicyStatus.KNOWN
            or policy.epoch != 2):
        raise RomFlashError("NEW trial activation requires boot-policy epoch 2")

def parse_firmware_header_version(content: str) -> str:
    matches = [match.group("version") for match in _HEADER_VERSION_RE.finditer(content)]
    if len(matches) != 1:
        raise RomFlashError(
            "firmware version header must contain exactly one FIRMWARE_VERSION declaration"
        )
    version = matches[0]
    if not _SAFE_VERSION_RE.fullmatch(version):
        raise RomFlashError("firmware version contains unsafe characters")
    return version

def read_firmware_header_version(path: Path | str) -> str:
    header = Path(path)
    try:
        content = header.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise RomFlashError(f"cannot read firmware version header {header}: {exc}") from exc
    return parse_firmware_header_version(content)

@dataclass(frozen=True)
class EsptoolDialect:
    major: int
    version: tuple[int, int, int]

    def __post_init__(self) -> None:
        if len(self.version) != 3 or any(value < 0 for value in self.version):
            raise RomFlashError("esptool version must contain three non-negative parts")
        if self.major != self.version[0]:
            raise RomFlashError("esptool dialect major disagrees with its version")
        if self.major == 4 and self.version[1] < 9:
            raise RomFlashError("esptool 4.9 or newer is required")
        if self.major not in {4, 5}:
            raise RomFlashError("only audited esptool majors 4 and 5 are allowed")

    @property
    def write_command(self) -> str:
        return "write_flash" if self.major == 4 else "write-flash"


    @property
    def read_flash_command(self) -> str:
        return "read_flash" if self.major == 4 else "read-flash"


    @property
    def no_reset(self) -> str:
        return "no_reset" if self.major == 4 else "no-reset"

    @property
    def default_reset(self) -> str:
        return "default_reset" if self.major == 4 else "default-reset"


    @property
    def watchdog_reset(self) -> str:
        return "watchdog_reset" if self.major == 4 else "watchdog-reset"

    @property
    def flash_mode_option(self) -> str:
        return "--flash_mode" if self.major == 4 else "--flash-mode"

    @property
    def flash_freq_option(self) -> str:
        return "--flash_freq" if self.major == 4 else "--flash-freq"

    @property
    def flash_size_option(self) -> str:
        return "--flash_size" if self.major == 4 else "--flash-size"


_ESPTOOL_VERSION_RE = re.compile(
    r"\besptool(?:\.py)?(?:\s+version)?\s+v?"
    r"(?P<major>\d+)\.(?P<minor>\d+)(?:\.(?P<patch>\d+))?\b",
    re.IGNORECASE,
)

_ANSI_CSI_RE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")

def parse_esptool_dialect(output: str) -> EsptoolDialect:
    matches = list(_ESPTOOL_VERSION_RE.finditer(output))
    versions = {
        (
            int(match.group("major")),
            int(match.group("minor")),
            int(match.group("patch") or 0),
        )
        for match in matches
    }
    if len(versions) != 1:
        raise RomFlashError("could not determine one unambiguous esptool version")
    version = versions.pop()
    major, minor, _ = version
    if major == 4 and minor < 9:
        raise RomFlashError("esptool 4.9 or newer is required")
    if major not in {4, 5}:
        raise RomFlashError(
            f"unsupported esptool major version {major}; only audited majors 4 and 5 are allowed"
        )
    return EsptoolDialect(major=major, version=version)

def _combined_output(result: CommandResultLike) -> str:
    combined = "\n".join(part for part in (result.stdout, result.stderr) if part)
    return _ANSI_CSI_RE.sub("", combined)

def _run(
    runner: Runner,
    argv: Sequence[str],
    *,
    timeout_seconds: float,
    step: str,
) -> CommandResultLike:
    if timeout_seconds <= 0:
        raise RomFlashError(f"{step} has no positive timeout remaining")
    try:
        result = runner.run(tuple(argv), timeout_seconds=timeout_seconds)
    except (TimeoutError, OSError) as exc:
        raise RomFlashError(f"{step} failed before a trustworthy result: {exc}") from exc
    if result.returncode != 0:
        detail = _combined_output(result).strip()
        raise RomFlashError(
            f"{step} failed with exit code {result.returncode}: "
            f"{detail or 'no diagnostic'}"
        )
    return result

def detect_esptool_dialect(
    runner: Runner,
    esptool_argv: Sequence[str],
    *,
    timeout_seconds: float = 5.0,
) -> EsptoolDialect:
    if not esptool_argv:
        raise RomFlashError("esptool command prefix is empty")
    result = _run(
        runner,
        (*esptool_argv, "version"),
        timeout_seconds=timeout_seconds,
        step="esptool version probe",
    )
    return parse_esptool_dialect(_combined_output(result))

def _flash_parameter_args(dialect: EsptoolDialect) -> tuple[str, ...]:
    """Return the standard S3 image-header parameters."""

    # PlatformIO intentionally maps the S3 Arduino `qio` memory type to a DIO
    # ROM image header. Forcing QIO here makes this board reset in the loader.
    return (
        dialect.flash_mode_option,
        "dio",
        dialect.flash_freq_option,
        "80m",
        dialect.flash_size_option,
        "8MB",
    )


def _direct_argv(esptool_argv: Sequence[str], dialect: EsptoolDialect, port: str,
                 *, reset: bool, before: str | None = None) -> tuple[str, ...]:
    args = (*esptool_argv, "--chip", "esp32s3", "--port", port, "--baud", "921600",
            "--before", before or dialect.no_reset)
    # Final writes select their full-system exit below. Intermediate commands
    # leave the board in its ROM loader.
    return args if reset else (*args, "--after", dialect.no_reset)


def build_direct_write_argv(esptool_argv: Sequence[str], dialect: EsptoolDialect,
                            *, port: str, segments: Sequence[FlashSegment]) -> tuple[str, ...]:
    # Esptool preserves supplied pair order for ordinary unencrypted writes.
    # Publish selectors after its normal application transfer/hash check.
    pairs = tuple(token for segment in segments for token in (f"0x{segment.offset:x}", str(segment.path)))
    # Native USB's core reset can leave either entry path in download mode.
    # Esptool's documented watchdog reset owns the complete system exit.
    args = (*_direct_argv(esptool_argv, dialect, port, reset=True),
            "--after", dialect.watchdog_reset)
    return (*args, dialect.write_command,
            "-z", *_flash_parameter_args(dialect), *pairs)


def _ota0_capacity(data: bytes) -> int:
    for index in range(0, len(data) - 31, 32):
        magic, kind, subtype, address, size, _, _ = struct.unpack_from("<HBBII16sI", data, index)
        if magic != 0x50AA:
            break
        if kind == 0 and subtype == 0x10 and address == 0x10000:
            return size
    raise RomFlashError("This board has no OTA0 slot; use factory provisioning for initial setup.")


def require_direct_inputs(build_dir: Path, *, factory: bool) -> int:
    """Find wholly local build errors before a destructive factory erase."""
    names = ("firmware.bin", "bootloader.bin", "partitions.bin") if factory else ("firmware.bin",)
    for name in names:
        path = build_dir / name
        with path.open("rb") as stream:
            if not stream.read(1):
                raise RomFlashError(f"built {name} is empty")
    size = (build_dir / "firmware.bin").stat().st_size
    if factory:
        capacity = _ota0_capacity((build_dir / "partitions.bin").read_bytes())
        if ((size + 0xFFF) & ~0xFFF) > capacity:
            raise RomFlashError(f"factory firmware needs {size} bytes but the generated OTA0 slot holds {capacity}")
    return size


def _large_image_capacity(runner: Runner, esptool_argv: Sequence[str], dialect: EsptoolDialect,
                          *, port: str) -> int:
    """Only a larger image needs the installed slot's actual capacity."""
    with tempfile.TemporaryDirectory(prefix="litch-slot-capacity-") as directory:
        table = Path(directory) / "partitions.bin"
        _run(runner, (*_direct_argv(esptool_argv, dialect, port, reset=False),
                      dialect.read_flash_command, "0x8000", str(PARTITION_TABLE_BINARY_BYTES), str(table)),
             timeout_seconds=30, step="installed application capacity read")
        data = table.read_bytes()
    return _ota0_capacity(data)


def flash_s3_direct(runner: Runner, esptool_argv: Sequence[str], dialect: EsptoolDialect,
                    *, port: str, build_dir: Path, factory: bool = False) -> None:
    firmware = build_dir / "firmware.bin"
    size = require_direct_inputs(build_dir, factory=factory)
    if not factory and size > 0x200000:
        capacity = _large_image_capacity(runner, esptool_argv, dialect, port=port)
        if ((size + 0xFFF) & ~0xFFF) > capacity:
            raise RomFlashError(f"Firmware needs {size} bytes but this board's application slot holds {capacity}; "
                                "use a smaller build or deliberate factory provisioning, which erases saved data.")
    with tempfile.TemporaryDirectory(prefix="litch-direct-flash-") as directory:
        selector = Path(directory) / "otadata.bin"
        selector.write_bytes(ota_trial.make_selector_sector(1, ota_trial.ESP_OTA_IMG_VALID)
                             + b"\xff" * ota_trial.OTA_DATA_SECTOR_SIZE)
        segments = [FlashSegment("firmware", 0x10000, firmware),
                    FlashSegment("boot_selection", 0xE000, selector)]
        if factory:
            segments[:0] = [FlashSegment("bootloader", 0, build_dir / "bootloader.bin"),
                            FlashSegment("partitions", 0x8000, build_dir / "partitions.bin")]
        _run(runner, build_direct_write_argv(esptool_argv, dialect, port=port, segments=segments),
             timeout_seconds=180, step="esptool flash")


def erase_s3_direct(runner: Runner, esptool_argv: Sequence[str], dialect: EsptoolDialect,
                    *, port: str) -> None:
    erase = "erase_flash" if dialect.major == 4 else "erase-flash"
    # A blank S3 can enumerate as USB Serial/JTAG without entering its loader.
    # Give esptool its normal bootloader entry before the first factory operation.
    _run(runner, (*_direct_argv(esptool_argv, dialect, port, reset=False,
                                before=dialect.default_reset), erase),
         timeout_seconds=180, step="factory erase")
