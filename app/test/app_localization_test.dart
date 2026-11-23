import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/main.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/engine/engine_runtime.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/game_library_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/services/game_library_repository.dart';
import 'package:litch_mobile/src/services/game_library_store.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/post_game_review_analyzer.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:litch_mobile/src/widgets/post_game_review_analyzer_scope.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'post_game_review_test_fixture.dart';
import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';
import 'support/settings_session_board.dart';

void main() {
  test('Litchcraft and move guidance do not promise a D-pad center action', () {
    const keys = <String>{
      'craftBoardExitControls',
      'promotionGuidance',
      'manualMovesBody',
      'manualLitchcraftBody',
      'craftControlsHelp',
      'craftControlsFull',
      'craftControlsSimple',
      'craftExitFailed',
      'craftBoardPaletteControls',
    };
    // The wide tablet tray legitimately puts LEDs and Effects in the middle.
    // Look for a named center control or an instruction to press it.
    final forbidden = RegExp(
      r'\b(?:center|centre|central) (?:button|key|control)|'
      r'\b(?:d[- ]pad|steuerkreuz)[ -](?:center|centre|mitte)|'
      r'\b(?:mitteltaste|mittlere[nr]? taste|botón central|bouton central|'
      r'(?:pulsante|tasto) centrale)|'
      r'\b(?:press|tap|hold|drücke|halte|tippe|pulsa|pulse|presiona|'
      r'appuie|appuyez|premi|tieni premuto) (?:the |die |auf die |el |le |sur le |il )?'
      r'(?:center|centre|mitte|centro)\b',
      caseSensitive: false,
    );
    for (final file in Directory('lib/l10n').listSync().whereType<File>()) {
      if (!file.path.endsWith('.arb')) continue;
      final catalog =
          jsonDecode(file.readAsStringSync()) as Map<String, dynamic>;
      for (final key in keys) {
        expect(
          catalog[key],
          isNot(contains(forbidden)),
          reason: '$key in ${file.path} must describe only available controls',
        );
      }
    }
  });

  testWidgets('opens one app-lifetime game library without blocking BLE', (
    tester,
  ) async {
    _PreferencesPlatform().install(tester);
    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(
        Directory('${Directory.systemTemp.path}/unused-litch-app-library'),
      ),
    );
    final service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoRadioBle(),
      permissionGateway: _DeniedPermissions(),
    );
    addTearDown(service.dispose);

    await tester.pumpWidget(
      LitchMobileApp(
        bleService: service,
        gameLibraryLoader: () async => repository,
      ),
    );
    await _flushPlatform(tester, () {
      final Finder gate = find.byType(ConnectionGateScreen);
      return gate.evaluate().isNotEmpty &&
          tester.widget<ConnectionGateScreen>(gate).gameLibrary != null;
    });

    final ConnectionGateScreen gate = tester.widget<ConnectionGateScreen>(
      find.byType(ConnectionGateScreen),
    );
    expect(gate.gameLibrary, same(repository));
    expect(find.byType(ConnectionGateScreen), findsOneWidget);
    expect(
      EngineRuntimeScope.maybeOf(
        tester.element(find.byType(ConnectionGateScreen)),
      ),
      isNull,
      reason: 'the first-release app root must not activate a phone engine',
    );
    expect(
      PostGameReviewJobOwnerScope.maybeOf(
        tester.element(find.byType(ConnectionGateScreen)),
      )?.analyzer,
      isA<BoundedLocalPostGameReviewAnalyzer>(),
      reason: 'quick review still has one app-lifetime fallback owner',
    );
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('app root preserves and retires review owners with runtime', (
    tester,
  ) async {
    _PreferencesPlatform().install(tester);
    final repository = GameLibraryRepository(
      GameLibraryStore(
        Directory('${Directory.systemTemp.path}/unused-litch-engine-library'),
      ),
    );
    final service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoRadioBle(),
      permissionGateway: _DeniedPermissions(),
    );
    final runtime = EngineRuntime();
    final replacementRuntime = EngineRuntime();
    addTearDown(service.dispose);
    addTearDown(runtime.close);
    addTearDown(replacementRuntime.close);

    await tester.pumpWidget(
      LitchMobileApp(
        bleService: service,
        gameLibrary: repository,
        engineRuntime: runtime,
      ),
    );
    await tester.pump();
    BuildContext context = tester.element(find.byType(ConnectionGateScreen));

    expect(EngineRuntimeScope.maybeOf(context), same(runtime));
    final firstOwner = PostGameReviewJobOwnerScope.maybeOf(context)!;
    expect(firstOwner.analyzer, isA<EngineBackedPostGameReviewAnalyzer>());

    await tester.pumpWidget(
      LitchMobileApp(
        bleService: service,
        gameLibrary: repository,
        engineRuntime: runtime,
      ),
    );
    await tester.pump();
    context = tester.element(find.byType(ConnectionGateScreen));
    expect(PostGameReviewJobOwnerScope.maybeOf(context), same(firstOwner));

    await tester.pumpWidget(
      LitchMobileApp(
        bleService: service,
        gameLibrary: repository,
        engineRuntime: replacementRuntime,
      ),
    );
    await tester.pump();
    context = tester.element(find.byType(ConnectionGateScreen));
    final secondOwner = PostGameReviewJobOwnerScope.maybeOf(context)!;
    expect(secondOwner, isNot(same(firstOwner)));
    expect(EngineRuntimeScope.maybeOf(context), same(replacementRuntime));
    expect(() => firstOwner.start(completedClassicArchive()), throwsStateError);

    await tester.pumpWidget(
      LitchMobileApp(bleService: service, gameLibrary: repository),
    );
    await tester.pump();
    context = tester.element(find.byType(ConnectionGateScreen));
    final fallbackOwner = PostGameReviewJobOwnerScope.maybeOf(context)!;
    expect(fallbackOwner, isNot(same(secondOwner)));
    expect(fallbackOwner.analyzer, isA<BoundedLocalPostGameReviewAnalyzer>());
    expect(EngineRuntimeScope.maybeOf(context), isNull);
    expect(
      () => secondOwner.start(completedClassicArchive()),
      throwsStateError,
    );

    await tester.pumpWidget(const SizedBox.shrink());
    expect(
      () => fallbackOwner.start(completedClassicArchive()),
      throwsStateError,
    );
  });

  testWidgets('saved games remain reachable when startup opening needs retry', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    _PreferencesPlatform().install(tester);
    final Directory directory = (await tester.runAsync(
      () => Directory.systemTemp.createTemp('litch-retry-library-'),
    ))!;
    addTearDown(() async {
      if (await directory.exists()) await directory.delete(recursive: true);
    });
    final GameLibraryRepository repository = GameLibraryRepository(
      GameLibraryStore(directory),
    );
    await tester.runAsync(repository.loadCatalog);
    final service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoRadioBle(),
      permissionGateway: _DeniedPermissions(),
    );
    addTearDown(service.dispose);
    int attempts = 0;

    await tester.pumpWidget(
      LitchMobileApp(
        bleService: service,
        gameLibraryLoader: () async {
          attempts++;
          if (attempts <= 2) {
            throw const FileSystemException('library path unavailable');
          }
          return repository;
        },
      ),
    );
    await _flushPlatform(tester, () => attempts == 1);

    await tester.tap(find.byKey(const ValueKey<String>('game-library-action')));
    await _flushPlatform(
      tester,
      () =>
          attempts == 2 &&
          find.text('Saved games aren’t available').evaluate().isNotEmpty,
    );
    expect(find.byType(GameLibraryRouteScreen), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey<String>('game-library-retry')));
    await _flushPlatform(
      tester,
      () =>
          attempts == 3 && find.byType(GameLibraryScreen).evaluate().isNotEmpty,
    );
    await _flushPlatform(
      tester,
      () => find.text('No saved games yet').evaluate().isNotEmpty,
    );
    expect(find.text('No saved games yet'), findsOneWidget);
    expect(find.byType(GameLibraryScreen), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  for (final scenario in <(Locale, String, String)>[
    (const Locale('en'), 'Appearance', 'Dark'),
    (const Locale('en', 'GB'), 'Appearance', 'Dark'),
    (const Locale('es'), 'Apariencia', 'Oscuro'),
    (const Locale('de'), 'Darstellung', 'Dunkel'),
    (const Locale('it'), 'Aspetto', 'Scuro'),
    (const Locale('fr'), 'Apparence', 'Sombre'),
  ]) {
    for (final scale in <double>[1, 2]) {
      testWidgets('app preferences ${scenario.$1} at ${scale * 100}% text', (
        tester,
      ) async {
        final storage = _PreferencesPlatform()..install(tester);
        tester.view.physicalSize = const Size(400, 850);
        tester.view.devicePixelRatio = 1;
        tester.platformDispatcher.textScaleFactorTestValue = scale;
        tester.platformDispatcher.localesTestValue = <Locale>[scenario.$1];
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
        addTearDown(tester.platformDispatcher.clearLocalesTestValue);
        await _pumpOfflineApp(tester);
        expect(
          find.byKey(const ValueKey<String>('disconnected-navigation')),
          findsOneWidget,
        );
        await _openOfflineSetup(tester);
        expect(find.text(scenario.$2), findsOneWidget);
        final dark = find.text(scenario.$3);
        await tester.ensureVisible(dark);
        await tester.tap(dark);
        await tester.pumpAndSettle();
        expect(storage.values['flutter.app_appearance_mode'], 'dark');
        expect(Theme.of(tester.element(dark)).brightness, Brightness.dark);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      });
    }
  }

  testWidgets(
    'offline Settings changes language without replacing navigation or BLE',
    (tester) async {
      final storage = _PreferencesPlatform()..install(tester);
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('en')];
      addTearDown(tester.platformDispatcher.clearLocalesTestValue);
      final service = await _pumpOfflineApp(tester);
      final gate = tester.state(find.byType(ConnectionGateScreen));
      final navigator = tester.state<NavigatorState>(find.byType(Navigator));
      final connectionToken = service.connectionSessionToken;
      await _openOfflineSetup(tester);
      final settings = tester.state(find.byType(Scaffold));
      await tester.tap(find.byKey(const ValueKey<String>('app-language')));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Deutsch').last);
      await tester.pumpAndSettle();
      expect(find.text('Sprache · Language'), findsOneWidget);
      expect(tester.state(find.byType(Scaffold)), same(settings));
      expect(
        Localizations.localeOf(tester.element(find.text('Sprache · Language'))),
        const Locale('de'),
      );
      expect(find.text('Darstellung'), findsOneWidget);
      await tester.tap(find.text('Dunkel'));
      await tester.pumpAndSettle();
      expect(
        Theme.of(tester.element(find.text('Sprache · Language'))).brightness,
        Brightness.dark,
      );
      expect(storage.values['flutter.app_appearance_mode'], 'dark');
      expect(
        tester.state<NavigatorState>(find.byType(Navigator)),
        same(navigator),
      );
      final gateFinder = find.byType(ConnectionGateScreen, skipOffstage: false);
      expect(tester.state(gateFinder), same(gate));
      expect(
        tester.widget<ConnectionGateScreen>(gateFinder).bleService,
        same(service),
      );
      expect(service.connectionSessionToken, connectionToken);
      expect(find.byType(ConnectionGateScreen), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
      SharedPreferences.resetStatic();
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('fr')];
      await tester.pumpWidget(LitchMobileApp(bleService: service));
      await tester.pumpAndSettle();
      expect(
        Localizations.localeOf(
          tester.element(find.byType(ConnectionGateScreen)),
        ),
        const Locale('de'),
      );
      expect(storage.values['flutter.app_locale_mode'], 'de');
      await _openOfflineSetup(tester);
      expect(find.text('Sprache · Language'), findsOneWidget);
      expect(
        Theme.of(tester.element(find.text('Sprache · Language'))).brightness,
        Brightness.dark,
      );
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('explicit System choice wins over a delayed saved locale', (
    tester,
  ) async {
    final storage = _PreferencesPlatform()..holdLoad = Completer<void>();
    storage.values['flutter.app_locale_mode'] = 'de';
    storage.install(tester);
    tester.platformDispatcher.localesTestValue = const <Locale>[Locale('fr')];
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    await tester.tap(find.byKey(const ValueKey<String>('app-language')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Système (Français)').last);
    await tester.pumpAndSettle();
    storage.holdLoad!.complete();
    await tester.pumpAndSettle();
    expect(find.text('Langue · Language'), findsOneWidget);
    expect(
      tester
          .widget<DropdownButton<LitchLocalePreference>>(
            find.byKey(const ValueKey<String>('app-language')),
          )
          .value,
      const LitchLocalePreference.system(),
    );
    expect(storage.values['flutter.app_locale_mode'], 'system');
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('System follows ordered phone locales with English fallback', (
    tester,
  ) async {
    _PreferencesPlatform().install(tester);
    tester.platformDispatcher.localesTestValue = const <Locale>[
      Locale('ja'),
      Locale('fr', 'CA'),
    ];
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    final navigator = tester.state<NavigatorState>(find.byType(Navigator));
    final settings = tester.state(find.byType(Scaffold));
    final dropdown = find.byKey(const ValueKey<String>('app-language'));
    expect(
      Localizations.localeOf(tester.element(dropdown)),
      const Locale('fr'),
    );
    expect(find.text('Système (Français)'), findsOneWidget);
    for (final scenario in <(List<Locale>, Locale, String)>[
      (
        [const Locale('es', 'MX'), const Locale('de')],
        const Locale('es'),
        'Sistema (Español)',
      ),
      (
        [const Locale('en', 'GB')],
        const Locale('en', 'GB'),
        'System (English (UK))',
      ),
      ([const Locale('ja')], const Locale('en'), 'System (English)'),
      ([const Locale('en', 'XA')], const Locale('en'), 'System (English)'),
    ]) {
      tester.platformDispatcher.localesTestValue = scenario.$1;
      await tester.pumpAndSettle();
      expect(Localizations.localeOf(tester.element(dropdown)), scenario.$2);
      expect(find.text(scenario.$3), findsOneWidget);
      expect(
        tester.state<NavigatorState>(find.byType(Navigator)),
        same(navigator),
      );
      expect(tester.state(find.byType(Scaffold)), same(settings));
    }
    final choices = tester.widget<DropdownButton<LitchLocalePreference>>(
      dropdown,
    );
    expect(choices.value, const LitchLocalePreference.system());
    expect(choices.items!.map((item) => item.value), <LitchLocalePreference>[
      const LitchLocalePreference.fixed(litchGermanLocale),
      const LitchLocalePreference.fixed(litchEnglishLocale),
      const LitchLocalePreference.fixed(litchBritishEnglishLocale),
      const LitchLocalePreference.fixed(litchSpanishLocale),
      const LitchLocalePreference.fixed(litchFrenchLocale),
      const LitchLocalePreference.fixed(litchItalianLocale),
      const LitchLocalePreference.system(),
    ]);
    expect(litchSupportedLocales, isNot(contains(litchPseudoLocale)));
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('failed language save stays live and same choice retries', (
    tester,
  ) async {
    final storage = _PreferencesPlatform()
      ..rejectNextWriteForKey = 'flutter.app_locale_mode';
    storage.install(tester);
    tester.platformDispatcher.localesTestValue = const <Locale>[Locale('en')];
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    final dropdown = find.byKey(const ValueKey<String>('app-language'));
    await tester.tap(dropdown);
    await tester.pumpAndSettle();
    await tester.tap(find.text('Deutsch').last);
    await tester.pumpAndSettle();
    expect(find.text('Sprache · Language'), findsOneWidget);
    expect(
      find.text(
        'Die Sprache wurde geändert, konnte aber nicht für das nächste Mal gespeichert werden.',
      ),
      findsOneWidget,
    );
    expect(storage.values['flutter.app_locale_mode'], isNull);
    expect(storage.rejectNextWriteForKey, isNull);
    await tester.tap(dropdown);
    await tester.pumpAndSettle();
    await tester.tap(find.text('Deutsch').last);
    await tester.pumpAndSettle();
    expect(storage.values['flutter.app_locale_mode'], 'de');
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('explicit tips choice wins over a delayed startup read', (
    tester,
  ) async {
    final storage = _PreferencesPlatform()
      ..values['flutter.app_show_tips'] = true
      ..snapshotLoadBeforeHold = true
      ..holdLoad = Completer<void>();
    storage.install(tester);
    await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    final tips = find.byKey(const ValueKey<String>('app-show-tips'));
    await tester.ensureVisible(tips);
    await tester.tap(tips);
    await tester.pump();
    expect(tester.widget<Switch>(tips).value, isFalse);

    storage.holdLoad!.complete();
    await tester.pumpAndSettle();
    expect(tester.widget<Switch>(tips).value, isFalse);
    expect(storage.values['flutter.app_show_tips'], isFalse);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('failed tips save stays live and reports a localized retry', (
    tester,
  ) async {
    final storage = _PreferencesPlatform()
      ..rejectNextWriteForKey = 'flutter.app_show_tips';
    storage.install(tester);
    await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    final tips = find.byKey(const ValueKey<String>('app-show-tips'));
    await tester.ensureVisible(tips);
    await tester.tap(tips);
    await tester.pumpAndSettle();

    expect(tester.widget<Switch>(tips).value, isFalse);
    expect(
      find.text("Tips changed for this session, but couldn't be saved."),
      findsOneWidget,
    );
    expect(storage.values['flutter.app_show_tips'], isNull);
    expect(storage.rejectNextWriteForKey, isNull);

    await tester.tap(tips);
    await tester.pumpAndSettle();
    await tester.tap(tips);
    await tester.pumpAndSettle();
    expect(storage.values['flutter.app_show_tips'], isFalse);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  for (final saved in <Object>['en-XA', 'ja', 42]) {
    testWidgets('invalid saved locale $saved falls back to System', (
      tester,
    ) async {
      final storage = _PreferencesPlatform();
      storage.values['flutter.app_locale_mode'] = saved;
      storage.install(tester);
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('it')];
      addTearDown(tester.platformDispatcher.clearLocalesTestValue);
      await _pumpOfflineApp(tester);
      await _openOfflineSetup(tester);
      final dropdown = find.byKey(const ValueKey<String>('app-language'));
      expect(
        Localizations.localeOf(tester.element(dropdown)),
        const Locale('it'),
      );
      expect(
        tester.widget<DropdownButton<LitchLocalePreference>>(dropdown).value,
        const LitchLocalePreference.system(),
      );
      for (final locale in <Locale>[litchPseudoLocale, const Locale('ja')]) {
        await expectLater(
          LitchLocalePreferences.save(LitchLocalePreference.fixed(locale)),
          throwsArgumentError,
        );
      }
      expect(storage.values['flutter.app_locale_mode'], saved);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }

  testWidgets('unavailable startup preferences leave System usable', (
    tester,
  ) async {
    final storage = _PreferencesPlatform()..failNextLoad = true;
    storage.install(tester);
    tester.platformDispatcher.localesTestValue = const <Locale>[Locale('fr')];
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    expect(find.text('Langue · Language'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey<String>('app-language')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Deutsch').last);
    await tester.pumpAndSettle();
    expect(find.text('Sprache · Language'), findsOneWidget);
    expect(storage.values['flutter.app_locale_mode'], 'de');
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  for (final layout in <(String, Size)>[
    ('portrait navigation', const Size(390, 844)),
    ('expanded navigation', const Size(800, 600)),
  ]) {
    testWidgets('Settings stays live across the real gate connect and loss with '
        '${layout.$1}', (tester) async {
      tester.view.physicalSize = layout.$2;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      _PreferencesPlatform().install(tester);
      tester.platformDispatcher.localesTestValue = const <Locale>[Locale('en')];
      addTearDown(tester.platformDispatcher.clearLocalesTestValue);
      final ble = _SettingsJourneyBle();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: ble,
        permissionGateway: _GrantedPermissions(),
      );
      bool cleanedUp = false;
      Future<void> cleanup() async {
        if (cleanedUp) return;
        await tester.pumpWidget(const SizedBox.shrink());
        bool disposed = false;
        final disposal = disposeBoard(service, ble).then((_) => disposed = true);
        await _flushPlatform(tester, () => disposed);
        await disposal;
        await ble.statusController.close();
        await ble.scan.close();
        await tester.pump(const Duration(milliseconds: 40));
        cleanedUp = true;
      }

      addTearDown(cleanup);
      await tester.pumpWidget(
        LitchMobileApp(
          bleService: service,
          // The unrelated release page alone is offline, as in the gate journey.
          firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
        ),
      );
      await tester.pump();
      ble.statusController.add(BleStatus.ready);
      await tester.pump();
      final navigator = Navigator.of(
        tester.element(find.byType(ConnectionGateScreen)),
        rootNavigator: true,
      );
      final gate = tester.state(find.byType(ConnectionGateScreen));
      await _openOfflineSetup(tester);
      expect(_selectedNavigationIndex(tester), 2);

      Future<void> connect(int count) async {
        expect(ble.scan.hasListener, isTrue);
        ble.scan.add(
          DiscoveredDevice(
            id: 'locale-board',
            name: 'Foxie_Litch',
            serviceUuids: <Uuid>[BleProtocol.baseService],
            serviceData: const <Uuid, Uint8List>{},
            manufacturerData: Uint8List(0),
            rssi: -50,
            connectable: Connectable.available,
          ),
        );
        await tester.pump();
        await tester.pump(const Duration(seconds: 2));
        await _flushPlatform(tester, () => ble.connections.length == count);
        ble.connections.last.add(
          const ConnectionStateUpdate(
            deviceId: 'locale-board',
            connectionState: DeviceConnectionState.connected,
            failure: null,
          ),
        );
        await _flushPlatform(tester, () => service.productReady);
        expect(ble.sessionActivated, isTrue);
      }

      await connect(1);
      expect(
        find.byType(LitchMainScaffold, skipOffstage: false),
        findsOneWidget,
      );
      expect(find.byType(SettingsScreen), findsOneWidget);
      expect(_selectedNavigationIndex(tester), 2);
      Future<void> openPhoneSettings() async {
        final phone = find.byKey(const ValueKey<String>('setup-open:phone'));
        await tester.scrollUntilVisible(
          phone,
          200,
          scrollable: find
              .descendant(
                of: find.byKey(const ValueKey<String>('settings-scroll')),
                matching: find.byType(Scrollable),
              )
              .first,
        );
        await tester.pumpAndSettle();
        expect(phone.hitTestable(), findsOneWidget);
        await tester.tap(phone);
        await tester.pumpAndSettle();
      }

      await openPhoneSettings();
      await _chooseLanguage(tester, 'Deutsch');
      expect(find.text('Sprache · Language'), findsOneWidget);
      ble.connections.last.add(
        const ConnectionStateUpdate(
          deviceId: 'locale-board',
          connectionState: DeviceConnectionState.disconnected,
          failure: null,
        ),
      );
      await tester.pump(const Duration(milliseconds: 16));
      expect(service.isConnected, isFalse);
      expect(
        find.byType(LitchMainScaffold, skipOffstage: false),
        findsOneWidget,
        reason:
            'A transient reconnect keeps the shell and its navigation geometry stable.',
      );
      expect(find.byType(SettingsScreen), findsOneWidget);
      expect(find.byType(AppPreferences), findsOneWidget);
      expect(find.text('Sprache · Language'), findsOneWidget);
      expect(_selectedNavigationIndex(tester), 2);
      expect(tester.state(find.byType(ConnectionGateScreen)), same(gate));
      await connect(2);
      final token = service.connectionSessionToken;
      _selectNavigationDestination(tester, 0);
      await tester.pumpAndSettle();
      final home = tester.state(find.byType(HomeScreen));
      _selectNavigationDestination(tester, 2);
      await tester.pumpAndSettle();
      await openPhoneSettings();
      final scaffold = tester.state(find.byType(LitchMainScaffold));
      final settings = tester.state(find.byType(SettingsScreen));
      await tester.ensureVisible(
        find.byKey(const ValueKey<String>('app-language')),
      );
      await tester.pumpAndSettle();
      expect(find.text('Sprache · Language'), findsOneWidget);
      await _chooseLanguage(tester, 'Français');
      expect(find.text('Langue · Language'), findsOneWidget);
      expect(tester.state(find.byType(SettingsScreen)), same(settings));
      expect(
        tester.state(find.byType(HomeScreen, skipOffstage: false)),
        same(home),
      );
      expect(tester.state(find.byType(LitchMainScaffold)), same(scaffold));
      expect(tester.state(find.byType(ConnectionGateScreen)), same(gate));
      expect(
        Navigator.of(
          tester.element(find.byType(ConnectionGateScreen)),
          rootNavigator: true,
        ),
        same(navigator),
      );
      expect(service.connectionSessionToken, token);
      expect(ble.connections, hasLength(2));
      expect(tester.takeException(), isNull);
      await cleanup();
    }, variant: TargetPlatformVariant.only(TargetPlatform.iOS));
  }

  testWidgets('phone Settings language picker remains usable at 200 percent', (
    tester,
  ) async {
    _PreferencesPlatform().install(tester);
    tester.view.physicalSize = const Size(400, 850);
    tester.view.devicePixelRatio = 1;
    tester.platformDispatcher.textScaleFactorTestValue = 2;
    tester.platformDispatcher.localesTestValue = const <Locale>[
      Locale('en', 'GB'),
    ];
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
    addTearDown(tester.platformDispatcher.clearLocalesTestValue);
    await _pumpOfflineApp(tester);
    final navigation = find.byKey(
      const ValueKey<String>('disconnected-navigation'),
    );
    expect(tester.getSize(navigation).height, greaterThanOrEqualTo(48));
    await _openOfflineSetup(tester);
    final dropdown = find.byKey(const ValueKey<String>('app-language'));
    await tester.ensureVisible(dropdown);
    await tester.pumpAndSettle();
    expect(tester.getSize(dropdown).height, greaterThanOrEqualTo(48));
    expect(find.text('System (English (UK))'), findsOneWidget);
    await tester.tap(dropdown);
    await tester.pumpAndSettle();
    // Seven exclusive choices need a scrolling popup at this text size.
    final french = find.text('Français');
    await tester.scrollUntilVisible(
      french,
      100,
      scrollable: find.byType(Scrollable).last,
    );
    await tester.pumpAndSettle();
    expect(
      tester
          .getSize(
            find.ancestor(
              of: french,
              matching: find.byType(DropdownMenuItem<LitchLocalePreference>),
            ),
          )
          .height,
      greaterThanOrEqualTo(48),
    );
    await tester.tap(french);
    await tester.pumpAndSettle();
    expect(find.text('Langue · Language'), findsOneWidget);
    expect(
      tester.widget<DropdownButton<LitchLocalePreference>>(dropdown).value,
      const LitchLocalePreference.fixed(Locale('fr')),
    );
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  for (final scale in <double>[1, 2]) {
    testWidgets('offline Settings remains accessible at ${scale * 100}% text', (
      tester,
    ) async {
      _PreferencesPlatform().install(tester);
      await tester.binding.setSurfaceSize(const Size(320, 700));
      tester.platformDispatcher.textScaleFactorTestValue = scale;
      addTearDown(() => tester.binding.setSurfaceSize(null));
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      await _pumpOfflineApp(tester);
      await tester.pumpAndSettle();
      final navigation = find.byKey(
        const ValueKey<String>('disconnected-navigation'),
      );
      expect(navigation.hitTestable(), findsOneWidget);
      expect(tester.getSize(navigation).width, greaterThanOrEqualTo(48));
      expect(tester.getSize(navigation).height, greaterThanOrEqualTo(48));
      final recovery = find.byKey(const ValueKey<String>('bluetooth-recovery'));
      if (recovery.hitTestable().evaluate().isEmpty) {
        await tester.scrollUntilVisible(
          recovery,
          160,
          scrollable: find.byType(Scrollable).first,
        );
        await tester.pumpAndSettle();
      }
      expect(recovery.hitTestable(), findsOneWidget);
      expect(tester.takeException(), isNull);
      await _openOfflineSetup(tester);
      final language = find.byKey(const ValueKey<String>('app-language'));
      await tester.ensureVisible(language);
      await tester.pumpAndSettle();
      expect(language.hitTestable(), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  }
}

int? _selectedNavigationIndex(WidgetTester tester) {
  final navigationBar = find.byType(NavigationBar);
  if (navigationBar.evaluate().isNotEmpty) {
    return tester.widget<NavigationBar>(navigationBar).selectedIndex;
  }
  return tester
      .widget<NavigationRail>(find.byType(NavigationRail))
      .selectedIndex;
}

void _selectNavigationDestination(WidgetTester tester, int index) {
  final navigationBar = find.byType(NavigationBar);
  if (navigationBar.evaluate().isNotEmpty) {
    tester.widget<NavigationBar>(navigationBar).onDestinationSelected!(index);
    return;
  }
  tester
      .widget<NavigationRail>(find.byType(NavigationRail))
      .onDestinationSelected!(index);
}

Future<void> _openOfflineSetup(WidgetTester tester) async {
  final compactSetup = find.byKey(
    const ValueKey<String>('landscape-setup-action'),
  );
  final portraitSetup = find.byKey(
    const ValueKey<String>('disconnected-destination-setup'),
  );
  if (compactSetup.evaluate().isNotEmpty) {
    await tester.tap(compactSetup);
  } else if (portraitSetup.evaluate().isNotEmpty) {
    await tester.tap(portraitSetup);
  } else if (find.byType(NavigationRail).evaluate().isNotEmpty) {
    tester
        .widget<NavigationRail>(find.byType(NavigationRail))
        .onDestinationSelected!(2);
  } else {
    fail('No offline Setup navigation target is available.');
  }
  await tester.pumpAndSettle();
}

Future<void> _chooseLanguage(WidgetTester tester, String autonym) async {
  final dropdown = find.byKey(const ValueKey<String>('app-language'));
  await tester.ensureVisible(dropdown);
  await tester.pumpAndSettle();
  if (dropdown.hitTestable().evaluate().isEmpty &&
      find.byType(SettingsScreen).evaluate().isNotEmpty) {
    await tester.scrollUntilVisible(
      dropdown,
      300,
      scrollable: find
          .descendant(
            of: find.byType(SettingsScreen),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    await tester.pumpAndSettle();
  }
  expect(dropdown.hitTestable(), findsOneWidget);
  await tester.tap(dropdown);
  await tester.pumpAndSettle();
  await tester.tap(find.text(autonym).last);
  await tester.pumpAndSettle();
}

Future<void> _flushPlatform(
  WidgetTester tester,
  bool Function() complete,
) async {
  // Yield SDK cancellation only; widget/service actions use the test clock.
  for (int pass = 0; pass < 8 && !complete(); pass++) {
    await tester.pump();
    await tester.runAsync(() async {});
    await tester.pump();
  }
  expect(complete(), isTrue);
}

Future<LitchBleService> _pumpOfflineApp(WidgetTester tester) async {
  PackageInfo.setMockInitialValues(
    appName: 'Foxie Litch',
    packageName: 'com.foxie.litch',
    version: '0.5.0',
    buildNumber: '1',
    buildSignature: '',
  );
  final service = LitchBleService(
    pendingOtaStore: MemoryOtaTargetStore(),
    ble: _NoRadioBle(),
    permissionGateway: _DeniedPermissions(),
  );
  addTearDown(service.dispose);
  await tester.pumpWidget(LitchMobileApp(bleService: service));
  await tester.pump();
  return service;
}

final class _PreferencesPlatform {
  static const channel = MethodChannel('plugins.flutter.io/shared_preferences');
  final values = <String, Object>{};
  Completer<void>? holdLoad;
  String? rejectNextWriteForKey;
  bool failNextLoad = false;
  bool snapshotLoadBeforeHold = false;

  void install(WidgetTester tester) {
    SharedPreferences.resetStatic();
    final messenger = tester.binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(channel, (call) async {
      switch (call.method) {
        case 'getAll':
          final snapshot = snapshotLoadBeforeHold
              ? Map<String, Object>.from(values)
              : null;
          await holdLoad?.future;
          if (failNextLoad) {
            failNextLoad = false;
            throw PlatformException(code: 'unavailable');
          }
          return snapshot ?? Map<String, Object>.from(values);
        case 'setString':
        case 'setBool':
          final arguments = call.arguments as Map<Object?, Object?>;
          final key = arguments['key']! as String;
          // Startup also persists app identity; fail only the setting under test.
          if (rejectNextWriteForKey == key) {
            rejectNextWriteForKey = null;
            return false;
          }
          values[key] = arguments['value']!;
          return true;
        default:
          throw StateError('Unexpected preference operation: ${call.method}');
      }
    });
    addTearDown(() {
      if (holdLoad case final pending? when !pending.isCompleted) {
        pending.complete();
      }
      messenger.setMockMethodCallHandler(channel, null);
      SharedPreferences.resetStatic();
    });
  }
}

final class _NoRadioBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

final class _DeniedPermissions implements BlePermissionGateway {
  @override
  Future<PermissionStatus> status(Permission permission) async =>
      PermissionStatus.denied;
  @override
  Future<Map<Permission, PermissionStatus>> request(
    List<Permission> permissions,
  ) async => throw StateError('No permission request expected');
  @override
  Future<bool> openSettings() async =>
      throw StateError('No settings launch expected');
}

final class _GrantedPermissions extends _DeniedPermissions {
  @override
  Future<PermissionStatus> status(Permission permission) async =>
      PermissionStatus.granted;
}

// The simulated board discovers and activates a fresh shared Settings session
// on each link. Only scanning and the OS connection events are scripted here.
final class _SettingsJourneyBle extends SettingsSessionBoard {
  _SettingsJourneyBle() : super(includeHomeSnapshot: true);

  final statusController = StreamController<BleStatus>.broadcast(sync: true);
  final scan = StreamController<DiscoveredDevice>.broadcast(sync: true);

  @override
  Stream<BleStatus> get statusStream => statusController.stream;

  @override
  Stream<DiscoveredDevice> scanForDevices({
    required List<Uuid> withServices,
    ScanMode scanMode = ScanMode.balanced,
    bool requireLocationServicesEnabled = true,
  }) => scan.stream;
}
