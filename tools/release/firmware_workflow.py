#!/usr/bin/env python3
"""Dispatch a locally checked firmware build or supervise one authorized channel promotion."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import time
from uuid import uuid4

from github_promotion import PublicationError, candidate, gh

REPO = candidate.REPOSITORY
WORKFLOW = "firmware_candidate.yml"
ENVIRONMENT = "firmware-publishing"
VARIABLES = f"repos/{REPO}/environments/{ENVIRONMENT}/variables"
SWITCH = "FIRMWARE_PUBLISH_ENABLED"
OWNER = "FIRMWARE_PROMOTION_OWNER"
RUN_URL = f"https://github.com/{REPO}/actions/runs/"
NUMBER = re.compile(r"[1-9][0-9]*")
ROOT = Path(__file__).resolve().parents[2]


def validate(inputs: dict) -> None:
    patterns = {
        "operation": re.compile("promote"), "channel": re.compile("beta|stable"),
        "source_sha": candidate.SOURCE_PATTERN, "candidate_run_id": NUMBER,
        "candidate_run_attempt": NUMBER, "receipt_sha256": candidate.artifact.SHA256_PATTERN,
        "notes_sha256": candidate.artifact.SHA256_PATTERN,
        "previous_manifest_sha256": candidate.artifact.SHA256_PATTERN,
    }
    if not isinstance(inputs, dict) or inputs.keys() != patterns.keys():
        raise PublicationError("Supply the eight reviewed workflow inputs for one promotion.")
    for key, pattern in patterns.items():
        if not isinstance(inputs[key], str) or not pattern.fullmatch(inputs[key]):
            raise PublicationError(f"Invalid promotion input {key}")


def wait_for_run(run_id: str, timeout_seconds: int = 3600) -> None:
    if not NUMBER.fullmatch(run_id):
        raise PublicationError("Expected a positive GitHub Actions run ID.")
    print(RUN_URL + run_id, flush=True)
    deadline = time.monotonic() + timeout_seconds
    previous = None
    while time.monotonic() < deadline:
        try:
            run = json.loads(gh("api", f"repos/{REPO}/actions/runs/{run_id}"))
        except (PublicationError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            # A dispatch can be acknowledged before either the run or its jobs are readable.
            if re.search(r"HTTP 4\d\d", str(error)) and not re.search(
                r"HTTP (404|408|429)|rate limit", str(error), re.I
            ):
                raise
            state = "awaiting GitHub readback"
        else:
            path = f".github/workflows/{WORKFLOW}"
            if (run.get("id") != int(run_id) or run.get("repository", {}).get("full_name") != REPO
                    or run.get("path") not in (path, path + "@main", path + "@refs/heads/main")
                    or run.get("head_branch") != "main" or run.get("event") != "workflow_dispatch"):
                raise PublicationError(f"Unexpected identity for firmware run {run_id}")
            state = run.get("status")
            if state == "completed" and run.get("conclusion"):
                conclusion = run["conclusion"]
                if conclusion != "success":
                    raise PublicationError(f"Firmware run {run_id} concluded {conclusion}.")
                print(f"Firmware run {run_id} succeeded.", flush=True)
                return
        if state != previous:
            print(f"Firmware run {run_id}: {state}", flush=True)
            previous = state
        time.sleep(8)
    raise PublicationError(f"GitHub has not confirmed completion of run {run_id}. Inspect the same run before retrying.")


def read_variable(name: str) -> str:
    for attempt in range(1, 5):
        try:
            return json.loads(gh("api", f"{VARIABLES}/{name}"))["value"]
        except (PublicationError, subprocess.TimeoutExpired, json.JSONDecodeError):
            if attempt == 4:
                raise
            time.sleep(2)


def set_switch(value: str) -> None:
    for attempt in range(3):
        try:
            gh("api", "--method", "PATCH", f"{VARIABLES}/{SWITCH}", "-f", f"value={value}")
            break
        except (PublicationError, subprocess.TimeoutExpired):
            if read_variable(SWITCH) == value:
                return
            if value == "true" or attempt == 2:
                raise
            time.sleep(2)
    for _ in range(4):
        if read_variable(SWITCH) == value:
            return
        time.sleep(2)
    raise PublicationError(f"Could not confirm {SWITCH}={value} in {ENVIRONMENT}.")


def require_idle_publishers() -> None:
    workflows = {f".github/workflows/{name}" for name in (WORKFLOW, "firmware_provenance.yml")}
    for status in ("in_progress", "queued", "waiting", "pending", "requested"):
        path = f"repos/{REPO}/actions/runs?per_page=100&status={status}"
        pages = json.loads(gh("api", "--paginate", "--slurp", path))
        for page in pages:
            for run in page["workflow_runs"]:
                if run["path"].split("@", 1)[0] in workflows:
                    raise PublicationError(f"Firmware run {run['id']} is already active. Check it before promoting.")


def dispatch(inputs: dict) -> str:
    fields = []
    for key, value in inputs.items():
        fields.extend(("-f", f"inputs[{key}]={value}"))
    try:
        receipt = json.loads(gh(
            "api", "--method", "POST", f"repos/{REPO}/actions/workflows/{WORKFLOW}/dispatches",
            "-H", "X-GitHub-Api-Version: 2026-03-10", "-f", "ref=main", *fields,
        ))
    except (PublicationError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        raise PublicationError(f"Check Actions for the original dispatch before retrying. {error}") from error
    run_id = receipt.get("workflow_run_id") if isinstance(receipt, dict) else None
    if type(run_id) is not int or run_id < 1 or receipt.get("html_url") != RUN_URL + str(run_id):
        raise PublicationError("Unexpected dispatch receipt. Inspect Actions before retrying.")
    return str(run_id)


def build(channel: str, evidence: str) -> None:
    source = candidate.git(ROOT, "rev-parse", "HEAD").decode().strip()
    candidate.check_source(ROOT, source)
    candidate.validate_evidence_ref(evidence)
    subprocess.run([sys.executable, str(ROOT / "litch"), "release", "check", "--firmware"],
                   cwd=ROOT, check=True)
    candidate.check_source(ROOT, source)
    main = json.loads(gh("api", f"repos/{REPO}/git/ref/heads/main"))
    if main["object"]["sha"] != source:
        raise PublicationError("The locally checked commit must match GitHub main to build an official candidate.")
    run_id = dispatch(dict(operation="build", source_sha=source, channel=channel, local_validation_ref=evidence))
    wait_for_run(run_id, timeout_seconds=7200)


def promote(inputs: dict) -> None:
    validate(inputs)
    previous = read_variable(SWITCH)
    if previous not in ("false", "true"):
        raise PublicationError(f"Expected an explicit true or false for {SWITCH}.")
    # Creating this variable is exclusive across operator machines. Setting it would overwrite another owner.
    owner = f"{inputs['channel']}-{inputs['candidate_run_id']}-prior-{previous}-{uuid4().hex}"
    try:
        gh("api", "--method", "POST", VARIABLES, "-f", f"name={OWNER}", "-f", f"value={owner}")
    except (PublicationError, subprocess.TimeoutExpired) as error:
        # A lost creation response can still leave our exact reservation in GitHub.
        try:
            current_owner = read_variable(OWNER)
        except (PublicationError, subprocess.TimeoutExpired) as lookup:
            raise PublicationError(f"Could not confirm the reservation. Inspect {OWNER} in {ENVIRONMENT}.") from lookup
        if current_owner != owner:
            raise PublicationError(f"Could not reserve {ENVIRONMENT}. Check {OWNER} before retrying. {error}") from error
    restore = False
    try:
        require_idle_publishers()
        if read_variable(SWITCH) != previous:
            raise PublicationError(f"{SWITCH} changed during admission. Check the publishing window.")
        restore = previous == "false"
        if restore:
            set_switch("true")
        wait_for_run(dispatch(inputs))
    finally:
        try:
            if read_variable(OWNER) != owner:
                raise PublicationError("Publishing window ownership changed.")
            if restore:
                set_switch("false")
            gh("api", "--method", "DELETE", f"{VARIABLES}/{OWNER}")
        except (PublicationError, subprocess.TimeoutExpired) as error:
            raise PublicationError(f"Could not confirm window cleanup. Inspect {OWNER} and {SWITCH} "
                                   f"in {ENVIRONMENT} (prior setting {previous}). {error}") from error


def verify(inputs: dict) -> None:
    validate(inputs)
    fields = {key: inputs[key] for key in ("channel", "source_sha", "candidate_run_id",
                                           "candidate_run_attempt", "receipt_sha256")}
    wait_for_run(dispatch(dict(operation="verify", **fields)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    start = commands.add_parser("build", help="check the S3 source bundle locally before dispatching a build")
    start.add_argument("channel", choices=("beta", "stable"))
    start.add_argument("--validation-ref", required=True, help="public reference to local checks for this commit")
    commands.add_parser("promote", help="promote the reviewed candidate").add_argument("inputs", type=Path)
    commands.add_parser("verify", help="check delivery using the same promotion inputs").add_argument("inputs", type=Path)
    commands.add_parser("wait", help="read the status of an existing firmware run").add_argument("run_id")
    args = parser.parse_args()
    if args.action == "build":
        build(args.channel, args.validation_ref)
    elif args.action == "wait":
        wait_for_run(args.run_id, timeout_seconds=7200)
    elif args.action == "promote":
        promote(json.loads(args.inputs.read_text()))
    else:
        verify(json.loads(args.inputs.read_text()))


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise KeyboardInterrupt

    for sig in (signal.SIGHUP, signal.SIGTERM):
        signal.signal(sig, interrupted)
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("Interrupted. Check the existing Actions run and publishing window before retrying.")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        sys.exit(re.sub(r"(?i)[0-9a-f]{64}", "[digest]", str(error)))
