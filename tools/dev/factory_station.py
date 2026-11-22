"""Bind each scanned PCB to one exact USB factory qualification attempt."""

from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass
import os
from pathlib import Path
import shlex
import sys
import time

from .batch_flash import _flash_file_hashes
from .batch_input import BatchInput
from .build_paths import platformio_build_directory
from .devices import LITCH_APP_PID, PhysicalUsbIdentity
from .factory_camera import CameraChoice, FactoryCamera
from .factory_records import FactoryRecords, FactoryRecordsError, _board_number
from .factory_serial import FactoryCommandNotSent, FactorySerial, FactorySerialError
from .factory_status import FactoryStatus
from .hardware import (
    FIRMWARE_VERSION_HEADER,
    TARGET_ENVIRONMENTS,
    BuildTarget,
    CoreDirectory,
    discover_possible_usb_boards,
    flash_exact_device,
)
from .locks import LockError, LockManager, LockTimeoutError
from .rom_flash import RomFlashError, read_firmware_header_version
from .runner import DevError, Runner
from .workspace import Workspace


_POLL_SECONDS = 0.25
_STATUS_COLORS = {"NEXT": 36, "PASS": 32, "WAIT": 33, "ERROR": 31}


def _say(level: str, subject: str, message: str, *, banner: bool = False) -> None:
    tag = level
    if sys.stdout.isatty() and "NO_COLOR" not in os.environ:
        tag = f"\033[1;{_STATUS_COLORS[level]}m{level}\033[0m"
    line = f"{tag} [{subject}] {message}"
    if banner:
        line = f"\n{'=' * 64}\n{line}\n{'=' * 64}\n"
    print(line, flush=True)


@dataclass(frozen=True)
class FactoryFirmware:
    build_dir: Path
    hashes: tuple[str, ...]
    version: str
    source_commit: str

    def verify(self, _target: str = "s3") -> None:
        if _flash_file_hashes(self.build_dir) != self.hashes:
            raise DevError("S3 flash files changed after the station build. Stop and start a new batch invocation.")


def build_factory_firmware(*, workspace: Workspace, lock_timeout: float,
                           build_target: BuildTarget) -> FactoryFirmware:
    """Finish and identify one build before any operator prompts or camera use."""
    build_dir = platformio_build_directory(workspace.root, TARGET_ENVIRONMENTS["s3"])
    _say("WAIT", "factory", "Building and validating S3 firmware before batch setup.")
    with LockManager(workspace, argv=sys.argv).lane("platformio", timeout=lock_timeout):
        build_target("s3")
        try:
            version = read_firmware_header_version(workspace.root / FIRMWARE_VERSION_HEADER)
        except RomFlashError as error:
            raise DevError(str(error)) from error
        return FactoryFirmware(build_dir, _flash_file_hashes(build_dir), version, workspace.head)


@dataclass
class _Board:
    label: str
    identity: PhysicalUsbIdentity
    attempt_id: str
    serial: FactorySerial
    result: str = "pending"
    reboot_from: str | None = None
    can_unplug: bool = False
    last_message: str = ""

    def announce(self, batch: str, message: str, *, level: str = "WAIT", banner: bool = False) -> None:
        if message == self.last_message:
            return
        self.last_message = message
        _say(level, f"{batch} | {self.label}", message, banner=banner)


def _record_status(
    board: _Board, records: FactoryRecords, status: FactoryStatus, *, first: bool, needs_initial_peer: bool,
) -> None:
    if board.result != "pending":
        return
    if status.durable_pass or status.override:
        result = "pass" if status.durable_pass else "override"
        records.update(board.attempt_id, phase=status.step, result=result,
                       failure_text="", status=status, notes="")
        board.result = result
        if result == "pass":
            message = "PASS saved."
            # The saved radio duration is cumulative from test start. Subtract
            # the automated and operator phases to get the post-local interval.
            auto_ms, operator_ms, radio_ms = (int(status.observations[key]) for key in
                                               ("auto_ms", "operator_ms", "radio_ms"))
            wireless_ms = radio_ms - auto_ms - operator_ms
            if wireless_ms >= 0:
                message += f" Wireless wait + test {wireless_ms / 1000:.3f} s."
            if first:
                message += " Keep this board connected as the verifier for the whole batch."
            else:
                message += " Leave connected until the station confirms normal mode and says you can unplug."
        else:
            message = "OVERRIDE saved. Board did not qualify."
        board.announce(records.batch_name, message, level="PASS" if result == "pass" else "ERROR", banner=True)
        return

    faults = " | ".join(status.current_failures)
    waiting_for_peer = status.stage in {"radio", "local_complete_waiting_for_peer"}
    records.update(board.attempt_id, phase=status.step, failure_text=faults, status=status, notes="")
    if faults:
        board.announce(records.batch_name, f"FACTORY FAULT: {faults}", level="ERROR", banner=True)
    elif status.save_pending or status.override_pending:
        board.announce(records.batch_name, "Waiting for the board to save its result.")
    elif waiting_for_peer:
        if status.observations.get("peer_candidate") == "1":
            message = "Wireless peer found. Verifying the link. Leave this board connected."
        elif needs_initial_peer:
            message = "Waiting for a wireless peer. Keep this board connected and prepare a second board."
        else:
            message = "Waiting for a wireless peer. Keep the boards connected."
        board.announce(records.batch_name, message)
    elif status.step == "physical_buttons" and status.observations.get("manual_ambient_response") == "required":
        # Console sensor numbers follow driver order, opposite the physical
        # sensor names. Give the operator the fixed board corners instead.
        pending = [corner for check, corner in (("light_2", "A1"), ("light_1", "H8"))
                   if status.checks.get(check) != "pass"]
        if (status.observations.get("current_hall_read") == "valid"
                and status.observations.get("current_hall_clear") == "0"):
            message = "Remove the magnet or piece from the board so the test can continue."
        elif pending:
            message = ("Release the buttons and hold the light steady briefly. "
                       f"Then cover or uncover {' and '.join(pending)} and hold until the inner lights turn green "
                       "or the wireless test's amber center square appears. Test any remaining buttons.")
        else:
            message = "Both light sensors passed. Test and release any remaining buttons."
        board.announce(records.batch_name, message, level="NEXT")
    else:
        board.announce(records.batch_name, f"Factory test: {status.step.replace('_', ' ')}.", level="NEXT")


def _confirm_service_mode(
    board: _Board, batch: str, status: FactoryStatus, keys: BatchInput, *,
    first: bool, verifier_ready: bool,
) -> bool:
    """Return whether the designated first board is currently serving as verifier."""
    if board.result != "pass" or not status.durable_pass or status.active:
        return False
    saved_flag = status.observations.get("verifier")
    nonce = board.serial.boot_nonce
    if board.reboot_from is not None and (nonce is None or nonce == board.reboot_from):
        board.announce(batch, "Waiting for a fresh boot to confirm verifier mode.")
        return False

    saved_verifier = (status.stage, status.step, status.radio_role, saved_flag) == (
        "passed", "pass_saved", "VERIFIER", "1",
    )
    # Cold boot deliberately stays normal even if verifier=on was saved. It
    # needs the same explicit service-entry command as a flag-off normal boot.
    normal_mode = status.stage == "inactive"
    if first and saved_verifier:
        board.announce(batch, "Saved verifier ready. Keep this board connected for the whole batch.", level="NEXT")
        return True
    if not first and normal_mode and saved_flag == "0":
        board.can_unplug = True
        board.announce(batch, "PASS confirmed in normal mode. You can unplug this board.", level="NEXT")
        return False

    # Wait for the first service board before retiring an elected peer, so the
    # next DUT never loses its verified responder during the handoff.
    # Normal boot runs OTA acceptance. The saved verifier's minimal runtime
    # can report it as false throughout service and still exit to normal boot.
    needs_on = first and normal_mode and board.serial.ota_accepted is True
    needs_off = not first and verifier_ready and saved_verifier
    if not first and saved_verifier and not verifier_ready:
        board.announce(batch, "Leave connected until the first board is ready as saved verifier.")
    if board.reboot_from is not None:
        board.announce(batch, "Reboot did not confirm the requested verifier mode. Check this board before unplugging.",
                       level="ERROR", banner=True)
    elif (needs_on or needs_off) and nonce and not keys.quit_requested():
        # Even an acknowledgement timeout can mean the write reached firmware.
        # Reconcile only through a fresh boot and never submit the command twice.
        board.reboot_from = nonce
        command = "hwtest verifier on" if needs_on else "hwtest verifier off"
        try:
            board.serial.command(command, expected_boot_nonce=nonce)
        except FactoryCommandNotSent as error:
            board.reboot_from = None
            board.announce(batch, f"Verifier request did not start. Will recheck current boot. {error}")
        except FactorySerialError as error:
            board.announce(batch, f"Verifier request unconfirmed. Waiting for boot readback. {error}",
                           level="ERROR", banner=True)
        else:
            board.reboot_from = board.serial.boot_nonce or nonce
            board.announce(batch, "Verifier request accepted. Waiting for reboot and saved-mode readback.")
    return False


def run_factory_station(
    *,
    workspace: Workspace,
    runner: Runner,
    lock_timeout: float,
    firmware: FactoryFirmware,
    platformio_core_dir: CoreDirectory,
    batch_name: str,
    records_path: Path,
    camera_choice: CameraChoice,
    verbose: bool = False,
    resume: bool = False,
) -> None:
    """Bind new scans or confirmed recorded labels to exact USB boards before erasing."""
    locks = LockManager(workspace, argv=sys.argv)
    with locks.lane("platformio", timeout=lock_timeout):
        firmware.verify()
    with ExitStack() as resources:
        records = resources.enter_context(FactoryRecords(
            records_path, locks, batch_name=batch_name, source_commit=firmware.source_commit,
        ))
        resumed = records.resume() if resume else None
        completed = {row["usb_serial"]: row for row in resumed.passed} if resumed else {}
        batch_args = ["--batch-name", batch_name, "--records", str(records.path)]
        resume_command = shlex.join(["./litch", "flash", "s3", "--batch", "--factory", "--resume", *batch_args])
        keys = resources.enter_context(BatchInput())
        _say("WAIT", batch_name, f"Factory batch | Camera: {camera_choice.label} | CSV {records.path}")
        if resumed:
            _say("NEXT", batch_name, f"Resuming with verifier {resumed.verifier['qr_id']} "
                 f"(USB {resumed.verifier['usb_serial']}). Keep it connected. "
                 "Other recorded PASS boards require Y before erasing and testing again.")
        _say("NEXT", batch_name, "Connect one board and scan its PCB label in either order. "
             "The station erases the board after both are ready. Press q to stop after any flash in progress.")
        if keys.quit_requested():
            return
        camera = resources.enter_context(FactoryCamera(choice=camera_choice))

        boards: dict[str, _Board] = {}
        used_labels: set[str] = {row["qr_id"] for row in completed.values()}
        first: _Board | None = None
        armed: str | None = None
        scan_reset_reason = ""
        must_unplug = False
        confirmation_identity: PhysicalUsbIdentity | None = None
        confirmation_row: dict[str, str] | None = None
        confirmation_prompt = ""
        scan_needs_clear = False
        last_wait = ""
        try:
            while not keys.quit_requested():
                try:
                    candidates = tuple(candidate for candidate in
                                       discover_possible_usb_boards(workspace, runner, min(lock_timeout, _POLL_SECONDS))
                                       if candidate.target_hint in (None, "s3"))
                except LockTimeoutError:
                    wait = "Waiting for PlatformIO to finish its current operation before checking USB."
                    if wait != last_wait:
                        _say("WAIT", batch_name, wait)
                        last_wait = wait
                    time.sleep(_POLL_SECONDS)
                    continue
                for serial, board in tuple(boards.items()):
                    if (board is not first and board.can_unplug and not any(
                            candidate.usb_parent is not None and candidate.usb_parent.identity == board.identity
                            for candidate in candidates)):
                        # Only a new attachment may offer a completed board another
                        # erase; remaining connected after PASS never does so.
                        del boards[serial]
                if resumed and first is None:
                    saved = resumed.verifier
                    matches = [candidate.usb_parent for candidate in candidates
                               if candidate.identity == saved["usb_serial"]]
                    parent = matches[0] if len(matches) == 1 else None
                    if parent is not None and parent.pid == LITCH_APP_PID:
                        # The serial identifies the recorded board. Its current
                        # USB location is bound afresh for this resumed invocation.
                        first = _Board(saved["qr_id"], parent.identity, saved["attempt_id"], FactorySerial(
                            workspace, runner, parent.identity, min(lock_timeout, _POLL_SECONDS),
                            expected_version=saved["firmware_version"],
                        ), result="pass")
                        boards[parent.identity.serial] = first
                        first.announce(batch_name, "Checking saved PASS and verifier service. No flash is needed.")
                protected_serial = first.identity.serial if first is not None else (
                    resumed.verifier["usb_serial"] if resumed else None)
                unclaimed = [candidate for candidate in candidates
                             if candidate.identity not in boards and candidate.identity != protected_serial]
                first_connected = first is None and resumed is None
                observed: list[tuple[_Board, FactoryStatus]] = []
                for board in boards.values():
                    if keys.quit_requested():
                        break
                    matches = [candidate.usb_parent for candidate in candidates
                               if candidate.identity == board.identity.serial]
                    parent = matches[0] if len(matches) == 1 else None
                    attached = parent is not None and parent.identity == board.identity
                    if board is first:
                        first_connected = attached
                    if board.can_unplug or board.result == "override":
                        continue
                    if not attached or parent.pid != LITCH_APP_PID:
                        message = ("Waiting for application USB after write or reboot." if attached
                                   else f"USB disconnected or changed. Reconnect at {board.identity.location}.")
                        if board.result == "pass" and board is not first:
                            message = ("Waiting for USB to return after PASS. Leave this board connected. "
                                       f"If you removed it, reconnect it at {board.identity.location}.")
                        if board.result == "pending":
                            records.update(board.attempt_id, notes=message)
                        board.announce(batch_name, message)
                        continue
                    try:
                        status = board.serial.poll_status()
                    except FactorySerialError as error:
                        message = f"Awaiting verified factory status. {error}"
                        if board.result == "pending":
                            records.update(board.attempt_id, notes=message)
                        board.announce(batch_name, message)
                        continue
                    _record_status(board, records, status, first=board is first,
                                   needs_initial_peer=board is first and len(candidates) == 1)
                    if resumed and board is first and (not status.durable_pass or status.active):
                        board.announce(batch_name, "Resume is waiting for this board's saved PASS in normal or verifier mode. "
                                       "Release its controls and reboot it; do not factory-flash it.", level="ERROR")
                    observed.append((board, status))

                if first is not None and first.result == "override":
                    raise DevError(f"First board {first.label} saved Override. This batch has no designated verifier.")
                verifier_ready = False
                for board, status in observed:
                    if keys.quit_requested():
                        break
                    ready = _confirm_service_mode(
                        board, batch_name, status, keys, first=board is first, verifier_ready=verifier_ready,
                    )
                    if board is first:
                        verifier_ready = ready

                if keys.quit_requested():
                    break
                if not unclaimed:
                    must_unplug = False
                if len(unclaimed) > 1 and not must_unplug:
                    armed = None
                    scan_reset_reason = "Multiple unclaimed USB boards were connected. Scan cancelled."
                    must_unplug = True
                candidate = unclaimed[0] if len(unclaimed) == 1 else None
                parent = candidate.usb_parent if candidate is not None else None
                update = camera.poll()
                # The reader discards pre-gap frames. Drop the station's matching
                # scan and any offered erase, even if frames have already resumed.
                frame_gap = any(reading.state == "stalled" for reading in update.transitions)
                if frame_gap:
                    armed = None
                    scan_needs_clear = True
                admission_ready = not update.stalled and first_connected and (resumed is None or verifier_ready)
                history = records.lookup(usbserial=parent.identity.serial) if parent is not None else ()
                recorded_pass = history[-1] if history and history[-1]["result"] == "pass" else None
                rebatch_ready = admission_ready and (first is None or verifier_ready) and not must_unplug
                cancelled_confirmation = False
                if confirmation_identity is not None and (
                        frame_gap or parent is None or parent.identity != confirmation_identity or not rebatch_ready
                        or recorded_pass is None or confirmation_row is None
                        or recorded_pass["attempt_id"] != confirmation_row["attempt_id"]):
                    keys.end_confirmation()
                    confirmation_identity = None
                    confirmation_row = None
                    cancelled_confirmation = True
                    scan_needs_clear = True
                    _say("WAIT", batch_name, "Re-batch confirmation cancelled. No board was erased.")
                    last_wait = ""

                admitted_label: str | None = None
                rebatching = False
                scan_error = ""
                current = update.current
                scan_transitions = update.transitions
                if scan_needs_clear and not cancelled_confirmation:
                    clear_index = next((index for index, reading in enumerate(scan_transitions)
                                        if reading.state == "clear"), None)
                    if clear_index is not None:
                        # Keep the post-clear history: another label can already
                        # be visible after a slow serial exchange or flash.
                        scan_transitions = scan_transitions[clear_index:]
                        scan_needs_clear = False
                        scan_reset_reason = ""
                    elif current is not None and current.state == "clear":
                        scan_transitions = ()
                        scan_needs_clear = False
                        scan_reset_reason = ""
                if recorded_pass is not None:
                    # The persisted one-to-one binding supplies this label, not
                    # the camera. Discard camera input until a later clear frame.
                    # A label shown while this recorded board owns admission
                    # cannot become the next board's scan after Y. Only a
                    # currently clear preview can retire that quarantine.
                    scan_needs_clear = current is None or current.state != "clear"
                    armed = None
                    if not must_unplug:
                        scan_reset_reason = ""
                    if rebatch_ready and not cancelled_confirmation:
                        if confirmation_identity is None:
                            scan_needs_clear = True
                            confirmation_identity = parent.identity
                            confirmation_row = recorded_pass
                            confirmation_prompt = (
                                f"Recorded PASS: {recorded_pass['qr_id']} (USB {parent.identity.serial} "
                                f"at {parent.identity.location}, batch {recorded_pass['batch_name']}). "
                                f"Press Y to erase settings, games and artwork, flash and retest in {batch_name}. "
                                "Unplug to cancel; q stops the station."
                            )
                            _say("NEXT", batch_name, confirmation_prompt)
                            last_wait = confirmation_prompt
                            keys.begin_confirmation()
                        elif keys.confirmation_requested():
                            assert confirmation_row is not None
                            admitted_label = confirmation_row["qr_id"]
                            rebatching = True
                            keys.end_confirmation()
                            confirmation_identity = None
                            confirmation_row = None
                elif scan_needs_clear:
                    armed = None
                else:
                    seen_label = armed
                    for reading in scan_transitions:
                        if reading.state == "clear":
                            if armed is None:
                                seen_label = None
                            scan_reset_reason = ""
                        elif reading.state in ("ambiguous", "reset"):
                            armed = None
                            seen_label = None
                            scan_reset_reason = ("Camera scan history filled while the station was busy. Scan cancelled."
                                                 if reading.state == "reset" else "Multiple labels in view. Scan cancelled.")
                            must_unplug |= bool(unclaimed)
                        elif reading.state == "label":
                            label = reading.payload
                            assert label is not None
                            if seen_label is not None and label != seen_label:
                                scan_reset_reason = f"Scan changed from {seen_label} to {label}. Previous scan cancelled."
                                armed = None
                                seen_label = None
                                must_unplug |= bool(unclaimed)
                            elif not scan_reset_reason and label not in used_labels:
                                seen_label = label

                    # A stable code remains usable after a temporary USB/verifier
                    # wait. Historical transitions invalidate conflicts, but a
                    # stale queued label alone cannot authorize an erase.
                    if current is not None and current.state == "label":
                        label = current.payload
                        assert label is not None
                        if armed is not None and label != armed:
                            scan_reset_reason = f"Scan changed from {armed} to {label}. Previous scan cancelled."
                            armed = None
                            must_unplug |= bool(unclaimed)
                        elif (armed is None and not scan_reset_reason and not must_unplug
                              and len(unclaimed) <= 1 and admission_ready):
                            if label in used_labels:
                                attached_pass = not unclaimed and any(
                                    board.label == label and board.result == "pass" and any(
                                        candidate.usb_parent is not None
                                        and candidate.usb_parent.identity == board.identity
                                        for candidate in candidates)
                                    for board in boards.values()
                                )
                                # Leaving a finished board under the camera is
                                # normal. A different USB target still makes this
                                # old label a binding conflict, never an admission.
                                if not attached_pass:
                                    scan_error = f"{label} is already recorded in this run or resumed batch. Scan a new board."
                            else:
                                try:
                                    _board_number(label)
                                except FactoryRecordsError as error:
                                    scan_error = f"{error}. Scan the PCB label."
                                else:
                                    armed = label

                if len(unclaimed) > 1 or (unclaimed and scan_reset_reason):
                    must_unplug = True
                if (armed is not None and not must_unplug and admission_ready and parent is not None
                        and current is not None and current.state in ("clear", "label")):
                    admitted_label = armed
                if admitted_label is not None and parent is not None:
                    identity = parent.identity
                    if keys.quit_requested():
                        break
                    try:
                        attempt_id = records.begin(admitted_label, identity.serial, identity.location,
                                                   firmware.hashes[0], firmware.version,
                                                   station_role="verifier" if first is None else "dut")
                    except FactoryRecordsError as error:
                        if not rebatching:
                            raise
                        scan_reset_reason = f"Cannot re-batch {admitted_label}: {error}."
                        must_unplug = True
                        _say("ERROR", batch_name, f"{scan_reset_reason} Unplug this board to cancel.", banner=True)
                        time.sleep(_POLL_SECONDS)
                        continue
                    board = _Board(admitted_label, identity, attempt_id, FactorySerial(
                        workspace, runner, identity, min(lock_timeout, _POLL_SECONDS),
                        expected_version=firmware.version,
                    ))
                    boards[identity.serial] = board
                    used_labels.add(admitted_label)
                    if first is None:
                        first = board
                    armed = None
                    board.announce(batch_name, f"Flashing USB serial {identity.serial} at {identity.location}.")
                    records.update(attempt_id, phase="flashing")
                    if keys.quit_requested():
                        break
                    try:
                        # Only a successful full erase/write admits this exact
                        # identity to status polling on the next iteration.
                        flash_exact_device(
                            workspace=workspace, target="s3", usb_identity=identity.serial, ble_identity=None,
                            runner=runner, lock_timeout=lock_timeout, build_target=firmware.verify,
                            platformio_core_dir=platformio_core_dir, verbose=verbose, factory_provision=True,
                            expected_usb_identity=identity,
                        )
                    except (DevError, LockError) as error:
                        records.update(attempt_id, phase="flash-failed", result="fail", failure_text=str(error))
                        board.result = "fail"
                        board.announce(batch_name, f"FLASH FAILED: {error}", level="ERROR", banner=True)
                        if first is not board and first.result == "pass":
                            raise DevError(
                                f"Factory batch stopped after a flash failure. Keep verifier {first.label} connected.\n"
                                f"Restart the recorded batch with:\n{resume_command}\n"
                                f"Rescan {board.label} for a new attempt; earlier records are preserved."
                            ) from error
                        raise
                    records.update(attempt_id, phase="waiting-for-factory-boot")
                    board.announce(batch_name, "Firmware written. Leave connected and complete the board's tests.",
                                   level="NEXT")
                    last_wait = ""
                    continue

                level = "NEXT"
                if update.stalled:
                    level = "WAIT"
                    wait = ("Camera frames paused. Board checks continue. When the preview moves again, "
                            "clear it before scanning a new board.")
                    if resumed or (first is not None and first.result == "pass"):
                        wait += (" If it stays frozen, finish connected boards, keep the verifier connected, "
                                 f"then press q and resume with:\n{resume_command}")
                    else:
                        wait += " If it stays frozen, press q to stop. Attempts without a saved result will be incomplete."
                elif scan_reset_reason:
                    unplug = " Unplug all unclaimed USB boards." if must_unplug else ""
                    wait = f"{scan_reset_reason}{unplug} Clear the preview, then scan one PCB again."
                    level = "ERROR"
                elif resumed and first is None:
                    wait = (f"Connect saved verifier {resumed.verifier['qr_id']} "
                            f"(USB {resumed.verifier['usb_serial']}) in normal/verifier mode. It will not be flashed.")
                elif not first_connected:
                    wait = f"Reconnect first board {first.label} to its original USB location before admitting another."
                elif resumed and not verifier_ready:
                    wait = f"Waiting for saved verifier {first.label} to be ready before flashing another board."
                    level = "WAIT"
                elif must_unplug:
                    wait = "Unplug all unclaimed USB boards. Clear the preview, then scan one PCB again."
                    level = "ERROR"
                elif scan_error:
                    wait = scan_error
                    level = "ERROR"
                elif confirmation_identity is not None:
                    wait = confirmation_prompt
                elif recorded_pass is not None and not rebatch_ready:
                    wait = "Waiting for the designated verifier before offering this recorded board another test."
                    level = "WAIT"
                elif scan_needs_clear:
                    wait = "Clear the camera preview before scanning the next PCB label."
                elif parent is not None:
                    wait = f"USB board {parent.identity.serial} connected. Scan this board's PCB label."
                elif unclaimed:
                    wait = "Waiting for the new USB board to report its exact serial and location."
                    level = "WAIT"
                elif armed is not None:
                    wait = f"Scanned {armed}. Plug in that board by USB."
                else:
                    wait = "Scan the next PCB label or connect the next board by USB."
                if wait != last_wait:
                    _say(level, batch_name, wait)
                    last_wait = wait
                time.sleep(_POLL_SECONDS)
        finally:
            if confirmation_identity is not None:
                keys.end_confirmation()
            error = sys.exc_info()[1]
            for board in boards.values():
                if board.result == "pending":
                    message = "Station stopped before a saved result was verified."
                    if error is not None and not isinstance(error, KeyboardInterrupt):
                        message += f" {error}"
                    try:
                        records.update(board.attempt_id, result="incomplete", notes=message)
                    except FactoryRecordsError as record_error:
                        if error is None:
                            raise
                        print(f"ERROR [{batch_name} | {board.label}] Incomplete result could not be recorded: {record_error}",
                              file=sys.stderr, flush=True)
                    else:
                        board.announce(batch_name, f"INCOMPLETE. {message}")
