import 'game_archive.dart';
import 'home_board_snapshot.dart';
import 'standard_chess_replay.dart';
import 'package:chess/chess.dart' as rules;

/// One immutable position in an archived game's replay timeline.
class PostGameReviewFrame {
  const PostGameReviewFrame({
    required this.index,
    required this.position,
    required this.incomingTransition,
  });

  final int index;
  final ArchivedPosition position;
  final ArchivedTransition? incomingTransition;
}

const int postGameReviewMaximumInsights = 3;
const int postGameReviewMaximumContinuationPlies = 3;

enum PostGameReviewInsightKind { materialChange, mateInOne, candidateLine }

enum PostGameReviewPiece { pawn, knight, bishop, rook, queen }

class PostGameReviewInsight {
  factory PostGameReviewInsight({
    required int ply,
    required PostGameReviewInsightKind kind,
    GameSide? side,
    PostGameReviewPiece? capturedPiece,
    int? captureSquare,
    PromotionPiece? promotion,
    Iterable<String> continuation = const <String>[],
  }) {
    final moves = List<String>.unmodifiable(continuation);
    if (ply < 0 || ply > gameArchiveMaximumPlies) {
      throw RangeError.range(ply, 0, gameArchiveMaximumPlies, 'ply');
    }
    if (kind != PostGameReviewInsightKind.candidateLine && side == null) {
      throw const FormatException('Material and mate insights require a side.');
    }
    if (kind == PostGameReviewInsightKind.candidateLine && side != null) {
      throw const FormatException('Candidate-line insight cannot have a side.');
    }
    if ((capturedPiece == null) != (captureSquare == null)) {
      throw const FormatException('Capture detail must be complete.');
    }
    if (captureSquare != null && (captureSquare < 0 || captureSquare > 63)) {
      throw RangeError.range(captureSquare, 0, 63, 'captureSquare');
    }
    if (kind == PostGameReviewInsightKind.materialChange) {
      if (capturedPiece == null && promotion == null) {
        throw const FormatException('Material insight requires an event.');
      }
    } else if (capturedPiece != null || promotion != null) {
      throw const FormatException('Only material insight has material detail.');
    }
    if (kind == PostGameReviewInsightKind.candidateLine) {
      if (moves.isEmpty ||
          moves.length > postGameReviewMaximumContinuationPlies) {
        throw const FormatException('Candidate line must contain 1–3 plies.');
      }
    } else if (kind == PostGameReviewInsightKind.mateInOne) {
      if (moves.length != 1) {
        throw const FormatException('Mate-in-one insight requires one move.');
      }
    } else if (moves.isNotEmpty) {
      throw const FormatException('Material-change insight cannot have moves.');
    }
    for (final move in moves) {
      if (!_uciMove.hasMatch(move)) {
        throw const FormatException('Candidate line contains invalid UCI.');
      }
    }
    return PostGameReviewInsight._(
      ply: ply,
      kind: kind,
      side: side,
      capturedPiece: capturedPiece,
      captureSquare: captureSquare,
      promotion: promotion,
      continuation: moves,
    );
  }

  const PostGameReviewInsight._({
    required this.ply,
    required this.kind,
    required this.side,
    required this.capturedPiece,
    required this.captureSquare,
    required this.promotion,
    required this.continuation,
  });

  final int ply;
  final PostGameReviewInsightKind kind;
  final GameSide? side;
  final PostGameReviewPiece? capturedPiece;
  final int? captureSquare;
  final PromotionPiece? promotion;
  final List<String> continuation;
}

class PostGameReviewAnalysis {
  factory PostGameReviewAnalysis.forArchive({
    required GameArchive archive,
    required Iterable<PostGameReviewInsight> insights,
  }) {
    if (archive.variant != GameVariant.classic) {
      throw UnsupportedError('Duck post-game analysis is not supported.');
    }
    final ordered = List<PostGameReviewInsight>.of(insights)
      ..sort((left, right) {
        final byPly = left.ply.compareTo(right.ply);
        return byPly != 0 ? byPly : left.kind.index.compareTo(right.kind.index);
      });
    if (ordered.length > postGameReviewMaximumInsights) {
      throw const FormatException('Post-game analysis has too many insights.');
    }
    final positions = <int, ArchivedPosition>{
      for (final position in archive.positions) position.ply: position,
    };
    final seen = <int>{};
    for (final insight in ordered) {
      final position = positions[insight.ply];
      if (position == null || !seen.add(insight.ply)) {
        throw const FormatException(
          'Insight references an unavailable or duplicate ply.',
        );
      }
      if (insight.kind == PostGameReviewInsightKind.candidateLine ||
          insight.kind == PostGameReviewInsightKind.mateInOne) {
        final chess = validatedStandardChessInitialPosition(position.fen);
        for (final uci in insight.continuation) {
          if (!chess.move(<String, String>{
            'from': uci.substring(0, 2),
            'to': uci.substring(2, 4),
            if (uci.length == 5) 'promotion': uci[4],
          })) {
            throw const FormatException(
              'Candidate line contains an illegal move.',
            );
          }
        }
        if (insight.kind == PostGameReviewInsightKind.mateInOne &&
            (!chess.in_checkmate ||
                (insight.side == GameSide.white) !=
                    (validatedStandardChessInitialPosition(position.fen).turn ==
                        rules.Chess.WHITE))) {
          throw const FormatException(
            'Mate-in-one insight does not prove checkmate for its side.',
          );
        }
      }
      if (insight.kind == PostGameReviewInsightKind.materialChange) {
        final index = archive.positions.indexWhere(
          (value) => value.ply == insight.ply,
        );
        if (index <= 0) {
          throw const FormatException('Material event has no incoming move.');
        }
        final transition = archive.transitions[index - 1];
        final captured = transition.captureSquare == null
            ? null
            : validatedStandardChessInitialPosition(
                archive.positions[index - 1].fen,
              ).get(algebraicSquare(transition.captureSquare!).toLowerCase());
        if (transition.side != insight.side ||
            transition.captureSquare != insight.captureSquare ||
            transition.promotion != insight.promotion ||
            (captured == null ? null : _reviewPiece(captured.type.name)) !=
                insight.capturedPiece) {
          throw const FormatException(
            'Material detail does not match archive.',
          );
        }
      }
    }
    return PostGameReviewAnalysis._(
      libraryId: archive.libraryId,
      gameId: archive.gameId,
      origin: archive.origin,
      lineRevision: archive.lineRevision,
      lastPly: archive.currentPly,
      status: archive.status,
      captureQuality: archive.captureQuality,
      insights: List.unmodifiable(ordered),
    );
  }

  const PostGameReviewAnalysis._({
    required this.libraryId,
    required this.gameId,
    required this.origin,
    required this.lineRevision,
    required this.lastPly,
    required this.status,
    required this.captureQuality,
    required this.insights,
  });

  final String libraryId;
  final String gameId;
  final GameArchiveOrigin origin;
  final int lineRevision;
  final int lastPly;
  final GameArchiveStatus status;
  final GameCaptureQuality captureQuality;
  final List<PostGameReviewInsight> insights;

  bool matches(GameArchive archive) =>
      archive.variant == GameVariant.classic &&
      archive.libraryId == libraryId &&
      archive.gameId == gameId &&
      archive.origin == origin &&
      archive.lineRevision == lineRevision &&
      archive.currentPly == lastPly &&
      archive.status == status &&
      archive.captureQuality == captureQuality;
}

final RegExp _uciMove = RegExp(r'^[a-h][1-8][a-h][1-8][qrbn]?$');

PostGameReviewPiece _reviewPiece(String piece) => switch (piece) {
  'p' => PostGameReviewPiece.pawn,
  'n' => PostGameReviewPiece.knight,
  'b' => PostGameReviewPiece.bishop,
  'r' => PostGameReviewPiece.rook,
  'q' => PostGameReviewPiece.queen,
  _ => throw const FormatException('King cannot be captured.'),
};

/// Read-only navigation over a validated [GameArchive].
///
/// Indices are deliberately separate from archive plies because a partial
/// archive may begin after ply zero.
class PostGameReviewTimeline {
  const PostGameReviewTimeline(this.archive);

  final GameArchive archive;

  int get length => archive.positions.length;

  int get lastIndex => length - 1;

  PostGameReviewFrame frameAt(int index) {
    RangeError.checkValidIndex(index, archive.positions, 'index');
    return PostGameReviewFrame(
      index: index,
      position: archive.positions[index],
      incomingTransition: index == 0 ? null : archive.transitions[index - 1],
    );
  }

  int? indexForPly(int ply) {
    final int index = ply - archive.positions.first.ply;
    if (index < 0 || index >= length || archive.positions[index].ply != ply) {
      return null;
    }
    return index;
  }

  HomeBoardSnapshot boardSnapshotAt(int index) {
    final PostGameReviewFrame frame = frameAt(index);
    final ArchivedTransition? move = frame.incomingTransition;
    final HomeSide? moveSide = switch (move?.side) {
      GameSide.white => HomeSide.white,
      GameSide.black => HomeSide.black,
      null => null,
    };
    final HomeMoveSquares? moveSquares = move == null
        ? null
        : HomeMoveSquares(fromSquare: move.from, toSquare: move.to);

    return HomeBoardSnapshot(
      position: HomeChessPosition.tryParse(frame.position.fen),
      gameStatus: HomeGameStatus(
        variant: archive.variant.name,
        duckSquare: frame.position.duckSquare,
        duckMovePending: false,
        outcome: archive.outcome.name,
        winner: archive.winner.name,
      ),
      lastMoves: HomeLastMoves(
        white: moveSide == HomeSide.white ? moveSquares : null,
        black: moveSide == HomeSide.black ? moveSquares : null,
        mostRecent: moveSide,
      ),
    );
  }
}
