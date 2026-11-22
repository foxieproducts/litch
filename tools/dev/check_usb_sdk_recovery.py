#!/usr/bin/env python3
"""Compile the exact SDK software owners with controlled hardware boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path.home() / '.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/include/arduino_tinyusb/tinyusb/src')
    parser.add_argument('--baseline-root', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    fixture = root / 'tools/dev/tests/usb_sdk_recovery'
    manifest = json.loads((fixture / 'manifest.json').read_text())
    for relative, expected in manifest['headers'].items():
        path = args.sdk / relative
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            parser.error(f'exact installed SDK header missing or changed: {path}')
    for relative, expected in manifest['sources'].items():
        path = root / relative
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            parser.error(f'exact SDK source changed: {path}')
    json_headers = list((root / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
    if not json_headers:
        parser.error('cached ArduinoJson headers required; build the native environment first')
    source_root = (args.baseline_root or root).resolve()
    with tempfile.TemporaryDirectory(prefix='litch-usb-sdk-') as temporary:
        build = Path(temporary)
        includes = ['-I' + str(p) for p in [fixture, args.sdk, args.sdk/'device', args.sdk/'common', args.sdk/'class/cdc']]
        objects = []
        for relative in manifest['sources']:
            source = root / relative
            obj = build / (source.name + '.o')
            flags = ['-Ddcd_event_handler=__real_dcd_event_handler'] if source.name == 'usbd.c' else []
            subprocess.run(['cc', '-std=c11', *flags, *includes, '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        defines = ['-D'+name+'="'+str(source_root/'src'/file)+'"' for name,file in [
            ('LITCH_USB_RECOVERY_SOURCE','native_usb_reset_recovery.cpp'),
            ('LITCH_USB_IRQ_SOURCE','usb_irq_probe.cpp'),
            ('LITCH_USB_EVENT_SOURCE','usb_boot_probe.cpp')]]
        if args.baseline_root:
            defines.append('-DLITCH_USB_OLD_SOURCE=1')
        executable = build / 'recovery'
        subprocess.run(['c++', '-std=c++17', '-Wno-c++11-narrowing', *defines, *includes,
                        '-I'+str(root/'include'), '-I'+str(root/'test/include'),
                        '-I'+str(root/'lib/foxie_embedded/include'), '-I'+str(json_headers[0].parent),
                        str(fixture/'fixture.cpp'), str(root/'lib/foxie_embedded/src/millis.cpp'),
                        *objects, '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], text=True, capture_output=True)
        print(result.stdout + result.stderr, end='')
        if args.output:
            args.output.write_text(result.stdout + result.stderr)
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
