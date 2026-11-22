"""Exercise capture with real SCons substitution, without executing builders."""

import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "capture_firmware_relink.py"
spec = importlib.util.spec_from_file_location("capture_firmware_relink", SCRIPT)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)

try:
    import SCons.Environment
except ModuleNotFoundError:
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    installs = sorted((core / "packages" / "tool-scons").glob("scons-local-*"))
    if not installs:
        raise RuntimeError("Install the pinned PlatformIO tool-scons package before this test")
    sys.path.insert(0, str(installs[-1]))
    import SCons.Environment


class RelinkCaptureTests(unittest.TestCase):
    def test_long_command_is_inline_and_preserves_argv_quoting(self):
        with tempfile.TemporaryDirectory(prefix="relink graph ") as directory:
            root = Path(directory)
            env = SCons.Environment.Environment(tools=[])
            env.Replace(
                LINK="/bin/echo", SHELL="/bin/sh",
                MAXLINELENGTH=1,
                LINKCOM='${TEMPFILE("$LINK -DNAME=\\\\\\"quoted\\\\\\" -o $TARGET $SOURCES")}',
            )
            sources = [str(root / f"input {index}.o") for index in range(150)]
            node = env.Command(str(root / "firmware.elf"), sources, "$LINKCOM")[0]
            argv = capture.single_command(node)["argv"]
            self.assertEqual(argv[1], '-DNAME="quoted"')
            self.assertEqual(argv[2:4], ["-o", str(root / "firmware.elf")])
            self.assertEqual(argv[4:], sources)
            self.assertFalse(any(word.startswith("@") for word in argv))
            self.assertEqual(list(root.iterdir()), [])

    def test_variant_source_points_to_real_framework_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "arduino"
            source.mkdir()
            (source / "core.cpp").write_text("void example() {}\n")
            env = SCons.Environment.Environment(tools=[])
            env.VariantDir(str(root / "build"), str(source), duplicate=False)
            node = env.File(str(root / "build" / "core.cpp"))
            self.assertEqual(capture.node_path(node), str((source / "core.cpp").resolve()))

    def test_map_retains_implicit_inputs_and_space_in_path(self):
        paths = capture.map_load_paths(
            "LOAD objects/main.o\nLOAD /tmp/compiler runtime/libgcc.a\n"
            "LOAD objects/main.o\nOUTPUT(firmware.elf elf32-xtensa-le)\n", "/tmp/build"
        )
        self.assertEqual(paths, sorted([
            str(Path("/tmp/build/objects/main.o").resolve()),
            str(Path("/tmp/compiler runtime/libgcc.a").resolve()),
        ]))
        with self.assertRaisesRegex(ValueError, "no LOAD"):
            capture.map_load_paths("OUTPUT(only)\n", "/tmp")

    def test_missing_file_cannot_be_certified(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.o"
            inputs = capture.InputFiles()
            with self.assertRaisesRegex(ValueError, "missing"):
                inputs.add(path)
            path.write_bytes(b"object bytes")
            record = inputs.add(path)
            self.assertEqual(record["sha256"], hashlib.sha256(b"object bytes").hexdigest())
            self.assertEqual(record["size"], 12)
            self.assertIs(inputs.add(path), record)

    def test_complete_dependencies_include_system_header_transitives_without_codegen(self):
        compiler = shutil.which("c++")
        if compiler is None:
            self.skipTest("a host C++ preprocessor is required")
        with tempfile.TemporaryDirectory(prefix="relink complete deps ") as directory:
            root = Path(directory).resolve()
            shim = root / "shim"
            library = root / "library"
            temporary = root / "temporary"
            for path in (shim, library, temporary):
                path.mkdir()
            (shim / "api.h").write_text(
                '#pragma GCC system_header\n#include_next "api.h"\n'
            )
            (library / "api.h").write_text('#include "hidden.h"\n')
            hidden = library / "hidden.h"
            hidden.write_text("struct LibraryLayout { int value; };\n")
            source = root / "consumer.cpp"
            source.write_text(
                '#ifndef CAPTURE_CONDITION\n#error lost compile condition\n#endif\n'
                '#include "api.h"\nint size = sizeof(LibraryLayout);\n'
            )
            obj = root / "consumer.o"
            obj.write_bytes(b"original object")
            old_depfile = root / "consumer.d"
            old_depfile.write_bytes(b"original dependencies")
            flags = ["-DCAPTURE_CONDITION=1", "-I", str(shim), "-I" + str(library)]
            baseline = root / "baseline.d"
            subprocess.run(
                [compiler, "-E", "-MMD", "-MF", str(baseline), "-MQ", str(obj),
                 *flags, str(source)], cwd=root, check=True, timeout=30,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            )
            self.assertNotIn(
                str(hidden), capture.compiler_dependencies(baseline, root, str(obj), str(source))
            )
            record = {
                "source": str(source), "object": str(obj),
                "argv": [compiler, "-o", str(obj), "-c", "-MMD", "-MP",
                         "-MF" + str(old_depfile), "-MT", "old-target", *flags, str(source)],
                "dependencies": [str(source)],
            }
            original_argv = list(record["argv"])
            dependencies = capture.complete_compile_dependencies(record, root, temporary)
            for path in (source, shim / "api.h", library / "api.h", hidden):
                self.assertIn(str(path), dependencies)
            self.assertEqual(record["argv"], original_argv)
            self.assertEqual(record["dependencies"], [str(source)])
            self.assertEqual(obj.read_bytes(), b"original object")
            self.assertEqual(old_depfile.read_bytes(), b"original dependencies")
            self.assertEqual(list(temporary.iterdir()), [])

    def test_application_selection_follows_transitive_library_headers(self):
        with tempfile.TemporaryDirectory(prefix="relink deps ") as directory:
            root = Path(directory).resolve()
            (root / "src").mkdir()
            library = root / "library"
            library.mkdir()
            header = library / "NeoPixelBus.h"
            header.write_text("struct Pixel { int storage; };\n")
            (library / "Bridge.hpp").write_text('#include "NeoPixelBus.h"\n')
            env = SCons.Environment.Environment(tools=["g++"])
            env.Replace(CXXCOM="/bin/echo -c $SOURCE -o $TARGET", CPPPATH=[str(library)])
            nodes = []
            def make_path(path):
                return str(path).replace("\\", "\\\\").replace(" ", "\\ ").replace(":", "\\:")

            for name, content in (
                ("renderer", '#include "Bridge.hpp"\nPixel value;\n'),
                ("layout_only", '#include "Bridge.hpp"\nint size = sizeof(Pixel);\n'),
                ("unrelated", '#if 0\n#include "Bridge.hpp"\n#endif\nint unrelated;\n'),
            ):
                source = root / "src" / (name + ".cpp")
                source.write_text(content)
                obj = root / (name + ".o")
                obj.write_bytes(b"existing object")
                nodes.extend(env.Object(str(obj), str(source)))
                dependencies = [source]
                if name != "unrelated":
                    dependencies.extend([library / "Bridge.hpp", header])
                obj.with_suffix(".d").write_text(
                    make_path(obj) + ": \\\n " + " \\\n ".join(make_path(path) for path in dependencies) + "\n"
                )
            vendor = library / "library.cpp"
            vendor.write_text('#include "NeoPixelBus.h"\n#include "esp_efuse.h"\nPixel vendor;\n')
            shim = library / "shim" / "esp_efuse.h"
            sdk_header = library / "sdk" / "esp_efuse.h"
            shim.parent.mkdir()
            sdk_header.parent.mkdir()
            shim.write_text('#include_next "esp_efuse.h"\n')
            sdk_header.write_text("struct Efuse { int value; };\n")
            vendor_obj = root / "vendor.o"
            vendor_obj.write_bytes(b"existing vendor object")
            vendor_nodes = env.Clone(CPPPATH=[str(library), str(shim.parent), str(sdk_header.parent)]).Object(
                str(vendor_obj), str(vendor)
            )
            nodes.extend(vendor_nodes)
            vendor_obj.with_suffix(".d").write_text(
                make_path(vendor_obj) + ": \\\n " + " \\\n ".join(make_path(path) for path in (vendor, header, shim, sdk_header)) + "\n"
            )
            assembly = root / "src" / "boot.S"
            assembly.write_text(".text\n")
            assembly_obj = root / "boot.S.o"
            assembly_obj.write_bytes(b"existing assembly object")
            nodes.extend(env.Command(str(assembly_obj), str(assembly), "/bin/echo $SOURCE"))
            inputs = capture.InputFiles()
            selected = capture.application_rebuild_records(
                capture.descendants(nodes), root, header, inputs
            )
            self.assertEqual(
                {Path(item["source"]).name for item in selected},
                {"renderer.cpp", "layout_only.cpp"},
            )
            for item in selected:
                self.assertIn(str(header), item["dependencies"])
                self.assertIn(str(library / "Bridge.hpp"), item["dependencies"])
                self.assertEqual(item["object_sha256"], hashlib.sha256(b"existing object").hexdigest())
                depfile = Path(item["object"]).with_suffix(".d")
                self.assertEqual(item["dependency_file"]["sha256"], hashlib.sha256(depfile.read_bytes()).hexdigest())
            self.assertNotIn(str(vendor), inputs.records)
            self.assertNotIn(str(assembly), inputs.records)
            self.assertNotIn(str(root / "src" / "unrelated.cpp"), inputs.records)
            local_sources = [root / relative for relative in (
                "lib/foxie_embedded/src/settings.cpp",
                "lib/litch-max/src/classic_game.cpp",
                "lib/litch-max/vendor/mcu-max/src/mcu-max-position.c",
            )]
            for local_source in local_sources:
                local_source.parent.mkdir(parents=True)
                local_source.write_text("int local_source;\n")
                local_obj = root / (local_source.stem + ".o")
                local_obj.write_bytes(b"existing local library object")
                nodes.extend(env.Command(str(local_obj), str(local_source), "/bin/echo -c $SOURCE -o $TARGET"))
                local_obj.with_suffix(".d").write_text(
                    make_path(local_obj) + ": " + make_path(local_source) + "\n")
            delivered_inputs = capture.InputFiles()
            delivered = capture.project_source_records(capture.descendants(nodes), root, delivered_inputs)
            self.assertEqual({Path(item["source"]).name for item in delivered},
                             {"renderer.cpp", "layout_only.cpp", "unrelated.cpp", "boot.S",
                              "settings.cpp", "classic_game.cpp", "mcu-max-position.c"})
            for source in (*local_sources, assembly, root / "src/unrelated.cpp"):
                self.assertEqual(delivered_inputs.records[str(source)]["sha256"],
                                 hashlib.sha256(source.read_bytes()).hexdigest())
            self.assertNotIn(str(vendor), delivered_inputs.records)
            self.assertFalse(any("argv" in item for item in delivered))
            unreviewed = root / "lib/another_original_library/src/opaque.cpp"
            unreviewed.parent.mkdir(parents=True)
            unreviewed.write_text("int opaque;\n")
            unreviewed_nodes = env.Object(str(root / "opaque.o"), str(unreviewed))
            with self.assertRaisesRegex(ValueError, "unreviewed local compiled source"):
                capture.project_source_records(capture.descendants(nodes + unreviewed_nodes),
                                               root, capture.InputFiles())
            library_record = capture.compile_record(vendor_nodes[0], capture.InputFiles(), root)
            self.assertIn(str(shim), library_record["dependencies"])
            self.assertIn(str(sdk_header), library_record["dependencies"])
            self.assertEqual(library_record["dependency_file"]["path"], str(vendor_obj.with_suffix(".d")))

            depfile = root / "renderer.d"
            source_path = str(root / "src" / "renderer.cpp")
            object_path = str(root / "renderer.o")
            for content, message in (
                (make_path(root / "old.o") + ": " + make_path(source_path), "does not match"),
                (make_path(object_path) + ": " + make_path(source_path) + " " + make_path(root / "missing.hpp"), "missing"),
            ):
                with self.subTest(rejection=message):
                    depfile.write_text(content + "\n")
                    with self.assertRaisesRegex(ValueError, message):
                        capture.compiler_dependencies(depfile, root, object_path, source_path)
            depfile.unlink()
            with self.assertRaisesRegex(ValueError, "missing"):
                capture.compiler_dependencies(depfile, root, object_path, source_path)


if __name__ == "__main__":
    unittest.main()
