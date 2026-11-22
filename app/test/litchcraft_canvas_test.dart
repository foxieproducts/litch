import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';

void main() {
  final haptics = <String>[];
  setUp(() {
    haptics.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(SystemChannels.platform, (call) async {
          if (call.method == 'HapticFeedback.vibrate') {
            haptics.add(call.arguments as String);
          }
          return null;
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(SystemChannels.platform, null);
  });

  testWidgets('canvas hold recognizes once then confirms erase at 500ms', (
    tester,
  ) async {
    late StateSetter rebuild;
    final painted = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Center(
          child: SizedBox.square(
            dimension: 320,
            child: StatefulBuilder(
              builder: (_, setState) {
                rebuild = setState;
                return LitchcraftCanvasView(
                  canvas: LitchcraftCanvas.blank(),
                  onBegin: () {},
                  onPaint: painted.addAll,
                  onEnd: () {},
                  onEraseSquare: (square) => haptics.add('erase-$square'),
                );
              },
            ),
          ),
        ),
      ),
    );
    final rect = tester.getRect(find.byType(LitchcraftCanvasView));
    final pointer = await tester.startGesture(
      rect.topLeft + const Offset(20, 20),
    );
    await tester.pump(const Duration(milliseconds: 99));
    expect(haptics, isEmpty);
    await tester.pump(const Duration(milliseconds: 1));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    await pointer.moveBy(const Offset(2, 1));
    await tester.pump(const Duration(milliseconds: 200));
    rebuild(() {});
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 199));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    await tester.pump(const Duration(milliseconds: 1));
    expect(haptics, [
      'HapticFeedbackType.lightImpact',
      'erase-0',
      'HapticFeedbackType.mediumImpact',
    ]);
    await pointer.up();
    await tester.pump(const Duration(seconds: 1));
    expect(haptics, hasLength(3));
    expect(painted, isEmpty);
  });

  testWidgets('canvas cancellation and source changes cannot confirm a hold', (
    tester,
  ) async {
    var enabled = true;
    var previewEnabled = true;
    var validPreview = true;
    final cancelGestures = ValueNotifier<int>(0);
    addTearDown(cancelGestures.dispose);
    late StateSetter rebuild;
    final erased = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Center(
          child: SizedBox.square(
            dimension: 320,
            child: StatefulBuilder(
              builder: (_, setState) {
                rebuild = setState;
                return LitchcraftCanvasView(
                  canvas: LitchcraftCanvas.blank(),
                  enabled: enabled,
                  cancelGestures: cancelGestures,
                  previewPaint: previewEnabled
                      ? (canvas, square) => validPreview ? canvas : null
                      : null,
                  onBegin: () {},
                  onPaint: (_) {},
                  onEnd: () {},
                  onEraseSquare: erased.add,
                  onRotateQuarterTurn: (_) {},
                );
              },
            ),
          ),
        ),
      ),
    );
    final position =
        tester.getTopLeft(find.byType(LitchcraftCanvasView)) +
        const Offset(20, 20);
    final released = await tester.startGesture(position);
    await tester.pump(const Duration(milliseconds: 100));
    await released.up();
    await tester.pump(const Duration(seconds: 1));
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    haptics.clear();

    final cancelled = await tester.startGesture(position);
    await tester.pump(const Duration(milliseconds: 50));
    await cancelled.cancel();
    await tester.pump(const Duration(seconds: 1));
    expect(haptics, isEmpty);

    final drag = await tester.startGesture(position);
    await tester.pump(const Duration(milliseconds: 50));
    await drag.moveBy(const Offset(20, 0));
    await tester.pump(const Duration(milliseconds: 250));
    await drag.moveBy(const Offset(40, 0));
    await drag.up();
    await tester.pump(const Duration(seconds: 1));
    expect(haptics, isEmpty);

    rebuild(() => previewEnabled = false);
    await tester.pump();
    final disabled = await tester.startGesture(position);
    await tester.pump(const Duration(milliseconds: 100));
    rebuild(() => enabled = false);
    await tester.pump();
    rebuild(() => enabled = true);
    await tester.pump(const Duration(seconds: 1));
    await disabled.up();
    await tester.pump();
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    haptics.clear();

    rebuild(() => previewEnabled = true);
    await tester.pump();
    final invalidated = await tester.startGesture(position);
    await tester.pump(const Duration(milliseconds: 50));
    // The source can change before Flutter rebuilds the pointer's canvas.
    validPreview = false;
    await tester.pump(const Duration(milliseconds: 600));
    await invalidated.up();
    await tester.pump();
    expect(haptics, isEmpty);
    validPreview = true;

    final externalCancel = await tester.startGesture(position);
    await tester.pump(const Duration(milliseconds: 100));
    cancelGestures.value++;
    await tester.pump(const Duration(seconds: 1));
    await externalCancel.up();
    await tester.pump();
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    haptics.clear();
    final rotating = await tester.startGesture(position, pointer: 21);
    await tester.pump(const Duration(milliseconds: 100));
    final second = await tester.startGesture(
      position + const Offset(100, 100),
      pointer: 22,
    );
    await tester.pump(const Duration(seconds: 1));
    await rotating.cancel();
    await second.cancel();
    await tester.pump();
    expect(haptics, ['HapticFeedbackType.lightImpact']);
    expect(erased, isEmpty);
  });

  for (final flipped in [false, true]) {
    testWidgets(
      'effect marker preserves painting and square semantics flipped=$flipped',
      (tester) async {
        final painted = <int>[];
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: Center(
                child: SizedBox.square(
                  dimension: 320,
                  child: LitchcraftCanvasView(
                    canvas: LitchcraftCanvas.blank(),
                    flipped: flipped,
                    effectLabels: const {9: 'Breathe'},
                    onBegin: () {},
                    onEnd: () {},
                    onPaint: painted.addAll,
                  ),
                ),
              ),
            ),
          ),
        );
        final square = find.byKey(const ValueKey('craft-square-9'));
        final marker = find.byKey(const ValueKey('craft-effect-marker-9'));
        expect(marker, findsOneWidget);
        expect(find.descendant(of: square, matching: marker), findsOneWidget);
        expect(tester.widget<Semantics>(square).properties.value, 'Breathe');
        expect(tester.getSize(marker).longestSide, lessThanOrEqualTo(12));
        await tester.tapAt(tester.getCenter(marker));
        await tester.pump();
        expect(painted, contains(9));
        expect(tester.takeException(), isNull);
      },
    );
  }
  testWidgets('shared artwork renders as a useful full-size PNG', (
    tester,
  ) async {
    final canvas = LitchcraftCanvas.blank().paint(<int>[0, 63], 15, 0xff4020);
    const preview = ValueKey('litchcraft-display-preview');
    await tester.pumpWidget(
      Center(
        child: SizedBox.square(
          dimension: 64,
          child: RepaintBoundary(
            key: preview,
            child: LitchcraftThumbnail(canvas: canvas),
          ),
        ),
      ),
    );
    late List<int> signature;
    late int width, height;
    late List<int> previewPixel, exportPixel;
    await tester.runAsync(() async {
      Future<List<int>> pixel(ui.Image image, int x, int y) async {
        final data = (await image.toByteData(
          format: ui.ImageByteFormat.rawRgba,
        ))!;
        final offset = (y * image.width + x) * 4;
        return data.buffer.asUint8List(data.offsetInBytes + offset, 4).toList();
      }

      final boundary = tester.renderObject<RenderRepaintBoundary>(
        find.byKey(preview),
      );
      final rendered = await boundary.toImage();
      previewPixel = await pixel(rendered, 4, 4);
      rendered.dispose();
      final bytes = await renderLitchcraftArtworkPng(canvas);
      signature = bytes.take(8).toList();
      final codec = await ui.instantiateImageCodec(bytes);
      final frame = await codec.getNextFrame();
      width = frame.image.width;
      height = frame.image.height;
      exportPixel = await pixel(frame.image, 64, 64);
      frame.image.dispose();
      codec.dispose();
    });
    expect(signature, <int>[137, 80, 78, 71, 13, 10, 26, 10]);
    expect(width, 1024);
    expect(height, 1024);
    expect(previewPixel, [255, 137, 99, 255]);
    expect(exportPixel, [255, 64, 32, 255]);
    expect(canvas.colors.first, 0xff4020);
  });

  test(
    'legacy center preview matches rounded physical top/bottom average without flattening source',
    () {
      expect(litchcraftLegacyCenterColor(0xff0000, 0x0000ff), 0x800080);
      expect(litchcraftLegacyCenterColor(0xff0000, 0), 0x800000);
      expect(litchcraftLegacyCenterColor(0x010203, 0x020304), 0x020304);
      final source = LitchcraftCanvas.blank(
        topology: LitchcraftTopology.v1,
      ).paint(<int>[7], 2, 0xff0000).paint(<int>[7], 8, 0x0000ff);
      litchcraftLegacyCenterColor(
        source.colors[7 * 4 + 1],
        source.colors[7 * 4 + 3],
      );
      expect(source.colors[7 * 4 + 1], 0xff0000);
      expect(source.colors[7 * 4 + 3], 0x0000ff);
    },
  );

  test(
    'fast sparse segments cover every crossed square in either direction',
    () {
      expect(
        litchcraftSquaresAlongSegment(
          const Offset(1, 25),
          const Offset(399, 25),
          const Size.square(400),
        ),
        List<int>.generate(8, (i) => i),
      );
      expect(
        litchcraftSquaresAlongSegment(
          const Offset(399, 25),
          const Offset(1, 25),
          const Size.square(400),
        ),
        List<int>.generate(8, (i) => 7 - i),
      );
      expect(
        litchcraftSquaresAlongSegment(
          const Offset(1, 1),
          const Offset(399, 399),
          const Size.square(400),
        ),
        List<int>.generate(8, (i) => i * 9),
      );
    },
  );

  test(
    'all square addresses and asymmetric corners obey the Home orientation',
    () {
      for (var square = 0; square < 64; square++) {
        final point = Offset(square % 8 * 50 + 25, square ~/ 8 * 50 + 25);
        expect(
          litchcraftSquaresAlongSegment(point, point, const Size.square(400)),
          <int>[square],
        );
        expect(
          litchcraftSquaresAlongSegment(
            point,
            point,
            const Size.square(400),
            flipped: true,
          ),
          <int>[63 - square],
        );
      }
    },
  );

  test('twist threshold requests only one artwork turn direction', () {
    expect(litchcraftQuarterTurnDirectionForAngle(math.pi / 4), 1);
    expect(litchcraftQuarterTurnDirectionForAngle(-math.pi / 4), -1);
    expect(litchcraftQuarterTurnDirectionForAngle(5 * math.pi / 4), 1);
    expect(litchcraftQuarterTurnDirectionForAngle(-5 * math.pi / 4), -1);
    expect(litchcraftQuarterTurnDirectionForAngle(math.pi / 4 - 0.0001), 0);
  });

  test('artwork rotation moves squares and their emitter regions together', () {
    final colors = List<int>.filled(256, 0)
      ..setRange(0, 4, const <int>[0x110000, 0x002200, 0x000033, 0x444444]);
    final canvas = LitchcraftCanvas(colors: colors);

    final clockwise = canvas.rotated(1);
    expect(clockwise.colors.take(4), everyElement(0));
    expect(clockwise.colors.skip(7 * 4).take(4), <int>[
      0x444444,
      0x110000,
      0x002200,
      0x000033,
    ]);
    expect(clockwise.rotated(3), canvas);
    expect(canvas.rotated(-1), clockwise.rotated(2));
    expect(canvas.rotated(4), same(canvas));
  });

  testWidgets(
    'gesture starting inside canvas owns fast drag across PageView and async rebuilds',
    (tester) async {
      final pages = PageController(initialPage: 1);
      final rebuild = ValueNotifier<int>(0);
      var began = 0, ended = 0;
      final painted = <int>{};
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: PageView(
              controller: pages,
              children: <Widget>[
                const Text('Left page'),
                Center(
                  child: SizedBox(
                    width: 400,
                    child: ValueListenableBuilder<int>(
                      valueListenable: rebuild,
                      builder: (_, value, _) => LitchcraftCanvasView(
                        canvas: LitchcraftCanvas.blank(),
                        onBegin: () => began++,
                        onPaint: painted.addAll,
                        onEnd: () => ended++,
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ),
      );
      final rect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final gesture = await tester.startGesture(
        rect.topLeft + const Offset(25, 25),
      );
      await gesture.moveTo(rect.topLeft + const Offset(100, 25));
      await tester.pump();
      rebuild.value++;
      await tester.pump();
      await gesture.moveTo(rect.topLeft + const Offset(375, 25));
      await tester.pump();
      await gesture.up();
      await tester.pumpAndSettle();
      expect(pages.page, 1);
      expect(painted, Set<int>.from(List.generate(8, (i) => i)));
      expect(began, 1);
      expect(ended, 1);
      await tester.pumpWidget(const SizedBox());
      pages.dispose();
      rebuild.dispose();
    },
  );

  testWidgets('tap is one stroke and second finger does not draw', (
    tester,
  ) async {
    var began = 0, ended = 0;
    final painted = <int>{};
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: SizedBox(
            width: 400,
            child: LitchcraftCanvasView(
              canvas: LitchcraftCanvas.blank(),
              onBegin: () => began++,
              onPaint: painted.addAll,
              onEnd: () => ended++,
            ),
          ),
        ),
      ),
    );
    final rect = tester.getRect(
      find.byKey(const ValueKey('litchcraft-canvas')),
    );
    final first = await tester.startGesture(
      rect.topLeft + const Offset(25, 25),
      pointer: 1,
    );
    // A stationary first finger remains undecided until release so a second
    // finger can turn the gesture into a rotation without painting.
    expect(painted, isEmpty);
    expect(began, 0);
    expect(ended, 0);
    final second = await tester.startGesture(
      rect.topLeft + const Offset(375, 375),
      pointer: 2,
    );
    await second.up();
    await first.up();
    await tester.pump();
    expect(painted, <int>{0});
    expect(began, 1);
    expect(ended, 1);
  });

  testWidgets(
    'dragging beyond every canvas edge never emits an invalid square',
    (tester) async {
      final painted = <int>[];
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Center(
              child: SizedBox(
                width: 320,
                child: LitchcraftCanvasView(
                  canvas: LitchcraftCanvas.blank(),
                  onBegin: () {},
                  onPaint: painted.addAll,
                  onEnd: () {},
                ),
              ),
            ),
          ),
        ),
      );
      final rect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final pointer = await tester.startGesture(rect.center);
      for (final position in <Offset>[
        rect.topLeft - const Offset(1000, 1000),
        rect.topRight + const Offset(1000, -1000),
        rect.bottomRight + const Offset(1000, 1000),
        rect.bottomLeft + const Offset(-1000, 1000),
        rect.center,
      ]) {
        await pointer.moveTo(position);
        await tester.pump();
      }
      await pointer.up();
      await tester.pump();

      expect(tester.takeException(), isNull);
      expect(painted, isNotEmpty);
      expect(painted, everyElement(inInclusiveRange(0, 63)));
    },
  );

  for (final cancel in <bool>[false, true]) {
    testWidgets(
      'one-shot inspection owns gesture through rebuild and ${cancel ? 'cancel' : 'release'} without painting',
      (tester) async {
        final pages = PageController(initialPage: 1);
        final inspecting = ValueNotifier<bool>(true);
        var began = 0, ended = 0;
        final painted = <int>{};
        final inspected = <int>[];
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: PageView(
                controller: pages,
                children: <Widget>[
                  const Text('Left page'),
                  Center(
                    child: SizedBox(
                      width: 400,
                      child: ValueListenableBuilder<bool>(
                        valueListenable: inspecting,
                        builder: (_, active, _) => LitchcraftCanvasView(
                          canvas: LitchcraftCanvas.blank(),
                          flipped: true,
                          onBegin: () => began++,
                          onPaint: painted.addAll,
                          onEnd: () => ended++,
                          onInspectSquare: active
                              ? (square) {
                                  inspected.add(square);
                                  inspecting.value = false;
                                }
                              : null,
                          inspectLabel: 'Inspect square',
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        );
        final rect = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final first = await tester.startGesture(
          rect.topLeft + const Offset(25, 25),
          pointer: 1,
        );
        expect(inspected, <int>[63]);
        expect(began, 0);
        expect(painted, isEmpty);
        await tester.pump();
        final second = await tester.startGesture(
          rect.topLeft + const Offset(375, 375),
          pointer: 2,
        );
        await second.up();
        await first.moveTo(rect.topLeft + const Offset(100, 25));
        await tester.pump();
        await first.moveTo(rect.topLeft + const Offset(375, 25));
        await tester.pump();
        if (cancel) {
          await first.cancel();
        } else {
          await first.up();
        }
        await tester.pumpAndSettle();
        expect(pages.page, 1);
        expect(inspected, <int>[63]);
        expect(painted, isEmpty);
        expect(began, 0);
        expect(ended, 0);

        final next = await tester.startGesture(
          rect.topLeft + const Offset(25, 25),
        );
        expect(painted, isEmpty);
        expect(began, 0);
        await next.up();
        await tester.pump();
        expect(painted, <int>{63});
        expect(began, 1);
        expect(ended, 1);
        await tester.pumpWidget(const SizedBox());
        pages.dispose();
        inspecting.dispose();
      },
    );
  }

  testWidgets('inspection semantics select square and respect disabled state', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();
    try {
      final strings = lookupAppLocalizations(const Locale('en'));
      var inspecting = true, enabled = true, erasing = false;
      var began = 0, ended = 0;
      final painted = <int>{};
      final erased = <int>{};
      final inspected = <int>[];
      late StateSetter rebuild;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SizedBox(
              width: 400,
              child: StatefulBuilder(
                builder: (_, setState) {
                  rebuild = setState;
                  return LitchcraftCanvasView(
                    canvas: LitchcraftCanvas.blank(),
                    enabled: enabled,
                    onBegin: () => began++,
                    onPaint: erasing ? erased.addAll : painted.addAll,
                    onEnd: () => ended++,
                    onInspectSquare: inspecting ? inspected.add : null,
                    inspectLabel: 'Inspect square',
                    paintLabel: erasing ? strings.craftErase : null,
                  );
                },
              ),
            ),
          ),
        ),
      );
      final square = find.byKey(const ValueKey('craft-square-7'));
      final inspectingNode = tester.getSemantics(square);
      final squareLabel = strings.craftSquareSemantics('h8', strings.craftOff);
      expect(inspectingNode.getSemanticsData().label, squareLabel);
      expect(tester.widget<Semantics>(square).properties.enabled, isTrue);
      expect(
        inspectingNode.getSemanticsData().hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      expect(inspectingNode.hintOverrides?.onTapHint, 'Inspect square');
      inspectingNode.owner!.performAction(
        inspectingNode.id,
        ui.SemanticsAction.tap,
      );
      expect(inspected, <int>[7]);
      expect(painted, isEmpty);
      expect(began, 0);
      expect(ended, 0);

      rebuild(() => enabled = false);
      await tester.pump();
      final disabledNode = tester.getSemantics(square);
      expect(disabledNode.getSemanticsData().label, squareLabel);
      expect(disabledNode.hintOverrides?.onTapHint, isNull);
      expect(tester.widget<Semantics>(square).properties.enabled, isFalse);
      expect(
        tester
            .getSemantics(square)
            .getSemanticsData()
            .hasAction(ui.SemanticsAction.tap),
        isFalse,
      );
      await tester.tapAt(tester.getCenter(square));
      expect(inspected, <int>[7]);
      expect(painted, isEmpty);

      rebuild(() {
        enabled = true;
        inspecting = false;
      });
      await tester.pump();
      final paintingNode = tester.getSemantics(square);
      expect(paintingNode.getSemanticsData().label, squareLabel);
      expect(paintingNode.hintOverrides?.onTapHint, strings.craftPaint);
      expect(tester.widget<Semantics>(square).properties.enabled, isTrue);
      expect(
        paintingNode.getSemanticsData().hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      paintingNode.owner!.performAction(
        paintingNode.id,
        ui.SemanticsAction.tap,
      );
      expect(inspected, <int>[7]);
      expect(painted, <int>{7});
      expect(began, 1);
      expect(ended, 1);
      expect(erased, isEmpty);

      rebuild(() => erasing = true);
      await tester.pump();
      final erasingNode = tester.getSemantics(square);
      expect(erasingNode.getSemanticsData().label, squareLabel);
      expect(erasingNode.hintOverrides?.onTapHint, strings.craftErase);
      expect(tester.widget<Semantics>(square).properties.enabled, isTrue);
      expect(
        erasingNode.getSemanticsData().hasAction(ui.SemanticsAction.tap),
        isTrue,
      );
      erasingNode.owner!.performAction(erasingNode.id, ui.SemanticsAction.tap);
      expect(inspected, <int>[7]);
      expect(painted, <int>{7});
      expect(erased, <int>{7});
      expect(began, 2);
      expect(ended, 2);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets(
    'multicolor square semantics do not announce one arbitrary emitter',
    (tester) async {
      final semantics = tester.ensureSemantics();
      final canvas = LitchcraftCanvas.blank()
          .paint(<int>[7], 1, 0xff0000)
          .paint(<int>[7], 2, 0x0000ff);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SizedBox.square(
              dimension: 400,
              child: LitchcraftCanvasView(
                canvas: canvas,
                onBegin: () {},
                onPaint: (_) {},
                onEnd: () {},
              ),
            ),
          ),
        ),
      );
      expect(
        tester
            .getSemantics(find.byKey(const ValueKey('craft-square-7')))
            .getSemanticsData()
            .label,
        contains('Multiple colors'),
      );
      semantics.dispose();
    },
  );
}
