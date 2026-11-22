import 'dart:async';

import 'package:litch_mobile/src/engine/chess_engine_models.dart';
import 'package:litch_mobile/src/engine/chess_engine_provider.dart';
import 'package:litch_mobile/src/engine/engine_runtime.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/standard_chess_replay.dart';
import 'package:litch_mobile/src/services/post_game_review_analyzer.dart';

void main() {
  const analyzer = BoundedLocalPostGameReviewAnalyzer();

  test('finds exhaustively verified mate in one for either side', () async {
    final white = await analyzer.analyze(
      _singlePosition('7k/5Q2/6K1/8/8/8/8/8 w - - 0 1'),
    );
    final black = await analyzer.analyze(
      _singlePosition('8/8/8/8/8/6k1/5q2/7K b - - 0 1'),
    );

    expect(
      white.insights
          .where((value) => value.kind == PostGameReviewInsightKind.mateInOne)
          .single
          .side,
      GameSide.white,
    );
    expect(
      white.insights
          .where((value) => value.kind == PostGameReviewInsightKind.mateInOne)
          .single
          .continuation,
      hasLength(1),
    );
    expect(
      black.insights
          .where((value) => value.kind == PostGameReviewInsightKind.mateInOne)
          .single
          .side,
      GameSide.black,
    );
  });

  test('does not confuse check or stalemate with mate in one', () async {
    final checked = await analyzer.analyze(
      _singlePosition('7k/6Q1/6K1/8/8/8/8/8 b - - 0 1'),
    );
    final stalemate = await analyzer.analyze(
      _singlePosition('7k/5Q2/6K1/8/8/8/8/8 b - - 0 1'),
    );

    expect(checked.insights, isEmpty);
    expect(stalemate.insights, isEmpty);
  });

  test('rejects impossible en-passant and castling metadata', () async {
    await expectLater(
      analyzer.analyze(_singlePosition('4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1')),
      throwsFormatException,
    );
    await expectLater(
      analyzer.analyze(_singlePosition('4k3/8/8/8/8/8/8/4K3 w K - 0 1')),
      throwsFormatException,
    );
  });

  test('reports special-move material only as factual changes', () async {
    final archive = _played(<String>[
      'e2e4',
      'a7a6',
      'e4e5',
      'd7d5',
      'e5d6',
      'c7d6',
      'g1f3',
      'b8c6',
      'f1e2',
      'c8f5',
      'e1g1',
      'a6a5',
    ]);
    final analysis = await analyzer.analyze(archive);

    expect(analysis.insights, hasLength(3));
    expect(
      analysis.insights.take(2).map((value) => value.kind),
      everyElement(PostGameReviewInsightKind.materialChange),
    );
    expect(analysis.insights.take(2).map((value) => value.ply), <int>[5, 6]);
    expect(analysis.insights.first.side, GameSide.white);
    expect(analysis.insights.first.capturedPiece, PostGameReviewPiece.pawn);
    expect(analysis.insights.first.captureSquare, 27);

    final promotion = await analyzer.analyze(
      _playedFrom('7k/P7/8/8/8/8/7p/7K w - - 0 1', <String>['a7a8q']),
    );
    expect(
      promotion.insights.map((value) => value.kind),
      contains(PostGameReviewInsightKind.materialChange),
    );
    expect(
      promotion.insights
          .firstWhere(
            (value) => value.kind == PostGameReviewInsightKind.materialChange,
          )
          .promotion,
      PromotionPiece.queen,
    );
  });

  test('candidate line is legal, stable, bounded, and budgeted', () async {
    final archive = _played(<String>['e2e4', 'e7e5', 'g1f3']);
    final first = await analyzer.analyze(archive);
    final second = await analyzer.analyze(archive);
    final line = first.insights
        .singleWhere(
          (value) => value.kind == PostGameReviewInsightKind.candidateLine,
        )
        .continuation;

    expect(line, second.insights.single.continuation);
    expect(line, hasLength(lessThanOrEqualTo(3)));
    final noNodes = await const BoundedLocalPostGameReviewAnalyzer(
      nodeBudget: 0,
    ).analyze(archive);
    expect(noNodes.insights, isEmpty);
  });

  test(
    'candidate line ranks material over lexicographic quiet moves',
    () async {
      final analysis = await analyzer.analyze(
        _played(<String>['e2e4', 'd7d5']),
      );
      final line = analysis.insights
          .singleWhere(
            (value) => value.kind == PostGameReviewInsightKind.candidateLine,
          )
          .continuation;

      expect(line.first, 'e4d5');
    },
  );

  test('candidate line prefers queen capture over pawn capture', () async {
    final analysis = await analyzer.analyze(
      _singlePosition('6k1/8/8/8/p2Q3q/8/8/1K6 w - - 0 1'),
    );
    final line = analysis.insights
        .singleWhere(
          (value) => value.kind == PostGameReviewInsightKind.candidateLine,
        )
        .continuation;
    expect(line.first, 'd4h4');
  });

  test('candidate line avoids an immediate poisoned capture', () async {
    final analysis = await analyzer.analyze(
      _singlePosition('3qk3/8/8/3p4/3Q4/8/8/4K3 w - - 0 1'),
    );
    final line = analysis.insights
        .singleWhere(
          (value) => value.kind == PostGameReviewInsightKind.candidateLine,
        )
        .continuation;
    expect(line.first, isNot('d4d5'));
  });

  test(
    'fails softly for illegal archives and rejects illegal continuations',
    () async {
      final legal = _played(<String>['e2e4']);
      final illegal = _singlePosition('8/8/8/8/8/8/4k3/4K3 w - - 0 1');
      await expectLater(analyzer.analyze(illegal), throwsFormatException);
      expect(
        () => PostGameReviewAnalysis.forArchive(
          archive: legal,
          insights: <PostGameReviewInsight>[
            PostGameReviewInsight(
              ply: 1,
              kind: PostGameReviewInsightKind.candidateLine,
              continuation: const <String>['e2e4'],
            ),
          ],
        ),
        throwsFormatException,
      );
    },
  );

  test('cooperatively yields and cancels before completing', () async {
    final cancellation = PostGameReviewCancellation();
    var yields = 0;
    final cooperative = BoundedLocalPostGameReviewAnalyzer(
      yieldEvery: 1,
      yieldControl: () async {
        yields++;
        cancellation.cancel();
      },
    );

    await expectLater(
      cooperative.analyze(
        _played(<String>['e2e4']),
        cancellation: cancellation,
      ),
      throwsA(isA<PostGameReviewCancelled>()),
    );
    expect(yields, 1);
  });

  test(
    'validates a maximum-length archive within the fixed zero-node budget',
    () async {
      const cycle = <String>['g1f3', 'g8f6', 'f3g1', 'f6g8'];
      final moves = List<String>.generate(
        gameArchiveMaximumPlies,
        (index) => cycle[index % cycle.length],
      );
      final analysis = await const BoundedLocalPostGameReviewAnalyzer(
        nodeBudget: 0,
      ).analyze(_played(moves));

      expect(analysis.insights, isEmpty);
    },
  );

  test(
    'engine owner compares played moves sequentially and publishes once',
    () async {
      final provider = _ReviewProvider();
      final runtime = EngineRuntime(provider: provider);
      final archive = _played(<String>['e2e4', 'e7e5']);
      final analysis = await EngineBackedPostGameReviewAnalyzer(
        runtime: runtime,
        maximumPositions: 2,
        limits: EngineSearchLimits(depth: 8, multiPv: 2),
      ).analyze(archive);

      expect(provider.positionRevisions, <int>[0, 1]);
      expect(
        analysis.insights.map((insight) => insight.continuation.first),
        <String>['d2d4', 'c7c5'],
      );
      expect(analysis.matches(archive), isTrue);
      await runtime.close();
    },
  );

  test(
    'engine owner falls back atomically when provider is unavailable',
    () async {
      final archive = _played(<String>['e2e4', 'd7d5']);
      final expected = await analyzer.analyze(archive);
      final runtime = EngineRuntime();
      final actual = await EngineBackedPostGameReviewAnalyzer(
        runtime: runtime,
      ).analyze(archive);

      expect(
        actual.insights.map((insight) => insight.continuation),
        expected.insights.map((insight) => insight.continuation),
      );
      await runtime.close();
    },
  );

  test(
    'engine owner skips positions when played move is outside MultiPV',
    () async {
      final provider = _ReviewProvider(omitPlayedMove: true);
      final runtime = EngineRuntime(provider: provider);
      final analysis = await EngineBackedPostGameReviewAnalyzer(
        runtime: runtime,
        maximumPositions: 1,
        limits: EngineSearchLimits(depth: 8, multiPv: 2),
      ).analyze(_played(<String>['e2e4']));

      expect(analysis.insights, isEmpty);
      await runtime.close();
    },
  );

  test('engine owner enforces its search-count bound in release code', () {
    final runtime = EngineRuntime();
    for (final value in <int>[0, 13, 1000000]) {
      expect(
        () => EngineBackedPostGameReviewAnalyzer(
          runtime: runtime,
          maximumPositions: value,
        ),
        throwsArgumentError,
      );
    }
  });

  test('computer-move preemption cannot publish a partial review', () async {
    final provider = _PreemptedReviewProvider();
    final runtime = EngineRuntime(provider: provider);
    final fallback = _CountingFallback();
    final cache = PostGameReviewAnalysisCache();
    final archive = _played(<String>['e2e4']);
    final analysisFuture = EngineBackedPostGameReviewAnalyzer(
      runtime: runtime,
      fallback: fallback,
      cache: cache,
      maximumPositions: 1,
      limits: EngineSearchLimits(depth: 8, multiPv: 2),
    ).analyze(archive);
    await provider.reviewInfoPublished.future;

    final play = runtime.openSession();
    final playFuture = play
        .searchComputerMove(
          fen: archive.positions.first.fen,
          positionRevision: 0,
          limits: EngineSearchLimits(depth: 8),
        )
        .toList();
    provider.releaseReview.complete();

    final analysis = await analysisFuture;
    expect(fallback.calls, 1);
    expect(cache.entryCount, 0);
    expect(analysis.insights, isEmpty);
    expect((await playFuture).last, isA<EngineSearchCompleted>());
    await runtime.close();
  });

  test(
    'failure after a real first insight falls back without partial cache',
    () async {
      final provider = _SecondPositionFailureProvider();
      final runtime = EngineRuntime(provider: provider);
      final fallback = _CountingFallback();
      final cache = PostGameReviewAnalysisCache();
      final archive = _played(<String>['e2e4', 'e7e5']);

      final analysis = await EngineBackedPostGameReviewAnalyzer(
        runtime: runtime,
        fallback: fallback,
        cache: cache,
        maximumPositions: 2,
        limits: EngineSearchLimits(depth: 8, multiPv: 2),
      ).analyze(archive);

      expect(provider.positionRevisions, <int>[0, 1]);
      expect(fallback.calls, 1);
      expect(cache.entryCount, 0);
      expect(analysis.insights, isEmpty);
      await runtime.close();
    },
  );

  test('completed engine review is reused from the bounded cache', () async {
    final provider = _ReviewProvider();
    final runtime = EngineRuntime(provider: provider);
    final cache = PostGameReviewAnalysisCache(maximumBytes: 4096);
    final engineAnalyzer = EngineBackedPostGameReviewAnalyzer(
      runtime: runtime,
      cache: cache,
      maximumPositions: 2,
      limits: EngineSearchLimits(depth: 8, multiPv: 2),
    );
    final archive = _played(<String>['e2e4', 'e7e5']);

    final first = await engineAnalyzer.analyze(archive);
    final second = await engineAnalyzer.analyze(archive);

    expect(second, same(first));
    expect(provider.positionRevisions, <int>[0, 1]);
    expect(cache.entryCount, 1);
    expect(cache.usedBytes, lessThanOrEqualTo(cache.maximumBytes));
    await runtime.close();
  });

  test('cache key fences engine version and search limits', () async {
    final cache = PostGameReviewAnalysisCache();
    final archive = _played(<String>['e2e4']);
    final firstProvider = _ReviewProvider(engineVersion: '1');
    final firstRuntime = EngineRuntime(provider: firstProvider);
    await EngineBackedPostGameReviewAnalyzer(
      runtime: firstRuntime,
      cache: cache,
      maximumPositions: 1,
      limits: EngineSearchLimits(depth: 8, multiPv: 2),
    ).analyze(archive);
    await EngineBackedPostGameReviewAnalyzer(
      runtime: firstRuntime,
      cache: cache,
      maximumPositions: 1,
      limits: EngineSearchLimits(
        moveTime: const Duration(milliseconds: 50),
        nodes: 100,
        depth: 9,
        multiPv: 2,
      ),
    ).analyze(archive);
    expect(firstProvider.positionRevisions, <int>[0, 0]);
    await firstRuntime.close();

    final secondProvider = _ReviewProvider(engineVersion: '2');
    final secondRuntime = EngineRuntime(provider: secondProvider);
    await EngineBackedPostGameReviewAnalyzer(
      runtime: secondRuntime,
      cache: cache,
      maximumPositions: 1,
      limits: EngineSearchLimits(depth: 8, multiPv: 2),
    ).analyze(archive);
    expect(secondProvider.positionRevisions, <int>[0]);
    expect(cache.entryCount, 3);
    await secondRuntime.close();
  });

  test(
    'cache key fences played line, revision, origin, and provider ID',
    () async {
      final cache = PostGameReviewAnalysisCache();
      final provider = _ReviewProvider();
      final runtime = EngineRuntime(provider: provider);
      final engineAnalyzer = EngineBackedPostGameReviewAnalyzer(
        runtime: runtime,
        cache: cache,
        maximumPositions: 1,
        limits: EngineSearchLimits(depth: 8, multiPv: 2),
      );
      final e4 = _withCacheIdentity(_played(<String>['e2e4']));
      final d4 = _withCacheIdentity(_played(<String>['d2d4']));
      final revised = _withCacheIdentity(
        _played(<String>['d2d4']),
        lineRevision: 2,
      );
      final appOrigin = _withCacheIdentity(
        _played(<String>['d2d4']),
        lineRevision: 2,
        origin: GameArchiveOrigin.app,
      );

      await engineAnalyzer.analyze(e4);
      await engineAnalyzer.analyze(d4);
      await engineAnalyzer.analyze(revised);
      await engineAnalyzer.analyze(appOrigin);
      expect(provider.positionRevisions, <int>[0, 0, 0, 0]);
      await runtime.close();

      final otherProvider = _ReviewProvider(providerId: 'review-fixture-b');
      final otherRuntime = EngineRuntime(provider: otherProvider);
      await EngineBackedPostGameReviewAnalyzer(
        runtime: otherRuntime,
        cache: cache,
        maximumPositions: 1,
        limits: EngineSearchLimits(depth: 8, multiPv: 2),
      ).analyze(e4);
      expect(otherProvider.positionRevisions, <int>[0]);
      await otherRuntime.close();
    },
  );

  test('cache enforces byte and entry bounds with LRU eviction', () {
    final cache = PostGameReviewAnalysisCache(
      maximumBytes: 1024,
      maximumEntries: 2,
    );
    final first = PostGameReviewAnalysis.forArchive(
      archive: _played(<String>['e2e4']),
      insights: const <PostGameReviewInsight>[],
    );
    final second = PostGameReviewAnalysis.forArchive(
      archive: _played(<String>['d2d4']),
      insights: const <PostGameReviewInsight>[],
    );
    final third = PostGameReviewAnalysis.forArchive(
      archive: _played(<String>['c2c4']),
      insights: const <PostGameReviewInsight>[],
    );
    cache
      ..write('first', first)
      ..write('second', second);
    expect(cache.read('first'), same(first));
    cache.write('third', third);

    expect(cache.entryCount, 2);
    expect(cache.read('second'), isNull);
    expect(cache.read('first'), same(first));
    expect(cache.read('third'), same(third));
    expect(cache.usedBytes, lessThanOrEqualTo(cache.maximumBytes));

    final byteBound = PostGameReviewAnalysisCache(
      maximumBytes: 520,
      maximumEntries: 10,
    );
    byteBound
      ..write('first', first)
      ..write('second', second);
    expect(byteBound.entryCount, 1);
    expect(byteBound.read('first'), isNull);
    expect(byteBound.read('second'), same(second));
    final beforeOversized = byteBound.usedBytes;
    byteBound.write('x' * 1024, third);
    expect(byteBound.entryCount, 1);
    expect(byteBound.usedBytes, beforeOversized);
    byteBound.write('second', third);
    expect(byteBound.entryCount, 1);
    expect(byteBound.read('second'), same(third));
  });

  test(
    'review job owner cancels its predecessor before starting again',
    () async {
      final analyzer = _ControlledReviewAnalyzer();
      final owner = PostGameReviewJobOwner(
        analyzer: analyzer,
        timeout: const Duration(seconds: 1),
      );
      final firstArchive = _played(<String>['e2e4']);
      final secondArchive = _played(<String>['d2d4']);
      final first = owner.start(firstArchive);
      final firstFailure = expectLater(
        first.result,
        throwsA(isA<PostGameReviewCancelled>()),
      );
      await Future<void>.delayed(Duration.zero);
      final second = owner.start(secondArchive);
      await Future<void>.delayed(Duration.zero);

      await firstFailure;
      expect(analyzer.cancellations.first.isCancelled, isTrue);
      analyzer.results[1].complete(
        PostGameReviewAnalysis.forArchive(
          archive: secondArchive,
          insights: const <PostGameReviewInsight>[],
        ),
      );
      expect(await second.result, isA<PostGameReviewAnalysis>());
      expect(owner.active, isFalse);
      owner.close();
    },
  );

  test(
    'review job owner times out and retires an uncooperative analyzer',
    () async {
      final analyzer = _ControlledReviewAnalyzer();
      final owner = PostGameReviewJobOwner(
        analyzer: analyzer,
        timeout: const Duration(milliseconds: 10),
      );
      final job = owner.start(_played(<String>['e2e4']));

      await expectLater(job.result, throwsA(isA<TimeoutException>()));
      expect(analyzer.cancellations.single.isCancelled, isTrue);
      expect(owner.active, isFalse);
      owner.close();
    },
  );

  test('late timed-out success cannot displace its replacement', () async {
    final analyzer = _ControlledReviewAnalyzer();
    final owner = PostGameReviewJobOwner(
      analyzer: analyzer,
      timeout: const Duration(milliseconds: 10),
    );
    final firstArchive = _played(<String>['e2e4']);
    final secondArchive = _played(<String>['d2d4']);
    final first = owner.start(firstArchive);
    await Future<void>.delayed(Duration.zero);
    await expectLater(first.result, throwsA(isA<TimeoutException>()));

    final second = owner.start(secondArchive);
    await Future<void>.delayed(Duration.zero);
    analyzer.results.first.complete(
      PostGameReviewAnalysis.forArchive(
        archive: firstArchive,
        insights: const <PostGameReviewInsight>[],
      ),
    );
    expect(owner.active, isTrue);
    final expected = PostGameReviewAnalysis.forArchive(
      archive: secondArchive,
      insights: const <PostGameReviewInsight>[],
    );
    analyzer.results[1].complete(expected);

    expect(await second.result, same(expected));
    expect(owner.active, isFalse);
    owner.close();
  });

  test('late timed-out error cannot displace its replacement', () async {
    final analyzer = _ControlledReviewAnalyzer();
    final owner = PostGameReviewJobOwner(
      analyzer: analyzer,
      timeout: const Duration(milliseconds: 10),
    );
    final first = owner.start(_played(<String>['e2e4']));
    await Future<void>.delayed(Duration.zero);
    await expectLater(first.result, throwsA(isA<TimeoutException>()));

    final secondArchive = _played(<String>['d2d4']);
    final second = owner.start(secondArchive);
    await Future<void>.delayed(Duration.zero);
    analyzer.results.first.completeError(StateError('late engine error'));
    expect(owner.active, isTrue);
    final expected = PostGameReviewAnalysis.forArchive(
      archive: secondArchive,
      insights: const <PostGameReviewInsight>[],
    );
    analyzer.results[1].complete(expected);

    expect(await second.result, same(expected));
    expect(owner.active, isFalse);
    owner.close();
  });

  test(
    'review job owner rejects a result for a stale archive revision',
    () async {
      final analyzer = _ControlledReviewAnalyzer();
      final owner = PostGameReviewJobOwner(analyzer: analyzer);
      final requested = _withCacheIdentity(
        _played(<String>['e2e4']),
        lineRevision: 1,
      );
      final stale = _withCacheIdentity(
        _played(<String>['e2e4']),
        lineRevision: 2,
      );
      final job = owner.start(requested);
      await Future<void>.delayed(Duration.zero);
      analyzer.results.single.complete(
        PostGameReviewAnalysis.forArchive(
          archive: stale,
          insights: const <PostGameReviewInsight>[],
        ),
      );

      await expectLater(job.result, throwsA(isA<FormatException>()));
      owner.close();
    },
  );

  test('review owner resists cancel, start, and close reentrancy', () async {
    final analyzer = _ControlledReviewAnalyzer();
    final owner = PostGameReviewJobOwner(analyzer: analyzer);
    final first = owner.start(_played(<String>['e2e4']));
    final firstFailure = expectLater(
      first.result,
      throwsA(isA<PostGameReviewCancelled>()),
    );
    await Future<void>.delayed(Duration.zero);
    var nestedStartRejected = false;
    analyzer.cancellations.single.addListener(() {
      first.cancel();
      try {
        owner.start(_played(<String>['d2d4']));
      } on StateError {
        nestedStartRejected = true;
      }
      owner.close();
    });

    expect(() => owner.start(_played(<String>['c2c4'])), throwsStateError);
    await firstFailure;
    expect(nestedStartRejected, isTrue);
    expect(owner.active, isFalse);
    analyzer.results.single.completeError(StateError('late analyzer error'));
    await Future<void>.delayed(Duration.zero);
  });
}

class _ControlledReviewAnalyzer implements LocalPostGameReviewAnalyzer {
  final List<PostGameReviewCancellation> cancellations =
      <PostGameReviewCancellation>[];
  final List<Completer<PostGameReviewAnalysis>> results =
      <Completer<PostGameReviewAnalysis>>[];

  @override
  Future<PostGameReviewAnalysis> analyze(
    GameArchive archive, {
    PostGameReviewCancellation? cancellation,
  }) {
    cancellations.add(cancellation!);
    final result = Completer<PostGameReviewAnalysis>();
    results.add(result);
    return result.future;
  }
}

class _CountingFallback implements LocalPostGameReviewAnalyzer {
  int calls = 0;

  @override
  Future<PostGameReviewAnalysis> analyze(
    GameArchive archive, {
    PostGameReviewCancellation? cancellation,
  }) async {
    calls++;
    cancellation?.throwIfCancelled();
    return PostGameReviewAnalysis.forArchive(
      archive: archive,
      insights: const <PostGameReviewInsight>[],
    );
  }
}

class _PreemptedReviewProvider implements ChessEngineProvider {
  final Completer<void> reviewInfoPublished = Completer<void>();
  final Completer<void> releaseReview = Completer<void>();

  @override
  EngineCapabilities get capabilities => EngineCapabilities(
    providerId: 'preemption-fixture',
    engineVersion: '1',
    privacyClass: EnginePrivacyClass.onDevice,
    variants: <ChessEngineVariant>{ChessEngineVariant.classic},
    purposes: <EngineSearchPurpose>{
      EngineSearchPurpose.computerMove,
      EngineSearchPurpose.postGameAnalysis,
    },
    supportsDepth: true,
    maximumMultiPv: 2,
  );

  @override
  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  ) async* {
    yield EngineSearchStarted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );
    if (request.purpose == EngineSearchPurpose.postGameAnalysis) {
      yield EngineSearchInfo(
        jobId: request.jobId,
        sessionId: request.sessionId,
        positionRevision: request.positionRevision,
        depth: 8,
        nodes: 1000,
        elapsed: const Duration(milliseconds: 20),
        score: EngineScore.centipawns(30),
        principalVariation: const <String>['d2d4'],
      );
      yield EngineSearchInfo(
        jobId: request.jobId,
        sessionId: request.sessionId,
        positionRevision: request.positionRevision,
        depth: 8,
        nodes: 1000,
        elapsed: const Duration(milliseconds: 20),
        score: EngineScore.centipawns(10),
        principalVariation: const <String>['e2e4'],
        multiPv: 2,
      );
      reviewInfoPublished.complete();
      await releaseReview.future;
      return;
    }
    yield EngineBestMove(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
      uci: 'e2e4',
    );
    yield EngineSearchCompleted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );
  }

  @override
  Future<void> close() async {}
}

class _SecondPositionFailureProvider implements ChessEngineProvider {
  final List<int> positionRevisions = <int>[];

  @override
  EngineCapabilities get capabilities => EngineCapabilities(
    providerId: 'second-position-failure-fixture',
    engineVersion: '1',
    privacyClass: EnginePrivacyClass.onDevice,
    variants: <ChessEngineVariant>{ChessEngineVariant.classic},
    purposes: <EngineSearchPurpose>{EngineSearchPurpose.postGameAnalysis},
    supportsDepth: true,
    maximumMultiPv: 2,
  );

  @override
  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  ) async* {
    positionRevisions.add(request.positionRevision);
    yield EngineSearchStarted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );
    if (request.positionRevision == 1) {
      yield EngineSearchFailed(
        jobId: request.jobId,
        sessionId: request.sessionId,
        positionRevision: request.positionRevision,
        reason: EngineFailureReason.unavailable,
      );
      return;
    }
    yield EngineSearchInfo(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
      depth: 8,
      nodes: 1000,
      elapsed: const Duration(milliseconds: 20),
      score: EngineScore.centipawns(30),
      principalVariation: const <String>['d2d4'],
    );
    yield EngineSearchInfo(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
      depth: 8,
      nodes: 1000,
      elapsed: const Duration(milliseconds: 20),
      score: EngineScore.centipawns(10),
      principalVariation: const <String>['e2e4'],
      multiPv: 2,
    );
    yield EngineBestMove(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
      uci: 'd2d4',
    );
    yield EngineSearchCompleted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );
  }

  @override
  Future<void> close() async {}
}

class _ReviewProvider implements ChessEngineProvider {
  _ReviewProvider({
    this.omitPlayedMove = false,
    this.providerId = 'review-fixture',
    this.engineVersion = '1',
  });

  final bool omitPlayedMove;
  final String providerId;
  final String engineVersion;
  final List<int> positionRevisions = <int>[];

  @override
  EngineCapabilities get capabilities => EngineCapabilities(
    providerId: providerId,
    engineVersion: engineVersion,
    privacyClass: EnginePrivacyClass.onDevice,
    variants: <ChessEngineVariant>{ChessEngineVariant.classic},
    purposes: <EngineSearchPurpose>{EngineSearchPurpose.postGameAnalysis},
    supportsDepth: true,
    supportsNodes: true,
    maximumMultiPv: 2,
  );

  @override
  Stream<EngineSearchEvent> search(
    EngineSearchRequest request,
    EngineCancellation cancellation,
  ) async* {
    positionRevisions.add(request.positionRevision);
    final white = request.positionRevision == 0;
    final best = white ? 'd2d4' : 'c7c5';
    final played = white ? 'e2e4' : 'e7e5';
    yield EngineSearchStarted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );
    yield EngineSearchInfo(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
      depth: 8,
      nodes: 1000,
      elapsed: const Duration(milliseconds: 20),
      score: EngineScore.centipawns(white ? 30 : 5),
      principalVariation: <String>[best],
    );
    if (!omitPlayedMove) {
      yield EngineSearchInfo(
        jobId: request.jobId,
        sessionId: request.sessionId,
        positionRevision: request.positionRevision,
        depth: 8,
        nodes: 1000,
        elapsed: const Duration(milliseconds: 20),
        score: EngineScore.centipawns(white ? 10 : 20),
        principalVariation: <String>[played],
        multiPv: 2,
      );
    }
    yield EngineBestMove(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
      uci: best,
    );
    yield EngineSearchCompleted(
      jobId: request.jobId,
      sessionId: request.sessionId,
      positionRevision: request.positionRevision,
    );
  }

  @override
  Future<void> close() async {}
}

GameArchive _singlePosition(String fen) {
  final now = DateTime.utc(2026, 9, 11);
  return GameArchive(
    libraryId: 'single-${fen.hashCode}',
    boardGameId: 'board-${fen.hashCode}',
    createdAt: now,
    updatedAt: now,
    status: GameArchiveStatus.completed,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.draw,
    winner: GameWinner.draw,
    lineRevision: 1,
    positions: <ArchivedPosition>[ArchivedPosition(ply: 0, fen: fen)],
    transitions: const <ArchivedTransition>[],
  );
}

GameArchive _played(List<String> moves) {
  return _playedFrom(
    'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
    moves,
  );
}

GameArchive _withCacheIdentity(
  GameArchive archive, {
  int lineRevision = 1,
  GameArchiveOrigin origin = GameArchiveOrigin.board,
}) => GameArchive(
  libraryId: 'cache-library',
  boardGameId: origin == GameArchiveOrigin.board ? 'cache-board' : null,
  gameId: 'cache-board',
  origin: origin,
  createdAt: archive.createdAt,
  updatedAt: archive.updatedAt,
  status: archive.status,
  captureQuality: archive.captureQuality,
  variant: archive.variant,
  outcome: archive.outcome,
  winner: archive.winner,
  lineRevision: lineRevision,
  positions: archive.positions,
  transitions: archive.transitions,
);

GameArchive _playedFrom(String fen, List<String> moves) {
  final now = DateTime.utc(2026, 9, 11);
  final identity = Object.hash(fen, Object.hashAll(moves));
  final seed = GameArchive(
    libraryId: 'played-$identity',
    boardGameId: 'board-$identity',
    createdAt: now,
    updatedAt: now,
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 1,
    positions: <ArchivedPosition>[ArchivedPosition(ply: 0, fen: fen)],
    transitions: const <ArchivedTransition>[],
  );
  final played = replayStandardChessExtension(seed, moves);
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
