"""Static activity contracts; platform behavior still requires HIL."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
ACTIVITY = ROOT / "app/android/app/src/main/kotlin/com/foxieproducts/litch_mobile/MainActivity.kt"


class AndroidSystemUiContractTest(unittest.TestCase):
    def test_native_owner_uses_transient_system_bars_and_edge_to_edge(self):
        source = ACTIVITY.read_text()
        self.assertIn("WindowCompat.setDecorFitsSystemWindows(window, false)", source)
        self.assertIn("BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE", source)
        self.assertIn("controller.hide(WindowInsetsCompat.Type.systemBars())", source)
        self.assertIn("controller.show(WindowInsetsCompat.Type.systemBars())", source)
        self.assertIn("Configuration.ORIENTATION_LANDSCAPE && !multiWindow", source)
        self.assertIn("isInMultiWindowMode", source)

    def test_configuration_resume_and_focus_reapply_after_flutter(self):
        source = ACTIVITY.read_text()
        for callback in ("onConfigurationChanged", "onPostResume", "onWindowFocusChanged"):
            start = source.index(f"override fun {callback}")
            body = source[start:source.index("\n    }", start)]
            self.assertLess(body.index(f"super.{callback}"), body.index("updateSystemBars()"))
        self.assertIn("if (hasFocus) updateSystemBars()", source)
        self.assertIn("window.decorView.post", source)

    def test_orientation_resolves_display_through_android_version_guard(self):
        source = ACTIVITY.read_text()
        self.assertIn("private fun currentDisplay()", source)
        start = source.index("private fun currentDisplay()")
        end = source.index("\n    }", start)
        helper = source[start:end]
        self.assertIn("Build.VERSION.SDK_INT >= Build.VERSION_CODES.R", helper)
        self.assertRegex(helper, r"\{\s*display\s*\} else \{")
        self.assertIn("windowManager.defaultDisplay", helper)
        self.assertIn("currentDisplay()?.displayId == displayId", source)
        self.assertIn("when (currentDisplay()?.rotation)", source)

    def test_no_competing_dart_visibility_authority(self):
        for path in (ROOT / "app/lib").rglob("*.dart"):
            self.assertNotIn("setEnabledSystemUIMode(", path.read_text(), str(path))

    def test_support_email_launches_mail_target_so_missing_handler_can_fail(self):
        source = ACTIVITY.read_text().split(
            '"com.foxieproducts.litch/support",', 1
        )[1].split('"com.foxieproducts.litch/external_links",', 1)[0]
        self.assertIn('val intent = Intent(Intent.ACTION_SENDTO, uri)', source)
        self.assertNotIn('Intent.createChooser(', source)
        self.assertRegex(
            source,
            r'try \{\s*startActivity\(intent\)\s*result.success\(true\)\s*'
            r'\} catch \(_: ActivityNotFoundException\) \{\s*result.success\(false\)',
        )


if __name__ == "__main__":
    unittest.main()
