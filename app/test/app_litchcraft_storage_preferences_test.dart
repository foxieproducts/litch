import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/main.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/services/app_litchcraft_preferences.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'storage defaults to decimal 1 GB and restores a custom limit',
    () async {
      final platform = _PreferencesPlatform()..install();
      expect(await AppLitchcraftPreferences.loadStorageLimit(), 1_000_000_000);
      await AppLitchcraftPreferences.saveStorageLimit(2_500_000_000);
      expect(platform.values[_PreferencesPlatform.storageKey], 2_500_000_000);
      SharedPreferences.resetStatic();
      expect(await AppLitchcraftPreferences.loadStorageLimit(), 2_500_000_000);
    },
  );

  test('the receiving preference boundary rejects invalid budgets', () async {
    final platform = _PreferencesPlatform()..install();
    for (final bytes in <int>[
      -1,
      0,
      AppLitchcraftPreferences.minimumStorageLimitBytes - 1,
      AppLitchcraftPreferences.maximumStorageLimitBytes + 1,
    ]) {
      await expectLater(
        AppLitchcraftPreferences.saveStorageLimit(bytes),
        throwsRangeError,
      );
    }
    expect(platform.values, isEmpty);
    for (final value in <Object>[
      0,
      AppLitchcraftPreferences.maximumStorageLimitBytes + 1,
      'invalid',
    ]) {
      platform.values[_PreferencesPlatform.storageKey] = value;
      SharedPreferences.resetStatic();
      expect(
        await AppLitchcraftPreferences.loadStorageLimit(),
        AppLitchcraftPreferences.defaultStorageLimitBytes,
      );
    }
    await AppLitchcraftPreferences.saveStorageLimit(
      AppLitchcraftPreferences.minimumStorageLimitBytes,
    );
    expect(await AppLitchcraftPreferences.loadStorageLimit(), 1_000_000);
  });

  test('rejected writes restore the same-instance backend budget', () async {
    final platform = _PreferencesPlatform()..install();
    platform.values[_PreferencesPlatform.storageKey] = 2_000_000_000;
    final preferences = await SharedPreferences.getInstance();
    platform.rejectNextWrite = true;
    await expectLater(
      AppLitchcraftPreferences.saveStorageLimit(1_000_000_000),
      throwsStateError,
    );
    expect(
      preferences.getInt(AppLitchcraftPreferences.storageLimitKey),
      2_000_000_000,
    );
    expect(await AppLitchcraftPreferences.loadStorageLimit(), 2_000_000_000);
    expect(platform.values[_PreferencesPlatform.storageKey], 2_000_000_000);

    platform.throwEveryWrite = true;
    await expectLater(
      AppLitchcraftPreferences.saveStorageLimit(1_000_000_000),
      throwsA(isA<PlatformException>()),
    );
    expect(
      preferences.getInt(AppLitchcraftPreferences.storageLimitKey),
      2_000_000_000,
    );
  });

  testWidgets(
    'storage edit is accessible at 200% text and keeps failed limit',
    (tester) async {
      final platform = _PreferencesPlatform()..install();
      final budget = ValueNotifier<int>(1_000_000_000);
      addTearDown(budget.dispose);
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: TextScaler.linear(2)),
            child: ValueListenableBuilder<int>(
              valueListenable: budget,
              builder: (context, bytes, _) => AppPreferencesScope(
                appearanceMode: LitchAppearanceMode.system,
                onAppearanceModeChanged: (_) async {},
                localePreference: const LitchLocalePreference.system(),
                onLocalePreferenceChanged: (_) async {},
                litchcraftStorageLimitBytes: bytes,
                onLitchcraftStorageLimitChanged: (next) async {
                  await AppLitchcraftPreferences.saveStorageLimit(next);
                  budget.value = next;
                },
                child: child!,
              ),
            ),
          ),
          home: const AppSettingsScreen(),
        ),
      );
      await tester.pumpAndSettle();
      final preference = find.byKey(
        const ValueKey<String>('app-litchcraft-storage-limit'),
      );
      await tester.ensureVisible(preference);
      await tester.pumpAndSettle();
      expect(tester.getSize(preference).height, greaterThanOrEqualTo(48));
      expect(
        tester.getSemantics(preference).label,
        contains('Artwork storage limit'),
      );
      expect(find.text('1000 MB'), findsOneWidget);
      await tester.tap(preference);
      await tester.pumpAndSettle();
      final input = find.byKey(
        const ValueKey<String>('app-litchcraft-storage-megabytes'),
      );
      final save = find.byKey(
        const ValueKey<String>('app-litchcraft-storage-save'),
      );
      expect(tester.widget<TextField>(input).controller!.text, '1000');
      for (final value in <String>['', '0', '-3', '1.5', '9007199255']) {
        await tester.enterText(input, value);
        await tester.ensureVisible(save);
        await tester.tap(save);
        await tester.pumpAndSettle();
        expect(budget.value, 1_000_000_000);
        expect(platform.values, isEmpty);
        expect(find.byType(AlertDialog), findsOneWidget);
      }
      platform.rejectNextWrite = true;
      await tester.enterText(input, '2000');
      await tester.ensureVisible(save);
      await tester.tap(save);
      await tester.pumpAndSettle();
      expect(
        find.text('Could not save the storage limit. Try again.'),
        findsOneWidget,
      );
      expect(budget.value, 1_000_000_000);
      expect(await AppLitchcraftPreferences.loadStorageLimit(), 1_000_000_000);
      await tester.tap(save);
      await tester.pumpAndSettle();
      expect(find.byType(AlertDialog), findsNothing);
      expect(find.text('2000 MB'), findsOneWidget);
      expect(budget.value, 2_000_000_000);
      expect(tester.takeException(), isNull);
      semantics.dispose();
    },
  );

  testWidgets('offline Setup saves the app storage limit across restart', (
    tester,
  ) async {
    final platform = _PreferencesPlatform()..install();
    final service = await _pumpOfflineApp(tester);
    await _openOfflineSetup(tester);
    expect(find.text('1000 MB'), findsOneWidget);
    await tester.tap(_storagePreference);
    await tester.pumpAndSettle();
    await tester.enterText(_storageInput, '2500');
    await tester.tap(_storageSave);
    await tester.pumpAndSettle();
    expect(find.text('2500 MB'), findsOneWidget);
    expect(_rootBudget(tester), 2_500_000_000);
    expect(platform.values[_PreferencesPlatform.storageKey], 2_500_000_000);
    expect(service.connectedId, isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpAndSettle();
    SharedPreferences.resetStatic();
    await tester.pumpWidget(LitchMobileApp(bleService: service));
    await tester.pumpAndSettle();
    await _openOfflineSetup(tester);
    expect(find.text('2500 MB'), findsOneWidget);
    expect(_rootBudget(tester), 2_500_000_000);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });

  testWidgets('root publishes a new budget only after persistence succeeds', (
    tester,
  ) async {
    final platform = _PreferencesPlatform()..install();
    platform.values[_PreferencesPlatform.storageKey] = 2_000_000_000;
    final directory = (await tester.runAsync(
      () => Directory.systemTemp.createTemp('litch-root-budget-'),
    ))!;
    const pathChannel = MethodChannel('plugins.flutter.io/path_provider');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      pathChannel,
      (call) async => call.method == 'getApplicationSupportDirectory'
          ? directory.path
          : null,
    );
    addTearDown(() async {
      messenger.setMockMethodCallHandler(pathChannel, null);
      await directory.delete(recursive: true);
    });
    await _pumpOfflineApp(tester);
    late LitchcraftArtworkStore effectiveStore;
    await tester.runAsync(() async {
      effectiveStore = await tester
          .widget<ConnectionGateScreen>(find.byType(ConnectionGateScreen))
          .craftStoreLoader!();
    });
    expect(effectiveStore.storageLimitBytes, 2_000_000_000);
    await _openOfflineSetup(tester);
    expect(find.text('2000 MB'), findsOneWidget);
    await tester.tap(_storagePreference);
    await tester.pumpAndSettle();
    await tester.enterText(_storageInput, '5000');
    platform.holdNextWrite = Completer<void>();
    platform.rejectNextWrite = true;
    await tester.tap(_storageSave);
    await tester.pump();
    expect(_rootBudget(tester), 2_000_000_000);
    expect(
      effectiveStore.storageLimitBytes,
      2_000_000_000,
      reason: 'a pending increase is not the file store admission budget',
    );
    expect(tester.widget<FilledButton>(_storageSave).onPressed, isNull);
    platform.holdNextWrite!.complete();
    await tester.pumpAndSettle();
    expect(
      find.text('Could not save the storage limit. Try again.'),
      findsOneWidget,
    );
    expect(_rootBudget(tester), 2_000_000_000);
    final preferences = await SharedPreferences.getInstance();
    expect(
      preferences.getInt(AppLitchcraftPreferences.storageLimitKey),
      2_000_000_000,
    );
    await tester.tap(_storageSave);
    await tester.pumpAndSettle();
    expect(_rootBudget(tester), 5_000_000_000);
    expect(effectiveStore.storageLimitBytes, 5_000_000_000);
    expect(find.text('5000 MB'), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });
}

final _storagePreference = find.byKey(
  const ValueKey<String>('app-litchcraft-storage-limit'),
);
final _storageInput = find.byKey(
  const ValueKey<String>('app-litchcraft-storage-megabytes'),
);
final _storageSave = find.byKey(
  const ValueKey<String>('app-litchcraft-storage-save'),
);

int _rootBudget(WidgetTester tester) => AppPreferencesScope.maybeOf(
  tester.element(find.byType(ConnectionGateScreen)),
)!.litchcraftStorageLimitBytes;

Future<void> _openOfflineSetup(WidgetTester tester) async {
  await tester.tap(
    find.byKey(const ValueKey<String>('disconnected-destination-setup')),
  );
  await tester.pumpAndSettle();
  await tester.ensureVisible(_storagePreference);
  await tester.pumpAndSettle();
}

Future<LitchBleService> _pumpOfflineApp(WidgetTester tester) async {
  tester.view.physicalSize = const Size(390, 844);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
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
  await tester.pumpAndSettle();
  return service;
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

final class _PreferencesPlatform {
  static const channel = MethodChannel('plugins.flutter.io/shared_preferences');
  static const storageKey =
      'flutter.${AppLitchcraftPreferences.storageLimitKey}';
  final values = <String, Object>{};
  bool rejectNextWrite = false;
  bool throwEveryWrite = false;
  Completer<void>? holdNextWrite;

  void install() {
    SharedPreferences.resetStatic();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'getAll') return Map<String, Object>.from(values);
      if (holdNextWrite case final pending?) {
        await pending.future;
        holdNextWrite = null;
      }
      if (throwEveryWrite) throw PlatformException(code: 'unavailable');
      if (rejectNextWrite) {
        rejectNextWrite = false;
        return false;
      }
      final arguments = call.arguments as Map<Object?, Object?>;
      final key = arguments['key']! as String;
      switch (call.method) {
        case 'setInt':
          values[key] = arguments['value']!;
          return true;
        case 'remove':
          values.remove(key);
          return true;
        default:
          throw StateError('Unexpected preference operation: ${call.method}');
      }
    });
    addTearDown(() {
      messenger.setMockMethodCallHandler(channel, null);
      SharedPreferences.resetStatic();
    });
  }
}
