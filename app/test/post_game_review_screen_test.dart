import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/models/provider_game_provenance.dart';
import 'package:litch_mobile/src/screens/post_game_review_screen.dart';
import 'package:litch_mobile/src/services/post_game_review_analyzer.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';

import 'post_game_review_test_fixture.dart';

void main() {
  Widget harness({
    required GameArchive archive,
    LocalPostGameReviewAnalyzer? analyzer,
    TextScaler textScaler = TextScaler.noScaling,
  }) {
    final PostGameReviewJobOwner? reviewOwner = analyzer == null
        ? null
        : PostGameReviewJobOwner(analyzer: analyzer);
    return MaterialApp(
      theme: buildLitchTheme(),
      builder: (BuildContext context, Widget? child) => MediaQuery(
        data: MediaQuery.of(context).copyWith(textScaler: textScaler),
        child: child!,
      ),
      home: PostGameReviewScreen(archive: archive, reviewOwner: reviewOwner),
    );
  }

  testWidgets('replays every archived position and jumps to key moments', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final GameArchive archive = completedClassicArchive();
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer(
      (GameArchive value) async => PostGameReviewAnalysis.forArchive(
        archive: value,
        insights: <PostGameReviewInsight>[
          PostGameReviewInsight(
            ply: 1,
            kind: PostGameReviewInsightKind.candidateLine,
            continuation: const <String>['e7e5', 'g1f3'],
          ),
          PostGameReviewInsight(
            ply: 3,
            kind: PostGameReviewInsightKind.candidateLine,
            continuation: const <String>['a7a6'],
          ),
        ],
      ),
    );

    await tester.pumpWidget(harness(archive: archive, analyzer: analyzer));
    await tester.pumpAndSettle();

    expect(analyzer.calls, 1);
    expect(find.text('Position 4 of 4'), findsOneWidget);
    expect(find.text('P1 (W) · Move 2 · g1–f3'), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-f3')), findsOneWidget);
    expect(find.text('Quick review'), findsOneWidget);
    expect(
      find.textContaining('One line to explore: e7–e5 · g1–f3'),
      findsOneWidget,
    );
    expect(
      find.bySemanticsLabel('Quick review ready. 2 observations.'),
      findsOneWidget,
    );

    await tester.tap(
      find.byKey(const ValueKey<String>('review-first-position')),
    );
    await tester.pump();
    expect(find.text('Starting position · 1 of 4'), findsOneWidget);
    expect(find.text('Before the first move'), findsOneWidget);

    await tester.tap(
      find.byKey(const ValueKey<String>('review-next-position')),
    );
    await tester.pump();
    expect(find.text('Position 2 of 4'), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-e4')), findsOneWidget);

    await tester.tap(
      find.byKey(const ValueKey<String>('review-choose-position')),
    );
    await tester.pumpAndSettle();
    expect(find.text('Choose a move'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey<String>('review-position-2')));
    await tester.pumpAndSettle();
    expect(find.text('Position 3 of 4'), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('piece-e5')), findsOneWidget);

    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('review-moment-0')),
      240,
      scrollable: find.byType(Scrollable).first,
    );
    await tester.tap(find.byKey(const ValueKey<String>('review-moment-0')));
    await tester.pump();
    expect(find.text('Position 2 of 4'), findsOneWidget);
  });

  testWidgets('keeps analysis post-game and Duck replay-only', (
    WidgetTester tester,
  ) async {
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer(
      (GameArchive archive) async => PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: const [],
      ),
    );

    await tester.pumpWidget(
      harness(archive: duckArchive(), analyzer: analyzer),
    );
    await tester.pumpAndSettle();

    expect(analyzer.calls, 0);
    expect(
      find.text('Duck analysis isn’t available yet. Replay remains available.'),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey<String>('duck-marker-e5')), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('duck-marker-d5')),
      findsOneWidget,
    );
    expect(find.text('P1 (W) · Move 1 · e2–e4 · Duck e5–d5'), findsOneWidget);
    expect(
      find.bySemanticsLabel(RegExp(r'Duck moved from e5 to d5\.')),
      findsOneWidget,
    );

    await tester.pumpWidget(
      harness(archive: activeClassicArchive(), analyzer: analyzer),
    );
    await tester.pumpAndSettle();

    expect(analyzer.calls, 0);
    expect(
      find.text(
        'Analysis stays off until this game ends. Replay remains available.',
      ),
      findsOneWidget,
    );

    await tester.pumpWidget(
      harness(archive: completedClassicArchive(), analyzer: analyzer),
    );
    await tester.pumpAndSettle();

    expect(analyzer.calls, 1);
    expect(find.text('Quick review'), findsOneWidget);
  });

  testWidgets('accepts quick review for an app-owned completed game', (
    WidgetTester tester,
  ) async {
    final GameArchive source = completedClassicArchive();
    final GameArchive appArchive = GameArchive(
      libraryId: 'app-review-screen',
      gameId: 'app-review-screen-id',
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
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer(
      (GameArchive archive) async => PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: const <PostGameReviewInsight>[],
      ),
    );

    await tester.pumpWidget(harness(archive: appArchive, analyzer: analyzer));
    await tester.pumpAndSettle();

    expect(analyzer.calls, 1);
    expect(find.text('Quick review'), findsOneWidget);
    expect(find.textContaining('Analysis is unavailable'), findsNothing);
  });

  testWidgets('mate-in-one copy includes the verified move', (
    WidgetTester tester,
  ) async {
    final archive = mateInOneArchive();
    final analyzer = _FixtureAnalyzer(
      (GameArchive value) async => PostGameReviewAnalysis.forArchive(
        archive: value,
        insights: <PostGameReviewInsight>[
          PostGameReviewInsight(
            ply: 0,
            kind: PostGameReviewInsightKind.mateInOne,
            side: GameSide.white,
            continuation: const <String>['f7g7'],
          ),
        ],
      ),
    );

    await tester.pumpWidget(harness(archive: archive, analyzer: analyzer));
    await tester.pumpAndSettle();

    expect(
      find.text('Mate in one for P1 (W) · Starting position'),
      findsOneWidget,
    );
    expect(find.text('Verified checkmate: f7–g7'), findsOneWidget);
  });

  testWidgets('fails softly when local analysis is unavailable or stale', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive();
    final _FixtureAnalyzer failing = _FixtureAnalyzer(
      (_) => Future<PostGameReviewAnalysis>.error(StateError('not ready')),
    );
    await tester.pumpWidget(harness(archive: archive, analyzer: failing));
    await tester.pumpAndSettle();

    expect(
      find.text('Analysis wasn’t available. Replay remains available.'),
      findsOneWidget,
    );
    expect(
      find.bySemanticsLabel(
        'Analysis wasn’t available. Replay remains available.',
      ),
      findsOneWidget,
    );
    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('review-retry-analysis')))
          .height,
      greaterThanOrEqualTo(48),
    );
    expect(find.text('Position 4 of 4'), findsOneWidget);

    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('review-retry-analysis')),
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('review-retry-analysis')),
    );
    await tester.pumpAndSettle();
    expect(failing.calls, 2);

    final GameArchive otherArchive = completedClassicArchiveWithRevision(4);
    final _FixtureAnalyzer stale = _FixtureAnalyzer(
      (_) async => PostGameReviewAnalysis.forArchive(
        archive: otherArchive,
        insights: const <PostGameReviewInsight>[],
      ),
    );
    await tester.pumpWidget(harness(archive: archive, analyzer: stale));
    await tester.pumpAndSettle();

    expect(
      find.text('Analysis wasn’t available. Replay remains available.'),
      findsOneWidget,
    );
  });

  testWidgets('times out stalled analysis and allows retry', (
    WidgetTester tester,
  ) async {
    final Completer<PostGameReviewAnalysis> stalled =
        Completer<PostGameReviewAnalysis>();
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer((_) => stalled.future);

    await tester.pumpWidget(
      harness(archive: completedClassicArchive(), analyzer: analyzer),
    );
    await tester.pump();
    expect(find.text('Reviewing this game on this device…'), findsOneWidget);

    await tester.pump(const Duration(seconds: 21));
    expect(
      find.text('Analysis wasn’t available. Replay remains available.'),
      findsOneWidget,
    );
    expect(analyzer.cancellations.single.isCancelled, isTrue);

    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('review-retry-analysis')),
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('review-retry-analysis')),
    );
    await tester.pump();
    expect(analyzer.calls, 2);
    expect(find.text('Reviewing this game on this device…'), findsOneWidget);
    stalled.complete(
      PostGameReviewAnalysis.forArchive(
        archive: completedClassicArchive(),
        insights: const <PostGameReviewInsight>[],
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Quick review'), findsOneWidget);
  });

  testWidgets('keeps partial replay honest and does not request analysis', (
    WidgetTester tester,
  ) async {
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer(
      (GameArchive archive) async => PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: const [],
      ),
    );

    await tester.pumpWidget(
      harness(archive: partialClassicArchive(), analyzer: analyzer),
    );
    await tester.pumpAndSettle();

    expect(analyzer.calls, 0);
    expect(find.text('Partial replay'), findsOneWidget);
    expect(
      find.text('This archive starts partway through, so analysis is paused.'),
      findsOneWidget,
    );
    await tester.ensureVisible(
      find.byKey(const ValueKey<String>('review-first-position')),
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('review-first-position')),
    );
    await tester.pump();
    expect(find.text('Archive begins after p1 (w) move 1'), findsOneWidget);
    expect(find.textContaining('ply'), findsNothing);
  });

  testWidgets('exposes calm local progress and ignores late results', (
    WidgetTester tester,
  ) async {
    final Completer<PostGameReviewAnalysis> completer =
        Completer<PostGameReviewAnalysis>();
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer((_) => completer.future);

    await tester.pumpWidget(
      harness(archive: completedClassicArchive(), analyzer: analyzer),
    );
    await tester.pump();

    expect(find.text('Reviewing this game on this device…'), findsOneWidget);

    await tester.pumpWidget(
      harness(archive: duckArchive(), analyzer: analyzer),
    );
    completer.complete(
      PostGameReviewAnalysis.forArchive(
        archive: completedClassicArchive(),
        insights: const <PostGameReviewInsight>[],
      ),
    );
    await tester.pumpAndSettle();

    expect(
      find.text(
        'Material changes and short lines to explore. Longer tactics may be missed.',
      ),
      findsNothing,
    );
    expect(
      find.text('Duck analysis isn’t available yet. Replay remains available.'),
      findsOneWidget,
    );
  });

  testWidgets('replacing app-lifetime analyzer cancels and reloads review', (
    WidgetTester tester,
  ) async {
    final archive = completedClassicArchive();
    final firstResult = Completer<PostGameReviewAnalysis>();
    final first = _FixtureAnalyzer((_) => firstResult.future);
    final second = _FixtureAnalyzer(
      (value) async => PostGameReviewAnalysis.forArchive(
        archive: value,
        insights: const <PostGameReviewInsight>[],
      ),
    );

    await tester.pumpWidget(harness(archive: archive, analyzer: first));
    await tester.pump();
    expect(find.text('Reviewing this game on this device…'), findsOneWidget);

    await tester.pumpWidget(harness(archive: archive, analyzer: second));
    await tester.pumpAndSettle();

    expect(first.cancellations.single.isCancelled, isTrue);
    expect(second.calls, 1);
    expect(find.text('Quick review'), findsOneWidget);
    firstResult.complete(
      PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: const <PostGameReviewInsight>[],
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Quick review'), findsOneWidget);
  });

  testWidgets('same owner reloads when canonical game identity changes', (
    WidgetTester tester,
  ) async {
    final source = completedClassicArchive();
    GameArchive withIdentity(String gameId, GameArchiveOrigin origin) =>
        GameArchive(
          libraryId: source.libraryId,
          boardGameId: origin == GameArchiveOrigin.board ? gameId : null,
          gameId: gameId,
          origin: origin,
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
          provider: origin == GameArchiveOrigin.provider
              ? const ProviderGameProvenance(
                  provider: 'lichess',
                  gameId: 'providergame',
                  accountId: 'player',
                  localSide: 'white',
                  rawStatus: 'timeout',
                  winner: 'white',
                )
              : null,
        );
    final analyzer = _FixtureAnalyzer(
      (archive) async => PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: const <PostGameReviewInsight>[],
      ),
    );
    final owner = PostGameReviewJobOwner(analyzer: analyzer);
    addTearDown(owner.close);
    Widget app(GameArchive archive) => MaterialApp(
      theme: buildLitchTheme(),
      home: PostGameReviewScreen(archive: archive, reviewOwner: owner),
    );

    await tester.pumpWidget(app(withIdentity('app-a', GameArchiveOrigin.app)));
    await tester.pumpAndSettle();
    expect(analyzer.calls, 1);

    await tester.pumpWidget(app(withIdentity('app-b', GameArchiveOrigin.app)));
    await tester.pumpAndSettle();
    expect(analyzer.calls, 2);

    await tester.pumpWidget(
      app(withIdentity('app-b', GameArchiveOrigin.provider)),
    );
    await tester.pumpAndSettle();
    expect(analyzer.calls, 3);
    expect(find.text('Quick review'), findsOneWidget);
  });

  testWidgets('key-moment jumps announce and reveal the selected position', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 620));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();
    final GameArchive archive = completedClassicArchive();
    final _FixtureAnalyzer analyzer = _FixtureAnalyzer(
      (GameArchive value) async => PostGameReviewAnalysis.forArchive(
        archive: value,
        insights: <PostGameReviewInsight>[
          PostGameReviewInsight(
            ply: 1,
            kind: PostGameReviewInsightKind.candidateLine,
            continuation: const <String>['e7e5'],
          ),
        ],
      ),
    );

    await tester.pumpWidget(harness(archive: archive, analyzer: analyzer));
    await tester.pumpAndSettle();
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('review-moment-0')),
      300,
      scrollable: find.byType(Scrollable).first,
    );

    await tester.tap(find.byKey(const ValueKey<String>('review-moment-0')));
    await tester.pumpAndSettle();

    expect(find.text('Position 2 of 4'), findsOneWidget);
    expect(
      tester.getSemantics(
        find.byKey(const ValueKey<String>('review-position-heading')),
      ),
      isSemantics(
        label: 'Position 2 of 4. P1 (W) · Move 1 · e2–e4',
        isLiveRegion: true,
      ),
    );
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('review-moment-0')))
          .flagsCollection
          .isSelected,
      ui.Tristate.isTrue,
    );
    expect(
      tester
          .getBottomLeft(
            find.byKey(const ValueKey<String>('review-position-heading')),
          )
          .dy,
      lessThan(620),
    );
    semantics.dispose();
  });

  testWidgets('keeps navigation accessible at 200 percent text', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();

    final analyzer = _FixtureAnalyzer(
      (GameArchive archive) async => PostGameReviewAnalysis.forArchive(
        archive: archive,
        insights: <PostGameReviewInsight>[
          PostGameReviewInsight(
            ply: 3,
            kind: PostGameReviewInsightKind.materialChange,
            side: GameSide.white,
            capturedPiece: PostGameReviewPiece.pawn,
            captureSquare: 27,
          ),
          PostGameReviewInsight(
            ply: 1,
            kind: PostGameReviewInsightKind.candidateLine,
            continuation: const <String>['e7e5', 'g1f3', 'b8c6'],
          ),
        ],
      ),
    );
    await tester.pumpWidget(
      harness(
        archive: materialReviewArchive(),
        analyzer: analyzer,
        textScaler: const TextScaler.linear(2),
      ),
    );
    await tester.pumpAndSettle();

    expect(tester.takeException(), isNull);
    expect(find.text('Quick review'), findsOneWidget);
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('review-moment-1')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    expect(
      find.textContaining('P1 (W) captured a pawn on d5.'),
      findsOneWidget,
    );
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('review-moment-1')))
          .label,
      contains('P1 (W) captured a pawn on d5.'),
    );
    expect(find.textContaining('One line to explore: e7–e5'), findsOneWidget);
    for (final String key in <String>[
      'review-first-position',
      'review-previous-position',
      'review-next-position',
      'review-last-position',
      'review-rotate-board',
    ]) {
      final Finder action = find.byKey(ValueKey<String>(key));
      expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
      expect(tester.getSize(action).width, greaterThanOrEqualTo(48));
    }
    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('review-choose-position')))
          .height,
      greaterThanOrEqualTo(48),
    );
    expect(
      find.bySemanticsLabel(
        RegExp(r'^Review board\. P2 \(B\) to move\. 31 pieces shown\.'),
      ),
      findsOneWidget,
    );
    final promotionArchive = promotionReviewArchive();
    await tester.pumpWidget(
      harness(
        archive: promotionArchive,
        analyzer: _FixtureAnalyzer(
          (GameArchive archive) async => PostGameReviewAnalysis.forArchive(
            archive: archive,
            insights: <PostGameReviewInsight>[
              PostGameReviewInsight(
                ply: 1,
                kind: PostGameReviewInsightKind.materialChange,
                side: GameSide.white,
                promotion: PromotionPiece.queen,
              ),
            ],
          ),
        ),
        textScaler: const TextScaler.linear(2),
      ),
    );
    await tester.pumpAndSettle();
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('review-moment-0')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    expect(
      find.textContaining('P1 (W) promoted a pawn to a queen.'),
      findsOneWidget,
    );
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('review-moment-0')))
          .label,
      contains('P1 (W) promoted a pawn to a queen.'),
    );
    final mateArchive = mateInOneArchive();
    await tester.pumpWidget(
      harness(
        archive: mateArchive,
        analyzer: _FixtureAnalyzer(
          (GameArchive archive) async => PostGameReviewAnalysis.forArchive(
            archive: archive,
            insights: <PostGameReviewInsight>[
              PostGameReviewInsight(
                ply: 0,
                kind: PostGameReviewInsightKind.mateInOne,
                side: GameSide.white,
                continuation: const <String>['f7g7'],
              ),
            ],
          ),
        ),
        textScaler: const TextScaler.linear(2),
      ),
    );
    await tester.pumpAndSettle();
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('review-moment-0')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    expect(tester.takeException(), isNull);
    expect(find.text('Verified checkmate: f7–g7'), findsOneWidget);
    expect(
      tester
          .getSemantics(find.byKey(const ValueKey<String>('review-moment-0')))
          .label,
      contains('Verified checkmate: f7–g7'),
    );
    semantics.dispose();
  });

  testWidgets('rotates only the review board view', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();
    await tester.pumpWidget(harness(archive: completedClassicArchive()));
    await tester.pumpAndSettle();

    void expectCoordinates({required bool flipped}) {
      final files = flipped ? 'hgfedcba' : 'abcdefgh';
      final ranks = flipped ? '12345678' : '87654321';
      Finder label(String edge, String value) => find.descendant(
        of: find.byKey(ValueKey('play-$edge')),
        matching: find.text(value),
      );
      for (final index in [0, 3, 7]) {
        final file = files[index];
        final rank = ranks[index];
        final square = tester.getCenter(
          find.byKey(ValueKey('square-$file$rank')),
        );
        for (final edge in ['files-top', 'files-bottom']) {
          expect(tester.getCenter(label(edge, file)).dx, closeTo(square.dx, 1));
        }
        for (final edge in ['ranks-left', 'ranks-right']) {
          expect(tester.getCenter(label(edge, rank)).dy, closeTo(square.dy, 1));
        }
      }
      final first = tester.getCenter(label('files-top', files[0]));
      final last = tester.getCenter(label('files-top', files[7]));
      expect(first.dx, lessThan(last.dx));
    }

    expectCoordinates(flipped: false);
    final Offset before = tester.getCenter(
      find.byKey(const ValueKey<String>('square-a8')),
    );
    Semantics orientation = tester.widget<Semantics>(
      find.byKey(const ValueKey<String>('review-rotate-board')),
    );
    expect(orientation.properties.label, 'Board orientation');
    expect(orientation.properties.value, 'P1 (W) at bottom');
    expect(orientation.properties.toggled, isFalse);
    expect(find.bySemanticsLabel('a8, P2 rook'), findsOneWidget);
    final orientationNode = tester.getSemantics(
      find.byKey(const ValueKey<String>('review-rotate-board')),
    );
    expect(
      orientationNode.getSemanticsData().hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    orientationNode.owner!.performAction(
      orientationNode.id,
      ui.SemanticsAction.tap,
    );
    await tester.pump();
    final Offset after = tester.getCenter(
      find.byKey(const ValueKey<String>('square-a8')),
    );

    expect(after.dx, greaterThan(before.dx));
    expect(after.dy, greaterThan(before.dy));
    expectCoordinates(flipped: true);
    expect(find.text('P1 (W) · Move 2 · g1–f3'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('review-previous-position')));
    await tester.pump();
    expect(find.text('P2 (B) · Move 1 · e7–e5'), findsOneWidget);
    expect(find.byKey(const ValueKey('piece-e5')), findsOneWidget);
    expectCoordinates(flipped: true);
    orientation = tester.widget<Semantics>(
      find.byKey(const ValueKey<String>('review-rotate-board')),
    );
    expect(orientation.properties.value, 'P2 (B) at bottom');
    expect(orientation.properties.toggled, isTrue);
    const tabletSize = Size(1032, 1376);
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = tabletSize;
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await tester.binding.setSurfaceSize(tabletSize);
    await tester.pumpAndSettle();
    expect(
      tester
          .getCenter(find.byKey(const ValueKey('home-chessboard-surface')))
          .dx,
      closeTo(tabletSize.width / 2, 1),
    );
    semantics.dispose();
  });
}

class _FixtureAnalyzer implements LocalPostGameReviewAnalyzer {
  _FixtureAnalyzer(this.result);

  final Future<PostGameReviewAnalysis> Function(GameArchive archive) result;
  int calls = 0;
  final List<PostGameReviewCancellation> cancellations =
      <PostGameReviewCancellation>[];

  @override
  Future<PostGameReviewAnalysis> analyze(
    GameArchive archive, {
    PostGameReviewCancellation? cancellation,
  }) {
    calls += 1;
    cancellations.add(cancellation ?? PostGameReviewCancellation());
    return result(archive);
  }
}

GameArchive activeClassicArchive() {
  final GameArchive completed = completedClassicArchive();
  return GameArchive(
    libraryId: completed.libraryId,
    boardGameId: completed.boardGameId,
    createdAt: completed.createdAt,
    updatedAt: completed.updatedAt,
    status: GameArchiveStatus.active,
    captureQuality: completed.captureQuality,
    variant: completed.variant,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: completed.lineRevision,
    positions: completed.positions,
    transitions: completed.transitions,
  );
}

GameArchive completedClassicArchiveWithRevision(int revision) {
  final GameArchive archive = completedClassicArchive();
  return GameArchive(
    libraryId: archive.libraryId,
    boardGameId: archive.boardGameId,
    createdAt: archive.createdAt,
    updatedAt: archive.updatedAt,
    status: archive.status,
    captureQuality: archive.captureQuality,
    variant: archive.variant,
    outcome: archive.outcome,
    winner: archive.winner,
    lineRevision: revision,
    positions: archive.positions,
    transitions: archive.transitions,
  );
}
