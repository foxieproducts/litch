from __future__ import annotations

import asyncio
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
import zlib
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools import ble_update_firmware as updater


class ReplayGattClient:
    """Replay receiver notifications through the production session ingress."""

    mtu_size = 247

    def __init__(self, *, failure=None, final_ack=True, terminal="done", abort_error=False, info=None, probe=None, begin=None):
        self.failure = failure
        self.info = info if info is not None else {"i": [3, 4, 16]}
        self.probe = probe
        self.begin = begin
        self.final_ack = final_ack
        self.terminal = terminal
        self.abort_error = abort_error
        self.callbacks = {}
        self.controls = []
        self.sent = bytearray()
        self.acked = 0
        self.total = 40
        self.end_acked = None
        self.abort_started = asyncio.Event()
        self.abort_release = asyncio.Event()
        self.services = [SimpleNamespace(
            uuid=updater.BASE_SERVICE_UUID,
            characteristics=[SimpleNamespace(uuid=uuid) for uuid in (
                updater.COMMAND_CHARACTERISTIC_UUID,
                updater.CONSOLE_STREAM_CHARACTERISTIC_UUID,
                updater.OTA_CONTROL_CHARACTERISTIC_UUID,
                updater.OTA_DATA_CHARACTERISTIC_UUID,
            )],
        )]

    async def start_notify(self, characteristic, callback):
        self.callbacks[characteristic.uuid] = callback

    def notify(self, payload):
        self.callbacks[updater.OTA_CONTROL_CHARACTERISTIC_UUID](
            None, bytearray(json.dumps(payload).encode()),
        )

    async def write_gatt_char(self, characteristic, data, *, response):
        if characteristic.uuid == updater.OTA_DATA_CHARACTERISTIC_UUID:
            if self.failure == "cancel":
                raise asyncio.CancelledError("cancelled during data write")
            if self.failure == "write":
                raise OSError("data write refused")
            if self.failure == "disconnect":
                self.disconnect()
                return
            self.sent.extend(data)
            if self.final_ack == "stall_window":
                pass
            elif len(self.sent) < self.total or self.final_ack is True:
                self.acknowledge()
            elif self.final_ack == "delayed":
                asyncio.get_running_loop().call_later(0.005, self.acknowledge)
            elif self.final_ack == "delayed_compact":
                asyncio.get_running_loop().call_later(0.005, self.acknowledge)
            elif self.final_ack == "early_done":
                self.notify({"st": "done"})
            elif self.final_ack == "percent_only":
                self.notify({"p": 100})
            return

        command = json.loads(data)
        self.controls.append(command)
        name = command["cmd"]
        if name == "info":
            self.notify(self.info)
        elif name == "ota_probe":
            self.notify(self.probe if self.probe is not None else {
                "st": "probe", "proto": 2, "mode": command["mode"], "size": command["size"],
                "max_chunk": 244, "chunk": 244, "burst": 48, "window": 11712, "supports_sim": 1,
            })
        elif name in {"ota_begin", "ota_sim_begin"}:
            self.total = command["size"]
            if self.failure == "begin_send":
                raise OSError("begin write refused")
            if self.failure != "begin_ack":
                self.notify(self.begin if self.begin is not None else {
                    "st": "begin", "proto": 2, "sim": int(name == "ota_sim_begin"), "rx": 0, "total": command["size"],
                    "chunk": command["chunk"], "burst": command["burst"], "window": command["window"],
                })
        elif name == "ota_end":
            self.end_acked = self.acked
            if self.failure == "cancel_end":
                raise asyncio.CancelledError("cancelled during End")
            if self.failure == "finalize":
                raise OSError("end write refused")
            if self.terminal == "disconnected":
                self.disconnect()
            else:
                self.notify({"st": self.terminal})
        elif name == "ota_abort":
            self.abort_started.set()
            if self.abort_error == "hang":
                while not self.abort_release.is_set():
                    try:
                        await self.abort_release.wait()
                    except asyncio.CancelledError:
                        pass
            elif self.abort_error:
                raise OSError("abort transport unavailable")

    def acknowledge(self):
        self.acked = len(self.sent)
        if self.final_ack == "delayed_compact" and self.acked == self.total:
            self.notify({"r": self.acked})
        else:
            self.notify({"st": "progress", "rx": self.acked, "total": self.total})


class SessionAcquisitionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.client = ReplayGattClient()
        self.client.connected = False
        self.client.disconnect_calls = 0

        async def connect():
            self.client.connected = True

        async def disconnect():
            self.client.disconnect_calls += 1
            self.client.connected = False
            self.client.callbacks.clear()

        self.client.connect = connect
        self.client.disconnect = disconnect
        self.candidate = updater.CandidateDevice("Litch", "exact-board", -40, (), "test")

        class BleakFailure(Exception):
            pass

        self.bleak_failure = BleakFailure
        self.client_factory = mock.Mock(return_value=self.client)
        patcher = mock.patch.object(
            updater, "require_bleak", return_value=(self.client_factory, None, BleakFailure),
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def assert_released(self):
        self.assertEqual(self.client.disconnect_calls, 1)
        self.assertFalse(self.client.connected)
        self.assertFalse(self.client.callbacks)

    async def test_success_transfers_live_client_to_session(self):
        session = await updater.connect_session(self.candidate)
        self.assertIs(session._client, self.client)
        self.assertTrue(self.client.connected)
        self.assertEqual(self.client.disconnect_calls, 0)
        self.assertEqual(len(self.client.callbacks), 2)
        self.client_factory.call_args.kwargs["disconnected_callback"](self.client)
        self.assertTrue(session._disconnect_event.is_set())

    async def test_connect_error_releases_client_and_retains_cli_message(self):
        failure = self.bleak_failure("link setup failed")

        async def fail():
            self.client.connected = True
            raise failure

        self.client.connect = fail
        with self.assertRaises(SystemExit) as caught:
            await updater.connect_session(self.candidate)
        self.assertIn("Failed to connect to", str(caught.exception))
        self.assertIs(caught.exception.__cause__, failure)
        self.assert_released()

    async def test_session_construction_failure_releases_client(self):
        self.client.services = []
        with self.assertRaisesRegex(RuntimeError, "Characteristic .* was not found"):
            await updater.connect_session(self.candidate)
        self.assert_released()

    async def test_second_subscription_failure_releases_partial_native_state(self):
        failure = OSError("second subscription failed")
        subscribe = self.client.start_notify

        async def fail_second(characteristic, callback):
            if self.client.callbacks:
                raise failure
            await subscribe(characteristic, callback)

        self.client.start_notify = fail_second
        with self.assertRaises(OSError) as caught:
            await updater.connect_session(self.candidate)
        self.assertIs(caught.exception, failure)
        self.assert_released()

    async def test_cancellation_during_connect_releases_client(self):
        started = asyncio.Event()

        async def wait():
            self.client.connected = True
            started.set()
            await asyncio.Event().wait()

        self.client.connect = wait
        task = asyncio.create_task(updater.connect_session(self.candidate))
        await started.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assert_released()

    async def test_cancellation_during_second_subscription_releases_partial_state(self):
        started = asyncio.Event()
        subscribe = self.client.start_notify

        async def wait_second(characteristic, callback):
            if self.client.callbacks:
                started.set()
                await asyncio.Event().wait()
            await subscribe(characteristic, callback)

        self.client.start_notify = wait_second
        task = asyncio.create_task(updater.connect_session(self.candidate))
        await started.wait()
        self.assertEqual(len(self.client.callbacks), 1)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assert_released()

    async def test_cleanup_failure_keeps_primary_error_and_records_uncertainty(self):
        failure = RuntimeError("primary setup failure")

        async def fail_connect():
            raise failure

        async def fail_disconnect():
            self.client.disconnect_calls += 1
            raise OSError("cleanup failed")

        self.client.connect = fail_connect
        self.client.disconnect = fail_disconnect
        with self.assertRaises(RuntimeError) as caught:
            await updater.connect_session(self.candidate)
        self.assertIs(caught.exception, failure)
        self.assertEqual(self.client.disconnect_calls, 1)
        self.assertIn("cleanup failed", " ".join(caught.exception.__notes__))

    async def test_cleanup_timeout_keeps_primary_and_does_not_retry(self):
        failure = RuntimeError("primary setup failure")
        canceled = asyncio.Event()

        async def fail_connect():
            raise failure

        async def blocked_disconnect():
            self.client.disconnect_calls += 1
            try:
                await asyncio.Event().wait()
            finally:
                canceled.set()

        self.client.connect = fail_connect
        self.client.disconnect = blocked_disconnect
        with mock.patch.object(updater, "CONNECT_CLEANUP_TIMEOUT_SECONDS", 0.01):
            with self.assertRaises(RuntimeError) as caught:
                await updater.connect_session(self.candidate)
        self.assertIs(caught.exception, failure)
        self.assertEqual(self.client.disconnect_calls, 1)
        self.assertTrue(canceled.is_set())
        self.assertIn("TimeoutError", " ".join(caught.exception.__notes__))


class FirmwareTransferReplayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.firmware = Path(self.temporary.name) / "firmware.bin"
        self.firmware.write_bytes(b"firmware" * 5)
        self.config = updater.TransferConfig(
            chunk_size=20, mode="auto", burst_limit=2, begin_attempts=1,
            retry_delay_seconds=0, pre_begin_delay_seconds=0,
            final_settle_seconds=0, terminal_timeout_seconds=0.03,
        )

    async def replay(self, client):
        session = updater.LitchBleSession(
            client, updater.CandidateDevice("Litch", "exact-board", -40, (), "test"),
        )
        client.session = session
        client.disconnect = session._disconnect_event.set
        await session.start_notifications()
        real_wait = session.wait_for_ota_event
        real_wait_rx = session.wait_for_board_rx
        client.compact_receipt_matches = []

        async def bounded_wait(**kwargs):
            # Scale only notification deadlines; retain actual queue, status
            # decoding, cumulative RX tracking and transfer orchestration.
            kwargs["timeout_seconds"] = min(kwargs["timeout_seconds"], 0.03)
            predicate = kwargs.get("predicate")

            def observe_receipt(event):
                matched = predicate is None or predicate(event)
                if isinstance(event.get("raw"), dict) and "r" in event["raw"]:
                    client.compact_receipt_matches.append(matched)
                return matched

            kwargs["predicate"] = observe_receipt
            return await real_wait(**kwargs)

        async def bounded_wait_rx(expected_rx, *, timeout_seconds):
            return await real_wait_rx(expected_rx, timeout_seconds=min(timeout_seconds, 0.03))

        session.wait_for_ota_event = bounded_wait
        session.wait_for_board_rx = bounded_wait_rx
        with (
            mock.patch.object(session, "write_ota_control_json", wraps=session.write_ota_control_json) as control,
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            try:
                result = await session.upload_file(self.firmware, self.config)
            finally:
                client.abort_calls = sum(
                    call.args[0].get("cmd") == "ota_abort" for call in control.call_args_list
                )
                self.assertIsNone(session._active_transfer_profile)
        return result

    async def test_incompatible_or_incomplete_profile_never_begins(self):
        for mode in ("auto", "fast", "reliable"):
            self.config = replace(self.config, mode=mode)
            for info, probe in (({"i": [2, 4, 16]}, None), ({"i": [3, 4, 0]}, None),
                                ({"i": [3, 4, 16]}, {"proto": 2})):
                with self.subTest(mode=mode, info=info, probe=probe):
                    client = ReplayGattClient(info=info, probe=probe)
                    with self.assertRaisesRegex(RuntimeError, "could not be verified"):
                        await self.replay(client)
                    self.assertFalse(client.sent)
                    self.assertFalse(any(item["cmd"] == "ota_begin" for item in client.controls))

    async def test_receive_window_stall_stops_before_overrun_and_end(self):
        self.firmware.write_bytes(b"x" * 1000)
        client = ReplayGattClient(final_ack="stall_window")
        with self.assertRaisesRegex(RuntimeError, "receive window"):
            await self.replay(client)
        self.assertEqual(len(client.sent), 960)
        self.assertIsNone(client.end_acked)
        self.assertEqual(client.abort_calls, 1)

    async def test_simulated_upload_uses_current_profile_and_final_ack(self):
        client = ReplayGattClient()
        session = updater.LitchBleSession(
            client, updater.CandidateDevice("Litch", "exact-board", -40, (), "test"),
        )
        client.session = session
        await session.start_notifications()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = await session.run_simulated_upload_benchmark(40, self.config)
        self.assertEqual(result["terminal_state"], "done")
        self.assertEqual(client.end_acked, 40)
        self.assertEqual([item["cmd"] for item in client.controls],
                         ["info", "ota_probe", "ota_sim_begin", "ota_end"])

    async def test_incomplete_begin_never_sends_image_data(self):
        client = ReplayGattClient(begin={"b": 1})
        with self.assertRaisesRegex(RuntimeError, "begin protocol/window verification failed"):
            await self.replay(client)
        self.assertFalse(client.sent)
        self.assertEqual(client.abort_calls, 1)

    def test_legacy_flag_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            updater.build_parser().parse_args(["update", "--legacy"])

    async def test_auto_fast_requires_final_receiver_ack_before_end(self):
        client = ReplayGattClient(final_ack="delayed")
        result = await self.replay(client)
        self.assertEqual(result["terminal_state"], "done")
        self.assertEqual(client.end_acked, 40)
        self.assertEqual(client.abort_calls, 0)
        end = next(command for command in client.controls if command["cmd"] == "ota_end")
        self.assertEqual(end, {
            "cmd": "ota_end", "size": 40, "crc32": f"{zlib.crc32(b'firmware' * 5):08x}",
        })

    async def test_compact_final_ack_wakes_rx_wait_before_deadline(self):
        client = ReplayGattClient(final_ack="delayed_compact")
        result = await self.replay(client)
        # Observe the actual queued notification passing the wait predicate;
        # a timeout snapshot that happens to see the updated RX cannot pass.
        self.assertEqual(client.compact_receipt_matches, [True])
        self.assertEqual(client.end_acked, 40)
        self.assertEqual(result["terminal_state"], "done")
        self.assertEqual(client.abort_calls, 0)

    async def test_lost_final_ack_never_sends_end_and_aborts_once(self):
        for final_ack in (False, "early_done", "percent_only"):
            with self.subTest(final_ack=final_ack):
                client = ReplayGattClient(final_ack=final_ack)
                with self.assertRaisesRegex(RuntimeError, "only acknowledged 20/40"):
                    await self.replay(client)
                self.assertIsNone(client.end_acked)
                self.assertEqual(client.abort_calls, 1)

    async def test_each_failed_transfer_phase_aborts_once(self):
        for failure, terminal, error in (
            ("begin_send", "done", "failed to send begin"),
            ("begin_ack", "done", "begin confirmation"),
            ("write", "done", "data write failed"),
            ("disconnect", "done", "board failed before ota_end"),
        ):
            with self.subTest(failure=failure, terminal=terminal):
                client = ReplayGattClient(failure=failure, terminal=terminal)
                with self.assertRaisesRegex(RuntimeError, error):
                    await self.replay(client)
                self.assertEqual(client.abort_calls, 1)
                if failure in {"begin_send", "begin_ack"}:
                    self.assertEqual(client.sent, b"")

    async def test_cancellation_preserves_primary_error_even_if_abort_fails(self):
        for abort_error in (False, True):
            with self.subTest(abort_error=abort_error):
                client = ReplayGattClient(failure="cancel", abort_error=abort_error)
                with self.assertRaisesRegex(asyncio.CancelledError, "cancelled during data write"):
                    await self.replay(client)
                self.assertEqual(client.abort_calls, 1)
                self.assertIsNone(client.end_acked)

    async def test_stuck_abort_is_bounded_and_quarantines_old_session(self):
        for cancel_cleanup in (False, True):
            with self.subTest(cancel_cleanup=cancel_cleanup):
                client = ReplayGattClient(failure="cancel", abort_error="hang")
                with mock.patch.object(updater, "DEFAULT_TIMEOUT_SECONDS", 0.01):
                    transfer = asyncio.create_task(self.replay(client))
                    await client.abort_started.wait()
                    if cancel_cleanup:
                        transfer.cancel("second cancellation during cleanup")
                    try:
                        done, _ = await asyncio.wait({transfer}, timeout=0.1)
                        self.assertIn(transfer, done, "native abort must not hold transfer completion")
                        with self.assertRaisesRegex(asyncio.CancelledError, "cancelled during data write"):
                            await transfer
                        self.assertFalse(client.session._ota_abort_task.done())
                        controls_before_retry = list(client.controls)
                        with self.assertRaisesRegex(RuntimeError, "abort.*pending"):
                            await client.session.upload_file(self.firmware, self.config)
                        self.assertEqual(client.controls, controls_before_retry)
                        self.assertEqual(client.abort_calls, 1)
                    finally:
                        client.abort_release.set()
                        await asyncio.gather(transfer, return_exceptions=True)
                        abort_task = getattr(client.session, "_ota_abort_task", None)
                        if abort_task is not None:
                            await asyncio.gather(abort_task, return_exceptions=True)
                self.assertEqual(sum(command["cmd"] == "ota_abort" for command in client.controls), 1)

    async def test_uncertain_end_never_replays_or_aborts(self):
        for failure, terminal in (("finalize", "done"), (None, "timeout"), (None, "disconnected")):
            with self.subTest(failure=failure, terminal=terminal):
                client = ReplayGattClient(failure=failure, terminal=terminal)
                result = await self.replay(client)
                self.assertEqual(result["terminal_state"], "pending_verification")
                self.assertEqual(client.end_acked, 40)
                self.assertEqual(sum(item["cmd"] == "ota_end" for item in client.controls), 1)
                self.assertEqual(client.abort_calls, 0)

    async def test_end_cancellation_does_not_replay_or_abort(self):
        client = ReplayGattClient(failure="cancel_end")
        with self.assertRaisesRegex(asyncio.CancelledError, "cancelled during End"):
            await self.replay(client)
        self.assertEqual(sum(item["cmd"] == "ota_end" for item in client.controls), 1)
        self.assertEqual(client.abort_calls, 0)

    async def test_uncertain_simulation_stops_without_replay(self):
        client = ReplayGattClient(failure="finalize")
        session = updater.LitchBleSession(
            client, updater.CandidateDevice("Litch", "exact-board", -40, (), "test"),
        )
        await session.start_notifications()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(updater.OtaOutcomeUnknown, "no further benchmark"):
                await session.run_simulated_upload_benchmark(40, self.config)
        self.assertEqual(sum(item["cmd"] == "ota_end" for item in client.controls), 1)
        self.assertFalse(any(item["cmd"] == "ota_abort" for item in client.controls))

    async def test_explicit_post_end_error_does_not_abort(self):
        client = ReplayGattClient(terminal="error")
        with self.assertRaisesRegex(RuntimeError, "terminal state error"):
            await self.replay(client)
        self.assertEqual(sum(item["cmd"] == "ota_end" for item in client.controls), 1)
        self.assertEqual(client.abort_calls, 0)

    async def test_owner_required_probe_refusal_explains_next_action(self):
        client = ReplayGattClient(info={"i": [3, 4, 0x210]}, probe={"e": "access_denied"})
        with self.assertRaisesRegex(RuntimeError, "owner admission.*companion app or USB"):
            await self.replay(client)
        self.assertFalse(client.sent)
        self.assertFalse(any(item["cmd"] == "ota_begin" for item in client.controls))


class StandaloneUpdateRunnerTests(unittest.TestCase):
    def test_primary_failure_survives_cancellation_resistant_abort_at_shutdown(self):
        script = f"""
import asyncio
import sys

repo_root = {str(updater.REPO_ROOT)!r}
sys.path.insert(0, repo_root)
from tools import ble_update_firmware as updater
from tools.dev.tests.test_ble_update_firmware import ReplayGattClient

async def failing_update(args):
    client = ReplayGattClient(abort_error="hang")
    session = updater.LitchBleSession(
        client, updater.CandidateDevice("Litch", "exact-board", -40, (), "test"),
    )
    await session._abort_ota_best_effort()
    assert not session._ota_abort_task.done()
    raise RuntimeError("original update failure")

updater.async_main = failing_update
updater.DEFAULT_TIMEOUT_SECONDS = 0.01
updater.UPDATE_TIMEOUT_SECONDS = 0.1
sys.argv = ["ble_update_firmware.py", "update"]
# Exercise the direct-script import path, where only tools/ is normally on
# sys.path before the update runner loads its existing bounded-loop owner.
sys.path.remove(repo_root)
try:
    updater.main()
except RuntimeError as error:
    assert str(error) == "original update failure", str(error)
    print("PRIMARY_FAILURE_RETURNED")
else:
    raise AssertionError("update should fail")
"""
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run(
                [sys.executable, "-c", script], cwd=directory,
                capture_output=True, text=True, timeout=2,
            )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn("PRIMARY_FAILURE_RETURNED", completed.stdout)
