import 'dart:async';
import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:io';

import 'package:flutter/foundation.dart';
import '../models/owner_pairing_session.dart';
import '../models/support_action.dart';
export '../models/support_action.dart';

typedef AppTraceClock = DateTime Function();
typedef AppTraceMonotonicClock = int Function();

enum AppConsolePreparationPhase {
  disconnected,
  connecting,
  discovering,
  confirmingOwner,
  failed,
  ready,
}

/// Bounded current-link facts for the local Console observation only.
final class AppConsoleReadiness {
  const AppConsoleReadiness({
    required this.phase,
    required this.productReady,
    required this.sessionActivated,
    this.owner,
  });

  final AppConsolePreparationPhase phase;
  final bool productReady, sessionActivated;
  final OwnerPairingSessionStatus? owner;

  Map<String, Object?> toJson() => {
    'phase': phase.name,
    'product_ready': productReady,
    'session_activated': sessionActivated,
    'owner_state': owner?.state.name,
    'owner_reason': owner?.reason.name,
    'owner_flags': owner?.flags,
    'owner_attempt': owner?.attempt,
  };
}

abstract interface class AppTraceSink {
  void record(String event, {Map<String, Object?> fields = const {}});
}

/// Local-only latest Console observation. This intentionally retains identifiers
/// and complete text; it is separate from the privacy-filtered lifecycle ring.
abstract interface class AppConsoleSnapshotSink {
  void captureConsole({
    required List<String> lines,
    required int connectionGeneration,
    required bool connected,
    String? connectedBoardId,
    String? historyBoardId,
    AppConsoleReadiness? readiness,
  });
}

final class _ConsoleSnapshotWriter implements AppConsoleSnapshotSink {
  _ConsoleSnapshotWriter(this.directory, this.now)
    : processStartedAt = now().toUtc().toIso8601String();

  static const fileName = 'console_snapshot.json';
  static const maximumBytes = 320 * 1024;
  static const maximumLines = 120;
  // Firmware lines are 2047 bytes plus a small timestamp prefix. Retain those
  // exactly, while bounding an unexpected unbounded host-side error string.
  static const maximumLineCharacters = 4096;
  static const interval = Duration(seconds: 1);
  final Future<Directory?> directory;
  final AppTraceClock now;
  final String processStartedAt;
  final Stopwatch elapsed = Stopwatch()..start();
  Timer? timer;
  Completer<void>? completion;
  Map<String, Object?>? pending;
  List<String> previousLines = const [];
  (int, bool, String?)? previousConnection;
  String? connectionObservedAt, linesObservedAt;
  int revision = 0, writtenRevision = 0, lastStarted = -1000;
  bool writing = false, closed = false, failed = false;

  @override
  void captureConsole({
    required List<String> lines,
    required int connectionGeneration,
    required bool connected,
    String? connectedBoardId,
    String? historyBoardId,
    AppConsoleReadiness? readiness,
  }) {
    if (closed || failed) return;
    final capturedAt = now().toUtc().toIso8601String();
    final connection = (connectionGeneration, connected, connectedBoardId);
    if (connection != previousConnection) connectionObservedAt = capturedAt;
    previousConnection = connection;
    final retained = lines
        .skip((lines.length - maximumLines).clamp(0, lines.length))
        .toList(growable: false);
    if (!listEquals(retained, previousLines)) linesObservedAt = capturedAt;
    previousLines = retained;
    pending = {
      'schema': 1,
      'process_id': pid,
      'process_started_at': processStartedAt,
      'connection_generation': connectionGeneration,
      'connected': connected,
      'connected_board_id': connectedBoardId,
      'readiness': readiness?.toJson(),
      'history_board_id': historyBoardId,
      'history_is_cached':
          historyBoardId != null &&
          (!connected || historyBoardId != connectedBoardId),
      'captured_at': capturedAt,
      'connection_observed_at': connectionObservedAt,
      'lines_observed_at': linesObservedAt,
      'source_line_count': lines.length,
      'lines': retained,
      'revision': ++revision,
    };
    _schedule();
  }

  void _schedule() {
    if (writing || timer != null || pending == null || closed || failed) return;
    completion = Completer<void>();
    final waitMs =
        (interval.inMilliseconds - (elapsed.elapsedMilliseconds - lastStarted))
            .clamp(0, interval.inMilliseconds);
    timer = Timer(Duration(milliseconds: waitMs), () => unawaited(_write()));
  }

  Future<void> _write() async {
    timer = null;
    final value = pending!;
    pending = null;
    writing = true;
    lastStarted = elapsed.elapsedMilliseconds;
    final finished = completion!;
    File? temporary;
    try {
      final resolvedDirectory = await directory;
      if (resolvedDirectory == null) {
        throw const FileSystemException('Console storage unavailable');
      }
      temporary = File('${resolvedDirectory.path}/$fileName.tmp');
      final json = _encode(value);
      await temporary.writeAsString(json, flush: true);
      await temporary.rename('${resolvedDirectory.path}/$fileName');
      writtenRevision = value['revision']! as int;
    } catch (_) {
      failed = true;
      pending = null;
      try {
        if (temporary != null && await temporary.exists()) {
          await temporary.delete();
        }
      } catch (_) {}
    } finally {
      writing = false;
      completion = null;
      finished.complete();
      _schedule();
    }
  }

  String _encode(Map<String, Object?> value) {
    final lines = value['lines']! as List<String>;
    final sourceCount = value['source_line_count']! as int;
    final metadata = {...value}..remove('lines');
    metadata['written_at'] = now().toUtc().toIso8601String();
    // Reserve the largest omission metadata before selecting the exact tail.
    metadata['omitted_lines'] = sourceCount;
    metadata['omission_reason'] = 'byte_budget_and_source_bounds';
    var remaining =
        maximumBytes - utf8.encode(jsonEncode(metadata)).length - 12;
    if (remaining < 0) throw StateError('Console metadata exceeds bound');
    var sourceBound = sourceCount > lines.length, byteBound = false;
    final selected = <String>[];
    for (final line in lines.reversed) {
      if (line.length > maximumLineCharacters) {
        sourceBound = true;
        continue;
      }
      final encoded = jsonEncode(line);
      final bytes = utf8.encode(encoded).length + (selected.isEmpty ? 0 : 1);
      if (bytes > remaining) {
        byteBound = true;
        break;
      }
      selected.add(encoded);
      remaining -= bytes;
    }
    metadata['omitted_lines'] = sourceCount - selected.length;
    metadata['omission_reason'] = byteBound
        ? (sourceBound ? 'byte_budget_and_source_bounds' : 'byte_budget')
        : sourceBound
        ? 'source_bounds'
        : null;
    final prefix = jsonEncode(metadata);
    return '${prefix.substring(0, prefix.length - 1)},"lines":[${selected.reversed.join(',')}]}\n';
  }

  Future<void> flush() async {
    final target = revision;
    while (!failed && !closed && writtenRevision < target) {
      final next = completion;
      if (next == null) break;
      await next.future;
    }
  }

  Future<void> close() async {
    closed = true;
    timer?.cancel();
    timer = null;
    pending = null;
    final active = completion;
    if (writing) {
      await active?.future;
    } else {
      completion = null;
      active?.complete();
    }
  }
}

abstract interface class AppTraceSnapshotSource {
  Future<AppTraceSnapshot> snapshot();
}

final class AppTraceSnapshot {
  const AppTraceSnapshot({
    required this.available,
    required this.generatedAt,
    required this.windowStart,
    required this.droppedEvents,
    required this.jsonl,
    this.failure,
  });

  final bool available;
  final DateTime generatedAt;
  final DateTime windowStart;
  final int droppedEvents;
  final String jsonl;
  final String? failure;

  Map<String, Object?> toJson() => <String, Object?>{
    'schema': 1,
    'available': available,
    'generated_at': generatedAt.toUtc().toIso8601String(),
    'window_start': windowStart.toUtc().toIso8601String(),
    'dropped_events': droppedEvents,
    'failure': failure,
    'jsonl': jsonl,
  };
}

/// A small disk-backed diagnostic ring. Seven fixed files cap retained storage
/// at 448 KiB and cover a full hour across ten-minute bucket boundaries. At
/// most 256 encoded records wait for asynchronous file I/O, so a stalled disk
/// cannot grow RAM.
final class AppTraceRecorder
    implements
        AppTraceSink,
        AppTraceSnapshotSource,
        AppConsoleSnapshotSink,
        AppUserActionSink {
  AppTraceRecorder._(this._directory, this._now, this._monotonicMs);

  static const Duration retention = Duration(hours: 1);
  static const Duration segmentDuration = Duration(minutes: 10);
  static const int segmentCount = 7;
  static const int maxSegmentBytes = 64 * 1024;
  static const int maxRecordBytes = 512;
  // A 32-record native journal may need bounded continuation records beside
  // ordinary app events. Encoded pending storage remains capped at 128 KiB.
  static const int maxPendingWrites = 256;
  static const int maxSnapshotBytes = segmentCount * maxSegmentBytes;
  static const int schema = 1;

  static final RegExp _safeName = RegExp(r'^[a-z][a-z0-9_.]{0,63}$');
  static final RegExp _safeToken = RegExp(r'^[A-Za-z0-9_.:-]{0,64}$');
  static const Set<String> _forbiddenKeyParts = <String>{
    'address',
    'artwork',
    'board_id',
    'color',
    'credential',
    'device_id',
    'error_text',
    'fen',
    'move',
    'payload',
    'peer',
    'secret',
    'token',
  };
  static const Set<String> _reservedKeys = <String>{
    'schema',
    'type',
    'event',
    'ts',
    'mono_ms',
    'seq',
  };

  /// Starts with one bounded queue and clock before storage is ready. Callers
  /// can build the UI immediately; early records keep their original timestamps.
  static AppTraceRecorder start({
    required Future<Directory> Function() directory,
    AppTraceClock? now,
    AppTraceMonotonicClock? monotonicMs,
  }) {
    final stopwatch = Stopwatch()..start();
    final recorder = AppTraceRecorder._(
      Directory(''),
      now ?? DateTime.now,
      monotonicMs ?? (() => stopwatch.elapsedMilliseconds),
    );
    recorder._consoleEnabled = true;
    // Capture starts now too: the one latest bounded observation can replace
    // this empty process boundary while directory discovery is still pending.
    recorder._console.captureConsole(
      lines: const [],
      connectionGeneration: 0,
      connected: false,
    );
    recorder._storageReady = recorder._initialize(directory);
    recorder._tail = recorder._storageReady;
    return recorder;
  }

  static Future<AppTraceRecorder> open(
    Directory directory, {
    AppTraceClock? now,
    AppTraceMonotonicClock? monotonicMs,
  }) async {
    final recorder = start(
      directory: () async => directory,
      now: now,
      monotonicMs: monotonicMs,
    );
    await recorder._storageReady;
    return recorder;
  }

  Future<void> _initialize(Future<Directory> Function() directory) async {
    try {
      _directory = await directory();
      await _directory.create(recursive: true);
      _consoleDirectory.complete(_directory);
      await _console.flush();
    } catch (_) {
      _disable('storage_unavailable');
      if (!_consoleDirectory.isCompleted) _consoleDirectory.complete(null);
      _consoleEnabled = false;
    }
  }

  factory AppTraceRecorder.disabled({AppTraceClock? now}) {
    final Stopwatch stopwatch = Stopwatch()..start();
    final AppTraceRecorder recorder = AppTraceRecorder._(
      Directory(''),
      now ?? DateTime.now,
      () => stopwatch.elapsedMilliseconds,
    );
    recorder._disable('storage_unavailable');
    return recorder;
  }

  final Completer<Directory?> _consoleDirectory = Completer<Directory?>();
  bool _consoleEnabled = false;
  late final _ConsoleSnapshotWriter _console = _ConsoleSnapshotWriter(
    _consoleDirectory.future,
    _now,
  );
  @override
  void captureConsole({
    required List<String> lines,
    required int connectionGeneration,
    required bool connected,
    String? connectedBoardId,
    String? historyBoardId,
    AppConsoleReadiness? readiness,
  }) {
    if (!_consoleEnabled || _closed || _closing) return;
    _console.captureConsole(
      lines: lines,
      connectionGeneration: connectionGeneration,
      connected: connected,
      connectedBoardId: connectedBoardId,
      historyBoardId: historyBoardId,
      readiness: readiness,
    );
  }

  /// Waits for the current Console observation without bypassing its throttle.
  /// Privacy-ring reads and shutdown do not wait for a scheduled observation.
  @visibleForTesting
  Future<void> waitForConsoleSnapshotPublication() => _console.flush();

  Directory _directory;
  Future<void> _storageReady = Future<void>.value();
  final AppTraceClock _now;
  final AppTraceMonotonicClock _monotonicMs;
  Future<void> _tail = Future<void>.value();
  RandomAccessFile? _activeFile;
  int? _activeBucket;
  int _activeBytes = 0;
  int _pendingWrites = 0;
  int _sequence = 0;
  int _droppedEvents = 0;
  bool _available = true;
  bool _closing = false;
  bool _closed = false;
  String? _failure;
  Future<AppTraceSnapshot>? _snapshotInFlight;

  @override
  late final SupportActionHistory userActions = SupportActionHistory(
    _monotonicMs,
  );
  @override
  int get supportMonotonicMs => _monotonicMs();

  bool get available => _available && !_closing && !_closed;
  int get droppedEvents => _droppedEvents;

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    if (!available) return;
    if (!_safeName.hasMatch(event) || _pendingWrites >= maxPendingWrites) {
      _droppedEvents++;
      return;
    }
    final DateTime timestamp = _now().toUtc();
    final int bucket =
        timestamp.millisecondsSinceEpoch ~/ segmentDuration.inMilliseconds;
    final Map<String, Object?> safeFields = _sanitize(fields);
    Map<String, Object?> record = <String, Object?>{
      'schema': schema,
      'type': 'app_trace',
      'event': event,
      'ts': timestamp.toIso8601String(),
      'mono_ms': _monotonicMs(),
      'seq': ++_sequence,
      ...safeFields,
    };
    String line = '${jsonEncode(record)}\n';
    if (utf8.encode(line).length > maxRecordBytes) {
      record = <String, Object?>{
        'schema': schema,
        'type': 'app_trace',
        'event': event,
        'ts': timestamp.toIso8601String(),
        'mono_ms': _monotonicMs(),
        'seq': _sequence,
        'fields_omitted': true,
      };
      line = '${jsonEncode(record)}\n';
    }
    _enqueueWrite(bucket, line);
  }

  Map<String, Object?> _sanitize(Map<String, Object?> fields) {
    final Map<String, Object?> result = <String, Object?>{};
    for (final MapEntry<String, Object?> entry in fields.entries.take(16)) {
      final String key = entry.key;
      if (!_safeName.hasMatch(key) ||
          _reservedKeys.contains(key) ||
          _forbiddenKeyParts.any(key.contains)) {
        continue;
      }
      final Object? value = entry.value;
      if (value == null || value is bool || value is int) {
        result[key] = value;
      } else if (value is double && value.isFinite) {
        result[key] = value;
      } else if (value is String) {
        result[key] = _safeToken.hasMatch(value) ? value : 'redacted';
      }
    }
    return result;
  }

  void _enqueueWrite(int bucket, String line) {
    _pendingWrites++;
    final Future<void> previous = _tail;
    _tail = () async {
      await previous;
      try {
        await _write(bucket, line);
      } catch (_) {
        _disable('write_failed');
      } finally {
        _pendingWrites--;
      }
    }();
  }

  Future<void> _write(int bucket, String line) async {
    if (!_available || _closed) return;
    if (_activeBucket != bucket) await _openBucket(bucket);
    final int length = utf8.encode(line).length;
    if (_activeBytes + length > maxSegmentBytes) {
      _droppedEvents++;
      return;
    }
    final RandomAccessFile? file = _activeFile;
    if (file == null) return;
    await file.writeString(line);
    _activeBytes += length;
  }

  Future<void> _openBucket(int bucket) async {
    await _activeFile?.flush();
    await _activeFile?.close();
    _activeFile = null;
    _activeBucket = null;
    _activeBytes = 0;

    final int slot = bucket % segmentCount;
    final File file = File('${_directory.path}/app_trace_$slot.jsonl');
    bool append = false;
    int appendLength = 0;
    if (await file.exists()) {
      RandomAccessFile? reader;
      try {
        reader = await file.open();
        final int fileLength = await reader.length();
        final int prefixLength = fileLength.clamp(0, 160);
        final List<int> prefix = await reader.read(prefixLength);
        final String firstLine = utf8
            .decode(prefix, allowMalformed: true)
            .split('\n')
            .first;
        final Object? decoded = jsonDecode(firstLine);
        append =
            decoded is Map<String, Object?> &&
            decoded['type'] == 'app_trace_segment' &&
            decoded['bucket'] == bucket &&
            fileLength < maxSegmentBytes;
        if (append) {
          final int tailLength = fileLength.clamp(0, maxRecordBytes);
          final int tailStart = fileLength - tailLength;
          await reader.setPosition(tailStart);
          final List<int> tail = await reader.read(tailLength);
          final int newline = tail.lastIndexOf(10);
          if (newline < 0) {
            append = false;
          } else {
            appendLength = tailStart + newline + 1;
          }
        }
      } catch (_) {
        append = false;
      } finally {
        await reader?.close();
      }
    }
    final RandomAccessFile opened = await file.open(
      mode: append ? FileMode.append : FileMode.write,
    );
    if (append) {
      // Discard only a crash-truncated final record. The 512-byte record cap
      // bounds this backwards repair without loading the segment.
      await opened.truncate(appendLength);
      await opened.setPosition(appendLength);
    } else {
      final String header =
          '${jsonEncode(<String, Object?>{'schema': schema, 'type': 'app_trace_segment', 'bucket': bucket})}\n';
      await opened.writeString(header);
    }
    _activeFile = opened;
    _activeBucket = bucket;
    _activeBytes = await opened.length();
  }

  Future<void> flush() async {
    final Future<void> previous = _tail;
    final Future<void> barrier = () async {
      await previous;
      if (!_available || _closed) return;
      try {
        await _activeFile?.flush();
      } catch (_) {
        _disable('flush_failed');
      }
    }();
    // A record arriving while flush is in progress queues behind this barrier,
    // preserving RandomAccessFile's single-async-operation contract.
    _tail = barrier;
    await barrier;
  }

  @override
  Future<AppTraceSnapshot> snapshot() {
    final Future<AppTraceSnapshot>? existing = _snapshotInFlight;
    if (existing != null) return existing;
    late final Future<AppTraceSnapshot> snapshot;
    snapshot = _createSnapshot().whenComplete(() {
      if (identical(_snapshotInFlight, snapshot)) _snapshotInFlight = null;
    });
    _snapshotInFlight = snapshot;
    return snapshot;
  }

  Future<AppTraceSnapshot> _createSnapshot() async {
    await flush();
    final DateTime generatedAt = _now().toUtc();
    final DateTime windowStart = generatedAt.subtract(retention);
    if (!_available) {
      return AppTraceSnapshot(
        available: false,
        generatedAt: generatedAt,
        windowStart: windowStart,
        droppedEvents: _droppedEvents,
        jsonl: '',
        failure: _failure,
      );
    }
    final List<_SnapshotLine> retained = <_SnapshotLine>[];
    int order = 0;
    try {
      for (int slot = 0; slot < segmentCount; ++slot) {
        final File file = File('${_directory.path}/app_trace_$slot.jsonl');
        if (!await file.exists()) continue;
        final List<String> lines = await _readBoundedLines(file);
        for (final String line in lines) {
          if (line.isEmpty) continue;
          try {
            final Object? decoded = jsonDecode(line);
            if (decoded is! Map<String, Object?> ||
                decoded['type'] != 'app_trace' ||
                decoded['schema'] != schema) {
              continue;
            }
            final DateTime? timestamp = DateTime.tryParse(
              decoded['ts']?.toString() ?? '',
            )?.toUtc();
            if (timestamp == null ||
                timestamp.isBefore(windowStart) ||
                timestamp.isAfter(generatedAt)) {
              continue;
            }
            retained.add(
              _SnapshotLine(timestamp, order++, jsonEncode(decoded)),
            );
          } catch (_) {
            // A crash-truncated or corrupt record does not hide valid peers.
          }
        }
      }
    } catch (_) {
      return AppTraceSnapshot(
        available: false,
        generatedAt: generatedAt,
        windowStart: windowStart,
        droppedEvents: _droppedEvents,
        jsonl: '',
        failure: 'read_failed',
      );
    }
    retained.sort((a, b) {
      final int time = a.timestamp.compareTo(b.timestamp);
      return time != 0 ? time : a.order.compareTo(b.order);
    });
    final StringBuffer output = StringBuffer();
    int bytes = 0;
    for (final _SnapshotLine line in retained.reversed) {
      final int lineBytes = utf8.encode('${line.json}\n').length;
      if (bytes + lineBytes > maxSnapshotBytes) break;
      output.write('${line.json}\n');
      bytes += lineBytes;
    }
    final String jsonl = output
        .toString()
        .split('\n')
        .where((line) => line.isNotEmpty)
        .toList()
        .reversed
        .map((line) => '$line\n')
        .join();
    return AppTraceSnapshot(
      available: true,
      generatedAt: generatedAt,
      windowStart: windowStart,
      droppedEvents: _droppedEvents,
      jsonl: jsonl,
    );
  }

  Future<List<String>> _readBoundedLines(File file) async {
    final RandomAccessFile reader = await file.open();
    try {
      final int length = await reader.length();
      final int readLength = length.clamp(0, maxSegmentBytes);
      final int start = length - readLength;
      await reader.setPosition(start);
      List<int> bytes = await reader.read(readLength);
      if (start > 0) {
        final int firstNewline = bytes.indexOf(10);
        if (firstNewline < 0) return const <String>[];
        bytes = bytes.sublist(firstNewline + 1);
      }
      return const LineSplitter().convert(
        utf8.decode(bytes, allowMalformed: true),
      );
    } finally {
      await reader.close();
    }
  }

  Future<void> close() async {
    if (_closed || _closing) return;
    _closing = true;
    await flush();
    await _console.close();
    final Future<void> previous = _tail;
    final Future<void> barrier = () async {
      await previous;
      try {
        await _activeFile?.close();
      } finally {
        _activeFile = null;
        _closed = true;
        _closing = false;
      }
    }();
    _tail = barrier;
    await barrier;
  }

  void _disable(String failure) {
    _available = false;
    _failure ??= failure;
  }
}

final class _SnapshotLine {
  const _SnapshotLine(this.timestamp, this.order, this.json);

  final DateTime timestamp;
  final int order;
  final String json;
}

Future<Map<String, Object?>> appTraceDebugSnapshot(
  AppTraceSnapshotSource source, {
  Duration timeout = const Duration(seconds: 3),
}) async {
  try {
    return (await source.snapshot().timeout(timeout)).toJson();
  } catch (_) {
    final DateTime now = DateTime.now().toUtc();
    return AppTraceSnapshot(
      available: false,
      generatedAt: now,
      windowStart: now.subtract(AppTraceRecorder.retention),
      droppedEvents: 0,
      jsonl: '',
      failure: 'snapshot_failed',
    ).toJson();
  }
}

void registerAppTraceServiceExtension(AppTraceSnapshotSource source) {
  // Internal debug and profile builds already expose Flutter's authenticated
  // VM service. Reuse it for bounded trace retrieval; never add an app-owned
  // listener or expose diagnostics from a release build.
  if (kReleaseMode) return;
  developer.registerExtension(
    'ext.litch.trace.snapshot',
    (String _, Map<String, String> _) async =>
        developer.ServiceExtensionResponse.result(
          jsonEncode(await appTraceDebugSnapshot(source)),
        ),
  );
}

// Narrow optional typed projection: test/developer sinks that only implement
// AppTraceSink remain valid, and arbitrary lifecycle events are never promoted
// into the user-action inventory.
extension SupportUserActions on AppTraceSink? {
  AppUserActionSink? get supportActions =>
      this is AppUserActionSink ? this as AppUserActionSink : null;
  int beginUserAction(SupportActionKind kind, {int count = 0}) =>
      supportActions?.userActions.begin(kind, count: count) ?? 0;
  void finishUserAction(
    int action,
    SupportActionOutcome outcome, {
    int count = 0,
  }) => supportActions?.userActions.finish(action, outcome, count: count);
}
