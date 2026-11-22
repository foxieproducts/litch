#!/usr/bin/env python3
"""Host support contract for the local code-health command family.

The subprocess containment used by code-health relies on native POSIX process
groups and signals.  Keep host detection in this dependency-free module so a
CLI can reject unsupported compatibility environments before starting any
measurement lane.
"""

from __future__ import annotations

import os
import platform
from dataclasses import dataclass
from typing import Mapping, Optional


SUPPORTED_HOSTS = "native macOS and native Linux"


class UnsupportedHostError(ValueError):
    """A calm, user-facing rejection of an unsupported execution host."""


@dataclass(frozen=True)
class HostContract:
    """Detected host kind and whether it satisfies the code-health contract."""

    kind: str
    supported: bool
    detail: str


def detect_host_contract(
    *,
    system: Optional[str] = None,
    release: Optional[str] = None,
    version: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
) -> HostContract:
    """Classify the current host without performing work or mutating state.

    Optional values make the decision deterministic and directly testable.
    They are snapshots: passing an empty mapping deliberately suppresses use of
    the process environment.
    """

    detected_system = platform.system() if system is None else system
    detected_release = platform.release() if release is None else release
    detected_version = platform.version() if version is None else version
    detected_environ = os.environ if environ is None else environ

    system_upper = detected_system.strip().upper()
    ostype_lower = detected_environ.get("OSTYPE", "").strip().lower()

    if system_upper.startswith("CYGWIN") or ostype_lower.startswith("cygwin"):
        return HostContract(
            kind="cygwin",
            supported=False,
            detail="Cygwin does not provide the native POSIX host contract",
        )

    if (
        system_upper.startswith(("MSYS", "MINGW"))
        or bool(detected_environ.get("MSYSTEM", "").strip())
        or ostype_lower.startswith(("msys", "mingw"))
    ):
        return HostContract(
            kind="msys2",
            supported=False,
            detail="MSYS2 and MinGW do not provide the native POSIX host contract",
        )

    linux_description = " ".join(
        (detected_release, detected_version)
    ).casefold()
    has_wsl_environment = any(
        detected_environ.get(name, "").strip()
        for name in ("WSL_DISTRO_NAME", "WSL_INTEROP")
    )
    if system_upper == "LINUX" and (
        "microsoft" in linux_description or has_wsl_environment
    ):
        return HostContract(
            kind="wsl",
            supported=False,
            detail="WSL is not a native Linux host for this command",
        )

    if system_upper == "DARWIN":
        return HostContract(
            kind="macos",
            supported=True,
            detail="native macOS",
        )
    if system_upper == "LINUX":
        return HostContract(
            kind="linux",
            supported=True,
            detail="native Linux",
        )
    if system_upper == "WINDOWS" or system_upper.startswith("WIN"):
        return HostContract(
            kind="windows",
            supported=False,
            detail="Windows does not provide the native POSIX host contract",
        )

    display_name = detected_system.strip() or "unknown"
    return HostContract(
        kind="unsupported",
        supported=False,
        detail="unrecognized operating system: {0}".format(display_name),
    )


def require_supported_host(
    *,
    system: Optional[str] = None,
    release: Optional[str] = None,
    version: Optional[str] = None,
    environ: Optional[Mapping[str, str]] = None,
) -> HostContract:
    """Return the supported host or raise a user-facing error before work."""

    contract = detect_host_contract(
        system=system,
        release=release,
        version=version,
        environ=environ,
    )
    if not contract.supported:
        raise UnsupportedHostError(
            "unsupported host ({0}): {1}; code-health supports only {2}".format(
                contract.kind,
                contract.detail,
                SUPPORTED_HOSTS,
            )
        )
    return contract
