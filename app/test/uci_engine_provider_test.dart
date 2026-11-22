import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/engine/chess_engine_models.dart';
import 'package:litch_mobile/src/engine/uci_engine_provider.dart';
import 'package:litch_mobile/src/engine/uci_transport.dart';

void main() {
  test('handshakes once and maps bounded MultiPV scores to White', () async {
    final transport = _FakeUciTransport();
    final provider = UciEngineProvider(
      transport: transport,
      providerId: 'stockfish-local',
      engineVersion: 'test',
    );
    transport.onCommand = (command) {
      if (command == 'uci') transport.emit('uciok');
      if (command == 'isready') transport.emit('readyok');
      if (command.startsWith('go ')) {
        transport
          ..emit(
            'info depth 12 nodes 400 time 20 multipv 1 score cp -34 pv e7e5 g1f3',
          )
          ..emit(
            'info depth 11 nodes 300 time 18 multipv 2 score mate -3 pv c7c5',
          )
          ..emit('bestmove e7e5 ponder g1f3');
      }
    };

    final events = await provider
        .search(_request(multiPv: 2), EngineCancellation())
        .toList();

    expect(events.map((event) => event.runtimeType), <Type>[
      EngineSearchStarted,
      EngineSearchInfo,
      EngineSearchInfo,
      EngineBestMove,
      EngineSearchCompleted,
    ]);
    final info = events.whereType<EngineSearchInfo>().toList();
    expect(info[0].score.value, 34);
    expect(info[1].score.mateIn, 3);
    expect(info.map((value) => value.multiPv), <int>[1, 2]);
    expect(transport.commands.where((line) => line == 'uci'), hasLength(1));
    await provider.close();
  });

  test(
    'consumer cancellation sends stop and drains the terminal bestmove',
    () async {
      final transport = _FakeUciTransport();
      final provider = UciEngineProvider(
        transport: transport,
        providerId: 'stockfish-local',
        engineVersion: 'test',
      );
      transport.onCommand = (command) {
        if (command == 'uci') transport.emit('uciok');
        if (command == 'isready') transport.emit('readyok');
        if (command == 'stop') transport.emit('bestmove e2e4');
      };
      final subscription = provider
          .search(_request(), EngineCancellation())
          .listen((_) {});
      await pumpEventQueue();

      await subscription.cancel();

      expect(transport.commands, contains('stop'));
      await provider.close();
    },
  );

  test('search output flood fails closed without another command', () async {
    final transport = _FakeUciTransport();
    final provider = UciEngineProvider(
      transport: transport,
      providerId: 'stockfish-local',
      engineVersion: 'test',
    );
    transport.onCommand = (command) {
      if (command == 'uci') transport.emit('uciok');
      if (command == 'isready') transport.emit('readyok');
      if (command.startsWith('go ')) {
        for (var index = 0; index <= 4096; index++) {
          transport.emit('string ignored $index');
        }
      }
    };

    final events = await provider
        .search(_request(), EngineCancellation())
        .toList();

    expect(
      events.whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.invalidResponse,
    );
    final commandCount = transport.commands.length;
    final retryEvents = await provider
        .search(_request(), EngineCancellation())
        .toList();
    expect(
      retryEvents.whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.unavailable,
    );
    expect(transport.commands, hasLength(commandCount));
    await provider.close();
  });

  test('close is bounded when an active engine ignores stop', () async {
    final transport = _FakeUciTransport();
    final provider = UciEngineProvider(
      transport: transport,
      providerId: 'stockfish-local',
      engineVersion: 'test',
    );
    transport.onCommand = (command) {
      if (command == 'uci') transport.emit('uciok');
      if (command == 'isready') transport.emit('readyok');
    };
    provider.search(_request(), EngineCancellation()).listen((_) {});
    await pumpEventQueue();

    await provider.close().timeout(const Duration(seconds: 1));

    expect(transport.commands, contains('stop'));
  });

  test(
    'transport EOF during handshake surfaces as transport failure',
    () async {
      final transport = _FakeUciTransport();
      final provider = UciEngineProvider(
        transport: transport,
        providerId: 'stockfish-local',
        engineVersion: 'test',
      );
      transport.onCommand = (command) {
        if (command == 'uci') unawaited(transport.endOutput());
      };

      final events = await provider
          .search(_request(), EngineCancellation())
          .toList();

      expect(
        events.whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.transport,
      );
      await provider.close();
    },
  );

  test('hung handshake is bounded and permanently retires transport', () async {
    final transport = _FakeUciTransport();
    final provider = UciEngineProvider(
      transport: transport,
      providerId: 'stockfish-local',
      engineVersion: 'test',
      handshakeTimeout: const Duration(milliseconds: 100),
    );

    final events = await provider
        .search(_request(), EngineCancellation())
        .toList()
        .timeout(const Duration(seconds: 1));
    expect(
      events.whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.transport,
    );
    final commandCount = transport.commands.length;
    final retry = await provider
        .search(_request(), EngineCancellation())
        .toList();
    expect(
      retry.whereType<EngineSearchFailed>().single.reason,
      EngineFailureReason.unavailable,
    );
    expect(transport.commands, hasLength(commandCount));
    await provider.close();
  });

  test('accepts terminal no-move output without quarantining', () async {
    final transport = _FakeUciTransport();
    final provider = UciEngineProvider(
      transport: transport,
      providerId: 'stockfish-local',
      engineVersion: 'test',
    );
    var searches = 0;
    transport.onCommand = (command) {
      if (command == 'uci') transport.emit('uciok');
      if (command == 'isready') transport.emit('readyok');
      if (command.startsWith('go ')) {
        transport.emit(searches++ == 0 ? 'bestmove 0000' : 'bestmove e7e5');
      }
    };

    final terminal = await provider
        .search(
          _request(fen: '7k/6Q1/6K1/8/8/8/8/8 b - - 0 1'),
          EngineCancellation(),
        )
        .toList();
    expect(terminal.whereType<EngineNoLegalMove>(), hasLength(1));
    expect(terminal.last, isA<EngineSearchCompleted>());
    final next = await provider
        .search(_request(), EngineCancellation())
        .toList();
    expect(next.whereType<EngineBestMove>().single.uci, 'e7e5');
    await provider.close();
  });

  test('malformed bestmove and oversized lines fail closed', () async {
    for (final output in <String>['bestmove e2e9', 'x' * 4097]) {
      final transport = _FakeUciTransport();
      final provider = UciEngineProvider(
        transport: transport,
        providerId: 'stockfish-local',
        engineVersion: 'test',
      );
      transport.onCommand = (command) {
        if (command == 'uci') transport.emit('uciok');
        if (command == 'isready') transport.emit('readyok');
        if (command.startsWith('go ')) transport.emit(output);
      };

      final events = await provider
          .search(_request(), EngineCancellation())
          .toList();

      expect(
        events.whereType<EngineSearchFailed>().single.reason,
        EngineFailureReason.invalidResponse,
      );
      await provider.close();
    }
  });
}

EngineSearchRequest _request({
  int multiPv = 1,
  String fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1',
}) => EngineSearchRequest(
  jobId: 'job',
  sessionId: 4,
  positionRevision: 8,
  fen: fen,
  variant: ChessEngineVariant.classic,
  purpose: EngineSearchPurpose.postGameAnalysis,
  limits: EngineSearchLimits(
    moveTime: const Duration(milliseconds: 250),
    multiPv: multiPv,
  ),
);

class _FakeUciTransport implements UciTransport {
  final StreamController<List<int>> _output =
      StreamController<List<int>>.broadcast(sync: true);
  final List<String> commands = <String>[];
  void Function(String command)? onCommand;

  @override
  Stream<List<int>> get lines => _output.stream;

  @override
  Future<void> start() async {}

  @override
  Future<void> send(String command) async {
    commands.add(command);
    onCommand?.call(command);
  }

  void emit(String line) => _output.add(line.codeUnits);

  Future<void> endOutput() => _output.close();

  @override
  Future<void> close() async {
    if (!_output.isClosed) await _output.close();
  }
}
