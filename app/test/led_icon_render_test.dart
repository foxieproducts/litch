import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';

Future<ByteData> _targetPixels(
  WidgetTester tester, {
  required int mask,
  bool flipped = false,
  LitchcraftTopology topology = LitchcraftTopology.v3,
}) async {
  await tester.pumpWidget(
    MaterialApp(
      home: Center(
        child: RepaintBoundary(
          key: const ValueKey('target-glyph'),
          child: LitchcraftTargetIcon(
            mask: mask,
            topology: topology,
            flipped: flipped,
            color: 0xffffff,
            size: 36,
          ),
        ),
      ),
    ),
  );
  await tester.pump();
  final boundary = tester.renderObject<RenderRepaintBoundary>(
    find.byKey(const ValueKey('target-glyph')),
  );
  return (await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 1);
    final bytes = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
    image.dispose();
    return bytes!;
  }))!;
}

void main() {
  testWidgets('target diagrams emphasize the physical edge at icon size', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    const samples = <int, (int, int)>{
      1: (9, 12),
      2: (12, 9),
      4: (27, 12),
      8: (12, 27),
    };
    for (final sample in [
      (1, false, <int>{1}),
      (2, false, <int>{2}),
      (4, false, <int>{4}),
      (8, false, <int>{8}),
      (15, false, <int>{1, 2, 4, 8}),
      (1, true, <int>{4}),
      (2, true, <int>{8}),
      (4, true, <int>{1}),
      (8, true, <int>{2}),
    ]) {
      final pixels = await _targetPixels(
        tester,
        mask: sample.$1,
        flipped: sample.$2,
      );
      for (final edge in samples.entries) {
        final offset = (edge.value.$2 * 36 + edge.value.$1) * 4;
        final red = pixels.getUint8(offset);
        expect(
          red,
          sample.$3.contains(edge.key) ? greaterThan(240) : lessThan(100),
          reason: 'mask ${sample.$1}, flipped ${sample.$2}, edge ${edge.key}',
        );
      }
    }
  });

  testWidgets('legacy target diagram keeps its one center emitter centered', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    for (final flipped in [false, true]) {
      final pixels = await _targetPixels(
        tester,
        mask: 10,
        flipped: flipped,
        topology: LitchcraftTopology.v2,
      );
      expect(pixels.getUint8((18 * 36 + 18) * 4), greaterThan(240));
      expect(pixels.getUint8((12 * 36 + 9) * 4), lessThan(100));
      expect(pixels.getUint8((12 * 36 + 27) * 4), lessThan(100));
    }
  });

  testWidgets('renders every LED target and the shared tool icon', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1200, 280);
    addTearDown(tester.view.reset);

    Widget button(int mask, {bool tool = false}) => Container(
      width: tool ? 132 : 156,
      height: tool ? 132 : 156,
      decoration: BoxDecoration(
        color: tool ? const Color(0xff25383d) : const Color(0xff172529),
        borderRadius: BorderRadius.circular(28),
        border: Border.all(color: const Color(0xff60777b), width: 2),
      ),
      alignment: Alignment.center,
      child: LitchcraftTargetIcon(
        mask: mask,
        topology: LitchcraftTopology.v3,
        flipped: false,
        color: 0xff7836,
        multicolor: tool,
        size: tool ? 84 : 108,
      ),
    );

    await tester.pumpWidget(
      MaterialApp(
        debugShowCheckedModeBanner: false,
        home: Scaffold(
          backgroundColor: const Color(0xff091012),
          body: Center(
            child: RepaintBoundary(
              key: const ValueKey<String>('led-icons-render'),
              child: Container(
                color: const Color(0xff091012),
                padding: const EdgeInsets.all(34),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    button(1),
                    const SizedBox(width: 20),
                    button(2),
                    const SizedBox(width: 20),
                    button(4),
                    const SizedBox(width: 20),
                    button(8),
                    const SizedBox(width: 20),
                    button(15),
                    const SizedBox(width: 28),
                    button(15, tool: true),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();

    const outputPath = String.fromEnvironment('LED_RENDER_PATH');
    if (outputPath.isNotEmpty) {
      final boundary = tester.renderObject<RenderRepaintBoundary>(
        find.byKey(const ValueKey<String>('led-icons-render')),
      );
      await tester.runAsync(() async {
        final image = await boundary.toImage(pixelRatio: 2);
        final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
        await File(outputPath).writeAsBytes(bytes!.buffer.asUint8List());
        image.dispose();
      });
    }
  });
}
