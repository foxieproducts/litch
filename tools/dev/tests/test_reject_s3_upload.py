from __future__ import annotations

import contextlib
import io
import unittest

from tools.pio import reject_s3_upload


class RejectS3UploadTests(unittest.TestCase):
    def test_raw_upload_explains_installed_layout_preservation(self) -> None:
        stderr = io.StringIO()

        with contextlib.redirect_stderr(stderr):
            result = reject_s3_upload.main()

        self.assertEqual(result, 2)
        self.assertIn("overwrites the installed partition table", stderr.getvalue())
        self.assertIn("./litch flash s3 --device <usb-serial>", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
