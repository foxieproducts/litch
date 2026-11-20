import 'dart:async';
import 'dart:convert';
import 'dart:io';

/// One process owns one production EngineBackedJourney. No board state lives
/// in Dart; this client transports raw bytes, physical input and explicit time.
class VirtualLitchProcess {
  // The same wrapper accepts a controlled OS child for failure-path tests.
  VirtualLitchProcess.fromProcess(this._process, {
    Duration requestTimeout = const Duration(seconds: 15),
    Duration shutdownTimeout = const Duration(seconds: 10),
    Duration terminateTimeout = const Duration(seconds: 1),
  }) : _requestTimeout = requestTimeout,
       _shutdownTimeout = shutdownTimeout,
       _terminateTimeout = terminateTimeout {
    _process.stdout.transform(utf8.decoder).transform(const LineSplitter()).listen(
      (String line) {
        final response = jsonDecode(line) as Map<String, dynamic>;
        _requests.remove(response['id'])?.complete(response);
      },
      onError: _fail,
      onDone: () => _fail(StateError('VirtualLitch stdout closed')),
    );
    // Observe asynchronous pipe failures before a later request writes again.
    unawaited(_process.stdin.done.then<void>((_) {}, onError: _fail));
    _process.stderr.transform(utf8.decoder).listen((String chunk) {
      stderr.write(chunk);
    });
  }

  final Process _process;
  final Duration _requestTimeout;
  final Duration _shutdownTimeout;
  final Duration _terminateTimeout;
  final Map<int, Completer<Map<String, dynamic>>> _requests = {};
  int _nextId = 1;
  Future<void>? _closing;
  Object? _failure;

  int get pendingRequestCount => _requests.length;
  Future<int> get exitCode => _process.exitCode;

  static Future<VirtualLitchProcess> start(int firstPlayer, bool rotate, {bool hardwareV3 = false}) async {
    final String? executable = Platform.environment['LITCH_VIRTUAL_BRIDGE'];
    if (executable == null) {
      throw StateError('Run python3 tools/virtual_litch/coaching_journey.py');
    }
    return VirtualLitchProcess.fromProcess(await Process.start(
      executable, <String>['$firstPlayer', rotate ? '1' : '0', if (hardwareV3) 'v3'],
    ));
  }

  Future<Map<String, dynamic>> request(Map<String, Object?> operation) async {
    if (_closing != null) throw StateError('VirtualLitch is closing');
    if (_failure != null) throw _failure!;
    final int id = _nextId++;
    final completer = Completer<Map<String, dynamic>>();
    _requests[id] = completer;
    try {
      _process.stdin.writeln(jsonEncode(<String, Object?>{...operation, 'id': id}));
      final response = await completer.future.timeout(_requestTimeout);
      if (response['ok'] != true) {
        throw StateError(
          'VirtualLitch ${operation['op']} '
          'channel=${operation['channel']} bytes=${(operation['bytes'] as List?)?.length} '
          'failed: ${response['error']}',
        );
      }
      return response;
    } finally {
      _requests.remove(id);
    }
  }

  void _fail(Object error) {
    _failure ??= error;
    for (final request in _requests.values) {
      request.completeError(error);
    }
    _requests.clear();
  }

  Future<void> close() => _closing ??= _close();

  Future<void> _close() async {
    _fail(StateError('VirtualLitch is closing'));
    try {
      await _process.stdin.close().timeout(_shutdownTimeout);
    } finally {
      // Reap even when closing stdin fails. SIGTERM gets one bounded grace
      // period; an unresponsive child is killed and awaited, never abandoned.
      try {
        final int exit = await _process.exitCode.timeout(_shutdownTimeout);
        if (exit != 0) throw StateError('VirtualLitch exited $exit');
      } on TimeoutException {
        _process.kill();
        try {
          await _process.exitCode.timeout(_terminateTimeout);
        } on TimeoutException {
          _process.kill(ProcessSignal.sigkill);
          await _process.exitCode;
        }
        throw TimeoutException('VirtualLitch shutdown timed out; child reaped',
          _shutdownTimeout);
      }
    }
  }
}
