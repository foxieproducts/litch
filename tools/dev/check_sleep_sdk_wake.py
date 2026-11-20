#!/usr/bin/env python3
"""Run the production S3 sleep path with recording SDK hardware boundaries."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.build_paths import platformio_sleep_dependencies
from tools.dev.runner import DevError


# Arduino-ESP32 2.0.17 / repository-pinned 3.20017.241212+sha.dcc1105b.
SDK_HEADERS = {
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
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--json-include', type=Path)
    parser.add_argument('--environment', help='resolve dependencies for this PlatformIO environment')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.environment:
        try:
            sdk, json_include = platformio_sleep_dependencies(root, args.environment)
        except DevError as error:
            parser.error(str(error))
        args.sdk = args.sdk or sdk
        args.json_include = args.json_include or json_include
    if args.sdk is None:
        args.sdk = Path.home() / '.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3'
    fixture = root / 'tools/dev/tests/sleep_sdk_wake'
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
    with tempfile.TemporaryDirectory(prefix='litch-sleep-sdk-') as temporary:
        build = Path(temporary)
        # These are SDK hardware boundaries, not copied SDK implementations.
        for name in ('Arduino.h', 'Wire.h', 'esp_attr.h',
                     'esp_system.h', 'esp_err.h', 'esp_assert.h', 'tusb.h',
                     'driver/gpio.h', 'driver/rtc_io.h', 'soc/soc_caps.h',
                     'hal/touch_sensor_types.h', 'soc/gpio_periph.h'):
            header = build / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "' + str(fixture / 'sdk_boundary.hpp') + '"\n')
        executable = build / 'sleep-wake'
        sources = [fixture / 'fixture.cpp', root / 'lib/foxie_embedded/src/millis.cpp',
                   root / 'lib/foxie_embedded/src/settings_store.cpp',
                   root / 'lib/foxie_embedded/src/settings_store_internal.cpp',
                   root / 'lib/foxie_embedded/src/logger.cpp',
                   root / 'src/support_runtime_stats.cpp']
        dead_strip = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
        subprocess.run(['c++', '-std=c++17', '-Wno-c99-designator', '-Wno-c++11-narrowing',
                        '-ffunction-sections', '-fdata-sections', dead_strip,
                        '-I' + str(build), '-I' + str(fixture), '-I' + str(ulp),
                        '-I' + str(args.sdk / 'include/esp_hw_support/include'),
                        '-I' + str(args.sdk / 'include/soc/esp32s3/include'),
                        '-I' + str(args.sdk / 'include/esp_common/include'),
                        '-I' + str(args.sdk / 'include/hal/include'),
                        '-I' + str(root / 'include'),
                        '-I' + str(root / 'lib/foxie_embedded/include'),
                        '-I' + str(json_include), *map(str, sources),
                        '-o', str(executable)], check=True)
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
