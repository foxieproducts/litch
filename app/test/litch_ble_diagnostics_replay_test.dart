import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_reactive_ble/src/connected_device_operation.dart';
import 'package:flutter_reactive_ble/src/debug_logger.dart';
import 'package:flutter_reactive_ble/src/device_connector.dart';
import 'package:flutter_reactive_ble/src/device_scanner.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
// The app deliberately vendors this transitive plugin and tests its real channel.
// ignore: depend_on_referenced_packages
import 'package:reactive_ble_mobile/reactive_ble_mobile.dart';
// ignore: depend_on_referenced_packages
import 'package:reactive_ble_mobile/src/generated/bledata.pb.dart' as pb;

const _boardA = '11111111-2222-3333-4444-555555555555';
const _boardB = '66666666-7777-8888-9999-AAAAAAAAAAAA';
const _lifetimeA = '12345678-1234-1234-1234-123456789012';
const _lifetimeB = 'ABCDEFAB-ABCD-ABCD-ABCD-ABCDEFABCDEF';
const _traceMetadata = <String>{
  'schema',
  'type',
  'event',
  'ts',
  'mono_ms',
  'seq',
};

void main() {
  setUp(() => debugDefaultTargetPlatformOverride = TargetPlatform.iOS);
  tearDown(() => debugDefaultTargetPlatformOverride = null);

  testWidgets(
    'native discovery timeout remains distinguishable from Dart admission and recovery',
    (tester) async {
      final replay = await _ChannelReplay.open(tester);
      try {
        final timedOut = replay.service.connect(_boardA);
        await _drain(tester);
        final first = replay.connects.single;
        expect(first.request.deviceId, _boardA);
        expect(first.request.timeoutInMs, 5000);
        expect(replay.connectionSinkActive, isTrue);

        // Native reports connecting after its ACK, before the method reply has
        // reached Dart. The production global listener already owns the sink;
        // the actual connector still waits for its method stream to complete.
        replay.diagnostics[_boardA] = <Map<String, Object?>>[
          _native(1, 'connect_requested'),
          _native(2, 'connect_ack'),
          _native(
            3,
            'connection_sent',
            fields: <String, Object?>{
              'state': DeviceConnectionState.connecting.index,
            },
          ),
          _native(
            4,
            'did_connect',
            fields: const <String, Object?>{'state': 2, 'task_match': true},
          ),
          _native(
            5,
            'services_requested',
            fields: const <String, Object?>{
              'all_services': true,
              'service_count': 0,
            },
          ),
        ];
        await replay.emit(_boardA, DeviceConnectionState.connecting);
        await _drain(tester);
        expect(
          replay.platformLogs.any(
            (line) => line.contains(
              'connectionState: DeviceConnectionState.connecting',
            ),
          ),
          isTrue,
          reason:
              'The real global listener must decode the early native event.',
        );
        first.reply.complete();
        await _drain(tester);
        bool firstSettled = false;
        timedOut.then((_) => firstSettled = true);
        await tester.pump(const Duration(seconds: 5));
        expect(firstSettled, isFalse);
        await tester.pump(const Duration(seconds: 1));
        expect(await _settle(tester, timedOut), isFalse);
        expect(replay.disconnects, <String>[_boardA]);
        expect(
          replay.connectionSinkActive,
          isTrue,
          reason:
              'The persistent production disconnection listener survives cancellation.',
        );

        final failedTrace = await replay.snapshot(tester);
        final timeout = _events(failedTrace, 'ble.connection_timeout').single;
        expect(timeout['last_state'], 'none');
        expect(timeout['mode'], 'direct');
        expect(
          _events(failedTrace, 'ble.connection_observed'),
          isEmpty,
          reason: 'Native delivery must not be relabeled as Dart admission.',
        );
        final failedNative = _nativeRecords(failedTrace);
        expect(failedNative.map((record) => record['phase']), <String>[
          'connect_requested',
          'connect_ack',
          'connection_sent',
          'did_connect',
          'services_requested',
        ]);
        expect(
          failedNative.any(
            (record) => record['phase'] == 'discovery_completed',
          ),
          isFalse,
        );

        final recovered = replay.service.connect(_boardA);
        await _drain(tester);
        replay.connects.last.reply.complete();
        await _drain(tester);
        replay.diagnostics[_boardA] = <Map<String, Object?>>[
          ...replay.diagnostics[_boardA]!,
          _native(6, 'connect_requested', attempt: 2),
          _native(
            7,
            'did_connect',
            attempt: 2,
            fields: const <String, Object?>{'state': 2, 'task_match': true},
          ),
          _native(
            8,
            'discovery_completed',
            attempt: 2,
            fields: const <String, Object?>{'state': 2, 'error_count': 0},
          ),
          _native(
            9,
            'connection_sent',
            attempt: 2,
            fields: <String, Object?>{
              'state': DeviceConnectionState.connected.index,
            },
          ),
        ];
        await replay.emit(_boardA, DeviceConnectionState.connected);
        expect(await _settle(tester, recovered), isTrue);
        expect(replay.service.isConnected, isTrue);

        replay.diagnostics[_boardA]!.addAll(<Map<String, Object?>>[
          _native(
            10,
            'did_disconnect',
            attempt: 2,
            fields: const <String, Object?>{
              'state': 0,
              'task_match': true,
              'has_error': false,
            },
          ),
          _native(
            11,
            'connection_sent',
            attempt: 2,
            fields: <String, Object?>{
              'state': DeviceConnectionState.disconnected.index,
            },
          ),
        ]);
        await replay.emit(_boardA, DeviceConnectionState.disconnected);
        await _drain(tester);
        expect(replay.service.isConnected, isFalse);
        final trace = await replay.snapshot(tester);
        final observed = _events(trace, 'ble.connection_observed');
        expect(observed.map((record) => record['state']), <String>[
          'connected',
          'disconnected',
        ]);
        expect(
          observed.every((record) => record['owns_reservation'] == true),
          isTrue,
        );
        expect(
          _events(
            trace,
            'ble.connection_state',
          ).map((record) => record['state']),
          <String>['connected', 'disconnected'],
        );
        expect(observed.first['generation'], isNot(timeout['generation']));
        final captured = _nativeRecords(trace);
        expect(
          captured,
          hasLength(11),
          reason: 'Overlapping captures must deduplicate records.',
        );
        expect(
          captured.singleWhere((record) => record['native_seq'] == 8)['phase'],
          'discovery_completed',
        );
        expect(
          captured.singleWhere((record) => record['native_seq'] == 10)['phase'],
          'did_disconnect',
        );
        expect(trace.jsonl, isNot(contains(_boardA)));
      } finally {
        await replay.close(tester);
      }
    },
  );

  testWidgets(
    'overlapping device-filtered snapshots retain unseen old identities across native lifetimes',
    (tester) async {
      final replay = await _ChannelReplay.open(tester);
      replay.holdSnapshots = true;
      try {
        await replay.failConnection(tester, _boardA);
        expect(replay.snapshots, hasLength(2));
        expect(
          replay.snapshots.every((request) => request.deviceId == _boardA),
          isTrue,
        );
        // The old snapshot was taken before its records left the native ring.
        // Its reply remains pending while the newer snapshot arrives first.
        replay.snapshots[1].reply.complete(<Map<String, Object?>>[
          _native(55, 'sink_listen'),
          _native(
            62,
            'discovery_completed',
            fields: const <String, Object?>{'error_count': 0},
          ),
        ]);
        await _drain(tester);

        await replay.failConnection(tester, _boardB);
        expect(replay.snapshots, hasLength(4));
        expect(replay.snapshots[2].deviceId, _boardB);
        expect(replay.snapshots[3].deviceId, _boardB);
        // B's sequence 45 is still in the 32-record ring, but A's filter did not expose it.
        replay.snapshots[2].reply.complete(<Map<String, Object?>>[
          _native(45, 'did_connect', attempt: 2),
          _native(55, 'sink_listen'),
          _native(63, 'did_disconnect', attempt: 2),
        ]);
        await _drain(tester);
        final deviceFiltered = await replay.snapshot(tester);
        expect(
          _events(deviceFiltered, 'ble.native_bootstrap').where(
            (record) =>
                record['native_lifetime'] == _lifetimeA &&
                record['native_seq'] == 45,
          ),
          hasLength(1),
          reason:
              'A newer A snapshot must not hide an unseen lower sequence for B.',
        );

        await replay.failConnection(tester, _boardA);
        expect(replay.snapshots, hasLength(6));
        final restarted = <Map<String, Object?>>[
          _native(1, 'sink_listen', lifetime: _lifetimeB),
          _native(2, 'connect_requested', lifetime: _lifetimeB),
        ];
        replay.snapshots[4].reply.complete(restarted);
        replay.snapshots[5].reply.complete(restarted);
        await _drain(tester);
        // Responses from the previous journal lifetime arrive after the new one.
        replay.snapshots[0].reply.complete(<Map<String, Object?>>[
          _native(1, 'sink_listen'),
          _native(2, 'did_connect'),
        ]);
        replay.snapshots[3].reply.complete(<Map<String, Object?>>[
          _native(45, 'did_connect', attempt: 2),
          _native(55, 'sink_listen'),
          _native(63, 'did_disconnect', attempt: 2),
        ]);
        await _drain(tester);

        final trace = await replay.snapshot(tester);
        final native = _nativeRecords(trace);
        expect(native, hasLength(8));
        expect(
          native
              .where((record) => record['native_lifetime'] == _lifetimeA)
              .map((record) => record['native_seq'])
              .toSet(),
          <int>{1, 2, 45, 55, 62, 63},
        );
        expect(
          native
              .where((record) => record['native_lifetime'] == _lifetimeB)
              .map((record) => record['native_seq'])
              .toSet(),
          <int>{1, 2},
        );
        expect(_events(trace, 'ble.native_diagnostics_unavailable'), isEmpty);
        expect(trace.droppedEvents, 0);
        expect(trace.jsonl, isNot(contains(_boardA)));
        expect(trace.jsonl, isNot(contains(_boardB)));
      } finally {
        await replay.close(tester);
      }
    },
  );

  testWidgets(
    'maximal 32-record native journal survives bounded disk trace and restart',
    (tester) async {
      final replay = await _ChannelReplay.open(tester);
      try {
        final scalars = <String, Object?>{
          for (int index = 0; index < 12; ++index)
            'metric_${index.toString().padLeft(2, '0')}_${'x' * 22}':
                1.7976931348623157e308,
        };
        expect(scalars.keys.every((key) => key.length == 32), isTrue);
        final journal = <Map<String, Object?>>[
          for (int seq = 1; seq <= 32; ++seq)
            _native(
              seq,
              'characteristics_callback',
              fields: scalars,
              attempt: 9223372036854775807,
              nativeMs: 1.7976931348623157e308,
              drops: 9223372036854775807,
            ),
        ];
        replay.diagnostics[_boardA] = journal;
        await replay.failConnection(tester, _boardA);
        final beforeRestart = await replay.snapshot(tester);
        expect(beforeRestart.droppedEvents, 0);
        expect(beforeRestart.jsonl, isNot(contains('fields_omitted')));

        // Read the actual fixed-segment files through a fresh recorder. Inspecting
        // just the method result or an in-memory trace sink misses persistence loss.
        await _settle(tester, replay.trace.close());
        final reader = await tester.runAsync(
          () => AppTraceRecorder.open(
            replay.directory,
            now: () => replay.now,
            monotonicMs: () => 0,
          ),
        );
        final persisted = await _settle(tester, reader!.snapshot());
        await _settle(tester, reader.close());
        expect(persisted.available, isTrue);
        final native = _nativeRecords(persisted);
        expect(native, hasLength(32));
        for (int index = 0; index < journal.length; ++index) {
          final expected = journal[index];
          final actual = native.singleWhere(
            (record) => record['native_seq'] == index + 1,
          );
          for (final entry in expected.entries) {
            expect(
              actual[entry.key == 'seq' ? 'native_seq' : entry.key],
              entry.value,
              reason:
                  'Native record ${index + 1} must preserve ${entry.key} on disk.',
            );
          }
          expect(actual['generation'], isA<int>());
        }
        final records = _records(persisted);
        expect(_events(persisted, 'ble.native_bootstrap_detail'), isNotEmpty);
        final missingFinalPart = AppTraceSnapshot(
          available: persisted.available,
          generatedAt: persisted.generatedAt,
          windowStart: persisted.windowStart,
          droppedEvents: persisted.droppedEvents,
          jsonl:
              '${records.where((record) => !(record['native_seq'] == 1 && record['last_part'] == true)).map(jsonEncode).join('\n')}\n',
        );
        expect(
          () => _nativeRecords(missingFinalPart),
          throwsA(isA<TestFailure>()),
          reason:
              'A controlled missing final part must not resemble a complete native record.',
        );
        for (final record in records) {
          expect(record['fields_omitted'], isNull);
          expect(
            record.keys.where((key) => !_traceMetadata.contains(key)).length,
            lessThanOrEqualTo(16),
          );
          expect(
            utf8.encode('${jsonEncode(record)}\n').length,
            lessThanOrEqualTo(AppTraceRecorder.maxRecordBytes),
          );
        }
        expect(
          records.length,
          lessThanOrEqualTo(AppTraceRecorder.maxPendingWrites),
        );
        final files = await tester.runAsync(
          () async => replay.directory.listSync().whereType<File>().toList(),
        );
        expect(files!.length, lessThanOrEqualTo(AppTraceRecorder.segmentCount));
        for (final file in files) {
          final length = await tester.runAsync(file.length);
          expect(length, lessThanOrEqualTo(AppTraceRecorder.maxSegmentBytes));
        }
        expect(persisted.jsonl, isNot(contains(_boardA)));
      } finally {
        await replay.close(tester);
      }
    },
  );

  testWidgets(
    'diagnostic read waits for existing initialization and cannot revive a retired native owner',
    (tester) async {
      final replay = await _ChannelReplay.open(
        tester,
        holdInitialization: true,
      );
      try {
        expect(replay.initializeCalls, 1);
        bool readCompleted = false;
        final pending = replay.ble
            .getConnectionDiagnostics(_boardA)
            .whenComplete(() => readCompleted = true);
        await _drain(tester);
        expect(
          readCompleted,
          isFalse,
          reason:
              'Diagnostics must await the existing native initialization reply.',
        );
        expect(replay.snapshots, isEmpty);
        expect(replay.initializeCalls, 1);

        await _settle(tester, replay.ble.deinitialize());
        replay.initializationReply.complete();
        expect(
          await _settle(tester, pending),
          isEmpty,
          reason:
              'A late initialization reply must not admit a diagnostic request after teardown.',
        );
        expect(replay.snapshots, isEmpty);
        expect(
          await _settle(tester, replay.ble.getConnectionDiagnostics(_boardA)),
          isEmpty,
        );
        expect(
          replay.initializeCalls,
          1,
          reason:
              'Reading a cold/deinitialized owner must not initialize native BLE.',
        );
        expect(replay.connectionSinkActive, isFalse);
      } finally {
        await replay.close(tester);
      }
    },
  );
}

Map<String, Object?> _native(
  int seq,
  String phase, {
  String lifetime = _lifetimeA,
  int attempt = 1,
  num? nativeMs,
  int drops = 0,
  Map<String, Object?> fields = const <String, Object?>{},
}) => <String, Object?>{
  'seq': seq,
  'native_ms': nativeMs ?? seq * 10.0,
  'native_lifetime': lifetime,
  'native_attempt': attempt,
  'phase': phase,
  ...fields,
  'drops': drops,
};

List<Map<String, Object?>> _records(AppTraceSnapshot snapshot) => snapshot.jsonl
    .split('\n')
    .where((line) => line.isNotEmpty)
    .map((line) => jsonDecode(line) as Map<String, Object?>)
    .toList();

List<Map<String, Object?>> _events(AppTraceSnapshot snapshot, String event) =>
    _records(snapshot).where((record) => record['event'] == event).toList();

List<Map<String, Object?>> _nativeRecords(AppTraceSnapshot snapshot) {
  final parts = <String, List<Map<String, Object?>>>{};
  for (final record in _records(snapshot)) {
    if (record['event'] != 'ble.native_bootstrap' &&
        record['event'] != 'ble.native_bootstrap_detail') {
      continue;
    }
    final key = '${record['native_lifetime']}/${record['native_seq']}';
    (parts[key] ??= <Map<String, Object?>>[]).add(record);
  }
  return parts.values.map((records) {
    // Accept old unsplit records as part 0 so old-fail evidence identifies data
    // loss or deduplication, rather than failing on new multipart notation.
    records.sort(
      (a, b) => (a['part'] as int? ?? 0).compareTo(b['part'] as int? ?? 0),
    );
    expect(
      records.map((record) => record['part'] ?? 0),
      List<int>.generate(records.length, (index) => index),
      reason:
          'Every native identity must have contiguous, nonduplicate persisted parts.',
    );
    expect(records.first['event'], 'ble.native_bootstrap');
    expect(
      records
          .skip(1)
          .every((record) => record['event'] == 'ble.native_bootstrap_detail'),
      isTrue,
    );
    if (records.any((record) => record['part'] != null)) {
      expect(records.every((record) => record['part'] is int), isTrue);
      expect(
        records.map((record) => record['last_part']),
        List<bool>.generate(
          records.length,
          (index) => index == records.length - 1,
        ),
        reason:
            'Only the final persisted part may mark a native record complete.',
      );
    }
    final merged = <String, Object?>{};
    for (final record in records) {
      for (final key in <String>[
        'generation',
        'native_seq',
        'native_lifetime',
      ]) {
        expect(record[key], records.first[key]);
      }
      merged.addAll(<String, Object?>{
        for (final entry in record.entries)
          if (!_traceMetadata.contains(entry.key) && entry.key != 'part')
            entry.key: entry.value,
      });
    }
    return merged;
  }).toList();
}

Future<void> _drain(WidgetTester tester) async {
  for (int pass = 0; pass < 8; ++pass) {
    await tester.pump(Duration.zero);
    await tester.runAsync(() async {});
  }
}

Future<T> _settle<T>(WidgetTester tester, Future<T> future) async {
  bool completed = false;
  final result = future.whenComplete(() => completed = true);
  // Timers remain on the widget's clock. Only SDK cancellation and disk I/O
  // yield to the outer scheduler, so there are no wall-clock connection waits.
  // A full multipart journal serializes many real file callbacks. Bound turns
  // by the fixed queue budget while leaving timeout ownership on the fake clock.
  for (
    int pass = 0;
    pass < AppTraceRecorder.maxPendingWrites * 16 && !completed;
    ++pass
  ) {
    await tester.pump(Duration.zero);
    await tester.runAsync(() => Future<void>(() {}));
  }
  expect(
    completed,
    isTrue,
    reason: 'Replay operation must settle within a bounded scheduler drain.',
  );
  return result;
}

final class _ConnectCall {
  _ConnectCall(this.request);
  final pb.ConnectToDeviceRequest request;
  final reply = Completer<void>();
}

final class _SnapshotCall {
  _SnapshotCall(this.deviceId);
  final String deviceId;
  final reply = Completer<List<Map<String, Object?>>>();
}

final class _ChannelReplay {
  _ChannelReplay(this.directory, this.trace, this.messenger);

  static const _method = MethodChannel('flutter_reactive_ble_method');
  static const _connection = 'flutter_reactive_ble_connected_device';
  static const _channels = <String>[
    _connection,
    'flutter_reactive_ble_char_update',
    'flutter_reactive_ble_scan',
    'flutter_reactive_ble_status',
  ];
  final Directory directory;
  final AppTraceRecorder trace;
  final TestDefaultBinaryMessenger messenger;
  final now = DateTime.utc(2026, 10, 12, 1);
  final connects = <_ConnectCall>[];
  final disconnects = <String>[];
  final snapshots = <_SnapshotCall>[];
  final diagnostics = <String, List<Map<String, Object?>>>{};
  final platformLogs = <String>[];
  late final FlutterReactiveBle ble;
  late final LitchBleService service;
  bool holdSnapshots = false;
  bool holdInitialization = false;
  int initializeCalls = 0;
  final initializationReply = Completer<void>();
  bool connectionSinkActive = false;

  static Future<_ChannelReplay> open(
    WidgetTester tester, {
    bool holdInitialization = false,
  }) async {
    final directory = await tester.runAsync(
      () => Directory.systemTemp.createTemp('litch_ble_diagnostic_replay_'),
    );
    final trace = await tester.runAsync(
      () => AppTraceRecorder.open(
        directory!,
        now: () => DateTime.utc(2026, 10, 12, 1),
        monotonicMs: () => 0,
      ),
    );
    final replay = _ChannelReplay(
      directory!,
      trace!,
      tester.binding.defaultBinaryMessenger,
    );
    replay.holdInitialization = holdInitialization;
    replay.messenger.setMockMethodCallHandler(_method, replay._handleMethod);
    for (final channel in _channels) {
      replay.messenger.setMockMethodCallHandler(MethodChannel(channel), (
        call,
      ) async {
        if (channel == _connection) {
          if (call.method == 'listen') replay.connectionSinkActive = true;
          if (call.method == 'cancel') replay.connectionSinkActive = false;
        }
        return null;
      });
    }
    final logger = DebugLogger(
      'REPLAY',
      (object) => replay.platformLogs.add(object.toString()),
    )..logLevel = LogLevel.verbose;
    final platform = const ReactiveBleMobilePlatformFactory().create(
      logger: logger,
    );
    final scanner = DeviceScannerImpl(
      blePlatform: platform,
      platformIsAndroid: () => false,
      delayAfterScanCompletion: Future<void>.value(),
      addToScanRegistry: (_) {},
    );
    final connector = DeviceConnectorImpl(
      blePlatform: platform,
      deviceScanner: scanner,
      deviceIsDiscoveredRecently:
          ({required deviceId, required cacheValidity}) => true,
      delayAfterScanFailure: Duration.zero,
    );
    replay.ble = FlutterReactiveBle.witDependencies(
      deviceScanner: scanner,
      deviceConnector: connector,
      connectedDeviceOperation: ConnectedDeviceOperationImpl(
        blePlatform: platform,
      ),
      debugLogger: logger,
      initialization: platform.initialize(),
      reactiveBlePlatform: platform,
    );
    replay.service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: replay.ble,
      gattSessionFactory: ReactiveBleGattSessionFactory(replay.ble),
      appTrace: replay.trace,
    );
    // Production installs this persistent listener on its first GATT resolution
    // and retains it through retries. Exercise that public hook, not a fake BLE
    // facade or an extra test-owned listener that keeps the native sink alive.
    await _settle(tester, replay.ble.getDiscoveredServices(_boardA));
    await _drain(tester);
    expect(replay.connectionSinkActive, isTrue);
    return replay;
  }

  Future<Object?> _handleMethod(MethodCall call) async {
    switch (call.method) {
      case 'initialize':
        ++initializeCalls;
        if (holdInitialization) await initializationReply.future;
        return null;
      case 'deinitialize':
      case 'readNotifications':
      case 'stopNotifications':
        return null;
      case 'connectToDevice':
        final request = _ConnectCall(
          pb.ConnectToDeviceRequest.fromBuffer(call.arguments as List<int>),
        );
        connects.add(request);
        await request.reply.future;
        return null;
      case 'disconnectFromDevice':
        disconnects.add(
          pb.DisconnectFromDeviceRequest.fromBuffer(
            call.arguments as List<int>,
          ).deviceId,
        );
        return null;
      case 'getConnectionDiagnostics':
        final deviceId = call.arguments as String;
        final request = _SnapshotCall(deviceId);
        snapshots.add(request);
        if (holdSnapshots) return request.reply.future;
        final records = diagnostics[deviceId] ?? const <Map<String, Object?>>[];
        return records
            .map((record) => Map<String, Object?>.of(record))
            .toList();
      case 'getDiscoveredServices':
        return _gattInventory(
          pb.DiscoverServicesRequest.fromBuffer(
            call.arguments as List<int>,
          ).deviceId,
        );
      case 'negotiateMtuSize':
        return pb.NegotiateMtuInfo(mtuSize: 247).writeToBuffer();
      case 'writeCharacteristicWithResponse':
      case 'writeCharacteristicWithoutResponse':
        final request = pb.WriteCharacteristicRequest.fromBuffer(
          call.arguments as List<int>,
        );
        return pb.WriteCharacteristicInfo(
          characteristic: request.characteristic,
        ).writeToBuffer();
      default:
        throw StateError('Unexpected BLE method ${call.method}');
    }
  }

  Future<void> emit(String deviceId, DeviceConnectionState state) async {
    expect(connectionSinkActive, isTrue);
    await messenger.handlePlatformMessage(
      _connection,
      const StandardMethodCodec().encodeSuccessEnvelope(
        pb.DeviceInfo(
          id: deviceId,
          connectionState: state.index,
        ).writeToBuffer(),
      ),
      null,
    );
  }

  Future<void> failConnection(WidgetTester tester, String deviceId) async {
    final connecting = service.connect(deviceId);
    await _drain(tester);
    connects.last.reply.complete();
    await _drain(tester);
    await emit(deviceId, DeviceConnectionState.disconnected);
    expect(await _settle(tester, connecting), isFalse);
    await _drain(tester);
  }

  Future<AppTraceSnapshot> snapshot(WidgetTester tester) =>
      _settle(tester, trace.snapshot());

  Future<void> close(WidgetTester tester) async {
    if (!initializationReply.isCompleted) initializationReply.complete();
    for (final request in connects) {
      if (!request.reply.isCompleted) request.reply.complete();
    }
    for (final request in snapshots) {
      if (!request.reply.isCompleted) {
        request.reply.complete(const <Map<String, Object?>>[]);
      }
    }
    await _drain(tester);
    await _settle(tester, service.dispose());
    await _settle(tester, ble.deinitialize());
    await messenger.handlePlatformMessage(
      'flutter_reactive_ble_status',
      null,
      null,
    );
    await _settle(tester, trace.close());
    messenger.setMockMethodCallHandler(_method, null);
    for (final channel in _channels) {
      messenger.setMockMethodCallHandler(MethodChannel(channel), null);
    }
    await tester.runAsync(() => directory.delete(recursive: true));
    // testWidgets verifies foundation globals before ordinary tearDown runs.
    debugDefaultTargetPlatformOverride = null;
  }
}

List<int> _gattInventory(String deviceId) => pb.DiscoverServicesInfo(
  deviceId: deviceId,
  services: <pb.DiscoveredService>[
    _gattService(BleProtocol.baseService, <Uuid>[
      BleProtocol.commandCharacteristic,
      BleProtocol.consoleStreamCharacteristic,
      BleProtocol.otaControlCharacteristic,
      BleProtocol.otaDataCharacteristic,
      BleProtocol.connectionSessionIngressCharacteristic,
      BleProtocol.connectionSessionEgressCharacteristic,
    ]),
    _gattService(BleProtocol.settingsService, <Uuid>[
      BleProtocol.settingsEnumerateCharacteristic,
      BleProtocol.settingsReadWriteCharacteristic,
    ]),
  ],
).writeToBuffer();

pb.DiscoveredService _gattService(Uuid id, List<Uuid> characteristics) =>
    pb.DiscoveredService(
      serviceUuid: pb.Uuid(data: id.data),
      serviceInstanceId: '0',
      characteristicUuids: characteristics.map((id) => pb.Uuid(data: id.data)),
      characteristics: characteristics.map(
        (characteristic) => pb.DiscoveredCharacteristic(
          serviceId: pb.Uuid(data: id.data),
          characteristicId: pb.Uuid(data: characteristic.data),
          characteristicInstanceId: '0',
          isReadable: true,
          isWritableWithResponse: true,
          isWritableWithoutResponse: true,
          isNotifiable: true,
        ),
      ),
    );
