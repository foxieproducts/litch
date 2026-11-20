import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' show SemanticsAction;

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart'
    show debugDefaultTargetPlatformOverride;
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart'
    show
        ConnectionStateUpdate,
        DeviceConnectionState,
        BleStatus,
        DiscoveredDevice,
        Uuid,
        ScanMode,
        Connectable,
        GenericFailure,
        ConnectionError;
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/services/connection_identify_codec.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/services/home_board_feed.dart';
import 'package:litch_mobile/src/services/physical_game_history_ingestion.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/owner_pairing_session.dart';
import 'package:litch_mobile/src/services/bluetooth_pairing_recovery.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'health_render_capture.dart';
import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

Future<void> turns([int count = 30]) async {
  for (var i = 0; i < count; i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

final class _BondGateway implements BluetoothPairingRecoveryGateway {
  final starts = <(String, int, int)>[];
  BluetoothPairingBondStartResult result =
      BluetoothPairingBondStartResult.started;
  @override
  Stream<BluetoothPairingKeyMissing> get keyMissing => const Stream.empty();
  @override
  Future<void> watchDevice(String? deviceId, int generation) async {}
  @override
  Future<BluetoothPairingBondStartResult> startBond(
    String deviceId,
    int generation,
    int attempt,
  ) async {
    starts.add((deviceId, generation, attempt));
    return result;
  }

  @override
  Future<bool> canRepairPairing() async => false;
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

final class _ConsentDuringCancelBoard extends OwnerPairingBoard {
  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.ordered &&
        packet.code == 27 &&
        packet.body[1] == 2) {
      setState(3, nextFlags: 20);
      await Future<void>.delayed(Duration.zero);
    }
    await super.sharedWrite(value);
  }
}

final class _OwnerTraceBoard extends OwnerPairingBoard {
  List<Map<String, Object?>> diagnostics = [];

  @override
  Future<List<Map<String, Object?>>> getConnectionDiagnostics(
    String deviceId,
  ) async => diagnostics;
}

final class _BusyVersionBoard extends OwnerPairingBoard {
  @override
  Future<void> commandWrite(List<int> value) async {
    if ((jsonDecode(utf8.decode(value)) as Map)['cmd'] == 'fw_version') {
      ++versionRequests;
      emitStatus('{"e":"busy_test"}');
      return;
    }
    await super.commandWrite(value);
  }
}

final class _OwnerBlockedInfoBoard extends OwnerPairingBoard {
  int infoRequests = 0;

  @override
  Future<void> commandWrite(List<int> value) async {
    if ((jsonDecode(utf8.decode(value)) as Map)['cmd'] == 'info') {
      ++infoRequests;
      if (sessionActivated && state != 5) {
        throw StateError('Private Objects await owner proof.');
      }
    }
    await super.commandWrite(value);
  }
}

final class _DelayedPlayerEndService extends LitchBleService {
  _DelayedPlayerEndService(OwnerPairingBoard board)
    : super(ble: board, pendingOtaStore: MemoryOtaTargetStore());
  final delayedEnd = Completer<Object?>();
  bool heldEnd = false;
  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) {
    if (key == 'GAME_FIRST_PLAYER' && !heldEnd) {
      heldEnd = true;
      return delayedEnd.future;
    }
    return super.readSettingValue(deviceId: deviceId, key: key);
  }
}

final class _HeldControlBoard extends OwnerPairingBoard {
  ConnectionSessionPacket? pendingControl;

  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.ordered &&
        packet.code == LitchControlProtocol.connectionActionOpcode) {
      pendingControl = packet;
      progress(packet.token, packet.sequence);
      return;
    }
    await super.sharedWrite(value);
  }

  void completeControl() {
    final pending = pendingControl;
    if (pending == null) return;
    pendingControl = null;
    control(
      ConnectionSessionControl.checkpoint,
      pending.token,
      pending.sequence,
      Uint8List(10),
    );
  }
}

final class _HeldIdentifyBoard extends OwnerPairingBoard {
  final heldWrite = Completer<void>();
  bool holding = false;
  @override
  Future<void> sharedWrite(Uint8List value) async {
    final packet = ConnectionSessionCodec.decode(value)!;
    if (packet.kind == ConnectionSessionKind.ordered &&
        packet.code == 28 &&
        packet.body[1] != 2) {
      final nativeLink = connections.length;
      holding = true;
      await heldWrite.future;
      if (connections.length != nativeLink) return;
    }
    await super.sharedWrite(value);
  }
}

final class _OwnerRetryBoard extends _HeldIdentifyBoard {
  final cancellationStarted = Completer<void>();
  final nativeDisconnected = Completer<void>();

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    final first = connections.isEmpty;
    final source = super.connectToDevice(
      id: id,
      servicesWithCharacteristicsToDiscover:
          servicesWithCharacteristicsToDiscover,
      connectionTimeout: connectionTimeout,
    );
    if (!first) return source;
    late StreamController<ConnectionStateUpdate> controller;
    StreamSubscription<ConnectionStateUpdate>? subscription;
    controller = StreamController<ConnectionStateUpdate>(
      sync: true,
      onListen: () {
        subscription = source.listen(controller.add, onDone: controller.close);
      },
      onCancel: () async {
        if (!cancellationStarted.isCompleted) cancellationStarted.complete();
        await nativeDisconnected.future;
        await subscription?.cancel();
      },
    );
    return controller.stream;
  }
}

final class _IdentifyPermissionGateway implements BlePermissionGateway {
  @override
  Future<PermissionStatus> status(Permission permission) async =>
      PermissionStatus.granted;
  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async => {
    for (final permission in permissions) permission: PermissionStatus.granted,
  };
  @override
  Future<bool> openSettings() async => true;
}

final class _ScanningIdentifyBoard extends _HeldIdentifyBoard {
  int scanStarts = 0;
  int scanStops = 0;
  late final discoveries = StreamController<DiscoveredDevice>.broadcast(
    sync: true,
    onCancel: () {
      ++scanStops;
    },
  );
  @override
  Stream<BleStatus> get statusStream => Stream.value(BleStatus.ready);
  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) {
    ++scanStarts;
    return discoveries.stream;
  }
}

final class _OwnerPairingUiService extends LitchBleService {
  _OwnerPairingUiService(
    OwnerPairingBoard board, {
    super.ownerPairingReadinessTimeout,
  }) : super(ble: board, pendingOtaStore: MemoryOtaTargetStore());

  // Only OS scan permission/advertising is outside these cancellation tests.
  @override
  Future<void> startScan() async {}
}

final class _IdentifyUiService extends _OwnerPairingUiService {
  _IdentifyUiService(super.board, {super.ownerPairingReadinessTimeout});
  Future<bool>? identification;
  @override
  Future<bool> sendIdentify(
    String deviceId,
    int number, {
    int durationMs = 10000,
    bool untilConnected = true,
  }) {
    return identification = super.sendIdentify(
      deviceId,
      number,
      durationMs: durationMs,
      untilConnected: untilConnected,
    );
  }

  // Only advertisements are controlled; Identify uses the real connection,
  // public discovery, shared-session activation and Ordered native writer.
  @override
  Stream<List<LitchBoard>> get boardsStream => Stream.value(const [
    LitchBoard(id: 'board-1', name: 'First Litch', rssi: -40),
    LitchBoard(id: 'board-2', name: 'Second Litch', rssi: -50),
  ]);
}

Future<void> _fillFailedClaimIntents(
  LitchBleService service,
  OwnerPairingBoard board,
) async {
  for (var identity = 1; identity <= 64; ++identity) {
    board.identity[0] = identity;
    board.state = 1;
    board.flags = 4;
    board.reason = 0;
    ++board.revision;
    final nativeId = 'claim-limit-$identity';
    await connectBoard(service, board, deviceId: nativeId);
    await turns();
    expect(
      board.pairingCommands.length,
      identity,
      reason: 'each distinct board has exactly one actual Begin',
    );
    board.setState(1, nextFlags: 4, nextReason: 7);
    expect(await service.ensureProductReady(nativeId), isFalse);
    await service.disconnect();
  }
}

Uint8List _wireFixture(String file, String name) {
  final row = File(
    '../test/fixtures/$file',
  ).readAsLinesSync().firstWhere((line) => line.startsWith('$name,'));
  final hex = row.substring(row.indexOf(',') + 1);
  return Uint8List.fromList([
    for (var i = 0; i < hex.length; i += 2)
      int.parse(hex.substring(i, i + 2), radix: 16),
  ]);
}

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
  });

  test(
    'owner pairing trace retains accepted states and recovery across disconnect',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final directory = await Directory.systemTemp.createTemp(
        'litch_owner_trace_',
      );
      final trace = await AppTraceRecorder.open(
        directory,
        now: () => DateTime.utc(2026, 11, 1, 22, 20),
      );
      final board = _OwnerTraceBoard();
      final service = LitchBleService(
        ble: board,
        appTrace: trace,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() async {
        await disposeBoard(service, board);
        await trace.close();
        await directory.delete(recursive: true);
      });
      Future<List<Map<String, dynamic>>> events(String event) async => [
        for (final line in (await trace.snapshot()).jsonl.split('\n'))
          if (line.isNotEmpty)
            if (jsonDecode(line) case final Map<String, dynamic> row)
              if (row['event'] == event) row,
      ];

      await connectBoard(service, board);
      await turns();
      board.setState(3, nextFlags: 4);
      await turns();
      // State/identity replay in one bucket must leave room for native terminal
      // evidence. Flush small batches to isolate storage capacity from queue size.
      for (var batch = 0; batch < 30; ++batch) {
        for (var replay = 0; replay < 10; ++replay) {
          board.setState(3, nextFlags: 4);
        }
        await turns();
        await trace.flush();
      }
      board.setState(8, nextFlags: 4, nextReason: 9);
      await turns();
      board.setState(1, nextFlags: 4, nextReason: 9);
      await turns();
      final oldStateToken = board.stateToken;
      board.diagnostics = [
        {
          'native_lifetime': 'owner_trace_fixture',
          'seq': 1,
          'native_ms': 8000,
          'native_attempt': 1,
          'phase': 'did_disconnect',
          'error_code': 7,
          'has_error': true,
        },
      ];
      board.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await turns();
      expect(service.ownerPairingStatus, isNull);
      expect(
        (await trace.snapshot()).droppedEvents,
        0,
        reason:
            'Repeated identical owner states must not exhaust the trace bucket.',
      );
      expect(
        (await events('ble.native_bootstrap')).single['phase'],
        'did_disconnect',
      );
      final retained = await events('ble.owner_pairing_state');
      expect(retained.map((row) => row['state']), [
        'unpaired',
        'awaitPhysical',
        'securing',
        'failed',
        'unpaired',
      ]);
      final failed = retained.firstWhere((row) => row['state'] == 'failed');
      expect(
        {
          for (final key in [
            'generation',
            'state',
            'reason',
            'flags',
            'attempt',
          ])
            key: failed[key],
        },
        {
          'generation': 1,
          'state': 'failed',
          'reason': 'securityFailed',
          'flags': 4,
          'attempt': 1,
        },
      );
      expect(retained.every((row) => row['generation'] == 1), isTrue);

      board.publish(token: oldStateToken);
      await turns();
      expect(
        await events('ble.owner_pairing_state'),
        retained,
        reason: 'A disconnected generation cannot add owner observations.',
      );
      await connectBoard(service, board);
      await turns();
      final beforeLate = await events('ble.owner_pairing_state');
      expect(beforeLate.last['generation'], 3);
      expect(
        beforeLate.last['state'],
        'unpaired',
        reason: 'A new session records its initial tuple even when unchanged.',
      );
      board.emitState(
        40,
        [1, 8, 10, 4, 1, 0, 0, 0],
        token: 0x800000ff,
        sequence: board.revision + 10,
      );
      await turns();
      expect(
        await events('ble.owner_pairing_state'),
        beforeLate,
        reason:
            'A rejected State token cannot add a current-generation diagnostic.',
      );
      board.setState(8, nextFlags: 4, nextReason: 10);
      await turns();
      final current = (await events('ble.owner_pairing_state')).last;
      expect(current['generation'], 3);
      expect(current['reason'], 'storageFailed');
      board.setState(8, nextFlags: 5, nextReason: 10);
      await turns();
      expect((await events('ble.owner_pairing_state')).last['flags'], 5);
      ++board.attempt;
      ++board.revision;
      board.publish();
      await turns();
      expect((await events('ble.owner_pairing_state')).last['attempt'], 2);

      await service.disconnect();
      final retry = service.connect('board-1');
      await turns();
      expect(board.connections, hasLength(3));
      board.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.disconnected,
          failure: GenericFailure(
            code: ConnectionError.failedToConnect,
            message: '$iosPairingKeyMissingMarker\ncontrolled native failure',
          ),
        ),
      );
      expect(await retry, isFalse);
      await turns();
      expect(
        service.pairingRecovery?.stage,
        BluetoothPairingRecoveryStage.ready,
      );
      final recovery = await events('ble.pairing_recovery');
      expect(recovery.map((row) => row['stage']), ['checking', 'ready']);
      expect(recovery.first['generation'], 5);
      expect(recovery.every((row) => row['can_repair'] == false), isTrue);
      await service.dismissPairingRecovery();
      expect((await events('ble.pairing_recovery')).last['stage'], 'none');
      expect((await trace.snapshot()).jsonl, isNot(contains('board-1')));
      expect(
        (await trace.snapshot()).jsonl,
        isNot(contains('controlled native failure')),
      );
    },
  );

  test(
    'available saved-phone slot uses existing consent and durable readiness',
    () async {
      final board = OwnerPairingBoard()
        ..state = 6
        ..flags = 13
        ..reason = 8;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(service.isConnected, isTrue);
      expect(board.pairingCommands, hasLength(1));
      expect(
        board.pairingCommands.single.body[4],
        OwnerPairingSessionStatus.eitherEnd,
      );
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.awaitPhysical,
      );
      expect(service.productReady, isFalse);
      expect(board.productQueries, isEmpty);
      board.setState(3, nextFlags: 5);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(4, nextFlags: 5);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(service.ownerPairingRefusedDeviceId, isNull);
    },
  );

  test(
    'typed Identify codec preserves the strict eight-byte minimum-MTU body',
    () {
      expect(ConnectionIdentifyCodec.probe(), [1, 0, 0, 0, 0, 0, 0, 0]);
      expect(ConnectionIdentifyCodec.stop(), [1, 2, 0, 0, 0, 0, 0, 0]);
      expect(ConnectionIdentifyCodec.start(99, 60000), [
        1,
        1,
        99,
        0,
        0,
        255,
        96,
        234,
      ]);
      for (final request in [(100, 3000), (-1, 3000), (1, 249), (1, 60001)]) {
        expect(
          () => ConnectionIdentifyCodec.start(request.$1, request.$2),
          throwsFormatException,
        );
      }
    },
  );

  test(
    'Android native bonding waits for exact post-journal admission and starts once',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = OwnerPairingBoard();
      final gateway = _BondGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(board.pairingCommands.single.body, [1, 3, 1, 0, 2]);
      expect(gateway.starts, isEmpty);
      board.setState(3, nextFlags: 4);
      await turns();
      expect(
        gateway.starts,
        isEmpty,
        reason: 'Securing can still mean an unfinished journal.',
      );
      board.attempt = 2;
      board.setState(3, nextFlags: 20);
      await turns();
      expect(
        gateway.starts,
        isEmpty,
        reason: 'A different attempt cannot authorize this OS operation.',
      );
      board.attempt = 1;
      board.setState(3, nextFlags: 20);
      await turns();
      expect(gateway.starts, hasLength(1));
      expect(gateway.starts.single.$1, 'board-1');
      expect(gateway.starts.single.$3, 1);
      board.setState(3, nextFlags: 20);
      await turns();
      expect(gateway.starts, hasLength(1));
      expect(
        service.productReady,
        isFalse,
        reason: 'OS admission or bonding is not board ownership.',
      );
      board.setState(4, nextFlags: 4);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
    },
  );

  test(
    'Android native bonding refuses a conflicting durable board identity',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = OwnerPairingBoard();
      final gateway = _BondGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      final preparing = service.ensureProductReady('board-1');
      expect(service.durableBoardIdentity, isNotNull);
      expect(board.pairingCommands.single.body, [1, 3, 1, 0, 2]);
      // The complete changed identity arrives ahead of the matching allowed
      // State in one delivery batch. Its conflict must fence the synchronous
      // bond listener before the readiness waiter's failure can disconnect.
      board.identity[0] ^= 0x40;
      ++board.revision;
      board.state = 3;
      board.flags = 20;
      board.physicalEnd = 0;
      board.emitState(41, board.identity.sublist(0, 8));
      board.emitState(42, board.identity.sublist(8));
      board.emitState(40, [1, 3, 0, 20, 1, 0, 0, 0]);
      await turns();
      expect(
        gateway.starts,
        isEmpty,
        reason:
            'A matching admission attempt cannot override identity conflict.',
      );
      expect(await preparing, isFalse);
      expect(service.productReady, isFalse);
      expect(service.isConnected, isFalse);
    },
  );

  test(
    'Android native bonding cannot start after departure or reuse existing phone keys',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = OwnerPairingBoard();
      final gateway = _BondGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      await service.disconnect();
      board.setState(3, nextFlags: 20);
      await turns();
      expect(gateway.starts, isEmpty);
      board.state = 1;
      board.flags = 4;
      board.attempt = 0;
      gateway.result = BluetoothPairingBondStartResult.alreadyBonded;
      await connectBoard(service, board);
      await turns();
      final retry = service.ensureProductReady('board-1', retry: true);
      await turns();
      board.setState(3, nextFlags: 20);
      await turns();
      expect(await retry, isFalse);
      expect(gateway.starts, hasLength(1));
      expect(service.productReady, isFalse);
      expect(service.isConnected, isFalse);
      expect(
        service.pairingRecovery,
        isNotNull,
        reason: 'Existing phone keys require explicit pairing repair.',
      );
    },
  );

  test(
    'Android native bonding cannot start from consent racing app Cancel',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = _ConsentDuringCancelBoard();
      final gateway = _BondGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      await service.cancelOwnerPairing();
      await turns();
      expect(gateway.starts, isEmpty);
      expect(board.pairingCommands.map((p) => p.body[1]), [3, 2]);
      expect(service.productReady, isFalse);
    },
  );

  test(
    'Android native bonding deadline retires a late journal admission',
    () async {
      expect(
        OwnerPairingSession.defaultReadinessTimeout,
        const Duration(seconds: 20),
      );
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = OwnerPairingBoard();
      final gateway = _BondGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
        ownerPairingReadinessTimeout: const Duration(seconds: 1),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(board.pairingCommands.single.body[1], 3);
      board.setState(3, nextFlags: 4); // Adding has not released its receipt.
      // Shorten only this instance's genuine monotonic State-reader deadline.
      // Await retirement before delivering the late journal admission.
      expect(await service.ensureProductReady('board-1'), isFalse);
      expect(service.isConnected, isFalse);
      board.setState(3, nextFlags: 20);
      await turns();
      expect(gateway.starts, isEmpty);
      expect(service.productReady, isFalse);
    },
  );

  test(
    'Android native bonding failure retires the link without product readiness',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.android;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = OwnerPairingBoard();
      final gateway = _BondGateway()
        ..result = BluetoothPairingBondStartResult.failed;
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      board.setState(3, nextFlags: 20);
      await turns();
      expect(gateway.starts, hasLength(1));
      expect(service.isConnected, isFalse);
      expect(service.productReady, isFalse);
      board.setState(3, nextFlags: 20);
      await turns();
      expect(gateway.starts, hasLength(1));
    },
  );

  test(
    'iOS keeps board-initiated security without invoking Android native bonding',
    () async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final board = OwnerPairingBoard();
      final gateway = _BondGateway();
      final service = LitchBleService(
        ble: board,
        pairingRecoveryGateway: gateway,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(board.pairingCommands.single.body, [1, 1, 1, 0, 2]);
      board.setState(3, nextFlags: 4);
      await turns();
      expect(gateway.starts, isEmpty);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
    },
  );

  test('C++ and Dart agree on the lightweight pairing corpus', () {
    final lines = File(
      '../test/fixtures/owner_pairing_session.csv',
    ).readAsLinesSync();
    for (final line in lines.skip(1)) {
      final cells = line.split(',');
      final hex = cells[3];
      final bytes = [
        for (var i = 0; i < hex.length; i += 2)
          int.parse(hex.substring(i, i + 2), radix: 16),
      ];
      bool valid;
      if (cells[1] == 'status') {
        valid = OwnerPairingSessionStatus.tryParse(bytes) != null;
      } else {
        try {
          final encoded = OwnerPairingSessionStatus.command(
            cancel: bytes[1] == 2,
            phoneInitiated: bytes[1] == 3,
            attempt: bytes[2] | bytes[3] << 8,
            physicalEnd: bytes.length == 5 ? bytes[4] : -1,
          );
          valid =
              encoded.length == bytes.length &&
              List.generate(
                bytes.length,
                (i) => encoded[i] == bytes[i],
              ).every((v) => v);
        } on FormatException {
          valid = false;
        }
      }
      expect(valid, cells[2] == '1', reason: cells[0]);
    }
  });

  test(
    'production status-first zero identity halves promote to one prompt and Ready',
    () async {
      final board = OwnerPairingBoard()
        ..state = 0
        ..flags = 0;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      final preparing = service.ensureProductReady('board-1');
      expect(service.ownerPairingBusy, isTrue);
      expect(service.productReady, isFalse);
      expect(board.pairingCommands, isEmpty);
      expect(board.productQueries, isEmpty);
      // An idle local Bluetooth hold prompt is not the phone's new claim.
      board.attempt = 73;
      board.physicalEnd = 1;
      board.setState(1, nextFlags: 4);
      await turns();
      expect(board.pairingCommands, hasLength(1));
      expect(
        board.pairingCommands.single.body,
        [1, 1, 1, 0, 2],
        reason: 'The actual service Begin must offer either same-end X/O pair.',
      );
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.awaitPhysical,
      );
      expect(service.productReady, isFalse);
      expect(board.productQueries, isEmpty);
      expect(service.durableBoardIdentity, '0102030405060708090a0b0c0d0e0f10');
      board.setState(5, nextFlags: 7);
      expect(await preparing, isTrue);
      expect(board.pairingCommands, hasLength(1));
    },
  );

  for (final terminal in [
    (state: 1, reason: 7, flags: 4), // timed out or cancelled prompt
    (state: 8, reason: 10, flags: 0), // first identity could not persist
  ]) {
    test('initial identity wait stops on authoritative $terminal', () async {
      final board = OwnerPairingBoard()
        ..state = 0
        ..flags = 0;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      final preparing = service.ensureProductReady('board-1');
      board.setState(
        terminal.state,
        nextFlags: terminal.flags,
        nextReason: terminal.reason,
      );
      expect(await preparing, isFalse);
      expect(service.productReady, isFalse);
      expect(board.pairingCommands, isEmpty);
      expect(board.productQueries, isEmpty);
    });
  }

  test('disconnect interrupts the initial identity wait', () async {
    final board = OwnerPairingBoard()
      ..state = 0
      ..flags = 0;
    final service = LitchBleService(
      ble: board,
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    addTearDown(() => disposeBoard(service, board));
    await connectBoard(service, board);
    await turns();
    final preparing = service.ensureProductReady('board-1');
    await service.disconnect();
    expect(await preparing, isFalse);
    expect(service.productReady, isFalse);
    expect(board.pairingCommands, isEmpty);
    expect(board.productQueries, isEmpty);
  });

  test(
    'authoritative foreign-owner refusal disconnects and remains actionable',
    () async {
      final board = OwnerPairingBoard()
        ..state = 6
        ..flags = 5;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(
        service.isConnected,
        isTrue,
        reason: 'OwnedOther/none is quiet owner verification.',
      );
      final preparing = service.ensureProductReady('board-1');
      board.setState(6, nextFlags: 5, nextReason: 8);
      await turns();
      expect(await preparing, isFalse);
      expect(
        service.isConnected,
        isFalse,
        reason:
            'The app owns ordinary disconnect after receiving terminal refusal.',
      );
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.ownedOther,
      );
      expect(
        service.ownerPairingStatus?.reason,
        OwnerPairingSessionReason.wrongConnection,
      );
      expect(service.ownerPairingRefusedDeviceId, 'board-1');
      expect(service.productReady, isFalse);
      expect(board.productQueries, isEmpty);
      expect(board.pairingCommands, isEmpty);
      board.setState(5, nextFlags: 7);
      await turns();
      expect(
        service.productReady,
        isFalse,
        reason: 'Late proof from the retired link cannot clear refusal.',
      );
      expect(service.ownerPairingRefusedDeviceId, 'board-1');
      board.state = 6;
      board.reason = 0;
      board.flags = 5;
      await connectBoard(service, board, deviceId: 'board-2');
      await turns();
      expect(
        service.ownerPairingRefusedDeviceId,
        isNull,
        reason: 'Explicit connection starts a fresh refusal lifetime.',
      );
      expect(
        service.ownerPairingStatus?.reason,
        OwnerPairingSessionReason.none,
      );
      expect(service.isConnected, isTrue);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-2'), isTrue);
    },
  );

  test(
    'initial terminal refusal does not wait for durable identity fragments',
    () async {
      final board = OwnerPairingBoard()
        ..state = 6
        ..flags = 5
        ..reason = 8
        ..includeIdentity = false;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(service.isConnected, isFalse);
      expect(service.ownerPairingStatus?.foreignOwnerRefused, isTrue);
      expect(service.ownerPairingRefusedDeviceId, 'board-1');
      expect(board.productQueries, isEmpty);
      expect(board.pairingCommands, isEmpty);
    },
  );

  test(
    'queued refusal disconnect cannot retire a replacement connection',
    () async {
      final board = OwnerPairingBoard()
        ..state = 6
        ..flags = 5;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      Future<bool>? replacement;
      final subscription = service.ownerPairingStatusStream.listen((status) {
        if (status?.foreignOwnerRefused == true && replacement == null) {
          // Queue a user connection before the refusal's deferred disconnect.
          scheduleMicrotask(() => replacement = service.connect('board-2'));
        }
      });
      addTearDown(subscription.cancel);
      board.setState(6, nextFlags: 5, nextReason: 8);
      await turns();
      expect(board.connections, hasLength(2));
      board.state = 6;
      board.flags = 5;
      board.reason = 0;
      board.connections[1].add(
        const ConnectionStateUpdate(
          deviceId: 'board-2',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      expect(await replacement!, isTrue);
      await turns();
      expect(service.connectedId, 'board-2');
      expect(service.isConnected, isTrue);
      expect(service.ownerPairingRefusedDeviceId, isNull);
      expect(service.productReady, isFalse);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-2'), isTrue);
    },
  );

  for (final flags in [0, 4, 5]) {
    test('authoritative Failed cannot retry claim with flags $flags', () async {
      final board = OwnerPairingBoard()
        ..state = 8
        ..reason = 10
        ..flags = flags;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(await service.ensureProductReady('board-1'), isFalse);
      expect(service.productReady, isFalse);
      expect(board.pairingCommands, isEmpty);
      expect(board.productQueries, isEmpty);
    });
  }

  test(
    'timed out consent stays terminal until explicit retry and keeps the transport alive',
    () async {
      final board = OwnerPairingBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(board.pairingCommands, hasLength(1));
      board.setState(1, nextFlags: 4, nextReason: 7);
      expect(await service.ensureProductReady('board-1'), isFalse);
      await turns();
      bool? backgroundResult;
      unawaited(
        service
            .ensureProductReady('board-1')
            .then((value) => backgroundResult = value),
      );
      await turns();
      expect(board.pairingCommands, hasLength(1));
      expect(backgroundResult, isFalse);
      expect(board.heartbeats, greaterThan(0));
      expect(service.isConnected, isTrue);
      expect(service.productReady, isFalse);
      expect(
        await service.fetchBoardFirmwareVersion(deviceId: 'board-1'),
        isNull,
      );
      expect(board.pairingCommands, hasLength(1));
      final retry = service.ensureProductReady('board-1', retry: true);
      await turns();
      expect(board.pairingCommands, hasLength(2));
      board.setState(5, nextFlags: 7);
      expect(await retry, isTrue);
      expect(service.productReady, isTrue);
    },
  );

  test(
    'automatic reconnect cannot rearm a spent pairing attempt before explicit retry',
    () async {
      final board = OwnerPairingBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(board.pairingCommands, hasLength(1));
      board.setState(1, nextFlags: 4, nextReason: 7);
      expect(await service.ensureProductReady('board-1'), isFalse);
      await service.disconnect();
      board.state = 1;
      board.reason = 0;
      ++board.revision;
      await connectBoard(
        service,
        board,
        deviceId: 'same-board-new-native-alias',
      );
      await turns();
      final repeated = board.pairingCommands.length;
      board.setState(1, nextFlags: 4, nextReason: 7);
      await turns();
      expect(
        repeated,
        1,
        reason:
            'native reconnect is not fresh permission for an OS pairing request',
      );
      expect(
        await service.ensureProductReady('same-board-new-native-alias'),
        isFalse,
      );
      final retry = service.ensureProductReady(
        'same-board-new-native-alias',
        retry: true,
      );
      await turns();
      expect(board.pairingCommands, hasLength(2));
      board.setState(5, nextFlags: 7);
      expect(await retry, isTrue);
    },
  );

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    test(
      'temporary typed Identify uses the real session owner without a claim on $platform',
      () async {
        debugDefaultTargetPlatformOverride = platform;
        addTearDown(() => debugDefaultTargetPlatformOverride = null);
        final board = OwnerPairingBoard()
          ..state = 6
          ..flags = 5
          ..reason = 8;
        final service = LitchBleService(
          ble: board,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        addTearDown(() => disposeBoard(service, board));
        final observed = <ConnectionStateUpdate>[];
        final subscription = service.connectionStream.listen(observed.add);
        addTearDown(subscription.cancel);
        final identifying = service.sendIdentify(
          'board-1',
          7,
          durationMs: 3000,
          untilConnected: false,
        );
        for (var turn = 0; turn < 60 && board.connections.isEmpty; ++turn) {
          await turns(1);
        }
        expect(board.connections, hasLength(1));
        board.connections.single.add(
          const ConnectionStateUpdate(
            deviceId: 'board-1',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        expect(await identifying, isTrue);
        await turns();
        expect(board.identifyCommands.map((p) => p.body[1]), [0, 1]);
        expect(board.identifyCommands.last.body, [1, 1, 7, 0, 0, 255, 184, 11]);
        expect(board.pairingCommands, isEmpty);
        expect(board.productQueries, isEmpty);
        expect(board.protectedReads, isEmpty);
        expect(service.isConnected, isFalse);
        expect(
          observed,
          isEmpty,
          reason:
              'diagnostic transport does not become a product connection journey',
        );
      },
    );
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final failure in ['disconnected', 'error', 'done']) {
      for (final replacement in [false, true]) {
        test(
          'temporary Identify $failure recovers scan without product events on $platform replacement=$replacement',
          () async {
            debugDefaultTargetPlatformOverride = platform;
            addTearDown(() => debugDefaultTargetPlatformOverride = null);
            final board = _ScanningIdentifyBoard()
              ..state = 6
              ..flags = 5;
            final service = LitchBleService(
              ble: board,
              pendingOtaStore: MemoryOtaTargetStore(),
              permissionGateway: _IdentifyPermissionGateway(),
            );
            addTearDown(() async {
              await disposeBoard(service, board);
              await board.discoveries.close();
            });
            final events = <ConnectionStateUpdate>[];
            final rosters = <List<String>>[];
            final connectionSub = service.connectionStream.listen(events.add);
            final rosterSub = service.boardsStream.listen(
              (boards) => rosters.add(boards.map((board) => board.id).toList()),
            );
            addTearDown(connectionSub.cancel);
            addTearDown(rosterSub.cancel);
            await service.startScan();
            expect(board.scanStarts, 1);
            for (final id in ['board-1', 'board-2']) {
              board.discoveries.add(
                DiscoveredDevice(
                  id: id,
                  name: 'Foxie Litch',
                  serviceUuids: [BleProtocol.baseService],
                  serviceData: const {},
                  manufacturerData: Uint8List(0),
                  rssi: -40,
                  connectable: Connectable.available,
                ),
              );
            }
            await turns();
            expect(rosters.last, ['board-1', 'board-2']);
            final emptyRosters = rosters
                .where((roster) => roster.isEmpty)
                .length;
            final identifying = service.sendIdentify(
              'board-1',
              7,
              durationMs: 3000,
            );
            for (var turn = 0; turn < 60 && board.connections.isEmpty; ++turn) {
              await turns(1);
            }
            expect(board.connections, hasLength(1));
            board.connections.single.add(
              const ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.connected,
                failure: null,
              ),
            );
            for (var turn = 0; turn < 60 && !board.holding; ++turn) {
              await turns(1);
            }
            expect(board.holding, isTrue);
            expect(board.scanStops, 1);
            expect(events, isEmpty);
            final oldNative = board.connections.single;
            switch (failure) {
              case 'disconnected':
                oldNative.add(
                  const ConnectionStateUpdate(
                    deviceId: 'board-1',
                    connectionState: DeviceConnectionState.disconnected,
                    failure: null,
                  ),
                );
              case 'error':
                oldNative.addError(
                  StateError('controlled temporary native disconnect'),
                );
              case 'done':
                unawaited(oldNative.close());
            }
            if (replacement) {
              await connectBoard(service, board, deviceId: 'board-2');
            }
            expect(
              await identifying.timeout(const Duration(seconds: 3)),
              isFalse,
            );
            await turns();
            expect(
              events.where((event) => event.deviceId != 'board-2'),
              isEmpty,
              reason:
                  'temporary transport failure cannot publish a product disconnect',
            );
            expect(
              board.scanStarts,
              replacement ? 1 : 2,
              reason:
                  'only an unclaimed failed Identify lifetime resumes discovery exactly once',
            );
            expect(service.isConnected, replacement);
            if (replacement) {
              expect(service.connectedId, 'board-2');
              expect(events.map((event) => event.connectionState), [
                DeviceConnectionState.connected,
              ]);
            } else {
              expect(events, isEmpty);
            }
            expect(rosters.last, ['board-1', 'board-2']);
            expect(
              rosters.where((roster) => roster.isEmpty).length,
              emptyRosters,
            );
            board.heldWrite.complete();
            await turns();
            expect(
              board.scanStarts,
              replacement ? 1 : 2,
              reason: 'late old SDK completion cannot restart scan',
            );
            expect(service.isConnected, replacement);
            if (replacement) expect(service.connectedId, 'board-2');
            expect(board.connections.length, replacement ? 2 : 1);
          },
        );
      }
    }
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final failure in ['disconnected', 'error', 'done']) {
      test(
        'normal native $failure retains product connection events on $platform',
        () async {
          debugDefaultTargetPlatformOverride = platform;
          addTearDown(() => debugDefaultTargetPlatformOverride = null);
          final board = OwnerPairingBoard()
            ..state = 5
            ..flags = 7;
          final service = LitchBleService(
            ble: board,
            pendingOtaStore: MemoryOtaTargetStore(),
          );
          addTearDown(() => disposeBoard(service, board));
          final events = <ConnectionStateUpdate>[];
          final subscription = service.connectionStream.listen(events.add);
          addTearDown(subscription.cancel);
          await connectBoard(service, board);
          await turns();
          expect(await service.ensureProductReady('board-1'), isTrue);
          expect(events.map((event) => event.connectionState), [
            DeviceConnectionState.connected,
          ]);
          final native = board.connections.single;
          switch (failure) {
            case 'disconnected':
              native.add(
                const ConnectionStateUpdate(
                  deviceId: 'board-1',
                  connectionState: DeviceConnectionState.disconnected,
                  failure: null,
                ),
              );
            case 'error':
              native.addError(
                StateError('controlled normal native disconnect'),
              );
            case 'done':
              unawaited(native.close());
          }
          await turns();
          expect(events.map((event) => event.connectionState), [
            DeviceConnectionState.connected,
            DeviceConnectionState.disconnected,
          ]);
          expect(service.isConnected, isFalse);
          expect(board.connections, hasLength(1));
        },
      );
    }
  }

  test(
    'failed claim intents reject the sixty-fifth identity before any dispatch',
    () async {
      final board = OwnerPairingBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await _fillFailedClaimIntents(service, board);
      board.identity[0] = 65;
      board.state = 1;
      board.flags = 4;
      board.reason = 0;
      ++board.revision;
      await connectBoard(service, board, deviceId: 'claim-limit-overflow');
      await turns();
      expect(
        board.pairingCommands,
        hasLength(64),
        reason:
            'capacity must reject a new identity before an actual pairing Begin',
      );
      expect(service.debugOwnerClaimIntentCount, 64);
      expect(service.ownerPairingClaimLimitReached, isTrue);
      expect(
        await service.ensureProductReady('claim-limit-overflow', retry: true),
        isFalse,
      );
      expect(board.pairingCommands, hasLength(64));
      expect(service.debugOwnerClaimIntentCount, 64);
      await service.disconnect();
      board.identity[0] = 1;
      board.state = 1;
      board.flags = 4;
      board.reason = 0;
      ++board.revision;
      await connectBoard(service, board, deviceId: 'known-native-alias');
      await turns();
      expect(
        board.pairingCommands,
        hasLength(64),
        reason: 'the oldest spent identity was never evicted',
      );
      expect(service.ownerPairingClaimLimitReached, isFalse);
      final retry = service.ensureProductReady(
        'known-native-alias',
        retry: true,
      );
      await turns();
      expect(board.pairingCommands, hasLength(65));
      expect(service.debugOwnerClaimIntentCount, 64);
      board.setState(1, nextFlags: 4, nextReason: 7);
      expect(await retry, isFalse);
      final ready = service.ensureProductReady(
        'known-native-alias',
        retry: true,
      );
      await turns();
      board.setState(5, nextFlags: 7);
      expect(await ready, isTrue);
      expect(
        service.debugOwnerClaimIntentCount,
        63,
        reason: 'verified Ready releases one retained intent',
      );
      final remembered = (await SharedPreferences.getInstance()).getString(
        'owned_litch_boards_v1',
      );
      expect(
        remembered,
        isNotNull,
        reason: 'verified board and history storage identity is retained',
      );
      await service.disconnect();
      board.identity[0] = 65;
      board.state = 1;
      board.flags = 4;
      board.reason = 0;
      ++board.revision;
      await connectBoard(service, board, deviceId: 'overflow-after-ready');
      await turns();
      expect(board.pairingCommands, hasLength(67));
      expect(service.debugOwnerClaimIntentCount, 64);
      await service.dispose();
      expect(
        service.debugOwnerClaimIntentCount,
        0,
        reason: 'dispose ends and clears every retained intent',
      );
      board.setState(5, nextFlags: 7);
      await turns();
      expect(
        service.debugOwnerClaimIntentCount,
        0,
        reason: 'late prior-session proof cannot repopulate disposed storage',
      );
      final reopenedBoard = OwnerPairingBoard();
      reopenedBoard.identity[0] = 65;
      final reopened = LitchBleService(
        ble: reopenedBoard,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(reopened, reopenedBoard));
      await connectBoard(
        reopened,
        reopenedBoard,
        deviceId: 'fresh-app-overflow',
      );
      await turns();
      expect(
        reopenedBoard.pairingCommands,
        hasLength(1),
        reason:
            'a reopened app session can attempt the previously blocked board',
      );
      expect(reopened.debugOwnerClaimIntentCount, 1);
      expect(reopened.ownerPairingClaimLimitReached, isFalse);
      expect(
        (await SharedPreferences.getInstance()).getString(
          'owned_litch_boards_v1',
        ),
        remembered,
        reason:
            'intent disposal/reopen preserves the saved board and its history storage key',
      );
      reopenedBoard.setState(1, nextFlags: 4, nextReason: 7);
      expect(await reopened.ensureProductReady('fresh-app-overflow'), isFalse);
    },
  );

  test(
    'a rejected Identify probe stops the request on either ownership profile',
    () async {
      for (final private in [true, false]) {
        final board = OwnerPairingBoard()
          ..state = 6
          ..flags = 5
          ..typedIdentifySupported = false
          ..capabilityMask = private ? 671 : 31;
        final service = LitchBleService(
          ble: board,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        await connectBoard(service, board);
        await turns();
        expect(
          await service.sendIdentify('board-1', 7, durationMs: 3000),
          isFalse,
        );
        expect(board.identifyCommands, isNotEmpty);
        expect(
          board.directIdentifyCommands.where((command) => command['cmd'] == 'identify'),
          isEmpty,
        );
        expect(board.pairingCommands, isEmpty);
        if (private) {
          expect(board.protectedReads, isEmpty);
          expect(service.productReady, isFalse);
        }
        await disposeBoard(service, board);
      }
    },
  );

  test(
    'Identify cooldown rejection leaves the actual owner connection usable and Stop free',
    () async {
      final board = OwnerPairingBoard()
        ..state = 5
        ..flags = 7;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      expect(await service.ensureProductReady('board-1'), isTrue);
      board.identifyRejection = 0x1c01;
      expect(
        await service.sendIdentify('board-1', 7, durationMs: 3000),
        isFalse,
      );
      expect(service.isConnected, isTrue);
      expect(service.productReady, isTrue);
      expect(await service.sendIdentifyStop('board-1'), isTrue);
      expect(board.identifyCommands.map((p) => p.body[1]), [0, 1, 0, 2]);
      expect(board.pairingCommands, isEmpty);
    },
  );

  test(
    'retired temporary Identify cannot claim or tear down a newer real connection',
    () async {
      final board = _HeldIdentifyBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      final identifying = service.sendIdentify('board-1', 7, durationMs: 3000);
      for (var turn = 0; turn < 60 && board.connections.isEmpty; ++turn) {
        await turns(1);
      }
      board.connections.single.add(
        const ConnectionStateUpdate(
          deviceId: 'board-1',
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      );
      for (var turn = 0; turn < 60 && !board.holding; ++turn) {
        await turns(1);
      }
      expect(board.holding, isTrue);
      await connectBoard(service, board, deviceId: 'board-2');
      expect(await identifying, isFalse);
      board.heldWrite.complete();
      await turns();
      expect(service.isConnected, isTrue);
      expect(service.connectedId, 'board-2');
      expect(
        board.pairingCommands,
        hasLength(1),
        reason:
            'only the fresh real connection may offer physical confirmation',
      );
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-2'), isTrue);
    },
  );

  for (final failed in [false, true]) {
    test(
      'background keeps shared heartbeat without owner access failed=$failed',
      () async {
        final board = OwnerPairingBoard();
        final service = LitchBleService(
          ble: board,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        addTearDown(() => disposeBoard(service, board));
        await connectBoard(service, board);
        await turns();
        if (failed) {
          board.setState(1, nextFlags: 4, nextReason: 7);
          expect(await service.ensureProductReady('board-1'), isFalse);
        }
        final token = service.connectionSessionToken;
        await service.setAppForegroundActive(false);
        final heartbeats = board.heartbeats;
        await Future<void>.delayed(const Duration(milliseconds: 4100));
        expect(board.heartbeats, greaterThan(heartbeats));
        expect(service.connectionSessionToken, token);
        expect(service.isConnected, isTrue);
        expect(service.productReady, isFalse);
        expect(board.pairingCommands, hasLength(1));
        expect(board.productQueries, isEmpty);
        expect(board.protectedReads, isEmpty);
      },
    );

    test(
      'foreground resumes pairing heartbeat after failed=$failed without retry',
      () async {
        final board = OwnerPairingBoard();
        final service = LitchBleService(
          ble: board,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        addTearDown(() => disposeBoard(service, board));
        await connectBoard(service, board);
        await turns();
        final preparing = service.ensureProductReady('board-1');
        if (failed) {
          board.setState(1, nextFlags: 4, nextReason: 7);
          expect(await preparing, isFalse);
        }
        await service.setAppForegroundActive(false);
        await turns();
        final heartbeatsBeforeResume = board.heartbeats;
        final resumed = service.setAppForegroundActive(true);
        await turns();
        expect(board.heartbeats, greaterThan(heartbeatsBeforeResume));
        expect(board.pairingCommands, hasLength(1));
        expect(board.productQueries, isEmpty);
        expect(service.isConnected, isTrue);
        expect(service.productReady, isFalse);
        board.setState(5, nextFlags: 7);
        await resumed;
        await turns();
        expect(service.productReady, isTrue);
        expect(board.pairingCommands, hasLength(1));
      },
    );
  }

  test(
    'proven owner retains authority and pending receipt in background then revokes on Clearing',
    () async {
      final board = _HeldControlBoard()
        ..state = 5
        ..flags = 7
        ..attempt = 1;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(await service.ensureProductReady('board-1'), isTrue);

      final home = _wireFixture('home_state_wire_v1.csv', 'representative');
      final settings = _wireFixture('settings_state_wire_v3.csv', 'defaults');
      for (var i = 0; i < HomeStateWireCodec.fragmentCount; ++i) {
        board.emitState(
          HomeStateWireCodec.firstStateCode + i,
          home.sublist(i * 10, (i + 1) * 10),
          sequence: 7,
        );
      }
      for (var i = 0; i < SettingsStateWireCodec.fragmentCount; ++i) {
        board.emitState(
          SettingsStateWireCodec.fragmentCode(i),
          settings.sublist(
            i * 10,
            i * 10 + SettingsStateWireCodec.fragmentLength(i),
          ),
          sequence: 7,
        );
      }
      await turns();
      final retainedHome = service.latestHomeState;
      expect(retainedHome, isNotNull);
      final brightness = service.latestSettingValue('BRIGHTNESS');
      final events = <LitchControlEvent>[];
      final subscription = service.controlActionEvents.listen(events.add);
      addTearDown(subscription.cancel);
      var completed = false;
      final pending = service
          .dispatchControlAction(
            deviceId: 'board-1',
            action: LitchControlAction.hint,
          )
          .whenComplete(() => completed = true);
      addTearDown(() async {
        board.completeControl();
        await pending;
      });
      await turns();
      expect(board.pendingControl, isNotNull);

      Future<void> expectRetainedAuthority() async {
        await turns();
        expect(service.productReady, isTrue);
        expect(
          service.settingsReadAuthority('board-1'),
          LitchSettingsReadAuthority.confirmed,
        );
        expect(service.latestSettingValue('BRIGHTNESS'), brightness);
        expect(service.latestHomeState, same(retainedHome));
        expect(completed, isFalse);
        expect(events.map((event) => event.phase), [LitchControlPhase.pending]);
      }

      await expectRetainedAuthority();
      final queriesBeforePrompt = board.productQueries.length;
      for (final state in [9, 5]) {
        board.revision++;
        board.emitState(40, [1, state, 0, 7, 1, 0, 0, 0]);
        await expectRetainedAuthority();
        board.emitState(41, board.identity.sublist(0, 8));
        await expectRetainedAuthority();
        board.emitState(42, board.identity.sublist(8));
        await expectRetainedAuthority();
      }
      expect(
        board.productQueries.length,
        queriesBeforePrompt,
        reason: 'Fragment assembly must not restart product setup.',
      );
      await service.setAppForegroundActive(false);
      await expectRetainedAuthority();
      board.completeControl();
      expect((await pending).phase, LitchControlPhase.applied);

      board.emitState(40, [1, 7, 0, 5, 1, 0, 0, 0], sequence: ++board.revision);
      expect(service.productReady, isFalse);
      expect(service.latestHomeState, isNull);
      expect(
        service.settingsReadAuthority('board-1'),
        LitchSettingsReadAuthority.unavailable,
      );
    },
  );

  test(
    'modern version errors preserve confirmed settings without Console fallback',
    () async {
      final board = _BusyVersionBoard()
        ..state = 5
        ..flags = 7;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(await service.ensureProductReady('board-1'), isTrue);
      final settings = _wireFixture('settings_state_wire_v3.csv', 'defaults');
      for (var i = 0; i < SettingsStateWireCodec.fragmentCount; ++i) {
        board.emitState(
          SettingsStateWireCodec.fragmentCode(i),
          settings.sublist(
            i * 10,
            i * 10 + SettingsStateWireCodec.fragmentLength(i),
          ),
          sequence: 7,
        );
      }
      await turns();
      expect(
        service.settingsReadAuthority('board-1'),
        LitchSettingsReadAuthority.confirmed,
      );
      final brightness = service.latestSettingValue('BRIGHTNESS');
      expect(
        await service.fetchBoardFirmwareVersion(deviceId: 'board-1'),
        isNull,
      );
      expect(board.versionRequests, 3);
      expect(
        service.deviceCapabilities('board-1').supportsConnectionSession,
        isTrue,
      );
      expect(
        service.consoleLinesSnapshot.any(
          (line) => line.contains('console fallback'),
        ),
        isFalse,
      );
      expect(service.productReady, isTrue);
      expect(
        service.settingsReadAuthority('board-1'),
        LitchSettingsReadAuthority.confirmed,
      );
      expect(service.latestSettingValue('BRIGHTNESS'), brightness);
    },
  );

  test(
    'capability and version refresh during first claim retain current profile and owner readiness',
    () async {
      final board = _OwnerBlockedInfoBoard()..dropFirstVersionReply = false;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(
        service.deviceCapabilities('board-1').supportsConnectionSession,
        isTrue,
      );
      expect(service.productReady, isFalse);
      final capabilities = service.deviceCapabilities('board-1');
      expect(
        await service.fetchDeviceCapabilities(
          deviceId: 'board-1',
          refresh: true,
        ),
        same(capabilities),
      );
      expect(board.infoRequests, 1);
      expect(service.isConnected, isTrue);
      var completed = false;
      final reading = service
          .fetchBoardFirmwareVersion(deviceId: 'board-1')
          .whenComplete(() => completed = true);
      await turns();
      expect(board.versionRequests, 0);
      expect(
        service.deviceCapabilities('board-1').supportsConnectionSession,
        isTrue,
      );
      expect(
        completed,
        isFalse,
        reason: 'the hidden firmware page must join pairing',
      );
      board.setState(3);
      await turns();
      board.setState(4);
      await turns();
      expect(board.versionRequests, 0);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(await reading, board.runningVersion);
      expect(board.versionRequests, 1);
      expect(service.productReady, isTrue);
      expect(
        service.deviceCapabilities('board-1').supportsConnectionSession,
        isTrue,
      );
    },
  );

  test(
    'actual connection gates product reads until durable owner proof and preserves history aliases',
    () async {
      final board = OwnerPairingBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(service.isConnected, isTrue);
      expect(service.productReady, isFalse);
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.awaitPhysical,
      );
      expect(board.pairingCommands, hasLength(1));
      expect(board.productQueries, isEmpty);
      expect(board.protectedReads, isEmpty);
      final settings = service.readSettings('board-1');
      final console = service.sendConsoleLine('board-1', 'status');
      await turns();
      expect(board.productQueries, isEmpty);
      expect(board.protectedReads, isEmpty);
      board.setState(3);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(4);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(5, nextFlags: 6);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(service.productReady, isTrue);
      expect(service.durableBoardIdentity, '0102030405060708090a0b0c0d0e0f10');
      await settings;
      expect(board.protectedReads, isNotEmpty);
      await console;
      expect(board.productQueries, isNotEmpty);
      final records = await service.rememberedOwnedBoards();
      expect(records.keys.single, service.durableBoardIdentity);
      final originalHistoryKey = await service.historyStorageIdentity(
        'board-1',
      );
      board.state = 6;
      board.flags = 5;
      board.attempt = 0;
      await connectBoard(service, board, deviceId: 'new-platform-alias');
      await turns();
      expect(service.productReady, isFalse);
      expect(service.ownerPairingBusy, isTrue);
      board.attempt = 7;
      board.setState(3);
      await turns();
      expect(service.productReady, isFalse);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('new-platform-alias'), isTrue);
      expect(
        board.pairingCommands,
        hasLength(1),
        reason: 'Owner reconnect is quiet.',
      );
      expect(
        await service.historyStorageIdentity('new-platform-alias'),
        originalHistoryKey,
      );
      expect((await service.rememberedOwnedBoards()).values.single.aliases, [
        'board-1',
        'new-platform-alias',
      ]);
    },
  );

  test(
    'corrupt phone registry cannot veto proven owner or overwrite retained history identity data',
    () async {
      final preferences = await SharedPreferences.getInstance();
      const raw = '{"damaged-board-entry": "keep these bytes"}';
      await preferences.setString('owned_litch_boards_v1', raw);
      final board = OwnerPairingBoard()
        ..state = 6
        ..flags = 5;
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      expect(service.productReady, isFalse);
      expect(board.pairingCommands, isEmpty);
      board.setState(5, nextFlags: 7);
      expect(await service.ensureProductReady('board-1'), isTrue);
      expect(service.productReady, isTrue);
      expect(preferences.getString('owned_litch_boards_v1'), raw);
      await expectLater(
        service.historyStorageIdentity('board-1'),
        throwsFormatException,
      );
    },
  );

  test(
    'unknown discovery pauses Home; late proof resumes bridges, Home and history on the same link',
    () async {
      final board = OwnerPairingBoard()
        ..state = 0
        ..flags = 4
        ..capabilityReplyGate = Completer<void>();
      final service = _DelayedPlayerEndService(board);
      final directory = await Directory.systemTemp.createTemp(
        'litch-pairing-history-',
      );
      final repository = GameLibraryRepository(
        GameLibraryStore(Directory('${directory.path}/library')),
      );
      await connectBoard(service, board);
      final feed = HomeBoardFeed(bleService: service, boardId: 'board-1');
      final ingestion = PhysicalGameHistoryIngestion(
        bleService: service,
        repository: repository,
        receiveDirectory: Directory('${directory.path}/receive'),
      )..start();
      addTearDown(() async {
        if (!service.delayedEnd.isCompleted) service.delayedEnd.complete(null);
        await ingestion.close();
        await feed.dispose();
        await disposeBoard(service, board);
        await repository.close();
        await directory.delete(recursive: true);
      });
      final refreshing = feed.refresh();
      expect(
        service.heldEnd,
        isFalse,
        reason: 'Unknown capabilities do not authorize a player-end read.',
      );
      board.capabilityReplyGate!.complete();
      await refreshing;
      await turns();
      expect(await service.ensureProductReady('board-1'), isFalse);
      expect(
        feed.current.isRefreshing,
        isFalse,
        reason:
            'The owner gate keeps Home paused until product access is proven.',
      );
      expect(board.productQueries, isEmpty);
      expect(board.historyRequests, 0);
      expect(
        board.subscriptions.where(
          (c) => c.characteristicId == BleProtocol.consoleStreamCharacteristic,
        ),
        isEmpty,
      );
      final token = service.connectionSessionToken;
      final home = _wireFixture('home_state_wire_v1.csv', 'representative');
      final settings = _wireFixture('settings_state_wire_v3.csv', 'defaults');
      settings[26] = 2;
      settings[27] = 0;
      var productRevision = 100;
      void publishProduct() {
        productRevision++;
        for (var i = 0; i < HomeStateWireCodec.fragmentCount; i++) {
          board.emitState(
            HomeStateWireCodec.firstStateCode + i,
            home.sublist(i * 10, (i + 1) * 10),
            sequence: productRevision,
          );
        }
        for (var i = 0; i < SettingsStateWireCodec.fragmentCount; i++) {
          board.emitState(
            SettingsStateWireCodec.fragmentCode(i),
            settings.sublist(
              i * 10,
              i * 10 + SettingsStateWireCodec.fragmentLength(i),
            ),
            sequence: productRevision,
          );
        }
      }

      board.onProductQuery = (code) {
        if (code == 0) publishProduct();
      };
      board.setState(5, nextFlags: 7);
      await turns(60);
      expect(service.productReady, isTrue);
      expect(service.connectionSessionToken, token);
      expect(board.connections, hasLength(1));
      expect(
        service.heldEnd,
        isTrue,
        reason: 'The orientation read starts after the first owner proof.',
      );
      expect(
        board.subscriptions.where(
          (c) => c.characteristicId == BleProtocol.consoleStreamCharacteristic,
        ),
        hasLength(1),
      );
      expect(service.hasBoardStateSnapshot, isTrue);
      expect(feed.hasCurrentBoardState, isTrue);
      expect(board.historyRequests, greaterThan(0));
      final initialHistoryRequests = board.historyRequests;
      final retainedPosition = feed.current.position;
      board.setState(6, nextFlags: 5);
      await turns();
      expect(feed.hasCurrentBoardState, isFalse);
      publishProduct(); // queued old domain data is not authority while blocked.
      service.delayedEnd.complete(1);
      await turns();
      await ingestion.idle;
      expect(feed.hasCurrentBoardState, isFalse);
      expect(feed.current.liveSequence, isNull);
      expect(
        feed.current.position,
        same(retainedPosition),
        reason:
            'The last known drawing is retained; blocked callbacks cannot replace it.',
      );
      expect(feed.current.physicalStatus?.p1AtOppositeEnd, isTrue);
      board.returnAbsentHistory = true;
      board.setState(5, nextFlags: 7);
      await turns(60);
      await ingestion.idle;
      expect(service.connectionSessionToken, token);
      expect(feed.hasCurrentBoardState, isTrue);
      expect(
        feed.current.physicalStatus?.p1AtOppositeEnd,
        isTrue,
        reason:
            'Late prior-generation end read cannot replace current orientation.',
      );
      expect(board.historyRequests, greaterThan(initialHistoryRequests));
    },
  );

  test(
    'Cancel follows Begin ACK; interrupted native claim cannot unlock next generation',
    () async {
      final board = OwnerPairingBoard();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      addTearDown(() => disposeBoard(service, board));
      await connectBoard(service, board);
      await turns();
      final preparing = service.ensureProductReady('board-1');
      await service.cancelOwnerPairing();
      expect(await preparing, isFalse);
      expect(board.pairingCommands.map((p) => p.body[1]), [1, 2]);
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.unpaired,
      );
      expect(service.productReady, isFalse);
      final interrupted = service.ensureProductReady('board-1', retry: true);
      await turns();
      board.setState(3);
      await service.disconnect();
      expect(await interrupted, isFalse);
      board.state = 1;
      board.flags = 4;
      await connectBoard(service, board);
      await turns();
      board.emitState(
        40,
        [1, 5, 0, 7, 1, 0, 0, 0],
        token: 0x800000ff,
        sequence: board.revision + 10,
      );
      await turns();
      expect(service.productReady, isFalse);
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.unpaired,
      );
      expect(board.productQueries, isEmpty);
      expect(board.pairingCommands.map((p) => p.body[1]), [1, 2, 1]);
      final newRetry = service.ensureProductReady('board-1', retry: true);
      await turns();
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.awaitPhysical,
      );
      board.setState(5, nextFlags: 7);
      expect(await newRetry, isTrue);
    },
  );

  test(
    'received owner state with unpromoted identity reports the durability predicate',
    () async {
      final board = OwnerPairingBoard()
        ..state = 0
        ..flags = 1;
      var dropStateQueries = false;
      final session = ConnectionSession(
        packets: board.sessionPackets.stream,
        send: (bytes) async {
          final packet = ConnectionSessionCodec.decode(bytes)!;
          if (dropStateQueries &&
              packet.kind == ConnectionSessionKind.control &&
              packet.code == ConnectionSessionControl.query &&
              packet.sequence == 40) {
            return;
          }
          await board.sharedWrite(bytes);
        },
        clientNonce: 1,
        recoveryProbeDelay: const Duration(milliseconds: 5),
        recoveryFailureTimeout: const Duration(milliseconds: 100),
      );
      await session.activate();
      final pairing = OwnerPairingSession(
        session,
        readinessTimeout: const Duration(milliseconds: 40),
      );
      expect((await pairing.readStatus()).flags, 1);
      await expectLater(
        pairing.waitForInitialIdentity(),
        throwsA(isA<OwnerIdentityNotDurable>()),
      );
      expect(session.isActivated, isTrue);
      expect(pairing.ready, isFalse);
      board.setState(5, nextFlags: 7);
      await turns();
      expect(
        pairing.ready,
        isTrue,
        reason: 'Late verified promotion still finishes this same live owner.',
      );
      await pairing.dispose();
      dropStateQueries = true;
      final noResponse = OwnerPairingSession(
        session,
        readinessTimeout: const Duration(milliseconds: 40),
      );
      await expectLater(
        noResponse.waitForInitialIdentity(),
        throwsA(isA<TimeoutException>()),
      );
      expect(noResponse.status, isNull);
      expect(session.isActivated, isTrue);
      await noResponse.dispose();
      await session.dispose();
      await board.sessionPackets.close();
      await board.packets.close();
    },
  );

  test(
    'shared reader repairs lost identity halves and observes revocation during repair',
    () async {
      final board = OwnerPairingBoard()
        ..state = 5
        ..flags = 7
        ..attempt = 1
        ..revision = 2
        ..includeIdentity = false;
      final session = ConnectionSession(
        packets: board.sessionPackets.stream,
        send: board.sharedWrite,
        clientNonce: 1,
        recoveryProbeDelay: const Duration(milliseconds: 5),
        recoveryFailureTimeout: const Duration(milliseconds: 100),
      );
      await session.activate();
      final pairing = OwnerPairingSession(session);
      var snapshotQueries = 0;
      board.onStateQuery = (code) {
        if (code != 40) return;
        if (++snapshotQueries == 2) {
          board.emitState(41, List<int>.filled(8, 0), sequence: 1);
          board.emitState(42, board.identity.sublist(8));
        }
        if (snapshotQueries == 3) board.includeIdentity = true;
      };
      await pairing.readStatus();
      expect(pairing.ready, isTrue);
      expect(
        snapshotQueries,
        3,
        reason: 'Shared reader retries until both halves match current State.',
      );
      await pairing.dispose();
      final next = OwnerPairingSession(session);
      board.includeIdentity = false;
      var revokeQueries = 0;
      board.onStateQuery = (code) {
        if (code == 40 && ++revokeQueries == 2) {
          board.state = 7;
          board.flags = 5;
          board.revision++;
          board.includeIdentity = true;
        }
      };
      final status = await next.readStatus();
      expect(status.state, OwnerPairingSessionState.clearing);
      expect(
        next.ready,
        isFalse,
        reason:
            'A prior Ready cannot survive newer revoke during identity repair.',
      );
      await next.dispose();
      await session.dispose();
      await board.sessionPackets.close();
      await board.packets.close();
    },
  );

  test(
    'coherent identity comparison ignores mixed revisions, stale replay and duplicates',
    () async {
      final board = OwnerPairingBoard();
      final session = ConnectionSession(
        packets: board.sessionPackets.stream,
        send: board.sharedWrite,
        clientNonce: 1,
        recoveryProbeDelay: const Duration(milliseconds: 5),
        recoveryFailureTimeout: const Duration(milliseconds: 100),
      );
      await session.activate();
      final pairing = OwnerPairingSession(session);
      addTearDown(() async {
        await pairing.dispose();
        await session.dispose();
        await board.sessionPackets.close();
        await board.packets.close();
      });
      await pairing.readStatus();
      const identity = '0102030405060708090a0b0c0d0e0f10';
      expect(pairing.durableIdentity, identity);
      final pairingStatus = pairing.status;
      board.emitState(43, [1, 7, 0, 0, 0, 0x1f], sequence: 900);
      await turns();
      expect(
        pairing.status,
        same(pairingStatus),
        reason:
            'The private pairing consumer ignores additive assistance State.',
      );
      expect(pairing.durableIdentity, identity);

      final ready = [1, 5, 0, 7, 1, 0, 0, 0];
      board.emitState(40, ready, sequence: 2);
      board.emitState(42, board.identity.sublist(8), sequence: 2);
      expect(
        pairing.durableIdentity,
        identity,
        reason: 'Existing proof survives assembly of a newer revision.',
      );
      expect(pairing.ready, isTrue);
      board.emitState(41, List<int>.filled(8, 0), sequence: 1);
      board.emitState(41, board.identity.sublist(0, 8), sequence: 2);
      expect(pairing.durableIdentity, identity);
      expect(pairing.ready, isTrue);
      board.emitState(42, List<int>.filled(8, 0), sequence: 1);
      board.emitState(42, board.identity.sublist(8), sequence: 2);
      expect(
        pairing.ready,
        isTrue,
        reason: 'Stale halves and duplicate callbacks cannot replace proof.',
      );

      board.emitState(40, [1, 3, 0, 5, 1, 0, 0, 0], sequence: 3);
      board.identity[0] = 99;
      board.emitState(41, board.identity.sublist(0, 8), sequence: 3);
      expect(pairing.durableIdentity, identity);
      expect(pairing.ready, isFalse);
      final rejected = expectLater(
        pairing.waitUntilReady(),
        throwsA(
          isA<StateError>().having(
            (error) => error.message,
            'message',
            'Board identity changed during pairing.',
          ),
        ),
      );
      board.emitState(42, board.identity.sublist(8), sequence: 3);
      await rejected;
      expect(pairing.ready, isFalse);
    },
  );

  test(
    'newer revoke fences stale ready and a durable identity cannot change',
    () async {
      final board = OwnerPairingBoard()
        ..state = 5
        ..flags = 7
        ..attempt = 1;
      final session = ConnectionSession(
        packets: board.sessionPackets.stream,
        send: board.sharedWrite,
        clientNonce: 1,
        recoveryProbeDelay: const Duration(milliseconds: 5),
        recoveryFailureTimeout: const Duration(milliseconds: 100),
      );
      await session.activate();
      final pairing = OwnerPairingSession(session);
      addTearDown(() async {
        await pairing.dispose();
        await session.dispose();
        await board.sessionPackets.close();
        await board.packets.close();
      });
      await pairing.readStatus();
      expect(pairing.ready, isTrue);
      board.setState(7, nextFlags: 5);
      board.emitState(40, [1, 5, 0, 7, 1, 0, 0, 0], sequence: 1);
      expect(pairing.ready, isFalse);
      expect(pairing.status?.state, OwnerPairingSessionState.clearing);
      board.identity[0] = 99;
      board.setState(5, nextFlags: 7);
      expect(pairing.ready, isFalse);
    },
  );

  testWidgets(
    'retained-owner recovery and clear consent name board actions at 200 percent text',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      try {
        final board = OwnerPairingBoard()
          ..state = 8
          ..flags = 5
          ..reason = 10;
        final service = LitchBleService(
          ble: board,
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        await tester.runAsync(() async {
          await connectBoard(service, board);
          await turns();
        });
        await tester.pumpWidget(
          MaterialApp(
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.pump();
        expect(find.textContaining('Restart your Litch'), findsOneWidget);
        expect(find.byKey(const ValueKey('owner-pairing-retry')), findsNothing);
        expect(
          find.byWidgetPredicate(
            (widget) =>
                widget is Semantics && widget.properties.liveRegion == true,
          ),
          findsOneWidget,
        );
        expect(
          tester
              .getSize(find.byKey(const ValueKey('owner-pairing-cancel')))
              .height,
          greaterThanOrEqualTo(48),
        );
        board.setState(8, nextFlags: 4, nextReason: 10);
        await tester.pump();
        expect(find.textContaining('Restart your Litch'), findsOneWidget);
        expect(find.byKey(const ValueKey('owner-pairing-retry')), findsNothing);
        expect(
          board.pairingCommands,
          isEmpty,
          reason:
              'Failed requires board reconciliation even without a retained owner.',
        );
        board.setState(7, nextReason: 9);
        await tester.pump();
        expect(
          find.text('Ownership is being cleared on the board.'),
          findsOneWidget,
        );
        expect(find.byType(CircularProgressIndicator), findsOneWidget);
        expect(find.byKey(const ValueKey('owner-pairing-retry')), findsNothing);
        board.attempt = 7;
        board.setState(9, nextFlags: 4);
        await tester.pump();
        expect(find.textContaining('two seconds'), findsOneWidget);
        expect(find.byKey(const ValueKey('owner-pairing-retry')), findsNothing);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => disposeBoard(service, board));
      } finally {
        semantics.dispose();
      }
    },
  );

  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets(
      'quiet owner reconnect waits through native proof without asking for another claim $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final semantics = tester.ensureSemantics();
        try {
          final board = OwnerPairingBoard()
            ..state = 6
            ..flags = 5;
          final service = LitchBleService(
            ble: board,
            pendingOtaStore: MemoryOtaTargetStore(),
          );
          await tester.runAsync(() async {
            await connectBoard(service, board);
            await turns();
          });
          await tester.pumpWidget(
            MaterialApp(home: ConnectionGateScreen(bleService: service)),
          );
          await tester.pump();
          final retainedMain = tester.state(
            find.byType(LitchMainScaffold, skipOffstage: false),
          );
          expect(
            find.byType(LitchMainScaffold, skipOffstage: false),
            findsOneWidget,
          );
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          final blockedMain = find.ancestor(
            of: find.byType(LitchMainScaffold, skipOffstage: false),
            matching: find.byWidgetPredicate(
              (widget) => widget is AbsorbPointer && widget.absorbing,
              skipOffstage: false,
            ),
          );
          expect(blockedMain, findsOneWidget);
          expect(
            find.ancestor(
              of: find.byType(LitchMainScaffold, skipOffstage: false),
              matching: find.byWidgetPredicate(
                (widget) => widget is ExcludeFocus && widget.excluding,
                skipOffstage: false,
              ),
            ),
            findsOneWidget,
          );
          expect(board.productQueries, isEmpty);
          expect(find.text('Securing the connection…'), findsNothing);
          expect(
            find.byKey(const ValueKey('chosen-board-cancel')),
            findsOneWidget,
          );
          expect(
            find.byWidgetPredicate(
              (widget) =>
                  widget is CircularProgressIndicator ||
                  widget is LinearProgressIndicator,
            ),
            findsOneWidget,
          );
          expect(find.bySemanticsLabel('Connecting…'), findsOneWidget);
          expect(find.textContaining('another device'), findsNothing);
          expect(find.text('X + O'), findsNothing);
          expect(board.pairingCommands, isEmpty);
          board.attempt = 7;
          board.setState(3);
          await tester.pump();
          expect(service.productReady, isFalse);
          expect(board.productQueries, isEmpty);
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          board.setState(5, nextFlags: 7);
          // State delivery and widget/preferences work cross the real and fake
          // event loops. Let both progress before awaiting completed proof.
          for (var pass = 0; pass < 30 && !service.productReady; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(service.productReady, isTrue);
          await tester.runAsync(() => service.ensureProductReady('board-1'));
          await tester.pump();
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          expect(
            tester.state(find.byType(LitchMainScaffold, skipOffstage: false)),
            same(retainedMain),
          );
          expect(service.productReady, isTrue);
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(
            find.byKey(const ValueKey('connection-progress-overlay')),
            findsOneWidget,
          );
          expect(
            find.descendant(
              of: find.byKey(const ValueKey('connection-progress-overlay')),
              matching: find.bySemanticsLabel('Connecting…'),
            ),
            findsOneWidget,
            reason: 'The same cue remains until current-link mode proof.',
          );
          expect(blockedMain, findsNothing);
          expect(board.pairingCommands, isEmpty);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.runAsync(() => disposeBoard(service, board));
        } finally {
          semantics.dispose();
        }
      },
    );
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      'Identify button keeps discovery visible through its actual typed temporary session $platform',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final board = _HeldIdentifyBoard()
          ..state = 6
          ..flags = 5
          ..reason = 8;
        final service = _IdentifyUiService(board);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme().copyWith(platform: platform),
            builder: (_, child) => RepaintBoundary(child: child!),
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.runAsync(() => turns());
        await tester.pump();
        final card = find.byKey(const ValueKey('discovered-board-board-1'));
        final action = find.descendant(
          of: card,
          matching: find.widgetWithText(OutlinedButton, 'Identify'),
        );
        expect(action.hitTestable(), findsOneWidget);
        expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
        await tester.tap(action);
        await tester.pump();
        await tester.runAsync(() async {
          for (var turn = 0; turn < 60 && board.connections.isEmpty; ++turn) {
            await turns(1);
          }
          expect(board.connections, hasLength(1));
          board.connections.single.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          for (var turn = 0; turn < 60 && !board.holding; ++turn) {
            await turns(1);
          }
        });
        await tester.pump();
        expect(board.holding, isTrue);
        expect(service.identifyConnectionActive, isTrue);
        expect(service.productReady, isFalse);
        expect(board.pairingCommands, isEmpty);
        expect(
          find.byKey(const ValueKey('connection-discovery-underlay')),
          findsOneWidget,
        );
        expect(card, findsOneWidget);
        expect(find.byKey(const ValueKey('owner-pairing-gate')), findsNothing);
        await captureHealthReview(
          tester,
          'identify-temporary-${platform.name}-held',
        );
        bool completed = false;
        bool? result;
        unawaited(
          service.identification!.then((value) {
            completed = true;
            result = value;
          }),
        );
        board.heldWrite.complete();
        for (var pass = 0; pass < 100 && !completed; ++pass) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() => turns(1));
        }
        expect(
          completed,
          isTrue,
          reason: 'the actual temporary connection cleanup must finish',
        );
        expect(result, isTrue);
        await tester.pump();
        expect(service.isConnected, isFalse);
        expect(card, findsOneWidget);
        await tester.pump(const Duration(milliseconds: 250));
        expect(board.identifyCommands.map((packet) => packet.body[1]), [0, 1]);
        expect(
          find.text('Sent identify #1 to First Litch.'),
          findsOneWidget,
          reason: tester
              .widgetList<Text>(find.byType(Text))
              .map((widget) => widget.data)
              .join(' | '),
        );
        expect(board.pairingCommands, isEmpty);
        expect(board.productQueries, isEmpty);
        await captureHealthReview(
          tester,
          'identify-temporary-${platform.name}-complete',
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => disposeBoard(service, board));
      },
      variant: TargetPlatformVariant.only(platform),
    );
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      'Connect while actual typed Identify is connected queues exactly one primary attempt on $platform',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final board = _HeldIdentifyBoard()
          ..state = 6
          ..flags = 5;
        final service = _IdentifyUiService(board);
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme().copyWith(platform: platform),
            builder: (_, child) => RepaintBoundary(child: child!),
            home: ConnectionGateScreen(bleService: service),
          ),
        );
        await tester.runAsync(() => turns());
        await tester.pump();
        final first = find.byKey(const ValueKey('discovered-board-board-1'));
        final second = find.byKey(const ValueKey('discovered-board-board-2'));
        await tester.tap(
          find.descendant(
            of: first,
            matching: find.widgetWithText(OutlinedButton, 'Identify'),
          ),
        );
        await tester.pump();
        await tester.runAsync(() async {
          for (var turn = 0; turn < 60 && board.connections.isEmpty; ++turn) {
            await turns(1);
          }
          board.connections.single.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          for (var turn = 0; turn < 60 && !board.holding; ++turn) {
            await turns(1);
          }
        });
        await tester.pump();
        expect(board.holding, isTrue);
        expect(service.isConnected, isTrue);
        expect(service.identifyConnectionActive, isTrue);
        final connect = find.descendant(
          of: second,
          matching: find.widgetWithText(FilledButton, 'Connect'),
        );
        expect(connect.hitTestable(), findsOneWidget);
        expect(tester.getSize(connect).height, greaterThanOrEqualTo(48));
        await tester.tap(connect);
        await tester.pump();
        expect(
          find.byKey(const ValueKey('connection-progress-overlay')),
          findsOneWidget,
          reason:
              'the enabled real Connect tap must queue selected-board progress while Identify owns transport',
        );
        expect(
          board.connections,
          hasLength(1),
          reason: 'normal connect waits for actual temporary cleanup',
        );
        await captureHealthReview(
          tester,
          'identify-connect-queued-${platform.name}',
        );
        board.heldWrite.complete();
        for (var pass = 0; pass < 100 && board.connections.length < 2; ++pass) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() => turns(1));
        }
        expect(board.connections, hasLength(2));
        board.connections.last.add(
          const ConnectionStateUpdate(
            deviceId: 'board-2',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        for (var pass = 0; pass < 10; ++pass) {
          await tester.pump();
          await tester.runAsync(() => turns(1));
        }
        expect(service.isConnected, isTrue);
        expect(service.connectedId, 'board-2');
        expect(service.identifyConnectionActive, isFalse);
        expect(board.connections, hasLength(2));
        expect(board.pairingCommands, isEmpty);
        await captureHealthReview(
          tester,
          'identify-connect-primary-${platform.name}',
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => disposeBoard(service, board));
      },
      variant: TargetPlatformVariant.only(platform),
    );
  }

  for (final (platform, cancelRetry, dropBeforeReady) in [
    (TargetPlatform.android, false, false),
    (TargetPlatform.android, true, false),
    (TargetPlatform.iOS, false, false),
    (TargetPlatform.iOS, false, true),
  ]) {
    testWidgets(
      'quiet owner proof timeout never claims another device on $platform cancel=$cancelRetry drop=$dropBeforeReady',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        final semantics = tester.ensureSemantics();
        final board = _OwnerRetryBoard()
          ..state = 6
          ..flags = 5;
        expect(
          OwnerPairingSession.defaultReadinessTimeout,
          const Duration(seconds: 20),
        );
        final service = _IdentifyUiService(
          board,
          ownerPairingReadinessTimeout: const Duration(seconds: 1),
        );
        try {
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme().copyWith(platform: platform),
              builder: (_, child) => RepaintBoundary(child: child!),
              home: ConnectionGateScreen(bleService: service),
            ),
          );
          await tester.runAsync(() => turns());
          await tester.pump();
          final first = find.byKey(const ValueKey('discovered-board-board-1'));
          await tester.tap(
            find.descendant(
              of: first,
              matching: find.widgetWithText(FilledButton, 'Connect'),
            ),
          );
          for (var pass = 0; pass < 100 && board.connections.isEmpty; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(board.connections, hasLength(1));
          board.connections.single.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          for (
            var pass = 0;
            pass < 100 && service.durableBoardIdentity == null;
            ++pass
          ) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          for (var pass = 0; pass < 10; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(
            service.ownerPairingStatus?.state,
            OwnerPairingSessionState.ownedOther,
          );
          expect(
            service.ownerPairingStatus?.reason,
            OwnerPairingSessionReason.none,
          );
          expect(service.ownerPairingBusy, isTrue);
          final session = await tester.runAsync(
            () => service.openConnectionSession('board-1'),
          );
          expect(session, isNotNull);
          expect(service.productReady, isFalse);
          expect(board.pairingCommands, isEmpty);
          expect(board.protectedReads, isEmpty);
          // Exercise the genuine State-reader timeout, shortened only for this
          // instance. Advance widget timers alongside its real Stopwatch.
          final deadline = Stopwatch()..start();
          while (service.ownerPairingBusy &&
              deadline.elapsed < const Duration(seconds: 3)) {
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 20)),
            );
            await tester.pump(const Duration(milliseconds: 20));
          }
          for (var pass = 0; pass < 10; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(service.isConnected, isTrue);
          expect(service.ownerPairingBusy, isFalse);
          expect(
            service.ownerPairingStatus?.state,
            OwnerPairingSessionState.ownedOther,
          );
          expect(
            service.ownerPairingStatus?.reason,
            OwnerPairingSessionReason.none,
          );
          expect(service.productReady, isFalse);
          expect(board.pairingCommands, isEmpty);
          await captureHealthReview(tester, 'quiet-timeout-${platform.name}');
          expect(
            find.textContaining('This Litch belongs to another device'),
            findsNothing,
            reason:
                'bounded quiet-proof timeout is not positive foreign ownership evidence',
          );
          final failure = find.text(
            'Couldn’t confirm this Litch. Try again, or disconnect.',
          );
          expect(failure, findsOneWidget);
          expect(find.text('Confirm your Litch'), findsOneWidget);
          expect(find.text('Connecting…'), findsNothing);
          expect(
            tester
                .getSemantics(failure)
                .getSemanticsData()
                .flagsCollection
                .isLiveRegion,
            isTrue,
          );
          expect(find.byType(CircularProgressIndicator), findsNothing);
          final retry = find.byKey(const ValueKey('owner-pairing-retry'));
          final disconnect = find.byKey(const ValueKey('owner-pairing-cancel'));
          for (final control in [retry, disconnect]) {
            expect(tester.getSize(control).height, greaterThanOrEqualTo(48));
            expect(
              tester
                  .getSemantics(control)
                  .getSemanticsData()
                  .hasAction(SemanticsAction.tap),
              isTrue,
            );
          }
          tester.platformDispatcher.textScaleFactorTestValue = 2;
          await tester.pump();
          for (final control in [retry, disconnect]) {
            final target = tester.getRect(control);
            expect(target.height, greaterThanOrEqualTo(48));
            expect(target.top, greaterThanOrEqualTo(0));
            expect(target.bottom, lessThanOrEqualTo(844));
          }
          expect(failure, findsOneWidget);
          expect(tester.takeException(), isNull);
          await captureHealthReview(
            tester,
            'quiet-timeout-2x-${platform.name}',
          );
          tester.platformDispatcher.clearTextScaleFactorTestValue();
          await tester.pump();
          final mainFinder = find.byType(
            LitchMainScaffold,
            skipOffstage: false,
          );
          final retainedMain = tester.state(mainFinder);
          final retainedDocument = tester
              .widget<LitchMainScaffold>(mainFinder)
              .craftController!
              .documentSession
              .document;
          await tester.tap(retry);
          for (var pass = 0; pass < 10; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(service.ownerPairingBusy, isTrue);
          expect(service.productReady, isFalse);
          expect(
            board.cancellationStarted.isCompleted,
            isTrue,
            reason:
                'Explicit retry must retire the native link that cannot prove ownership.',
          );
          expect(board.connections, hasLength(1));
          expect(session!.isActivated, isFalse);
          expect(board.pairingCommands, isEmpty);
          expect(board.productQueries, isEmpty);
          expect(board.protectedReads, isEmpty);
          expect(service.connectedId, 'board-1');
          expect(tester.state(mainFinder), same(retainedMain));
          expect(
            tester
                .widget<LitchMainScaffold>(mainFinder)
                .craftController!
                .documentSession
                .document,
            same(retainedDocument),
          );
          expect(
            find.byKey(const ValueKey('connection-progress-overlay')),
            findsOneWidget,
          );
          if (cancelRetry) {
            await tester.tap(find.byKey(const ValueKey('chosen-board-cancel')));
            await tester.pump();
            final second = find.byKey(
              const ValueKey('discovered-board-board-2'),
            );
            await tester.tap(
              find.descendant(
                of: second,
                matching: find.widgetWithText(FilledButton, 'Connect'),
              ),
            );
            for (var pass = 0; pass < 10; ++pass) {
              await tester.pump();
              await tester.runAsync(() => turns(1));
            }
            final secondFailure = lookupAppLocalizations(
              const Locale('en'),
            ).connectFailedBoard('Second Litch');
            expect(find.text(secondFailure), findsOneWidget);
            board.nativeDisconnected.complete();
            for (var pass = 0; pass < 20; ++pass) {
              await tester.pump();
              await tester.runAsync(() => turns(1));
            }
            expect(board.connections, hasLength(1));
            expect(
              find.text(secondFailure),
              findsOneWidget,
              reason:
                  'Old Retry completion must preserve the later board choice.',
            );
            expect(service.isConnected, isFalse);
            expect(service.productReady, isFalse);
            expect(board.pairingCommands, isEmpty);
            expect(
              find.byKey(const ValueKey('connection-progress-overlay')),
              findsNothing,
            );
            return;
          }
          board.nativeDisconnected.complete();
          for (
            var pass = 0;
            pass < 100 && board.connections.length < 2;
            ++pass
          ) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(board.connections, hasLength(2));
          board.connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'board-1',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          for (var pass = 0; pass < 10; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          if (dropBeforeReady) {
            board.connections.last.add(
              const ConnectionStateUpdate(
                deviceId: 'board-1',
                connectionState: DeviceConnectionState.disconnected,
                failure: null,
              ),
            );
            for (var pass = 0; pass < 30; ++pass) {
              await tester.pump();
              await tester.runAsync(() => turns(1));
            }
            expect(service.isConnected, isFalse);
            expect(service.productReady, isFalse);
            expect(
              find.byKey(const ValueKey('connection-progress-overlay')),
              findsNothing,
              reason: 'A retired fresh link must not leave Connecting stuck.',
            );
            expect(
              find.textContaining('Connection ended before setup finished.'),
              findsOneWidget,
            );
            expect(board.pairingCommands, isEmpty);
            return;
          }
          expect(
            await tester.runAsync(
              () => service.openConnectionSession('board-1'),
            ),
            isNot(same(session)),
          );
          expect(
            find.byKey(const ValueKey('owner-pairing-retry')),
            findsNothing,
          );
          await captureHealthReview(tester, 'quiet-retry-${platform.name}');
          board.setState(5, nextFlags: 7);
          for (var pass = 0; pass < 30; ++pass) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(service.productReady, isTrue);
          expect(service.ownerPairingBusy, isFalse);
          expect(
            service.ownerPairingStatus?.state,
            OwnerPairingSessionState.ownerReady,
          );
          expect(board.connections, hasLength(2));
          expect(board.pairingCommands, isEmpty);
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          expect(
            find.textContaining('This Litch belongs to another device'),
            findsNothing,
          );
          await captureHealthReview(tester, 'quiet-proof-${platform.name}');
          expect(tester.takeException(), isNull);
        } finally {
          if (!board.nativeDisconnected.isCompleted) {
            board.nativeDisconnected.complete();
          }
          await tester.pumpWidget(const SizedBox.shrink());
          bool disposed = false;
          unawaited(
            disposeBoard(service, board).then((_) {
              disposed = true;
            }),
          );
          for (var pass = 0; pass < 100 && !disposed; ++pass) {
            await tester.pump(const Duration(milliseconds: 10));
            await tester.runAsync(() => turns(1));
          }
          expect(disposed, isTrue);
          semantics.dispose();
        }
      },
      variant: TargetPlatformVariant.only(platform),
    );
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'pairing capacity has a spoken reopen recovery and reachable Cancel on $platform ${scale}x',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          final semantics = tester.ensureSemantics();
          final board = _ScanningIdentifyBoard();
          final service = LitchBleService(
            ble: board,
            pendingOtaStore: MemoryOtaTargetStore(),
            permissionGateway: _IdentifyPermissionGateway(),
          );
          await tester.runAsync(() async {
            await _fillFailedClaimIntents(service, board);
            board.identity[0] = 65;
            board.state = 1;
            board.flags = 4;
            board.reason = 0;
            ++board.revision;
          });
          final locale = Locale(scale == 2 ? 'de' : 'en');
          final copy = lookupAppLocalizations(locale).ownerPairingClaimLimit;
          await tester.pumpWidget(
            MaterialApp(
              locale: locale,
              theme: buildLitchTheme().copyWith(platform: platform),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => RepaintBoundary(
                child: MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(scale)),
                  child: child!,
                ),
              ),
              home: ConnectionGateScreen(bleService: service),
            ),
          );
          await tester.runAsync(() => service.startScan());
          for (final id in ['claim-limit-overflow', 'known-native-alias']) {
            board.discoveries.add(
              DiscoveredDevice(
                id: id,
                name: id == 'claim-limit-overflow'
                    ? 'Next Litch'
                    : 'Known Litch',
                serviceUuids: [BleProtocol.baseService],
                serviceData: const {},
                manufacturerData: Uint8List(0),
                rssi: -40,
                connectable: Connectable.available,
              ),
            );
          }
          await tester.runAsync(() => turns());
          await tester.pump();
          final card = find.byKey(
            const ValueKey('discovered-board-claim-limit-overflow'),
          );
          await tester.tap(
            find.descendant(of: card, matching: find.byType(FilledButton)),
          );
          for (
            var pass = 0;
            pass < 100 && board.connections.length < 65;
            ++pass
          ) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          expect(board.connections, hasLength(65));
          board.connections.last.add(
            const ConnectionStateUpdate(
              deviceId: 'claim-limit-overflow',
              connectionState: DeviceConnectionState.connected,
              failure: null,
            ),
          );
          for (
            var pass = 0;
            pass < 100 &&
                (service.ownerPairingBusy ||
                    !service.ownerPairingClaimLimitReached);
            ++pass
          ) {
            await tester.pump();
            await tester.runAsync(() => turns(1));
          }
          await tester.pump();
          expect(find.text('Next Litch'), findsOneWidget);
          expect(
            find.text(lookupAppLocalizations(locale).ownerPairingTitle),
            findsOneWidget,
          );
          final message = find.text(copy);
          expect(message, findsOneWidget);
          final spoken = tester.getSemantics(message).getSemanticsData();
          expect(spoken.label, copy);
          expect(spoken.flagsCollection.isLiveRegion, isTrue);
          expect(
            find.byKey(const ValueKey('owner-pairing-retry')),
            findsNothing,
          );
          expect(find.byType(CircularProgressIndicator), findsNothing);
          expect(service.ownerPairingClaimLimitReached, isTrue);
          expect(service.debugOwnerClaimIntentCount, 64);
          expect(board.pairingCommands, hasLength(64));
          final cancel = find.byKey(const ValueKey('owner-pairing-cancel'));
          await tester.ensureVisible(cancel);
          await tester.pump();
          expect(cancel.hitTestable(), findsOneWidget);
          expect(tester.getSize(cancel).height, greaterThanOrEqualTo(48));
          await captureHealthReview(
            tester,
            'pairing-capacity-${platform.name}-${locale.languageCode}-${scale}x',
          );
          await tester.tap(cancel);
          await tester.runAsync(() => turns());
          await tester.pump();
          expect(service.isConnected, isFalse);
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
          bool disposed = false;
          unawaited(
            (() async {
              await disposeBoard(service, board);
              await board.discoveries.close();
              disposed = true;
            })(),
          );
          for (var pass = 0; pass < 100 && !disposed; ++pass) {
            await tester.pump(const Duration(milliseconds: 10));
            await tester.runAsync(() => turns(1));
          }
          expect(disposed, isTrue);
          expect(service.debugOwnerClaimIntentCount, 0);
          semantics.dispose();
        },
        variant: TargetPlatformVariant.only(platform),
      );
    }
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'owned-other public instructions remain complete and actionable $platform ${scale}x',
        (tester) async {
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          final semantics = tester.ensureSemantics();
          final board = OwnerPairingBoard()
            ..state = 6
            ..flags = 5
            ..reason = 8;
          final service = _OwnerPairingUiService(board);
          await tester.runAsync(() async {
            await connectBoard(service, board);
            await turns();
          });
          await tester.pumpWidget(
            MaterialApp(
              theme: buildLitchTheme().copyWith(platform: platform),
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => RepaintBoundary(
                child: MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(scale)),
                  child: child!,
                ),
              ),
              home: ConnectionGateScreen(bleService: service),
            ),
          );
          await tester.pump();
          final message = find.textContaining(
            'Litch already has three paired Bluetooth devices.',
          );
          expect(message, findsOneWidget);
          final copy = tester.widget<Text>(message).data!;
          for (final step in [
            'Play mode',
            'long press X',
            'Quick Setup',
            'Left/Right',
            'Bluetooth icon',
            'same end',
            'X + O',
            'two seconds',
            'blue/orange',
            'then release',
            'Wait for clearing to finish',
            'try again',
          ]) {
            expect(copy, contains(step));
          }
          expect(copy.toLowerCase(), isNot(contains('d-pad')));
          expect(copy.toLowerCase(), isNot(contains('center')));
          expect(board.pairingCommands, isEmpty);
          expect(board.protectedReads, isEmpty);
          expect(service.isConnected, isFalse);
          await captureHealthReview(
            tester,
            'pairing-owned-other-${platform.name}-${scale}x-top',
          );
          final cancel = find.byKey(const ValueKey('owner-pairing-cancel'));
          await tester.ensureVisible(cancel);
          await tester.pump();
          expect(cancel.hitTestable(), findsOneWidget);
          expect(tester.getSize(cancel).height, greaterThanOrEqualTo(48));
          await captureHealthReview(
            tester,
            'pairing-owned-other-${platform.name}-${scale}x-bottom',
          );
          await tester.tap(cancel);
          await tester.pump();
          expect(
            find.byKey(const ValueKey('owner-pairing-gate')),
            findsNothing,
          );
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.runAsync(() => disposeBoard(service, board));
          semantics.dispose();
        },
        variant: TargetPlatformVariant.only(platform),
      );
    }
  }

  testWidgets(
    'terminal other-device refusal stays visible after ordinary disconnect',
    (tester) async {
      final board = OwnerPairingBoard()
        ..state = 6
        ..flags = 5;
      final service = _OwnerPairingUiService(board);
      await tester.runAsync(() async {
        await connectBoard(service, board);
        await turns();
      });
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      board.setState(6, nextFlags: 5, nextReason: 8);
      await tester.runAsync(() => turns());
      await tester.pump();
      expect(service.isConnected, isFalse);
      expect(find.byKey(const ValueKey('owner-pairing-gate')), findsOneWidget);
      expect(
        find.textContaining('three paired Bluetooth devices'),
        findsOneWidget,
      );
      expect(find.byType(CircularProgressIndicator), findsNothing);
      await tester.pump(const Duration(seconds: 2));
      expect(
        board.connections,
        hasLength(1),
        reason: 'Terminal refusal never starts automatic connection retries.',
      );
      await tester.tap(find.byKey(const ValueKey('owner-pairing-cancel')));
      await tester.runAsync(() => turns());
      await tester.pump();
      expect(find.byKey(const ValueKey('owner-pairing-gate')), findsNothing);
      expect(board.connections, hasLength(1));
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.runAsync(() => disposeBoard(service, board));
    },
  );

  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets(
      'quiet owner verification exposes semantic Cancel before native proof $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final semantics = tester.ensureSemantics();
        final board = OwnerPairingBoard()
          ..state = 6
          ..flags = 5;
        final service = _OwnerPairingUiService(board);
        await tester.runAsync(() async {
          await connectBoard(service, board);
          await turns();
        });
        await tester.pumpWidget(
          MaterialApp(home: ConnectionGateScreen(bleService: service)),
        );
        await tester.pump();
        final cancel = find.byKey(const ValueKey('chosen-board-cancel'));
        expect(
          tester
              .getSemantics(cancel)
              .getSemanticsData()
              .hasAction(SemanticsAction.tap),
          isTrue,
        );
        expect(tester.getSize(cancel).height, greaterThanOrEqualTo(48));
        await tester.tap(cancel);
        await tester.runAsync(() => turns());
        await tester.pump();
        expect(service.isConnected, isFalse);
        expect(service.productReady, isFalse);
        board.setState(5, nextFlags: 7);
        await tester.runAsync(() => turns());
        await tester.pump();
        expect(find.byType(LitchMainScaffold), findsNothing);
        expect(board.productQueries, isEmpty);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => disposeBoard(service, board));
        semantics.dispose();
      },
    );
  }

  testWidgets(
    'connect screen asks blue X+O and waits for durable owner readiness',
    (tester) async {
      final board = OwnerPairingBoard()
        ..capabilityReplyGate = Completer<void>();
      final service = LitchBleService(
        ble: board,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      await tester.runAsync(() async {
        await connectBoard(service, board);
        await turns();
      });
      await tester.pumpWidget(
        MaterialApp(home: ConnectionGateScreen(bleService: service)),
      );
      await tester.pump();
      expect(
        find.textContaining('Couldn’t confirm'),
        findsNothing,
        reason:
            'Capability discovery is loading, including capability-off boards.',
      );
      board.capabilityReplyGate!.complete();
      for (var turn = 0;
          turn < 60 &&
              service.ownerPairingStatus?.state !=
                  OwnerPairingSessionState.awaitPhysical;
          turn++) {
        await tester.pump(const Duration(milliseconds: 20));
        await tester.runAsync(() => turns(1));
      }
      await tester.pump();
      expect(board.pairingCommands, hasLength(1));
      expect(service.productReady, isFalse);
      expect(find.text('X + O'), findsOneWidget);
      expect(find.textContaining('within ten seconds'), findsOneWidget);
      expect(find.textContaining('At either end of the board'), findsOneWidget);
      expect(find.textContaining('hold'), findsNothing);
      board.setState(3);
      await tester.pump();
      expect(find.text('Confirmed. Connecting…'), findsOneWidget);
      expect(service.productReady, isFalse);
      board.setState(4);
      await tester.pump();
      expect(find.text('Confirmed. Connecting…'), findsOneWidget);
      expect(service.productReady, isFalse);
      expect(find.byKey(const ValueKey('owner-pairing-gate')), findsOneWidget);
      board.setState(5, nextFlags: 7);
      // Ready delivery resumes fake-zone preparation before preferences finish
      // on the outer scheduler. Progress both before awaiting that same work.
      for (var pass = 0; pass < 30 && !service.productReady; ++pass) {
        await tester.pump();
        await tester.runAsync(() => turns(1));
      }
      expect(service.productReady, isTrue);
      expect(
        service.ownerPairingStatus?.state,
        OwnerPairingSessionState.ownerReady,
      );
      expect(service.ownerPairingStatus?.identityDurable, isTrue);
      await tester.runAsync(() => service.ensureProductReady('board-1'));
      await tester.pump();
      expect(service.productReady, isTrue);
      expect(find.byKey(const ValueKey('owner-pairing-gate')), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.runAsync(() => disposeBoard(service, board));
    },
  );
}
