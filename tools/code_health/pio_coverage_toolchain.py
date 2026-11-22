"""PlatformIO post-script that makes native coverage toolchain selection explicit.

PlatformIO's native builder deliberately loads its default GCC/G++ tools after
pre-scripts run, and SCons classifies coverage compiler flags as ``CCFLAGS``
rather than ``LINKFLAGS``.  The code-health runner supplies the selected local
compiler and the matching link flags through the environment; this post-script
applies both after the native builder has constructed its program graph.
"""

from __future__ import annotations

import os
import shlex
from typing import Iterable


def _apply_toolchain(target_env: object, cc: str, cxx: str, link_flags: list[str]) -> None:
    """Apply coverage settings to one existing SCons construction environment."""

    target_env.Replace(CC=cc, CXX=cxx, LINK=cxx)  # type: ignore[attr-defined]
    target_env.AppendUnique(LINKFLAGS=link_flags)  # type: ignore[attr-defined]


def _coverage_environments(root_env: object, project_env: object) -> Iterable[object]:
    """Yield every environment that owns already-created native build actions.

    PlatformIO constructs the project/test and library graphs with clones of the
    root environment before POST scripts run.  Mutating only the root therefore
    changes the final program link but leaves compile and archive actions on the
    platform-native GCC/G++ defaults.  The library builders retain both their
    active clone and an origin clone that can be used for subsequent discovery,
    so both must receive the selected coverage toolchain.
    """

    candidates = [root_env, project_env]
    get_lib_builders = getattr(root_env, "GetLibBuilders", None)
    if not callable(get_lib_builders):
        raise RuntimeError(
            "code-health coverage post-script requires PlatformIO library builders"
        )
    for builder in get_lib_builders():
        candidates.extend(
            (getattr(builder, "env", None), getattr(builder, "envorigin", None))
        )

    seen: set[int] = set()
    for candidate in candidates:
        if candidate is None or id(candidate) in seen:
            continue
        seen.add(id(candidate))
        yield candidate


def _configure_coverage_toolchain(root_env: object, project_env: object) -> None:
    cc = os.environ.get("CODE_HEALTH_CC")
    cxx = os.environ.get("CODE_HEALTH_CXX")
    raw_link_flags = os.environ.get("CODE_HEALTH_LINK_FLAGS")
    if not cc or not cxx or not raw_link_flags:
        raise RuntimeError(
            "code-health coverage post-script requires CODE_HEALTH_CC, "
            "CODE_HEALTH_CXX, and CODE_HEALTH_LINK_FLAGS"
        )
    link_flags = shlex.split(raw_link_flags)
    if not link_flags:
        raise RuntimeError("code-health coverage link flags must not be empty")

    for target_env in _coverage_environments(root_env, project_env):
        _apply_toolchain(target_env, cc, cxx, link_flags)


Import("env")  # type: ignore[name-defined]  # Provided by SCons/PlatformIO.
Import("projenv")  # type: ignore[name-defined]  # Exported by ProjectAsLibBuilder.
_configure_coverage_toolchain(env, projenv)  # type: ignore[name-defined]
