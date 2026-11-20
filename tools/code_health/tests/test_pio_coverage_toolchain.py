from __future__ import annotations

import os
from pathlib import Path
import runpy
from typing import Any
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "pio_coverage_toolchain.py"


class FakeEnvironment:
    def __init__(self, builders: list[FakeBuilder] | None = None) -> None:
        self.builders = builders or []
        self.values: dict[str, Any] = {"LINKFLAGS": []}

    def Replace(self, **values: Any) -> None:
        self.values.update(values)

    def AppendUnique(self, **values: list[str]) -> None:
        for key, additions in values.items():
            existing = self.values.setdefault(key, [])
            for addition in additions:
                if addition not in existing:
                    existing.append(addition)

    def GetLibBuilders(self) -> list[FakeBuilder]:
        return self.builders

    def compile_command(self) -> tuple[str, ...]:
        return (self.values["CXX"], *self.values.get("CCFLAGS", []), "-c", "source.cpp")

    def link_command(self) -> tuple[str, ...]:
        return (
            self.values["LINK"],
            *self.values["LINKFLAGS"],
            "-o",
            "program",
            "source.o",
        )


class FakeBuilder:
    def __init__(self, active: FakeEnvironment, origin: FakeEnvironment) -> None:
        self.env = active
        self.envorigin = origin


def run_script(root: FakeEnvironment, project: FakeEnvironment) -> dict[str, Any]:
    return runpy.run_path(
        str(SCRIPT),
        init_globals={
            "env": root,
            "projenv": project,
            # SCons injects imported variables into the script frame.  Tests
            # provide them above, so Import itself can remain a no-op.
            "Import": lambda _name: None,
        },
    )


class PioCoverageToolchainTests(unittest.TestCase):
    def test_updates_root_project_test_and_library_clone_environments(self) -> None:
        library = FakeEnvironment()
        library_origin = FakeEnvironment()
        builder = FakeBuilder(library, library_origin)
        root = FakeEnvironment([builder])
        project_test = FakeEnvironment()

        with mock.patch.dict(
            os.environ,
            {
                "CODE_HEALTH_CC": "/opt/llvm/bin/clang",
                "CODE_HEALTH_CXX": "/opt/llvm/bin/clang++",
                "CODE_HEALTH_LINK_FLAGS": "-fprofile-instr-generate -pthread",
            },
            clear=False,
        ):
            run_script(root, project_test)

        for target in (root, project_test, library, library_origin):
            self.assertEqual(target.values["CC"], "/opt/llvm/bin/clang")
            self.assertEqual(target.values["CXX"], "/opt/llvm/bin/clang++")
            self.assertEqual(target.values["LINK"], "/opt/llvm/bin/clang++")
            self.assertEqual(
                target.values["LINKFLAGS"],
                ["-fprofile-instr-generate", "-pthread"],
            )

        self.assertEqual(
            project_test.compile_command(),
            ("/opt/llvm/bin/clang++", "-c", "source.cpp"),
        )
        self.assertEqual(
            root.link_command(),
            (
                "/opt/llvm/bin/clang++",
                "-fprofile-instr-generate",
                "-pthread",
                "-o",
                "program",
                "source.o",
            ),
        )

    def test_deduplicates_shared_clones_and_link_flags(self) -> None:
        shared = FakeEnvironment()
        shared.values["LINKFLAGS"] = ["-fprofile-instr-generate"]
        builder = FakeBuilder(shared, shared)
        root = FakeEnvironment([builder])

        with mock.patch.dict(
            os.environ,
            {
                "CODE_HEALTH_CC": "clang",
                "CODE_HEALTH_CXX": "clang++",
                "CODE_HEALTH_LINK_FLAGS": "-fprofile-instr-generate",
            },
            clear=False,
        ):
            run_script(root, shared)

        self.assertEqual(shared.values["LINKFLAGS"], ["-fprofile-instr-generate"])

    def test_rejects_missing_toolchain_contract(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "CODE_HEALTH_CC": "",
                "CODE_HEALTH_CXX": "clang++",
                "CODE_HEALTH_LINK_FLAGS": "-fprofile-instr-generate",
            },
            clear=False,
        ):
            with self.assertRaisesRegex(RuntimeError, "requires CODE_HEALTH_CC"):
                run_script(FakeEnvironment(), FakeEnvironment())


if __name__ == "__main__":
    unittest.main()
