import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_history_segment_importer.dart';
import 'package:litch_mobile/src/services/game_history_sync_state_store.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const String _gameId = '000102030405060708090a0b0c0d0e0f';
const ArchivedPosition _initial = ArchivedPosition(
  ply: 0,
  fen:
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
      'RNBQKBNR w KQkq - 0 1',
);
const ArchivedPosition _afterE4 = ArchivedPosition(
  ply: 1,
  fen:
      'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/'
      'RNBQKBNR b KQkq e3 0 1',
);
const ArchivedTransition _e4 = ArchivedTransition(
  side: GameSide.white,
  from: 52,
  to: 36,
);

void main() {
  late Directory root;
  late GameLibraryRepository repository;
  late GameHistorySegmentImporter importer;
  late GameHistorySyncStateStore cursorStore;
  late GameHistoryDurableSegmentProcessor processor;

  setUp(() async {
    root = await Directory.systemTemp.createTemp('litch-history-import-');
    repository = GameLibraryRepository(
      GameLibraryStore(Directory('${root.path}/library')),
    );
    importer = GameHistorySegmentImporter(repository);
    cursorStore = GameHistorySyncStateStore(Directory('${root.path}/cursors'));
    processor = GameHistoryDurableSegmentProcessor(
      importer: importer,
      stateStore: cursorStore,
    );
  });

  tearDown(() async {
    await repository.close();
    if (await root.exists()) await root.delete(recursive: true);
  });

  for (final bool extendFirst in <bool>[true, false]) {
    test(
      'same-revision ${extendFirst ? 'extension then seal' : 'seal'} preserves players and survives reload',
      () async {
        const GameHistoryLineBaseRecord base = GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 2,
          variant: GameVariant.classic,
          position: _initial,
        );
        Future<GameHistoryDurableSegmentResult> receive(
          List<GameHistoryRecord> records,
        ) {
          final _EncodedSegment segment = _segment(records);
          return processor.process(
            boardIdentity: 'paired-board-1',
            journalId: '0102030405060708',
            descriptor: segment.descriptor,
            bytes: segment.bytes,
            observedAt: DateTime.utc(2026, 9, 7),
          );
        }

        await receive(<GameHistoryRecord>[base]);
        final GameArchive initial = (await repository.load()).games.single;
        final Map<String, Object?> attributed = initial.toJson();
        attributed['whitePlayer'] = ArchivedPlayerAttribution.profile(
          'lp_0123456789abcdef0123456789abcdef',
        ).toJson();
        attributed['blackPlayer'] = const ArchivedPlayerAttribution.guest()
            .toJson();
        await repository.save(GameArchive.fromJson(attributed));
        if (extendFirst) {
          await receive(const <GameHistoryRecord>[
            GameHistoryCommittedPlyRecord(
              outboxSequence: 2,
              boardGameId: _gameId,
              lineRevision: 2,
              position: _afterE4,
              transition: _e4,
            ),
          ]);
        }
        final List<GameHistoryRecord> seal = <GameHistoryRecord>[
          GameHistorySealRecord(
            outboxSequence: extendFirst ? 3 : 2,
            boardGameId: _gameId,
            lineRevision: 2,
            status: GameArchiveStatus.unfinished,
            outcome: GameOutcome.ongoing,
            winner: GameWinner.none,
          ),
        ];
        final GameHistoryDurableSegmentResult result = await receive(seal);
        final GameArchive reloaded = (await GameLibraryStore(
          Directory('${root.path}/library'),
        ).load()).games.single;
        expect(result.commit.lastOutboxSequence, extendFirst ? 3 : 2);
        expect(reloaded.status, GameArchiveStatus.unfinished);
        expect(reloaded.currentPly, extendFirst ? 1 : 0);
        expect(
          reloaded.whitePlayer.playerId,
          'lp_0123456789abcdef0123456789abcdef',
        );
        expect(reloaded.blackPlayer.kind, ArchivedPlayerKind.guest);
        expect((await receive(seal)).alreadyDurable, isTrue);
        expect(
          (await cursorStore.load('paired-board-1')).cursor?.durableAck,
          extendFirst ? 3 : 2,
        );
      },
    );
  }

  test('durably imports a fresh line before exposing its commit', () async {
    final _EncodedSegment segment = _segment(<GameHistoryRecord>[
      const GameHistoryLineBaseRecord(
        outboxSequence: 1,
        boardGameId: _gameId,
        lineRevision: 1,
        variant: GameVariant.classic,
        position: _initial,
      ),
      const GameHistoryCommittedPlyRecord(
        outboxSequence: 2,
        boardGameId: _gameId,
        lineRevision: 2,
        position: _afterE4,
        transition: _e4,
      ),
    ]);

    final GameHistoryDurableSegmentResult result = await processor.process(
      boardIdentity: 'paired-board-1',
      journalId: '0102030405060708',
      descriptor: segment.descriptor,
      bytes: segment.bytes,
      observedAt: DateTime.utc(2026, 9, 3),
    );

    expect(result.alreadyDurable, isFalse);
    expect(result.archive?.currentPly, 1);
    expect(result.commit.lastOutboxSequence, 2);
    expect((await cursorStore.load('paired-board-1')).cursor?.durableAck, 2);
    expect((await repository.load()).games.single.currentPly, 1);
  });

  test(
    'replayed durable segment is validated but does not rewrite archive',
    () async {
      final _EncodedSegment segment = _segment(<GameHistoryRecord>[
        const GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
        const GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: _gameId,
          lineRevision: 2,
          position: _afterE4,
          transition: _e4,
        ),
      ]);
      await processor.process(
        boardIdentity: 'paired-board-1',
        journalId: '0102030405060708',
        descriptor: segment.descriptor,
        bytes: segment.bytes,
        observedAt: DateTime.utc(2026, 9, 3),
      );

      final GameHistoryDurableSegmentResult replay = await processor.process(
        boardIdentity: 'paired-board-1',
        journalId: '0102030405060708',
        descriptor: segment.descriptor,
        bytes: segment.bytes,
        observedAt: DateTime.utc(2026, 9, 4),
      );

      expect(replay.alreadyDurable, isTrue);
      expect(replay.archive, isNull);
      expect(
        (await repository.load()).games.single.updatedAt,
        DateTime.utc(2026, 9, 3),
      );
    },
  );

  test(
    'continuation and replacement suffixes merge at their durable base',
    () async {
      await importer.importRecords(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: _gameId,
          lineRevision: 2,
          position: _afterE4,
          transition: _e4,
        ),
      ], observedAt: DateTime.utc(2026, 9, 3));
      const ArchivedPosition afterE5 = ArchivedPosition(
        ply: 2,
        fen:
            'rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/'
            'RNBQKBNR w KQkq e6 0 2',
      );
      await importer.importRecords(const <GameHistoryRecord>[
        GameHistoryCommittedPlyRecord(
          outboxSequence: 3,
          boardGameId: _gameId,
          lineRevision: 3,
          position: afterE5,
          transition: ArchivedTransition(
            side: GameSide.black,
            from: 12,
            to: 28,
          ),
        ),
      ], observedAt: DateTime.utc(2026, 9, 3, 0, 1));
      const ArchivedPosition afterC5 = ArchivedPosition(
        ply: 2,
        fen:
            'rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/'
            'RNBQKBNR w KQkq c6 0 2',
      );

      final GameArchive replaced = (await importer
          .importRecords(const <GameHistoryRecord>[
            GameHistoryLineBaseRecord(
              outboxSequence: 4,
              boardGameId: _gameId,
              lineRevision: 4,
              variant: GameVariant.classic,
              position: _afterE4,
            ),
            GameHistoryCommittedPlyRecord(
              outboxSequence: 5,
              boardGameId: _gameId,
              lineRevision: 4,
              position: afterC5,
              transition: ArchivedTransition(
                side: GameSide.black,
                from: 10,
                to: 26,
              ),
            ),
          ], observedAt: DateTime.utc(2026, 9, 3, 0, 2)))!;

      expect(replaced.currentPly, 2);
      expect(replaced.positions.last.fen, afterC5.fen);
      expect(replaced.transitions.last.from, 10);
      expect(replaced.captureQuality, GameCaptureQuality.complete);
    },
  );

  test(
    'a retained nonzero line base creates an explicit partial archive',
    () async {
      const ArchivedPosition retained = ArchivedPosition(
        ply: 100,
        fen: '8/8/8/8/8/8/8/8 w - - 0 51',
      );

      final GameArchive archive = (await importer
          .importRecords(const <GameHistoryRecord>[
            GameHistoryLineBaseRecord(
              outboxSequence: 50,
              boardGameId: _gameId,
              lineRevision: 100,
              variant: GameVariant.classic,
              position: retained,
            ),
          ], observedAt: DateTime.utc(2026, 9, 3)))!;

      expect(archive.captureQuality, GameCaptureQuality.partial);
      expect(archive.positions.single.ply, 100);
    },
  );

  for (final bool appendMove in <bool>[false, true]) {
    test(
      'newer revision cannot ${appendMove ? 'extend' : 'reopen'} an unfinished durable game',
      () async {
        Future<GameHistoryDurableSegmentResult> receive(
          List<GameHistoryRecord> records,
        ) {
          final _EncodedSegment segment = _segment(records);
          return processor.process(
            boardIdentity: 'paired-board-1',
            journalId: '0102030405060708',
            descriptor: segment.descriptor,
            bytes: segment.bytes,
            observedAt: DateTime.utc(2026, 9, 8),
          );
        }

        final List<GameHistoryRecord> original = <GameHistoryRecord>[
          const GameHistoryLineBaseRecord(
            outboxSequence: 1,
            boardGameId: _gameId,
            lineRevision: 1,
            variant: GameVariant.classic,
            position: _initial,
          ),
          const GameHistorySealRecord(
            outboxSequence: 2,
            boardGameId: _gameId,
            lineRevision: 2,
            status: GameArchiveStatus.unfinished,
            outcome: GameOutcome.ongoing,
            winner: GameWinner.none,
          ),
        ];
        await receive(original);
        final Map<String, Object?> before = (await repository.load())
            .games
            .single
            .toJson();
        final Map<String, Object> cursorBefore = (await cursorStore.load(
          'paired-board-1',
        )).cursor!.toJson();

        await expectLater(
          receive(<GameHistoryRecord>[
            if (appendMove)
              const GameHistoryCommittedPlyRecord(
                outboxSequence: 3,
                boardGameId: _gameId,
                lineRevision: 3,
                position: _afterE4,
                transition: _e4,
              )
            else
              const GameHistoryLineBaseRecord(
                outboxSequence: 3,
                boardGameId: _gameId,
                lineRevision: 3,
                variant: GameVariant.classic,
                position: _initial,
              ),
          ]),
          throwsFormatException,
        );

        // Reload the actual files: a rejected segment must not grant an ACK
        // that could allow firmware to discard the only retained history.
        expect(
          (await GameLibraryStore(
            Directory('${root.path}/library'),
          ).load()).games.single.toJson(),
          before,
        );
        expect(
          (await cursorStore.load('paired-board-1')).cursor!.toJson(),
          cursorBefore,
        );
        expect((await receive(original)).alreadyDurable, isTrue);
      },
    );
  }

  test(
    'newer terminal Undo resumes a completed game with the same identity',
    () async {
      Future<GameHistoryDurableSegmentResult> receive(
        List<GameHistoryRecord> records,
      ) {
        final _EncodedSegment segment = _segment(records);
        return processor.process(
          boardIdentity: 'paired-board-1',
          journalId: '0102030405060708',
          descriptor: segment.descriptor,
          bytes: segment.bytes,
          observedAt: DateTime.utc(2026, 9, 8),
        );
      }

      await receive(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: _gameId,
          lineRevision: 2,
          position: _afterE4,
          transition: _e4,
        ),
        GameHistorySealRecord(
          outboxSequence: 3,
          boardGameId: _gameId,
          lineRevision: 2,
          status: GameArchiveStatus.completed,
          outcome: GameOutcome.timeout,
          winner: GameWinner.white,
        ),
      ]);
      final GameArchive completed = (await repository.load()).games.single;
      expect(completed.status, GameArchiveStatus.completed);

      await receive(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 4,
          boardGameId: _gameId,
          lineRevision: 3,
          variant: GameVariant.classic,
          position: _initial,
        ),
      ]);
      final GameArchive resumed = (await repository.load()).games.single;
      expect(resumed.libraryId, completed.libraryId);
      expect(resumed.boardGameId, completed.boardGameId);
      expect(resumed.status, GameArchiveStatus.active);
      expect(resumed.outcome, GameOutcome.ongoing);
      expect(resumed.currentPly, 0);
      expect(resumed.transitions, isEmpty);
      expect((await cursorStore.load('paired-board-1')).cursor!.durableAck, 4);
    },
  );

  test('seal-only continuation finalizes the durable local line', () async {
    await importer.importRecords(const <GameHistoryRecord>[
      GameHistoryLineBaseRecord(
        outboxSequence: 1,
        boardGameId: _gameId,
        lineRevision: 1,
        variant: GameVariant.classic,
        position: _initial,
      ),
    ], observedAt: DateTime.utc(2026, 9, 3));

    final GameArchive sealed = (await importer
        .importRecords(const <GameHistoryRecord>[
          GameHistorySealRecord(
            outboxSequence: 2,
            boardGameId: _gameId,
            lineRevision: 2,
            status: GameArchiveStatus.unfinished,
            outcome: GameOutcome.ongoing,
            winner: GameWinner.none,
          ),
        ], observedAt: DateTime.utc(2026, 9, 3, 0, 1)))!;

    expect(sealed.status, GameArchiveStatus.unfinished);
    expect(sealed.positions.single.fen, _initial.fen);
  });

  test(
    'ACKed committed-ply overlap is checked and ignored as a transition',
    () async {
      await importer.importRecords(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: _gameId,
          lineRevision: 2,
          position: _afterE4,
          transition: _e4,
        ),
      ], observedAt: DateTime.utc(2026, 9, 3));
      await cursorStore.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 2,
        ),
      );
      final _EncodedSegment overlap = _segment(const <GameHistoryRecord>[
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: _gameId,
          lineRevision: 2,
          position: _afterE4,
          transition: _e4,
        ),
        GameHistorySealRecord(
          outboxSequence: 3,
          boardGameId: _gameId,
          lineRevision: 3,
          status: GameArchiveStatus.unfinished,
          outcome: GameOutcome.ongoing,
          winner: GameWinner.none,
        ),
      ], flags: gameHistorySegmentFlagAcknowledgedOverlap);

      final GameHistoryDurableSegmentResult result = await processor.process(
        boardIdentity: 'paired-board-1',
        journalId: '0102030405060708',
        descriptor: overlap.descriptor,
        bytes: overlap.bytes,
        observedAt: DateTime.utc(2026, 9, 3, 0, 1),
      );

      expect(result.archive?.status, GameArchiveStatus.unfinished);
      expect(result.archive?.transitions, hasLength(1));
      expect(result.archive?.positions, hasLength(2));
      expect(result.commit.lastOutboxSequence, 3);
    },
  );

  for (final String scenario in <String>['matching', 'changed', 'missing']) {
    test(
      'acknowledged line-base overlap $scenario validates durable position',
      () async {
        const GameHistoryLineBaseRecord original = GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        );
        if (scenario != 'missing') {
          await importer.importRecords(<GameHistoryRecord>[
            original,
          ], observedAt: DateTime.utc(2026, 9, 7));
        }
        await cursorStore.save(
          'paired-board-1',
          const GameHistorySyncCursor(
            journalId: '0102030405060708',
            durableAck: 1,
          ),
        );
        final GameHistoryLineBaseRecord overlapBase = GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: scenario == 'changed'
              ? ArchivedPosition(
                  ply: 0,
                  fen: _initial.fen.replaceFirst('0 1', '0 2'),
                )
              : _initial,
        );
        final _EncodedSegment overlap = _segment(<GameHistoryRecord>[
          overlapBase,
          const GameHistorySealRecord(
            outboxSequence: 2,
            boardGameId: _gameId,
            lineRevision: 2,
            status: GameArchiveStatus.unfinished,
            outcome: GameOutcome.ongoing,
            winner: GameWinner.none,
          ),
        ], flags: gameHistorySegmentFlagAcknowledgedOverlap);
        final Future<GameHistoryDurableSegmentResult> processing = processor
            .process(
              boardIdentity: 'paired-board-1',
              journalId: '0102030405060708',
              descriptor: overlap.descriptor,
              bytes: overlap.bytes,
              observedAt: DateTime.utc(2026, 9, 7, 1),
            );
        if (scenario == 'matching') {
          expect((await processing).commit.lastOutboxSequence, 2);
        } else {
          await expectLater(processing, throwsFormatException);
          expect(
            (await cursorStore.load('paired-board-1')).cursor?.durableAck,
            1,
          );
          final List<GameArchive> games = (await repository.load()).games;
          if (scenario == 'missing') {
            expect(games, isEmpty);
          } else {
            expect(games.single.positions.single.fen, _initial.fen);
            expect(games.single.status, GameArchiveStatus.active);
          }
        }
      },
    );
  }

  test(
    'cursor write failure cannot produce commit and replay repairs cursor',
    () async {
      final _EncodedSegment segment = _segment(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
      ]);
      final File blocker = File('${root.path}/cursors');
      await blocker.writeAsString('not a directory');
      Future<GameHistoryDurableSegmentResult> process() => processor.process(
        boardIdentity: 'paired-board-1',
        journalId: '0102030405060708',
        descriptor: segment.descriptor,
        bytes: segment.bytes,
        observedAt: DateTime.utc(2026, 9, 7),
      );
      await expectLater(process(), throwsA(isA<FileSystemException>()));
      expect(
        (await GameLibraryStore(
          Directory('${root.path}/library'),
        ).load()).games,
        hasLength(1),
      );
      expect((await cursorStore.load('paired-board-1')).cursor, isNull);
      await blocker.delete();
      expect((await process()).commit.lastOutboxSequence, 1);
      expect((await repository.load()).games, hasLength(1));
      expect((await cursorStore.load('paired-board-1')).cursor?.durableAck, 1);
    },
  );

  for (final bool changed in <bool>[true, false]) {
    test(
      'durable replay ${changed ? 'rejects changed bytes' : 'survives intentional archive deletion'}',
      () async {
        final _EncodedSegment original = _segment(const <GameHistoryRecord>[
          GameHistoryLineBaseRecord(
            outboxSequence: 1,
            boardGameId: _gameId,
            lineRevision: 1,
            variant: GameVariant.classic,
            position: _initial,
          ),
        ]);
        Future<GameHistoryDurableSegmentResult> receive(
          _EncodedSegment segment,
        ) => processor.process(
          boardIdentity: 'paired-board-1',
          journalId: '0102030405060708',
          descriptor: segment.descriptor,
          bytes: segment.bytes,
          observedAt: DateTime.utc(2026, 9, 7),
        );
        await receive(original);
        final GameArchive saved = (await repository.load()).games.single;
        if (changed) {
          final _EncodedSegment replay = _segment(<GameHistoryRecord>[
            GameHistoryLineBaseRecord(
              outboxSequence: 1,
              boardGameId: _gameId,
              lineRevision: 2,
              variant: GameVariant.classic,
              position: ArchivedPosition(
                ply: 0,
                fen: _initial.fen.replaceFirst('0 1', '0 2'),
              ),
            ),
          ]);
          // CRC over a record including its CRC has a fixed residue; the local
          // SHA-256 receipt must still distinguish these different valid bytes.
          expect(replay.descriptor.crc32, original.descriptor.crc32);
          expect(replay.bytes, isNot(original.bytes));
          await expectLater(receive(replay), throwsFormatException);
          expect(
            (await repository.load()).games.single.toJson(),
            saved.toJson(),
          );
        } else {
          await repository.delete(saved.libraryId);
          expect((await receive(original)).alreadyDurable, isTrue);
          expect((await repository.load()).games, isEmpty);
        }
        expect(
          (await cursorStore.load('paired-board-1')).cursor?.durableAck,
          1,
        );
      },
    );
  }

  test(
    'legacy watermark cannot authorize a duplicate without a receipt',
    () async {
      await cursorStore.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 1,
        ),
      );
      final _EncodedSegment segment = _segment(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
      ]);
      await expectLater(
        processor.process(
          boardIdentity: 'paired-board-1',
          journalId: '0102030405060708',
          descriptor: segment.descriptor,
          bytes: segment.bytes,
          observedAt: DateTime.utc(2026, 9, 7),
        ),
        throwsFormatException,
      );
      expect((await repository.load()).games, isEmpty);
      expect((await cursorStore.load('paired-board-1')).cursor?.durableAck, 1);
    },
  );

  test(
    'archive write failure leaves cursor absent and permits retry',
    () async {
      final File blocker = File('${root.path}/library');
      await blocker.writeAsString('not a directory');
      final _EncodedSegment segment = _segment(const <GameHistoryRecord>[
        GameHistoryLineBaseRecord(
          outboxSequence: 1,
          boardGameId: _gameId,
          lineRevision: 1,
          variant: GameVariant.classic,
          position: _initial,
        ),
      ]);
      Future<GameHistoryDurableSegmentResult> receive() => processor.process(
        boardIdentity: 'paired-board-1',
        journalId: '0102030405060708',
        descriptor: segment.descriptor,
        bytes: segment.bytes,
        observedAt: DateTime.utc(2026, 9, 7),
      );
      await expectLater(receive(), throwsA(isA<FileSystemException>()));
      expect((await cursorStore.load('paired-board-1')).cursor, isNull);
      await blocker.delete();
      expect((await receive()).commit.lastOutboxSequence, 1);
      expect((await repository.load()).games, hasLength(1));
    },
  );

  for (final String context in <String>[
    'missing',
    'wrong',
    'continuation',
    'overlap',
    'matching',
  ]) {
    test('retained-gap handoff $context validates its declared base', () async {
      await cursorStore.save(
        'paired-board-1',
        const GameHistorySyncCursor(
          journalId: '0102030405060708',
          durableAck: 40,
        ),
      );
      final _EncodedSegment segment = _segment(
        <GameHistoryRecord>[
          if (context == 'continuation')
            const GameHistoryCommittedPlyRecord(
              outboxSequence: 50,
              boardGameId: _gameId,
              lineRevision: 2,
              position: _afterE4,
              transition: _e4,
            )
          else
            const GameHistoryLineBaseRecord(
              outboxSequence: 50,
              boardGameId: _gameId,
              lineRevision: 1,
              variant: GameVariant.classic,
              position: _initial,
            ),
        ],
        flags: context == 'overlap'
            ? gameHistorySegmentFlagAcknowledgedOverlap
            : 0,
      );
      final Future<GameHistoryDurableSegmentResult> result = processor.process(
        boardIdentity: 'paired-board-1',
        journalId: '0102030405060708',
        descriptor: segment.descriptor,
        bytes: segment.bytes,
        observedAt: DateTime.utc(2026, 9, 7),
        retainedStartAfterGap: context == 'missing'
            ? null
            : context == 'wrong'
            ? 49
            : 50,
      );
      if (context == 'matching') {
        expect((await result).commit.lastOutboxSequence, 50);
        expect(
          (await cursorStore.load('paired-board-1')).cursor?.durableAck,
          50,
        );
      } else {
        await expectLater(result, throwsFormatException);
        expect((await repository.load()).games, isEmpty);
        expect(
          (await cursorStore.load('paired-board-1')).cursor?.durableAck,
          40,
        );
      }
    });
  }

  test('continuation without a durable base fails closed', () async {
    await expectLater(
      importer.importRecords(const <GameHistoryRecord>[
        GameHistoryCommittedPlyRecord(
          outboxSequence: 2,
          boardGameId: _gameId,
          lineRevision: 2,
          position: _afterE4,
          transition: _e4,
        ),
      ], observedAt: DateTime.utc(2026, 9, 3)),
      throwsFormatException,
    );
    expect((await repository.load()).games, isEmpty);
  });
}

class _EncodedSegment {
  const _EncodedSegment(this.bytes, this.descriptor);

  final Uint8List bytes;
  final GameHistorySegmentDescriptor descriptor;
}

_EncodedSegment _segment(List<GameHistoryRecord> records, {int flags = 0}) {
  final Uint8List bytes = Uint8List.fromList(<int>[
    for (final GameHistoryRecord record in records)
      ...GameHistoryCodec.encodeRecord(record),
  ]);
  return _EncodedSegment(
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
