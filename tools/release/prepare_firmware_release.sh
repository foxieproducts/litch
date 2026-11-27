#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_ROOT="${FW_RELEASE_OUTPUT_ROOT:-$REPO_ROOT/build/fw_release}"
ARTIFACT_TOOL="$SCRIPT_DIR/firmware_artifact.py"

usage() {
  cat <<'EOF'
Usage:
  tools/release/prepare_firmware_release.sh [--channel stable|beta] [--env platformio_env] [--notes-count N] [--version VERSION] [--public-base-url URL]

Examples:
  tools/release/prepare_firmware_release.sh --channel stable --env litch-esp32s3
  tools/release/prepare_firmware_release.sh --channel beta --version 0.6.0-beta.1

Outputs include:
  - Permanent S3 aliases: firmware.bin/version.md or firmware_beta.bin/version_beta.md
  - SHA-addressed immutable firmware and notes
  - manifest.json or manifest_beta.json (schema 1, generated last)
EOF
}

log() {
  echo "[fw-release] $*"
}

channel="stable"
env_name="litch-esp32s3"
notes_count="20"
forced_version=""
public_base_url="https://updates.foxieproducts.com/fw"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --channel)
      channel="$2"
      shift 2
      ;;
    --env)
      env_name="$2"
      shift 2
      ;;
    --notes-count)
      notes_count="$2"
      shift 2
      ;;
    --version)
      forced_version="$2"
      shift 2
      ;;
    --public-base-url)
      public_base_url="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "$channel" != "stable" && "$channel" != "beta" ]]; then
  echo "--channel must be stable or beta" >&2
  exit 1
fi

for required_command in pio git python3; do
  if ! command -v "$required_command" >/dev/null 2>&1; then
    echo "$required_command not found in PATH." >&2
    exit 1
  fi
done

cd "$REPO_ROOT"
build_bin="$(python3 - "$env_name" <<'PY'
from pathlib import Path
import sys

from tools.dev.build_paths import platformio_build_directory
from tools.dev.runner import DevError

try:
    print(platformio_build_directory(Path.cwd(), sys.argv[1]) / "firmware.bin")
except DevError as error:
    raise SystemExit(str(error))
PY
)"
log "Building ESP32-S3 firmware candidate for env $env_name"
pio run -e "$env_name"

log "Checking S3 Sleep/wake before preparing release artifacts"
python3 -m tools.dev.check_sleep_sdk_wake --environment "$env_name"

if [[ ! -f "$build_bin" ]]; then
  echo "Expected firmware file missing: $build_bin" >&2
  exit 1
fi

# Inspection is the target gate: only a complete, hashed chip-ID-9 image can ship.
artifact_sha="$(python3 "$ARTIFACT_TOOL" inspect --firmware "$build_bin" --field sha256)"

release_version="$forced_version"
if [[ -z "$release_version" ]]; then
  version_header="$REPO_ROOT/include/firmware/firmware_version.hpp"
  if [[ -f "$version_header" ]]; then
    release_version="$(sed -n 's/.*FIRMWARE_VERSION = "\([^"]*\)".*/\1/p' "$version_header" | head -n 1)"
  fi
  if [[ -z "$release_version" ]]; then
    release_version="$(git describe --tags --match 'fw-*' --always --dirty 2>/dev/null || git rev-parse --short HEAD)"
  fi
fi

if [[ "$channel" == "stable" ]]; then
  tag_prefix="fw-stable-"
  firmware_alias="firmware.bin"
  notes_alias="version.md"
  manifest_name="manifest.json"
else
  tag_prefix="fw-beta-"
  firmware_alias="firmware_beta.bin"
  notes_alias="version_beta.md"
  manifest_name="manifest_beta.json"
fi

short_sha="$(git rev-parse --short HEAD)"
build_time_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
last_tag="$(git tag --list "${tag_prefix}*" --sort=-creatordate | head -n 1 || true)"
if [[ -n "$last_tag" ]]; then
  log_args=("$last_tag..HEAD")
else
  log_args=(--max-count="$notes_count" HEAD)
fi

release_dir="$OUTPUT_ROOT/$channel"
mkdir -p "$release_dir"
firmware_alias_path="$release_dir/$firmware_alias"
notes_alias_path="$release_dir/$notes_alias"
manifest_path="$release_dir/$manifest_name"

cp "$build_bin" "$firmware_alias_path"

{
  echo "# Litch Firmware $(echo "$channel" | tr '[:lower:]' '[:upper:]')"
  echo
  echo "Version: $release_version"
  echo "Build UTC: $build_time_utc"
  echo "Git SHA: $short_sha"
  echo "PlatformIO env: $env_name"
  if [[ -n "$last_tag" ]]; then
    echo "Base tag: $last_tag"
  else
    echo "Base tag: none"
  fi
  echo
  echo "## Changes"
  git log --no-merges --pretty='- %h %s' "${log_args[@]}"
} > "$notes_alias_path"

notes_sha="$(python3 "$ARTIFACT_TOOL" inspect-notes --notes "$notes_alias_path" --field sha256)"
name_field() {
  python3 "$ARTIFACT_TOOL" name \
    --channel "$channel" \
    --version "$release_version" \
    --sha256 "$artifact_sha" \
    --notes-sha256 "$notes_sha" \
    --field "$1"
}

firmware_immutable="$(name_field firmware_immutable)"
notes_immutable="$(name_field notes_immutable)"
firmware_immutable_path="$release_dir/$firmware_immutable"
notes_immutable_path="$release_dir/$notes_immutable"
cp "$firmware_alias_path" "$firmware_immutable_path"
cp "$notes_alias_path" "$notes_immutable_path"

# Remove obsolete path-bearing metadata from older local preparations.
rm -f "$release_dir/manifest.txt"

# The manifest is generated only after every file it names exists locally.
python3 "$ARTIFACT_TOOL" create-manifest \
  --firmware "$firmware_alias_path" \
  --notes "$notes_alias_path" \
  --channel "$channel" \
  --version "$release_version" \
  --public-base-url "$public_base_url" \
  --output "$manifest_path"
python3 "$ARTIFACT_TOOL" verify \
  --firmware "$firmware_immutable_path" \
  --notes "$notes_immutable_path" \
  --manifest "$manifest_path" \
  --manifest-url "${public_base_url%/}/$manifest_name"

log "Prepared verified ESP32-S3 firmware artifacts"
log "Permanent firmware alias: $firmware_alias_path"
log "Permanent version alias: $notes_alias_path"
log "Immutable firmware: $firmware_immutable_path"
log "Immutable notes: $notes_immutable_path"
log "Schema-1 manifest: $manifest_path"
