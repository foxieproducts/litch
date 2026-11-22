import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/localization/litch_locale.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/theme/litch_appearance.dart';
import 'package:litch_mobile/src/widgets/app_preferences.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';

void main() {
  final events = <String>[];
  setUp(() {
    events.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(SystemChannels.platform, (call) async {
          if (call.method == 'HapticFeedback.vibrate') {
            events.add(call.arguments as String);
          }
          return null;
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(SystemChannels.platform, null);
  });

  testWidgets('held action recognizes once and confirms after dispatch', (
    tester,
  ) async {
    late StateSetter rebuild;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Center(
            child: StatefulBuilder(
              builder: (_, setState) {
                rebuild = setState;
                return LitchcraftHoldAction(
                  holdLabel: 'Hold to clear',
                  completedLabel: '',
                  showCompletionSnackBar: false,
                  onTap: () => events.add('tap'),
                  onHold: () => events.add('hold'),
                  child: const SizedBox.square(dimension: 48),
                );
              },
            ),
          ),
        ),
      ),
    );
    final pointer = await tester.startGesture(
      tester.getCenter(find.byType(LitchcraftHoldAction)),
    );
    await tester.pump(const Duration(milliseconds: 99));
    expect(events, isEmpty);
    await tester.pump(const Duration(milliseconds: 1));
    expect(events, ['HapticFeedbackType.lightImpact']);
    await pointer.moveBy(const Offset(2, 1));
    await tester.pump(const Duration(milliseconds: 250));
    rebuild(() {});
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 399));
    expect(events, ['HapticFeedbackType.lightImpact']);
    await tester.pump(const Duration(milliseconds: 1));
    expect(events, [
      'HapticFeedbackType.lightImpact',
      'hold',
      'HapticFeedbackType.mediumImpact',
    ]);
    await pointer.up();
    await tester.pump(const Duration(seconds: 1));
    expect(events, hasLength(3));
    expect(find.byKey(const ValueKey('craft-hold-progress')), findsNothing);
  });

  testWidgets(
    'tap, cancelled hold, drag and disable never confirm completion',
    (tester) async {
      var enabled = true;
      late StateSetter rebuild;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Center(
              child: StatefulBuilder(
                builder: (_, setState) {
                  rebuild = setState;
                  return LitchcraftHoldAction(
                    holdLabel: 'Hold to clear',
                    completedLabel: '',
                    showCompletionSnackBar: false,
                    onTap: enabled ? () => events.add('tap') : null,
                    onHold: enabled ? () => events.add('hold') : null,
                    child: const SizedBox.square(dimension: 48),
                  );
                },
              ),
            ),
          ),
        ),
      );
      final center = tester.getCenter(find.byType(LitchcraftHoldAction));
      final tap = await tester.startGesture(center);
      await tester.pump(const Duration(milliseconds: 50));
      await tap.up();
      await tester.pump(const Duration(seconds: 1));
      expect(events, ['tap']);
      events.clear();

      final cancelled = await tester.startGesture(center);
      await tester.pump(const Duration(milliseconds: 100));
      await cancelled.cancel();
      await tester.pump(const Duration(seconds: 1));
      expect(events, ['HapticFeedbackType.lightImpact']);
      events.clear();

      final drag = await tester.startGesture(center);
      await tester.pump(const Duration(milliseconds: 50));
      await drag.moveBy(const Offset(20, 0));
      await tester.pump(const Duration(milliseconds: 200));
      await drag.moveBy(const Offset(40, 0));
      await drag.up();
      await tester.pump(const Duration(seconds: 1));
      expect(events, isEmpty);

      final disabled = await tester.startGesture(center);
      await tester.pump(const Duration(milliseconds: 100));
      rebuild(() => enabled = false);
      await tester.pump();
      rebuild(() => enabled = true);
      await tester.pump(const Duration(seconds: 1));
      await disabled.up();
      await tester.pump();
      expect(events, ['HapticFeedbackType.lightImpact']);
      expect(find.byKey(const ValueKey('craft-hold-progress')), findsNothing);
      events.clear();
      final removed = await tester.startGesture(center);
      await tester.pump(const Duration(milliseconds: 50));
      await tester.pumpWidget(const SizedBox());
      await tester.pump(const Duration(seconds: 1));
      await removed.cancel();
      expect(events, isEmpty);
    },
  );

  testWidgets('palette honors 500ms hold and accessible action confirms once', (
    tester,
  ) async {
    final semantics = tester.ensureSemantics();
    await tester.pumpWidget(
      MaterialApp(
        home: AppPreferencesScope(
          appearanceMode: LitchAppearanceMode.system,
          onAppearanceModeChanged: (_) async {},
          localePreference: const LitchLocalePreference.system(),
          onLocalePreferenceChanged: (_) async {},
          child: Scaffold(
            body: Center(
              child: SizedBox(
                width: 48,
                height: 384,
                child: LitchcraftPaletteRail(
                  axis: Axis.vertical,
                  color: 0xff0000,
                  enabled: true,
                  onSelected: (_) => events.add('tap'),
                  onLongPress: (_) => events.add('fill'),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    final swatch = find.byKey(const ValueKey('craft-wheel-color-0'));
    final pointer = await tester.startGesture(tester.getCenter(swatch));
    await tester.pump(const Duration(milliseconds: 100));
    expect(events, ['HapticFeedbackType.lightImpact']);
    await tester.pump(const Duration(milliseconds: 399));
    expect(events, hasLength(1));
    await tester.pump(const Duration(milliseconds: 1));
    expect(events, [
      'HapticFeedbackType.lightImpact',
      'fill',
      'HapticFeedbackType.mediumImpact',
    ]);
    await pointer.up();
    await tester.pump();
    events.clear();
    final node = tester.getSemantics(swatch);
    node.owner!.performAction(node.id, ui.SemanticsAction.longPress);
    await tester.pump();
    expect(events, ['fill', 'HapticFeedbackType.mediumImpact']);
    semantics.dispose();
  });

  testWidgets('a rejected hold callback cannot emit completion feedback', (
    tester,
  ) async {
    final failure = StateError('Action could not be applied');
    await tester.pumpWidget(
      MaterialApp(
        home: Center(
          child: LitchcraftHoldAction(
            holdLabel: 'Hold to apply',
            completedLabel: '',
            showCompletionSnackBar: false,
            onHold: () => throw failure,
            child: const SizedBox.square(dimension: 48),
          ),
        ),
      ),
    );
    final pointer = await tester.startGesture(
      tester.getCenter(find.byType(LitchcraftHoldAction)),
    );
    await tester.pump(const Duration(milliseconds: 100));
    await tester.pump(const Duration(milliseconds: 650));
    expect(tester.takeException(), same(failure));
    await pointer.up();
    await tester.pump();
    expect(events, ['HapticFeedbackType.lightImpact']);
  });

  testWidgets('hold action is keyboard reachable with visible focus', (
    tester,
  ) async {
    var taps = 0;
    var holds = 0;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Center(
            child: LitchcraftHoldAction(
              key: const ValueKey<String>('swatch'),
              holdLabel: 'Hold to edit Teal',
              completedLabel: 'Editing Teal',
              onTap: () => taps += 1,
              onHold: () => holds += 1,
              child: const SizedBox.square(dimension: 48),
            ),
          ),
        ),
      ),
    );
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pumpAndSettle();

    final focusDecoration =
        tester
                .widgetList<AnimatedContainer>(
                  find.descendant(
                    of: find.byKey(const ValueKey<String>('swatch')),
                    matching: find.byType(AnimatedContainer),
                  ),
                )
                .single
                .decoration
            as BoxDecoration;
    expect(focusDecoration.border, isNotNull);

    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(taps, 1);
    expect(holds, 0);
    expect(events, isEmpty);

    final rect = tester.getRect(find.byKey(const ValueKey<String>('swatch')));
    expect(rect.width, greaterThanOrEqualTo(48));
    expect(rect.height, greaterThanOrEqualTo(48));
  });

  testWidgets('frame hold wrapper keeps one working keyboard stop per frame', (
    tester,
  ) async {
    final session = LitchcraftDocumentSession(
      staticLitchcraftDocument(LitchcraftCanvas.blank()),
    )..addFrame();
    addTearDown(session.dispose);
    final selected = <int>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 320,
              child: LitchcraftSwipeFrameStrip(
                document: session.document,
                selectedFrameIndex: 0,
                playing: false,
                enabled: true,
                frameLabel: (frame, count) => 'Frame $frame of $count',
                onSelectFrame: selected.add,
                onFrameLongPress: (_, _) => events.add('frame actions'),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(selected, [0]);
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(selected, [0, 1]);
    expect(events, isEmpty);
  });
}
