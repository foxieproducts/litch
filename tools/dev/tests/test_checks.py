from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

from tools.dev.checks import run_tools_check, run_tools_static_checks, validate_vscode_tasks
from tools.dev.runner import DevError
from tools.dev.cli import _parser


class StaticCheckTests(unittest.TestCase):
    def test_tools_default_is_static_and_aggregate_requires_explicit_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / '.vscode').mkdir()
            (root / 'platformio.ini').write_text('[env:native]\nplatform = native\n')
            (root / '.vscode/tasks.json').write_text('{"tasks": []}')
            runner = Mock()
            run_tools_static_checks(SimpleNamespace(root=root), runner)
            commands = [call.args[0] for call in runner.run.call_args_list]
            self.assertEqual(len(commands), 2)
            self.assertIn('compileall', commands[0])
            self.assertEqual(commands[1], ['git', 'diff', '--check'])
            runner.reset_mock()
            run_tools_check(SimpleNamespace(root=root), runner)
            self.assertEqual([call.args[0] for call in runner.run.call_args_list], commands)
            runner.reset_mock()
            run_tools_check(SimpleNamespace(root=root), runner, include_tests=True)
            self.assertEqual([call.args[0] for call in runner.run.call_args_list][:-1], commands)
            self.assertIn('unittest', runner.run.call_args.args[0])

    def test_checked_in_editor_tasks_parse_with_prompt_values(self) -> None:
        root = Path(__file__).resolve().parents[3]
        decoded = json.loads((root / ".vscode" / "tasks.json").read_text(encoding="utf-8"))
        parser = _parser()
        for task in decoded["tasks"]:
            with self.subTest(task=task["label"]):
                args = ["exact-fixture-device" if value.startswith("${input:") else value
                        for value in task.get("args", [])]
                parser.parse_args(args)

    def test_vscode_tasks_require_litch_process_wrappers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / ".vscode" / "tasks.json"
            path.parent.mkdir(parents=True)
            path.write_text(
                json.dumps(
                    {
                        "version": "2.0.0",
                        "tasks": [
                            {
                                "label": "Build S3",
                                "type": "process",
                                "command": "${workspaceFolder}/litch",
                                "args": ["build", "s3"],
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            validate_vscode_tasks(root)

            decoded = json.loads(path.read_text(encoding="utf-8"))
            decoded["tasks"][0]["command"] = "pio"
            path.write_text(json.dumps(decoded), encoding="utf-8")
            with self.assertRaisesRegex(DevError, "bypasses"):
                validate_vscode_tasks(root)

    def test_vscode_tasks_do_not_advertise_legacy_firmware_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / ".vscode" / "tasks.json"
            path.parent.mkdir(parents=True)
            for target in ("c3", "all"):
                path.write_text(
                    json.dumps(
                        {
                            "version": "2.0.0",
                            "tasks": [
                                {
                                    "label": "Legacy firmware",
                                    "type": "process",
                                    "command": "${workspaceFolder}/litch",
                                    "args": ["build", target],
                                }
                            ],
                        }
                    ),
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(DevError, "explicit-only legacy"):
                    validate_vscode_tasks(root)


if __name__ == "__main__":
    unittest.main()
