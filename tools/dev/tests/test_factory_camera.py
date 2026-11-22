from __future__ import annotations

import io
import stat
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

from tools.dev import factory_camera
from tools.dev.factory_camera import CameraChoice, CameraReading, CameraUpdate, FactoryCamera
from tools.dev.runner import DevError


_LABEL_LINE = b'{"state":"label","payload":"sample_0034","symbology":"QR"}\n'
_NEXT_LABEL_LINE = b'{"state":"label","payload":"sample_0035","symbology":"QR"}\n'
_CLEAR_LINE = b'{"state":"clear"}\n'
_AMBIGUOUS_LINE = b'{"state":"ambiguous"}\n'
_ANDROID = CameraChoice("android", "TESTANDROID01", "Galaxy A26 rear camera 0", "0")
_NATIVE = CameraChoice("native", "exact-native-id", "MacBook Pro Camera (Built-in camera)")


class FactoryCameraTests(unittest.TestCase):
    def running_camera(self) -> FactoryCamera:
        camera = FactoryCamera(choice=_NATIVE)
        child = mock.Mock()
        child.poll.return_value = None
        camera._children = {"factory camera preview": child}
        return camera

    def test_stable_label_ignores_frame_jitter_and_can_rearm_after_clear_view(self) -> None:
        label = CameraReading("label", "sample_0034", "QR")
        clear = CameraReading("clear")
        ambiguous = CameraReading("ambiguous")
        debounce = factory_camera._ReadingDebounce()
        sequence = [label, clear, label, label, label, clear, label, ambiguous,
                    clear, clear, clear, label, label]
        changes = [change for reading in sequence if (change := debounce.observe(reading))]
        self.assertEqual(changes, [label, ambiguous, clear, label])

    def test_held_label_can_be_polled_again_after_the_latest_frame_matches(self) -> None:
        camera = self.running_camera()
        label = CameraReading("label", "sample_0034", "QR", generation=1)

        camera._read(io.BytesIO(_LABEL_LINE * 2 + _CLEAR_LINE))
        self.assertEqual(camera.poll(), CameraUpdate((label,)))
        camera._read(io.BytesIO(_LABEL_LINE * 2))
        # A station gate can open after the initial transition was consumed.
        self.assertEqual(camera.poll(), CameraUpdate(current=label))
        self.assertEqual(camera.poll(), CameraUpdate(current=label))

        # A running preview can stop sending frames, including while the
        # station is busy. Discard both the held label and queued old scans.
        with mock.patch.object(factory_camera.time, "monotonic", return_value=camera._last_frame) as clock:
            camera._read(io.BytesIO(_NEXT_LABEL_LINE * 2))
            clock.return_value += factory_camera._FRAME_TIMEOUT_SECONDS + 1
            gap = camera.poll()
            self.assertTrue(gap.stalled)
            self.assertEqual([reading.state for reading in gap.transitions], ["stalled"])
            self.assertIsNone(gap.current)
            self.assertEqual(camera.poll(), CameraUpdate(stalled=True))
            camera._read(io.BytesIO(_LABEL_LINE))
            self.assertEqual(camera.poll(), CameraUpdate())
            camera._read(io.BytesIO(_LABEL_LINE))
            self.assertEqual(camera.poll().current.payload, label.payload)
            camera._read(io.BytesIO(_NEXT_LABEL_LINE * 2))
            clock.return_value += factory_camera._FRAME_TIMEOUT_SECONDS + 1
            camera._read(io.BytesIO(_CLEAR_LINE * 3 + _LABEL_LINE * 2))
            resumed = camera.poll()
            self.assertFalse(resumed.stalled)
            self.assertEqual([reading.state for reading in resumed.transitions], ["stalled", "clear", "label"])
            self.assertEqual(resumed.current.payload, label.payload)

        camera._read(io.BytesIO(b"{" * 1025 + b"\n"))
        with self.assertRaisesRegex(DevError, "oversized or truncated"):
            camera.poll()

    def test_ambiguity_clear_and_both_label_changes_survive_a_station_pause(self) -> None:
        camera = self.running_camera()
        first = CameraReading("label", "sample_0034", "QR", generation=1)
        camera._read(io.BytesIO(_LABEL_LINE * 2))
        self.assertEqual(camera.poll(), CameraUpdate((first,), first))

        camera._read(io.BytesIO(_AMBIGUOUS_LINE + _CLEAR_LINE * 3 + _NEXT_LABEL_LINE * 2 + _LABEL_LINE * 2))
        same_label_again = CameraReading("label", "sample_0034", "QR", generation=5)
        self.assertEqual(camera.poll(), CameraUpdate((
            CameraReading("ambiguous", generation=2),
            CameraReading("clear", generation=3),
            CameraReading("label", "sample_0035", "QR", generation=4),
            same_label_again,
        ), same_label_again))
        self.assertEqual(camera.poll(), CameraUpdate(current=same_label_again))

    def test_transition_overflow_reports_reset_and_discards_pre_reset_label(self) -> None:
        camera = self.running_camera()
        capacity = factory_camera._MAX_PENDING_TRANSITIONS
        too_many_changes = (_LABEL_LINE * 2 + _NEXT_LABEL_LINE * 2) * capacity
        camera._read(io.BytesIO(too_many_changes + _CLEAR_LINE * 3 + _LABEL_LINE * 2))
        self.assertEqual(camera.poll(), CameraUpdate((CameraReading("reset", generation=capacity + 1),)))
        self.assertEqual(camera.poll(), CameraUpdate())

        camera._read(io.BytesIO(_CLEAR_LINE * 3 + _NEXT_LABEL_LINE * 2))
        fresh_label = CameraReading("label", "sample_0035", "QR", generation=capacity + 3)
        self.assertEqual(camera.poll(), CameraUpdate((
            CameraReading("clear", generation=capacity + 2), fresh_label,
        ), fresh_label))

    def test_camera_launch_uses_exact_phone_and_limited_pipe_and_cleans_up(self) -> None:
        ffmpeg = mock.Mock()
        ffmpeg.poll.return_value = None
        window = mock.Mock()
        window.poll.return_value = None
        scrcpy = mock.Mock()
        scrcpy.poll.return_value = None
        adb = "/sdk/platform-tools/adb"
        inherited = {"ADB": "/previous/adb", "FACTORY_CAMERA_TEST": "kept"}
        with (
            mock.patch.object(factory_camera.sys, "platform", "darwin"),
            mock.patch.dict(factory_camera.os.environ, inherited, clear=True),
            mock.patch.object(factory_camera.shutil, "which", return_value=adb) as which,
            mock.patch.object(factory_camera, "_require_command", side_effect=lambda command: command),
            mock.patch.object(factory_camera, "_build_preview", return_value=Path("preview")),
            mock.patch.object(factory_camera.subprocess, "Popen", side_effect=[ffmpeg, window, scrcpy]) as launch,
        ):
            ffmpeg.stdout = io.BytesIO()
            window.stdout = io.BytesIO(_LABEL_LINE * 2)
            scrcpy.stdout = None
            with FactoryCamera(choice=replace(_ANDROID, adb_executable=adb)) as camera:
                self.assertIsNotNone(camera._reader)
                camera._reader.join(timeout=1)
                label = CameraReading("label", "sample_0034", "QR", generation=1)
                self.assertEqual(camera.poll(), CameraUpdate((label,), label))
                decode, preview, capture = [call.args[0] for call in launch.call_args_list]
                self.assertEqual(preview, ["preview", "1920", "1440", _ANDROID.label])
                self.assertEqual(capture[:6], ["scrcpy", "--serial", "TESTANDROID01", "--video-source=camera",
                                                "--camera-id=0", "--no-audio"])
                self.assertIn("--no-control", capture)
                self.assertIn("--no-window", capture)
                self.assertIn("--verbosity=warn", capture)
                self.assertIn("--record-format=mkv", capture)
                which.assert_called_once_with(adb)
                self.assertIsNone(launch.call_args_list[0].kwargs["env"])
                self.assertIsNone(launch.call_args_list[1].kwargs["env"])
                self.assertEqual(launch.call_args_list[2].kwargs["env"], dict(inherited, ADB=adb))
                self.assertEqual(dict(factory_camera.os.environ), inherited)
                fifo = Path(capture[-1].removeprefix("--record="))
                self.assertEqual(fifo.name, "camera-stream")
                self.assertEqual(decode[decode.index("-i") + 1], str(fifo))
                filters = decode[decode.index("-vf") + 1].split(",")
                self.assertEqual(filters[:3], ["fps=6", "crop=iw/2:ih/2",
                                               "scale=1920:1440:force_original_aspect_ratio=decrease"])
                self.assertTrue(stat.S_ISFIFO(fifo.stat().st_mode))
            self.assertFalse(fifo.parent.exists())
            for process in (ffmpeg, window, scrcpy):
                process.terminate.assert_called_once()
                process.wait.assert_called_once()
            camera.close()

    def test_missing_or_non_usb_selector_never_uses_default_camera(self) -> None:
        for selector in ("", "-d", "192.0.2.1:5555"):
            with self.subTest(selector=selector), self.assertRaisesRegex(DevError, "exact Android USB serial"):
                FactoryCamera(choice=replace(_ANDROID, device_id=selector))
        with self.assertRaisesRegex(DevError, "exact Android USB serial and rear camera ID"):
            FactoryCamera(choice=replace(_ANDROID, camera_id=None))

    def test_adb_uses_valid_override_then_path_and_missing_fails_before_launch(self) -> None:
        configured = "/configured/adb"
        on_path = "/usr/bin/adb"
        executables = {configured: configured, "adb": on_path}
        with (
            mock.patch.object(factory_camera.sys, "platform", "darwin"),
            mock.patch.dict(factory_camera.os.environ, {"ADB": configured}, clear=True),
            mock.patch.object(factory_camera.shutil, "which", side_effect=executables.get),
            mock.patch.object(factory_camera, "_require_command", side_effect=lambda command: command),
            mock.patch.object(factory_camera, "_build_preview") as build,
        ):
            self.assertEqual(factory_camera._resolve_adb(None), configured)
            del executables[configured]
            self.assertEqual(factory_camera._resolve_adb(None), on_path)
            with self.assertRaisesRegex(DevError, "selected adb"):
                with FactoryCamera(choice=replace(_ANDROID, adb_executable=configured)):
                    self.fail("invalid explicit adb must not fall back to PATH")
            del executables["adb"]
            with self.assertRaisesRegex(DevError, "Install Android SDK Platform-Tools"):
                with FactoryCamera(choice=_ANDROID):
                    self.fail("missing adb must stop camera setup")
            build.assert_not_called()

    def test_discovery_uses_native_ids_and_vendor_agnostic_usb_rear_camera_ids(self) -> None:
        adb = "/sdk/platform-tools/adb"
        scrcpy = "/opt/bin/scrcpy"
        paths = {"scrcpy": scrcpy, "ffmpeg": "/opt/bin/ffmpeg", adb: adb}
        native = ('[{"id":"exact-native-id","name":"MacBook Pro Camera","type":"built-in"},'
                  '{"id":"iphone-id","name":"iPhone","type":"continuity"},'
                  '{"id":"uvc-id","name":"Desk Camera","type":"external"}]')
        devices = ("List of devices attached\n"
                   "PIXEL123 device usb:3-1 model:Pixel_7 transport_id:1\n"
                   "PIXEL123-WIFI device model:Pixel_7 transport_id:2\n"
                   "OTHER unauthorized usb:3-2 transport_id:3\n")
        rear_and_front = ("[server] INFO: List of cameras:\n"
                          "    --camera-id=0    (back, 4080x3060, fps={15, 30})\n"
                          "    --camera-id=1    (front, 4128x3096, fps={15, 30})\n"
                          "    --camera-id=2    (back, 3264x2448, fps={15, 30})\n")
        for native_listing in (native, ""):
            with (
                self.subTest(native_available=bool(native_listing)),
                mock.patch.object(factory_camera.sys, "platform", "darwin"),
                mock.patch.object(factory_camera.shutil, "which", side_effect=paths.get),
                mock.patch.object(factory_camera, "_build_preview", return_value=Path("preview")),
                mock.patch.object(factory_camera, "_camera_inventory",
                                  side_effect=[native_listing, devices, rear_and_front]) as inventory,
            ):
                native_choices = (_NATIVE, CameraChoice("native", "iphone-id", "iPhone (Continuity Camera)"),
                                  CameraChoice("native", "uvc-id", "Desk Camera (External camera)")) if native_listing else ()
                android_choices = tuple(CameraChoice("android", "PIXEL123", f"Pixel 7 rear camera {camera_id} (PIXEL123)",
                                                     camera_id, adb) for camera_id in ("0", "2"))
                self.assertEqual(factory_camera.discover_cameras(adb_executable=adb), native_choices + android_choices)
                self.assertEqual([call.args[0] for call in inventory.call_args_list], [
                    ["preview", "--list-native"], [adb, "devices", "-l"],
                    [scrcpy, "--serial", "PIXEL123", "--list-cameras"],
                ])
                self.assertEqual(inventory.call_args.kwargs["env"]["ADB"], adb)

    def test_discovery_without_adb_keeps_native_and_reports_empty_inventory(self) -> None:
        native = '[{"id":"exact-native-id","name":"MacBook Pro Camera","type":"built-in"}]'
        paths = {"scrcpy": "/opt/bin/scrcpy", "ffmpeg": "/opt/bin/ffmpeg"}
        for listing, choices in ((native, (_NATIVE,)), ("[]", ())):
            with (
                self.subTest(listing=listing),
                mock.patch.object(factory_camera.sys, "platform", "darwin"),
                mock.patch.dict(factory_camera.os.environ, {}, clear=True),
                mock.patch.object(factory_camera.shutil, "which", side_effect=paths.get),
                mock.patch.object(factory_camera, "_build_preview", return_value=Path("preview")),
                mock.patch.object(factory_camera, "_camera_inventory", return_value=listing) as inventory,
            ):
                self.assertEqual(factory_camera.discover_cameras(), choices)
                inventory.assert_called_once_with(["preview", "--list-native"])

    def test_native_launch_uses_exact_id_cleans_up_and_fails_when_disconnected(self) -> None:
        preview = mock.Mock()
        preview.poll.return_value = None
        preview.stdout = io.BytesIO(_LABEL_LINE * 2)
        with (
            mock.patch.object(factory_camera.sys, "platform", "darwin"),
            mock.patch.object(factory_camera, "_require_command") as android_command,
            mock.patch.object(factory_camera, "_resolve_adb") as adb,
            mock.patch.object(factory_camera, "_build_preview", side_effect=lambda folder: folder / "preview"),
            mock.patch.object(factory_camera.subprocess, "Popen", return_value=preview) as launch,
        ):
            with FactoryCamera(choice=_NATIVE) as camera:
                camera._reader.join(timeout=1)
                label = CameraReading("label", "sample_0034", "QR", generation=1)
                self.assertEqual(camera.poll(), CameraUpdate((label,), label))
                executable, mode, device_id = launch.call_args.args[0]
                self.assertEqual((mode, device_id), ("--native", _NATIVE.device_id))
                directory = Path(executable).parent
                self.assertTrue(directory.is_dir())
                preview.poll.return_value = 1
                with self.assertRaisesRegex(DevError, "factory camera preview stopped"):
                    camera.poll()
            self.assertFalse(directory.exists())
            self.assertTrue(preview.stdout.closed)
            launch.assert_called_once()
            android_command.assert_not_called()
            adb.assert_not_called()
