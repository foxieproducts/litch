"""Pinned host-tool requirements shared by setup and runtime diagnostics."""

from __future__ import annotations

import importlib.util
import shlex
import shutil
import sys
from pathlib import Path


ESPTOOL_REQUIREMENT = "esptool==4.9.0"
PYSERIAL_REQUIREMENT = "pyserial==3.5"
ESPTOOL_INSTALL_REMEDIATION = (
    "run ./litch bootstrap firmware; direct install: "
    f"python3 -m pip install {ESPTOOL_REQUIREMENT}"
)


def pyserial_install_argv() -> tuple[str, ...]:
    """Install serial verification support into the ``./litch`` Python."""

    return (
        str(Path(sys.executable)),
        "-m",
        "pip",
        "install",
        PYSERIAL_REQUIREMENT,
    )


def firmware_python_setup_remediation() -> str:
    """Describe explicit setup without changing a distribution-owned Python."""

    create = shlex.join((str(Path(sys.executable)), "-m", "venv", ".venv"))
    return (
        f"create a writable virtual environment with the same Python: {create}; "
        ". .venv/bin/activate; python -m pip install platformio==6.1.19; "
        "./litch bootstrap firmware. If venv/ensurepip is unavailable, install "
        "your distribution's Python venv package first. Keep this environment "
        "active when running ./litch (including from your editor)."
    )


def resolve_esptool_argv() -> tuple[str, ...] | None:
    """Resolve esptool as a console script or current-Python module."""

    try:
        module = importlib.util.find_spec("esptool")
    except (ImportError, ValueError):
        module = None
    if module is not None:
        return (str(Path(sys.executable)), "-m", "esptool")
    executable = shutil.which("esptool") or shutil.which("esptool.py")
    if executable is not None:
        return (str(Path(executable).resolve()),)
    pio = shutil.which("pio")
    if pio is not None:
        return (
            str(Path(pio).resolve()),
            "pkg",
            "exec",
            "--package",
            "tool-esptoolpy",
            "--",
            "esptool.py",
        )
    return None
