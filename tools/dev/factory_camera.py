"""Select and stream one exact camera to the macOS factory preview."""

from __future__ import annotations

import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from dataclasses import dataclass, replace
from pathlib import Path
from typing import BinaryIO, Literal

from .runner import DevError


_WIDTH = 1920
_HEIGHT = 1440
_FRAMES_PER_SECOND = 6
_MAX_FRAME_EVENT_BYTES = 1024
_MAX_PENDING_TRANSITIONS = 32
_START_TIMEOUT_SECONDS = 20
_FRAME_TIMEOUT_SECONDS = 8
_USB_SERIAL = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
_CAMERA_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,31}\Z")
_REAR_CAMERA = re.compile(r"^\s*--camera-id=([^\s]+)\s+\(back,", re.MULTILINE)


@dataclass(frozen=True)
class CameraChoice:
    provider: Literal["native", "android"]
    device_id: str
    label: str
    camera_id: str | None = None
    adb_executable: str | None = None


@dataclass(frozen=True)
class CameraReading:
    state: Literal["label", "clear", "ambiguous", "reset", "stalled"]
    payload: str | None = None
    symbology: str | None = None
    generation: int = 0


@dataclass(frozen=True)
class CameraUpdate:
    transitions: tuple[CameraReading, ...] = ()
    current: CameraReading | None = None
    stalled: bool = False


def _read_frame(line: bytes) -> CameraReading:
    try:
        frame = json.loads(line)
        if isinstance(frame, dict):
            state = frame.get("state")
            if state in ("clear", "ambiguous") and set(frame) == {"state"}:
                return CameraReading(state)
            if state == "label" and set(frame) == {"state", "payload", "symbology"}:
                payload = frame["payload"]
                symbology = frame["symbology"]
                if (isinstance(payload, str) and 0 < len(payload.encode()) <= 128
                        and symbology in ("QR", "Data Matrix")):
                    return CameraReading(state, payload, symbology)
    except ValueError:
        pass
    raise DevError("factory camera preview sent an invalid reading")


class _ReadingDebounce:
    def __init__(self) -> None:
        self._current: CameraReading | None = None
        self._observed: CameraReading | None = None
        self._count = 0

    def observe(self, reading: CameraReading) -> CameraReading | None:
        if reading != self._observed:
            self._observed = reading
            self._count = 0
        required = {"clear": 3, "label": 2, "ambiguous": 1}[reading.state]
        self._count = min(self._count + 1, required)
        if reading != self._current and self._count == required:
            self._current = reading
            return reading
        return None


def _require_command(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise DevError(f"factory camera requires {name} on PATH")
    return path


def _resolve_adb(executable: str | None) -> str:
    if executable is not None:
        if path := shutil.which(executable):
            return os.path.abspath(path)
        raise DevError(f"factory camera could not execute the selected adb: {executable}")
    configured = os.environ.get("ADB")
    path = (shutil.which(configured) if configured else None) or shutil.which("adb")
    if path is None:
        raise DevError("factory camera requires adb. Install Android SDK Platform-Tools, "
                       "add adb to PATH, or set ADB to its executable.")
    return os.path.abspath(path)


def _build_preview(build_dir: Path) -> Path:
    contents = build_dir / "Litch Factory Camera.app" / "Contents"
    executable = contents / "MacOS" / "LitchFactoryCamera"
    executable.parent.mkdir(parents=True)
    (contents / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleIdentifier": "com.foxielitch.factory-camera",
        "CFBundleExecutable": executable.name,
        "CFBundleName": "Litch Factory Camera",
        "CFBundlePackageType": "APPL",
        "NSCameraUsageDescription": "Preview the selected camera and read factory board labels locally.",
        "NSCameraUseContinuityCameraDeviceType": True,
        "NSHighResolutionCapable": True,
    }))
    command = [
        "/usr/bin/xcrun", "swiftc", str(Path(__file__).with_suffix(".swift")),
        "-module-cache-path", str(build_dir / "modules"), "-o", str(executable),
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=45, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DevError(f"could not compile the factory camera preview: {error}") from error
    if result.returncode:
        raise DevError("could not compile the factory camera preview. Check Xcode Command Line Tools. "
                       + result.stderr.strip())
    return executable


def _camera_inventory(argv: list[str], *, env: dict[str, str] | None = None) -> str:
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=8, check=False, env=env)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout if result.returncode == 0 else ""


def _native_choices(preview: Path) -> tuple[CameraChoice, ...]:
    try:
        cameras = json.loads(_camera_inventory([str(preview), "--list-native"]))
    except ValueError:
        return ()
    if not isinstance(cameras, list):
        return ()
    labels = {"built-in": "Built-in camera", "continuity": "Continuity Camera", "external": "External camera"}
    choices = []
    for camera in cameras:
        if not isinstance(camera, dict):
            continue
        device_id, name, kind = (camera.get(key) for key in ("id", "name", "type"))
        if not all(isinstance(value, str) and value.isprintable() and value.strip()
                   for value in (device_id, name, kind)) or kind not in labels:
            continue
        choices.append(CameraChoice("native", device_id, f"{name} ({labels[kind]})"))
    return tuple(choices)


def _android_choices(adb_executable: str | None) -> tuple[CameraChoice, ...]:
    scrcpy = shutil.which("scrcpy")
    if not scrcpy or not shutil.which("ffmpeg"):
        return ()
    try:
        adb = _resolve_adb(adb_executable)
    except DevError:
        return ()
    choices = []
    for line in _camera_inventory([adb, "devices", "-l"]).splitlines():
        fields = line.split()
        if (len(fields) < 3 or fields[1] != "device" or not _USB_SERIAL.fullmatch(fields[0])
                or not any(field.startswith("usb:") for field in fields[2:])):
            continue
        serial = fields[0]
        model = next((field.removeprefix("model:").replace("_", " ")
                      for field in fields[2:] if field.startswith("model:")), "Android")
        if not model.isprintable():
            model = "Android"
        listing = _camera_inventory([scrcpy, "--serial", serial, "--list-cameras"], env=dict(os.environ, ADB=adb))
        for camera_id in _REAR_CAMERA.findall(listing):
            if _CAMERA_ID.fullmatch(camera_id):
                choices.append(CameraChoice("android", serial, f"{model} rear camera {camera_id} ({serial})",
                                            camera_id, adb))
    return tuple(choices)


def discover_cameras(adb_executable: str | None = None) -> tuple[CameraChoice, ...]:
    """List camera identities without opening a camera feed."""

    if sys.platform != "darwin":
        raise DevError("the factory camera picker requires macOS")
    with tempfile.TemporaryDirectory(prefix="litch-camera-picker-") as directory:
        native = _native_choices(_build_preview(Path(directory)))
    cameras = native + _android_choices(adb_executable)
    identities = Counter((choice.provider, choice.device_id, choice.camera_id) for choice in cameras)
    labels = Counter(choice.label for choice in cameras)
    return tuple(
        replace(choice, label=f"{choice.label} [{choice.device_id}]") if labels[choice.label] > 1 else choice
        for choice in cameras if identities[choice.provider, choice.device_id, choice.camera_id] == 1
    )


class FactoryCamera:
    """Own the selected camera, visible preview, and latest debounced observation."""

    def __init__(self, *, choice: CameraChoice) -> None:
        if choice.provider == "android":
            if not _USB_SERIAL.fullmatch(choice.device_id) or not _CAMERA_ID.fullmatch(choice.camera_id or ""):
                raise DevError("factory camera requires an exact Android USB serial and rear camera ID", exit_code=2)
        elif choice.provider != "native" or not (choice.device_id.isprintable() and choice.device_id.strip()):
            raise DevError("factory camera requires an exact native camera ID", exit_code=2)
        self._choice = choice
        self._directory: tempfile.TemporaryDirectory[str] | None = None
        self._children: dict[str, subprocess.Popen[bytes]] = {}
        self._reader: threading.Thread | None = None
        self._readings_lock = threading.Lock()
        self._debounce = _ReadingDebounce()
        self._transitions: list[CameraReading] = []
        self._current: CameraReading | None = None
        self._overflowed = False
        self._latest_frame: CameraReading | None = None
        self._reader_error: DevError | None = None
        self._generation = 0
        self._started = 0.0
        self._last_frame = 0.0
        self._stalled = False
        self._pending_stall: CameraReading | None = None

    def __enter__(self) -> FactoryCamera:
        if sys.platform != "darwin":
            raise DevError("the factory camera preview requires macOS")
        if self._directory is not None:
            raise DevError("factory camera preview is already running")
        if self._choice.provider == "android":
            scrcpy = _require_command("scrcpy")
            ffmpeg = _require_command("ffmpeg")
            adb = _resolve_adb(self._choice.adb_executable)
        try:
            self._directory = tempfile.TemporaryDirectory(prefix="litch-factory-camera-")
            directory = Path(self._directory.name)
            preview = _build_preview(directory)
            if self._choice.provider == "android":
                window = self._launch_android(preview, directory, scrcpy=scrcpy, ffmpeg=ffmpeg, adb=adb)
            else:
                window = self._launch("factory camera preview", [
                    str(preview), "--native", self._choice.device_id,
                ], stdout=subprocess.PIPE)
            assert window.stdout is not None
            self._debounce = _ReadingDebounce()
            self._transitions.clear()
            self._current = None
            self._overflowed = False
            self._latest_frame = None
            self._reader_error = None
            self._last_frame = 0.0
            self._stalled = False
            self._pending_stall = None
            self._started = time.monotonic()
            self._reader = threading.Thread(target=self._read, args=(window.stdout,), daemon=True)
            self._reader.start()
        except BaseException:
            self.close()
            raise
        return self

    def _launch_android(self, preview: Path, directory: Path, *, scrcpy: str, ffmpeg: str,
                        adb: str) -> subprocess.Popen[bytes]:
        choice = self._choice
        # scrcpy sends encoded frames through this live pipe to FFmpeg.
        fifo = directory / "camera-stream"
        os.mkfifo(fifo, 0o600)
        filters = (f"fps={_FRAMES_PER_SECOND},"
                   "crop=iw/2:ih/2,"
                   f"scale={_WIDTH}:{_HEIGHT}:force_original_aspect_ratio=decrease,"
                   f"pad={_WIDTH}:{_HEIGHT}:(ow-iw)/2:(oh-ih)/2")
        decoded = self._launch("camera frame decoder", [
            ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-threads", "2",
            "-f", "matroska", "-probesize", "32768", "-analyzeduration", "250000", "-i", str(fifo),
            "-an", "-filter_threads", "2", "-vf", filters, "-threads", "2",
            "-pix_fmt", "bgra", "-f", "rawvideo", "pipe:1",
        ], stdout=subprocess.PIPE)
        assert decoded.stdout is not None
        try:
            window = self._launch("factory camera preview", [
                str(preview), str(_WIDTH), str(_HEIGHT), choice.label,
            ], stdin=decoded.stdout, stdout=subprocess.PIPE)
        finally:
            decoded.stdout.close()
        self._launch("Android camera", [
            scrcpy, "--serial", choice.device_id, "--video-source=camera",
            f"--camera-id={choice.camera_id}", "--no-audio", "--no-control", "--no-window",
            "--max-size=1920", "--camera-fps=15", "--video-bit-rate=8M",
            "--verbosity=warn", "--record-format=mkv", f"--record={fifo}",
        ], stdout=sys.stderr, env=dict(os.environ, ADB=adb))
        return window

    def _launch(self, name: str, argv: list[str], *, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, env: dict[str, str] | None = None) -> subprocess.Popen[bytes]:
        try:
            child = subprocess.Popen(argv, stdin=stdin, stdout=stdout, env=env, start_new_session=True)
        except OSError as error:
            raise DevError(f"could not start {name}: {error}") from error
        self._children[name] = child
        return child

    def _check_frame_gap(self, now: float) -> None:
        # Check under the readings lock at both ingress and poll: an entire gap
        # can pass while the station is flashing or exchanging serial commands.
        if self._stalled or not self._last_frame or now - self._last_frame <= _FRAME_TIMEOUT_SECONDS:
            return
        self._stalled = True
        self._generation += 1
        self._pending_stall = CameraReading("stalled", generation=self._generation)
        self._transitions.clear()
        self._debounce = _ReadingDebounce()
        self._overflowed = False
        self._current = None
        self._latest_frame = None

    def _read(self, stream: BinaryIO) -> None:
        try:
            while line := stream.readline(_MAX_FRAME_EVENT_BYTES + 1):
                if len(line) > _MAX_FRAME_EVENT_BYTES or not line.endswith(b"\n"):
                    raise DevError("factory camera preview sent an oversized or truncated reading")
                reading = _read_frame(line)
                with self._readings_lock:
                    now = time.monotonic()
                    self._check_frame_gap(now)
                    self._last_frame = now
                    self._stalled = False
                    self._latest_frame = reading
                    if self._overflowed:
                        continue
                    changed = self._debounce.observe(reading)
                    if changed is None:
                        continue
                    self._generation += 1
                    if len(self._transitions) == _MAX_PENDING_TRANSITIONS:
                        self._transitions.clear()
                        self._current = None
                        self._overflowed = True
                        continue
                    self._current = replace(changed, generation=self._generation)
                    self._transitions.append(self._current)
        except (OSError, DevError) as error:
            with self._readings_lock:
                self._reader_error = DevError(f"could not read the factory camera preview: {error}")

    def poll(self) -> CameraUpdate:
        """Drain ordered transitions and report the stable reading still in view."""

        if not self._children:
            raise DevError("factory camera preview is not running")
        for name, child in reversed(self._children.items()):
            if (code := child.poll()) is not None:
                recovery = ("If this batch has a recorded verifier PASS, keep the verifier connected and restart with "
                            "--resume. Otherwise, leave completed boards disconnected and start a new station run.")
                if name == "factory camera preview" and code == 0:
                    raise DevError(f"factory camera preview closed. {recovery}")
                raise DevError(f"{name} stopped (exit {code}). Check the selected camera. {recovery}")
        with self._readings_lock:
            if self._reader_error:
                raise self._reader_error
            now = time.monotonic()
            if not self._last_frame and now - self._started > _START_TIMEOUT_SECONDS:
                raise DevError("factory camera did not start sending frames. Check the selected camera and its permissions.")
            self._check_frame_gap(now)
            transitions = (self._pending_stall,) if self._pending_stall is not None else ()
            self._pending_stall = None
            if self._overflowed:
                # Discard readings until the station can react to the lost sequence.
                self._overflowed = False
                self._debounce = _ReadingDebounce()
                return CameraUpdate(transitions + (CameraReading("reset", generation=self._generation),))
            transitions += tuple(self._transitions)
            self._transitions.clear()
            current = self._current
            if current is not None and replace(current, generation=0) != self._latest_frame:
                current = None
            return CameraUpdate(transitions, current, stalled=self._stalled)

    def close(self) -> None:
        """Stop only this preview's owned processes and delete its temporary files."""

        # Close from camera to preview so the display still drains decoded frames.
        for name in ("Android camera", "camera frame decoder", "factory camera preview"):
            child = self._children.get(name)
            if child is None:
                continue
            if child.poll() is None:
                child.terminate()
            try:
                child.wait(timeout=0.75)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if self._reader is not None:
            self._reader.join(timeout=1)
            self._reader = None
        for child in self._children.values():
            if child.stdout is not None:
                child.stdout.close()
        self._children.clear()
        if self._directory is not None:
            self._directory.cleanup()
            self._directory = None

    def __exit__(self, *_exc: object) -> None:
        self.close()
