import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:litch_mobile/src/screens/manual_screen.dart';
import 'package:litch_mobile/src/services/external_link_launcher.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/board_controls_glyph.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:package_info_plus/package_info_plus.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';

Future<void> _tapTopic(WidgetTester tester, Finder topic) async {
  await Scrollable.ensureVisible(tester.element(topic), alignment: .3);
  await tester.pumpAndSettle();
  expect(topic.hitTestable(), findsOneWidget);
  await tester.tap(topic);
  await tester.pumpAndSettle();
}

const _readerIds = <String>[
  'start',
  'controls',
  'moves',
  'litchcraft',
  'computer',
  'duck',
  'assistance',
  'undo',
  'clock',
  'lights',
  'recovery',
  'factory-reset',
];

Widget _readerHarness({
  String? topic = 'start',
  double scale = 1,
  Locale locale = const Locale('en'),
  bool rtl = false,
}) => MaterialApp(
  locale: locale,
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  theme: buildLitchTheme(),
  builder: (context, child) => MediaQuery(
    data: MediaQuery.of(context).copyWith(textScaler: TextScaler.linear(scale)),
    child: Directionality(
      textDirection: rtl ? TextDirection.rtl : TextDirection.ltr,
      child: child!,
    ),
  ),
  home: ManualScreen(initialTopic: topic),
);

Future<void> _swipeReader(
  WidgetTester tester,
  String id, {
  required bool forward,
  bool rtl = false,
}) async {
  final viewport = tester.getRect(
    find.byKey(PageStorageKey<String>('manual-$id-article-scroll')),
  );
  final direction = (forward ? -1.0 : 1.0) * (rtl ? -1 : 1);
  final start = viewport.center - Offset(direction * viewport.width * .3, 0);
  final gesture = await tester.startGesture(start);
  for (var step = 1; step <= 5; step++) {
    await gesture.moveTo(
      start + Offset(direction * viewport.width * .13 * step, 0),
    );
    await tester.pump(const Duration(milliseconds: 60));
  }
  await gesture.up();
  await tester.pumpAndSettle();
}

void main() {
  test(
    'customer Manual uses current six-control Bluetooth clear and X-hold entry',
    () async {
      for (final locale in AppLocalizations.supportedLocales) {
        final strings = await AppLocalizations.delegate.load(locale);
        final controls = strings.manualControlsBody;
        final start = strings.manualStartBody;
        expect(
          RegExp(
            r'center|centre|central|Mitteltaste|centro|centrale',
            caseSensitive: false,
          ).hasMatch('$controls $start'),
          isFalse,
          reason: 'No center control in ${locale.toLanguageTag()}',
        );
        expect(controls, contains('X'));
        expect(start, contains('Bluetooth'));
        // Number words and prose directions are localized; the shared recovery
        // paragraph, gesture order and capacity are the maintained contract.
        final timings = switch (locale.languageCode) {
          'de' => ['eine Sekunde', 'zwei Sekunden', 'drei Erfolgsblitze'],
          'es' => ['un segundo', 'dos segundos', 'tres destellos'],
          'fr' => ['une seconde', 'deux secondes', 'trois clignotements'],
          'it' => ['un secondo', 'due secondi', 'tre lampeggi'],
          _ => ['one second', 'two seconds', 'three success flashes'],
        };
        final clearHelp = strings.ownerVerificationRecoverySteps;
        expect(strings.ownerPairingOtherOwner, contains(clearHelp));
        expect(
          strings.ownerPairingOtherOwner.split('\n\n').first,
          contains(timings.last.split(' ').first),
          reason: 'The saved-device limit remains three in $locale',
        );
        for (final body in <String>[
          strings.manualControlsBody,
          strings.manualConnectionBody,
        ]) {
          expect(body.split('\n\n'), contains(clearHelp));
          expect(clearHelp, contains('X'));
          expect(clearHelp, contains('Bluetooth'));
          for (final timing in timings) {
            expect(clearHelp, contains(timing), reason: '$locale: $timing');
          }
          expect(
            clearHelp.indexOf('X'),
            lessThan(clearHelp.indexOf(timings[0])),
          );
          expect(
            clearHelp.indexOf(timings[0]),
            lessThan(clearHelp.indexOf('Bluetooth')),
          );
          expect(
            clearHelp.indexOf('Bluetooth'),
            lessThan(clearHelp.lastIndexOf('X')),
          );
          expect(
            clearHelp.lastIndexOf('X'),
            lessThan(clearHelp.indexOf(timings[1])),
          );
          expect(
            clearHelp.indexOf(timings[1]),
            lessThan(clearHelp.indexOf(timings[2])),
          );
          expect(
            clearHelp.indexOf(timings[2]),
            lessThan(clearHelp.indexOf('Foxie Litch')),
          );
        }
        final oneDevicePolicy = switch (locale.languageCode) {
          'de' => 'einem Gerät',
          'es' => 'dispositivo a la vez',
          'fr' => 'un seul appareil',
          'it' => 'un solo dispositivo',
          _ => 'one device',
        };
        expect(strings.connectionHelpBasics, contains(oneDevicePolicy));
        expect(strings.manualConnectionBody, contains(oneDevicePolicy));
        expect(
          strings.manualConnectionBody.indexOf(oneDevicePolicy),
          lessThan(strings.manualConnectionBody.indexOf(clearHelp)),
        );
        expect(
          RegExp(
            r'older firmware|ältere firmware|firmware anterior|ancien firmware|firmware più vecchi',
            caseSensitive: false,
          ).hasMatch(strings.manualLightsBody),
          isFalse,
        );
      }
    },
  );

  testWidgets(
    'German 200% reader uses its constrained portrait space without header clipping',
    (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(320, 700));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          theme: buildLitchTheme().copyWith(platform: TargetPlatform.iOS),
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () => Navigator.of(
                  context,
                ).push<void>(ManualScreen.route(initialTopic: 'start')),
                child: const Text('Open reader'),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text('Open reader'));
      await tester.pumpAndSettle();
      final title = find.text(
        lookupAppLocalizations(const Locale('de')).manualTitle,
      );
      final appBar = tester.getRect(find.byType(AppBar));
      final titleRect = tester.getRect(title);
      expect(titleRect.top, greaterThanOrEqualTo(appBar.top));
      expect(titleRect.bottom, lessThanOrEqualTo(appBar.bottom));
      expectReviewTextFits(
        tester,
        title,
        'The entire enlarged German reader title is inside its actual toolbar.',
      );
      final strings = lookupAppLocalizations(const Locale('de'));
      final previous = find.byKey(const ValueKey('manual-previous-article'));
      final next = find.byKey(const ValueKey('manual-next-article'));
      expect(tester.getSize(previous), tester.getSize(next));
      for (final (button, text) in [
        (previous, strings.manualPrevious),
        (next, strings.manualNext),
      ]) {
        final bounds = tester.getRect(button);
        expect(bounds.size.shortestSide, greaterThanOrEqualTo(48));
        final label = find.descendant(of: button, matching: find.text(text));
        expectReviewTextFits(tester, label, 'The full navigation label fits.');
        expect(tester.getRect(label).left, greaterThanOrEqualTo(bounds.left));
        expect(tester.getRect(label).right, lessThanOrEqualTo(bounds.right));
        final paragraph = tester.renderObject<RenderParagraph>(
          find.descendant(of: label, matching: find.byType(RichText)),
        );
        expect(paragraph.textScaler.scale(10), 13);
      }
      final reading = tester.renderObject<RenderParagraph>(
        find.descendant(
          of: find.byKey(const ValueKey('manual-start-body')),
          matching: find.byType(RichText),
        ),
      );
      expect(reading.textScaler.scale(10), 20);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('reader buttons move forward left and backward right', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_readerHarness());
    await tester.pumpAndSettle();
    final start = find.byKey(const ValueKey('manual-start-body'));
    final controls = find.byKey(const ValueKey('manual-controls-body'));
    final next = find.byKey(const ValueKey('manual-next-article'));
    await tester.ensureVisible(next);
    await tester.pumpAndSettle();
    final startX = tester.getTopLeft(start).dx;
    await tester.tap(next);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 80));
    expect(
      tester.getTopLeft(start).dx,
      lessThan(startX),
      reason: 'Next moves the current reading page left.',
    );
    await tester.pumpAndSettle();
    final previous = find.byKey(const ValueKey('manual-previous-article'));
    await tester.ensureVisible(previous);
    await tester.pumpAndSettle();
    final controlsX = tester.getTopLeft(controls).dx;
    await tester.tap(previous);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 80));
    expect(
      tester.getTopLeft(controls).dx,
      greaterThan(controlsX),
      reason:
          'Previous must reverse the reading motion, not enter as another Next.',
    );
    await tester.pumpAndSettle();
    expect(start.hitTestable(), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('a multi-point reading swipe advances and reverses the topic', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_readerHarness());
    await tester.pumpAndSettle();
    await _swipeReader(tester, 'start', forward: true);
    expect(
      find
          .byKey(const PageStorageKey<String>('manual-controls-article-scroll'))
          .hitTestable(),
      findsOneWidget,
      reason: 'An ordinary center-of-reader swipe must open the next section.',
    );
    await _swipeReader(tester, 'controls', forward: false);
    expect(
      find
          .byKey(const PageStorageKey<String>('manual-start-article-scroll'))
          .hitTestable(),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('Previous brings the earlier topic in from the left', (
    tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_readerHarness(topic: 'controls'));
    await tester.pumpAndSettle();
    final controls = find.byKey(const ValueKey('manual-controls-body'));
    final previous = find.byKey(const ValueKey('manual-previous-article'));
    await tester.ensureVisible(previous);
    await tester.pumpAndSettle();
    final settledX = tester.getTopLeft(controls).dx;
    await tester.tap(previous);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 80));
    expect(
      tester.getTopLeft(find.byKey(const ValueKey('manual-start-body'))).dx,
      lessThan(settledX),
      reason:
          'Earlier reading content enters from the left; the original route replacement incorrectly enters from the right.',
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey('manual-start-body')).hitTestable(),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
  });

  for (final rtl in [false, true]) {
    testWidgets('all twelve sections swipe with stable controls ($rtl RTL)', (
      tester,
    ) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final semantics = tester.ensureSemantics();
      try {
        await tester.pumpWidget(_readerHarness(rtl: rtl));
        await tester.pumpAndSettle();
        final previous = find.byKey(const ValueKey('manual-previous-article'));
        final next = find.byKey(const ValueKey('manual-next-article'));
        final previousRect = tester.getRect(previous);
        final nextRect = tester.getRect(next);
        expect(previousRect.size, nextRect.size);
        expect(previousRect.height, greaterThanOrEqualTo(48));
        expect(tester.widget<TextButton>(previous).onPressed, isNull);
        expect(
          tester
              .getSemantics(previous)
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isFalse,
        );
        await _swipeReader(tester, 'start', forward: false, rtl: rtl);
        expect(
          find.byKey(const ValueKey('manual-start-body')).hitTestable(),
          findsOneWidget,
        );
        for (var i = 0; i < _readerIds.length; i++) {
          final id = _readerIds[i];
          final viewport = find.byKey(
            PageStorageKey<String>('manual-$id-article-scroll'),
          );
          expect(
            viewport.hitTestable(),
            findsOneWidget,
            reason: '$id is the visible reader.',
          );
          expect(
            tester.getRect(previous),
            previousRect,
            reason: '$id keeps Previous in place.',
          );
          expect(
            tester.getRect(next),
            nextRect,
            reason: '$id keeps Next in place.',
          );
          expect(tester.widget<TextButton>(previous).onPressed == null, i == 0);
          expect(
            tester.widget<TextButton>(next).onPressed == null,
            i == _readerIds.length - 1,
          );
          if (i + 1 < _readerIds.length) {
            await _swipeReader(tester, id, forward: true, rtl: rtl);
          }
        }
        expect(
          tester
              .getSemantics(next)
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isFalse,
        );
        await _swipeReader(tester, _readerIds.last, forward: true, rtl: rtl);
        expect(
          find
              .byKey(
                PageStorageKey<String>(
                  'manual-${_readerIds.last}-article-scroll',
                ),
              )
              .hitTestable(),
          findsOneWidget,
        );
        for (var i = _readerIds.length - 1; i > 0; i--) {
          await _swipeReader(tester, _readerIds[i], forward: false, rtl: rtl);
          expect(
            find
                .byKey(
                  PageStorageKey<String>(
                    'manual-${_readerIds[i - 1]}-article-scroll',
                  ),
                )
                .hitTestable(),
            findsOneWidget,
          );
        }
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    });
  }

  testWidgets('horizontal reading preserves independent vertical positions', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 520));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_readerHarness(topic: 'controls', scale: 2));
    await tester.pumpAndSettle();
    final controls = find.byKey(
      const PageStorageKey<String>('manual-controls-article-scroll'),
    );
    final scrollable = find.descendant(
      of: controls,
      matching: find.byType(Scrollable),
    );
    final navBefore = tester.getRect(
      find.byKey(const ValueKey('manual-next-article')),
    );
    final position = tester.state<ScrollableState>(scrollable).position;
    final originalPage = tester
        .widget<PageView>(find.byType(PageView))
        .controller!
        .page;
    final rect = tester.getRect(controls);
    final gesture = await tester.startGesture(
      rect.center + const Offset(0, 60),
    );
    for (var i = 1; i <= 4; i++) {
      await gesture.moveTo(rect.center + Offset(0, 60 - 55.0 * i));
      await tester.pump(const Duration(milliseconds: 60));
    }
    await gesture.up();
    await tester.pumpAndSettle();
    final offset = position.pixels;
    expect(offset, greaterThan(100));
    expect(
      tester.widget<PageView>(find.byType(PageView)).controller!.page,
      originalPage,
    );
    expect(
      tester.getRect(find.byKey(const ValueKey('manual-next-article'))),
      navBefore,
    );
    await _swipeReader(tester, 'controls', forward: true);
    await _swipeReader(tester, 'moves', forward: false);
    expect(
      tester
          .state<ScrollableState>(
            find.descendant(of: controls, matching: find.byType(Scrollable)),
          )
          .position
          .pixels,
      moreOrLessEquals(offset),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('Duck credit is searchable and uses the bounded native launcher', (
    tester,
  ) async {
    final calls = <String>[];
    var result = true;
    const channel = MethodChannel('com.foxieproducts.litch/external_links');
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(channel, (
      call,
    ) async {
      expect(call.method, 'openUrl');
      calls.add((call.arguments as Map)['url'] as String);
      return result;
    });
    addTearDown(
      () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        channel,
        null,
      ),
    );
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(_readerHarness(topic: null));
    await tester.pumpAndSettle();
    final search = find.byKey(const ValueKey('manual-search'));
    await tester.enterText(search, 'Tim Paulden');
    await tester.pumpAndSettle();
    await _tapTopic(tester, find.byKey(const ValueKey('manual-search-duck')));
    expect(
      find.text(
        'Duck Chess was created by Dr. Tim Paulden. Duck Chess and duckchess.com are unaffiliated with Foxie Products.',
      ),
      findsOneWidget,
    );
    final link = find.byKey(const ValueKey('manual-duck-link'));
    await tester.ensureVisible(link);
    await tester.pumpAndSettle();
    expect(tester.getSize(link).height, greaterThanOrEqualTo(48));
    await tester.tap(link);
    await tester.pumpAndSettle();
    expect(calls, ['https://duckchess.com']);
    expect(find.byType(SnackBar), findsNothing);
    result = false;
    await tester.tap(link);
    await tester.pumpAndSettle();
    expect(
      find.text(lookupAppLocalizations(const Locale('en')).linkOpenFailed),
      findsOneWidget,
    );
    expect(find.byKey(const ValueKey('manual-duck-body')), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(tester.widget<TextField>(search).controller!.text, 'Tim Paulden');
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'pending Duck launches coalesce and settle safely after leaving',
    (tester) async {
      final pending = Completer<bool>();
      var calls = 0;
      const channel = MethodChannel('com.foxieproducts.litch/external_links');
      tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(channel, (
        _,
      ) {
        calls++;
        return pending.future;
      });
      addTearDown(
        () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
          channel,
          null,
        ),
      );
      await tester.pumpWidget(_readerHarness(topic: null));
      await tester.pumpAndSettle();
      await _tapTopic(
        tester,
        find.byKey(const PageStorageKey<String>('manual-duck')),
      );
      final link = find.byKey(const ValueKey('manual-duck-link'));
      await tester.ensureVisible(link);
      await tester.pumpAndSettle();
      await tester.tap(link);
      await tester.pump();
      await tester.tap(link);
      await tester.pump();
      expect(calls, 1);
      expect(find.byType(SnackBar), findsNothing);
      await tester.tap(find.byType(BackButton));
      await tester.pumpAndSettle();
      pending.complete(false);
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('manual-topic-index')), findsOneWidget);
      expect(find.byType(SnackBar), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  for (final locale in const [Locale('de'), Locale('fr'), Locale('it')]) {
    testWidgets(
      'reader controls keep equal geometry and full 200% $locale labels',
      (tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        await tester.binding.setSurfaceSize(const Size(320, 700));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        await tester.pumpWidget(
          _readerHarness(topic: 'controls', scale: 2, locale: locale),
        );
        await tester.pumpAndSettle();
        final previous = find.byKey(const ValueKey('manual-previous-article'));
        final next = find.byKey(const ValueKey('manual-next-article'));
        final firstRect = tester.getRect(previous);
        expect(firstRect.size, tester.getRect(next).size);
        expect(firstRect.height, greaterThanOrEqualTo(48));
        for (final button in [previous, next]) {
          expectReviewTextFits(
            tester,
            find.descendant(of: button, matching: find.byType(Text)),
            'Full localized reader label',
          );
        }
        await tester.tap(next);
        await tester.pumpAndSettle();
        expect(tester.getRect(previous), firstRect);
        FocusManager.instance.primaryFocus?.unfocus();
        await tester.pump();
        var reachedPrevious = false;
        for (var step = 0; step < 8 && !reachedPrevious; step++) {
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pump();
          final focused = find.byWidget(
            FocusManager.instance.primaryFocus!.context!.widget,
          );
          reachedPrevious = find
              .ancestor(of: focused, matching: previous)
              .evaluate()
              .isNotEmpty;
          for (final id in _readerIds.where((id) => id != 'moves')) {
            expect(
              find.ancestor(
                of: focused,
                matching: find.byKey(
                  PageStorageKey<String>('manual-$id-article-scroll'),
                ),
              ),
              findsNothing,
              reason: 'Cached $id reading content must not take focus.',
            );
          }
        }
        expect(reachedPrevious, isTrue);
        await tester.sendKeyEvent(LogicalKeyboardKey.enter);
        await tester.pumpAndSettle();
        expect(
          find
              .byKey(
                const PageStorageKey<String>('manual-controls-article-scroll'),
              )
              .hitTestable(),
          findsOneWidget,
          reason:
              'Keyboard focus stays on the visible Previous control, not a cached reader page.',
        );
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets('topic opens a reading route and Back restores the index', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        theme: buildLitchTheme(),
        home: const ManualScreen(),
      ),
    );
    await tester.pumpAndSettle();
    final clock = find.byKey(const PageStorageKey<String>('manual-clock'));
    await Scrollable.ensureVisible(tester.element(clock), alignment: .3);
    await tester.pumpAndSettle();
    final indexOffset = tester
        .state<ScrollableState>(find.byType(Scrollable).first)
        .position
        .pixels;
    await _tapTopic(tester, clock);
    final body = find.byKey(const ValueKey<String>('manual-clock-body'));
    expect(
      ModalRoute.of(tester.element(body))?.settings.name,
      '/manual/clock',
      reason:
          'A topic opens a normal reading page instead of expanding a tile.',
    );
    expect(find.byKey(const ValueKey<String>('manual-search')), findsNothing);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(clock, findsOneWidget);
    expect(body, findsNothing);
    expect(
      tester
          .state<ScrollableState>(find.byType(Scrollable).first)
          .position
          .pixels,
      moreOrLessEquals(indexOffset),
    );
    expect(tester.takeException(), isNull);
  });

  for (final surface in const [
    Size(390, 844),
    Size(320, 700),
    Size(844, 390),
  ]) {
    testWidgets('About whole production review at $surface', (tester) async {
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.binding.setSurfaceSize(surface);
      addTearDown(() => tester.binding.setSurfaceSize(null));
      final scale = surface.width == 390 ? 1.0 : 2.0;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          theme: buildLitchTheme(),
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: TextScaler.linear(scale)),
            child: RepaintBoundary(
              key: const ValueKey('about-whole-capture'),
              child: child!,
            ),
          ),
          home: AboutScreen(
            loadPackageInfo: () async => PackageInfo(
              appName: 'Foxie Litch',
              packageName: 'com.foxieproducts.litch_mobile',
              version: '0.5.0',
              buildNumber: '7',
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('Version 0.5.0 (7)'), findsOneWidget);
      if (surface.width == 390) {
        final manualRect = tester.getRect(
          find.byKey(const ValueKey('about-manual')),
        );
        final discoverRect = tester.getRect(find.text('Discover'));
        final productRect = tester.getRect(
          find.byKey(const ValueKey('about-product-link')),
        );
        expect(discoverRect.top, greaterThan(manualRect.bottom));
        expect(productRect.top, greaterThan(discoverRect.bottom));
      }
      await captureHealthReview(
        tester,
        'about-whole-${surface.width.toInt()}x${surface.height.toInt()}-${scale.toInt()}x',
        boundaryFinder: find.byKey(const ValueKey('about-whole-capture')),
      );
      final manual = find.byKey(const ValueKey('about-manual'));
      await tester.scrollUntilVisible(manual, 200);
      await tester.pumpAndSettle();
      expect(manual, findsOneWidget);
      expect(tester.getSize(manual).height, greaterThanOrEqualTo(48));
      expectReviewTextFits(
        tester,
        find.text('Litch Manual'),
        'About Manual title remains complete.',
      );
      await tester.tap(manual);
      await tester.pumpAndSettle();
      expect(find.byType(ManualScreen), findsOneWidget);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('adjacent reading keeps Back one step from the retained index', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: const ManualScreen(),
      ),
    );
    await tester.pumpAndSettle();
    final search = find.byKey(const ValueKey('manual-search'));
    await tester.enterText(search, '3+2');
    await tester.pumpAndSettle();
    await _tapTopic(
      tester,
      find.byKey(const PageStorageKey<String>('manual-clock')),
    );
    final next = find.byKey(const ValueKey('manual-next-article'));
    await tester.ensureVisible(next);
    await tester.pumpAndSettle();
    expect(tester.getSize(next).height, greaterThanOrEqualTo(48));
    await tester.tap(next);
    await tester.pumpAndSettle();
    final lights = find.byKey(const ValueKey('manual-lights-body'));
    expect(lights, findsOneWidget);
    final viewport = tester.getRect(
      find.byKey(const PageStorageKey<String>('manual-lights-article-scroll')),
    );
    expect(tester.getRect(lights).top, greaterThanOrEqualTo(viewport.top));
    expect(tester.getRect(lights).top, lessThan(viewport.bottom));
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(tester.widget<TextField>(search).controller!.text, '3+2');
    expect(find.byKey(const ValueKey('manual-search-clock')), findsOneWidget);
    expect(find.byKey(const ValueKey('manual-clock-body')), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('index topics support keyboard navigation', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: const ManualScreen(),
      ),
    );
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pump();
    expect(
      tester.widget<EditableText>(find.byType(EditableText)).focusNode.hasFocus,
      isTrue,
    );
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pump();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('manual-start-body')), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('manual-topic-index')), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('offline search matches the board controls recovery paragraph', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: const ManualScreen(),
      ),
    );
    await tester.pumpAndSettle();
    final strings = lookupAppLocalizations(const Locale('en'));
    expect(strings.manualControlsRecovery, contains('never replays'));
    expect(strings.manualControlsBody, isNot(contains('never replays')));
    await tester.enterText(
      find.byKey(const ValueKey('manual-search')),
      'never replays',
    );
    await tester.pumpAndSettle();
    expect(
      find.byKey(const ValueKey('manual-search-controls')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey('manual-controls-recovery')),
      findsNothing,
    );
    await _tapTopic(
      tester,
      find.byKey(const PageStorageKey<String>('manual-controls')),
    );
    expect(find.text(strings.manualControlsRecovery), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  for (final dark in [false, true]) {
    testWidgets(
      'Manual reuses Play and Vex with Vex alone in full color (${dark ? 'dark' : 'light'})',
      (WidgetTester tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        await tester.binding.setSurfaceSize(const Size(414, 896));
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final theme = dark ? buildLitchDarkTheme() : buildLitchTheme();
        await tester.pumpWidget(
          MaterialApp(
            theme: theme,
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            builder: (context, child) => RepaintBoundary(
              key: const ValueKey('manual-icon-review'),
              child: child!,
            ),
            home: const ManualScreen(),
          ),
        );
        await tester.pumpAndSettle();
        await tester.runAsync(
          () => precacheImage(
            const ResizeImage(AssetImage(HomeChessboard.duckAsset), width: 104),
            tester.element(find.byType(ManualScreen)),
          ),
        );
        await tester.pumpAndSettle();
        final boundary = find.byKey(const ValueKey('manual-icon-review'));
        await captureHealthReview(
          tester,
          'manual-canonical-${dark ? 'dark' : 'light'}-icons-overview',
          boundaryFinder: boundary,
        );
        final duckTopic = find.byKey(const PageStorageKey('manual-duck'));
        await tester.ensureVisible(duckTopic);
        await tester.pumpAndSettle();
        await captureHealthReview(
          tester,
          'manual-canonical-${dark ? 'dark' : 'light'}-icons-vex',
          boundaryFinder: boundary,
        );

        final start = find.byKey(const ValueKey('manual-start-icon'));
        expect(
          find.descendant(of: start, matching: find.byType(LitchPlayGlyph)),
          findsOneWidget,
          reason: 'Starting chess reuses the same board identity as Play.',
        );
        final craft = find.byKey(const ValueKey('manual-litchcraft-icon'));
        expect(
          find.descendant(
            of: craft,
            matching: find.byType(LitchcraftIdentityGlyph),
          ),
          findsOneWidget,
        );
        final vex = tester.widget<Image>(
          find.byKey(const ValueKey('manual-duck-icon')),
        );
        expect(
          (vex.image as ResizeImage).imageProvider,
          const AssetImage(HomeChessboard.duckAsset),
        );
        expect(
          vex.color,
          isNull,
          reason: 'Vex retains the chessboard asset colors.',
        );
        expect(
          tester.widget<ColorFiltered>(start).colorFilter,
          tester.widget<ColorFiltered>(craft).colorFilter,
        );
        expect(find.byIcon(Icons.flag_outlined), findsNothing);
        expect(find.byIcon(Icons.extension_outlined), findsNothing);
        for (final icon in tester.widgetList<Icon>(find.byType(Icon))) {
          if (icon.color != null) {
            expect(icon.color, theme.colorScheme.onSurface);
          }
        }
        await _tapTopic(tester, duckTopic);
        expect(find.byKey(const ValueKey('manual-duck-body')), findsOneWidget);
        expect(find.byKey(const ValueKey('manual-topic-index')), findsNothing);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'About, index and articles support edge-swipe back',
    (WidgetTester tester) async {
      await tester.binding.setSurfaceSize(const Size(390, 844));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            appBar: AppBar(title: const LitchAboutBrandButton()),
            body: const Text('Play'),
          ),
        ),
      );

      await tester.tap(find.byKey(const ValueKey<String>('open-about')));
      await tester.pumpAndSettle();
      expect(find.byType(AboutScreen), findsOneWidget);

      await tester.dragFrom(const Offset(1, 400), const Offset(340, 0));
      await tester.pumpAndSettle();
      expect(find.byType(AboutScreen), findsNothing);
      expect(find.text('Play'), findsOneWidget);

      await tester.tap(find.byKey(const ValueKey<String>('open-about')));
      await tester.pumpAndSettle();
      final Finder manual = find.byKey(const ValueKey<String>('about-manual'));
      await tester.scrollUntilVisible(manual, 200);
      await tester.tap(manual);
      await tester.pumpAndSettle();
      expect(find.byType(ManualScreen), findsOneWidget);
      await _tapTopic(
        tester,
        find.byKey(const PageStorageKey<String>('manual-controls')),
      );
      expect(
        find.byKey(const ValueKey('manual-controls-body')),
        findsOneWidget,
      );
      await tester.dragFrom(const Offset(1, 400), const Offset(340, 0));
      await tester.pumpAndSettle();
      expect(find.byType(ManualScreen), findsOneWidget);
      expect(find.byKey(const ValueKey('manual-controls-body')), findsNothing);

      await tester.dragFrom(const Offset(1, 400), const Offset(340, 0));
      await tester.pumpAndSettle();
      expect(find.byType(ManualScreen), findsNothing);
      expect(find.byType(AboutScreen), findsOneWidget);
    },
    variant: TargetPlatformVariant(<TargetPlatform>{
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );

  testWidgets('About opens the bundled manual without a connection', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final List<Uri> opened = <Uri>[];
    final Completer<PackageInfo> pendingMetadata = Completer<PackageInfo>();
    final strings = lookupAppLocalizations(const Locale('en'));
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: AboutScreen(
          loadPackageInfo: () => pendingMetadata.future,
          openExternalLink: (Uri uri) async {
            opened.add(uri);
            return ExternalLinkLaunchResult.opened;
          },
        ),
      ),
    );
    await tester.pumpAndSettle();

    final Finder entry = find.byKey(const ValueKey<String>('about-manual'));
    await tester.scrollUntilVisible(entry, 200);
    expect(find.text(strings.aboutDiscover), findsOneWidget);
    expect(find.text(strings.manualTitle), findsOneWidget);
    expect(tester.getSize(entry).height, greaterThanOrEqualTo(48));
    await captureHealthReview(tester, 'manual-about-discover');
    await tester.tap(entry);
    await tester.pumpAndSettle();

    expect(find.byType(ManualScreen), findsOneWidget);
    expect(find.text(strings.manualIntroduction), findsOneWidget);
    expect(find.byKey(const ValueKey('manual-topic-index')), findsOneWidget);
    expect(
      find.byKey(const PageStorageKey<String>('manual-controls')),
      findsOneWidget,
    );
    final Finder manualScrollable = find.byWidgetPredicate(
      (Widget widget) =>
          widget is Scrollable && widget.axisDirection == AxisDirection.down,
    );
    final Finder factoryReset = find.byKey(
      const PageStorageKey<String>('manual-factory-reset'),
    );
    await tester.scrollUntilVisible(
      factoryReset,
      200,
      scrollable: manualScrollable,
    );
    expect(factoryReset, findsOneWidget);
    expect(
      find.byKey(const PageStorageKey<String>('manual-factory-test')),
      findsNothing,
    );
    expect(
      find.byKey(const PageStorageKey<String>('manual-clock')),
      findsOneWidget,
    );
    expect(
      find.byKey(const PageStorageKey<String>('manual-recovery')),
      findsOneWidget,
    );
    expect(
      find.byKey(const PageStorageKey<String>('manual-lights')),
      findsOneWidget,
    );
    await captureHealthReview(tester, 'manual-en-overview');
    final Finder duck = find.byKey(const PageStorageKey<String>('manual-duck'));
    await tester.scrollUntilVisible(duck, 200, scrollable: manualScrollable);
    await _tapTopic(tester, duck);
    expect(find.text(strings.manualDuckBody), findsOneWidget);
    expect(strings.manualDuckBody, contains('four empty rows'));
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    final Finder litchcraft = find.byKey(
      const PageStorageKey<String>('manual-litchcraft'),
    );
    await tester.scrollUntilVisible(
      litchcraft,
      200,
      scrollable: manualScrollable,
    );
    await _tapTopic(tester, litchcraft);
    expect(find.text(strings.manualLitchcraftBody), findsOneWidget);
    expect(strings.manualLitchcraftBody, contains('empty Setup board'));
    expect(
      strings.manualLitchcraftBody,
      contains('Effects applies an effect to individual squares'),
    );
    expect(
      strings.manualLitchcraftBody,
      contains('Add copies the selected frame'),
    );
    expect(
      strings.manualLitchcraftBody,
      contains('$litchcraftAnimationMaximumFrameCount frames'),
    );
    expect(strings.manualLitchcraftBody, contains('confirm Delete'));
    expect(strings.manualLitchcraftBody, contains('Tap Fill to fill'));
    expect(strings.manualLitchcraftBody, contains('half a second'));
    expect(
      strings.manualLitchcraftBody,
      contains('to clear the frame instead'),
    );
    expect(strings.manualLitchcraftBody, isNot(contains('eight frames')));
    expect(strings.manualLitchcraftBody, isNot(contains('Motion')));
    await captureHealthReview(tester, 'manual-en-duck');
    expect(opened, isEmpty);
    expect(tester.takeException(), isNull);

    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byType(ManualScreen), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsOneWidget);
    expect(find.byType(ManualScreen), findsNothing);
    expect(opened, isEmpty);
  });

  testWidgets('open manual keeps its topic through a language change at 200%', (
    WidgetTester tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final ValueNotifier<Locale> locale = ValueNotifier<Locale>(
      const Locale('en'),
    );
    addTearDown(locale.dispose);
    final strings = lookupAppLocalizations(const Locale('de'));
    final Completer<PackageInfo> pendingMetadata = Completer<PackageInfo>();
    await tester.pumpWidget(
      ValueListenableBuilder<Locale>(
        valueListenable: locale,
        builder: (BuildContext context, Locale value, Widget? child) =>
            MaterialApp(
              locale: value,
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              theme: buildLitchTheme(),
              builder: (BuildContext context, Widget? child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: const TextScaler.linear(2),
                  padding: const EdgeInsets.only(bottom: 34),
                  viewPadding: const EdgeInsets.only(bottom: 34),
                ),
                child: child!,
              ),
              home: AboutScreen(loadPackageInfo: () => pendingMetadata.future),
            ),
      ),
    );
    await tester.pumpAndSettle();
    final Finder entry = find.byKey(const ValueKey<String>('about-manual'));
    await tester.scrollUntilVisible(entry, 250);
    await tester.pumpAndSettle();
    await tester.tap(entry);
    await tester.pumpAndSettle();
    final Finder start = find.byKey(
      const PageStorageKey<String>('manual-start'),
    );
    await tester.scrollUntilVisible(
      start,
      250,
      scrollable: find.byWidgetPredicate(
        (Widget widget) =>
            widget is Scrollable && widget.axisDirection == AxisDirection.down,
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.getSize(start).height, greaterThanOrEqualTo(48));
    await tester.tap(
      find.text(lookupAppLocalizations(const Locale('en')).manualStartTitle),
    );
    await tester.pumpAndSettle();

    locale.value = const Locale('de');
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('manual-start-body')), findsOneWidget);
    expect(find.text(strings.manualTitle), findsOneWidget);
    expect(find.text(strings.manualStartBody), findsOneWidget);
    expect(find.text('Start a game'), findsNothing);
    final Finder titleText = find.text(strings.manualTitle);
    final RenderParagraph title = tester.renderObject<RenderParagraph>(
      find.descendant(of: titleText, matching: find.byType(RichText)),
    );
    expect(
      title.textScaler.scale(10),
      13,
      reason: 'Compact navigation caps text scaling at 1.3',
    );
    expect(title.softWrap, isTrue);
    final TextPainter fullTitle = TextPainter(
      text: title.text,
      textDirection: title.textDirection,
      textScaler: title.textScaler,
    )..layout(maxWidth: title.size.width);
    expect(fullTitle.computeLineMetrics().length, lessThanOrEqualTo(2));
    expect(title.size.height, greaterThanOrEqualTo(fullTitle.height - .1));
    fullTitle.dispose();
    expectReviewTextFits(
      tester,
      titleText,
      'German manual page title fits at the compact chrome limit',
    );
    expectReviewTextFits(
      tester,
      find.text(strings.manualStartTitle).first,
      'German manual topic title wraps at 200%',
    );
    expectReviewTextFits(
      tester,
      find.text(strings.manualStartBody),
      'German manual paragraphs remain complete at 200%',
    );
    final Rect scaffold = tester.getRect(find.byType(Scaffold).last);
    final Rect scroll = tester.getRect(
      find.byKey(const PageStorageKey<String>('manual-start-article-scroll')),
    );
    final footer = tester.getRect(
      find.byKey(const ValueKey('manual-next-article')),
    );
    expect(scroll.bottom, lessThanOrEqualTo(footer.top));
    expect(footer.bottom, lessThanOrEqualTo(scaffold.bottom - 34));
    expect(tester.takeException(), isNull);
    await captureHealthReview(tester, 'manual-de-200-reading');

    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byType(ManualScreen), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsOneWidget);
    expect(find.text(strings.aboutTitle), findsOneWidget);
  });

  testWidgets('offline search finds body text, reveals it, and clears', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final strings = lookupAppLocalizations(const Locale('en'));
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        theme: buildLitchTheme(),
        home: const ManualScreen(),
      ),
    );
    await tester.pumpAndSettle();

    final Finder search = find.byKey(const ValueKey<String>('manual-search'));
    expect(tester.getSize(search).height, greaterThanOrEqualTo(48));
    await tester.enterText(search, 'promotion');
    await tester.pump();

    expect(
      find.byKey(const ValueKey<String>('manual-search-moves')),
      findsOneWidget,
    );
    expect(find.text(strings.manualMovesBody), findsNothing);
    await _tapTopic(
      tester,
      find.byKey(const PageStorageKey<String>('manual-moves')),
    );
    expect(find.text(strings.manualMovesBody), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(tester.widget<TextField>(search).controller?.text, 'promotion');

    await tester.enterText(search, 'not a litch topic');
    await tester.pump();
    expect(
      find.byKey(const ValueKey<String>('manual-search-empty')),
      findsOneWidget,
    );

    await tester.tap(find.byKey(const ValueKey<String>('manual-search-clear')));
    await tester.pump();
    expect(find.byKey(const ValueKey('manual-topic-index')), findsOneWidget);
    expect(tester.widget<TextField>(search).controller?.text, isEmpty);
    expect(tester.takeException(), isNull);
  });

  testWidgets('offline search reveals current clock and recovery next steps', (
    WidgetTester tester,
  ) async {
    final strings = lookupAppLocalizations(const Locale('en'));
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        theme: buildLitchTheme(),
        home: const ManualScreen(),
      ),
    );
    await tester.pumpAndSettle();
    final search = find.byKey(const ValueKey<String>('manual-search'));
    await tester.enterText(search, '3+2');
    await tester.pump();
    await _tapTopic(
      tester,
      find.byKey(const PageStorageKey<String>('manual-clock')),
    );
    expect(find.text(strings.manualClockBody), findsOneWidget);
    expect(strings.manualClockBody, contains('15+10'));
    expect(strings.manualClockBody, contains('first legal pickup'));
    expect(strings.manualClockBody, contains('does not pause'));
    expect(strings.manualClockBody, contains('one timed turn'));
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    await tester.enterText(search, 'recovery');
    await tester.pump();
    await _tapTopic(
      tester,
      find.byKey(const PageStorageKey<String>('manual-recovery')),
    );
    expect(find.text(strings.manualRecoveryBody), findsOneWidget);
    expect(strings.manualRecoveryBody, contains('Keep the pieces still'));
    expect(strings.manualRecoveryBody, contains('without deleting'));
    expect(strings.manualRecoveryBody, contains('may not survive power-off'));
    expect(
      strings.manualRecoveryBody,
      contains('Releasing either button cancels'),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('lights context opens its article and Back exits to the caller', (
    WidgetTester tester,
  ) async {
    final strings = lookupAppLocalizations(const Locale('en'));
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        theme: buildLitchTheme(),
        home: Builder(
          builder: (context) => Scaffold(
            body: TextButton(
              onPressed: () => Navigator.of(
                context,
              ).push<void>(ManualScreen.route(initialTopic: 'lights')),
              child: const Text('Board lights caller'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('Board lights caller'));
    await tester.pumpAndSettle();
    expect(find.text(strings.manualLightsBody), findsOneWidget);
    expect(find.byKey(const ValueKey('manual-topic-index')), findsNothing);
    expect(find.byKey(const ValueKey('manual-search')), findsNothing);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.text('Board lights caller'), findsOneWidget);
    expect(find.byType(ManualScreen), findsNothing);
    await tester.tap(find.text('Board lights caller'));
    await tester.pumpAndSettle();
    final next = find.byKey(const ValueKey('manual-next-article'));
    await tester.ensureVisible(next);
    await tester.pumpAndSettle();
    await tester.tap(next);
    await tester.pumpAndSettle();
    expect(find.text(strings.manualRecoveryBody), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.text('Board lights caller'), findsOneWidget);
    expect(find.byKey(const ValueKey('manual-topic-index')), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('quiet index has twelve accessible rows in reading order', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(800, 600));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final strings = lookupAppLocalizations(const Locale('en'));
    final semantics = tester.ensureSemantics();
    try {
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          theme: buildLitchTheme(),
          home: const ManualScreen(),
        ),
      );
      await tester.pumpAndSettle();
      const ids = <String>[
        'start',
        'controls',
        'moves',
        'litchcraft',
        'computer',
        'duck',
        'assistance',
        'undo',
        'clock',
        'lights',
        'recovery',
        'factory-reset',
      ];
      Rect? firstInRow;
      double previousBottom = 0;
      for (final (index, id) in ids.indexed) {
        final row = find.byKey(PageStorageKey<String>('manual-$id'));
        expect(
          row,
          findsOneWidget,
          reason: 'Every topic has exactly one entry.',
        );
        final properties = tester.widget<Semantics>(row).properties;
        expect(properties.button, isTrue);
        expect(
          properties.expanded,
          isNull,
          reason: 'Rows open pages, not accordions.',
        );
        expect(properties.selected, isNull);
        expect(find.byKey(ValueKey<String>('manual-$id-icon')), findsOneWidget);
        final rect = tester.getRect(row);
        expect(rect.height, greaterThanOrEqualTo(48));
        expect(rect.width, greaterThanOrEqualTo(48));
        if (index.isEven) {
          expect(rect.top, greaterThanOrEqualTo(previousBottom));
          firstInRow = rect;
        } else {
          expect(rect.top, firstInRow!.top);
          expect(rect.left, greaterThanOrEqualTo(firstInRow.right));
          previousBottom = rect.bottom > firstInRow.bottom
              ? rect.bottom
              : firstInRow.bottom;
        }
      }
      expect(
        find.byKey(const PageStorageKey<String>('manual-connection')),
        findsNothing,
      );
      expect(find.text(strings.manualConnectionTitle), findsNothing);
      expect(find.byType(BoardControlsGlyph), findsOneWidget);
      expect(find.byIcon(Icons.sports_esports_outlined), findsNothing);
      final clock = find.byKey(const PageStorageKey<String>('manual-clock'));
      await tester.ensureVisible(clock);
      await tester.pumpAndSettle();
      expect(
        tester.getSemantics(clock).getSemanticsData().label,
        strings.manualClockTitle,
      );
      expect(
        tester.getSemantics(clock),
        isSemantics(isButton: true, hasTapAction: true),
      );
      final clockNode = tester.getSemantics(clock);
      clockNode.owner!.performAction(clockNode.id, ui.SemanticsAction.tap);
      await tester.pumpAndSettle();
      expect(find.text(strings.manualClockBody), findsOneWidget);
      expect(find.text(strings.manualStartBody), findsNothing);
      expect(find.byKey(const ValueKey('manual-topic-index')), findsNothing);
      await tester.tap(find.byType(BackButton));
      await tester.pumpAndSettle();
      expect(clock.hitTestable(), findsOneWidget);
      expect(find.text(strings.manualClockBody), findsNothing);
      expect(tester.takeException(), isNull);
    } finally {
      semantics.dispose();
    }
  });

  for (final locale in [const Locale('en'), const Locale('de')]) {
    for (final surface in [const Size(320, 640), const Size(640, 320)]) {
      for (final scale in [1.0, 2.0]) {
        testWidgets('quiet index Manual $locale on $surface at ${scale}x', (
          WidgetTester tester,
        ) async {
          await loadHealthReviewFonts(tester, useSdkFonts: true);
          await tester.binding.setSurfaceSize(surface);
          addTearDown(() => tester.binding.setSurfaceSize(null));
          final strings = lookupAppLocalizations(locale);
          await tester.pumpWidget(
            MaterialApp(
              locale: locale,
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              theme: buildLitchTheme(),
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(
                  context,
                ).copyWith(textScaler: TextScaler.linear(scale)),
                child: RepaintBoundary(
                  key: const ValueKey('manual-quiet-index-capture'),
                  child: child!,
                ),
              ),
              home: const ManualScreen(),
            ),
          );
          await tester.pumpAndSettle();
          final capture = find.byKey(
            const ValueKey('manual-quiet-index-capture'),
          );
          final name =
              'manual-quiet-index-$locale-${surface.width.toInt()}x${surface.height.toInt()}-${scale.toInt()}x';
          await captureHealthReview(
            tester,
            '$name-overview',
            boundaryFinder: capture,
          );
          expectReviewTextFits(
            tester,
            find.text(strings.manualStartTitle),
            'Start topic wraps without truncation.',
          );
          expectReviewTextFits(
            tester,
            find.text(strings.manualControlsTitle),
            'Board controls topic wraps without truncation.',
          );
          final clock = find.byKey(
            const PageStorageKey<String>('manual-clock'),
          );
          await _tapTopic(tester, clock);
          expect(find.text(strings.manualClockBody), findsOneWidget);
          final viewport = tester.getRect(
            find.byKey(
              const PageStorageKey<String>('manual-clock-article-scroll'),
            ),
          );
          final heading = tester.getRect(
            find.text(strings.manualClockTitle).last,
          );
          final body = tester.getRect(find.text(strings.manualClockBody));
          expect(
            heading.top,
            greaterThanOrEqualTo(viewport.top),
            reason:
                'Selecting a topic must show its heading, not jump into a long paragraph.',
          );
          expect(body.top, greaterThanOrEqualTo(viewport.top));
          final paragraph = tester.renderObject<RenderParagraph>(
            find.descendant(
              of: find.text(strings.manualClockBody),
              matching: find.byType(RichText),
            ),
          );
          final painter = TextPainter(
            text: paragraph.text,
            textDirection: paragraph.textDirection,
            textScaler: paragraph.textScaler,
          )..layout(maxWidth: paragraph.size.width);
          expect(
            body.top + painter.computeLineMetrics().first.height,
            lessThanOrEqualTo(viewport.bottom),
            reason:
                'The first instruction must be visible immediately after selecting the topic.',
          );
          painter.dispose();
          expectReviewTextFits(
            tester,
            find.text(strings.manualClockTitle).first,
            'Clock article title wraps without truncation.',
          );
          expectReviewTextFits(
            tester,
            find.text(strings.manualClockBody),
            'Selected article keeps its complete paragraphs.',
          );
          await captureHealthReview(
            tester,
            '$name-clock',
            boundaryFinder: capture,
          );
          await tester.tap(find.byType(BackButton));
          await tester.pumpAndSettle();
          final search = find.byKey(const ValueKey('manual-search'));
          await tester.ensureVisible(search);
          await tester.enterText(search, '3+2');
          await tester.pumpAndSettle();
          expect(
            find.byKey(const ValueKey('manual-search-clock')),
            findsOneWidget,
          );
          expect(find.text(strings.manualClockBody), findsNothing);
          await captureHealthReview(
            tester,
            '$name-search',
            boundaryFinder: capture,
          );
          expect(tester.takeException(), isNull);
        });
      }
    }
  }

  for (final Size surface in const <Size>[Size(320, 520), Size(844, 390)]) {
    testWidgets(
      'Board controls recovery remains reachable at 200% on $surface',
      (WidgetTester tester) async {
        await loadHealthReviewFonts(tester, useSdkFonts: true);
        await tester.binding.setSurfaceSize(surface);
        addTearDown(() => tester.binding.setSurfaceSize(null));
        final strings = lookupAppLocalizations(const Locale('en'));
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            theme: buildLitchTheme(),
            builder: (BuildContext context, Widget? child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                textScaler: const TextScaler.linear(2),
                padding: const EdgeInsets.fromLTRB(24, 0, 24, 20),
                viewPadding: const EdgeInsets.fromLTRB(24, 0, 24, 20),
              ),
              child: child!,
            ),
            home: const ManualScreen(initialTopic: 'controls'),
          ),
        );
        await tester.pumpAndSettle();

        expect(strings.manualControlsBody, contains('quick palette'));
        expect(strings.manualControlsBody, contains('fine-adjusts'));
        expect(
          strings.manualControlsBody,
          contains('Use the app to select, add,'),
        );
        expect(strings.manualControlsBody, contains('and play frames'));
        expect(
          strings.manualControlsBody,
          contains('Circle for Redo'),
        );
        expect(strings.manualControlsBody, contains('shake'));
        expect(strings.manualControlsBody, contains('exit'));
        expect(strings.manualControlsRecovery, contains('Busy'));
        expect(strings.manualControlsRecovery, contains('Lift'));
        expect(strings.manualControlsRecovery, contains('never replays'));
        expect(find.text(strings.manualControlsBody.split('\n\n').first), findsOneWidget);

        final Finder recovery = find.byKey(
          const ValueKey<String>('manual-controls-recovery'),
        );
        expect(recovery, findsOneWidget);
        await Scrollable.ensureVisible(
          tester.element(recovery),
          alignment: 1,
          duration: Duration.zero,
        );
        await tester.pumpAndSettle();

        final Rect viewport = tester.getRect(
          find.byKey(
            const PageStorageKey<String>('manual-controls-article-scroll'),
          ),
        );
        final Rect recoveryRect = tester.getRect(recovery);
        expect(
          recoveryRect.height,
          lessThanOrEqualTo(viewport.height),
          reason: 'The complete recovery paragraph must fit in the viewport.',
        );
        expect(
          recoveryRect.top,
          greaterThanOrEqualTo(viewport.top),
          reason: 'The final recovery paragraph must be fully reachable.',
        );
        expect(
          recoveryRect.bottom,
          lessThanOrEqualTo(viewport.bottom),
          reason: 'Recovery must stay above the safe bottom inset.',
        );
        expectReviewTextFits(
          tester,
          recovery,
          'Board controls recovery remains complete at 200%',
        );
        expect(tester.takeException(), isNull);
      },
    );
  }
}
