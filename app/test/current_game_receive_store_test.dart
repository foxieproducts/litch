import 'dart:io';
import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/current_game_receive_store.dart';
import 'package:litch_mobile/src/services/current_game_sync_protocol.dart';
import 'package:litch_mobile/src/services/game_history_protocol.dart';
import 'package:litch_mobile/src/services/game_history_segment_importer.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

const String _boardIdentity = 'paired-board-1';
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

void main() {
  late Directory root;
  late Directory receiveDirectory;
  late GameLibraryRepository repository;
  late GameHistorySegmentImporter importer;
  late CurrentGameReceiveStore receiveStore;

  const GameHistoryLineBaseRecord base = GameHistoryLineBaseRecord(
    outboxSequence: 1,
    boardGameId: _gameId,
    lineRevision: 1,
    variant: GameVariant.classic,
    position: _initial,
  );
  const GameHistoryCommittedPlyRecord move = GameHistoryCommittedPlyRecord(
    outboxSequence: 2,
    boardGameId: _gameId,
    lineRevision: 2,
    position: _afterE4,
    transition: ArchivedTransition(side: GameSide.white, from: 52, to: 36),
  );

  setUp(() async {
    root = await Directory.systemTemp.createTemp('litch-current-receive-');
    receiveDirectory = Directory('${root.path}/receive');
    repository = GameLibraryRepository(
      GameLibraryStore(Directory('${root.path}/library')),
    );
    importer = GameHistorySegmentImporter(repository);
    receiveStore = CurrentGameReceiveStore(
      directory: receiveDirectory,
      importer: importer,
    );
  });

  tearDown(() async {
    await repository.close();
    if (await root.exists()) await root.delete(recursive: true);
  });

  test(
    'same-board identity replacement leaves only that board current game active after reload',
    () async {
      const replacementId = '101112131415161718191a1b1c1d1e1f';
      const otherBoardGameId = '202122232425262728292a2b2c2d2e2f';
      await _offer(receiveStore, _boardIdentity, _playedLine(_gameId));
      await _offer(
        receiveStore,
        'paired-board-2',
        _playedLine(otherBoardGameId),
      );
      await _offer(receiveStore, _boardIdentity, _playedLine(replacementId));
      final reopened = GameLibraryRepository(
        GameLibraryStore(Directory('${root.path}/library')),
      );
      addTearDown(reopened.close);
      final durable = await reopened.load();
      expect(durable.games, hasLength(3));
      final previous = durable.games.singleWhere(
        (game) => game.boardGameId == _gameId,
      );
      expect(previous.status, GameArchiveStatus.unfinished);
      expect(previous.positions.map((position) => position.fen), [
        _initial.fen,
        _afterE4.fen,
      ]);
      expect(
        durable.games
            .singleWhere((game) => game.boardGameId == replacementId)
            .status,
        GameArchiveStatus.active,
      );
      expect(
        durable.games
            .singleWhere((game) => game.boardGameId == otherBoardGameId)
            .status,
        GameArchiveStatus.active,
      );
    },
  );

  test(
    'partial replacement preserves the completed board association across restart',
    () async {
      const replacementId = '303132333435363738393a3b3c3d3e3f';
      await _offer(receiveStore, _boardIdentity, _playedLine(_gameId));
      final records = _playedLine(replacementId);
      final all = _segment(records);
      final first = _segment(records.take(1).toList());
      await receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: _digest(all.bytes),
        bounds: _bounds(records),
        descriptor: first.descriptor,
        bytes: first.bytes,
        observedAt: DateTime.utc(2026, 10, 26),
        restartFromBeginning: true,
      );
      expect(
        (await repository.findByBoardGameId(_gameId))!.status,
        GameArchiveStatus.active,
      );
      expect(await repository.findByBoardGameId(replacementId), isNull);
      final metadata =
          jsonDecode(
                await File(
                  '${receiveDirectory.path}/current-game-$_boardIdentity.json',
                ).readAsString(),
              )
              as Map;
      expect(metadata['completedArchiveGameId'], _gameId);
      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      final last = _segment(records.skip(1).toList());
      await receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: _digest(all.bytes),
        bounds: _bounds(records),
        descriptor: last.descriptor,
        bytes: last.bytes,
        observedAt: DateTime.utc(2026, 10, 26),
      );
      expect(
        (await repository.findByBoardGameId(_gameId))!.status,
        GameArchiveStatus.unfinished,
      );
      expect(
        (await repository.findByBoardGameId(replacementId))!.status,
        GameArchiveStatus.active,
      );
    },
  );

  test(
    'C durable then receipt failure restarts into D without orphaning C or changing another board or terminal game',
    () async {
      const c = '404142434445464748494a4b4c4d4e4f';
      const d = '505152535455565758595a5b5c5d5e5f';
      const other = '606162636465666768696a6b6c6d6e6f';
      const terminal = '707172737475767778797a7b7c7d7e7f';
      await repository.close();
      final blocked = Directory(
        '${receiveDirectory.path}/current-game-$_boardIdentity.json.tmp',
      );
      final failingStore = _ReceiptFailureStore(
        Directory('${root.path}/library'),
        blocked,
        c,
      );
      repository = GameLibraryRepository(failingStore);
      importer = GameHistorySegmentImporter(repository);
      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      await _offer(
        receiveStore,
        _boardIdentity,
        _playedLine(terminal, completed: true),
      );
      await _offer(receiveStore, _boardIdentity, _playedLine(_gameId));
      await _offer(receiveStore, 'paired-board-2', _playedLine(other));
      final terminalFile = File(
        '${root.path}/library/game-board-$terminal.json',
      );
      final terminalBytes = await terminalFile.readAsBytes();
      failingStore.armed = true;
      await expectLater(
        _offer(receiveStore, _boardIdentity, _playedLine(c)),
        throwsA(isA<FileSystemException>()),
      );
      expect(
        (await repository.findByBoardGameId(c))!.status,
        GameArchiveStatus.active,
      );
      final pending =
          jsonDecode(
                await File(
                  '${receiveDirectory.path}/current-game-$_boardIdentity.json',
                ).readAsString(),
              )
              as Map;
      expect(pending['completedArchiveGameId'], _gameId);
      expect((pending['pendingCompleteImport'] as Map)['gameId'], c);
      // C is already durable. Its identity association must survive loss of
      // staging rather than reconstructing an archive from cached wire bytes.
      await File(
        '${receiveDirectory.path}/current-game-$_boardIdentity.bin',
      ).delete();
      await repository.close();
      await blocked.delete();
      repository = GameLibraryRepository(
        GameLibraryStore(Directory('${root.path}/library')),
      );
      importer = GameHistorySegmentImporter(repository);
      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      // No replay of C is requested by this test. The receive owner must finish
      // its pending receipt despite the missing staged bytes before accepting D.
      await _offer(receiveStore, _boardIdentity, _playedLine(d));
      final games = (await repository.load()).games;
      expect(
        games.singleWhere((game) => game.boardGameId == _gameId).status,
        GameArchiveStatus.unfinished,
      );
      expect(
        games.singleWhere((game) => game.boardGameId == c).status,
        GameArchiveStatus.unfinished,
      );
      expect(
        games.singleWhere((game) => game.boardGameId == d).status,
        GameArchiveStatus.active,
      );
      expect(
        games.singleWhere((game) => game.boardGameId == other).status,
        GameArchiveStatus.active,
      );
      expect(
        games.singleWhere((game) => game.boardGameId == terminal).status,
        GameArchiveStatus.completed,
      );
      expect(await terminalFile.readAsBytes(), terminalBytes);
      final settled =
          jsonDecode(
                await File(
                  '${receiveDirectory.path}/current-game-$_boardIdentity.json',
                ).readAsString(),
              )
              as Map;
      expect(settled['completedArchiveGameId'], d);
      expect(settled.containsKey('pendingCompleteImport'), isFalse);
    },
  );

  for (final legacy in ['matching', 'partial', 'different archive revision']) {
    test(
      'legacy $legacy cursor seeds only a proven completed association',
      () async {
        const replacement = '808182838485868788898a8b8c8d8e8f';
        await _offer(receiveStore, _boardIdentity, _playedLine(_gameId));
        final file = File(
          '${receiveDirectory.path}/current-game-$_boardIdentity.json',
        );
        final metadata = jsonDecode(await file.readAsString()) as Map;
        final cursor = Map<String, dynamic>.from(metadata['cursor'] as Map);
        if (legacy == 'partial') {
          final first = _segment(_playedLine(_gameId).take(1).toList());
          cursor['acceptedBytes'] = first.bytes.length;
          cursor['nextRecordSequence'] = 2;
          cursor['prefixSha256'] = sha256.convert(first.bytes).toString();
        } else if (legacy == 'different archive revision') {
          await repository.mergeAuthoritativeSegment(
            AuthoritativeLineSegment(
              boardGameId: _gameId,
              lineRevision: 3,
              status: GameArchiveStatus.active,
              variant: GameVariant.classic,
              outcome: GameOutcome.ongoing,
              winner: GameWinner.none,
              positions: [_initial, _afterE4],
              transitions: [
                const ArchivedTransition(
                  side: GameSide.white,
                  from: 52,
                  to: 36,
                ),
              ],
            ),
            observedAt: DateTime.utc(2026, 10, 26),
          );
        }
        await file.writeAsString(jsonEncode(cursor), flush: true);
        final previous = File('${file.path}.previous');
        if (await previous.exists()) await previous.delete();
        receiveStore = CurrentGameReceiveStore(
          directory: receiveDirectory,
          importer: importer,
        );
        await _offer(receiveStore, _boardIdentity, _playedLine(replacement));
        expect(
          (await repository.findByBoardGameId(_gameId))!.status,
          legacy == 'matching'
              ? GameArchiveStatus.unfinished
              : GameArchiveStatus.active,
        );
      },
    );
  }

  test(
    'resumes a durable prefix and acknowledges only after archive save',
    () async {
      final _EncodedSegment baseSegment = _segment(<GameHistoryRecord>[base]);
      final _EncodedSegment moveSegment = _segment(<GameHistoryRecord>[move]);
      final Uint8List completeBytes = Uint8List.fromList(<int>[
        ...baseSegment.bytes,
        ...moveSegment.bytes,
      ]);
      final CurrentGameDigest digest = _digest(completeBytes);
      const CurrentGameBounds bounds = CurrentGameBounds(
        lineRevision: 2,
        recordCount: 2,
        flags: currentGameSyncPresentFlag | currentGameSyncArchiveRequiredFlag,
      );

      final CurrentGameAcceptedSegment partial = await receiveStore
          .acceptSegment(
            boardIdentity: _boardIdentity,
            advertisedDigest: digest,
            bounds: bounds,
            descriptor: baseSegment.descriptor,
            bytes: baseSegment.bytes,
            observedAt: DateTime.utc(2026, 9, 11),
          );
      expect(partial.complete, isFalse);
      expect(partial.cursor.nextRecordSequence, 2);
      expect(
        partial.cursor.prefixFingerprint,
        sha256.convert(baseSegment.bytes).bytes.take(8),
      );
      expect((await repository.load()).games, isEmpty);

      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      expect(
        (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
        2,
      );
      final CurrentGameAcceptedSegment complete = await receiveStore
          .acceptSegment(
            boardIdentity: _boardIdentity,
            advertisedDigest: digest,
            bounds: bounds,
            descriptor: moveSegment.descriptor,
            bytes: moveSegment.bytes,
            observedAt: DateTime.utc(2026, 9, 11),
          );

      expect(complete.complete, isTrue);
      expect(complete.archive?.currentPly, 1);
      expect(complete.archiveAcknowledgement?.fingerprint, digest.fingerprint);
      expect(complete.cursor.nextRecordSequence, 3);
      expect((await repository.load()).games.single.currentPly, 1);

      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      final CurrentGameAcceptedSegment? replayedAck = await receiveStore
          .reconcileComplete(
            boardIdentity: _boardIdentity,
            advertisedDigest: digest,
            bounds: bounds,
            observedAt: DateTime.utc(2026, 9, 12),
          );
      expect(replayedAck?.alreadyDurable, isTrue);
      expect(
        replayedAck?.archiveAcknowledgement?.fingerprint,
        digest.fingerprint,
      );
      expect(replayedAck?.cursor.nextRecordSequence, 3);
    },
  );

  test(
    'exact record count completes an arbitrary-FEN base at nonzero ply',
    () async {
      const GameHistoryLineBaseRecord fenBase = GameHistoryLineBaseRecord(
        outboxSequence: 1,
        boardGameId: '100102030405060708090a0b0c0d0e0f',
        lineRevision: 1,
        variant: GameVariant.classic,
        position: ArchivedPosition(
          ply: 42,
          fen: '8/8/8/8/8/8/8/K6k w - - 0 22',
        ),
      );
      final _EncodedSegment segment = _segment(<GameHistoryRecord>[fenBase]);
      final CurrentGameAcceptedSegment result = await receiveStore
          .acceptSegment(
            boardIdentity: _boardIdentity,
            advertisedDigest: _digest(segment.bytes),
            bounds: const CurrentGameBounds(
              lineRevision: 1,
              recordCount: 1,
              flags:
                  currentGameSyncPresentFlag |
                  currentGameSyncArchiveRequiredFlag,
            ),
            descriptor: segment.descriptor,
            bytes: segment.bytes,
            observedAt: DateTime.utc(2026, 9, 11),
          );

      expect(result.complete, isTrue);
      expect(result.archive?.currentPly, 42);
      expect(result.archive?.captureQuality, GameCaptureQuality.partial);
    },
  );

  test('uncommitted raw suffix is truncated to the durable cursor', () async {
    final _EncodedSegment baseSegment = _segment(<GameHistoryRecord>[base]);
    final _EncodedSegment moveSegment = _segment(<GameHistoryRecord>[move]);
    final CurrentGameDigest digest = _digest(
      Uint8List.fromList(<int>[...baseSegment.bytes, ...moveSegment.bytes]),
    );
    const CurrentGameBounds bounds = CurrentGameBounds(
      lineRevision: 2,
      recordCount: 2,
      flags: currentGameSyncPresentFlag,
    );
    await receiveStore.acceptSegment(
      boardIdentity: _boardIdentity,
      advertisedDigest: digest,
      bounds: bounds,
      descriptor: baseSegment.descriptor,
      bytes: baseSegment.bytes,
      observedAt: DateTime.utc(2026, 9, 11),
    );
    final File data = File(
      '${receiveDirectory.path}/current-game-$_boardIdentity.bin',
    );
    await data.writeAsBytes(
      moveSegment.bytes,
      mode: FileMode.append,
      flush: true,
    );
    expect(
      await data.length(),
      baseSegment.bytes.length + moveSegment.bytes.length,
    );

    receiveStore = CurrentGameReceiveStore(
      directory: receiveDirectory,
      importer: importer,
    );
    expect(
      (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
      2,
    );
    expect(await data.length(), baseSegment.bytes.length);
  });

  test('metadata failure leaves no acknowledged suffix and recovers', () async {
    final _EncodedSegment baseSegment = _segment(<GameHistoryRecord>[base]);
    final _EncodedSegment moveSegment = _segment(<GameHistoryRecord>[move]);
    final _EncodedSegment sealSegment = _segment(const <GameHistoryRecord>[
      GameHistorySealRecord(
        outboxSequence: 3,
        boardGameId: _gameId,
        lineRevision: 3,
        status: GameArchiveStatus.unfinished,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
      ),
    ]);
    final CurrentGameDigest digest = _digest(
      Uint8List.fromList(<int>[
        ...baseSegment.bytes,
        ...moveSegment.bytes,
        ...sealSegment.bytes,
      ]),
    );
    const CurrentGameBounds bounds = CurrentGameBounds(
      lineRevision: 3,
      recordCount: 3,
      flags: currentGameSyncPresentFlag | currentGameSyncSealedFlag,
    );
    await receiveStore.acceptSegment(
      boardIdentity: _boardIdentity,
      advertisedDigest: digest,
      bounds: bounds,
      descriptor: baseSegment.descriptor,
      bytes: baseSegment.bytes,
      observedAt: DateTime.utc(2026, 9, 11),
    );
    final Directory blockedTemporary = Directory(
      '${receiveDirectory.path}/current-game-$_boardIdentity.json.tmp',
    );
    await blockedTemporary.create();

    await expectLater(
      receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: digest,
        bounds: bounds,
        descriptor: moveSegment.descriptor,
        bytes: moveSegment.bytes,
        observedAt: DateTime.utc(2026, 9, 11),
      ),
      throwsA(isA<FileSystemException>()),
    );
    if (await blockedTemporary.exists()) {
      await blockedTemporary.delete(recursive: true);
    }
    receiveStore = CurrentGameReceiveStore(
      directory: receiveDirectory,
      importer: importer,
    );
    expect(
      (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
      2,
    );
    expect((await repository.load()).games, isEmpty);
  });

  test('a missing cursor cannot admit a continuation segment', () async {
    final _EncodedSegment moveSegment = _segment(<GameHistoryRecord>[move]);
    await expectLater(
      receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: _digest(moveSegment.bytes),
        bounds: const CurrentGameBounds(
          lineRevision: 2,
          recordCount: 2,
          flags: currentGameSyncPresentFlag,
        ),
        descriptor: moveSegment.descriptor,
        bytes: moveSegment.bytes,
        observedAt: DateTime.utc(2026, 9, 11),
      ),
      throwsFormatException,
    );
    expect(await receiveDirectory.list().toList(), isEmpty);
  });

  test(
    'wrong final digest cannot advance the cursor or create an archive',
    () async {
      final _EncodedSegment baseSegment = _segment(<GameHistoryRecord>[base]);
      final _EncodedSegment moveSegment = _segment(<GameHistoryRecord>[move]);
      const CurrentGameBounds bounds = CurrentGameBounds(
        lineRevision: 2,
        recordCount: 2,
        flags: currentGameSyncPresentFlag,
      );
      await receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: _digest(
          Uint8List.fromList(<int>[...baseSegment.bytes, ...moveSegment.bytes]),
        ),
        bounds: bounds,
        descriptor: baseSegment.descriptor,
        bytes: baseSegment.bytes,
        observedAt: DateTime.utc(2026, 9, 11),
      );

      await expectLater(
        receiveStore.acceptSegment(
          boardIdentity: _boardIdentity,
          advertisedDigest: const CurrentGameDigest(
            fingerprint: <int>[1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1],
          ),
          bounds: bounds,
          descriptor: moveSegment.descriptor,
          bytes: moveSegment.bytes,
          observedAt: DateTime.utc(2026, 9, 11),
        ),
        throwsFormatException,
      );
      expect((await repository.load()).games, isEmpty);

      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      expect(
        (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
        2,
      );
    },
  );

  test(
    'archive is durable before cursor promotion and replay finishes it',
    () async {
      final _EncodedSegment baseSegment = _segment(<GameHistoryRecord>[base]);
      final _EncodedSegment moveSegment = _segment(<GameHistoryRecord>[move]);
      final Uint8List completeBytes = Uint8List.fromList(<int>[
        ...baseSegment.bytes,
        ...moveSegment.bytes,
      ]);
      final CurrentGameDigest digest = _digest(completeBytes);
      const CurrentGameBounds bounds = CurrentGameBounds(
        lineRevision: 2,
        recordCount: 2,
        flags: currentGameSyncPresentFlag,
      );
      await receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: digest,
        bounds: bounds,
        descriptor: baseSegment.descriptor,
        bytes: baseSegment.bytes,
        observedAt: DateTime.utc(2026, 9, 11),
      );
      final Directory blockedTemporary = Directory(
        '${receiveDirectory.path}/current-game-$_boardIdentity.json.tmp',
      );
      await repository.close();
      final failingStore = _ReceiptFailureStore(
        Directory('${root.path}/library'),
        blockedTemporary,
        _gameId,
      )..armed = true;
      repository = GameLibraryRepository(failingStore);
      importer = GameHistorySegmentImporter(repository);
      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      await expectLater(
        receiveStore.acceptSegment(
          boardIdentity: _boardIdentity,
          advertisedDigest: digest,
          bounds: bounds,
          descriptor: moveSegment.descriptor,
          bytes: moveSegment.bytes,
          observedAt: DateTime.utc(2026, 9, 11),
        ),
        throwsA(isA<FileSystemException>()),
      );
      expect((await repository.load()).games.single.currentPly, 1);
      await expectLater(
        receiveStore.loadCursor(_boardIdentity),
        throwsA(isA<FileSystemException>()),
      );
      if (await blockedTemporary.exists()) {
        await blockedTemporary.delete(recursive: true);
      }
      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      final CurrentGameAcceptedSegment recovered = await receiveStore
          .acceptSegment(
            boardIdentity: _boardIdentity,
            advertisedDigest: digest,
            bounds: bounds,
            descriptor: moveSegment.descriptor,
            bytes: moveSegment.bytes,
            observedAt: DateTime.utc(2026, 9, 11),
          );
      expect(recovered.complete, isTrue);
      expect((await repository.load()).games.single.currentPly, 1);
    },
  );

  test(
    'rejected unfinished revision never promotes its receive cursor',
    () async {
      const GameHistorySealRecord unfinished = GameHistorySealRecord(
        outboxSequence: 2,
        boardGameId: _gameId,
        lineRevision: 1,
        status: GameArchiveStatus.unfinished,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
      );
      final _EncodedSegment original = _segment(<GameHistoryRecord>[
        base,
        unfinished,
      ]);
      await receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: _digest(original.bytes),
        bounds: const CurrentGameBounds(
          lineRevision: 1,
          recordCount: 2,
          flags: currentGameSyncPresentFlag | currentGameSyncSealedFlag,
        ),
        descriptor: original.descriptor,
        bytes: original.bytes,
        observedAt: DateTime.utc(2026, 9, 11),
      );
      expect(
        (await repository.load()).games.single.status,
        GameArchiveStatus.unfinished,
      );

      const GameHistoryLineBaseRecord rejected = GameHistoryLineBaseRecord(
        outboxSequence: 1,
        boardGameId: _gameId,
        lineRevision: 2,
        variant: GameVariant.classic,
        position: _initial,
      );
      final _EncodedSegment replacement = _segment(<GameHistoryRecord>[
        rejected,
      ]);
      await expectLater(
        receiveStore.acceptSegment(
          boardIdentity: _boardIdentity,
          advertisedDigest: _digest(replacement.bytes),
          bounds: const CurrentGameBounds(
            lineRevision: 2,
            recordCount: 1,
            flags: currentGameSyncPresentFlag,
          ),
          descriptor: replacement.descriptor,
          bytes: replacement.bytes,
          observedAt: DateTime.utc(2026, 9, 11),
        ),
        throwsFormatException,
      );

      receiveStore = CurrentGameReceiveStore(
        directory: receiveDirectory,
        importer: importer,
      );
      expect(await receiveStore.loadCursor(_boardIdentity), isNull);
      expect((await repository.load()).games.single.lineRevision, 1);
    },
  );

  test(
    'authoritative record-one restart retires an obsolete matching prefix',
    () async {
      final _EncodedSegment original = _segment(<GameHistoryRecord>[
        base,
        move,
      ]);
      await receiveStore.acceptSegment(
        boardIdentity: _boardIdentity,
        advertisedDigest: _digest(original.bytes),
        bounds: const CurrentGameBounds(
          lineRevision: 2,
          recordCount: 2,
          flags: currentGameSyncPresentFlag,
        ),
        descriptor: original.descriptor,
        bytes: original.bytes,
        observedAt: DateTime.utc(2026, 9, 11),
      );
      expect(
        (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
        3,
      );

      final _EncodedSegment matchingBase = _segment(<GameHistoryRecord>[base]);
      final Uint8List replacementBytes = Uint8List.fromList(<int>[
        ...matchingBase.bytes,
        ..._segment(<GameHistoryRecord>[
          const GameHistoryCommittedPlyRecord(
            outboxSequence: 2,
            boardGameId: _gameId,
            lineRevision: 3,
            position: _afterE4,
            transition: ArchivedTransition(
              side: GameSide.white,
              from: 51,
              to: 35,
            ),
          ),
        ]).bytes,
      ]);
      final CurrentGameAcceptedSegment restarted = await receiveStore
          .acceptSegment(
            boardIdentity: _boardIdentity,
            advertisedDigest: _digest(replacementBytes),
            bounds: const CurrentGameBounds(
              lineRevision: 3,
              recordCount: 2,
              flags: currentGameSyncPresentFlag,
            ),
            descriptor: matchingBase.descriptor,
            bytes: matchingBase.bytes,
            observedAt: DateTime.utc(2026, 9, 11),
            restartFromBeginning: true,
          );

      expect(restarted.complete, isFalse);
      expect(restarted.cursor.nextRecordSequence, 2);
      expect(
        (await receiveStore.loadCursor(_boardIdentity))?.nextRecordSequence,
        2,
      );
    },
  );
}

class _ReceiptFailureStore extends GameLibraryStore {
  _ReceiptFailureStore(super.directory, this.blocked, this.gameId);
  final Directory blocked;
  final String gameId;
  bool armed = false;

  @override
  Future<void> save(GameArchive archive) async {
    await super.save(archive);
    if (armed && archive.boardGameId == gameId) {
      armed = false;
      await blocked.create();
    }
  }
}

List<GameHistoryRecord> _playedLine(String id, {bool completed = false}) => [
  GameHistoryLineBaseRecord(
    outboxSequence: 1,
    boardGameId: id,
    lineRevision: 1,
    variant: GameVariant.classic,
    position: _initial,
  ),
  GameHistoryCommittedPlyRecord(
    outboxSequence: 2,
    boardGameId: id,
    lineRevision: 2,
    position: _afterE4,
    transition: const ArchivedTransition(
      side: GameSide.white,
      from: 52,
      to: 36,
    ),
  ),
  if (completed)
    GameHistorySealRecord(
      outboxSequence: 3,
      boardGameId: id,
      lineRevision: 3,
      status: GameArchiveStatus.completed,
      outcome: GameOutcome.checkmate,
      winner: GameWinner.white,
    ),
];

CurrentGameBounds _bounds(List<GameHistoryRecord> records) => CurrentGameBounds(
  lineRevision: records.last.lineRevision,
  recordCount: records.length,
  flags:
      currentGameSyncPresentFlag |
      currentGameSyncArchiveRequiredFlag |
      (records.last is GameHistorySealRecord ? currentGameSyncSealedFlag : 0),
);

Future<CurrentGameAcceptedSegment> _offer(
  CurrentGameReceiveStore receive,
  String board,
  List<GameHistoryRecord> records,
) {
  final encoded = _segment(records);
  return receive.acceptSegment(
    boardIdentity: board,
    advertisedDigest: _digest(encoded.bytes),
    bounds: _bounds(records),
    descriptor: encoded.descriptor,
    bytes: encoded.bytes,
    observedAt: DateTime.utc(2026, 10, 26),
    restartFromBeginning: true,
  );
}

class _EncodedSegment {
  const _EncodedSegment(this.descriptor, this.bytes);

  final GameHistorySegmentDescriptor descriptor;
  final Uint8List bytes;
}

_EncodedSegment _segment(List<GameHistoryRecord> records) {
  final Uint8List bytes = Uint8List.fromList(<int>[
    for (final GameHistoryRecord record in records)
      ...GameHistoryCodec.encodeRecord(record),
  ]);
  return _EncodedSegment(
    GameHistorySegmentDescriptor(
      encodedBytes: bytes.length,
      recordCount: records.length,
      flags: 0,
      firstOutboxSequence: records.first.outboxSequence,
      crc32: GameHistoryCrc32.compute(bytes),
    ),
    bytes,
  );
}

CurrentGameDigest _digest(Uint8List bytes) => CurrentGameDigest(
  fingerprint: sha256.convert(bytes).bytes.take(12).toList(growable: false),
);
