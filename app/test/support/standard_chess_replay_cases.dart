import 'dart:convert';

import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/standard_chess_replay.dart';

const opening = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';
var checks = 0;
var maximumReplayMilliseconds = 0;
void expect(bool value, String label) {
  checks++;
  if (!value) throw StateError('FAIL: $label');
}

void rejects(void Function() action, String label) {
  try {
    action();
  } on FormatException {
    checks++;
    return;
  }
  throw StateError('FAIL: accepted $label');
}

GameArchive seed(
  String fen, {
  GameArchiveStatus status = GameArchiveStatus.active,
}) => GameArchive(
  libraryId: 'evaluation',
  boardGameId: 'evaluation-board',
  createdAt: DateTime.utc(2026),
  updatedAt: DateTime.utc(2026),
  status: status,
  captureQuality: GameCaptureQuality.complete,
  variant: GameVariant.classic,
  outcome: GameOutcome.ongoing,
  winner: GameWinner.none,
  lineRevision: 7,
  positions: [ArchivedPosition(ply: 0, fen: fen)],
  transitions: [],
);
void special(
  String fen,
  String move, {
  required int from,
  required int to,
  int? capture,
  int? rookFrom,
  int? rookTo,
  PromotionPiece? promotion,
}) {
  final archive = replayStandardChessExtension(seed(fen), [move]);
  archive.validate();
  final t = archive.transitions.single;
  expect(t.from == from && t.to == to, '$move a8 mapping');
  expect(t.captureSquare == capture, '$move capture square');
  expect(
    t.secondaryFrom == rookFrom && t.secondaryTo == rookTo,
    '$move rook relocation',
  );
  expect(t.promotion == promotion, '$move promotion');
  expect(
    t.side == (fen.split(' ')[1] == 'w' ? GameSide.white : GameSide.black),
    '$move side',
  );
  expect(
    archive.positions.length == 2 && archive.currentPly == 1,
    '$move archive factory path',
  );
  final duplicate = replayStandardChessExtension(archive, [move]);
  expect(identical(duplicate, archive), '$move duplicate object identity');
}

void runSpecialMoveCases() {
  special(
    '4k2r/8/8/8/8/8/8/4K2R w K - 0 1',
    'e1g1',
    from: 60,
    to: 62,
    rookFrom: 63,
    rookTo: 61,
  );
  special(
    'r3k3/8/8/8/8/8/8/R3K3 w Q - 0 1',
    'e1c1',
    from: 60,
    to: 58,
    rookFrom: 56,
    rookTo: 59,
  );
  special(
    '4k2r/8/8/8/8/8/8/4K3 b k - 0 1',
    'e8g8',
    from: 4,
    to: 6,
    rookFrom: 7,
    rookTo: 5,
  );
  special(
    'r3k3/8/8/8/8/8/8/4K3 b q - 0 1',
    'e8c8',
    from: 4,
    to: 2,
    rookFrom: 0,
    rookTo: 3,
  );
  special(
    '7k/8/8/3pP3/8/8/8/K7 w - d6 0 2',
    'e5d6',
    from: 28,
    to: 19,
    capture: 27,
  );
  special(
    'k7/8/8/8/3pP3/8/8/7K b - e3 0 1',
    'd4e3',
    from: 35,
    to: 44,
    capture: 36,
  );
  for (final entry in {
    'q': PromotionPiece.queen,
    'r': PromotionPiece.rook,
    'b': PromotionPiece.bishop,
    'n': PromotionPiece.knight,
  }.entries) {
    special(
      '7k/P7/8/8/8/8/8/K7 w - - 0 1',
      'a7a8${entry.key}',
      from: 8,
      to: 0,
      promotion: entry.value,
    );
    special(
      '1r5k/P7/8/8/8/8/8/K7 w - - 0 1',
      'a7b8${entry.key}',
      from: 8,
      to: 1,
      capture: 1,
      promotion: entry.value,
    );
    special(
      'k7/8/8/8/8/8/p7/7K b - - 0 1',
      'a2a1${entry.key}',
      from: 48,
      to: 56,
      promotion: entry.value,
    );
    special(
      'k7/8/8/8/8/8/p7/1R5K b - - 0 1',
      'a2b1${entry.key}',
      from: 48,
      to: 57,
      capture: 57,
      promotion: entry.value,
    );
  }
}

void runCumulativeReplayCases() {
  final first = replayStandardChessExtension(seed(opening), ['e2e4']);
  final serialized = jsonEncode(first.toJson());
  final coalesced = replayStandardChessExtension(first, [
    'e2e4',
    'e7e5',
    'g1f3',
  ]);
  expect(coalesced.currentPly == 3, 'coalesced move gap replays every ply');
  expect(
    coalesced.positions[1].fen == first.positions[1].fen,
    'prior prefix preserved',
  );
  expect(
    jsonEncode(first.toJson()) == serialized,
    'successful replay never mutates input',
  );
  expect(
    jsonEncode(
          replayStandardChessExtension(seed(opening), [
            'e2e4',
            'e7e5',
            'g1f3',
          ]).toJson(),
        ) ==
        jsonEncode(coalesced.toJson()),
    'fresh deterministic output',
  );
  for (final bad in <List<String>>[
    [],
    ['d2d4'],
    ['e2e4', 'not-uci'],
    ['e2e4', 'e7e6', 'a1a8'],
    ['e2e4', 'e7e5q'],
    ['e2e4', 'E7E5'],
    ['e2e4', 'e7e5 '],
    ['e2e4', 'e7e5\n'],
    ['e2e4', 'e7e7'],
    ['e2e4', 'a0a1'],
    ['e2e4', 'e7e5', 'g1f3', 'b8b6'],
    ['e2e4', 'e7e5', 'g1f3', 'b8c6x'],
  ]) {
    rejects(
      () => replayStandardChessExtension(first, bad),
      'invalid/regressed/divergent batch $bad',
    );
    expect(jsonEncode(first.toJson()) == serialized, 'rejection atomic $bad');
  }
  for (final move in ['a7a8', 'a7b8']) {
    rejects(
      () => replayStandardChessExtension(
        seed('1r5k/P7/8/8/8/8/8/K7 w - - 0 1'),
        [move],
      ),
      'missing promotion $move',
    );
  }
  for (final fen in [
    '', ' ', ' $opening', '$opening ', opening.replaceFirst(' w ', '  w '),
    opening.replaceFirst(' w ', '\tw '), opening.replaceFirst(' w ', ' é '),
    '8/8/8/8/8/8/8/8 w - - 0 1',
    '7k/8/8/8/P7/8/PPPPPPPP/K7 w - - 0 1',
    '7k/8/8/8/8/8/PPPPPPPP/K1Q1Q3 w - - 0 1',
    '7k/8/8/8/8/8/PPPPPPPP/KB1B4 w - - 0 1',
    '4k3/8/8/8/8/8/8/4K3 w K - 0 1',
    '4k3/8/8/8/8/8/8/3K3R w K - 0 1',
    '7k/8/8/4P3/8/8/8/K7 w - d6 0 2',
    '7k/8/3N4/3pP3/8/8/8/K7 w - d6 0 2',
    '7k/3n4/8/3pP3/8/8/8/K7 w - d6 0 2',
    '7k/8/8/3pP3/8/8/8/K7 w - d6 1 2',
    '7k/8/8/3pP3/8/8/8/K7 b - d6 0 2',
    opening.replaceFirst(' 0 1', ' -1 1'), opening.replaceFirst(' 0 1', ' 0 0'),
    opening.replaceFirst(' 0 1', ' 00 1'),
    opening.replaceFirst(' 0 1', ' +0 1'),
    opening.replaceFirst(' 0 1', ' 0 01'),
    opening.replaceFirst(
      ' 0 1',
      ' ${standardReplayMaximumInitialCounter + 1} 1',
    ),
    opening.replaceFirst(
      ' 0 1',
      ' 0 ${standardReplayMaximumInitialCounter + 1}',
    ),
    opening.replaceFirst(' 0 1', ' ${'9' * 160} 1'),
    '7k/8/8/8/8/8/8/K6R w - - 0 1', // non-moving king already checked
  ]) {
    rejects(
      () => replayStandardChessExtension(seed(fen), []),
      'strict initial FEN $fen',
    );
  }
  final promotedMaterial = seed('7k/8/8/8/8/8/1PPPPPPP/K1Q1Q3 w - - 0 1');
  expect(
    identical(
      replayStandardChessExtension(promotedMaterial, []),
      promotedMaterial,
    ),
    'promoted material within missing pawn budget accepted',
  );
  final counters =
      '7k/8/8/8/8/8/8/K7 w - - $standardReplayMaximumInitialCounter $standardReplayMaximumInitialCounter';
  final high = replayStandardChessExtension(seed(counters), ['a1b1', 'h8g8']);
  expect(
    high.positions.last.fen.endsWith(
      '${standardReplayMaximumInitialCounter + 2} ${standardReplayMaximumInitialCounter + 1}',
    ),
    'bounded custom counters preserved/incremented',
  );
  final epWithoutCapturer = '7k/8/8/3p4/8/8/8/K7 w - d6 0 2';
  expect(
    identical(
      replayStandardChessExtension(
        seed(epWithoutCapturer),
        [],
      ).positions.first.fen,
      epWithoutCapturer,
    ),
    'FEN EP target may exist without adjacent capturer',
  );
  final boundedMoves = List<String>.generate(
    gameArchiveMaximumPlies,
    (i) => ['g1f3', 'g8f6', 'f3g1', 'f6g8'][i % 4],
  );
  final timer = Stopwatch()..start();
  final maximum = replayStandardChessExtension(seed(opening), boundedMoves);
  timer.stop();
  expect(
    maximum.currentPly == 4096 && maximum.positions.length == 4097,
    '4096 boundary accepted',
  );
  expect(
    maximum.positions.last.fen.endsWith('4096 2049'),
    '4096 exact counter reconstruction',
  );
  rejects(
    () => replayStandardChessExtension(maximum, [...boundedMoves, 'g1f3']),
    '4097 rejected',
  );
  final beforeMaximum = jsonEncode(maximum.toJson());
  rejects(
    () => replayStandardChessExtension(maximum, boundedMoves.sublist(0, 4095)),
    '4095 cannot regress4096',
  );
  expect(
    jsonEncode(maximum.toJson()) == beforeMaximum,
    'maximum rejection preserves all archive bytes',
  );
  try {
    maximum.positions.add(maximum.positions.last);
    throw StateError('mutable result');
  } on UnsupportedError {
    checks++;
  }
  // Existing archive accepts a board-delta-valid teleport; replay must reject it.
  final forged = GameArchive(
    libraryId: 'forged',
    boardGameId: 'forged',
    createdAt: DateTime.utc(2026),
    updatedAt: DateTime.utc(2026),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: [
      ArchivedPosition(ply: 0, fen: '4k3/8/8/8/8/8/2P5/2B1K3 w - - 0 1'),
      ArchivedPosition(ply: 1, fen: '4k3/8/8/2B5/8/8/2P5/4K3 b - - 1 1'),
    ],
    transitions: [ArchivedTransition(side: GameSide.white, from: 58, to: 26)],
  );
  rejects(
    () => replayStandardChessExtension(forged, ['c1c5']),
    'archive-valid illegal prior move',
  );
  maximumReplayMilliseconds = timer.elapsedMilliseconds;
}

void runArchiveLifecycleCases() {
  final first = replayStandardChessExtension(seed(opening), ['e2e4']);
  final completed = GameArchive(
    libraryId: first.libraryId,
    boardGameId: first.boardGameId,
    createdAt: first.createdAt,
    updatedAt: first.updatedAt,
    status: GameArchiveStatus.completed,
    captureQuality: first.captureQuality,
    variant: first.variant,
    outcome: GameOutcome.timeout,
    winner: GameWinner.white,
    lineRevision: first.lineRevision,
    positions: first.positions,
    transitions: first.transitions,
  );
  expect(
    identical(replayStandardChessExtension(completed, ['e2e4']), completed),
    'completed terminal duplicate retained',
  );
  rejects(
    () => replayStandardChessExtension(completed, ['e2e4', 'e7e5']),
    'completed cannot append otherwise legal move',
  );
  expect(
    completed.status == GameArchiveStatus.completed &&
        completed.outcome == GameOutcome.timeout &&
        completed.currentPly == 1,
    'completed value unchanged after rejected extension',
  );
  final sealed = seed(opening, status: GameArchiveStatus.unfinished);
  expect(
    identical(replayStandardChessExtension(sealed, []), sealed),
    'sealed duplicate retained',
  );
  rejects(
    () => replayStandardChessExtension(sealed, ['e2e4']),
    'sealed extension rejected',
  );
}
