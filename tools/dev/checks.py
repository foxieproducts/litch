"""Local static and focused host-tool checks."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from .doctor import active_port_overrides
from .runner import DevError, Runner


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DevError(f"invalid JSON in {path}: {error}") from error


def validate_vscode_tasks(root: Path) -> None:
    decoded = _load_json(root / ".vscode" / "tasks.json")
    if not isinstance(decoded, dict) or not isinstance(decoded.get("tasks"), list):
        raise DevError(".vscode/tasks.json must contain a tasks array")
    for task in decoded["tasks"]:
        if not isinstance(task, dict):
            raise DevError("every VS Code task must be an object")
        label = str(task.get("label", "(unlabeled)"))
        command = str(task.get("command", ""))
        if command not in {"${workspaceFolder}/litch", "./litch"}:
            raise DevError(f"VS Code task {label!r} bypasses the ./litch entrypoint: {command!r}")
        args = task.get("args", [])
        if not isinstance(args, list) or not all(isinstance(item, str) for item in args):
            raise DevError(f"VS Code task {label!r} must use a string args array")
        joined = " ".join(args).lower()
        if (
            len(args) >= 2
            and args[0] in {"build", "flash", "monitor", "bin"}
            and args[1] in {"c3", "all"}
        ):
            raise DevError(
                f"VS Code task {label!r} advertises the explicit-only legacy firmware target"
            )
        if any(token in joined for token in ("emulator", "publish", "upload_firmware_site")):
            raise DevError(f"VS Code task {label!r} exposes a non-canonical/destructive lane")


def run_tools_check(workspace: object, runner: Runner, *, include_tests: bool = False) -> None:
    run_tools_static_checks(workspace, runner)
    if not include_tests:
        return
    root = Path(getattr(workspace, "root"))
    runner.run(
        [sys.executable, "-m", "unittest", "discover", "-s", "tools/dev/tests", "-v"],
        cwd=root,
        timeout_seconds=180,
    )


def run_tools_static_checks(workspace: object, runner: Runner) -> None:
    """Keep cheap source/configuration guards fresh when unit evidence is reused."""
    root = Path(getattr(workspace, "root"))
    overrides = active_port_overrides(root / "platformio.ini")
    if overrides:
        raise DevError(f"active local port overrides are prohibited: {', '.join(overrides)}")
    validate_vscode_tasks(root)
    runner.run(
        [sys.executable, "-m", "compileall", "-q", "tools/dev"],
        cwd=root,
        timeout_seconds=60,
    )
    runner.run(["git", "diff", "--check"], cwd=root, timeout_seconds=30)
