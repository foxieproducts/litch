#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VERSION_HEADER="$REPO_ROOT/include/firmware/firmware_version.hpp"
PREP_SCRIPT="$REPO_ROOT/tools/release/prepare_firmware_release.sh"

if [[ ! -f "$VERSION_HEADER" ]]; then
  echo "Firmware version header missing: $VERSION_HEADER" >&2
  exit 1
fi

if [[ ! -f "$PREP_SCRIPT" ]]; then
  echo "Prepare script missing: $PREP_SCRIPT" >&2
  exit 1
fi

# Identity is reserved before QA, never bumped by a local build or retry.
identity="$(python3 "$SCRIPT_DIR/release_identity.py" check --product firmware --root "$REPO_ROOT")"
current_version="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])' <<< "$identity")"
echo "Prepared firmware version: $current_version"
echo
echo "Build target channel?"
echo "  1) release (stable)"
echo "  2) beta"
echo -n "Choose 1/2 [1]: "
read -r channel_input

channel="stable"
case "${channel_input:-1}" in
  1|""|release|stable|r|s)
    channel="stable"
    ;;
  2|beta|b)
    channel="beta"
    ;;
  *)
    echo "Invalid choice: ${channel_input}" >&2
    exit 1
    ;;
esac

echo
echo "Preparing ${channel} firmware with reserved version ${current_version}"
"$PREP_SCRIPT" --channel "$channel" --env litch-esp32s3 --version "$current_version"

echo
echo "Done. Artifacts are under build/fw_release/$channel."
echo "Local preparation does not publish or increment the beta identity."
