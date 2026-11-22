import 'dart:convert';
import 'dart:typed_data';
import 'health_render_capture.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/board_health_status.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/models/support_diagnostics.dart';
import 'package:litch_mobile/src/widgets/runtime_health_card.dart';

void main() {
  testWidgets(
    'session-only without an I/O witness stays informative and quiet',
    (WidgetTester tester) async {
      final context = ConnectionBoardHealthContext.tryParse(
        Uint8List.fromList(<int>[0x44, 1, 1, 0, 0, 0, 0, 0, 0, 0]),
      )!;
      final health = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('000000000')!,
      );
      expect(
        boardHealthNeedsAttention(
          health,
          persistence: context.persistence,
          sessionOnly: context.sessionOnly,
        ),
        isFalse,
      );
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: RuntimeHealthCard(
              result: health,
              persistence: context.persistence,
              persistenceSessionOnly: context.sessionOnly,
              refreshing: false,
              onRetry: () {},
            ),
          ),
        ),
      );
      expect(find.text('Saving needs attention'), findsNothing);
      expect(find.text('Reformat board storage…'), findsNothing);
      expect(find.text('Changes are not saved'), findsOneWidget);
      expect(
        find.textContaining('will be lost when the board turns off'),
        findsOneWidget,
      );
    },
  );
  test('board health event preserves orthogonal storage and watchdog truth', () {
    final BoardHealthStatus s = BoardHealthStatus.tryParse(
      '{"event":"board_health","v":1,"h":"000000000","ps":"i","rr":6,"boot":2,"wd":1}',
    )!;
    expect(s.runtimeHealth.state, RuntimeHealthState.healthy);
    expect(s.persistence, HomePersistenceDegradation.storageIo);
    expect(s.watchdogRestart, isTrue);
    expect(
      BoardHealthStatus.tryParse(
        '{"event":"board_health","v":1,"h":"bad","ps":"n","rr":0,"boot":0,"wd":0}',
      ),
      isNull,
    );
    expect(
      BoardHealthStatus.tryParse(
        '{"event":"board_health","v":1,"h":"000000000","ps":"x","rr":0,"boot":0,"wd":0}',
      ),
      isNull,
    );
  });

  test('minimum-MTU State health preserves the same board-owned facts', () {
    final runtimeBody = Uint8List(6);
    final runtimeData = ByteData.sublistView(runtimeBody);
    runtimeBody[0] = 1;
    runtimeData.setUint16(1, 0x1234, Endian.little);
    runtimeBody[3] = 2;
    runtimeData.setUint16(4, 0x21, Endian.little);
    final contextBody = Uint8List(10);
    final contextData = ByteData.sublistView(contextBody);
    contextBody[0] = 0x46;
    contextBody[1] = 6;
    contextData.setUint32(2, 7, Endian.little);
    contextData.setUint32(6, 3, Endian.little);

    final status = BoardHealthStatus.fromConnectionState(
      ConnectionRuntimeHealthState.tryParse(runtimeBody)!,
      ConnectionBoardHealthContext.tryParse(contextBody)!,
    );
    expect(status.runtimeHealth.revision, 0x1234);
    expect(status.runtimeHealth.state, RuntimeHealthState.inputPaused);
    expect(status.runtimeHealth.causeMask, 0x21);
    expect(status.persistence, HomePersistenceDegradation.storageIo);
    expect(status.sessionOnly, isTrue);
    expect(status.resetReason, 6);
    expect(status.bootCount, 7);
    expect(status.watchdogRestarts, 3);
    expect(status.watchdogRestart, isTrue);

    expect(ConnectionRuntimeHealthState.tryParse(Uint8List(5)), isNull);
    contextBody[0] = 0x78;
    expect(ConnectionBoardHealthContext.tryParse(contextBody), isNull);
  });

  test(
    'worst-case incident report retains fields and redacts unknown data',
    () {
      final Map<String, Object> data = <String, Object>{
        'event': 'support_incident',
        'v': 1,
        for (final String key in SupportIncident.scalarKeys) key: 0xffffffff,
        for (final String key in SupportIncident.masks) key: 'FFFFFFFFFFFFFFFF',
        'stages': List<int>.filled(4, 14),
        'stage_ms': List<int>.filled(4, 0xffffffff),
        'now_stages': List<int>.filled(4, 14),
        'now_ms': List<int>.filled(4, 0xffffffff),
        'recent': List<int>.filled(8, 14),
        'secret': 'never-include',
      };
      final SupportIncident incident = SupportIncident.tryParse(
        jsonEncode(data),
      )!;
      final String long = 'x' * 10000;
      final SupportReportDraft draft = const SupportReportBuilder().build(
        app: SupportAppInfo(
          version: long,
          buildNumber: long,
          platform: long,
          osVersion: long,
        ),
        board: SupportBoardInfo(
          connected: true,
          incident: incident,
          firmwareVersion: long,
          firmwareDigest: long,
        ),
        session: LitchSupportSessionDiagnostics(
          settingsWriteAttempts: 1,
          settingsWriteSucceeded: 1,
          settingsWriteFailed: 0,
          lastSettingsOperation: long,
          lastSettingsFailureToken: long,
        ),
      );
      expect(draft.body.length, lessThan(6000));
      expect(draft.body, contains('"prior_up":4294967295'));
      expect(draft.body, contains('privacy:'));
      expect(draft.body, isNot(contains('never-include')));
    },
  );

  for (final HomePersistenceDegradation persistence
      in HomePersistenceDegradation.values) {
    testWidgets(
      'storage $persistence remains explicit with healthy sensors at 200% text',
      (WidgetTester tester) async {
        await tester.binding.setSurfaceSize(const Size(320, 1000));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final SemanticsHandle semantics = tester.ensureSemantics();
        await tester.pumpWidget(
          MaterialApp(
            home: MediaQuery(
              data: const MediaQueryData(textScaler: TextScaler.linear(2)),
              child: Scaffold(
                body: SingleChildScrollView(
                  child: RuntimeHealthCard(
                    result: RuntimeHealthAvailable(
                      RuntimeHealthSnapshot.fromToken('000000000')!,
                    ),
                    persistence: persistence,
                    refreshing: false,
                    onRetry: () {},
                  ),
                ),
              ),
            ),
          ),
        );
        expect(
          find.text(
            persistence == HomePersistenceDegradation.storageIo
                ? 'Saving needs attention'
                : 'Saved game recovery',
          ),
          findsOneWidget,
        );
        expect(
          find.textContaining(persistenceLossMessage(persistence)),
          findsOneWidget,
        );
        expect(tester.takeException(), isNull);
        expect(
          tester
              .getSize(
                find.byKey(const ValueKey<String>('runtime-health-toggle')),
              )
              .height,
          greaterThanOrEqualTo(48),
        );
        semantics.dispose();
      },
    );
  }
  testWidgets(
    'loading to combined healthy storage restart keeps all facts expanded',
    (WidgetTester tester) async {
      await loadHealthReviewFonts(tester);
      await tester.binding.setSurfaceSize(const Size(360, 780));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      Widget card(RuntimeHealthResult? result, bool incident) => MaterialApp(
        home: Scaffold(
          body: SafeArea(
            child: SingleChildScrollView(
              child: RuntimeHealthCard(
                result: result,
                persistence: incident
                    ? HomePersistenceDegradation.storageIo
                    : null,
                persistenceSessionOnly: incident,
                watchdogRestart: incident,
                refreshing: false,
                onRetry: () {},
              ),
            ),
          ),
        ),
      );
      await tester.pumpWidget(card(null, false));
      await tester.pumpWidget(
        card(
          RuntimeHealthAvailable(RuntimeHealthSnapshot.fromToken('000000000')!),
          true,
        ),
      );
      expect(
        find.textContaining('Saving is unavailable for this session'),
        findsOneWidget,
      );
      expect(
        find.textContaining('after it stopped responding'),
        findsOneWidget,
      );
      expect(
        find.textContaining('normal sensor and power health'),
        findsOneWidget,
      );
      await captureHealthReview(tester, 'combined-health-card');
    },
  );
  testWidgets(
    'late watchdog snapshot expands unavailable card without claiming live fault',
    (WidgetTester tester) async {
      Widget card(bool restart) => MaterialApp(
        home: Scaffold(
          body: RuntimeHealthCard(
            result: const RuntimeHealthUnavailable(
              RuntimeHealthUnavailableReason.timedOut,
            ),
            watchdogRestart: restart,
            refreshing: false,
            onRetry: () {},
          ),
        ),
      );
      await tester.pumpWidget(card(false));
      await tester.pumpWidget(card(true));
      expect(find.text('Board restarted'), findsOneWidget);
      expect(
        find.textContaining('after it stopped responding'),
        findsOneWidget,
      );
      expect(find.textContaining('did not reply'), findsOneWidget);
    },
  );
}
