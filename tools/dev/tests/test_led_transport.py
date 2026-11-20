"""Compile the Arduino renderer against a deterministic SDK/RMT adapter.

This checks pixel routing, RGB/RGBW feature selection, interrupt and flash
ownership at the real output call site, including the PCB-derived bottom
insertion boundary. It cannot establish wire encoding or optical output.
Captured pairing snapshots also pass through the production animation renderer,
compositor and power pipeline. Time and SDK/RMT acknowledgments are controlled;
the fixture does not run FreeRTOS task scheduling or physical button contacts.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "led_transport"


class LedTransportTest(unittest.TestCase):
    def test_captured_pairing_uses_production_animation_renderer(self):
        sources = [
            "neopixel_led_renderer", "budget_aware_led_pipeline",
            "led_animation_renderer", "led_animation_render_game", "led_animation_render_output",
            "board_owner_pairing_controller", "board_owner_pairing_runtime", "board_owner_pairing_presentation",
            "interaction_model_presentation", "control_led_feedback", "control_led_thinking_feedback",
            "control_led_resignation_feedback", "board_style_preview", "water_slosh",
            "water_slosh_session", "water_slosh_presentation", "boot_display",
            "sleep_countdown_led_frame", "manufacturing_test_led_frame", "compliance_test",
            "ota_led_presentation", "litchcraft_presentation", "litchcraft_presentation_playback",
            "boot_fox_asset", "edge_led_renderer", "edge_led_hint_feedback", "player_end_palette",
            "litchcraft_animation", "beginner_mode_policy", "chess_interaction_position_projection",
            "game_state_machine_castling", "board_owner_add_journal", "board_owner_pairing_persistence",
            "board_owner_pairing_add",
        ]
        files = [ROOT / "src" / (name + ".cpp") for name in sources]
        files += [ROOT / "lib/foxie_embedded/src" / (name + ".cpp")
                  for name in ("settings_store", "settings_store_internal")]
        files += sorted((ROOT / "src").glob("led_frame_composer*.cpp"))
        files += sorted((ROOT / "src").glob("litchcraft_frame_stream*.cpp"))
        with tempfile.TemporaryDirectory(prefix="litch-pairing-renderer-") as tmp:
            binary = Path(tmp) / "pairing_renderer"
            result = subprocess.run([
                "c++", "-std=c++17", "-O1", "-ffunction-sections", "-fdata-sections",
                "-DARDUINO", "-DLITCH_HW_VARIANT_S3", "-DLITCH_ESP32S3", "-DARDUINO_ARCH_ESP32",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                "-I", str(FIXTURE), "-I", str(ROOT / "include"),
                "-I", str(ROOT / "lib/foxie_embedded/include"),
                "-I", str(ROOT / "lib/litch-max/include"),
                "-I", str(ROOT / "lib/litch-max/vendor/mcu-max/src"),
                "-I", str(ROOT / ".pio/libdeps/native/ArduinoJson/src"),
                *map(str, files), str(FIXTURE / "pairing_renderer.cpp"),
                "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                "-o", str(binary),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("runtime_poll_between_render_samples=0", result.stdout)
            print(result.stdout, end="")

    def test_dual_strip_boot_and_recovery_keep_refill_serviceable(self):
        with tempfile.TemporaryDirectory(prefix="litch-led-transport-") as tmp:
            binary = Path(tmp) / "led_transport"
            recovery = (ROOT / "src/firmware_recovery.cpp").read_text()
            bodies = []
            for signature in ("bool FramesEqual(", "void RenderRecoverySurface("):
                start = recovery.index(signature)
                end = recovery.index("{", start) + 1
                depth = 1
                while depth:
                    depth += (recovery[end] == "{") - (recovery[end] == "}")
                    end += 1
                bodies.append(recovery[start:end])
            # Compile the actual presentation/cache owner, without importing
            # unrelated boot, storage and recovery-input adapters into this seam.
            (Path(tmp) / "recovery_surface.inc").write_text("\n".join(bodies))
            result = subprocess.run([
                "c++", "-std=c++17", "-DARDUINO", "-DLITCH_HW_VARIANT_S3", "-DARDUINO_ARCH_ESP32",
                "-I", str(FIXTURE), "-I", str(ROOT / "include"),
                "-I", tmp,
                "-I", str(ROOT / "lib/foxie_embedded/include"),
                str(ROOT / "src/neopixel_led_renderer.cpp"),
                str(ROOT / "src/power_rail.cpp"),
                str(ROOT / "src/budget_aware_led_pipeline.cpp"),
                str(ROOT / "src/recovery_led_frame.cpp"),
                str(ROOT / "src/recovery_policy.cpp"),
                str(FIXTURE / "main.cpp"), str(FIXTURE / "rail_lifecycle.cpp"),
                "-o", str(binary),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_runtime_uses_the_same_lifetime_bound_guard_before_ota(self):
        system = (ROOT / "include/firmware/firmware_internal.hpp").read_text()
        construction = system.split("hardware::NeoPixelLedRenderer& Leds()", 1)[1]
        self.assertLess(construction.index("SetPowerEnabledState(powerRail.LedOutputEnabledState())"),
                        construction.index("m_ledsConstructed = true"))
        render = (ROOT / "src/led_animation_renderer.cpp").read_text()
        render = render.split("void LedAnimationRenderer::Render(", 1)[1]
        guard = render.index("if (!OutputEnabled()) return;")
        self.assertLess(guard, render.index("LedBrightnessAttempt attempt"))
        for route in ("RenderGame(", "RenderArtwork(", "RenderSpecial("):
            self.assertLess(guard, render.index(route))
