import 'dart:async';

import 'package:flutter/services.dart';

/// Native evidence of missing keys, rather than a generic connection failure.
final class BluetoothPairingKeyMissing {
  const BluetoothPairingKeyMissing(
    this.deviceId,
    this.generation, {
    this.encryptionKeyMissing = false,
  });
  final String deviceId;
  final int generation;
  final bool encryptionKeyMissing;
}

/// A native bond attempt ended without a bond. This does not identify its cause.
final class BluetoothPairingBondFailed {
  const BluetoothPairingBondFailed(this.deviceId, this.generation);
  final String deviceId;
  final int generation;
}

enum BluetoothPairingRecoveryReason {
  keyMissing,
  ownerVerificationFailed,
  freshBondFailed,
  savedEncryptionKeyMissing,
}

enum BluetoothPairingRepairResult { removed, unsupported, cancelled, failed }

enum BluetoothPairingBondStartResult { started, alreadyBonded, failed }

enum BluetoothPairingRecoveryStage { checking, ready, repairing, reconnecting }

final class BluetoothPairingRecoveryState {
  const BluetoothPairingRecoveryState({
    required this.deviceId,
    this.reason = BluetoothPairingRecoveryReason.keyMissing,
    this.canRepair = false,
    this.stage = BluetoothPairingRecoveryStage.checking,
    this.failed = false,
  });

  final String deviceId;
  final BluetoothPairingRecoveryReason reason;
  final bool canRepair;
  final BluetoothPairingRecoveryStage stage;
  final bool failed;
  bool get busy => stage != BluetoothPairingRecoveryStage.ready;

  BluetoothPairingRecoveryState copyWith({
    bool? canRepair,
    BluetoothPairingRecoveryStage? stage,
    bool? failed,
  }) => BluetoothPairingRecoveryState(
    deviceId: deviceId,
    reason: reason,
    canRepair: canRepair ?? this.canRepair,
    stage: stage ?? this.stage,
    failed: failed ?? this.failed,
  );
}

abstract interface class BluetoothPairingRecoveryGateway {
  Stream<BluetoothPairingKeyMissing> get keyMissing;
  Stream<BluetoothPairingBondFailed> get bondFailures;
  Future<void> watchDevice(String? deviceId, int generation);
  Future<bool> canRepairPairing();
  Future<BluetoothPairingBondStartResult> startBond(
    String deviceId,
    int generation,
    int attempt,
  );
  Future<BluetoothPairingRepairResult> repairPairing(
    String deviceId,
    int generation,
  );
  Future<bool> openBluetoothSettings();
  Future<void> dispose();
}

const iosPairingKeyMissingMarker = 'LITCH_PAIRING_KEY_MISSING:CBErrorDomain:14';

bool isNativePairingKeyMissing(String? message) =>
    message?.split('\n').first == iosPairingKeyMissingMarker;

/// Narrow OS pairing operations. This grants no board/session authority.
final class MethodChannelBluetoothPairingRecovery
    implements BluetoothPairingRecoveryGateway {
  MethodChannelBluetoothPairingRecovery({MethodChannel? channel})
    : _channel =
          channel ??
          const MethodChannel('com.foxieproducts.litch/bluetooth_recovery') {
    _channel.setMethodCallHandler(_handle);
  }

  final MethodChannel _channel;
  final _events = StreamController<BluetoothPairingKeyMissing>.broadcast(
    sync: true,
  );

  final _bondFailures = StreamController<BluetoothPairingBondFailed>.broadcast(
    sync: true,
  );

  @override
  Stream<BluetoothPairingBondFailed> get bondFailures => _bondFailures.stream;

  Future<void> _handle(MethodCall call) async {
    if (call.method == 'pairingBondFailed' && !_bondFailures.isClosed) {
      final arguments = call.arguments;
      if (arguments is! Map || arguments['source'] != 'android_bond_failed') {
        return;
      }
      final id = arguments['deviceId'];
      final generation = arguments['generation'];
      if (id is String &&
          id.isNotEmpty &&
          generation is int &&
          generation >= 0) {
        _bondFailures.add(BluetoothPairingBondFailed(id, generation));
      }
      return;
    }
    if (call.method != 'pairingKeyMissing' || _events.isClosed) return;
    final arguments = call.arguments;
    if (arguments is! Map ||
        (arguments['source'] != 'android_key_missing' &&
            arguments['source'] != 'android_encryption_key_missing')) {
      return;
    }
    final id = arguments['deviceId'];
    final generation = arguments['generation'];
    if (id is String && id.isNotEmpty && generation is int && generation >= 0) {
      _events.add(
        BluetoothPairingKeyMissing(
          id,
          generation,
          encryptionKeyMissing:
              arguments['source'] == 'android_encryption_key_missing',
        ),
      );
    }
  }

  @override
  Stream<BluetoothPairingKeyMissing> get keyMissing => _events.stream;

  Future<T?> _call<T>(String method, [Map<String, Object?>? arguments]) async {
    try {
      return await _channel.invokeMethod<T>(method, arguments);
    } on MissingPluginException {
      return null;
    } on PlatformException {
      return null;
    }
  }

  @override
  Future<void> watchDevice(String? deviceId, int generation) async {
    await _call<void>('watchDevice', {
      'deviceId': deviceId,
      'generation': generation,
    });
  }

  @override
  Future<bool> canRepairPairing() async =>
      await _call<bool>('canRepairPairing') == true;

  @override
  Future<BluetoothPairingBondStartResult> startBond(
    String deviceId,
    int generation,
    int attempt,
  ) async {
    final result = await _call<Object?>('startBond', {
      'deviceId': deviceId,
      'generation': generation,
      'attempt': attempt,
    });
    return switch (result) {
      'started' => BluetoothPairingBondStartResult.started,
      'alreadyBonded' => BluetoothPairingBondStartResult.alreadyBonded,
      _ => BluetoothPairingBondStartResult.failed,
    };
  }

  @override
  Future<BluetoothPairingRepairResult> repairPairing(
    String deviceId,
    int generation,
  ) async {
    final reply = await _call<Map>('repairPairing', {
      'deviceId': deviceId,
      'generation': generation,
    });
    return switch (reply?['status']) {
      'removed' => BluetoothPairingRepairResult.removed,
      'cancelled' => BluetoothPairingRepairResult.cancelled,
      'failed' => BluetoothPairingRepairResult.failed,
      _ => BluetoothPairingRepairResult.unsupported,
    };
  }

  @override
  Future<bool> openBluetoothSettings() async =>
      await _call<bool>('openBluetoothSettings') == true;

  @override
  Future<void> dispose() async {
    _channel.setMethodCallHandler(null);
    await _events.close();
    await _bondFailures.close();
  }
}

/// Virtual peers do not have an OS bond database.
final class UnavailableBluetoothPairingRecovery
    implements BluetoothPairingRecoveryGateway {
  const UnavailableBluetoothPairingRecovery();
  @override
  Stream<BluetoothPairingKeyMissing> get keyMissing => const Stream.empty();
  @override
  Stream<BluetoothPairingBondFailed> get bondFailures => const Stream.empty();
  @override
  Future<void> watchDevice(String? deviceId, int generation) async {}
  @override
  Future<bool> canRepairPairing() async => false;
  @override
  Future<BluetoothPairingBondStartResult> startBond(
    String deviceId,
    int generation,
    int attempt,
  ) async => BluetoothPairingBondStartResult.failed;
  @override
  Future<BluetoothPairingRepairResult> repairPairing(
    String deviceId,
    int generation,
  ) async => BluetoothPairingRepairResult.unsupported;
  @override
  Future<bool> openBluetoothSettings() async => false;
  @override
  Future<void> dispose() async {}
}
