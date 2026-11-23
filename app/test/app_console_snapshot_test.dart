import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/models/home_state_wire_codec.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/services/ota/ota_post_reboot_verifier.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'support/owner_pairing_board.dart';
import 'support/reboot_reply_ble.dart';

class _NoRadioBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _HeldRestoreStore extends MemoryOtaTargetStore {
  final started = Completer<void>();
  final released = Completer<void>();
  @override
  Future<OtaVerificationTarget?> load() async {
    started.complete();
    await released.future;
    return null;
  }
}

void main() {
  test(
    'held connect restore is saved as connecting instead of retired Ready',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch_connect_snapshot_',
      );
      final trace = await AppTraceRecorder.open(directory);
      final store = _HeldRestoreStore();
      final board = RebootReplyBle();
      final service = LitchBleService(
        ble: board,
        appTrace: trace,
        pendingOtaStore: store,
      );
      Future<bool>? connecting;
      try {
        service.debugSetConnectedDevice('board-1');
        expect(await service.ensureProductReady('board-1'), isTrue);
        expect(service.productReady, isTrue);
        await trace.waitForConsoleSnapshotPublication();
        connecting = service.connect('board-1');
        await store.started.future;
        expect(service.productReady, isFalse);
        await trace.waitForConsoleSnapshotPublication();
        final saved =
            jsonDecode(
                  await File(
                    '${directory.path}/console_snapshot.json',
                  ).readAsString(),
                )
                as Map;
        expect(saved['readiness'], containsPair('phase', 'connecting'));
        expect(saved['readiness'], containsPair('product_ready', false));
      } finally {
        await service.disconnect();
        if (!store.released.isCompleted) store.released.complete();
        await connecting;
        await disposeBoard(service, board);
        await trace.close();
        await directory.delete(recursive: true);
      }
    },
  );
  test('live owner readiness is saved without another Console line', () async {
    SharedPreferences.setMockInitialValues({});
    final directory = await Directory.systemTemp.createTemp(
      'litch_owner_snapshot_',
    );
    final trace = await AppTraceRecorder.open(directory);
    final board = OwnerPairingBoard()
      ..state = 6
      ..flags = 5;
    final service = LitchBleService(
      ble: board,
      appTrace: trace,
      pendingOtaStore: MemoryOtaTargetStore(),
      ownerPairingReadinessTimeout: const Duration(seconds: 2),
    );
    final file = File('${directory.path}/console_snapshot.json');
    Future<Map<String, dynamic>> saved() async {
      await trace.waitForConsoleSnapshotPublication();
      return jsonDecode(await file.readAsString()) as Map<String, dynamic>;
    }

    try {
      await service.setAppForegroundActive(false);
      await connectBoard(service, board);
      for (
        var turn = 0;
        turn < 100 && service.ownerPairingStatus == null;
        ++turn
      ) {
        await Future<void>.delayed(Duration.zero);
      }
      final waiting = await saved();
      expect(waiting['readiness'], containsPair('phase', 'confirmingOwner'));
      expect(waiting['readiness'], containsPair('owner_state', 'ownedOther'));
      expect(waiting['readiness'], containsPair('owner_reason', 'none'));
      expect(waiting['readiness'], containsPair('owner_flags', 5));
      expect(waiting['readiness'], containsPair('owner_attempt', 0));
      expect(waiting['readiness'], containsPair('session_activated', true));
      expect(await service.ensureProductReady('board-1'), isFalse);
      final failed = await saved();
      expect(failed['readiness'], containsPair('phase', 'failed'));
      final lines = service.consoleLinesSnapshot;
      Uint8List fixture(String file, String name) {
        final row = File(
          '../test/fixtures/$file',
        ).readAsLinesSync().firstWhere((line) => line.startsWith('$name,'));
        final hex = row.substring(row.indexOf(',') + 1);
        return Uint8List.fromList([
          for (var i = 0; i < hex.length; i += 2)
            int.parse(hex.substring(i, i + 2), radix: 16),
        ]);
      }

      final home = fixture('home_state_wire_v1.csv', 'representative');
      final settings = fixture('settings_state_wire_v3.csv', 'defaults');
      board.onProductQuery = (code) {
        if (code != 0) return;
        for (var i = 0; i < HomeStateWireCodec.fragmentCount; ++i) {
          board.emitState(
            HomeStateWireCodec.firstStateCode + i,
            home.sublist(i * 10, (i + 1) * 10),
            sequence: 100,
          );
        }
        for (var i = 0; i < SettingsStateWireCodec.fragmentCount; ++i) {
          board.emitState(
            SettingsStateWireCodec.fragmentCode(i),
            settings.sublist(
              i * 10,
              i * 10 + SettingsStateWireCodec.fragmentLength(i),
            ),
            sequence: 100,
          );
        }
      };
      board.setState(5, nextFlags: 7);
      for (var turn = 0; turn < 100 && !service.productReady; ++turn) {
        await Future<void>.delayed(Duration.zero);
      }
      expect(service.productReady, isTrue);
      expect(service.consoleLinesSnapshot, lines);
      final ready = await saved();
      expect(ready['lines'], failed['lines']);
      expect(ready['readiness'], containsPair('phase', 'ready'));
      expect(ready['readiness'], containsPair('product_ready', true));
      expect(ready['readiness'], containsPair('owner_state', 'ownerReady'));
      expect(ready['readiness'], containsPair('owner_flags', 7));
      await service.disconnect();
      final disconnected = await saved();
      expect(disconnected['lines'], ready['lines']);
      expect(disconnected['history_is_cached'], isTrue);
      expect(disconnected['readiness'], containsPair('phase', 'disconnected'));
      expect(disconnected['readiness'], containsPair('product_ready', false));
      expect(
        disconnected['readiness'],
        containsPair('session_activated', false),
      );
      for (final field in [
        'owner_state',
        'owner_reason',
        'owner_flags',
        'owner_attempt',
      ]) {
        expect((disconnected['readiness'] as Map)[field], isNull);
      }
    } finally {
      await disposeBoard(service, board);
      await trace.close();
      await directory.delete(recursive: true);
    }
  });
  test(
    'held storage retains latest public Console capture through connection transitions',
    () async {
      for (final connected in [true, false]) {
        final directory = await Directory.systemTemp.createTemp(
          'litch_console_held_',
        );
        final opening = Completer<Directory>();
        var now = DateTime.utc(2026, 10, 25, 18);
        final trace = AppTraceRecorder.start(
          directory: () => opening.future,
          now: () => now,
        );
        try {
          trace.captureConsole(
            lines: ['early board reply'],
            connectionGeneration: 7,
            connected: true,
            connectedBoardId: 'board-a',
            historyBoardId: 'board-a',
          );
          now = now.add(const Duration(milliseconds: 20));
          if (!connected) {
            trace.captureConsole(
              lines: ['early board reply'],
              connectionGeneration: 8,
              connected: false,
              historyBoardId: 'board-a',
            );
          }
          opening.complete(directory);
          await trace.flush();
          await trace.waitForConsoleSnapshotPublication();
          final value =
              jsonDecode(
                    await File(
                      '${directory.path}/console_snapshot.json',
                    ).readAsString(),
                  )
                  as Map<String, dynamic>;
          expect(value['lines'], ['early board reply']);
          expect(value['connected'], connected);
          expect(value['connection_generation'], connected ? 7 : 8);
          expect(value['history_board_id'], 'board-a');
          expect(value['history_is_cached'], !connected);
          expect(
            value['lines_observed_at'],
            DateTime.utc(2026, 10, 25, 18).toIso8601String(),
          );
          expect(
            value['connection_observed_at'],
            (connected ? DateTime.utc(2026, 10, 25, 18) : now)
                .toIso8601String(),
          );
        } finally {
          await trace.close();
          await directory.delete(recursive: true);
        }
      }
    },
  );

  test(
    'held Console captures fail closed when asynchronous storage is unavailable',
    () async {
      final opening = Completer<Directory>();
      final trace = AppTraceRecorder.start(directory: () => opening.future);
      trace.captureConsole(
        lines: ['early reply'],
        connectionGeneration: 1,
        connected: true,
      );
      opening.completeError(const FileSystemException('storage unavailable'));
      await trace.flush();
      await trace.waitForConsoleSnapshotPublication();
      expect(trace.available, isFalse);
      await trace.close();
      final disabled = AppTraceRecorder.disabled();
      disabled.captureConsole(
        lines: ['ignored'],
        connectionGeneration: 2,
        connected: true,
      );
      await disabled.waitForConsoleSnapshotPublication();
      expect(disabled.available, isFalse);
      await disabled.close();
    },
  );

  testWidgets(
    'privacy ring snapshot and close do not wait for a scheduled Console publication',
    (tester) async {
      final directory = (await tester.runAsync(
        () => Directory.systemTemp.createTemp('litch_console_independent_'),
      ))!;
      final trace = (await tester.runAsync(
        () => AppTraceRecorder.open(directory),
      ))!;
      final file = File('${directory.path}/console_snapshot.json');
      final initial = (await tester.runAsync(file.readAsString))!;
      try {
        trace.captureConsole(
          lines: ['pending observation'],
          connectionGeneration: 1,
          connected: true,
          connectedBoardId: 'board-a',
          historyBoardId: 'board-a',
        );
        AppTraceSnapshot? snapshot;
        expect(
          await _settlesWithoutTimeAdvance(
            tester,
            () async {
              trace.record('test.independent_ring');
              snapshot = await trace.snapshot();
            },
          ),
          isTrue,
          reason: 'Privacy diagnostics must not wait for the Console throttle',
        );
        expect(snapshot!.jsonl, contains('test.independent_ring'));
        expect(await tester.runAsync(file.readAsString), initial);
        expect(
          await _settlesWithoutTimeAdvance(tester, trace.close),
          isTrue,
          reason:
              'Close must cancel a scheduled observation without advancing its timer',
        );
        await tester.pump(const Duration(seconds: 2));
        expect(await tester.runAsync(file.readAsString), initial);
      } finally {
        // Also drain the old candidate after the expected red assertion.
        await tester.pump(const Duration(seconds: 2));
        expect(await _settlesWithoutTimeAdvance(tester, trace.close), isTrue);
        await tester.runAsync(() => directory.delete(recursive: true));
      }
    },
  );
  test(
    'complete INFO console is available from the ordinary local app file',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch_console_snapshot_',
      );
      final trace = await AppTraceRecorder.open(directory);
      final service = LitchBleService(
        ble: _NoRadioBle(),
        appTrace: trace,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      try {
        service.debugSetConnectedDevice('actual-board-identity');
        service.debugAppendConsolePacket(
          utf8.encode(
            '[INFO] [DEBUG-store-timing] phase=first elapsed=878\npartial',
          ),
        );
        await trace.waitForConsoleSnapshotPublication();
        final file = File('${directory.path}/console_snapshot.json');
        expect(
          await file.exists(),
          isTrue,
          reason:
              'Release diagnostics must be retrievable without Mirroring or transcription',
        );
        final snapshot =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        expect(snapshot['connected'], isTrue);
        expect(snapshot['connected_board_id'], 'actual-board-identity');
        expect(snapshot['lines'], service.consoleLinesSnapshot);
        expect(
          (snapshot['lines'] as List).single,
          contains('phase=first elapsed=878'),
        );
        expect(
          (snapshot['lines'] as List).any(
            (line) => line.toString().contains('partial'),
          ),
          isFalse,
        );
        await service.disconnect();
        await trace.waitForConsoleSnapshotPublication();
        final disconnected =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        expect(disconnected['connected'], isFalse);
        expect(disconnected['connected_board_id'], isNull);
        expect(
          disconnected['connection_generation'],
          greaterThan(snapshot['connection_generation'] as int),
        );
      } finally {
        await service.dispose();
        await trace.close();
        await directory.delete(recursive: true);
      }
    },
  );
  test(
    'latest-only console writes coalesce without changing the JSONL ring',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch_console_snapshot_',
      );
      final trace = await AppTraceRecorder.open(directory);
      final file = File('${directory.path}/console_snapshot.json');
      try {
        final before = await file.readAsString();
        var latest = <String>[];
        for (var batch = 0; batch < 500; ++batch) {
          latest = List.generate(120, (line) => 'complete $batch/$line');
          trace.captureConsole(
            lines: latest,
            connectionGeneration: 9,
            connected: true,
            connectedBoardId: 'board-a',
            historyBoardId: 'board-a',
          );
        }
        latest[0] = 'caller-mutated';
        expect(
          file.readAsStringSync(),
          before,
          reason: 'No per-packet or synchronous snapshot writes',
        );
        trace.record('test.ring', fields: {'value': 1});
        await trace.waitForConsoleSnapshotPublication();
        final value =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        expect(value['revision'], 501);
        expect(
          value['lines'],
          List.generate(120, (line) => 'complete 499/$line'),
        );
        expect(value['source_line_count'], 120);
        expect(value['omitted_lines'], 0);
        expect(value['omission_reason'], isNull);
        expect(value['history_is_cached'], isFalse);
        expect(await File('${file.path}.tmp').exists(), isFalse);
        final ring = await trace.snapshot();
        expect(ring.jsonl, contains('test.ring'));
        expect(ring.jsonl, isNot(contains('complete 499')));
        expect(
          directory
              .listSync()
              .whereType<File>()
              .where((f) => f.path.endsWith('.json'))
              .length,
          1,
        );
      } finally {
        await trace.close();
        await directory.delete(recursive: true);
      }
    },
  );

  test(
    'encoded byte budget retains exact newest complete lines and reports omissions',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch_console_snapshot_',
      );
      final trace = await AppTraceRecorder.open(directory);
      try {
        final lines = List.generate(
          120,
          (i) => '$i ${List.filled(2040, '\\').join()}',
        );
        trace.captureConsole(
          lines: lines,
          connectionGeneration: 2,
          connected: true,
          connectedBoardId: 'actual-board',
          historyBoardId: 'actual-board',
        );
        await trace.waitForConsoleSnapshotPublication();
        final bytes = await File(
          '${directory.path}/console_snapshot.json',
        ).readAsBytes();
        expect(bytes.length, lessThanOrEqualTo(320 * 1024));
        final value = jsonDecode(utf8.decode(bytes)) as Map<String, dynamic>;
        expect(value['app_generation'], 2);
        final retained = value['lines'] as List;
        expect(retained.length, inInclusiveRange(1, 119));
        expect(retained, lines.skip(lines.length - retained.length).toList());
        expect(value['source_line_count'], 120);
        expect(value['omitted_lines'], 120 - retained.length);
        expect(value['omission_reason'], 'byte_budget');
        // Oversized host error strings never become truncated console lines.
        trace.captureConsole(
          lines: ['old', List.filled(5000, 'x').join(), 'new'],
          connectionGeneration: 2,
          connected: true,
        );
        await trace.waitForConsoleSnapshotPublication();
        final bounded =
            jsonDecode(
                  await File(
                    '${directory.path}/console_snapshot.json',
                  ).readAsString(),
                )
                as Map<String, dynamic>;
        expect(bounded['lines'], ['old', 'new']);
        expect(bounded['omitted_lines'], 1);
        expect(bounded['omission_reason'], 'source_bounds');
      } finally {
        await trace.close();
        await directory.delete(recursive: true);
      }
    },
  );

  test(
    'disconnect and recorder restart cannot label old console as a live current board',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch_console_snapshot_',
      );
      var now = DateTime.utc(2026, 10, 24, 22);
      var trace = await AppTraceRecorder.open(directory, now: () => now);
      final file = File('${directory.path}/console_snapshot.json');
      try {
        trace.captureConsole(
          lines: ['board A complete'],
          connectionGeneration: 7,
          connected: true,
          connectedBoardId: 'board-a',
          historyBoardId: 'board-a',
        );
        await trace.waitForConsoleSnapshotPublication();
        final original =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        now = now.add(const Duration(seconds: 10));
        trace.captureConsole(
          lines: ['board A complete'],
          connectionGeneration: 8,
          connected: false,
          historyBoardId: 'board-a',
        );
        await trace.waitForConsoleSnapshotPublication();
        final cached =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        expect(cached['connected'], isFalse);
        expect(cached['connected_board_id'], isNull);
        expect(cached['history_board_id'], 'board-a');
        expect(cached['history_is_cached'], isTrue);
        expect(cached['connection_generation'], 8);
        expect(cached['lines_observed_at'], original['lines_observed_at']);
        expect(cached['connection_observed_at'], now.toIso8601String());
        await trace.close();
        now = now.add(const Duration(seconds: 10));
        trace = await AppTraceRecorder.open(directory, now: () => now);
        final restarted =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        expect(restarted['process_id'], pid);
        expect(restarted['process_started_at'], now.toIso8601String());
        expect(restarted['connected'], isFalse);
        expect(restarted['connection_generation'], 0);
        expect(restarted['history_board_id'], isNull);
        expect(restarted['lines'], isEmpty);
        trace.captureConsole(
          lines: ['board B'],
          connectionGeneration: 1,
          connected: true,
          connectedBoardId: 'board-b',
          historyBoardId: 'board-b',
        );
        await trace.waitForConsoleSnapshotPublication();
        final current =
            jsonDecode(await file.readAsString()) as Map<String, dynamic>;
        expect(current['lines'], ['board B']);
        expect(current['history_board_id'], 'board-b');
        expect(current['history_is_cached'], isFalse);
      } finally {
        await trace.close();
        await directory.delete(recursive: true);
      }
    },
  );

  test(
    'atomic snapshot failure preserves the previous complete file and leaves BLE and AppTrace usable',
    () async {
      final directory = await Directory.systemTemp.createTemp(
        'litch_console_snapshot_',
      );
      final trace = await AppTraceRecorder.open(directory);
      final file = File('${directory.path}/console_snapshot.json');
      final before = await file.readAsString();
      // Fail the temporary write before rename, leaving the published JSON intact.
      await Directory('${file.path}.tmp').create();
      final service = LitchBleService(
        ble: _NoRadioBle(),
        appTrace: trace,
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      try {
        service.debugSetConnectedDevice('board-a');
        service.debugAppendConsolePacket(
          utf8.encode('[INFO] still delivered\n'),
        );
        trace.record('test.after_console_failure', fields: {'value': 1});
        await trace.waitForConsoleSnapshotPublication();
        expect(file.readAsStringSync(), before);
        expect(jsonDecode(before), isA<Map>());
        expect(
          service.consoleLinesSnapshot.single,
          contains('still delivered'),
        );
        expect(trace.available, isTrue);
        expect(
          (await trace.snapshot()).jsonl,
          contains('test.after_console_failure'),
        );
      } finally {
        await service.dispose();
        await trace.close();
        await directory.delete(recursive: true);
      }
    },
  );

  test(
    'a throwing optional sink cannot interrupt Console delivery or disconnect',
    () async {
      final service = LitchBleService(
        ble: _NoRadioBle(),
        consoleSnapshotSink: _ThrowingSnapshotSink(),
        pendingOtaStore: MemoryOtaTargetStore(),
      );
      service.debugSetConnectedDevice('board-a');
      service.debugAppendConsolePacket(utf8.encode('[INFO] safe\n'));
      expect(service.consoleLinesSnapshot.single, contains('safe'));
      await service.dispose();
    },
  );
}

Future<bool> _settlesWithoutTimeAdvance(
  WidgetTester tester,
  Future<void> Function() operation,
) async {
  // Keep the scheduled Console timer on fake time while disk I/O completes.
  Object? failure;
  final completed = await tester.runAsync(() async {
    try {
      await operation().timeout(const Duration(seconds: 2));
      return true;
    } on TimeoutException {
      return false;
    } catch (error) {
      failure = error;
      return false;
    }
  });
  if (failure != null) throw failure!;
  return completed!;
}

class _ThrowingSnapshotSink implements AppConsoleSnapshotSink {
  @override
  void captureConsole({
    required List<String> lines,
    required int connectionGeneration,
    required bool connected,
    String? connectedBoardId,
    String? historyBoardId,
    AppConsoleReadiness? readiness,
  }) => throw FileSystemException('controlled snapshot failure');
}
