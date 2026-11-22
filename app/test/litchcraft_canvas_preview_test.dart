import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';

const _captureKey = ValueKey('preview-capture');
const _canvasKey = ValueKey('litchcraft-canvas');

Future<int> _pixel(WidgetTester tester, Offset local) async {
  late int rgb;
  await tester.runAsync(() async {
    final boundary = tester.renderObject<RenderRepaintBoundary>(
      find.byKey(_captureKey),
    );
    final image = await boundary.toImage(pixelRatio: 1);
    final data = (await image.toByteData(format: ui.ImageByteFormat.rawRgba))!;
    final offset = (local.dy.floor() * image.width + local.dx.floor()) * 4;
    rgb =
        data.getUint8(offset) << 16 |
        data.getUint8(offset + 1) << 8 |
        data.getUint8(offset + 2);
    image.dispose();
  });
  return rgb;
}

LitchcraftCanvas _rendered(WidgetTester tester) =>
    tester.widget<LitchcraftThumbnail>(find.byType(LitchcraftThumbnail)).canvas;

Future<void> _mount(
  WidgetTester tester, {
  required LitchcraftCanvas canvas,
  required LitchcraftCanvas? Function(LitchcraftCanvas, int) previewPaint,
  required VoidCallback onBegin,
  required ValueChanged<Iterable<int>> onPaint,
  required VoidCallback onEnd,
  ValueChanged<int>? onErase,
  ValueChanged<int>? onRotate,
  bool flipped = false,
  Listenable? cancelGestures,
}) => tester.pumpWidget(
  MaterialApp(
    home: Scaffold(
      body: Center(
        child: RepaintBoundary(
          key: _captureKey,
          child: SizedBox.square(
            dimension: 320,
            child: LitchcraftCanvasView(
              canvas: canvas,
              previewPaint: previewPaint,
              flipped: flipped,
              onBegin: onBegin,
              onPaint: onPaint,
              onEnd: onEnd,
              onEraseSquare: onErase,
              onRotateQuarterTurn: onRotate,
              cancelGestures: cancelGestures,
            ),
          ),
        ),
      ),
    ),
  ),
);

void main() {
  for (final flipped in [false, true]) {
    testWidgets(
      'primary down renders masked brush immediately then sparse drag owns one stroke flipped=$flipped',
      (tester) async {
        final base = LitchcraftCanvas.blank();
        var begins = 0, ends = 0;
        final painted = <int>[];
        await _mount(
          tester,
          canvas: base,
          flipped: flipped,
          previewPaint: (canvas, square) => canvas.paint([square], 1, 0xff4020),
          onBegin: () => begins++,
          onPaint: painted.addAll,
          onEnd: () => ends++,
        );
        final rect = tester.getRect(find.byKey(_canvasKey));
        final pointer = await tester.startGesture(
          rect.topLeft + const Offset(20, 20),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final square = flipped ? 63 : 0;
        expect(
          await _pixel(tester, Offset(flipped ? 35 : 5, 20)),
          litchcraftPreviewColor(0xff4020).toARGB32() & 0xffffff,
        );
        expect(_rendered(tester).colors[square * 4], 0xff4020);
        expect(_rendered(tester).colors[square * 4 + 1], 0);
        expect(base.colors, everyElement(0));
        expect(begins, 0);
        expect(painted, isEmpty);
        expect(ends, 0);
        await pointer.moveTo(rect.topLeft + const Offset(302, 20));
        await tester.pump(const Duration(milliseconds: 16));
        expect(
          painted.toSet(),
          flipped
              ? {for (var n = 56; n <= 63; n++) n}
              : {for (var n = 0; n < 8; n++) n},
        );
        expect(begins, 1);
        expect(ends, 0);
        expect(
          _rendered(tester).colors,
          base.colors,
          reason: 'Committed display belongs to the parent document again.',
        );
        await pointer.up();
        await tester.pump();
        expect(ends, 1);
        await tester.pumpWidget(const SizedBox());
      },
    );
  }

  testWidgets(
    'same-cell jitter retains preview and long hold resolves only as erase',
    (tester) async {
      final base = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
      var begins = 0, ends = 0;
      final painted = <int>[], erased = <int>[];
      await _mount(
        tester,
        canvas: base,
        previewPaint: (canvas, square) => canvas.paint([square], 15, 0xff4020),
        onBegin: () => begins++,
        onPaint: painted.addAll,
        onEnd: () => ends++,
        onErase: erased.add,
      );
      final rect = tester.getRect(find.byKey(_canvasKey));
      final pointer = await tester.startGesture(
        rect.topLeft + const Offset(20, 20),
      );
      await tester.pump(const Duration(milliseconds: 16));
      await pointer.moveBy(const Offset(2, 1));
      // Keep the preview at 499 ms, then erase at the standard 500 ms hold.
      await tester.pump(const Duration(milliseconds: 483));
      expect(_rendered(tester).colors.take(4), everyElement(0xff4020));
      expect(begins, 0);
      expect(painted, isEmpty);
      expect(erased, isEmpty);
      await tester.pump(const Duration(milliseconds: 1));
      expect(erased, [0]);
      expect(_rendered(tester).colors, base.colors);
      expect(begins, 0);
      expect(painted, isEmpty);
      expect(ends, 0);
      await pointer.up();
      await tester.pump(const Duration(milliseconds: 300));
      expect(erased, [0]);
      expect(painted, isEmpty);
      await tester.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'second contact removes provisional paint and keeps existing one-turn twist',
    (tester) async {
      final base = LitchcraftCanvas.blank().paint([0], 1, 0x112233);
      var begins = 0, ends = 0;
      final painted = <int>[], erased = <int>[], turns = <int>[];
      await _mount(
        tester,
        canvas: base,
        previewPaint: (canvas, square) => canvas.paint([square], 15, 0xff4020),
        onBegin: () => begins++,
        onPaint: painted.addAll,
        onEnd: () => ends++,
        onErase: erased.add,
        onRotate: turns.add,
      );
      final rect = tester.getRect(find.byKey(_canvasKey));
      final first = await tester.startGesture(rect.center, pointer: 1);
      await first.moveBy(const Offset(2, 1));
      await tester.pump();
      expect(_rendered(tester).colors[36 * 4], 0xff4020);
      final second = await tester.startGesture(
        rect.center + const Offset(60, 0),
        pointer: 2,
      );
      await tester.pump();
      expect(_rendered(tester).colors, base.colors);
      await second.moveTo(rect.center + const Offset(42, 42));
      await tester.pump();
      await second.moveTo(rect.center + const Offset(0, 60));
      await tester.pump(const Duration(milliseconds: 600));
      expect(turns, [1]);
      expect(begins, 0);
      expect(painted, isEmpty);
      expect(erased, isEmpty);
      await second.up();
      await first.up();
      await tester.pump();
      expect(ends, 0);
      expect(painted, isEmpty);
      await tester.pumpWidget(const SizedBox());
    },
  );

  for (final cancelWithSignal in [false, true]) {
    testWidgets(
      'pending pointer cancellation restores underlying canvas and stale up cannot tap signal=$cancelWithSignal',
      (tester) async {
        final base = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
        final cancellation = ValueNotifier(0);
        var begins = 0, ends = 0;
        final painted = <int>[];
        await _mount(
          tester,
          canvas: base,
          previewPaint: (canvas, square) =>
              canvas.paint([square], 15, 0xff4020),
          onBegin: () => begins++,
          onPaint: painted.addAll,
          onEnd: () => ends++,
          cancelGestures: cancellation,
        );
        final rect = tester.getRect(find.byKey(_canvasKey));
        final pointer = await tester.startGesture(
          rect.topLeft + const Offset(20, 20),
        );
        await tester.pump();
        expect(_rendered(tester).colors.take(4), everyElement(0xff4020));
        if (cancelWithSignal) {
          cancellation.value++;
        } else {
          await pointer.cancel();
        }
        await tester.pump();
        expect(_rendered(tester).colors, base.colors);
        if (cancelWithSignal) await pointer.up();
        await tester.pump();
        expect(begins, 0);
        expect(painted, isEmpty);
        expect(ends, 0);
        await tester.pumpWidget(const SizedBox());
        cancellation.dispose();
      },
    );
  }

  testWidgets(
    'held pointer keeps latched preview callback across harmless asynchronous rebuild',
    (tester) async {
      final base = LitchcraftCanvas.blank();
      var color = 0xff4020, mask = 1;
      late StateSetter rebuild;
      final painted = <int>[];
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Center(
              child: SizedBox.square(
                dimension: 320,
                child: StatefulBuilder(
                  builder: (context, setState) {
                    rebuild = setState;
                    final capturedColor = color, capturedMask = mask;
                    return LitchcraftCanvasView(
                      canvas: base,
                      previewPaint: (canvas, square) =>
                          canvas.paint([square], capturedMask, capturedColor),
                      onBegin: () {},
                      onPaint: painted.addAll,
                      onEnd: () {},
                    );
                  },
                ),
              ),
            ),
          ),
        ),
      );
      final rect = tester.getRect(find.byKey(_canvasKey));
      final pointer = await tester.startGesture(
        rect.topLeft + const Offset(20, 20),
      );
      await tester.pump();
      rebuild(() {
        color = 0x00ff00;
        mask = 2;
      });
      await tester.pump(const Duration(milliseconds: 16));
      expect(_rendered(tester).colors.take(4), [0xff4020, 0, 0, 0]);
      await pointer.cancel();
      await tester.pump();
      expect(painted, isEmpty);
      expect(_rendered(tester).colors, base.colors);
      final next = await tester.startGesture(
        rect.topLeft + const Offset(20, 20),
      );
      await tester.pump();
      expect(_rendered(tester).colors.take(4), [0, 0x00ff00, 0, 0]);
      await next.cancel();
      await tester.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'accepted stroke ends once before existing second-contact twist',
    (tester) async {
      final base = LitchcraftCanvas.blank();
      var begins = 0, ends = 0;
      final painted = <int>[], turns = <int>[];
      await _mount(
        tester,
        canvas: base,
        previewPaint: (canvas, square) => canvas.paint([square], 15, 0xff4020),
        onBegin: () => begins++,
        onPaint: painted.addAll,
        onEnd: () => ends++,
        onRotate: turns.add,
      );
      final rect = tester.getRect(find.byKey(_canvasKey));
      final first = await tester.startGesture(
        rect.center + const Offset(0, 20),
        pointer: 1,
      );
      await first.moveBy(const Offset(20, 0));
      await tester.pump();
      expect(begins, 1);
      expect(painted, isNotEmpty);
      expect(ends, 0);
      final paintCount = painted.length;
      final pivot = rect.center + const Offset(20, 20);
      final second = await tester.startGesture(
        pivot + const Offset(60, 0),
        pointer: 2,
      );
      await tester.pump();
      expect(ends, 1);
      await second.moveTo(pivot + const Offset(42, 42));
      await tester.pump();
      await second.moveTo(pivot + const Offset(0, 60));
      await tester.pump();
      expect(turns, [1]);
      expect(begins, 1);
      expect(painted.length, paintCount);
      await second.up();
      await first.up();
      await tester.pump();
      expect(ends, 1);
      expect(painted.length, paintCount);
      await tester.pumpWidget(const SizedBox());
    },
  );
}
