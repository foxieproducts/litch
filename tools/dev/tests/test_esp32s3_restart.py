"""Compile the production restart wrapper against a bounded SDK boundary.

The peer-lock/reset/XTAL ordering reproduces the observed orphaned lock. It does
not emulate native USB, the entire SDK or physical watchdog/electrical behavior.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "esp32s3_restart"
SDK = """
#pragma once
#include <initializer_list>
#define IRAM_ATTR
int cpu_hal_get_core_id();
void regi2c_enter_critical();
unsigned ReadRegister(unsigned);
#define RTC_CNTL_OPTIONS0_REG 0x60008000U
#define RTC_CNTL_SW_CPU_STALL_REG 0x600080bcU
#define RTC_CNTL_SW_STALL_APPCPU_C0_S 0
#define RTC_CNTL_SW_STALL_APPCPU_C0_M 3U
#define RTC_CNTL_SW_STALL_APPCPU_C1_S 20
#define RTC_CNTL_SW_STALL_APPCPU_C1_M (0x3fU << 20)
#define RTC_CNTL_SW_STALL_PROCPU_C0_S 2
#define RTC_CNTL_SW_STALL_PROCPU_C0_M (3U << 2)
#define RTC_CNTL_SW_STALL_PROCPU_C1_S 26
#define RTC_CNTL_SW_STALL_PROCPU_C1_M (0x3fU << 26)
#define REG_GET_FIELD(reg, field) ((ReadRegister(reg) & field##_M) >> field##_S)
"""


class Esp32S3RestartTest(unittest.TestCase):
    def run_fixture(self, *, prior_ordering=False):
        with tempfile.TemporaryDirectory(prefix="litch-restart-order-") as tmp:
            directory = Path(tmp)
            (directory / "fixture_sdk.hpp").write_text(SDK)
            for name in ("esp_attr.h", "hal/cpu_hal.h", "regi2c_ctrl.h", "soc/rtc_cntl_reg.h"):
                header = directory / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('#include "fixture_sdk.hpp"\n')
            binary = directory / "restart_order"
            argv = ["c++", "-std=c++17", "-DARDUINO", "-DLITCH_ESP32S3",
                    "-I", str(directory), str(ROOT / "src/esp32s3_restart.cpp"),
                    str(FIXTURE / "main.cpp"), "-o", str(binary)]
            if prior_ordering:
                argv.append("-DLITCH_WITHOUT_RESTART_ORDERING_GUARD")
            compiled = subprocess.run(argv, capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            return subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)

    def test_live_peer_releases_before_teardown_and_stalled_peer_keeps_sdk_path(self):
        result = self.run_fixture()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_prior_sdk_ordering_reproduces_orphaned_lock(self):
        result = self.run_fixture(prior_ordering=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("core=0 peer_stalled=0 owner=1 outcome=1", result.stdout)
        self.assertIn("abandoned peer lock", result.stdout)
