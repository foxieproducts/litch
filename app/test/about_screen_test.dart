import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/cupertino.dart' show CupertinoPageRoute;
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:litch_mobile/src/services/external_link_launcher.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:package_info_plus/package_info_plus.dart';

import 'health_render_capture.dart';
import 'localization_test_support.dart';
import 'golden_test_support.dart';

// Register the SDK's real asset loader without constructing a second binding.
// The widget-test binding deliberately skips ServicesBinding.initLicenses.
class _LicenseBindingStub implements BindingBase, SchedulerBinding {
  @override
  dynamic noSuchMethod(Invocation invocation) => throw UnsupportedError(
    'Unexpected binding use: ${invocation.memberName}',
  );
}

class _SdkLicenseRegistrar extends _LicenseBindingStub with ServicesBinding {
  void register() => initLicenses();
}

void main() {
  setUp(() {
    PackageInfo.setMockInitialValues(
      appName: 'Foxie Litch',
      packageName: 'com.foxieproducts.litch_mobile',
      version: '0.5.0',
      buildNumber: '7',
      buildSignature: '',
    );
  });

  testWidgets(
    'About reuses the Play wordmark with the requested English tagline',
    (tester) async {
      final semantics = tester.ensureSemantics();
      try {
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: const AboutScreen(),
          ),
        );
        await tester.pumpAndSettle();
        final brand = find.descendant(
          of: find.byType(AboutScreen),
          matching: find.byType(LitchAboutBrandButton),
        );
        expect(
          tester.widget<LitchAboutBrandButton>(brand).interactive,
          isFalse,
        );
        final assets = tester
            .widgetList<Image>(
              find.descendant(of: brand, matching: find.byType(Image)),
            )
            .map((image) => (image.image as AssetImage).assetName);
        expect(assets, [
          'assets/branding/foxie-litch-wordmark.png',
          'assets/branding/foxie-fox.png',
        ]);
        expect(find.text('Foxie Litch'), findsNothing);
        expect(
          find.text(
            "The World's First Fully Lit Chessboard. With... a dark arts problem. Reanimate your expectations.",
          ),
          findsOneWidget,
        );
        expect(
          tester
              .getSemantics(find.byKey(const ValueKey('about-hero')))
              .getSemanticsData()
              .hasAction(ui.SemanticsAction.tap),
          isFalse,
        );
        expect(find.byKey(const ValueKey('about-version')), findsOneWidget);
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  testWidgets(
    'whole About card counts pointer, screen-reader and keyboard input once',
    (tester) async {
      final semantics = tester.ensureSemantics();
      try {
        final calls = <int>[];
        final navigator = GlobalKey<NavigatorState>();
        await tester.pumpWidget(
          MaterialApp(
            navigatorKey: navigator,
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: const Scaffold(body: Text('Canvas')),
          ),
        );
        navigator.currentState!.push(
          AboutScreen.route(
            onLoadVersionArtwork: calls.add,
            versionTapClock: tester.binding.clock.now,
          ),
        );
        await tester.pumpAndSettle();
        final hero = find.byKey(const ValueKey<String>('about-hero'));
        final card = tester.getRect(hero);
        final tagline = find.text(
          lookupAppLocalizations(const Locale('en')).aboutTagline,
        );
        final brand = find.descendant(
          of: find.byType(AboutScreen),
          matching: find.byType(LitchAboutBrandButton),
        );
        final points = <Offset>[
          card.topLeft + const Offset(16, 4),
          card.bottomRight - const Offset(16, 4),
          tester.getCenter(brand),
          tester.getCenter(tagline),
          tester.getCenter(find.byKey(const ValueKey('about-version'))),
        ];
        for (var index = 0; index < points.length; index++) {
          await tester.tapAt(points[index]);
          await tester.pump(const Duration(milliseconds: 50));
          final feedback = tester.widget<Text>(
            find.byKey(const ValueKey<String>('about-version-tap-feedback')),
          );
          expect(feedback.data, List.filled(index + 1, '•').join(' '));
          expect(
            tester.getSemantics(hero).value,
            'Taps recorded: ${index + 1}',
          );
          expect(calls, isEmpty);
        }
        final action = tester.getSemantics(hero);
        expect(
          action.getSemanticsData().hasAction(ui.SemanticsAction.tap),
          isTrue,
        );
        expect(action.label, contains('Version 0.5.0 (7)'));
        expect(
          action.label,
          contains("The World's First Fully Lit Chessboard."),
        );
        expect(
          find.descendant(of: hero, matching: find.byType(InkWell)),
          findsOneWidget,
          reason: 'The version has no second tap handler inside the card.',
        );
        action.owner!.performAction(action.id, ui.SemanticsAction.tap);
        await tester.pump();
        expect(tester.getSemantics(hero).value, 'Taps recorded: 6');
        final cardFocus = Focus.of(
          tester.element(find.byKey(const ValueKey('about-version'))),
        );
        // Traverse the actual route controls instead of calling the card action.
        FocusManager.instance.primaryFocus?.unfocus();
        await tester.pump();
        for (var step = 0; step < 2 && !cardFocus.hasPrimaryFocus; step++) {
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pump();
        }
        expect(cardFocus.hasPrimaryFocus, isTrue);
        await tester.sendKeyEvent(LogicalKeyboardKey.enter);
        await tester.pump();
        expect(tester.getSemantics(hero).value, 'Taps recorded: 7');
        expect(calls, isEmpty);
        await tester.pump(const Duration(milliseconds: 1301));
        await tester.pumpAndSettle();
        expect(calls, <int>[7]);
        expect(find.text('Canvas'), findsOneWidget);
        expect(tester.takeException(), isNull);
      } finally {
        semantics.dispose();
      }
    },
  );

  Widget buildHarness({
    List<LitchAcknowledgement> acknowledgements =
        const <LitchAcknowledgement>[],
    AboutLinkOpener? openExternalLink,
    TextScaler textScaler = TextScaler.noScaling,
    double bottomSystemInset = 0,
  }) {
    return MaterialApp(
      theme: buildLitchTheme(),
      builder: (BuildContext context, Widget? child) {
        final EdgeInsets systemPadding = EdgeInsets.only(
          bottom: bottomSystemInset,
        );
        return MediaQuery(
          data: MediaQuery.of(context).copyWith(
            textScaler: textScaler,
            padding: systemPadding,
            viewPadding: systemPadding,
          ),
          child: child!,
        );
      },
      home: AboutScreen(
        acknowledgements: acknowledgements,
        openExternalLink: openExternalLink,
      ),
    );
  }

  testWidgets(
    'About photo card fills behind full text at 200% in light and dark',
    (tester) async {
      await loadHealthReviewFonts(tester);
      await tester.binding.setSurfaceSize(const Size(320, 700));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      for (final theme in <ThemeData>[
        buildLitchTheme(),
        buildLitchDarkTheme(),
      ]) {
        await tester.pumpWidget(
          MaterialApp(
            theme: theme,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: const AboutScreen(),
          ),
        );
        await tester.pumpAndSettle();
        final hero = find.byKey(const ValueKey<String>('about-hero'));
        final photo = find.descendant(
          of: hero,
          matching: find.byWidgetPredicate(
            (widget) =>
                widget is Image &&
                widget.image is AssetImage &&
                (widget.image as AssetImage).assetName ==
                    'assets/images/foxie_planet.jpeg',
          ),
        );
        expect(photo, findsOneWidget);
        expect(tester.widget<Image>(photo).fit, BoxFit.cover);
        expect(tester.getRect(photo), tester.getRect(hero));
        for (final text in <Finder>[
          find.text(lookupAppLocalizations(const Locale('en')).aboutTagline),
          find.byKey(const ValueKey('about-version')),
        ]) {
          expectReviewTextFits(tester, text, 'Photo-card reading text wraps.');
          expect(tester.widget<Text>(text).style?.color, Colors.white);
          expect(
            tester.getRect(text).left,
            greaterThan(tester.getRect(hero).left),
          );
          expect(
            tester.getRect(text).right,
            lessThan(tester.getRect(hero).right),
          );
          expect(
            tester.getRect(text).bottom,
            lessThan(tester.getRect(hero).bottom),
          );
        }
        expect(tester.takeException(), isNull);
        await decodeVisibleReviewImages(tester);
        await captureHealthReview(
          tester,
          'about-photo-200-${theme.brightness.name}',
        );
      }
    },
  );

  testWidgets('keeps About content above platform navigation', (
    WidgetTester tester,
  ) async {
    const double bottomSystemInset = 34;
    await tester.pumpWidget(buildHarness(bottomSystemInset: bottomSystemInset));
    await tester.pumpAndSettle();

    final Rect scaffold = tester.getRect(find.byType(Scaffold));
    final Rect scrollView = tester.getRect(
      find.byKey(const ValueKey<String>('about-scroll-view')),
    );
    expect(
      scrollView.bottom,
      moreOrLessEquals(scaffold.bottom - bottomSystemInset),
    );
  });

  testWidgets('card feedback and Manual remain available when metadata fails', (
    tester,
  ) async {
    final calls = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: AboutScreen(
          loadPackageInfo: () async => throw StateError('offline metadata'),
          onLoadVersionArtwork: calls.add,
          versionTapClock: tester.binding.clock.now,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('about-version')), findsNothing);
    await tester.tap(find.byKey(const ValueKey('about-hero')));
    await tester.pump();
    expect(
      tester
          .widget<Text>(
            find.byKey(const ValueKey('about-version-tap-feedback')),
          )
          .data,
      '•',
    );
    final manual = find.byKey(const ValueKey('about-manual'));
    await tester.ensureVisible(manual);
    await tester.pumpAndSettle();
    expect(manual, findsOneWidget);
    await tester.tap(manual);
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('manual-topic-index')), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 1301));
    expect(calls, isEmpty);
    expect(
      find.byKey(const ValueKey('about-version-tap-feedback')),
      findsNothing,
    );
    expect(tester.takeException(), isNull);
  });

  for (final fails in [false, true]) {
    testWidgets(
      'late package metadata after leaving About is harmless ($fails)',
      (tester) async {
        final metadata = Completer<PackageInfo>();
        await tester.pumpWidget(
          MaterialApp(
            theme: buildLitchTheme(),
            home: Builder(
              builder: (context) => Scaffold(
                body: TextButton(
                  onPressed: () => Navigator.of(context).push<void>(
                    CupertinoPageRoute<void>(
                      builder: (_) =>
                          AboutScreen(loadPackageInfo: () => metadata.future),
                    ),
                  ),
                  child: const Text('Open About'),
                ),
              ),
            ),
          ),
        );
        await tester.tap(find.text('Open About'));
        await tester.pumpAndSettle();
        expect(find.byKey(const ValueKey('about-manual')), findsOneWidget);
        await tester.tap(find.byType(BackButton));
        await tester.pumpAndSettle();
        if (fails) {
          metadata.completeError(StateError('metadata failed after exit'));
        } else {
          metadata.complete(
            PackageInfo(
              appName: 'Foxie Litch',
              packageName: 'com.foxieproducts.litch_mobile',
              version: '0.5.0',
              buildNumber: '7',
            ),
          );
        }
        await tester.pumpAndSettle();
        expect(find.text('Open About'), findsOneWidget);
        expect(find.byType(AboutScreen), findsNothing);
        expect(tester.takeException(), isNull);
      },
    );
  }

  Future<void> scrollIntoView(WidgetTester tester, Finder target) async {
    final Finder scrollable = find.byType(Scrollable).first;
    await tester.scrollUntilVisible(target, 240, scrollable: scrollable);
    await tester.pumpAndSettle();
  }

  testWidgets('brand artwork remains one 48 px keyboard and About action', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final SemanticsHandle semantics = tester.ensureSemantics();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildLitchTheme(),
        home: RepaintBoundary(
          child: Scaffold(appBar: AppBar(title: const LitchAboutBrandButton())),
        ),
      ),
    );
    await tester.pumpAndSettle();

    final Finder aboutAction = find.bySemanticsLabel('About Foxie Litch');
    expect(aboutAction, findsOneWidget);
    expect(
      tester.getSemantics(aboutAction),
      isSemantics(isButton: true, hasTapAction: true),
    );
    expect(
      tester.getSemantics(aboutAction).rect.height,
      greaterThanOrEqualTo(48),
    );
    expect(
      tester.getSemantics(aboutAction).rect.width,
      greaterThanOrEqualTo(48),
    );
    final Iterable<Image> artwork = tester.widgetList<Image>(
      find.descendant(
        of: find.byType(LitchAboutBrandButton),
        matching: find.byType(Image),
      ),
    );
    expect(
      artwork.map((image) => (image.image as AssetImage).assetName),
      <String>[
        'assets/branding/foxie-litch-wordmark.png',
        'assets/branding/foxie-fox.png',
      ],
    );
    expect(artwork.every((image) => image.excludeFromSemantics), isTrue);
    expect(artwork.map((image) => image.height), <double>[23, 26.3]);
    await decodeVisibleReviewImages(tester);
    await captureHealthReview(tester, 'brand-fox-larger');

    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pump();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsOneWidget);
    expect(find.text('About'), findsOneWidget);
    expect(find.byType(BackButton), findsOneWidget);
    await tester.tap(find.byType(BackButton));
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsNothing);
    await tester.tap(aboutAction);
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsOneWidget);
    expect(tester.takeException(), isNull);
    semantics.dispose();
  });

  testWidgets(
    'Litchcraft uses its native wordmark at the normal header height',
    (WidgetTester tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          home: Scaffold(
            appBar: AppBar(
              title: const LitchAboutBrandButton(litchcraft: true),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final artwork = tester.widgetList<Image>(
        find.descendant(
          of: find.byType(LitchAboutBrandButton),
          matching: find.byType(Image),
        ),
      );
      expect(
        artwork.map((image) => (image.image as AssetImage).assetName),
        <String>[
          'assets/branding/foxie-litchcraft-wordmark.png',
          'assets/branding/foxie-fox.png',
        ],
      );
      expect(artwork.map((image) => image.height), <double>[23, 26.3]);
    },
  );

  testWidgets('shows version and opens the two exact external links', (
    WidgetTester tester,
  ) async {
    final List<Uri> opened = <Uri>[];
    await tester.pumpWidget(
      buildHarness(
        openExternalLink: (Uri uri) async {
          opened.add(uri);
          return ExternalLinkLaunchResult.opened;
        },
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Version 0.5.0 (7)'), findsOneWidget);
    final Finder productLink = find.byKey(
      const ValueKey<String>('about-product-link'),
    );
    await scrollIntoView(tester, productLink);
    await tester.tap(productLink);
    await tester.pump();
    final Finder githubLink = find.byKey(
      const ValueKey<String>('about-github-link'),
    );
    await scrollIntoView(tester, githubLink);
    expect(find.textContaining('github.com/foxieproducts'), findsOneWidget);
    await tester.tap(githubLink);
    await tester.pump();

    expect(opened, <Uri>[
      LitchExternalLinks.product,
      LitchExternalLinks.github,
    ]);
  });

  testWidgets('reports an external link failure without leaving About', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      buildHarness(
        openExternalLink: (Uri uri) async =>
            ExternalLinkLaunchResult.unavailable,
      ),
    );
    await tester.pumpAndSettle();

    final Finder productLink = find.byKey(
      const ValueKey<String>('about-product-link'),
    );
    await scrollIntoView(tester, productLink);
    await tester.tap(productLink);
    await tester.pumpAndSettle();

    expect(find.byType(AboutScreen), findsOneWidget);
    expect(
      find.text('That link could not open. Try again from your browser.'),
      findsOneWidget,
    );
  });

  testWidgets('opens the bundled project and license registry', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final String nativeNotices = File(
      'assets/licenses/native_runtime_notices.txt',
    ).readAsStringSync();
    expect(nativeNotices, isNotEmpty);
    expect(nativeNotices.contains('MaterialIcons'), isTrue);
    expect(nativeNotices.contains('SwiftProtobuf'), isTrue);
    expect(nativeNotices.toLowerCase().contains('rxandroidble'), isTrue);
    final ByteData bundled = await rootBundle.load('NOTICES.Z');
    final String bundledNotices = utf8.decode(
      gzip.decode(
        bundled.buffer.asUint8List(
          bundled.offsetInBytes,
          bundled.lengthInBytes,
        ),
      ),
    );
    expect(
      bundledNotices.contains(nativeNotices),
      isTrue,
      reason: 'The reviewed notice supplement must reach NOTICES.Z unchanged.',
    );

    LicenseRegistry.reset();
    addTearDown(LicenseRegistry.reset);
    _SdkLicenseRegistrar().register();
    final List<LicenseEntry> entries = (await tester.runAsync(
      () => LicenseRegistry.licenses.toList(),
    ))!;
    final List<LicenseEntryWithLineBreaks> texts = entries
        .cast<LicenseEntryWithLineBreaks>();
    for (final (String label, String path) in <(String, String)>[
      ('Litch', '../LICENSE'),
      if (File('../LICENSE-HARDWARE').existsSync())
        ('Litch hardware designs', '../LICENSE-HARDWARE'),
      if (File('../LICENSE-DOCUMENTATION').existsSync())
        ('Litch documentation and images', '../LICENSE-DOCUMENTATION'),
    ]) {
      final Iterable<LicenseEntryWithLineBreaks> grants = texts.where(
        (entry) => entry.packages.contains(label),
      );
      expect(grants, hasLength(1), reason: '$label must appear exactly once.');
      expect(grants.single.text, File(path).readAsStringSync());
    }

    const String evidence = '../tools/release/license_evidence';
    for (final (String label, String path) in <(String, String)>[
      (
        'MaterialIcons ',
        '$evidence/flutter-sdk/3.41.2/material-icons/MaterialIcons_LICENSE.txt',
      ),
      (
        'MaterialIcons ',
        '$evidence/flutter-sdk/3.41.2/material-icons/ATTRIBUTION.txt',
      ),
      (
        'SwiftProtobuf ',
        '$evidence/cocoapods/SwiftProtobuf-1.35.1/LICENSE.txt',
      ),
      (
        'com.polidea.rxandroidble2:rxandroidble ',
        '$evidence/maven/terms/Apache-2.0.txt',
      ),
      (
        'Board firmware — Arduino core and source libraries',
        '$evidence/platformio-framework/arduino-3.20017.241212+sha.dcc1105b/arduino/LICENSE.md',
      ),
      (
        'Board firmware — GCC runtime',
        '$evidence/platformio-runtime/espressif__toolchain-xtensa-esp32s3-8.4.0+2021r2-patch5/gcc/COPYING.RUNTIME',
      ),
      (
        'Board firmware — makuna/NeoPixelBus 2.8.4',
        '$evidence/platformio-lib/makuna__NeoPixelBus-2.8.4/COPYING',
      ),
    ]) {
      final String canonical = File(path)
          .readAsStringSync()
          .replaceAll('\r\n', '\n')
          .replaceAll('\r', '\n')
          .trim();
      expect(
        texts.any(
          (entry) =>
              entry.packages.any((package) => package.startsWith(label)) &&
              entry.text.trim() == canonical,
        ),
        isTrue,
        reason: '$label must retain the complete text from $path.',
      );
    }
    for (final (String package, String term) in <(String, String)>[
      ('flutter', 'Copyright 2014 The Flutter Authors'),
      ('fallback_root_certificates', 'Mozilla Public License Version 2.0'),
      ('icu', 'UNICODE LICENSE V3'),
      ('libcxx', 'Boost Software License - Version 1.0'),
      ('libcxx', 'LLVM Exceptions to the Apache 2.0 License'),
      ('zlib', 'This software is provided'),
    ]) {
      expect(
        texts.any(
          (entry) =>
              entry.packages.contains(package) && entry.text.contains(term),
        ),
        isTrue,
        reason: 'Flutter must still supply $package and its $term notice.',
      );
    }

    // Reuse the real parser's entries for UI pumps under the test's fake clock;
    // the production collector's background isolates were awaited above.
    LicenseRegistry.reset();
    LicenseRegistry.addLicense(
      () => Stream<LicenseEntry>.fromIterable(entries),
    );
    await tester.pumpWidget(buildHarness());
    await tester.pumpAndSettle();

    final Finder licenses = find.byKey(
      const ValueKey<String>('about-licenses'),
    );
    await scrollIntoView(tester, licenses);
    await tester.tap(licenses);
    await tester.pumpAndSettle();

    expect(find.byType(LicensePage), findsOneWidget);
    expect(find.text('Licenses'), findsOneWidget);
    expect(
      find.text(
        'Software licenses and notices for the app and board firmware.',
      ),
      findsOneWidget,
    );

    for (final (String prefix, String visibleText) in <(String, String)>[
      ('Litch', 'Litch original software'),
      if (File('../LICENSE-HARDWARE').existsSync())
        ('Litch hardware designs', 'Litch hardware design'),
      ('Litch licensing overview', 'Mobile app'),
      (
        'Board firmware — Arduino core and source libraries',
        'GNU LESSER GENERAL PUBLIC LICENSE',
      ),
      ('MaterialIcons ', 'Attribution 4.0 International'),
      ('SwiftProtobuf ', 'Apache License'),
      ('fallback_root_certificates', 'Mozilla Public License Version 2.0'),
    ]) {
      final String package = entries
          .expand((entry) => entry.packages)
          .firstWhere(
            (package) => prefix.endsWith(' ')
                ? package.startsWith(prefix)
                : package == prefix,
          );
      tester
          .state<ScrollableState>(find.byType(Scrollable).first)
          .position
          .jumpTo(0);
      await tester.pump();
      await tester.scrollUntilVisible(
        find.text(package),
        480,
        scrollable: find.byType(Scrollable).first,
        maxScrolls: 100,
      );
      await tester.pumpAndSettle();
      await tester.tap(find.text(package));
      await tester.pumpAndSettle();
      expect(
        find.textContaining(visibleText, findRichText: true),
        findsWidgets,
        reason: '$package must expose its notice when selected from About.',
      );
      await tester.pageBack();
      await tester.pumpAndSettle();
    }
    expect(tester.takeException(), isNull);
  });

  testWidgets('lazily scrolls a long personal acknowledgements list at 200%', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final List<LitchAcknowledgement> acknowledgements = List.generate(
      80,
      (int index) => LitchAcknowledgement(
        name: 'Person $index',
        note: 'A thoughtful contribution to Litch number $index.',
      ),
    );
    await tester.pumpWidget(
      buildHarness(
        acknowledgements: acknowledgements,
        textScaler: const TextScaler.linear(2),
      ),
    );
    await tester.pumpAndSettle();

    final semantics = tester.ensureSemantics();
    try {
      final scrollable = find.byType(Scrollable).first;
      final viewport = tester.getRect(
        find.byKey(const ValueKey<String>('about-scroll-view')),
      );
      expect(viewport.width, 320);
      expect(viewport.bottom, 700);
      expect(tester.state<ScrollableState>(scrollable).position.pixels, 0);
      expect(find.text('Person 79'), findsNothing);

      final Finder licenses = find.byKey(
        const ValueKey<String>('about-licenses'),
      );
      await scrollIntoView(tester, licenses);
      expect(licenses, findsOneWidget);
      final firstPerson = find.text('Person 0');
      // A lazy sliver can cache its first child below the viewport. Require
      // real scrolling into view before ensureVisible positions that child.
      await scrollIntoView(tester, firstPerson.hitTestable());
      expect(firstPerson.hitTestable(), findsOneWidget);
      expect(
        tester.getRect(firstPerson).top,
        greaterThanOrEqualTo(viewport.top),
      );
      expect(
        tester.getRect(firstPerson).bottom,
        lessThanOrEqualTo(viewport.bottom),
      );
      expect(tester.getSemantics(firstPerson).label, contains('Person 0'));
      expectReviewTextFits(
        tester,
        firstPerson,
        'The first person is readable.',
      );

      final lastPerson = find.text('Person 79');
      await tester.scrollUntilVisible(
        lastPerson.hitTestable(),
        600,
        scrollable: scrollable,
        maxScrolls: 100,
      );
      await tester.pumpAndSettle();

      expect(lastPerson.hitTestable(), findsOneWidget);
      expect(
        tester.getRect(lastPerson).top,
        greaterThanOrEqualTo(viewport.top),
      );
      expect(
        tester.getRect(lastPerson).bottom,
        lessThanOrEqualTo(viewport.bottom),
      );
      expect(tester.getSemantics(lastPerson).label, contains('Person 79'));
      expectReviewTextFits(tester, lastPerson, 'The last person is readable.');
      expect(tester.takeException(), isNull);
    } finally {
      semantics.dispose();
    }
  });

  testWidgets('matches the intended 390 by 844 About layout', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(390, 844));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(buildHarness());
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);

    await expectLater(
      find.byType(AboutScreen),
      matchesGoldenFile(hostGoldenPath('about_screen_390x844.png')),
    );
  });

  testWidgets('matches the intended 320 px About layout at 200% text', (
    WidgetTester tester,
  ) async {
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    await tester.pumpWidget(
      buildHarness(textScaler: const TextScaler.linear(2)),
    );
    await tester.pumpAndSettle();
    await decodeVisibleReviewImages(tester);

    await expectLater(
      find.byType(AboutScreen),
      matchesGoldenFile(hostGoldenPath('about_screen_320_200.png')),
    );
  });
}

// Registered in localization_screens_test.dart so SDK fonts cannot alter
// the existing Ahem goldens or interaction fixtures in this file.
void registerAboutLocalizationTests() {
  testWidgets(
    'About compact header caps text and wraps German title',
    (tester) async {
      await prepareLocalizationReview(tester);
      tester.view.physicalSize = const Size(240, 700);
      final strings = reviewStrings(const Locale('de'));
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('de'),
          scale: 2,
          home: Scaffold(appBar: AppBar(title: const LitchAboutBrandButton())),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey<String>('open-about')));
      await tester.pumpAndSettle();
      final title = find.text(strings.aboutTitle);
      final paragraph = tester.renderObject<RenderParagraph>(
        find.descendant(of: title, matching: find.byType(RichText)),
      );
      expect(
        paragraph.textScaler.scale(1),
        1.3,
        reason:
            'Compact About controls cap text scaling at 1.3 while the reading text follows the system setting.',
      );
      final fullTitle = TextPainter(
        text: paragraph.text,
        textDirection: paragraph.textDirection,
        textScaler: paragraph.textScaler,
      )..layout(maxWidth: paragraph.size.width);
      expect(fullTitle.computeLineMetrics().length, lessThanOrEqualTo(2));
      expect(
        paragraph.size.height,
        greaterThanOrEqualTo(fullTitle.height - .1),
      );
      fullTitle.dispose();
      expectReviewTextFits(
        tester,
        title,
        'The complete German About title fits at the compact header limit.',
      );
      await captureHealthReview(tester, 'about-requested-200-de-240x700');
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  for (final locale in localizationReviewLocales) {
    for (final scale in <double>[1, 2]) {
      testWidgets(
        'localization About $locale at ${scale * 100}% text',
        (tester) async {
          await prepareLocalizationReview(tester);
          final strings = reviewStrings(locale);
          await tester.pumpWidget(
            localizationHarness(
              locale: locale,
              scale: scale,
              home: Scaffold(
                appBar: AppBar(title: const LitchAboutBrandButton()),
              ),
            ),
          );
          await tester.pumpAndSettle();
          await tester.tap(find.byKey(const ValueKey<String>('open-about')));
          await tester.pumpAndSettle();
          expect(find.byType(BackButton), findsOneWidget);
          await decodeVisibleReviewImages(tester);
          expect(find.text(strings.aboutTitle), findsOneWidget);
          expect(
            tester
                .widget<LitchAboutBrandButton>(
                  find.descendant(
                    of: find.byType(AboutScreen),
                    matching: find.byType(LitchAboutBrandButton),
                  ),
                )
                .interactive,
            isFalse,
          );
          final mark = find.descendant(
            of: find.byType(AboutScreen),
            matching: find.byWidgetPredicate((widget) {
              if (widget is! Image) return false;
              final provider = widget.image;
              final asset = provider is ResizeImage
                  ? provider.imageProvider
                  : provider;
              return asset is AssetImage &&
                  asset.assetName == 'assets/branding/foxie-app-icon.png';
            }),
          );
          expect(mark, findsOneWidget);
          expect(tester.getSize(mark), const Size(48, 48));
          expect(tester.takeException(), isNull);
          await captureHealthReview(
            tester,
            'locale-$locale-${scale.toInt()}x-ios-about',
          );
          await tester.scrollUntilVisible(
            find.byKey(const ValueKey<String>('about-licenses')),
            300,
          );
          expect(find.text(strings.aboutLicenses), findsOneWidget);
          expect(find.text(strings.aboutOpenSourceBody), findsOneWidget);
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
        },
        variant: TargetPlatformVariant.only(TargetPlatform.iOS),
      );
    }
  }
  testWidgets(
    'localization owner regression: About follows German and opens privacy and credits',
    (tester) async {
      // Each platform variant runs in a separate fake clock.
      rootBundle.evict('assets/privacy/foxie_litch_privacy_policy.md');
      await tester.binding.setSurfaceSize(const Size(320, 700));
      addTearDown(() => tester.binding.setSurfaceSize(null));
      await loadHealthReviewFonts(tester, useSdkFonts: true);
      await tester.pumpWidget(
        localizationHarness(
          locale: const Locale('de'),
          scale: 2,
          home: const AboutScreen(),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('About'), findsNothing);
      expect(find.text('Über Litch'), findsOneWidget);
      expect(
        find.text(lookupAppLocalizations(const Locale('de')).aboutTagline),
        findsOneWidget,
      );
      await tester.scrollUntilVisible(
        find.byKey(const ValueKey<String>('about-licenses')),
        300,
      );
      expect(find.text('Open-Source-Projekte und Lizenzen'), findsOneWidget);
      expect(
        find.text(
          lookupAppLocalizations(const Locale('de')).aboutOpenSourceBody,
        ),
        findsOneWidget,
      );
      final privacy = find.byKey(
        const ValueKey<String>('about-privacy-policy'),
      );
      const links = MethodChannel('com.foxieproducts.litch/external_links');
      final messenger = tester.binding.defaultBinaryMessenger;
      final browserCalls = <MethodCall>[];
      messenger.setMockMethodCallHandler(links, (call) async {
        browserCalls.add(call);
        return false;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(links, null));
      await tester.scrollUntilVisible(privacy, 300);
      expect(tester.getSemantics(privacy).label, contains('auf Englisch'));
      await tester.tap(privacy);
      await tester.pumpAndSettle();
      expect(find.text('Datenschutzerklärung'), findsOneWidget);
      expect(
        tester
            .getSemantics(find.text('Foxie Litch App Privacy Policy'))
            .getSemanticsData()
            .headingLevel,
        1,
      );
      final section = find.text('Connecting to your board');
      await tester.ensureVisible(section);
      expect(tester.getSemantics(section).getSemanticsData().headingLevel, 2);
      final contact = find.textContaining('support@foxieproducts.com');
      await tester.ensureVisible(contact);
      expect(contact.hitTestable(), findsOneWidget);
      expect(
        find.ancestor(of: contact, matching: find.byType(SelectionArea)),
        findsOneWidget,
      );
      final online = find.text('Datenschutzerklärung online lesen');
      await tester.ensureVisible(online);
      final onlineSemantics = tester.getSemantics(online).getSemanticsData();
      expect(onlineSemantics.flagsCollection.isLink, isTrue);
      expect(onlineSemantics.hasAction(ui.SemanticsAction.tap), isTrue);
      expect(browserCalls, isEmpty);
      await tester.tap(online);
      await tester.pumpAndSettle();
      expect(browserCalls.single.method, 'openUrl');
      expect(browserCalls.single.arguments, {
        'url': 'https://foxieproducts.com/policies/privacy-policy',
      });
      expect(
        find.text(lookupAppLocalizations(const Locale('de')).linkOpenFailed),
        findsOneWidget,
      );
      await tester.tap(find.byType(BackButton));
      await tester.pumpAndSettle();
      expect(privacy.hitTestable(), findsOneWidget);
      await tester.scrollUntilVisible(
        find.text(lookupAppLocalizations(const Locale('de')).aboutThanksBody),
        300,
      );
      expect(
        find.text(lookupAppLocalizations(const Locale('de')).aboutThanksBody),
        findsOneWidget,
      );
      await tester.scrollUntilVisible(
        find.byKey(const ValueKey<String>('about-licenses')).hitTestable(),
        -200,
      );
      await tester.tap(find.byKey(const ValueKey<String>('about-licenses')));
      await tester.pumpAndSettle();
      expect(find.byType(LicensePage), findsOneWidget);
      expect(
        find.text(
          lookupAppLocalizations(const Locale('de')).aboutLicenseNotice,
        ),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    },
    variant: const TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
  );
}
