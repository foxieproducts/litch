#!/usr/bin/env python3
"""Exercise the production formatter/readback through a controlled SDK seam.

The explicit format, remount and primary verification code is unchanged. Save
writes use SettingsStore's native filesystem backend; physical flash remains a
separate, explicitly authorized gate.
"""
from pathlib import Path
import os
import subprocess
import tempfile


def main() -> None:
    project = Path(__file__).resolve().parents[2]
    suite = Path(__file__).resolve().parent
    json_include = project / ".pio/libdeps/native/ArduinoJson/src"
    if not json_include.is_dir():
        raise SystemExit("Native ArduinoJson dependency is unavailable; build native first.")
    sources = [
        path
        for path in sorted((project / "lib/foxie_embedded/src").glob("*.cpp"))
        if path.name != "settings_store_reformat.cpp"
    ]
    with tempfile.TemporaryDirectory(prefix="litch-reformat-check-") as output:
        binary = Path(output) / "recovery_test"
        command = [
            os.environ.get("CXX", "c++"),
            "-std=c++17",
            "-DLITCH_NATIVE=1",
            "-DLITCH_STORAGE_REFORMAT_FILESYSTEM_TEST=1",
            "-pthread",
            f"-I{project / 'include'}",
            f"-I{project / 'test/include'}",
            f"-I{suite / 'sdk'}",
            f"-I{project / 'lib/foxie_embedded/include'}",
            f"-I{json_include}",
            str(suite / "test_main.cpp"),
            str(project / "src/progress_watchdog_platform.cpp"),
            str(project / "src/progress_watchdog.cpp"),
            *map(str, sources),
            "-o",
            str(binary),
        ]
        subprocess.run(command, cwd=output, check=True)
        subprocess.run([str(binary)], cwd=output, check=True)


if __name__ == "__main__":
    main()
