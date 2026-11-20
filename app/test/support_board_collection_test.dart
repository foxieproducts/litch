import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/support_board_collection.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/services/support_report_service.dart';
import 'package:litch_mobile/src/models/support_snapshot.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/support_session_board.dart';

class _SupportService extends LitchBleService {
  _SupportService(SupportSessionBoard board)
    : super(ble: board, pendingOtaStore: MemoryOtaTargetStore());
  bool ownerReady = true;
  bool quiescing = false;
  @override
  OwnerPairingSessionStatus? get ownerPairingStatus => quiescing
      ? const OwnerPairingSessionStatus(
          state: OwnerPairingSessionState.clearing,
          reason: OwnerPairingSessionReason.none,
          flags: 0,
        )
      : super.ownerPairingStatus;
  @override
  bool get productReady => ownerReady && super.productReady;
}

Future<void> _flush(WidgetTester tester) async {
  for (var i = 0; i < 8; i++) {
    await tester.pump();
    await tester.runAsync(() async {});
  }
}

Future<_SupportService> _start(
  WidgetTester tester,
  SupportSessionBoard board,
) async {
  final service = _SupportService(board);
  await tester.runAsync(() => connectBoard(service, board));
  await _flush(tester);
  return service;
}

Future<void> _until(WidgetTester tester, bool Function() ready) async {
  for (var i = 0; i < 120 && !ready(); i++) {
    await tester.pump();
    await tester.runAsync(() async {});
  }
  expect(
    ready(),
    isTrue,
    reason:
        'Expected bounded shared-session progress without advancing its deadlines',
  );
}

Future<void> _cleanup(
  WidgetTester tester,
  _SupportService service,
  SupportSessionBoard board,
) async {
  if (board.heldWrite case final held?) {
    if (!held.isCompleted) held.complete();
  }
  await tester.runAsync(() => disposeBoard(service, board));
  await _flush(tester);
  expect(tester.takeException(), isNull);
}

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues({});
  });
  testWidgets(
    'SupportSnapshotSessionJourney: minimum MTU preserves public metrics and private20-action capsule',
    (tester) async {
      final board = SupportSessionBoard();
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      SupportBoardCollection? result;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((value) => result = value),
      );
      await _until(tester, () => result != null);
      expect(result, isNotNull);
      expect(result!.publicHealth.value?.uptimeMs, greaterThan(0xffffffff));
      expect(result!.firmwareDigest.value, 'a' * 64);
      expect(result!.actionHistory.value?.actions, hasLength(20));
      expect(result!.actionHistory.value?.saveTotal, 25);
      expect(result!.incident.value?.fields['saved'], 7);
      expect(board.requests.map((r) => (r.$1, r.$2)), [
        (0x45, 0),
        (0x44, 0),
        (0x44, 1),
      ]);
      expect(board.firstOversizeNativeWrite, isNull);
    },
  );
  testWidgets(
    'pending or failed owner proof reads public health without private action requests or pairing mutations',
    (tester) async {
      final board = SupportSessionBoard();
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      service.ownerReady = false;
      final pairingBefore = board.pairingCommands.length;
      SupportBoardCollection? result;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((value) => result = value),
      );
      await _flush(tester);
      expect(result!.publicHealth.outcome, SupportQueryOutcome.available);
      expect(result!.actionHistory.outcome, SupportQueryOutcome.ownerNotReady);
      expect(result!.incident.outcome, SupportQueryOutcome.ownerNotReady);
      expect(board.requests.map((r) => r.$1), [0x45]);
      expect(board.pairingCommands.length, pairingBefore);
    },
  );
  testWidgets(
    'one10-second collection deadline includes stalled native write and retains single-flight until settlement',
    (tester) async {
      final board = SupportSessionBoard();
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      final held = board.heldWrite = Completer<void>();
      SupportBoardCollection? result;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((value) => result = value),
      );
      await _flush(tester);
      expect(board.nativeSupportWrites, hasLength(1));
      await tester.pump(const Duration(milliseconds: 9999));
      await _flush(tester);
      expect(result, isNull);
      await tester.pump(const Duration(milliseconds: 1));
      await _flush(tester);
      expect(result!.publicHealth.outcome, SupportQueryOutcome.timedOut);
      expect(
        result!.actionHistory.outcome,
        SupportQueryOutcome.notAttemptedDeadline,
      );
      final busy = await service.collectSupportBoardDiagnostics(
        deviceId: 'board-1',
      );
      expect(busy.publicHealth.outcome, SupportQueryOutcome.busy);
      board.heldWrite = null;
      held.completeError(StateError('late controlled native failure'));
      await _flush(tester);
      expect(board.requests.where((r) => r.$1 == 0x44), isEmpty);
    },
  );
  testWidgets(
    'cancel support before native settlement prevents follow-up requests and leaves shared session live',
    (tester) async {
      final board = SupportSessionBoard();
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      final cancel = Completer<void>(),
          held = board.heldWrite = Completer<void>();
      SupportBoardCollection? result;
      unawaited(
        service
            .collectSupportBoardDiagnostics(
              deviceId: 'board-1',
              cancelled: cancel.future,
            )
            .then((value) => result = value),
      );
      await _flush(tester);
      cancel.complete();
      await _flush(tester);
      expect(result!.publicHealth.outcome, SupportQueryOutcome.canceled);
      board.heldWrite = null;
      held.complete();
      await _flush(tester);
      expect(board.requests.where((r) => r.$1 == 0x44), isEmpty);
      expect(
        (await service.openConnectionSession('board-1'))?.isActivated,
        isTrue,
      );
    },
  );
  testWidgets(
    'retired shared session on a live BLE link yields partial support without reconnect or consent',
    (tester) async {
      final board = SupportSessionBoard()..recoveryOnly = true;
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      await tester.runAsync(() async {
        final session = await service.openConnectionSession('board-1');
        await session!.dispose();
      });
      expect(service.isConnected, isTrue);
      final connections = board.connections.length;
      final pairing = board.pairingCommands.length;
      SupportReportDraft? draft;
      unawaited(
        SupportReportService(
          bleService: service,
          boardId: 'board-1',
          loadAppInfo: () async => const SupportAppInfo(
            version: 'test',
            buildNumber: '1',
            platform: 'ios',
            osVersion: 'test',
          ),
        ).createDraft().then((v) => draft = v),
      );
      await _flush(tester);
      expect(
        board.connections.length,
        connections,
        reason: 'Contact Support must not reconnect a retired shared owner',
      );
      expect(board.pairingCommands.length, pairing);
      await tester.pump(const Duration(seconds: 3));
      await _until(tester, () => draft != null);
      expect(draft!.body, contains('public_support: unavailable'));
      expect(draft!.body, contains('incident_snapshot: unavailable'));
      expect(draft!.body, contains('Please add your answers'));
      expect(draft!.body.length, lessThanOrEqualTo(6000));
      expect(service.isConnected, isTrue);
      expect(board.connections.length, connections);
      expect(board.requests, isEmpty);
    },
  );

  testWidgets(
    'revoked owner proof discards private facts after public collection',
    (tester) async {
      final board = SupportSessionBoard()..silent.add((0x44, 0));
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      SupportBoardCollection? result;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((value) => result = value),
      );
      await _flush(tester);
      final held = board.staleReplies.last;
      service.ownerReady = false;
      board.emitSupport(held.$1, held.$2);
      await _until(tester, () => result != null);
      expect(result!.actionHistory.outcome, SupportQueryOutcome.ownerNotReady);
      expect(result!.incident.value, isNull);
    },
  );
  testWidgets(
    'owner revocation keeps a partial support draft and safe State2 usable until normal renewal',
    (tester) async {
      final board = SupportSessionBoard();
      final service = await _start(tester, board);
      try {
        // Receive real safe shared State2 before quiescence. No BoardContext State3
        // is supplied, so cached health cannot depend on private context.
        board.emitState(2, [1, 9, 0, 1, 1, 0]);
        await _flush(tester);
        service.ownerReady = false;
        service.quiescing = true;
        board.silent.add((
          0x45,
          0,
        )); // Model the retained private Object sequence hole.
        SupportReportDraft? draft;
        unawaited(
          SupportReportService(
            bleService: service,
            boardId: 'board-1',
            loadAppInfo: () async => const SupportAppInfo(
              version: 'test',
              buildNumber: '1',
              platform: 'ios',
              osVersion: 'test',
            ),
          ).createDraft().then((v) => draft = v),
        );
        await _until(tester, () => board.requests.isNotEmpty);
        await tester.pump(const Duration(seconds: 10));
        await _until(tester, () => draft != null);
        expect(draft!.body, contains('runtime_health_state: degradedUsable'));
        expect(
          draft!.body,
          contains('full_snapshot_unavailable_until_session_renewed'),
        );
        expect(draft!.body, contains('incident_snapshot: unavailable'));
        expect(draft!.body, contains('Board actions: unavailable'));
        expect(board.requests.every((r) => r.$1 == 0x45), isTrue);
        final before = board.requests.length;
        // Use the real existing disconnect/connect boundary, never collector-driven
        // reconnect or cursor skipping. Fresh session must regain public Objects.
        await tester.runAsync(service.disconnect);
        board.silent.clear();
        service.quiescing = false;
        await tester.runAsync(() => connectBoard(service, board));
        SupportBoardCollection? renewed;
        unawaited(
          service
              .collectSupportBoardDiagnostics(deviceId: 'board-1')
              .then((v) => renewed = v),
        );
        await _until(tester, () => renewed != null);
        expect(renewed!.publicHealth.outcome, SupportQueryOutcome.available);
        expect(board.requests.length, before + 1);
        expect(renewed!.incident.outcome, SupportQueryOutcome.ownerNotReady);
      } finally {
        await _cleanup(tester, service, board);
      }
    },
  );

  testWidgets(
    'late previous capture cannot satisfy a newer support request in the same session',
    (tester) async {
      final board = SupportSessionBoard()..silent.add((0x45, 0));
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      SupportBoardCollection? first, second;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((v) => first = v),
      );
      await _until(tester, () => board.requests.isNotEmpty);
      final old = board.staleReplies.first;
      await tester.pump(const Duration(seconds: 10));
      await _flush(tester);
      expect(first!.publicHealth.outcome, SupportQueryOutcome.timedOut);
      board.epochUptime += 5000;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((v) => second = v),
      );
      await _until(tester, () => board.requests.length == 2);
      board.emitSupport(old.$1, old.$2);
      await _flush(tester);
      expect(
        second,
        isNull,
        reason: 'Matching connection alone cannot authorize a stale capture',
      );
      final current = board.staleReplies.last;
      board.emitSupport(current.$1, current.$2);
      await _until(tester, () => second != null);
      expect(second!.publicHealth.value?.uptimeMs, board.epochUptime);
    },
  );
  testWidgets(
    'a changed connection generation discards already captured board facts',
    (tester) async {
      final board = SupportSessionBoard()..silent.add((0x44, 0));
      final service = await _start(tester, board);
      addTearDown(() => _cleanup(tester, service, board));
      SupportBoardCollection? result;
      unawaited(
        service
            .collectSupportBoardDiagnostics(deviceId: 'board-1')
            .then((v) => result = v),
      );
      await _until(tester, () => board.requests.length == 2);
      service.debugSetConnectedDevice('board-2');
      await tester.pump(const Duration(seconds: 10));
      await _flush(tester);
      expect(result!.publicHealth.outcome, SupportQueryOutcome.sessionChanged);
      expect(result!.firmwareDigest.value, isNull);
      expect(result!.actionHistory.value, isNull);
    },
  );

  test(
    'public default fields never fabricate healthy authority or image identity',
    () {
      final board = SupportSessionBoard()..publicValidity = 4;
      final bytes = board.publicBytes(3);
      bytes[20] = 0;
      final value = SupportPublicHealth.decode(bytes, 3);
      expect(value.runtimeHealth, isNull);
      expect(value.firmwareDigest, isNull);
      expect(() => SupportPublicHealth.decode(bytes, 4), throwsFormatException);
    },
  );
}
