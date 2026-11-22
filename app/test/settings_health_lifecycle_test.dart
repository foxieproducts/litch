import 'support/memory_ota_target_store.dart';
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_end_palette.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/models/runtime_health_snapshot.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/owner_pairing_session.dart';

import 'health_render_capture.dart';

class _Ble extends Fake implements FlutterReactiveBle {}

class _HealthService extends LitchBleService {
  _HealthService()
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _Ble());

  final updates = StreamController<ConnectionStateUpdate>.broadcast(sync: true);
  bool connected = false;
  int token = 1;
  int healthReads = 0;
  int settingsReads = 0;
  Completer<List<LitchSetting>>? pendingSettings;
  List<LitchSetting> settings = [];
  Completer<RuntimeHealthResult>? pendingHealth;
  Completer<RuntimeHealthRecheckResult>? pendingRecheck;
  int rechecks = 0;
  RuntimeHealthResult health = RuntimeHealthAvailable(
    RuntimeHealthSnapshot.fromToken('00B000000')!,
  );

  @override
  bool get isConnected => connected;
  @override
  String? get connectedId => connected ? 'board-1' : null;
  @override
  int get connectionSessionToken => token;
  @override
  Stream<ConnectionStateUpdate> get connectionStream => updates.stream;
  @override
  Future<List<LitchSetting>> readSettings(String deviceId) async {
    settingsReads++;
    return pendingSettings?.future ?? settings;
  }

  @override
  LitchSettingsReadAuthority settingsReadAuthority(String deviceId) =>
      LitchSettingsReadAuthority.confirmed;

  @override
  Future<LitchEndPalette?> readPhysicalEndPalette({
    required String deviceId,
  }) async => null;
  @override
  Future<RuntimeHealthResult> fetchRuntimeHealth({
    required String deviceId,
  }) async {
    healthReads++;
    if (!connected) return super.fetchRuntimeHealth(deviceId: deviceId);
    return pendingHealth?.future ?? health;
  }

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      const LitchDeviceCapabilities(
        contractVersion: 3,
        hardwareRevision: LitchHardwareRevision.v3,
        capabilityMask:
            LitchDeviceCapabilities.settingsBit |
            LitchDeviceCapabilities.runtimeHealthBit |
            LitchDeviceCapabilities.runtimeHealthRecheckBit,
        explicitlyReported: true,
      );

  @override
  Future<RuntimeHealthRecheckResult> recheckRuntimeHealth({
    required String deviceId,
  }) {
    rechecks++;
    return pendingRecheck!.future;
  }

  void transition(bool nextConnected) {
    connected = nextConnected;
    token++;
    updates.add(
      ConnectionStateUpdate(
        deviceId: 'board-1',
        connectionState: nextConnected
            ? DeviceConnectionState.connected
            : DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
  }

  @override
  Future<void> dispose() async {
    await updates.close();
    await super.dispose();
  }
}

Widget _harness(
  _HealthService service, {
  bool deferInitialLoad = false,
  String boardId = '',
  int healthRefreshEpoch = 0,
  double textScale = 1,
  TargetPlatform? platform,
}) => MaterialApp(
  theme: ThemeData(platform: platform),
  localizationsDelegates: AppLocalizations.localizationsDelegates,
  supportedLocales: AppLocalizations.supportedLocales,
  builder: (context, child) => MediaQuery(
    data: MediaQuery.of(
      context,
    ).copyWith(textScaler: TextScaler.linear(textScale)),
    child: child!,
  ),
  home: Scaffold(
    body: SettingsScreen(
      bleService: service,
      boardId: boardId,
      deferInitialLoad: deferInitialLoad,
      healthRefreshEpoch: healthRefreshEpoch,
      enableAppConsole: false,
      onEnableAppConsoleChanged: (_) async {},
    ),
  ),
);

final _card = find.byKey(const ValueKey<String>('runtime-health-card'));
final _spinner = find.descendant(
  of: _card,
  matching: find.byType(CircularProgressIndicator),
);

LitchSetting _lightSquares(bool value) => LitchSetting(
  key: 'WHITE_SQUARES_ON',
  type: 'bool',
  category: 'display',
  value: value,
  defaultValue: false,
  description: 'Light-square LEDs',
);

// Setup now uses real platform routes. Finish only the route transition:
// pumpAndSettle would also wait forever for intentionally pending health/load spinners.
Future<void> _finishSetupTransition(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 500));
  await tester.pump();
}

void _expectOffline() {
  expect(find.byKey(const ValueKey('setup-open:support')), findsNothing);
  expect(find.byKey(const ValueKey('setup-back')), findsNothing);
  expect(find.byKey(const ValueKey('advanced-search')), findsOneWidget);
  expect(_card, findsOneWidget);
  expect(_spinner, findsNothing);
  expect(find.text('Board health unavailable'), findsOneWidget);
  expect(find.text('Connect to the board to read its health.'), findsOneWidget);
  expect(find.text('Healthy'), findsNothing);
  expect(find.text('Checking board health'), findsNothing);
  expect(
    find.byKey(const ValueKey<String>('runtime-health-retry')),
    findsNothing,
  );
}

void main() {
  testWidgets('offline Setup entry has guidance without a health request', (
    tester,
  ) async {
    await loadHealthReviewFonts(tester, useSdkFonts: true);
    await tester.binding.setSurfaceSize(const Size(320, 700));
    addTearDown(() => tester.binding.setSurfaceSize(null));
    final service = _HealthService();
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service, textScale: 2));
    await tester.pump(const Duration(seconds: 5));
    _expectOffline();
    expect(service.healthReads, 0);
    expect(service.settingsReads, 0);
    expect(find.byType(CircularProgressIndicator), findsNothing);
    final toggle = find.byKey(const ValueKey<String>('runtime-health-toggle'));
    await tester.ensureVisible(toggle);
    await tester.pumpAndSettle();
    expect(tester.getSize(toggle).height, greaterThanOrEqualTo(48));
    expect(
      tester.getSemantics(toggle).getSemanticsData().label,
      contains('Board health unavailable'),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('prewarmed disconnected Setup is never checking health', (
    tester,
  ) async {
    final service = _HealthService();
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service, deferInitialLoad: true));
    await tester.pump();
    _expectOffline();
    expect(service.healthReads, 0);
    await tester.pumpWidget(_harness(service));
    await tester.pump(const Duration(seconds: 5));
    _expectOffline();
    expect(service.healthReads, 0);
  });

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets('returns healthy Board health to Support on $platform', (
      tester,
    ) async {
      final service = _HealthService()..connected = true;
      addTearDown(service.dispose);
      await tester.pumpWidget(_harness(service, platform: platform));
      await tester.pumpAndSettle();
      final priority = find.byKey(const ValueKey('board-health-priority'));
      expect(priority, findsNothing);
      expect(find.text('Healthy'), findsOneWidget);
      void publishHealth(String token) => service.debugAppendConsolePacket(
        '{"event":"board_health","v":1,"h":"$token","ps":"n","rr":3,"boot":2,"wd":0,"sd":0}\n'
            .codeUnits,
      );
      publishHealth('00AF20004');
      await tester.pumpAndSettle();
      expect(priority, findsOneWidget);
      expect(
        find.descendant(of: priority, matching: find.text('Input paused')),
        findsOneWidget,
      );
      publishHealth('00B000000');
      await tester.pumpAndSettle();
      expect(
        priority,
        findsNothing,
        reason: 'A recovered board no longer needs the top attention card',
      );
      expect(_card, findsOneWidget);
      expect(find.text('Healthy'), findsOneWidget);
      publishHealth('00AF20004');
      await tester.pumpAndSettle();
      expect(priority, findsOneWidget);
      expect(find.text('Input paused'), findsOneWidget);
      expect(
        service.healthReads,
        1,
        reason: 'Live health does not add another sensor read',
      );
      expect(tester.takeException(), isNull);
    });
  }

  for (final boardId in ['', 'board-1']) {
    testWidgets(
      'healthy Setup ${boardId.isEmpty ? 'without' : 'with'} a saved board follows disconnect',
      (tester) async {
        final service = _HealthService()..connected = true;
        addTearDown(service.dispose);
        await tester.pumpWidget(_harness(service, boardId: boardId));
        await tester.pumpAndSettle();
        expect(find.text('Healthy'), findsOneWidget);
        service.transition(false);
        await tester.pump();
        _expectOffline();
        expect(find.textContaining('Support code:'), findsNothing);
      },
    );
  }

  testWidgets('disconnect clears pending health and rejects its late result', (
    tester,
  ) async {
    final pending = Completer<RuntimeHealthResult>();
    final service = _HealthService()
      ..connected = true
      ..pendingHealth = pending;
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service));
    await tester.pump();
    expect(_spinner, findsOneWidget);
    service.transition(false);
    await tester.pump();
    _expectOffline();
    pending.complete(service.health);
    await tester.pump();
    _expectOffline();
  });

  testWidgets('reconnect admits new health while old generation completes', (
    tester,
  ) async {
    final oldRead = Completer<RuntimeHealthResult>();
    final currentRead = Completer<RuntimeHealthResult>();
    final service = _HealthService()
      ..connected = true
      ..pendingHealth = oldRead;
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service));
    await tester.pump();
    service.transition(false);
    await tester.pump();
    _expectOffline();
    service.pendingHealth = currentRead;
    service.transition(true);
    await tester.pump();
    expect(_spinner, findsOneWidget);
    expect(service.healthReads, 2);
    oldRead.complete(
      RuntimeHealthAvailable(RuntimeHealthSnapshot.fromToken('00AF20004')!),
    );
    await tester.pump();
    expect(find.text('Input paused'), findsNothing);
    expect(_spinner, findsOneWidget);
    currentRead.complete(service.health);
    await tester.pumpAndSettle();
    expect(find.text('Healthy'), findsOneWidget);
    expect(_spinner, findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('explicit board identity excludes another connected board health', (
    tester,
  ) async {
    final service = _HealthService()..connected = true;
    addTearDown(service.dispose);
    service.debugAppendConsolePacket(
      '{"event":"board_health","v":1,"h":"00AF20004","ps":"i","rr":6,"boot":2,"wd":1,"sd":1}\n'
          .codeUnits,
    );
    await tester.pumpWidget(_harness(service, boardId: 'board-2'));
    await tester.pump();
    _expectOffline();
    expect(find.text('Input paused'), findsNothing);
    expect(find.textContaining('Saving is unavailable'), findsNothing);
    expect(service.healthReads, 0);
    service.debugAppendConsolePacket(
      '{"event":"board_health","v":1,"h":"00AF20004","ps":"i","rr":6,"boot":2,"wd":1,"sd":1}\n'
          .codeUnits,
    );
    service.transition(true);
    await tester.pump();
    _expectOffline();
    expect(service.healthReads, 0);
  });

  testWidgets('power recheck cannot publish across disconnect and reconnect', (
    tester,
  ) async {
    final oldRecheck = Completer<RuntimeHealthRecheckResult>();
    final service = _HealthService()
      ..connected = true
      ..pendingRecheck = oldRecheck
      ..health = RuntimeHealthAvailable(
        RuntimeHealthSnapshot.fromToken('00AF10010')!,
      );
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service));
    await tester.pumpAndSettle();
    final recheck = find.byKey(
      const ValueKey<String>('runtime-health-recheck'),
    );
    await tester.ensureVisible(recheck);
    await tester.tap(recheck);
    await tester.pump();
    expect(_spinner, findsOneWidget);
    expect(service.rechecks, 1);
    service.transition(false);
    await tester.pump();
    _expectOffline();
    expect(recheck, findsNothing);
    service.health = RuntimeHealthAvailable(
      RuntimeHealthSnapshot.fromToken('00B000000')!,
    );
    service.transition(true);
    await tester.pumpAndSettle();
    expect(find.text('Healthy'), findsOneWidget);
    oldRecheck.complete(
      RuntimeHealthRecheckResult(
        outcome: RuntimeHealthRecheckOutcome.succeeded,
        health: RuntimeHealthAvailable(
          RuntimeHealthSnapshot.fromToken('00AF10010')!,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Healthy'), findsOneWidget);
    expect(find.textContaining('board accepted the power check'), findsNothing);
    expect(recheck, findsNothing);
    expect(_spinner, findsNothing);
  });

  for (final replacedBeforeFailure in [false, true]) {
    testWidgets(
      'SettingsAuthorityLossJourney ${replacedBeforeFailure ? 'old failure cannot clear a newer load' : 'current authority loss settles loading'} and fresh connection retries',
      (tester) async {
        final oldRead = Completer<List<LitchSetting>>();
        final currentRead = Completer<List<LitchSetting>>();
        final service = _HealthService()
          ..connected = true
          ..pendingSettings = oldRead;
        addTearDown(service.dispose);
        try {
          await tester.pumpWidget(_harness(service, boardId: 'board-1'));
          await tester.pump();
          expect(service.settingsReads, 1);
          expect(find.byType(CircularProgressIndicator), findsOneWidget);
          if (replacedBeforeFailure) {
            service.transition(false);
            await tester.pump();
            _expectOffline();
            service.pendingSettings = currentRead;
            service.transition(true);
            await tester.pump();
            expect(service.settingsReads, 2);
          }
          // Controlled production receiver fault: readSettings loses owner
          // authority during disconnect or GATT quarantine. The BLE guard
          // itself is unchanged; this exercises its escaped widget await.
          oldRead.completeError(const OwnerPairingNotReady());
          await tester.pump();
          expect(tester.takeException(), isNull);
          if (!replacedBeforeFailure) {
            expect(find.byType(CircularProgressIndicator), findsNothing);
            service.transition(false);
            await tester.pump();
            _expectOffline();
            service.pendingSettings = currentRead;
            service.transition(true);
            await tester.pump();
          }
          expect(service.settingsReads, 2);
          expect(
            find.byType(CircularProgressIndicator),
            findsOneWidget,
            reason:
                'The current generation still owns its pending settings load',
          );
          currentRead.complete([_lightSquares(false)]);
          await tester.pumpAndSettle();
          expect(find.byType(CircularProgressIndicator), findsNothing);
          final lights = find.byKey(const ValueKey('setup-open:lights'));
          await tester.ensureVisible(lights);
          await tester.pump();
          await tester.tap(lights);
          await _finishSetupTransition(tester);
          final lightSwitch = find.descendant(
            of: find.byKey(const ValueKey('quick:WHITE_SQUARES_ON')),
            matching: find.byType(Switch),
          );
          expect(tester.widget<Switch>(lightSwitch).value, isFalse);
          expect(tester.widget<Switch>(lightSwitch).onChanged, isNotNull);
          expect(tester.takeException(), isNull);
        } finally {
          if (!oldRead.isCompleted) oldRead.complete([]);
          if (!currentRead.isCompleted) currentRead.complete([]);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pump();
        }
      },
    );
  }

  testWidgets('disconnect retires pending Settings load before reconnect', (
    tester,
  ) async {
    final oldRead = Completer<List<LitchSetting>>();
    final currentRead = Completer<List<LitchSetting>>();
    final service = _HealthService()
      ..connected = true
      ..pendingSettings = oldRead;
    addTearDown(service.dispose);
    await tester.pumpWidget(_harness(service, boardId: 'board-1'));
    await tester.pump();
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    service.transition(false);
    await tester.pump();
    _expectOffline();
    expect(find.byType(CircularProgressIndicator), findsNothing);
    service.pendingSettings = currentRead;
    service.transition(true);
    await tester.pump();
    expect(_card, findsOneWidget);
    expect(find.text('Healthy'), findsOneWidget);
    expect(_spinner, findsNothing);
    // Current health and the pending settings generation share the overview.
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    expect(find.byKey(const ValueKey('setup-back')), findsNothing);
    expect(service.settingsReads, 2);
    oldRead.complete([_lightSquares(true)]);
    await tester.pump();
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    expect(_spinner, findsNothing);
    expect(find.text('Healthy'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('quick:WHITE_SQUARES_ON')),
      findsNothing,
    );
    currentRead.complete([_lightSquares(false)]);
    await tester.pumpAndSettle();
    final lights = find.byKey(const ValueKey<String>('setup-open:lights'));
    await tester.ensureVisible(lights);
    await tester.pump();
    await tester.tap(lights);
    await _finishSetupTransition(tester);
    final lightSquares = find.byKey(
      const ValueKey<String>('quick:WHITE_SQUARES_ON'),
    );
    expect(lightSquares, findsOneWidget);
    final lightSwitch = find.descendant(
      of: lightSquares,
      matching: find.byType(Switch),
    );
    expect(tester.widget<Switch>(lightSwitch).value, false);
    expect(tester.widget<Switch>(lightSwitch).onChanged, isNotNull);
    expect(find.byType(CircularProgressIndicator), findsNothing);
    service.transition(false);
    await tester.pump();
    expect(tester.widget<Switch>(lightSwitch).onChanged, isNull);
    await tester.tap(find.byKey(const ValueKey<String>('setup-back')));
    await _finishSetupTransition(tester);
    _expectOffline();
    final developer = find.byKey(
      const ValueKey<String>('setup-open:developer'),
    );
    await tester.ensureVisible(developer);
    await tester.pump();
    await tester.tap(developer);
    await tester.pump();
    expect(
      tester
          .widget<Switch>(
            find.byKey(const ValueKey<String>('enable-app-console')),
          )
          .onChanged,
      isNotNull,
    );
    expect(tester.takeException(), isNull);
  });
}
