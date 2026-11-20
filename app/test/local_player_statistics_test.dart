import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/local_player_profile.dart';
import 'package:litch_mobile/src/models/local_player_statistics.dart';

const String _alexId = 'lp_00000000000000000000000000000001';
const String _blairId = 'lp_00000000000000000000000000000002';
const String _oldAlexId = 'lp_00000000000000000000000000000003';
const String _deletedId = 'lp_00000000000000000000000000000004';

void main() {
  test('derives a small result set and keeps Classic and Duck separate', () {
    final LocalPlayerStatisticsReport report =
        LocalPlayerStatisticsReport.fromArchives(
          directory: _directory(),
          games: <GameArchive>[
            _classicGame(
              id: 'classic-win',
              winner: GameWinner.white,
              whitePlayer: ArchivedPlayerAttribution.profile(_alexId),
              blackPlayer: ArchivedPlayerAttribution.profile(_blairId),
            ),
            _classicGame(
              id: 'classic-loss',
              winner: GameWinner.white,
              whitePlayer: ArchivedPlayerAttribution.profile(_blairId),
              blackPlayer: ArchivedPlayerAttribution.profile(_alexId),
            ),
            _classicGame(
              id: 'classic-draw',
              winner: GameWinner.draw,
              whitePlayer: ArchivedPlayerAttribution.profile(_alexId),
              blackPlayer: const ArchivedPlayerAttribution.guest(),
            ),
            _duckGame(
              id: 'duck-win',
              whitePlayer: ArchivedPlayerAttribution.profile(_oldAlexId),
              blackPlayer: const ArchivedPlayerAttribution.guest(),
            ),
          ],
        );

    final LocalPlayerStatistics alex = report.byPlayerId[_alexId]!;
    expect(alex.allVariants.completedGames, 4);
    expect(alex.allVariants.wins, 2);
    expect(alex.allVariants.draws, 1);
    expect(alex.allVariants.losses, 1);
    expect(alex.classic.completedGames, 3);
    expect(alex.duck.completedGames, 1);

    final LocalPlayerStatistics blair = report.byPlayerId[_blairId]!;
    expect(blair.classic.completedGames, 2);
    expect(blair.classic.wins, 1);
    expect(blair.classic.losses, 1);
  });

  test('omits partial, unfinished, and self-play results honestly', () {
    final GameArchive active = GameArchive(
      libraryId: 'active',
      boardGameId: 'board-active',
      createdAt: DateTime.utc(2026, 9, 3),
      updatedAt: DateTime.utc(2026, 9, 3),
      status: GameArchiveStatus.active,
      captureQuality: GameCaptureQuality.complete,
      variant: GameVariant.classic,
      outcome: GameOutcome.ongoing,
      winner: GameWinner.none,
      lineRevision: 0,
      positions: const <ArchivedPosition>[_openingPosition],
      transitions: const <ArchivedTransition>[],
      whitePlayer: ArchivedPlayerAttribution.profile(_alexId),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );
    final LocalPlayerStatisticsReport report =
        LocalPlayerStatisticsReport.fromArchives(
          directory: _directory(),
          games: <GameArchive>[
            active,
            _classicGame(
              id: 'self-play',
              winner: GameWinner.white,
              whitePlayer: ArchivedPlayerAttribution.profile(_alexId),
              blackPlayer: ArchivedPlayerAttribution.profile(_oldAlexId),
            ),
          ],
        );

    final LocalPlayerStatistics alex = report.byPlayerId[_alexId]!;
    expect(alex.allVariants.completedGames, 0);
    expect(alex.excludedIncompleteOrPartialGames, 1);
    expect(alex.excludedSelfPlayGames, 1);
    expect(report.excludedIncompleteOrPartialGames, 1);
  });

  test('distinguishes Guest from board-only and unlinked profiles', () {
    final LocalPlayerStatisticsReport report =
        LocalPlayerStatisticsReport.fromArchives(
          directory: _directory(),
          games: <GameArchive>[
            _classicGame(
              id: 'guests',
              winner: GameWinner.draw,
              whitePlayer: const ArchivedPlayerAttribution.guest(),
              blackPlayer: const ArchivedPlayerAttribution.guest(),
            ),
            _classicGame(
              id: 'board-only',
              winner: GameWinner.draw,
              whitePlayer: const ArchivedPlayerAttribution.unknown(),
              blackPlayer: const ArchivedPlayerAttribution.unknown(),
            ),
            _classicGame(
              id: 'deleted-profile',
              winner: GameWinner.draw,
              whitePlayer: ArchivedPlayerAttribution.profile(_deletedId),
              blackPlayer: const ArchivedPlayerAttribution.guest(),
            ),
          ],
        );

    expect(report.eligibleCompletedGames, 3);
    expect(report.gamesWithUnknownOrUnlinkedPlayers, 2);
    expect(report.byPlayerId[_alexId]!.allVariants.completedGames, 0);
  });
}

LocalPlayerDirectory _directory() {
  return LocalPlayerDirectory(
    profiles: <LocalPlayerProfile>[
      LocalPlayerProfile(id: _alexId, displayName: 'Alex'),
      LocalPlayerProfile(id: _blairId, displayName: 'Blair'),
    ],
    mergedPlayerIds: const <String, String>{_oldAlexId: _alexId},
  );
}

GameArchive _classicGame({
  required String id,
  required GameWinner winner,
  required ArchivedPlayerAttribution whitePlayer,
  required ArchivedPlayerAttribution blackPlayer,
}) {
  return GameArchive(
    libraryId: id,
    boardGameId: 'board-$id',
    createdAt: DateTime.utc(2026, 9, 3),
    updatedAt: DateTime.utc(2026, 9, 3),
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: winner == GameWinner.draw
        ? GameOutcome.draw
        : GameOutcome.checkmate,
    winner: winner,
    lineRevision: 1,
    positions: <ArchivedPosition>[
      winner == GameWinner.white ? _openingBlackTurn : _openingWhiteTurn,
    ],
    transitions: const <ArchivedTransition>[],
    whitePlayer: whitePlayer,
    blackPlayer: blackPlayer,
  );
}

GameArchive _duckGame({
  required String id,
  required ArchivedPlayerAttribution whitePlayer,
  required ArchivedPlayerAttribution blackPlayer,
}) {
  return GameArchive(
    libraryId: id,
    boardGameId: 'board-$id',
    createdAt: DateTime.utc(2026, 9, 3),
    updatedAt: DateTime.utc(2026, 9, 3),
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.duck,
    outcome: GameOutcome.fowled,
    winner: GameWinner.white,
    lineRevision: 1,
    positions: const <ArchivedPosition>[
      ArchivedPosition(
        ply: 0,
        fen:
            'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
            'RNBQKBNR w KQkq - 0 1',
        duckSquare: 27,
      ),
    ],
    transitions: const <ArchivedTransition>[],
    whitePlayer: whitePlayer,
    blackPlayer: blackPlayer,
  );
}

const ArchivedPosition _openingPosition = _openingWhiteTurn;

const ArchivedPosition _openingWhiteTurn = ArchivedPosition(
  ply: 0,
  fen:
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
      'RNBQKBNR w KQkq - 0 1',
);

const ArchivedPosition _openingBlackTurn = ArchivedPosition(
  ply: 0,
  fen:
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/'
      'RNBQKBNR b KQkq - 0 1',
);
