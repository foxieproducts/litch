import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/app_chess_game_session.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

void main() {
  test(
    'new app game owns identity, legal moves, and Fool mate result',
    () async {
      var now = DateTime.utc(2026, 9, 13, 2);
      final List<GameArchive> saved = <GameArchive>[];
      final AppChessGameSession session = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_game_1',
          libraryId: 'app-game-1',
        ),
        now: () => now,
        persist: (GameArchive archive) async => saved.add(archive),
      );
      await session.settlePersistence();
      expect(session.legalUciMovesFrom(52), <String>['e2e3', 'e2e4']);

      for (final String move in <String>['f2f3', 'e7e5', 'g2g4', 'd8h4']) {
        now = now.add(const Duration(seconds: 1));
        expect(session.playUci(move), AppChessMoveResult.accepted);
      }
      await session.settlePersistence();

      expect(session.archive.origin, GameArchiveOrigin.app);
      expect(session.archive.boardGameId, isNull);
      expect(session.archive.status, GameArchiveStatus.completed);
      expect(session.archive.outcome, GameOutcome.checkmate);
      expect(session.archive.winner, GameWinner.black);
      expect(session.archive.lineRevision, 4);
      expect(session.archive.positions, hasLength(5));
      expect(saved.last.toJson(), session.archive.toJson());
      expect(session.playUci('a2a3'), AppChessMoveResult.gameOver);
      session.dispose();
    },
  );

  test(
    'illegal move is a synchronous no-op with no persistence write',
    () async {
      final List<GameArchive> saved = <GameArchive>[];
      final AppChessGameSession session = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_game_2',
          libraryId: 'app-game-2',
        ),
        now: () => DateTime.utc(2026, 9, 13),
        persist: (GameArchive archive) async => saved.add(archive),
      );
      await session.settlePersistence();
      final Map<String, Object?> before = session.archive.toJson();
      final int writes = saved.length;

      expect(session.playUci('e2e5'), AppChessMoveResult.illegal);
      expect(session.playUci('not-a-move'), AppChessMoveResult.illegal);
      await session.settlePersistence();

      expect(session.archive.toJson(), before);
      expect(session.uciMoves, isEmpty);
      expect(saved, hasLength(writes));
      session.dispose();
    },
  );

  test(
    'castling, en passant, promotion, restore, and undo use one rules seam',
    () async {
      final List<GameArchive> saved = <GameArchive>[];
      final AppChessGameSession promotion = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_special_1',
          libraryId: 'app-special-1',
        ),
        initialFen: '4k3/P7/8/8/8/8/7p/4K3 w - - 0 1',
        now: () => DateTime.utc(2026, 9, 13),
        persist: (GameArchive archive) async => saved.add(archive),
      );
      expect(promotion.playUci('a7a8q'), AppChessMoveResult.accepted);
      expect(
        promotion.archive.transitions.single.promotion,
        PromotionPiece.queen,
      );
      await promotion.settlePersistence();

      final AppChessGameSession restored = AppChessGameSession.restore(
        archive: promotion.archive,
        now: () => DateTime.utc(2026, 9, 13, 0, 1),
        persist: (GameArchive archive) async => saved.add(archive),
      );
      expect(restored.uciMoves, <String>['a7a8q']);
      expect(restored.undo(), isTrue);
      expect(restored.archive.status, GameArchiveStatus.active);
      expect(restored.archive.gameId, 'app_special_1');
      expect(restored.archive.positions, hasLength(1));
      expect(restored.archive.lineRevision, 2);
      await restored.settlePersistence();

      final AppChessGameSession castle = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_special_2',
          libraryId: 'app-special-2',
        ),
        initialFen: 'r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1',
        now: () => DateTime.utc(2026, 9, 13),
        persist: (_) async {},
      );
      expect(castle.playUci('e1g1'), AppChessMoveResult.accepted);
      expect(castle.archive.transitions.single.secondaryFrom, 63);
      expect(castle.archive.transitions.single.secondaryTo, 61);

      final AppChessGameSession enPassant = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_special_3',
          libraryId: 'app-special-3',
        ),
        initialFen: '4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1',
        now: () => DateTime.utc(2026, 9, 13),
        persist: (_) async {},
      );
      expect(enPassant.playUci('e5d6'), AppChessMoveResult.accepted);
      expect(enPassant.archive.transitions.single.captureSquare, 27);

      promotion.dispose();
      restored.dispose();
      castle.dispose();
      enPassant.dispose();
    },
  );

  test(
    'persistence coalesces faster moves and retries the latest authority',
    () async {
      final Completer<void> firstWrite = Completer<void>();
      final List<GameArchive> writes = <GameArchive>[];
      var failNext = false;
      final AppChessGameSession session = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_game_3',
          libraryId: 'app-game-3',
        ),
        now: () => DateTime.utc(2026, 9, 13),
        persist: (GameArchive archive) async {
          writes.add(archive);
          if (writes.length == 1) await firstWrite.future;
          if (failNext) {
            failNext = false;
            throw StateError('disk unavailable');
          }
        },
      );

      expect(session.playUci('e2e4'), AppChessMoveResult.accepted);
      expect(session.playUci('e7e5'), AppChessMoveResult.accepted);
      firstWrite.complete();
      await session.settlePersistence();

      expect(writes, hasLength(2));
      expect(writes.last.currentPly, 2);

      failNext = true;
      expect(session.playUci('g1f3'), AppChessMoveResult.accepted);
      await session.settlePersistence();
      expect(session.saveFailed, isTrue);
      expect(session.archive.currentPly, 3);

      await session.retryPersistence();
      expect(session.saveFailed, isFalse);
      expect(writes.last.currentPly, 3);
      session.dispose();
    },
  );

  test('successful persistence publishes pending to idle', () async {
    final Completer<void> write = Completer<void>();
    final AppChessGameSession session = AppChessGameSession.newGame(
      identity: const AppChessGameIdentity(
        gameId: 'app_pending_game',
        libraryId: 'app-pending-game',
      ),
      now: () => DateTime.utc(2026, 9, 13),
      persist: (_) => write.future,
    );
    final List<bool> observedPending = <bool>[];
    session.addListener(() => observedPending.add(session.savePending));

    expect(session.savePending, isTrue);
    write.complete();
    await session.settlePersistence();

    expect(session.savePending, isFalse);
    expect(observedPending, contains(false));
    session.dispose();
  });

  test(
    'restore rejects unfinished and false active terminal archives',
    () async {
      final AppChessGameSession session = AppChessGameSession.newGame(
        identity: const AppChessGameIdentity(
          gameId: 'app_restore_truth',
          libraryId: 'app-restore-truth',
        ),
        now: () => DateTime.utc(2026, 9, 13),
        persist: (_) async {},
      );
      await session.settlePersistence();

      final GameArchive unfinished = _withLifecycle(
        session.archive,
        status: GameArchiveStatus.unfinished,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
      );
      expect(
        () => AppChessGameSession.restore(
          archive: unfinished,
          now: () => DateTime.utc(2026, 9, 13),
          persist: (_) async {},
        ),
        throwsFormatException,
      );

      for (final String move in <String>['f2f3', 'e7e5', 'g2g4', 'd8h4']) {
        expect(session.playUci(move), AppChessMoveResult.accepted);
      }
      final GameArchive falseActive = _withLifecycle(
        session.archive,
        status: GameArchiveStatus.active,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
      );
      expect(
        () => AppChessGameSession.restore(
          archive: falseActive,
          now: () => DateTime.utc(2026, 9, 13),
          persist: (_) async {},
        ),
        throwsFormatException,
      );
      session.dispose();
    },
  );

  test('generated identity is deterministic when random is injected', () {
    final AppChessGameIdentity first = AppChessGameIdentity.generate(Random(7));
    final AppChessGameIdentity second = AppChessGameIdentity.generate(
      Random(7),
    );

    expect(first.gameId, second.gameId);
    expect(first.libraryId, second.libraryId);
    expect(first.gameId, startsWith('app_'));
    expect(first.libraryId, startsWith('app-'));
    first.validate();
  });

  test(
    'legacy computer attribution restores and persists as two guests',
    () async {
      final List<GameArchive> saved = <GameArchive>[];
      final GameArchive legacy = GameArchive(
        libraryId: 'app-computer-restore',
        gameId: 'app_computer_restore',
        origin: GameArchiveOrigin.app,
        createdAt: DateTime.utc(2026, 9, 13),
        updatedAt: DateTime.utc(2026, 9, 13),
        status: GameArchiveStatus.active,
        captureQuality: GameCaptureQuality.complete,
        variant: GameVariant.classic,
        outcome: GameOutcome.ongoing,
        winner: GameWinner.none,
        lineRevision: 0,
        positions: const <ArchivedPosition>[
          ArchivedPosition(ply: 0, fen: standardChessInitialFen),
        ],
        transitions: const <ArchivedTransition>[],
        whitePlayer: const ArchivedPlayerAttribution.guest(),
        blackPlayer: const ArchivedPlayerAttribution.computer(),
      );
      final AppChessGameSession restored = AppChessGameSession.restore(
        archive: legacy,
        now: () => DateTime.utc(2026, 9, 13, 0, 1),
        persist: (GameArchive archive) async => saved.add(archive),
      );
      await restored.settlePersistence();

      expect(restored.archive.whitePlayer.kind, ArchivedPlayerKind.guest);
      expect(restored.archive.blackPlayer.kind, ArchivedPlayerKind.guest);
      expect(restored.sideToMove, GameSide.white);
      expect(restored.playUci('e2e4'), AppChessMoveResult.accepted);
      expect(saved, isNotEmpty);
      expect(saved.last.blackPlayer.kind, ArchivedPlayerKind.guest);
      restored.dispose();
    },
  );

  test(
    'repository writer restores the latest app-owned game from disk',
    () async {
      final Directory directory = await Directory.systemTemp.createTemp(
        'litch-app-chess-session-',
      );
      try {
        final GameLibraryRepository repository = GameLibraryRepository(
          GameLibraryStore(directory),
        );
        final Completer<void> firstWrite = Completer<void>();
        var isFirstWrite = true;
        final AppChessGameSession session = AppChessGameSession.newGame(
          identity: const AppChessGameIdentity(
            gameId: 'app_durable_game',
            libraryId: 'app-durable-game',
          ),
          now: () => DateTime.utc(2026, 9, 13),
          persist: (GameArchive archive) async {
            if (isFirstWrite) {
              isFirstWrite = false;
              await firstWrite.future;
            }
            await repository.save(archive);
          },
        );
        expect(session.playUci('e2e4'), AppChessMoveResult.accepted);
        expect(session.playUci('e7e5'), AppChessMoveResult.accepted);
        session.dispose();
        firstWrite.complete();
        await session.settlePersistence();
        await repository.close();

        final GameLibraryRepository reopened = GameLibraryRepository(
          GameLibraryStore(directory),
        );
        final GameArchive? saved = await reopened.findByIdentity(
          'app-durable-game',
          'app_durable_game',
          GameArchiveOrigin.app,
        );
        expect(saved, isNotNull);
        expect(saved!.boardGameId, isNull);
        expect(saved.currentPly, 2);
        expect(saved.origin, GameArchiveOrigin.app);

        final AppChessGameSession restored = AppChessGameSession.restore(
          archive: saved,
          now: () => DateTime.utc(2026, 9, 13, 0, 1),
          persist: reopened.save,
        );
        expect(restored.playUci('g1f3'), AppChessMoveResult.accepted);
        await restored.settlePersistence();
        expect(reopened.snapshot.games.single.currentPly, 3);
        restored.dispose();
        await reopened.close();
      } finally {
        await directory.delete(recursive: true);
      }
    },
  );
}

GameArchive _withLifecycle(
  GameArchive source, {
  required GameArchiveStatus status,
  required GameOutcome outcome,
  required GameWinner winner,
}) {
  return GameArchive(
    libraryId: source.libraryId,
    gameId: source.gameId,
    origin: source.origin,
    createdAt: source.createdAt,
    updatedAt: source.updatedAt,
    status: status,
    captureQuality: source.captureQuality,
    variant: source.variant,
    outcome: outcome,
    winner: winner,
    lineRevision: source.lineRevision,
    positions: source.positions,
    transitions: source.transitions,
    whitePlayer: source.whitePlayer,
    blackPlayer: source.blackPlayer,
  );
}
