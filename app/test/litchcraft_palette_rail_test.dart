import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';

void main() {
  testWidgets('Play glyph paints four filled chamfered alternating squares', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: ThemeData.dark(),
        home: Center(
          child: RepaintBoundary(
            key: const ValueKey('play-glyph'),
            child: const LitchPlayGlyph(),
          ),
        ),
      ),
    );
    final boundary = tester.renderObject<RenderRepaintBoundary>(
      find.byKey(const ValueKey('play-glyph')),
    );
    await tester.runAsync(() async {
      final image = await boundary.toImage(pixelRatio: 1);
      final bytes = (await image.toByteData(
        format: ui.ImageByteFormat.rawRgba,
      ))!;
      int pixel(int x, int y) => bytes.getUint32((y * 24 + x) * 4);
      expect(pixel(6, 6), pixel(18, 18));
      expect(pixel(18, 6), pixel(6, 18));
      expect(pixel(6, 6), isNot(pixel(18, 6)));
      expect(pixel(6, 6) & 255, 255);
      expect(pixel(18, 6) & 255, 255);
      Color colorAt(int x, int y) {
        final value = pixel(x, y);
        return Color.fromARGB(
          255,
          value >> 24,
          (value >> 16) & 255,
          (value >> 8) & 255,
        );
      }

      expect(
        (colorAt(6, 6).computeLuminance() + .05) /
            (colorAt(18, 6).computeLuminance() + .05),
        greaterThanOrEqualTo(5),
      );
      expect(pixel(1, 1) & 255, lessThan(255));
      expect(pixel(12, 12) & 255, 0);
      image.dispose();
    });
  });
  test('rail contains 32 full-value firmware quick-palette samples', () {
    expect(litchcraftPaletteRailColors.length, 32);
    expect(litchcraftPaletteRailColors.toSet().length, 32);
    // Independent integer oracle for wheel 0,8,...248, R → G → B → R.
    for (var index = 0; index < 32; index++) {
      final wheel = index * 8;
      final int expected;
      if (wheel <= 85) {
        expected = ((255 - wheel * 3) << 16) | (wheel * 3 << 8);
      } else if (wheel <= 170) {
        final offset = wheel - 85;
        expected = ((255 - offset * 3) << 8) | (offset * 3);
      } else {
        final offset = wheel - 170;
        expected = (offset * 3 << 16) | (255 - offset * 3);
      }
      final channels = [
        (expected >> 16) & 255,
        (expected >> 8) & 255,
        expected & 255,
      ];
      final peak = channels.reduce((a, b) => a > b ? a : b);
      final normalized = channels.fold(
        0,
        (rgb, channel) => (rgb << 8) | ((channel * 255 + peak ~/ 2) ~/ peak),
      );
      final color = HSVColor.fromColor(
        Color(0xff000000 | litchcraftPaletteRailColors[index]),
      );
      expect(color.saturation, 1);
      expect(color.value, 1);
      expect(
        litchcraftPaletteRailColors[index],
        normalized,
        reason: 'wheel $wheel',
      );
    }
  });

  for (final axis in Axis.values) {
    testWidgets('swatch tap and deliberate hold remain exclusive $axis', (
      tester,
    ) async {
      final taps = <int>[];
      final fills = <int>[];
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: Center(
              child: SizedBox(
                width: axis == Axis.vertical ? 48 : 384,
                height: axis == Axis.vertical ? 384 : 48,
                child: LitchcraftPaletteRail(
                  axis: axis,
                  color: 0xff0000,
                  enabled: true,
                  onSelected: taps.add,
                  onLongPress: fills.add,
                ),
              ),
            ),
          ),
        ),
      );
      final swatch = find.byKey(const ValueKey('craft-wheel-color-0'));
      expect(tester.getSize(swatch), const Size(48, 48));
      await tester.tap(swatch);
      expect(taps, [0xff0000]);
      expect(fills, isEmpty);
      taps.clear();
      final pointer = await tester.startGesture(tester.getCenter(swatch));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
      expect(find.byKey(const ValueKey('craft-hold-progress')), findsOneWidget);
      expect(fills, isEmpty);
      await tester.pump(const Duration(milliseconds: 450));
      expect(fills, [0xff0000]);
      await pointer.up();
      await tester.pump();
      expect(taps, isEmpty);
      expect(fills, hasLength(1));
      expect(find.byKey(const ValueKey('craft-hold-progress')), findsNothing);
    });
    testWidgets(
      'rapid swatch taps cannot strand hold progress after selection rebuild $axis',
      (tester) async {
        final color = ValueNotifier(litchcraftPaletteRailColors[1]);
        addTearDown(color.dispose);
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: Center(
                child: SizedBox(
                  width: axis == Axis.vertical ? 48 : 384,
                  height: axis == Axis.vertical ? 384 : 48,
                  child: ValueListenableBuilder<int>(
                    valueListenable: color,
                    builder: (_, selected, _) => LitchcraftPaletteRail(
                      axis: axis,
                      color: selected,
                      enabled: true,
                      onSelected: (value) => color.value = value,
                      onLongPress: (_) {},
                    ),
                  ),
                ),
              ),
            ),
          ),
        );

        final swatch = find.byKey(const ValueKey('craft-wheel-color-0'));
        for (var attempt = 0; attempt < 8; attempt++) {
          final pointer = await tester.startGesture(tester.getCenter(swatch));
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 40));
          expect(
            find.byKey(const ValueKey('craft-hold-progress')),
            findsOneWidget,
          );
          await pointer.up();
          await tester.pump();
          expect(
            find.byKey(const ValueKey('craft-hold-progress')),
            findsNothing,
            reason: 'a completed tap must clear its hold affordance',
          );
        }
      },
    );
    testWidgets(
      'real held drags wrap continuously both ways without selecting $axis',
      (tester) async {
        final selections = <int>[];
        final color = ValueNotifier(0xff0000);
        addTearDown(color.dispose);
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: Center(
                child: SizedBox(
                  width: axis == Axis.vertical ? 48 : 384,
                  height: axis == Axis.vertical ? 384 : 48,
                  child: ValueListenableBuilder<int>(
                    valueListenable: color,
                    builder: (_, current, _) => LitchcraftPaletteRail(
                      axis: axis,
                      color: current,
                      enabled: true,
                      onSelected: selections.add,
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
        final rail = find.byType(LitchcraftPaletteRail);
        final center = tester.getCenter(rail);
        final wheel = tester.widget<ListWheelScrollView>(
          find.byType(ListWheelScrollView),
        );
        final controller = wheel.controller! as FixedExtentScrollController;
        Offset movement(double delta) =>
            axis == Axis.vertical ? Offset(0, delta) : Offset(delta, 0);
        expect(
          find.byKey(const ValueKey('craft-wheel-color-31')).hitTestable(),
          findsOneWidget,
        );
        expect(
          find.byKey(const ValueKey('craft-wheel-color-0')).hitTestable(),
          findsOneWidget,
        );
        for (final direction in [-1.0, 1.0]) {
          final start = controller.offset;
          final pointer = await tester.startGesture(center);
          await pointer.moveBy(movement(direction * 24));
          await tester.pump(const Duration(milliseconds: 30));
          for (var step = 0; step < 40; step++) {
            await pointer.moveBy(movement(direction * 48));
            await tester.pump(const Duration(milliseconds: 30));
            final item = controller.selectedItem;
            final index = item % 32;
            final swatch = find.byKey(ValueKey('craft-wheel-color-$index'));
            // Scrollable intentionally ignores taps while a pointer owns a drag.
            expect(swatch, findsOneWidget);
            expect(tester.getSize(swatch), const Size(48, 48));
            final swatchCenter = tester.getCenter(swatch);
            expect(
              (axis == Axis.vertical
                      ? swatchCenter.dy - center.dy
                      : swatchCenter.dx - center.dx)
                  .abs(),
              lessThanOrEqualTo(25),
            );
            expect(selections, isEmpty);
            if (step == 5) {
              final heldOffset = controller.offset;
              color.value = 0xabcdef;
              await tester.pump(const Duration(milliseconds: 30));
              expect(controller.offset, heldOffset);
            }
          }
          await pointer.up();
          await tester.pumpAndSettle();
          expect((controller.offset - start).abs(), greaterThan(32 * 48));
        }
        final selected = controller.selectedItem % 32;
        await tester.tap(find.byKey(ValueKey('craft-wheel-color-$selected')));
        expect(selections, [litchcraftPaletteRailColors[selected]]);
        expect(tester.takeException(), isNull);
      },
    );
  }
}
