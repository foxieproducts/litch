"""Compact terminal selection over the existing exact USB flash workflow."""

from __future__ import annotations

import shlex
from collections import Counter
from typing import Callable

from . import hardware, local_state
from .locks import LockError
from .runner import DevError, Runner
from .workspace import Workspace


class UsbDeviceMenu:
    def __init__(self, workspace: Workspace, runner: Runner, *, lock_timeout: float = 30.0) -> None:
        self.workspace = workspace
        self.runner = runner
        self.lock_timeout = lock_timeout
        self.state_path = workspace.root / ".cache" / hardware.LOCAL_STATE_NAME
        self.choices = local_state.usb_choices(self.state_path)
        if not self.choices:
            previous = hardware._remembered_flash(self.state_path, "s3")
            if previous is not None and previous.transport == "usb" and previous.usb_identity:
                self.choices[previous.usb_identity] = {"target": "s3", "selected": True}
        self.explicit_targets: dict[str, str] = {}
        self.candidates: tuple[hardware.FlashCandidate, ...] = ()
        self.selected: set[str] = set()
        self.discovery_ok = False
        self.serial_counts: Counter[str] = Counter()

    def _target(self, candidate: hardware.FlashCandidate) -> str | None:
        if candidate.target_hint == "s3":
            return "s3"
        explicit = self.explicit_targets.get(candidate.identity)
        if explicit is not None:
            return explicit
        choice = self.choices.get(candidate.identity, {})
        # Legacy hardware always needs a fresh explicit target request.
        return "s3" if choice.get("target") == "s3" else None

    def refresh(self) -> None:
        print("Looking for possible Litch USB boards...", flush=True)
        try:
            candidates = hardware.discover_possible_usb_boards(
                self.workspace, self.runner, self.lock_timeout
            )
        except (DevError, LockError) as error:
            self.discovery_ok = False
            print(f"Device list unavailable: {error}\nUse l to retry, m for setup commands, or q to exit.")
            return
        self.candidates = candidates
        self.serial_counts = Counter(candidate.identity for candidate in candidates)
        self.discovery_ok = True
        self.selected = {
            candidate.identity for candidate in candidates
            if candidate.usb_parent is not None and self.serial_counts[candidate.identity] == 1
            and self._target(candidate) == "s3"
            and self.choices.get(candidate.identity, {}).get("selected") is True
        } | {
            candidate.identity for candidate in candidates
            if candidate.usb_parent is not None and self.serial_counts[candidate.identity] == 1
            and candidate.identity in self.selected
            and candidate.identity in self.explicit_targets
        }
        self.show()

    def show(self) -> None:
        print("Possible Litch USB boards (* selected):")
        if not self.candidates:
            print("  No possible boards connected. Connect a board, then enter l.")
        for index, candidate in enumerate(self.candidates, start=1):
            marker = "*" if candidate.identity in self.selected else " "
            target = self._target(candidate)
            status = target.upper() if target else "target unknown"
            if candidate.usb_parent is None:
                status = "cannot select: USB identity incomplete"
            elif self.serial_counts[candidate.identity] != 1:
                status = "cannot select: duplicate USB serial"
            port = candidate.ports[0] if candidate.ports else "no serial port"
            print(f"  {index}. [{marker}] {candidate.identity or 'serial unknown'} | {status} | {port}")
        live = {candidate.identity for candidate in self.candidates}
        for serial in self.choices:
            if serial not in live:
                print(f"  Remembered {serial}: not connected (not selected)")
        if self.selected:
            print(f"Selected: {len(self.selected)} board(s). f flashes them one at a time.")

    def _save(self) -> None:
        # Keep disconnected hints, bounded to the most recently edited boards.
        choices = dict(self.choices)
        for candidate in self.candidates:
            target = self._target(candidate)
            if candidate.usb_parent is not None and target is not None:
                choices.pop(candidate.identity, None)
                choices[candidate.identity] = {
                    "target": target, "selected": candidate.identity in self.selected,
                }
        choices = dict(list(choices.items())[-local_state.MAX_USB_CHOICES:])
        local_state.remember_usb_choices(self.state_path, choices)
        self.choices = choices

    def _row(self, raw: str) -> hardware.FlashCandidate:
        if not raw.isascii() or not raw.isdigit() or not 1 <= int(raw) <= len(self.candidates):
            raise DevError("Use a connected board number from the list.", exit_code=2)
        candidate = self.candidates[int(raw) - 1]
        if candidate.usb_parent is None:
            raise DevError("That board has no complete USB identity. Reconnect it and enter l.", exit_code=2)
        if self.serial_counts[candidate.identity] != 1:
            raise DevError("That USB serial is ambiguous. Disconnect its duplicate and enter l.", exit_code=2)
        return candidate

    def _ask_target(self, candidate: hardware.FlashCandidate) -> str | None:
        if candidate.target_hint == "s3":
            print(f"{candidate.identity} identifies itself as S3; no target choice is needed.")
            return "s3"
        print(f"{candidate.identity}: USB boot mode does not identify the board model.")
        print("Enter s3 for an S3 board, or the exact target you explicitly requested; q cancels.")
        while True:
            raw = input("Target [q cancels]: ").strip().lower()
            if raw in {"", "q"}:
                print("Target choice cancelled; selection unchanged.")
                return None
            if raw in hardware.TARGET_ENVIRONMENTS:
                return raw
            print("Target not recognized. Enter s3 or your explicitly requested target; q cancels.")

    def select(self, raw: str) -> None:
        if not self.discovery_ok:
            raise DevError("Refresh the device list with l before selecting.", exit_code=2)
        if raw == "0":
            self.selected.clear()
            self.choices = {
                serial: {**choice, "selected": False}
                for serial, choice in self.choices.items()
            }
            self._save()
            self.show()
            return
        tokens = raw.replace(",", " ").split()
        if not tokens:
            raise DevError("Enter board numbers, f, l, t NUMBER, m, or q.", exit_code=2)
        # Validate every row first: a partial choice must never build or write.
        rows = [self._row(token) for token in tokens]
        targets = {}
        for candidate in rows:
            target = self._target(candidate)
            if target is None:
                target = self._ask_target(candidate)
            if target is None:
                return
            targets[candidate.identity] = target
        self.explicit_targets.update(targets)
        self.selected = set(targets)
        self._save()
        self.show()

    def edit_target(self, raw: str) -> None:
        if not self.discovery_ok:
            raise DevError("Refresh the device list with l before changing a target.", exit_code=2)
        candidate = self._row(raw)
        target = self._ask_target(candidate)
        if target is None:
            return
        self.explicit_targets[candidate.identity] = target
        self._save()
        self.show()

    def flash(self, build_target: hardware.BuildTarget, core_directory: hardware.CoreDirectory) -> None:
        if not self.discovery_ok:
            raise DevError("Refresh the device list with l before flashing.", exit_code=2)
        rows = [candidate for candidate in self.candidates if candidate.identity in self.selected]
        if not rows:
            raise DevError("No boards selected. Enter board numbers first; q exits.", exit_code=2)
        for candidate in rows:
            if candidate.usb_parent is None or self._target(candidate) is None:
                raise DevError("Selection is incomplete. Choose boards and their targets before flashing.", exit_code=2)
            self._row(str(self.candidates.index(candidate) + 1))
        print(f"Flashing {len(rows)} selected board(s) one at a time. Ctrl-C stops the batch.", flush=True)
        completed = 0
        try:
            for candidate in rows:
                target = self._target(candidate)
                command = ["./litch", "flash", target, "--device", candidate.identity]
                print(f"litch: {shlex.join(command)}", flush=True)
                hardware.flash_selected_target(
                    workspace=self.workspace, requested_target=target,
                    usb_selector=candidate.identity, ble_selector=None,
                    runner=self.runner, lock_timeout=self.lock_timeout,
                    build_target=build_target, platformio_core_dir=core_directory,
                    expected_usb_identity=candidate.usb_parent.identity,
                )
                completed += 1
        except (DevError, LockError, KeyboardInterrupt) as error:
            if completed == len(rows):
                print(f"All {len(rows)} selected board(s) flashed; selection retained.")
                raise
            self.selected = {row.identity for row in rows[completed:]}
            self._save()
            reason = "interrupted" if isinstance(error, KeyboardInterrupt) else str(error)
            print(f"Flash stopped at {rows[completed].identity}: {reason}", flush=True)
            print(f"{completed} completed; {len(rows) - completed - 1} remaining board(s) were not flashed.")
            print("Retry selection (unfinished boards):")
            self.show()
            if isinstance(error, KeyboardInterrupt):
                print("Retry selection remembered for the next start.")
                raise
            print("Use l to check connections, numbers to change selection, f to retry, or q to exit.")
            return
        print(f"All {len(rows)} selected board(s) flashed. q exits; f repeats the selection.")


def run_device_menu(
    workspace: Workspace, runner: Runner, *,
    build_target: hardware.BuildTarget, core_directory: hardware.CoreDirectory,
    other_commands: Callable[[], None],
) -> int:
    menu = UsbDeviceMenu(workspace, runner)
    menu.refresh()
    while True:
        print("Enter all board numbers to select (e.g. 1 3); 0 clears | f flash | l list/refresh | q exit")
        print("t NUMBER changes target | m build, app, tests, or monitor")
        raw = input("litch> ").strip().lower()
        if raw == "q":
            print("litch: exited")
            return 0
        try:
            if raw == "l":
                menu.refresh()
            elif raw == "f":
                menu.flash(build_target, core_directory)
            elif raw == "m":
                other_commands()
            elif raw.startswith("t "):
                menu.edit_target(raw[2:].strip())
            else:
                menu.select(raw)
        except DevError as error:
            print(f"litch: {error}")
