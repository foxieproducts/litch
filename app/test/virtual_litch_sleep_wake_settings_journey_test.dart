import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/settings_screen.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/theme/litch_theme.dart';
import 'package:litch_mobile/src/widgets/settings_ui.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

void main() {
  for (final locale in const [Locale('en'), Locale('de')]) {
    testWidgets(
      'Sleep/Wake actual Settings UI uses native catalogue $locale',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        tester.view.physicalSize = const Size(430, 932);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(native);
        final service = LitchBleService(
          ble: ble,
          gattSessionFactory: ReactiveBleGattSessionFactory(ble),
          pendingOtaStore: MemoryOtaTargetStore(),
        );
        const boardId = 'virtual-litch-production';
        addTearDown(() => ble.disposeJourney(tester, service));

        Future<T> finish<T>(Future<T> work) async {
          var done = false;
          T? result;
          Object? error;
          work.then(
            (value) {
              result = value;
              done = true;
            },
            onError: (Object failure) {
              error = failure;
              done = true;
            },
          );
          for (var turn = 0; turn < 100 && !done; ++turn) {
            await ble.flush(tester);
            await ble.advance(tester, 20);
          }
          expect(
            done,
            isTrue,
            reason: 'Production SDK transaction did not finish',
          );
          if (error != null) throw error!;
          return result as T;
        }

        List<ConnectionSessionPacket> mutations() => ble.writes
            .where((write) => write['channel'] == 'connection_session')
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList((write['bytes'] as List).cast<int>()),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .where(
              (packet) =>
                  packet.kind == ConnectionSessionKind.ordered &&
                  packet.code == 24,
            )
            .toList();
        Finder editor(String key) =>
            find.byKey(ValueKey<String>('setting-editor:$key'));
        Future<void> search(String key) async {
          while (find
              .byKey(const ValueKey('setup-back'))
              .evaluate()
              .isNotEmpty) {
            await tester.tap(find.byKey(const ValueKey('setup-back')));
            await tester.pump();
          }
          final field = find.byKey(const ValueKey('advanced-search'));
          await tester.ensureVisible(field);
          await tester.pump();
          await tester.enterText(field, key);
          await tester.pump();
          await ble.flush(tester);
          await tester.ensureVisible(editor(key));
          await tester.pump();
        }

        Future<void> select(
          String key,
          String label,
          String token,
          List<int> expectedBody,
        ) async {
          await search(key);
          final choice = find.descendant(
            of: editor(key),
            matching: find.text(label),
          );
          expect(choice, findsOneWidget);
          await tester.ensureVisible(choice);
          final before = mutations().length;
          await tester.tap(choice);
          await tester.pump();
          await ble.flush(tester);
          await ble.advance(tester, 2500);
          expect(mutations().length, before + 1);
          expect(mutations().last.body, expectedBody);
          expect(service.latestSettingValue(key), token);
          expect(
            service.settingsReadAuthority(boardId),
            LitchSettingsReadAuthority.confirmed,
          );
          expect(
            tester.widget<SettingsChoicePills<String>>(editor(key)).selected,
            {token},
          );
          expect(
            find.byType(SnackBar),
            findsNothing,
            reason: 'The mounted editor must receive its board confirmation',
          );
          debugPrint(
            'UI_SLEEP_WAKE locale=$locale key=$key token=$token '
            'body=${mutations().last.body} displayed=$token notice=none',
          );
        }

        try {
          expect(await finish(service.connect(boardId)), isTrue);
          final session = await finish(service.openConnectionSession(boardId));
          expect(session?.isActivated, isTrue);
          final seed =
              await finish(
                    service.writeSettingAcknowledged(
                      deviceId: boardId,
                      key: 'SLEEP_TIMEOUT',
                      value: 'disabled',
                    ),
                  )
                  as LitchSettingWriteAcknowledged;
          expect(seed.outcome.ok, isTrue);
          expect(await finish(seed.boardStateConfirmation!), isTrue);
          await tester.pumpWidget(
            MaterialApp(
              locale: locale,
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              theme: buildLitchTheme(),
              home: Scaffold(
                body: SettingsScreen(
                  bleService: service,
                  boardId: boardId,
                  enableAppConsole: false,
                  onEnableAppConsoleChanged: (_) async {},
                ),
              ),
            ),
          );
          await ble.flush(tester);
          await ble.advance(tester, 100);
          final strings = AppLocalizations.of(
            tester.element(find.byType(SettingsScreen)),
          );
          await search('SLEEP_TIMEOUT');
          expect(
            tester
                .widget<SettingsChoicePills<String>>(editor('SLEEP_TIMEOUT'))
                .selected,
            {'disabled'},
          );
          expect(
            find.descendant(
              of: editor('SLEEP_TIMEOUT'),
              matching: find.text(strings.neverSleep),
            ),
            findsOneWidget,
          );
          await select('SLEEP_TIMEOUT', strings.minutesValue(5), '5m', [
            2,
            13,
            0,
            0,
          ]);
          await search('MOTION_WAKE_MODE');
          expect(service.latestSettingValue('MOTION_WAKE_MODE'), 'tap');
          expect(
            tester
                .widget<SettingsChoicePills<String>>(editor('MOTION_WAKE_MODE'))
                .selected,
            {'tap'},
          );
          await select('MOTION_WAKE_MODE', strings.motionMode, 'motion', [
            2,
            21,
            0,
            0,
          ]);
          await select(
            'MOTION_WAKE_MODE',
            strings.motionAndTapMode,
            'motion_and_tap',
            [2, 21, 2, 0],
          );

          while (find
              .byKey(const ValueKey('setup-back'))
              .evaluate()
              .isNotEmpty) {
            await tester.tap(find.byKey(const ValueKey('setup-back')));
            await tester.pump();
          }
          final field = find.byKey(const ValueKey('advanced-search'));
          await tester.enterText(field, '');
          FocusManager.instance.primaryFocus?.unfocus();
          await tester.pump();
          final lights = find.byKey(const ValueKey('setup-open:lights'));
          await tester.ensureVisible(lights);
          await tester.tap(lights);
          await tester.pumpAndSettle();
          final toggle = find.descendant(
            of: find.byKey(const ValueKey('quick:WHITE_SQUARES_ON')),
            matching: find.byType(Switch),
          );
          await tester.ensureVisible(toggle);
          await tester.pumpAndSettle();
          expect(toggle.hitTestable(), findsOneWidget);
          final before = service.latestSettingValue('WHITE_SQUARES_ON') as bool;
          await tester.tap(toggle);
          await tester.pump();
          await ble.flush(tester);
          await ble.advance(tester, 2500);
          expect(mutations().last.body, [2, 1, before ? 0 : 1, 0]);
          expect(service.latestSettingValue('WHITE_SQUARES_ON'), !before);
          expect(tester.widget<Switch>(toggle).value, !before);
          expect(find.byType(SnackBar), findsNothing);
          expect(tester.takeException(), isNull);
        } finally {
          await finish(service.disconnect());
          await tester.pumpWidget(const SizedBox.shrink());
        }
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
