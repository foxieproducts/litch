"""Bounded macOS CDC host recovery for one exact Litch application USB parent."""

from __future__ import annotations

import platform
import re
import subprocess
import tempfile
from pathlib import Path

from tools.dev.devices import DeviceInventoryError

_SOURCE = Path(__file__).with_suffix(".m")
_EXACT_SERIAL = re.compile(r"[0-9a-fA-F]{12}\Z")


def reset_exact_litch_application_usb(serial: str) -> None:
    """Reenumerate the selected host USB device without resetting the MCU."""

    if platform.system() != "Darwin":
        raise DeviceInventoryError("USB host recovery is available only on macOS")
    if _EXACT_SERIAL.fullmatch(serial) is None:
        raise DeviceInventoryError("USB host recovery requires the exact S3 serial")
    with tempfile.TemporaryDirectory(prefix="litch-exact-usb-reset-") as temporary:
        helper = Path(temporary) / "litch_usb_reset"
        compiled = subprocess.run(
            [
                "/usr/bin/clang",
                "-fobjc-arc",
                "-framework", "Foundation",
                "-framework", "IOKit",
                "-framework", "IOUSBHost",
                str(_SOURCE),
                "-o", str(helper),
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        if compiled.returncode != 0:
            raise DeviceInventoryError(
                "macOS USB reset helper could not build: "
                + (compiled.stderr.strip() or f"clang exit {compiled.returncode}")
            )
        reset = subprocess.run(
            [str(helper), serial],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        if reset.returncode != 0:
            raise DeviceInventoryError(
                "exact macOS USB reset failed: "
                + (reset.stderr.strip() or f"helper exit {reset.returncode}")
            )
