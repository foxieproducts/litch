import 'support/memory_ota_target_store.dart';
import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _NoopBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _ResolvingController extends LitchcraftController {
  _ResolvingController(super.bleService);

  @override
  bool get authorityResolving => true;
}

void main() {
  setUp(() {
    final view =
        TestWidgetsFlutterBinding.instance.platformDispatcher.implicitView!;
    view.devicePixelRatio = 1;
    view.physicalSize = const Size(390, 844);
    addTearDown(view.reset);
    SharedPreferences.setMockInitialValues({});
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'test.litch',
      version: '1.0.0',
      buildNumber: '1',
      buildSignature: '',
    );
  });

  testWidgets(
    'three version taps open 256 autoplay-ready editable frames once',
    (tester) async {
      final ble = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: _NoopBle(),
      );
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await controller.fillAllWithColor(0x123456);
      final original = library.animationDocument;
      var loads = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildLitchTheme(),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            appBar: AppBar(
              title: LitchAboutBrandButton(
                onLoadVersionArtwork: (taps) {
                  loads++;
                  library.loadBouncingBallsBenchmark();
                },
              ),
            ),
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.longPress(find.byKey(const ValueKey('craft-palette')));
      await tester.pumpAndSettle();
      expect(find.byType(AlertDialog), findsNothing);
      expect(library.animationDocument, same(original));
      await tester.tap(find.byKey(const ValueKey('open-about')));
      await tester.pumpAndSettle();
      final version = find.byKey(const ValueKey('about-version'));
      for (var i = 0; i < 2; i++) {
        await tester.tap(version);
        await tester.pump();
        expect(loads, 0);
        expect(library.animationDocument, same(original));
        expect(find.byType(AlertDialog), findsNothing);
      }
      await tester.tap(version);
      await tester.pump();
      expect(find.byType(AboutScreen), findsOneWidget);
      expect(loads, 0);
      expect(library.animationDocument, same(original));
      await tester.pump(const Duration(milliseconds: 1301));
      await tester.pumpAndSettle();
      expect(loads, 1);
      expect(find.byType(AboutScreen), findsNothing);
      expect(find.byType(AlertDialog), findsNothing);
      expect(find.byType(LitchcraftCanvasView), findsOneWidget);
      expect(find.byKey(const ValueKey('craft-surprise-canvas')), findsNothing);
      expect(
        find.byKey(const ValueKey('craft-return-to-artwork')),
        findsNothing,
      );
      expect(library.animationDocument.frames, hasLength(256));
      expect(library.activeName, isNot('Bouncing Balls'));
      expect(library.animationDocument.playbackOnEntry, isTrue);
      expect(controller.animationPlaying, isFalse);
      expect(library.previousDraft!.document, same(original));
      await tester.tap(find.byKey(const ValueKey('craft-animate')));
      await tester.pump();
      expect(find.byKey(const ValueKey('craft-preview')), findsOneWidget);
      library.selectFrame(255);
      await tester.pump();
      final before = library.animationDocument.frames[255];
      controller.paintSquares([0]);
      await tester.pump();
      expect(library.animationDocument.frames[255], isNot(same(before)));
      expect(library.animationDocument.frames, hasLength(256));
      await tester.tap(find.byKey(const ValueKey('open-about')));
      await tester.pumpAndSettle();
      await tester.tap(version);
      await tester.pump();
      expect(loads, 1, reason: 'A fresh About page starts with no prior taps.');
      await tester.pumpWidget(const SizedBox());
      await library.flush();
      library.dispose();
      controller.dispose();
      await ble.dispose();
    },
  );

  testWidgets('eight slower version taps load RR without a stray canvas tap', (
    tester,
  ) async {
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await controller.fillAllWithColor(0x123456);
    final original = library.animationDocument;
    var bounceLoads = 0;
    var rrLoads = 0;
    var tapTime = DateTime.utc(2026, 9, 28);
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          appBar: AppBar(
            title: Builder(
              builder: (context) => TextButton(
                onPressed: () => Navigator.of(context).push<void>(
                  AboutScreen.route(
                    versionTapClock: () => tapTime,
                    onLoadVersionArtwork: (taps) {
                      if (taps == 3) {
                        bounceLoads++;
                        return;
                      }
                      rrLoads++;
                      library.loadRickroll();
                    },
                  ),
                ),
                child: const Text('About'),
              ),
            ),
          ),
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.text('About'));
    await tester.pumpAndSettle();
    final version = find.byKey(const ValueKey('about-version'));
    for (var tap = 1; tap <= 8; tap++) {
      tapTime = tapTime.add(const Duration(milliseconds: 450));
      await tester.tap(version);
      await tester.pump();
      expect(
        find.byKey(const ValueKey('about-version-tap-feedback')),
        findsOneWidget,
      );
      expect(bounceLoads, 0);
      if (tap < 8) {
        expect(rrLoads, 0);
        expect(library.animationDocument, same(original));
      }
    }
    expect(find.byType(AboutScreen), findsOneWidget);
    for (var extra = 0; extra < 2; extra++) {
      tapTime = tapTime.add(const Duration(milliseconds: 450));
      await tester.tap(version);
      await tester.pump();
    }
    expect(library.animationDocument, same(original));
    await tester.pump(const Duration(milliseconds: 1301));
    await tester.pumpAndSettle();
    expect(find.byType(AboutScreen), findsNothing);
    expect(rrLoads, 1);
    expect(bounceLoads, 0);
    expect(library.animationDocument.frames, hasLength(32));
    expect(library.previousDraft!.document, same(original));

    await tester.pumpWidget(const SizedBox());
    await library.flush();
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });

  testWidgets('unresolved authority keeps local editor enabled after 250 ms', (
    tester,
  ) async {
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = _ResolvingController(ble);
    final library = LitchcraftLibrary(controller);
    await library.ready;
    library.dismissedWelcome = true;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pump(const Duration(seconds: 2));
    final canvas = tester.widget<LitchcraftCanvasView>(
      find.byType(LitchcraftCanvasView),
    );
    expect(canvas.enabled, isTrue);
    expect(find.text('Preparing board…'), findsNothing);
    expect(find.byType(CircularProgressIndicator), findsNothing);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  });
}
