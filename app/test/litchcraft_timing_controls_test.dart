import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/widgets/litchcraft_motion_editor.dart';

void main() {
  testWidgets(
    'retained frame timing displays and speaks exact document values',
    (tester) async {
      final semantics = tester.ensureSemantics();
      final rig = _TimingRig(_TimingKind.frame);
      await rig.mount(tester);

      for (final durationMs in [50, 700, 701, 1250, 5000]) {
        rig.update(() => rig.document = _document([700, durationMs]));
        await tester.pump();
        final bytes = encodeLitchcraftAnimationDocument(rig.document).bytes;
        final expectedValue = '${litchcraftDurationSeconds(durationMs)} s';
        expect(tester.widget<Slider>(rig.slider).value, 5050 - durationMs);
        expect(
          find.descendant(
            of: rig.valueButton,
            matching: find.text(expectedValue),
          ),
          findsOneWidget,
        );
        expect(
          tester.getSemantics(rig.slider).getSemanticsData().value,
          'Frame duration: $expectedValue',
        );
        expect(
          tester.getSemantics(rig.valueButton).getSemanticsData().label,
          'Frame duration: $expectedValue',
        );
        expect(
          tester.getSize(rig.valueButton).height,
          greaterThanOrEqualTo(48),
        );

        await tester.tap(rig.valueButton);
        await tester.pumpAndSettle();
        expect(
          tester.widget<TextFormField>(_exactField).controller!.text,
          '$durationMs',
        );
        await tester.tap(find.text('Cancel'));
        await tester.pumpAndSettle();
        expect(encodeLitchcraftAnimationDocument(rig.document).bytes, bytes);
        expect(rig.commits, isEmpty);
        expect(rig.previews, isEmpty);
      }
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      semantics.dispose();
    },
  );

  for (final kind in _TimingKind.values) {
    testWidgets('$kind semantic adjustment captures and commits exactly once', (
      tester,
    ) async {
      final semantics = tester.ensureSemantics();
      final rig = _TimingRig(kind);
      await rig.mount(tester);
      final node = tester.getSemantics(rig.slider);
      expect(
        node.getSemanticsData().hasAction(ui.SemanticsAction.increase),
        isTrue,
      );
      node.owner!.performAction(node.id, ui.SemanticsAction.increase);
      await tester.pump();
      expect(rig.starts, 1);
      expect(rig.commitAttempts, 1);
      expect(rig.commits, hasLength(1));
      expect(
        rig.commits.single.durationMs,
        lessThan(kind == _TimingKind.frame ? 1250 : 2000),
      );
      await tester.pumpWidget(const SizedBox.shrink());
      semantics.dispose();
    });
    testWidgets(
      '$kind drag previews each change, survives stale rebuilds and commits once',
      (tester) async {
        final rig = _TimingRig(kind);
        await rig.mount(tester);
        final originalBytes = encodeLitchcraftAnimationDocument(
          rig.document,
        ).bytes;
        expect(
          tester.getCenter(find.text('Slower')).dx,
          lessThan(tester.getCenter(find.text('Faster')).dx),
        );
        final rect = tester.getRect(rig.slider);
        final gesture = await tester.startGesture(
          Offset(rect.left + 24, rect.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 40));
        final firstDuration = rig.previews.last;
        expect(
          tester.getRect(rig.slider),
          rect,
          reason: 'The exact value label must not resize the held track.',
        );
        expect(rig.starts, 1);
        expect(rig.commits, isEmpty);
        expect(tester.widget<TextButton>(rig.valueButton).onPressed, isNull);

        await gesture.moveTo(
          rect.center,
          timeStamp: const Duration(milliseconds: 80),
        );
        await tester.pump(const Duration(milliseconds: 40));
        final middleDuration = rig.previews.last;
        expect(tester.getRect(rig.slider), rect);
        final heldSliderValue = tester.widget<Slider>(rig.slider).value;
        expect(middleDuration, lessThan(firstDuration));
        expect(find.text('Preview $middleDuration ms'), findsOneWidget);
        expect(rig.commits, isEmpty);
        expect(
          encodeLitchcraftAnimationDocument(rig.document).bytes,
          originalBytes,
        );

        // Model readback and playback selection can rebuild the production
        // panel while the same real pointer still owns its timing preview.
        rig.update(() {
          rig.selectedFrame = 0;
          if (kind == _TimingKind.effect) rig.effectDurationMs = 9500;
        });
        await tester.pump(const Duration(milliseconds: 50));
        expect(tester.widget<Slider>(rig.slider).value, heldSliderValue);
        expect(
          tester.getRect(rig.slider),
          rect,
          reason:
              'Asynchronous readback must preserve the held track geometry.',
        );
        expect(find.text('Preview $middleDuration ms'), findsOneWidget);
        expect(
          find.descendant(
            of: rig.valueButton,
            matching: find.text(
              '${litchcraftDurationSeconds(middleDuration)} s',
            ),
          ),
          findsOneWidget,
        );
        expect(rig.commits, isEmpty);

        final previewCount = rig.previews.length;
        await gesture.moveTo(
          rect.center,
          timeStamp: const Duration(milliseconds: 120),
        );
        await tester.pump(const Duration(milliseconds: 40));
        expect(rig.previews.length, previewCount);

        await gesture.moveTo(
          Offset(rect.right - 24, rect.center.dy),
          timeStamp: const Duration(milliseconds: 160),
        );
        await tester.pump(const Duration(milliseconds: 40));
        final finalDuration = rig.previews.last;
        expect(tester.getRect(rig.slider), rect);
        expect(finalDuration, lessThan(middleDuration));
        expect(find.text('Preview $finalDuration ms'), findsOneWidget);
        expect(rig.commits, isEmpty);
        expect(
          encodeLitchcraftAnimationDocument(rig.document).bytes,
          originalBytes,
        );
        if (kind == _TimingKind.effect) expect(rig.effectDurationMs, 9500);

        await gesture.up();
        await tester.pump();
        expect(rig.commits, [(frame: 1, durationMs: finalDuration)]);
        expect(rig.starts, 1);
        expect(tester.widget<TextButton>(rig.valueButton).onPressed, isNotNull);
        if (kind == _TimingKind.frame) {
          expect(rig.document.frames[0].durationMs, 700);
          expect(rig.document.frames[1].durationMs, finalDuration);
        } else {
          expect(rig.effectDurationMs, finalDuration);
          expect(finalDuration % 100, 0);
        }
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );

    testWidgets('$kind authority loss retires a held edit before reconnect', (
      tester,
    ) async {
      final rig = _TimingRig(kind);
      await rig.mount(tester);
      final originalBytes = encodeLitchcraftAnimationDocument(
        rig.document,
      ).bytes;
      final rect = tester.getRect(rig.slider);
      final gesture = await tester.startGesture(
        Offset(rect.left + 24, rect.center.dy),
      );
      await tester.pump();
      await gesture.moveTo(rect.center);
      await tester.pump(const Duration(milliseconds: 50));
      expect(rig.previews, isNotEmpty);
      rig.update(() => rig.enabled = false);
      await tester.pump();
      expect(tester.widget<Slider>(rig.slider).onChanged, isNull);
      rig.update(() {
        rig.enabled = true;
        rig.selectedFrame = 0;
        rig.epoch++;
      });
      await tester.pump();
      await gesture.moveTo(Offset(rect.right - 24, rect.center.dy));
      await tester.pump(const Duration(milliseconds: 50));
      await gesture.up();
      await tester.pump();
      expect(rig.commitAttempts, 0);
      expect(rig.commits, isEmpty);
      expect(
        encodeLitchcraftAnimationDocument(rig.document).bytes,
        originalBytes,
      );
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('$kind exact entry rejects invalid text, bounds and step', (
      tester,
    ) async {
      final rig = _TimingRig(kind);
      await rig.mount(tester);
      final originalBytes = encodeLitchcraftAnimationDocument(
        rig.document,
      ).bytes;
      final originalEffectDuration = rig.effectDurationMs;
      await tester.tap(rig.valueButton);
      await tester.pumpAndSettle();
      final invalidValues = kind == _TimingKind.frame
          ? ['abc', '', '49', '5001']
          : ['abc', '', '499', '10001', '701'];
      final invalidMessage = kind == _TimingKind.frame
          ? 'Enter a value from 50 to 5000 ms.'
          : 'Enter 500 to 10000 ms in 100 ms steps.';
      for (final raw in invalidValues) {
        await tester.enterText(_exactField, raw);
        await tester.tap(_setExact);
        await tester.pump();
        expect(_exactField, findsOneWidget);
        expect(find.text(invalidMessage), findsOneWidget);
        expect(rig.commitAttempts, 0);
        expect(
          encodeLitchcraftAnimationDocument(rig.document).bytes,
          originalBytes,
        );
        expect(rig.effectDurationMs, originalEffectDuration);
      }
      await tester.tap(find.text('Cancel'));
      await tester.pumpAndSettle();
      expect(rig.commits, isEmpty);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets(
      '$kind exact entry accepts boundaries without slider rounding',
      (tester) async {
        final rig = _TimingRig(kind);
        await rig.mount(tester);
        final acceptedValues = kind == _TimingKind.frame
            ? [50, 701, 5000]
            : [500, 700, 10000];
        for (final durationMs in acceptedValues) {
          await tester.tap(rig.valueButton);
          await tester.pumpAndSettle();
          await tester.enterText(_exactField, '$durationMs');
          await tester.tap(_setExact);
          await tester.pumpAndSettle();
          expect(_exactField, findsNothing);
          expect(rig.commits.last.durationMs, durationMs);
          expect(
            find.descendant(
              of: rig.valueButton,
              matching: find.text('${litchcraftDurationSeconds(durationMs)} s'),
            ),
            findsOneWidget,
          );
        }
        expect(rig.commits, hasLength(acceptedValues.length));
        expect(rig.previews, isEmpty);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets('exact frame timing captures its frame before dialog await', (
    tester,
  ) async {
    final rig = _TimingRig(_TimingKind.frame);
    await rig.mount(tester);
    await tester.tap(rig.valueButton);
    await tester.pumpAndSettle();
    expect(rig.starts, 1);
    rig.update(() => rig.selectedFrame = 0);
    await tester.pump();
    await tester.enterText(_exactField, '701');
    await tester.tap(_setExact);
    await tester.pumpAndSettle();
    expect(rig.commits, [(frame: 1, durationMs: 701)]);
    expect(rig.document.frames[0].durationMs, 700);
    expect(rig.document.frames[1].durationMs, 701);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('exact frame timing cannot replace a stale target on reconnect', (
    tester,
  ) async {
    final rig = _TimingRig(_TimingKind.frame);
    await rig.mount(tester);
    final originalBytes = encodeLitchcraftAnimationDocument(rig.document).bytes;
    await tester.tap(rig.valueButton);
    await tester.pumpAndSettle();
    rig.update(() {
      rig.selectedFrame = 0;
      rig.epoch++;
    });
    await tester.pump();
    await tester.enterText(_exactField, '701');
    await tester.tap(_setExact);
    await tester.pumpAndSettle();
    expect(rig.starts, 1);
    expect(rig.commitAttempts, 1);
    expect(rig.commits, isEmpty);
    expect(
      encodeLitchcraftAnimationDocument(rig.document).bytes,
      originalBytes,
    );
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });
}

final _exactField = find.byKey(const ValueKey('craft-exact-duration-field'));
final _setExact = find.byKey(const ValueKey('craft-set-exact-duration'));

enum _TimingKind { frame, effect }

LitchcraftAnimationDocument _document(List<int> durations) =>
    LitchcraftAnimationDocument(
      topology: LitchcraftTopology.v3,
      favoriteColors: litchcraftDefaultFavoriteColors,
      frames: [
        for (final durationMs in durations)
          LitchcraftAnimationFrame(
            durationMs: durationMs,
            emitters: List.generate(
              litchcraftAnimationEmitterCount,
              (_) => const LitchcraftAnimatedEmitter(color: 0x123456),
            ),
          ),
      ],
    );

// The real production panels, slider gestures and exact dialog are exercised.
// This local callback owner exposes preview/commit boundaries and captured
// targets; controller ACK and generation fences belong to the screen/session
// tests rather than this hardware-free presentation fixture.
class _TimingRig {
  _TimingRig(this.kind);

  final _TimingKind kind;
  LitchcraftAnimationDocument document = _document([700, 1250]);
  int selectedFrame = 1;
  int effectDurationMs = 2000;
  bool enabled = true;
  int epoch = 1;
  int starts = 0;
  int commitAttempts = 0;
  int? previewMs;
  int? _capturedFrame;
  int? _capturedEpoch;
  final List<int> previews = [];
  final List<({int frame, int durationMs})> commits = [];
  late StateSetter update;

  Finder get slider => find.byKey(ValueKey('craft-${kind.name}-duration'));
  Finder get valueButton =>
      find.byKey(ValueKey('craft-${kind.name}-duration-context'));

  void _start() {
    starts++;
    _capturedFrame = selectedFrame;
    _capturedEpoch = epoch;
  }

  void _preview(int durationMs) => update(() {
    previews.add(durationMs);
    previewMs = durationMs;
  });

  void _commit(int durationMs) {
    commitAttempts++;
    final targetFrame = _capturedFrame;
    if (!enabled || _capturedEpoch != epoch || targetFrame == null) return;
    update(() {
      commits.add((frame: targetFrame, durationMs: durationMs));
      if (kind == _TimingKind.frame) {
        document = LitchcraftAnimationDocument(
          topology: document.topology,
          favoriteColors: document.favoriteColors,
          frames: [
            for (var frame = 0; frame < document.frames.length; frame++)
              if (frame == targetFrame)
                LitchcraftAnimationFrame(
                  durationMs: durationMs,
                  emitters: document.frames[frame].emitters,
                )
              else
                document.frames[frame],
          ],
        );
      } else {
        effectDurationMs = durationMs;
      }
    });
  }

  Future<void> mount(WidgetTester tester) async {
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        theme: ThemeData.dark(),
        home: Scaffold(
          body: Center(
            child: StatefulBuilder(
              builder: (context, setState) {
                update = setState;
                return MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(disableAnimations: true),
                  child: SizedBox(
                    width: 380,
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        if (kind == _TimingKind.frame)
                          LitchcraftFrameEditorPanel(
                            document: document,
                            selectedFrameIndex: selectedFrame,
                            enabled: enabled,
                            onDurationChangeStarted: _start,
                            onDurationPreview: _preview,
                            onDurationChanged: _commit,
                            onDuplicate: () {},
                            onDelete: () {},
                            onOpenEffects: () {},
                          )
                        else
                          LitchcraftEffectEditorPanel(
                            selectedEffect: LitchcraftEffectStyle.breathe,
                            effectDurationMs: effectDurationMs,
                            enabled: enabled,
                            onEffectChanged: (_) {},
                            onDurationChangeStarted: _start,
                            onDurationPreview: _preview,
                            onDurationChanged: _commit,
                          ),
                        Text('Preview ${previewMs ?? 'none'} ms'),
                      ],
                    ),
                  ),
                );
              },
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
  }
}
