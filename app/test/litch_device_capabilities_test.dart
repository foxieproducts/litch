import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'support/reboot_reply_ble.dart';
import 'support/support_session_board.dart';

class _CapabilityBle implements FlutterReactiveBle {
  _CapabilityBle(this.response);

  String response;
  String? healthResponse = '{"h":"000000000"}';
  String? healthRecheckResponse = '{"c":"ok"}';
  String? healthResponseAfterRecheck;
  int healthResponsesToSkip = 0;
  int healthRequestCount = 0;
  int mtuRequestCount = 0;
  bool infoRepliesEnabled = true;
  Completer<void>? infoWriteReady;
  final List<Map<String, Object?>> writes = <Map<String, Object?>>[];
  final StreamController<List<int>> packets =
      StreamController<List<int>>.broadcast(sync: true);

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) {
    return packets.stream;
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) async {
    if (characteristic.characteristicId !=
        BleProtocol.otaControlCharacteristic) {
      return;
    }
    final Object? payload = jsonDecode(utf8.decode(value));
    if (payload is Map<String, Object?>) {
      writes.add(Map<String, Object?>.from(payload));
      if (payload['cmd'] == 'info') {
        await infoWriteReady?.future;
        if (infoRepliesEnabled) {
          scheduleMicrotask(() => packets.add(utf8.encode(response)));
        }
      } else if (payload['cmd'] == 'health') {
        ++healthRequestCount;
        if (healthRequestCount > healthResponsesToSkip &&
            healthResponse != null) {
          scheduleMicrotask(() => packets.add(utf8.encode(healthResponse!)));
        }
      } else if (payload['cmd'] == 'health_chk' &&
          healthRecheckResponse != null) {
        final String response = healthRecheckResponse!;
        final String? nextHealth = healthResponseAfterRecheck;
        if (nextHealth != null) {
          healthResponse = nextHealth;
        }
        scheduleMicrotask(() => packets.add(utf8.encode(response)));
      }
    }
  }

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) async {
    mtuRequestCount += 1;
    return 247;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

/// The iOS SDK installs notifications asynchronously. A successful info write
/// can reach firmware before its response characteristic has a native listener.
class _StartingNotificationCapabilityBle extends _CapabilityBle {
  _StartingNotificationCapabilityBle() : super('{"i":[3,3,7295]}');

  final Completer<void> notificationReady = Completer<void>();
  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) => notificationReady.future.asStream().asyncExpand(
    (_) => super.subscribeToCharacteristic(characteristic),
  );
}

class _CapabilityTrace implements AppTraceSink {
  final List<Map<String, Object?>> records = <Map<String, Object?>>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    records.add(<String, Object?>{'event': event, ...fields});
  }
}

// Exercise the real app session with the V1 ESP32-C3 capability report.
// Native tests own the matching firmware macro boundary. This peer reuses the
// existing shared admission/grants and permits only read-only diagnostics.
class _V1DiagnosticsBoard extends SupportSessionBoard {
  _V1DiagnosticsBoard() {
    capabilityMask = 47 | LitchDeviceCapabilities.litchcraftBit;
    dropFirstVersionReply = false;
  }
  int infoRequests = 0;
  int healthRequests = 0;
  final objectCodes = <int>[];

  @override
  void emitStatus(String json) {
    if (!sessionActivated) {
      super.emitStatus(json);
      return;
    }
    scheduleMicrotask(() {
      if (!sessionPackets.isClosed) {
        emitSupport(0x32, Uint8List.fromList(utf8.encode(json)));
      }
    });
  }

  @override
  Future<void> commandWrite(List<int> value) async {
    final command = jsonDecode(utf8.decode(value)) as Map;
    switch (command['cmd']) {
      case 'info':
        ++infoRequests;
        emitStatus('{"i":[3,1,$capabilityMask]}');
        return;
      case 'health':
        ++healthRequests;
        emitStatus('{"h":"000100000"}');
        return;
      case 'ota_begin' || 'ota_end' || 'ota_abort':
        throw StateError('This profile has no OTA transaction adapter.');
    }
    await super.commandWrite(value);
  }

  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.object) {
      objectCodes.add(packet.code);
      if (packet.code == 0x31) {
        throw StateError('This profile has no OTA data adapter.');
      }
    }
    await super.sharedWrite(value);
  }
}

void main() {
  test('recovery info is truthful and rejects unknown or domain modes', () {
    final recovery = LitchDeviceCapabilities.fromPayload({
      'i': [3, 4, 24, 1],
    })!;
    expect(recovery.recoveryOnly, isTrue);
    expect(recovery.supportsConnectionSession, isTrue);
    expect(recovery.supportsFirmwareDigest, isTrue);
    expect(recovery.supportsFirmwareUpdate, isTrue);
    expect(recovery.supportsConsole, isFalse);
    expect(recovery.supportsSettings, isFalse);
    expect(recovery.supportsIdentify, isFalse);
    expect(recovery.supportsRuntimeHealth, isFalse);
    expect(recovery.supportsGameHistory, isFalse);
    expect(
      LitchDeviceCapabilities.fromPayload({
        'i': [3, 4, 24],
      })!.recoveryOnly,
      isFalse,
    );
    for (final info in [
      [3, 4, 24, 2],
      [3, 4, 24, 'bad'],
      [3, 4, 31, 1],
      [2, 4, 24, 1],
    ]) {
      expect(LitchDeviceCapabilities.fromPayload({'i': info}), isNull);
    }
  });

  test(
    'opening a session bootstraps unknown capabilities on the pre-session channel',
    () async {
      final transport = _CapabilityBle('{"e":"unknown_cmd"}');
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: transport,
      );
      service.debugSetConnectedDevice('v3-cold-start');
      final opening = service.openConnectionSession('v3-cold-start');

      await Future<void>.delayed(Duration.zero);

      expect(
        transport.writes.where((write) => write['cmd'] == 'info'),
        hasLength(1),
        reason:
            'The capability probe must remain a direct pre-session exchange; '
            'waiting for the session here deadlocks cold start.',
      );

      await Future<void>.delayed(Duration.zero);
      expect(await opening.timeout(const Duration(seconds: 2)), isNull);
      await service.dispose();
      await transport.packets.close();
    },
  );

  testWidgets(
    'iOS capability discovery recovers a reply lost before native notifications',
    (WidgetTester tester) async {
      final transport = _StartingNotificationCapabilityBle();
      final trace = _CapabilityTrace();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: transport,
        appTrace: trace,
      );
      service.debugSetConnectedDevice('v3-starting');
      final discovery = service.fetchDeviceCapabilities(
        deviceId: 'v3-starting',
      );
      await tester.pump();
      expect(
        transport.writes.where((write) => write['cmd'] == 'info'),
        hasLength(1),
      );
      expect(transport.packets.hasListener, isFalse);
      await tester.pump(const Duration(milliseconds: 400));
      transport.notificationReady.complete();
      await tester.pump();
      expect(transport.packets.hasListener, isTrue);
      for (int tick = 0; tick < 40; tick++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      final capabilities = await discovery;
      await tester.runAsync(() async {
        await service.dispose();
        await transport.packets.close();
      });

      expect(capabilities.supportsLitchcraft, isTrue);
      expect(capabilities.explicitlyReported, isTrue);
      expect(capabilities.hardwareRevision, LitchHardwareRevision.v3);
      expect(transport.mtuRequestCount, 1);
      expect(
        trace.records
            .where((record) => record['event'] == 'ble.capability_result')
            .map((record) => record['outcome']),
        <String>['timeout', 'reported'],
      );
      expect(jsonEncode(trace.records), isNot(contains('v3-starting')));
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  // Current epoch3 Store adoption with this notification-startup fault is
  // covered by virtual_litch_cold_attach_journey_test.dart using native owners.

  testWidgets(
    'slow initial info write retries current capability discovery',
    (WidgetTester tester) async {
      final transport = _CapabilityBle('{"i":[3,3,7295]}')
        ..infoWriteReady = Completer<void>();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: transport,
      );
      service.debugSetConnectedDevice('v3-busy');
      final discovery = service.fetchDeviceCapabilities(deviceId: 'v3-busy');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 1400));
      transport.infoWriteReady!.complete();
      for (int tick = 0; tick < 40; tick++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      final capabilities = await discovery;
      await tester.runAsync(() async {
        await service.dispose();
        await transport.packets.close();
      });

      expect(capabilities.supportsLitchcraft, isTrue);
      expect(
        transport.writes.where((write) => write['cmd'] == 'info'),
        hasLength(2),
      );
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  test(
    'silent capability discovery fails closed and reconnect permits rediscovery',
    () async {
      final transport = _CapabilityBle('{"i":[3,3,7295]}')
        ..infoRepliesEnabled = false;
      final trace = _CapabilityTrace();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: transport,
        appTrace: trace,
      );
      service.debugSetConnectedDevice('silent-board');
      final discovery = service.fetchDeviceCapabilities(
        deviceId: 'silent-board',
      );
      final joiningRefresh = service.fetchDeviceCapabilities(
        deviceId: 'silent-board',
        refresh: true,
      );
      final capabilities = await discovery.timeout(const Duration(seconds: 6));
      expect(await joiningRefresh, same(capabilities));
      final attemptsBeforeRetry = transport.writes.length;
      transport.infoRepliesEnabled = true;
      final cached = await service.fetchDeviceCapabilities(
        deviceId: 'silent-board',
      );
      expect(transport.writes, hasLength(attemptsBeforeRetry));
      service.debugSetConnectedDevice('silent-board');
      final retry = service.fetchDeviceCapabilities(
        deviceId: 'silent-board',
        refresh: true,
      );
      await Future<void>.delayed(Duration.zero);
      final recovered = await retry;
      await service.dispose();
      await transport.packets.close();

      expect(capabilities.explicitlyReported, isFalse);
      expect(cached, same(capabilities));
      expect(capabilities.supportsFirmwareUpdate, isFalse);
      expect(attemptsBeforeRetry, 3);
      expect(recovered.supportsLitchcraft, isTrue);
      expect(
        trace.records.where(
          (record) => record['event'] == 'ble.capability_exhausted',
        ),
        hasLength(1),
      );
    },
  );

  test('versioned capability mask keeps history and coach distinct', () {
    final LitchDeviceCapabilities? historyOnly =
        LitchDeviceCapabilities.fromPayload(<String, Object?>{
          'i': <Object?>[3, 3, 255],
        });
    final LitchDeviceCapabilities? capabilities =
        LitchDeviceCapabilities.fromPayload(<String, Object?>{
          'i': <Object?>[3, 3, 511],
        });

    expect(historyOnly, isNotNull);
    expect(historyOnly!.supportsGameHistory, isTrue);
    expect(historyOnly.supportsCoachInsight, isFalse);
    expect(capabilities, isNotNull);
    expect(capabilities!.supportsGameHistory, isTrue);
    expect(capabilities.supportsCoachInsight, isTrue);
    expect(capabilities.supportsOwnerPairing, isFalse);
    final LitchDeviceCapabilities? pairing =
        LitchDeviceCapabilities.fromPayload(<String, Object?>{
          'i': <Object?>[3, 3, 0x200],
        });
    expect(pairing!.supportsOwnerPairing, isTrue);
  });

  test('only current capability contract grants session or update access', () {
    for (final contract in [0, 1, 2, 4]) {
      expect(
        LitchDeviceCapabilities.fromPayload({
          'i': [contract, 3, 0xffff],
        }),
        isNull,
      );
    }
    final current = LitchDeviceCapabilities.fromPayload({
      'i': [3, 3, 0xffff],
    })!;
    expect(current.supportsConnectionSession, isTrue);
    expect(current.supportsFirmwareUpdate, isTrue);
    expect(
      LitchDeviceCapabilities.unavailable.supportsConnectionSession,
      isFalse,
    );
    expect(LitchDeviceCapabilities.unavailable.supportsFirmwareUpdate, isFalse);
    expect(LitchDeviceCapabilities.unavailable.supportsSettings, isFalse);
    for (final malformed in [
      [3.0, 3, 31],
      ['3', 3, 31],
      [3, 3, 31, 0, 0],
      [3, -1, 31],
      [3, 256, 31],
    ]) {
      expect(LitchDeviceCapabilities.fromPayload({'i': malformed}), isNull);
    }
    final unknownRevision = LitchDeviceCapabilities.fromPayload({
      'i': [3, 255, 31],
    })!;
    expect(unknownRevision.hardwareLabel, 'Litch board');
    expect(unknownRevision.supportsFirmwareUpdate, isTrue);
  });

  test(
    'V1 ESP32-C3 capability refresh retains its session and diagnostics',
    () async {
      final board = _V1DiagnosticsBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      expect(await service.ensureProductReady('board-1'), isTrue);
      final capabilities = service.deviceCapabilities('board-1');
      expect(capabilities.hardwareRevision, LitchHardwareRevision.v1);
      expect(capabilities.supportsFirmwareUpdate, isFalse);
      expect(capabilities.supportsLitchcraft, isTrue);
      expect(board.infoRequests, 1);

      expect(
        await service.fetchDeviceCapabilities(
          deviceId: 'board-1',
          refresh: true,
        ),
        same(capabilities),
      );
      expect(board.infoRequests, 1);
      expect(
        await service.fetchBoardFirmwareVersion(deviceId: 'board-1'),
        board.runningVersion,
      );
      expect(
        (await service.fetchFirmwareProvenance(deviceId: 'board-1')).sha256,
        board.runningDigest,
      );
      final health = await service.fetchRuntimeHealth(deviceId: 'board-1');
      expect(health, isA<RuntimeHealthAvailable>());
      expect((health as RuntimeHealthAvailable).snapshot.revision, 1);
      expect(board.healthRequests, 1);
      expect(board.objectCodes, isNotEmpty);
      expect(board.objectCodes.toSet(), {0x32});
      expect(board.imageWrites, 0);
      expect(board.uploadCommands, isEmpty);
      expect(service.productReady, isTrue);
      expect(service.isConnected, isTrue);

      await service.disconnect();
      await connectBoard(service, board);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(
        board.infoRequests,
        2,
        reason: 'A fresh connection must rediscover.',
      );
    },
  );

  test('bulk Litchcraft transfer is additive and explicit', () {
    final capabilities = LitchDeviceCapabilities.fromPayload(<String, Object?>{
      'i': <Object?>[3, 3, 0xc00],
    });
    expect(capabilities, isNotNull);
    expect(capabilities!.supportsLitchcraft, isTrue);
    expect(capabilities.supportsLitchcraftBulk, isTrue);
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 3, 0x400],
      })!.supportsLitchcraftBulk,
      isFalse,
    );
  });

  test('lightweight Litchcraft inspection is additive and explicit', () {
    final capabilities = LitchDeviceCapabilities.fromPayload(<String, Object?>{
      'i': <Object?>[3, 3, 0x1c00],
    });
    expect(capabilities, isNotNull);
    expect(capabilities!.supportsLitchcraftInspect, isTrue);
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 3, 0x0c00],
      })!.supportsLitchcraftInspect,
      isFalse,
    );
  });

  test('animated Litchcraft requires its explicit bit and inspection', () {
    final capabilities = LitchDeviceCapabilities.fromPayload(<String, Object?>{
      'i': <Object?>[3, 3, 0x7c00],
    });
    expect(capabilities, isNotNull);
    expect(capabilities!.supportsLitchcraftAnimation, isTrue);
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 3, 0x4c00],
      })!.supportsLitchcraftAnimation,
      isFalse,
    );
  });

  test('shake erase requires its explicit bit and V3 motion hardware', () {
    final capabilities = LitchDeviceCapabilities.fromPayload(<String, Object?>{
      'i': <Object?>[3, 3, 0xfc7f],
    });
    expect(capabilities, isNotNull);
    expect(capabilities!.supportsLitchcraftShakeErase, isTrue);
    final v3_3 = LitchDeviceCapabilities.fromPayload(<String, Object?>{
      'i': <Object?>[3, 4, 0xfc7f],
    });
    expect(v3_3!.hardwareRevision, LitchHardwareRevision.v3_3);
    expect(v3_3.hardwareLabel, 'V3.3 board');
    expect(v3_3.supportsLitchcraftShakeErase, isTrue);
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 3, 0x7c7f],
      })!.supportsLitchcraftShakeErase,
      isFalse,
    );
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 2, 0xfc7f],
      })!.supportsLitchcraftShakeErase,
      isFalse,
    );
  });

  test('unsupported contracts and oversized capability masks fail closed', () {
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[1, 2, 383],
      }),
      isNull,
    );
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 2, 65536],
      }),
      isNull,
    );
    expect(
      LitchDeviceCapabilities.fromPayload(<String, Object?>{
        'i': <Object?>[3, 2, 65536],
      }),
      isNull,
    );
  });

  test('unknown info command grants no product or update permission', () async {
    final _CapabilityBle transport = _CapabilityBle('{"e":"unknown_cmd"}');
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: transport,
    );
    service.debugSetConnectedDevice('unsupported-board');
    addTearDown(() async {
      await service.dispose();
      await transport.packets.close();
    });

    final LitchDeviceCapabilities capabilities = await service
        .fetchDeviceCapabilities(deviceId: 'unsupported-board');

    expect(capabilities.explicitlyReported, isFalse);
    expect(capabilities.supportsFirmwareUpdate, isFalse);
    expect(service.productReady, isFalse);
    expect(await service.sendIdentify('unsupported-board', 1), isFalse);
    expect(
      await service.sendConsoleLine('unsupported-board', 'version'),
      isFalse,
    );
    expect(transport.writes.every((write) => write['cmd'] == 'info'), isTrue);
    expect(
      await service.fetchDeviceCapabilities(deviceId: 'unsupported-board'),
      same(capabilities),
    );
    expect(transport.writes, hasLength(1));
  });

  // Current Health/recheck recovery is exercised through the native shared
  // session in virtual_litch_interactive_sync_journey_test.dart. The removed
  // direct characteristic health checks represented unsupported peer epochs.
}
