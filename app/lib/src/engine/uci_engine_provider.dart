import 'dart:async';
import 'dart:convert';

import 'chess_engine_models.dart';
import 'chess_engine_provider.dart';
import 'uci_transport.dart';

const int _maximumLineBytes = 4096;
const int _maximumSearchLines = 4096;
const int _maximumHandshakeLines = 256;
final RegExp _uciMove = RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$');

/// Adapts one long-lived local UCI session to the bounded engine-provider port.
/// The router remains the app-wide search authority; this class owns process
/// ordering, handshake, stop/drain, and UCI parsing only.
class UciEngineProvider implements ChessEngineProvider {
  UciEngineProvider({
    required UciTransport transport,
    required String providerId,
    required String engineVersion,
    int maximumMultiPv = 3,
    this.handshakeTimeout = const Duration(seconds: 2),
  }) : _transport = transport,
       capabilities = EngineCapabilities(
         providerId: providerId,
         engineVersion: engineVersion,
         privacyClass: EnginePrivacyClass.onDevice,
         variants: const <ChessEngineVariant>{ChessEngineVariant.classic},
         purposes: EngineSearchPurpose.values.toSet(),
         supportsNodes: true,
         supportsDepth: true,
         maximumMultiPv: maximumMultiPv,
       ) {
    if (handshakeTimeout < const Duration(milliseconds: 100) ||
        handshakeTimeout > const Duration(seconds: 10)) {
      throw ArgumentError.value(handshakeTimeout, 'handshakeTimeout');
    }
  }

  final UciTransport _transport;
  final Duration handshakeTimeout;

  @override
  final EngineCapabilities capabilities;

  StreamSubscription<List<int>>? _transportSubscription;
  Completer<void>? _uciOk;
  Completer<void>? _readyOk;
  Future<void>? _ready;
  _UciJob? _active;
  int _handshakeLines = 0;
  bool _closed = false;
  bool _unavailable = false;

  @override
  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  ) {
    late final _UciJob job;
    job = _UciJob(
      request,
      cancellation,
      onListen: () => unawaited(_begin(job)),
      onCancel: () => _cancel(job),
    );
    return job.controller.stream;
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    final active = _active;
    if (active != null && !active.terminal) {
      active.consumerGone = true;
      active.cancellation.cancel();
      if (active.goIssued && !active.stopSent) {
        active.stopSent = true;
        try {
          await _send('stop');
        } on Object {
          _unavailable = true;
        }
      }
      _finish(active);
    }
    try {
      if (!_unavailable) await _send('quit');
    } on Object {
      _unavailable = true;
    }
    await _transportSubscription?.cancel();
    await _transport.close();
  }

  Future<void> _begin(_UciJob job) async {
    if (_closed || _unavailable) {
      _fail(job, EngineFailureReason.unavailable);
      return;
    }
    if (_active != null) {
      _fail(job, EngineFailureReason.busy);
      return;
    }
    try {
      await (_ready ??= _handshake().timeout(handshakeTimeout));
      if (job.consumerGone || job.cancellation.isCancelled) {
        _finish(job);
        return;
      }
      if (_active != null) {
        _fail(job, EngineFailureReason.busy);
        return;
      }
      _active = job;
      await _send('setoption name MultiPV value ${job.request.limits.multiPv}');
      await _send('position fen ${job.request.fen}');
      job.goIssued = true;
      if (!job.consumerGone) {
        job.controller.add(_started(job.request));
      }
      await _send(_go(job.request));
    } on Object {
      _unavailable = true;
      _fail(job, EngineFailureReason.transport);
      unawaited(Future<void>.microtask(_transport.close));
    }
  }

  Future<void> _handshake() async {
    _transportSubscription ??= _transport.lines.listen(
      _onLine,
      onError: (Object error, StackTrace stackTrace) => _transportFailed(),
      onDone: _transportFailed,
      cancelOnError: false,
    );
    await _transport.start();
    _uciOk = Completer<void>();
    await Future.wait<void>(<Future<void>>[
      Future<void>.microtask(() => _send('uci')),
      _uciOk!.future,
    ]);
    _readyOk = Completer<void>();
    await Future.wait<void>(<Future<void>>[
      Future<void>.microtask(() => _send('isready')),
      _readyOk!.future,
    ]);
  }

  void _onLine(List<int> raw) {
    if (!_validLine(raw)) {
      final active = _active;
      if (active != null) {
        _rejectResponse(active);
      } else {
        _transportFailed();
      }
      return;
    }
    final line = String.fromCharCodes(raw).trim();
    if (line.isEmpty) return;
    if (_uciOk != null && !_uciOk!.isCompleted) {
      if (++_handshakeLines > _maximumHandshakeLines) {
        _transportFailed();
      } else if (line == 'uciok') {
        _uciOk!.complete();
      }
      return;
    }
    if (_readyOk != null && !_readyOk!.isCompleted) {
      if (++_handshakeLines > _maximumHandshakeLines) {
        _transportFailed();
      } else if (line == 'readyok') {
        _readyOk!.complete();
      }
      return;
    }
    final job = _active;
    if (job == null || job.terminal) return;
    if (++job.lines > _maximumSearchLines) {
      _rejectResponse(job);
      return;
    }
    if (line.startsWith('info ')) {
      final info = _parseInfo(job.request, line);
      if (info != null && !job.consumerGone) job.controller.add(info);
      return;
    }
    if (line.startsWith('bestmove ')) {
      if (_isNoLegalMove(line)) {
        if (!job.consumerGone) {
          job.controller
            ..add(_noLegalMove(job.request))
            ..add(_completed(job.request));
        }
        _finish(job);
        return;
      }
      final best = _parseBestMove(job.request, line);
      if (best == null) {
        _rejectResponse(job);
        return;
      }
      if (!job.consumerGone) {
        job.controller
          ..add(best)
          ..add(_completed(job.request));
      }
      _finish(job);
    }
  }

  Future<void> _cancel(_UciJob job) async {
    if (job.terminal) return;
    job.consumerGone = true;
    job.cancellation.cancel();
    if (!job.goIssued) {
      _finish(job);
      return;
    }
    if (!job.stopSent) {
      job.stopSent = true;
      try {
        await _send('stop');
      } on Object catch (error, stackTrace) {
        _unavailable = true;
        _finish(job, error: error, stackTrace: stackTrace);
      }
    }
    return job.finished.future;
  }

  void _transportFailed() {
    if (_unavailable) return;
    _unavailable = true;
    final error = StateError('UCI transport closed.');
    if (_uciOk != null && !_uciOk!.isCompleted) {
      _uciOk!.completeError(error);
    }
    if (_readyOk != null && !_readyOk!.isCompleted) {
      _readyOk!.completeError(error);
    }
    final active = _active;
    if (active != null && !active.terminal) {
      if (!active.consumerGone) active.controller.addError(error);
      _finish(active);
    }
  }

  void _fail(_UciJob job, EngineFailureReason reason) {
    if (job.terminal) return;
    if (!job.consumerGone) {
      job.controller.add(
        EngineSearchFailed(
          jobId: job.request.jobId,
          sessionId: job.request.sessionId,
          positionRevision: job.request.positionRevision,
          reason: reason,
        ),
      );
    }
    _finish(job);
  }

  void _rejectResponse(_UciJob job) {
    // Once framing or terminal output is invalid, output boundaries are no
    // longer trustworthy. Retiring this transport is cheaper and safer than
    // allowing an old bestmove to satisfy a later job.
    _unavailable = true;
    _fail(job, EngineFailureReason.invalidResponse);
    unawaited(Future<void>.microtask(_transport.close));
  }

  void _finish(_UciJob job, {Object? error, StackTrace? stackTrace}) {
    if (job.terminal) return;
    job.terminal = true;
    if (identical(_active, job)) _active = null;
    if (error != null) {
      job.finished.completeError(error, stackTrace);
    } else {
      job.finished.complete();
    }
    if (!job.controller.isClosed) unawaited(job.controller.close());
  }

  Future<void> _send(String command) {
    if (!_validCommand(command)) {
      throw const FormatException('Invalid UCI command.');
    }
    return _transport.send(command);
  }
}

class _UciJob {
  _UciJob(
    this.request,
    this.cancellation, {
    required void Function() onListen,
    required Future<void> Function() onCancel,
  }) : controller = StreamController<EngineSearchEvent>(
         onListen: onListen,
         onCancel: onCancel,
       );

  final EngineSearchRequest request;
  final EngineCancellation cancellation;
  final StreamController<EngineSearchEvent> controller;
  final Completer<void> finished = Completer<void>();
  int lines = 0;
  bool goIssued = false;
  bool stopSent = false;
  bool consumerGone = false;
  bool terminal = false;
}

String _go(EngineSearchRequest request) {
  final parts = <String>['go'];
  final time = request.timeControl;
  if (time != null) {
    parts.addAll(<String>[
      'wtime',
      '${time.whiteRemaining.inMilliseconds}',
      'btime',
      '${time.blackRemaining.inMilliseconds}',
      'winc',
      '${time.whiteIncrement.inMilliseconds}',
      'binc',
      '${time.blackIncrement.inMilliseconds}',
    ]);
  }
  final limits = request.limits;
  if (limits.moveTime != null) {
    parts.addAll(<String>['movetime', '${limits.moveTime!.inMilliseconds}']);
  }
  if (limits.nodes != null) parts.addAll(<String>['nodes', '${limits.nodes}']);
  if (limits.depth != null) parts.addAll(<String>['depth', '${limits.depth}']);
  return parts.join(' ');
}

EngineSearchStarted _started(EngineSearchRequest request) =>
    EngineSearchStarted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );

EngineSearchCompleted _completed(EngineSearchRequest request) =>
    EngineSearchCompleted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );

EngineNoLegalMove _noLegalMove(EngineSearchRequest request) =>
    EngineNoLegalMove(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );

EngineSearchInfo? _parseInfo(EngineSearchRequest request, String line) {
  final tokens = line.split(RegExp(r'\s+'));
  int? depth;
  int? nodes;
  int? timeMs;
  int multiPv = 1;
  EngineScore? score;
  List<String>? pv;
  var index = 1;
  while (index < tokens.length) {
    final key = tokens[index++];
    if (key == 'pv') {
      pv = tokens.sublist(index);
      break;
    }
    if (key == 'lowerbound' || key == 'upperbound') continue;
    if (index >= tokens.length) break;
    if (key == 'depth') {
      depth = int.tryParse(tokens[index++]);
    } else if (key == 'nodes') {
      nodes = int.tryParse(tokens[index++]);
    } else if (key == 'time') {
      timeMs = int.tryParse(tokens[index++]);
    } else if (key == 'multipv') {
      multiPv = int.tryParse(tokens[index++]) ?? 0;
    } else if (key == 'score') {
      if (index + 1 >= tokens.length) return null;
      final kind = tokens[index++];
      final raw = int.tryParse(tokens[index++]);
      if (raw == null) return null;
      final whiteScore = _whiteToMove(request.fen) ? raw : -raw;
      try {
        score = kind == 'cp'
            ? EngineScore.centipawns(whiteScore)
            : kind == 'mate'
            ? EngineScore.mate(whiteScore)
            : null;
      } on ArgumentError {
        return null;
      }
    }
  }
  if (depth == null ||
      nodes == null ||
      timeMs == null ||
      score == null ||
      pv == null ||
      depth < 0 ||
      depth > 256 ||
      nodes < 0 ||
      nodes > 1000000000 ||
      timeMs < 0 ||
      timeMs > const Duration(minutes: 10).inMilliseconds ||
      multiPv < 1 ||
      multiPv > request.limits.multiPv ||
      pv.length > 16 ||
      pv.any((move) => !_uciMove.hasMatch(move))) {
    return null;
  }
  return EngineSearchInfo(
    jobId: request.jobId,
    sessionId: request.sessionId,
    positionRevision: request.positionRevision,
    depth: depth,
    nodes: nodes,
    elapsed: Duration(milliseconds: timeMs),
    score: score,
    principalVariation: pv,
    multiPv: multiPv,
  );
}

EngineBestMove? _parseBestMove(EngineSearchRequest request, String line) {
  final tokens = line.split(RegExp(r'\s+'));
  if (tokens.length != 2 && !(tokens.length == 4 && tokens[2] == 'ponder')) {
    return null;
  }
  final move = tokens[1];
  final ponder = tokens.length == 4 ? tokens[3] : null;
  if (!_uciMove.hasMatch(move) ||
      (ponder != null && !_uciMove.hasMatch(ponder))) {
    return null;
  }
  return EngineBestMove(
    jobId: request.jobId,
    sessionId: request.sessionId,
    positionRevision: request.positionRevision,
    uci: move,
    ponder: ponder,
  );
}

bool _whiteToMove(String fen) => fen.split(' ')[1] == 'w';

bool _isNoLegalMove(String line) =>
    line == 'bestmove 0000' || line == 'bestmove (none)';

bool _validLine(List<int> line) =>
    line.length <= _maximumLineBytes &&
    line.every((unit) => unit == 0x09 || (unit >= 0x20 && unit <= 0x7e));

bool _validCommand(String command) =>
    command.isNotEmpty &&
    utf8.encode(command).length <= _maximumLineBytes &&
    command.codeUnits.every((unit) => unit >= 0x20 && unit <= 0x7e);
