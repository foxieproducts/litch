import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/post_game_review.dart';
import 'package:litch_mobile/src/services/post_game_review_analyzer.dart';

import 'post_game_review_test_fixture.dart';

void main() {
  group('PostGameReviewTimeline', () {
    test('navigates a partial archive by absolute ply', () {
      final GameArchive archive = partialClassicArchive();
      final PostGameReviewTimeline timeline = PostGameReviewTimeline(archive);

      expect(timeline.length, 2);
      expect(timeline.indexForPly(0), isNull);
      expect(timeline.indexForPly(1), 0);
      expect(timeline.indexForPly(2), 1);
      expect(timeline.indexForPly(3), isNull);
      expect(timeline.frameAt(0).incomingTransition, isNull);
      expect(timeline.frameAt(1).incomingTransition?.from, 12);
      expect(timeline.frameAt(1).incomingTransition?.to, 28);
    });

    test('builds an archive-only board snapshot with last-move context', () {
      final PostGameReviewTimeline timeline = PostGameReviewTimeline(
        completedClassicArchive(),
      );

      final snapshot = timeline.boardSnapshotAt(2);

      expect(snapshot.position?.fen, classicAfterE5);
      expect(snapshot.physicalStatus, isNull);
      expect(snapshot.lastMoves?.black?.fromSquare, 12);
      expect(snapshot.lastMoves?.black?.toSquare, 28);
      expect(snapshot.lastMoves?.white, isNull);
    });
  });

  group('PostGameReviewAnalysis', () {
    test('sorts and binds at most three moments to one archive revision', () {
      final GameArchive archive = completedClassicArchive();

      final PostGameReviewAnalysis analysis = PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: <PostGameReviewInsight>[
          PostGameReviewInsight(
            ply: 3,
            kind: PostGameReviewInsightKind.candidateLine,
            continuation: const <String>['d7d6'],
          ),
          PostGameReviewInsight(
            ply: 1,
            kind: PostGameReviewInsightKind.candidateLine,
            continuation: const <String>['e7e5', 'g1f3', 'b8c6'],
          ),
        ],
      );

      expect(analysis.insights.map((value) => value.ply), <int>[1, 3]);
      expect(analysis.matches(archive), isTrue);
      expect(
        analysis.matches(
          GameArchive(
            libraryId: archive.libraryId,
            boardGameId: archive.boardGameId,
            createdAt: archive.createdAt,
            updatedAt: archive.updatedAt,
            status: archive.status,
            captureQuality: archive.captureQuality,
            variant: archive.variant,
            outcome: archive.outcome,
            winner: archive.winner,
            lineRevision: archive.lineRevision + 1,
            positions: archive.positions,
            transitions: archive.transitions,
          ),
        ),
        isFalse,
      );
    });

    test('binds analysis to an app-owned game without a board identity', () {
      final GameArchive source = completedClassicArchive();
      final GameArchive appArchive = GameArchive(
        libraryId: 'app-review',
        gameId: 'app-review-id',
        origin: GameArchiveOrigin.app,
        createdAt: source.createdAt,
        updatedAt: source.updatedAt,
        status: source.status,
        captureQuality: source.captureQuality,
        variant: source.variant,
        outcome: source.outcome,
        winner: source.winner,
        lineRevision: source.lineRevision,
        positions: source.positions,
        transitions: source.transitions,
      );

      final PostGameReviewAnalysis analysis = PostGameReviewAnalysis.forArchive(
        archive: appArchive,
        insights: const <PostGameReviewInsight>[],
      );

      expect(analysis.matches(appArchive), isTrue);
      expect(analysis.gameId, appArchive.gameId);
      expect(analysis.origin, GameArchiveOrigin.app);
    });

    test('rejects unbounded, stale, or non-Classic analysis payloads', () {
      final GameArchive archive = completedClassicArchive();

      expect(
        () => PostGameReviewInsight(
          ply: 1,
          kind: PostGameReviewInsightKind.candidateLine,
          continuation: const <String>['not-a-move'],
        ),
        throwsFormatException,
      );
      expect(
        () => PostGameReviewAnalysis.forArchive(
          archive: archive,
          insights: List<PostGameReviewInsight>.generate(
            4,
            (int index) => PostGameReviewInsight(
              ply: index,
              kind: PostGameReviewInsightKind.materialChange,
              side: GameSide.white,
              capturedPiece: PostGameReviewPiece.pawn,
              captureSquare: 28,
            ),
          ),
        ),
        throwsFormatException,
      );
      expect(
        () => PostGameReviewAnalysis.forArchive(
          archive: archive,
          insights: <PostGameReviewInsight>[
            PostGameReviewInsight(
              ply: 30,
              kind: PostGameReviewInsightKind.materialChange,
              side: GameSide.white,
              capturedPiece: PostGameReviewPiece.pawn,
              captureSquare: 28,
            ),
          ],
        ),
        throwsFormatException,
      );
      expect(
        () => PostGameReviewAnalysis.forArchive(
          archive: duckArchive(),
          insights: const <PostGameReviewInsight>[],
        ),
        throwsUnsupportedError,
      );
    });
  });
}
