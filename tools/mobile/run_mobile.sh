#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
APP_DIR="$REPO_ROOT/app"

usage() {
  cat <<'EOF'
Usage:
  tools/mobile/run_mobile.sh ios [--device <id>] [--release]
  tools/mobile/run_mobile.sh android [--device <id>] [--release]

Examples:
  tools/mobile/run_mobile.sh ios --device "$IOS_DEVICE_ID"
  tools/mobile/run_mobile.sh android
  tools/mobile/run_mobile.sh ios --release
EOF
}

log() {
  echo "[mobile-run] $*"
}

require_brew() {
  if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required for automated setup but was not found." >&2
    echo "Install Homebrew from https://brew.sh and rerun." >&2
    exit 1
  fi
}

ensure_flutter() {
  if command -v flutter >/dev/null 2>&1; then
    return
  fi

  require_brew
  log "Flutter not found; installing via Homebrew cask..."
  brew install --cask flutter
}

ensure_xcode_ios() {
  if [[ ! -d /Applications/Xcode.app ]]; then
    echo "Xcode is required at /Applications/Xcode.app for iOS runs." >&2
    exit 1
  fi

  local current_dev
  current_dev="$(xcode-select -p 2>/dev/null || true)"
  local target_dev="/Applications/Xcode.app/Contents/Developer"

  if [[ "$current_dev" != "$target_dev" ]]; then
    log "Switching active Xcode developer directory to $target_dev"
    if sudo xcode-select --switch "$target_dev"; then
      log "xcode-select switched successfully"
    else
      echo "Failed to switch xcode-select automatically." >&2
      echo "Run this manually and rerun:" >&2
      echo "  sudo xcode-select --switch $target_dev" >&2
      exit 1
    fi
  fi

  if ! xcrun xcodebuild -version >/dev/null 2>&1; then
    echo "Xcode command-line tools are not ready." >&2
    echo "Run this manually and rerun:" >&2
    echo "  sudo xcodebuild -runFirstLaunch" >&2
    exit 1
  fi
}

ensure_cocoapods() {
  if command -v pod >/dev/null 2>&1; then
    return
  fi

  require_brew
  log "CocoaPods not found; installing via Homebrew..."
  brew install cocoapods
}

ensure_android_sdk() {
  local sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"

  if [[ ! -d "$sdk_root" ]]; then
    echo "Android SDK not found at $sdk_root." >&2
    echo "Install Android Studio and SDK, then rerun." >&2
    exit 1
  fi

  export ANDROID_SDK_ROOT="$sdk_root"
  export ANDROID_HOME="$sdk_root"
  export PATH="$sdk_root/platform-tools:$sdk_root/cmdline-tools/latest/bin:$PATH"

  if ! command -v adb >/dev/null 2>&1; then
    echo "adb not found. Ensure Android SDK platform-tools are installed." >&2
    exit 1
  fi

  if ! command -v sdkmanager >/dev/null 2>&1; then
    echo "sdkmanager not found. Install Android SDK cmdline-tools (latest) in Android Studio." >&2
    exit 1
  fi
}

run_flutter() {
  local platform="$1"
  local device_id="$2"
  local mode="$3"

  local -a run_args
  run_args=()
  if [[ -n "$device_id" ]]; then
    run_args+=("-d" "$device_id")
  fi
  if [[ "$mode" == "release" ]]; then
    run_args+=("--release")
  fi

  cd "$APP_DIR"

  log "Running flutter pub get..."
  flutter pub get

  if [[ "$platform" == "ios" ]]; then
    log "Running pod install (iOS dependencies)..."
    (cd ios && pod install --repo-update)
  fi

  log "Starting Flutter run for $platform ${mode:+($mode)}"
  if [[ ${#run_args[@]} -gt 0 ]]; then
    flutter run "${run_args[@]}"
  else
    flutter run
  fi
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 1
  fi

  if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
  fi

  local platform="$1"
  shift

  local device_id=""
  local mode="debug"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --device)
        if [[ $# -lt 2 ]]; then
          echo "--device requires a value" >&2
          exit 1
        fi
        device_id="$2"
        shift 2
        ;;
      --release)
        mode="release"
        shift
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

  ensure_flutter

  case "$platform" in
    ios)
      ensure_xcode_ios
      ensure_cocoapods
      ;;
    android)
      ensure_android_sdk
      ;;
    *)
      echo "Unknown platform: $platform" >&2
      usage
      exit 1
      ;;
  esac

  run_flutter "$platform" "$device_id" "$mode"
}

main "$@"
