#!/usr/bin/env python3
"""Build the production VirtualLitch bridge and run the real iOS Home journey."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--build-only', action='store_true',
                        help='Build only the native bridge; do not invoke Flutter')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = (args.output or root / '.pio' / 'virtual-litch-coaching').resolve()
    build = output / 'build'
    evidence = output / 'evidence'
    evidence.mkdir(parents=True, exist_ok=True)
    # pio test delegates to pio run, whose supported job default is read from
    # PLATFORMIO_RUN_JOBS. The test command itself has no --jobs option.
    env = dict(os.environ, PLATFORMIO_BUILD_DIR=str(build))
    env.setdefault('PLATFORMIO_RUN_JOBS', '2')
    nice = ['nice', '-n', '15']
    if not args.build_only and not (root / 'app' / '.dart_tool' / 'package_config.json').is_file():
        subprocess.run([*nice, 'flutter', 'pub', 'get'],
                       cwd=root / 'app', env=env, check=True)
    if not args.skip_build:
        subprocess.run([*nice, 'pio', 'test', '-e', 'native', '-f',
                        'test_virtual_litch_app_bridge_manual',
                        '--without-testing'], cwd=root, env=env, check=True)
    program = build / 'native' / ('program.exe' if os.name == 'nt' else 'program')
    if not program.is_file():
        parser.error(f'Bridge executable missing: {program}')
    if args.build_only:
        print(program)
        return
    # Widget tests need the platform font explicitly; iOS otherwise falls back
    # to Ahem even when the common review helper loads Roboto.
    cupertino_font = Path('/System/Library/Fonts/SFNS.ttf')
    if cupertino_font.is_file():
        env.setdefault('LITCH_HEALTH_CUPERTINO_FONT', str(cupertino_font))
    if not env.get('LITCH_HEALTH_CUPERTINO_FONT'):
        parser.error('Readable iOS captures require LITCH_HEALTH_CUPERTINO_FONT '
                     'pointing to an available system font')
    env.update(LITCH_VIRTUAL_BRIDGE=str(program),
               LITCH_VIRTUAL_EVIDENCE=str(evidence),
               LITCH_HEALTH_CAPTURE_DIR=str(evidence))
    subprocess.run([*nice, 'flutter', 'test', '--no-pub', '--concurrency=1',
                    'test/virtual_litch_bridge_failure_test.dart',
                    'test/virtual_litch_coaching_journey_test.dart',
                    'test/virtual_litch_clock_setup_journey_test.dart',
                    'test/virtual_litch_bootstrap_journey_test.dart',
                    '--reporter', 'expanded'], cwd=root / 'app', env=env, check=True)


if __name__ == '__main__':
    main()
