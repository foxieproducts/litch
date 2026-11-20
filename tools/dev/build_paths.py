"""Artifact and dependency paths shared by firmware validation and delivery."""

from __future__ import annotations

import os
from pathlib import Path

from .runner import DevError


def _literal_directory(project_root: Path, selected: str, variable: str) -> Path:
    if "${" in selected:
        raise DevError(
            f"{variable}: use a literal absolute or project-relative path; "
            "./litch does not expand PlatformIO ${...} references",
            exit_code=2,
        )
    directory = Path(selected).expanduser()
    if not directory.is_absolute():
        directory = project_root / directory
    return directory


def platformio_build_directory(project_root: Path, environment: str) -> Path:
    selected = os.environ.get("PLATFORMIO_BUILD_DIR")
    variable = "PLATFORMIO_BUILD_DIR"
    if not selected:
        selected = str(Path(os.environ.get("PLATFORMIO_WORKSPACE_DIR") or project_root / ".pio") / "build")
        variable = "PLATFORMIO_WORKSPACE_DIR"
    return _literal_directory(project_root, selected, variable) / environment


def platformio_sleep_dependencies(project_root: Path, environment: str) -> tuple[Path, Path]:
    packages = os.environ.get("PLATFORMIO_PACKAGES_DIR")
    if packages:
        packages_dir = _literal_directory(project_root, packages, "PLATFORMIO_PACKAGES_DIR")
    else:
        core_dir = _literal_directory(project_root,
            os.environ.get("PLATFORMIO_CORE_DIR") or str(Path.home() / ".platformio"),
            "PLATFORMIO_CORE_DIR")
        packages_dir = core_dir / "packages"
    libdeps = os.environ.get("PLATFORMIO_LIBDEPS_DIR")
    if libdeps:
        libdeps_dir = _literal_directory(project_root, libdeps, "PLATFORMIO_LIBDEPS_DIR")
    else:
        workspace_dir = _literal_directory(project_root,
            os.environ.get("PLATFORMIO_WORKSPACE_DIR") or str(project_root / ".pio"),
            "PLATFORMIO_WORKSPACE_DIR")
        libdeps_dir = workspace_dir / "libdeps"
    return (packages_dir / "framework-arduinoespressif32/tools/sdk/esp32s3",
            libdeps_dir / environment / "ArduinoJson/src")
