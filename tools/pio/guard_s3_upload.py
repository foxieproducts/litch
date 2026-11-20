"""PlatformIO hook that blocks the unsafe generic S3 upload target."""

import sys

from SCons.Errors import UserError

Import("env")  # type: ignore[name-defined]  # Provided by SCons/PlatformIO.

sys.path.insert(0, env.subst("$PROJECT_DIR/tools/pio"))  # type: ignore[name-defined]

from reject_s3_upload import MESSAGE  # noqa: E402


def reject_raw_upload(source, target, env) -> None:
    del source, target, env
    raise UserError(MESSAGE)


env.AddPreAction("upload", reject_raw_upload)  # type: ignore[name-defined]
