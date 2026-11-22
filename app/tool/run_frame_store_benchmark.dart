import 'dart:async';
import 'dart:convert';
import 'dart:io';

Future<void> main(List<String> arguments) async {
  if ((arguments.length != 1 && arguments.length != 2) ||
      !arguments.first.startsWith('ws://') ||
      (arguments.length == 2 &&
          !const <String>{
            'repeated',
            'sparse',
            'dense',
          }.contains(arguments[1]))) {
    stderr.writeln(
      'usage: dart run tool/run_frame_store_benchmark.dart '
      '<vm-service-ws-uri> [repeated|sparse|dense]',
    );
    exitCode = 64;
    return;
  }

  final socket = await WebSocket.connect(arguments.first);
  final pending = <int, Completer<Map<String, Object?>>>{};
  var nextId = 0;
  late final StreamSubscription<Object?> subscription;
  subscription = socket.listen((message) {
    final decoded = jsonDecode(message as String);
    if (decoded is! Map<String, Object?> || decoded['id'] is! int) return;
    pending.remove(decoded['id'])?.complete(decoded);
  });

  Future<Map<String, Object?>> call(
    String method, [
    Map<String, Object?> parameters = const <String, Object?>{},
  ]) async {
    final id = ++nextId;
    final response = Completer<Map<String, Object?>>();
    pending[id] = response;
    socket.add(
      jsonEncode(<String, Object?>{
        'jsonrpc': '2.0',
        'id': id,
        'method': method,
        'params': parameters,
      }),
    );
    final envelope = await response.future.timeout(const Duration(minutes: 3));
    if (envelope['error'] case final Object error) throw StateError('$error');
    return (envelope['result'] as Map).cast<String, Object?>();
  }

  try {
    final vm = await call('getVM');
    final isolates = vm['isolates'] as List<Object?>;
    final isolate = (isolates.single as Map).cast<String, Object?>();
    final result = await call(
      'ext.litch.litchcraft.frameStore256',
      <String, Object?>{
        'isolateId': isolate['id'],
        if (arguments.length == 2) 'corpus': arguments[1],
      },
    );
    stdout.writeln(const JsonEncoder.withIndent('  ').convert(result));
  } finally {
    for (final completion in pending.values) {
      if (!completion.isCompleted) {
        completion.completeError(StateError('VM service disconnected'));
      }
    }
    await subscription.cancel();
    await socket.close();
  }
}
