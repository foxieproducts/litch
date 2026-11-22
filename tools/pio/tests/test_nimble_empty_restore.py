"""Exact SDK restore body; host types/NVS are controlled, S3 ABI is a build gate."""
from __future__ import annotations
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "harden_nimble_repeat_pairing.py"
FIXTURE = Path(__file__).parent / "fixtures/nimble_237"
spec = importlib.util.spec_from_file_location("litch_empty_restore", SCRIPT)
hardening = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hardening)


def RestoreBody(source):
    start = source.index("static int\nble_nvs_restore_sec_keys(void)")
    brace = source.index("{", start)
    end = brace + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class EmptyStoreRestoreTests(unittest.TestCase):
    def setUp(self):
        self.original = (FIXTURE / "ble_store_nvs.c").read_bytes()

    def test_only_reviewed_restore_readiness_and_native_persistence_sites_change(self):
        from test_nimble_security_store import Function
        actual = hardening.HardenNvsSource(self.original)
        # These are the complete changed NVS bodies. The rest of the pinned
        # SDK remains byte-exact, including unrelated local IRK/EAD owners.
        def without_reviewed(source):
            for name in ("get_nvs_db_value", "get_nvs_peer_record",
                         "ble_nvs_write_key_value", "ble_nvs_delete_value", "ble_nvs_restore_sec_keys",
                         "ble_store_config_conf_init", "ble_store_config_persist_peer_secs",
                         "ble_store_config_persist_our_secs", "ble_store_config_persist_cccds",
                         "ble_store_config_persist_csfcs", "ble_store_config_persist_rpa_recs"):
                source = source.replace(Function(source.decode(), name).encode(),
                                        ("<" + name + ">").encode())
            return source.replace(b'#include "litch_nimble_security_store.inc"\n\n', b"").replace(
                b'extern int LitchSdkNvsBeginAccess(void);\nextern void LitchSdkNvsEndAccess(void);\n', b'')
        self.assertEqual(without_reviewed(actual), without_reviewed(self.original))
        with self.assertRaises(ValueError):
            hardening.HardenNvsSource(self.original + b"\n")

    def test_source_node_private_header_and_copy_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / "deps/NimBLE-Arduino"
            source = package / "src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
            source.parent.mkdir(parents=True)
            source.write_bytes(self.original)
            shutil.copy2(FIXTURE / "library.properties", package)
            variant = root / "build/lib123/NimBLE-Arduino/ble_store_nvs.c"
            class Node:
                def __init__(self, path, original=None):
                    self.path, self.original = path, original
                def get_abspath(self):
                    return str(self.path)
                def srcnode(self):
                    return self.original or self
            class Environment:
                def subst(self, name):
                    case.assertIn(name, ("$BUILD_DIR", "$PROJECT_DIR"))
                    return str(root / "build") if name == "$BUILD_DIR" else str(SCRIPT.parents[2])
                def AppendUnique(self, **values):
                    case.assertEqual(values, {"CPPPATH": [str(source.parent)]})
                def File(self, path):
                    return Node(Path(path))
            case = self
            output = hardening._HardenNvsNode(Environment(), Node(variant, Node(source))).path
            self.assertEqual(output, root / "build/litch_sdk/ble_store_nvs.c")
            stamp = output.stat().st_mtime_ns
            self.assertEqual(output.read_bytes(), hardening.HardenNvsSource(self.original))
            self.assertEqual((output.parent / "litch_nimble_security_store.inc").read_bytes(),
                             SCRIPT.with_name("litch_nimble_security_store.inc").read_bytes())
            self.assertEqual(hardening.WriteHardenedNvsSource(package, root / "build", SCRIPT.with_name("litch_nimble_security_store.inc")), output)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            self.assertEqual(source.read_bytes(), self.original)
            self.assertFalse(variant.exists())
            (package / "library.properties").write_text("name=NimBLE-Arduino\nversion=2.3.8\n")
            with self.assertRaises(ValueError):
                hardening.WriteHardenedNvsSource(package, root / "unknown", SCRIPT.with_name("litch_nimble_security_store.inc"))
            self.assertFalse((root / "unknown").exists())

    def test_scons_node_without_file_global_uses_explicit_project_template(self):
        # Exact former writer/node bodies from the first rejected S3 candidate.
        # Import-based tests supplied __file__; SCons does not supply it.
        former = r"""
def WriteHardenedNvsSource(packageDir: Path, buildDir: Path) -> Path:
    properties = dict(
        line.split("=", 1)
        for line in (packageDir / "library.properties").read_text().splitlines()
        if "=" in line and not line.startswith("#")
    )
    if properties.get("name") != "NimBLE-Arduino" or properties.get("version") != "2.3.7":
        raise ValueError("Litch empty-store hardening requires NimBLE-Arduino 2.3.7")
    source = packageDir / "src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
    hardened = HardenNvsSource(source.read_bytes())
    include = buildDir / "litch_sdk/litch_nimble_security_store.inc"
    include.parent.mkdir(parents=True, exist_ok=True)
    body = Path(__file__).with_name("litch_nimble_security_store.inc").read_bytes()
    if not include.exists() or include.read_bytes() != body:
        include.write_bytes(body)
    destination = buildDir / "litch_sdk/ble_store_nvs.c"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != hardened:
        destination.write_bytes(hardened)
    return destination


def _HardenNvsNode(env, node):
    source = Path(node.srcnode().get_abspath())
    hardened = WriteHardenedNvsSource(source.parents[7], Path(env.subst("$BUILD_DIR")))
    # The generated copy retains its dependency-owned private header.
    env.AppendUnique(CPPPATH=[str(source.parent)])
    return env.File(str(hardened))

"""
        old_scope = dict(hardening.__dict__)
        old_scope.pop("__file__", None)
        exec(former, old_scope)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "deps/NimBLE-Arduino"
            source = package / "src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
            source.parent.mkdir(parents=True)
            source.write_bytes(self.original)
            shutil.copy2(FIXTURE / "library.properties", package)
            case = self
            class Node:
                def __init__(self, path, original=None): self.path, self.original = path, original
                def get_abspath(self): return str(self.path)
                def srcnode(self): return self.original or self
            class Environment:
                def subst(self, name):
                    case.assertIn(name, ("$BUILD_DIR", "$PROJECT_DIR"))
                    return str(root / "build") if name == "$BUILD_DIR" else str(SCRIPT.parents[2])
                def AppendUnique(self, **values):
                    case.assertEqual(values, {"CPPPATH": [str(source.parent)]})
                def File(self, path): return Node(Path(path))
            variant = Node(root / "build/lib123/NimBLE-Arduino/ble_store_nvs.c", Node(source))
            with self.assertRaisesRegex(NameError, "__file__"):
                old_scope["_HardenNvsNode"](Environment(), variant)
            print("OLD exact former SCons node: NameError __file__ with original source node")
            missing = object()
            previous = hardening.__dict__.pop("__file__", missing)
            try:
                self.assertNotIn("__file__", hardening.__dict__)
                output = hardening._HardenNvsNode(Environment(), variant).path
            finally:
                if previous is not missing:
                    hardening.__dict__["__file__"] = previous
            self.assertEqual(output.read_bytes(), hardening.HardenNvsSource(self.original))
            self.assertEqual((output.parent / "litch_nimble_security_store.inc").read_bytes(),
                             SCRIPT.with_name("litch_nimble_security_store.inc").read_bytes())
            self.assertEqual(source.read_bytes(), self.original)
            print("NEW actual SCons node: PASS without __file__, explicit PROJECT_DIR template")

    def test_generated_restore_keeps_sdk_counter_declarations(self):
        # Unlike the wider corpus, the real SDK NVS TU has no file-scope
        # counter declarations. Its restore body must retain those externs.
        fixture = (FIXTURE / "security_store_corpus.c").read_text()
        harness = fixture[:fixture.index("uint16_t ble_store_config_our_bond_count,")]
        harness += "\nstatic const char *LOG_TAG=\"counter-seam\";\n"
        harness += "static int populate_db_from_nvs(int t,void *d,int *n) { return 0; }\n"
        harness += "int ble_store_config_compare_bond_count(const void *a,const void *b) { return 0; }\n"
        source = RestoreBody(hardening.HardenNvsSource(self.original).decode())
        declarations = ["    extern uint16_t ble_store_config_" + kind + "_bond_count;\n"
                        for kind in ("our", "peer")]
        broken = source
        for declaration in declarations:
            broken = broken.replace(declaration, "")
        compiler = shutil.which("clang")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # The separate config TU owns the definitions, matching the SDK.
            definitions = root / "config.c"
            definitions.write_text("#include <stdint.h>\nuint16_t ble_store_config_our_bond_count,ble_store_config_peer_bond_count;\n")
            for label, body in (("old-missing-declaration", broken), ("new", source)):
                path = root / (label + ".c")
                path.write_text(harness + body + "\nint main(void) { return ble_nvs_restore_sec_keys(); }\n")
                result = subprocess.run([compiler, "-std=c11", str(path), str(definitions),
                                         "-o", str(root / label)], capture_output=True, text=True)
                if label == "new":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(subprocess.run([str(root / label)]).returncode, 0)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("undeclared identifier 'ble_store_config_our_bond_count'", result.stderr)
                    self.assertIn("undeclared identifier 'ble_store_config_peer_bond_count'", result.stderr)
                print("SDK counter declaration", label, "compile exit", result.returncode)

    def test_old_empty_restore_fails_new_handles_empty_and_present_keys(self):
        compiler = shutil.which("clang")
        self.assertIsNotNone(compiler, "clang sanitizer is required for native SDK bounds evidence")
        from test_nimble_security_store import Corpus
        config = hardening.HardenConfigSource((FIXTURE / "ble_store_config.c").read_bytes()).decode()
        nvs = hardening.HardenNvsSource(self.original).decode()
        corpus = Corpus(config, nvs, (FIXTURE / "ble_store_util.c").read_text(), True).split("int main(")[0]
        main = r"""
int main(int argc,char **argv) {
    int ourCount=atoi(argv[1]),peerCount=atoi(argv[2]);
    for(int i=0;i<ourCount;++i) { struct ble_store_value_sec v=value(i+1,17); if(put(1,&v)) return 2; }
    for(int i=0;i<peerCount;++i) { struct ble_store_value_sec v=value(i+1,17); if(put(2,&v)) return 2; }
    int rc=reboot();
    printf("restore our=%d peer=%d rc=%d counters=%u/%u\n",ourCount,peerCount,rc,ble_store_config_our_bond_count,ble_store_config_peer_bond_count);
    return rc || ble_store_config_num_our_secs!=ourCount || ble_store_config_num_peer_secs!=peerCount ||
        ble_store_config_our_bond_count!=ourCount || ble_store_config_peer_bond_count!=peerCount;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for label, source in [("old", self.original.decode()), ("new", hardening.HardenNvsSource(self.original).decode())]:
                path = root / (label + ".c")
                path.write_text(corpus.replace(RestoreBody(nvs), RestoreBody(source)) + main)
                build = subprocess.run([compiler, "-std=c11", "-O1", "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I", str(SCRIPT.parent), str(path), "-o", str(root / label)], capture_output=True, text=True)
                self.assertEqual(build.returncode, 0, build.stderr)
            old = subprocess.run([str(root / "old"), "0", "0"], capture_output=True, text=True)
            self.assertNotEqual(old.returncode, 0)
            self.assertIn("index -1", old.stderr)
            print("OLD empty return=" + str(old.returncode) + "\n" + old.stderr, end="")
            for our, peer in [(0,0),(1,0),(0,1),(1,1),(3,0),(0,3),(3,3)]:
                new = subprocess.run([str(root / "new"), str(our), str(peer)], capture_output=True, text=True)
                self.assertEqual(new.returncode, 0, new.stdout + new.stderr)
                self.assertEqual(new.stderr, "")
                print("NEW " + new.stdout, end="")

if __name__ == "__main__":
    unittest.main()
