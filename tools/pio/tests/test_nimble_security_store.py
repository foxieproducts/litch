"""Actual pinned SDK security bodies with controlled NVS; S3 proves ABI/linkage."""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "harden_nimble_repeat_pairing.py"
FIXTURE = Path(__file__).parent / "fixtures/nimble_237"
spec = importlib.util.spec_from_file_location("litch_security_store", SCRIPT)
hardening = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardening)


def Function(source: str, name: str) -> str:
    match = re.search(r"(?m)^(?:static )?(?:int|void)(?:\n| )" + name + r"\(", source)
    if match is None:
        raise ValueError("SDK function not found: " + name)
    brace = source.index("{", match.start())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def Corpus(config: str, nvs: str, util: str, hardened: bool) -> str:
    config_names = (
        "ble_store_config_compare_bond_count", "ble_store_config_find_sec",
        "ble_store_config_read_our_sec", "ble_store_config_write_our_sec",
        "ble_store_config_delete_obj", "ble_store_config_delete_sec",
        "ble_store_config_delete_our_sec", "ble_store_config_delete_peer_sec",
        "ble_store_config_read_peer_sec", "ble_store_config_write_peer_sec",
    )
    nvs_names = (
        "get_nvs_key_string", "get_nvs_matching_index", "get_nvs_max_obj_value",
        "get_nvs_db_value", "get_nvs_db_attribute", "ble_nvs_delete_value",
        "ble_nvs_write_key_value", "ble_store_nvs_write", "populate_db_from_nvs",
        "ble_nvs_restore_sec_keys", "ble_store_config_persist_cccds",
        "ble_store_config_persist_csfcs", "ble_store_config_persist_our_secs",
        "ble_store_config_persist_peer_secs",
    )
    body = "\n".join(Function(config, name) for name in config_names)
    # Keep the generated NVS translation unit's real declaration order. Moving
    # its helper before callers here hid an S3 static-declaration build failure.
    nvs_parts = []
    for name in nvs_names:
        function = Function(nvs, name)
        nvs_parts.append((nvs.index(function), function))
    include = '#include "litch_nimble_security_store.inc"'
    if hardened:
        nvs_parts.extend((match.start(), include) for match in re.finditer(re.escape(include), nvs))
    body += "\n" + "\n".join(part for _, part in sorted(nvs_parts))
    utils = "\n".join(Function(util, name) for name in (
        "ble_store_util_delete_all", "ble_store_util_delete_peer"))
    return (FIXTURE / "security_store_corpus.c").read_text().replace(
        "/* SDK_FUNCTIONS */", body).replace("/* SDK_DELETE_UTILS */", utils)


class DurableSecurityStoreTests(unittest.TestCase):
    def test_source_guards_and_single_generated_config_owner(self):
        original = (FIXTURE / "ble_store_config.c").read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), hardening.CONFIG_ORIGINAL_SHA256)
        self.assertEqual(hashlib.sha256((FIXTURE / "ble_store_util.c").read_bytes()).hexdigest(),
                         "a25a4886c9cd47522287d9f43eb7e917c8e1383cd4247701bd2c5b875304524a")
        with self.assertRaises(ValueError):
            hardening.HardenConfigSource(original + b"\n")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "deps/NimBLE-Arduino"
            source = package / "src/nimble/nimble/host/store/config/src/ble_store_config.c"
            source.parent.mkdir(parents=True)
            source.write_bytes(original)
            shutil.copy2(FIXTURE / "library.properties", package)
            # Host checks use the licensed pinned fixture; production also
            # checks the complete installed dependency before generating output.
            privacy = FIXTURE / "ble_hs_pvcy.c"
            self.assertEqual(hashlib.sha256(privacy.read_bytes()).hexdigest(), hardening.PRIVACY_ORIGINAL_SHA256)
            privacy_copy = package / "src/nimble/nimble/host/src/ble_hs_pvcy.c"
            privacy_copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(privacy, privacy_copy)
            class Node:
                def __init__(self, path, original=None): self.path, self.original = path, original
                def get_abspath(self): return str(self.path)
                def srcnode(self): return self.original or self
            case = self
            class Environment:
                def subst(self, value):
                    case.assertIn(value, ("$BUILD_DIR", "$PROJECT_DIR"))
                    return str(root / "build" if value == "$BUILD_DIR" else SCRIPT.parents[2])
                def AppendUnique(self, **values):
                    case.assertEqual(values, {"CPPPATH": [str(source.parent)]})
                def File(self, path): return Node(Path(path))
            variant = root / "build/lib123/NimBLE-Arduino/ble_store_config.c"
            # SCons executes middleware without defining module __file__.
            script_file = hardening.__dict__.pop("__file__")
            try:
                output = hardening._HardenConfigNode(Environment(), Node(variant, Node(source))).path
            finally:
                hardening.__dict__["__file__"] = script_file
            self.assertEqual(output, root / "build/litch_sdk/ble_store_config.c")
            self.assertEqual(output.read_bytes(), hardening.HardenConfigSource(original))
            stamp = output.stat().st_mtime_ns
            self.assertEqual(hardening.WriteHardenedConfigSource(package, root / "build"), output)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            self.assertEqual(source.read_bytes(), original)
            self.assertFalse(variant.exists())
            privacy_copy.write_bytes(privacy.read_bytes() + b"\n")
            with self.assertRaises(ValueError):
                hardening.WriteHardenedConfigSource(package, root / "drifted_privacy")
            self.assertFalse((root / "drifted_privacy").exists())
            shutil.copy2(privacy, privacy_copy)
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.8\n")
            with self.assertRaises(ValueError):
                hardening.WriteHardenedConfigSource(package, root / "unknown")
            self.assertFalse((root / "unknown").exists())

    def test_old_false_success_new_exact_content_retry_and_reopen(self):
        compiler = shutil.which("clang")
        self.assertIsNotNone(compiler, "clang sanitizers are required for SDK storage evidence")
        config = (FIXTURE / "ble_store_config.c").read_bytes()
        nvs = (FIXTURE / "ble_store_nvs.c").read_bytes()
        util = (FIXTURE / "ble_store_util.c").read_text()
        # Preserve the already accepted empty-store repair in the OLD control.
        old_nvs = nvs.replace(hardening.NVS_ORIGINAL_COUNTERS.encode(),
                              hardening.NVS_HARDENED_COUNTERS.encode())
        variants = [("old", config, old_nvs, False),
                    ("new", hardening.HardenConfigSource(config), hardening.HardenNvsSource(nvs), True)]
        old_failures = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for label, config_source, nvs_source, hardened in variants:
                path = root / (label + ".c")
                path.write_text(Corpus(config_source.decode(), nvs_source.decode(), util, hardened))
                build = subprocess.run([compiler, "-std=c11", "-O1", "-fsanitize=address,undefined",
                                        "-fno-sanitize-recover=all", "-I", str(SCRIPT.parent),
                                        str(path), "-o", str(root / label)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
                for case in range(21):
                    result = subprocess.run([str(root / label), str(case)], capture_output=True, text=True)
                    print(label.upper(), "case", case, "exit", result.returncode, result.stdout.strip())
                    if label == "new":
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(result.stderr, "")
                    elif result.returncode:
                        old_failures.append(case)
                        print(result.stderr.splitlines()[0])
            self.assertTrue({1, 2, 3, 4, 7, 8, 9, 10, 11, 13, 14, 15, 18, 19, 20}.issubset(old_failures), old_failures)


if __name__ == "__main__":
    unittest.main()
