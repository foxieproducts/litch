"""SDK OTA selector encoding shared by direct flashing and offline factory export."""

from __future__ import annotations

import binascii
import struct
from dataclasses import dataclass


OTA_DATA_SIZE = 0x2000
OTA_DATA_SECTOR_SIZE = 0x1000
OTA_SELECT_ENTRY_SIZE = 32
OTA_SLOT_OFFSETS = (0x10000, 0x340000)
OTA_SLOT_SIZE = 0x330000




ESP_OTA_IMG_NEW = 0
ESP_OTA_IMG_PENDING_VERIFY = 1
ESP_OTA_IMG_VALID = 2
ESP_OTA_IMG_INVALID = 3
ESP_OTA_IMG_ABORTED = 4
ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF

_NON_BOOTABLE_STATES = frozenset((ESP_OTA_IMG_INVALID, ESP_OTA_IMG_ABORTED))
_KNOWN_STATES = frozenset(
    (
        ESP_OTA_IMG_NEW,
        ESP_OTA_IMG_PENDING_VERIFY,
        ESP_OTA_IMG_VALID,
        ESP_OTA_IMG_INVALID,
        ESP_OTA_IMG_ABORTED,
        ESP_OTA_IMG_UNDEFINED,
    )
)


class OtaTrialError(RuntimeError):
    """Raised when current flash evidence cannot support a safe trial plan."""


@dataclass(frozen=True)
class OtaSelectEntry:
    sector: int
    sequence: int
    state: int
    crc: int
    crc_valid: bool

    @property
    def slot(self) -> int:
        if self.sequence in (0, 0xFFFFFFFF):
            raise OtaTrialError("invalid OTA sequence has no application slot")
        return (self.sequence - 1) % len(OTA_SLOT_OFFSETS)

    @property
    def selectable(self) -> bool:
        # SDK selection includes sequence zero and unknown states. Select first,
        # then reject unsupported authority instead of choosing an older VALID.
        return (
            self.sequence != 0xFFFFFFFF
            and self.crc_valid
            and self.state not in _NON_BOOTABLE_STATES
        )




def _sequence_crc(sequence: int) -> int:
    return binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF) & 0xFFFFFFFF


def parse_otadata(data: bytes) -> tuple[OtaSelectEntry, OtaSelectEntry]:
    if len(data) != OTA_DATA_SIZE:
        raise OtaTrialError(
            f"otadata readback must be exactly {OTA_DATA_SIZE} bytes"
        )
    entries: list[OtaSelectEntry] = []
    for sector in range(2):
        start = sector * OTA_DATA_SECTOR_SIZE
        sequence = struct.unpack_from("<I", data, start)[0]
        state = struct.unpack_from("<I", data, start + 24)[0]
        crc = struct.unpack_from("<I", data, start + 28)[0]
        entries.append(
            OtaSelectEntry(
                sector=sector,
                sequence=sequence,
                state=state,
                crc=crc,
                crc_valid=crc == _sequence_crc(sequence),
            )
        )
    return entries[0], entries[1]






def make_selector_sector(sequence: int, state: int) -> bytes:
    if sequence in (0, 0xFFFFFFFF):
        raise OtaTrialError("OTA sequence must be a finite positive integer")
    if state not in _KNOWN_STATES or state == ESP_OTA_IMG_UNDEFINED:
        raise OtaTrialError("selector state must be an explicit ESP OTA state")
    sector = bytearray(b"\xFF" * OTA_DATA_SECTOR_SIZE)
    struct.pack_into("<I", sector, 0, sequence)
    struct.pack_into("<I", sector, 24, state)
    struct.pack_into("<I", sector, 28, _sequence_crc(sequence))
    return bytes(sector)




def make_dual_slot_bootstrap_otadata(candidate_sha256: bytes) -> bytes:
    """Select slot 1 as a NEW trial while retaining slot 0 as VALID rollback."""
    # Preserve the existing factory helper's bounded input contract. SDK state,
    # never a reserved-label digest, authorizes selection and acceptance.
    if len(candidate_sha256) != 32:
        raise OtaTrialError("candidate SHA-256 must be exactly 32 bytes")
    return (
        make_selector_sector(1, ESP_OTA_IMG_VALID)
        + make_selector_sector(
            2,
            ESP_OTA_IMG_NEW,
        )
    )
