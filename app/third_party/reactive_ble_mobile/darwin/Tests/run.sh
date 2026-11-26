#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_build=$(mktemp -d "/private/tmp/litch-native-diagnostics.XXXXXX")
trap 'rm -rf "$test_build"' EXIT
test_frameworks="$(xcode-select -p)/Platforms/MacOSX.platform/Developer/Library/Frameworks"
test_libraries="$(xcode-select -p)/Platforms/MacOSX.platform/Developer/usr/lib"
test_private_frameworks="$(xcode-select -p)/Platforms/MacOSX.platform/Developer/Library/PrivateFrameworks"
xcrun swiftc -swift-version 4.2 -j 2 -num-threads 2 -module-cache-path "$test_build/module-cache" \
  "$test_dir/../Classes/Plugin/PairingRecoveryError.swift" \
  "$test_dir/pairing_recovery_error_tests.swift" -o "$test_build/pairing-recovery-tests"
"$test_build/pairing-recovery-tests"
xcrun swiftc -swift-version 4.2 -j 2 -num-threads 2 -module-cache-path "$test_build/module-cache" \
  -F "$test_frameworks" -Xlinker -rpath -Xlinker "$test_frameworks" \
  -I "$test_libraries" -L "$test_libraries" -Xlinker -rpath -Xlinker "$test_libraries" \
  -Xlinker -rpath -Xlinker "$test_private_frameworks" \
  "$test_dir/../Classes/Plugin/ScanNames.swift" \
  "$test_dir/../Classes/ReactiveBle/WriteWithoutResponseAdmission.swift" \
  "$test_dir/../Classes/Plugin/ConnectionDiagnostics.swift" \
  "$test_dir/main.swift" -o "$test_build/write-admission-tests"
"$test_build/write-admission-tests"

# Use only a counting SDK surface; compile the real guarded task/registry owners.
# -O executes their release-mode return instead of aborting at assert(false).
xcrun swiftc -swift-version 4.2 -O -j 2 -num-threads 2 \
  -module-cache-path "$test_build/module-cache" \
  -emit-library -emit-module -module-name CoreBluetooth \
  "$test_dir/core_bluetooth_connect_shim.swift" \
  -emit-module-path "$test_build/CoreBluetooth.swiftmodule" \
  -o "$test_build/libCoreBluetooth.dylib"
xcrun swiftc -swift-version 4.2 -O -j 2 -num-threads 2 \
  -module-cache-path "$test_build/module-cache" \
  -F "$test_frameworks" -Xlinker -rpath -Xlinker "$test_frameworks" \
  -I "$test_build" -L "$test_build" -lCoreBluetooth -Xlinker -rpath -Xlinker "$test_build" \
  -I "$test_libraries" -L "$test_libraries" -Xlinker -rpath -Xlinker "$test_libraries" \
  -Xlinker -rpath -Xlinker "$test_private_frameworks" \
  "$test_dir/../Classes/Plugin/ConnectionDiagnostics.swift" \
  "$test_dir/../Classes/Prelude/papply.swift" \
  "$test_dir/../Classes/ReactiveBle/CentralManagerDelegate.swift" \
  "$test_dir/../Classes/ReactiveBle/Tasks/Connect/ConnectTaskSpec.swift" \
  "$test_dir/../Classes/ReactiveBle/Tasks/Connect/ConnectTaskController.swift" \
  "$test_dir/../Classes/ReactiveBle/Tasks/PeripheralTaskRegistry/PeripheralTask.swift" \
  "$test_dir/../Classes/ReactiveBle/Tasks/PeripheralTaskRegistry/PeripheralTaskController.swift" \
  "$test_dir/../Classes/ReactiveBle/Tasks/PeripheralTaskRegistry/PeripheralTaskRegistry.swift" \
  "$test_dir/connect_probe_support.swift" "$test_dir/connect_probe_tests.swift" \
  -o "$test_build/connect-probe-tests"
"$test_build/connect-probe-tests"

# Compile the actual Central, GATT addressing, delegate and task owners. Flutter
# and protobuf support is limited to unused scalar/error shapes in the probe.
xcrun swiftc -swift-version 4.2 -O -j 2 -num-threads 2 \
  -module-cache-path "$test_build/module-cache" \
  -F "$test_frameworks" -Xlinker -rpath -Xlinker "$test_frameworks" \
  -I "$test_build" -L "$test_build" -lCoreBluetooth -Xlinker -rpath -Xlinker "$test_build" \
  -I "$test_libraries" -L "$test_libraries" -Xlinker -rpath -Xlinker "$test_libraries" \
  -Xlinker -rpath -Xlinker "$test_private_frameworks" \
  "$test_dir/../Classes/ReactiveBle/"*.swift \
  "$test_dir/../Classes/ReactiveBle/Tasks/"*/*.swift \
  "$test_dir/../Classes/Prelude/base.swift" \
  "$test_dir/../Classes/Prelude/papply.swift" \
  "$test_dir/../Classes/Plugin/Common/Failable.swift" \
  "$test_dir/../Classes/Plugin/ConnectionDiagnostics.swift" \
  "$test_dir/../Classes/BleData extras/CharacteristicInstance.swift" \
  "$test_dir/write_probe_support.swift" "$test_dir/write_probe_tests.swift" \
  -o "$test_build/write-probe-tests"
"$test_build/write-probe-tests"
