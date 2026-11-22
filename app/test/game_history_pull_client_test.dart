import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_history_pull_client.dart';
import 'package:litch_mobile/src/services/game_history_segment_importer.dart';
import 'package:litch_mobile/src/services/game_history_sync_state_store.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const String _gameId = '000102030405060708090a0b0c0d0e0f';

void main() {
  late Directory root;
  late GameLibraryRepository repository;
  late GameHistorySyncStateStore stateStore;
  late GameHistoryDurableSegmentProcessor processor;
  late _FakeTransport transport;

  setUp(() async {
    root = await Directory.systemTemp.createTemp('litch-pull-client-');
    repository = GameLibraryRepository(
      GameLibraryStore(Directory('${root.path}/library')),
    );
    stateStore = GameHistorySyncStateStore(Directory('${root.path}/cursors'));
    processor = GameHistoryDurableSegmentProcessor(
      importer: GameHistorySegmentImporter(repository),
      stateStore: stateStore,
    );
    transport = _FakeTransport();
  });

  tearDown(() async {
    await transport.close();
    await repository.close();
    if (await root.exists()) await root.delete(recursive: true);
  });

  test(
    'subscribes first and ACKs only after archive and cursor durability',
    () async {
      final _Segment segment = _openingSegment(firstSequence: 1);
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        expect(
          GameHistoryBeginPayload.decode(begin.payload).journalId,
          '0000000000000000',
        );
        fake
          ..emit(_state(begin.transactionId, 1, oldest: 1))
          ..emit(_bounds(begin.transactionId, 2, newest: 2))
          ..emit(
            GameHistoryFrame(
              opcode: GameHistoryOpcode.segmentBegin.wireValue,
              transactionId: begin.transactionId,
              frameSequence: 3,
              payload: segment.descriptor.encode(),
            ),
          );
        int sequence = 4;
        for (int offset = 0; offset < segment.bytes.length; offset += 12) {
          final int end = offset + 12 < segment.bytes.length
              ? offset + 12
              : segment.bytes.length;
          fake.emit(
            GameHistoryFrame(
              opcode: GameHistoryOpcode.data.wireValue,
              transactionId: begin.transactionId,
              frameSequence: sequence++,
              payload: Uint8List.sublistView(segment.bytes, offset, end),
            ),
          );
        }
      };
      transport
          .onCommit = (_FakeTransport fake, GameHistoryFrame commit) async {
        expect((await repository.load()).games.single.currentPly, 1);
        expect((await stateStore.load('paired-board')).cursor?.durableAck, 2);
        fake.emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.committed.wireValue,
            transactionId: commit.transactionId,
            frameSequence: commit.frameSequence + 1,
            payload: commit.payload,
          ),
        );
      };
      final GameHistoryPullClient client = _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
      );

      final GameHistorySyncResult result = await client.pull();

      expect(transport.events.take(2), <String>['subscribe', 'write:begin']);
      expect(result.segmentCount, 1);
      expect(result.durableAck, 2);
      expect(result.encodedBytes, segment.bytes.length);
      expect(transport.events.last, 'unsubscribe');
    },
  );

  test(
    'refuses history before subscribing when owner-ready is false',
    () async {
      final GameHistoryPullClient client = _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
        isOwnerReady: () => false,
      );

      await expectLater(client.pull(), throwsStateError);

      expect(transport.events, isEmpty);
      expect(transport.writes, isEmpty);
    },
  );

  test(
    'up-to-date reconnect succeeds while acknowledged records remain',
    () async {
      await stateStore.save(
        'paired-board',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 2,
        ),
      );
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        fake.emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.state.wireValue,
            flags:
                gameHistoryStateFlagJournalMatched |
                gameHistoryStateFlagCursorReconciled |
                gameHistoryStateFlagNoRecords,
            transactionId: begin.transactionId,
            frameSequence: 1,
            payload: const GameHistoryStatePayload(
              journalId: '0102030405060708',
              oldestRetained: 1,
            ).encode(),
          ),
        );
        fake.emit(_bounds(begin.transactionId, 2, newest: 2, ack: 2));
      };
      final GameHistorySyncResult result = await _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
      ).pull();
      expect(result.durableAck, 2);
      expect(result.segmentCount, 0);
      expect(result.gapObserved, isFalse);
      expect(transport.writes, hasLength(1));
      expect(transport.events.last, 'unsubscribe');
    },
  );

  test(
    'cancel during cursor persistence prevents the pruning commit',
    () async {
      final _PausedCursorStore paused = _PausedCursorStore(
        stateStore.directory,
      );
      final GameHistoryDurableSegmentProcessor realProcessor =
          GameHistoryDurableSegmentProcessor(
            importer: GameHistorySegmentImporter(repository),
            stateStore: paused,
          );
      final _Segment segment = _openingSegment(firstSequence: 1);
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        fake
          ..emit(_state(begin.transactionId, 1, oldest: 1))
          ..emit(_bounds(begin.transactionId, 2, newest: 2))
          ..emit(
            GameHistoryFrame(
              opcode: GameHistoryOpcode.segmentBegin.wireValue,
              transactionId: begin.transactionId,
              frameSequence: 3,
              payload: segment.descriptor.encode(),
            ),
          )
          ..emit(
            GameHistoryFrame(
              opcode: GameHistoryOpcode.data.wireValue,
              transactionId: begin.transactionId,
              frameSequence: 4,
              payload: segment.bytes,
            ),
          );
      };
      final GameHistoryPullClient client = _client(
        transport: transport,
        stateStore: paused,
        processor: realProcessor,
      );
      final Future<void> failed = expectLater(client.pull(), throwsStateError);
      await paused.saved.future;
      await client.cancel();
      paused.release.complete();
      await failed;
      expect((await repository.load()).games, hasLength(1));
      expect((await paused.load('paired-board')).cursor?.durableAck, 2);
      expect(
        transport.writes.map((GameHistoryFrame frame) => frame.knownOpcode),
        isNot(contains(GameHistoryOpcode.commit)),
      );
    },
  );

  test('stale notifications cannot extend the progress deadline', () async {
    Duration elapsed = Duration.zero;
    int boundsSeen = 0;
    transport.onDeliver = (GameHistoryFrame frame) {
      if (frame.knownOpcode == GameHistoryOpcode.stateBounds &&
          ++boundsSeen == 2) {
        elapsed = const Duration(seconds: 2);
      }
    };
    final _Segment segment = _openingSegment(firstSequence: 1);
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
      fake
        ..emit(_state(begin.transactionId, 1, oldest: 1))
        ..emit(_bounds(begin.transactionId, 2, newest: 2))
        ..emit(_bounds(begin.transactionId, 2, newest: 2))
        ..emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.segmentBegin.wireValue,
            transactionId: begin.transactionId,
            frameSequence: 3,
            payload: segment.descriptor.encode(),
          ),
        )
        ..emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.data.wireValue,
            transactionId: begin.transactionId,
            frameSequence: 4,
            payload: segment.bytes,
          ),
        );
    };
    transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) async {
      fake.emit(
        GameHistoryFrame(
          opcode: GameHistoryOpcode.committed.wireValue,
          transactionId: commit.transactionId,
          frameSequence: commit.frameSequence + 1,
          payload: commit.payload,
        ),
      );
    };
    final GameHistoryPullClient client = GameHistoryPullClient(
      boardIdentity: 'paired-board',
      transport: transport,
      stateStore: stateStore,
      processor: processor,
      isOwnerReady: () => true,
      nextTransactionId: () => 0x1234,
      elapsed: () => elapsed,
      inactivityTimeout: const Duration(seconds: 1),
    );
    await expectLater(client.pull(), throwsA(isA<TimeoutException>()));
    expect(transport.writes, hasLength(1));
    expect((await repository.load()).games, isEmpty);
  });

  for (final bool subscribeFailure in <bool>[true, false]) {
    test(
      'failed ${subscribeFailure ? 'subscribe' : 'cleanup'} releases client for reuse',
      () async {
        transport.failSubscribe = subscribeFailure;
        transport.failUnsubscribe = !subscribeFailure;
        transport.onBegin =
            (_FakeTransport fake, GameHistoryFrame begin) async {
              fake
                ..emit(_state(begin.transactionId, 1, oldest: 0))
                ..emit(_bounds(begin.transactionId, 2, newest: 0));
            };
        final GameHistoryPullClient client = _client(
          transport: transport,
          stateStore: stateStore,
          processor: processor,
        );
        await expectLater(client.pull(), throwsStateError);
        expect(transport.events.last, 'unsubscribe');
        expect((await client.pull()).segmentCount, 0);
        expect(
          transport.events.where((String value) => value == 'unsubscribe'),
          hasLength(2),
        );
      },
    );
  }

  test(
    'notification failure during BEGIN write is handled and cleaned up',
    () async {
      final Completer<void> entered = Completer<void>();
      final Completer<void> release = Completer<void>();
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        entered.complete();
        await release.future;
      };
      final GameHistoryPullClient client = _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
      );
      final Future<void> failed = expectLater(client.pull(), throwsStateError);
      await entered.future;
      transport.emitError(StateError('notification setup failed'));
      await Future<void>.value();
      release.complete();
      await failed;
      expect(transport.events.last, 'unsubscribe');
      expect(transport.writes, hasLength(1));
    },
  );

  for (final String contradiction in <String>[
    'ack',
    'noRecords',
    'missingBase',
  ]) {
    test('rejects contradictory history state: $contradiction', () async {
      await stateStore.save(
        'paired-board',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 2,
        ),
      );
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        fake.emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.state.wireValue,
            flags:
                gameHistoryStateFlagJournalMatched |
                gameHistoryStateFlagCursorReconciled |
                (contradiction == 'missingBase'
                    ? gameHistoryStateFlagSegmentFollows
                    : gameHistoryStateFlagNoRecords),
            transactionId: begin.transactionId,
            frameSequence: 1,
            payload: GameHistoryStatePayload(
              journalId: '0102030405060708',
              oldestRetained: contradiction == 'missingBase'
                  ? 0
                  : contradiction == 'noRecords'
                  ? 3
                  : 1,
            ).encode(),
          ),
        );
        fake.emit(
          _bounds(
            begin.transactionId,
            2,
            newest: 3,
            ack: contradiction == 'ack' ? 1 : 2,
          ),
        );
      };
      await expectLater(
        _client(
          transport: transport,
          stateStore: stateStore,
          processor: processor,
        ).pull(),
        throwsFormatException,
      );
      expect(transport.writes, hasLength(1));
      expect((await stateStore.load('paired-board')).cursor?.durableAck, 2);
    });
  }

  test(
    'valid stale STATE flags are ignored before the next expected frame',
    () async {
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        fake
          ..emit(_state(begin.transactionId, 1, oldest: 0))
          ..emit(_state(begin.transactionId, 1, oldest: 0))
          ..emit(_bounds(begin.transactionId, 2, newest: 0));
      };
      expect(
        (await _client(
          transport: transport,
          stateStore: stateStore,
          processor: processor,
        ).pull()).segmentCount,
        0,
      );
    },
  );

  test(
    'empty retained journal reports an allocated gap without inventing history',
    () async {
      await stateStore.save(
        'paired-board',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 2,
        ),
      );
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        fake
          ..emit(_state(begin.transactionId, 1, oldest: 0, matched: true))
          ..emit(_bounds(begin.transactionId, 2, newest: 3, ack: 2, lost: 1));
      };
      final GameHistorySyncResult result = await _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
      ).pull();
      expect(result.gapObserved, isTrue);
      expect(result.durableAck, 2);
      expect(result.segmentCount, 0);
      expect(transport.writes, hasLength(1));
      expect((await repository.load()).games, isEmpty);
    },
  );

  test('cancel during cursor load never subscribes or writes', () async {
    final _PausedLoadStore paused = _PausedLoadStore(stateStore.directory);
    final GameHistoryPullClient client = _client(
      transport: transport,
      stateStore: paused,
      processor: processor,
    );
    final Future<void> failed = expectLater(client.pull(), throwsStateError);
    await paused.entered.future;
    await client.cancel();
    paused.release.complete();
    await failed;
    expect(transport.events, isEmpty);
  });

  test('owner loss after durability prevents the pruning commit', () async {
    final _Segment segment = _openingSegment(firstSequence: 1);
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
      fake
        ..emit(_state(begin.transactionId, 1, oldest: 1))
        ..emit(_bounds(begin.transactionId, 2, newest: 2))
        ..emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.segmentBegin.wireValue,
            transactionId: begin.transactionId,
            frameSequence: 3,
            payload: segment.descriptor.encode(),
          ),
        )
        ..emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.data.wireValue,
            transactionId: begin.transactionId,
            frameSequence: 4,
            payload: segment.bytes,
          ),
        );
    };
    final GameHistoryPullClient client = _client(
      transport: transport,
      stateStore: stateStore,
      processor: processor,
      isOwnerReady: () async =>
          (await stateStore.load('paired-board')).cursor == null,
    );

    await expectLater(client.pull(), throwsStateError);

    expect((await repository.load()).games, hasLength(1));
    expect((await stateStore.load('paired-board')).cursor?.durableAck, 2);
    expect(
      transport.writes.map((GameHistoryFrame frame) => frame.knownOpcode),
      isNot(contains(GameHistoryOpcode.commit)),
    );
  });

  for (final bool sameJournal in <bool>[false, true]) {
    test(
      '${sameJournal ? 'same' : 'new'} journal retained gap imports partial base',
      () async {
        await stateStore.save(
          'paired-board',
          const GameHistorySyncCursor(
            journalId: '1111111111111111',
            durableAck: 40,
          ),
        );
        const GameHistoryLineBaseRecord retained = GameHistoryLineBaseRecord(
          outboxSequence: 50,
          boardGameId: _gameId,
          lineRevision: 100,
          variant: GameVariant.classic,
          position: ArchivedPosition(
            ply: 100,
            fen: '8/8/8/8/8/8/8/8 w - - 0 51',
          ),
        );
        final _Segment segment = _encodeSegment(<GameHistoryRecord>[retained]);
        transport.onBegin =
            (_FakeTransport fake, GameHistoryFrame begin) async {
              final GameHistoryBeginPayload request =
                  GameHistoryBeginPayload.decode(begin.payload);
              expect(request.journalId, '1111111111111111');
              expect(request.appDurableAck, 40);
              fake
                ..emit(
                  _state(
                    begin.transactionId,
                    1,
                    oldest: 50,
                    journalId: sameJournal
                        ? '1111111111111111'
                        : '2222222222222222',
                    matched: sameJournal,
                  ),
                )
                ..emit(
                  _bounds(
                    begin.transactionId,
                    2,
                    newest: 50,
                    lost: 3,
                    ack: sameJournal ? 40 : 0,
                  ),
                )
                ..emit(
                  GameHistoryFrame(
                    opcode: GameHistoryOpcode.segmentBegin.wireValue,
                    transactionId: begin.transactionId,
                    frameSequence: 3,
                    payload: segment.descriptor.encode(),
                  ),
                )
                ..emit(
                  GameHistoryFrame(
                    opcode: GameHistoryOpcode.data.wireValue,
                    transactionId: begin.transactionId,
                    frameSequence: 4,
                    payload: segment.bytes,
                  ),
                );
            };
        transport.onCommit =
            (_FakeTransport fake, GameHistoryFrame commit) async {
              fake.emit(
                GameHistoryFrame(
                  opcode: GameHistoryOpcode.committed.wireValue,
                  transactionId: commit.transactionId,
                  frameSequence: commit.frameSequence + 1,
                  payload: commit.payload,
                ),
              );
            };

        final GameHistorySyncResult result = await _client(
          transport: transport,
          stateStore: stateStore,
          processor: processor,
        ).pull();

        expect(result.gapObserved, isTrue);
        expect(result.lostGameCount, 3);
        expect(
          (await repository.load()).games.single.captureQuality,
          GameCaptureQuality.partial,
        );
        final GameHistorySyncCursor cursor = (await stateStore.load(
          'paired-board',
        )).cursor!;
        expect(
          cursor.journalId,
          sameJournal ? '1111111111111111' : '2222222222222222',
        );
        expect(cursor.durableAck, 50);
      },
    );
  }

  test('pulls each following segment in a fresh transaction', () async {
    const String secondGameId = '101112131415161718191a1b1c1d1e1f';
    final _Segment first = _encodeSegment(const <GameHistoryRecord>[
      GameHistoryLineBaseRecord(
        outboxSequence: 1,
        boardGameId: _gameId,
        lineRevision: 1,
        variant: GameVariant.classic,
        position: ArchivedPosition(ply: 0, fen: '8/8/8/8/8/8/8/8 w - - 0 1'),
      ),
      GameHistorySealRecord(
        outboxSequence: 2,
        boardGameId: _gameId,
        lineRevision: 2,
        status: GameArchiveStatus.unfinished,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
      ),
    ]);
    final _Segment second = _encodeSegment(const <GameHistoryRecord>[
      GameHistoryLineBaseRecord(
        outboxSequence: 3,
        boardGameId: secondGameId,
        lineRevision: 1,
        variant: GameVariant.classic,
        position: ArchivedPosition(ply: 0, fen: '8/8/8/8/8/8/8/8 w - - 0 1'),
      ),
    ]);
    int begins = 0;
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
      begins += 1;
      final _Segment segment = begins == 1 ? first : second;
      final GameHistoryBeginPayload request = GameHistoryBeginPayload.decode(
        begin.payload,
      );
      if (begins == 1) {
        expect(request.journalId, '0000000000000000');
      } else {
        expect(request.journalId, '0102030405060708');
        expect(request.appDurableAck, 2);
      }
      fake
        ..emit(_state(begin.transactionId, 1, oldest: 1, matched: begins > 1))
        ..emit(
          _bounds(begin.transactionId, 2, newest: 3, ack: begins == 1 ? 0 : 2),
        )
        ..emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.segmentBegin.wireValue,
            transactionId: begin.transactionId,
            frameSequence: 3,
            payload: segment.descriptor.encode(),
          ),
        )
        ..emit(
          GameHistoryFrame(
            opcode: GameHistoryOpcode.data.wireValue,
            transactionId: begin.transactionId,
            frameSequence: 4,
            payload: segment.bytes,
          ),
        );
    };
    transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) async {
      fake.emit(
        GameHistoryFrame(
          opcode: GameHistoryOpcode.committed.wireValue,
          transactionId: commit.transactionId,
          frameSequence: commit.frameSequence + 1,
          payload: commit.payload,
        ),
      );
    };
    int transactionId = 0x2000;
    final GameHistoryPullClient client = GameHistoryPullClient(
      boardIdentity: 'paired-board',
      transport: transport,
      stateStore: stateStore,
      processor: processor,
      isOwnerReady: () => true,
      nextTransactionId: () => ++transactionId,
      clock: () => DateTime.utc(2026, 9, 3),
    );

    final GameHistorySyncResult result = await client.pull();

    expect(begins, 2);
    expect(result.segmentCount, 2);
    expect(result.durableAck, 3);
    expect((await repository.load()).games, hasLength(2));
  });

  test(
    'pulls a greater-than-32-ply same-revision game across transactions',
    () async {
      final List<GameHistoryRecord> records =
          _lineRecords(count: 40, lineRevision: 40)..add(
            const GameHistorySealRecord(
              outboxSequence: 42,
              boardGameId: _gameId,
              lineRevision: 40,
              status: GameArchiveStatus.unfinished,
              outcome: GameOutcome.ongoing,
              winner: GameWinner.none,
            ),
          );
      final _Segment first = _encodeSegment(records.sublist(0, 24));
      final _Segment second = _encodeSegment(<GameHistoryRecord>[
        records[23],
        ...records.sublist(24),
      ], flags: gameHistorySegmentFlagAcknowledgedOverlap);
      int begins = 0;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
        begins += 1;
        final bool continuation = begins == 2;
        final GameHistoryBeginPayload request = GameHistoryBeginPayload.decode(
          begin.payload,
        );
        expect(
          request.journalId,
          continuation ? '0102030405060708' : '0000000000000000',
        );
        expect(request.appDurableAck, continuation ? 24 : 0);
        final _Segment segment = continuation ? second : first;
        fake
          ..emit(
            _state(begin.transactionId, 1, oldest: 1, matched: continuation),
          )
          ..emit(
            _bounds(
              begin.transactionId,
              2,
              newest: 42,
              ack: continuation ? 24 : 0,
            ),
          )
          ..emit(
            GameHistoryFrame(
              opcode: GameHistoryOpcode.segmentBegin.wireValue,
              transactionId: begin.transactionId,
              frameSequence: 3,
              payload: segment.descriptor.encode(),
            ),
          );
        int frameSequence = 4;
        for (int offset = 0; offset < segment.bytes.length; offset += 400) {
          final int end = offset + 400 < segment.bytes.length
              ? offset + 400
              : segment.bytes.length;
          fake.emit(
            GameHistoryFrame(
              opcode: GameHistoryOpcode.data.wireValue,
              transactionId: begin.transactionId,
              frameSequence: frameSequence++,
              payload: Uint8List.sublistView(segment.bytes, offset, end),
            ),
          );
        }
      };
      transport.onCommit =
          (_FakeTransport fake, GameHistoryFrame commit) async {
            fake.emit(
              GameHistoryFrame(
                opcode: GameHistoryOpcode.committed.wireValue,
                transactionId: commit.transactionId,
                frameSequence: commit.frameSequence + 1,
                payload: commit.payload,
              ),
            );
          };
      int transactionId = 0x3000;
      final GameHistoryPullClient client = GameHistoryPullClient(
        boardIdentity: 'paired-board',
        transport: transport,
        stateStore: stateStore,
        processor: processor,
        isOwnerReady: () => true,
        nextTransactionId: () => ++transactionId,
        clock: () => DateTime.utc(2026, 9, 3),
      );

      final GameHistorySyncResult result = await client.pull();
      final GameArchive archive = (await repository.load()).games.single;

      expect(begins, 2);
      expect(result.segmentCount, 2);
      expect(result.durableAck, 42);
      expect(archive.libraryId, 'board-$_gameId');
      expect(archive.boardGameId, _gameId);
      expect(archive.lineRevision, 40);
      expect(archive.currentPly, 40);
      expect(archive.status, GameArchiveStatus.unfinished);
      expect(archive.captureQuality, GameCaptureQuality.complete);
      expect(
        archive.positions.map((ArchivedPosition value) => value.ply),
        orderedEquals(List<int>.generate(41, (int index) => index)),
      );
      expect(archive.transitions, hasLength(40));
    },
  );

  test('typed rejection and frame gaps fail closed', () async {
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
      fake.emit(
        GameHistoryFrame(
          opcode: GameHistoryOpcode.reject.wireValue,
          transactionId: begin.transactionId,
          frameSequence: 1,
          payload: const GameHistoryRejectPayload(
            reason: GameHistoryRejectReason.busy,
            rejectedOpcode: 1,
            expectedFrameSequence: 1,
            detail: 0,
          ).encode(),
        ),
      );
    };
    await expectLater(
      _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
      ).pull(),
      throwsA(isA<GameHistoryRejectedException>()),
    );

    await transport.close();
    transport = _FakeTransport();
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) async {
      fake.emit(_state(begin.transactionId, 2, oldest: 0));
    };
    await expectLater(
      _client(
        transport: transport,
        stateStore: stateStore,
        processor: processor,
      ).pull(),
      throwsFormatException,
    );
  });
}

GameHistoryPullClient _client({
  required _FakeTransport transport,
  required GameHistorySyncStateStore stateStore,
  required GameHistoryDurableSegmentProcessor processor,
  FutureOr<bool> Function()? isOwnerReady,
}) {
  return GameHistoryPullClient(
    boardIdentity: 'paired-board',
    transport: transport,
    stateStore: stateStore,
    processor: processor,
    isOwnerReady: isOwnerReady ?? () => true,
    nextTransactionId: () => 0x1234,
    clock: () => DateTime.utc(2026, 9, 3),
    inactivityTimeout: const Duration(seconds: 1),
  );
}

GameHistoryFrame _state(
  int transactionId,
  int sequence, {
  required int oldest,
  String journalId = '0102030405060708',
  bool matched = false,
}) {
  return GameHistoryFrame(
    opcode: GameHistoryOpcode.state.wireValue,
    flags:
        gameHistoryStateFlagCursorReconciled |
        (matched ? gameHistoryStateFlagJournalMatched : 0) |
        (oldest == 0
            ? gameHistoryStateFlagNoRecords
            : gameHistoryStateFlagSegmentFollows),
    transactionId: transactionId,
    frameSequence: sequence,
    payload: GameHistoryStatePayload(
      journalId: journalId,
      oldestRetained: oldest,
    ).encode(),
  );
}

GameHistoryFrame _bounds(
  int transactionId,
  int sequence, {
  required int newest,
  int lost = 0,
  int ack = 0,
}) {
  return GameHistoryFrame(
    opcode: GameHistoryOpcode.stateBounds.wireValue,
    transactionId: transactionId,
    frameSequence: sequence,
    payload: GameHistoryStateBoundsPayload(
      newestAllocated: newest,
      boardDurableAck: ack,
      lostGameCount: lost,
    ).encode(),
  );
}

_Segment _openingSegment({required int firstSequence}) {
  return _encodeSegment(<GameHistoryRecord>[
    GameHistoryLineBaseRecord(
      outboxSequence: firstSequence,
      boardGameId: _gameId,
      lineRevision: 1,
      variant: GameVariant.classic,
      position: const ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
      ),
    ),
    GameHistoryCommittedPlyRecord(
      outboxSequence: firstSequence + 1,
      boardGameId: _gameId,
      lineRevision: 2,
      position: const ArchivedPosition(
        ply: 1,
        fen:
            'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
            'RNBQKBNR b KQkq e3 0 1',
      ),
      transition: const ArchivedTransition(
        side: GameSide.white,
        from: 52,
        to: 36,
      ),
    ),
  ]);
}

class _Segment {
  const _Segment(this.bytes, this.descriptor);

  final Uint8List bytes;
  final GameHistorySegmentDescriptor descriptor;
}

_Segment _encodeSegment(List<GameHistoryRecord> records, {int flags = 0}) {
  final Uint8List bytes = Uint8List.fromList(<int>[
    for (final GameHistoryRecord record in records)
      ...GameHistoryCodec.encodeRecord(record),
  ]);
  return _Segment(
    bytes,
    GameHistorySegmentDescriptor(
      encodedBytes: bytes.length,
      recordCount: records.length,
      flags: flags,
      firstOutboxSequence: records.first.outboxSequence,
      crc32: GameHistoryCrc32.compute(bytes),
    ),
  );
}

List<GameHistoryRecord> _lineRecords({
  required int count,
  required int lineRevision,
}) {
  final List<GameHistoryRecord> records = <GameHistoryRecord>[];
  final Map<int, String> pieces = <int, String>{
    0: 'r',
    1: 'k',
    56: 'R',
    57: 'K',
  };
  records.add(
    GameHistoryLineBaseRecord(
      outboxSequence: 1,
      boardGameId: _gameId,
      lineRevision: lineRevision,
      variant: GameVariant.classic,
      position: ArchivedPosition(
        ply: 0,
        fen: _fenFromPieces(pieces, whiteToMove: true, fullmoveNumber: 1),
      ),
    ),
  );
  for (int ply = 1; ply <= count; ply += 1) {
    final bool whiteMoved = ply.isOdd;
    final int from;
    final int to;
    if (whiteMoved) {
      from = pieces.containsKey(56) ? 56 : 48;
      to = from == 56 ? 48 : 56;
    } else {
      from = pieces.containsKey(0) ? 0 : 8;
      to = from == 0 ? 8 : 0;
    }
    pieces[to] = pieces.remove(from)!;
    records.add(
      GameHistoryCommittedPlyRecord(
        outboxSequence: ply + 1,
        boardGameId: _gameId,
        lineRevision: lineRevision,
        position: ArchivedPosition(
          ply: ply,
          fen: _fenFromPieces(
            pieces,
            whiteToMove: !whiteMoved,
            fullmoveNumber: (ply ~/ 2) + 1,
          ),
        ),
        transition: ArchivedTransition(
          side: whiteMoved ? GameSide.white : GameSide.black,
          from: from,
          to: to,
        ),
      ),
    );
  }
  return records;
}

String _fenFromPieces(
  Map<int, String> pieces, {
  required bool whiteToMove,
  required int fullmoveNumber,
}) {
  final List<String> ranks = <String>[];
  for (int rank = 0; rank < 8; rank += 1) {
    final StringBuffer buffer = StringBuffer();
    int empty = 0;
    for (int file = 0; file < 8; file += 1) {
      final String? piece = pieces[(rank * 8) + file];
      if (piece == null) {
        empty += 1;
        continue;
      }
      if (empty != 0) {
        buffer.write(empty);
        empty = 0;
      }
      buffer.write(piece);
    }
    if (empty != 0) buffer.write(empty);
    ranks.add(buffer.toString());
  }
  return '${ranks.join('/')} ${whiteToMove ? 'w' : 'b'} - - 0 '
      '$fullmoveNumber';
}

typedef _FrameHandler =
    Future<void> Function(_FakeTransport transport, GameHistoryFrame frame);

class _FakeTransport implements GameHistoryTransport {
  final StreamController<Uint8List> _notifications =
      StreamController<Uint8List>.broadcast();
  final List<String> events = <String>[];
  final List<GameHistoryFrame> writes = <GameHistoryFrame>[];
  void Function(GameHistoryFrame)? onDeliver;
  _FrameHandler? onBegin;
  _FrameHandler? onCommit;
  bool failSubscribe = false;
  bool failUnsubscribe = false;

  @override
  Stream<Uint8List> get notifications =>
      _notifications.stream.map((Uint8List bytes) {
        onDeliver?.call(GameHistoryFrame.decode(bytes));
        return bytes;
      });

  @override
  Future<void> subscribe() async {
    events.add('subscribe');
    if (failSubscribe) {
      failSubscribe = false;
      throw StateError('subscribe failed');
    }
  }

  @override
  Future<void> unsubscribe() async {
    events.add('unsubscribe');
    if (failUnsubscribe) {
      failUnsubscribe = false;
      throw StateError('unsubscribe failed');
    }
  }

  @override
  Future<void> write(Uint8List value) async {
    final GameHistoryFrame frame = GameHistoryFrame.decode(value);
    writes.add(frame);
    events.add('write:${frame.knownOpcode?.name ?? frame.opcode}');
    if (frame.knownOpcode == GameHistoryOpcode.begin) {
      await onBegin?.call(this, frame);
    } else if (frame.knownOpcode == GameHistoryOpcode.commit) {
      await onCommit?.call(this, frame);
    }
  }

  void emitError(Object error) => _notifications.addError(error);

  void emit(GameHistoryFrame frame) {
    _notifications.add(frame.encode());
  }

  Future<void> close() async {
    if (_notifications.isClosed) return;
    final Future<void> closed = _notifications.close();
    if (_notifications.hasListener) await closed;
  }
}

class _PausedCursorStore extends GameHistorySyncStateStore {
  _PausedCursorStore(super.directory);
  final Completer<void> saved = Completer<void>();
  final Completer<void> release = Completer<void>();
  @override
  Future<void> save(String boardIdentity, GameHistorySyncCursor cursor) async {
    await super.save(boardIdentity, cursor);
    saved.complete();
    await release.future;
  }
}

class _PausedLoadStore extends GameHistorySyncStateStore {
  _PausedLoadStore(super.directory);
  final Completer<void> entered = Completer<void>();
  final Completer<void> release = Completer<void>();
  @override
  Future<GameHistorySyncStateRead> load(String boardIdentity) async {
    entered.complete();
    await release.future;
    return super.load(boardIdentity);
  }
}
