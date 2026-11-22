#!/usr/bin/env python3
"""Run the portable mcu-max tests and the Litch firmware search policy check."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MCU_MAX_ROOT = REPOSITORY_ROOT / "lib" / "litch-max" / "vendor" / "mcu-max"
MCU_MAX_SOURCE = MCU_MAX_ROOT / "src" / "mcu-max.c"
MCU_MAX_INCLUDE = MCU_MAX_ROOT / "src"
TEST_ROOT = MCU_MAX_ROOT / "tests"


def main() -> int:
    compiler = os.environ.get("CC", "cc")
    tests = sorted(TEST_ROOT.glob("test_upstream_*.c"))
    if not tests:
        raise SystemExit(f"no upstream-facing mcu-max tests found in {TEST_ROOT}")
    tests.append(TEST_ROOT / "test_position_rules.c")

    with tempfile.TemporaryDirectory(prefix="mcu-max-tests-") as build_dir:
        for test_source in tests:
            executable = Path(build_dir) / test_source.stem
            test_config = test_source.with_name(
                f"{test_source.stem}_config.h"
            )
            compile_command = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{MCU_MAX_INCLUDE}",
                f"-I{TEST_ROOT}",
                str(MCU_MAX_SOURCE),
                str(MCU_MAX_ROOT / "src" / "mcu-max-position.c"),
                str(test_source),
                "-o",
                str(executable),
            ]
            if test_config.exists():
                compile_command[1:1] = ["-include", str(test_config)]
            print(f"[build] {test_source.name}", flush=True)
            subprocess.run(compile_command, check=True)
            print(f"[run]   {test_source.name}", flush=True)
            subprocess.run([str(executable)], check=True)

        policy_source = REPOSITORY_ROOT / "test/test_litch_max_search_policy_manual/test_main.cpp"
        policy_executable = Path(build_dir) / "test_litch_max_search_policy"
        print("[build] Litch-Max S3 search policy (host)", flush=True)
        subprocess.run([
            os.environ.get("CXX", "c++"),
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DLITCH_ESP32=1",
            "-DLITCH_ESP32S3=1",
            f"-I{REPOSITORY_ROOT / 'include'}",
            f"-I{TEST_ROOT}",
            f"-I{TEST_ROOT / 'fakes'}",
            str(policy_source),
            "-o",
            str(policy_executable),
        ], check=True)
        print("[run]   Litch-Max S3 search policy (host)", flush=True)
        subprocess.run([str(policy_executable)], check=True)

    print(f"mcu-max: {len(tests)} portable tests and Litch-Max S3 search policy passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
