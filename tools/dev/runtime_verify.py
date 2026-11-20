"""Post-ROM-reset verification of the exact running Litch firmware."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import re
import shlex
import sys
import time
from dataclasses import dataclass
from typing import Any, Callable, Coroutine, Mapping, Protocol, TypeVar



BASE_SERVICE_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0100"
OTA_CONTROL_CHARACTERISTIC_UUID = "6f2a20d1-89df-4ac6-a9f2-53f6f7db0104"
CLIENT_CLEANUP_TIMEOUT_SECONDS = 2.0
RUNNING_VERIFICATION_TIMEOUT_SECONDS = 45.0
SERIAL_PROBE_HARD_TIMEOUT_SECONDS = 18.0
SERIAL_PROBE_SCHEMA = "litch.serial-version.v1"
SERIAL_MAX_UNANSWERED_VERSION_REQUESTS = 2
_VERSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$")
_DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
_DIGEST_FRAGMENT_RE = re.compile(r"^[0-9a-f]{8}$")
_SERIAL_VERSION_LINE_RE = re.compile(
    r"^\s*firmware:\s*(?P<version>\S+)\s*$", re.MULTILINE
)
_SERIAL_DIGEST_LINE_RE = re.compile(
    r"^\s*image_sha256:\s*(?P<digest>[0-9a-f]{64}|pending)\s*$", re.MULTILINE
)
_SERIAL_ACCEPTED_LINE_RE = re.compile(
    r"^\s*ota_accepted:\s*(?P<accepted>[01])\s*$", re.MULTILINE
)
_SUPPORT_INCIDENT_FIELDS = (
    "rr", "boot", "wd", "up", "prior", "prior_up", "hc", "se", "be",
    "flags", "prior_hc", "prior_se", "prior_flags", "stages", "stage_ms",
)


class RuntimeVerificationError(RuntimeError):
    """Raised when the running image cannot be proven to match the build."""


def _add_exception_note(error: BaseException, note: str) -> None:
    """Preserve exception cleanup notes on Python 3.9 through 3.11+."""

    add_note = getattr(error, "add_note", None)
    if callable(add_note):
        add_note(note)
        return
    notes = getattr(error, "__notes__", None)
    if notes is None:
        notes = []
        setattr(error, "__notes__", notes)
    notes.append(note)


class SerialTransport(Protocol):
    def set_control_lines(self, *, dtr: bool, rts: bool) -> None: ...

    def reset_input_buffer(self) -> None: ...

    def write(self, data: bytes) -> int: ...

    def read(self, size: int) -> bytes: ...

    def close(self) -> None: ...


class SerialFactory(Protocol):
    def open(
        self,
        port: str,
        *,
        baudrate: int,
        read_timeout_seconds: float,
        dtr: bool,
        rts: bool,
    ) -> SerialTransport: ...


def close_serial_transport(
    transport: SerialTransport,
    *,
    primary_error: BaseException | None = None,
) -> None:
    """Release ordinary CDC sessions safely, retaining an active failure."""

    try:
        try:
            # Arduino recognizes01->11->10->00 as a ROM request. Release
            # DTR first through the adapter before OS close can choose the
            # opposite order and complete a stale handshake.
            transport.set_control_lines(dtr=False, rts=False)
        finally:
            transport.close()
    except Exception as error:
        message = f"serial session teardown failed: {error}"
        if primary_error is not None:
            _add_exception_note(primary_error, message)
        else:
            raise RuntimeVerificationError(message) from error


def _read_failure_incident(
    transport: SerialTransport,
    *,
    monotonic: Callable[[], float],
    sleep: Callable[[float], None],
) -> str:
    """Capture the first post-trial incident before another reset replaces it."""

    try:
        if transport.write(b"support\n") != len(b"support\n"):
            return "unavailable:short_write"
        deadline = monotonic() + 1.5
        # The existing USB TX queue can contain 6 KiB of boot output before
        # this solicited 1 KiB response. Bound the scan beyond both without
        # discarding first-boot evidence from the same serial transport.
        response = bytearray()
        while monotonic() < deadline and len(response) < 8192:
            chunk = transport.read(min(1024, 8192 - len(response)))
            if chunk:
                response.extend(chunk)
                lines = response.decode("utf-8", errors="replace").replace(
                    "\r", "\n"
                ).splitlines()
                for line in lines:
                    start = line.find('{"event":"support_incident"')
                    if start < 0:
                        continue
                    try:
                        incident = json.loads(line[start:])
                    except json.JSONDecodeError:
                        continue
                    if not isinstance(incident, dict) or incident.get("v") != 1:
                        continue
                    bounded = {
                        key: incident[key]
                        for key in _SUPPORT_INCIDENT_FIELDS
                        if key in incident
                    }
                    return json.dumps(bounded, separators=(",", ":"))
            else:
                sleep(min(0.02, max(0.0, deadline - monotonic())))
    except Exception as error:
        return f"unavailable:{type(error).__name__}"
    return "unavailable:no_complete_incident"


class _PySerialTransport:
    def __init__(self, serial_port: object) -> None:
        self._serial = serial_port

    def set_control_lines(self, *, dtr: bool, rts: bool) -> None:
        setattr(self._serial, "dtr", dtr)
        setattr(self._serial, "rts", rts)

    def reset_input_buffer(self) -> None:
        self._serial.reset_input_buffer()  # type: ignore[attr-defined]

    def write(self, data: bytes) -> int:
        return int(self._serial.write(data))  # type: ignore[attr-defined]

    def read(self, size: int) -> bytes:
        return bytes(self._serial.read(size))  # type: ignore[attr-defined]

    def close(self) -> None:
        self._serial.close()  # type: ignore[attr-defined]


class PySerialFactory:
    """Optional runtime adapter; importing this module does not require pyserial."""

    @staticmethod
    def require_available() -> Any:
        """Load verification support without enumerating or opening a device."""

        try:
            import serial  # type: ignore
        except ImportError as exc:
            install_command = shlex.join(
                (sys.executable, "-m", "pip", "install", "pyserial==3.5")
            )
            raise RuntimeVerificationError(
                f"pyserial is required for device verification by {sys.executable}; "
                f"install it with `{install_command}`"
            ) from exc
        return serial

    def open(
        self,
        port: str,
        *,
        baudrate: int,
        read_timeout_seconds: float,
        dtr: bool = False,
        rts: bool = False,
    ) -> SerialTransport:
        serial = self.require_available()
        raw = None
        try:
            raw = serial.Serial(
                port=None,
                baudrate=baudrate,
                timeout=read_timeout_seconds,
                write_timeout=read_timeout_seconds,
                dsrdtr=False,
                rtscts=False,
            )
            raw.port = port
            # Native USB CDC observes the line state at open. Applying DTR
            # afterward can leave the console silent for the entire probe.
            raw.dtr = dtr
            raw.rts = rts
            raw.open()
        except Exception as exc:
            if raw is not None:
                try:
                    raw.close()
                except Exception:
                    pass
            raise RuntimeVerificationError(
                f"could not open exact application serial port {port}: {exc}"
            ) from exc
        return _PySerialTransport(raw)


def parse_serial_firmware_version(output: str) -> str:
    versions = {
        match.group("version") for match in _SERIAL_VERSION_LINE_RE.finditer(output)
    }
    if len(versions) != 1:
        raise RuntimeVerificationError(
            "serial response must contain one unambiguous `firmware: VERSION` line"
        )
    version = versions.pop()
    if not _VERSION_RE.fullmatch(version):
        raise RuntimeVerificationError("serial firmware version is malformed")
    return version












def serial_probe_argv(
    *,
    python_executable: str,
    port: str,
    expected_version: str,
    expected_image_sha256: str | None = None,
    assert_dtr: bool = False,
) -> tuple[str, ...]:
    """Build the argv for the disposable production serial probe."""

    argv = (
        python_executable,
        "-m",
        "tools.dev.runtime_verify",
        "serial-probe",
        "--port",
        port,
        "--expected-version",
        expected_version,
    )
    if assert_dtr:
        argv += ("--assert-dtr",)
    if expected_image_sha256 is not None:
        argv += ("--expected-image-sha256", expected_image_sha256)
    return argv


def parse_serial_probe_result(output: str, *, expected_version: str) -> str:
    """Validate the complete, deliberately tiny serial-probe wire result."""

    try:
        decoded = json.loads(output)
    except json.JSONDecodeError as exc:
        raise RuntimeVerificationError(
            "serial version probe returned malformed JSON"
        ) from exc
    if not isinstance(decoded, dict) or set(decoded) != {"schema", "version"}:
        raise RuntimeVerificationError("serial version probe returned an invalid result")
    if decoded["schema"] != SERIAL_PROBE_SCHEMA:
        raise RuntimeVerificationError("serial version probe schema did not match")
    actual = decoded["version"]
    if not isinstance(actual, str) or not _VERSION_RE.fullmatch(actual):
        raise RuntimeVerificationError("serial version probe returned a malformed version")
    if actual != expected_version:
        raise RuntimeVerificationError(
            f"serial firmware version {actual!r} does not match "
            f"expected {expected_version!r}"
        )
    return actual


def verify_serial_version(
    factory: SerialFactory,
    *,
    port: str,
    expected_version: str,
    expected_image_sha256: str | None = None,
    assert_dtr: bool = False,
    timeout_seconds: float = 5.0,
    read_timeout_seconds: float = 0.1,
    poll_interval_seconds: float = 0.02,
    request_retry_seconds: float = 0.25,
    max_response_bytes: int = 16 * 1024,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> str:
    """Ask the exact USB CDC child for its compiled firmware version."""

    if not port.strip():
        raise RuntimeVerificationError("exact application serial port is required")
    if not _VERSION_RE.fullmatch(expected_version):
        raise RuntimeVerificationError("expected firmware version is malformed")
    if expected_image_sha256 is not None and not _DIGEST_RE.fullmatch(expected_image_sha256):
        raise RuntimeVerificationError("expected image_sha256 is malformed")

    if min(timeout_seconds, read_timeout_seconds, poll_interval_seconds,
           request_retry_seconds) <= 0:
        raise ValueError("serial verification timeouts must be positive")
    if max_response_bytes < 128:
        raise ValueError("serial response bound is too small")

    try:
        transport = factory.open(
            port,
            baudrate=115200,
            read_timeout_seconds=read_timeout_seconds,
            # Older S3 images require both DTR and RTS for console output.
            # Keep the complete line state for compatibility even though newer
            # firmware also replies to input with inactive modem lines.
            dtr=assert_dtr,
            rts=assert_dtr,
        )
    except RuntimeVerificationError:
        raise
    except Exception as exc:
        raise RuntimeVerificationError(
            f"could not open exact application serial port {port}: {exc}"
        ) from exc
    try:
        try:
            transport.set_control_lines(dtr=assert_dtr, rts=assert_dtr)
            # A guarded trial may have already printed its only panic before
            # the application USB port appeared. Keep that bounded first-boot
            # output for diagnosis; the generic digest probe also checks acceptance.
            if expected_image_sha256 is None:
                transport.reset_input_buffer()
        except Exception as exc:
            raise RuntimeVerificationError(
                f"could not configure exact application serial port {port}: {exc}"
            ) from exc
        def with_trial_incident(message: str) -> str:
            if expected_image_sha256 is None:
                return message
            incident = _read_failure_incident(
                transport, monotonic=monotonic, sleep=sleep
            )
            return f"{message}; first_post_trial_incident={incident}"

        request = b"version\n"
        requests_sent = 0
        completed_requests = 0
        last_completed_reply_start = -1
        def send_request() -> None:
            nonlocal requests_sent
            try:
                written = transport.write(request)
            except Exception as exc:
                raise RuntimeVerificationError(
                    with_trial_incident(f"serial version request failed: {exc}")
                ) from exc
            if written != len(request):
                raise RuntimeVerificationError(
                    with_trial_incident(
                        "serial version request was only partially written"
                    )
                )
            requests_sent += 1

        send_request()

        deadline = monotonic() + timeout_seconds
        next_request_at = monotonic() + request_retry_seconds
        response = bytearray()
        while monotonic() < deadline:
            try:
                chunk = transport.read(1024)
            except Exception as exc:
                raise RuntimeVerificationError(
                    with_trial_incident(f"serial version read failed: {exc}")
                ) from exc
            if chunk:
                response.extend(chunk)
                if len(response) > max_response_bytes:
                    raise RuntimeVerificationError(
                        with_trial_incident(
                            "serial version response exceeded its bounded buffer"
                        )
                    )
                # The board console also uses bare CR when redrawing its
                # prompt. Python's MULTILINE anchor recognizes LF only, so
                # normalize before matching the exact identity fields.
                text = response.decode("utf-8", errors="replace")
                text = text.replace("\r\n", "\n").replace("\r", "\n")
                complete_end = text.rfind("\n")
                complete_text = text[: complete_end + 1]
                matches = [
                    match.group("version")
                    for match in _SERIAL_VERSION_LINE_RE.finditer(complete_text)
                ]
                if matches:
                    if any(version != matches[0] for version in matches):
                        raise RuntimeVerificationError(
                            with_trial_incident(
                                "serial returned disagreeing firmware versions"
                            )
                        )
                    actual = matches[0]
                    if actual != expected_version:
                        raise RuntimeVerificationError(
                            with_trial_incident(
                                f"serial firmware version {actual!r} does not match "
                                f"expected {expected_version!r}"
                            )
                        )
                    if expected_image_sha256 is None:
                        return actual
                    digest_matches = list(_SERIAL_DIGEST_LINE_RE.finditer(complete_text))
                    for index, digest_match in enumerate(digest_matches):
                        digest = digest_match.group("digest")
                        if digest != "pending" and digest != expected_image_sha256:
                            raise RuntimeVerificationError(
                                with_trial_incident(
                                    "serial running image_sha256 does not match the verified artifact: "
                                    f"observed={digest} expected={expected_image_sha256}"
                                )
                            )
                        next_digest_start = (
                            digest_matches[index + 1].start()
                            if index + 1 < len(digest_matches)
                            else len(complete_text)
                        )
                        next_version = _SERIAL_VERSION_LINE_RE.search(
                            complete_text, digest_match.end()
                        )
                        if next_version is not None:
                            next_digest_start = min(next_digest_start, next_version.start())
                        accepted = _SERIAL_ACCEPTED_LINE_RE.search(
                            complete_text, digest_match.end(), next_digest_start
                        )
                        completion = accepted
                        if (accepted is not None and
                                completion is not None and
                                completion.start() > last_completed_reply_start):
                            completed_requests = min(requests_sent, completed_requests + 1)
                            # A match's end can grow as later whitespace arrives;
                            # its start identifies this completed reply once.
                            last_completed_reply_start = completion.start()
                        if (digest == expected_image_sha256 and
                                accepted is not None and
                                completion is not None and
                                accepted.group("accepted") == "1"):
                            return actual
            now = monotonic()
            # One startup-loss retry is allowed. Silence and partial replies
            # cannot build a command backlog ahead of the failure diagnostic.
            if (now < deadline and now >= next_request_at and
                    requests_sent - completed_requests < SERIAL_MAX_UNANSWERED_VERSION_REQUESTS):
                send_request()
                next_request_at = now + request_retry_seconds
            if not chunk:
                sleep(min(poll_interval_seconds, max(0.0, deadline - monotonic())))
        if expected_image_sha256 is not None:
            observed = response.decode("utf-8", errors="replace")
            observed = observed.replace("\r\n", "\n").replace("\r", "\n")
            if (_SERIAL_VERSION_LINE_RE.search(observed) and
                    not _SERIAL_DIGEST_LINE_RE.search(observed)):
                # The same version can be present in the preserved rescue
                # image. Retain a bounded observation so a rollback or boot
                # failure is diagnosable without treating version as proof.
                first_boot = observed[:2400].strip().replace("\x00", "")
                tail = observed[-800:].strip().replace("\x00", "")
                raise RuntimeVerificationError(
                    with_trial_incident(
                        "serial answered with a firmware version but no running image "
                        "digest; the trial image was not verified. "
                        f"Observed first boot: {first_boot!r}; serial tail: {tail!r}"
                    )
                )
        raise RuntimeVerificationError(
            with_trial_incident(
                "timed out waiting for serial firmware identity and OTA acceptance"
            )
            if expected_image_sha256 is not None
            else "timed out waiting for serial firmware version"
        )
    finally:
        close_serial_transport(transport, primary_error=sys.exc_info()[1])


class BleControlSession(Protocol):
    async def send_json(self, payload: Mapping[str, object]) -> None: ...

    async def next_notification(self, timeout_seconds: float) -> bytes: ...

    async def close(self) -> None: ...


class BleControlFactory(Protocol):
    async def connect_exact(
        self,
        selector: str,
        *,
        scan_timeout_seconds: float,
    ) -> BleControlSession: ...


class _BleakControlSession:
    def __init__(self, client: object) -> None:
        self._client = client
        self._notifications: asyncio.Queue[bytes] = asyncio.Queue()
        self._subscribed = False

    async def start(self) -> None:
        def on_notification(_: object, payload: bytearray) -> None:
            self._notifications.put_nowait(bytes(payload))

        await self._client.start_notify(  # type: ignore[attr-defined]
            OTA_CONTROL_CHARACTERISTIC_UUID,
            on_notification,
        )
        self._subscribed = True

    async def send_json(self, payload: Mapping[str, object]) -> None:
        encoded = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        await self._client.write_gatt_char(  # type: ignore[attr-defined]
            OTA_CONTROL_CHARACTERISTIC_UUID,
            encoded,
            response=True,
        )

    async def next_notification(self, timeout_seconds: float) -> bytes:
        return await asyncio.wait_for(
            self._notifications.get(),
            timeout=timeout_seconds,
        )

    async def close(self) -> None:
        cancellation: asyncio.CancelledError | None = None
        teardown_error: RuntimeVerificationError | None = None
        if self._subscribed:
            try:
                await self._client.stop_notify(  # type: ignore[attr-defined]
                    OTA_CONTROL_CHARACTERISTIC_UUID
                )
            except asyncio.CancelledError as error:
                cancellation = error
            except Exception as error:
                teardown_error = RuntimeVerificationError(
                    f"BLE stop-notify teardown failed: {error}"
                )
            self._subscribed = False
        try:
            await _disconnect_client_bounded(self._client)
        except asyncio.CancelledError as error:
            cancellation = cancellation or error
        except RuntimeVerificationError as error:
            if teardown_error is None:
                teardown_error = error
            else:
                _add_exception_note(teardown_error, str(error))
        if cancellation is not None:
            if teardown_error is not None:
                _add_exception_note(cancellation, str(teardown_error))
            raise cancellation
        if teardown_error is not None:
            raise teardown_error


async def _disconnect_client_bounded(client: object) -> None:
    """Confirm disconnect, or fail boundedly without suppressing uncertainty."""

    try:
        awaitable = client.disconnect()  # type: ignore[attr-defined]
        operation = asyncio.create_task(awaitable)
    except Exception as error:
        raise RuntimeVerificationError(
            f"BLE disconnect could not start: {error}"
        ) from error
    try:
        done, pending = await asyncio.wait(
            {operation},
            timeout=CLIENT_CLEANUP_TIMEOUT_SECONDS,
        )
    except asyncio.CancelledError:
        await _cancel_and_drain_bounded(operation)
        raise
    if pending:
        await _cancel_and_drain_bounded(operation)
        raise RuntimeVerificationError(
            "BLE disconnect exceeded its bounded teardown deadline"
        )
    assert done == {operation}
    try:
        operation.result()
    except asyncio.CancelledError as error:
        raise RuntimeVerificationError("BLE disconnect was cancelled") from error
    except Exception as error:
        raise RuntimeVerificationError(f"BLE disconnect failed: {error}") from error


def _consume_task_result(task: asyncio.Task[object]) -> None:
    try:
        task.result()
    except BaseException:
        pass


async def _cancel_and_drain_bounded(
    operation: asyncio.Task[object],
    *,
    timeout_seconds: float | None = None,
) -> None:
    """Cancel a cleanup task without ever waiting beyond the cleanup bound."""

    cleanup_timeout = (
        CLIENT_CLEANUP_TIMEOUT_SECONDS
        if timeout_seconds is None
        else timeout_seconds
    )
    operation.cancel()
    done, pending = await asyncio.wait(
        {operation},
        timeout=cleanup_timeout,
    )
    for task in done:
        _consume_task_result(task)
    for task in pending:
        task.cancel()
        task.add_done_callback(_consume_task_result)


async def _close_session_bounded(
    session: BleControlSession,
    *,
    timeout_seconds: float,
) -> None:
    """Require confirmed session teardown without trusting cancellation."""

    try:
        operation = asyncio.create_task(session.close())
    except Exception as error:
        raise RuntimeVerificationError(
            f"BLE session teardown could not start: {error}"
        ) from error
    try:
        done, pending = await asyncio.wait({operation}, timeout=timeout_seconds)
    except asyncio.CancelledError:
        await _cancel_and_drain_bounded(
            operation,
            timeout_seconds=min(timeout_seconds, CLIENT_CLEANUP_TIMEOUT_SECONDS),
        )
        raise
    if pending:
        await _cancel_and_drain_bounded(
            operation,
            timeout_seconds=min(timeout_seconds, CLIENT_CLEANUP_TIMEOUT_SECONDS),
        )
        raise RuntimeVerificationError(
            "BLE session teardown exceeded its bounded deadline"
        )
    assert done == {operation}
    try:
        operation.result()
    except asyncio.CancelledError as error:
        raise RuntimeVerificationError("BLE session teardown was cancelled") from error
    except RuntimeVerificationError:
        raise
    except Exception as error:
        raise RuntimeVerificationError(f"BLE session teardown failed: {error}") from error


class BleakControlFactory:
    """Optional exact-selector Bleak adapter with no only-board fallback."""

    @staticmethod
    def _load_bleak() -> tuple[object, object, type[Exception]]:
        try:
            from bleak import BleakClient, BleakScanner  # type: ignore
            from bleak.exc import BleakError  # type: ignore
        except ImportError as exc:
            raise RuntimeVerificationError(
                "bleak is required for running-image digest verification; install it "
                "with `python3 -m pip install bleak`"
            ) from exc
        return BleakClient, BleakScanner, BleakError

    async def connect_exact(
        self,
        selector: str,
        *,
        scan_timeout_seconds: float,
    ) -> BleControlSession:
        if not selector.strip():
            raise RuntimeVerificationError(
                "an exact BLE address or CoreBluetooth UUID is required"
            )
        BleakClient, BleakScanner, _ = self._load_bleak()
        try:
            discovered = await BleakScanner.discover(  # type: ignore[attr-defined]
                timeout=scan_timeout_seconds,
                return_adv=True,
            )
        except Exception as exc:
            raise RuntimeVerificationError(f"BLE scan failed: {exc}") from exc

        items = discovered.values() if isinstance(discovered, dict) else discovered
        matches: list[object] = []
        selector_key = selector.strip().casefold()
        for item in items:
            device = item
            advertisement = None
            if isinstance(item, tuple) and len(item) == 2:
                device, advertisement = item
            services = set()
            if advertisement is not None:
                services.update(
                    str(value).casefold()
                    for value in (getattr(advertisement, "service_uuids", None) or ())
                )
            metadata = getattr(device, "metadata", None) or {}
            services.update(
                str(value).casefold() for value in (metadata.get("uuids", ()) or ())
            )
            if BASE_SERVICE_UUID not in services:
                continue
            address = str(getattr(device, "address", "")).strip()
            if selector_key == address.casefold():
                matches.append(device)

        if not matches:
            raise RuntimeVerificationError(
                "no service-advertising Litch board exactly matched BLE address/UUID "
                f"{selector!r}"
            )
        if len(matches) != 1:
            raise RuntimeVerificationError(
                f"BLE selector {selector!r} is ambiguous across {len(matches)} boards"
            )

        address = str(getattr(matches[0], "address", ""))
        client = BleakClient(address)  # type: ignore[operator]
        try:
            await client.connect()
            session = _BleakControlSession(client)
            await session.start()
            return session
        except asyncio.CancelledError as primary:
            try:
                await _disconnect_client_bounded(client)
            except BaseException as cleanup_error:
                _add_exception_note(
                    primary,
                    f"BLE connect cancellation cleanup failed: {cleanup_error}",
                )
            raise
        except Exception as exc:
            try:
                await _disconnect_client_bounded(client)
            except BaseException as cleanup_error:
                _add_exception_note(
                    exc,
                    f"BLE connect failure cleanup failed: {cleanup_error}",
                )
            raise RuntimeVerificationError(
                f"failed to connect to exact BLE board {selector!r}: {exc}"
            ) from exc


def _decode_control_json(payload: bytes) -> dict[str, object]:
    try:
        text = payload.decode("utf-8")
        decoded = json.loads(text)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeVerificationError(
            "BLE OTA-control notification was not valid UTF-8 JSON"
        ) from exc
    if not isinstance(decoded, dict):
        raise RuntimeVerificationError("BLE OTA-control notification was not an object")
    return decoded


class FirmwareDigestAssembler:
    """Strict parser for pending, ordered h0..h7 fragments, then ready."""

    def __init__(self) -> None:
        self._fragments: list[str] = []
        self._pending_seen = False
        self._complete = False

    def feed(self, payload: bytes) -> str | None:
        if self._complete:
            raise RuntimeVerificationError("digest notification arrived after ready")
        decoded = _decode_control_json(payload)
        if "e" in decoded:
            raise RuntimeVerificationError(
                f"firmware digest request failed: {decoded.get('e')}"
            )

        digest_keys = [
            key for key in decoded if key == "h" or re.fullmatch(r"h\d+", key)
        ]
        if not digest_keys:
            return None
        if len(digest_keys) != 1:
            raise RuntimeVerificationError(
                "digest notification must contain exactly one lifecycle field"
            )
        key = digest_keys[0]
        value = decoded[key]
        if key == "h":
            if value == "p":
                if self._pending_seen or self._fragments:
                    raise RuntimeVerificationError("digest pending state was repeated or late")
                self._pending_seen = True
                return None
            if value in {"e", "u"}:
                raise RuntimeVerificationError(
                    "firmware digest calculation failed or is unavailable"
                )
            if value != "r":
                raise RuntimeVerificationError(f"unknown digest lifecycle token {value!r}")
            if len(self._fragments) != 8:
                raise RuntimeVerificationError(
                    "firmware digest became ready before all eight fragments"
                )
            digest = "".join(self._fragments)
            if not _DIGEST_RE.fullmatch(digest):
                raise RuntimeVerificationError("assembled firmware digest is malformed")
            self._complete = True
            return digest

        index = int(key[1:])
        if index >= 8:
            raise RuntimeVerificationError(f"digest fragment index {index} is out of range")
        if index != len(self._fragments):
            raise RuntimeVerificationError(
                f"digest fragment h{index} was duplicated or arrived out of order"
            )
        if not isinstance(value, str) or not _DIGEST_FRAGMENT_RE.fullmatch(value):
            raise RuntimeVerificationError(
                f"digest fragment h{index} must be eight lowercase hex characters"
            )
        self._fragments.append(value)
        return None


@dataclass(frozen=True)
class BleRuntimeVerification:
    version: str
    image_sha256: str


@dataclass(frozen=True)
class RunningFirmwareVerification:
    serial_version: str
    ble_version: str
    image_sha256: str


async def _next_before_deadline(
    session: BleControlSession,
    *,
    deadline: float,
    monotonic: Callable[[], float],
    label: str,
) -> bytes:
    remaining = deadline - monotonic()
    if remaining <= 0:
        raise RuntimeVerificationError(f"timed out waiting for BLE {label}")
    try:
        return await session.next_notification(remaining)
    except (asyncio.TimeoutError, TimeoutError) as exc:
        raise RuntimeVerificationError(f"timed out waiting for BLE {label}") from exc
    except Exception as exc:
        raise RuntimeVerificationError(f"BLE {label} read failed: {exc}") from exc


async def _send_before_deadline(
    session: BleControlSession,
    payload: Mapping[str, object],
    *,
    deadline: float,
    monotonic: Callable[[], float],
    label: str,
) -> None:
    remaining = deadline - monotonic()
    if remaining <= 0:
        raise RuntimeVerificationError(f"timed out sending BLE {label}")
    try:
        await asyncio.wait_for(session.send_json(payload), timeout=remaining)
    except (asyncio.TimeoutError, TimeoutError) as exc:
        raise RuntimeVerificationError(f"timed out sending BLE {label}") from exc
    except Exception as exc:
        raise RuntimeVerificationError(f"BLE {label} request failed to send: {exc}") from exc


async def verify_ble_runtime(
    factory: BleControlFactory,
    *,
    selector: str,
    expected_version: str,
    expected_image_sha256: str,
    require_ota_accepted: bool = False,
    acceptance_timeout_seconds: float = 20.0,
    scan_timeout_seconds: float = 4.0,
    connect_timeout_seconds: float = 12.0,
    request_timeout_seconds: float = 8.0,
    close_timeout_seconds: float = 3.0,
    monotonic: Callable[[], float] = time.monotonic,
) -> BleRuntimeVerification:
    """Verify firmware-reported version and running-partition image SHA-256."""

    if not selector.strip():
        raise RuntimeVerificationError("exact BLE selector is required")
    if not _VERSION_RE.fullmatch(expected_version):
        raise RuntimeVerificationError("expected firmware version is malformed")
    if not _DIGEST_RE.fullmatch(expected_image_sha256):
        raise RuntimeVerificationError(
            "expected image_sha256 must be 64 lowercase hex characters"
        )
    if min(
        scan_timeout_seconds,
        connect_timeout_seconds,
        request_timeout_seconds,
        close_timeout_seconds,
        acceptance_timeout_seconds,
    ) <= 0:
        raise ValueError("BLE verification timeouts must be positive")

    try:
        session = await asyncio.wait_for(
            factory.connect_exact(
                selector,
                scan_timeout_seconds=scan_timeout_seconds,
            ),
            timeout=connect_timeout_seconds,
        )
    except (asyncio.TimeoutError, TimeoutError) as exc:
        raise RuntimeVerificationError(
            f"timed out connecting to exact BLE board {selector!r}"
        ) from exc
    except RuntimeVerificationError:
        raise
    except Exception as exc:
        raise RuntimeVerificationError(
            f"could not connect to exact BLE board {selector!r}: {exc}"
        ) from exc

    try:
        version_deadline = monotonic() + request_timeout_seconds
        await _send_before_deadline(
            session,
            {"cmd": "fw_version"},
            deadline=version_deadline,
            monotonic=monotonic,
            label="firmware version",
        )
        actual_version: str | None = None
        while actual_version is None:
            payload = await _next_before_deadline(
                session,
                deadline=version_deadline,
                monotonic=monotonic,
                label="firmware version",
            )
            decoded = _decode_control_json(payload)
            if "e" in decoded:
                raise RuntimeVerificationError(
                    f"firmware version request failed: {decoded.get('e')}"
                )
            if "v" not in decoded:
                continue
            value = decoded["v"]
            if not isinstance(value, str) or not _VERSION_RE.fullmatch(value):
                raise RuntimeVerificationError("BLE firmware version is malformed")
            actual_version = value
        if actual_version != expected_version:
            raise RuntimeVerificationError(
                f"BLE firmware version {actual_version!r} does not match "
                f"expected {expected_version!r}"
            )

        digest_deadline = monotonic() + request_timeout_seconds
        await _send_before_deadline(
            session,
            {"cmd": "fw_digest"},
            deadline=digest_deadline,
            monotonic=monotonic,
            label="firmware digest",
        )
        assembler = FirmwareDigestAssembler()
        actual_digest: str | None = None
        while actual_digest is None:
            payload = await _next_before_deadline(
                session,
                deadline=digest_deadline,
                monotonic=monotonic,
                label="running image digest",
            )
            actual_digest = assembler.feed(payload)
        if actual_digest != expected_image_sha256:
            raise RuntimeVerificationError(
                "running image_sha256 does not match the verified firmware artifact: "
                f"observed={actual_digest} expected={expected_image_sha256}"
            )
        if require_ota_accepted:
            acceptance_deadline = monotonic() + acceptance_timeout_seconds
            while True:
                await _send_before_deadline(
                    session,
                    {"cmd": "ota_state"},
                    deadline=acceptance_deadline,
                    monotonic=monotonic,
                    label="OTA runtime acceptance",
                )
                while True:
                    payload = await _next_before_deadline(
                        session,
                        deadline=acceptance_deadline,
                        monotonic=monotonic,
                        label="OTA runtime acceptance",
                    )
                    state = _decode_control_json(payload)
                    if "e" in state:
                        raise RuntimeVerificationError(
                            f"OTA acceptance proof unavailable: {state['e']}"
                        )
                    if "ov" in state:
                        break
                if state == {"ov": "v"}:
                    break
                if state != {"ov": "p"}:
                    raise RuntimeVerificationError("malformed OTA acceptance proof")
                # Poll only after an explicit not-yet-accepted response. Time
                # bounds retries; elapsed time is never acceptance evidence.
                await asyncio.sleep(min(0.25, max(0.0, acceptance_deadline - monotonic())))
        verified = BleRuntimeVerification(actual_version, actual_digest)
    except BaseException as primary_error:
        try:
            await _close_session_bounded(
                session,
                timeout_seconds=close_timeout_seconds,
            )
        except BaseException as cleanup_error:
            _add_exception_note(
                primary_error,
                f"BLE verification teardown failed: {cleanup_error}",
            )
        raise
    else:
        await _close_session_bounded(
            session,
            timeout_seconds=close_timeout_seconds,
        )
        return verified


async def verify_running_firmware(
    *,
    serial_factory: SerialFactory,
    ble_factory: BleControlFactory,
    serial_port: str,
    ble_selector: str,
    expected_version: str,
    expected_image_sha256: str,
) -> RunningFirmwareVerification:
    serial_version = verify_serial_version(
        serial_factory,
        port=serial_port,
        expected_version=expected_version,
    )
    ble = await verify_ble_runtime(
        ble_factory,
        selector=ble_selector,
        expected_version=expected_version,
        expected_image_sha256=expected_image_sha256,
    )
    return RunningFirmwareVerification(
        serial_version=serial_version,
        ble_version=ble.version,
        image_sha256=ble.image_sha256,
    )


_AsyncResult = TypeVar("_AsyncResult")


def _drain_owned_loop_bounded(
    loop: asyncio.AbstractEventLoop,
    *,
    timeout_seconds: float,
) -> None:
    """Cancel loop tasks, then abandon cancellation-resistant work boundedly."""

    pending = {task for task in asyncio.all_tasks(loop) if not task.done()}
    if not pending:
        return
    for task in pending:
        task.cancel()

    def stop_when_drained(_: asyncio.Task[object]) -> None:
        if all(task.done() for task in pending):
            loop.stop()

    for task in pending:
        task.add_done_callback(stop_when_drained)
    stopper = loop.call_later(timeout_seconds, loop.stop)
    try:
        loop.run_forever()
    finally:
        stopper.cancel()
        for task in pending:
            task.remove_done_callback(stop_when_drained)

    for task in pending:
        if task.done():
            _consume_task_result(task)
            continue
        task.cancel()
        # asyncio.run() gathers pending tasks without a deadline. This helper
        # instead owns and closes its loop, so a library awaitable that rejects
        # cancellation cannot outlive the bounded hardware-verification lane.
        # Suppress only the CPython destruction diagnostic for that deliberately
        # abandoned task; the closed loop cannot schedule further device I/O.
        setattr(task, "_log_destroy_pending", False)


def _run_coroutine_in_owned_loop(
    coroutine: Coroutine[Any, Any, _AsyncResult],
    *,
    timeout_seconds: float,
    cleanup_timeout_seconds: float = CLIENT_CLEANUP_TIMEOUT_SECONDS,
    label: str,
) -> _AsyncResult:
    """Run async hardware work with a hard deadline and bounded loop teardown."""

    if (
        not math.isfinite(timeout_seconds)
        or not math.isfinite(cleanup_timeout_seconds)
        or min(timeout_seconds, cleanup_timeout_seconds) <= 0
    ):
        coroutine.close()
        raise ValueError("async workflow timeouts must be positive")
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        pass
    else:
        coroutine.close()
        raise RuntimeError("owned async workflow cannot run inside another event loop")

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    task = loop.create_task(coroutine)
    task.add_done_callback(lambda _: loop.stop())
    stopper = loop.call_later(timeout_seconds, loop.stop)
    try:
        loop.run_forever()
        if not task.done():
            raise RuntimeVerificationError(
                f"{label} exceeded its {timeout_seconds:g}s hard deadline"
            )
        return task.result()
    finally:
        stopper.cancel()
        _drain_owned_loop_bounded(
            loop,
            timeout_seconds=cleanup_timeout_seconds,
        )
        loop.close()
        asyncio.set_event_loop(None)


def run_ble_runtime_bounded(
    *,
    ble_factory: BleControlFactory,
    ble_selector: str,
    expected_version: str,
    expected_image_sha256: str,
    require_ota_accepted: bool = False,
    timeout_seconds: float = RUNNING_VERIFICATION_TIMEOUT_SECONDS,
) -> BleRuntimeVerification:
    """Synchronously verify BLE runtime identity with bounded loop teardown."""

    return _run_coroutine_in_owned_loop(
        verify_ble_runtime(
            factory=ble_factory,
            selector=ble_selector,
            expected_version=expected_version,
            expected_image_sha256=expected_image_sha256,
            require_ota_accepted=require_ota_accepted,
        ),
        timeout_seconds=timeout_seconds,
        label="BLE running-firmware verification",
    )


def _serial_probe_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.dev.runtime_verify serial-probe",
    )
    subcommands = parser.add_subparsers(dest="command", required=True)
    serial_probe = subcommands.add_parser("serial-probe")
    serial_probe.add_argument("--port", required=True)
    serial_probe.add_argument("--expected-version", required=True)
    serial_probe.add_argument("--expected-image-sha256")
    serial_probe.add_argument("--assert-dtr", action="store_true")
    args = parser.parse_args(argv)
    assert args.command == "serial-probe"
    try:
        actual = verify_serial_version(
            PySerialFactory(),
            port=args.port,
            expected_version=args.expected_version,
            expected_image_sha256=args.expected_image_sha256,
            assert_dtr=args.assert_dtr,
            timeout_seconds=14.0 if args.expected_image_sha256 else 5.0,
        )
    except Exception as error:
        print(f"serial version probe failed: {error}", file=sys.stderr, flush=True)
        return 1
    print(
        json.dumps(
            {"schema": SERIAL_PROBE_SCHEMA, "version": actual},
            separators=(",", ":"),
            sort_keys=True,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(_serial_probe_main())
