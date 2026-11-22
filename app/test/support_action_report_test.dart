import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/support_snapshot.dart';
import 'package:litch_mobile/src/models/support_report.dart';
import 'package:litch_mobile/src/models/support_diagnostics.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'support/support_session_board.dart';

void main() {
  test(
    '20 semantic records survive held/failing storage with original monotonic times and no retry events',
    () async {
      var mono = 5;
      final opening = Completer<Directory>();
      final trace = AppTraceRecorder.start(
        directory: () => opening.future,
        monotonicMs: () => mono,
      );
      final action = trace.userActions.begin(SupportActionKind.artworkLoad);
      trace.record('litchcraft.retry');
      trace.record('ble.poll');
      mono = 25;
      trace.userActions.finish(action, SupportActionOutcome.accepted);
      mono = 55;
      trace.userActions.finish(action, SupportActionOutcome.applied);
      expect(trace.userActions.actions, hasLength(1));
      expect(trace.userActions.actions.single.uptimeMs, 5);
      expect(trace.userActions.actions.single.durationMs, 50);
      opening.completeError(
        const FileSystemException('controlled storage unavailable'),
      );
      await trace.flush();
      expect(trace.available, isFalse);
      expect(
        trace.userActions.actions.single.outcome,
        SupportActionOutcome.applied,
      );
      for (var i = 0; i < 25; i++) {
        mono++;
        trace.userActions.begin(SupportActionKind.paint);
      }
      expect(trace.userActions.actions, hasLength(20));
      expect(trace.userActions.omitted, 6);
      await trace.close();
    },
  );
  test(
    'wire history refuses duplicate, future or reversed action times and signed overflow',
    () {
      final peer = SupportSessionBoard();
      for (final corrupt in <void Function(ByteData)>[
        (d) => d.setUint32(108, 1, Endian.little),
        (d) => d.setUint64(80, peer.epochUptime + 1, Endian.little),
        (d) => d.setUint64(100, 1, Endian.little),
        (d) => d.setUint8(35, 0x80),
      ]) {
        final bytes = peer.historyBytes(1);
        corrupt(ByteData.sublistView(bytes));
        expect(
          () => SupportBoardHistory.decode(bytes, 1),
          throwsFormatException,
        );
      }
      expect(
        SupportBoardHistory.decode(peer.historyBytes(1), 1).uptimeMs,
        greaterThan(0xffffffff),
      );
    },
  );
  test(
    'report reserves20total human-readable rows, honest separate t0 anchors and required metrics inside6000characters',
    () {
      var now = 100;
      final app = SupportActionHistory(() => now);
      for (var i = 0; i < 20; i++) {
        final id = app.begin(SupportActionKind.paint, count: 64);
        now += 200;
        app.finish(id, SupportActionOutcome.appliedOnPhone);
      }
      final peer = SupportSessionBoard();
      final history = SupportBoardHistory.decode(
        peer.historyBytes(7),
        7,
        appRequestedMs: 5000,
        appReceivedMs: 5050,
        bootNonce: 1,
      );
      final draft = const SupportReportBuilder().build(
        app: SupportAppInfo(
          version: 'v' * 64,
          buildNumber: 'b' * 32,
          platform: 'ios',
          osVersion: 'o' * 240,
        ),
        board: SupportBoardInfo(
          connected: true,
          actionHistory: history,
          publicHealth: SupportPublicHealth.decode(peer.publicBytes(6), 6),
          incident: decodeSupportIncident(peer.incidentBytes(8), 8),
        ),
        appActions: SupportAppActionSnapshot(
          actions: app.actions,
          capturedMs: now,
          omitted: app.omitted,
        ),
        session: const LitchSupportSessionDiagnostics(
          settingsWriteAttempts: 0xffffffff,
          settingsWriteSucceeded: 0xffffffff,
          settingsWriteFailed: 0xffffffff,
        ),
      );
      final rows = draft.body
          .split('\n')
          .where((line) => RegExp(r'^(App|Board) \+\d+ms:').hasMatch(line))
          .toList();
      expect(draft.body.length, lessThanOrEqualTo(6000));
      expect(rows, hasLength(20));
      expect(rows.where((r) => r.startsWith('App ')), hasLength(10));
      expect(rows.where((r) => r.startsWith('Board ')), hasLength(10));
      expect(draft.body, contains('App +0ms:'));
      expect(draft.body, contains('Board +0ms:'));
      expect(
        draft.body,
        contains('board boot uptime=${history.actions[10].uptimeMs} ms'),
      );
      expect(draft.body, contains('firmware_settings_saved_total: 25'));
      expect(draft.body, contains('firmware_uptime_ms64: ${peer.epochUptime}'));
      expect(draft.body, contains('RTT uncertainty=50ms'));
      expect(
        draft.body,
        contains('past clock drift unbounded; no synchronized order claimed'),
      );
      expect(draft.body, contains('applied in the app, board unconfirmed'));
      expect(
        draft.body,
        contains('unsaved_boots_runtime_tail_and_sleep_counts_may_be_lost'),
      );
      expect(draft.body, isNot(contains('00:00:')));
    },
  );
  test(
    'public recovery reset and RTC boots remain readable when private incident is unavailable',
    () {
      final peer = SupportSessionBoard()..recoveryOnly = true;
      final bytes = peer.publicBytes(5)..[7] = 4;
      final draft = const SupportReportBuilder().build(
        app: const SupportAppInfo(
          version: 'test',
          buildNumber: '1',
          platform: 'ios',
          osVersion: 'test',
        ),
        board: SupportBoardInfo(
          connected: true,
          publicHealth: SupportPublicHealth.decode(bytes, 5),
        ),
        session: const LitchSupportSessionDiagnostics(
          settingsWriteAttempts: 0,
          settingsWriteSucceeded: 0,
          settingsWriteFailed: 0,
        ),
      );
      expect(
        draft.body,
        contains(
          'firmware_reset_reason: panic (4); source=public_boot_snapshot',
        ),
      );
      expect(
        draft.body,
        contains('firmware_boot_count: 4; source=public_boot_snapshot'),
      );
      expect(draft.body, contains('firmware_watchdog_restarts: unavailable'));
      expect(draft.body, contains('incident_prior_context: unavailable'));
      expect(draft.body, contains('incident_current_flags: unavailable'));
      expect(
        draft.body,
        contains(
          'boot_watchdog_counter_scope: RTC-retained; not lifetime totals',
        ),
      );
    },
  );

  test(
    'public unavailable health never gains private zero counters or a fabricated order',
    () {
      final peer = SupportSessionBoard()..publicValidity = 4;
      final draft = const SupportReportBuilder().build(
        app: const SupportAppInfo(
          version: 'test',
          buildNumber: '1',
          platform: 'ios',
          osVersion: 'test',
        ),
        board: SupportBoardInfo(
          connected: true,
          publicHealth: SupportPublicHealth.decode(peer.publicBytes(1), 1),
        ),
        session: const LitchSupportSessionDiagnostics(
          settingsWriteAttempts: 0,
          settingsWriteSucceeded: 0,
          settingsWriteFailed: 0,
        ),
      );
      expect(
        draft.body,
        contains('firmware_settings_saved_total: unavailable'),
      );
      expect(draft.body, contains('firmware_runtime_metrics: unavailable'));
      expect(draft.body, contains('Board actions: unavailable'));
      expect(draft.body, contains('source timelines cannot be synchronized'));
      expect(draft.body, isNot(contains('runtime_health_state: healthy')));
    },
  );
}
