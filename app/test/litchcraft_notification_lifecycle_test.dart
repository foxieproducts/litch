import 'support/memory_ota_target_store.dart';
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_reactive_ble/src/connected_device_operation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
// The production plugin seam uses its pinned platform-interface dependency.
// ignore: depend_on_referenced_packages
import 'package:reactive_ble_platform_interface/reactive_ble_platform_interface.dart';

final _characteristic = CharacteristicInstance(
  characteristicId: Uuid.parse('6f2a20d1-89df-4ac6-a9f2-53f6f7db0106'),
  characteristicInstanceId: 'craft',
  serviceId: Uuid.parse('6f2a20d1-89df-4ac6-a9f2-53f6f7db0100'),
  serviceInstanceId: 'base',
  deviceId: 'board',
);

/// Only native notification completion timing is controlled. Subscription
/// setup and disposal run through the deployed plugin's real Repeater.
class _NativePlatform implements ReactiveBlePlatform {
  final updates = StreamController<CharacteristicValue>.broadcast(sync: true);
  final firstDisableFinished = Completer<void>();
  final disconnected = Completer<void>();
  int disableCalls = 0;
  int enableCalls = 0;
  bool notificationsEnabled = false;
  int silentEnables = 0;

  @override
  Stream<CharacteristicValue> get charValueUpdateStream => updates.stream;

  @override
  Stream<void> subscribeToNotifications(CharacteristicInstance characteristic) {
    enableCalls++;
    notificationsEnabled = enableCalls > silentEnables;
    return Stream<void>.value(null);
  }

  @override
  Future<void> stopSubscribingToNotifications(
    CharacteristicInstance characteristic,
  ) async {
    disableCalls++;
    if (disableCalls == 1) await firstDisableFinished.future;
    notificationsEnabled = false;
  }

  void emit(List<int> bytes) {
    if (!notificationsEnabled) return;
    updates.add(
      CharacteristicValue(
        characteristic: _characteristic,
        result: Result.success(bytes),
      ),
    );
  }

  Future<void> dispose() async {
    if (!firstDisableFinished.isCompleted) firstDisableFinished.complete();
    if (!disconnected.isCompleted) disconnected.complete();
    await updates.close();
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _UnusedSdk implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _MemoryTrace implements AppTraceSink {
  final List<(String, Map<String, Object?>)> records =
      <(String, Map<String, Object?>)>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    records.add((event, Map<String, Object?>.from(fields)));
  }
}

/// Explicit protocol responses, without a second implementation of the board's
/// painting logic. Packet transport is the real notification operator above.
class _Peer extends LitchBleService {
  _Peer(this.platform)
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _UnusedSdk()) {
    operation = ConnectedDeviceOperationImpl(blePlatform: platform);
  }

  final _NativePlatform platform;
  late final ConnectedDeviceOperationImpl operation;
  final requests = <int>[];
  int revision = 5;
  int session = 42;
  int token = 0;
  int snapshotBeginsToDrop = 0;
  int exitFailures = 0;
  int disconnectCalls = 0;
  bool interceptDisconnect = false;
  bool capabilitiesUnavailable = false;
  Completer<void>? capabilityReady;
  Completer<void>? orientationReady;

  @override
  bool get isConnected => true;
  @override
  int get connectionSessionToken => token;
  @override
  String? get connectedId => 'board';

  // This fixture isolates the pinned notification operator and controller
  // repair policy; it supplies a domain boundary, not a second GATT session.
  @override
  Future<bool> litchcraftControlAvailable(String deviceId) async =>
      !capabilitiesUnavailable && (capabilityReady?.isCompleted ?? true);
  @override
  Future<ConnectionSession?> openConnectionSession(String deviceId) async =>
      null;

  @override
  Future<void> disconnect() async {
    disconnectCalls++;
    if (!interceptDisconnect) await super.disconnect();
  }

  @override
  Object? latestSettingValue(String key) =>
      key == 'BOARD_ROTATE' ? (orientationReady == null ? false : null) : false;

  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    await orientationReady?.future;
    return 1;
  }

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    await capabilityReady?.future;
    if (capabilitiesUnavailable) return LitchDeviceCapabilities.unavailable;
    return const LitchDeviceCapabilities(
      contractVersion: 3,
      hardwareRevision: LitchHardwareRevision.v3,
      capabilityMask:
          LitchDeviceCapabilities.litchcraftBit |
          LitchDeviceCapabilities.litchcraftInspectBit,
      explicitlyReported: true,
    );
  }

  @override
  Stream<List<int>> litchcraftPackets(String deviceId) => operation
      .subscribeToCharacteristic(_characteristic, platform.disconnected.future);

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      capabilitiesUnavailable
      ? LitchDeviceCapabilities.unavailable
      : capabilityReady != null && !capabilityReady!.isCompleted
      ? super.deviceCapabilities(deviceId)
      : const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v3,
          capabilityMask:
              LitchDeviceCapabilities.litchcraftBit |
              LitchDeviceCapabilities.litchcraftInspectBit,
          explicitlyReported: true,
        );

  @override
  Future<void> writeLitchcraftPacket(
    String deviceId,
    List<int> bytes, {
    bool withResponse = true,
  }) async {
    return _respondPacket(bytes);
  }

  Future<void> _respondPacket(List<int> bytes) async {
    final opcode = bytes[1];
    final request = bytes[6] | bytes[7] << 8;
    requests.add(opcode);
    if (opcode == LitchcraftProtocol.exit && exitFailures > 0) {
      exitFailures--;
      throw StateError('controlled Exit transport failure');
    }
    _emit(LitchcraftProtocol.state, request, [0, 1, 3, 0, 0, 0, 15, 0]);
    if (opcode != LitchcraftProtocol.snapshot) return;
    if (snapshotBeginsToDrop > 0) {
      snapshotBeginsToDrop--;
    } else {
      _emit(LitchcraftProtocol.snapshotBegin, request, [
        0,
        1,
        3,
        0,
        0,
        0,
        15,
        0,
      ]);
    }
    _emit(LitchcraftProtocol.brush, request, [255, 116, 48, 0, 15]);
    for (int offset = 0; offset < 256; offset += 2) {
      _emit(LitchcraftProtocol.snapshotChunk, request, [
        ...LitchcraftProtocol.u16(offset),
        0,
        0,
        0,
        0,
        0,
        0,
      ]);
    }
    _emit(LitchcraftProtocol.snapshotEnd, request, const []);
  }

  void _emit(int event, int request, List<int> payload) {
    final bytes = Uint8List(12 + payload.length);
    final data = ByteData.sublistView(bytes);
    bytes[0] = LitchcraftProtocol.magic;
    bytes[1] = event;
    data.setUint32(2, session, Endian.little);
    data.setUint32(6, revision, Endian.little);
    data.setUint16(10, request, Endian.little);
    bytes.setRange(12, bytes.length, payload);
    platform.emit(bytes);
  }
}

Future<void> _eventTurns() async {
  for (int i = 0; i < 10; i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

Future<void> _pumpTransport(WidgetTester tester) async {
  for (int i = 0; i < 20; i++) {
    await tester.pump(const Duration(milliseconds: 1));
  }
}

void main() {
  // The pinned native notification operator and controller repair policy are
  // exercised at a scripted domain boundary. Actual GATT/session replacement
  // remains covered by current native/session and connection recovery journeys.
  for (final authority in [
    'same-session',
    'fresh-token',
    'different-session',
  ]) {
    testWidgets(
      'Android canvas repair ${authority == 'different-session' ? 'admits fresh session' : 'rejects stale revision'} ($authority)',
      (tester) async {
        debugDefaultTargetPlatformOverride = TargetPlatform.android;
        addTearDown(() => debugDefaultTargetPlatformOverride = null);
        final platform = _NativePlatform();
        platform.firstDisableFinished.complete();
        final peer = _Peer(platform);
        final trace = _MemoryTrace();
        final controller = LitchcraftController(peer, appTrace: trace);
        unawaited(controller.enter());
        await _pumpTransport(tester);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.revision, 5);
        peer.revision = 1;
        if (authority == 'fresh-token') peer.token++;
        if (authority == 'different-session') peer.session--;
        peer.requests.clear();
        unawaited(controller.retryConnection());
        for (int second = 0; second < 30; second++) {
          await tester.pump(const Duration(seconds: 1));
          await tester.runAsync(_eventTurns);
          await _pumpTransport(tester);
        }
        final freshSession = authority == 'different-session';
        final revision = controller.revision;
        final exhaustedStatus = controller.status;
        final exhaustedRecoveryRequired = controller.connectionRecoveryRequired;
        final exhaustedEnableCalls = platform.enableCalls;
        final exhaustedDiagnostics = peer.supportSessionDiagnostics;
        if (!freshSession) {
          // Four silent repairs cannot authorize an older same-session
          // snapshot. The bounded recovery action starts a new sequence; a
          // now-authoritative revision then recovers without overwriting the
          // retained canvas with the stale prefix.
          expect(exhaustedStatus, LitchcraftStatus.error);
          expect(exhaustedRecoveryRequired, isTrue);
          expect(
            exhaustedEnableCalls,
            6,
          ); // initial link + retry + four repairs
          expect(exhaustedDiagnostics.notificationRepairOperations, 1);
          expect(exhaustedDiagnostics.notificationRepairAttempts, 4);
          expect(exhaustedDiagnostics.notificationRepairRecovered, 0);
          expect(exhaustedDiagnostics.notificationRepairExhausted, 1);
          expect(exhaustedDiagnostics.notificationRepairCancelled, 0);
          expect(exhaustedDiagnostics.notificationRepairPending, 0);
          peer.revision = 6;
          unawaited(controller.retryConnection());
          for (
            int second = 0;
            second < 20 && controller.revision != 6;
            second++
          ) {
            await tester.pump(const Duration(seconds: 1));
            await tester.runAsync(_eventTurns);
            await _pumpTransport(tester);
          }
        }
        final status = controller.status;
        final recoveredRevision = controller.revision;
        final pending = controller.pending;
        final enableCalls = platform.enableCalls;
        final requests = List<int>.of(peer.requests);
        final traceRecords = List.of(trace.records);
        controller.dispose();
        await tester.runAsync(_eventTurns);
        await peer.dispose();
        await platform.dispose();
        debugDefaultTargetPlatformOverride = null;
        // A reboot owns a fresh session. Reusing the same session with an
        // older revision models stale frames, not permission to regress art.
        expect(
          status,
          LitchcraftStatus.live,
          reason:
              'received complete revision=1 snapshots, enables=$enableCalls requests=$requests pending=$pending retainedRevision=$revision',
        );
        expect(revision, freshSession ? 1 : 5);
        expect(recoveredRevision, freshSession ? 1 : 6);
        expect(enableCalls, freshSession ? 2 : greaterThan(2));
        expect(
          requests.where((op) => op == LitchcraftProtocol.snapshot),
          freshSession ? hasLength(1) : isNotEmpty,
        );
        expect(pending, isFalse);
        final rejectedOlderRevision = traceRecords.where(
          (record) =>
              record.$1 == 'litchcraft.snapshot_begin_rejected' &&
              record.$2['reason'] == 'older_revision' &&
              record.$2['request_id'] is int &&
              record.$2['generation'] is int &&
              record.$2['connection_epoch'] is int,
        );
        expect(
          rejectedOlderRevision,
          freshSession ? isEmpty : isNotEmpty,
          reason:
              'A rejected complete canvas must identify its authority boundary without recording pixel data',
        );
      },
    );
  }

  for (final missingBegin in [false, true]) {
    testWidgets(
      'native notification operator repairs ${missingBegin ? 'missing SnapshotBegin' : 'silent canvas notifications'} while transport stays connected',
      (tester) async {
        debugDefaultTargetPlatformOverride = TargetPlatform.android;
        addTearDown(() => debugDefaultTargetPlatformOverride = null);
        final platform = _NativePlatform()
          ..silentEnables = missingBegin ? 0 : 2;
        platform.firstDisableFinished.complete();
        final peer = _Peer(platform)
          ..snapshotBeginsToDrop = missingBegin ? 1 : 0;
        final controller = LitchcraftController(peer);
        unawaited(controller.enter());
        await _pumpTransport(tester);
        expect(controller.status, LitchcraftStatus.connecting);
        expect(peer.isConnected, isTrue);
        for (int second = 0; second < 40; second++) {
          await tester.pump(const Duration(seconds: 1));
          await tester.runAsync(_eventTurns);
          await _pumpTransport(tester);
        }
        final status = controller.status;
        final pending = controller.pending;
        final enabled = platform.notificationsEnabled;
        final diagnostics = peer.supportSessionDiagnostics;
        controller.dispose();
        await tester.runAsync(_eventTurns);
        await peer.dispose();
        await platform.dispose();
        debugDefaultTargetPlatformOverride = null;
        expect(platform.enableCalls, missingBegin ? 2 : 3);
        expect(enabled, isTrue);
        expect(status, LitchcraftStatus.live);
        expect(pending, isFalse);
        expect(diagnostics.notificationRepairOperations, 1);
        expect(diagnostics.notificationRepairAttempts, missingBegin ? 1 : 2);
        expect(diagnostics.notificationRepairRecovered, 1);
        expect(diagnostics.notificationRepairExhausted, 0);
        expect(diagnostics.notificationRepairCancelled, 0);
        expect(diagnostics.notificationRepairPending, 0);
      },
    );
  }

  testWidgets(
    'disposing cancels a scheduled notification repair exactly once',
    (tester) async {
      final platform = _NativePlatform();
      platform.firstDisableFinished.complete();
      final peer = _Peer(platform);
      final controller = LitchcraftController(peer);
      unawaited(controller.enter());
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);

      platform.updates.addError(StateError('controlled notification failure'));
      await tester.pump();
      final pending = peer.supportSessionDiagnostics;
      expect(pending.notificationRepairOperations, 1);
      expect(pending.notificationRepairAttempts, 0);
      expect(pending.notificationRepairPending, 1);

      controller.dispose();
      final cancelled = peer.supportSessionDiagnostics;
      await peer.dispose();
      await platform.dispose();

      expect(cancelled.notificationRepairOperations, 1);
      expect(cancelled.notificationRepairAttempts, 0);
      expect(cancelled.notificationRepairRecovered, 0);
      expect(cancelled.notificationRepairExhausted, 0);
      expect(cancelled.notificationRepairCancelled, 1);
      expect(cancelled.notificationRepairPending, 0);
    },
  );

  testWidgets('automatic Exit repair settles recovered exactly once', (
    tester,
  ) async {
    final platform = _NativePlatform();
    platform.firstDisableFinished.complete();
    final peer = _Peer(platform)..exitFailures = 1;
    final controller = LitchcraftController(peer);
    unawaited(controller.enter());
    await _pumpTransport(tester);
    expect(controller.status, LitchcraftStatus.live);

    await expectLater(controller.leave(), throwsA(isA<StateError>()));
    expect(peer.supportSessionDiagnostics.notificationRepairPending, 1);
    await tester.pump(const Duration(milliseconds: 600));
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    final diagnostics = peer.supportSessionDiagnostics;

    expect(controller.boardActive, isFalse);
    expect(diagnostics.notificationRepairOperations, 1);
    expect(diagnostics.notificationRepairAttempts, 1);
    expect(diagnostics.notificationRepairRecovered, 1);
    expect(diagnostics.notificationRepairExhausted, 0);
    expect(diagnostics.notificationRepairCancelled, 0);
    expect(diagnostics.notificationRepairPending, 0);
    controller.dispose();
    await peer.dispose();
    await platform.dispose();
  });

  testWidgets('four automatic Exit failures settle exhausted exactly once', (
    tester,
  ) async {
    final platform = _NativePlatform();
    platform.firstDisableFinished.complete();
    final peer = _Peer(platform)..exitFailures = 5;
    final controller = LitchcraftController(peer);
    unawaited(controller.enter());
    await _pumpTransport(tester);
    expect(controller.status, LitchcraftStatus.live);

    await expectLater(controller.leave(), throwsA(isA<StateError>()));
    for (int second = 0; second < 30; second++) {
      await tester.pump(const Duration(seconds: 1));
      await tester.runAsync(_eventTurns);
      await _pumpTransport(tester);
    }
    final diagnostics = peer.supportSessionDiagnostics;

    expect(controller.status, LitchcraftStatus.error);
    expect(diagnostics.notificationRepairOperations, 1);
    expect(diagnostics.notificationRepairAttempts, 4);
    expect(diagnostics.notificationRepairRecovered, 0);
    expect(diagnostics.notificationRepairExhausted, 1);
    expect(diagnostics.notificationRepairCancelled, 0);
    expect(diagnostics.notificationRepairPending, 0);
    controller.dispose();
    await peer.dispose();
    await platform.dispose();
  });

  testWidgets(
    'unavailable capabilities cannot recover product notification authority',
    (tester) async {
      final platform = _NativePlatform();
      platform.firstDisableFinished.complete();
      final peer = _Peer(platform);
      final controller = LitchcraftController(peer);
      unawaited(controller.enter());
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);

      platform.updates.addError(StateError('controlled notification failure'));
      await tester.pump();
      peer.capabilitiesUnavailable = true;
      for (int second = 0; second < 30; second++) {
        await tester.pump(const Duration(seconds: 1));
        await tester.runAsync(_eventTurns);
        await _pumpTransport(tester);
      }
      final diagnostics = peer.supportSessionDiagnostics;

      expect(controller.status, LitchcraftStatus.unsupported);
      expect(diagnostics.notificationRepairOperations, 1);
      expect(diagnostics.notificationRepairAttempts, 4);
      expect(diagnostics.notificationRepairRecovered, 0);
      expect(diagnostics.notificationRepairExhausted, 1);
      expect(diagnostics.notificationRepairCancelled, 0);
      expect(diagnostics.notificationRepairPending, 0);
      controller.dispose();
      await peer.dispose();
      await platform.dispose();
    },
  );

  test(
    'native notification disable drains before replacement enable',
    () async {
      final platform = _NativePlatform();
      final operation = ConnectedDeviceOperationImpl(blePlatform: platform);
      final first = operation
          .subscribeToCharacteristic(
            _characteristic,
            platform.disconnected.future,
          )
          .listen((_) {});
      await _eventTurns();
      bool cancellationReturned = false;
      final cancelling = first.cancel().then(
        (_) => cancellationReturned = true,
      );
      await _eventTurns();
      final returnedBeforeNativeDisable = cancellationReturned;
      final repaired = cancelling.then(
        (_) => operation
            .subscribeToCharacteristic(
              _characteristic,
              platform.disconnected.future,
            )
            .listen((_) {}),
      );
      await _eventTurns();
      final startsBeforeDisableFinished = platform.enableCalls;
      platform.firstDisableFinished.complete();
      final second = await repaired;
      await _eventTurns();
      final repairRemainedEnabled = platform.notificationsEnabled;
      await second.cancel();
      await platform.dispose();

      expect(
        returnedBeforeNativeDisable,
        isFalse,
        reason:
            'A completed cancel is the controller\'s native handoff boundary',
      );
      expect(
        startsBeforeDisableFinished,
        1,
        reason: 'Old native disable must never overlap the replacement enable',
      );
      expect(
        repairRemainedEnabled,
        isTrue,
        reason: 'Old cleanup must not silence the repaired Litchcraft channel',
      );
    },
  );

  testWidgets('Litchcraft retry retains native ownership until cleanup settles', (
    tester,
  ) async {
    final platform = _NativePlatform();
    final peer = _Peer(platform);
    final controller = LitchcraftController(peer);
    unawaited(controller.enter());
    await _pumpTransport(tester);
    expect(controller.status, LitchcraftStatus.live);
    expect(controller.boardActive, isTrue);
    final canvas = controller.canvas;
    final revision = controller.revision;

    platform.updates.addError(StateError('controlled notification failure'));
    await tester.pump();
    expect(controller.status, LitchcraftStatus.error);
    await tester.pump(const Duration(milliseconds: 600));
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    final startsDuringCleanup = platform.enableCalls;
    final requestsDuringCleanup = peer.requests.length;
    platform.firstDisableFinished.complete();
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    await tester.pump(const Duration(milliseconds: 300));
    await _pumpTransport(tester);
    // A fresh channel must answer after cleanup, not merely complete one
    // optimistic inspection before the old subscription turns it off.
    if (controller.status == LitchcraftStatus.live) {
      unawaited(controller.coherentSnapshot());
    }
    await _pumpTransport(tester);
    final recoveredStatus = controller.status;
    final stillEnabled = platform.notificationsEnabled;
    final recoveredCanvas = controller.canvas;
    final recoveredRevision = controller.revision;
    controller.dispose();
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    await peer.dispose();
    await platform.dispose();

    expect(startsDuringCleanup, 1);
    expect(
      requestsDuringCleanup,
      2,
      reason:
          'No repair request may use a subscription whose predecessor is still disabling',
    );
    expect(stillEnabled, isTrue);
    expect(
      recoveredStatus,
      LitchcraftStatus.live,
      reason:
          'enables=${platform.enableCalls} disables=${platform.disableCalls} '
          'requests=${peer.requests} error=${controller.errorMessage}',
    );
    expect(recoveredCanvas, canvas);
    expect(recoveredRevision, revision);
  });

  testWidgets('stalled native cleanup does not leave Connecting indefinitely', (
    tester,
  ) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    addTearDown(() => debugDefaultTargetPlatformOverride = null);
    final platform = _NativePlatform();
    final peer = _Peer(platform)..interceptDisconnect = true;
    final trace = _MemoryTrace();
    final controller = LitchcraftController(peer, appTrace: trace);
    unawaited(controller.enter());
    await _pumpTransport(tester);
    expect(controller.status, LitchcraftStatus.live);
    platform.updates.addError(StateError('controlled notification failure'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 600));
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    expect(platform.disableCalls, 1);
    for (int i = 0; i < 120; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    final stalledStatus = controller.status;
    final stalledPending = controller.pending;
    final startsWhileStalled = platform.enableCalls;
    platform.firstDisableFinished.complete();
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    final afterLateCleanup = controller.status;
    unawaited(controller.retryConnection());
    await _pumpTransport(tester);
    await tester.pump(const Duration(milliseconds: 300));
    await _pumpTransport(tester);
    final retriedStatus = controller.status;
    controller.dispose();
    await tester.runAsync(_eventTurns);
    await _pumpTransport(tester);
    await peer.dispose();
    await platform.dispose();
    debugDefaultTargetPlatformOverride = null;

    expect(
      stalledStatus,
      isNot(LitchcraftStatus.connecting),
      reason:
          'Native cleanup can fail to acknowledge while the radio stays connected',
    );
    expect(
      stalledPending,
      isFalse,
      reason: 'A timed-out native boundary must release the UI handoff slot',
    );
    expect(
      startsWhileStalled,
      1,
      reason:
          'Timeout never grants permission to overlap pending native cleanup',
    );
    expect(peer.disconnectCalls, 1);
    expect(
      afterLateCleanup,
      LitchcraftStatus.error,
      reason:
          'Late cleanup cannot revive an already retired connection attempt',
    );
    expect(
      retriedStatus,
      LitchcraftStatus.live,
      reason:
          'Explicit retry after native cleanup must regain the same artwork',
    );
    expect(
      trace.records.any(
        (record) =>
            record.$1 == 'litchcraft.preparation_timeout' &&
            record.$2['stage'] == 'notification_cleanup',
      ),
      isTrue,
    );
    expect(
      trace.records.any(
        (record) =>
            record.$1 == 'litchcraft.state' && record.$2['status'] == 'live',
      ),
      isTrue,
    );
    expect(
      trace.records.every(
        (record) =>
            !record.$2.containsKey('device_id') &&
            !record.$2.containsKey('board_id'),
      ),
      isTrue,
    );
  });

  testWidgets(
    'stalled logical cleanup retires the poisoned BLE generation once',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final platform = _NativePlatform();
      final peer = _Peer(platform)..interceptDisconnect = true;
      final controller = LitchcraftController(peer);
      unawaited(controller.enter());
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);

      platform.updates.addError(StateError('controlled notification failure'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 600));
      await tester.runAsync(_eventTurns);
      await _pumpTransport(tester);
      expect(platform.disableCalls, 1);
      await tester.pump(const Duration(seconds: 7));
      await tester.runAsync(_eventTurns);
      await _pumpTransport(tester);

      expect(
        peer.disconnectCalls,
        1,
        reason:
            'one cleanup timeout must restart the BLE generation instead of preserving a connected-but-unusable lease',
      );

      platform.firstDisableFinished.complete();
      controller.dispose();
      await tester.runAsync(_eventTurns);
      await _pumpTransport(tester);
      await peer.dispose();
      await platform.dispose();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final stage in ['capability', 'orientation']) {
    testWidgets(
      'stalled $stage preparation is bounded and late results stay retired',
      (tester) async {
        final platform = _NativePlatform();
        platform.firstDisableFinished.complete();
        final peer = _Peer(platform);
        final ready = Completer<void>();
        if (stage == 'capability') {
          peer.capabilityReady = ready;
        } else {
          peer.orientationReady = ready;
        }
        final controller = LitchcraftController(peer);
        final canvas = controller.canvas;
        unawaited(controller.enter());
        await _pumpTransport(tester);
        expect(controller.status, LitchcraftStatus.connecting);
        await tester.pump(const Duration(seconds: 6));
        await _pumpTransport(tester);
        final timedOutStatus = controller.status;
        final pendingAtTimeout = controller.pending;
        final requestsAtTimeout = peer.requests.length;
        ready.complete();
        await _pumpTransport(tester);
        final afterLatePreparation = controller.status;
        unawaited(controller.retryConnection());
        await tester.runAsync(_eventTurns);
        await _pumpTransport(tester);
        final retriedStatus = controller.status;
        final recoveredCanvas = controller.canvas;
        controller.dispose();
        await tester.runAsync(_eventTurns);
        await _pumpTransport(tester);
        await peer.dispose();
        await platform.dispose();

        expect(timedOutStatus, LitchcraftStatus.error);
        expect(pendingAtTimeout, isFalse);
        expect(requestsAtTimeout, stage == 'orientation' ? 1 : 0);
        expect(afterLatePreparation, LitchcraftStatus.error);
        expect(retriedStatus, LitchcraftStatus.live);
        expect(recoveredCanvas, canvas);
      },
    );
  }
}
