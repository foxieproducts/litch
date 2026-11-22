import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/current_game_pull_client.dart';
import 'package:litch_mobile/src/services/current_game_receive_store.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_history_pull_client.dart';
import 'package:litch_mobile/src/services/game_history_segment_importer.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const String _boardIdentity = 'paired-board';
const String _gameId = '000102030405060708090a0b0c0d0e0f';
const GameHistoryLineBaseRecord _base = GameHistoryLineBaseRecord(
  outboxSequence: 1,
  boardGameId: _gameId,
  lineRevision: 1,
  variant: GameVariant.classic,
  position: ArchivedPosition(
    ply: 0,
    fen:
        'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
        'RNBQKBNR w KQkq - 0 1',
  ),
);
const GameHistoryCommittedPlyRecord _move = GameHistoryCommittedPlyRecord(
  outboxSequence: 2,
  boardGameId: _gameId,
  lineRevision: 2,
  position: ArchivedPosition(
    ply: 1,
    fen:
        'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
        'RNBQKBNR b KQkq e3 0 1',
  ),
  transition: ArchivedTransition(side: GameSide.white, from: 52, to: 36),
);

void main() {
  late Directory root;
  late GameLibraryRepository repository;
  late GameHistorySegmentImporter importer;
  late CurrentGameReceiveStore receiveStore;
  late _FakeTransport transport;
  late _Segment baseSegment;
  late _Segment moveSegment;
  late CurrentGameDigest digest;
  const CurrentGameBounds bounds = CurrentGameBounds(
    lineRevision: 2,
    recordCount: 2,
    flags: currentGameSyncPresentFlag | currentGameSyncArchiveRequiredFlag,
  );

  CurrentGamePullClient client({required List<int> transactionIds}) {
    int index = 0;
    return CurrentGamePullClient(
      boardIdentity: _boardIdentity,
      transport: transport,
      receiveStore: receiveStore,
      isOwnerReady: () => true,
      nextTransactionId: () => transactionIds[index++],
      clock: () => DateTime.utc(2026, 9, 11),
    );
  }

  Future<void> seedOriginalCompleteLine() async {
    final _Segment complete = _segment(<GameHistoryRecord>[_base, _move]);
    await receiveStore.acceptSegment(
      boardIdentity: _boardIdentity,
      advertisedDigest: digest,
      bounds: bounds,
      descriptor: complete.descriptor,
      bytes: complete.bytes,
      observedAt: DateTime.utc(2026, 9, 11),
    );
  }

  setUp(() async {
    root = await Directory.systemTemp.createTemp('litch-current-pull-');
    repository = GameLibraryRepository(
      GameLibraryStore(Directory('${root.path}/library')),
    );
    importer = GameHistorySegmentImporter(repository);
    receiveStore = CurrentGameReceiveStore(
      directory: Directory('${root.path}/receive'),
      importer: importer,
    );
    transport = _FakeTransport();
    baseSegment = _segment(<GameHistoryRecord>[_base]);
    moveSegment = _segment(<GameHistoryRecord>[_move]);
    digest = _digest(
      Uint8List.fromList(<int>[...baseSegment.bytes, ...moveSegment.bytes]),
    );
  });

  tearDown(() async {
    await transport.close();
    await repository.close();
    if (await root.exists()) await root.delete(recursive: true);
  });

  test(
    'pulls multiple bounded segments with a fresh transaction each time',
    () async {
      int beginCount = 0;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        beginCount += 1;
        final CurrentGameResumeCursor cursor = CurrentGameResumeCursor.decode(
          begin.payload,
        );
        expect(begin.frameSequence, 0);
        if (beginCount == 1) {
          expect(cursor.nextRecordSequence, 0);
          _emitSegment(fake, begin, digest, bounds, baseSegment);
        } else {
          expect(cursor.nextRecordSequence, 2);
          _emitSegment(fake, begin, digest, bounds, moveSegment);
        }
      };
      transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) {
        fake.emit(
          _frame(
            CurrentGameSyncOpcode.segmentCommitted,
            commit.transactionId,
            commit.frameSequence + 1,
            commit.payload,
          ),
        );
      };
      transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
        expect(ack.payload, digest.fingerprint);
        fake.emit(
          _frame(
            CurrentGameSyncOpcode.archived,
            ack.transactionId,
            ack.frameSequence + 1,
            ack.payload,
          ),
        );
      };

      final CurrentGameSyncResult result = await client(
        transactionIds: <int>[11, 12],
      ).pull();

      expect(transport.events.take(2), <String>['subscribe', 'write:begin']);
      expect(beginCount, 2);
      expect(result.segmentCount, 2);
      expect(
        result.encodedBytes,
        baseSegment.bytes.length + moveSegment.bytes.length,
      );
      expect(result.cursor?.nextRecordSequence, 3);
      expect(result.archiveAcknowledged, isTrue);
      expect((await repository.load()).games.single.currentPly, 1);
      expect(transport.events.last, 'unsubscribe');
    },
  );

  test(
    'admitted Begin waits for delayed State without creating another request',
    () async {
      int beginCount = 0;
      Timer? response;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        beginCount += 1;
        // Model a storage-backed semantic reply after admission. Reliable
        // transport repair owns a lost reply; this owner must not resend Begin.
        response ??= Timer(const Duration(milliseconds: 350), () {
          _emitSegment(
            fake,
            begin,
            digest,
            bounds,
            _segment(<GameHistoryRecord>[_base, _move]),
          );
        });
      };
      addTearDown(() => response?.cancel());
      _echoCommitAndArchive(transport);
      final CurrentGamePullClient pullClient = CurrentGamePullClient(
        boardIdentity: _boardIdentity,
        transport: transport,
        receiveStore: receiveStore,
        isOwnerReady: () => true,
        nextTransactionId: () => 14,
        clock: () => DateTime.utc(2026, 9, 11),
      );

      final CurrentGameSyncResult result = await pullClient.pull();

      expect(result.archiveAcknowledged, isTrue);
      expect(result.archive?.currentPly, 1);
      expect(beginCount, 1);
      expect(
        transport.writes.where(
          (GameHistoryFrame frame) =>
              frame.opcode == CurrentGameSyncOpcode.begin.wireValue,
        ),
        hasLength(1),
      );
      expect((await repository.load()).games.single.currentPly, 1);
    },
  );

  test('cold recovery replays ArchiveAck without requesting DATA', () async {
    final _Segment complete = _segment(<GameHistoryRecord>[_base, _move]);
    await receiveStore.acceptSegment(
      boardIdentity: _boardIdentity,
      advertisedDigest: digest,
      bounds: bounds,
      descriptor: complete.descriptor,
      bytes: complete.bytes,
      observedAt: DateTime.utc(2026, 9, 11),
    );
    receiveStore = CurrentGameReceiveStore(
      directory: Directory('${root.path}/receive'),
      importer: importer,
    );
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      expect(
        CurrentGameResumeCursor.decode(begin.payload).nextRecordSequence,
        3,
      );
      _emitStateAndBounds(fake, begin, digest, bounds);
    };
    transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
      fake.emit(
        _frame(
          CurrentGameSyncOpcode.archived,
          ack.transactionId,
          ack.frameSequence + 1,
          ack.payload,
        ),
      );
    };

    final CurrentGameSyncResult result = await client(
      transactionIds: <int>[20],
    ).pull();

    expect(result.segmentCount, 0);
    expect(result.archiveAcknowledged, isTrue);
    expect(
      transport.writes.map((GameHistoryFrame frame) => frame.opcode),
      <int>[
        CurrentGameSyncOpcode.begin.wireValue,
        CurrentGameSyncOpcode.archiveAck.wireValue,
      ],
    );
  });

  test(
    'durable duplicate segment recovers after a lost Committed reply',
    () async {
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        _emitSegment(fake, begin, digest, bounds, baseSegment);
      };
      transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) {
        fake.emitError(StateError('Committed reply lost'));
      };
      await expectLater(
        client(transactionIds: <int>[30]).pull(),
        throwsStateError,
      );
      expect(
        (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
        2,
      );

      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        // A conservative board may replay the exact already-durable segment.
        _emitSegment(fake, begin, digest, bounds, baseSegment);
      };
      transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) {
        fake.emit(
          _frame(
            CurrentGameSyncOpcode.segmentCommitted,
            commit.transactionId,
            commit.frameSequence + 1,
            commit.payload,
          ),
        );
      };
      int pullBegins = 0;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        pullBegins += 1;
        _emitSegment(
          fake,
          begin,
          digest,
          bounds,
          pullBegins == 1 ? baseSegment : moveSegment,
        );
      };
      transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
        fake.emit(
          _frame(
            CurrentGameSyncOpcode.archived,
            ack.transactionId,
            ack.frameSequence + 1,
            ack.payload,
          ),
        );
      };
      final CurrentGameSyncResult recovered = await client(
        transactionIds: <int>[31, 32],
      ).pull();
      expect(recovered.segmentCount, 2);
      expect(recovered.cursor?.nextRecordSequence, 3);
      expect((await repository.load()).games.single.currentPly, 1);
    },
  );

  test(
    'cancellation during durable accept permits no later transport write',
    () async {
      final _PausedReceiveStore paused = _PausedReceiveStore(
        directory: Directory('${root.path}/receive'),
        importer: importer,
      );
      receiveStore = paused;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        _emitSegment(fake, begin, digest, bounds, baseSegment);
      };
      final CurrentGamePullClient pullClient = client(
        transactionIds: <int>[40],
      );
      final Future<void> failure = expectLater(
        pullClient.pull(),
        throwsStateError,
      );
      await paused.accepted.future;
      await pullClient.cancel();
      paused.release.complete();
      await failure;

      expect(
        transport.writes.map((GameHistoryFrame frame) => frame.opcode),
        <int>[CurrentGameSyncOpcode.begin.wireValue],
      );
      expect(
        (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
        2,
      );
    },
  );

  test(
    'reconnect after lost Archived replays ACK after archive gate clears',
    () async {
      await seedOriginalCompleteLine();
      final List<Uint8List> acknowledgements = <Uint8List>[];
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        _emitStateAndBounds(fake, begin, digest, bounds);
      };
      transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
        acknowledgements.add(Uint8List.fromList(ack.payload));
        fake.emitError(StateError('Archived reply lost'));
      };

      await expectLater(
        client(transactionIds: <int>[50]).pull(),
        throwsStateError,
      );

      await transport.close();
      transport = _FakeTransport();
      receiveStore = CurrentGameReceiveStore(
        directory: Directory('${root.path}/receive'),
        importer: importer,
      );
      const CurrentGameBounds adoptedBounds = CurrentGameBounds(
        lineRevision: 2,
        recordCount: 2,
        flags: currentGameSyncPresentFlag,
      );
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        expect(
          CurrentGameResumeCursor.decode(begin.payload).nextRecordSequence,
          3,
        );
        _emitStateAndBounds(fake, begin, digest, adoptedBounds);
      };
      transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
        acknowledgements.add(Uint8List.fromList(ack.payload));
        fake.emit(
          _frame(
            CurrentGameSyncOpcode.archived,
            ack.transactionId,
            ack.frameSequence + 1,
            ack.payload,
          ),
        );
      };
      final CurrentGameSyncResult result = await client(
        transactionIds: <int>[51],
      ).pull();

      expect(result.archiveAcknowledged, isTrue);
      expect(acknowledgements, hasLength(2));
      expect(acknowledgements[0], digest.fingerprint);
      expect(acknowledgements[1], acknowledgements[0]);
    },
  );

  test('shorter Undo replacement restarts at record 1 and wins', () async {
    await seedOriginalCompleteLine();
    const GameHistoryLineBaseRecord undoneBase = GameHistoryLineBaseRecord(
      outboxSequence: 1,
      boardGameId: _gameId,
      lineRevision: 3,
      variant: GameVariant.classic,
      position: ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
      ),
    );
    final _Segment replacement = _segment(<GameHistoryRecord>[undoneBase]);
    final CurrentGameDigest replacementDigest = _digest(replacement.bytes);
    const CurrentGameBounds replacementBounds = CurrentGameBounds(
      lineRevision: 3,
      recordCount: 1,
      flags: currentGameSyncPresentFlag | currentGameSyncArchiveRequiredFlag,
    );
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      expect(
        CurrentGameResumeCursor.decode(begin.payload).nextRecordSequence,
        3,
      );
      _emitSegment(
        fake,
        begin,
        replacementDigest,
        replacementBounds,
        replacement,
      );
    };
    _echoCommitAndArchive(transport);

    final CurrentGameSyncResult result = await client(
      transactionIds: <int>[52],
    ).pull();

    expect(result.segmentCount, 1);
    expect(result.cursor?.nextRecordSequence, 2);
    final GameArchive archive = (await repository.load()).games.single;
    expect(archive.lineRevision, 3);
    expect(archive.currentPly, 0);
  });

  test('equal-count changed-line replacement supersedes old archive', () async {
    await seedOriginalCompleteLine();
    const GameHistoryLineBaseRecord replacementBase = GameHistoryLineBaseRecord(
      outboxSequence: 1,
      boardGameId: _gameId,
      lineRevision: 2,
      variant: GameVariant.classic,
      position: ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
      ),
    );
    const GameHistoryCommittedPlyRecord d4 = GameHistoryCommittedPlyRecord(
      outboxSequence: 2,
      boardGameId: _gameId,
      lineRevision: 3,
      position: ArchivedPosition(
        ply: 1,
        fen:
            'rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/'
            'RNBQKBNR b KQkq d3 0 1',
      ),
      transition: ArchivedTransition(side: GameSide.white, from: 51, to: 35),
    );
    final _Segment replacement = _segment(<GameHistoryRecord>[
      replacementBase,
      d4,
    ]);
    final CurrentGameDigest replacementDigest = _digest(replacement.bytes);
    const CurrentGameBounds replacementBounds = CurrentGameBounds(
      lineRevision: 3,
      recordCount: 2,
      flags: currentGameSyncPresentFlag | currentGameSyncArchiveRequiredFlag,
    );
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      expect(
        CurrentGameResumeCursor.decode(begin.payload).nextRecordSequence,
        3,
      );
      _emitSegment(
        fake,
        begin,
        replacementDigest,
        replacementBounds,
        replacement,
      );
    };
    _echoCommitAndArchive(transport);

    final CurrentGameSyncResult result = await client(
      transactionIds: <int>[53],
    ).pull();

    expect(result.segmentCount, 1);
    expect(result.cursor?.nextRecordSequence, 3);
    final GameArchive archive = (await repository.load()).games.single;
    expect(archive.lineRevision, 3);
    expect(archive.currentPly, 1);
    expect(archive.transitions.single.from, 51);
    expect(archive.transitions.single.to, 35);
  });

  test(
    'record-1 fallback truncates an identical old prefix before replacement',
    () async {
      await seedOriginalCompleteLine();
      const GameHistoryCommittedPlyRecord d4 = GameHistoryCommittedPlyRecord(
        outboxSequence: 2,
        boardGameId: _gameId,
        lineRevision: 3,
        position: ArchivedPosition(
          ply: 1,
          fen:
              'rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/'
              'RNBQKBNR b KQkq d3 0 1',
        ),
        transition: ArchivedTransition(side: GameSide.white, from: 51, to: 35),
      );
      final _Segment replacementBase = _segment(<GameHistoryRecord>[_base]);
      final _Segment replacementMove = _segment(<GameHistoryRecord>[d4]);
      final CurrentGameDigest replacementDigest = _digest(
        Uint8List.fromList(<int>[
          ...replacementBase.bytes,
          ...replacementMove.bytes,
        ]),
      );
      const CurrentGameBounds replacementBounds = CurrentGameBounds(
        lineRevision: 3,
        recordCount: 2,
        flags: currentGameSyncPresentFlag | currentGameSyncArchiveRequiredFlag,
      );
      int beginCount = 0;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        beginCount += 1;
        _emitSegment(
          fake,
          begin,
          replacementDigest,
          replacementBounds,
          beginCount == 1 ? replacementBase : replacementMove,
        );
      };
      _echoCommitAndArchive(transport);

      final CurrentGameSyncResult result = await client(
        transactionIds: <int>[54, 55],
      ).pull();

      expect(beginCount, 2);
      expect(result.segmentCount, 2);
      final GameArchive archive = (await repository.load()).games.single;
      expect(archive.lineRevision, 3);
      expect(archive.transitions.single.from, 51);
      expect(archive.transitions.single.to, 35);
    },
  );

  test(
    'absent current line succeeds without an archive acknowledgement',
    () async {
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        _emitStateAndBounds(
          fake,
          begin,
          const CurrentGameDigest.absent(),
          const CurrentGameBounds.absent(),
        );
      };
      final CurrentGameSyncResult result = await client(
        transactionIds: <int>[60],
      ).pull();
      expect(result.linePresent, isFalse);
      expect(result.archiveAcknowledged, isFalse);
      expect(transport.writes, hasLength(1));
    },
  );

  test('rejects a changed Committed tuple after durable acceptance', () async {
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      _emitSegment(fake, begin, digest, bounds, baseSegment);
    };
    transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) {
      final GameHistoryCommitPayload sent = GameHistoryCommitPayload.decode(
        commit.payload,
      );
      fake.emit(
        _frame(
          CurrentGameSyncOpcode.segmentCommitted,
          commit.transactionId,
          commit.frameSequence + 1,
          GameHistoryCommitPayload(
            firstOutboxSequence: sent.firstOutboxSequence,
            lastOutboxSequence: sent.lastOutboxSequence,
            crc32: sent.crc32 ^ 1,
          ).encode(),
        ),
      );
    };

    await expectLater(
      client(transactionIds: <int>[61]).pull(),
      throwsFormatException,
    );
    expect(
      (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
      2,
    );
  });

  test('rejects a changed CurrentArchived fingerprint', () async {
    final _Segment complete = _segment(<GameHistoryRecord>[_base, _move]);
    await receiveStore.acceptSegment(
      boardIdentity: _boardIdentity,
      advertisedDigest: digest,
      bounds: bounds,
      descriptor: complete.descriptor,
      bytes: complete.bytes,
      observedAt: DateTime.utc(2026, 9, 11),
    );
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      _emitStateAndBounds(fake, begin, digest, bounds);
    };
    transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
      final Uint8List changed = Uint8List.fromList(ack.payload)..[0] ^= 1;
      fake.emit(
        _frame(
          CurrentGameSyncOpcode.archived,
          ack.transactionId,
          ack.frameSequence + 1,
          changed,
        ),
      );
    };

    await expectLater(
      client(transactionIds: <int>[62]).pull(),
      throwsFormatException,
    );
  });

  test('shared connection owner excludes a second current-line pull', () async {
    final GameHistoryTransactionOwner owner = GameHistoryTransactionOwner();
    final Completer<void> began = Completer<void>();
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      began.complete();
    };
    int transactionId = 63;
    CurrentGamePullClient ownedClient() => CurrentGamePullClient(
      boardIdentity: _boardIdentity,
      transport: transport,
      receiveStore: receiveStore,
      isOwnerReady: () => true,
      nextTransactionId: () => transactionId++,
      transactionOwner: owner,
      inactivityTimeout: const Duration(seconds: 1),
    );
    final CurrentGamePullClient first = ownedClient();
    final Future<void> firstFailure = expectLater(
      first.pull(),
      throwsStateError,
    );
    await began.future;

    await expectLater(ownedClient().pull(), throwsStateError);
    expect(
      transport.events.where((String event) => event == 'subscribe'),
      hasLength(1),
    );
    await first.cancel();
    await firstFailure;
  });

  test('stale duplicate frames do not extend the progress deadline', () async {
    Duration elapsed = Duration.zero;
    int deliveredBounds = 0;
    transport.onDeliver = (GameHistoryFrame frame) {
      if (frame.opcode == CurrentGameSyncOpcode.bounds.wireValue &&
          ++deliveredBounds == 2) {
        elapsed = const Duration(seconds: 2);
      }
    };
    transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
      fake
        ..emit(
          _frame(
            CurrentGameSyncOpcode.state,
            begin.transactionId,
            1,
            digest.encode(allowAbsent: false),
          ),
        )
        ..emit(
          _frame(
            CurrentGameSyncOpcode.bounds,
            begin.transactionId,
            2,
            bounds.encode(),
          ),
        )
        ..emit(
          _frame(
            CurrentGameSyncOpcode.bounds,
            begin.transactionId,
            2,
            bounds.encode(),
          ),
        )
        ..emit(
          _frame(
            CurrentGameSyncOpcode.segmentBegin,
            begin.transactionId,
            3,
            baseSegment.descriptor.encode(),
          ),
        );
    };
    final CurrentGamePullClient client = CurrentGamePullClient(
      boardIdentity: _boardIdentity,
      transport: transport,
      receiveStore: receiveStore,
      isOwnerReady: () => true,
      nextTransactionId: () => 70,
      elapsed: () => elapsed,
      inactivityTimeout: const Duration(seconds: 1),
    );
    await expectLater(client.pull(), throwsA(isA<TimeoutException>()));
    expect(await receiveStore.loadCursor(_boardIdentity), isNull);
  });

  for (final String malformed in <String>['sequence', 'digest', 'changed']) {
    test('rejects malformed current-game $malformed', () async {
      int beginCount = 0;
      transport.onBegin = (_FakeTransport fake, GameHistoryFrame begin) {
        beginCount += 1;
        if (malformed == 'sequence') {
          fake.emit(
            _frame(
              CurrentGameSyncOpcode.state,
              begin.transactionId,
              2,
              digest.encode(allowAbsent: false),
            ),
          );
          return;
        }
        if (malformed == 'digest') {
          _emitStateAndBounds(
            fake,
            begin,
            const CurrentGameDigest.absent(),
            bounds,
          );
          return;
        }
        final CurrentGameDigest advertised = beginCount == 1
            ? digest
            : CurrentGameDigest(
                fingerprint: digest.fingerprint
                    .map((int byte) => byte ^ 0xff)
                    .toList(growable: false),
              );
        _emitSegment(
          fake,
          begin,
          advertised,
          bounds,
          beginCount == 1 ? baseSegment : moveSegment,
        );
      };
      transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) {
        fake.emit(
          _frame(
            CurrentGameSyncOpcode.segmentCommitted,
            commit.transactionId,
            commit.frameSequence + 1,
            commit.payload,
          ),
        );
      };

      await expectLater(
        client(transactionIds: <int>[80, 81]).pull(),
        throwsFormatException,
      );
    });
  }
}

class _PausedReceiveStore extends CurrentGameReceiveStore {
  _PausedReceiveStore({required super.directory, required super.importer});

  final Completer<void> accepted = Completer<void>();
  final Completer<void> release = Completer<void>();

  @override
  Future<CurrentGameAcceptedSegment> acceptSegment({
    required String boardIdentity,
    required CurrentGameDigest advertisedDigest,
    required CurrentGameBounds bounds,
    required GameHistorySegmentDescriptor descriptor,
    required Uint8List bytes,
    required DateTime observedAt,
    bool restartFromBeginning = false,
  }) async {
    final CurrentGameAcceptedSegment result = await super.acceptSegment(
      boardIdentity: boardIdentity,
      advertisedDigest: advertisedDigest,
      bounds: bounds,
      descriptor: descriptor,
      bytes: bytes,
      observedAt: observedAt,
      restartFromBeginning: restartFromBeginning,
    );
    accepted.complete();
    await release.future;
    return result;
  }
}

class _FakeTransport implements GameHistoryTransport {
  final StreamController<Uint8List> _notifications =
      StreamController<Uint8List>.broadcast();
  final List<String> events = <String>[];
  final List<GameHistoryFrame> writes = <GameHistoryFrame>[];
  void Function(_FakeTransport, GameHistoryFrame)? onBegin;
  void Function(_FakeTransport, GameHistoryFrame)? onCommit;
  void Function(_FakeTransport, GameHistoryFrame)? onArchiveAck;
  void Function(GameHistoryFrame)? onDeliver;

  @override
  Stream<Uint8List> get notifications =>
      _notifications.stream.map((Uint8List bytes) {
        onDeliver?.call(GameHistoryFrame.decode(bytes));
        return bytes;
      });

  @override
  Future<void> subscribe() async {
    events.add('subscribe');
  }

  @override
  Future<void> unsubscribe() async {
    events.add('unsubscribe');
  }

  @override
  Future<void> write(Uint8List value) async {
    final GameHistoryFrame frame = GameHistoryFrame.decode(value);
    writes.add(frame);
    final CurrentGameSyncOpcode opcode = CurrentGameSyncOpcode.values
        .singleWhere(
          (CurrentGameSyncOpcode candidate) =>
              candidate.wireValue == frame.opcode,
        );
    events.add('write:${opcode.name}');
    switch (opcode) {
      case CurrentGameSyncOpcode.begin:
        onBegin?.call(this, frame);
      case CurrentGameSyncOpcode.segmentCommit:
        onCommit?.call(this, frame);
      case CurrentGameSyncOpcode.archiveAck:
        onArchiveAck?.call(this, frame);
      case CurrentGameSyncOpcode.state:
      case CurrentGameSyncOpcode.bounds:
      case CurrentGameSyncOpcode.segmentBegin:
      case CurrentGameSyncOpcode.data:
      case CurrentGameSyncOpcode.segmentCommitted:
      case CurrentGameSyncOpcode.archived:
        throw StateError('Unexpected app write: ${opcode.name}.');
    }
  }

  void emit(GameHistoryFrame frame) {
    _notifications.add(frame.encode());
  }

  void emitError(Object error) => _notifications.addError(error);

  Future<void> close() => _notifications.close();
}

class _Segment {
  const _Segment(this.descriptor, this.bytes);

  final GameHistorySegmentDescriptor descriptor;
  final Uint8List bytes;
}

_Segment _segment(List<GameHistoryRecord> records) {
  final Uint8List bytes = Uint8List.fromList(<int>[
    for (final GameHistoryRecord record in records)
      ...GameHistoryCodec.encodeRecord(record),
  ]);
  return _Segment(
    GameHistorySegmentDescriptor(
      encodedBytes: bytes.length,
      recordCount: records.length,
      firstOutboxSequence: records.first.outboxSequence,
      crc32: GameHistoryCrc32.compute(bytes),
    ),
    bytes,
  );
}

CurrentGameDigest _digest(Uint8List bytes) => CurrentGameDigest(
  fingerprint: sha256.convert(bytes).bytes.take(12).toList(growable: false),
);

GameHistoryFrame _frame(
  CurrentGameSyncOpcode opcode,
  int transactionId,
  int frameSequence,
  Uint8List payload,
) => GameHistoryFrame(
  opcode: opcode.wireValue,
  transactionId: transactionId,
  frameSequence: frameSequence,
  payload: payload,
);

void _emitStateAndBounds(
  _FakeTransport fake,
  GameHistoryFrame begin,
  CurrentGameDigest digest,
  CurrentGameBounds bounds,
) {
  fake
    ..emit(
      _frame(
        CurrentGameSyncOpcode.state,
        begin.transactionId,
        1,
        digest.encode(allowAbsent: !bounds.present),
      ),
    )
    ..emit(
      _frame(
        CurrentGameSyncOpcode.bounds,
        begin.transactionId,
        2,
        bounds.encode(),
      ),
    );
}

void _emitSegment(
  _FakeTransport fake,
  GameHistoryFrame begin,
  CurrentGameDigest digest,
  CurrentGameBounds bounds,
  _Segment segment,
) {
  _emitStateAndBounds(fake, begin, digest, bounds);
  fake
    ..emit(
      _frame(
        CurrentGameSyncOpcode.segmentBegin,
        begin.transactionId,
        3,
        segment.descriptor.encode(),
      ),
    )
    ..emit(
      _frame(CurrentGameSyncOpcode.data, begin.transactionId, 4, segment.bytes),
    );
}

void _echoCommitAndArchive(_FakeTransport transport) {
  transport.onCommit = (_FakeTransport fake, GameHistoryFrame commit) {
    fake.emit(
      _frame(
        CurrentGameSyncOpcode.segmentCommitted,
        commit.transactionId,
        commit.frameSequence + 1,
        commit.payload,
      ),
    );
  };
  transport.onArchiveAck = (_FakeTransport fake, GameHistoryFrame ack) {
    fake.emit(
      _frame(
        CurrentGameSyncOpcode.archived,
        ack.transactionId,
        ack.frameSequence + 1,
        ack.payload,
      ),
    );
  };
}
