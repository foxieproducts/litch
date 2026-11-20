import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/widgets/litchcraft_motion_editor.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';

import 'health_render_capture.dart';

Widget _shell(Widget child, {double scale = 1, Locale? locale}) => MaterialApp(
  locale: locale,
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  builder: (context, child) => MediaQuery(
    data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(scale)),
    child: child!,
  ),
  home: Scaffold(body: SafeArea(child: child)),
);

void main() {
  for (final config in <({Size size, double scale, Locale locale})>[
    (size: Size(320, 700), scale: 1, locale: Locale('en')),
    (size: Size(320, 700), scale: 2, locale: Locale('de')),
    (size: Size(844, 390), scale: 2, locale: Locale('fr')),
  ]) {
    testWidgets(
      'hex keypad uses numeric rows and one A–F column ${config.size} ${config.scale}x',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        tester.view.physicalSize = config.size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        var color = 0x123456;
        final semantics = tester.ensureSemantics();
        try {
          await tester.pumpWidget(
            MaterialApp(
              locale: config.locale,
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              builder: (context, child) => RepaintBoundary(
                key: const ValueKey('hex-review-root'),
                child: MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: TextScaler.linear(config.scale)),
                  child: child!,
                ),
              ),
              home: Scaffold(
                body: SafeArea(
                  child: Center(
                    child: SizedBox(
                      width: 240,
                      height: 320,
                      child: StatefulBuilder(
                        builder: (context, update) => LitchcraftColorInspector(
                          color: color,
                          previousColor: null,
                          recentColors: const [],
                          enabled: true,
                          onPreviousColor: null,
                          onChanged: (value) => update(() => color = value),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          final field = find.byKey(const ValueKey('craft-inspector-hex'));
          await tester.ensureVisible(field);
          await tester.pumpAndSettle();
          await tester.tap(field);
          await tester.pumpAndSettle();
          Finder key(String digit) =>
              find.byKey(ValueKey('craft-hex-key-$digit'));
          Rect rect(String digit) => tester.getRect(key(digit));
          for (final row in ['123', '456', '789']) {
            expect(rect(row[0]).top, rect(row[1]).top);
            expect(rect(row[1]).top, rect(row[2]).top);
            expect(rect(row[0]).right, lessThanOrEqualTo(rect(row[1]).left));
            expect(rect(row[1]).right, lessThanOrEqualTo(rect(row[2]).left));
          }
          expect(rect('4').top, greaterThanOrEqualTo(rect('1').bottom));
          expect(rect('7').top, greaterThanOrEqualTo(rect('4').bottom));
          expect(rect('0').top, greaterThanOrEqualTo(rect('7').bottom));
          expect(rect('0').center.dx, rect('2').center.dx);
          for (final letter in 'ABCDEF'.split('')) {
            expect(rect(letter).left, greaterThan(rect('3').right));
            expect(rect(letter).left, rect('A').left);
          }
          for (var i = 1; i < 6; i++) {
            expect(
              rect('ABCDEF'[i]).top,
              greaterThanOrEqualTo(rect('ABCDEF'[i - 1]).bottom),
            );
          }
          for (final digit in '0123456789ABCDEF'.split('')) {
            await tester.ensureVisible(key(digit));
            await tester.pumpAndSettle();
            expect(tester.getSize(key(digit)).width, greaterThanOrEqualTo(48));
            expect(tester.getSize(key(digit)).height, greaterThanOrEqualTo(48));
            expect(key(digit).hitTestable(), findsOneWidget);
            expect(
              tester
                  .getSemantics(key(digit))
                  .getSemanticsData()
                  .hasAction(SemanticsAction.tap),
              isTrue,
            );
          }
          final name =
              'hex-keypad-${config.size.width.toInt()}-${config.scale.toInt()}x-${config.locale}';
          await captureHealthReview(
            tester,
            name,
            boundaryFinder: find.byKey(const ValueKey('hex-review-root')),
          );
          expect(tester.testTextInput.isVisible, isFalse);
          for (final digit in '123ABC'.split('')) {
            await tester.ensureVisible(key(digit));
            await tester.pumpAndSettle();
            await tester.tap(key(digit));
          }
          expect(tester.widget<TextField>(field).controller!.text, '#123ABC');
          final apply = find.byKey(const ValueKey('craft-hex-apply'));
          await tester.ensureVisible(apply);
          await tester.pumpAndSettle();
          await tester.tap(apply);
          await tester.pumpAndSettle();
          expect(color, 0x123abc);
          expect(find.byKey(const ValueKey('craft-hex-pad')), findsNothing);
          await tester.tap(field);
          await tester.pumpAndSettle();
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
          await tester.pumpAndSettle();
          expect(find.byKey(const ValueKey('craft-hex-pad')), findsNothing);
          expect(tester.testTextInput.isVisible, isFalse);
          expect(tester.takeException(), isNull);
        } finally {
          semantics.dispose();
        }
      },
    );
  }

  testWidgets(
    'disabled color field exposes disabled semantics and cannot mutate',
    (tester) async {
      var color = 0x123456;
      final changes = <int>[];
      final semantics = tester.ensureSemantics();
      await tester.pumpWidget(
        _shell(
          SizedBox(
            width: 240,
            child: LitchcraftColorInspector(
              color: color,
              previousColor: 0x654321,
              recentColors: const <int>[0x123456, 0x654321],
              enabled: false,
              onPreviousColor: () => color = 0x654321,
              onChanged: changes.add,
            ),
          ),
        ),
      );

      final field = find.byKey(const ValueKey('craft-inspector-color-field'));
      final rect = tester.getRect(field);
      await tester.tapAt(rect.bottomRight - const Offset(2, 2));
      final drag = await tester.startGesture(rect.topLeft + const Offset(2, 2));
      await drag.moveTo(rect.center);
      await drag.moveTo(rect.bottomRight - const Offset(2, 2));
      await drag.up();
      await tester.pump();

      expect(changes, isEmpty);
      expect(color, 0x123456);
      final node = tester.getSemantics(field);
      expect(
        node.getSemanticsData().flagsCollection.isEnabled,
        ui.Tristate.isFalse,
      );
      expect(node.getSemanticsData().hasAction(SemanticsAction.tap), isFalse);
      semantics.dispose();
    },
  );

  testWidgets(
    'persistent color inspector supports exact drag and hex recovery',
    (tester) async {
      var color = 0xff0000;
      late StateSetter refreshBrush;
      final changes = <int>[];
      await tester.pumpWidget(
        _shell(
          Center(
            child: SizedBox(
              width: 240,
              child: StatefulBuilder(
                builder: (context, setState) {
                  refreshBrush = setState;
                  return LitchcraftColorInspector(
                    color: color,
                    previousColor: null,
                    recentColors: const <int>[],
                    enabled: true,
                    onPreviousColor: null,
                    onChanged: (value) => setState(() {
                      color = value;
                      changes.add(value);
                    }),
                  );
                },
              ),
            ),
          ),
        ),
      );
      final field = find.byKey(const ValueKey('craft-inspector-color-field'));
      final rect = tester.getRect(field);
      final gesture = await tester.startGesture(rect.topLeft);
      await tester.pump();
      expect(color, 0xffffff);
      await gesture.moveTo(rect.center);
      await tester.pump();
      await gesture.moveTo(Offset(rect.right, rect.center.dy));
      await tester.pump();
      await gesture.moveTo(Offset(rect.right, rect.bottom));
      await tester.pump();
      expect(color, 0x000000);
      expect(changes.length, greaterThanOrEqualTo(3));
      await gesture.up();

      final hex = find.byKey(const ValueKey('craft-inspector-hex'));
      await tester.tap(hex);
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-hex-pad')), findsOneWidget);
      for (final digit in '123ABC'.split('')) {
        await tester.tap(find.byKey(ValueKey('craft-hex-key-$digit')));
      }
      expect(tester.widget<TextField>(hex).controller!.text, '#123ABC');
      await tester.tap(find.byKey(const ValueKey('craft-hex-apply')));
      await tester.pump();
      expect(color, 0x123abc);
      expect(find.byKey(const ValueKey('craft-hex-pad')), findsNothing);

      await tester.tap(hex);
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('craft-hex-key-A')));
      await tester.tap(find.byKey(const ValueKey('craft-hex-apply')));
      await tester.pump();
      expect(find.byIcon(Icons.error_outline), findsOneWidget);
      refreshBrush(() => color = 0x00ff00);
      await tester.pump();
      expect(tester.widget<TextField>(hex).controller!.text, '#A');
      final hue = find.byKey(const ValueKey('craft-inspector-hue'));
      expect(tester.widget<Slider>(hue).value, 120);
      await tester.tapAt(const Offset(4, 4));
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-hex-pad')), findsNothing);
      expect(find.byIcon(Icons.error_outline), findsNothing);
      expect(tester.widget<TextField>(hex).controller!.text, '#00FF00');
      await tester.tapAt(rect.center);
      await tester.pump();
      expect(color, 0x408040);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('hex pad sanitizes pasted text without opening system input', (
    tester,
  ) async {
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
      if (call.method == 'Clipboard.getData') {
        return <String, Object>{'text': '  #12-3a bc! '};
      }
      return null;
    });
    addTearDown(
      () => messenger.setMockMethodCallHandler(SystemChannels.platform, null),
    );
    var color = 0xff0000;
    await tester.pumpWidget(
      _shell(
        Center(
          child: SizedBox(
            width: 240,
            child: StatefulBuilder(
              builder: (context, setState) => LitchcraftColorInspector(
                color: color,
                previousColor: null,
                recentColors: const <int>[],
                enabled: true,
                onPreviousColor: null,
                onChanged: (value) => setState(() => color = value),
              ),
            ),
          ),
        ),
      ),
    );

    final field = find.byKey(const ValueKey('craft-inspector-hex'));
    await tester.tap(field);
    await tester.pumpAndSettle();
    expect(tester.testTextInput.isVisible, isFalse);
    await tester.tap(find.byKey(const ValueKey('craft-hex-paste')));
    await tester.pump();
    expect(tester.widget<TextField>(field).controller!.text, '#123ABC');
    await tester.tap(find.byKey(const ValueKey('craft-hex-apply')));
    await tester.pump();
    expect(color, 0x123abc);
  });

  testWidgets('color field owns horizontal drags inside a page view', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final pages = PageController(initialPage: 1);
    addTearDown(pages.dispose);
    var color = 0xff0000;
    await tester.pumpWidget(
      _shell(
        PageView(
          controller: pages,
          children: [
            const ColoredBox(color: Colors.black),
            Center(
              child: SizedBox(
                width: 260,
                height: 420,
                child: StatefulBuilder(
                  builder: (context, setState) => LitchcraftColorInspector(
                    color: color,
                    previousColor: null,
                    recentColors: const <int>[],
                    enabled: true,
                    onPreviousColor: null,
                    onChanged: (value) => setState(() => color = value),
                  ),
                ),
              ),
            ),
            const ColoredBox(color: Colors.white),
          ],
        ),
      ),
    );
    await tester.pumpAndSettle();

    final field = find.byKey(const ValueKey('craft-inspector-color-field'));
    final fieldRect = tester.getRect(field);
    final before = color;
    final drag = await tester.startGesture(
      Offset(fieldRect.right - 12, fieldRect.center.dy),
    );
    await drag.moveTo(
      Offset(fieldRect.left + 12, fieldRect.center.dy),
      timeStamp: const Duration(milliseconds: 250),
    );
    await drag.up();
    await tester.pumpAndSettle();

    expect(color, isNot(before));
    expect(pages.page, moreOrLessEquals(1));
    final hueTrack = tester.getRect(
      find.byKey(const ValueKey('craft-inspector-hue-track')),
    );
    final hueSlider = tester.getRect(
      find.byKey(const ValueKey('craft-inspector-hue')),
    );
    final hex = tester.getRect(
      find.byKey(const ValueKey('craft-inspector-hex')),
    );
    expect(hueTrack.height, 10);
    expect(hueSlider.height, greaterThanOrEqualTo(48));
    expect(hex.left, lessThan(hueSlider.left));
    expect(hex.center.dy, moreOrLessEquals(hueSlider.center.dy));
    expect(fieldRect.top - hueSlider.bottom, greaterThanOrEqualTo(6));

    await tester.pumpWidget(
      _shell(
        Center(
          child: SizedBox(
            width: 500,
            height: 80,
            child: LitchcraftColorInspector(
              color: color,
              previousColor: null,
              recentColors: const <int>[],
              enabled: true,
              onChanged: (value) => color = value,
              onPreviousColor: null,
              spacious: true,
            ),
          ),
        ),
      ),
    );
    await tester.pump();
    expect(
      tester.getRect(find.byKey(const ValueKey('craft-inspector-hue'))).height,
      greaterThanOrEqualTo(48),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'landscape color inspector keeps system-edge drags below hex and hue',
    (tester) async {
      await tester.binding.setSurfaceSize(const Size(844, 390));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await tester.pumpWidget(
        _shell(
          const Center(
            child: SizedBox(
              width: 188,
              height: 300,
              child: LitchcraftColorInspector(
                color: 0x3366ff,
                previousColor: 0xff6633,
                recentColors: <int>[],
                enabled: true,
                onChanged: _ignoreColor,
                onPreviousColor: null,
                spacious: true,
              ),
            ),
          ),
        ),
      );

      final hex = tester.getRect(
        find.byKey(const ValueKey('craft-inspector-hex')),
      );
      final hue = tester.getRect(
        find.byKey(const ValueKey('craft-inspector-hue')),
      );
      final field = tester.getRect(
        find.byKey(const ValueKey('craft-inspector-color-field')),
      );

      expect(hex.bottom, lessThanOrEqualTo(hue.top));
      expect(hue.bottom, lessThanOrEqualTo(field.top));
      expect(hex.width, greaterThanOrEqualTo(180));
      expect(field.height, greaterThanOrEqualTo(150));
      expect(tester.takeException(), isNull);
    },
  );

  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets('anchored palette stays reachable at $size and 200% text', (
      tester,
    ) async {
      await tester.binding.setSurfaceSize(size);
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      int? selected;
      var custom = 0;
      await tester.pumpWidget(
        _shell(
          Align(
            alignment: Alignment.topRight,
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: LitchcraftPaletteButton(
                color: litchcraftPalette.first,
                recentColors: const [0x112233, 0x445566],
                enabled: true,
                onSelected: (value) => selected = value,
                onCustomColor: () => custom++,
                onOpen: () {},
                onClose: () {},
              ),
            ),
          ),
          scale: 2,
        ),
      );
      final palette = find.byKey(const ValueKey('craft-palette'));
      final colorControl = find.byKey(const ValueKey('craft-color-control'));
      final collapsedRect = tester.getRect(colorControl);
      await tester.tap(palette);
      await tester.pumpAndSettle();
      expect(find.byType(BottomSheet), findsNothing);
      final tray = tester.getRect(
        find.byKey(const ValueKey('craft-palette-tray')),
      );
      expect(tray.top, collapsedRect.top);
      expect(tray.height, collapsedRect.height);
      expect(
        find.byKey(const ValueKey('craft-palette-collapse')),
        findsOneWidget,
      );
      final recent = find.byKey(const ValueKey('craft-picker-color-1122867'));
      await tester.ensureVisible(recent);
      expect(tester.getSize(recent).shortestSide, greaterThanOrEqualTo(48));
      await tester.tap(recent);
      await tester.pumpAndSettle();
      expect(selected, 0x112233);
      expect(find.byKey(const ValueKey('craft-palette-tray')), findsNothing);
      await tester.ensureVisible(palette);
      await tester.tap(palette);
      await tester.pumpAndSettle();
      final customAction = find.byKey(const ValueKey('craft-custom-color'));
      await tester.ensureVisible(customAction);
      await tester.tap(customAction);
      await tester.pumpAndSettle();
      expect(custom, 1);
      expect(find.byKey(const ValueKey('craft-palette-tray')), findsNothing);
      await tester.ensureVisible(palette);
      await tester.tap(palette);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-palette-tray')), findsNothing);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('200% Effects choices use full-width whole-word labels', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(360, 780));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.pumpWidget(
      _shell(
        SingleChildScrollView(
          padding: const EdgeInsets.symmetric(horizontal: 20),
          child: LitchcraftEffectEditorPanel(
            selectedEffect: LitchcraftEffectStyle.breathe,
            effectDurationMs: 2000,
            enabled: true,
            onEffectChanged: (_) {},
            onDurationChanged: (_) {},
          ),
        ),
        scale: 2,
      ),
    );
    await tester.pump();
    final selector = find.byKey(const ValueKey('craft-effect-selector'));
    expect(tester.getSize(selector).width, 320);
    await tester.tap(selector);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    final first = tester.getRect(
      find.byKey(const ValueKey('craft-effect-bounce')),
    );
    final second = tester.getRect(
      find.byKey(const ValueKey('craft-effect-breathe')),
    );
    expect(first.width, 320);
    expect(second.left, first.left);
    expect(second.top, greaterThanOrEqualTo(first.bottom + 8));
    final sparkle = find.text('Sparkle');
    await tester.ensureVisible(sparkle);
    await tester.pump();
    // A one-word choice must not become the previously observed "Sparkl / e".
    final paragraph = tester.renderObject<RenderParagraph>(
      find.descendant(of: sparkle, matching: find.byType(RichText)),
    );
    expect(
      paragraph.getBoxesForSelection(
        const TextSelection(baseOffset: 0, extentOffset: 7),
      ),
      hasLength(1),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('200% German Effects names remain complete', (tester) async {
    await tester.binding.setSurfaceSize(const Size(240, 780));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.pumpWidget(
      _shell(
        SingleChildScrollView(
          child: LitchcraftEffectEditorPanel(
            selectedEffect: LitchcraftEffectStyle.orbitCounterclockwise,
            effectDurationMs: 2000,
            enabled: true,
            onEffectChanged: (_) {},
            onDurationChanged: (_) {},
          ),
        ),
        scale: 2,
        locale: const Locale('de'),
      ),
    );
    await tester.pump();
    const label = 'Umlauf gegen den Uhrzeigersinn';
    final selectorText = find.descendant(
      of: find.byKey(const ValueKey('craft-effect-selector')),
      matching: find.text(label),
    );
    expect(selectorText, findsOneWidget);
    expect(
      tester.renderObject<RenderParagraph>(selectorText).didExceedMaxLines,
      isFalse,
    );
    await tester.tap(find.byKey(const ValueKey('craft-effect-selector')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    final menuText = find.descendant(
      of: find.byKey(const ValueKey('craft-effect-orbitCounterclockwise')),
      matching: find.text(label),
    );
    expect(menuText, findsOneWidget);
    expect(
      tester.renderObject<RenderParagraph>(menuText).didExceedMaxLines,
      isFalse,
    );
    expect(tester.takeException(), isNull);
  });

  test('timing values retain exact useful seconds', () {
    expect(litchcraftDurationSeconds(50), '0.05');
    expect(litchcraftDurationSeconds(250), '0.25');
    expect(litchcraftDurationSeconds(500), '0.5');
    expect(litchcraftDurationSeconds(1000), '1.0');
    expect(litchcraftDurationSeconds(2137), '2.137');
  });

  testWidgets(
    'frame and effect timing share live sliders, units, endpoints, and help',
    (tester) async {
      var frameDuration = 1000;
      final frameChanges = <int>[];
      LitchcraftAnimationDocument frameDocument() =>
          LitchcraftAnimationDocument(
            topology: LitchcraftTopology.v3,
            favoriteColors: litchcraftDefaultFavoriteColors,
            frames: <LitchcraftAnimationFrame>[
              LitchcraftAnimationFrame(
                emitters: List<LitchcraftAnimatedEmitter>.filled(
                  256,
                  const LitchcraftAnimatedEmitter(color: 0x123456),
                ),
                durationMs: frameDuration,
              ),
            ],
          );
      await tester.pumpWidget(
        _shell(
          SizedBox(
            width: 320,
            child: StatefulBuilder(
              builder: (context, update) => LitchcraftFrameEditorPanel(
                document: frameDocument(),
                selectedFrameIndex: 0,
                enabled: true,
                onDurationChanged: (duration) => update(() {
                  frameDuration = duration;
                  frameChanges.add(duration);
                }),
                onDuplicate: () {},
                onDelete: () {},
                onOpenEffects: () {},
              ),
            ),
          ),
        ),
      );
      final frameSlider = find.byKey(const ValueKey('craft-frame-duration'));
      final turtle = find.text('Slower');
      final rabbit = find.text('Faster');
      expect(turtle, findsOneWidget);
      expect(rabbit, findsOneWidget);
      expect(
        tester.getCenter(turtle).dx,
        lessThan(tester.getCenter(rabbit).dx),
      );
      expect(tester.widget<Slider>(frameSlider).divisions, 99);
      expect(tester.getSemantics(frameSlider).value, 'Frame duration: 1.0 s');
      final frameRect = tester.getRect(frameSlider);
      final frameGesture = await tester.startGesture(
        Offset(frameRect.left + 24, frameRect.center.dy),
      );
      await tester.pump();
      await frameGesture.moveTo(
        Offset(frameRect.right - 24, frameRect.center.dy),
        timeStamp: const Duration(milliseconds: 120),
      );
      await tester.pump();
      await frameGesture.moveTo(
        Offset(frameRect.right - 24, frameRect.center.dy),
        timeStamp: const Duration(milliseconds: 240),
      );
      await tester.pump();
      await frameGesture.up();
      expect(
        frameChanges,
        hasLength(1),
        reason: 'Frame timing commits on release',
      );
      expect(frameDuration, frameChanges.last);
      expect(frameDuration, 50);
      expect(
        frameChanges.every(
          (value) => value >= 50 && value <= 5000 && value % 50 == 0,
        ),
        isTrue,
      );

      var effectDuration = 2000;
      final effectChanges = <int>[];
      await tester.pumpWidget(
        _shell(
          SizedBox(
            width: 320,
            child: StatefulBuilder(
              builder: (context, update) => LitchcraftEffectEditorPanel(
                selectedEffect: LitchcraftEffectStyle.breathe,
                effectDurationMs: effectDuration,
                enabled: true,
                onEffectChanged: (_) {},
                onDurationChanged: (duration) => update(() {
                  effectDuration = duration;
                  effectChanges.add(duration);
                }),
              ),
            ),
          ),
        ),
      );
      final effectSlider = find.byKey(const ValueKey('craft-effect-duration'));
      expect(find.text('Faster'), findsOneWidget);
      expect(find.text('Slower'), findsOneWidget);
      expect(find.text('2.0 s'), findsOneWidget);
      expect(tester.getSemantics(effectSlider).value, 'Effect cycle: 2.0 s');
      final effectRect = tester.getRect(effectSlider);
      final effectGesture = await tester.startGesture(
        Offset(effectRect.left + 24, effectRect.center.dy),
      );
      await tester.pump();
      await effectGesture.moveTo(
        Offset(effectRect.right - 24, effectRect.center.dy),
        timeStamp: const Duration(milliseconds: 120),
      );
      await tester.pump();
      await effectGesture.moveTo(
        Offset(effectRect.center.dx, effectRect.center.dy),
        timeStamp: const Duration(milliseconds: 240),
      );
      await tester.pump();
      await effectGesture.up();
      expect(
        effectChanges,
        hasLength(1),
        reason:
            'Effect timing commits once after the held multi-point preview.',
      );
      expect(effectDuration, effectChanges.last);
      await tester.longPress(
        find.byKey(const ValueKey('craft-effect-duration-context')),
      );
      await tester.pump(const Duration(milliseconds: 600));
      expect(find.textContaining('Effect cycle:'), findsWidgets);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'landscape frame strip selects an adjacent frame with semantics',
    (tester) async {
      final semantics = tester.ensureSemantics();
      var selected = 0;
      int? heldFrame;
      late StateSetter updateSelection;
      final document = LitchcraftAnimationDocument(
        topology: LitchcraftTopology.v3,
        favoriteColors: litchcraftDefaultFavoriteColors,
        frames: [
          for (var index = 0; index < 8; index++)
            LitchcraftAnimationFrame(
              emitters: List.generate(
                256,
                (_) => LitchcraftAnimatedEmitter(color: 0x110000 * (index + 1)),
              ),
            ),
        ],
      );
      await tester.pumpWidget(
        _shell(
          Center(
            child: SizedBox(
              width: 312,
              child: StatefulBuilder(
                builder: (context, setState) {
                  updateSelection = setState;
                  return LitchcraftAnimationBar(
                    document: document,
                    selectedFrameIndex: selected,
                    playing: false,
                    enabled: true,
                    frameEditingEnabled: true,
                    motionEnabled: true,
                    playbackVisible: true,
                    motionLabel: 'Motion',
                    playLabel: 'Play',
                    pauseLabel: 'Pause',
                    previousFrameLabel: 'Previous',
                    nextFrameLabel: 'Next',
                    addFrameLabel: 'Add frame',
                    deleteFrameLabel: 'Delete frame',
                    motionDisabledHint: 'Unavailable',
                    frameLabel: (frame, count) => 'Frame $frame of $count',
                    expanded: true,
                    onOpenEffects: () {},
                    onOpenFrameDetails: () {},
                    onTogglePlayback: () {},
                    onSelectFrame: (index) => setState(() => selected = index),
                    onAddFrame: () {},
                    onDeleteFrame: () {},
                    onFrameLongPress: (index, _) => heldFrame = index,
                  );
                },
              ),
            ),
          ),
        ),
      );
      final controls = [
        'craft-frame-previous',
        'craft-preview',
        'craft-frame-next',
      ].map((key) => tester.getCenter(find.byKey(ValueKey(key))).dy);
      expect(controls.toSet(), hasLength(1));
      expect(find.byKey(const ValueKey('craft-motion')), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-delete-current-frame')),
        findsNothing,
      );
      expect(
        tester
            .getTopLeft(find.byKey(const ValueKey('craft-frame-filmstrip')))
            .dy,
        greaterThan(
          tester.getBottomLeft(find.byKey(const ValueKey('craft-preview'))).dy,
        ),
      );
      final viewport = find.byKey(
        const ValueKey('craft-frame-scroll-viewport'),
      );
      ScrollPosition position() => tester
          .state<ScrollableState>(
            find.descendant(of: viewport, matching: find.byType(Scrollable)),
          )
          .position;
      void expectFullyVisible(int index) {
        final bounds = tester.getRect(viewport);
        final target = find.byKey(ValueKey('craft-select-frame-$index'));
        final rect = tester.getRect(target);
        expect(rect.left, greaterThanOrEqualTo(bounds.left - .5));
        expect(rect.right, lessThanOrEqualTo(bounds.right + .5));
        expect(target.hitTestable(), findsOneWidget);
      }

      final next = find.byKey(const ValueKey('craft-select-frame-1'));
      expectFullyVisible(1);
      final beforeVisibleTap = position().pixels;
      await tester.tap(next);
      await tester.pumpAndSettle();
      expect(selected, 1);
      expect(
        position().pixels,
        closeTo(beforeVisibleTap, .01),
        reason: 'Selecting a fully visible adjacent frame preserves browsing.',
      );
      expectFullyVisible(selected);
      expect(tester.getSize(next).shortestSide, greaterThanOrEqualTo(48));
      expect(
        tester.getSemantics(next),
        matchesSemantics(
          label: 'Frame 2 of 8',
          isButton: true,
          isEnabled: true,
          hasEnabledState: true,
          isSelected: true,
          hasSelectedState: true,
          hasTapAction: true,
          hasLongPressAction: true,
        ),
      );
      await tester.longPress(next);
      await tester.pump();
      expect(heldFrame, 1);
      updateSelection(() => selected = 4);
      await tester.pump(const Duration(milliseconds: 200));
      expect(selected, 4, reason: 'programmatic reveal keeps frame authority');
      expectFullyVisible(selected);
      updateSelection(() => selected = 0);
      await tester.pumpAndSettle();
      expect(selected, 0, reason: 'clamped first-frame reveal stays selected');
      final strip = find.byKey(const ValueKey('craft-frame-filmstrip'));
      expectFullyVisible(selected);
      final swipe = await tester.startGesture(tester.getCenter(strip));
      await swipe.moveBy(
        const Offset(-20, 0),
        timeStamp: const Duration(milliseconds: 200),
      );
      await tester.pump(const Duration(milliseconds: 200));
      await swipe.moveBy(
        const Offset(-33, 0),
        timeStamp: const Duration(milliseconds: 800),
      );
      await tester.pump(const Duration(milliseconds: 600));
      await swipe.up(timeStamp: const Duration(milliseconds: 1000));
      await tester.pump(const Duration(milliseconds: 200));
      expect(selected, 0, reason: 'scrolling only browses thumbnails');
      await tester.drag(strip, const Offset(-500, 0));
      await tester.pump(const Duration(milliseconds: 200));
      expect(selected, 0, reason: 'flinging to the end keeps frame authority');
      expect(
        find.byKey(const ValueKey('craft-select-frame-7')).hitTestable(),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-select-frame-7')));
      await tester.pumpAndSettle();
      expect(selected, 7, reason: 'an explicit tap selects the browsed frame');
      expectFullyVisible(selected);
      expect(tester.takeException(), isNull);
      semantics.dispose();
    },
  );
}

void _ignoreColor(int _) {}
