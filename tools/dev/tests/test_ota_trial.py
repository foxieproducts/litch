from __future__ import annotations
import unittest
from tools.dev import ota_trial as policy

class SelectorEncodingTests(unittest.TestCase):
    def test_direct_selector_selects_valid_zero_and_clears_previous_selection(self):
        data = policy.make_selector_sector(1, policy.ESP_OTA_IMG_VALID) + b"\xff" * 0x1000
        first, second = policy.parse_otadata(data)
        self.assertEqual((first.slot, first.sequence, first.state, first.crc_valid), (0, 1, 2, True))
        self.assertFalse(second.selectable)

    def test_offline_factory_bootstrap_still_contains_valid_source_and_new_target(self):
        data = policy.make_dual_slot_bootstrap_otadata(bytes(32))
        first, second = policy.parse_otadata(data)
        self.assertEqual((first.slot, first.state, second.slot, second.state), (0, 2, 1, 0))

    def test_selector_encoding_rejects_invalid_bounds(self):
        for sequence in (0, 0xffffffff):
            with self.assertRaises(policy.OtaTrialError):
                policy.make_selector_sector(sequence, policy.ESP_OTA_IMG_VALID)
        with self.assertRaises(policy.OtaTrialError):
            policy.parse_otadata(bytes(32))
