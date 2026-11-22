"""Pinned SDK callback evidence; native radio/store APIs are controlled seams."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "harden_nimble_repeat_pairing.py"
FIXTURE = Path(__file__).parent / "fixtures/nimble_237"
spec = importlib.util.spec_from_file_location("litch_nimble_hardening", SCRIPT)
hardening = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardening)


class RepeatPairingTests(unittest.TestCase):
    def setUp(self):
        self.original = (FIXTURE / "NimBLEServer.cpp").read_bytes()

    def test_only_repeat_pairing_and_failed_connect_restart_sites_change(self):
        patched = hardening.HardenServerSource(self.original)
        before, after = self.original.split(hardening.ORIGINAL_CASE.encode())
        expected = (before + hardening.HARDENED_CASE.encode() + after).replace(
            hardening.FAILED_CONNECT_RESTART.encode(), hardening.MAIN_LOOP_CONNECT_RESTART.encode(), 1)
        self.assertEqual(patched, expected)
        self.assertEqual(patched.count(b"case BLE_GAP_EVENT_REPEAT_PAIRING:"), 1)

    def test_unknown_source_fails_closed(self):
        for source in [b"", self.original + b"\n", self.original.replace(b"delete_peer", b"delete_pear")]:
            with self.subTest(source_bytes=len(source)), self.assertRaises(ValueError):
                hardening.HardenServerSource(source)

    def test_build_copy_preserves_dependency_and_incremental_timestamp(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / "NimBLE-Arduino"
            (package / "src").mkdir(parents=True)
            (package / "src/NimBLEServer.cpp").write_bytes(self.original)
            shutil.copy2(FIXTURE / "library.properties", package)
            output = hardening.WriteHardenedSource(package, root / "build")
            stamp = output.stat().st_mtime_ns
            self.assertEqual(output.read_bytes(), hardening.HardenServerSource(self.original))
            self.assertEqual(hardening.WriteHardenedSource(package, root / "build"), output)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            self.assertEqual((package / "src/NimBLEServer.cpp").read_bytes(), self.original)

    def test_middleware_reads_dependency_source_instead_of_build_variant(self):
        # PlatformIO passes a build variant File node to the middleware.
        # Its srcnode() is the dependency source, which owns the properties.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / "dependencies/NimBLE-Arduino"
            (package / "src").mkdir(parents=True)
            source = package / "src/NimBLEServer.cpp"
            source.write_bytes(self.original)
            shutil.copy2(FIXTURE / "library.properties", package)
            variant = root / "build/lib123/NimBLE-Arduino/NimBLEServer.cpp"

            class Node:
                def __init__(self, path, sourceNode=None):
                    self.path = path
                    self.sourceNode = sourceNode

                def get_abspath(self):
                    return str(self.path)

                def srcnode(self):
                    return self.sourceNode or self

            class Environment:
                def subst(self, expression):
                    testCase.assertEqual(expression, "$BUILD_DIR")
                    return str(root / "build")

                def File(self, path):
                    return Node(Path(path))

            testCase = self
            result = hardening._HardenNode(Environment(), Node(variant, Node(source)))
            output = root / "build/litch_sdk/NimBLEServer.cpp"
            self.assertEqual(result.path, output)
            self.assertEqual(output.read_bytes(), hardening.HardenServerSource(self.original))
            self.assertEqual(source.read_bytes(), self.original)
            self.assertFalse(variant.exists())

    def test_unknown_version_rejects_without_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / "NimBLE-Arduino"
            package.mkdir()
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.8\n")
            with self.assertRaises(ValueError):
                hardening.WriteHardenedSource(package, root / "build")
            self.assertFalse((root / "build").exists())

    def test_same_sdk_callback_corpus_old_fails_new_passes(self):
        # Compile the exact callback cases from the pinned production SDK and
        # its build copy. GAP structures, lookup and delete are controlled here;
        # the S3 build must separately prove that the actual SDK uses the copy.
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "A C++ compiler is required for SDK callback evidence")
        patched = hardening.HardenServerSource(self.original).decode()
        caseStart = "        case BLE_GAP_EVENT_REPEAT_PAIRING: {"
        caseEnd = "        } // BLE_GAP_EVENT_REPEAT_PAIRING"

        def case(source):
            start = source.index(caseStart)
            end = source.index(caseEnd, start) + len(caseEnd)
            return source[start:end]

        harness = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
// NimBLE 2.3.7 ble_gap.h defines RETRY=1 and IGNORE=2.
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_GAP_REPEAT_PAIRING_IGNORE 2
#define BLE_GAP_EVENT_REPEAT_PAIRING 1
struct Descriptor { int peer_id_addr = 0; };
struct PeerInfo { Descriptor m_desc; };
struct Event { int type; struct { uint16_t conn_handle; } repeat_pairing; };
int deletes = 0, lookups = 0;
bool found = false;
int ble_gap_conn_find(uint16_t, Descriptor* descriptor) {
    ++lookups;
    if (!found) return 1;
    descriptor->peer_id_addr = 42;
    return 0;
}
int ble_store_util_delete_peer(const int*) { ++deletes; return 0; }
"""
        for name, source in [("Old", self.original.decode()), ("New", patched)]:
            harness += (
                f"int {name}(Event* event) {{ PeerInfo peerInfo; int rc = 0;"
                "switch (event->type) {\n" + case(source) + "\n} return -1; }\n"
            )
        harness += r"""
int main(int argc, char** argv) {
    const bool old = argc == 2 && std::strcmp(argv[1], "old") == 0;
    int failures = 0;
    for (const uint16_t handle : {uint16_t(0), uint16_t(1), uint16_t(65535)}) {
        for (int exists = 0; exists != 2; ++exists) {
            found = exists != 0; deletes = lookups = 0;
            Event event{BLE_GAP_EVENT_REPEAT_PAIRING, {handle}};
            const int result = old ? Old(&event) : New(&event);
            const bool safe = result == BLE_GAP_REPEAT_PAIRING_IGNORE && deletes == 0;
            failures += !safe;
            std::printf("repeat mode=%s handle=%u found=%d result=%d deletes=%d lookups=%d safe=%d\n",
                        old ? "old" : "new", handle, exists, result, deletes, lookups, safe);
        }
    }
    std::printf("repeat failures=%d\n", failures);
    return failures ? 1 : 0;
}
"""
        harness = harness.replace("#include <cstdint>", "#include <cstdint>\n#include <initializer_list>")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "repeat.cpp"
            source.write_text(harness)
            build = subprocess.run([compiler, "-std=c++17", str(source), "-o", str(root / "repeat")], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            old = subprocess.run([str(root / "repeat"), "old"], capture_output=True, text=True)
            new = subprocess.run([str(root / "repeat"), "new"], capture_output=True, text=True)
            self.assertEqual(old.returncode, 1, old.stdout)
            self.assertIn("repeat failures=3", old.stdout)
            self.assertEqual(new.returncode, 0, new.stdout)
            self.assertIn("repeat failures=0", new.stdout)
            print(old.stdout + new.stdout, end="")


if __name__ == "__main__":
    unittest.main()
