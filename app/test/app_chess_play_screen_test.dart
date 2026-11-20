import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/game_archive.dart';
import 'package:litch_mobile/src/screens/app_chess_play_screen.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';

void main() {
  late _MemoryGameLibraryRepository repository;

  setUp(() {
    repository = _MemoryGameLibraryRepository();
  });

  tearDown(() => repository.close());

  testWidgets('phone game selects, moves, saves, and undoes locally', (
    WidgetTester tester,
  ) async {
    _setView(tester, const Size(390, 844));
    await _pumpPlay(tester, repository: repository);

    expect(find.text('Game on this device'), findsOneWidget);
    expect(find.text('Choose a P1 (W) piece.'), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey<String>('square-e2')));
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey<String>('legal-target-e3')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('legal-target-e4')),
      findsOneWidget,
    );
    expect(find.text('Move from e2.'), findsOneWidget);
    await captureHealthReview(tester, 'app-chess-portrait-selected-390');

    await tester.tap(find.byKey(const ValueKey<String>('square-e4')));
    await tester.pumpAndSettle();
    expect(find.text('Choose a P2 (B) piece.'), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('app-chess-moves')), findsNothing);

    await repository.load();
    final GameArchive saved = repository.snapshot.games.single;
    expect(saved.origin, GameArchiveOrigin.app);
    expect(saved.boardGameId, isNull);
    expect(saved.currentPly, 1);

    await tester.tap(find.byKey(const ValueKey<String>('app-chess-undo')));
    await tester.pumpAndSettle();
    expect(find.text('Choose a P1 (W) piece.'), findsOneWidget);
    await repository.load();
    expect(repository.snapshot.games.single.currentPly, 0);
  });

  testWidgets('phone game remains usable in compact landscape', (
    WidgetTester tester,
  ) async {
    _setView(tester, const Size(844, 390));

    await _pumpPlay(tester, repository: repository);

    expect(
      find.byKey(const ValueKey<String>('app-chess-landscape')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey<String>('square-a8')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('square-h1')), findsOneWidget);
    expect(
      tester.getSize(find.byKey(const ValueKey<String>('square-a8'))).width,
      greaterThanOrEqualTo(48),
    );
    expect(
      find.byKey(const ValueKey<String>('app-chess-landscape-back')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('app-chess-new-game')),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'app-chess-landscape-844x390');
  });

  testWidgets(
    'compact landscape keeps every square reachable above home inset',
    (WidgetTester tester) async {
      _setView(tester, const Size(844, 390));
      await _pumpPlay(
        tester,
        repository: repository,
        systemPadding: const EdgeInsets.only(bottom: 21),
      );

      expect(find.byKey(const ValueKey<String>('square-a8')), findsOneWidget);
      expect(find.byKey(const ValueKey<String>('square-h1')), findsOneWidget);
      expect(
        tester.getSize(find.byKey(const ValueKey<String>('square-a8'))).width,
        greaterThanOrEqualTo(46),
      );
      expect(
        find.byKey(const ValueKey<String>('app-chess-landscape-back')),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('promotion is explicit and commits the chosen piece', (
    WidgetTester tester,
  ) async {
    final GameArchive promotion = _appArchive(
      fen: '7k/P7/8/8/8/8/8/K7 w - - 0 1',
    );
    await _pumpPlay(tester, repository: repository, initialArchive: promotion);

    await tester.tap(find.byKey(const ValueKey<String>('square-a7')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('square-a8')));
    await tester.pumpAndSettle();

    expect(find.text('Choose a promotion'), findsOneWidget);
    await tester.tap(
      find.byKey(const ValueKey<String>('app-chess-promotion-n')),
    );
    await tester.pumpAndSettle();

    expect(
      repository.latest!.transitions.single.promotion,
      PromotionPiece.knight,
    );
    expect(find.byKey(const ValueKey<String>('app-chess-moves')), findsNothing);
  });

  testWidgets('save failure stays visible and retries the latest position', (
    WidgetTester tester,
  ) async {
    repository.failuresRemaining = 1;
    _setView(tester, const Size(390, 844));
    await _pumpPlay(tester, repository: repository);

    expect(
      find.text('This position is still here, but it could not be saved.'),
      findsOneWidget,
    );
    await captureHealthReview(tester, 'app-chess-save-failure-390');
    await tester.tap(find.text('Retry saving'));
    await tester.pumpAndSettle();

    expect(find.text('Retry saving'), findsNothing);
    expect(repository.latest?.currentPly, 0);
  });

  testWidgets('back waits for the current position to finish saving', (
    WidgetTester tester,
  ) async {
    final Completer<void> blocker = Completer<void>();
    repository.blocker = blocker;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: const <LocalizationsDelegate<dynamic>>[
          AppLocalizations.delegate,
          GlobalMaterialLocalizations.delegate,
          GlobalWidgetsLocalizations.delegate,
          GlobalCupertinoLocalizations.delegate,
        ],
        supportedLocales: AppLocalizations.supportedLocales,
        home: Builder(
          builder: (BuildContext context) => Scaffold(
            body: TextButton(
              onPressed: () => Navigator.of(context).push<void>(
                MaterialPageRoute<void>(
                  builder: (_) => AppChessPlayScreen(repository: repository),
                ),
              ),
              child: const Text('Open phone game'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('Open phone game'));
    await tester.pumpAndSettle();
    expect(find.byType(AppChessPlayScreen), findsOneWidget);

    await tester.binding.handlePopRoute();
    await tester.pump();
    expect(find.byType(AppChessPlayScreen), findsOneWidget);

    blocker.complete();
    await tester.pumpAndSettle();
    expect(find.byType(AppChessPlayScreen), findsNothing);
    expect(find.text('Open phone game'), findsOneWidget);
  });

  testWidgets('new game suspends and saves the old clock before replacement', (
    WidgetTester tester,
  ) async {
    _setView(tester, const Size(390, 844));
    await _pumpPlay(tester, repository: repository);
    await tester.tap(
      find.byKey(const ValueKey<String>('app-chess-clock-picker')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('app-chess-clock-1')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('square-e2')));
    await tester.pumpAndSettle();
    final String oldGameId = repository.latest!.gameId;
    final Completer<void> blocker = Completer<void>();
    repository.blocker = blocker;

    await tester.tap(find.byKey(const ValueKey<String>('app-chess-new-game')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Start new game'));
    await tester.pump();

    expect(
      find.byKey(const ValueKey<String>('legal-target-e4')),
      findsOneWidget,
    );
    expect(
      tester
          .widget<TextButton>(
            find.byKey(const ValueKey<String>('app-chess-new-game')),
          )
          .onPressed,
      isNull,
    );
    blocker.complete();
    await tester.pumpAndSettle();

    expect(find.byKey(const ValueKey<String>('legal-target-e4')), findsNothing);
    expect(repository.latest!.gameId, isNot(oldGameId));
    final GameArchive oldGame = repository.saved.lastWhere(
      (GameArchive archive) => archive.gameId == oldGameId,
    );
    expect(oldGame.clock!.runState, ArchivedGameClockRunState.suspended);
  });

  testWidgets('new game keeps failed old save available for retry', (
    WidgetTester tester,
  ) async {
    _setView(tester, const Size(390, 844));
    await _pumpPlay(tester, repository: repository);
    await tester.tap(
      find.byKey(const ValueKey<String>('app-chess-clock-picker')),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('app-chess-clock-1')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('square-e2')));
    await tester.pumpAndSettle();
    final String oldGameId = repository.latest!.gameId;
    repository.failuresRemaining = 1;

    await tester.tap(find.byKey(const ValueKey<String>('app-chess-new-game')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Start new game'));
    await tester.pumpAndSettle();

    expect(repository.latest!.gameId, oldGameId);
    expect(
      find.byKey(const ValueKey<String>('legal-target-e4')),
      findsOneWidget,
    );
    expect(find.text('Retry saving'), findsOneWidget);
    await tester.tap(find.text('Retry saving'));
    await tester.pumpAndSettle();
    expect(find.text('Retry saving'), findsNothing);
    expect(repository.latest!.gameId, oldGameId);
    expect(
      repository.latest!.clock!.runState,
      ArchivedGameClockRunState.suspended,
    );
  });

  testWidgets('200 percent text keeps the full board reachable', (
    WidgetTester tester,
  ) async {
    _setView(tester, const Size(390, 844));

    await _pumpPlay(
      tester,
      repository: repository,
      textScaler: const TextScaler.linear(2),
    );

    expect(
      find.byKey(const ValueKey<String>('app-chess-portrait')),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey<String>('square-a8')), findsOneWidget);
    expect(find.byKey(const ValueKey<String>('square-h1')), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey<String>('square-e2')));
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('legal-target-e4')),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'app-chess-portrait-390-200');
  });

  testWidgets('phone game exposes no computer-opponent controls', (
    WidgetTester tester,
  ) async {
    await _pumpPlay(tester, repository: repository);

    expect(
      find.byKey(const ValueKey<String>('app-chess-opponent')),
      findsNothing,
    );
    expect(find.text('Computer is choosing a move.'), findsNothing);
  });

  testWidgets('legacy computer save continues as a two-player game', (
    WidgetTester tester,
  ) async {
    final GameArchive legacy = _appArchive(
      fen: 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
      blackPlayer: const ArchivedPlayerAttribution.computer(),
    );
    await _pumpPlay(tester, repository: repository, initialArchive: legacy);

    await tester.tap(find.byKey(const ValueKey<String>('square-e2')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('square-e4')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey<String>('square-e7')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('square-e5')));
    await tester.pumpAndSettle();

    expect(repository.latest!.currentPly, 2);
    expect(repository.latest!.blackPlayer.kind, ArchivedPlayerKind.guest);
  });
}

void _setView(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

class _MemoryGameLibraryRepository extends GameLibraryRepository {
  _MemoryGameLibraryRepository()
    : super(GameLibraryStore(Directory.systemTemp));

  GameArchive? latest;
  final List<GameArchive> saved = <GameArchive>[];
  int failuresRemaining = 0;
  Completer<void>? blocker;

  @override
  GameLibrarySnapshot get snapshot => GameLibrarySnapshot(
    games: latest == null ? const <GameArchive>[] : <GameArchive>[latest!],
  );

  @override
  Future<void> save(GameArchive game) async {
    await blocker?.future;
    if (failuresRemaining > 0) {
      failuresRemaining -= 1;
      throw StateError('storage unavailable');
    }
    latest = game;
    saved.add(game);
  }

  @override
  Future<GameLibrarySnapshot> load() async => snapshot;

  @override
  Future<void> close() async {}
}

Future<void> _pumpPlay(
  WidgetTester tester, {
  required GameLibraryRepository repository,
  GameArchive? initialArchive,
  TextScaler textScaler = TextScaler.noScaling,
  EdgeInsets systemPadding = EdgeInsets.zero,
}) async {
  final Widget app = MaterialApp(
    localizationsDelegates: const <LocalizationsDelegate<dynamic>>[
      AppLocalizations.delegate,
      GlobalMaterialLocalizations.delegate,
      GlobalWidgetsLocalizations.delegate,
      GlobalCupertinoLocalizations.delegate,
    ],
    supportedLocales: AppLocalizations.supportedLocales,
    builder: (BuildContext context, Widget? child) => MediaQuery(
      data: MediaQuery.of(context).copyWith(
        textScaler: textScaler,
        padding: systemPadding,
        viewPadding: systemPadding,
      ),
      child: RepaintBoundary(
        key: const ValueKey<String>('app-chess-review-boundary'),
        child: child!,
      ),
    ),
    home: AppChessPlayScreen(
      repository: repository,
      initialArchive: initialArchive,
    ),
  );
  await tester.pumpWidget(app);
  await tester.pumpAndSettle();
  await decodeVisibleReviewImages(tester);
}

GameArchive _appArchive({
  required String fen,
  ArchivedPlayerAttribution blackPlayer =
      const ArchivedPlayerAttribution.guest(),
}) {
  return GameArchive(
    libraryId: 'app-promotion-test',
    gameId: 'app_promotion_test',
    origin: GameArchiveOrigin.app,
    createdAt: DateTime.utc(2026, 9, 13),
    updatedAt: DateTime.utc(2026, 9, 13),
    status: GameArchiveStatus.active,
    captureQuality: GameCaptureQuality.complete,
    variant: GameVariant.classic,
    outcome: GameOutcome.ongoing,
    winner: GameWinner.none,
    lineRevision: 0,
    positions: <ArchivedPosition>[ArchivedPosition(ply: 0, fen: fen)],
    transitions: const <ArchivedTransition>[],
    whitePlayer: const ArchivedPlayerAttribution.guest(),
    blackPlayer: blackPlayer,
  );
}
