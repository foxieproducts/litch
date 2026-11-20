import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/board_health_status.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litch_end_palette.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/models/board_storage_recovery.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';

class _Ble extends Fake implements FlutterReactiveBle {
  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) async =>
      23;
}

/// Firmware boundary only: real session framing, query recovery, retained
/// outcomes, generation fencing and the app's typed adapter run unchanged.
class _BoardPeer implements LitchGattSessionFactory, LitchGattSession {
  final packets = StreamController<List<int>>.broadcast(sync: true);
  final List<ConnectionSessionPacket> actions = <ConnectionSessionPacket>[];
  final List<Uuid> subscribed = <Uuid>[];
  int status = 1;
  int revision = 1;
  int openCalls = 0;
  int queries = 0;
  int terminalThrough = 0;
  bool holdTerminal = false;
  Completer<void>? nextAction;
  bool malformedStatus = false;
  bool malformedStatusOnAction = false;
  bool holdStatusReply = false;
  int terminalState = 5;
  int rejection = 0;

  @override
  Future<LitchGattSession> open(String deviceId) async {
    openCalls++;
    return this;
  }

  @override
  Future<List<int>> read({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) async => <int>[91, 93];
  @override
  Stream<List<int>> subscribe({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) {
    subscribed.add(characteristicId);
    return packets.stream;
  }

  void _emit(ConnectionSessionPacket packet) =>
      packets.add(ConnectionSessionCodec.encode(packet));
  void _control(int code, int token, int sequence, Uint8List body) => _emit(
    ConnectionSessionPacket(
      kind: ConnectionSessionKind.control,
      code: code,
      token: token,
      sequence: sequence,
      body: body,
    ),
  );
  void grant({int received = 0, int? terminal}) {
    terminal ??= terminalThrough;
    final body = Uint8List(10);
    final data = ByteData.sublistView(body);
    data.setUint32(0, terminal, Endian.little);
    data.setUint32(4, terminal + 8, Endian.little);
    if (received > terminal) {
      data.setUint16(8, 1 << (received - terminal - 1), Endian.little);
    }
    _control(ConnectionSessionControl.progress, 2, terminal, body);
  }

  void state() => _emit(
    ConnectionSessionPacket(
      kind: ConnectionSessionKind.state,
      code: 38,
      token: 0x80000001,
      sequence: revision,
      body: Uint8List.fromList(
        malformedStatus
            ? <int>[1, 5, 0]
            : <int>[1, status, status == 5 ? 1 : 0],
      ),
    ),
  );
  void finish() {
    status = terminalState;
    revision++;
    state();
    final action = actions.last;
    terminalThrough = action.sequence;
    if (rejection != 0) {
      _control(
        ConnectionSessionControl.outcome,
        action.token,
        action.sequence,
        Uint8List.fromList(<int>[1, rejection, 0]),
      );
    }
    final checkpoint = Uint8List(10);
    ByteData.sublistView(
      checkpoint,
    ).setUint32(0, action.sequence, Endian.little);
    checkpoint[8] = rejection != 0 ? 1 : 0;
    _control(
      ConnectionSessionControl.checkpoint,
      action.token,
      action.sequence,
      checkpoint,
    );
  }

  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    if (characteristicId !=
        BleProtocol.connectionSessionIngressCharacteristic) {
      return;
    }
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(value))!;
    if (packet.kind == ConnectionSessionKind.ordered) {
      actions.add(packet);
      nextAction?.complete();
      nextAction = null;
      if (malformedStatusOnAction) malformedStatus = true;
      status = 4;
      revision++;
      state();
      grant(received: packet.sequence);
      if (!holdTerminal) finish();
      return;
    }
    if (packet.kind != ConnectionSessionKind.control) return;
    switch (packet.code) {
      case ConnectionSessionControl.hello:
        _control(ConnectionSessionControl.welcome, 1, 0, packet.body);
        final identity = Uint8List(10);
        final data = ByteData.sublistView(identity);
        data.setUint64(0, 1, Endian.little);
        data.setUint16(
          8,
          ConnectionSession.orderedFeature |
              ConnectionSession.stateFeature |
              ConnectionSession.objectFeature |
              ConnectionSession.reverseObjectFeature,
          Endian.little,
        );
        _control(ConnectionSessionControl.identity, 1, 0, identity);
      case ConnectionSessionControl.ready:
        _control(ConnectionSessionControl.activated, 1, 0, packet.body);
        for (final sequence in <int>[1, 2]) {
          final body = Uint8List(8);
          final data = ByteData.sublistView(body);
          data.setUint16(
            0,
            sequence == 1
                ? ConnectionSession.litchcraftStateTopic
                : ConnectionSession.litchcraftReverseObjectTopic,
            Endian.little,
          );
          body[2] = 1;
          body[3] = sequence == 1
              ? ConnectionSession.stateCapacity
              : ConnectionSession.reverseObjectCapacity;
          data.setUint16(4, 10, Endian.little);
          _control(ConnectionSessionControl.open, 1, sequence, body);
        }
      case ConnectionSessionControl.open:
        final body = Uint8List(10);
        final data = ByteData.sublistView(body);
        data.setUint32(0, packet.sequence == 1 ? 2 : 3, Endian.little);
        data.setUint16(
          4,
          ByteData.sublistView(packet.body).getUint16(4, Endian.little),
          Endian.little,
        );
        body[6] = packet.body[3];
        body[7] = packet.body[2];
        _control(ConnectionSessionControl.opened, 1, packet.sequence, body);
      case ConnectionSessionControl.confirm:
        if (packet.token == 2) grant(terminal: packet.sequence);
      case ConnectionSessionControl.query:
        if (packet.token == 2) {
          grant(received: actions.isEmpty ? 0 : actions.last.sequence);
        }
        if (packet.token == 0x80000001 &&
            (packet.sequence == 0 || packet.sequence == 38)) {
          queries++;
          if (!holdStatusReply) state();
        }
    }
  }
}

/// Settings data unrelated to recovery is fixed. Submitted production futures
/// are recorded and IO runs in the fixture's real async zone, while Flutter
/// animations use the widget clock. Recovery, the stream, session owner, packet
/// codec and Ordered receipts run unchanged.
class _SettingsService extends LitchBleService {
  _SettingsService(_BoardPeer peer)
    : ioZone = Zone.current,
      super(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _Ble(),
        gattSessionFactory: peer,
      );

  final Zone ioZone;
  final submissions = <Future<BoardStorageRecoverySnapshot>>[];
  final recoveryReads = <Future<BoardStorageRecoverySnapshot>>[];
  @override
  Future<BoardStorageRecoverySnapshot> fetchBoardStorageRecovery({
    required String deviceId,
  }) {
    final read = ioZone.run(
      () => super.fetchBoardStorageRecovery(deviceId: deviceId),
    );
    recoveryReads.add(read);
    return read;
  }

  @override
  Future<BoardStorageRecoverySnapshot> reformatBoardStorage({
    required String deviceId,
  }) {
    final result = ioZone.run(
      () => super.reformatBoardStorage(deviceId: deviceId),
    );
    submissions.add(result);
    return result;
  }

  @override
  BoardHealthStatus get latestBoardHealth => BoardHealthStatus(
    runtimeHealth: RuntimeHealthSnapshot.fromToken('000000000')!,
    persistence: HomePersistenceDegradation.storageIo,
    sessionOnly: true,
    resetReason: 0,
    bootCount: 1,
    watchdogRestarts: 0,
  );
  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async => RuntimeHealthAvailable(latestBoardHealth.runtimeHealth);
  @override
  Future<List<LitchSetting>> readSettings(String deviceId) async =>
      <LitchSetting>[];
  @override
  Future<LitchEndPalette?> readPhysicalEndPalette({
    required String deviceId,
  }) async => null;
}

Future<({LitchBleService service, _BoardPeer peer})> _connected({
  bool withSettings = false,
}) async {
  final peer = _BoardPeer();
  final LitchBleService service = withSettings
      ? _SettingsService(peer)
      : LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: _Ble(),
          gattSessionFactory: peer,
        );
  service.debugSetConnectedDevice('board-1');
  service.debugSetDeviceCapabilities(
    'board-1',
    const LitchDeviceCapabilities(
      contractVersion: 3,
      hardwareRevision: LitchHardwareRevision.v3,
      capabilityMask: 0,
      explicitlyReported: true,
    ),
  );
  await service.openConnectionSession('board-1');
  peer.grant();
  addTearDown(() async {
    await service.dispose();
    await peer.packets.close();
  });
  return (service: service, peer: peer);
}

Widget _settingsRecoveryApp(LitchBleService service) => MaterialApp(
  locale: const Locale('en'),
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: Scaffold(
    body: SettingsScreen(
      bleService: service,
      boardId: 'board-1',
      enableAppConsole: false,
      onEnableAppConsoleChanged: (_) async {},
    ),
  ),
);

final _storageOpen = find.byKey(
  const ValueKey<String>('storage-reformat-open'),
);
final _storageConfirm = find.byKey(
  const ValueKey<String>('storage-reformat-confirm'),
);
final _storageStatus = find.byKey(
  const ValueKey<String>('storage-reformat-status'),
);

/// Mounting Settings starts readonly recovery IO in the captured real zone.
/// Pumping widget animations alone does not settle its typed State reply.
Future<void> _settleInitialStorageRead(
  WidgetTester tester,
  _SettingsService service,
) async {
  expect(service.recoveryReads, hasLength(1));
  final snapshot = await tester.runAsync(
    () => service.recoveryReads.single.timeout(const Duration(seconds: 3)),
  );
  expect(snapshot?.status, BoardStorageRecoveryStatus.idle);
  expect(service.submissions, isEmpty);
  await tester.pumpAndSettle();
}

Future<void> _openStorageDialog(WidgetTester tester) async {
  await tester.ensureVisible(_storageOpen);
  await tester.pumpAndSettle();
  await tester.tap(_storageOpen);
  await tester.pumpAndSettle();
}

Future<BoardStorageRecoverySnapshot?> _confirmStorageDialog(
  WidgetTester tester,
  _SettingsService service,
) async {
  await tester.tap(_storageConfirm);
  await tester.pump(const Duration(milliseconds: 350));
  final result = await tester.runAsync(
    () => service.submissions.last.timeout(const Duration(seconds: 3)),
  );
  await tester.pumpAndSettle();
  return result;
}

void main() {
  test('strict recovery State requires exact terminal reboot proof', () {
    for (int status = 0; status <= 6; status++) {
      final snapshot = BoardStorageRecoverySnapshot.tryParse(
        Uint8List.fromList(<int>[1, status, status == 5 ? 1 : 0]),
      );
      expect(snapshot?.status, BoardStorageRecoveryStatus.values[status]);
      expect(snapshot?.rebootRequired, status == 5);
    }
    for (final malformed in <List<int>>[
      <int>[],
      <int>[2, 5, 1],
      <int>[1, 7, 0],
      <int>[1, 5, 0],
      <int>[1, 4, 1],
      <int>[1, 5, 1, 0],
    ]) {
      expect(
        BoardStorageRecoverySnapshot.tryParse(Uint8List.fromList(malformed)),
        isNull,
      );
    }
    expect(LitchControlProtocol.encodeConnectionStorageReformat(), <int>[
      1,
      5,
      0,
      0,
      1,
    ]);
  });

  test('legacy board cannot query or send destructive recovery', () async {
    final peer = _BoardPeer();
    final service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _Ble(),
      gattSessionFactory: peer,
    );
    addTearDown(() async {
      await service.dispose();
      await peer.packets.close();
    });
    service.debugSetConnectedDevice('board-1');
    expect(
      (await service.fetchBoardStorageRecovery(deviceId: 'board-1')).status,
      BoardStorageRecoveryStatus.unavailable,
    );
    expect(
      (await service.reformatBoardStorage(deviceId: 'board-1')).status,
      BoardStorageRecoveryStatus.unavailable,
    );
    expect(peer.queries, 0);
    expect(peer.actions, isEmpty);
  });

  test(
    'real typed adapter queries reliable State and formats only once',
    () async {
      final fixture = await _connected();
      final first = fixture.service.reformatBoardStorage(deviceId: 'board-1');
      final duplicate = fixture.service.reformatBoardStorage(
        deviceId: 'board-1',
      );
      expect(identical(first, duplicate), isTrue);
      final result = await first;
      expect(result.status, BoardStorageRecoveryStatus.succeeded);
      expect(result.rebootRequired, isTrue);
      expect(fixture.peer.actions, hasLength(1));
      expect(
        fixture.peer.actions.single.code,
        LitchControlProtocol.connectionActionOpcode,
      );
      expect(fixture.peer.actions.single.body, <int>[1, 5, 0, 0, 1]);
      expect(fixture.peer.queries, greaterThanOrEqualTo(3));
      await fixture.service.reformatBoardStorage(deviceId: 'board-1');
      expect(fixture.peer.actions, hasLength(1));
      expect(
        fixture.peer.subscribed.where(
          (uuid) => uuid == BleProtocol.connectionSessionEgressCharacteristic,
        ),
        hasLength(1),
      );
      expect(
        fixture.peer.subscribed,
        isNot(contains(BleProtocol.consoleStreamCharacteristic)),
      );
    },
  );

  test(
    'worker retains the same Ordered position until its terminal outcome',
    () async {
      final fixture = await _connected();
      fixture.peer.holdTerminal = true;
      var completed = false;
      final result = fixture.service
          .reformatBoardStorage(deviceId: 'board-1')
          .then((value) {
            completed = true;
            return value;
          });
      await Future<void>.delayed(Duration.zero);
      expect(fixture.peer.actions, hasLength(1));
      // Native admission cannot complete the worker's domain transaction.
      await Future<void>.delayed(const Duration(milliseconds: 100));
      expect(completed, isFalse);
      expect(fixture.peer.actions, hasLength(1));
      fixture.peer.finish();
      await Future<void>.delayed(Duration.zero);
      expect((await result).status, BoardStorageRecoveryStatus.succeeded);
    },
  );

  test(
    'Applied receipt without verified successful State stays unknown',
    () async {
      final fixture = await _connected();
      fixture.peer.terminalState = 4;
      final result = await fixture.service.reformatBoardStorage(
        deviceId: 'board-1',
      );
      expect(result.status, BoardStorageRecoveryStatus.unknown);
      await fixture.service.reformatBoardStorage(deviceId: 'board-1');
      expect(
        fixture.peer.actions,
        hasLength(1),
        reason: 'Uncertain completion cannot replay a destructive action.',
      );
    },
  );

  test(
    'known busy refusal sends nothing and allows a later explicit request',
    () async {
      final fixture = await _connected();
      fixture.peer.status = 2;
      fixture.peer.revision++;
      expect(
        (await fixture.service.reformatBoardStorage(
          deviceId: 'board-1',
        )).status,
        BoardStorageRecoveryStatus.busy,
      );
      expect(fixture.peer.actions, isEmpty);
      fixture.peer.status = 1;
      fixture.peer.revision++;
      expect(
        (await fixture.service.reformatBoardStorage(
          deviceId: 'board-1',
        )).status,
        BoardStorageRecoveryStatus.succeeded,
      );
      expect(fixture.peer.actions, hasLength(1));
    },
  );

  test('worker failure is surfaced from reliable terminal State', () async {
    final fixture = await _connected();
    fixture.peer.terminalState = 6;
    fixture.peer.rejection = 14;
    final result = await fixture.service.reformatBoardStorage(
      deviceId: 'board-1',
    );
    expect(result.status, BoardStorageRecoveryStatus.failed);
    expect(result.rebootRequired, isFalse);
  });

  test(
    'verified failure permits a fresh confirmed retry with one second Ordered action',
    () async {
      final fixture = await _connected();
      fixture.peer.terminalState = 6;
      fixture.peer.rejection = 14;
      final first = fixture.service.reformatBoardStorage(deviceId: 'board-1');
      expect((await first).status, BoardStorageRecoveryStatus.failed);
      expect(fixture.peer.actions, hasLength(1));
      await Future<void>.delayed(Duration.zero);
      expect(
        fixture.peer.actions,
        hasLength(1),
        reason: 'Failure cannot automatically replay the destructive action.',
      );
      fixture.peer.holdTerminal = true;
      fixture.peer.terminalState = 5;
      fixture.peer.rejection = 0;
      final retry = fixture.service.reformatBoardStorage(deviceId: 'board-1');
      final duplicate = fixture.service.reformatBoardStorage(
        deviceId: 'board-1',
      );
      expect(identical(retry, duplicate), isTrue);
      expect(identical(first, retry), isFalse);
      await Future<void>.delayed(Duration.zero);
      expect(fixture.peer.actions, hasLength(2));
      expect(fixture.peer.actions.last.sequence, 2);
      expect(fixture.peer.actions.last.body, <int>[1, 5, 0, 0, 1]);
      // A terminal State can arrive before the reliable Ordered waiter completes.
      // It does not release an in-flight request or create a third submission.
      fixture.peer.status = 6;
      fixture.peer.revision++;
      fixture.peer.state();
      expect(
        identical(
          retry,
          fixture.service.reformatBoardStorage(deviceId: 'board-1'),
        ),
        isTrue,
      );
      expect(fixture.peer.actions, hasLength(2));
      fixture.peer.finish();
      expect((await retry).status, BoardStorageRecoveryStatus.succeeded);
      await fixture.service.reformatBoardStorage(deviceId: 'board-1');
      expect(
        fixture.peer.actions,
        hasLength(2),
        reason: 'Successful completion retains its original Future.',
      );
    },
  );

  test(
    'uncertain receipt requires reliable Failed proof before a fresh retry',
    () async {
      final fixture = await _connected();
      fixture.peer.terminalState = 4;
      final unknown = fixture.service.reformatBoardStorage(deviceId: 'board-1');
      expect((await unknown).status, BoardStorageRecoveryStatus.unknown);
      expect(
        identical(
          unknown,
          fixture.service.reformatBoardStorage(deviceId: 'board-1'),
        ),
        isTrue,
      );
      expect(fixture.peer.actions, hasLength(1));
      fixture.peer.status = 6;
      fixture.peer.revision++;
      fixture.peer.state();
      await Future<void>.delayed(Duration.zero);
      expect(
        fixture.peer.actions,
        hasLength(1),
        reason: 'Reliable failure status never submits a retry by itself.',
      );
      fixture.peer.terminalState = 5;
      final retry = await fixture.service.reformatBoardStorage(
        deviceId: 'board-1',
      );
      expect(retry.status, BoardStorageRecoveryStatus.succeeded);
      expect(fixture.peer.actions, hasLength(2));
    },
  );

  testWidgets(
    'Settings retry passes fresh confirmation through the real typed adapter to a second Ordered action',
    (tester) async {
      final fixture = (await tester.runAsync(
        () => _connected(withSettings: true),
      ))!;
      final service = fixture.service as _SettingsService;
      fixture.peer.terminalState = 6;
      fixture.peer.rejection = 14;
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('en'),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: SettingsScreen(
              bleService: fixture.service,
              boardId: 'board-1',
              enableAppConsole: false,
              onEnableAppConsoleChanged: (_) async {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _settleInitialStorageRead(tester, service);
      final open = find.byKey(const ValueKey<String>('storage-reformat-open'));
      final confirm = find.byKey(
        const ValueKey<String>('storage-reformat-confirm'),
      );
      final cancel = find.byKey(
        const ValueKey<String>('storage-reformat-cancel'),
      );
      final dialog = find.byKey(
        const ValueKey<String>('storage-reformat-confirmation'),
      );
      final scrollable = find
          .ancestor(of: open, matching: find.byType(Scrollable))
          .first;
      await tester.scrollUntilVisible(open, 200, scrollable: scrollable);
      await tester.pumpAndSettle();
      await tester.tap(open);
      await tester.pumpAndSettle();
      expect(fixture.peer.actions, isEmpty);
      await tester.tap(confirm);
      await tester.pump(const Duration(milliseconds: 350));
      expect(service.submissions, hasLength(1));
      final failed = await tester.runAsync(
        () => service.submissions.single.timeout(const Duration(seconds: 3)),
      );
      expect(failed?.status, BoardStorageRecoveryStatus.failed);
      await tester.pumpAndSettle();
      expect(fixture.peer.actions, hasLength(1));
      expect(find.textContaining('You can keep playing'), findsOneWidget);
      expect(tester.widget<OutlinedButton>(open).onPressed, isNotNull);
      await tester.ensureVisible(open);
      await tester.tap(open);
      await tester.pumpAndSettle();
      expect(dialog, findsOneWidget);
      expect(find.textContaining('all files on the board'), findsOneWidget);
      expect(find.textContaining('saved games and artwork'), findsOneWidget);
      expect(
        find.textContaining('saved on this device are unaffected'),
        findsOneWidget,
      );
      expect(fixture.peer.actions, hasLength(1));
      await tester.tap(cancel);
      await tester.pumpAndSettle();
      expect(fixture.peer.actions, hasLength(1));
      await tester.tap(open);
      await tester.pumpAndSettle();
      fixture.peer.holdTerminal = true;
      fixture.peer.rejection = 0;
      fixture.peer.terminalState = 4;
      final retrySubmitted = service.ioZone.run(Completer<void>.new);
      fixture.peer.nextAction = retrySubmitted;
      await tester.tap(confirm);
      await tester.pump(const Duration(milliseconds: 350));
      await tester.runAsync(
        () => retrySubmitted.future.timeout(const Duration(seconds: 3)),
      );
      await tester.pump();
      expect(fixture.peer.actions, hasLength(2));
      expect(fixture.peer.actions.last.sequence, 2);
      expect(fixture.peer.actions.last.body, <int>[1, 5, 0, 0, 1]);
      expect(open, findsNothing);
      expect(find.textContaining('Reformatting board storage'), findsOneWidget);
      await tester.pump(const Duration(seconds: 1));
      expect(fixture.peer.actions, hasLength(2));
      fixture.peer.finish();
      expect(service.submissions, hasLength(2));
      final unknown = await tester.runAsync(
        () => service.submissions.last.timeout(const Duration(seconds: 3)),
      );
      expect(unknown?.status, BoardStorageRecoveryStatus.unknown);
      await tester.pumpAndSettle();
      expect(open, findsNothing);
      expect(
        find.textContaining('result could not be confirmed'),
        findsOneWidget,
      );
      expect(fixture.peer.actions, hasLength(2));
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  for (final reason in <LitchMutationReason>[
    LitchMutationReason.busy,
    LitchMutationReason.queueFull,
    LitchMutationReason.unsupported,
    LitchMutationReason.unknownAction,
  ]) {
    test(
      'terminally rejected ${reason.name} permits a fresh explicit request',
      () async {
        final fixture = await _connected();
        fixture.peer.rejection = reason.wireCode;
        fixture.peer.terminalState =
            reason == LitchMutationReason.busy ||
                reason == LitchMutationReason.queueFull
            ? 2
            : 0;
        final first = fixture.service.reformatBoardStorage(deviceId: 'board-1');
        final refused = await first;
        expect(
          refused.status,
          fixture.peer.terminalState == 2
              ? BoardStorageRecoveryStatus.busy
              : BoardStorageRecoveryStatus.unavailable,
        );
        expect(fixture.peer.actions, hasLength(1));
        fixture.peer.status = 1;
        fixture.peer.revision++;
        fixture.peer.rejection = 0;
        fixture.peer.terminalState = 5;
        await Future<void>.delayed(Duration.zero);
        expect(
          fixture.peer.actions,
          hasLength(1),
          reason: 'A refusal cannot retry automatically.',
        );
        final retry = fixture.service.reformatBoardStorage(deviceId: 'board-1');
        expect(identical(first, retry), isFalse);
        expect(
          identical(
            retry,
            fixture.service.reformatBoardStorage(deviceId: 'board-1'),
          ),
          isTrue,
        );
        expect((await retry).status, BoardStorageRecoveryStatus.succeeded);
        expect(fixture.peer.actions, hasLength(2));
        expect(fixture.peer.actions.last.body, <int>[1, 5, 0, 0, 1]);
      },
    );
  }

  for (final status in <int>[2, 3, 4]) {
    test(
      'rejection without retry-safe refusal retains status $status submission',
      () async {
        final fixture = await _connected();
        fixture.peer.rejection = 14;
        fixture.peer.terminalState = status;
        final first = fixture.service.reformatBoardStorage(deviceId: 'board-1');
        expect((await first).status, BoardStorageRecoveryStatus.values[status]);
        fixture.peer.status = 1;
        fixture.peer.revision++;
        expect(
          identical(
            first,
            fixture.service.reformatBoardStorage(deviceId: 'board-1'),
          ),
          isTrue,
        );
        expect(
          fixture.peer.actions,
          hasLength(1),
          reason: 'A status alone does not prove a retry-safe refusal.',
        );
      },
    );
  }

  test(
    'new reliable Failed truth resolves an otherwise uncertain applied receipt',
    () async {
      final fixture = await _connected();
      fixture.peer.terminalState = 6;
      expect(
        (await fixture.service.reformatBoardStorage(
          deviceId: 'board-1',
        )).status,
        BoardStorageRecoveryStatus.failed,
      );
      expect(fixture.peer.actions, hasLength(1));
    },
  );

  testWidgets(
    'old Failed plus malformed new State stays Unknown in Settings and cannot replay',
    (tester) async {
      final fixture = (await tester.runAsync(
        () => _connected(withSettings: true),
      ))!;
      final service = fixture.service as _SettingsService;
      fixture.peer.terminalState = 6;
      fixture.peer.rejection = 14;
      await tester.pumpWidget(_settingsRecoveryApp(service));
      await tester.pumpAndSettle();
      await _settleInitialStorageRead(tester, service);
      await _openStorageDialog(tester);
      expect(
        (await _confirmStorageDialog(tester, service))?.status,
        BoardStorageRecoveryStatus.failed,
      );
      final oldFailureRevision = fixture.peer.revision;
      fixture.peer.terminalState = 5;
      fixture.peer.rejection = 0;
      fixture.peer.malformedStatusOnAction = true;
      await _openStorageDialog(tester);
      expect(
        (await _confirmStorageDialog(tester, service))?.status,
        BoardStorageRecoveryStatus.unknown,
      );
      expect(
        find.textContaining('result could not be confirmed'),
        findsOneWidget,
      );
      expect(
        find.textContaining('Storage could not be reformatted'),
        findsNothing,
      );
      expect(_storageOpen, findsNothing);
      expect(_storageStatus, findsOneWidget);
      expect(fixture.peer.actions, hasLength(2));
      fixture.peer.malformedStatus = false;
      fixture.peer.status = 6;
      fixture.peer.revision = oldFailureRevision;
      await tester.ensureVisible(_storageStatus);
      await tester.tap(_storageStatus);
      await tester.pump(const Duration(milliseconds: 350));
      await tester.runAsync(
        () => service.fetchBoardStorageRecovery(deviceId: 'board-1'),
      );
      await tester.pumpAndSettle();
      expect(
        find.textContaining('result could not be confirmed'),
        findsOneWidget,
      );
      expect(_storageOpen, findsNothing);
      expect(_storageStatus, findsOneWidget);
      final uncertain = service.submissions.last;
      expect(
        identical(uncertain, service.reformatBoardStorage(deviceId: 'board-1')),
        isTrue,
      );
      expect(fixture.peer.actions, hasLength(2));
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  testWidgets(
    'rejected Busy refreshes to Idle then needs fresh confirmation for a second Ordered action',
    (tester) async {
      final fixture = (await tester.runAsync(
        () => _connected(withSettings: true),
      ))!;
      final service = fixture.service as _SettingsService;
      fixture.peer.terminalState = 2;
      fixture.peer.rejection = LitchMutationReason.busy.wireCode;
      await tester.pumpWidget(_settingsRecoveryApp(service));
      await tester.pumpAndSettle();
      await _settleInitialStorageRead(tester, service);
      await _openStorageDialog(tester);
      expect(
        (await _confirmStorageDialog(tester, service))?.status,
        BoardStorageRecoveryStatus.busy,
      );
      expect(tester.widget<OutlinedButton>(_storageOpen).onPressed, isNull);
      expect(_storageStatus, findsOneWidget);
      expect(fixture.peer.actions, hasLength(1));
      fixture.peer.status = 1;
      fixture.peer.revision++;
      await tester.ensureVisible(_storageStatus);
      await tester.tap(_storageStatus);
      await tester.pump(const Duration(milliseconds: 350));
      await tester.runAsync(
        () => service.fetchBoardStorageRecovery(deviceId: 'board-1'),
      );
      await tester.pumpAndSettle();
      expect(tester.widget<OutlinedButton>(_storageOpen).onPressed, isNotNull);
      await _openStorageDialog(tester);
      expect(find.textContaining('all files on the board'), findsOneWidget);
      expect(find.textContaining('saved games and artwork'), findsOneWidget);
      expect(fixture.peer.actions, hasLength(1));
      await tester.tap(
        find.byKey(const ValueKey<String>('storage-reformat-cancel')),
      );
      await tester.pumpAndSettle();
      expect(fixture.peer.actions, hasLength(1));
      await _openStorageDialog(tester);
      fixture.peer.terminalState = 5;
      fixture.peer.rejection = 0;
      expect(
        (await _confirmStorageDialog(tester, service))?.status,
        BoardStorageRecoveryStatus.succeeded,
      );
      expect(fixture.peer.actions, hasLength(2));
      expect(fixture.peer.actions.last.body, <int>[1, 5, 0, 0, 1]);
      expect(find.textContaining('Restart the board'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  test('old Failed preflight cannot release a new unknown retry', () async {
    final fixture = await _connected();
    fixture.peer.terminalState = 6;
    fixture.peer.rejection = 14;
    expect(
      (await fixture.service.reformatBoardStorage(deviceId: 'board-1')).status,
      BoardStorageRecoveryStatus.failed,
    );
    final priorFailureRevision = fixture.peer.revision;
    fixture.peer.terminalState = 5;
    fixture.peer.rejection = 0;
    fixture.peer.malformedStatusOnAction = true;
    final retry = fixture.service.reformatBoardStorage(deviceId: 'board-1');
    expect((await retry).status, BoardStorageRecoveryStatus.unknown);
    expect(fixture.peer.actions, hasLength(2));
    // The reverse State owner still holds the previous attempt's terminal
    // failure when every new status packet is lost or malformed.
    fixture.peer.malformedStatus = false;
    fixture.peer.status = 6;
    fixture.peer.revision = priorFailureRevision;
    expect(
      (await fixture.service.fetchBoardStorageRecovery(
        deviceId: 'board-1',
      )).status,
      BoardStorageRecoveryStatus.unknown,
    );
    expect(
      fixture.service.latestBoardStorageRecovery?.status,
      BoardStorageRecoveryStatus.failed,
    );
    expect(
      identical(
        retry,
        fixture.service.reformatBoardStorage(deviceId: 'board-1'),
      ),
      isTrue,
    );
    expect(fixture.peer.actions, hasLength(2));
  });

  test('malformed State never proves support or successful format', () async {
    final fixture = await _connected();
    fixture.peer.malformedStatus = true;
    expect(
      (await fixture.service.fetchBoardStorageRecovery(
        deviceId: 'board-1',
      )).status,
      BoardStorageRecoveryStatus.unknown,
    );
    expect(
      (await fixture.service.reformatBoardStorage(deviceId: 'board-1')).status,
      BoardStorageRecoveryStatus.unknown,
    );
    expect(fixture.peer.actions, isEmpty);
  });

  test(
    'disconnect during readonly preflight never submits storage erase',
    () async {
      final fixture = await _connected();
      fixture.peer.holdStatusReply = true;
      final result = fixture.service.reformatBoardStorage(deviceId: 'board-1');
      await Future<void>.delayed(Duration.zero);
      expect(fixture.peer.actions, isEmpty);
      await fixture.service.disconnect();
      expect((await result).status, BoardStorageRecoveryStatus.unknown);
      expect(fixture.peer.actions, isEmpty);
    },
  );

  test('generation change fences submitted format terminal result', () async {
    final fixture = await _connected();
    fixture.peer.holdTerminal = true;
    final result = fixture.service.reformatBoardStorage(deviceId: 'board-1');
    await Future<void>.delayed(Duration.zero);
    expect(fixture.peer.actions, hasLength(1));
    await fixture.service.disconnect();
    fixture.peer.finish();
    expect((await result).status, BoardStorageRecoveryStatus.unknown);
    expect(fixture.peer.actions, hasLength(1));
  });
}
