"""Flash S3 boards one at a time using one firmware build."""

from __future__ import annotations

import hashlib
import sys
import time
from pathlib import Path

from .batch_input import BatchInput
from .build_paths import platformio_build_directory
from .hardware import (
    TARGET_ENVIRONMENTS,
    BuildTarget,
    CoreDirectory,
    discover_possible_usb_boards,
    flash_exact_device,
)
from .locks import LockManager
from .runner import DevError, Runner
from .workspace import Workspace


_FLASH_FILES = ("firmware.bin", "bootloader.bin", "partitions.bin")
_POLL_SECONDS = 1.0


def _flash_file_hashes(build_dir: Path) -> tuple[str, ...]:
    hashes = []
    for name in _FLASH_FILES:
        path = build_dir / name
        digest = hashlib.sha256()
        length = 0
        try:
            with path.open("rb") as artifact:
                for chunk in iter(lambda: artifact.read(65536), b""):
                    length += len(chunk)
                    digest.update(chunk)
        except OSError as error:
            raise DevError(f"batch flash cannot read {path}: {error}") from error
        if length == 0:
            raise DevError(f"batch flash file is empty: {path}")
        hashes.append(digest.hexdigest())
    return tuple(hashes)


def flash_batch(
    *,
    workspace: Workspace,
    runner: Runner,
    lock_timeout: float,
    build_target: BuildTarget,
    platformio_core_dir: CoreDirectory,
    verbose: bool = False,
    factory_provision: bool = False,
) -> None:
    """Wait for each distinct exact USB serial and stop on a flash failure."""

    build_dir = platformio_build_directory(workspace.root, TARGET_ENVIRONMENTS["s3"])
    print(
        "litch: S3 USB factory batch will erase and provision every admitted board"
        if factory_provision else "litch: S3 USB flash batch",
        flush=True,
    )
    print(
        "litch: connect only boards intended for this batch, one at a time. "
        "A single eligible board already connected is included.",
        flush=True,
    )
    print("litch: building S3 once for this batch", flush=True)
    with LockManager(workspace, argv=sys.argv).lane("platformio", timeout=lock_timeout):
        build_target("s3")
        built_hashes = _flash_file_hashes(build_dir)

    def verify_build(_target: str) -> None:
        # The exact writer holds the same lane from this check through the write.
        if _flash_file_hashes(build_dir) != built_hashes:
            raise DevError(
                "S3 flash files changed after the batch build. "
                "Disconnect completed boards before starting a new batch."
            )

    print("litch: q stops after the current write; Ctrl-C interrupts.", flush=True)
    with BatchInput() as operator:
        flashed: set[str] = set()
        last_wait = ""
        while True:
            if operator.quit_requested():
                print(f"litch: batch stopped. Wrote {len(flashed)} board(s).", flush=True)
                return
            connected = [
                candidate for candidate in discover_possible_usb_boards(workspace, runner, lock_timeout)
                if candidate.target_hint in (None, "s3")
            ]
            pending = [candidate for candidate in connected if candidate.identity not in flashed]
            if operator.quit_requested():
                continue
            if len(pending) == 1 and pending[0].usb_parent is not None:
                identity = pending[0].usb_parent.identity
                print(
                    f"litch: batch board {len(flashed) + 1}: USB serial {identity.serial} "
                    f"at location {identity.location}",
                    flush=True,
                )
                flash_exact_device(
                    workspace=workspace,
                    target="s3",
                    usb_identity=identity.serial,
                    ble_identity=None,
                    runner=runner,
                    lock_timeout=lock_timeout,
                    build_target=verify_build,
                    platformio_core_dir=platformio_core_dir,
                    verbose=verbose,
                    factory_provision=factory_provision,
                    expected_usb_identity=identity,
                )
                flashed.add(identity.serial)
                print(f"litch: wrote firmware to {identity.serial}. Batch total {len(flashed)}.", flush=True)
                if factory_provision:
                    print(f"litch: {identity.serial} requires factory qualification", flush=True)
                last_wait = ""
                continue

            if len(pending) > 1:
                wait = "multiple unflashed USB boards detected. Leave only one connected."
            elif pending:
                wait = "waiting for the USB board to report both serial and location"
            elif connected:
                serials = ", ".join(sorted({candidate.identity for candidate in connected}))
                wait = f"{serials} already flashed in this batch. Connect the next S3 USB board."
            else:
                wait = "waiting for the next S3 USB board. q or Ctrl-C stops the batch."
            if wait != last_wait:
                print(f"litch: {wait}", flush=True)
                last_wait = wait
            time.sleep(_POLL_SECONDS)
