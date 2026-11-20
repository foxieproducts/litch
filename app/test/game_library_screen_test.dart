import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/engine/engine_runtime.dart';
import 'package:litch_mobile/src/models/provider_game_provenance.dart';
import 'package:litch_mobile/src/screens/app_chess_play_screen.dart';
import 'package:litch_mobile/src/screens/game_library_screen.dart';
import 'package:litch_mobile/src/screens/post_game_review_screen.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_bundle.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';
import 'package:litch_mobile/src/services/post_game_review_analyzer.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/game_position_thumbnail.dart';
import 'package:litch_mobile/src/widgets/recent_games_section.dart';
import 'package:litch_mobile/src/widgets/post_game_review_analyzer_scope.dart';

import 'health_render_capture.dart';
import 'post_game_review_test_fixture.dart';

Widget _harness(
  GameLibraryRepository repository, {
  TextScaler textScaler = TextScaler.noScaling,
  LocalPostGameReviewAnalyzer? analyzer,
}) {
  Widget result = RepaintBoundary(
    key: const ValueKey<String>('game-library-review-boundary'),
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildLitchTheme(),
      localizationsDelegates: AppLocalizations.localizationsDelegates,
      supportedLocales: AppLocalizations.supportedLocales,
      builder: (BuildContext context, Widget? child) => MediaQuery(
        data: MediaQuery.of(context).copyWith(textScaler: textScaler),
        child: child!,
      ),
      initialRoute: '/games',
      routes: <String, WidgetBuilder>{
        '/': (_) => const Scaffold(body: SizedBox.expand()),
        '/games': (_) => GameLibraryScreen(repository: repository),
      },
    ),
  );
  if (analyzer != null) {
    result = PostGameReviewJobOwnerScope(
      owner: PostGameReviewJobOwner(analyzer: analyzer),
      child: result,
    );
  }
  return result;
}

Widget _directHarness(
  GameLibraryRepository repository, {
  TextScaler textScaler = TextScaler.noScaling,
  Locale? locale,
}) => RepaintBoundary(
  key: const ValueKey<String>('game-library-direct-review-boundary'),
  child: MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: buildLitchTheme(),
    localizationsDelegates: AppLocalizations.localizationsDelegates,
    supportedLocales: AppLocalizations.supportedLocales,
    locale: locale,
    builder: (BuildContext context, Widget? child) => MediaQuery(
      data: MediaQuery.of(context).copyWith(textScaler: textScaler),
      child: child!,
    ),
    home: GameLibraryScreen(repository: repository),
  ),
);

Widget _recentHarness(GameLibraryRepository repository) => MaterialApp(
  theme: buildLitchTheme(),
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: Scaffold(
    body: RecentGamesSection(repository: repository, onOpenAll: () {}),
  ),
);

Widget _recentToGamesHarness(GameLibraryRepository repository) => MaterialApp(
  theme: buildLitchTheme(),
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  home: Builder(
    builder: (BuildContext context) => Scaffold(
      body: RecentGamesSection(
        repository: repository,
        onOpenAll: () => Navigator.of(context).push<void>(
          MaterialPageRoute<void>(
            builder: (_) => GameLibraryScreen(repository: repository),
          ),
        ),
      ),
    ),
  ),
);

Future<void> _finishLibraryLoad(WidgetTester tester) async {
  await tester.runAsync(
    () => Future<void>.delayed(const Duration(milliseconds: 20)),
  );
  await tester.pump();
}

Future<
  ({
    GameLibraryCatalog firstCatalog,
    GameLibraryWindow firstWindow,
    GameLibraryCatalog recoveredCatalog,
    GameLibraryWindow recoveredWindow,
  })
>
_persistentRecoverySequence(
  WidgetTester tester,
  Directory directory,
  GameArchive archive,
) async {
  return (await tester.runAsync(() async {
    final GameLibraryStore store = GameLibraryStore(directory);
    await store.save(archive);
    final GameLibraryRepository indexed = GameLibraryRepository(store);
    await indexed.loadCatalog();
    await indexed.close();
    await store.save(archive.rename('Recovered game'));
    await File(
      '${directory.path}/game-${archive.libraryId}.json',
    ).writeAsString('{broken', flush: true);

    final GameLibraryRepository repository = GameLibraryRepository(store);
    final GameLibraryCatalog firstCatalog = await repository.loadCatalog();
    final GameLibraryWindow firstWindow = await repository.loadWindow(
      offset: 0,
      limit: 49,
    );
    final GameLibraryCatalog recoveredCatalog = await repository.loadCatalog();
    final GameLibraryWindow recoveredWindow = await repository.loadWindow(
      offset: 0,
      limit: 49,
    );
    await repository.close();
    return (
      firstCatalog: firstCatalog,
      firstWindow: firstWindow,
      recoveredCatalog: recoveredCatalog,
      recoveredWindow: recoveredWindow,
    );
  }))!;
}

Future<({GameLibraryCatalog catalog, GameLibraryWindow window})>
_blockedIndexProjection(
  WidgetTester tester,
  Directory directory,
  GameArchive archive,
) async {
  return (await tester.runAsync(() async {
    final GameLibraryStore store = GameLibraryStore(directory);
    await store.save(archive);
    await File(
      '${directory.path}/.game-library-index-v1/catalog.json',
    ).writeAsString('{broken', flush: true);
    final Directory blockedTemporary = Directory(
      '${directory.path}/.game-library-index-v1/catalog.json.tmp',
    );
    await blockedTemporary.create();
    final GameLibraryRepository repository = GameLibraryRepository(store);
    final GameLibraryCatalog catalog = await repository.loadCatalog();
    final GameLibraryWindow window = await repository.loadWindow(
      offset: 0,
      limit: 49,
    );
    await repository.close();
    await blockedTemporary.delete();
    return (catalog: catalog, window: window);
  }))!;
}

GameArchive _updated(GameArchive archive, {required int revision}) {
  return GameArchive(
    libraryId: archive.libraryId,
    boardGameId: archive.boardGameId,
    createdAt: archive.createdAt,
    updatedAt: archive.updatedAt.add(Duration(minutes: revision)),
    status: archive.status,
    captureQuality: archive.captureQuality,
    variant: archive.variant,
    outcome: archive.outcome,
    winner: archive.winner,
    lineRevision: revision,
    positions: archive.positions,
    transitions: archive.transitions,
    whitePlayer: archive.whitePlayer,
    blackPlayer: archive.blackPlayer,
    displayName: archive.displayName,
    provenance: archive.provenance,
    provider: archive.provider,
  );
}

GameArchive _withCaptureQuality(
  GameArchive archive,
  GameCaptureQuality quality,
) {
  return GameArchive(
    libraryId: archive.libraryId,
    boardGameId: archive.boardGameId,
    createdAt: archive.createdAt,
    updatedAt: archive.updatedAt,
    status: archive.status,
    captureQuality: quality,
    variant: archive.variant,
    outcome: archive.outcome,
    winner: archive.winner,
    lineRevision: archive.lineRevision,
    positions: archive.positions,
    transitions: archive.transitions,
    whitePlayer: archive.whitePlayer,
    blackPlayer: archive.blackPlayer,
    displayName: archive.displayName,
    provenance: archive.provenance,
    provider: archive.provider,
  );
}

GameArchive _scaledArchive(int index) {
  final GameArchive source = completedClassicArchive();
  final DateTime timestamp = source.updatedAt.add(Duration(minutes: index));
  final String suffix = index.toString().padLeft(4, '0');
  return GameArchive(
    libraryId: 'scale-$suffix',
    boardGameId: 'scale-board-$suffix',
    createdAt: source.createdAt,
    updatedAt: timestamp,
    status: source.status,
    captureQuality: source.captureQuality,
    variant: source.variant,
    outcome: source.outcome,
    winner: source.winner,
    lineRevision: source.lineRevision,
    positions: source.positions,
    transitions: source.transitions,
    whitePlayer: source.whitePlayer,
    blackPlayer: source.blackPlayer,
  );
}

GameArchive _providerArchive(
  String rawStatus,
  GameOutcome outcome, {
  String? providerWinner,
}) {
  final GameArchive source = completedClassicArchive();
  final bool ongoing = outcome == GameOutcome.ongoing;
  return GameArchive(
    libraryId: 'provider-${rawStatus.toLowerCase()}',
    boardGameId: 'provider-board-$rawStatus',
    createdAt: source.createdAt,
    updatedAt: source.updatedAt,
    status: ongoing ? GameArchiveStatus.active : GameArchiveStatus.completed,
    captureQuality: source.captureQuality,
    variant: GameVariant.classic,
    outcome: outcome,
    winner: outcome == GameOutcome.draw
        ? GameWinner.draw
        : providerWinner == 'white'
        ? GameWinner.white
        : providerWinner == 'black'
        ? GameWinner.black
        : GameWinner.none,
    lineRevision: source.lineRevision,
    positions: source.positions,
    transitions: source.transitions,
    whitePlayer: source.whitePlayer,
    blackPlayer: source.blackPlayer,
    provider: ProviderGameProvenance(
      provider: 'lichess',
      gameId: 'game$rawStatus',
      accountId: 'player',
      localSide: 'white',
      rawStatus: rawStatus,
      winner: providerWinner,
    ),
  );
}

void main() {
  late GameLibraryRepository repository;

  setUp(() {
    repository = _MemoryRepository();
  });

  tearDown(() async {
    await repository.close();
  });

  testWidgets('shows a truthful empty library', (WidgetTester tester) async {
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    expect(find.text('No saved games yet'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('game-library-list')),
      findsNothing,
    );
  });

  testWidgets('renders 1, 5, 100, and 1000 games with a lazy list', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();

    for (final int count in <int>[1, 5, 100, 1000]) {
      final _MemoryRepository scaleRepository = _MemoryRepository(
        snapshot: GameLibrarySnapshot(
          games: List<GameArchive>.generate(count, _scaledArchive),
        ),
      );
      await tester.pumpWidget(_harness(scaleRepository));
      await _finishLibraryLoad(tester);

      expect(
        find.text(count == 1 ? '1 saved game' : '$count saved games'),
        findsOneWidget,
      );
      final int builtRows = find
          .byWidgetPredicate(
            (Widget widget) =>
                widget.key is ValueKey<String> &&
                ((widget.key! as ValueKey<String>).value).startsWith(
                  'game-library-game-',
                ),
          )
          .evaluate()
          .length;
      expect(builtRows, lessThan(32), reason: '$count games');
      if (count == 1000) {
        final Finder list = find.byKey(
          const ValueKey<String>('game-library-list'),
        );
        final Finder scrollable = find
            .descendant(of: list, matching: find.byType(Scrollable))
            .first;
        await tester.fling(list, const Offset(0, -8000), 10000);
        await tester.pumpAndSettle();
        expect(
          tester.state<ScrollableState>(scrollable).position.pixels,
          greaterThan(3000),
        );
        expect(
          find.byKey(const ValueKey<String>('game-library-game-scale-0999')),
          findsNothing,
        );
        final int rowsAfterFling = find
            .byWidgetPredicate(
              (Widget widget) =>
                  widget.key is ValueKey<String> &&
                  ((widget.key! as ValueKey<String>).value).startsWith(
                    'game-library-game-',
                  ),
            )
            .evaluate()
            .length;
        expect(rowsAfterFling, greaterThan(0));
        expect(rowsAfterFling, lessThan(48));
        expect(
          scaleRepository.windowRequests.any((request) => request.offset > 0),
          isTrue,
        );
        expect(
          scaleRepository.windowRequests.every(
            (request) => request.limit <= 49,
          ),
          isTrue,
        );
        expect(tester.takeException(), isNull);
      }
      await captureHealthReview(tester, 'game-library-$count-games-390');

      await tester.pumpWidget(const SizedBox());
      await scaleRepository.close();
    }

    repository = _MemoryRepository();
  });

  testWidgets('held page IO cannot block replacement or disposal', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();
    final _HeldPageRepository held = _HeldPageRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(1000, _scaledArchive),
      ),
    );
    repository = held;
    await tester.pumpWidget(
      _directHarness(held, textScaler: const TextScaler.linear(2)),
    );
    await _finishLibraryLoad(tester);

    final ScrollableState scrollable = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byKey(const ValueKey<String>('game-library-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    final double unnamedExtent = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
        .height;
    final double listStart = tester
        .renderObject<RenderSliverVariedExtentList>(
          find.byType(SliverVariedExtentList),
        )
        .constraints
        .precedingScrollExtent;
    scrollable.position.jumpTo(listStart + 49 * unnamedExtent);
    await tester.pump();
    expect(held.heldCalls, 1);
    final Finder loadingRows = find.byKey(
      const ValueKey<String>('game-library-placeholder-49'),
    );
    expect(loadingRows, findsOneWidget);
    expect(tester.getSize(loadingRows).height, unnamedExtent);
    expect(
      unnamedExtent,
      lessThan(500),
      reason: 'A scaled unnamed card must not waste a full phone viewport.',
    );
    expect(find.bySemanticsLabel('Loading saved games'), findsWidgets);
    await captureHealthReview(tester, 'game-library-held-loading-320-200');

    final _MemoryRepository replacement = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(100, _scaledArchive),
      ),
    );
    await tester.pumpWidget(
      _directHarness(replacement, textScaler: const TextScaler.linear(2)),
    );
    await _finishLibraryLoad(tester);
    expect(find.text('100 saved games'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('game-library-game-scale-0099')),
      findsOneWidget,
    );
    await captureHealthReview(tester, 'game-library-loaded-unnamed-320-200');

    final ScrollableState replacementScrollable = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byKey(const ValueKey<String>('game-library-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    final double replacementExtent = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
        .height;
    expect(replacementExtent, unnamedExtent);
    replacementScrollable.position.jumpTo(listStart + 49 * replacementExtent);
    await tester.pump();
    await _finishLibraryLoad(tester);
    expect(
      replacement.windowRequests.any((request) => request.offset > 0),
      isTrue,
    );
    expect(
      find.byKey(const ValueKey<String>('game-library-game-scale-0050')),
      findsOneWidget,
    );

    held.release.complete(
      GameLibraryWindow(offset: 40, totalCount: 1000, games: const []),
    );
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('game-library-game-scale-0050')),
      findsOneWidget,
    );
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    expect(tester.takeException(), isNull);
    await replacement.close();
    repository = _MemoryRepository();
  });

  testWidgets('named loading row keeps its 200 percent loaded geometry', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();
    final List<GameArchive> games = List<GameArchive>.generate(
      100,
      _scaledArchive,
    );
    games[50] = games[50].rename('A named saved game');
    final _HeldPageRepository held = _HeldPageRepository(
      snapshot: GameLibrarySnapshot(games: games),
    );
    repository = held;
    await tester.pumpWidget(
      _directHarness(held, textScaler: const TextScaler.linear(2)),
    );
    await _finishLibraryLoad(tester);

    final ScrollableState scrollable = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byKey(const ValueKey<String>('game-library-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    final double unnamedExtent = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
        .height;
    final double listStart = tester
        .renderObject<RenderSliverVariedExtentList>(
          find.byType(SliverVariedExtentList),
        )
        .constraints
        .precedingScrollExtent;
    scrollable.position.jumpTo(listStart + 49 * unnamedExtent);
    await tester.pump();
    final Finder placeholder = find.byKey(
      const ValueKey<String>('game-library-placeholder-49'),
    );
    expect(placeholder, findsOneWidget);
    final double placeholderExtent = tester.getSize(placeholder).height;
    expect(placeholderExtent, greaterThan(unnamedExtent));
    expect(
      placeholderExtent,
      lessThan(640),
      reason:
          'The named card must stay shorter than the complete compact phone screen.',
    );
    expect(find.bySemanticsLabel('Loading saved games'), findsWidgets);

    held.release.complete(await held.window(offset: 40, limit: 49));
    await tester.pump();
    await _finishLibraryLoad(tester);
    final Finder loadedRow = find.byKey(
      const ValueKey<String>('game-library-row-49'),
    );
    expect(loadedRow, findsOneWidget);
    expect(
      tester.getSize(loadedRow).height,
      placeholderExtent,
      reason: 'Loading archive details must not move the reading position.',
    );
    expect(find.text('A named saved game'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'game-library-loaded-named-320-200');

    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await held.close();
    repository = _MemoryRepository();
  });

  testWidgets('normal and intermediate text rows keep bounded geometry', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await repository.close();
    final List<GameArchive> games = List<GameArchive>.generate(
      100,
      _scaledArchive,
    );
    games[99] = games[99].rename('A named saved game');
    final _HeldPageRepository held = _HeldPageRepository(
      snapshot: GameLibrarySnapshot(games: games),
    );
    repository = held;
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_directHarness(held));
    await _finishLibraryLoad(tester);
    final ScrollableState scrollable = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byKey(const ValueKey<String>('game-library-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    final double namedExtent = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
        .height;
    final double unnamedExtent = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-1')))
        .height;
    final double listStart = tester
        .renderObject<RenderSliverVariedExtentList>(
          find.byType(SliverVariedExtentList),
        )
        .constraints
        .precedingScrollExtent;
    scrollable.position.jumpTo(listStart + namedExtent + 48 * unnamedExtent);
    await tester.pump();
    final Finder placeholder = find.byKey(
      const ValueKey<String>('game-library-placeholder-49'),
    );
    expect(placeholder, findsOneWidget);
    expect(tester.getSize(placeholder).height, unnamedExtent);
    expect(unnamedExtent, inInclusiveRange(112, 160));
    expect(namedExtent, inInclusiveRange(unnamedExtent, 180));
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'game-library-held-loading-390-100');

    final _MemoryRepository loaded = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[
          _scaledArchive(0),
          _scaledArchive(1).rename('A named saved game'),
        ],
      ),
    );
    await tester.pumpWidget(
      _directHarness(loaded, textScaler: const TextScaler.linear(1.4)),
    );
    await _finishLibraryLoad(tester);
    final double intermediateNamed = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
        .height;
    final double intermediateUnnamed = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-1')))
        .height;
    expect(intermediateNamed, greaterThan(intermediateUnnamed));
    expect(intermediateNamed, inInclusiveRange(namedExtent, 220));
    expect(intermediateUnnamed, inInclusiveRange(unnamedExtent, 200));
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'game-library-loaded-390-140');

    await tester.binding.setSurfaceSize(const Size(320, 640));
    await tester.pumpWidget(_directHarness(loaded));
    await _finishLibraryLoad(tester);
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'game-library-loaded-320-100');

    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await loaded.close();
    await held.close();
    repository = _MemoryRepository();
  });

  testWidgets('German named provider card fits at 320px and 200 percent', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();
    repository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[
          _providerArchive(
            'mate',
            GameOutcome.checkmate,
            providerWinner: 'white',
          ).rename('Eine benannte gespeicherte Partie'),
        ],
      ),
    );
    await tester.pumpWidget(
      _directHarness(
        repository,
        textScaler: const TextScaler.linear(2),
        locale: const Locale('de'),
      ),
    );
    await _finishLibraryLoad(tester);

    final Finder row = find.byKey(const ValueKey<String>('game-library-row-0'));
    expect(row, findsOneWidget);
    expect(
      tester.getSize(row).height,
      inExclusiveRange(180, 640),
      reason:
          'Full scaled content can grow without reserving more than a phone screen per card.',
    );
    final Finder quality = find.text('Vollständige Wiedergabe');
    expect(quality, findsOneWidget);
    expect(
      tester.getRect(quality).bottom,
      lessThanOrEqualTo(tester.getRect(row).bottom - 8),
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(
      tester,
      'game-library-loaded-named-provider-de-320-200',
    );
  });

  testWidgets('German named provider card keeps compact phone geometry', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await repository.close();
    repository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[
          _providerArchive(
            'mate',
            GameOutcome.checkmate,
            providerWinner: 'white',
          ).rename('Eine benannte gespeicherte Partie'),
        ],
      ),
    );
    addTearDown(() => tester.binding.setSurfaceSize(null));
    double? normalPhoneExtent;

    for (final Size size in <Size>[
      const Size(320, 640),
      const Size(390, 844),
    ]) {
      await tester.binding.setSurfaceSize(size);
      await tester.pumpWidget(
        _directHarness(repository, locale: const Locale('de')),
      );
      await _finishLibraryLoad(tester);

      final Finder row = find.byKey(
        const ValueKey<String>('game-library-row-0'),
      );
      expect(row, findsOneWidget);
      final double extent = tester.getSize(row).height;
      expect(
        extent,
        inInclusiveRange(112, 180),
        reason: 'Default text keeps a compact scan row.',
      );
      if (size.width == 390) normalPhoneExtent = extent;
      expect(find.text('Mi., 2. Sept.'), findsOneWidget);
      expect(find.text('Klassisch · 3 Züge'), findsOneWidget);
      expect(find.textContaining('Gespeichert am'), findsNothing);
      expect(find.textContaining('Vollständige Wiedergabe'), findsNothing);
      expect(
        find.bySemanticsLabel(RegExp(r'Schachmatt.*hat gewonnen')),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await captureHealthReview(
        tester,
        'game-library-loaded-named-provider-de-${size.width.toInt()}-100',
      );
    }

    await tester.binding.setSurfaceSize(const Size(390, 844));
    for (final double scale in <double>[1.1, 1.2]) {
      await tester.pumpWidget(
        _directHarness(
          repository,
          textScaler: TextScaler.linear(scale),
          locale: const Locale('de'),
        ),
      );
      await _finishLibraryLoad(tester);
      final Finder row = find.byKey(
        const ValueKey<String>('game-library-row-0'),
      );
      expect(
        tester.getSize(row).height,
        inInclusiveRange(normalPhoneExtent!, normalPhoneExtent * scale + 8),
        reason:
            'A small font adjustment must not trigger a fixed-height layout jump.',
      );
      expect(
        find.bySemanticsLabel(RegExp(r'Schachmatt.*hat gewonnen')),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
      await captureHealthReview(
        tester,
        'game-library-loaded-named-provider-de-390-'
        '${(scale * 100).round()}',
      );
    }
  });

  testWidgets('English named provider card keeps compact scan hierarchy', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await repository.close();
    repository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[
          _providerArchive(
            'mate',
            GameOutcome.checkmate,
            providerWinner: 'white',
          ).rename('A named saved game'),
        ],
      ),
    );
    addTearDown(() => tester.binding.setSurfaceSize(null));

    for (final Size size in <Size>[
      const Size(320, 640),
      const Size(390, 844),
    ]) {
      await tester.binding.setSurfaceSize(size);
      await tester.pumpWidget(
        _directHarness(repository, locale: const Locale('en')),
      );
      await _finishLibraryLoad(tester);

      final Finder row = find.byKey(
        const ValueKey<String>('game-library-row-0'),
      );
      expect(
        tester.getSize(row).height,
        inInclusiveRange(112, 180),
        reason: 'Default text keeps a compact scan row.',
      );
      expect(find.text('Wed, Sep 2'), findsOneWidget);
      expect(find.text('Classic · 3 moves'), findsOneWidget);
      expect(find.textContaining('Saved'), findsNothing);
      expect(find.textContaining('Complete replay'), findsNothing);
      expect(tester.takeException(), isNull);
      await captureHealthReview(
        tester,
        'game-library-loaded-named-provider-en-${size.width.toInt()}-100',
      );
    }
  });

  testWidgets('compact partial replay remains visibly distinguishable', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();
    repository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[
          _withCaptureQuality(
            _providerArchive(
              'started',
              GameOutcome.ongoing,
            ).rename('A named saved game'),
            GameCaptureQuality.partial,
          ),
        ],
      ),
    );
    await tester.pumpWidget(_directHarness(repository));
    await _finishLibraryLoad(tester);

    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
          .height,
      inInclusiveRange(112, 180),
    );
    expect(find.text('Classic · 3 moves'), findsOneWidget);
    expect(find.text('Partial replay'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('German unnamed provider card fits at 320px and 200 percent', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();
    repository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[
          _providerArchive(
            'mate',
            GameOutcome.checkmate,
            providerWinner: 'white',
          ),
        ],
      ),
    );
    await tester.pumpWidget(
      _directHarness(
        repository,
        textScaler: const TextScaler.linear(2),
        locale: const Locale('de'),
      ),
    );
    await _finishLibraryLoad(tester);

    final Finder row = find.byKey(const ValueKey<String>('game-library-row-0'));
    expect(row, findsOneWidget);
    expect(
      tester.getSize(row).height,
      inExclusiveRange(180, 500),
      reason:
          'An unnamed scaled card must remain smaller than a full compact phone viewport.',
    );
    final Finder quality = find.text('Vollständige Wiedergabe');
    expect(quality, findsOneWidget);
    expect(
      tester.getRect(quality).bottom,
      lessThanOrEqualTo(tester.getRect(row).bottom - 8),
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(
      tester,
      'game-library-loaded-unnamed-provider-de-320-200',
    );
  });

  testWidgets('late Games recovery is visible after returning to Recent', (
    WidgetTester tester,
  ) async {
    await repository.close();
    final _LateRecoveryRepository lateRecovery = _LateRecoveryRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(100, _scaledArchive),
      ),
    );
    repository = lateRecovery;
    await tester.pumpWidget(_recentToGamesHarness(repository));
    await _finishLibraryLoad(tester);

    await tester.tap(find.byKey(const ValueKey<String>('game-library-action')));
    await tester.pumpAndSettle();
    await _finishLibraryLoad(tester);
    final ScrollableState scrollable = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byKey(const ValueKey<String>('game-library-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    scrollable.position.jumpTo(7000);
    await tester.pump();
    await _finishLibraryLoad(tester);
    await tester.pumpAndSettle();

    expect(lateRecovery.discovered, isTrue);
    scrollable.position.jumpTo(0);
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('game-library-recovery-warning')),
      findsOneWidget,
    );
    Navigator.of(tester.element(find.byType(GameLibraryScreen))).pop();
    await tester.pumpAndSettle();

    expect(
      find.byKey(const ValueKey<String>('recent-games-recovery-warning')),
      findsOneWidget,
    );
  });

  testWidgets('deep replay returns to the top at 200 percent text', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.close();
    final _MemoryRepository deepRepository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: List<GameArchive>.generate(200, _scaledArchive),
      ),
    );
    repository = deepRepository;
    await tester.pumpWidget(
      _harness(repository, textScaler: const TextScaler.linear(2)),
    );
    await _finishLibraryLoad(tester);
    final ScrollableState scrollable = tester.state<ScrollableState>(
      find
          .descendant(
            of: find.byKey(const ValueKey<String>('game-library-list')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    final double rowExtent = tester
        .getSize(find.byKey(const ValueKey<String>('game-library-row-0')))
        .height;
    final double listStart = tester
        .renderObject<RenderSliverVariedExtentList>(
          find.byType(SliverVariedExtentList),
        )
        .constraints
        .precedingScrollExtent;
    scrollable.position.jumpTo(listStart + 80 * rowExtent);
    await tester.pump();
    await _finishLibraryLoad(tester);
    final Finder selected = find.byKey(
      const ValueKey<String>('game-library-game-scale-0119'),
    );
    expect(selected, findsOneWidget);
    await tester.ensureVisible(selected);
    await tester.pump();
    final double pixelsBefore = scrollable.position.pixels;
    final Finder selectedRow = find.byKey(
      const ValueKey<String>('game-library-row-80'),
    );
    expect(tester.getSize(selectedRow).height, rowExtent);
    expect(
      tester.getSize(selected).height,
      inInclusiveRange(48, rowExtent - 8),
    );

    await tester.tap(selected);
    await tester.pumpAndSettle();
    await deepRepository.rename('scale-0119', 'Deep anchor');
    await tester.pump();
    Navigator.of(tester.element(find.byType(PostGameReviewScreen))).pop();
    await tester.pump();
    await _finishLibraryLoad(tester);

    expect(pixelsBefore, greaterThan(0));
    expect(scrollable.position.pixels, 0);
    expect(
      find.byKey(const ValueKey<String>('game-library-game-scale-0199')),
      findsOneWidget,
    );
    expect(
      deepRepository.snapshot.games
          .singleWhere((game) => game.libraryId == 'scale-0119')
          .displayName,
      'Deep anchor',
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('opens the production offline position draft', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-fen-preview')),
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('fen-preview-field')),
      findsOneWidget,
    );

    const draft = '4k3/8/8/8/8/8/8/4K3 w - - 0 1';
    await tester.enterText(
      find.byKey(const ValueKey<String>('fen-preview-field')),
      draft,
    );
    expect(
      tester
          .widget<TextField>(
            find.byKey(const ValueKey<String>('fen-preview-field')),
          )
          .controller!
          .text,
      draft,
    );
    expect(
      find.textContaining(RegExp(r'loaded|synced', caseSensitive: false)),
      findsNothing,
    );
  });

  testWidgets('adds saved games from the live repository stream', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpAndSettle();

    expect(
      find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
      findsOneWidget,
    );
    expect(find.text('P1 (W) won'), findsOneWidget);
    expect(find.text('Classic'), findsOneWidget);
    expect(find.text('Complete replay'), findsOneWidget);
    expect(find.text('3 moves'), findsOneWidget);
    expect(
      find.byKey(ValueKey<String>('game-thumbnail-${archive.libraryId}')),
      findsOneWidget,
    );
  });

  testWidgets('names and renames a saved game from its visible actions', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    expect(
      find.bySemanticsLabel(RegExp(r'Open game review\.$')),
      findsOneWidget,
    );
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    expect(find.text('Name game'), findsOneWidget);
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    await captureHealthReview(tester, 'game-library-name-dialog-390');
    await tester.enterText(
      find.byKey(const ValueKey<String>('game-library-name-field')),
      '  Friday   rematch  ',
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pumpAndSettle();

    expect(find.text('Friday rematch'), findsOneWidget);
    expect(find.text('P1 (W) won'), findsOneWidget);
    expect(repository.snapshot.games.single.displayName, 'Friday rematch');
    await captureHealthReview(tester, 'game-library-named-390');
    expect(
      tester
          .getSize(
            find.byKey(
              ValueKey<String>('game-library-menu-${archive.libraryId}'),
            ),
          )
          .height,
      greaterThanOrEqualTo(48),
    );
  });

  testWidgets('saved game menu deletes with confirmation and immediate undo', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    expect(find.text('Delete game'), findsOneWidget);
    await tester.tap(find.text('Delete game'));
    await tester.pumpAndSettle();
    expect(find.text('Delete this saved game?'), findsOneWidget);
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-delete-confirm')),
    );
    await tester.pumpAndSettle();

    expect(repository.snapshot.games, isEmpty);
    expect(find.text('Game deleted'), findsOneWidget);
    expect(find.text('No saved games yet'), findsOneWidget);
    await tester.tap(find.text('Undo'));
    await tester.pumpAndSettle();
    expect(repository.snapshot.games.single.libraryId, archive.libraryId);
    expect(
      find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
      findsOneWidget,
    );
  });

  testWidgets('an already removed game offers no stale Undo', (tester) async {
    final archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Delete game'));
    await tester.pumpAndSettle();

    await repository.delete(archive.libraryId);
    await tester.tap(find.byKey(const ValueKey('game-library-delete-confirm')));
    await tester.pumpAndSettle();

    expect(find.text('Game deleted'), findsOneWidget);
    expect(find.text('Undo'), findsNothing);
    expect(repository.snapshot.games, isEmpty);
  });

  for (final unreadableDelete in [false, true]) {
    testWidgets(
      unreadableDelete
          ? 'Delete preserves a record that becomes unreadable during confirmation'
          : 'Undo keeps a visible retry after marker cleanup fails',
      (tester) async {
        await repository.close();
        final directory = (await tester.runAsync(
          () => Directory.systemTemp.createTemp('litch-delete-recovery-'),
        ))!;
        addTearDown(() async {
          await tester.runAsync(() async {
            await repository.close();
            await directory.delete(recursive: true);
          });
          repository = _MemoryRepository();
        });
        final store = GameLibraryStore(directory);
        repository = GameLibraryRepository(store);
        final archive = completedClassicArchive();
        await tester.runAsync(() async {
          await repository.save(archive);
          await tester.pumpWidget(_harness(repository));
          await repository.loadCatalog();
        });
        await _finishLibraryLoad(tester);
        await tester.runAsync(
          () => tester.tap(
            find.byKey(ValueKey('game-library-menu-${archive.libraryId}')),
          ),
        );
        await tester.pumpAndSettle();
        await tester.runAsync(() => tester.tap(find.text('Delete game')));
        await tester.pumpAndSettle();
        final current = File(
          '${directory.path}/game-${archive.libraryId}.json',
        );
        if (unreadableDelete) {
          await tester.runAsync(
            () => current.writeAsString('{unreadable-current'),
          );
        }
        await tester.runAsync(
          () => tester.tap(
            find.byKey(const ValueKey('game-library-delete-confirm')),
          ),
        );
        await _finishLibraryLoad(tester);
        await tester.pumpAndSettle();

        if (unreadableDelete) {
          expect(
            find.text('Couldn’t delete the game. Try again.'),
            findsOneWidget,
          );
          expect(find.text('Game deleted'), findsNothing);
          expect(
            await tester.runAsync(current.readAsString),
            '{unreadable-current',
          );
          expect(
            await tester.runAsync(
              () => store.isBoardGameCaptureSuppressed(archive.gameId),
            ),
            isFalse,
          );
        } else {
          expect(find.text('Game deleted'), findsOneWidget);
          final marker = (await tester.runAsync(() async {
            final files = await Directory(
              '${directory.path}/.board-game-deletions-v1',
            ).list().toList();
            final marker = files.single;
            await marker.delete();
            await Directory(marker.path).create();
            await File('${marker.path}/blocked').writeAsString('retain');
            return marker;
          }))!;
          await tester.runAsync(() => tester.tap(find.text('Undo')));
          await _finishLibraryLoad(tester);
          await tester.pumpAndSettle();
          expect(find.text('Undo'), findsOneWidget);
          expect(
            find.text('Couldn’t restore the game. Try again.'),
            findsOneWidget,
          );
          await tester.runAsync(() async {
            await Directory(marker.path).delete(recursive: true);
            await File(marker.path).writeAsString('${archive.gameId}\n');
          });

          await tester.runAsync(() => tester.tap(find.text('Undo')));
          await _finishLibraryLoad(tester);
          await tester.pumpAndSettle();

          expect(
            find.byKey(ValueKey('game-library-game-${archive.libraryId}')),
            findsOneWidget,
          );
          expect(
            (await tester.runAsync(repository.loadCatalog))!.totalCount,
            1,
          );
          expect(
            (await tester.runAsync(store.load))!.games.single.toJson(),
            archive.toJson(),
          );
          expect(
            await tester.runAsync(
              () => store.isBoardGameCaptureSuppressed(archive.gameId),
            ),
            isFalse,
          );
        }
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
      },
    );
  }

  for (final updateAfterDelete in [false, true]) {
    testWidgets(
      'delete Undo preserves history ${updateAfterDelete ? 'imported after deletion' : 'received during confirmation'}',
      (tester) async {
        await repository.close();
        final directory = (await tester.runAsync(
          () => Directory.systemTemp.createTemp('litch-delete-undo-'),
        ))!;
        addTearDown(() async {
          await tester.runAsync(() async {
            await repository.close();
            if (await directory.exists()) {
              await directory.delete(recursive: true);
            }
          });
          repository = _MemoryRepository();
        });
        final store = GameLibraryStore(directory);
        repository = GameLibraryRepository(store);
        final fixture = completedClassicArchive();
        final observedAt = fixture.createdAt;
        AuthoritativeLineSegment segment({required bool completed}) =>
            AuthoritativeLineSegment(
              boardGameId: '00112233445566778899aabbccddeeff',
              lineRevision: 1,
              status: completed
                  ? GameArchiveStatus.completed
                  : GameArchiveStatus.active,
              variant: fixture.variant,
              outcome: completed ? fixture.outcome : GameOutcome.ongoing,
              winner: completed ? fixture.winner : GameWinner.none,
              positions: completed
                  ? fixture.positions
                  : fixture.positions.take(2),
              transitions: completed
                  ? fixture.transitions
                  : fixture.transitions.take(1),
            );
        final original = (await tester.runAsync(
          () => repository.mergeAuthoritativeSegment(
            segment(completed: false),
            observedAt: observedAt,
          ),
        ))!;
        await tester.runAsync(() async {
          await tester.pumpWidget(_harness(repository));
          await repository.loadCatalog();
        });
        await _finishLibraryLoad(tester);
        await tester.runAsync(
          () => tester.tap(
            find.byKey(ValueKey('game-library-menu-${original.libraryId}')),
          ),
        );
        await tester.pumpAndSettle();
        await tester.runAsync(() => tester.tap(find.text('Delete game')));
        await tester.pumpAndSettle();

        Future<GameArchive?>
        receiveFinishedLine() => tester.runAsync<GameArchive?>(() async {
          final completed = segment(completed: true);
          final updatedAt = observedAt.add(const Duration(minutes: 1));
          if (!updateAfterDelete) {
            return repository.mergeAuthoritativeSegment(
              completed,
              observedAt: updatedAt,
            );
          }
          // Explicit import can restore a deleted game; automatic sync cannot.
          final imported = original.mergeAuthoritativeSegment(
            completed,
            observedAt: updatedAt,
          );
          final bundle = File('${directory.path}/incoming.litchgames');
          await bundle.writeAsString(
            GameLibraryBundle.encode(games: [imported], exportedAt: updatedAt),
          );
          await repository.importFromFile(bundle);
          return imported;
        });
        GameArchive? received;
        if (!updateAfterDelete) received = await receiveFinishedLine();
        await tester.runAsync(() async {
          await tester.tap(
            find.byKey(const ValueKey('game-library-delete-confirm')),
          );
        });
        await _finishLibraryLoad(tester);
        await tester.pumpAndSettle();
        expect(find.text('Game deleted'), findsOneWidget);
        expect((await tester.runAsync(store.load))!.games, isEmpty);
        if (updateAfterDelete) received = await receiveFinishedLine();
        await tester.pumpAndSettle();
        await tester.runAsync(() async => tester.tap(find.text('Undo')));
        await _finishLibraryLoad(tester);
        await tester.pumpAndSettle();

        final restored = (await tester.runAsync(store.load))!.games.single;
        expect(restored.toJson(), received!.toJson());
        expect(restored.currentPly, 3);
        expect(restored.positions.last.fen, classicAfterNf3);
        expect(restored.status, GameArchiveStatus.completed);
        expect(find.text('P1 (W) won'), findsOneWidget);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
      },
    );
  }

  testWidgets(
    'deleted game notices stay retired across repeated review returns',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      for (int index = 0; index < 12; index += 1) {
        await repository.save(_scaledArchive(index));
      }
      await tester.pumpWidget(_harness(repository));
      await _finishLibraryLoad(tester);
      for (final String id in <String>['scale-0011', 'scale-0010']) {
        await tester.tap(find.byKey(ValueKey<String>('game-library-menu-$id')));
        await tester.pumpAndSettle();
        await tester.tap(find.text('Delete game'));
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey<String>('game-library-delete-confirm')),
        );
        await tester.pumpAndSettle();
        expect(find.text('Game deleted'), findsOneWidget);
      }
      expect(repository.snapshot.games.length, 10);
      final List<bool> staleNotices = <bool>[];
      final List<double> returnOffsets = <double>[];
      for (int visit = 0; visit < 3; visit += 1) {
        final ScrollableState list = tester.state<ScrollableState>(
          find
              .descendant(
                of: find.byKey(const ValueKey<String>('game-library-list')),
                matching: find.byType(Scrollable),
              )
              .first,
        );
        list.position.jumpTo(list.position.maxScrollExtent);
        await tester.pump();
        await _finishLibraryLoad(tester);
        final Finder lastGame = find.byKey(
          const ValueKey<String>('game-library-game-scale-0000'),
        );
        await tester.ensureVisible(lastGame);
        await tester.pump();
        await tester.tap(lastGame);
        await tester.pumpAndSettle();
        expect(find.byType(PostGameReviewScreen), findsOneWidget);
        final Finder first = find.byKey(
          const ValueKey<String>('review-first-position'),
        );
        await tester.ensureVisible(first);
        await tester.pump();
        await tester.tap(first);
        await tester.pump();
        final Finder previous = find.byKey(
          const ValueKey<String>('review-previous-position'),
        );
        expect(tester.widget<IconButton>(previous).onPressed, isNull);
        await tester.tap(
          find.byKey(const ValueKey<String>('review-next-position')),
        );
        await tester.pump();
        expect(tester.widget<IconButton>(previous).onPressed, isNotNull);
        await tester.pageBack();
        await tester.pumpAndSettle();
        await _finishLibraryLoad(tester);
        staleNotices.add(find.text('Game deleted').evaluate().isNotEmpty);
        returnOffsets.add(list.position.pixels);
      }
      expect(
        staleNotices,
        <bool>[false, false, false],
        reason:
            'Delete feedback must not replay after a review round trip. '
            'Return offsets: $returnOffsets',
      );
      expect(returnOffsets, <double>[0, 0, 0]);
      expect(repository.snapshot.games.length, 10);
      expect(tester.takeException(), isNull);
    },
  );

  for (final String entry in <String>[
    'Games',
    'Recent review',
    'Recent all Games',
  ]) {
    for (final bool prior in <bool>[true, false]) {
      testWidgets(
        '$entry ${prior ? 'replaces older queued notices on delete' : 'preserves later queued notice on navigation'}',
        (WidgetTester tester) async {
          await repository.save(_scaledArchive(0));
          await repository.save(_scaledArchive(1));
          final bool recent = entry != 'Games';
          await tester.pumpWidget(
            recent ? _recentToGamesHarness(repository) : _harness(repository),
          );
          await _finishLibraryLoad(tester);
          final BuildContext origin = tester.element(
            recent
                ? find.byType(RecentGamesSection)
                : find.byType(GameLibraryScreen),
          );
          final ScaffoldMessengerState messenger = ScaffoldMessenger.of(origin);
          if (prior) {
            messenger.showSnackBar(
              const SnackBar(
                content: Text('Older visible notice'),
                persist: true,
              ),
            );
            messenger.showSnackBar(
              const SnackBar(
                content: Text('Older queued notice'),
                persist: true,
              ),
            );
            await tester.pumpAndSettle();
            expect(find.text('Older visible notice'), findsOneWidget);
            expect(find.text('Older queued notice'), findsNothing);
          }
          await tester.tap(
            find.byKey(
              ValueKey<String>(
                recent
                    ? 'recent-game-menu-scale-0001'
                    : 'game-library-menu-scale-0001',
              ),
            ),
          );
          await tester.pumpAndSettle();
          await tester.tap(find.text('Delete game'));
          await tester.pumpAndSettle();
          await tester.tap(
            find.byKey(const ValueKey<String>('game-library-delete-confirm')),
          );
          await tester.pumpAndSettle();
          expect(find.text('Game deleted'), findsOneWidget);
          expect(find.text('Undo').hitTestable(), findsOneWidget);
          expect(find.text('Older visible notice'), findsNothing);
          expect(find.text('Older queued notice'), findsNothing);
          ScaffoldFeatureController<SnackBar, SnackBarClosedReason>? later;
          if (!prior) {
            later = messenger.showSnackBar(
              const SnackBar(
                content: Text('Later unrelated notice'),
                persist: true,
              ),
            );
            await tester.pumpAndSettle();
            expect(find.text('Later unrelated notice'), findsNothing);
          }
          await tester.tap(
            find.byKey(
              ValueKey<String>(
                entry == 'Recent all Games'
                    ? 'game-library-action'
                    : recent
                    ? 'recent-game-scale-0000'
                    : 'game-library-game-scale-0000',
              ),
            ),
          );
          await tester.pumpAndSettle();
          await _finishLibraryLoad(tester);
          expect(find.text('Game deleted'), findsNothing);
          expect(find.text('Older visible notice'), findsNothing);
          expect(find.text('Older queued notice'), findsNothing);
          if (!prior) {
            expect(find.text('Later unrelated notice'), findsOneWidget);
          }
          await tester.pageBack();
          await tester.pumpAndSettle();
          expect(find.text('Game deleted'), findsNothing);
          expect(find.text('Older visible notice'), findsNothing);
          expect(find.text('Older queued notice'), findsNothing);
          if (!prior) {
            expect(find.text('Later unrelated notice'), findsOneWidget);
            later!.close();
            await tester.pumpAndSettle();
            expect(find.text('Game deleted'), findsNothing);
          }
          expect(repository.snapshot.games.length, 1);
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  for (final bool recent in <bool>[false, true]) {
    testWidgets(
      '${recent ? 'Recent' : 'Games'} held delete cannot replace review feedback',
      (WidgetTester tester) async {
        await repository.close();
        final _HeldDeleteRepository held = _HeldDeleteRepository();
        repository = held;
        await repository.save(_scaledArchive(0));
        await repository.save(_scaledArchive(1));
        await tester.pumpWidget(
          recent ? _recentToGamesHarness(repository) : _harness(repository),
        );
        await _finishLibraryLoad(tester);
        await tester.tap(
          find.byKey(
            ValueKey<String>(
              recent
                  ? 'recent-game-menu-scale-0001'
                  : 'game-library-menu-scale-0001',
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.tap(find.text('Delete game'));
        await tester.pumpAndSettle();
        await tester.tap(
          find.byKey(const ValueKey<String>('game-library-delete-confirm')),
        );
        await tester.pumpAndSettle();
        expect(held.deleteCalls, 1);
        expect(repository.snapshot.games.length, 2);
        await tester.tap(
          find.byKey(
            ValueKey<String>(
              recent
                  ? 'recent-game-scale-0000'
                  : 'game-library-game-scale-0000',
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.byType(PostGameReviewScreen), findsOneWidget);
        ScaffoldMessenger.of(
          tester.element(find.byType(PostGameReviewScreen)),
        ).showSnackBar(
          const SnackBar(content: Text('Current review notice'), persist: true),
        );
        await tester.pumpAndSettle();
        held.release.complete();
        await tester.pumpAndSettle();
        expect(repository.snapshot.games.length, 1);
        expect(find.text('Current review notice'), findsOneWidget);
        expect(find.text('Game deleted'), findsNothing);
        await tester.pageBack();
        await tester.pumpAndSettle();
        expect(find.text('Current review notice'), findsOneWidget);
        expect(find.text('Game deleted'), findsNothing);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets('review navigation preserves an unrelated notice after Undo', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Delete game'));
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-delete-confirm')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Undo'));
    await tester.pumpAndSettle();
    ScaffoldMessenger.of(
      tester.element(find.byType(GameLibraryScreen)),
    ).showSnackBar(
      const SnackBar(content: Text('Unrelated notice'), persist: true),
    );
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    expect(find.text('Unrelated notice'), findsOneWidget);
    await tester.pageBack();
    await tester.pumpAndSettle();
    expect(find.text('Unrelated notice'), findsOneWidget);
    expect(find.text('Game deleted'), findsNothing);
  });

  for (final bool allGames in <bool>[false, true]) {
    testWidgets('Recent delete notice retires before '
        '${allGames ? 'all Games' : 'review'} navigation', (
      WidgetTester tester,
    ) async {
      await repository.save(_scaledArchive(0));
      await repository.save(_scaledArchive(1));
      await tester.pumpWidget(_recentToGamesHarness(repository));
      await _finishLibraryLoad(tester);
      await tester.tap(
        find.byKey(const ValueKey<String>('recent-game-menu-scale-0001')),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text('Delete game'));
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(const ValueKey<String>('game-library-delete-confirm')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Game deleted'), findsOneWidget);
      await tester.tap(
        find.byKey(
          ValueKey<String>(
            allGames ? 'game-library-action' : 'recent-game-scale-0000',
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _finishLibraryLoad(tester);
      expect(find.text('Game deleted'), findsNothing);
      expect(
        allGames
            ? find.byType(GameLibraryScreen)
            : find.byType(PostGameReviewScreen),
        findsOneWidget,
      );
      await tester.pageBack();
      await tester.pumpAndSettle();
      expect(find.text('Game deleted'), findsNothing);
      expect(repository.snapshot.games.length, 1);
    });
  }

  testWidgets('Recent games exposes the same recoverable delete action', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_recentHarness(repository));
    await _finishLibraryLoad(tester);

    await tester.tap(
      find.byKey(ValueKey<String>('recent-game-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Delete game'));
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-delete-confirm')),
    );
    await tester.pumpAndSettle();

    expect(repository.snapshot.games, isEmpty);
    expect(find.text('Game deleted'), findsOneWidget);
    await tester.tap(find.text('Undo'));
    await tester.pumpAndSettle();
    expect(repository.snapshot.games.single.libraryId, archive.libraryId);
    expect(
      find.byKey(ValueKey<String>('recent-game-${archive.libraryId}')),
      findsOneWidget,
    );
  });

  testWidgets('failed rename keeps the draft and succeeds on retry', (
    WidgetTester tester,
  ) async {
    await repository.close();
    final _FailsFirstRenameRepository retryRepository =
        _FailsFirstRenameRepository();
    repository = retryRepository;
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const ValueKey<String>('game-library-name-field')),
      'Keep this draft',
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pumpAndSettle();

    expect(find.byType(AlertDialog), findsOneWidget);
    expect(find.text('Keep this draft'), findsOneWidget);
    expect(
      find.text('The game name couldn’t be saved. Try again.'),
      findsOneWidget,
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pumpAndSettle();

    expect(find.byType(AlertDialog), findsNothing);
    expect(find.text('Keep this draft'), findsOneWidget);
    expect(retryRepository.renameCalls, 2);
  });

  testWidgets('pending rename can be dismissed without cancelling its write', (
    WidgetTester tester,
  ) async {
    await repository.close();
    final _HeldRenameRepository heldRepository = _HeldRenameRepository();
    repository = heldRepository;
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const ValueKey<String>('game-library-name-field')),
      'Still saving',
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pump();

    expect(heldRepository.renameCalls, 1);
    expect(find.text('Saving continues after you close this.'), findsOneWidget);
    final TextButton cancel = tester.widget<TextButton>(
      find.widgetWithText(TextButton, 'Close'),
    );
    expect(cancel.onPressed, isNotNull);
    await tester.tap(find.widgetWithText(TextButton, 'Close'));
    await tester.pump();
    heldRepository.release.complete();
    await tester.pump();
    await tester.pumpAndSettle();
    expect(find.byType(AlertDialog), findsNothing);
    expect(find.byType(GameLibraryScreen), findsOneWidget);
    expect(repository.snapshot.games.single.displayName, 'Still saving');
  });

  testWidgets('permanent name bounds ask the user to shorten the draft', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    final String oversizedBytes = List<String>.filled(21, '👨‍👩‍👧‍👦').join();
    await tester.enterText(
      find.byKey(const ValueKey<String>('game-library-name-field')),
      oversizedBytes,
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pump();

    expect(find.text('Shorten this name before saving it.'), findsOneWidget);
    expect(
      find.text('The game name couldn’t be saved. Try again.'),
      findsNothing,
    );
    expect(find.byType(AlertDialog), findsOneWidget);
  });

  testWidgets('recovered game naming explains the recovery prerequisite', (
    WidgetTester tester,
  ) async {
    await repository.close();
    final GameArchive archive = completedClassicArchive();
    repository = _RecoveredRenameRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[archive],
        recoveredFiles: <String>['game-${archive.libraryId}.json'],
      ),
    );
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-name-save')),
    );
    await tester.pumpAndSettle();

    expect(
      find.text(
        'Names can’t be changed for games shown from a recovery copy. You can still review this game.',
      ),
      findsOneWidget,
    );
    expect(
      find.text('The game name couldn’t be saved. Try again.'),
      findsNothing,
    );
  });

  testWidgets('rename dialog remains usable with keyboard at 200 percent', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(
      _harness(repository, textScaler: const TextScaler.linear(2)),
    );
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-menu-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('Name game'));
    await tester.pumpAndSettle();
    tester.view.viewInsets = FakeViewPadding(
      bottom: 260 * tester.view.devicePixelRatio,
    );
    addTearDown(() => tester.view.resetViewInsets());
    await tester.pumpAndSettle();

    expect(find.byType(Dialog), findsOneWidget);
    expect(find.byType(AlertDialog), findsNothing);
    const String helper = 'Leave blank to use the game result.';
    final Finder helperText = find.text(helper);
    expect(helperText, findsOneWidget);
    final Finder helperParagraph = find.descendant(
      of: helperText,
      matching: find.byType(RichText),
    );
    expect(
      tester.renderObject<RenderParagraph>(helperParagraph).didExceedMaxLines,
      isFalse,
    );
    expect(
      find.byKey(const ValueKey<String>('game-library-name-save')),
      findsOneWidget,
    );
    expect(find.text('Cancel'), findsOneWidget);
    expect(
      tester.getBottomRight(helperText).dy,
      lessThanOrEqualTo(
        tester
            .getTopLeft(
              find.byKey(const ValueKey<String>('game-library-name-save')),
            )
            .dy,
      ),
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'game-library-name-dialog-320-200');
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    expect(find.byType(Dialog), findsNothing);
    expect(
      find.byKey(const ValueKey<String>('game-library-name-field')),
      findsNothing,
    );
    expect(find.byType(GameLibraryScreen), findsOneWidget);
  });

  testWidgets('opens a saved game through its accessibility action', (
    WidgetTester tester,
  ) async {
    final SemanticsHandle semantics = tester.ensureSemantics();
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    final Finder game = find.bySemanticsLabel(
      RegExp(r'^Saved .+\. Classic\. P1 \(W\) won\.'),
    );
    final gameNode = tester.getSemantics(game);
    expect(
      gameNode.getSemanticsData().hasAction(ui.SemanticsAction.tap),
      isTrue,
    );
    gameNode.owner!.performAction(gameNode.id, ui.SemanticsAction.tap);
    await tester.pumpAndSettle();

    expect(find.byType(PostGameReviewScreen), findsOneWidget);
    expect(
      tester
          .widget<PostGameReviewScreen>(find.byType(PostGameReviewScreen))
          .reviewOwner
          ?.analyzer,
      isA<BoundedLocalPostGameReviewAnalyzer>(),
    );
    await tester.scrollUntilVisible(
      find.byKey(const ValueKey<String>('review-moment-0')),
      300,
      scrollable: find.byType(Scrollable).first,
    );
    expect(find.textContaining('One line to explore'), findsWidgets);
    semantics.dispose();
  });

  testWidgets('saved game uses the app-lifetime engine review owner', (
    WidgetTester tester,
  ) async {
    final archive = completedClassicArchive();
    await repository.save(archive);
    final runtime = EngineRuntime();
    addTearDown(runtime.close);
    final analyzer = EngineBackedPostGameReviewAnalyzer(runtime: runtime);
    await tester.pumpWidget(_harness(repository, analyzer: analyzer));
    await _finishLibraryLoad(tester);

    await tester.tap(find.byType(GamePositionThumbnail).first);
    await tester.pumpAndSettle();

    expect(find.byType(PostGameReviewScreen), findsOneWidget);
    expect(
      tester
          .widget<PostGameReviewScreen>(find.byType(PostGameReviewScreen))
          .reviewOwner
          ?.analyzer,
      same(analyzer),
    );
  });

  testWidgets('Games keeps an active phone-owned archive review-only', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive().branchAt(
      ply: 1,
      libraryId: 'active-phone-game',
      gameId: 'active_phone_game',
      createdAt: DateTime.utc(2026, 9, 13),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    await tester.tap(
      find.byKey(const ValueKey<String>('game-library-game-active-phone-game')),
    );
    await tester.pumpAndSettle();

    expect(find.byType(AppChessPlayScreen), findsNothing);
    expect(find.byType(PostGameReviewScreen), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('app-chess-moves')), findsNothing);
  });

  testWidgets('Recent games keeps an active phone-owned archive review-only', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive().branchAt(
      ply: 1,
      libraryId: 'recent-phone-game',
      gameId: 'recent_phone_game',
      createdAt: DateTime.utc(2026, 9, 13),
      whitePlayer: const ArchivedPlayerAttribution.guest(),
      blackPlayer: const ArchivedPlayerAttribution.guest(),
    );
    await repository.save(archive);
    await tester.pumpWidget(_recentHarness(repository));
    await _finishLibraryLoad(tester);

    expect(
      find.bySemanticsLabel(RegExp(r'Continue game on this device\.$')),
      findsNothing,
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('recent-game-recent-phone-game')),
    );
    await tester.pumpAndSettle();

    expect(find.byType(AppChessPlayScreen), findsNothing);
    expect(find.byType(PostGameReviewScreen), findsOneWidget);
  });

  testWidgets('deletion while detail is open gives recovery and exit', (
    WidgetTester tester,
  ) async {
    final GameArchive archive = completedClassicArchive();
    await repository.save(archive);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
    );
    await tester.pumpAndSettle();
    expect(find.byType(PostGameReviewScreen), findsOneWidget);

    await repository.delete(archive.libraryId);
    await tester.pumpAndSettle();

    expect(find.text('This saved game is no longer available'), findsOneWidget);
    final Finder back = find.byKey(
      const ValueKey<String>('game-library-detail-back'),
    );
    expect(tester.getSize(back).height, greaterThanOrEqualTo(48));
    await tester.tap(back);
    await tester.pumpAndSettle();
    expect(find.text('No saved games yet'), findsOneWidget);
  });

  testWidgets('different-game updates cannot replace an open detail', (
    WidgetTester tester,
  ) async {
    final GameArchive selected = completedClassicArchive();
    final GameArchive other = duckArchive();
    await repository.save(selected);
    await repository.save(other);
    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);
    await tester.tap(
      find.byKey(ValueKey<String>('game-library-game-${selected.libraryId}')),
    );
    await tester.pumpAndSettle();

    await repository.save(_updated(other, revision: 4));
    await tester.pumpAndSettle();
    expect(
      tester
          .widget<PostGameReviewScreen>(find.byType(PostGameReviewScreen))
          .archive
          .libraryId,
      selected.libraryId,
    );

    await repository.save(_updated(selected, revision: 5));
    await tester.pumpAndSettle();
    expect(
      tester
          .widget<PostGameReviewScreen>(find.byType(PostGameReviewScreen))
          .archive
          .lineRevision,
      5,
    );
  });

  testWidgets('reports recovered and rejected files without hiding games', (
    WidgetTester tester,
  ) async {
    await repository.close();
    repository = _MemoryRepository(
      snapshot: GameLibrarySnapshot(
        games: <GameArchive>[completedClassicArchive()],
        recoveredFiles: const <String>['recovered.json'],
        rejectedFiles: const <String>['rejected.json'],
      ),
    );

    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    expect(
      find.text('Library recovery: 1 game restored; 1 file unavailable.'),
      findsOneWidget,
    );
    expect(find.text('P1 (W) won'), findsOneWidget);
  });

  testWidgets(
    'Games remains available for a persistent current-corrupt recovery',
    (WidgetTester tester) async {
      await repository.close();
      final Directory directory = (await tester.runAsync(
        () => Directory.systemTemp.createTemp('litch-games-recovery-'),
      ))!;
      addTearDown(() async {
        if (await directory.exists()) await directory.delete(recursive: true);
      });
      final GameArchive archive = completedClassicArchive();
      final sequence = await _persistentRecoverySequence(
        tester,
        directory,
        archive,
      );
      expect(
        sequence.firstWindow.generation,
        isNot(sequence.firstCatalog.generation),
      );
      expect(
        sequence.recoveredCatalog.generation,
        sequence.firstWindow.generation,
      );
      expect(
        sequence.recoveredWindow.generation,
        sequence.recoveredCatalog.generation,
      );
      final _RecoverySequenceRepository sequenceRepository =
          _RecoverySequenceRepository(
            directory,
            catalogs: <GameLibraryCatalog>[
              sequence.firstCatalog,
              sequence.recoveredCatalog,
            ],
            windows: <GameLibraryWindow>[
              sequence.firstWindow,
              sequence.recoveredWindow,
            ],
          );
      repository = sequenceRepository;

      await tester.pumpWidget(_directHarness(repository));
      await _finishLibraryLoad(tester);

      expect(find.text('Saved games aren’t available'), findsNothing);
      expect(
        find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('game-library-recovery-warning')),
        findsOneWidget,
      );
      expect(sequenceRepository.catalogCalls, 2);
      expect(sequenceRepository.windowCalls, 2);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      await repository.close();
      repository = _MemoryRepository();
      await tester.runAsync(() => directory.delete(recursive: true));
    },
  );

  testWidgets(
    'Recent remains available for a persistent current-corrupt recovery',
    (WidgetTester tester) async {
      await repository.close();
      final Directory directory = (await tester.runAsync(
        () => Directory.systemTemp.createTemp('litch-recent-recovery-'),
      ))!;
      addTearDown(() async {
        if (await directory.exists()) await directory.delete(recursive: true);
      });
      final GameArchive archive = completedClassicArchive();
      final sequence = await _persistentRecoverySequence(
        tester,
        directory,
        archive,
      );
      expect(
        sequence.firstWindow.generation,
        isNot(sequence.firstCatalog.generation),
      );
      expect(
        sequence.recoveredWindow.generation,
        sequence.recoveredCatalog.generation,
      );
      final _RecoverySequenceRepository sequenceRepository =
          _RecoverySequenceRepository(
            directory,
            catalogs: <GameLibraryCatalog>[
              sequence.firstCatalog,
              sequence.recoveredCatalog,
            ],
            windows: <GameLibraryWindow>[
              sequence.firstWindow,
              sequence.recoveredWindow,
            ],
          );
      repository = sequenceRepository;

      await tester.pumpWidget(_recentHarness(repository));
      await _finishLibraryLoad(tester);

      expect(
        find.text(
          'The library could not be read. Your saved files have not been changed.',
        ),
        findsNothing,
      );
      expect(
        find.byKey(ValueKey<String>('recent-game-${archive.libraryId}')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey<String>('recent-games-recovery-warning')),
        findsOneWidget,
      );
      expect(sequenceRepository.catalogCalls, 2);
      expect(sequenceRepository.windowCalls, 2);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
      await repository.close();
      repository = _MemoryRepository();
      await tester.runAsync(() => directory.delete(recursive: true));
    },
  );

  testWidgets('Games serves validated archives while index repair is blocked', (
    WidgetTester tester,
  ) async {
    await repository.close();
    final Directory directory = (await tester.runAsync(
      () => Directory.systemTemp.createTemp('litch-games-index-blocked-'),
    ))!;
    addTearDown(() async {
      if (await directory.exists()) await directory.delete(recursive: true);
    });
    final GameArchive archive = completedClassicArchive();
    final projection = await _blockedIndexProjection(
      tester,
      directory,
      archive,
    );
    final _RecoverySequenceRepository sequenceRepository =
        _RecoverySequenceRepository(
          directory,
          catalogs: <GameLibraryCatalog>[projection.catalog],
          windows: <GameLibraryWindow>[projection.window],
        );
    repository = sequenceRepository;

    await tester.pumpWidget(_directHarness(repository));
    await _finishLibraryLoad(tester);

    expect(find.text('Saved games aren’t available'), findsNothing);
    expect(
      find.byKey(ValueKey<String>('game-library-game-${archive.libraryId}')),
      findsOneWidget,
    );
    expect(sequenceRepository.catalogCalls, 1);
    expect(sequenceRepository.windowCalls, 1);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await repository.close();
    repository = _MemoryRepository();
    await tester.runAsync(() => directory.delete(recursive: true));
  });

  testWidgets('Recent serves validated archives while index repair is blocked', (
    WidgetTester tester,
  ) async {
    await repository.close();
    final Directory directory = (await tester.runAsync(
      () => Directory.systemTemp.createTemp('litch-recent-index-blocked-'),
    ))!;
    addTearDown(() async {
      if (await directory.exists()) await directory.delete(recursive: true);
    });
    final GameArchive archive = completedClassicArchive();
    final projection = await _blockedIndexProjection(
      tester,
      directory,
      archive,
    );
    final _RecoverySequenceRepository sequenceRepository =
        _RecoverySequenceRepository(
          directory,
          catalogs: <GameLibraryCatalog>[projection.catalog],
          windows: <GameLibraryWindow>[projection.window],
        );
    repository = sequenceRepository;

    await tester.pumpWidget(_recentHarness(repository));
    await _finishLibraryLoad(tester);

    expect(
      find.text(
        'The library could not be read. Your saved files have not been changed.',
      ),
      findsNothing,
    );
    expect(
      find.byKey(ValueKey<String>('recent-game-${archive.libraryId}')),
      findsOneWidget,
    );
    expect(sequenceRepository.catalogCalls, 1);
    expect(sequenceRepository.windowCalls, 1);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    await repository.close();
    repository = _MemoryRepository();
    await tester.runAsync(() => directory.delete(recursive: true));
  });

  testWidgets('reports an unreadable library and offers a 48px retry', (
    WidgetTester tester,
  ) async {
    await repository.close();
    repository = _FailingRepository();

    await tester.pumpWidget(_harness(repository));
    await _finishLibraryLoad(tester);

    expect(find.text('Saved games aren’t available'), findsOneWidget);
    expect(
      tester
          .getSize(find.byKey(const ValueKey<String>('game-library-retry')))
          .height,
      greaterThanOrEqualTo(48),
    );
  });

  testWidgets('library remains usable at 200 percent text on a narrow phone', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 640));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await repository.save(completedClassicArchive());
    await repository.save(duckArchive());

    await tester.pumpWidget(
      _harness(repository, textScaler: TextScaler.linear(2)),
    );
    await _finishLibraryLoad(tester);

    expect(
      find.byKey(const ValueKey<String>('game-library-list')),
      findsOneWidget,
    );
    expect(find.text('Load position'), findsOneWidget);
    expect(find.text('Load'), findsNothing);
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'game-library-320-200');
  });

  testWidgets(
    'labels every pinned Lichess result without collapsing its cause',
    (WidgetTester tester) async {
      final cases = <(GameArchive, String)>[
        (_providerArchive('created', GameOutcome.ongoing), 'Game in progress'),
        (_providerArchive('started', GameOutcome.ongoing), 'Game in progress'),
        (
          _providerArchive(
            'mate',
            GameOutcome.checkmate,
            providerWinner: 'white',
          ),
          'Checkmate · P1 (W) won',
        ),
        (_providerArchive('resign', GameOutcome.resignation), 'Resignation'),
        (_providerArchive('stalemate', GameOutcome.draw), 'Draw by stalemate'),
        (_providerArchive('draw', GameOutcome.draw), 'Draw'),
        (
          _providerArchive('insufficientMaterialClaim', GameOutcome.draw),
          'Draw by insufficient material',
        ),
        (_providerArchive('timeout', GameOutcome.timeout), 'Timeout'),
        (_providerArchive('outoftime', GameOutcome.timeout), 'Out of time'),
        (_providerArchive('aborted', GameOutcome.aborted), 'Game aborted'),
        (
          _providerArchive('noStart', GameOutcome.aborted),
          'Game did not start',
        ),
        (
          _providerArchive('cheat', GameOutcome.providerDefined),
          'Fair-play decision',
        ),
        (
          _providerArchive('unknownFinish', GameOutcome.providerDefined),
          'Game ended',
        ),
        (
          _providerArchive('variantEnd', GameOutcome.providerDefined),
          'Variant ended',
        ),
      ];
      await tester.binding.setSurfaceSize(const Size(800, 2400));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await repository.close();
      repository = _MemoryRepository(
        snapshot: GameLibrarySnapshot(games: cases.map((entry) => entry.$1)),
      );
      await tester.pumpWidget(_harness(repository));
      await _finishLibraryLoad(tester);

      for (final (archive, label) in cases) {
        final Finder row = find.byKey(
          ValueKey<String>('game-library-game-${archive.libraryId}'),
        );
        expect(row, findsOneWidget);
        expect(
          find.descendant(of: row, matching: find.text(label)),
          findsOneWidget,
        );
      }

      final GameArchive aborted = cases
          .firstWhere((entry) => entry.$1.provider!.rawStatus == 'aborted')
          .$1;
      await tester.tap(
        find.byKey(ValueKey<String>('game-library-game-${aborted.libraryId}')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Game aborted'), findsOneWidget);
      await tester.pageBack();
      await tester.pumpAndSettle();

      final GameArchive checkmate = cases
          .firstWhere((entry) => entry.$1.provider!.rawStatus == 'mate')
          .$1;
      await tester.tap(
        find.byKey(
          ValueKey<String>('game-library-game-${checkmate.libraryId}'),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('Checkmate · P1 (W) won'), findsOneWidget);
    },
  );
}

class _MemoryRepository extends GameLibraryRepository {
  _MemoryRepository({GameLibrarySnapshot? snapshot})
    : _snapshot = snapshot ?? GameLibrarySnapshot(),
      super(GameLibraryStore(Directory.systemTemp));

  GameLibrarySnapshot _snapshot;
  final StreamController<GameLibrarySnapshot> _controller =
      StreamController<GameLibrarySnapshot>.broadcast(sync: true);
  final List<({int offset, int limit})> windowRequests =
      <({int offset, int limit})>[];

  @override
  GameLibrarySnapshot get snapshot => _snapshot;

  @override
  Stream<GameLibrarySnapshot> get changes => _controller.stream;

  @override
  Future<GameLibrarySnapshot> load() {
    _controller.add(_snapshot);
    return Future<GameLibrarySnapshot>.value(_snapshot);
  }

  List<GameArchive> get _orderedGames => List<GameArchive>.of(_snapshot.games)
    ..sort((GameArchive left, GameArchive right) {
      final int byUpdate = right.updatedAt.compareTo(left.updatedAt);
      return byUpdate != 0
          ? byUpdate
          : left.libraryId.compareTo(right.libraryId);
    });

  @override
  Future<GameLibraryCatalog> loadCatalog() async {
    final GameLibrarySnapshot snapshot = _snapshot;
    return GameLibraryCatalog(
      totalCount: snapshot.games.length,
      recoveredFiles: snapshot.recoveredFiles,
      rejectedFiles: snapshot.rejectedFiles,
      namedGames: _orderedGames
          .map((GameArchive game) => game.displayName != null)
          .toList(growable: false),
    );
  }

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async {
    windowRequests.add((offset: offset, limit: limit));
    final List<GameArchive> games = _orderedGames;
    final int start = offset.clamp(0, games.length);
    final int end = (start + limit).clamp(start, games.length);
    return GameLibraryWindow(
      offset: start,
      totalCount: games.length,
      games: games.sublist(start, end),
    );
  }

  @override
  Future<GameArchive?> findByIdentity(
    String libraryId,
    String gameId,
    GameArchiveOrigin origin,
  ) async => _snapshot.games
      .where(
        (GameArchive game) =>
            game.libraryId == libraryId &&
            game.gameId == gameId &&
            game.origin == origin,
      )
      .firstOrNull;

  @override
  Future<void> save(GameArchive game) {
    final List<GameArchive> games =
        _snapshot.games
            .where(
              (GameArchive existing) => existing.libraryId != game.libraryId,
            )
            .toList()
          ..add(game);
    _snapshot = GameLibrarySnapshot(
      games: games,
      recoveredFiles: _snapshot.recoveredFiles,
      rejectedFiles: _snapshot.rejectedFiles,
    );
    _controller.add(_snapshot);
    return Future<void>.value();
  }

  @override
  Future<void> rename(String libraryId, String? displayName) {
    final List<GameArchive> matches = _snapshot.games
        .where((GameArchive game) => game.libraryId == libraryId)
        .toList(growable: false);
    if (matches.length != 1) {
      return Future<void>.error(
        const FormatException(
          'The saved game is unavailable or has a duplicate identity.',
        ),
      );
    }
    _snapshot = GameLibrarySnapshot(
      games: _snapshot.games.map((GameArchive game) {
        return game.libraryId == libraryId ? game.rename(displayName) : game;
      }),
      recoveredFiles: _snapshot.recoveredFiles,
      rejectedFiles: _snapshot.rejectedFiles,
    );
    _controller.add(_snapshot);
    return Future<void>.value();
  }

  @override
  Future<GameArchive?> delete(String libraryId) {
    final removed = _snapshot.games
        .where((game) => game.libraryId == libraryId)
        .firstOrNull;
    _snapshot = GameLibrarySnapshot(
      games: _snapshot.games.where(
        (GameArchive game) => game.libraryId != libraryId,
      ),
      recoveredFiles: _snapshot.recoveredFiles,
      rejectedFiles: _snapshot.rejectedFiles,
    );
    _controller.add(_snapshot);
    return Future<GameArchive?>.value(removed);
  }

  @override
  Future<void> restoreDeleted(GameArchive removed) async {
    if (_snapshot.games.any(
      (game) =>
          game.libraryId == removed.libraryId ||
          (game.origin == removed.origin && game.gameId == removed.gameId),
    )) {
      return;
    }
    await save(removed);
  }

  @override
  Future<void> close() => _controller.close();
}

class _HeldDeleteRepository extends _MemoryRepository {
  final Completer<void> release = Completer<void>();
  int deleteCalls = 0;

  @override
  Future<GameArchive?> delete(String libraryId) async {
    deleteCalls += 1;
    await release.future;
    return super.delete(libraryId);
  }
}

class _HeldPageRepository extends _MemoryRepository {
  _HeldPageRepository({required super.snapshot});

  final Completer<GameLibraryWindow> release = Completer<GameLibraryWindow>();
  int calls = 0;
  int heldCalls = 0;

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) {
    calls += 1;
    if (calls == 1) return super.loadWindow(offset: offset, limit: limit);
    heldCalls += 1;
    return release.future;
  }

  Future<GameLibraryWindow> window({required int offset, required int limit}) {
    return super.loadWindow(offset: offset, limit: limit);
  }
}

class _LateRecoveryRepository extends _MemoryRepository {
  _LateRecoveryRepository({required super.snapshot});

  bool discovered = false;
  int generation = 0;

  @override
  Future<GameLibraryCatalog> loadCatalog() async {
    final GameLibrarySnapshot snapshot = _snapshot;
    return GameLibraryCatalog(
      totalCount: snapshot.games.length,
      recoveredFiles: snapshot.recoveredFiles,
      rejectedFiles: snapshot.rejectedFiles,
      namedGames: _orderedGames
          .map((GameArchive game) => game.displayName != null)
          .toList(growable: false),
      generation: generation,
    );
  }

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async {
    if (offset > 0 && !discovered) {
      discovered = true;
      generation += 1;
      _snapshot = GameLibrarySnapshot(
        games: _snapshot.games,
        recoveredFiles: const <String>['late-recovered.json'],
      );
      _controller.add(GameLibrarySnapshot());
    }
    final List<GameArchive> games = _orderedGames;
    final int start = offset.clamp(0, games.length);
    final int end = (start + limit).clamp(start, games.length);
    return GameLibraryWindow(
      offset: start,
      totalCount: games.length,
      games: games.sublist(start, end),
      generation: generation,
    );
  }
}

class _RecoverySequenceRepository extends GameLibraryRepository {
  _RecoverySequenceRepository(
    Directory directory, {
    required this.catalogs,
    required this.windows,
  }) : super(GameLibraryStore(directory));

  final List<GameLibraryCatalog> catalogs;
  final List<GameLibraryWindow> windows;
  int catalogCalls = 0;
  int windowCalls = 0;

  @override
  Future<GameLibraryCatalog> loadCatalog() async {
    return catalogs[catalogCalls++];
  }

  @override
  Future<GameLibraryWindow> loadWindow({
    required int offset,
    required int limit,
  }) async {
    return windows[windowCalls++];
  }
}

class _FailingRepository extends _MemoryRepository {
  _FailingRepository();

  @override
  Future<GameLibrarySnapshot> load() => Future<GameLibrarySnapshot>.error(
    const FileSystemException('unavailable'),
  );

  @override
  Future<GameLibraryCatalog> loadCatalog() => Future<GameLibraryCatalog>.error(
    const FileSystemException('unavailable'),
  );
}

class _FailsFirstRenameRepository extends _MemoryRepository {
  int renameCalls = 0;

  @override
  Future<void> rename(String libraryId, String? displayName) {
    renameCalls += 1;
    if (renameCalls == 1) {
      return Future<void>.error(
        const FileSystemException('simulated write failure'),
      );
    }
    return super.rename(libraryId, displayName);
  }
}

class _HeldRenameRepository extends _MemoryRepository {
  final Completer<void> release = Completer<void>();
  int renameCalls = 0;

  @override
  Future<void> rename(String libraryId, String? displayName) async {
    renameCalls += 1;
    await release.future;
    await super.rename(libraryId, displayName);
  }
}

class _RecoveredRenameRepository extends _MemoryRepository {
  _RecoveredRenameRepository({required super.snapshot});

  @override
  Future<void> rename(String libraryId, String? displayName) {
    return Future<void>.error(const GameLibraryRecoveredRenameException());
  }
}
