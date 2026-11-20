"""Keep the bounded capacity witness and local hints tied to production names."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ConsoleCommandInventoryTest(unittest.TestCase):
    def test_capacity_witness_covers_normal_firmware_registrations(self):
        production = []
        for filename in (
            "firmware_console.cpp", "firmware_console_feature_commands.cpp",
            "game_console_commands.cpp", "motion_console_commands.cpp",
            "trace_console_commands.cpp",
        ):
            production.extend(re.findall(
                r'\bRegisterCommand\(\s*"([^"]+)"',
                (ROOT / "src" / filename).read_text(),
            ))
        fixture = (ROOT / "test/test_console_input_bounds/test_main.cpp").read_text()
        inventory = fixture.split("const char* registrations[] = {", 1)[1].split("};", 1)[0]
        names = re.findall(r'"([^"]+)"', inventory)
        self.assertEqual(len(production), 33)
        self.assertEqual(len(names), len(set(names)))
        self.assertCountEqual(names, production,
                              "update the bounded capacity witness when commands change")

    def test_local_root_hints_are_real_commands_without_duplicate_aliases(self):
        production = set()
        for path in (ROOT / "src").glob("*console*.cpp"):
            production.update(re.findall(r'\bRegisterCommand\(\s*"([^"]+)"', path.read_text()))
        for path in (ROOT / "lib/foxie_embedded/src").glob("console_builtins_*.cpp"):
            production.update(re.findall(r'\bRegisterCommand\(\s*"([^"]+)"', path.read_text()))
        hints = (ROOT / "app/lib/src/models/console_completion.dart").read_text()
        names = re.findall(r"'([^']+)'", hints.split("const _commands = <String>[", 1)[1].split("];", 1)[0])
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(set(names) <= production, set(names) - production)
        self.assertFalse(set(names) & {"ver", "reset", "flip", "p1black", "ownerpair", "panicprobe"})


if __name__ == "__main__":
    unittest.main()
