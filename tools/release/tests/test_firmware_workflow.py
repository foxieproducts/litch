from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import firmware_workflow as publish


class CandidateBuildTests(unittest.TestCase):
    def test_packaging_failure_prevents_remote_dispatch(self):
        source = "a" * 40
        with (patch.object(publish.candidate, "git", return_value=source.encode()),
              patch.object(publish.candidate, "check_source"),
              patch.object(publish.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "preflight")) as local,
              patch.object(publish, "gh") as remote):
            with self.assertRaises(subprocess.CalledProcessError):
                publish.build("beta", "Reviewed S3 board checks")
        local.assert_called_once_with([sys.executable, str(publish.ROOT / "litch"), "release", "check", "--firmware"],
                                      cwd=publish.ROOT, check=True)
        remote.assert_not_called()

    def test_only_locally_verified_main_is_dispatched(self):
        source = "b" * 40
        with (patch.object(publish.candidate, "git", return_value=source.encode()),
              patch.object(publish.candidate, "check_source") as check,
              patch.object(publish.subprocess, "run"),
              patch.object(publish, "gh") as remote,
              patch.object(publish, "dispatch", return_value="17") as dispatch,
              patch.object(publish, "wait_for_run") as wait):
            remote.return_value = json.dumps({"object": {"sha": "c" * 40}})
            with self.assertRaisesRegex(publish.PublicationError, "main"):
                publish.build("beta", "Reviewed S3 board checks")
            dispatch.assert_not_called()
            remote.return_value = json.dumps({"object": {"sha": source}})
            publish.build("beta", "Reviewed S3 board checks")
        check.assert_called_with(publish.ROOT, source)
        dispatch.assert_called_once_with(dict(operation="build", channel="beta", source_sha=source,
                                              local_validation_ref="Reviewed S3 board checks"))
        wait.assert_called_once_with("17", timeout_seconds=7200)


class PromotionWindowTests(unittest.TestCase):
    def setUp(self):
        self.switch = "false"
        self.writes = []
        self.dispatches = []
        self.states = ["queued", "in_progress", "success"]
        self.owner = None
        self.active = []
        self.switch_at_read = []
        self.inputs = dict(operation="promote", channel="beta", source_sha="a" * 40,
                           candidate_run_id="12", candidate_run_attempt="1",
                           receipt_sha256="b" * 64, notes_sha256="c" * 64,
                           previous_manifest_sha256="d" * 64)
        for patched in (patch.object(publish, "gh", side_effect=self.github), patch.object(publish.time, "sleep")):
            patched.start()
            self.addCleanup(patched.stop)

    def github(self, *args):
        if len(args) > 3 and args[3].endswith("/dispatches"):
            self.assertEqual(args[:4], ("api", "--method", "POST",
                f"repos/{publish.REPO}/actions/workflows/firmware_candidate.yml/dispatches"))
            self.assertIn("ref=main", args)
            self.assertIn("X-GitHub-Api-Version: 2026-03-10", args)
            for key, value in self.inputs.items():
                self.assertIn(f"inputs[{key}]={value}", args)
            self.dispatches.append(args)
            return json.dumps(dict(workflow_run_id=42, html_url=publish.RUN_URL + "42"))
        if args[:3] == ("api", "--method", "PATCH"):
            self.assertEqual(args[3], publish.VARIABLES + "/" + publish.SWITCH)
            self.switch = args[-1].removeprefix("value=")
            self.writes.append(self.switch)
            return ""
        if args[-1] == publish.VARIABLES + "/" + publish.SWITCH:
            return json.dumps({"value": self.switch})
        if args[:3] == ("api", "--method", "POST"):
            if self.owner is not None:
                raise publish.PublicationError("HTTP 409")
            self.owner = args[-1].removeprefix("value=")
            return ""
        if args[-1] == publish.VARIABLES + "/" + publish.OWNER:
            if args[:3] == ("api", "--method", "DELETE"):
                self.owner = None
                return ""
            return json.dumps({"value": self.owner})
        if "actions/runs?" in args[-1]:
            return json.dumps([{"workflow_runs": self.active}])
        self.assertEqual(args, ("api", f"repos/{publish.REPO}/actions/runs/42"))
        self.switch_at_read.append(self.switch)
        state = self.states.pop(0)
        if isinstance(state, Exception):
            raise state
        done = state in ("success", "failure", "cancelled")
        run = dict(id=42, repository={"full_name": publish.REPO}, event="workflow_dispatch",
                   head_branch="main", path=".github/workflows/firmware_candidate.yml",
                   status="completed" if done else state, conclusion=state if done else None)
        return json.dumps(run)

    def test_404_and_temporary_network_failure_keep_one_dispatch_open_until_completed(self):
        self.states[:0] = [publish.PublicationError("gh: Not Found (HTTP 404)"),
                           subprocess.TimeoutExpired(["gh", "api"], 120)]
        publish.promote(self.inputs)
        self.assertEqual(self.writes, ["true", "false"])
        self.assertEqual(self.switch_at_read, ["true"] * 5)
        self.assertEqual(len(self.dispatches), 1)
        self.assertIsNone(self.owner)

    def test_failed_and_cancelled_runs_close_window_and_report_failure(self):
        for conclusion in ("failure", "cancelled"):
            with self.subTest(conclusion=conclusion):
                self.states = ["in_progress", conclusion]
                with self.assertRaisesRegex(publish.PublicationError, conclusion):
                    publish.promote(self.inputs)
                self.assertEqual(self.switch, "false")
                self.assertIsNone(self.owner)

    def test_preexisting_enabled_switch_is_preserved(self):
        self.switch = "true"
        publish.promote(self.inputs)
        self.assertEqual(self.writes, [])
        self.assertIsNone(self.owner)

    def test_verify_dispatches_only_the_read_only_operation_with_the_same_build(self):
        with patch.object(publish, "dispatch", return_value="42") as dispatch:
            publish.verify(self.inputs)
        dispatch.assert_called_once_with(dict(operation="verify", channel="beta", source_sha="a" * 40,
            candidate_run_id="12", candidate_run_attempt="1", receipt_sha256="b" * 64))
        self.assertEqual(self.writes, [])
        self.assertEqual(self.switch_at_read, ["false"] * 3)
        self.assertIsNone(self.owner)

    def test_timeout_closes_window_without_dispatching_again(self):
        self.states = [publish.PublicationError("HTTP 503")]
        with patch.object(publish.time, "monotonic", side_effect=[0, 0, 7200]):
            with self.assertRaisesRegex(publish.PublicationError, "42"):
                publish.promote(self.inputs)
        self.assertEqual(len(self.dispatches), 1)
        self.assertEqual(self.switch, "false")
        self.assertIsNone(self.owner)

    def test_wrong_run_identity_is_rejected_and_interruption_restores_the_switch(self):
        for error in (KeyboardInterrupt(), publish.PublicationError("Unexpected identity")):
            with self.subTest(error=type(error).__name__):
                def read(*args):
                    if args[-1].endswith("/actions/runs/42"):
                        if isinstance(error, KeyboardInterrupt):
                            raise error
                        return json.dumps({"id": 99})
                    return self.github(*args)
                with patch.object(publish, "gh", side_effect=read), self.assertRaises(type(error)):
                    publish.promote(self.inputs)
                self.assertEqual(self.switch, "false")
                self.assertIsNone(self.owner)

    def test_cleanup_failure_keeps_the_reservation_for_recovery(self):
        def cannot_close(*args):
            if args[:3] == ("api", "--method", "PATCH") and args[-1] == "value=false":
                raise publish.PublicationError("HTTP 503")
            return self.github(*args)
        with patch.object(publish, "gh", side_effect=cannot_close):
            with self.assertRaisesRegex(publish.PublicationError, "prior setting false"):
                publish.promote(self.inputs)
        self.assertEqual(self.switch, "true")
        self.assertIn("-prior-false-", self.owner)

    def test_ambiguous_dispatch_is_never_retried(self):
        def lost_response(*args):
            response = self.github(*args)
            if len(args) > 3 and args[3].endswith("/dispatches"):
                raise subprocess.TimeoutExpired(["gh", "api"], 120)
            return response
        with patch.object(publish, "gh", side_effect=lost_response):
            with self.assertRaisesRegex(publish.PublicationError, "dispatch"):
                publish.promote(self.inputs)
        self.assertEqual(len(self.dispatches), 1)
        self.assertEqual(self.writes, ["true", "false"])
        self.assertIsNone(self.owner)

    def test_second_operator_cannot_open_or_close_an_owned_window(self):
        self.owner = "other operator"
        with self.assertRaises(publish.PublicationError):
            publish.promote(self.inputs)
        self.assertEqual(self.owner, "other operator")
        self.assertEqual(self.writes, [])
        self.assertEqual(self.dispatches, [])

    def test_lost_reservation_acknowledgement_resumes_only_its_own_window(self):
        def lost_creation(*args):
            response = self.github(*args)
            if args[:4] == ("api", "--method", "POST", publish.VARIABLES):
                self.assertIn("-prior-false-", self.owner)
                raise publish.PublicationError("HTTP 502")
            return response
        with patch.object(publish, "gh", side_effect=lost_creation):
            publish.promote(self.inputs)
        self.assertEqual(len(self.dispatches), 1)
        self.assertIsNone(self.owner)

    def test_active_workflow_and_invalid_inputs_leave_switch_closed(self):
        self.active = [{"id": 9, "path": ".github/workflows/firmware_provenance.yml"}]
        with self.assertRaisesRegex(publish.PublicationError, "9"):
            publish.promote(self.inputs)
        with self.assertRaisesRegex(publish.PublicationError, "receipt_sha256"):
            publish.promote(dict(self.inputs, receipt_sha256="bad"))
        self.assertEqual(self.writes, [])
        self.assertEqual(self.dispatches, [])
        self.assertIsNone(self.owner)
