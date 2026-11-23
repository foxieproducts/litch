import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/bluetooth_pairing_recovery_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/bluetooth_pairing_recovery.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'health_render_capture.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

const _boardId = 'AA:BB:CC:DD:EE:01';
const _otherId = 'AA:BB:CC:DD:EE:02';

final class FakeBluetoothPairingRecoveryGateway
    implements BluetoothPairingRecoveryGateway {
  final events = StreamController<BluetoothPairingKeyMissing>.broadcast(
    sync: true,
  );
  final watches = <(String?, int)>[];
  final repairs = <(String, int)>[];
  final removal = Completer<BluetoothPairingRepairResult>();
  bool supported = true;
  int settingsOpened = 0;
  Completer<void>? nullWatchGate;

  @override
  Stream<BluetoothPairingKeyMissing> get keyMissing => events.stream;
  @override
  Stream<BluetoothPairingBondFailed> get bondFailures => const Stream.empty();
  @override
  Future<void> watchDevice(String? deviceId, int generation) async {
    watches.add((deviceId, generation));
    if (deviceId == null) await nullWatchGate?.future;
  }

  void emit({String? deviceId, int? generation}) {
    final watch = watches.last;
    events.add(
      BluetoothPairingKeyMissing(deviceId ?? watch.$1!, generation ?? watch.$2),
    );
  }

  @override
  Future<bool> canRepairPairing() async => supported;
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
  ) {
    repairs.add((deviceId, generation));
    return removal.future;
  }

  @override
  Future<bool> openBluetoothSettings() async {
    ++settingsOpened;
    return true;
  }

  @override
  Future<void> dispose() => events.close();
}

/// Reuses the real shared-session board replies; only SDK connection admission
/// is recorded here so advertising/direct fallbacks remain observable.
final class _RecoveryBoard extends OwnerPairingBoard {
  final modes = <String>[];
  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    modes.add('direct');
    return super.connectToDevice(id: id);
  }

  @override
  Stream<ConnectionStateUpdate> connectToAdvertisingDevice({
    required String id,
    required List<Uuid> withServices,
    required Duration prescanDuration,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    modes.add('advertising');
    return super.connectToDevice(id: id);
  }

  void fail(String message) {
    connections.last.add(
      ConnectionStateUpdate(
        deviceId: _boardId,
        connectionState: DeviceConnectionState.disconnected,
        failure: GenericFailure(
          code: ConnectionError.failedToConnect,
          message: message,
        ),
      ),
    );
  }
}

final class _ChoiceRecoveryService extends LitchBleService {
  _ChoiceRecoveryService(
    _RecoveryBoard board,
    BluetoothPairingRecoveryGateway gateway,
  ) : super(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
  bool holdNextStop = false;
  final heldStop = Completer<void>();
  @override
  Future<void> stopScan() async {
    if (holdNextStop) {
      holdNextStop = false;
      await heldStop.future;
    }
    await super.stopScan();
  }

  @override
  Future<void> startScan() async {}
  @override
  Stream<List<LitchBoard>> get boardsStream => Stream.value(const [
    LitchBoard(id: _boardId, name: 'Board A', rssi: -40),
    LitchBoard(id: _otherId, name: 'Board B', rssi: -40),
  ]);
}

Future<void> _turns([int count = 30]) async {
  for (var i = 0; i < count; ++i) {
    await Future<void>.delayed(Duration.zero);
  }
}

Future<void> _connectionAdmitted(_RecoveryBoard board, int count) async {
  for (var i = 0; i < 100 && board.connections.length < count; ++i) {
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
  expect(board.connections, hasLength(count));
}

LitchBleService _service(
  _RecoveryBoard board,
  BluetoothPairingRecoveryGateway gateway,
) {
  final service = LitchBleService(
    ble: board,
    pairingRecoveryGateway: gateway,
    pendingOtaStore: MemoryOtaTargetStore(),
  );
  addTearDown(() => disposeBoard(service, board));
  return service;
}

Future<void> _staleBeforeConnect(
  LitchBleService service,
  _RecoveryBoard board,
) async {
  final connecting = service.connect(_boardId);
  await _connectionAdmitted(board, 1);
  board.fail('$iosPairingKeyMissingMarker\nOriginal native diagnostic');
  expect(await connecting, isFalse);
  await _turns();
  expect(service.pairingRecovery?.stage, BluetoothPairingRecoveryStage.ready);
}

Widget _screen({
  required BluetoothPairingRecoveryState state,
  Future<bool> Function()? onRepair,
  Future<bool> Function()? onRetry,
  Future<bool> Function()? onOpenSettings,
  VoidCallback? onExit,
  Widget? support,
}) => MaterialApp(
  theme: buildLitchTheme(),
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  builder: (context, child) => RepaintBoundary(
    key: const ValueKey('pairing-recovery-review-root'),
    child: MediaQuery(
      data: MediaQuery.of(
        context,
      ).copyWith(textScaler: const TextScaler.linear(2)),
      child: child!,
    ),
  ),
  home: BluetoothPairingRecoveryScreen(
    state: state,
    boardName: 'Foxie Litch',
    onRepair: onRepair ?? () async => false,
    onRetry: onRetry ?? () async => false,
    onOpenSettings: onOpenSettings ?? () async => false,
    onExit: onExit ?? () {},
    support: support,
  ),
);

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues({});
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'litch.test',
      version: '0.9.0',
      buildNumber: '13',
      buildSignature: 'test',
    );
    debugDefaultTargetPlatformOverride = null;
  });
  tearDown(() => debugDefaultTargetPlatformOverride = null);

  for (final early in [false, true]) {
    test(
      'failed native bond during saved-owner verification stops doomed retry early=$early',
      () async {
        const channel = MethodChannel('test/litch-owner-bond-failure');
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        Map<dynamic, dynamic>? watch;
        messenger.setMockMethodCallHandler(channel, (call) async {
          if (call.method == 'watchDevice') watch = call.arguments as Map;
          return call.method == 'canRepairPairing' ? false : null;
        });
        addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
        final gate = Completer<void>();
        final board = _RecoveryBoard()
          ..state = 6
          ..flags = 5
          ..capabilityReplyGate = early ? gate : null;
        final gateway = MethodChannelBluetoothPairingRecovery(channel: channel);
        final service = _service(board, gateway);
        await connectBoard(service, board, deviceId: _boardId);
        final preparing = service.ensureProductReady(_boardId);
        await _turns();
        if (!early) expect(service.ownerPairingStatus?.ownerRetained, isTrue);
        expect(board.pairingCommands, isEmpty);
        // Recorded A26 sequence: peripheral security request, user Pair, native
        // BOND_BONDING -> BOND_NONE, while GATT remains connected. The native
        // event is evidence of failure, not evidence of why keys differ.
        await messenger.handlePlatformMessage(
          channel.name,
          const StandardMethodCodec().encodeMethodCall(
            MethodCall('pairingBondFailed', {
              ...watch!,
              'source': 'android_bond_failed',
            }),
          ),
          (_) {},
        );
        if (early) {
          expect(service.pairingRecovery, isNull);
          gate.complete();
        }
        await _turns();
        expect(service.pairingRecovery, isNotNull);
        expect(
          service.pairingRecovery?.reason,
          BluetoothPairingRecoveryReason.ownerVerificationFailed,
        );
        expect(service.pairingRecovery?.canRepair, isFalse);
        expect(await preparing, isFalse);
        expect(service.productReady, isFalse);
        expect(service.isConnected, isFalse);
        expect(await service.connect(_boardId), isFalse);
        expect(board.connections, hasLength(1));
        expect(board.pairingCommands, isEmpty);
      },
    );
  }

  test(
    'bond failure cannot reclassify fresh consent or healthy owner as stale',
    () async {
      const channel = MethodChannel('test/litch-fresh-bond-failure');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      Map<dynamic, dynamic>? watch;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'watchDevice') watch = call.arguments as Map;
        return null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final board = _RecoveryBoard();
      final service = _service(
        board,
        MethodChannelBluetoothPairingRecovery(channel: channel),
      );
      await connectBoard(service, board, deviceId: _boardId);
      final preparing = service.ensureProductReady(_boardId);
      await _turns();
      expect(board.pairingCommands, hasLength(1));
      Future<void> failed(Map<dynamic, dynamic> target) =>
          messenger.handlePlatformMessage(
            channel.name,
            const StandardMethodCodec().encodeMethodCall(
              MethodCall('pairingBondFailed', {
                ...target,
                'source': 'android_bond_failed',
              }),
            ),
            (_) {},
          );
      await failed({...watch!, 'deviceId': _otherId});
      await failed({
        ...watch!,
        'generation': (watch!['generation'] as int) - 1,
      });
      await failed(
        watch!,
      ); // OS Cancel/reject during fresh claim is not proof of stale keys.
      expect(service.pairingRecovery, isNull);
      expect(service.isConnected, isTrue);
      board.setState(
        5,
        nextFlags: 7,
      ); // Controlled authoritative completion; no native RF simulation.
      expect(await preparing, isTrue);
      await failed(watch!);
      expect(service.pairingRecovery, isNull);
      expect(service.productReady, isTrue);
      expect(board.pairingCommands, hasLength(1));
    },
  );

  testWidgets('virtual peer disposal needs no native event turn', (
    tester,
  ) async {
    final board = _RecoveryBoard();
    final service = LitchBleService(
      ble: board,
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    var disposed = false;
    unawaited(service.dispose().then((_) => disposed = true));
    await tester.pump();
    expect(disposed, isTrue);
    await disposeBoard(service, board);
  });

  test(
    'typed native error stops Android fallback before first connection',
    () async {
      final board = _RecoveryBoard();
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await _staleBeforeConnect(service, board);
      expect(board.modes, ['direct']);
      expect(service.productReady, isFalse);
      for (var i = 0; i < 3; ++i) {
        expect(await service.connect(_boardId), isFalse);
        await service.startScan();
      }
      expect(board.connections, hasLength(1));
      expect(service.pairingRecovery?.deviceId, _boardId);
    },
  );

  for (final message in [
    'CBATTErrorDomain Code=14 "Peer removed pairing information"',
    'GATT status 133',
    'Connection timeout',
  ]) {
    test('generic failure stays ambiguous: $message', () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final board = _RecoveryBoard();
      final service = _service(board, FakeBluetoothPairingRecoveryGateway());
      final connecting = service.connect(_boardId);
      await _connectionAdmitted(board, 1);
      board.fail(message);
      expect(await connecting, isFalse);
      expect(service.pairingRecovery, isNull);
      expect(isNativePairingKeyMissing(message), isFalse);
    });
  }

  test(
    'an ambiguous Android failure retains the existing advertising fallback',
    () async {
      final board = _RecoveryBoard();
      final service = _service(board, FakeBluetoothPairingRecoveryGateway());
      final connecting = service.connect(_boardId);
      await _connectionAdmitted(board, 1);
      board.fail('GATT status 133');
      await _connectionAdmitted(board, 2);
      expect(board.modes, ['direct', 'advertising']);
      expect(service.pairingRecovery, isNull);
      board.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: _boardId,
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);
    },
  );

  test(
    'only current native device and generation revoke ready owner authority',
    () async {
      final board = _RecoveryBoard()
        ..state = 5
        ..flags = 7;
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await connectBoard(service, board, deviceId: _boardId);
      expect(await service.ensureProductReady(_boardId), isTrue);
      final oldSession = service.connectionSessionToken;
      final readiness = <bool>[];
      final subscription = service.productReadinessStream.listen(readiness.add);
      addTearDown(subscription.cancel);
      gateway.emit(deviceId: _otherId);
      gateway.emit(generation: gateway.watches.last.$2 - 1);
      expect(service.productReady, isTrue);
      expect(service.pairingRecovery, isNull);
      gateway.emit();
      expect(service.productReady, isFalse);
      expect(readiness, [false]);
      await _turns();
      expect(service.isConnected, isFalse);
      expect(service.connectionSessionToken, isNot(oldSession));
      expect(await service.connect(_boardId), isFalse);
      expect(board.connections, hasLength(1));
    },
  );

  test(
    'queued native event after ordinary disconnect cannot reopen recovery',
    () async {
      final board = _RecoveryBoard()
        ..state = 5
        ..flags = 7;
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await connectBoard(service, board, deviceId: _boardId);
      expect(await service.ensureProductReady(_boardId), isTrue);
      final retiredWatch = gateway.watches.last;
      await service.disconnect();
      // The OS may have queued this while the old watch was still installed.
      gateway.emit(deviceId: retiredWatch.$1, generation: retiredWatch.$2);
      await _turns();
      expect(service.pairingRecovery, isNull);
      expect(service.isConnected, isFalse);
      expect(service.productReady, isFalse);
      expect(board.connections, hasLength(1));
      expect(gateway.repairs, isEmpty);
    },
  );

  test(
    'queued native event cannot retire a new reservation before its watch registers',
    () async {
      final board = _RecoveryBoard()
        ..state = 5
        ..flags = 7;
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await connectBoard(service, board, deviceId: _boardId);
      expect(await service.ensureProductReady(_boardId), isTrue);
      final retiredWatch = gateway.watches.last;
      final watchesBeforeReconnect = gateway.watches.length;
      final connecting = service.connect(_boardId);
      // connect() has retired authority synchronously, but its initial storage
      // await has not yet allowed the replacement OS watch to be installed.
      expect(gateway.watches, hasLength(watchesBeforeReconnect));
      gateway.emit(deviceId: retiredWatch.$1, generation: retiredWatch.$2);
      expect(service.pairingRecovery, isNull);
      await _connectionAdmitted(board, 2);
      expect(gateway.watches, hasLength(watchesBeforeReconnect + 1));
      expect(gateway.watches.last.$2, isNot(retiredWatch.$2));
      board.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: _boardId,
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await connecting, isTrue);
      expect(service.pairingRecovery, isNull);
      expect(service.connectedId, _boardId);
      expect(board.connections, hasLength(2));
      expect(gateway.repairs, isEmpty);
    },
  );

  test(
    'pre-connect Android missing-key event promptly retires the attempt',
    () async {
      final board = _RecoveryBoard();
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      bool? result;
      unawaited(service.connect(_boardId).then((value) => result = value));
      await _connectionAdmitted(board, 1);
      gateway.emit();
      await _turns();
      expect(
        service.pairingRecovery?.stage,
        BluetoothPairingRecoveryStage.ready,
      );
      expect(
        result,
        isFalse,
        reason: 'retired connect must not hold its six-second native timeout',
      );
      expect(board.connections, hasLength(1));
      final repairing = service.repairPhonePairing();
      await _turns();
      expect(gateway.repairs, hasLength(1));
      gateway.removal.complete(BluetoothPairingRepairResult.removed);
      await _connectionAdmitted(board, 2);
      board.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: _boardId,
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await repairing, isTrue);
      expect(service.productReady, isFalse);
    },
  );

  test(
    'confirmed removal admits one ordinary connection and still requires owner proof',
    () async {
      final board = _RecoveryBoard();
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await _staleBeforeConnect(service, board);
      final repairing = service.repairPhonePairing();
      await _turns();
      expect(gateway.repairs, hasLength(1));
      expect(gateway.repairs.single, gateway.watches.last);
      expect(
        service.pairingRecovery?.stage,
        BluetoothPairingRecoveryStage.repairing,
      );
      expect(await service.connect(_boardId), isFalse);
      expect(await service.retryPairingConnection(), isFalse);
      expect(board.connections, hasLength(1));
      gateway.removal.complete(BluetoothPairingRepairResult.removed);
      await _connectionAdmitted(board, 2);
      expect(board.modes, ['direct', 'direct']);
      expect(service.productReady, isFalse);
      board.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: _boardId,
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await repairing, isTrue);
      expect(service.pairingRecovery, isNull);
      await _turns();
      expect(service.productReady, isFalse);
      expect(board.pairingCommands, hasLength(1));
      expect(board.productQueries, isEmpty);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady(_boardId), isTrue);
      expect(board.connections, hasLength(2));
    },
  );

  for (final result in [
    BluetoothPairingRepairResult.cancelled,
    BluetoothPairingRepairResult.failed,
    BluetoothPairingRepairResult.unsupported,
  ]) {
    test('$result never grants a fresh connection', () async {
      final board = _RecoveryBoard();
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await _staleBeforeConnect(service, board);
      final repairing = service.repairPhonePairing();
      await _turns();
      gateway.removal.complete(result);
      expect(await repairing, isFalse);
      expect(
        service.pairingRecovery?.stage,
        BluetoothPairingRecoveryStage.ready,
      );
      expect(
        service.pairingRecovery?.failed,
        result != BluetoothPairingRepairResult.cancelled,
      );
      expect(
        service.pairingRecovery?.canRepair,
        result != BluetoothPairingRepairResult.unsupported,
      );
      expect(board.connections, hasLength(1));
      expect(await service.connect(_boardId), isFalse);
    });
  }

  test(
    'dismissal retires native watch and late removed cannot reconnect',
    () async {
      final board = _RecoveryBoard();
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = _service(board, gateway);
      await _staleBeforeConnect(service, board);
      final repairing = service.repairPhonePairing();
      await _turns();
      final retiredWatch = gateway.watches.last;
      await service.dismissPairingRecovery();
      expect(gateway.watches.last.$1, isNull);
      gateway.removal.complete(BluetoothPairingRepairResult.removed);
      expect(await repairing, isFalse);
      gateway.emit(deviceId: retiredWatch.$1, generation: retiredWatch.$2);
      await _turns();
      expect(service.pairingRecovery, isNull);
      expect(service.isConnected, isFalse);
      expect(board.connections, hasLength(1));
    },
  );

  testWidgets('late recovery dismissal cannot disconnect chosen next board', (
    tester,
  ) async {
    final board = _RecoveryBoard();
    final gateway = FakeBluetoothPairingRecoveryGateway();
    final service = _ChoiceRecoveryService(board, gateway);
    await tester.runAsync(() => _staleBeforeConnect(service, board));
    gateway.nullWatchGate = Completer<void>();
    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump(const Duration(seconds: 1));
    expect(
      find.byKey(const ValueKey('bluetooth-pairing-recovery')),
      findsOneWidget,
    );
    await tester.tap(find.byType(BackButton));
    await tester.tap(find.byType(BackButton));
    await tester.pump();
    expect(gateway.watches.where((watch) => watch.$1 == null), hasLength(1));
    expect(service.pairingRecovery, isNull);
    final card = find.byKey(const ValueKey('discovered-board-$_otherId'));
    expect(card, findsOneWidget);
    await tester.tap(
      find.descendant(of: card, matching: find.byType(FilledButton)).first,
    );
    await tester.runAsync(() => _connectionAdmitted(board, 2));
    board.connections.last.add(
      const ConnectionStateUpdate(
        deviceId: _otherId,
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    await tester.runAsync(() => _turns());
    await tester.pump();
    expect(service.connectedId, _otherId);
    final token = service.connectionSessionToken;
    gateway.nullWatchGate!.complete();
    await tester.runAsync(() => _turns());
    await tester.pump();
    expect(
      service.connectedId,
      _otherId,
      reason: 'old dismissal must not disconnect the new board',
    );
    expect(service.connectionSessionToken, token);
    await tester.pumpWidget(const SizedBox());
    var disposed = false;
    unawaited(disposeBoard(service, board).then((_) => disposed = true));
    for (var pass = 0; pass < 100 && !disposed; ++pass) {
      await tester.pump(const Duration(milliseconds: 10));
      await tester.runAsync(() => _turns(1));
    }
    expect(disposed, isTrue);
  });

  testWidgets('late recovery repair cannot disconnect chosen next board', (
    tester,
  ) async {
    final board = _RecoveryBoard();
    final gateway = FakeBluetoothPairingRecoveryGateway();
    final service = _ChoiceRecoveryService(board, gateway);
    await tester.runAsync(() => _staleBeforeConnect(service, board));
    await tester.pumpWidget(
      MaterialApp(home: ConnectionGateScreen(bleService: service)),
    );
    await tester.pump(const Duration(seconds: 1));
    service.holdNextStop = true;
    await tester.tap(find.byKey(const ValueKey('pairing-recovery-primary')));
    await tester.pump();
    expect(
      service.pairingRecovery?.stage,
      BluetoothPairingRecoveryStage.repairing,
    );
    await tester.tap(find.byType(BackButton));
    await tester.runAsync(() => _turns());
    await tester.pump();
    final card = find.byKey(const ValueKey('discovered-board-$_otherId'));
    expect(card, findsOneWidget);
    await tester.tap(
      find.descendant(of: card, matching: find.byType(FilledButton)).first,
    );
    await tester.runAsync(() => _connectionAdmitted(board, 2));
    board.connections.last.add(
      const ConnectionStateUpdate(
        deviceId: _otherId,
        connectionState: DeviceConnectionState.connected,
        failure: null,
      ),
    );
    await tester.runAsync(() => _turns());
    await tester.pump();
    expect(service.connectedId, _otherId);
    final token = service.connectionSessionToken;
    service.heldStop.complete();
    await tester.runAsync(() => _turns());
    await tester.pump();
    expect(
      service.connectedId,
      _otherId,
      reason: 'old repair must not disconnect the new board',
    );
    expect(service.connectionSessionToken, token);
    await tester.pumpWidget(const SizedBox());
    var disposed = false;
    unawaited(disposeBoard(service, board).then((_) => disposed = true));
    for (var pass = 0; pass < 100 && !disposed; ++pass) {
      await tester.pump(const Duration(milliseconds: 10));
      await tester.runAsync(() => _turns(1));
    }
    expect(disposed, isTrue);
  });

  test(
    'native gateway rejects unknown results and untrusted event shapes',
    () async {
      const channel = MethodChannel('test/litch-bluetooth-recovery');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      Object? bondReply;
      MethodCall? bondCall;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'startBond') {
          bondCall = call;
          return bondReply;
        }
        return {'status': 'future_status'};
      });
      final gateway = MethodChannelBluetoothPairingRecovery(channel: channel);
      final events = <BluetoothPairingKeyMissing>[];
      final subscription = gateway.keyMissing.listen(events.add);
      expect(
        await gateway.repairPairing(_boardId, 7),
        BluetoothPairingRepairResult.unsupported,
      );
      for (final (reply, expected)
          in <(Object?, BluetoothPairingBondStartResult)>[
            ('started', BluetoothPairingBondStartResult.started),
            ('alreadyBonded', BluetoothPairingBondStartResult.alreadyBonded),
            ('future_status', BluetoothPairingBondStartResult.failed),
            ({'status': 'started'}, BluetoothPairingBondStartResult.failed),
            (null, BluetoothPairingBondStartResult.failed),
          ]) {
        bondReply = reply;
        expect(await gateway.startBond(_boardId, 7, 3), expected);
        expect(bondCall?.arguments, {
          'deviceId': _boardId,
          'generation': 7,
          'attempt': 3,
        });
      }
      for (final arguments in [
        {'deviceId': _boardId, 'generation': 7, 'source': 'unknown'},
        {
          'deviceId': _boardId,
          'generation': -1,
          'source': 'android_key_missing',
        },
        {
          'deviceId': _boardId,
          'generation': 7,
          'source': 'android_key_missing',
        },
      ]) {
        await messenger.handlePlatformMessage(
          channel.name,
          const StandardMethodCodec().encodeMethodCall(
            MethodCall('pairingKeyMissing', arguments),
          ),
          (_) {},
        );
      }
      expect(events, hasLength(1));
      expect(events.single.deviceId, _boardId);
      expect(events.single.generation, 7);
      await subscription.cancel();
      await gateway.dispose();
      messenger.setMockMethodCallHandler(channel, null);
    },
  );

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      'owner verification failure offers neutral recovery at 200 percent $platform',
      (tester) async {
        debugDefaultTargetPlatformOverride = platform;
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        var retries = 0;
        var exits = 0;
        var repairs = 0;
        var support = 0;
        await tester.pumpWidget(
          _screen(
            state: const BluetoothPairingRecoveryState(
              deviceId: _boardId,
              canRepair: true,
              stage: BluetoothPairingRecoveryStage.ready,
              reason: BluetoothPairingRecoveryReason.ownerVerificationFailed,
            ),
            onRetry: () async {
              ++retries;
              return false;
            },
            onRepair: () async {
              ++repairs;
              return false;
            },
            onExit: () => ++exits,
            support: TextButton(
              onPressed: () => ++support,
              child: const Text('Support'),
            ),
          ),
        );
        expect(find.text('Repair pairing'), findsNothing);
        expect(
          find.byKey(const ValueKey('pairing-recovery-secondary')),
          findsNothing,
        );
        expect(
          find.byKey(const ValueKey('owner-verification-recovery-steps')),
          findsOneWidget,
        );
        expect(
          find.textContaining(
            platform == TargetPlatform.iOS
                ? 'Forget This Device'
                : 'Unpair or Forget',
          ),
          findsOneWidget,
        );
        expect(
          find.textContaining('all phones, tablets and other devices'),
          findsOneWidget,
        );
        expect(find.textContaining('three success flashes'), findsOneWidget);
        expect(
          find.textContaining('try connecting again in the app'),
          findsOneWidget,
        );
        final primary = find.byKey(const ValueKey('pairing-recovery-primary'));
        final cancel = find.byKey(const ValueKey('pairing-recovery-cancel'));
        expect(
          find.descendant(of: primary, matching: find.text('Try again')),
          findsOneWidget,
        );
        expect(
          find.descendant(of: cancel, matching: find.text('Cancel')),
          findsOneWidget,
        );
        await tester.ensureVisible(primary);
        await tester.pump();
        await captureHealthReview(
          tester,
          'verification-failed-${platform.name}-phone',
          boundaryFinder: find.byKey(
            const ValueKey('pairing-recovery-review-root'),
          ),
        );
        await tester.tap(primary);
        await tester.pump();
        await tester.ensureVisible(cancel);
        await tester.tap(cancel);
        await tester.ensureVisible(find.text('Support'));
        await tester.tap(find.text('Support'));
        expect(retries, 1);
        expect(exits, 1);
        expect(support, 1);
        expect(repairs, 0);
        expect(tester.takeException(), isNull);
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets('manual Settings becomes primary Retry at 200 percent $size', (
      tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      try {
        var settings = 0;
        var retries = 0;
        await tester.pumpWidget(
          _screen(
            state: const BluetoothPairingRecoveryState(
              deviceId: _boardId,
              stage: BluetoothPairingRecoveryStage.ready,
            ),
            onOpenSettings: () async {
              ++settings;
              return true;
            },
            onRetry: () async {
              ++retries;
              return false;
            },
          ),
        );
        final title = find.descendant(
          of: find.byType(AppBar),
          matching: find.text('Reconnect your Litch'),
        );
        expect(
          tester.renderObject<RenderParagraph>(title).didExceedMaxLines,
          isFalse,
          reason: 'the recovery title must remain readable at 200 percent text',
        );
        final primary = find.byKey(const ValueKey('pairing-recovery-primary'));
        expect(
          find.descendant(
            of: primary,
            matching: find.text('Bluetooth settings'),
          ),
          findsOneWidget,
        );
        await tester.ensureVisible(primary);
        await tester.pump();
        await captureHealthReview(
          tester,
          'android-phone-repair-${size.width.toInt()}-manual',
          boundaryFinder: find.byKey(
            const ValueKey('pairing-recovery-review-root'),
          ),
        );
        await tester.tap(primary);
        await tester.pump();
        expect(settings, 1);
        expect(
          find.descendant(of: primary, matching: find.text('Try again')),
          findsOneWidget,
        );
        expect(tester.getSize(primary).height, greaterThanOrEqualTo(48));
        expect(
          tester.getSemantics(primary),
          matchesSemantics(
            isButton: true,
            hasEnabledState: true,
            isEnabled: true,
            hasTapAction: true,
            hasFocusAction: true,
            isFocusable: true,
            label: 'Try again',
          ),
        );
        await tester.tap(primary);
        await tester.pump();
        expect(retries, 1);
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    });

    testWidgets(
      'repair pending blocks actions and announces progress at 200 percent $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final semantics = tester.ensureSemantics();
        try {
          var actions = 0;
          await tester.pumpWidget(
            _screen(
              state: const BluetoothPairingRecoveryState(
                deviceId: _boardId,
                canRepair: true,
                stage: BluetoothPairingRecoveryStage.repairing,
              ),
              onRepair: () async {
                ++actions;
                return false;
              },
              onOpenSettings: () async {
                ++actions;
                return true;
              },
            ),
          );
          for (final key in [
            'pairing-recovery-primary',
            'pairing-recovery-secondary',
          ]) {
            final button = find.byKey(ValueKey(key));
            expect(tester.widget<ButtonStyleButton>(button).onPressed, isNull);
          }
          final progress = find.text('Repairing saved pairing…');
          await tester.ensureVisible(progress);
          await tester.pump();
          expect(
            find.byWidgetPredicate(
              (widget) =>
                  widget is Semantics &&
                  widget.properties.liveRegion == true &&
                  widget.child is Text &&
                  (widget.child! as Text).data == 'Repairing saved pairing…',
            ),
            findsOneWidget,
          );
          expect(actions, 0);
          expect(tester.takeException(), isNull);
        } finally {
          semantics.dispose();
        }
      },
    );
  }

  testWidgets(
    'automatic repair and manual fallback keep distinct explicit actions',
    (tester) async {
      var repairs = 0;
      var retries = 0;
      await tester.pumpWidget(
        _screen(
          state: const BluetoothPairingRecoveryState(
            deviceId: _boardId,
            canRepair: true,
            stage: BluetoothPairingRecoveryStage.ready,
          ),
          onRepair: () async {
            ++repairs;
            return false;
          },
          onRetry: () async {
            ++retries;
            return false;
          },
        ),
      );
      final primary = find.byKey(const ValueKey('pairing-recovery-primary'));
      expect(
        find.descendant(of: primary, matching: find.text('Repair pairing')),
        findsOneWidget,
      );
      await tester.ensureVisible(primary);
      await tester.tap(primary);
      await tester.pump();
      expect(repairs, 1);
      expect(retries, 0);
      final secondary = find.byKey(
        const ValueKey('pairing-recovery-secondary'),
      );
      await tester.ensureVisible(secondary);
      await tester.tap(secondary);
      await tester.pump();
      expect(
        find.text(
          'Open Bluetooth in this device’s Settings to unpair this Litch.',
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(of: primary, matching: find.text('Try again')),
        findsOneWidget,
      );
      await tester.ensureVisible(primary);
      await tester.tap(primary);
      await tester.pump();
      expect(retries, 1);
      expect(repairs, 1);
      expect(tester.takeException(), isNull);
    },
  );

  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets(
      'iOS manual recovery starts with Retry without a settings detour $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final semantics = tester.ensureSemantics();
        var settings = 0;
        var retries = 0;
        var repairs = 0;
        try {
          await tester.pumpWidget(
            _screen(
              state: const BluetoothPairingRecoveryState(
                deviceId: _boardId,
                stage: BluetoothPairingRecoveryStage.ready,
              ),
              onOpenSettings: () async {
                ++settings;
                return true;
              },
              onRetry: () async {
                ++retries;
                return false;
              },
              onRepair: () async {
                ++repairs;
                return false;
              },
            ),
          );
          final primary = find.byKey(
            const ValueKey('pairing-recovery-primary'),
          );
          await tester.ensureVisible(primary);
          await tester.pump();
          await captureHealthReview(
            tester,
            'ios-phone-repair-${size.width.toInt()}-ready',
            boundaryFinder: find.byKey(
              const ValueKey('pairing-recovery-review-root'),
            ),
          );
          expect(
            find.descendant(of: primary, matching: find.text('Try again')),
            findsOneWidget,
          );
          expect(find.text('Open Settings'), findsNothing);
          expect(find.text('Bluetooth settings'), findsNothing);
          expect(
            find.byKey(const ValueKey('pairing-recovery-secondary')),
            findsNothing,
          );
          expect(
            find.textContaining('Settings → Bluetooth on your iPhone or iPad.'),
            findsOneWidget,
          );
          expect(
            find.textContaining(
              'Tap the info button (ⓘ) beside the correct Foxie Litch board and choose Forget This Device.',
            ),
            findsOneWidget,
          );
          expect(
            find.textContaining('Return to Foxie Litch, choose your board'),
            findsOneWidget,
          );
          expect(
            tester.getSize(primary).shortestSide,
            greaterThanOrEqualTo(48),
          );
          expect(
            tester
                .getSemantics(primary)
                .getSemanticsData()
                .hasAction(ui.SemanticsAction.tap),
            isTrue,
          );
          final title = find.descendant(
            of: find.byType(AppBar),
            matching: find.text('Reconnect your Litch'),
          );
          expect(
            tester.renderObject<RenderParagraph>(title).didExceedMaxLines,
            isFalse,
          );
          await tester.tap(primary);
          await tester.pump();
          await tester.tap(primary);
          await tester.pump();
          expect(retries, 2);
          expect(settings, 0);
          expect(repairs, 0);
          expect(tester.takeException(), isNull);
        } finally {
          semantics.dispose();
        }
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
    );

    testWidgets(
      'iOS manual recovery keeps Retry, busy and exit truthful $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        var retries = 0;
        var settings = 0;
        var repairs = 0;
        var exits = 0;
        Widget screen(BluetoothPairingRecoveryState state) => _screen(
          state: state,
          onRetry: () async {
            ++retries;
            return false;
          },
          onOpenSettings: () async {
            ++settings;
            return false;
          },
          onRepair: () async {
            ++repairs;
            return false;
          },
          onExit: () {
            ++exits;
          },
        );
        const failed = BluetoothPairingRecoveryState(
          deviceId: _boardId,
          canRepair: true,
          stage: BluetoothPairingRecoveryStage.ready,
          failed: true,
        );
        await tester.pumpWidget(screen(failed));
        final primary = find.byKey(const ValueKey('pairing-recovery-primary'));
        expect(
          find.textContaining('The connection still needs attention.'),
          findsOneWidget,
        );
        expect(
          find.descendant(of: primary, matching: find.text('Try again')),
          findsOneWidget,
        );
        await tester.ensureVisible(primary);
        await tester.pump();
        await captureHealthReview(
          tester,
          'ios-phone-repair-${size.width.toInt()}-failed',
          boundaryFinder: find.byKey(
            const ValueKey('pairing-recovery-review-root'),
          ),
        );
        await tester.tap(primary);
        await tester.pump();
        expect(retries, 1);
        expect(settings, 0);
        expect(repairs, 0);
        await tester.pumpWidget(
          screen(
            failed.copyWith(
              stage: BluetoothPairingRecoveryStage.reconnecting,
              failed: false,
            ),
          ),
        );
        expect(tester.widget<FilledButton>(primary).onPressed, isNull);
        expect(find.byType(CircularProgressIndicator), findsOneWidget);
        expect(find.text('Connecting…'), findsOneWidget);
        expect(
          find.byKey(const ValueKey('pairing-recovery-secondary')),
          findsNothing,
        );
        await tester.ensureVisible(primary);
        await tester.pump();
        await captureHealthReview(
          tester,
          'ios-phone-repair-${size.width.toInt()}-busy',
          boundaryFinder: find.byKey(
            const ValueKey('pairing-recovery-review-root'),
          ),
        );
        await tester.tap(find.byType(BackButton));
        await tester.pump();
        expect(exits, 1);
        expect(retries, 1);
        expect(settings, 0);
        expect(repairs, 0);
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
    );
  }

  testWidgets(
    'connection gate exposes real service recovery and keeps retries stopped',
    (tester) async {
      final board = _RecoveryBoard();
      final gateway = FakeBluetoothPairingRecoveryGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      await tester.runAsync(() => _staleBeforeConnect(service, board));
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: ConnectionGateScreen(bleService: service),
        ),
      );
      await tester.pump(const Duration(seconds: 2));
      expect(
        find.byKey(const ValueKey('bluetooth-pairing-recovery')),
        findsOneWidget,
      );
      expect(board.connections, hasLength(1));
      expect(service.productReady, isFalse);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await tester.runAsync(() => disposeBoard(service, board));
    },
  );
}
