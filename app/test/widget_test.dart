import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:litch_mobile/src/constants/firmware_release.dart';
import 'package:litch_mobile/src/models/firmware_provenance.dart';
import 'package:litch_mobile/src/models/firmware_release_snapshot.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/screens/about_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/firmware_screen.dart';
import 'package:litch_mobile/src/services/firmware_download_service.dart';
import 'package:litch_mobile/src/services/app_upgrade_notice_service.dart';
import 'package:litch_mobile/src/widgets/app_upgrade_notice_prompt.dart';
import 'package:litch_mobile/src/widgets/release_notes_dialog.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/firmware_test_fixture.dart';
import 'health_render_capture.dart';
import 'support/console_session_board.dart';
import 'support/reboot_reply_ble.dart';

class FakeFlutterReactiveBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) {
    return super.noSuchMethod(invocation);
  }
}

class FakeLitchBleService extends LitchBleService {
  FakeLitchBleService()
    : super(
        ble: FakeFlutterReactiveBle(),
        pendingOtaStore: MemoryOtaTargetStore(),
      );

  final StreamController<List<String>> _consoleController =
      StreamController<List<String>>.broadcast();
  List<LitchSetting> settingsToReturn = const <LitchSetting>[];
  LitchSettingsReadAuthority settingsAuthority =
      LitchSettingsReadAuthority.confirmed;
  String boardVersion = '0.6.0';
  bool connected = true;
  bool authorized = false;
  final readinessEvents = StreamController<bool>.broadcast(sync: true);

  @override
  bool get productReady => connected && authorized;
  @override
  BleAvailability get bleAvailability => BleAvailability.ready;
  @override
  Stream<bool> get productReadinessStream => readinessEvents.stream;

  @override
  Stream<List<String>> get consoleStream => _consoleController.stream;

  @override
  bool get isConnected => connected;

  @override
  String? get connectedId => connected ? 'board-1' : null;

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v2,
        capabilityMask:
            LitchDeviceCapabilities.consoleBit |
            LitchDeviceCapabilities.settingsBit |
            LitchDeviceCapabilities.identifyBit |
            LitchDeviceCapabilities.firmwareDigestBit |
            LitchDeviceCapabilities.firmwareUpdateBit |
            LitchDeviceCapabilities.runtimeHealthBit |
            LitchDeviceCapabilities.runtimeHealthRecheckBit,
        explicitlyReported: true,
      );

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async => deviceCapabilities(deviceId);

  @override
  Future<String?> fetchBoardFirmwareVersion({required String deviceId}) async =>
      boardVersion;

  @override
  Future<FirmwareProvenance> fetchFirmwareProvenance({
    required String deviceId,
  }) async => FirmwareProvenance.custom('a' * 64);

  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async =>
      RuntimeHealthAvailable(RuntimeHealthSnapshot.fromToken('00B000000')!);

  void emitConsoleLines(List<String> lines) {
    _consoleController.add(List<String>.from(lines));
  }

  @override
  Future<List<LitchSetting>> readSettings(String deviceId) async {
    return settingsToReturn;
  }

  @override
  LitchSettingsReadAuthority settingsReadAuthority(String deviceId) =>
      settingsAuthority;

  @override
  Future<void> dispose() async {
    await readinessEvents.close();
    await _consoleController.close();
    await super.dispose();
  }
}

class _WidgetFirmwareDownloadService extends FirmwareDownloadService {
  @override
  Future<FirmwareReleaseSnapshot> fetchReleaseSnapshot(
    FirmwareReleaseInfo release,
  ) async {
    final Uint8List firmware = makeEsp32s3Firmware();
    const String notes = 'Version: 0.6.1\n\nAccessible update layout.';
    return FirmwareReleaseSnapshot(
      release: release,
      manifest: makeFirmwareManifest(
        firmware,
        channel: release.channel.name,
        manifestUrl: release.manifestUrl,
        notesMarkdown: notes,
        version: '0.6.1',
      ),
      notesMarkdown: notes,
    );
  }
}

String _selectedNavigationLabel(WidgetTester tester) {
  final barFinder = find.byType(NavigationBar);
  if (barFinder.evaluate().isNotEmpty) {
    final navigation = tester.widget<NavigationBar>(barFinder);
    final selected =
        navigation.destinations[navigation.selectedIndex]
            as NavigationDestination;
    return selected.label;
  }
  final navigation = tester.widget<NavigationRail>(
    find.byKey(const ValueKey<String>('main-navigation-rail')),
  );
  return (navigation.destinations[navigation.selectedIndex!].label as Text)
      .data!;
}

Future<void> _tapNavigationLabel(WidgetTester tester, String label) async {
  final destinationKey = switch (label) {
    'Litchcraft' => 'main-destination-litchcraft',
    'Play' => 'main-destination-play',
    'Setup' => 'main-destination-setup',
    'Console' => 'main-destination-console',
    _ => null,
  };
  if (destinationKey != null) {
    final keyedTarget = find.byKey(ValueKey<String>(destinationKey));
    if (keyedTarget.evaluate().isNotEmpty) {
      await tester.tap(keyedTarget);
      return;
    }
  }
  final barFinder = find.byType(NavigationBar);
  if (barFinder.evaluate().isNotEmpty) {
    final target = find
        .descendant(of: barFinder, matching: find.text(label))
        .hitTestable();
    expect(target, findsOneWidget);
    await tester.tap(target);
    return;
  }
  final icon = switch (label) {
    'Litchcraft' => Icons.brush_outlined,
    'Play' => Icons.grid_view_outlined,
    'Setup' => Icons.tune_outlined,
    'Console' => Icons.terminal,
    _ => throw ArgumentError.value(label, 'label'),
  };
  final target = find
      .descendant(
        of: find.byKey(const ValueKey<String>('main-navigation-rail')),
        matching: find.byIcon(icon),
      )
      .hitTestable();
  expect(target, findsOneWidget);
  await tester.tap(target);
}

void _useViewport(WidgetTester tester, Size size) {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(() {
    tester.view.resetPhysicalSize();
    tester.view.resetDevicePixelRatio();
  });
}

void main() {
  Future<void> scrollIntoView(WidgetTester tester, Finder target) async {
    expect(target, findsOneWidget);
    final Finder scrollable = find
        .ancestor(of: target, matching: find.byType(Scrollable))
        .first;
    await tester.scrollUntilVisible(target, 200, scrollable: scrollable);
    await tester.pumpAndSettle();
  }

  Future<void> openSetupPage(WidgetTester tester, String page) async {
    final destination = find.byKey(ValueKey<String>('setup-open:$page'));
    await tester.scrollUntilVisible(
      destination,
      200,
      scrollable: find
          .descendant(
            of: find.byKey(const ValueKey<String>('settings-scroll')),
            matching: find.byType(Scrollable),
          )
          .first,
    );
    await tester.pumpAndSettle();
    expect(destination.hitTestable(), findsOneWidget);
    await tester.tap(destination);
    await tester.pumpAndSettle();
  }

  Widget buildHarness({
    required LitchBleService bleService,
    required Future<bool> Function() loadPreference,
    required Future<void> Function(bool enabled) savePreference,
    String boardId = 'board-1',
    FirmwareUpdateAvailability availability =
        const FirmwareUpdateAvailability(),
    TextScaler textScaler = TextScaler.noScaling,
    bool useRealFirmwarePage = false,
    AppUpgradeNoticeService? appUpgradeNoticeService,
    bool noticeStartupReady = true,
    ThemeData? theme,
    LitchAppearanceMode appearanceMode = LitchAppearanceMode.system,
  }) {
    Widget withAppNotes(Widget child) => appUpgradeNoticeService == null
        ? child
        : AppUpgradeNoticePrompt(
            bleService: bleService,
            service: appUpgradeNoticeService,
            startupReady: noticeStartupReady,
            child: child,
          );
    return MaterialApp(
      theme: theme ?? buildLitchTheme(),
      builder: (BuildContext context, Widget? child) {
        return MediaQuery(
          data: MediaQuery.of(context).copyWith(textScaler: textScaler),
          child: RepaintBoundary(
            key: const ValueKey<String>('release-notice-capture'),
            child: child!,
          ),
        );
      },
      home: withAppNotes(
        RepaintBoundary(
          key: const ValueKey<String>('litch-shell-capture'),
          child: LitchMainScaffold(
            bleService: bleService,
            boardId: boardId,
            boards: const <LitchBoard>[],
            onDisconnect: () async {},
            initialCraftModeResolved: true,
            loadAppConsolePreference: loadPreference,
            saveAppConsolePreference: savePreference,
            appearanceMode: appearanceMode,
            onAppearanceModeChanged: (LitchAppearanceMode mode) async {},
            firmwarePageBuilder:
                (
                  LitchBleService bleService,
                  String boardId,
                  VoidCallback onRequestShowFirmwareTab,
                  ValueChanged<bool> onUpdateActivityChanged,
                  ValueChanged<FirmwareUpdateAvailability>
                  onAvailabilityChanged,
                ) {
                  if (useRealFirmwarePage) {
                    return FirmwareScreen(
                      bleService: bleService,
                      boardId: boardId,
                      onRequestShowFirmwareTab: onRequestShowFirmwareTab,
                      onUpdateActivityChanged: onUpdateActivityChanged,
                      onAvailabilityChanged: onAvailabilityChanged,
                      downloadService: _WidgetFirmwareDownloadService(),
                    );
                  }
                  return Builder(
                    builder: (BuildContext context) {
                      WidgetsBinding.instance.addPostFrameCallback((_) {
                        onAvailabilityChanged(availability);
                      });
                      return const Center(child: Text('Firmware Placeholder'));
                    },
                  );
                },
          ),
        ),
      ),
    );
  }

  testWidgets('opens About from the connected app identity', (
    WidgetTester tester,
  ) async {
    PackageInfo.setMockInitialValues(
      appName: 'Foxie Litch',
      packageName: 'com.foxieproducts.litch_mobile',
      version: '0.5.0',
      buildNumber: '1',
      buildSignature: '',
    );
    final FakeLitchBleService bleService = FakeLitchBleService();
    addTearDown(bleService.dispose);
    await tester.pumpWidget(
      buildHarness(
        bleService: bleService,
        loadPreference: () async => false,
        savePreference: (bool enabled) async {},
      ),
    );
    await tester.pumpAndSettle();

    await tester.tap(find.byKey(const ValueKey<String>('open-about')));
    await tester.pumpAndSettle();

    expect(find.byType(AboutScreen), findsOneWidget);
  });

  testWidgets('keeps update status lightweight and opens update details', (
    WidgetTester tester,
  ) async {
    PackageInfo.setMockInitialValues(
      appName: 'Litch',
      packageName: 'com.foxieproducts.litchMobile',
      version: '0.6.0',
      buildNumber: '1',
      buildSignature: '',
    );
    SharedPreferences.setMockInitialValues(<String, Object>{
      'allow_beta_updates': false,
    });
    final FakeLitchBleService bleService = FakeLitchBleService();
    addTearDown(bleService.dispose);
    _useViewport(tester, const Size(320, 700));
    await loadHealthReviewFonts(tester, useSdkFonts: true);

    await tester.pumpWidget(
      buildHarness(
        bleService: bleService,
        loadPreference: () async => false,
        savePreference: (bool enabled) async {},
        textScaler: const TextScaler.linear(2),
        useRealFirmwarePage: true,
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Connected'), findsOneWidget);
    final Rect toolbar = tester.getRect(find.byType(AppBar));
    final Rect status = tester.getRect(
      find.byKey(const ValueKey<String>('connected-board-status-action')),
    );
    expect(status.height, greaterThanOrEqualTo(48));
    expect(status.top, greaterThanOrEqualTo(toolbar.top));
    expect(status.bottom, lessThanOrEqualTo(toolbar.bottom));
    expect(
      tester.getRect(find.byKey(const ValueKey<String>('open-about'))).right,
      lessThanOrEqualTo(status.left),
    );
    expect(find.text('Update available'), findsOneWidget);
    expect(find.textContaining('0.6.0'), findsNothing);
    expect(find.textContaining('0.6.1'), findsNothing);

    await tester.tap(find.text('Update available'));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);

    final updateRow = find.widgetWithText(ListTile, 'Check for updates');
    await scrollIntoView(tester, updateRow);
    await tester.tap(updateRow);
    await tester.pumpAndSettle();
    expect(find.text('Board update'), findsOneWidget);
    expect(find.byType(FirmwareScreen), findsOneWidget);
    expect(find.text('Version 0.6.1 is ready'), findsOneWidget);
    expect(find.text('Installed version 0.6.0'), findsOneWidget);
    expect(find.text('Firmware Placeholder'), findsNothing);
    expect(tester.takeException(), isNull);

    // App notes use native package identity and work with no connected board.
    await tester.pumpWidget(const SizedBox.shrink());
    bleService.connected = false;
    String? observedIdentity;
    AppUpgradeNoticeService appRelease(int build) => AppUpgradeNoticeService(
      platform: TargetPlatform.android,
      loadPackageInfo: () async => PackageInfo(
        appName: 'Foxie Litch',
        packageName: 'com.foxieproducts.litchMobile',
        version: '1.0.2',
        buildNumber: '$build',
        installTime: DateTime.utc(2026, 1, 1),
        updateTime: DateTime.utc(2026, 1, build == 51 ? 1 : 2),
      ),
      readIdentity: () async => observedIdentity,
      writeIdentity: (value) async {
        observedIdentity = value;
        return true;
      },
      loadReleaseNotes: () async => jsonEncode(<String, Object>{
        'schema_version': 1,
        'releases': <Object>[
          <String, Object>{
            'version': '1.0.2',
            'build_number': '$build',
            'notes':
                'Exact store notes for build $build.\n${List<String>.filled(20, 'Play and review your games.').join('\n')}',
          },
        ],
      }),
    );
    Widget appHarness(
      AppUpgradeNoticeService release, {
      bool ready = true,
      bool withFirmware = false,
    }) => buildHarness(
      bleService: bleService,
      loadPreference: () async => false,
      savePreference: (_) async {},
      textScaler: const TextScaler.linear(2),
      appUpgradeNoticeService: release,
      noticeStartupReady: ready,
      useRealFirmwarePage: withFirmware,
    );
    final appNotice = find.byKey(const ValueKey<String>('app-upgrade-notice'));
    final firmwareNotice = find.byKey(
      const ValueKey<String>('firmware-update-offer'),
    );
    await tester.pumpWidget(appHarness(appRelease(51)));
    await tester.pumpAndSettle();
    expect(
      appNotice,
      findsNothing,
      reason: 'Fresh installs establish a silent baseline.',
    );
    final preferences = await SharedPreferences.getInstance();
    await preferences.setBool('firmware_update_notices_disabled', true);

    await tester.pumpWidget(const SizedBox.shrink());
    final update52 = appRelease(52);
    await tester.pumpWidget(appHarness(update52, ready: false));
    await tester.pumpAndSettle();
    expect(appNotice, findsNothing, reason: 'Startup owns the first screen.');
    await tester.pumpWidget(appHarness(update52));
    await tester.pumpAndSettle();
    expect(appNotice, findsOneWidget);
    expect(find.text('App update: 1.0.2'), findsOneWidget);
    expect(
      find.textContaining('Exact store notes for build 52.'),
      findsOneWidget,
    );
    expect(bleService.isConnected, isFalse);
    await tester.pumpWidget(appHarness(update52, ready: false));
    await tester.pumpAndSettle();
    expect(appNotice, findsNothing);
    await tester.pumpWidget(appHarness(update52));
    await tester.pumpAndSettle();
    expect(
      appNotice,
      findsOneWidget,
      reason: 'The app notes must return when a startup reconnect finishes.',
    );
    await captureHealthReview(
      tester,
      'app-upgrade-notice-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('release-notice-capture'),
      ),
    );
    tester.view.physicalSize = const Size(700, 320);
    await tester.pumpAndSettle();
    expect(
      find
          .byKey(const ValueKey<String>('release-notice-opt-out'))
          .hitTestable(),
      findsOneWidget,
    );
    expect(
      find.widgetWithText(FilledButton, 'Continue').hitTestable(),
      findsOneWidget,
    );
    final appChoiceBefore = tester.getRect(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    final appContinueBefore = tester.getRect(
      find.widgetWithText(FilledButton, 'Continue'),
    );
    final appBodyScroll = find
        .ancestor(
          of: find.text('App update: 1.0.2'),
          matching: find.byType(Scrollable),
        )
        .first;
    await tester.drag(appBodyScroll, const Offset(0, -180));
    await tester.pumpAndSettle();
    expect(
      tester.state<ScrollableState>(appBodyScroll).position.pixels,
      greaterThan(0),
    );
    expect(
      tester.getRect(
        find.byKey(const ValueKey<String>('release-notice-opt-out')),
      ),
      appChoiceBefore,
    );
    expect(
      tester.getRect(find.widgetWithText(FilledButton, 'Continue')),
      appContinueBefore,
    );
    expect(
      tester
          .getRect(find.textContaining('Exact store notes for build 52.'))
          .overlaps(tester.getRect(appBodyScroll)),
      isTrue,
    );
    await captureHealthReview(
      tester,
      'app-upgrade-landscape-200-reading',
      boundaryFinder: find.byKey(
        const ValueKey<String>('release-notice-capture'),
      ),
    );
    await tester.tap(find.text('Continue'));
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpWidget(appHarness(appRelease(52)));
    await tester.pumpAndSettle();
    expect(
      appNotice,
      findsNothing,
      reason: 'The same installed update is not repeated.',
    );

    tester.view.physicalSize = const Size(320, 700);
    // Both are eligible: pairing finishes before app notes, and app notes
    // fully close before the board offer. The opt-outs are independent.
    await preferences.setBool('firmware_update_notices_disabled', false);
    await tester.pumpWidget(const SizedBox.shrink());
    bleService.connected = true;
    bleService.authorized = false;
    final update53 = appRelease(53);
    await tester.pumpWidget(appHarness(update53, withFirmware: true));
    await tester.pumpAndSettle();
    expect(appNotice, findsNothing);
    expect(firmwareNotice, findsNothing);
    bleService.authorized = true;
    bleService.readinessEvents.add(true);
    await tester.pumpAndSettle();
    expect(appNotice, findsOneWidget);
    expect(firmwareNotice, findsNothing);
    await tester.tap(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    await tester.pumpAndSettle();
    expect(preferences.getBool('app_update_notices_disabled'), isTrue);
    expect(preferences.getBool('firmware_update_notices_disabled'), isFalse);
    await tester.tap(find.text('Continue'));
    await tester.pump();
    expect(
      firmwareNotice,
      findsNothing,
      reason: 'Wait for the app dialog to finish closing.',
    );
    await tester.pumpAndSettle();
    expect(appNotice, findsNothing);
    expect(
      firmwareNotice,
      findsOneWidget,
      reason: 'Disabling app notes does not disable board offers.',
    );
    // The compact dialog keeps actions fixed; a real outer-body drag brings
    // the complete preference into view without scrolling through long notes.
    await tester.drag(
      find.text('Board firmware update: 0.6.1'),
      const Offset(0, -220),
    );
    await tester.pumpAndSettle();
    expect(
      find
          .byKey(const ValueKey<String>('release-notice-opt-out'))
          .hitTestable(),
      findsOneWidget,
    );
    await captureHealthReview(
      tester,
      'firmware-offer-choice-200',
      boundaryFinder: find.byKey(
        const ValueKey<String>('release-notice-capture'),
      ),
    );
    tester.view.physicalSize = const Size(700, 320);
    await tester.pumpAndSettle();
    expect(
      find
          .byKey(const ValueKey<String>('release-notice-opt-out'))
          .hitTestable(),
      findsOneWidget,
    );
    expect(
      find.widgetWithText(FilledButton, 'Review update').hitTestable(),
      findsOneWidget,
    );
    expect(
      find.widgetWithText(TextButton, 'Later').hitTestable(),
      findsOneWidget,
    );
    final choiceRect = tester.getRect(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    final dialogRect = tester.getRect(find.byType(AlertDialog));
    expect(choiceRect.shortestSide, greaterThanOrEqualTo(48 - 1e-9));
    expect(choiceRect.top, greaterThanOrEqualTo(dialogRect.top));
    expect(choiceRect.bottom, lessThanOrEqualTo(dialogRect.bottom));
    await captureHealthReview(
      tester,
      'firmware-offer-landscape-200-reading',
      boundaryFinder: find.byKey(
        const ValueKey<String>('release-notice-capture'),
      ),
    );
    await tester.tap(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    await tester.pumpAndSettle();
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(preferences.getBool('firmware_update_notices_disabled'), isTrue);
    expect(preferences.getBool('app_update_notices_disabled'), isTrue);
    await tester.pumpWidget(const SizedBox.shrink());
    bleService.connected = false;
    await tester.pumpWidget(appHarness(appRelease(54)));
    await tester.pumpAndSettle();
    expect(
      appNotice,
      findsNothing,
      reason: 'The app opt-out applies to future updates.',
    );

    // A pending preference save cannot disappear behind an immediate dismiss;
    // failure restores the old choice and leaves a normal exit available.
    await tester.pumpWidget(const SizedBox.shrink());
    await preferences.setBool('app_update_notices_disabled', false);
    var save = Completer<bool>();
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () => showDialog<bool>(
              context: context,
              builder: (_) => ReleaseNotesDialog(
                title: 'App notes',
                notes: const Text('Release notes'),
                preferenceExplanation: 'App update notes only.',
                suppressFuture: false,
                primaryAction: 'Continue',
                onSuppressFutureChanged: (value) async {
                  if (!await save.future) return false;
                  return preferences.setBool(
                    'app_update_notices_disabled',
                    value,
                  );
                },
              ),
            ),
            child: const Text('Open notes'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('Open notes'));
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    await tester.pump();
    await tester.binding.handlePopRoute();
    await tester.pump();
    expect(find.byType(ReleaseNotesDialog), findsOneWidget);
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, 'Continue'))
          .onPressed,
      isNull,
    );
    save.complete(false);
    await tester.pumpAndSettle();
    expect(find.text('Couldn’t save this choice. Try again.'), findsOneWidget);
    expect(
      tester
          .widget<CheckboxListTile>(
            find.byKey(const ValueKey<String>('release-notice-opt-out')),
          )
          .value,
      isFalse,
    );
    await tester.tap(find.text('Continue'));
    await tester.pumpAndSettle();
    expect(find.byType(ReleaseNotesDialog), findsNothing);
    save = Completer<bool>();
    await tester.tap(find.text('Open notes'));
    await tester.pumpAndSettle();
    await tester.tap(
      find.byKey(const ValueKey<String>('release-notice-opt-out')),
    );
    await tester.pump();
    await tester.binding.handlePopRoute();
    await tester.pump();
    expect(find.byType(ReleaseNotesDialog), findsOneWidget);
    save.complete(true);
    await tester.pumpAndSettle();
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(preferences.getBool('app_update_notices_disabled'), isTrue);
    expect(find.byType(ReleaseNotesDialog), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('board software opens directly from main Setup', (
    WidgetTester tester,
  ) async {
    final FakeLitchBleService bleService = FakeLitchBleService();
    addTearDown(bleService.dispose);

    await tester.pumpWidget(
      buildHarness(
        bleService: bleService,
        loadPreference: () async => false,
        savePreference: (bool enabled) async {},
      ),
    );
    await tester.pumpAndSettle();
    expect(
      tester.widget<AppBar>(find.byType(AppBar)).toolbarHeight,
      kToolbarHeight,
    );
    await _tapNavigationLabel(tester, 'Setup');
    await tester.pumpAndSettle();

    expect(find.byKey(const ValueKey('setup-open:support')), findsNothing);
    expect(find.byKey(const ValueKey('setup-back')), findsNothing);
    expect(find.byKey(const ValueKey('advanced-search')), findsOneWidget);

    final Finder boardSoftware = find.byKey(
      const ValueKey<String>('open-board-software'),
    );
    await scrollIntoView(tester, boardSoftware);
    await tester.tap(boardSoftware);
    await tester.pumpAndSettle();

    expect(find.text('Board update'), findsOneWidget);
    expect(find.text('Firmware Placeholder'), findsOneWidget);
  });

  testWidgets('loads persisted console preference and shows console tab', (
    WidgetTester tester,
  ) async {
    final commands = <String>[];
    final board = ConsoleSessionBoard(commands);
    final bleService = LitchBleService(
      ble: board,
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    addTearDown(() => disposeBoard(bleService, board));
    await openConsoleTestSession(tester, bleService, 'board-1');
    bleService.debugStartConsoleMirror('board-1');

    await tester.pumpWidget(
      buildHarness(
        bleService: bleService,
        loadPreference: () async => true,
        savePreference: (bool enabled) async {},
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Console'), findsOneWidget);

    await _tapNavigationLabel(tester, 'Console');
    await tester.pumpAndSettle();

    expect(_selectedNavigationLabel(tester), 'Console');
    expect(commands, isEmpty);
    board.packets.add(utf8.encode('Board console ready\n'));
    await tester.pumpAndSettle();
    expect(find.text('Board console ready'), findsOneWidget);

    await tester.enterText(find.byType(TextField), 'status');
    await tester.testTextInput.receiveAction(TextInputAction.send);
    await tester.pumpAndSettle();

    expect(commands, ['status\n']);
    await tester.pumpWidget(const SizedBox.shrink());
    var disconnected = false;
    final disconnecting =
        bleService.disconnect().whenComplete(() => disconnected = true);
    for (var turn = 0; turn < 80 && !disconnected; turn++) {
      await tester.pump(const Duration(milliseconds: 20));
      await tester.runAsync(() async {});
    }
    expect(disconnected, isTrue);
    await disconnecting;
  });

  testWidgets(
    'keeps Play selected while the Console preference and stream hydrate',
    (WidgetTester tester) async {
      final FakeLitchBleService bleService = FakeLitchBleService();
      final Completer<bool> consolePreference = Completer<bool>();
      addTearDown(bleService.dispose);

      await tester.pumpWidget(
        buildHarness(
          bleService: bleService,
          loadPreference: () => consolePreference.future,
          savePreference: (bool enabled) async {},
        ),
      );
      await tester.pump();

      expect(_selectedNavigationLabel(tester), 'Play');
      expect(find.text('Console'), findsNothing);

      consolePreference.complete(true);
      await tester.pumpAndSettle();

      expect(find.text('Console'), findsOneWidget);
      expect(_selectedNavigationLabel(tester), 'Play');
      expect(find.text('Waiting for board console stream...'), findsNothing);

      bleService.emitConsoleLines(const <String>['board stream update']);
      await tester.pumpAndSettle();

      expect(_selectedNavigationLabel(tester), 'Play');
      expect(find.text('board stream update'), findsNothing);
    },
  );

  testWidgets(
    'a held pointer cannot become Console activation while preference hydrates',
    (WidgetTester tester) async {
      final FakeLitchBleService bleService = FakeLitchBleService();
      final Completer<bool> consolePreference = Completer<bool>();
      addTearDown(bleService.dispose);
      _useViewport(tester, const Size(390, 844));

      await tester.pumpWidget(
        buildHarness(
          bleService: bleService,
          loadPreference: () => consolePreference.future,
          savePreference: (bool enabled) async {},
        ),
      );
      await tester.pump();

      final Finder navigationBar = find.byType(NavigationBar);
      final Rect navigationBounds = tester.getRect(navigationBar);
      final TestGesture gesture = await tester.startGesture(
        Offset(navigationBounds.right - 24, navigationBounds.center.dy),
      );
      await tester.pump();

      consolePreference.complete(true);
      await tester.pump();
      await gesture.up();
      await tester.pumpAndSettle();

      expect(find.text('Waiting for board console stream...'), findsNothing);
      expect(_selectedNavigationLabel(tester), isNot('Console'));
    },
  );

  testWidgets('board replacement cannot select Console', (
    WidgetTester tester,
  ) async {
    final FakeLitchBleService bleService = FakeLitchBleService();
    addTearDown(bleService.dispose);

    Widget harnessFor(String boardId) => buildHarness(
      bleService: bleService,
      boardId: boardId,
      loadPreference: () async => true,
      savePreference: (bool enabled) async {},
    );

    await tester.pumpWidget(harnessFor('board-1'));
    await tester.pumpAndSettle();
    expect(_selectedNavigationLabel(tester), 'Play');

    await tester.pumpWidget(harnessFor('board-2'));
    await tester.pumpAndSettle();

    expect(find.text('Waiting for board console stream...'), findsNothing);
    expect(_selectedNavigationLabel(tester), 'Play');
  });

  testWidgets('settings toggle persists and enables console tab', (
    WidgetTester tester,
  ) async {
    final FakeLitchBleService bleService = FakeLitchBleService();
    addTearDown(bleService.dispose);

    bool persistedValue = false;

    await tester.pumpWidget(
      buildHarness(
        bleService: bleService,
        loadPreference: () async => persistedValue,
        savePreference: (bool enabled) async {
          persistedValue = enabled;
        },
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Console'), findsNothing);

    await _tapNavigationLabel(tester, 'Setup');
    await tester.pumpAndSettle();

    await openSetupPage(tester, 'developer');

    final Finder consoleToggle = find.byKey(
      const ValueKey<String>('enable-app-console'),
    );
    await tester.ensureVisible(consoleToggle);
    await tester.pumpAndSettle();
    await tester.tap(consoleToggle);
    await tester.pumpAndSettle();

    expect(persistedValue, isTrue);
    expect(
      find.descendant(
        of: find.byWidgetPredicate(
          (widget) => widget is NavigationBar || widget is NavigationRail,
        ),
        matching: find.text('Console'),
      ),
      findsOneWidget,
    );
  });

  testWidgets('replays unresolved settings notify after index map refresh', (
    WidgetTester tester,
  ) async {
    final FakeLitchBleService bleService = FakeLitchBleService();
    addTearDown(bleService.dispose);

    final List<String> changedKeys = <String>[];
    final StreamSubscription<String> sub = bleService.settingsChangedStream
        .listen(changedKeys.add);
    addTearDown(sub.cancel);

    bleService.debugHandleSettingsRwPayload('board-1', <String, Object?>{
      'i': 7,
      'v': 6,
    });
    await tester.pump();

    expect(changedKeys, isEmpty);
    expect(bleService.latestSettingValue('BRIGHTNESS'), isNull);

    bleService.debugSetSettingsIndexMap(<int, String>{7: 'BRIGHTNESS'});
    await tester.pump();

    expect(changedKeys, <String>['BRIGHTNESS']);
    expect(bleService.latestSettingValue('BRIGHTNESS'), 6);
  });

  testWidgets('Setup explains how to recover unavailable board settings', (
    WidgetTester tester,
  ) async {
    final FakeLitchBleService bleService = FakeLitchBleService();
    bleService.settingsAuthority = LitchSettingsReadAuthority.unavailable;
    bleService.settingsToReturn = const <LitchSetting>[
      LitchSetting(
        key: 'BRIGHTNESS',
        type: 'u32',
        category: 'display',
        value: 5,
        defaultValue: 5,
        min: 1,
        max: 8,
        description: 'Board brightness step (1..8)',
      ),
    ];
    addTearDown(bleService.dispose);

    await tester.pumpWidget(
      buildHarness(
        bleService: bleService,
        loadPreference: () async => false,
        savePreference: (bool enabled) async {},
      ),
    );
    await tester.pumpAndSettle();

    await _tapNavigationLabel(tester, 'Setup');
    await tester.pumpAndSettle();

    expect(
      find.textContaining('Board settings are unavailable. Reconnect your Litch to try again.'),
      findsOneWidget,
    );
    // Brightness belongs to the Connected board sheet.
    expect(find.text('Brightness'), findsNothing);
    expect(find.text('Board brightness step (1..8)'), findsNothing);
  });

  testWidgets(
    'takeback grace is discoverable beside move confirmation and formats off',
    (WidgetTester tester) async {
      final FakeLitchBleService bleService = FakeLitchBleService();
      bleService.settingsToReturn = const <LitchSetting>[
        LitchSetting(
          key: 'MOVE_CONFIRM',
          type: 'enum',
          category: 'behavior',
          value: 'settle_2s',
          defaultValue: 'settle_2s',
          options: <String>[
            'settle_2s',
            'settle_5s',
            'press_circle_to_confirm',
          ],
          description: 'Move confirmation mode',
        ),
        LitchSetting(
          key: 'BRIGHTNESS',
          type: 'u32',
          category: 'display',
          value: 5,
          defaultValue: 5,
          min: 1,
          max: 8,
          description: 'Board brightness step (1..8)',
        ),
        LitchSetting(
          key: 'TAKEBACK_GRACE_SECONDS',
          type: 'u32',
          category: 'behavior',
          value: 0,
          defaultValue: 0,
          min: 0,
          max: 10,
          description: 'Takeback grace period in seconds (0 disables)',
        ),
      ];
      addTearDown(bleService.dispose);

      await tester.pumpWidget(
        buildHarness(
          bleService: bleService,
          loadPreference: () async => false,
          savePreference: (bool enabled) async {},
        ),
      );
      await tester.pumpAndSettle();
      await _tapNavigationLabel(tester, 'Setup');
      await tester.pumpAndSettle();

      await openSetupPage(tester, 'behavior');

      expect(find.text('Takeback grace period'), findsOneWidget);
      expect(find.text('Off'), findsOneWidget);
      expect(
        find.textContaining('Physically return a just-finished move'),
        findsOneWidget,
      );
      expect(
        tester.getTopLeft(find.text('Takeback grace period')).dy,
        greaterThan(tester.getTopLeft(find.text('Confirm moves')).dy),
      );
    },
  );
}
