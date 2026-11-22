import 'support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/screens/app_console_screen.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'support/console_session_board.dart';
import 'support/reboot_reply_ble.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _ConsoleBle extends ConsoleSessionBoard {
  _ConsoleBle() : super(<String>[]);

  bool rejectCommands = false;
  Completer<void>? deferredReceipt;
  int? _terminalSequence;
  int _session = 0;

  @override
  int orderedRejection(ConnectionSessionPacket packet) {
    if (packet.code == LitchControlProtocol.connectionConsoleOpcode) {
      if (rejectCommands ||
          packet.body[1] & LitchControlProtocol.connectionConsoleEnd != 0) {
        _terminalSequence = packet.sequence;
      }
      if (rejectCommands) return 4;
    }
    return super.orderedRejection(packet);
  }

  @override
  void control(int code, int token, int sequence, Uint8List body) {
    if (code == ConnectionSessionControl.activated) {
      ++_session;
      _terminalSequence = null;
    }
    final gate = deferredReceipt;
    if (gate != null &&
        sequence == _terminalSequence &&
        (code == ConnectionSessionControl.checkpoint ||
            code == ConnectionSessionControl.outcome)) {
      final session = _session;
      unawaited(gate.future.then((_) {
        if (session == _session) super.control(code, token, sequence, body);
      }));
      return;
    }
    super.control(code, token, sequence, body);
  }
}

class _TimestampConsoleService extends LitchBleService {
  _TimestampConsoleService(this.lines)
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _ConsoleBle());
  final List<String> lines;
  final updates = StreamController<List<String>>.broadcast();

  @override
  List<String> get consoleLinesSnapshot => lines;

  @override
  Stream<List<String>> get consoleStream => updates.stream;

  @override
  Future<void> dispose() async {
    await updates.close();
    await super.dispose();
  }
}

void main() {
  testWidgets(
    'timestamps default hidden, cancel, apply, relaunch and Defaults',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues({});
      final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
      addTearDown(service.dispose);
      Future<void> mount() async {
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
            ),
          ),
        );
        await tester.pumpAndSettle();
      }

      Future<void> tap(String key) async {
        final control = find.byKey(ValueKey(key));
        await tester.ensureVisible(control);
        await tester.tap(control);
        await tester.pumpAndSettle();
      }

      await mount();
      const welcome = ValueKey('console-welcome');
      var state = tester.state<AppConsoleScreenState>(
        find.byType(AppConsoleScreen),
      );
      final preferences = await SharedPreferences.getInstance();
      expect(find.byKey(welcome), findsNothing);
      expect(preferences.getBool('console_welcome_seen'), isNull);
      state.activateConsole();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 325));
      expect(find.textContaining('The L', findRichText: true), findsOneWidget);
      expect(preferences.getBool('console_welcome_seen'), isTrue);
      await tester.pump(const Duration(milliseconds: 2707));
      expect(
        find.textContaining('The Litch has you.', findRichText: true),
        findsOneWidget,
      );
      expect(
        find.textContaining('Follow the orange fox ', findRichText: true),
        findsOneWidget,
      );
      expect(find.byKey(welcome), findsOneWidget);
      await tester.pump(const Duration(milliseconds: 1982));
      expect(find.byKey(welcome), findsOneWidget);
      expect(
        tester
            .widget<FractionalTranslation>(
              find.descendant(
                of: find.byKey(welcome),
                matching: find.byType(FractionalTranslation),
              ),
            )
            .translation,
        Offset.zero,
      );
      await tester.pump(const Duration(milliseconds: 278));
      expect(find.byKey(welcome), findsNothing);
      state.deactivateConsole();
      state.activateConsole();
      await tester.pumpAndSettle();
      expect(find.byKey(welcome), findsNothing);
      expect(find.text('ready'), findsOneWidget);
      expect(
        find.textContaining('49:00.000', findRichText: true),
        findsNothing,
      );
      await tap('console-appearance-button');
      expect(
        tester
            .widget<SwitchListTile>(
              find.byKey(const ValueKey('console-timestamps-toggle')),
            )
            .value,
        isFalse,
      );
      await tap('console-timestamps-toggle');
      expect(
        find.textContaining('12:34:56.000', findRichText: true),
        findsOneWidget,
      );
      await tap('console-appearance-cancel');
      expect(find.text('ready'), findsOneWidget);
      await tap('console-appearance-button');
      await tap('console-timestamps-toggle');
      await tap('console-appearance-apply');
      expect(
        find.textContaining('49:00.000', findRichText: true),
        findsOneWidget,
      );
      expect(preferences.getBool('console_show_timestamps'), isTrue);
      expect(service.consoleLinesSnapshot, ['01:49:00.000 [FW] ready']);
      await tester.pumpWidget(const SizedBox());
      await mount();
      state = tester.state<AppConsoleScreenState>(
        find.byType(AppConsoleScreen),
      );
      state.activateConsole();
      await tester.pumpAndSettle();
      expect(find.byKey(welcome), findsNothing);
      expect(
        find.textContaining('49:00.000', findRichText: true),
        findsOneWidget,
      );
      await tap('console-appearance-button');
      expect(
        tester
            .widget<SwitchListTile>(
              find.byKey(const ValueKey('console-timestamps-toggle')),
            )
            .value,
        isTrue,
      );
      await tap('console-appearance-defaults');
      await tap('console-appearance-cancel');
      expect(preferences.getBool('console_welcome_seen'), isTrue);
      await tap('console-appearance-button');
      await tap('console-appearance-defaults');
      await tap('console-appearance-undo');
      await tap('console-appearance-apply');
      expect(preferences.getBool('console_welcome_seen'), isTrue);
      await tap('console-appearance-button');
      await tap('console-appearance-defaults');
      await tap('console-appearance-apply');
      expect(preferences.getBool('console_welcome_seen'), isFalse);
      expect(find.byKey(welcome), findsNothing);
      state.deactivateConsole();
      state.activateConsole();
      await tester.pump();
      expect(find.byKey(welcome), findsOneWidget);
      state.deactivateConsole();
      await tester.pumpAndSettle();
      await tap('console-appearance-button');
      await tap('console-appearance-defaults');
      await tap('console-appearance-apply');
      tester.platformDispatcher.accessibilityFeaturesTestValue =
          const FakeAccessibilityFeatures(disableAnimations: true);
      tester.platformDispatcher.textScaleFactorTestValue = 2;
      tester.view.physicalSize = const Size(844, 390);
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      addTearDown(
        tester.platformDispatcher.clearAccessibilityFeaturesTestValue,
      );
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      addTearDown(tester.view.resetViewInsets);
      await tester.pump();
      state.activateConsole();
      await tester.pump();
      expect(
        find.textContaining('The Litch has you.', findRichText: true),
        findsOneWidget,
      );
      expect(
        find.textContaining('Follow the orange fox ', findRichText: true),
        findsOneWidget,
      );
      await tester.pump(const Duration(milliseconds: 1999));
      expect(find.byKey(welcome), findsOneWidget);
      await tester.pump(const Duration(milliseconds: 17));
      expect(find.byKey(welcome), findsNothing);
      expect(tester.takeException(), isNull);
      expect(preferences.getBool('console_show_timestamps'), isFalse);
      expect(find.text('ready'), findsOneWidget);
      expect(
        find.textContaining('49:00.000', findRichText: true),
        findsNothing,
      );
    },
  );

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      'native output copy and command selection/paste on $platform',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        String? copied;
        tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
          SystemChannels.platform,
          (call) async {
            if (call.method == 'Clipboard.setData') {
              copied = (call.arguments as Map)['text'] as String;
            }
            if (call.method == 'Clipboard.getData') {
              return {'text': copied ?? 'help'};
            }
            if (call.method == 'Clipboard.hasStrings') return {'value': true};
            return null;
          },
        );
        addTearDown(
          () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
            SystemChannels.platform,
            null,
          ),
        );
        final service = _TimestampConsoleService(['Selectable reply']);
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(
            theme: ThemeData(platform: platform),
            home: Scaffold(
              body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
            ),
          ),
        );
        await tester.pumpAndSettle();
        await tester.longPressAt(
          tester.getTopLeft(find.text('Selectable reply')) +
              const Offset(20, 8),
        );
        await tester.pumpAndSettle();
        expect(find.text('Console appearance'), findsNothing);
        expect(find.text('Copy'), findsOneWidget);
        service.updates.add(['Selectable reply', 'Asynchronous result']);
        await tester.pump();
        await tester.tap(find.text('Copy'));
        await tester.pumpAndSettle();
        expect(copied, isNotEmpty);
        expect('Selectable reply', contains(copied!));
        final input = find.byKey(const ValueKey('console-command-input'));
        expect(
          tester.widget<TextField>(input).enableInteractiveSelection,
          isTrue,
        );
        await tester.enterText(input, 'status');
        await tester.pump();
        final editable = tester.state<EditableTextState>(
          find.descendant(of: input, matching: find.byType(EditableText)),
        );
        final caret = editable.renderEditable.getLocalRectForCaret(
          const TextPosition(offset: 1),
        );
        final point = editable.renderEditable.localToGlobal(
          caret.center + const Offset(2, 0),
        );
        if (platform == TargetPlatform.iOS) {
          // iOS long press moves the caret; double tap selects a word.
          await tester.tapAt(point);
          await tester.pump(const Duration(milliseconds: 50));
          await tester.tapAt(point);
        } else {
          await tester.longPressAt(point);
        }
        await tester.pumpAndSettle();
        expect(editable.textEditingValue.selection.isCollapsed, isFalse);
        copied = 'help';
        await editable.pasteText(SelectionChangedCause.toolbar);
        await tester.pumpAndSettle();
        expect(tester.widget<TextField>(input).controller!.text, 'help');
        expect(tester.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(platform),
    );
  }

  testWidgets(
    'appearance gear and switches stay accessible at 200 percent RTL offline',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      tester.view.physicalSize = const Size(320, 700);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final semantics = tester.ensureSemantics();
      final service = _TimestampConsoleService(['Offline history']);
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => Directionality(
            textDirection: TextDirection.rtl,
            child: MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
          ),
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final gear = find.byKey(const ValueKey('console-appearance-button'));
      expect(tester.getSize(gear), const Size(48, 48));
      expect(
        tester.getSemantics(gear).getSemanticsData().tooltip,
        'Console appearance',
      );
      await tester.tap(gear);
      await tester.pumpAndSettle();
      final timestamps = find.byKey(
        const ValueKey('console-timestamps-toggle'),
      );
      final trace = find.byKey(const ValueKey('console-trace-output-toggle'));
      expect(tester.widget<SwitchListTile>(timestamps).onChanged, isNotNull);
      expect(tester.widget<SwitchListTile>(trace).onChanged, isNull);
      expect(find.text('Not connected'), findsWidgets);
      expect(tester.getSize(timestamps).height, greaterThanOrEqualTo(48));
      expect(tester.getSize(trace).height, greaterThanOrEqualTo(48));
      semantics.dispose();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'native output selection holds reading position during new and evicted history',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      final lines = List.generate(120, (index) => 'Reply number $index');
      final service = _TimestampConsoleService(lines);
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final log = find.byType(ListView);
      final controller = tester.widget<ListView>(log).controller!;
      controller.jumpTo(40);
      await tester.pump();
      final reply = find.text('Reply number 5');
      await tester.longPressAt(tester.getTopLeft(reply) + const Offset(20, 8));
      await tester.pumpAndSettle();
      final position = controller.offset;
      service.updates.add([...lines.skip(1), 'Reply number 120']);
      await tester.pumpAndSettle();
      expect(controller.offset, position);
      expect(find.text('Copy'), findsOneWidget);
      expect(find.text('Console appearance'), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'commands and blank Enter keep the keyboard until explicit exit',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      final fakeBle = _ConsoleBle();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() => disposeBoard(service, fakeBle));
      await openConsoleTestSession(tester, service, 'board-1');
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      final input = find.byKey(const ValueKey('console-command-input'));
      Future<void> submitWithoutKeyboardRestart() async {
        final inputCallCount = tester.testTextInput.log.length;
        await tester.testTextInput.receiveAction(TextInputAction.send);
        await tester.pumpAndSettle();
        expect(
          tester.testTextInput.log
              .skip(inputCallCount)
              .map((call) => call.method)
              .where(
                (method) => const {
                  'TextInput.clearClient',
                  'TextInput.setClient',
                  'TextInput.hide',
                  'TextInput.show',
                }.contains(method),
              ),
          isEmpty,
          reason:
              'Send must retain the native keyboard connection continuously',
        );
        expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
        expect(tester.testTextInput.isVisible, isTrue);
      }

      await tester.enterText(input, 'amptest');
      await submitWithoutKeyboardRestart();
      expect(fakeBle.commands, ['amptest\n']);
      expect(tester.widget<TextField>(input).controller!.text, isEmpty);

      service.debugAppendConsolePacket(
        utf8.encode('> amptest\nReady\namptest ready\nx > y\n  [FW] 5 > 3\n'),
      );
      await tester.pumpAndSettle();
      for (final line in [
        '> amptest',
        'Ready',
        'amptest ready',
        'x > y',
        '  [FW] 5 > 3',
      ]) {
        expect(find.text(line), findsOneWidget);
      }

      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      expect(tester.testTextInput.isVisible, isTrue);
      // Reading output must not dismiss the command keyboard either.
      await tester.tap(find.byType(ListView));
      await tester.pump();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      await submitWithoutKeyboardRestart();
      expect(fakeBle.commands, ['amptest\n', '\n']);
      expect(tester.testTextInput.isVisible, isTrue);

      fakeBle.rejectCommands = true;
      await tester.enterText(input, 'help');
      await submitWithoutKeyboardRestart();
      expect(tester.widget<TextField>(input).controller!.text, 'help');
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      expect(tester.testTextInput.isVisible, isTrue);

      fakeBle.rejectCommands = false;
      fakeBle.deferredReceipt = Completer<void>();
      await submitWithoutKeyboardRestart();
      await tester.enterText(input, 'status');
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      addTearDown(tester.view.resetViewInsets);
      await tester.pump();
      await tester.tap(find.byTooltip('Hide keyboard'));
      await tester.pump();
      fakeBle.deferredReceipt!.complete();
      await tester.pumpAndSettle();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isFalse);
      expect(tester.testTextInput.isVisible, isFalse);
      expect(fakeBle.commands, ['amptest\n', '\n', 'help\n']);
      expect(tester.widget<TextField>(input).controller!.text, 'status');

      await tester.tap(input);
      await tester.pumpAndSettle();
      expect(tester.testTextInput.isVisible, isTrue);
      final state = tester.state<AppConsoleScreenState>(
        find.byType(AppConsoleScreen),
      );
      fakeBle.deferredReceipt = Completer<void>();
      await tester.enterText(input, 'status');
      await submitWithoutKeyboardRestart();
      state.deactivateConsole();
      await tester.pump();
      fakeBle.deferredReceipt!.complete();
      await tester.pumpAndSettle();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isFalse);
      expect(tester.testTextInput.isVisible, isFalse);
      expect(fakeBle.commands, ['amptest\n', '\n', 'help\n', 'status\n']);
      expect(tester.takeException(), isNull);
      await service.disconnect();
    },
  );

  testWidgets(
    'command input taps reveal latest logs through keyboard resize and repeat focus',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetViewInsets);
      final lines = List.generate(80, (index) => 'Log line $index');
      final service = _TimestampConsoleService(lines);
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final log = find.byType(ListView);
      final input = find.byKey(const ValueKey('console-command-input'));
      final controller = tester.widget<ListView>(log).controller!;

      Future<void> readEarlierLogs() async {
        await tester.sendEventToBinding(
          PointerScrollEvent(
            position: tester.getCenter(log),
            scrollDelta: const Offset(0, -5000),
          ),
        );
        await tester.pump();
        expect(controller.position.extentAfter, greaterThan(300));
      }

      void expectLatestVisible(String text) {
        expect(controller.position.extentAfter, closeTo(0, 1));
        final line = find.text(text);
        expect(line.hitTestable(), findsOneWidget);
        expect(
          tester.getRect(line).bottom,
          lessThan(tester.getRect(input).top),
        );
      }

      await readEarlierLogs();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isFalse);
      await tester.tap(input);
      await tester.pumpAndSettle();
      expectLatestVisible('Log line 79');
      await tester.enterText(input, 'help');

      // Keyboard entry and a later animation frame change the log viewport.
      tester.view.viewInsets = const FakeViewPadding(bottom: 160);
      await tester.pumpAndSettle();
      service.updates.add([...lines, 'Newest board reply']);
      tester.view.viewInsets = const FakeViewPadding(bottom: 300);
      await tester.pumpAndSettle();
      expectLatestVisible('Newest board reply');

      // A mouse wheel can browse history without changing command focus.
      await readEarlierLogs();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      await tester.tap(input);
      await tester.idle();
      expect(
        tester.binding.hasScheduledFrame,
        isTrue,
        reason: 'A focused tap must repaint without waiting for another event',
      );
      await tester.pump();
      await tester.pump();
      expectLatestVisible('Newest board reply');
      await readEarlierLogs();
      await tester.tap(input);
      await tester.idle();
      expect(
        tester.binding.hasScheduledFrame,
        isTrue,
        reason: 'A focused tap must repaint without waiting for another event',
      );
      await tester.pump();
      await tester.pump();
      expectLatestVisible('Newest board reply');
      expect(tester.widget<TextField>(input).controller!.text, 'help');
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('console appearance offers one picker including timestamps', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues({});
    final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
        ),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
    await tester.pumpAndSettle();
    expect(find.text('Timestamp'), findsOneWidget);
    expect(find.byKey(const ValueKey('console-color-hue')), findsOneWidget);
    expect(
      find.byKey(const ValueKey('console-color-saturation')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey('console-color-brightness')),
      findsOneWidget,
    );
    final targetRects = <Rect>[
      tester.getRect(
        find
            .ancestor(
              of: find.byKey(const ValueKey('console-foreground-color')),
              matching: find.byType(TextButton),
            )
            .first,
      ),
      tester.getRect(
        find
            .ancestor(
              of: find.byKey(const ValueKey('console-background-color')),
              matching: find.byType(TextButton),
            )
            .first,
      ),
      tester.getRect(
        find
            .ancestor(
              of: find.byKey(const ValueKey('console-timestamp-color')),
              matching: find.byType(TextButton),
            )
            .first,
      ),
    ];
    expect(
      targetRects.map((rect) => rect.top).toSet(),
      hasLength(1),
      reason: 'all three color targets stay on one horizontal line',
    );
    expect(targetRects.every((rect) => rect.height >= 48), isTrue);
  });
  testWidgets(
    'console color targets keep full text and reachable choices at 200%',
    (tester) async {
      tester.view.physicalSize = const Size(320, 700);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      SharedPreferences.setMockInitialValues({});
      final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(
              context,
            ).copyWith(textScaler: const TextScaler.linear(2)),
            child: child!,
          ),
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
      await tester.pumpAndSettle();

      final targets = <Finder>[
        find.byKey(const ValueKey('console-foreground-color')),
        find.byKey(const ValueKey('console-background-color')),
        find.byKey(const ValueKey('console-timestamp-color')),
      ];
      final rects = targets
          .map(
            (target) => tester.getRect(
              find
                  .ancestor(of: target, matching: find.byType(TextButton))
                  .first,
            ),
          )
          .toList();
      expect(rects[0].bottom, lessThanOrEqualTo(rects[1].top));
      expect(rects[1].bottom, lessThanOrEqualTo(rects[2].top));
      expect(rects.every((rect) => rect.height >= 48), isTrue);
      expect(rects.first.left, greaterThanOrEqualTo(20));
      expect(rects.last.right, lessThanOrEqualTo(300));
      expect(
        find.byKey(const ValueKey('console-color-targets')),
        findsOneWidget,
      );
      for (final target in targets) {
        final label = find
            .descendant(of: target, matching: find.byType(Text))
            .first;
        expect(MediaQuery.textScalerOf(tester.element(label)).scale(1), 2);
        expect(
          find.ancestor(of: label, matching: find.byType(FittedBox)),
          findsNothing,
        );
        await tester.ensureVisible(target);
        await tester.pumpAndSettle();
        expect(target.hitTestable(), findsOneWidget);
      }
    },
  );
  testWidgets(
    'console draft previews colors, undoes gestures and saves only Apply',
    (tester) async {
      SharedPreferences.setMockInitialValues({
        'console_foreground_rgb': 0xffabcdef,
        'console_background_rgb': 0xff112233,
        'console_timestamp_rgb': 0xff778899,
        'console_show_timestamps': true,
      });
      final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
      addTearDown(service.dispose);
      Widget screen() => MaterialApp(
        home: Scaffold(
          body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
        ),
      );
      await tester.pumpWidget(screen());
      await tester.pumpAndSettle();
      Future<void> open() async {
        await tester.tap(
          find.byKey(const ValueKey('console-appearance-button')),
        );
        await tester.pumpAndSettle();
      }

      Future<void> tapKey(String key) async {
        final label = find.byKey(ValueKey(key));
        final finder = key == 'console-timestamp-color'
            ? find.ancestor(of: label, matching: find.byType(TextButton)).first
            : label;
        await tester.ensureVisible(finder);
        await tester.pumpAndSettle();
        await tester.tap(finder);
        await tester.pumpAndSettle();
      }

      String hex() => tester
          .widget<TextField>(find.byKey(const ValueKey('console-color-hex')))
          .controller!
          .text;
      final preferences = await SharedPreferences.getInstance();
      await open();
      await tapKey('console-timestamp-color');
      expect(hex(), '#778899');
      final hue = find.byKey(const ValueKey('console-color-hue'));
      await tester.ensureVisible(hue);
      final rect = tester.getRect(hue);
      final gesture = await tester.startGesture(
        Offset(rect.left + 24, rect.center.dy),
      );
      await gesture.moveTo(
        Offset(rect.left + rect.width * .35, rect.center.dy),
      );
      await tester.pump(const Duration(milliseconds: 16));
      final firstHue = tester.widget<Slider>(hue).value;
      final firstHex = hex();
      service.updates.add(['01:49:01.000 [FW] asynchronous console update']);
      await tester.pump(const Duration(milliseconds: 100));
      expect(tester.widget<Slider>(hue).value, firstHue);
      expect(hex(), firstHex);
      expect(tester.getRect(hue), rect);
      await gesture.moveTo(Offset(rect.left + rect.width * .7, rect.center.dy));
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.widget<Slider>(hue).value, greaterThan(firstHue));
      expect(hex(), isNot(firstHex));
      expect(preferences.getInt('console_timestamp_rgb'), 0xff778899);
      await gesture.up();
      await tester.pumpAndSettle();
      await tapKey('console-appearance-undo');
      expect(hex(), '#778899', reason: 'Undo returns to before the whole drag');
      await tapKey('console-appearance-defaults');
      expect(hex(), '#9CAFAE');
      await tapKey('console-appearance-undo');
      expect(hex(), '#778899');
      await tapKey('console-appearance-defaults');
      await tapKey('console-appearance-cancel');
      expect(preferences.getInt('console_timestamp_rgb'), 0xff778899);
      await open();
      await tapKey('console-timestamp-color');
      expect(hex(), '#778899');
      final hexField = find.byKey(const ValueKey('console-color-hex'));
      await tester.ensureVisible(hexField);
      await tester.enterText(hexField, '#GG1234');
      await tester.pump();
      expect(
        tester
            .widget<FilledButton>(
              find.byKey(const ValueKey('console-appearance-apply')),
            )
            .onPressed,
        isNull,
      );
      await tester.enterText(hexField, '#123456');
      await tapKey('console-foreground-color');
      expect(hex(), '#ABCDEF');
      await tester.ensureVisible(hexField);
      await tester.enterText(hexField, '#fedcba');
      await tapKey('console-appearance-apply');
      expect(preferences.getInt('console_timestamp_rgb'), 0xff123456);
      expect(preferences.getInt('console_foreground_rgb'), 0xfffedcba);
      expect(preferences.getInt('console_background_rgb'), 0xff112233);
      await tester.pumpWidget(const SizedBox());
      await tester.pumpWidget(screen());
      await tester.pumpAndSettle();
      final line = tester.widget<Text>(
        find.byWidgetPredicate(
          (widget) =>
              widget is Text &&
              (widget.textSpan?.toPlainText().endsWith('ready') ?? false),
        ),
      );
      expect(line.style!.color, const Color(0xfffedcba));
      expect(
        (line.textSpan! as TextSpan).children!.first.style!.color,
        const Color(0xff123456),
      );
      await open();
      await tapKey('console-appearance-defaults');
      await tapKey('console-appearance-apply');
      expect(preferences.getInt('console_timestamp_rgb'), 0xff9cafae);
      expect(preferences.getInt('console_foreground_rgb'), 0xffd9f3f2);
      expect(preferences.getInt('console_background_rgb'), 0xff000000);
    },
  );
  for (final size in [const Size(390, 844), const Size(844, 390)]) {
    testWidgets('console picker remains usable at 200% text in $size', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues({});
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final landscape = size.width > size.height;
      final padding = landscape
          ? const EdgeInsets.fromLTRB(59, 0, 59, 21)
          : const EdgeInsets.fromLTRB(0, 44, 0, 34);
      final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
      addTearDown(service.dispose);
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              textScaler: const TextScaler.linear(2),
              padding: padding,
              viewPadding: padding,
            ),
            child: child!,
          ),
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
      await tester.pumpAndSettle();
      for (final key in [
        'console-timestamp-color',
        'console-color-hue',
        'console-color-saturation',
        'console-color-brightness',
        'console-appearance-defaults',
        'console-appearance-undo',
        'console-appearance-cancel',
        'console-appearance-apply',
      ]) {
        final label = find.byKey(ValueKey(key));
        final finder = key == 'console-timestamp-color'
            ? find.ancestor(of: label, matching: find.byType(TextButton)).first
            : label;
        await tester.ensureVisible(finder);
        await tester.pumpAndSettle();
        expect(finder.hitTestable(), findsOneWidget, reason: key);
        final rect = tester.getRect(finder);
        expect(rect.height, greaterThanOrEqualTo(48), reason: key);
        expect(rect.left, greaterThanOrEqualTo(padding.left), reason: key);
        expect(
          rect.right,
          lessThanOrEqualTo(size.width - padding.right),
          reason: key,
        );
        expect(
          rect.bottom,
          lessThanOrEqualTo(size.height - padding.bottom),
          reason: key,
        );
        expect(tester.takeException(), isNull, reason: key);
      }
      for (final key in [
        'console-color-saturation',
        'console-color-brightness',
      ]) {
        final slider = find.byKey(ValueKey(key));
        await tester.ensureVisible(slider);
        final rect = tester.getRect(slider);
        final gesture = await tester.startGesture(
          Offset(rect.left + rect.width * .2, rect.center.dy),
        );
        await gesture.moveTo(
          Offset(rect.left + rect.width * .4, rect.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final first = tester.widget<Slider>(slider).value;
        service.updates.add(['01:49:01.000 [FW] update']);
        await tester.pump(const Duration(milliseconds: 100));
        expect(tester.widget<Slider>(slider).value, first);
        await gesture.moveTo(
          Offset(rect.left + rect.width * .8, rect.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 16));
        expect(tester.widget<Slider>(slider).value, greaterThan(first));
        await gesture.up();
        await tester.pumpAndSettle();
      }
      final apply = find.byKey(const ValueKey('console-appearance-apply'));
      await tester.ensureVisible(apply);
      await tester.tap(apply);
      await tester.pumpAndSettle();
      final preferences = await SharedPreferences.getInstance();
      expect(preferences.getInt('console_foreground_rgb'), isNot(0xffd9f3f2));
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets(
    'system-back keyboard dismissal can reopen without losing command',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
      addTearDown(service.dispose);
      addTearDown(tester.view.resetViewInsets);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final state = tester.state<AppConsoleScreenState>(
        find.byType(AppConsoleScreen),
      );
      state.activateConsole();
      await tester.pump();
      final input = find.byKey(const ValueKey('console-command-input'));
      await tester.enterText(input, 'help');
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      await tester.pump();
      tester.testTextInput.hide();
      tester.view.viewInsets = FakeViewPadding.zero;
      await tester.pump();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      expect(find.byTooltip('Show keyboard'), findsOneWidget);
      await tester.tap(find.byTooltip('Show keyboard'));
      await tester.pump();
      if (!tester.testTextInput.isVisible) {
        expect(
          tester.binding.hasScheduledFrame,
          isTrue,
          reason: 'Async keyboard preparation must schedule its own repaint',
        );
        await tester.pump();
      }
      expect(tester.testTextInput.isVisible, isTrue);
      expect(tester.widget<TextField>(input).controller!.text, 'help');
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      await tester.pump();
      expect(find.byTooltip('Hide keyboard'), findsOneWidget);
    },
  );
  for (final scenario in [
    (const Locale('en', 'US'), false, '00:00:00.000', '12:00:00.000 AM'),
    (const Locale('en', 'US'), false, '12:00:00.000', '12:00:00.000 PM'),
    (const Locale('en', 'US'), false, '13:49:30.125', '1:49:30.125 PM'),
    (const Locale('en', 'US'), true, '13:49:30.125', '13:49:30.125'),
    (const Locale('en', 'GB'), false, '13:49:30.125', '13:49:30.125'),
  ]) {
    testWidgets(
      'console local timestamp ${scenario.$1} ${scenario.$2} ${scenario.$3}',
      (tester) async {
        SharedPreferences.setMockInitialValues({
          'console_show_timestamps': true,
        });
        final service = _TimestampConsoleService(['${scenario.$3} [FW] ready']);
        addTearDown(service.dispose);
        await tester.pumpWidget(
          MaterialApp(
            locale: scenario.$1,
            supportedLocales: AppLocalizations.supportedLocales,
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(
                context,
              ).copyWith(alwaysUse24HourFormat: scenario.$2),
              child: child!,
            ),
            home: Scaffold(
              body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final line = tester.widget<Text>(find.text('${scenario.$4} ready'));
        final spans = (line.textSpan! as TextSpan).children!.cast<TextSpan>();
        expect(spans.first.style!.color, isNot(line.style!.color));
        expect(spans.last.text, 'ready');
        expect(service.consoleLinesSnapshot, ['${scenario.$3} [FW] ready']);
      },
    );
  }
  testWidgets('fixed-width default resolves to Menlo on iOS', (tester) async {
    SharedPreferences.setMockInitialValues({});
    final service = _TimestampConsoleService(['01:49:00.000 [FW] ready']);
    addTearDown(service.dispose);
    await tester.pumpWidget(
      MaterialApp(
        theme: ThemeData(platform: TargetPlatform.iOS),
        home: Scaffold(
          body: AppConsoleScreen(bleService: service, boardId: 'board-1'),
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(
      tester
          .widget<TextField>(
            find.byKey(const ValueKey('console-command-input')),
          )
          .style!
          .fontFamily,
      'Menlo',
    );
    expect(
      tester.widget<Text>(find.textContaining('ready')).style!.fontFamily,
      'Menlo',
    );
  });
  Future<void> pumpConsole(
    WidgetTester tester,
    LitchBleService service, {
    TextScaler textScaler = TextScaler.noScaling,
    String boardId = 'board-1',
  }) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: MediaQuery(
            data: MediaQueryData(textScaler: textScaler),
            child: AppConsoleScreen(bleService: service, boardId: boardId),
          ),
        ),
      ),
    );
    await tester.pump();
  }

  testWidgets(
    'opening console leaves trace off and toggle controls session output',
    (WidgetTester tester) async {
      SharedPreferences.setMockInitialValues({});
      tester.view.physicalSize = const Size(320, 700);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetViewInsets);

      final _ConsoleBle fakeBle = _ConsoleBle();
      final LitchBleService service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() => disposeBoard(service, fakeBle));
      await openConsoleTestSession(tester, service, 'board-1');

      await pumpConsole(
        tester,
        service,
        textScaler: const TextScaler.linear(2),
      );

      final Finder toggle = find.byKey(
        const ValueKey<String>('console-trace-output-toggle'),
      );
      expect(toggle, findsNothing);
      final gear = find.byKey(const ValueKey('console-appearance-button'));
      expect(tester.getSize(gear), const Size(48, 48));
      expect(find.text("Type 'help'"), findsOneWidget);
      await tester.tap(gear);
      await tester.pumpAndSettle();
      expect(toggle, findsOneWidget);
      expect(tester.getSize(toggle).height, greaterThanOrEqualTo(48));
      expect(find.text('Trace output'), findsOneWidget);
      expect(find.text('Off for this session'), findsOneWidget);
      expect(fakeBle.commands, isEmpty);
      expect(tester.takeException(), isNull);

      await tester.tap(toggle);
      await tester.pump();
      expect(fakeBle.commands, <String>['debug trace\n']);

      service.debugAppendConsolePacket(
        utf8.encode('0.000 [TRACE] sensor chatter\n'),
      );
      await tester.pump();
      expect(
        service.consoleLinesSnapshot.any(
          (line) => line.contains('[TRACE] sensor chatter'),
        ),
        isTrue,
      );
      service.debugAppendConsolePacket(utf8.encode('0.001 [FW] ready\n'));
      await tester.pump();
      expect(find.text('0.001 [FW] ready'), findsOneWidget);
      expect(
        service.consoleLinesSnapshot.any((line) => line.contains('ready')),
        isTrue,
      );

      await tester.tap(toggle);
      await tester.pump();
      expect(fakeBle.commands, <String>['debug trace\n', 'debug info\n']);
      expect(find.textContaining('[TRACE] sensor chatter'), findsNothing);
      expect(find.text('Off for this session'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await service.disconnect();
    },
  );

  testWidgets('failed trace command stays off and explains recovery', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues({});
    final _ConsoleBle fakeBle = _ConsoleBle()..rejectCommands = true;
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );
    addTearDown(() => disposeBoard(service, fakeBle));
    await openConsoleTestSession(tester, service, 'board-1');
    await pumpConsole(tester, service);
    await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
    await tester.pumpAndSettle();

    await tester.tap(
      find.byKey(const ValueKey<String>('console-trace-output-toggle')),
    );
    await tester.pump();

    expect(service.consoleTraceOutputEnabled, isFalse);
    expect(find.byType(SnackBar), findsNothing);
    expect(find.text('Changes apply immediately.'), findsOneWidget);
    expect(
      find.text(
        'Couldn’t change trace output. Check the board connection and try again.',
      ),
      findsOneWidget,
    );
    await service.disconnect();
  });

  for (final retire in [false, true]) {
    testWidgets(
      'dismissed pending Trace failure ${retire ? "suppresses retired session" : "shows one current-session notice"}',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        final deferred = Completer<void>();
        final fakeBle = _ConsoleBle()
          ..rejectCommands = true
          ..deferredReceipt = deferred;
        final service = LitchBleService(
          pendingOtaStore: MemoryOtaTargetStore(),
          ble: fakeBle,
        );
        addTearDown(() => disposeBoard(service, fakeBle));
        await openConsoleTestSession(tester, service, 'board-1');
        await pumpConsole(tester, service);
        await tester.tap(
          find.byKey(const ValueKey('console-appearance-button')),
        );
        await tester.pumpAndSettle();
        expect(find.text('Changes apply immediately.'), findsOneWidget);
        await tester.tap(
          find.byKey(const ValueKey('console-trace-output-toggle')),
        );
        await tester.pump();
        expect(find.text('Turning trace output on…'), findsOneWidget);
        expect(find.text('Changes apply immediately.'), findsOneWidget);
        final cancel = find.byKey(const ValueKey('console-appearance-cancel'));
        await tester.ensureVisible(cancel);
        await tester.tap(cancel);
        await tester.pumpAndSettle();
        if (retire) {
          await openConsoleTestSession(tester, service, 'board-2');
          await pumpConsole(tester, service, boardId: 'board-2');
        }
        deferred.complete();
        await tester.pumpAndSettle();
        expect(find.byType(SnackBar), retire ? findsNothing : findsOneWidget);
        expect(
          find.text(
            'Couldn’t change trace output. Check the board connection and try again.',
          ),
          retire ? findsNothing : findsOneWidget,
        );
        expect(tester.takeException(), isNull);
        await service.disconnect();
      },
    );
  }

  testWidgets('deferred trace command announces pending state at 200 percent', (
    WidgetTester tester,
  ) async {
    SharedPreferences.setMockInitialValues({});
    tester.view.physicalSize = const Size(320, 700);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final _ConsoleBle fakeBle = _ConsoleBle()
      ..deferredReceipt = Completer<void>();
    final LitchBleService service = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: fakeBle,
    );
    addTearDown(() => disposeBoard(service, fakeBle));
    await openConsoleTestSession(tester, service, 'board-1');
    await pumpConsole(tester, service, textScaler: const TextScaler.linear(2));
    await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
    await tester.pumpAndSettle();
    final Finder toggle = find.byKey(
      const ValueKey<String>('console-trace-output-toggle'),
    );

    await tester.tap(toggle);
    await tester.pump();

    expect(find.text('Turning trace output on…'), findsOneWidget);

    expect(tester.widget<SwitchListTile>(toggle).onChanged, isNull);
    expect(tester.takeException(), isNull);

    final oldWrite = fakeBle.deferredReceipt!;
    await openConsoleTestSession(tester, service, 'board-2');
    await pumpConsole(
      tester,
      service,
      boardId: 'board-2',
      textScaler: const TextScaler.linear(2),
    );
    fakeBle.deferredReceipt = Completer<void>();
    if (toggle.evaluate().isEmpty) {
      await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
      await tester.pumpAndSettle();
    }
    await tester.tap(toggle);
    await tester.pump();
    expect(fakeBle.commands, ['debug trace\n', 'debug trace\n']);
    oldWrite.complete();
    await tester.pump();
    expect(find.text('Turning trace output on…'), findsOneWidget);
    expect(tester.widget<SwitchListTile>(toggle).onChanged, isNull);
    expect(
      find.text(
        'Couldn’t change trace output. Check the board connection and try again.',
      ),
      findsNothing,
    );
    fakeBle.deferredReceipt!.complete();
    await tester.pump();
    expect(service.consoleTraceOutputEnabled, isTrue);
    await tester.pump();

    expect(find.text('On for this session'), findsOneWidget);

    expect(tester.widget<SwitchListTile>(toggle).onChanged, isNotNull);

    // The real service retires an outstanding write on disconnect. A new
    // connection to the same board must not inherit that pending UI owner.
    fakeBle.deferredReceipt = Completer<void>();
    if (toggle.evaluate().isEmpty) {
      await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
      await tester.pumpAndSettle();
    }
    await tester.tap(toggle);
    await tester.pump();
    final retiredWrite = fakeBle.deferredReceipt!;
    await openConsoleTestSession(tester, service, 'board-2');
    await pumpConsole(tester, service, boardId: 'board-2');
    await tester.pump();
    expect(tester.widget<SwitchListTile>(toggle).onChanged, isNotNull);
    expect(
      find.text(
        'Couldn’t change trace output. Check the board connection and try again.',
      ),
      findsNothing,
    );
    retiredWrite.complete();
    await tester.pump();
    expect(tester.widget<SwitchListTile>(toggle).onChanged, isNotNull);
    expect(tester.takeException(), isNull);
    await service.disconnect();
  });

  testWidgets(
    'terminal appearance and keyboard choice persist for the session',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      tester.view.physicalSize = const Size(844, 390);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetViewInsets);
      final fakeBle = _ConsoleBle();
      final service = LitchBleService(
        pendingOtaStore: MemoryOtaTargetStore(),
        ble: fakeBle,
      );
      addTearDown(() => disposeBoard(service, fakeBle));
      await openConsoleTestSession(tester, service, 'board-1');
      await pumpConsole(
        tester,
        service,
        textScaler: const TextScaler.linear(2),
      );

      final state = tester.state<AppConsoleScreenState>(
        find.byType(AppConsoleScreen),
      );
      final input = find.byType(TextField).first;
      state.activateConsole();
      await tester.pumpAndSettle();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      await tester.pump();

      await tester.tap(find.byKey(const ValueKey('console-hide-keyboard')));
      await tester.pump();
      tester.view.viewInsets = FakeViewPadding.zero;
      await tester.pump();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isFalse);
      expect(find.byTooltip('Show keyboard'), findsOneWidget);
      await tester.tap(find.byTooltip('Show keyboard'));
      await tester.pump();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      await tester.pump();
      expect(find.byTooltip('Hide keyboard'), findsOneWidget);
      await tester.tap(find.byTooltip('Hide keyboard'));
      await tester.pump();
      tester.view.viewInsets = FakeViewPadding.zero;
      await tester.pump();
      state.deactivateConsole();
      state.activateConsole();
      await tester.pump();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isFalse);

      await tester.tap(input);
      await tester.pump();
      state.deactivateConsole();
      state.activateConsole();
      await tester.pump();
      expect(tester.widget<TextField>(input).focusNode!.hasFocus, isTrue);

      await tester.tap(find.byKey(const ValueKey('console-appearance-button')));
      await tester.pumpAndSettle();
      expect(find.text('Console appearance'), findsOneWidget);
      expect(
        find.byKey(const ValueKey('console-foreground-color')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('console-background-color')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('console-font-monospace')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('console-appearance-defaults')),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('console-background-color')));
      await tester.ensureVisible(
        find.byKey(const ValueKey('console-color-hex')),
      );
      await tester.tap(find.byKey(const ValueKey('console-color-hex')));
      tester.view.viewInsets = const FakeViewPadding(bottom: 200);
      await tester.pumpAndSettle();
      final apply = find.byKey(const ValueKey('console-appearance-apply'));
      await tester.ensureVisible(apply);
      await tester.pumpAndSettle();
      expect(
        find.byKey(const ValueKey('console-appearance-scroll')),
        findsOneWidget,
      );
      expect(apply.hitTestable(), findsOneWidget);
      expect(tester.takeException(), isNull);
      await service.disconnect();
    },
  );
}
