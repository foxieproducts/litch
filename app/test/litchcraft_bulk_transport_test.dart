import 'support/memory_ota_target_store.dart';
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';

class _Sdk implements FlutterReactiveBle {
  final mtu = Completer<int>();
  final writes = <List<int>>[];

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) =>
      this.mtu.future;

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    writes.add(List.of(value));
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async => writes.add(List.of(value));

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _SingleAdmissionSdk implements FlutterReactiveBle {
  final writes = <List<int>>[];
  final firstAdmission = Completer<void>();

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    writes.add(List<int>.of(value));
    if (writes.length == 1) await firstAdmission.future;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _Service extends LitchBleService {
  _Service(_Sdk sdk) : super(pendingOtaStore: MemoryOtaTargetStore(), ble: sdk);

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask:
            LitchDeviceCapabilities.litchcraftBit |
            LitchDeviceCapabilities.litchcraftBulkBit,
        explicitlyReported: true,
      );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'connection transport serializes writes through one native admission slot',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final sdk = _SingleAdmissionSdk();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: sdk,
      )..debugSetConnectedDevice('board');
      addTearDown(() async {
        await service.dispose();
        debugDefaultTargetPlatformOverride = null;
      });

      final first = service.writeConnectionSessionPacket(
        'board',
        List<int>.filled(10, 1),
      );
      final second = service.writeConnectionSessionPacket(
        'board',
        List<int>.filled(10, 2),
      );
      await Future<void>.delayed(Duration.zero);

      expect(sdk.writes, hasLength(1));
      sdk.firstAdmission.complete();
      await Future.wait(<Future<void>>[first, second]);
      expect(sdk.writes, <List<int>>[
        List<int>.filled(10, 1),
        List<int>.filled(10, 2),
      ]);
    },
  );

  test('minimum MTU refuses bulk before sending any staging bytes', () async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    final sdk = _Sdk()..mtu.complete(23);
    final service = _Service(sdk)..debugSetConnectedDevice('board');
    addTearDown(() async {
      await service.dispose();
      debugDefaultTargetPlatformOverride = null;
    });
    await expectLater(
      service.writeLitchcraftBulkArtwork('board', 41, 1, List.filled(256, 0)),
      throwsA(isA<LitchcraftBulkUnavailable>()),
    );
    expect(sdk.writes, isEmpty);
  });

  // Raw characteristic staging is retired. Shared Object admission/reconnect
  // is covered by the native interactive-sync and connection-session journeys.
  test(
    'bulk transfer rejects same-board reconnect during MTU calibration',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      final sdk = _Sdk();
      final service = _Service(sdk)..debugSetConnectedDevice('board');
      addTearDown(() async {
        await service.dispose();
        debugDefaultTargetPlatformOverride = null;
      });
      final transfer = service.writeLitchcraftBulkArtwork(
        'board',
        41,
        1,
        List.filled(256, 0x123456),
      );
      final rejected = expectLater(transfer, throwsStateError);
      for (var i = 0; i < 10; ++i) {
        await Future<void>.delayed(Duration.zero);
      }
      expect(sdk.writes, isEmpty);
      await service.disconnect();
      service.debugSetConnectedDevice('board');
      sdk.mtu.complete(247);
      await rejected;
      expect(sdk.writes, isEmpty);
    },
  );
}
