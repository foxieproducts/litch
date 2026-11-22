import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/standard_chess_replay.dart';

const String classicInitialFen =
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
const String classicAfterE4 =
    'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1';
const String classicAfterE5 =
    'rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2';
const String classicAfterNf3 =
    'rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2';

GameArchive completedClassicArchive() {
  final DateTime observedAt = DateTime.utc(2026, 9, 2, 20);
  return GameArchive(
    libraryId: 'classic-review-fixture',
    boardGameId: 'board-classic-review-fixture',
    createdAt: observedAt,
    updatedAt: observedAt,
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.timeout,
    winner: GameWinner.white,
    lineRevision: 3,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: classicInitialFen),
      ArchivedPosition(ply: 1, fen: classicAfterE4),
      ArchivedPosition(ply: 2, fen: classicAfterE5),
      ArchivedPosition(ply: 3, fen: classicAfterNf3),
    ],
    transitions: const <ArchivedTransition>[
      ArchivedTransition(side: GameSide.white, from: 52, to: 36),
      ArchivedTransition(side: GameSide.black, from: 12, to: 28),
      ArchivedTransition(side: GameSide.white, from: 62, to: 45),
    ],
  );
}

GameArchive partialClassicArchive() {
  final DateTime observedAt = DateTime.utc(2026, 9, 2, 20);
  return GameArchive(
    libraryId: 'partial-review-fixture',
    boardGameId: 'board-partial-review-fixture',
    createdAt: observedAt,
    updatedAt: observedAt,
    status: GameArchiveStatus.unfinished,
    captureQuality: GameCaptureQuality.partial,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 2,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 1, fen: classicAfterE4),
      ArchivedPosition(ply: 2, fen: classicAfterE5),
    ],
    transitions: const <ArchivedTransition>[
      ArchivedTransition(side: GameSide.black, from: 12, to: 28),
    ],
  );
}

GameArchive duckArchive() {
  final DateTime observedAt = DateTime.utc(2026, 9, 2, 20);
  return GameArchive(
    libraryId: 'duck-review-fixture',
    boardGameId: 'board-duck-review-fixture',
    createdAt: observedAt,
    updatedAt: observedAt,
    status: GameArchiveStatus.unfinished,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.duck,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: classicInitialFen, duckSquare: 28),
      ArchivedPosition(ply: 1, fen: classicAfterE4, duckSquare: 27),
    ],
    transitions: const <ArchivedTransition>[
      ArchivedTransition(
        side: GameSide.white,
        from: 52,
        to: 36,
        duckFrom: 28,
        duckTo: 27,
      ),
    ],
  );
}

GameArchive mateInOneArchive() {
  final DateTime observedAt = DateTime.utc(2026, 9, 2, 20);
  return GameArchive(
    libraryId: 'mate-review-fixture',
    boardGameId: 'board-mate-review-fixture',
    createdAt: observedAt,
    updatedAt: observedAt,
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.draw,
    winner: GameWinner.draw,
    lineRevision: 1,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: '7k/5Q2/6K1/8/8/8/8/8 w - - 0 1'),
    ],
    transitions: const <ArchivedTransition>[],
  );
}

GameArchive materialReviewArchive() {
  final DateTime observedAt = DateTime.utc(2026, 9, 2, 20);
  final seed = GameArchive(
    libraryId: 'material-review-fixture',
    boardGameId: 'board-material-review-fixture',
    createdAt: observedAt,
    updatedAt: observedAt,
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 1,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: classicInitialFen),
    ],
    transitions: const <ArchivedTransition>[],
  );
  final played = replayStandardChessExtension(seed, const <String>[
    'e2e4',
    'd7d5',
    'e4d5',
  ]);
  return GameArchive(
    libraryId: played.libraryId,
    boardGameId: played.boardGameId,
    createdAt: played.createdAt,
    updatedAt: played.updatedAt,
    status: GameArchiveStatus.completed,
    captureQuality: played.captureQuality,
    variant: played.variant,
    outcome: GameOutcome.draw,
    winner: GameWinner.draw,
    lineRevision: played.lineRevision,
    positions: played.positions,
    transitions: played.transitions,
  );
}

GameArchive promotionReviewArchive() {
  final DateTime observedAt = DateTime.utc(2026, 9, 2, 20);
  final seed = GameArchive(
    libraryId: 'promotion-review-fixture',
    boardGameId: 'board-promotion-review-fixture',
    createdAt: observedAt,
    updatedAt: observedAt,
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 1,
    positions: const <ArchivedPosition>[
      ArchivedPosition(ply: 0, fen: '7k/P7/8/8/8/8/7p/7K w - - 0 1'),
    ],
    transitions: const <ArchivedTransition>[],
  );
  final played = replayStandardChessExtension(seed, const <String>['a7a8q']);
  return GameArchive(
    libraryId: played.libraryId,
    boardGameId: played.boardGameId,
    createdAt: played.createdAt,
    updatedAt: played.updatedAt,
    status: GameArchiveStatus.completed,
    captureQuality: played.captureQuality,
    variant: played.variant,
    outcome: GameOutcome.draw,
    winner: GameWinner.draw,
    lineRevision: played.lineRevision,
    positions: played.positions,
    transitions: played.transitions,
  );
}
