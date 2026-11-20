#!/usr/bin/env python3
"""Deterministic sector-level endurance model for Litchcraft artwork stores.

This model consumes a measured write-amplification value. It does not measure
LittleFS and must not be used to turn an unidentified flash die into a lifetime
guarantee.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass, replace
from decimal import Decimal, ROUND_CEILING


DEFAULT_REPLACEMENTS = 20 * 365 * 10
DEFAULT_LOGICAL_BYTES = 289_024 + 1_088


@dataclass(frozen=True)
class EnduranceConfig:
    name: str
    replacements: int
    logical_bytes_per_replacement: int
    erase_block_bytes: int
    rotating_body_bytes: int
    write_amplification: Decimal
    metadata_ring_bytes: int = 0
    metadata_record_bytes: int = 0
    guaranteed_pe_cycles: int | None = None
    modeled_years: int = 10


@dataclass(frozen=True)
class EnduranceReport:
    name: str
    evidence: str
    replacements: int
    logical_bytes: int
    programmed_bytes: int
    effective_write_amplification: float
    erase_block_bytes: int
    body_sector_count: int
    metadata_sector_count: int
    total_sector_erases: int
    minimum_sector_erases: int
    mean_sector_erases: float
    p95_sector_erases: int
    maximum_sector_erases: int
    retired_sectors: int
    guaranteed_pe_cycles: int | None
    endurance_margin: float | None
    projected_years_to_rating: float | None
    passes_two_x_margin: bool


def _ceil_decimal(value: Decimal) -> int:
    return int(value.to_integral_value(rounding=ROUND_CEILING))


def _spread(total: int, count: int) -> list[int]:
    if count <= 0:
        if total != 0:
            raise ValueError("cannot distribute erases without sectors")
        return []
    base, remainder = divmod(total, count)
    return [base + (1 if index < remainder else 0) for index in range(count)]


def _percentile_nearest_rank(values: list[int], percentile: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def simulate(config: EnduranceConfig) -> EnduranceReport:
    if config.replacements <= 0 or config.modeled_years <= 0:
        raise ValueError("replacement count and modeled years must be positive")
    if config.guaranteed_pe_cycles is not None and config.guaranteed_pe_cycles <= 0:
        raise ValueError("guaranteed P/E cycles must be positive")
    if config.erase_block_bytes <= 0:
        raise ValueError("erase block size must be positive")
    if config.rotating_body_bytes <= 0:
        raise ValueError("rotating body region must be positive")
    if config.rotating_body_bytes % config.erase_block_bytes != 0:
        raise ValueError("body region must contain complete erase sectors")
    if config.write_amplification < 1:
        raise ValueError("write amplification cannot be below one")
    if (config.metadata_ring_bytes == 0) != (config.metadata_record_bytes == 0):
        raise ValueError("metadata ring and record sizes must both be zero or nonzero")
    if config.metadata_ring_bytes % config.erase_block_bytes != 0:
        raise ValueError("metadata ring must contain complete erase sectors")
    if config.metadata_record_bytes and (
        config.metadata_record_bytes > config.erase_block_bytes
        or config.erase_block_bytes % config.metadata_record_bytes != 0
    ):
        raise ValueError("metadata records must divide one erase sector")

    body_sector_count = config.rotating_body_bytes // config.erase_block_bytes
    physical_body_bytes_per_replacement = _ceil_decimal(
        Decimal(config.logical_bytes_per_replacement)
        * config.write_amplification
        / config.erase_block_bytes
    ) * config.erase_block_bytes
    body_erases = (
        physical_body_bytes_per_replacement
        * config.replacements
        // config.erase_block_bytes
    )
    body_counts = _spread(body_erases, body_sector_count)

    metadata_sector_count = (
        config.metadata_ring_bytes // config.erase_block_bytes
    )
    metadata_erases = 0
    if metadata_sector_count:
        records_per_sector = config.erase_block_bytes // config.metadata_record_bytes
        initial_records = metadata_sector_count * records_per_sector
        overwritten_records = max(0, config.replacements - initial_records)
        metadata_erases = math.ceil(overwritten_records / records_per_sector)
    metadata_counts = _spread(metadata_erases, metadata_sector_count)

    counts = body_counts + metadata_counts
    logical_bytes = config.logical_bytes_per_replacement * config.replacements
    programmed_bytes = _ceil_decimal(
        Decimal(logical_bytes) * config.write_amplification
    ) + config.metadata_record_bytes * config.replacements
    maximum = max(counts, default=0)
    guaranteed = config.guaranteed_pe_cycles
    margin = None if guaranteed is None or maximum == 0 else guaranteed / maximum
    projected_years = (
        None if margin is None else config.modeled_years * margin
    )
    return EnduranceReport(
        name=config.name,
        evidence=(
            "planning model; requires measured amplification and a guaranteed "
            "component P/E minimum"
        ),
        replacements=config.replacements,
        logical_bytes=logical_bytes,
        programmed_bytes=programmed_bytes,
        effective_write_amplification=programmed_bytes / logical_bytes,
        erase_block_bytes=config.erase_block_bytes,
        body_sector_count=body_sector_count,
        metadata_sector_count=metadata_sector_count,
        total_sector_erases=sum(counts),
        minimum_sector_erases=min(counts, default=0),
        mean_sector_erases=(sum(counts) / len(counts) if counts else 0.0),
        p95_sector_erases=_percentile_nearest_rank(counts, 0.95),
        maximum_sector_erases=maximum,
        retired_sectors=0,
        guaranteed_pe_cycles=guaranteed,
        endurance_margin=margin,
        projected_years_to_rating=projected_years,
        passes_two_x_margin=margin is not None and margin >= 2.0,
    )


def preset(name: str, write_amplification: Decimal | None) -> EnduranceConfig:
    if name == "current-n8-floor":
        return EnduranceConfig(
            name=name,
            replacements=DEFAULT_REPLACEMENTS,
            logical_bytes_per_replacement=DEFAULT_LOGICAL_BYTES,
            erase_block_bytes=4_096,
            rotating_body_bytes=1_638_400,
            write_amplification=write_amplification or Decimal("1"),
        )
    if name == "future-32m-plan":
        return EnduranceConfig(
            name=name,
            replacements=DEFAULT_REPLACEMENTS,
            logical_bytes_per_replacement=DEFAULT_LOGICAL_BYTES,
            erase_block_bytes=4_096,
            rotating_body_bytes=24 * 1_024 * 1_024,
            write_amplification=write_amplification or Decimal("4"),
            metadata_ring_bytes=256 * 1_024,
            metadata_record_bytes=256,
            guaranteed_pe_cycles=100_000,
        )
    raise ValueError(f"unknown preset: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--preset",
        choices=("current-n8-floor", "future-32m-plan"),
        default="current-n8-floor",
    )
    parser.add_argument(
        "--write-amplification",
        type=Decimal,
        help="Measured physical programmed bytes divided by logical bytes",
    )
    parser.add_argument(
        "--pe-cycles",
        type=int,
        help="Guaranteed minimum erase/program cycles for the exact flash part",
    )
    parser.add_argument(
        "--replacements",
        type=int,
        help="Override the default 73,000 complete animation replacements",
    )
    parser.add_argument(
        "--rotating-body-bytes",
        type=int,
        help="Hypothetical erase-aligned body pool; not a measured LittleFS free-space value",
    )
    args = parser.parse_args()
    config = preset(args.preset, args.write_amplification)
    if args.pe_cycles is not None:
        config = replace(config, guaranteed_pe_cycles=args.pe_cycles)
    if args.replacements is not None:
        config = replace(config, replacements=args.replacements)
    if args.rotating_body_bytes is not None:
        config = replace(config, rotating_body_bytes=args.rotating_body_bytes)
    report = simulate(config)
    print(json.dumps(asdict(report), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
