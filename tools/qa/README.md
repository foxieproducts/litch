# App and firmware integration tests

The test runner exercises the Flutter app against the native board implementation. Use `python3 tools/qa/interactive_sync.py --help` to see the supported profiles.

## Interactive sync backend preparation

First follow the [recursive checkout instructions](../../README.md#building-or-repairing-litch) to obtain the pinned Litch-Max submodule. Run these commands from the repository root with PlatformIO and Flutter installed. The native tests build the runner and generate the saved-game history fixture. The final command builds the bridge and runs the Flutter tests selected by the bridge profile.

```sh
(
  set -e
  qa_root="$PWD"
  qa_native_build="$qa_root/.pio/interactive-sync/native-build"
  mkdir -p "$qa_root/build"
  env -u LITCH_NATIVE_TEST_FILTER \
    PLATFORMIO_BUILD_DIR="$qa_native_build" PLATFORMIO_RUN_JOBS=1 \
    pio test -e native
  export LITCHCRAFT_NATIVE_RUNNER="$qa_native_build/native/program"
  export LITCH_HISTORY_REBASE_WIRE="$qa_root/build/test_history_rebase_segments.jsonl"
  test -x "$LITCHCRAFT_NATIVE_RUNNER"
  test -s "$LITCH_HISTORY_REBASE_WIRE"
  unset LITCH_VIRTUAL_BRIDGE
  python3 tools/qa/interactive_sync.py --profile bridge
)
```
