import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/board_health_status.dart';
import 'package:litch_mobile/src/models/support_board_collection.dart';
import 'package:litch_mobile/src/models/support_snapshot.dart';
import 'support/support_session_board.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/models/support_diagnostics.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/support_report_service.dart';

// Exercise the production report adapter and formatter with typed collection
// results. Real SDK timing and query ordering belong to the BLE collection suite.
class _ReportBle implements LitchBleService {
  String? currentId = 'private-board-A';
  int generation = 1;
  bool ownerReady = true;
  @override
  bool identifyConnectionActive = false;
  final List<String> calls = <String>[];
  final Set<String> failures = <String>{};
  Completer<void>? collectionGate;
  final Completer<void> collectionStarted = Completer<void>();
  RuntimeHealthResult health = RuntimeHealthAvailable(
    RuntimeHealthSnapshot.fromToken('000100000')!,
  );

  @override
  String? get connectedId => currentId;
  @override
  bool get isConnected => currentId != null;
  @override
  bool get productReady => isConnected && ownerReady;
  @override
  int get connectionSessionToken => generation;
  @override
  BoardHealthStatus? get latestBoardHealth => null;
  @override
  LitchSupportSessionDiagnostics get supportSessionDiagnostics =>
      const LitchSupportSessionDiagnostics(
        settingsWriteAttempts: 2,
        settingsWriteSucceeded: 1,
        settingsWriteFailed: 1,
        lastSettingsOperation: 'setting_write:BRIGHTNESS',
        lastSettingsFailureToken: 'board:storage_unavailable',
        notificationRepairOperations: 2,
        notificationRepairAttempts: 5,
        notificationRepairRecovered: 1,
        notificationRepairExhausted: 1,
      );

  SupportQueryResult<T> field<T>(String name, T value) => SupportQueryResult<T>(
    outcome: failures.contains(name)
        ? SupportQueryOutcome.unavailable
        : SupportQueryOutcome.available,
    value: failures.contains(name) ? null : value,
  );

  @override
  Future<SupportBoardCollection> collectSupportBoardDiagnostics({
    required String deviceId,
    int? connectionSessionToken,
    Duration timeout = const Duration(seconds: 10),
    Future<void>? cancelled,
  }) async {
    calls.add('board');
    if (!collectionStarted.isCompleted) collectionStarted.complete();
    await collectionGate?.future;
    final SupportIncident incident = SupportIncident.tryParse(
      jsonEncode(<String, Object>{
        'event': 'support_incident',
        'v': 1,
        for (final String key in SupportIncident.scalarKeys) key: 0,
        'boot': 3,
        'saved': 7,
        for (final String key in SupportIncident.masks) key: '0000000000000000',
        'stages': <int>[0, 0, 0, 0],
        'stage_ms': <int>[0, 0, 0, 0],
        'now_stages': <int>[3, 14, 14, 14],
        'now_ms': <int>[100, 90, 90, 90],
        'recent': List<int>.filled(8, 0),
      }),
    )!;
    return SupportBoardCollection(
      actionHistory: field(
        'history',
        SupportBoardHistory.decode(SupportSessionBoard().historyBytes(1), 1),
      ),
      incident: field('incident', incident),
      firmwareDigest: field('digest', List<String>.filled(64, 'a').join()),
      capabilities: field(
        'capabilities',
        const LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: LitchHardwareRevision.v2,
          capabilityMask: 127,
          explicitlyReported: true,
        ),
      ),
      runtimeHealth: failures.contains('health')
          ? const SupportQueryResult(
              outcome: SupportQueryOutcome.unavailable,
              value: RuntimeHealthUnavailable(
                RuntimeHealthUnavailableReason.sendFailed,
              ),
            )
          : field('health', health),
      firmwareVersion: field('version', 'audit-fixture-build'),
      sessionCurrent: true,
    );
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

SupportReportService _collector(_ReportBle ble) => SupportReportService(
  bleService: ble,
  boardId: 'private-board-A',
  loadAppInfo: () async => const SupportAppInfo(
    version: 'audit-fixture',
    buildNumber: '1',
    platform: 'ios',
    osVersion: 'synthetic-test-os',
  ),
);

void _expectBoundedPrivateDraft(SupportReportDraft draft) {
  expect(draft.body.length, lessThanOrEqualTo(6000));
  expect(draft.subject.length, lessThanOrEqualTo(160));
  expect(draft.body, isNot(contains('private-board-')));
  expect(draft.body, isNot(contains('private exception')));
  expect(draft.body, contains('app_version: audit-fixture'));
  expect(draft.body, contains('app_settings_write_failed: 1'));
  expect(
    draft.body,
    contains(
      'app_notification_repairs: ops=2, pending=0, attempts=5, '
      'recovered=1, exhausted=1, cancelled=0, saturated=false',
    ),
  );
}

void main() {
  test(
    'report adapter retains available same-session board evidence',
    () async {
      final _ReportBle ble = _ReportBle();
      final SupportReportDraft draft = await _collector(ble).createDraft();
      expect(ble.calls, <String>['board']);
      expect(draft.body, contains('board_connection: connected'));
      expect(draft.body, contains('board_firmware: audit-fixture-build'));
      expect(draft.body, contains('runtime_health_state: healthy'));
      expect(draft.body, contains('firmware_settings_completion_count: 7'));
      _expectBoundedPrivateDraft(draft);
    },
  );

  test(
    'collector preserves degraded health rather than declaring healthy',
    () async {
      final _ReportBle ble = _ReportBle()
        ..health = RuntimeHealthAvailable(
          RuntimeHealthSnapshot.fromToken('00AF10030')!,
        );
      final SupportReportDraft draft = await _collector(ble).createDraft();
      expect(draft.subject, contains('Board health'));
      expect(draft.body, contains('runtime_health_support_code: 00AF-1-0030'));
      _expectBoundedPrivateDraft(draft);
    },
  );

  for (final scenario in ['disconnected', 'different board', 'identify only']) {
    test('$scenario collector does not request board diagnostics', () async {
      final ble = _ReportBle();
      if (scenario == 'disconnected') ble.currentId = null;
      if (scenario == 'different board') ble.currentId = 'private-board-B';
      if (scenario == 'identify only') ble.identifyConnectionActive = true;
      final draft = await _collector(ble).createDraft();
      expect(ble.calls, isEmpty);
      expect(draft.body, contains('board_connection: disconnected'));
      expect(draft.body, contains('incident_snapshot: unavailable'));
      expect(
        draft.body,
        contains('runtime_health_unavailable_reason: disconnected'),
      );
      _expectBoundedPrivateDraft(draft);
    });
  }

  for (final String failed in <String>[
    'incident',
    'digest',
    'capabilities',
    'health',
    'version',
  ]) {
    test('unavailable $failed result still produces a partial draft', () async {
      final _ReportBle ble = _ReportBle()..failures.add(failed);
      final SupportReportDraft draft = await _collector(ble).createDraft();
      expect(ble.calls, <String>['board']);
      final String unavailable = switch (failed) {
        'incident' => 'incident_snapshot: unavailable',
        'digest' => 'firmware_sha256: unavailable',
        'capabilities' => 'board_hardware: unavailable',
        'health' => 'runtime_health_unavailable_reason: sendFailed',
        _ => 'board_firmware: unavailable',
      };
      expect(draft.body, contains(unavailable));
      _expectBoundedPrivateDraft(draft);
    });
  }

  test(
    'all optional board failures retain a useful private app report',
    () async {
      final _ReportBle ble = _ReportBle()
        ..failures.addAll(<String>[
          'incident',
          'digest',
          'capabilities',
          'health',
          'version',
        ]);
      final SupportReportDraft draft = await _collector(ble).createDraft();
      expect(ble.calls, <String>['board']);
      for (final String field in <String>[
        'incident_snapshot',
        'firmware_sha256',
        'board_hardware',
        'board_firmware',
      ]) {
        expect(draft.body, contains('$field: unavailable'));
      }
      expect(
        draft.body,
        contains('runtime_health_unavailable_reason: sendFailed'),
      );
      _expectBoundedPrivateDraft(draft);
    },
  );

  test(
    'proof loss while phone metadata is pending redacts already collected private board fields',
    () async {
      final ble = _ReportBle();
      final metadata = Completer<SupportAppInfo>();
      final collector = SupportReportService(
        bleService: ble,
        boardId: 'private-board-A',
        loadAppInfo: () => metadata.future,
      );
      final pending = collector.createDraft();
      await ble.collectionStarted.future;
      // Allow the already successful board collection to settle, then revoke
      // authority without changing the native connection generation.
      await Future<void>.delayed(Duration.zero);
      ble.ownerReady = false;
      metadata.complete(
        const SupportAppInfo(
          version: 'audit-fixture',
          buildNumber: '1',
          platform: 'ios',
          osVersion: 'test',
        ),
      );
      final draft = await pending;
      expect(draft.body, contains('incident_snapshot: unavailable'));
      expect(draft.body, contains('Board actions: unavailable'));
      expect(draft.body, contains('firmware_runtime_metrics: unavailable'));
      expect(
        draft.body,
        contains('actionHistory=ownerNotReady, incident=ownerNotReady'),
      );
      expect(draft.body, contains('board_firmware: audit-fixture-build'));
      expect(draft.body, isNot(contains('Board +0ms:')));
    },
  );

  for (final bool replaceBoard in <bool>[false, true]) {
    test(
      'session change discards all collected board facts (replace=$replaceBoard)',
      () async {
        final _ReportBle ble = _ReportBle()..collectionGate = Completer<void>();
        final SupportReportService collector = _collector(ble);
        final Future<SupportReportDraft> pending = collector.createDraft();
        await ble.collectionStarted.future;
        ble.generation++;
        if (replaceBoard) ble.currentId = 'private-board-B';
        ble.collectionGate!.complete();
        final SupportReportDraft draft = await pending;
        expect(
          draft.body,
          contains('collection_status: disconnected_or_session_changed'),
        );
        expect(draft.body, contains('board_connection: disconnected'));
        for (final String field in <String>[
          'incident_snapshot',
          'firmware_sha256',
          'board_hardware',
          'board_firmware',
        ]) {
          expect(draft.body, contains('$field: unavailable'));
        }
        expect(
          draft.body,
          contains('runtime_health_unavailable_reason: disconnected'),
        );
        _expectBoundedPrivateDraft(draft);

        // A separately requested report may collect the new session normally.
        final SupportReportDraft recovered = await collector.createDraft();
        expect(recovered.body, contains('board_connection: connected'));
        expect(recovered.body, contains('board_firmware: audit-fixture-build'));
        _expectBoundedPrivateDraft(recovered);
      },
    );
  }
}
