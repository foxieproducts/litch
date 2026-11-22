import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/home_assistance_state.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'golden_test_support.dart';
import 'localization_test_support.dart';

void main() {
  const Map<String, String> expectedPieces = <String, String>{
    'a8': 'pawn',
    'b8': 'rook',
    'c8': 'knight',
    'd8': 'bishop',
    'e8': 'queen',
    'f8': 'king',
    'a7': 'pawn',
    'b7': 'rook',
    'c7': 'knight',
    'd7': 'bishop',
    'e7': 'queen',
    'f7': 'king',
  };
  const List<String> assetNames = <String>[
    'pawn',
    'rook',
    'knight',
    'bishop',
    'queen',
    'king',
    'duck',
  ];

  HomeBoardSnapshot snapshot() {
    return HomeBoardSnapshot(
      position: HomeChessPosition.tryParse(
        'PRNBQK2/prnbqk2/8/8/8/8/8/8 w - - 0 1',
      ),
      gameStatus: const HomeGameStatus(
        variant: 'duck',
        duckSquare: 7,
        duckMovePending: false,
        outcome: 'none',
        winner: 'none',
      ),
    );
  }

  HomeBoardSnapshot mismatchSnapshot() {
    final HomeBoardSnapshot baseline = snapshot();
    return baseline.copyWith(
      physicalStatus: HomePhysicalBoardStatus(
        occupiedSquares:
            baseline.expectedOccupiedSquares! ^
            (BigInt.one << 4) ^
            (BigInt.one << 6),
      ),
    );
  }

  Future<void> pumpBoard(
    WidgetTester tester, {
    required HomeBoardSnapshot value,
    required double size,
    bool liveGuidanceAvailable = true,
  }) async {
    await tester.binding.setSurfaceSize(Size(size + 40, size + 40));
    await tester.pumpWidget(
      MaterialApp(
        debugShowCheckedModeBanner: false,
        // Static artwork/semantics fixtures use the supported steady cue.
        // Timed production motion is covered by the amber-guidance journey.
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context).copyWith(disableAnimations: true),
          child: child!,
        ),
        home: ColoredBox(
          color: Colors.white,
          child: Center(
            child: RepaintBoundary(
              key: const ValueKey<String>('board-capture'),
              child: SizedBox.square(
                dimension: size,
                child: HomeChessboard(
                  snapshot: value,
                  maxSize: size,
                  liveGuidanceAvailable: liveGuidanceAvailable,
                ),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
  }

  Image imageForSquare(WidgetTester tester, String square) {
    final Finder artwork = find.byKey(ValueKey<String>('piece-$square'));
    return tester.widget<Image>(
      find.descendant(of: artwork, matching: find.byType(Image)),
    );
  }

  String assetForSquare(WidgetTester tester, String square) {
    final ImageProvider<Object> provider = imageForSquare(tester, square).image;
    final AssetImage asset = provider is ResizeImage
        ? provider.imageProvider as AssetImage
        : provider as AssetImage;
    return asset.assetName;
  }

  testWidgets(
    'unavailable live authority retains pieces without correction or turn speech',
    (tester) async {
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final semantics = tester.ensureSemantics();
      try {
        final value = mismatchSnapshot();
        await pumpBoard(tester, value: value, size: 320);
        expect(
          find.byKey(const ValueKey('expected-missing-e8')),
          findsOneWidget,
        );
        await pumpBoard(
          tester,
          value: value,
          size: 320,
          liveGuidanceAvailable: false,
        );
        expect(find.byKey(const ValueKey('expected-missing-e8')), findsNothing);
        expect(
          find.byKey(const ValueKey('unexpected-occupied-g8')),
          findsNothing,
        );
        expect(
          find.bySemanticsLabel(RegExp(r'(expected on|lifted|to move)')),
          findsNothing,
        );
        expect(assetForSquare(tester, 'e8'), contains('queen.png'));
        await pumpBoard(tester, value: value, size: 320);
        expect(
          find.byKey(const ValueKey('expected-missing-e8')),
          findsOneWidget,
        );
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets(
    'both kings legal castling destinations follow rotation and seating',
    (WidgetTester tester) async {
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final SemanticsHandle semantics = tester.ensureSemantics();
      for (final bool black in <bool>[false, true]) {
        for (final bool rotated in <bool>[false, true]) {
          for (final bool oppositeEnd in <bool>[false, true]) {
            final HomeChessPosition position = HomeChessPosition.tryParse(
              'r3k2r/8/8/8/8/8/8/R3K2R ${black ? 'b' : 'w'} KQkq - 0 1',
            )!;
            final int source = black ? 4 : 60;
            final int queenSide = black ? 2 : 58;
            final int kingSide = black ? 6 : 62;
            final HomeBoardSnapshot value = HomeBoardSnapshot(
              position: position,
              homeStateAction: HomeStateAction.lift,
              physicalStatus: HomePhysicalBoardStatus(
                occupiedSquares:
                    position.occupiedSquares ^ (BigInt.one << source),
                rotated: rotated,
                p1AtOppositeEnd: oppositeEnd,
              ),
              assistance: HomeAssistanceState(
                requested: true,
                manualAvailable: false,
                coachingOwned: false,
                presentation: HomeAssistancePresentation.legal,
                legalSource: source,
                legalDestinations:
                    (BigInt.one << queenSide) | (BigInt.one << kingSide),
              ),
            );
            await pumpBoard(tester, value: value, size: 320);
            final String sourceName = algebraicSquare(
              oppositeEnd ? 63 - source : source,
            );
            expect(
              find.byKey(ValueKey<String>('assistance-from-$sourceName')),
              findsOneWidget,
            );
            for (final int destination in <int>[queenSide, kingSide]) {
              final String name = algebraicSquare(
                oppositeEnd ? 63 - destination : destination,
              );
              final Finder marker = find.byKey(
                ValueKey<String>('assistance-to-$name'),
              );
              final Finder square = find.byKey(
                ValueKey<String>('square-$name'),
              );
              expect(marker, findsOneWidget);
              expect(
                tester.getRect(square).contains(tester.getCenter(marker)),
                isTrue,
              );
              expect(
                tester.getSemantics(square).label,
                contains('Legal destination'),
              );
            }
            final Offset sourceCenter = tester.getCenter(
              find.byKey(ValueKey<String>('square-$sourceName')),
            );
            final Offset boardCenter = tester.getCenter(
              find.byKey(const ValueKey<String>('home-chessboard-surface')),
            );
            final bool sourceAtTop = black != oppositeEnd;
            expect(sourceCenter.dy < boardCenter.dy, sourceAtTop);
          }
        }
      }
      semantics.dispose();
    },
  );

  testWidgets('compound Duck markers coexist and clear behind CPU authority', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final HomeBoardSnapshot value = HomeBoardSnapshot(
      position: HomeChessPosition.tryParse('7k/8/8/8/8/8/4P3/K7 w - - 0 1'),
      assistance: const HomeAssistanceState(
        requested: true,
        manualAvailable: true,
        coachingOwned: false,
        presentation: HomeAssistancePresentation.compoundDuck,
        move: (from: 52, to: 36),
        duckMove: (from: 24, to: 35),
      ),
    );
    await pumpBoard(tester, value: value, size: 320);
    for (final String key in <String>[
      'assistance-from-e2',
      'assistance-to-e4',
      'assistance-from-a5',
      'duck-assistance-to-d4',
    ]) {
      expect(find.byKey(ValueKey<String>(key)), findsOneWidget);
    }
    expect(value.assistanceGuidance, 'Hint: e2 to e4, then duck a5 to d4.');
    await pumpBoard(
      tester,
      value: value.copyWith(
        homeStateAction: HomeStateAction.lift,
        assistance: HomeAssistanceState(
          requested: true,
          manualAvailable: false,
          coachingOwned: false,
          presentation: HomeAssistancePresentation.legal,
          move: (from: 52, to: 36),
          duckMove: (from: 24, to: 44),
          legalSource: 52,
          legalDestinations: (BigInt.one << 36) | (BigInt.one << 44),
        ),
      ),
      size: 320,
    );
    final chessTarget = find.byKey(const ValueKey('assistance-to-e3'));
    final duckTarget = find.byKey(const ValueKey('duck-assistance-to-e3'));
    expect(chessTarget, findsOneWidget);
    expect(
      find.descendant(of: duckTarget, matching: find.byType(Image)),
      findsOneWidget,
    );
    expect(
      tester.getRect(chessTarget).overlaps(tester.getRect(duckTarget)),
      isFalse,
    );
    final capturePosition = HomeChessPosition.tryParse(
      '7k/8/8/8/8/3p4/4P3/K7 w - - 0 1',
    )!;
    final capture = value.copyWith(
      position: capturePosition,
      physicalStatus: HomePhysicalBoardStatus(
        occupiedSquares: capturePosition.occupiedSquares | (BigInt.one << 24),
      ),
      assistance: const HomeAssistanceState(
        requested: true,
        manualAvailable: true,
        coachingOwned: false,
        presentation: HomeAssistancePresentation.compoundDuck,
        move: (from: 52, to: 43),
        duckMove: (from: 24, to: 35),
      ),
      gameStatus: const HomeGameStatus(
        variant: 'duck',
        duckSquare: 24,
        duckMovePending: false,
        outcome: 'none',
        winner: 'none',
      ),
    );
    await pumpBoard(tester, value: capture, size: 320);
    expect(find.byKey(const ValueKey('capture-removal-d3')), findsOneWidget);
    expect(find.byKey(const ValueKey('duck-assistance-to-d4')), findsOneWidget);
    expect(
      capture.assistanceGuidance,
      'Remove the piece on d3, then move the piece from e2 to d3. Duck hint: a5 to d4.',
    );

    await pumpBoard(
      tester,
      value: value.copyWith(
        homeStateAction: HomeStateAction.cpu,
        physicalMovePrompt: const HomePhysicalMovePrompt(
          fromSquare: 52,
          toSquare: 36,
        ),
      ),
      size: 320,
    );
    expect(
      find.byKey(const ValueKey<String>('cpu-move-to-e4')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('assistance-to-e4')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('duck-assistance-to-d4')),
      findsNothing,
    );
    await pumpBoard(
      tester,
      value: value.copyWith(clearAssistance: true),
      size: 320,
    );
    expect(
      find.byKey(const ValueKey<String>('assistance-from-e2')),
      findsNothing,
    );
  });

  testWidgets('maps every FEN piece token to the intended asset', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();

    await pumpBoard(tester, value: snapshot(), size: 320);

    for (final MapEntry<String, String> expected in expectedPieces.entries) {
      expect(
        assetForSquare(tester, expected.key),
        'assets/chess_pieces/cameos_v3/${expected.value}.png',
      );
      final ImageProvider<Object> provider = imageForSquare(
        tester,
        expected.key,
      ).image;
      expect(provider, isA<ResizeImage>());
      expect((provider as ResizeImage).width, 256);
    }
    expect(find.bySemanticsLabel('a8, P1 pawn'), findsOneWidget);
    expect(find.bySemanticsLabel('a7, P2 pawn'), findsOneWidget);
    expect(find.bySemanticsLabel(RegExp(r'^h8, empty, duck')), findsOneWidget);

    final Image duck = tester.widget<Image>(
      find.descendant(
        of: find.byKey(const ValueKey<String>('duck-marker-h8')),
        matching: find.byType(Image),
      ),
    );
    final ResizeImage duckProvider = duck.image as ResizeImage;
    expect(
      (duckProvider.imageProvider as AssetImage).assetName,
      'assets/chess_pieces/cameos_v2/duck.png',
    );
    expect(duckProvider.width, 256);

    expect(
      find.descendant(
        of: find.byKey(const ValueKey<String>('piece-a8')),
        matching: find.byType(ColorFiltered),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const ValueKey<String>('piece-a7')),
        matching: find.byType(ColorFiltered),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const ValueKey<String>('duck-marker-h8')),
        matching: find.byType(ColorFiltered),
      ),
      findsNothing,
    );
    semantics.dispose();
  });

  testWidgets('screen follows BOARD_ROTATE independently of player assignment', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final position = HomeChessPosition.tryParse(
      'r3k2r/8/8/8/8/8/8/R3K2R w - - 0 1',
    )!;
    for (final rotated in [false, true]) {
      for (final opposite in [false, true]) {
        final physical = HomePhysicalBoardStatus(
          occupiedSquares: position.occupiedSquares,
          rotated: rotated,
          p1AtOppositeEnd: opposite,
        );
        await pumpBoard(
          tester,
          value: HomeBoardSnapshot(
            position: position,
            physicalStatus: physical,
          ),
          size: 320,
        );
        final p1Y = tester
            .getCenter(
              find.byKey(ValueKey<String>(opposite ? 'piece-d8' : 'piece-e1')),
            )
            .dy;
        final p2Y = tester
            .getCenter(
              find.byKey(ValueKey<String>(opposite ? 'piece-d1' : 'piece-e8')),
            )
            .dy;
        expect(
          p1Y > p2Y,
          !opposite,
          reason:
              'the phone must mirror the effective physical square orientation; '
              'rotation=$rotated opposite=$opposite',
        );
        expect(
          physical.squareRotated,
          rotated != opposite,
          reason: 'raw gameplay mapping combines hardware pose and player end',
        );
      }
    }
  });

  testWidgets(
    'live rotated-board state mirrors the physical Play orientation once',
    (WidgetTester tester) async {
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final position = HomeChessPosition.tryParse(
        'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
      )!;
      final physical = HomePhysicalBoardStatus(
        occupiedSquares: position.occupiedSquares,
        rotated: true,
        p1AtOppositeEnd: false,
      );

      await pumpBoard(
        tester,
        value: HomeBoardSnapshot(
          position: position,
          physicalStatus: physical,
          p1PieceWheel: 227,
          p2PieceWheel: 191,
        ),
        size: 320,
      );

      expect(physical.squareRotated, isTrue);
      expect(
        tester.getCenter(find.byKey(const ValueKey<String>('piece-e1'))).dy,
        greaterThan(
          tester.getCenter(find.byKey(const ValueKey<String>('piece-e8'))).dy,
        ),
        reason: 'Play must present BOARD_ROTATE exactly once',
      );
      expect(
        find.bySemanticsLabel('e1, P1 king'),
        findsOneWidget,
        reason: 'screen orientation must not swap logical P1/P2 ownership',
      );
      expect(find.bySemanticsLabel('e8, P2 king'), findsOneWidget);
    },
  );

  testWidgets('stages Fresh Setup removals before missing-piece ghosts', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();
    final HomeChessPosition opening = HomeChessPosition.tryParse(
      'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1',
    )!;
    final HomeBoardSnapshot leftovers = HomeBoardSnapshot(
      position: opening,
      freshSetup: true,
      physicalStatus: HomePhysicalBoardStatus(
        occupiedSquares: BigInt.parse('8386040308882780', radix: 16),
        placementMode: true,
      ),
    );

    await pumpBoard(tester, value: leftovers, size: 320);
    expect(
      find.byKey(const ValueKey<String>('unexpected-occupied-c3')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('expected-missing-a8')),
      findsNothing,
    );
    expect(
      find.bySemanticsLabel('Remove leftover piece from c3'),
      findsOneWidget,
    );

    await pumpBoard(
      tester,
      value: leftovers.copyWith(
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: opening.occupiedSquares & ~BigInt.one,
          placementMode: true,
        ),
      ),
      size: 320,
    );
    expect(
      find.byKey(const ValueKey<String>('unexpected-occupied-c3')),
      findsNothing,
    );
    expect(
      find.byKey(const ValueKey<String>('expected-missing-a8')),
      findsOneWidget,
    );

    await pumpBoard(
      tester,
      value: leftovers.copyWith(
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: opening.occupiedSquares,
          placementMode: true,
        ),
      ),
      size: 320,
    );
    expect(
      find.bySemanticsLabel('New game setup. Checking the starting position.'),
      findsOneWidget,
    );
    semantics.dispose();
  });

  testWidgets('piece announcements keep player identity through end swaps', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final semantics = tester.ensureSemantics();
    for (final rotated in <bool>[false, true]) {
      for (final oppositeEnd in <bool>[false, true]) {
        final value = snapshot().copyWith(
          physicalStatus: HomePhysicalBoardStatus(
            rotated: rotated,
            p1AtOppositeEnd: oppositeEnd,
          ),
        );
        await pumpBoard(tester, value: value, size: 320);
        for (final entry in expectedPieces.entries) {
          final player = entry.key.endsWith('8') ? 'P1' : 'P2';
          expect(
            find.bySemanticsLabel(
              '${oppositeEnd ? algebraicSquare(63 - tryParseAlgebraicSquare(entry.key)!) : entry.key}, $player ${entry.value}',
            ),
            findsOneWidget,
          );
        }
        expect(value.playerForSide(HomeSide.white), 'P1');
        expect(value.playerForSide(HomeSide.black), 'P2');
      }
    }
    semantics.dispose();
  });

  test('matches the firmware wheel without inventing RGBW white', () {
    expect(litchWheelToColor(0), const Color(0xFFFF0000));
    expect(litchWheelToColor(85), const Color(0xFF00FF00));
    expect(litchWheelToColor(170), const Color(0xFF0000FF));
    expect(litchWheelToColor(255), const Color(0xFFFF0000));
    expect(litchWheelToColor(121).toARGB32(), 0xff00936c);
    expect(litchWheelToColor(24).toARGB32(), 0xffb74800);
    expect(litchWheelPreviewColor(121).toARGB32(), 0xff00c8ae);
    expect(litchWheelPreviewColor(24).toARGB32(), 0xffdc9100);
    expect(litchWheelPreviewColor(0), const Color(0xFFFF0000));
    expect(litchWheelPreviewColor(85), const Color(0xFF00FF00));
    expect(litchWheelPreviewColor(170), const Color(0xFF0000FF));

    const List<HomePhysicalBoardStatus> physicalOrientations =
        <HomePhysicalBoardStatus>[
          HomePhysicalBoardStatus(rotated: false),
          HomePhysicalBoardStatus(rotated: true),
          HomePhysicalBoardStatus(p1AtOppositeEnd: true, rotated: false),
          HomePhysicalBoardStatus(p1AtOppositeEnd: true, rotated: true),
        ];
    for (final HomePhysicalBoardStatus physicalStatus in physicalOrientations) {
      final HomeBoardSnapshot oriented = HomeBoardSnapshot(
        physicalStatus: physicalStatus,
        p1PieceWheel: 170,
        p2PieceWheel: 0,
      );
      expect(
        litchPieceAccentColor(oriented, whitePiece: true),
        physicalStatus.p1AtOppositeEnd
            ? const Color(0xFFFF0000)
            : const Color(0xFF0000FF),
        reason:
            'P1 selects its assigned physical end hue for '
            'p1AtOppositeEnd=${physicalStatus.p1AtOppositeEnd}, '
            'rotated=${physicalStatus.rotated}',
      );
      expect(
        litchPieceAccentColor(oriented, whitePiece: false),
        physicalStatus.p1AtOppositeEnd
            ? const Color(0xFF0000FF)
            : const Color(0xFFFF0000),
        reason:
            'P2 selects its assigned physical end hue for '
            'p1AtOppositeEnd=${physicalStatus.p1AtOppositeEnd}, '
            'rotated=${physicalStatus.rotated}',
      );
    }
  });

  testWidgets('fits compact and large boards without clipping artwork', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));

    for (final double size in <double>[288, 430]) {
      await pumpBoard(tester, value: snapshot(), size: size);
      expect(tester.takeException(), isNull);
      expect(tester.getSize(find.byType(GridView)), Size.square(size));

      for (final String square in expectedPieces.keys) {
        final Rect cell = tester.getRect(
          find.byKey(ValueKey<String>('square-$square')),
        );
        final Rect artwork = tester.getRect(
          find.byKey(ValueKey<String>('piece-$square')),
        );
        expect(cell.contains(artwork.topLeft), isTrue);
        expect(
          cell.contains(artwork.bottomRight - const Offset(0.01, 0.01)),
          isTrue,
        );
      }
      final Rect duckCell = tester.getRect(
        find.byKey(const ValueKey<String>('square-h8')),
      );
      final Rect duckMarker = tester.getRect(
        find.byKey(const ValueKey<String>('duck-marker-h8')),
      );
      expect(duckCell.contains(duckMarker.topLeft), isTrue);
      expect(
        duckCell.contains(duckMarker.bottomRight - const Offset(0.01, 0.01)),
        isTrue,
      );
    }

    await tester.binding.setSurfaceSize(const Size(1000, 600));
    await tester.pumpWidget(
      MaterialApp(home: HomeChessboard(snapshot: snapshot(), maxSize: 430)),
    );
    await tester.pumpAndSettle();
    expect(tester.getSize(find.byType(GridView)), const Size.square(430));
    expect(tester.takeException(), isNull);
  });

  testWidgets('distinguishes a known missing piece from unknown occupancy', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();
    final HomeChessPosition position = HomeChessPosition.tryParse(
      '8/8/8/8/8/8/8/6Nk w - - 0 1',
    )!;
    final int g1 = tryParseAlgebraicSquare('g1')!;
    final int f3 = tryParseAlgebraicSquare('f3')!;
    final HomeBoardSnapshot liveLift = HomeBoardSnapshot(
      position: position,
      physicalStatus: HomePhysicalBoardStatus(
        occupiedSquares:
            (position.occupiedSquares & ~(BigInt.one << g1)) |
            (BigInt.one << f3),
      ),
      liveSequence: 4,
      homeStateAction: HomeStateAction.correction,
    );

    await pumpBoard(tester, value: liveLift, size: 320);

    expect(find.bySemanticsLabel('P1 knight expected on g1'), findsOneWidget);
    expect(
      find.bySemanticsLabel('Unexpected occupied square on f3'),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('expected-missing-g1')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey<String>('unexpected-occupied-f3')),
      findsOneWidget,
    );
    expect(find.bySemanticsLabel('h1, P2 king'), findsOneWidget);
    semantics.dispose();
  });

  testWidgets('describes a legal lift as progress instead of correction', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();
    final HomeChessPosition position = HomeChessPosition.tryParse(
      '8/8/8/8/8/8/8/6Nk w - - 0 1',
    )!;
    final int g1 = tryParseAlgebraicSquare('g1')!;

    await pumpBoard(
      tester,
      value: HomeBoardSnapshot(
        position: position,
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: position.occupiedSquares & ~(BigInt.one << g1),
        ),
        liveSequence: 4,
        homeStateAction: HomeStateAction.lift,
      ),
      size: 320,
    );

    expect(find.bySemanticsLabel('P1 knight lifted from g1'), findsOneWidget);
    expect(find.bySemanticsLabel('P1 knight expected on g1'), findsNothing);
    semantics.dispose();
  });

  testWidgets('ships transparent cameos with compact-size visual weight', (
    WidgetTester tester,
  ) async {
    final Map<String, List<double>> opticalSizes = (await tester
        .runAsync<Map<String, List<double>>>(() async {
          final Map<String, List<double>> sizes = <String, List<double>>{};

          for (final String name in assetNames) {
            final String family = name == 'duck' ? 'cameos_v2' : 'cameos_v3';
            final ByteData data = await rootBundle.load(
              'assets/chess_pieces/$family/$name.png',
            );
            final ui.Codec codec = await ui.instantiateImageCodec(
              data.buffer.asUint8List(),
            );
            final ui.FrameInfo frame = await codec.getNextFrame();
            final ui.Image image = frame.image;
            final ByteData rgba = (await image.toByteData(
              format: ui.ImageByteFormat.rawRgba,
            ))!;
            final List<int> bytes = rgba.buffer.asUint8List();

            expect(image.width, greaterThanOrEqualTo(256), reason: name);
            expect(image.height, greaterThanOrEqualTo(256), reason: name);
            expect(
              image.width / image.height,
              inInclusiveRange(0.9, 1.1),
              reason: '$name aspect ratio',
            );
            int alphaAt(int x, int y) => bytes[((y * image.width + x) * 4) + 3];
            expect(alphaAt(0, 0), 0, reason: name);
            expect(alphaAt(image.width - 1, 0), 0, reason: name);
            expect(alphaAt(0, image.height - 1), 0, reason: name);
            expect(alphaAt(image.width - 1, image.height - 1), 0, reason: name);

            int minX = image.width;
            int minY = image.height;
            int maxX = -1;
            int maxY = -1;
            int paintedPixelCount = 0;
            int keyedAccentPixelCount = 0;
            for (
              int pixel = 0, alphaOffset = 3;
              alphaOffset < bytes.length;
              pixel += 1, alphaOffset += 4
            ) {
              if (bytes[alphaOffset] <= 16) {
                continue;
              }
              paintedPixelCount += 1;
              if (bytes[alphaOffset - 3] < 40 &&
                  bytes[alphaOffset - 2] > 200 &&
                  bytes[alphaOffset - 1] < 40) {
                keyedAccentPixelCount += 1;
              }
              final int x = pixel % image.width;
              final int y = pixel ~/ image.width;
              minX = x < minX ? x : minX;
              minY = y < minY ? y : minY;
              maxX = x > maxX ? x : maxX;
              maxY = y > maxY ? y : maxY;
            }
            expect(maxX, greaterThanOrEqualTo(minX), reason: name);
            expect(maxY, greaterThanOrEqualTo(minY), reason: name);

            final double compactScale =
                34 / (image.width > image.height ? image.width : image.height);
            final double compactWidth = (maxX - minX + 1) * compactScale;
            final double compactHeight = (maxY - minY + 1) * compactScale;
            final double compactArea =
                paintedPixelCount * compactScale * compactScale;
            expect(
              compactWidth,
              inInclusiveRange(18, 33.5),
              reason: '$name compact optical width',
            );
            expect(
              compactHeight,
              inInclusiveRange(26, 34),
              reason: '$name compact optical height',
            );
            expect(
              compactArea,
              inInclusiveRange(300, 700),
              reason: '$name compact painted area',
            );
            if (name != 'duck') {
              expect(
                keyedAccentPixelCount,
                greaterThan(1000),
                reason: '$name keyed player-color facets',
              );
            }
            sizes[name] = <double>[compactWidth, compactHeight];

            image.dispose();
            codec.dispose();
          }
          return sizes;
        }))!;

    expect(opticalSizes.keys, containsAll(assetNames));
  });

  testWidgets('matches the checked-in compact, large, and mismatch renders', (
    WidgetTester tester,
  ) async {
    addTearDown(() => tester.binding.setSurfaceSize(null));

    await pumpBoard(tester, value: snapshot(), size: 288);
    await decodeVisibleReviewImages(tester);
    await expectLater(
      find.byKey(const ValueKey<String>('board-capture')),
      matchesGoldenFile(hostGoldenPath('home_chessboard_compact.png')),
    );

    await pumpBoard(tester, value: snapshot(), size: 430);
    await decodeVisibleReviewImages(tester);
    await expectLater(
      find.byKey(const ValueKey<String>('board-capture')),
      matchesGoldenFile(hostGoldenPath('home_chessboard_large.png')),
    );

    await pumpBoard(tester, value: mismatchSnapshot(), size: 288);
    await decodeVisibleReviewImages(tester);
    await expectLater(
      find.byKey(const ValueKey<String>('board-capture')),
      matchesGoldenFile(hostGoldenPath('home_chessboard_mismatch_compact.png')),
    );
  });
}
