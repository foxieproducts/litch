import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';

import 'health_render_capture.dart';

const _captureKey = ValueKey<String>('amber-board-capture');
const _guided = <String>['c5', 'd5', 'c4', 'd4'];

HomeBoardSnapshot _correction({bool rotated = false, bool opposite = false}) {
  final position = HomeChessPosition.tryParse('8/8/8/2NK4/8/8/8/8 w - - 0 1')!;
  return HomeBoardSnapshot(
    position: position,
    homeStateAction: HomeStateAction.correction,
    physicalStatus: HomePhysicalBoardStatus(
      occupiedSquares: (BigInt.one << 34) | (BigInt.one << 35),
      rotated: rotated,
      p1AtOppositeEnd: opposite,
    ),
  );
}

Future<void> _pumpBoard(
  WidgetTester tester,
  HomeBoardSnapshot value, {
  bool available = true,
  bool reduceMotion = false,
  bool tickers = true,
  int? selected,
  Set<int> destinations = const <int>{},
}) async {
  await tester.pumpWidget(
    MaterialApp(
      debugShowCheckedModeBanner: false,
      home: MediaQuery(
        data: MediaQueryData(
          disableAnimations: reduceMotion,
          textScaler: TextScaler.linear(2),
        ),
        child: TickerMode(
          enabled: tickers,
          child: Center(
            child: RepaintBoundary(
              key: _captureKey,
              child: SizedBox.square(
                dimension: 320,
                child: HomeChessboard(
                  snapshot: value,
                  maxSize: 320,
                  liveGuidanceAvailable: available,
                  selectedSquare: selected,
                  legalDestinations: destinations,
                ),
              ),
            ),
          ),
        ),
      ),
    ),
  );
  // Decode the production artwork without advancing the animation clock.
  final context = tester.element(find.byType(HomeChessboard));
  final images = tester.widgetList<Image>(find.byType(Image)).toList();
  await tester.runAsync(() async {
    for (final image in images) {
      await precacheImage(image.image, context);
    }
  });
  await tester.pump();
}

class _Frame {
  const _Frame(this.bytes, this.width);
  final Uint8List bytes;
  final int width;

  List<int> pixel(int x, int y) =>
      bytes.sublist((y * width + x) * 4, (y * width + x + 1) * 4);

  bool amber(int x, int y) {
    final color = pixel(x, y);
    return color[0] > 150 && color[1] > 100 && color[2] < 90;
  }
}

Future<_Frame> _frame(WidgetTester tester) async {
  final boundary = tester.renderObject<RenderRepaintBoundary>(
    find.byKey(_captureKey),
  );
  return (await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 1);
    final data = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
    final result = _Frame(data!.buffer.asUint8List(), image.width);
    image.dispose();
    return result;
  }))!;
}

Rect _square(WidgetTester tester, String name) => tester
    .getRect(find.byKey(ValueKey<String>('square-$name')))
    .shift(-tester.getTopLeft(find.byKey(_captureKey)));

List<int> _borderPixel(WidgetTester tester, _Frame frame, String name) {
  final square = _square(tester, name);
  return frame.pixel(square.center.dx.toInt(), square.top.toInt() + 1);
}

Future<void> _capture(WidgetTester tester, String name) =>
    captureHealthReview(tester, name, boundaryFinder: find.byKey(_captureKey));

void main() {
  testWidgets('amber correction chamfers both colors in either board pose', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(360, 360));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    for (final opposite in <bool>[false, true]) {
      for (final rotated in <bool>[false, true]) {
        final value = _correction(rotated: rotated, opposite: opposite);
        await _pumpBoard(tester, value);
        final frame = await _frame(tester);
        await _capture(tester, 'amber-pose-$opposite-$rotated');
        for (final logicalName in _guided) {
          final logical = tryParseAlgebraicSquare(logicalName)!;
          final name = algebraicSquare(opposite ? 63 - logical : logical);
          final square = _square(tester, name);
          final left = square.left.toInt();
          final top = square.top.toInt();
          final right = square.right.toInt() - 1;
          final bottom = square.bottom.toInt() - 1;
          expect(frame.amber(square.center.dx.toInt(), top + 1), isTrue);
          for (final corner in <(int, int)>[
            (left, top),
            (right, top),
            (left, bottom),
            (right, bottom),
          ]) {
            expect(
              frame.amber(corner.$1, corner.$2),
              isFalse,
              reason:
                  '$name clips amber at every existing chamfer corner '
                  '(opposite=$opposite rotated=$rotated)',
            );
          }
        }
        expect(tester.takeException(), isNull);
      }
    }
  });

  testWidgets('amber border pulses gently while contents and semantics stay put', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(360, 360));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final semantics = tester.ensureSemantics();
    try {
      final value = _correction();
      await _pumpBoard(tester, value);
      final peak = await _frame(tester);
      final piece = tester.widget(find.byKey(const ValueKey('piece-c5')));
      final dot = tester.widget(
        find.byKey(const ValueKey('unexpected-occupied-d4')),
      );
      final label = tester
          .getSemantics(find.byKey(const ValueKey('square-c5')))
          .label;
      expect(label, 'P1 knight expected on c5');
      expect(
        tester.getSemantics(find.byKey(const ValueKey('square-d4'))).label,
        'Unexpected occupied square on d4',
      );
      await _capture(tester, 'amber-peak-0ms');

      await tester.pump(const Duration(milliseconds: 600));
      final quarter = await _frame(tester);
      await _capture(tester, 'amber-quarter-600ms');
      expect(
        _borderPixel(tester, quarter, 'c5'),
        isNot(_borderPixel(tester, peak, 'c5')),
        reason: 'the actual amber border must change at an intermediate frame',
      );
      expect(
        tester.widget(find.byKey(const ValueKey('piece-c5'))),
        same(piece),
      );
      expect(
        tester.widget(find.byKey(const ValueKey('unexpected-occupied-d4'))),
        same(dot),
      );
      await _pumpBoard(tester, value.copyWith(liveSequence: 2));
      expect(
        (await _frame(tester)).bytes,
        quarter.bytes,
        reason: 'ordinary snapshot rebuilds retain the pulse phase',
      );

      await tester.pump(const Duration(milliseconds: 600));
      final trough = await _frame(tester);
      await _capture(tester, 'amber-trough-1200ms');
      for (final name in _guided) {
        final square = _square(tester, name);
        final x = square.center.dx.toInt();
        final y = square.top.toInt() + 1;
        final background = name == 'c5' || name == 'd4'
            ? const <int>[82, 105, 128]
            : const <int>[232, 238, 243];
        final colors = <List<int>>[
          peak.pixel(x, y),
          quarter.pixel(x, y),
          trough.pixel(x, y),
        ];
        final channel = background[0] == 82 ? 0 : 2;
        final amber = channel == 0 ? 255 : 0;
        final opacity = colors
            .map(
              (color) =>
                  (color[channel] - background[channel]) /
                  (amber - background[channel]),
            )
            .toList();
        expect(opacity[0], closeTo(1, .02));
        expect(opacity[1], closeTo(.86, .02));
        expect(opacity[2], closeTo(.72, .02));
        expect(trough.amber(x, y), isTrue, reason: '$name remains readable');
      }
      // The rendered delta stays inside the guidance borders. This also checks
      // steady ghost artwork, unknown dots, ordinary squares and the board frame.
      final guidedRects = _guided.map((name) => _square(tester, name)).toList();
      for (int y = 0; y < 320; y++) {
        for (int x = 0; x < 320; x++) {
          final border = guidedRects.any((rect) {
            return rect.contains(Offset(x + .5, y + .5)) &&
                !rect.deflate(3).contains(Offset(x + .5, y + .5));
          });
          if (!border) expect(trough.pixel(x, y), peak.pixel(x, y));
        }
      }
      expect(
        tester.getSemantics(find.byKey(const ValueKey('square-c5'))).label,
        label,
      );
      await tester.pump(const Duration(milliseconds: 600));
      expect((await _frame(tester)).bytes, quarter.bytes);
      await tester.pump(const Duration(milliseconds: 600));
      expect((await _frame(tester)).bytes, peak.bytes);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'amber pulse stops for cleared, offline, reduced and hidden guidance',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(360, 360));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final value = _correction();
      await _pumpBoard(tester, value);
      await tester.pump(const Duration(milliseconds: 600));
      final held = await _frame(tester);
      await _pumpBoard(tester, value, tickers: false);
      await tester.pump(const Duration(seconds: 3));
      expect(tester.binding.hasScheduledFrame, isFalse);
      expect((await _frame(tester)).bytes, held.bytes);
      await _pumpBoard(tester, value);
      expect((await _frame(tester)).bytes, held.bytes);

      await _pumpBoard(tester, value, reduceMotion: true);
      final still = await _frame(tester);
      expect(_borderPixel(tester, still, 'c5'), <int>[255, 179, 0, 255]);
      await tester.pump(const Duration(seconds: 3));
      expect(tester.binding.hasScheduledFrame, isFalse);
      expect((await _frame(tester)).bytes, still.bytes);

      await _pumpBoard(tester, value, available: false);
      final offline = await _frame(tester);
      expect(find.byKey(const ValueKey('expected-missing-c5')), findsNothing);
      await tester.pump(const Duration(seconds: 3));
      expect(tester.binding.hasScheduledFrame, isFalse);
      expect((await _frame(tester)).bytes, offline.bytes);
      await _pumpBoard(tester, value);
      expect(_borderPixel(tester, await _frame(tester), 'c5'), <int>[
        255,
        179,
        0,
        255,
      ]);

      final corrected = value.copyWith(
        physicalStatus: HomePhysicalBoardStatus(
          occupiedSquares: value.expectedOccupiedSquares,
        ),
      );
      await _pumpBoard(tester, corrected);
      final clear = await _frame(tester);
      await tester.pump(const Duration(seconds: 3));
      expect(tester.binding.hasScheduledFrame, isFalse);
      expect((await _frame(tester)).bytes, clear.bytes);
    },
  );

  testWidgets(
    'selected, legal destination and positive move guidance stay steady',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(360, 360));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final value = _correction();
      await _pumpBoard(
        tester,
        value,
        selected: 26,
        destinations: const <int>{27},
      );
      final first = await _frame(tester);
      await tester.pump(const Duration(milliseconds: 1200));
      final later = await _frame(tester);
      for (final name in <String>['c5', 'd5']) {
        expect(
          _borderPixel(tester, later, name),
          _borderPixel(tester, first, name),
        );
      }
      for (final action in <HomeStateAction>[
        HomeStateAction.lift,
        HomeStateAction.cpu,
      ]) {
        await _pumpBoard(tester, value.copyWith(homeStateAction: action));
        final progress = await _frame(tester);
        expect(_borderPixel(tester, progress, 'c5'), <int>[58, 125, 82, 255]);
        await tester.pump(const Duration(milliseconds: 1200));
        expect(tester.binding.hasScheduledFrame, isFalse);
        expect((await _frame(tester)).bytes, progress.bytes);
      }
      await _pumpBoard(
        tester,
        value.copyWith(
          homeStateAction: HomeStateAction.cpu,
          physicalMovePrompt: const HomePhysicalMovePrompt(
            fromSquare: 26,
            toSquare: 27,
          ),
        ),
      );
      final prompt = await _frame(tester);
      expect(_borderPixel(tester, prompt, 'c5'), <int>[47, 97, 115, 255]);
      expect(find.byKey(const ValueKey('cpu-move-from-c5')), findsOneWidget);
      expect(find.byKey(const ValueKey('cpu-move-to-d5')), findsOneWidget);
      await tester.pump(const Duration(milliseconds: 1200));
      expect((await _frame(tester)).bytes, prompt.bytes);
    },
  );
}
