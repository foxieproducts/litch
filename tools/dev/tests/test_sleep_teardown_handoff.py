#!/usr/bin/env python3
"""Compile actual S3 sleep owners; native shutdown refusal and SDK hardware are leaves."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import os
import unittest
import tempfile


# Arduino-ESP32 2.0.17 / repository-pinned 3.20017.241212+sha.dcc1105b.
SDK_HEADERS = {
    'include/soc/esp32s3/include/soc/reset_reasons.h': '145dfe1b777844e685d57edc6a4ec507ce0d2e1269f0dc719f7de914c9fbcfaa',
    'include/esp_rom/include/esp_rom_sys.h': '66f7d2bed4bf521ff4edd6bf355d06136454be7820a317996aeab3805f852b83',
    'include/esp_hw_support/include/esp_sleep.h': '23601e8aea2eac11146689a1236eb571e7006108e43e8043351553c3ce72c291',
    'include/ulp/include/esp32s3/ulp.h': '07a75058eceede7706cb2fe54528d258e1808ad360cb7bfc253dd411d8fd140e',
    'include/ulp/include/ulp_common.h': 'ac5fb51c0d727a1001ef655269fb0629407465614d280ae93d036014676db912',
    'include/soc/esp32s3/include/soc/rtc_io_reg.h': 'e92ff8b2737c30ad88df65f2b296dbfd4ecb25fecf1b10fa8bef9d9db460ff37',
    'include/soc/esp32s3/include/soc/rtc_cntl_reg.h': '9dd6a1eceb3990d4a9552017a7f98bae2288ceb7e18d22b3267deaaa35d43a96',
    'include/hal/include/hal/gpio_types.h': '6fe36a52957c80d62db884b919cf29964e03b7b4d929f76d7d6c1437f0056a02',
    'include/hal/include/hal/rtc_io_types.h': '91e66d4248a689ceda008c20e830b19097ec1efb15352a765fdf80428297139f',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path.home() / '.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3')
    parser.add_argument('--json-include', type=Path)
    args = parser.parse_args()
    root = Path(os.environ.get('LITCH_SLEEP_TEST_ROOT', str(Path(__file__).resolve().parents[3])))
    fixture = Path(__file__).resolve().parent / 'sleep_teardown'
    ulp = args.sdk / 'include/ulp/include'
    for relative, expected in SDK_HEADERS.items():
        header = args.sdk / relative
        if not header.is_file() or hashlib.sha256(header.read_bytes()).hexdigest() != expected:
            parser.error(f'pinned installed S3 SDK header missing or changed: {header}')
    json_include = args.json_include
    if json_include is None:
        cached = list((root / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not cached:
            parser.error('provide --json-include for cached ArduinoJson/src')
        json_include = cached[0].parent
    with tempfile.TemporaryDirectory(prefix='litch-sleep-teardown-') as temporary:
        build = Path(temporary)
        # These are SDK hardware boundaries, not copied SDK implementations.
        for name in ('Arduino.h', 'Wire.h', 'esp_attr.h',
                     'esp_system.h', 'esp_err.h', 'esp_assert.h', 'tusb.h',
                     'esp_task_wdt.h', 'freertos/FreeRTOS.h', 'freertos/task.h', 'sdkconfig.h',
                     'foxie_embedded/console.hpp',
                     'driver/gpio.h', 'driver/rtc_io.h', 'soc/soc_caps.h',
                     'hal/touch_sensor_types.h', 'soc/gpio_periph.h'):
            header = build / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "' + str(fixture / 'sdk_boundary.hpp') + '"\n')
        def function(path, signature):
            source = path.read_text()
            start = source.index(signature)
            end = source.index("{", start) + 1
            depth = 1
            while depth:
                if source[end] == "{": depth += 1
                elif source[end] == "}": depth -= 1
                end += 1
            return source[start:end]
        runtime = (root / 'src/ble_service_runtime.cpp').read_text()
        stop_type = 'bool' if 'bool BleService::Stop()' in runtime else 'void'
        (build / 'ble_stop.inc').write_text(function(root / 'src/ble_service_runtime.cpp', stop_type + ' BleService::Stop()'))
        adapter_source = (root / 'src/firmware_app_boot_services.cpp').read_text()
        adapter_type = 'bool' if 'bool StopBleBeforeSleep(' in adapter_source else 'void'
        (build / 'sleep_adapter.inc').write_text(function(root / 'src/firmware_app_boot_services.cpp', adapter_type + ' StopBleBeforeSleep('))
        (build / 'sleep_request.inc').write_text(function(
            root / 'src/firmware_app_boot_services.cpp',
            'modules::ControlCoordinatorResult RequestControlSleep('))
        loop = (root / 'src/firmware_app_runtime.cpp').read_text()
        suppression_start = loop.index('    const bool sleepSuppressed =')
        suppression_end = loop.index('\n\n    runtime::NoteProgress', suppression_start)
        (build / 'sleep_suppression.inc').write_text(loop[suppression_start:suppression_end])
        source_defines = []
        for name in ('sleep_transition', 'sleep_controller', 'p1x_deep_wake', 'sleep_controller_wake',
                     'sleep_controller_hardware', 'sleep_controller_enter_sleep', 'sleep_entry_orchestration'):
            source_defines.append('-DLITCH_SOURCE_' + name.upper() + '="' + str(root / 'src' / (name + '.cpp')) + '"')
        executable = build / 'sleep-wake'
        sources = [fixture / 'fixture.cpp', root / 'lib/foxie_embedded/src/millis.cpp',
                   root / 'lib/foxie_embedded/src/settings_store.cpp',
                   root / 'lib/foxie_embedded/src/settings_store_internal.cpp',
                   root / 'lib/foxie_embedded/src/logger.cpp',
                   root / 'src/progress_watchdog.cpp', root / 'src/runtime_health.cpp',
                   root / 'src/support_runtime_stats.cpp']
        dead_strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        subprocess.run(['c++', '-std=c++17', '-Wno-c99-designator', '-Wno-c++11-narrowing',
                        '-ffunction-sections', '-fdata-sections', dead_strip,
                        '-I' + str(build), '-I' + str(fixture), '-I' + str(ulp),
                        '-I' + str(args.sdk / 'include/esp_hw_support/include'),
                        '-I' + str(args.sdk / 'include/soc/esp32s3/include'),
                        '-I' + str(args.sdk / 'include/esp_common/include'),
                        '-I' + str(args.sdk / 'include/esp_rom/include'),
                        '-I' + str(args.sdk / 'include/hal/include'),
                        '-I' + str(root / 'include'),
                        '-I' + str(root / 'lib/foxie_embedded/include'),
                        '-I' + str(json_include), *source_defines, *map(str, sources),
                        '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], check=False)
        if result.returncode:
            return result.returncode
        return 0


class SleepTeardownHandoffTest(unittest.TestCase):
    def test_native_stop_refusal_prevents_sdk_entry_and_uses_restart_recovery(self):
        saved = sys.argv
        try:
            sys.argv = [saved[0]]
            self.assertEqual(main(), 0)
        finally:
            sys.argv = saved


if __name__ == '__main__':
    raise SystemExit(main())
