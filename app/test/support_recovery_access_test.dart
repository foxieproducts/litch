import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/owner_pairing_session.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/widgets/runtime_health_card.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'firmware_screen_test.dart' show FakeFirmwareDownloadService;
import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/support_session_board.dart';

// Only the external board/SDK replies and release server are controlled. The
// production discovery, session, recovery routing, Setup health, updater, collector
// and report screen run unchanged. Recovery uses its shared transport directly.
class _RecoverySupportService extends LitchBleService {
  _RecoverySupportService(SupportSessionBoard board)
    : super(ble: board, pendingOtaStore: MemoryOtaTargetStore());

  @override
  Future<void> startScan() async {}
}

void _expectPublicRecoveryAccess(
  _RecoverySupportService service,
  SupportSessionBoard board,
) {
  const pairingStates = {
    OwnerPairingSessionStatus.stateCode,
    OwnerPairingSessionStatus.identityLowCode,
    OwnerPairingSessionStatus.identityHighCode,
  };
  expect(board.stateQueries.where(pairingStates.contains), isEmpty);
  expect(board.pairingCommands, isEmpty);
  expect(service.ownerPairingStatus, isNull);
  expect(service.durableBoardIdentity, isNull);
  expect(board.productQueries, isEmpty);
  expect(board.requests.every((request) => request.$1 == 0x45), isTrue);
}

Future<void> _until(WidgetTester tester, bool Function() ready) async {
  for (var i = 0; i < 150 && !ready(); i++) {
    // Let the production session's coalesced credit/notify deadlines run.
    await tester.pump(const Duration(milliseconds: 20));
    await tester.runAsync(() async {});
  }
  expect(ready(), isTrue);
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
  for (final available in [true, false]) {
    testWidgets(
      'RecoverySetupHealthSupportJourney available=$available opens Setup health with public facts and no owner or domain entry',
      (tester) async {
        final board = SupportSessionBoard()..recoveryOnly = true;
        if (!available) board.silent.add((0x45, 0));
        final service = _RecoverySupportService(board);
        final trace = AppTraceRecorder.disabled();
        await tester.binding.setSurfaceSize(const Size(390, 844));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        try {
          await tester.runAsync(() => connectBoard(service, board));
          await tester.runAsync(
            () => service.fetchDeviceCapabilities(deviceId: 'board-1'),
          );
          final generation = service.connectionSessionToken;
          expect(service.deviceCapabilities('board-1').recoveryOnly, isTrue);
          await tester.pumpWidget(
            MaterialApp(
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(available ? 1 : 2)),
                child: child!,
              ),
              home: ConnectionGateScreen(
                bleService: service,
                appTrace: trace,
                firmwarePageBuilder: (ble, id, show, activity, availability) =>
                    FirmwareScreen(
                      bleService: ble,
                      boardId: id,
                      onRequestShowFirmwareTab: show,
                      onUpdateActivityChanged: activity,
                      onAvailabilityChanged: availability,
                      downloadService: FakeFirmwareDownloadService(),
                    ),
              ),
            ),
          );
          await _until(
            tester,
            () =>
                find
                    .byKey(const ValueKey('runtime-health-card'))
                    .evaluate()
                    .isNotEmpty &&
                board.requests.isNotEmpty,
          );
          await tester.pump();
          if (!available) {
            await tester.pump(const Duration(seconds: 10));
            await _until(
              tester,
              () => find
                  .textContaining(
                    'Some board details are unavailable. You can still prepare a support report.',
                  )
                  .evaluate()
                  .isNotEmpty,
            );
          } else {
            await _until(
              tester,
              () => find
                  .textContaining(
                    'Normal startup stopped after repeated restarts.',
                  )
                  .evaluate()
                  .isNotEmpty,
            );
          }
          expect(find.byType(SettingsScreen).hitTestable(), findsOneWidget);
          expect(find.byType(RuntimeHealthCard), findsOneWidget);
          expect(
            find.byKey(const ValueKey('runtime-health-toggle')).hitTestable(),
            findsOneWidget,
          );
          expect(
            tester
                .getSize(find.byKey(const ValueKey('runtime-health-toggle')))
                .height,
            greaterThanOrEqualTo(48),
          );
          expect(
            tester
                .getSize(find.byKey(const ValueKey('recovery-disconnect')))
                .shortestSide,
            greaterThanOrEqualTo(48),
          );
          expect(find.text('Setup'), findsOneWidget);
          expect(find.text('Board health'), findsOneWidget);
          expect(find.text('Recovery'), findsOneWidget);
          expect(
            find.byKey(const ValueKey('runtime-health-details')),
            findsOneWidget,
          );
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(
            find.byType(LitchcraftScreen, skipOffstage: false),
            findsNothing,
          );
          expect(find.byType(NavigationBar), findsNothing);
          expect(find.byType(NavigationRail), findsNothing);
          expect(find.byType(TextField), findsNothing);
          for (final category in [
            'lights',
            'behavior',
            'phone',
            'advanced',
            'developer',
          ]) {
            expect(find.byKey(ValueKey('setup-open:$category')), findsNothing);
          }
          expect(find.text('Game behavior'), findsNothing);
          expect(find.text('Lights'), findsNothing);
          final healthCard = tester.widget<RuntimeHealthCard>(
            find.byType(RuntimeHealthCard),
          );
          debugPrint(
            '[RECOVERY_SETUP] public=${available ? 'available' : 'partial'} cause=${healthCard.recoveryHealth?.recoveryReason ?? 'unavailable'} text_scale=${available ? 1 : 2} next=software_or_support pairing_commands=${board.pairingCommands.length} native_connections=${board.connections.length}',
          );
          expect(service.isConnected, isTrue);
          expect(service.connectionSessionToken, generation);
          expect(board.connections, hasLength(1));
          _expectPublicRecoveryAccess(service, board);
          final software = find.byKey(const ValueKey('open-board-software'));
          await tester.ensureVisible(software);
          await tester.pump();
          await tester.tap(software);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          expect(find.byType(FirmwareScreen).hitTestable(), findsOneWidget);
          expect(find.text('Board update'), findsOneWidget);
          expect(
            find.byKey(const ValueKey('board-recovery-support')),
            findsNothing,
          );
          await tester.tap(find.byKey(const ValueKey('firmware-back')));
          await tester.pump();
          expect(find.byType(SettingsScreen).hitTestable(), findsOneWidget);
          expect(find.text('Recovery'), findsOneWidget);
          expect(
            board.requests,
            hasLength(1),
            reason:
                'Returning from software keeps the same public session observation',
          );
          final support = find.byKey(const ValueKey('open-contact-support'));
          await tester.ensureVisible(support);
          await tester.pump();
          expect(tester.getSize(support).height, greaterThanOrEqualTo(48));
          await tester.tap(support);
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          await tester.pump(Duration(seconds: available ? 3 : 10));
          final list = find.byKey(const ValueKey('support-report-list'));
          await _until(tester, () => list.evaluate().isNotEmpty);
          final body = find.byKey(const ValueKey('support-report-body'));
          await tester.scrollUntilVisible(
            body,
            300,
            scrollable: find
                .descendant(of: list, matching: find.byType(Scrollable))
                .first,
          );
          final text = tester.widget<TextField>(body).controller!.text;
          if (available) {
            expect(text, contains('mode=recovery, cause=early_crash'));
            expect(text, contains('early_crashes_rtc=3'));
            expect(text, contains('settings=not_loaded_in_recovery'));
            expect(
              text,
              contains('firmware_uptime_ms64: ${board.epochUptime}'),
            );
          } else {
            expect(text, contains('public_support: unavailable'));
          }
          expect(text, contains('incident_snapshot: unavailable'));
          expect(text, contains('Board actions: unavailable'));
          expect(text, isNot(contains('runtime_health_state: healthy')));
          expect(text, isNot(contains('persisted_state_corrupt')));
          expect(text.length, lessThanOrEqualTo(6000));
          _expectPublicRecoveryAccess(service, board);
          expect(board.connections, hasLength(1));
          await tester.enterText(
            body,
            'I can edit this partial recovery report.',
          );
          expect(
            tester.widget<TextField>(body).controller!.text,
            contains('I can edit'),
          );
        } finally {
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.runAsync(() => disposeBoard(service, board));
          await trace.close();
        }
        expect(tester.takeException(), isNull);
      },
    );
  }
  testWidgets(
    'RecoveryPendingUpdateSetupJourney cold pending update activates public health on Back to Setup once per session',
    (tester) async {
      final board = SupportSessionBoard()..recoveryOnly = true;
      final service = _RecoverySupportService(board);
      final trace = AppTraceRecorder.disabled();
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      try {
        await tester.runAsync(() => connectBoard(service, board));
        await tester.runAsync(
          () => service.fetchDeviceCapabilities(deviceId: 'board-1'),
        );
        final generation = service.connectionSessionToken;
        // A restored target without exact attempt evidence remains pending in
        // the production verifier. It must not prevent opening public health.
        final target = OtaVerificationTarget(
          deviceId: 'board-1',
          uploadConnectionToken: generation,
          version: '0.9.5',
          imageSha256: 'a' * 64,
        );
        await tester.runAsync(
          () => service.rememberPendingOtaVerification(target),
        );
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              appTrace: trace,
              firmwarePageBuilder: (ble, id, show, activity, availability) =>
                  FirmwareScreen(
                    bleService: ble,
                    boardId: id,
                    onRequestShowFirmwareTab: show,
                    onUpdateActivityChanged: activity,
                    onAvailabilityChanged: availability,
                    downloadService: FakeFirmwareDownloadService(),
                  ),
            ),
          ),
        );
        final back = find.byKey(const ValueKey('firmware-back'));
        await _until(
          tester,
          () =>
              back.evaluate().isNotEmpty &&
              tester.widget<IconButton>(back).onPressed != null,
        );
        expect(find.byType(FirmwareScreen).hitTestable(), findsOneWidget);
        expect(find.text('Board update'), findsOneWidget);
        expect(service.pendingOtaVerification, same(target));
        expect(
          board.requests,
          isEmpty,
          reason: 'Hidden Setup defers its public read',
        );
        await tester.tap(back);
        await tester.pump();
        await _until(tester, () => board.requests.isNotEmpty);
        await _until(
          tester,
          () => find
              .textContaining('Normal startup stopped after repeated restarts.')
              .evaluate()
              .isNotEmpty,
        );
        expect(find.byType(SettingsScreen).hitTestable(), findsOneWidget);
        expect(
          tester
              .widget<RuntimeHealthCard>(find.byType(RuntimeHealthCard))
              .recoveryHealth
              ?.recoveryReason,
          'earlyCrash',
        );
        expect(board.requests.map((request) => (request.$1, request.$2)), [
          (0x45, 0),
        ]);
        expect(service.connectionSessionToken, generation);
        expect(board.connections, hasLength(1));
        _expectPublicRecoveryAccess(service, board);
        expect(service.pendingOtaVerification, same(target));
        await tester.ensureVisible(
          find.byKey(const ValueKey('open-board-software')),
        );
        await tester.tap(find.byKey(const ValueKey('open-board-software')));
        await tester.pump();
        await tester.tap(back);
        await tester.pump();
        expect(
          board.requests,
          hasLength(1),
          reason: 'Returning again reuses the same session observation',
        );
        expect(
          find.textContaining(
            'Normal startup stopped after repeated restarts.',
          ),
          findsOneWidget,
        );
        debugPrint(
          '[RECOVERY_PENDING_SETUP] cold=update back=Setup public=earlyCrash reads=${board.requests.length} pairing_commands=${board.pairingCommands.length} connections=${board.connections.length} target=retained',
        );
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(() => disposeBoard(service, board));
        await trace.close();
      }
      expect(tester.takeException(), isNull);
    },
  );
}
