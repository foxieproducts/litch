"""Pinned SDK C bodies with explicit native seams; physical BLE remains required."""

import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = Path(os.environ.get("LITCH_ATT_TEST_REPO", str(HERE.parents[2])))
WITNESS = Path(os.environ.get("LITCH_ATT_TEST_WITNESS", str(REPO)))
MIDDLEWARE = Path(os.environ.get("LITCH_ATT_TEST_MIDDLEWARE",
                                 str(REPO / "tools/pio/harden_nimble_repeat_pairing.py")))
FIXTURE = HERE / "fixtures/nimble_237/att_host"
MODULE = runpy.run_path(str(MIDDLEWARE))


class SdkAttWitnessTests(unittest.TestCase):
    def test_actual_sdk_original_and_observed_seams(self):
        with tempfile.TemporaryDirectory(prefix="litch_sdk_att_proof_") as temporary:
            env = dict(os.environ)
            env.update(LITCH_PROOF_REPO=str(REPO), LITCH_PROOF_WITNESS=str(WITNESS),
                       LITCH_PROOF_MIDDLEWARE=str(MIDDLEWARE), LITCH_PROOF_SDK=str(FIXTURE),
                       LITCH_PROOF_HARNESS=str(HERE / "fixtures/nimble_237/att_corpus.c"),
                       LITCH_PROOF_OUTPUT=temporary)
            result = subprocess.run([sys.executable, str(HERE / "sdk_att_corpus.py")],
                                    env=env, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS 9 scenarios", result.stdout)

    def test_drift_fails_closed(self):
        for name in ("ble_att.c", "ble_att_svr.c"):
            original = (FIXTURE / "src" / name).read_bytes()
            for source in (b"", original + b"\n"):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    MODULE["ObserveAttSource"](name, source)

    def test_copy_preserves_sdk_and_incremental_timestamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "NimBLE-Arduino"
            shutil.copytree(FIXTURE, package / "src/nimble/nimble/host")
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.7\n")
            for name in ("ble_att.c", "ble_att_svr.c"):
                source = package / "src/nimble/nimble/host/src" / name
                original = source.read_bytes()
                destination = MODULE["WriteObservedAttSource"](package, root / "build", name)
                timestamp = destination.stat().st_mtime_ns
                self.assertEqual(destination.read_bytes(), MODULE["ObserveAttSource"](name, original))
                MODULE["WriteObservedAttSource"](package, root / "build", name)
                self.assertEqual(destination.stat().st_mtime_ns, timestamp)
                self.assertEqual(source.read_bytes(), original)
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.8\n")
            with self.assertRaises(ValueError):
                MODULE["WriteObservedAttSource"](package, root / "unsupported", "ble_att.c")
            self.assertFalse((root / "unsupported").exists())

    def test_variant_node_preserves_private_header_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "NimBLE-Arduino"
            shutil.copytree(FIXTURE, package / "src/nimble/nimble/host")
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.7\n")
            source = package / "src/nimble/nimble/host/src/ble_att.c"
            variant = root / "build/library/ble_att.c"
            class Node:
                def __init__(self, path, sourceNode=None):
                    self.path, self.sourceNode = path, sourceNode
                def srcnode(self):
                    return self.sourceNode or self
                def get_abspath(self):
                    return str(self.path)
            class Environment:
                includes = []
                def subst(self, expression):
                    return {"$BUILD_DIR": str(root / "build"),
                            "$PROJECT_INCLUDE_DIR": str(root / "project/include")}[expression]
                def AppendUnique(self, **values):
                    self.includes.extend(values["CPPPATH"])
                def File(self, path):
                    return Node(Path(path))
            env = Environment()
            result = MODULE["_ObserveAttNode"](env, Node(variant, Node(source)))
            self.assertEqual(result.path, root / "build/litch_sdk/ble_att.c")
            self.assertEqual(env.includes, [str(source.parent), str(root / "project/include")])
            self.assertFalse(variant.exists())


if __name__ == "__main__":
    unittest.main()
