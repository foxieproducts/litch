from __future__ import annotations

import asyncio
import contextlib
from collections import deque
import io
import json
import sys
import time
import types
import unittest
from pathlib import Path
from typing import Mapping, Sequence
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import factory_status  # noqa: E402
import runtime_verify  # noqa: E402


EXPECTED_VERSION = "0.7"
EXPECTED_DIGEST = "01234567" * 8


FACTORY_STATUS = """\
hwtest: radio_role=DUT/testing channel=red
hwtest: active=1 stage=local step=hall_sweep revision=3 buttons=0/12 halls_clear=64/64 halls_active=0/64
hwtest: button_mask=0x0000/0x37ef hall_clear=0xffffffffffffffff hall_active=0x0000000000000000
hwtest: responding_expanders=0x0f known=1 motion_present=1 known=1 forced_faults=0x00 current_failed_checks=0x00
hwtest: current_hall_read=valid current_hall_clear=1
hwtest: sensor=1 raw_lux=30 room=0 covered=0 recovered=0 next=uncover_in_room_light
hwtest: sensor=2 raw_lux=40 room=0 covered=0 recovered=0 next=uncover_in_room_light
hwtest: auto_mask=0x0e/0x0e auto_under_10s=1 motion_event=0 leds_confirmed=0 radio_mask=0x00
hwtest: checks led_scan=n/a board_io=pass light_1=uncover_in_room_light light_2=uncover_in_room_light motion_link=pass motion_event=pending led_review=pending radio_ble=pending
hwtest: radio_proof=pending_not_broken_radio_proof peer_candidate=0 discovery_deadline_ms=0 ble_endpoint_error=0 save_pending=0 override_pending=0
hwtest: retained_radio_phase current=0 previous=0 previous_valid=0 reset_reason=0
hwtest: manual_ambient_response=not_required observed_mask=0x00 manual_motion=not_required observed=0
hwtest: provisioning_state=pending completed_result=blocked next=complete_full_test_and_save
hwtest: saved_record=none
"""


class FactoryStatusTests(unittest.TestCase):
    def test_factory_status_accepts_required_light_change_and_keeps_legacy_policy(self) -> None:
        required = FACTORY_STATUS.replace("manual_ambient_response=not_required", "manual_ambient_response=required")
        for state in ("hold_light_steady", "change_light"):
            status = factory_status.parse_factory_status(required.replace("uncover_in_room_light", state))
            self.assertEqual(status.checks["light_1"], state)
            self.assertEqual(status.observations["manual_ambient_response"], "required")
            self.assertFalse(status.durable_pass)
        legacy = factory_status.parse_factory_status(FACTORY_STATUS)
        self.assertEqual(legacy.observations["manual_ambient_response"], "not_required")

    def test_factory_status_preserves_live_observations_with_console_noise(self) -> None:
        text = FACTORY_STATUS.replace(
            "hwtest: sensor=1", "I [storage] save complete\n> hwtest: sensor=1"
        ).replace("\n", "\r\n")
        status = factory_status.parse_factory_status("hwtest: saved_record=none\r>\r" + text + "> ")

        self.assertTrue(status.active)
        self.assertEqual((status.stage, status.step, status.revision), ("local", "hall_sweep", 3))
        self.assertEqual(status.radio_role, "DUT/testing")
        self.assertEqual(status.observations["halls_clear"], "64")
        self.assertEqual(status.observations["sensor_1_raw_lux"], "30")
        self.assertEqual(status.observations["sensor_2_raw_lux"], "40")
        self.assertEqual(status.checks["light_1"], "uncover_in_room_light")
        self.assertEqual(status.checks["motion_event"], "pending")
        self.assertEqual(status.checks["radio_ble"], "pending")
        self.assertEqual(status.current_failures, ())
        self.assertFalse(status.durable_pass)

    def test_factory_status_requires_current_settled_pass_or_a_saved_postboot_pass(self) -> None:
        saved = FACTORY_STATUS.replace(
            "provisioning_state=pending completed_result=blocked next=complete_full_test_and_save",
            "provisioning_state=passed completed_result=allowed next=none",
        ).replace("saved_record=none", "saved_record=valid radio=1 verifier=0 auto_ms=900 operator_ms=12000 radio_ms=16000")
        self.assertFalse(factory_status.parse_factory_status(saved).durable_pass)
        self.assertFalse(factory_status.parse_factory_status(saved.replace("active=1", "active=0")).durable_pass)
        stopped_failed = saved.replace("active=1 stage=local step=hall_sweep", "active=0 stage=failed step=failed_current_run")
        self.assertFalse(factory_status.parse_factory_status(stopped_failed).durable_pass)
        self.assertFalse(factory_status.parse_factory_status(
            saved.replace("stage=local step=hall_sweep", "stage=passed step=pass_saved")
        ).durable_pass)

        passed = (saved.replace("stage=local step=hall_sweep", "stage=passed step=pass_saved")
                  .replace("buttons=0/12", "buttons=12/12").replace("button_mask=0x0000", "button_mask=0x37ef")
                  .replace("halls_active=0/64", "halls_active=64/64").replace("hall_active=0x0000000000000000", "hall_active=0xffffffffffffffff")
                  .replace("leds_confirmed=0 radio_mask=0x00", "leds_confirmed=1 radio_mask=0x01")
                  .replace("led_review=pending radio_ble=pending", "led_review=pass radio_ble=pass")
                  .replace("radio_proof=pending_not_broken_radio_proof", "radio_proof=verified"))
        self.assertTrue(factory_status.parse_factory_status(passed).durable_pass)
        stopped_dut = passed.replace("active=1", "active=0")
        self.assertFalse(factory_status.parse_factory_status(stopped_dut).durable_pass)
        verifier = (stopped_dut.replace("radio_role=DUT/testing", "radio_role=VERIFIER")
                    .replace("radio=1 verifier=0", "radio=3 verifier=1"))
        self.assertTrue(factory_status.parse_factory_status(verifier).durable_pass)
        self.assertFalse(factory_status.parse_factory_status(verifier.replace("verifier=1", "verifier=0")).durable_pass)
        restored_verifier = (saved.replace("active=1 stage=local step=hall_sweep", "active=0 stage=passed step=pass_saved")
                             .replace("radio_role=DUT/testing", "radio_role=VERIFIER")
                             .replace("radio=1 verifier=0", "radio=3 verifier=1"))
        status = factory_status.parse_factory_status(restored_verifier)
        self.assertTrue(status.durable_pass)
        self.assertEqual(status.checks["radio_ble"], "pending")
        for flag in ("save_pending", "override_pending"):
            with self.subTest(flag=flag):
                unsettled = passed.replace(f"{flag}=0", f"{flag}=1")
                self.assertFalse(factory_status.parse_factory_status(unsettled).durable_pass)

        rebooted = (saved.replace("active=1 stage=local step=hall_sweep", "active=0 stage=inactive step=led_field")
                    .replace("buttons=0/12", "buttons=0/0").replace("button_mask=0x0000/0x37ef", "button_mask=0x0000/0x0000")
                    .replace("halls_clear=64/64", "halls_clear=0/64").replace("hall_clear=0xffffffffffffffff", "hall_clear=0x0000000000000000")
                    .replace("auto_mask=0x0e/0x0e", "auto_mask=0x00/0x00").replace("uncover_in_room_light", "n/a")
                    .replace("saved_record=valid radio=1", "saved_record=valid radio=3"))
        status = factory_status.parse_factory_status(rebooted)
        self.assertTrue(status.durable_pass)
        self.assertEqual(status.saved_radio, 3)
        self.assertEqual(status.observations["halls_clear"], "0")
        self.assertEqual(status.checks["radio_ble"], "pending")
        self.assertEqual(status.checks["light_1"], "n/a")

    def test_factory_status_distinguishes_override_peer_wait_and_current_failures(self) -> None:
        saved = "saved_record=valid radio={radio} verifier=0 auto_ms=900 operator_ms=12000 radio_ms=16000"
        waiting = FACTORY_STATUS.replace("stage=local step=hall_sweep", "stage=local_complete_waiting_for_peer step=local_complete_waiting_for_peer").replace(
            "saved_record=none", saved.format(radio=2)
        )
        status = factory_status.parse_factory_status(waiting)
        self.assertEqual(status.saved_radio, 2)
        self.assertFalse(status.durable_pass)
        self.assertEqual(status.current_failures, ())

        override = (FACTORY_STATUS.replace("active=1 stage=local step=hall_sweep",
                                           "active=0 stage=override_saved step=local_complete_waiting_for_peer")
                    .replace("provisioning_state=pending completed_result=blocked next=complete_full_test_and_save",
                             "provisioning_state=override completed_result=blocked next=normal_boot_override_retained")
                    .replace("saved_record=none", saved.format(radio=5)))
        status = factory_status.parse_factory_status(override)
        self.assertTrue(status.override)
        self.assertFalse(status.durable_pass)
        self.assertFalse(factory_status.parse_factory_status(override.replace("override_pending=0", "override_pending=1")).override)

        failed = (FACTORY_STATUS.replace("stage=local step=hall_sweep", "stage=failed step=failed_current_run")
                  .replace("current_failed_checks=0x00", "current_failed_checks=0x0a")
                  .replace("ble_endpoint_error=0", "ble_endpoint_error=1"))
        self.assertEqual(factory_status.parse_factory_status(failed).current_failures,
                         ("Board I/O failed", "Motion sensor link failed", "Factory BLE endpoint failed to start"))

    def test_factory_status_rejects_incomplete_duplicate_or_contradictory_transactions(self) -> None:
        for incomplete in ("", FACTORY_STATUS.rstrip("\n"), FACTORY_STATUS.split("hwtest: checks ")[0]):
            with self.subTest(incomplete_length=len(incomplete)):
                with self.assertRaises(factory_status.IncompleteFactoryStatus):
                    factory_status.parse_factory_status(incomplete)

        invalid = (
            FACTORY_STATUS * 2,
            FACTORY_STATUS.replace("active=1", "active=1 active=1"),
            FACTORY_STATUS.replace("saved_record=none", "saved_record=valid"),
            FACTORY_STATUS.replace("completed_result=blocked", "completed_result=allowed"),
            FACTORY_STATUS.replace("halls_clear=64/64", "halls_clear=0/64"),
            FACTORY_STATUS.replace("radio_proof=pending_not_broken_radio_proof", "radio_proof=verified"),
            "\n".join(line for line in FACTORY_STATUS.splitlines() if "hwtest: checks " not in line) + "\n",
        )
        for index, malformed in enumerate(invalid):
            with self.subTest(index=index):
                with self.assertRaises(factory_status.FactoryStatusError) as error:
                    factory_status.parse_factory_status(malformed)
                self.assertNotIsInstance(error.exception, factory_status.IncompleteFactoryStatus)


def message(payload: Mapping[str, object]) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


def digest_messages(*, pending: bool = True) -> list[bytes]:
    values: list[bytes] = []
    if pending:
        values.append(message({"h": "p"}))
    for index in range(8):
        values.append(message({f"h{index}": EXPECTED_DIGEST[index * 8 : (index + 1) * 8]}))
    values.append(message({"h": "r"}))
    return values


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class FakeSerialTransport:
    def __init__(
        self,
        chunks: Sequence[bytes | BaseException],
        *,
        write_count: int | None = None,
        support_response: bytes | None = None,
    ) -> None:
        self.chunks = list(chunks)
        self.write_count = write_count
        self.support_response = support_response
        self.control_lines: list[tuple[bool, bool]] = []
        self.reset_count = 0
        self.writes: list[bytes] = []
        self.closed = False

    def set_control_lines(self, *, dtr: bool, rts: bool) -> None:
        self.control_lines.append((dtr, rts))

    def reset_input_buffer(self) -> None:
        self.reset_count += 1

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        if data == b"support\n" and self.support_response is not None:
            self.chunks.append(self.support_response)
        return len(data) if self.write_count is None else self.write_count

    def read(self, size: int) -> bytes:
        if not self.chunks:
            return b""
        value = self.chunks.pop(0)
        if isinstance(value, BaseException):
            raise value
        return value[:size]

    def close(self) -> None:
        self.closed = True


class FakeSerialFactory:
    def __init__(
        self,
        transport: FakeSerialTransport | None = None,
        *,
        error: BaseException | None = None,
    ) -> None:
        self.transport = transport
        self.error = error
        self.calls: list[tuple[str, int, float, bool, bool]] = []

    def open(
        self,
        port: str,
        *,
        baudrate: int,
        read_timeout_seconds: float,
        dtr: bool = False,
        rts: bool = False,
    ) -> FakeSerialTransport:
        self.calls.append((port, baudrate, read_timeout_seconds, dtr, rts))
        if self.error is not None:
            raise self.error
        if self.transport is None:
            raise AssertionError("test factory has no transport")
        return self.transport


class DelayedSerialTransport(FakeSerialTransport):
    """Service queued commands after startup, retaining partial reply bytes."""

    def __init__(
        self,
        clock: FakeClock,
        *,
        service_at: float,
        replies: Sequence[bytes],
        drop_first_request: bool = False,
        chunk_size: int = 1024,
        read_seconds: float = 0.0,
    ) -> None:
        super().__init__(())
        self.clock = clock
        self.service_at = service_at
        self.replies = deque(replies)
        self.last_reply = replies[-1]
        self.drop_first_request = drop_first_request
        self.chunk_size = chunk_size
        self.read_seconds = read_seconds
        self.commands: deque[bytes] = deque()
        self.active = b""
        self.active_command = b""
        self.active_reply_completed = False
        self.version_requests = 0
        self.version_replies = 0
        self.max_outstanding = 0

    def write(self, data: bytes) -> int:
        if data == b"version\n":
            self.version_requests += 1
            self.max_outstanding = max(
                self.max_outstanding,
                self.version_requests - self.version_replies,
            )
        if not (self.drop_first_request and data == b"version\n" and
                self.version_requests == 1):
            self.commands.append(data)
        return super().write(data)

    def read(self, size: int) -> bytes:
        self.clock.sleep(self.read_seconds)
        if self.clock.now < self.service_at:
            return b""
        if not self.active:
            if not self.commands:
                return b""
            self.active_command = self.commands.popleft()
            self.active_reply_completed = False
            if self.active_command == b"version\n":
                self.active = self.replies.popleft() if self.replies else self.last_reply
            elif self.active_command == b"support\n":
                self.active = b'{"event":"support_incident","v":1,"boot":1,"wd":0}\r\n'
            else:
                raise AssertionError(self.active_command)
        limit = min(size, self.chunk_size)
        result, self.active = self.active[:limit], self.active[limit:]
        # Bare CR is already a complete Console line even when its optional
        # LF arrives in the next read. Count delivery at the same wire boundary.
        delivered = not self.active or (self.active == b"\n" and result.endswith(b"\r"))
        if (delivered and not self.active_reply_completed and
                self.active_command == b"version\n"):
            self.version_replies += 1
            self.active_reply_completed = True
        return result


class SerialVerificationTests(unittest.TestCase):












    def test_delayed_usb_reply_does_not_accumulate_unanswered_commands(self) -> None:
        reply = (f"firmware: {EXPECTED_VERSION}\r\n"
                 f"image_sha256: {EXPECTED_DIGEST}\r\nota_accepted: 1\r\n").encode()
        for drop_first in (False, True):
            with self.subTest(drop_first=drop_first):
                clock = FakeClock()
                transport = DelayedSerialTransport(
                    clock, service_at=9.036, replies=[reply],
                    drop_first_request=drop_first,
                )
                actual = runtime_verify.verify_serial_version(
                    FakeSerialFactory(transport), port="/dev/cu.exact-app",
                    expected_version=EXPECTED_VERSION,
                    expected_image_sha256=EXPECTED_DIGEST, assert_dtr=True,
                    timeout_seconds=14.0, monotonic=clock.monotonic, sleep=clock.sleep,
                )
                self.assertEqual(actual, EXPECTED_VERSION)
                self.assertLessEqual(transport.max_outstanding, 2)
                self.assertTrue(transport.closed)
                self.assertEqual(transport.control_lines, [(True, True), (False, False)])
                self.assertEqual(transport.reset_count, 0)

    def test_late_usb_reply_keeps_failure_diagnostic_reachable(self) -> None:
        # Preserve the witnessed Console echo/prompt shape: dozens of these
        # queued replies filled the incident reader before it reached support.
        reply = ("\r> \r> v\r> ve\r> ver\r> vers\r> versi\r> versio\r> version\r\n\r\r"
                 f"firmware: {EXPECTED_VERSION}\r\n\r> \r  \r"
                 f"image_sha256: {EXPECTED_DIGEST}\r\n\r> \r  \r"
                 "ota_accepted: 1\r\n").encode()
        clock = FakeClock()
        transport = DelayedSerialTransport(clock, service_at=14.5, replies=[reply])
        with self.assertRaises(runtime_verify.RuntimeVerificationError) as error:
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport), port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=EXPECTED_DIGEST, timeout_seconds=14.0,
                monotonic=clock.monotonic, sleep=clock.sleep,
            )
        self.assertIn("timed out", str(error.exception))
        self.assertIn('"boot":1', str(error.exception))
        self.assertLessEqual(transport.max_outstanding, 2)
        self.assertLessEqual(clock.now, 15.5)
        self.assertTrue(transport.closed)

    def test_partial_pending_replies_release_each_request_only_once(self) -> None:
        pending = f"firmware: {EXPECTED_VERSION}\r\nimage_sha256: pending\r\nota_accepted: 0\r\n".encode()
        identified = (f"firmware: {EXPECTED_VERSION}\r\n"
                      f"image_sha256: {EXPECTED_DIGEST}\r\nota_accepted: 0\r\n").encode()
        accepted = identified.replace(b"ota_accepted: 0", b"ota_accepted: 1")
        clock = FakeClock()
        transport = DelayedSerialTransport(
            clock, service_at=0.5, replies=[pending, identified, accepted],
            chunk_size=3, read_seconds=0.02,
        )
        self.assertEqual(
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport), port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=EXPECTED_DIGEST, timeout_seconds=14.0,
                monotonic=clock.monotonic, sleep=clock.sleep,
            ),
            EXPECTED_VERSION,
        )
        self.assertGreater(transport.version_requests, 2)
        self.assertLessEqual(transport.max_outstanding, 2)

    def test_usb_trial_waits_for_matching_digest_and_accepted_state(self) -> None:
        digest = "a" * 64
        transport = FakeSerialTransport((
            b"firmware: 0.7\r\nimage_sha256: pending\r\nota_accepted: 0\r\n",
            (f"firmware: 0.7\r\nimage_sha256: {digest}\r\nota_accepted: 0\r\n").encode(),
            (f"firmware: 0.7\r\nimage_sha256: {digest}\r\nota_accepted: 1\r\n").encode(),
        ))
        self.assertEqual(
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=digest,
            ),
            EXPECTED_VERSION,
        )

    def test_usb_trial_accepts_console_carriage_return_lines(self) -> None:
        digest = "a" * 64
        # The V3 console redraws its prompt with CR and emits some fields on
        # CR-only lines. The old LF-anchored parser saw firmware but missed the
        # digest and accepted state until its bounded probe timed out.
        transport = FakeSerialTransport((
            (f"firmware: 0.7\r  \rimage_sha256: {digest}\r"
             "ota_accepted: 1\r").encode(),
        ))
        self.assertEqual(
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=digest,
            ),
            EXPECTED_VERSION,
        )

    def test_usb_trial_rejects_same_version_rescue_digest(self) -> None:
        transport = FakeSerialTransport((
            (f"firmware: 0.7\r\nimage_sha256: {'b' * 64}\r\nota_accepted: 1\r\n").encode(),
        ), support_response=(
            b'{"event":"support_incident","v":1,"rr":9,"prior":1,'
            b'"prior_up":431,"stages":[4,14,14,14],"ignored":"private"}\r\n'
        ))
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "first_post_trial_incident=",
        ) as failure:
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256="a" * 64,
            )
        self.assertIn('"prior_up":431', str(failure.exception))
        self.assertNotIn("private", str(failure.exception))
        self.assertIn(b"support\n", transport.writes)

    def test_usb_trial_does_not_accept_matching_pending_image(self) -> None:
        transport = FakeSerialTransport((
            (f"firmware: 0.7\r\nimage_sha256: {'a' * 64}\r\nota_accepted: 0\r\n").encode(),
        ))
        clock = FakeClock()
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "identity and OTA acceptance",
        ):
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256="a" * 64,
                timeout_seconds=0.1,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )

    def test_usb_trial_reports_same_version_rescue_without_digest(self) -> None:
        transport = FakeSerialTransport((
            b"Guru Meditation Error: settings_io stack overflow\r\n",
            b"firmware: 0.7\r\n",
        ), support_response=(
            b'{"event":"support_incident","v":1,"rr":4,"prior":1,'
            b'"prior_up":239,"stage_ms":[100,0,0,0]}\r\n'
        ))
        clock = FakeClock()
        with self.assertRaises(runtime_verify.RuntimeVerificationError) as failure:
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256="a" * 64,
                timeout_seconds=0.1,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )
        self.assertIn("version but no running image digest", str(failure.exception))
        self.assertIn("settings_io stack overflow", str(failure.exception))
        self.assertIn('"prior_up":239', str(failure.exception))
        self.assertEqual(transport.reset_count, 0)
        self.assertIn(b"support\n", transport.writes)

    def test_usb_trial_queries_incident_after_different_rescue_version(self) -> None:
        transport = FakeSerialTransport(
            (b"firmware: 0.6\r\n",),
            support_response=(
                b'{"event":"support_incident","v":1,"rr":9,"prior":1,'
                b'"prior_up":219}\r\n'
            ),
        )
        with self.assertRaises(runtime_verify.RuntimeVerificationError) as failure:
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app", expected_version=EXPECTED_VERSION,
                expected_image_sha256="a" * 64,
            )
        self.assertIn("firmware version '0.6'", str(failure.exception))
        self.assertIn('"prior_up":219', str(failure.exception))
        self.assertIn(b"support\n", transport.writes)

    def test_failure_incident_scans_past_queued_boot_output(self) -> None:
        transport = FakeSerialTransport(
            [b"x" * 1024 for _ in range(6)],
            support_response=(
                b'{"event":"support_incident","v":1,"rr":4,"prior":1,'
                b'"prior_up":231}\r\n'
            ),
        )
        incident = runtime_verify._read_failure_incident(
            transport, monotonic=time.monotonic, sleep=time.sleep,
        )
        self.assertIn('"prior_up":231', incident)
        self.assertIn(b"support\n", transport.writes)

    def test_serial_parser_requires_one_unambiguous_version(self) -> None:
        self.assertEqual(
            runtime_verify.parse_serial_firmware_version(
                "banner\nfirmware: 0.7\nfirmware: 0.7\n"
            ),
            "0.7",
        )
        for output in (
            "banner only",
            "firmware: 0.7\nfirmware: 0.8\n",
            "firmware: bad version\n",
        ):
            with self.subTest(output=output):
                with self.assertRaises(runtime_verify.RuntimeVerificationError):
                    runtime_verify.parse_serial_firmware_version(output)

    def test_serial_verification_uses_exact_port_without_control_line_reset(self) -> None:
        transport = FakeSerialTransport((b"boot\r\n", b"firmware: 0.7\r\n"))
        factory = FakeSerialFactory(transport)
        clock = FakeClock()
        actual = runtime_verify.verify_serial_version(
            factory,
            port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION,
            timeout_seconds=1.0,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )
        self.assertEqual(actual, EXPECTED_VERSION)
        self.assertEqual(
            factory.calls,
            [("/dev/cu.exact-app", 115200, 0.1, False, False)],
        )
        self.assertEqual(transport.control_lines, [(False, False), (False, False)])
        self.assertEqual(transport.reset_count, 1)
        self.assertEqual(transport.writes, [b"version\n"])
        self.assertTrue(transport.closed)

    def test_serial_version_line_can_be_split_across_reads(self) -> None:
        transport = FakeSerialTransport((b"firmware: 0", b".7\r\n"))
        actual = runtime_verify.verify_serial_version(
            FakeSerialFactory(transport),
            port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION,
        )
        self.assertEqual(actual, EXPECTED_VERSION)

    def test_serial_version_request_retries_after_startup_discards_first_input(self) -> None:
        transport = FakeSerialTransport(
            (b"", b"", b"", b"", b"firmware: 0.7\r\n")
        )
        clock = FakeClock()
        actual = runtime_verify.verify_serial_version(
            FakeSerialFactory(transport),
            port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION,
            timeout_seconds=1.0,
            request_retry_seconds=0.05,
            monotonic=clock.monotonic,
            sleep=clock.sleep,
        )
        self.assertEqual(actual, EXPECTED_VERSION)
        self.assertEqual(transport.writes, [b"version\n", b"version\n"])

    def test_serial_verification_can_assert_dtr_for_native_usb_console(self) -> None:
        transport = FakeSerialTransport((b"firmware: 0.7\r\n",))
        factory = FakeSerialFactory(transport)
        actual = runtime_verify.verify_serial_version(
            factory,
            port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION,
            assert_dtr=True,
        )
        self.assertEqual(actual, EXPECTED_VERSION)
        self.assertEqual(
            factory.calls,
            [("/dev/cu.exact-app", 115200, 0.1, True, True)],
        )
        self.assertEqual(transport.control_lines, [(True, True), (False, False)])

    def test_serial_probe_releases_lines_before_driver_close(self) -> None:
        events = []

        class Transport(FakeSerialTransport):
            def set_control_lines(self, *, dtr: bool, rts: bool) -> None:
                events.append(("lines", dtr, rts))
                super().set_control_lines(dtr=dtr, rts=rts)

            def close(self) -> None:
                events.append(("close",))
                super().close()

        transport = Transport((b"firmware: 0.7\n",))
        runtime_verify.verify_serial_version(
            FakeSerialFactory(transport), port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION, assert_dtr=True,
        )
        # Closing at11 lets a driver-generated10->00 finish Arduino's ROM
        # handshake. Explicit00 uses the adapter's DTR-first setter instead.
        self.assertEqual(events[-2:], [("lines", False, False), ("close",)])

    def test_serial_transport_close_releases_dtr_before_rts_and_driver_close(self) -> None:
        events = []

        class RawSerial:
            def __init__(self, **kwargs) -> None:
                object.__setattr__(self, "dtr", False)
                object.__setattr__(self, "rts", False)

            def __setattr__(self, name, value) -> None:
                object.__setattr__(self, name, value)
                if name in {"dtr", "rts"}:
                    events.append(("lines", self.dtr, self.rts))

            def open(self) -> None:
                pass

            def close(self) -> None:
                events.append(("close",))

        serial_module = types.ModuleType("serial")
        serial_module.Serial = RawSerial
        with mock.patch.dict(sys.modules, {"serial": serial_module}):
            transport = runtime_verify.PySerialFactory().open(
                "/dev/cu.exact-app", baudrate=115200, read_timeout_seconds=0.1,
                dtr=True, rts=True,
            )
            runtime_verify.close_serial_transport(transport)
        self.assertEqual(events[-3:], [
            ("lines", False, True), ("lines", False, False), ("close",),
        ])

    def test_serial_probe_teardown_failure_preserves_primary_error(self) -> None:
        class Transport(FakeSerialTransport):
            def set_control_lines(self, *, dtr: bool, rts: bool) -> None:
                if not dtr and not rts:
                    raise OSError("release failed")
                super().set_control_lines(dtr=dtr, rts=rts)

        for version in (b"firmware: 0.7\n", b"firmware: 0.8\n"):
            with self.subTest(version=version):
                transport = Transport((version,))
                with self.assertRaises(runtime_verify.RuntimeVerificationError) as error:
                    runtime_verify.verify_serial_version(
                        FakeSerialFactory(transport), port="/dev/cu.exact-app",
                        expected_version=EXPECTED_VERSION, assert_dtr=True,
                    )
                self.assertTrue(transport.closed)
                if b"0.8" in version:
                    self.assertIn("does not match", str(error.exception))
                    self.assertTrue(any("teardown failed" in note
                                        for note in getattr(error.exception, "__notes__", [])))
                else:
                    self.assertIn("teardown failed", str(error.exception))

    def test_serial_transport_close_keeps_primary_error_when_release_fails(self) -> None:
        class Transport(FakeSerialTransport):
            def set_control_lines(self, *, dtr: bool, rts: bool) -> None:
                raise OSError("release failed")

        transport = Transport(())
        primary_error = StopIteration("no complete support response")
        with self.assertRaises(StopIteration) as raised:
            try:
                raise primary_error
            finally:
                runtime_verify.close_serial_transport(
                    transport, primary_error=sys.exc_info()[1],
                )
        self.assertIs(raised.exception, primary_error)
        self.assertTrue(transport.closed)
        self.assertTrue(any("release failed" in note
                            for note in getattr(primary_error, "__notes__", [])))

    def test_serial_transport_close_reports_driver_failure_without_primary_error(self) -> None:
        close_error = OSError("driver close failed")

        class Transport(FakeSerialTransport):
            def close(self) -> None:
                super().close()
                raise close_error

        transport = Transport(())
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError, "driver close failed",
        ) as raised:
            runtime_verify.close_serial_transport(transport)
        self.assertIs(raised.exception.__cause__, close_error)
        self.assertEqual(transport.control_lines, [(False, False)])
        self.assertTrue(transport.closed)

    def test_serial_dependency_preflight_does_not_open_a_device(self) -> None:
        serial_module = types.ModuleType("serial")
        serial_module.Serial = mock.Mock()  # type: ignore[attr-defined]
        with mock.patch.dict(sys.modules, {"serial": serial_module}):
            self.assertIs(
                runtime_verify.PySerialFactory.require_available(), serial_module
            )
        serial_module.Serial.assert_not_called()

    def test_missing_serial_dependency_names_the_current_python(self) -> None:
        with (
            mock.patch.dict(sys.modules, {"serial": None}),
            mock.patch.object(sys, "executable", "/tmp/Python Tools/python3"),
        ):
            with self.assertRaises(runtime_verify.RuntimeVerificationError) as error:
                runtime_verify.PySerialFactory.require_available()
        self.assertIn(
            "'/tmp/Python Tools/python3' -m pip install pyserial==3.5",
            str(error.exception),
        )

    def test_pyserial_factory_applies_requested_control_lines_before_open(self) -> None:
        events: list[tuple[object, ...]] = []

        class FakeRawSerial:
            def __init__(self, *, port: object, **_: object) -> None:
                self.port = port
                self.dtr = True
                self.rts = True
                events.append(("construct", port))

            def open(self) -> None:
                events.append(("open", self.port, self.dtr, self.rts))

            def close(self) -> None:
                events.append(("close",))

        serial_module = types.ModuleType("serial")
        serial_module.Serial = FakeRawSerial  # type: ignore[attr-defined]
        with mock.patch.dict(sys.modules, {"serial": serial_module}):
            transport = runtime_verify.PySerialFactory().open(
                "/dev/cu.exact-app",
                baudrate=115200,
                read_timeout_seconds=0.1,
                dtr=True,
                rts=False,
            )
        self.assertEqual(
            events[:2],
            [("construct", None), ("open", "/dev/cu.exact-app", True, False)],
        )
        transport.close()

    def test_serial_mismatch_disagreement_partial_write_and_read_error_fail_closed(self) -> None:
        transports = (
            FakeSerialTransport((b"firmware: 0.8\n",)),
            FakeSerialTransport((b"firmware: 0.7\nfirmware: 0.8\n",)),
            FakeSerialTransport((), write_count=2),
            FakeSerialTransport((OSError("read failed"),)),
        )
        for transport in transports:
            with self.subTest(transport=transport):
                clock = FakeClock()
                with self.assertRaises(runtime_verify.RuntimeVerificationError):
                    runtime_verify.verify_serial_version(
                        FakeSerialFactory(transport),
                        port="/dev/cu.exact-app",
                        expected_version=EXPECTED_VERSION,
                        timeout_seconds=0.2,
                        monotonic=clock.monotonic,
                        sleep=clock.sleep,
                    )
                self.assertTrue(transport.closed)

    def test_serial_wait_and_buffer_are_bounded(self) -> None:
        clock = FakeClock()
        transport = FakeSerialTransport(())
        with self.assertRaisesRegex(runtime_verify.RuntimeVerificationError, "timed out"):
            runtime_verify.verify_serial_version(
                FakeSerialFactory(transport),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                timeout_seconds=0.05,
                poll_interval_seconds=0.02,
                monotonic=clock.monotonic,
                sleep=clock.sleep,
            )
        self.assertGreaterEqual(clock.now, 0.05)
        self.assertTrue(transport.closed)

        oversized = FakeSerialTransport((b"x" * 129,))
        with self.assertRaisesRegex(runtime_verify.RuntimeVerificationError, "bounded buffer"):
            runtime_verify.verify_serial_version(
                FakeSerialFactory(oversized),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                max_response_bytes=128,
            )
        self.assertTrue(oversized.closed)

    def test_serial_open_error_is_wrapped_actionably(self) -> None:
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "exact application serial port",
        ):
            runtime_verify.verify_serial_version(
                FakeSerialFactory(error=OSError("missing")),
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
            )

    def test_serial_probe_protocol_is_strict_and_rechecks_expected_version(self) -> None:
        argv = runtime_verify.serial_probe_argv(
            python_executable="/python",
            port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION,
        )
        self.assertEqual(argv[:4], ("/python", "-m", "tools.dev.runtime_verify", "serial-probe"))
        self.assertNotIn("--assert-dtr", argv)
        usb_trial_argv = runtime_verify.serial_probe_argv(
            python_executable="/python",
            port="/dev/cu.exact-app",
            expected_version=EXPECTED_VERSION,
            expected_image_sha256="a" * 64,
        )
        self.assertEqual(
            usb_trial_argv[-2:], ("--expected-image-sha256", "a" * 64)
        )
        self.assertIn(
            "--assert-dtr",
            runtime_verify.serial_probe_argv(
                python_executable="/python",
                port="/dev/cu.exact-app",
                expected_version=EXPECTED_VERSION,
                assert_dtr=True,
            ),
        )
        valid = json.dumps(
            {
                "schema": runtime_verify.SERIAL_PROBE_SCHEMA,
                "version": EXPECTED_VERSION,
            }
        )
        self.assertEqual(
            runtime_verify.parse_serial_probe_result(
                valid,
                expected_version=EXPECTED_VERSION,
            ),
            EXPECTED_VERSION,
        )
        invalid = (
            "not-json",
            json.dumps({"version": EXPECTED_VERSION}),
            json.dumps(
                {
                    "schema": runtime_verify.SERIAL_PROBE_SCHEMA,
                    "version": "0.8",
                }
            ),
            json.dumps(
                {
                    "schema": runtime_verify.SERIAL_PROBE_SCHEMA,
                    "version": EXPECTED_VERSION,
                    "extra": True,
                }
            ),
        )
        for output in invalid:
            with self.subTest(output=output):
                with self.assertRaises(runtime_verify.RuntimeVerificationError):
                    runtime_verify.parse_serial_probe_result(
                        output,
                        expected_version=EXPECTED_VERSION,
                    )


class DigestAssemblerTests(unittest.TestCase):
    def test_accepts_optional_pending_then_ordered_fragments_and_ready(self) -> None:
        for pending in (False, True):
            with self.subTest(pending=pending):
                assembler = runtime_verify.FirmwareDigestAssembler()
                actual = None
                for payload in digest_messages(pending=pending):
                    actual = assembler.feed(payload)
                self.assertEqual(actual, EXPECTED_DIGEST)

    def test_ignores_unrelated_notifications(self) -> None:
        assembler = runtime_verify.FirmwareDigestAssembler()
        self.assertIsNone(assembler.feed(message({"st": "progress", "pct": 1})))
        actual = None
        for payload in digest_messages():
            actual = assembler.feed(payload)
        self.assertEqual(actual, EXPECTED_DIGEST)

    def test_rejects_out_of_order_duplicate_malformed_early_and_error_states(self) -> None:
        cases = (
            (message({"h1": "01234567"}),),
            (message({"h": "p"}), message({"h": "p"})),
            (message({"h0": "ABCDEF01"}),),
            (message({"h": "r"}),),
            (message({"h": "e"}),),
            (message({"h8": "01234567"}),),
            (message({"h0": "01234567", "h1": "01234567"}),),
            (message({"e": "busy"}),),
        )
        for notifications in cases:
            with self.subTest(notifications=notifications):
                assembler = runtime_verify.FirmwareDigestAssembler()
                with self.assertRaises(runtime_verify.RuntimeVerificationError):
                    for payload in notifications:
                        assembler.feed(payload)


class FakeBleSession:
    def __init__(
        self,
        notifications: Sequence[bytes | BaseException],
        *,
        send_error_at: int | None = None,
        close_error: BaseException | None = None,
    ) -> None:
        self.notifications = list(notifications)
        self.send_error_at = send_error_at
        self.close_error = close_error
        self.sent: list[dict[str, object]] = []
        self.waits: list[float] = []
        self.closed = False

    async def send_json(self, payload: Mapping[str, object]) -> None:
        self.sent.append(dict(payload))
        if self.send_error_at == len(self.sent):
            raise OSError("send failed")

    async def next_notification(self, timeout_seconds: float) -> bytes:
        self.waits.append(timeout_seconds)
        if not self.notifications:
            raise asyncio.TimeoutError()
        value = self.notifications.pop(0)
        if isinstance(value, BaseException):
            raise value
        return value

    async def close(self) -> None:
        self.closed = True
        if self.close_error is not None:
            raise self.close_error


class FakeBleFactory:
    def __init__(
        self,
        session: FakeBleSession | None = None,
        *,
        error: BaseException | None = None,
    ) -> None:
        self.session = session
        self.error = error
        self.calls: list[tuple[str, float]] = []

    async def connect_exact(
        self,
        selector: str,
        *,
        scan_timeout_seconds: float,
    ) -> FakeBleSession:
        self.calls.append((selector, scan_timeout_seconds))
        if self.error is not None:
            raise self.error
        if self.session is None:
            raise AssertionError("test factory has no BLE session")
        return self.session


class BoundedBleWrapperTests(unittest.TestCase):
    def test_success_calls_real_async_verifier_and_returns_provenance(self) -> None:
        session = FakeBleSession(
            (message({"v": EXPECTED_VERSION}), *digest_messages(pending=False))
        )
        factory = FakeBleFactory(session)
        verified = runtime_verify.run_ble_runtime_bounded(
            ble_factory=factory,
            ble_selector="00:00:5E:00:53:AB",
            expected_version=EXPECTED_VERSION,
            expected_image_sha256=EXPECTED_DIGEST,
            timeout_seconds=1.0,
        )
        self.assertEqual(verified.version, EXPECTED_VERSION)
        self.assertEqual(verified.image_sha256, EXPECTED_DIGEST)
        self.assertEqual(factory.calls, [("00:00:5E:00:53:AB", 4.0)])
        self.assertTrue(session.closed)


class BleRuntimeVerificationTests(unittest.IsolatedAsyncioTestCase):
    async def test_trial_waits_for_explicit_valid_after_matching_digest(self) -> None:
        session = FakeBleSession((
            message({"v": EXPECTED_VERSION}), *digest_messages(),
            message({"ov": "p"}), message({"ov": "p"}), message({"ov": "v"}),
        ))
        with mock.patch.object(runtime_verify.asyncio, "sleep", new_callable=mock.AsyncMock):
            result = await runtime_verify.verify_ble_runtime(
                FakeBleFactory(session), selector="exact-board",
                expected_version=EXPECTED_VERSION, expected_image_sha256=EXPECTED_DIGEST,
                require_ota_accepted=True,
            )
        self.assertEqual(result.image_sha256, EXPECTED_DIGEST)
        self.assertEqual(session.sent, [
            {"cmd": "fw_version"}, {"cmd": "fw_digest"},
            {"cmd": "ota_state"}, {"cmd": "ota_state"}, {"cmd": "ota_state"},
        ])
        self.assertTrue(session.closed)

    async def test_trial_never_infers_acceptance_from_identity_or_elapsed_time(self) -> None:
        for replies in (
            [], [message({"ov": "p"})], [message({"e": "unknown_cmd"})],
            [message({"ov": "valid"})], [message({"ov": "v", "extra": 1})],
        ):
            session = FakeBleSession((
                message({"v": EXPECTED_VERSION}), *digest_messages(), *replies,
            ))
            with (
                self.subTest(replies=replies),
                mock.patch.object(runtime_verify.asyncio, "sleep", new_callable=mock.AsyncMock),
                self.assertRaises(runtime_verify.RuntimeVerificationError),
            ):
                await runtime_verify.verify_ble_runtime(
                    FakeBleFactory(session), selector="exact-board",
                    expected_version=EXPECTED_VERSION, expected_image_sha256=EXPECTED_DIGEST,
                    require_ota_accepted=True,
                )
            self.assertTrue(session.closed)

    async def test_verifies_exact_ble_version_and_running_image_digest(self) -> None:
        session = FakeBleSession(
            (message({"unrelated": True}), message({"v": EXPECTED_VERSION}), *digest_messages())
        )
        factory = FakeBleFactory(session)
        verified = await runtime_verify.verify_ble_runtime(
            factory,
            selector="00:00:5E:00:53:AB",
            expected_version=EXPECTED_VERSION,
            expected_image_sha256=EXPECTED_DIGEST,
        )
        self.assertEqual(verified.version, EXPECTED_VERSION)
        self.assertEqual(verified.image_sha256, EXPECTED_DIGEST)
        self.assertEqual(factory.calls, [("00:00:5E:00:53:AB", 4.0)])
        self.assertEqual(
            session.sent,
            [{"cmd": "fw_version"}, {"cmd": "fw_digest"}],
        )
        self.assertTrue(session.closed)

    async def test_success_fails_closed_when_session_teardown_fails(self) -> None:
        session = FakeBleSession(
            (message({"v": EXPECTED_VERSION}), *digest_messages(pending=False)),
            close_error=OSError("disconnect uncertain"),
        )
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "teardown failed",
        ):
            await runtime_verify.verify_ble_runtime(
                FakeBleFactory(session),
                selector="exact",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=EXPECTED_DIGEST,
            )
        self.assertTrue(session.closed)

    async def test_primary_verification_failure_survives_teardown_failure(self) -> None:
        session = FakeBleSession(
            (message({"v": "0.8"}),),
            close_error=OSError("disconnect uncertain"),
        )
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "does not match",
        ) as raised:
            await runtime_verify.verify_ble_runtime(
                FakeBleFactory(session),
                selector="exact",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=EXPECTED_DIGEST,
            )
        self.assertTrue(
            any("teardown failed" in note for note in raised.exception.__notes__)
        )

    async def test_cancellation_resistant_session_teardown_is_bounded_and_fails(
        self,
    ) -> None:
        class ResistantCloseSession(FakeBleSession):
            async def close(self) -> None:
                self.closed = True
                try:
                    await asyncio.Event().wait()
                except asyncio.CancelledError:
                    await asyncio.Event().wait()

        session = ResistantCloseSession(
            (message({"v": EXPECTED_VERSION}), *digest_messages(pending=False))
        )
        started = time.monotonic()
        with mock.patch.object(
            runtime_verify,
            "CLIENT_CLEANUP_TIMEOUT_SECONDS",
            0.01,
        ):
            with self.assertRaisesRegex(
                runtime_verify.RuntimeVerificationError,
                "teardown exceeded",
            ):
                await runtime_verify.verify_ble_runtime(
                    FakeBleFactory(session),
                    selector="exact",
                    expected_version=EXPECTED_VERSION,
                    expected_image_sha256=EXPECTED_DIGEST,
                    close_timeout_seconds=0.01,
                )
        self.assertLess(time.monotonic() - started, 0.2)
        self.assertTrue(session.closed)

    async def test_version_and_digest_mismatches_fail_and_close(self) -> None:
        sessions = (
            FakeBleSession((message({"v": "0.8"}),)),
            FakeBleSession(
                (message({"v": EXPECTED_VERSION}), *digest_messages(pending=False))
            ),
        )
        expected_digests = (EXPECTED_DIGEST, "89abcdef" * 8)
        for session, expected_digest in zip(sessions, expected_digests):
            with self.subTest(expected_digest=expected_digest):
                with self.assertRaises(runtime_verify.RuntimeVerificationError) as failure:
                    await runtime_verify.verify_ble_runtime(
                        FakeBleFactory(session),
                        selector="exact",
                        expected_version=EXPECTED_VERSION,
                        expected_image_sha256=expected_digest,
                    )
                if expected_digest != EXPECTED_DIGEST:
                    self.assertIn(f"observed={EXPECTED_DIGEST}", str(failure.exception))
                    self.assertIn(f"expected={expected_digest}", str(failure.exception))
                self.assertTrue(session.closed)

    async def test_timeout_protocol_error_and_send_error_fail_and_close(self) -> None:
        sessions = (
            FakeBleSession(()),
            FakeBleSession((message({"e": "unknown_cmd"}),)),
            FakeBleSession((), send_error_at=1),
        )
        for session in sessions:
            with self.subTest(session=session):
                with self.assertRaises(runtime_verify.RuntimeVerificationError):
                    await runtime_verify.verify_ble_runtime(
                        FakeBleFactory(session),
                        selector="exact",
                        expected_version=EXPECTED_VERSION,
                        expected_image_sha256=EXPECTED_DIGEST,
                    )
                self.assertTrue(session.closed)

    async def test_connection_and_request_send_are_explicitly_bounded(self) -> None:
        class HangingFactory:
            async def connect_exact(
                self,
                selector: str,
                *,
                scan_timeout_seconds: float,
            ) -> FakeBleSession:
                await asyncio.Event().wait()
                raise AssertionError("unreachable")

        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "timed out connecting",
        ):
            await runtime_verify.verify_ble_runtime(
                HangingFactory(),
                selector="exact",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=EXPECTED_DIGEST,
                connect_timeout_seconds=0.01,
            )

        class HangingSendSession(FakeBleSession):
            async def send_json(self, payload: Mapping[str, object]) -> None:
                await asyncio.Event().wait()

        session = HangingSendSession(())
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "timed out sending",
        ):
            await runtime_verify.verify_ble_runtime(
                FakeBleFactory(session),
                selector="exact",
                expected_version=EXPECTED_VERSION,
                expected_image_sha256=EXPECTED_DIGEST,
                request_timeout_seconds=0.01,
            )
        self.assertTrue(session.closed)

    async def test_combined_verification_sequences_serial_before_ble(self) -> None:
        serial = FakeSerialTransport((b"firmware: 0.7\n",))
        ble = FakeBleSession(
            (message({"v": EXPECTED_VERSION}), *digest_messages(pending=False))
        )
        result = await runtime_verify.verify_running_firmware(
            serial_factory=FakeSerialFactory(serial),
            ble_factory=FakeBleFactory(ble),
            serial_port="/dev/cu.exact-app",
            ble_selector="exact",
            expected_version=EXPECTED_VERSION,
            expected_image_sha256=EXPECTED_DIGEST,
        )
        self.assertEqual(result.serial_version, EXPECTED_VERSION)
        self.assertEqual(result.ble_version, EXPECTED_VERSION)
        self.assertEqual(result.image_sha256, EXPECTED_DIGEST)


class FakeAdvertisement:
    def __init__(self, services: Sequence[str]) -> None:
        self.service_uuids = tuple(services)


class FakeBleDevice:
    def __init__(self, address: str, name: str) -> None:
        self.address = address
        self.name = name
        self.metadata: dict[str, object] = {}


class FakeScanner:
    discovered: object = {}

    @classmethod
    async def discover(cls, *, timeout: float, return_adv: bool) -> object:
        if not return_adv:
            raise AssertionError("advertisement data is required")
        return cls.discovered


class FakeClient:
    instances: list["FakeClient"] = []

    def __init__(self, address: str) -> None:
        self.address = address
        self.connected = False
        self.disconnected = False
        self.notified = False
        self.__class__.instances.append(self)

    async def connect(self) -> None:
        self.connected = True

    async def start_notify(self, uuid: str, callback: object) -> None:
        self.notified = uuid == runtime_verify.OTA_CONTROL_CHARACTERISTIC_UUID

    async def stop_notify(self, uuid: str) -> None:
        self.notified = False

    async def disconnect(self) -> None:
        self.disconnected = True


class FakeBleakFactory(runtime_verify.BleakControlFactory):
    @staticmethod
    def _load_bleak() -> tuple[object, object, type[Exception]]:
        return FakeClient, FakeScanner, Exception


class BleakExactSelectionTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        FakeClient.instances.clear()

    async def test_exact_address_selection_connects_only_service_advertiser(self) -> None:
        selected = FakeBleDevice("ABC", "Litch")
        other = FakeBleDevice("DEF", "Other")
        FakeScanner.discovered = {
            "one": (selected, FakeAdvertisement((runtime_verify.BASE_SERVICE_UUID,))),
            "two": (other, FakeAdvertisement((runtime_verify.BASE_SERVICE_UUID,))),
        }
        session = await FakeBleakFactory().connect_exact(
            "abc",
            scan_timeout_seconds=1.0,
        )
        self.assertEqual(FakeClient.instances[-1].address, "ABC")
        self.assertTrue(FakeClient.instances[-1].connected)
        await session.close()
        self.assertTrue(FakeClient.instances[-1].disconnected)

    async def test_no_only_board_or_unique_name_fallback(self) -> None:
        first = FakeBleDevice("ABC", "Litch")
        second = FakeBleDevice("DEF", "Litch")
        advertisement = FakeAdvertisement((runtime_verify.BASE_SERVICE_UUID,))

        FakeScanner.discovered = {"one": (first, advertisement)}
        with self.assertRaises(runtime_verify.RuntimeVerificationError):
            await FakeBleakFactory().connect_exact(
                "not-the-board",
                scan_timeout_seconds=1.0,
            )

        FakeScanner.discovered = {"one": (first, advertisement)}
        with self.assertRaisesRegex(runtime_verify.RuntimeVerificationError, "address/UUID"):
            await FakeBleakFactory().connect_exact(
                "Litch",
                scan_timeout_seconds=1.0,
            )

    async def test_connect_or_notify_cancellation_disconnects_client(self) -> None:
        advertisement = FakeAdvertisement((runtime_verify.BASE_SERVICE_UUID,))
        FakeScanner.discovered = {
            "one": (FakeBleDevice("ABC", "Litch"), advertisement)
        }

        for blocked_phase in ("connect", "notify"):
            with self.subTest(blocked_phase=blocked_phase):
                started = asyncio.Event()
                release = asyncio.Event()

                class BlockingClient(FakeClient):
                    instances: list["BlockingClient"] = []

                    async def connect(self) -> None:
                        if blocked_phase == "connect":
                            started.set()
                            await release.wait()
                        self.connected = True

                    async def start_notify(self, uuid: str, callback: object) -> None:
                        if blocked_phase == "notify":
                            started.set()
                            await release.wait()
                        await super().start_notify(uuid, callback)

                class BlockingFactory(runtime_verify.BleakControlFactory):
                    @staticmethod
                    def _load_bleak() -> tuple[object, object, type[Exception]]:
                        return BlockingClient, FakeScanner, Exception

                task = asyncio.create_task(
                    BlockingFactory().connect_exact("ABC", scan_timeout_seconds=1.0)
                )
                await asyncio.wait_for(started.wait(), timeout=1.0)
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task
                self.assertTrue(BlockingClient.instances[-1].disconnected)

    async def test_session_close_disconnects_when_stop_notify_is_cancelled(self) -> None:
        stop_started = asyncio.Event()
        never_release = asyncio.Event()

        class BlockingCloseClient(FakeClient):
            async def stop_notify(self, uuid: str) -> None:
                stop_started.set()
                await never_release.wait()

        raw = BlockingCloseClient("ABC")
        session = runtime_verify._BleakControlSession(raw)
        session._subscribed = True
        task = asyncio.create_task(session.close())
        await asyncio.wait_for(stop_started.wait(), timeout=1.0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(raw.disconnected)

    async def test_session_close_reports_disconnect_failure(self) -> None:
        class FailedDisconnectClient(FakeClient):
            async def disconnect(self) -> None:
                raise OSError("link state unknown")

        raw = FailedDisconnectClient("ABC")
        session = runtime_verify._BleakControlSession(raw)
        with self.assertRaisesRegex(
            runtime_verify.RuntimeVerificationError,
            "disconnect failed",
        ):
            await session.close()
        self.assertFalse(raw.disconnected)

    async def test_disconnect_cleanup_is_bounded_if_first_cancel_is_suppressed(self) -> None:
        first_cancel = asyncio.Event()
        never_release = asyncio.Event()

        class ResistantClient:
            async def disconnect(self) -> None:
                try:
                    await never_release.wait()
                except asyncio.CancelledError:
                    first_cancel.set()
                    await never_release.wait()

        with mock.patch.object(
            runtime_verify,
            "CLIENT_CLEANUP_TIMEOUT_SECONDS",
            0.01,
        ):
            with self.assertRaisesRegex(
                runtime_verify.RuntimeVerificationError,
                "bounded teardown deadline",
            ):
                await asyncio.wait_for(
                    runtime_verify._disconnect_client_bounded(ResistantClient()),
                    timeout=0.1,
                )
        self.assertTrue(first_cancel.is_set())


class OwnedLoopBoundTests(unittest.TestCase):
    def test_fully_cancellation_resistant_disconnect_cannot_hang_loop_teardown(
        self,
    ) -> None:
        cancellations: list[None] = []

        class ResistantClient:
            async def disconnect(self) -> None:
                while True:
                    try:
                        await asyncio.sleep(3600)
                    except asyncio.CancelledError:
                        cancellations.append(None)

        async def workflow() -> None:
            await runtime_verify._disconnect_client_bounded(ResistantClient())

        started = time.monotonic()
        with mock.patch.object(
            runtime_verify,
            "CLIENT_CLEANUP_TIMEOUT_SECONDS",
            0.01,
        ):
            with self.assertRaisesRegex(
                runtime_verify.RuntimeVerificationError,
                "bounded teardown deadline",
            ):
                runtime_verify._run_coroutine_in_owned_loop(
                    workflow(),
                    timeout_seconds=0.1,
                    cleanup_timeout_seconds=0.01,
                    label="test workflow",
                )
        self.assertLess(time.monotonic() - started, 0.25)
        self.assertTrue(cancellations)


if __name__ == "__main__":
    unittest.main()
