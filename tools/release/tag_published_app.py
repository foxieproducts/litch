#!/usr/bin/env python3
"""Tag an app build after its production store listing confirms publication."""

import argparse
from pathlib import Path
import re
import subprocess
import sys

import release_identity


def tag_published_app(root: Path, platform: str, version: str, build: int, source: str) -> str:
    def git(*args):
        return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()

    if platform not in ("ios", "android") or not re.fullmatch(r"[0-9a-f]{40}", source):
        raise ValueError("specify the store platform and full build commit SHA")
    if git("cat-file", "-t", source) != "commit":
        raise ValueError("build source must be a commit")
    identity = release_identity.app_source_identity({
        path: git("show", f"{source}:{path}")
        for path in (release_identity.PUBSPEC, release_identity.APP_IDENTITY)
    })
    if (identity["native_version"], identity["package_code"]) != (version, build):
        raise ValueError("store version or build number differs from the build commit")
    tag = f"app-{platform}-{version}-build-{build}"
    ref = f"refs/tags/{tag}"

    def remote_commit():
        listing = git("ls-remote", "origin", ref, ref + "^{}")
        refs = {name: sha for sha, name in (line.split() for line in listing.splitlines())}
        return refs.get(ref + "^{}", refs.get(ref))

    target = remote_commit()
    if target is None:
        # A non-forced creation also protects against another publisher racing us.
        git("push", "--no-follow-tags", "origin", f"{source}:{ref}")
        target = remote_commit()
    if target != source:
        raise ValueError(f"{tag} does not point to the build commit on origin")
    return tag


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("ios", "android"))
    parser.add_argument("--version", required=True, help="native version shown by the store")
    parser.add_argument("--build", type=int, required=True, help="published build number or version code")
    parser.add_argument("--source", required=True, help="full Git SHA recorded when the package was built")
    args = parser.parse_args()
    try:
        print(tag_published_app(Path(__file__).resolve().parents[2], args.platform,
                                args.version, args.build, args.source))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
