import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';

void main() {
  late Directory directory;
  late DateTime now;
  int monotonicMs = 0;

  setUp(() async {
    directory = await Directory.systemTemp.createTemp('litch_app_trace_');
    now = DateTime.utc(2026, 9, 10, 12);
  });

  tearDown(() async {
    if (await directory.exists()) await directory.delete(recursive: true);
  });

  Future<AppTraceRecorder> open() => AppTraceRecorder.open(
    directory,
    now: () => now,
    monotonicMs: () => monotonicMs,
  );

  test(
    'startup proceeds while storage is held and early records stay ordered',
    () async {
      final storage = Completer<Directory>();
      final recorder = AppTraceRecorder.start(
        directory: () => storage.future,
        now: () => now,
        monotonicMs: () => monotonicMs,
      );
      monotonicMs = 7;
      recorder.record('app.started');
      monotonicMs = 19;
      recorder.record('ble.discovery_started');
      var flushed = false;
      final flush = recorder.flush().then((_) => flushed = true);
      await Future<void>.delayed(Duration.zero);
      expect(flushed, isFalse);
      expect(recorder.available, isTrue);
      storage.complete(directory);
      await flush;
      final records = _records((await recorder.snapshot()).jsonl);
      expect(records.map((row) => row['event']), [
        'app.started',
        'ble.discovery_started',
      ]);
      expect(records.map((row) => row['mono_ms']), [7, 19]);
      expect(records.map((row) => row['seq']), [1, 2]);
      await recorder.close();
    },
  );

  test(
    'failed asynchronous startup settles queued flush without replacing owner',
    () async {
      final storage = Completer<Directory>();
      final recorder = AppTraceRecorder.start(directory: () => storage.future);
      recorder.record('app.started');
      final flush = recorder.flush();
      storage.completeError(const FileSystemException('held storage failed'));
      await flush;
      expect(recorder.available, isFalse);
      expect((await recorder.snapshot()).failure, 'storage_unavailable');
      await recorder.close();
    },
  );

  test('fixed segments retain the newest hour across restart', () async {
    AppTraceRecorder recorder = await open();
    for (int bucket = 0; bucket <= 6; ++bucket) {
      now = DateTime.utc(2026, 9, 10, 12).add(Duration(minutes: bucket * 10));
      monotonicMs = bucket * 600000;
      recorder.record(
        'test.bucket',
        fields: <String, Object?>{'value': bucket},
      );
      await recorder.flush();
    }
    await recorder.close();

    recorder = await open();
    final AppTraceSnapshot snapshot = await recorder.snapshot();
    final List<Map<String, Object?>> records = _records(snapshot.jsonl);
    expect(records.map((record) => record['value']), <int>[
      0,
      1,
      2,
      3,
      4,
      5,
      6,
    ]);
    expect(
      directory
          .listSync()
          .whereType<File>()
          .where((file) => file.path.endsWith('.jsonl'))
          .length,
      AppTraceRecorder.segmentCount,
    );
    now = DateTime.utc(2026, 9, 10, 13, 1);
    expect(
      _records(
        (await recorder.snapshot()).jsonl,
      ).map((record) => record['value']),
      <int>[1, 2, 3, 4, 5, 6],
    );
    await recorder.close();
  });

  test('segment and queue bounds remain fixed under a burst', () async {
    final AppTraceRecorder recorder = await open();
    for (int batch = 0; batch < 100; ++batch) {
      for (int index = 0; index < 20; ++index) {
        recorder.record(
          'test.volume',
          fields: <String, Object?>{'batch': batch, 'index': index},
        );
      }
      await recorder.flush();
    }
    final AppTraceSnapshot snapshot = await recorder.snapshot();
    expect(snapshot.droppedEvents, greaterThan(0));
    expect(
      utf8.encode(snapshot.jsonl).length,
      lessThanOrEqualTo(AppTraceRecorder.maxSnapshotBytes),
    );
    for (final File file in directory.listSync().whereType<File>()) {
      expect(
        await file.length(),
        lessThanOrEqualTo(AppTraceRecorder.maxSegmentBytes),
      );
    }
    await recorder.close();
  });

  test(
    'maximum session retirement counters fit one sanitized record',
    () async {
      final recorder = await open();
      monotonicMs = 0x7fffffffffffffff;
      now = DateTime.utc(2026, 10, 24, 6, 39, 1, 676, 778);
      recorder.record(
        'ble.connection_session_retired',
        fields: <String, Object?>{
          'generation': 0x7fffffffffffffff,
          'cause': 'ordered_completion_deadline',
          'error_kind': 'TimeoutException',
          'ordered_pending': 54,
          'object_pending': 54,
          'reverse_confirmed': 0xfffffffe,
          'reverse_grant': 0xfffffffe,
          'reverse_buffered': 64,
          'head_op': 255,
          'head_seq': 0xfffffffe,
          'obj_accepted': 0xfffffffe,
          'obj_grant': 0xfffffffe,
          'obj_query': false,
          'query_write': false,
        },
      );
      final jsonl = (await recorder.snapshot()).jsonl;
      final record = _records(jsonl).single;
      expect(utf8.encode(jsonl).length, lessThanOrEqualTo(512));
      expect(record['fields_omitted'], isNull);
      expect(
        utf8.encode(jsonEncode({...record, 'seq': 0x7fffffffffffffff})).length +
            1,
        lessThanOrEqualTo(512),
      );
      expect(record['head_op'], 255);
      expect(record['head_seq'], 0xfffffffe);
      expect(record['obj_accepted'], 0xfffffffe);
      expect(record['obj_grant'], 0xfffffffe);
      expect(record['obj_query'], isFalse);
      expect(record['query_write'], isFalse);
      await recorder.close();
    },
  );

  test('unsafe keys and free text never enter persisted records', () async {
    final AppTraceRecorder recorder = await open();
    recorder.record(
      'ble.failure',
      fields: <String, Object?>{
        'device_id': 'secret-board-id',
        'fen': 'rnbqkbnr/pppppppp',
        'message': 'raw failure text with spaces',
        'kind': 'BleFailure',
        'generation': 4,
        'schema': 999,
        'event': 'forged.event',
      },
    );
    final String jsonl = (await recorder.snapshot()).jsonl;
    expect(jsonl, isNot(contains('secret-board-id')));
    expect(jsonl, isNot(contains('rnbqkbnr')));
    expect(jsonl, isNot(contains('raw failure text')));
    expect(jsonl, contains('"message":"redacted"'));
    expect(jsonl, contains('"kind":"BleFailure"'));
    expect(jsonl, contains('"schema":1'));
    expect(jsonl, contains('"event":"ble.failure"'));
    expect(jsonl, isNot(contains('forged.event')));
    await recorder.close();
  });

  test('corrupt and crash-truncated records do not hide valid peers', () async {
    AppTraceRecorder recorder = await open();
    recorder.record('app.valid');
    await recorder.flush();
    final File file = directory
        .listSync()
        .whereType<File>()
        .where((file) => file.path.endsWith('.jsonl'))
        .single;
    await recorder.close();
    await file.writeAsString('{truncated', mode: FileMode.append);

    recorder = await open();
    recorder.record('app.after_restart');
    final AppTraceSnapshot snapshot = await recorder.snapshot();
    expect(snapshot.available, isTrue);
    expect(_records(snapshot.jsonl).map((record) => record['event']), <String>[
      'app.valid',
      'app.after_restart',
    ]);
    await recorder.close();
  });

  test(
    'retrieval reads only the bounded tail of an oversized segment',
    () async {
      final AppTraceRecorder recorder = await open();
      recorder.record('app.valid_tail');
      final String valid = (await recorder.snapshot()).jsonl;
      final File file = directory
          .listSync()
          .whereType<File>()
          .where((file) => file.path.endsWith('.jsonl'))
          .single;
      await recorder.close();
      final String prefix = List<String>.generate(
        600,
        (int index) => jsonEncode(<String, Object?>{
          'schema': 1,
          'type': 'app_trace',
          'event': 'app.old_prefix',
          'ts': now.toIso8601String(),
          'mono_ms': index,
          'seq': index,
          'prefix': index,
        }),
      ).join('\n');
      await file.writeAsString('$prefix\n$valid');

      final AppTraceRecorder reader = await open();
      final List<Map<String, Object?>> bounded = _records(
        (await reader.snapshot()).jsonl,
      );
      expect(
        bounded.any((record) => record['event'] == 'app.valid_tail'),
        isTrue,
      );
      expect(
        bounded.where((record) => record['event'] == 'app.old_prefix').length,
        lessThan(600),
      );
      await reader.close();
    },
  );

  test('pending in-memory work is capped and reports dropped events', () async {
    final AppTraceRecorder recorder = await open();
    final int attempts = AppTraceRecorder.maxPendingWrites + 68;
    for (int index = 0; index < attempts; ++index) {
      recorder.record('test.burst', fields: <String, Object?>{'index': index});
    }
    final AppTraceSnapshot snapshot = await recorder.snapshot();
    expect(
      _records(snapshot.jsonl).length,
      lessThanOrEqualTo(AppTraceRecorder.maxPendingWrites),
    );
    expect(snapshot.droppedEvents, greaterThanOrEqualTo(68));
    await recorder.close();
  });

  test('live writes serialize behind wireless snapshot flushing', () async {
    final AppTraceRecorder recorder = await open();
    final List<Future<AppTraceSnapshot>> snapshots =
        <Future<AppTraceSnapshot>>[];
    for (int index = 0; index < 100; ++index) {
      recorder.record(
        'test.before_snapshot',
        fields: <String, Object?>{'index': index},
      );
      snapshots.add(recorder.snapshot());
      recorder.record(
        'test.after_snapshot',
        fields: <String, Object?>{'index': index},
      );
    }
    await recorder.flush();
    expect(
      (await Future.wait(snapshots)).every((item) => item.available),
      isTrue,
    );
    expect((await recorder.snapshot()).available, isTrue);
    await recorder.close();
  });

  test('wireless snapshots are single-flight', () async {
    final AppTraceRecorder recorder = await open();
    recorder.record('test.snapshot');
    final Future<AppTraceSnapshot> first = recorder.snapshot();
    final Future<AppTraceSnapshot> second = recorder.snapshot();
    expect(identical(first, second), isTrue);
    expect((await first).available, isTrue);
    await recorder.close();
  });

  test('disabled storage and debug response fail closed', () async {
    final AppTraceRecorder recorder = AppTraceRecorder.disabled(now: () => now);
    recorder.record('app.ignored');
    final Map<String, Object?> response = await appTraceDebugSnapshot(recorder);
    expect(response['available'], isFalse);
    expect(response['failure'], 'storage_unavailable');
    expect(response['jsonl'], '');
    await recorder.close();
  });

  test('debug retrieval has an outer deadline', () async {
    final Map<String, Object?> response = await appTraceDebugSnapshot(
      _HungSnapshot(),
      timeout: Duration.zero,
    );
    expect(response['available'], isFalse);
    expect(response['failure'], 'snapshot_failed');
  });
}

class _HungSnapshot implements AppTraceSnapshotSource {
  @override
  Future<AppTraceSnapshot> snapshot() => Completer<AppTraceSnapshot>().future;
}

List<Map<String, Object?>> _records(String jsonl) => jsonl
    .split('\n')
    .where((line) => line.isNotEmpty)
    .map((line) => Map<String, Object?>.from(jsonDecode(line) as Map))
    .toList();
