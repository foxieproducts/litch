#!/usr/bin/env python3
"""Reject raw PlatformIO uploads that blindly replace the installed S3 layout."""

from __future__ import annotations

import sys


MESSAGE = (
    "Direct PlatformIO upload is disabled for Litch S3 because it overwrites "
    "the installed partition table and can invalidate saved data. Use "
    "'./litch flash s3 --device <usb-serial>' instead."
)


def main() -> int:
    print(MESSAGE, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
