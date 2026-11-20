"""Actual SDK queue/handoff boundaries with disclosed controlled native dependencies."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
HERE = Path(__file__).resolve().parent
REPO = Path(os.environ.get("LITCH_ACL_TEST_REPO", str(HERE.parents[2])))
WITNESS = Path(os.environ.get("LITCH_ACL_TEST_WITNESS", str(REPO)))
MIDDLEWARE = Path(os.environ.get("LITCH_ACL_TEST_MIDDLEWARE", str(REPO / "tools/pio/harden_nimble_repeat_pairing.py")))
class SdkAclWitnessTests(unittest.TestCase):
    def test_actual_sdk_queue_and_handoff_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="litch_sdk_acl_proof_") as temporary:
            env = dict(os.environ)
            env.update(LITCH_ACL_PROOF_OUTPUT=temporary, LITCH_ACL_PROOF_REPO=str(REPO),
                       LITCH_ACL_PROOF_WITNESS=str(WITNESS), LITCH_ACL_PROOF_MIDDLEWARE=str(MIDDLEWARE),
                       LITCH_ACL_PROOF_SDK=str(HERE / "fixtures/nimble_237/acl_host"),
                       LITCH_ACL_PROOF_HARNESS=str(HERE / "fixtures/nimble_237/acl_corpus.c"))
            result = subprocess.run([sys.executable, str(HERE / "sdk_acl_corpus.py")],
                                    env=env, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS 13 actual SDK C scenarios", result.stdout)
if __name__ == "__main__":
    unittest.main()
