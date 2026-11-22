"""Observe the existing factory console on one exact S3 USB parent."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import math
import re
import sys
import time
from typing import Callable, Sequence, TypeVar

from .devices import (
    ESPRESSIF_VID,
    LITCH_APP_PID,
    DeviceInventoryError,
    PhysicalUsbIdentity,
    PioUsbInventory,
    capture_exact_usb_parent,
)
from .factory_status import FactoryStatus, FactoryStatusError, IncompleteFactoryStatus, parse_factory_status
from .locks import LockError, LockManager
from .runner import CompletedCommand, DevError, Runner
from .runtime_verify import (
    PySerialFactory,
    RuntimeVerificationError,
    SerialFactory,
    SerialTransport,
    close_serial_transport,
    parse_serial_firmware_version,
)
from .workspace import Workspace


_STATUS = "hwtest status"
_VERIFIER_REPLIES = {
    "hwtest verifier on": "hwtest: saving verifier=on; clean reboot into factory verifier essentials",
    "hwtest verifier off": "hwtest: saving verifier=off; clean reboot into normal play",
}
_READ_WINDOW_SECONDS = 0.05
_CHILD_CLEANUP_SECONDS = 0.25
_NOT_SENT_EXIT = 3
_MAX_REPLY_BYTES = 16 * 1024
_ANSI_CSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_BOOT_NONCE = re.compile(r"[0-9a-f]{16}")
_Reply = TypeVar("_Reply")


class FactorySerialError(DevError):
    """The current exact-board factory console exchange could not be verified."""


class FactoryCommandNotSent(FactorySerialError):
    """The exchange failed before attempting any verifier command bytes."""


@dataclass(frozen=True)
class _Runtime:
    version: str
    boot_nonce: str | None
    ota_accepted: bool | None


@dataclass(frozen=True)
class _Exchange:
    runtime: _Runtime
    response: str


@dataclass(frozen=True)
class _InventoryRunner:
    runner: Runner
    workspace: Workspace

    def run(self, argv: Sequence[str], *, timeout_seconds: float) -> CompletedCommand:
        return self.runner.run(argv, cwd=self.workspace.root, timeout_seconds=timeout_seconds,
                               capture=True, check=False, announce=False)


def _timeout_seconds(command: str) -> float:
    return 2.0 if command == _STATUS else 3.0


def _valid_boot_nonce(nonce: str | None) -> bool:
    return isinstance(nonce, str) and _BOOT_NONCE.fullmatch(nonce) is not None and nonce != "0" * 16


def _runtime(lines: list[str]) -> _Runtime | None:
    # Version always ends with this brief evidence row on S3, even when OTA
    # acceptance is still pending. The preceding rows belong to this request.
    terminal = next((index for index, line in enumerate(lines)
                     if line.startswith("ota_admission: available=")), None)
    if terminal is None:
        return None
    lines = lines[:terminal]
    version = parse_serial_firmware_version("\n".join(lines))
    nonces = [line.removeprefix("boot_nonce: ").lower() for line in lines if line.startswith("boot_nonce: ")]
    accepted_values = [line.removeprefix("ota_accepted: ") for line in lines if line.startswith("ota_accepted: ")]
    if len(nonces) > 1 or any(not _BOOT_NONCE.fullmatch(value) for value in nonces):
        raise FactorySerialError("serial boot nonce is ambiguous or malformed")
    if len(accepted_values) > 1 or any(value not in {"0", "1"} for value in accepted_values):
        raise FactorySerialError("serial OTA acceptance is ambiguous or malformed")
    nonce = nonces[0] if nonces and nonces[0] != "0" * 16 else None
    accepted = accepted_values[0] == "1" if accepted_values else None
    return _Runtime(version, nonce, accepted)


def _status_response(lines: list[str]) -> str | None:
    # Stop at the first complete terminal row. Let the factory parser verify
    # ordering and consistency across this exact response.
    for index, line in enumerate(lines):
        if line.startswith("hwtest: saved_record="):
            response = "\n".join(lines[:index + 1]) + "\n"
            try:
                parse_factory_status(response)
            except IncompleteFactoryStatus:
                continue
            return response
    return None


def _verifier_response(lines: list[str], command: str) -> str | None:
    expected = _VERIFIER_REPLIES[command]
    for line in lines:
        if line == expected:
            return line
        if line.startswith("hwtest: "):
            raise FactorySerialError(f"verifier request was not acknowledged: {line}")
    return None


def _query(
    transport: SerialTransport,
    command: str,
    deadline: float,
    parse: Callable[[list[str]], _Reply | None],
) -> _Reply:
    if deadline - time.monotonic() <= _READ_WINDOW_SECONDS:
        error_type = FactoryCommandNotSent if command in _VERIFIER_REPLIES else FactorySerialError
        raise error_type(f"serial deadline expired before sending {command}")
    request = (command + "\n").encode("ascii")
    if transport.write(request) != len(request):
        raise FactorySerialError(f"{command} was only partially written")
    response = bytearray()
    while time.monotonic() < deadline and len(response) < _MAX_REPLY_BYTES:
        response.extend(transport.read(min(512, _MAX_REPLY_BYTES - len(response))))
        text = _ANSI_CSI.sub("", response.decode("utf-8", errors="replace")).replace("\r", "\n")
        # The console redraws each typed prefix with CR. Only the complete
        # submitted line admits response rows, including after a partial read.
        lines = [line.strip() for line in text.split("\n")[:-1]]
        echo = "> " + command
        if echo not in lines:
            continue
        rows = [line.lstrip("> \t") for line in lines[lines.index(echo) + 1:]]
        for row in rows:
            if row.startswith(("unknown command", "Unknown command", "Invalid command:",
                               "factory mode:", "usage: hwtest", "Usage: version")):
                raise FactorySerialError(f"{command} was refused: {row}")
        if (result := parse(rows)) is not None:
            return result
    if len(response) >= _MAX_REPLY_BYTES:
        raise FactorySerialError(f"{command} exceeded the console response size limit")
    raise FactorySerialError(f"{command} returned no complete current response")


def _exchange(factory: SerialFactory, port: str, command: str,
              expected_version: str | None, timeout_seconds: float,
              *, expected_boot_nonce: str | None = None) -> _Exchange:
    if command != _STATUS and command not in _VERIFIER_REPLIES:
        raise FactorySerialError("unsupported factory console command")
    mutation = command in _VERIFIER_REPLIES
    transport = None
    try:
        try:
            if not math.isfinite(timeout_seconds) or not 0 < timeout_seconds < _timeout_seconds(command):
                raise FactorySerialError("invalid factory serial I/O deadline")
            if mutation and not _valid_boot_nonce(expected_boot_nonce):
                raise FactorySerialError("verifier command requires a previously observed boot nonce")
            deadline = time.monotonic() + timeout_seconds
            transport = factory.open(port, baudrate=115200, read_timeout_seconds=_READ_WINDOW_SECONDS,
                                     dtr=True, rts=True)
            transport.reset_input_buffer()
            runtime = _query(transport, "version", deadline, _runtime)
            if expected_version is not None and runtime.version != expected_version:
                raise FactorySerialError(f"firmware {runtime.version!r} does not match expected {expected_version!r}")
            if mutation and runtime.boot_nonce != expected_boot_nonce:
                raise FactorySerialError("board boot nonce changed or is unavailable; read factory status again")
        except (FactorySerialError, RuntimeVerificationError, OSError) as error:
            if mutation:
                raise FactoryCommandNotSent(str(error)) from error
            raise
        parse = _status_response if command == _STATUS else lambda rows: _verifier_response(rows, command)
        return _Exchange(runtime, _query(transport, command, deadline, parse))
    finally:
        if transport is not None:
            close_serial_transport(transport, primary_error=sys.exc_info()[1])


def _result(text: str) -> _Exchange:
    try:
        data = json.loads(text)
        if not isinstance(data, dict) or set(data) != {"runtime", "response"}:
            raise TypeError
        runtime = data["runtime"]
        if not isinstance(runtime, dict) or set(runtime) != {"version", "boot_nonce", "ota_accepted"}:
            raise TypeError
        version, nonce, accepted = runtime["version"], runtime["boot_nonce"], runtime["ota_accepted"]
        if not isinstance(version, str) or not isinstance(data["response"], str):
            raise TypeError
        if nonce is not None and (not isinstance(nonce, str) or not _BOOT_NONCE.fullmatch(nonce)):
            raise TypeError
        if accepted is not None and type(accepted) is not bool:
            raise TypeError
        return _Exchange(_Runtime(parse_serial_firmware_version(f"firmware: {version}"), nonce, accepted),
                         data["response"])
    except (ValueError, TypeError, RuntimeVerificationError) as error:
        raise FactorySerialError("factory serial child returned an invalid result") from error


class FactorySerial:
    def __init__(self, workspace: Workspace, runner: Runner, identity: PhysicalUsbIdentity,
                 lock_timeout: float, *, expected_version: str | None = None) -> None:
        self._workspace = workspace
        self._runner = runner
        self._identity = identity
        self._locks = LockManager(workspace)
        self._lock_timeout = lock_timeout
        self._expected_version = expected_version
        self._runtime: _Runtime | None = None

    @property
    def firmware_version(self) -> str | None:
        return self._runtime.version if self._runtime is not None else None

    @property
    def boot_nonce(self) -> str | None:
        return self._runtime.boot_nonce if self._runtime is not None else None

    @property
    def ota_accepted(self) -> bool | None:
        return self._runtime.ota_accepted if self._runtime is not None else None

    def poll_status(self) -> FactoryStatus:
        exchange = self._run(_STATUS)
        try:
            status = parse_factory_status(exchange.response)
        except FactoryStatusError as error:
            raise FactorySerialError(str(error)) from error
        self._runtime = exchange.runtime
        return status

    def command(self, text: str, *, expected_boot_nonce: str) -> str:
        if text not in _VERIFIER_REPLIES:
            raise FactoryCommandNotSent("unsupported factory console command")
        if not _valid_boot_nonce(expected_boot_nonce):
            raise FactoryCommandNotSent("verifier command requires a previously observed boot nonce")
        exchange = self._run(text, expected_boot_nonce=expected_boot_nonce)
        if exchange.response != _VERIFIER_REPLIES[text]:
            raise FactorySerialError("factory serial child returned an invalid verifier acknowledgement")
        self._runtime = exchange.runtime
        return exchange.response

    def _run(self, command: str, *, expected_boot_nonce: str | None = None) -> _Exchange:
        mutation = command in _VERIFIER_REPLIES
        started_child = False
        try:
            with self._locks.device(self._identity.serial, timeout=self._lock_timeout):
                deadline = time.monotonic() + _timeout_seconds(command)
                inventory = PioUsbInventory(_InventoryRunner(self._runner, self._workspace),
                                            timeout_seconds=1.25)
                parent = capture_exact_usb_parent(inventory, identity=self._identity,
                                                  vid=ESPRESSIF_VID, pid=LITCH_APP_PID)
                remaining = deadline - time.monotonic()
                io_timeout = remaining - _CHILD_CLEANUP_SECONDS
                if io_timeout <= _READ_WINDOW_SECONDS:
                    raise FactorySerialError("serial deadline expired during exact USB discovery")
                argv = (sys.executable, "-m", "tools.dev.factory_serial", "--port", parent.port,
                        "--request", command, "--io-timeout", str(io_timeout))
                if self._expected_version is not None:
                    argv += ("--expected-version", self._expected_version)
                if expected_boot_nonce is not None:
                    argv += ("--expected-boot-nonce", expected_boot_nonce)
                started_child = True
                completed = self._runner.run(argv, cwd=self._workspace.root, timeout_seconds=remaining,
                                             capture=True, check=False, announce=False)
        except (DeviceInventoryError, LockError, DevError, OSError) as error:
            error_type = FactoryCommandNotSent if mutation and not started_child else FactorySerialError
            raise error_type(f"{command} on USB {self._identity.serial}: {error}") from error
        if mutation and completed.returncode == _NOT_SENT_EXIT and not completed.stderr.strip():
            try:
                failure = json.loads(completed.stdout)
            except ValueError:
                failure = None
            if (isinstance(failure, dict) and set(failure) == {"mutation", "error"}
                    and failure["mutation"] == "not_sent" and isinstance(failure["error"], str)):
                raise FactoryCommandNotSent(f"{command} on USB {self._identity.serial}: {failure['error'][:1000]}")
        if completed.returncode != 0 or completed.stderr.strip():
            detail = completed.stderr.strip()[:1000] or "serial child failed without a diagnostic"
            raise FactorySerialError(f"{command} on USB {self._identity.serial}: {detail}")
        exchange = _result(completed.stdout)
        if self._expected_version is not None and exchange.runtime.version != self._expected_version:
            raise FactorySerialError("factory serial child returned an unexpected firmware version")
        if expected_boot_nonce is not None and exchange.runtime.boot_nonce != expected_boot_nonce:
            raise FactorySerialError("factory serial child returned an unexpected boot nonce")
        return exchange


def _serial_main() -> int:
    parser = argparse.ArgumentParser(description="Private supervised factory serial exchange")
    parser.add_argument("--port", required=True)
    parser.add_argument("--request", choices=(_STATUS, *_VERIFIER_REPLIES), required=True)
    parser.add_argument("--expected-version")
    parser.add_argument("--expected-boot-nonce")
    parser.add_argument("--io-timeout", type=float, required=True)
    args = parser.parse_args()
    try:
        exchange = _exchange(PySerialFactory(), args.port, args.request, args.expected_version, args.io_timeout,
                             expected_boot_nonce=args.expected_boot_nonce)
    except FactoryCommandNotSent as error:
        print(json.dumps({"mutation": "not_sent", "error": str(error)}, separators=(",", ":")))
        return _NOT_SENT_EXIT
    except (FactorySerialError, FactoryStatusError, RuntimeVerificationError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(asdict(exchange), separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(_serial_main())
