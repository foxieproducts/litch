import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/support_board_collection.dart';
import 'package:litch_mobile/src/models/support_diagnostics.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/screens/support_report_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/support_report_service.dart';

// Adapter/screen lifecycle evidence only. The real BLE collection suite owns
// native-write deadlines, subscription cleanup, and transport single-flight.
class _CollectionBle implements LitchBleService {
  int generation = 1;
  int calls = 0;
  int? requestedToken;
  bool canceled = false;
  final Completer<SupportBoardCollection> result =
      Completer<SupportBoardCollection>();

  @override
  bool get isConnected => true;
  // These adapter-lifecycle cases isolate collection from an authorized board.
  // Owner-pending and recovery-only access use the real collector journeys.
  @override
  bool get productReady => true;
  @override
  String get connectedId => 'private-board';
  @override
  int get connectionSessionToken => generation;
  @override
  LitchSupportSessionDiagnostics get supportSessionDiagnostics =>
      const LitchSupportSessionDiagnostics(
        settingsWriteAttempts: 0,
        settingsWriteSucceeded: 0,
        settingsWriteFailed: 0,
      );

  @override
  Future<SupportBoardCollection> collectSupportBoardDiagnostics({
    required String deviceId,
    int? connectionSessionToken,
    Duration timeout = const Duration(seconds: 10),
    Future<void>? cancelled,
  }) {
    calls++;
    requestedToken = connectionSessionToken;
    cancelled?.then((_) => canceled = true);
    return result.future;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

const SupportAppInfo _app = SupportAppInfo(
  version: 'fixture',
  buildNumber: '1',
  platform: 'ios',
  osVersion: 'fixture',
);

void main() {
  testWidgets(
    'metadata wait cannot reattribute an already completed board snapshot',
    (tester) async {
      final _CollectionBle ble = _CollectionBle();
      final Completer<SupportAppInfo> metadata = Completer<SupportAppInfo>();
      final SupportReportService collector = SupportReportService(
        bleService: ble,
        loadAppInfo: () => metadata.future,
      );
      final Future<SupportReportDraft> pending = collector.createDraft();
      ble.result.complete(
        const SupportBoardCollection(
          incident: SupportQueryResult(
            outcome: SupportQueryOutcome.unavailable,
          ),
          firmwareDigest: SupportQueryResult(
            outcome: SupportQueryOutcome.unavailable,
          ),
          capabilities: SupportQueryResult(
            outcome: SupportQueryOutcome.unavailable,
          ),
          runtimeHealth: SupportQueryResult(
            outcome: SupportQueryOutcome.unavailable,
          ),
          firmwareVersion: SupportQueryResult(
            outcome: SupportQueryOutcome.available,
            value: 'old-session-version',
          ),
          sessionCurrent: true,
        ),
      );
      await tester.pump();
      ble.generation++;
      metadata.complete(_app);
      await tester.pump();
      final SupportReportDraft draft = await pending;
      expect(ble.requestedToken, 1);
      expect(
        draft.body,
        contains('collection_status: disconnected_or_session_changed'),
      );
      expect(draft.body, contains('firmwareVersion=sessionChanged'));
      expect(draft.body, isNot(contains('old-session-version')));
      expect(draft.body, isNot(contains('private-board')));
    },
  );

  testWidgets('board collection starts before stalled app metadata', (
    tester,
  ) async {
    final _CollectionBle ble = _CollectionBle();
    final Completer<SupportAppInfo> metadata = Completer<SupportAppInfo>();
    final SupportReportService collector = SupportReportService(
      bleService: ble,
      loadAppInfo: () => metadata.future,
    );
    SupportReportDraft? draft;
    final Future<void> pending = collector.createDraft().then(
      (value) => draft = value,
    );
    await tester.pump();
    expect(ble.calls, 1);
    expect(ble.requestedToken, 1);
    expect(draft, isNull);
    ble.result.completeError(StateError('private transport exception'));
    await tester.pump(const Duration(seconds: 3));
    await pending;
    expect(draft!.body, contains('app_version: unavailable'));
    expect(draft!.body, contains('incident=unavailable'));
    expect(draft!.body, isNot(contains('private transport exception')));
    metadata.complete(_app);
    await tester.pump();
    expect(draft!.body, contains('app_version: unavailable'));
  });

  testWidgets(
    'screen exit cancels its owned collection and ignores late failure',
    (tester) async {
      final _CollectionBle ble = _CollectionBle();
      await tester.pumpWidget(
        MaterialApp(home: SupportReportScreen(bleService: ble)),
      );
      await tester.pump();
      expect(ble.calls, 1);
      expect(find.byType(CircularProgressIndicator), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      expect(ble.canceled, isTrue);
      ble.result.completeError(StateError('private late failure'));
      await tester.pump(const Duration(seconds: 3));
      expect(tester.takeException(), isNull);
    },
  );
}
