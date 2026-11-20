import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/support_session_board.dart';

// Only owner-proof status is controlled for these UI routes. The actual
// report collector and shared-session/native packet adapter remain in use;
// firmware native tests independently prove the auth/revocation guards.
class _UnreadySupportService extends LitchBleService {
  _UnreadySupportService(SupportSessionBoard board, this.pendingProof)
    : super(ble: board, pendingOtaStore: MemoryOtaTargetStore());
  final Completer<bool>? pendingProof;
  int readinessRequests = 0;
  @override
  bool get productReady => false;
  @override
  bool get ownerPairingBusy => pendingProof?.isCompleted == false;
  @override
  OwnerPairingSessionStatus get ownerPairingStatus => OwnerPairingSessionStatus(
    state: ownerPairingBusy
        ? OwnerPairingSessionState.ownedOther
        : OwnerPairingSessionState.failed,
    reason: ownerPairingBusy
        ? OwnerPairingSessionReason.none
        : OwnerPairingSessionReason.storageFailed,
    flags: 5,
  );
  @override
  Future<bool> ensureProductReady(String deviceId, {bool retry = false}) {
    readinessRequests++;
    return pendingProof?.future ?? Future.value(false);
  }

  @override
  Future<void> startScan() async {}
}

void main() {
  setUp(() {
    SharedPreferences.setMockInitialValues({});
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'test.litch',
      version: '0.9.5',
      buildNumber: '1',
      buildSignature: 'test',
    );
  });
  for (final pending in [true, false]) {
    testWidgets(
      'Support public access from ${pending ? 'quiet pending' : 'failed'} owner proof keeps product locked',
      (tester) async {
        final board = SupportSessionBoard();
        board.capabilityMask |= LitchDeviceCapabilities.ownerPairingBit;
        final pendingProof = pending ? Completer<bool>() : null;
        final service = _UnreadySupportService(board, pendingProof);
        final trace = AppTraceRecorder.disabled();
        await tester.runAsync(() async {
          await connectBoard(service, board);
          final capabilities = await service.fetchDeviceCapabilities(
            deviceId: 'board-1',
          );
          expect(capabilities.supportsOwnerPairing, isTrue);
        });
        bool sessionActive = false;
        service.openConnectionSession('board-1').then((session) {
          sessionActive = session?.isActivated == true;
        });
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              appTrace: trace,
            ),
          ),
        );
        await tester.pump();
        // Startup can span native callbacks and the widget's microtask queue.
        // Its readiness request starts the proof that remains held across Support.
        for (
          var i = 0;
          i < 120 && (!sessionActive || service.readinessRequests == 0);
          i++
        ) {
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          await tester.pump();
        }
        expect(sessionActive, isTrue);
        expect(service.readinessRequests, greaterThan(0));
        final support = find.byKey(const ValueKey('owner-pairing-support'));
        if (pending) {
          expect(support, findsNothing);
          await tester.pump(const Duration(seconds: 3));
        }
        expect(support, findsOneWidget);
        expect(support.hitTestable(), findsOneWidget);
        expect(tester.widget<TextButton>(support).onPressed, isNotNull);
        expect(tester.getSize(support).height, greaterThanOrEqualTo(48));
        final before = service.readinessRequests;
        final pairingBefore = board.pairingCommands.length;
        final readsBefore = board.protectedReads.length;
        final historyBefore = board.historyRequests;
        await tester.tap(support);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        await tester.pump(const Duration(seconds: 3));
        final list = find.byKey(const ValueKey('support-report-list'));
        for (var i = 0; i < 120 && list.evaluate().isEmpty; i++) {
          await tester.pump();
          await tester.runAsync(() async {});
        }
        expect(
          list,
          findsOneWidget,
          reason:
              'public requests=${board.requests}; visible=${tester.widgetList<Text>(find.byType(Text)).map((t) => t.data).join('|')}; exceptions=${tester.takeException()}',
        );
        expect(
          find.textContaining('up to 20 recent app and board actions'),
          findsOneWidget,
        );
        final body = find.byKey(const ValueKey('support-report-body'));
        await tester.scrollUntilVisible(
          body,
          300,
          scrollable: find
              .descendant(of: list, matching: find.byType(Scrollable))
              .first,
        );
        expect(body, findsOneWidget);
        final text = tester.widget<TextField>(body).controller!.text;
        expect(text, contains('firmware_uptime_ms64: ${board.epochUptime}'));
        expect(text, contains('firmware_settings_saved_total: unavailable'));
        expect(text, contains('Board actions: unavailable'));
        expect(board.requests, isNotEmpty);
        expect(board.requests.every((r) => r.$1 == 0x45), isTrue);
        expect(
          service.readinessRequests,
          before,
          reason: 'Opening support must not retry or await owner proof',
        );
        expect(service.productReady, isFalse);
        final reportRoute = ModalRoute.of(tester.element(list))!;
        await tester.pageBack();
        for (var frame = 0; frame < 45 && list.evaluate().isNotEmpty; ++frame) {
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          await tester.pump(const Duration(milliseconds: 16));
        }
        expect(
          reportRoute.isCurrent,
          isFalse,
          reason: 'Back must dismiss the public report.',
        );
        expect(list, findsNothing);
        expect(support.hitTestable(), findsOneWidget);
        expect(service.readinessRequests, before);
        expect(board.pairingCommands.length, pairingBefore);
        expect(board.protectedReads.length, readsBefore);
        expect(board.historyRequests, historyBefore);
        expect(service.ownerPairingBusy, pending);
        await tester.pumpWidget(const SizedBox.shrink());
        if (pendingProof != null) {
          expect(pendingProof.isCompleted, isFalse);
          pendingProof.complete(false);
        }
        await tester.runAsync(() => disposeBoard(service, board));
        await trace.close();
        expect(tester.takeException(), isNull);
      },
    );
  }
}
