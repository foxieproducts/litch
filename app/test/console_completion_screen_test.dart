import 'dart:ui' show SemanticsAction;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/models/litch_setting.dart';
import 'package:litch_mobile/src/screens/app_console_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/console_session_board.dart';
import 'support/memory_ota_target_store.dart';
import 'support/reboot_reply_ble.dart';

class _CompletionService extends LitchBleService {
  _CompletionService(ConsoleSessionBoard ble)
    : super(ble: ble, pendingOtaStore: MemoryOtaTargetStore());

  @override
  List<LitchSetting> immediateSettingsSnapshot(String deviceId) =>
      isConnected && connectedId == deviceId
      ? const [
          LitchSetting(
            key: 'BOARD_STYLE',
            type: 'enum',
            category: 'board',
            value: 'Classic',
            defaultValue: 'Classic',
            options: ['Classic', 'Modern'],
          ),
        ]
      : const [];
}

void main() {
  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets('local suggestions preserve explicit Send on $platform', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues({});
      final ble = ConsoleSessionBoard(<String>[]);
      final service = _CompletionService(ble);
      addTearDown(() => disposeBoard(service, ble));
      final semantics = tester.ensureSemantics();
      Future<void> mount() => tester.pumpWidget(
        MaterialApp(
          theme: ThemeData(platform: platform),
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await mount();
      await tester.pumpAndSettle();
      final input = find.byKey(const ValueKey('console-command-input'));
      final field = tester.widget<TextField>(input);
      await tester.enterText(input, 'fo');
      await tester.pump();
      final fox = find.byKey(const ValueKey('console-insert-fox'));
      expect(tester.getSize(fox).shortestSide, greaterThanOrEqualTo(48));
      expect(find.byTooltip('Insert fox'), findsOneWidget);
      expect(
        tester.getSemantics(fox).getSemanticsData().label,
        contains('Insert fox'),
      );
      expect(
        tester
            .getSemantics(fox)
            .getSemanticsData()
            .hasAction(SemanticsAction.tap),
        isTrue,
        reason: 'keyboard traversal exclusion retains screen-reader activation',
      );
      semantics.dispose();
      await tester.tap(fox);
      await tester.pump();
      expect(field.controller!.text, 'fox ');
      expect(field.focusNode!.hasFocus, isTrue);
      expect(
        find.byKey(const ValueKey('console-insert-fortune')),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('console-insert-fortune')));
      await tester.pump();
      expect(field.controller!.text, 'fox fortune ');
      await tester.testTextInput.receiveAction(TextInputAction.send);
      await tester.pump();
      expect(
        ble.commands,
        isEmpty,
        reason: 'offline completion and Enter cannot send',
      );
      expect(field.controller!.text, 'fox fortune ');

      await openConsoleTestSession(tester, service, 'board-1');
      await mount();
      await tester.pump();
      expect(ble.commands, isEmpty, reason: 'reconnect never submits the draft');
      await tester.testTextInput.receiveAction(TextInputAction.send);
      await tester.pump();
      expect(ble.commands, ['fox fortune\n']);
      expect(field.controller!.text, isEmpty);

      await tester.enterText(input, 'settings set BO');
      await tester.pump();
      await tester.tap(
        find.byKey(const ValueKey('console-insert-BOARD_STYLE')),
      );
      await tester.pump();
      await tester.tap(find.byKey(const ValueKey('console-insert-Modern')));
      await tester.pump();
      expect(field.controller!.text, 'settings set BOARD_STYLE Modern ');
      expect(
        ble.commands,
        hasLength(1),
        reason: 'cached metadata makes no extra BLE request',
      );

      await tester.enterText(input, 'fox f');
      await tester.pump();
      final held = tester
          .widget<TextButton>(
            find.byKey(const ValueKey('console-insert-fortune')),
          )
          .onPressed!;
      field.controller!.value = const TextEditingValue(
        text: 'unrelated new draft',
        selection: TextSelection.collapsed(offset: 19),
      );
      held();
      expect(field.controller!.text, 'unrelated new draft');
      await tester.pump();

      await tester.enterText(input, 'fo');
      await tester.pump();
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pump();
      expect(field.controller!.text, 'fox ');
      expect(field.focusNode!.hasFocus, isTrue);
      expect(ble.commands, hasLength(1));
      await tester.enterText(input, 'fo');
      await tester.pump();
      await tester.sendKeyDownEvent(LogicalKeyboardKey.shiftLeft);
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.shiftLeft);
      await tester.pumpAndSettle();
      expect(
        field.controller!.text,
        'fo',
        reason: 'Shift+Tab retains focus traversal',
      );
      final focusedContext = FocusManager.instance.primaryFocus?.context;
      expect(focusedContext, isNotNull);
      expect(
        find.ancestor(
          of: find.byWidget(focusedContext!.widget),
          matching: find.byType(SelectionArea),
        ),
        findsOneWidget,
        reason: 'Shift+Tab must land on the surviving output selection region',
      );
      expect(field.focusNode!.hasFocus, isFalse);
      expect(find.byKey(const ValueKey('console-suggestions')), findsNothing);
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pumpAndSettle();
      expect(field.focusNode!.hasPrimaryFocus, isTrue);
      expect(field.controller!.text, 'fo');
      expect(find.byKey(const ValueKey('console-insert-fox')), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pumpAndSettle();
      expect(field.controller!.text, 'fox ');
      expect(field.focusNode!.hasPrimaryFocus, isTrue);
      expect(
        ble.commands,
        hasLength(1),
        reason: 'traversal and completion never send',
      );
      field.controller!.value = const TextEditingValue(
        text: 'fo',
        selection: TextSelection.collapsed(offset: 2),
        composing: TextRange(start: 0, end: 2),
      );
      await tester.pump();
      expect(find.byKey(const ValueKey('console-suggestions')), findsNothing);
      expect(tester.takeException(), isNull);
      await service.disconnect();
    });

    testWidgets(
      'suggestions scroll at 200 percent in landscape RTL on $platform',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        tester.view.physicalSize = const Size(844, 390);
        tester.view.devicePixelRatio = 1;
        tester.view.viewInsets = const FakeViewPadding(bottom: 140);
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        addTearDown(tester.view.resetViewInsets);
        final ble = ConsoleSessionBoard(<String>[]);
        final service = _CompletionService(ble);
        addTearDown(() => disposeBoard(service, ble));
        await tester.pumpWidget(
          MaterialApp(
            theme: ThemeData(platform: platform),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: Directionality(
                textDirection: TextDirection.rtl,
                child: child!,
              ),
            ),
            home: Scaffold(
              body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final input = find.byKey(const ValueKey('console-command-input'));
        await tester.enterText(input, 'settings ');
        await tester.pump();
        for (final label in ['status', 'keys', 'get']) {
          final button = find.byKey(ValueKey('console-insert-$label'));
          await tester.ensureVisible(button);
          expect(tester.getSize(button).height, greaterThanOrEqualTo(48));
        }
        await tester.tap(find.byKey(const ValueKey('console-insert-get')));
        await tester.pump();
        expect(
          tester.widget<TextField>(input).controller!.text,
          'settings get ',
        );
        expect(ble.commands, isEmpty);
        expect(tester.takeException(), isNull);
      },
    );
  }
}
