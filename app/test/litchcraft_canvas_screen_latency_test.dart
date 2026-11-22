import 'support/memory_ota_target_store.dart';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'fixtures/litchcraft_v2_functional_peer.dart';

// Only the absent native radio is bypassed; screen, renderer, document, brush,
// stroke/history ownership and library use their production implementations.
class _NoopBle implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

const _captureKey = ValueKey('canvas-latency-capture');

class _ScreenFixture {
  _ScreenFixture(this.ble, this.controller, this.library);
  final LitchBleService ble;
  final LitchcraftController controller;
  final LitchcraftLibrary library;
  int now = 0;

  static Future<_ScreenFixture> mount(
    WidgetTester tester, {
    LitchcraftAnimationDocument? document,
    int mask = 1,
  }) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    final ble = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: _NoopBle(),
    );
    final controller = LitchcraftController(
      ble,
      documentSession: document == null
          ? null
          : LitchcraftDocumentSession(document),
    );
    if (document == null) controller.restoreDraft(LitchcraftCanvas.blank());
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await controller.setBrush(color: 0xff4020, targetMask: mask);
    final result = _ScreenFixture(ble, controller, library);
    await tester.pumpWidget(
      RepaintBoundary(
        key: _captureKey,
        child: MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              monotonicMilliseconds: () => result.now,
            ),
          ),
        ),
      ),
    );
    // Live effects intentionally do not settle. Initial layout gets a bounded
    // pump, just as it does while a finger remains on a moving artwork.
    await tester.pump(const Duration(milliseconds: 100));
    return result;
  }

  LitchcraftCanvas get rendered =>
      (find
                  .descendant(
                    of: find.byKey(const ValueKey('litchcraft-canvas')),
                    matching: find.byType(LitchcraftThumbnail),
                  )
                  .evaluate()
                  .single
                  .widget
              as LitchcraftThumbnail)
          .canvas;

  LitchcraftCanvas get underlying =>
      (find.byType(LitchcraftCanvasView).evaluate().single.widget
              as LitchcraftCanvasView)
          .canvas;

  Future<void> dispose(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    library.dispose();
    controller.dispose();
    await ble.dispose();
  }
}

LitchcraftAnimationDocument _document({
  LitchcraftEffectStyle effect = LitchcraftEffectStyle.off,
  bool multipleFrames = false,
}) {
  LitchcraftAnimationFrame frame(int color) => LitchcraftAnimationFrame(
    durationMs: 1000,
    emitters: List.generate(
      256,
      (index) => LitchcraftAnimatedEmitter(
        color: index ~/ 4 == 9 ? color : 0,
        effect: index ~/ 4 == 9 ? effect : LitchcraftEffectStyle.off,
        effectDurationMs: index ~/ 4 == 9 && effect != LitchcraftEffectStyle.off
            ? 1000
            : 0,
      ),
    ),
  );
  return LitchcraftAnimationDocument(
    topology: LitchcraftTopology.v3,
    favoriteColors: litchcraftDefaultFavoriteColors,
    sparkleSeed: 17,
    frames: [frame(0x102030), if (multipleFrames) frame(0x405060)],
  );
}

Future<int> _pixel(WidgetTester tester, Offset global) async {
  late int rgb;
  await tester.runAsync(() async {
    final boundary = tester.renderObject<RenderRepaintBoundary>(
      find.byKey(_captureKey),
    );
    final image = await boundary.toImage(pixelRatio: 1);
    final bytes = (await image.toByteData(format: ui.ImageByteFormat.rawRgba))!;
    final local = boundary.globalToLocal(global);
    final offset = (local.dy.floor() * image.width + local.dx.floor()) * 4;
    rgb =
        bytes.getUint8(offset) << 16 |
        bytes.getUint8(offset + 1) << 8 |
        bytes.getUint8(offset + 2);
    image.dispose();
  });
  return rgb;
}

void main() {
  testWidgets(
    'primary down immediately paints the existing screen renderer without document/history mutation',
    (tester) async {
      final fixture = await _ScreenFixture.mount(tester);
      try {
        final rect = tester.getRect(
          find.byKey(const ValueKey('craft-square-9')),
        );
        final before = fixture.controller.documentSession.document;
        final pointer = await tester.startGesture(rect.center);
        await tester.pump(const Duration(milliseconds: 16));
        expect(
          await _pixel(
            tester,
            rect.topLeft + Offset(rect.width * .15, rect.height * .5),
          ),
          litchcraftPreviewColor(0xff4020).toARGB32() & 0xffffff,
          reason:
              'The first display frame after down must show the brush through the actual painter, before release/slop.',
        );
        expect(fixture.rendered.colors[9 * 4], 0xff4020);
        expect(fixture.rendered.colors[9 * 4 + 1], 0);
        expect(
          identical(fixture.controller.documentSession.document, before),
          isTrue,
        );
        expect(fixture.controller.canUndo, isFalse);
        await pointer.cancel();
        await tester.pump();
        expect(fixture.rendered.colors, fixture.underlying.colors);
        expect(fixture.controller.canUndo, isFalse);
      } finally {
        await fixture.dispose(tester);
      }
    },
  );

  testWidgets(
    'real screen accepts four pixel cell crossing while held and keeps one drag Undo',
    (tester) async {
      final fixture = await _ScreenFixture.mount(tester);
      try {
        final rect = tester.getRect(
          find.byKey(const ValueKey('craft-square-9')),
        );
        final pointer = await tester.startGesture(
          rect.topRight + Offset(-2, rect.height / 2),
        );
        await tester.pump(const Duration(milliseconds: 16));
        await pointer.moveBy(const Offset(4, 0));
        await tester.pump(const Duration(milliseconds: 16));
        expect(fixture.controller.canvas.colors[9 * 4], 0xff4020);
        expect(fixture.controller.canvas.colors[10 * 4], 0xff4020);
        // Sparse subsequent movement still paints each crossed cell, with real
        // asynchronous screen rebuilds between the pointer samples.
        await pointer.moveBy(Offset(rect.width * 3, 0));
        await tester.pump(const Duration(milliseconds: 16));
        for (var square = 9; square <= 13; square++) {
          expect(fixture.controller.canvas.colors[square * 4], 0xff4020);
        }
        await pointer.up();
        await tester.pump();
        expect(fixture.controller.canUndo, isTrue);
        await fixture.controller.undo();
        await tester.pump();
        expect(fixture.controller.canvas.colors, everyElement(0));
        expect(
          fixture.controller.canUndo,
          isFalse,
          reason: 'A drag creates one existing history entry.',
        );
      } finally {
        await fixture.dispose(tester);
      }
    },
  );

  for (final effect in [
    LitchcraftEffectStyle.breathe,
    LitchcraftEffectStyle.sparkle,
  ]) {
    testWidgets(
      'pending preview uses existing $effect composer through harmless live effect refresh',
      (tester) async {
        final source = _document(effect: effect);
        final fixture = await _ScreenFixture.mount(tester, document: source);
        try {
          final rect = tester.getRect(
            find.byKey(const ValueKey('craft-square-9')),
          );
          final pointer = await tester.startGesture(rect.center);
          for (final time in [0, 250, 500]) {
            fixture.now = time;
            await tester.pump(const Duration(milliseconds: 50));
            final expected = composeLitchcraftAnimatedSquare(
              emitters: [
                LitchcraftAnimatedEmitter(
                  color: 0xff4020,
                  effect: effect,
                  effectDurationMs: 1000,
                ),
                ...source.frames.single.emitters.skip(9 * 4 + 1).take(3),
              ],
              elapsedMs: time,
              sparkleSeed: 17,
              frameIndex: 0,
              emitterBaseIndex: 9 * 4,
            );
            expect(fixture.rendered.colors.skip(9 * 4).take(4), expected);
            expect(fixture.controller.documentSession.document, same(source));
            expect(fixture.controller.canUndo, isFalse);
          }
          await pointer.up();
          await tester.pump();
          final emitter = fixture
              .controller
              .documentSession
              .document
              .frames
              .single
              .emitters[9 * 4];
          expect(emitter.color, 0xff4020);
          expect(emitter.effect, effect);
          expect(emitter.effectDurationMs, 1000);
          expect(fixture.controller.canUndo, isTrue);
        } finally {
          await fixture.dispose(tester);
        }
      },
    );
  }

  testWidgets(
    'Erase preview clears masked effect and recomposes surviving grouped effect without committing',
    (tester) async {
      final source = _document(effect: LitchcraftEffectStyle.orbitClockwise);
      final fixture = await _ScreenFixture.mount(tester, document: source);
      try {
        await fixture.controller.setBrush(eraser: true);
        fixture.now = 300;
        await tester.pump(const Duration(milliseconds: 50));
        final rect = tester.getRect(
          find.byKey(const ValueKey('craft-square-9')),
        );
        final pointer = await tester.startGesture(rect.center);
        await tester.pump(const Duration(milliseconds: 16));
        final expected = composeLitchcraftAnimatedSquare(
          emitters: [
            const LitchcraftAnimatedEmitter(color: 0),
            ...source.frames.single.emitters.skip(9 * 4 + 1).take(3),
          ],
          elapsedMs: 300,
          sparkleSeed: 17,
          frameIndex: 0,
          emitterBaseIndex: 9 * 4,
        );
        expect(fixture.rendered.colors.skip(9 * 4).take(4), expected);
        expect(fixture.controller.documentSession.document, same(source));
        expect(fixture.controller.canUndo, isFalse);
        await pointer.up();
        await tester.pump();
        final actual = fixture
            .controller
            .documentSession
            .document
            .frames
            .single
            .emitters[9 * 4];
        expect(actual.color, 0);
        expect(actual.effect, LitchcraftEffectStyle.off);
        expect(actual.effectDurationMs, 0);
      } finally {
        await fixture.dispose(tester);
      }
    },
  );

  for (final change in [
    'color',
    'mask',
    'erase',
    'source',
    'selected frame',
    'frame body',
  ]) {
    for (final pumpBeforeCommit in [false, true]) {
      testWidgets(
        'pending touch cancels $change change pump=$pumpBeforeCommit and physical up cannot paint',
        (tester) async {
          final fixture = await _ScreenFixture.mount(
            tester,
            document: _document(multipleFrames: true),
          );
          try {
            final rect = tester.getRect(
              find.byKey(const ValueKey('craft-square-9')),
            );
            final pointer = await tester.startGesture(
              rect.topRight + Offset(-2, rect.height / 2),
            );
            await tester.pump(const Duration(milliseconds: 16));
            switch (change) {
              case 'color':
                await fixture.controller.setBrush(color: 0x00ff00);
              case 'mask':
                await fixture.controller.setBrush(targetMask: 2);
              case 'erase':
                await fixture.controller.setBrush(eraser: true);
              case 'source':
                fixture.controller.restoreDocument(_document());
              case 'selected frame':
                fixture.library.selectFrame(1);
              case 'frame body':
                fixture.controller.documentSession.paintSelectedFrame(
                  squares: [20],
                  mask: 15,
                  color: 0x112233,
                  history: false,
                );
            }
            final afterChange = fixture.controller.documentSession.document;
            if (pumpBeforeCommit) {
              await tester.pump(const Duration(milliseconds: 16));
            }
            await pointer.moveBy(const Offset(4, 0));
            await pointer.up();
            await tester.pump();
            expect(
              fixture.controller.documentSession.document,
              same(afterChange),
              reason:
                  'The undecided touch must not edit the new brush/source/frame.',
            );
            expect(fixture.controller.canUndo, isFalse);
            expect(fixture.rendered.colors, fixture.underlying.colors);
            // Cancellation ends just this physical gesture; the next down owns
            // the current brush and frame and can create one normal stroke.
            if (change == 'selected frame') {
              // Bare source selection bypasses the screen's normal select+seek
              // action. Restore coherence for the separate fresh-pointer check.
              fixture.library.selectFrame(0);
              await tester.pump();
            }
            final freshRect = tester.getRect(
              find.byKey(
                ValueKey(
                  change == 'erase' ? 'craft-square-9' : 'craft-square-21',
                ),
              ),
            );
            await tester.tapAt(freshRect.center);
            await tester.pump();
            expect(fixture.controller.canUndo, isTrue);
          } finally {
            await fixture.dispose(tester);
          }
        },
      );
    }
  }

  testWidgets('no-pump source change before hold erase cancels without history', (
    tester,
  ) async {
    final fixture = await _ScreenFixture.mount(tester, document: _document());
    try {
      final rect = tester.getRect(find.byKey(const ValueKey('craft-square-9')));
      final pointer = await tester.startGesture(rect.center);
      await tester.pump(const Duration(milliseconds: 16));
      fixture.controller.restoreDocument(
        _document(effect: LitchcraftEffectStyle.breathe),
      );
      final afterChange = fixture.controller.documentSession.document;
      // FakeAsync timer fires before the next build; guard must read live state.
      await tester.pump(const Duration(milliseconds: 500));
      await pointer.up();
      await tester.pump();
      expect(fixture.controller.documentSession.document, same(afterChange));
      expect(fixture.controller.canUndo, isFalse);
      expect(
        find.byKey(const ValueKey('craft-erase-feedback-1')),
        findsNothing,
      );
    } finally {
      await fixture.dispose(tester);
    }
  });
  testWidgets(
    'real screen long hold clears effects once and release adds no paint history',
    (tester) async {
      final fixture = await _ScreenFixture.mount(
        tester,
        document: _document(effect: LitchcraftEffectStyle.breathe),
      );
      try {
        final rect = tester.getRect(
          find.byKey(const ValueKey('craft-square-9')),
        );
        final pointer = await tester.startGesture(rect.center);
        await tester.pump(const Duration(milliseconds: 16));
        expect(fixture.controller.canUndo, isFalse);
        await pointer.moveBy(const Offset(2, 1));
        await tester.pump(const Duration(milliseconds: 534));
        final document = fixture.controller.documentSession.document;
        final erased = document.frames.single.emitters[9 * 4];
        expect(erased.color, 0);
        expect(erased.effect, LitchcraftEffectStyle.off);
        expect(erased.effectDurationMs, 0);
        expect(document.frames.single.emitters[9 * 4 + 1].color, 0x102030);
        expect(
          document.frames.single.emitters[9 * 4 + 1].effect,
          LitchcraftEffectStyle.breathe,
        );
        expect(fixture.controller.canUndo, isTrue);
        expect(fixture.rendered.colors, fixture.underlying.colors);
        await pointer.up();
        await tester.pump(const Duration(milliseconds: 300));
        expect(fixture.controller.documentSession.document, same(document));
        await fixture.controller.undo();
        await tester.pump();
        expect(
          fixture
              .controller
              .documentSession
              .document
              .frames
              .single
              .emitters[9 * 4]
              .color,
          0x102030,
        );
        expect(
          fixture
              .controller
              .documentSession
              .document
              .frames
              .single
              .emitters[9 * 4]
              .effect,
          LitchcraftEffectStyle.breathe,
        );
        expect(fixture.controller.canUndo, isFalse);
      } finally {
        await fixture.dispose(tester);
      }
    },
  );

  for (final pumpBeforeMove in [false, true]) {
    testWidgets(
      'real V2 timeline frame advance cancels undecided screen touch pump=$pumpBeforeMove',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final peer = LitchcraftV2FunctionalPeer()
          ..document = _document(multipleFrames: true);
        final ble = LitchcraftV2FunctionalService(peer);
        var now = 10000;
        final controller = LitchcraftController(
          ble,
          monotonicMilliseconds: () => now,
        );
        final library = LitchcraftLibrary(controller);
        await library.ready;
        library.dismissedWelcome = true;
        Future<void> drain() async {
          for (var i = 0; i < 30; i++) {
            await tester.pump(const Duration(milliseconds: 1));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
        }

        try {
          final entry = controller.enter();
          await drain();
          await entry;
          expect(controller.status, LitchcraftStatus.live);
          final brush = controller.setBrush(color: 0xff4020, targetMask: 1);
          await drain();
          await brush;
          peer.physicalSelect(0, play: true, elapsed: 900);
          await drain();
          expect(controller.animationPlaying, isTrue);
          expect(controller.animationSelectedFrameIndex, 0);
          await tester.pumpWidget(
            MaterialApp(
              home: Scaffold(
                body: LitchcraftScreen(
                  controller: controller,
                  library: library,
                  monotonicMilliseconds: () => now,
                ),
              ),
            ),
          );
          await tester.pump(const Duration(milliseconds: 16));
          final rect = tester.getRect(
            find.byKey(const ValueKey('craft-square-9')),
          );
          final pointer = await tester.startGesture(
            rect.topRight + Offset(-2, rect.height / 2),
          );
          await tester.pump(const Duration(milliseconds: 16));
          final rendered = tester.widget<LitchcraftThumbnail>(
            find.descendant(
              of: find.byKey(const ValueKey('litchcraft-canvas')),
              matching: find.byType(LitchcraftThumbnail),
            ),
          );
          expect(rendered.canvas.colors[9 * 4], 0xff4020);
          final before = controller.documentSession.document;
          final undoCount = peer.undo.length, writes = peer.writes.length;
          now += 200;
          expect(controller.animationSelectedFrameIndex, 1);
          if (pumpBeforeMove) {
            await tester.pump(const Duration(milliseconds: 50));
          }
          await pointer.moveBy(const Offset(4, 0));
          await pointer.up();
          await drain();
          expect(controller.documentSession.document, same(before));
          expect(peer.undo.length, undoCount);
          expect(
            peer.writes.length,
            writes,
            reason:
                'No Begin/Paint/End or Pause is sent for cancelled provisional intent.',
          );
          expect(controller.animationPlaying, isTrue);
          expect(peer.playing, isTrue);
          expect(tester.takeException(), isNull);
        } finally {
          await tester.pumpWidget(const SizedBox());
          library.dispose();
          controller.dispose();
          await ble.dispose();
          await peer.close();
        }
      },
    );
  }
  testWidgets(
    'no-pump source change fences second contact before twist and fresh coherent twist succeeds',
    (tester) async {
      final fixture = await _ScreenFixture.mount(tester, document: _document());
      try {
        final bounds = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final first = await tester.startGesture(bounds.center, pointer: 1);
        await tester.pump(const Duration(milliseconds: 16));
        fixture.controller.restoreDocument(
          _document(effect: LitchcraftEffectStyle.breathe),
        );
        final changed = fixture.controller.documentSession.document;
        // No pump between the production source change and second contact.
        final second = await tester.startGesture(
          bounds.center + const Offset(60, 0),
          pointer: 2,
        );
        await second.moveTo(bounds.center + const Offset(42, 42));
        await tester.pump(const Duration(milliseconds: 16));
        await second.moveTo(bounds.center + const Offset(0, 60));
        await tester.pump(const Duration(milliseconds: 100));
        expect(
          fixture.controller.documentSession.document,
          same(changed),
          reason:
              'Second contact must not discard the primary source fence and rotate the replacement artwork.',
        );
        expect(fixture.controller.canUndo, isFalse);
        expect(fixture.rendered.colors, fixture.underlying.colors);
        await second.up();
        await first.up();
        await tester.pump();
        expect(fixture.controller.documentSession.document, same(changed));
        expect(
          fixture.controller.canUndo,
          isFalse,
          reason: 'Cancelled physical releases must stay inert.',
        );

        final freshBounds = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final freshFirst = await tester.startGesture(
          freshBounds.center,
          pointer: 3,
        );
        final freshSecond = await tester.startGesture(
          freshBounds.center + const Offset(60, 0),
          pointer: 4,
        );
        await freshSecond.moveTo(freshBounds.center + const Offset(42, 42));
        await tester.pump();
        await freshSecond.moveTo(freshBounds.center + const Offset(0, 60));
        await tester.pump(const Duration(milliseconds: 100));
        await freshSecond.up();
        await freshFirst.up();
        await tester.pump();
        expect(fixture.controller.canUndo, isTrue);
        final rotated =
            fixture.controller.documentSession.document.frames.single.emitters;
        expect(rotated[14 * 4].color, 0x102030);
        expect(rotated[14 * 4].effect, LitchcraftEffectStyle.breathe);
        expect(rotated[9 * 4].color, 0);
        await fixture.controller.undo();
        await tester.pump();
        expect(
          fixture
              .controller
              .documentSession
              .document
              .frames
              .single
              .emitters[9 * 4]
              .color,
          0x102030,
        );
        expect(fixture.controller.canUndo, isFalse);
      } finally {
        await fixture.dispose(tester);
      }
    },
  );

  testWidgets(
    'no-pump playing-frame change fences second contact before twist and fresh coherent twist succeeds',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      final peer = LitchcraftV2FunctionalPeer()
        ..document = _document(multipleFrames: true);
      final ble = LitchcraftV2FunctionalService(peer);
      var now = 10000;
      final controller = LitchcraftController(
        ble,
        monotonicMilliseconds: () => now,
      );
      final library = LitchcraftLibrary(controller);
      await library.ready;
      library.dismissedWelcome = true;
      Future<void> drain() async {
        for (var i = 0; i < 30; i++) {
          await tester.pump(const Duration(milliseconds: 1));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
      }

      try {
        final entry = controller.enter();
        await drain();
        await entry;
        final brush = controller.setBrush(color: 0xff4020, targetMask: 1);
        await drain();
        await brush;
        peer.physicalSelect(0, play: true, elapsed: 900);
        await drain();
        expect(controller.animationPlaying, isTrue);
        expect(controller.animationSelectedFrameIndex, 0);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(
                controller: controller,
                library: library,
                monotonicMilliseconds: () => now,
              ),
            ),
          ),
        );
        await tester.pump(const Duration(milliseconds: 16));
        final bounds = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final first = await tester.startGesture(bounds.center, pointer: 1);
        await tester.pump(const Duration(milliseconds: 16));
        final before = controller.documentSession.document;
        final undoCount = peer.undo.length, writes = peer.writes.length;
        now += 200;
        expect(controller.animationSelectedFrameIndex, 1);
        // Projected playback advances before Flutter gets another build.
        final second = await tester.startGesture(
          bounds.center + const Offset(60, 0),
          pointer: 2,
        );
        await second.moveTo(bounds.center + const Offset(42, 42));
        await tester.pump();
        await second.moveTo(bounds.center + const Offset(0, 60));
        await drain();
        expect(controller.documentSession.document, same(before));
        expect(peer.document.frames[0].emitters[9 * 4].color, 0x102030);
        expect(peer.document.frames[1].emitters[9 * 4].color, 0x405060);
        expect(peer.undo.length, undoCount);
        expect(
          peer.writes.length,
          writes,
          reason:
              'Cancelled intent sends zero Paint/Rotate/history/Pause commands.',
        );
        expect(controller.animationPlaying, isTrue);
        expect(peer.playing, isTrue);
        await second.up();
        await first.up();
        await drain();
        expect(peer.writes.length, writes);
        expect(peer.undo.length, undoCount);

        // The functional peer does not advance its board metadata when the
        // app's injected clock advances. Use its existing authority event to
        // make the separate fresh gesture coherent at the displayed frame.
        peer.physicalSelect(1, play: true, elapsed: 1100);
        await drain();
        expect(controller.animationSelectedFrameIndex, 1);
        expect(library.selectedFrameIndex, 1);
        final freshBounds = tester.getRect(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final freshFirst = await tester.startGesture(
          freshBounds.center,
          pointer: 3,
        );
        final freshSecond = await tester.startGesture(
          freshBounds.center + const Offset(60, 0),
          pointer: 4,
        );
        await freshSecond.moveTo(freshBounds.center + const Offset(42, 42));
        await tester.pump();
        await freshSecond.moveTo(freshBounds.center + const Offset(0, 60));
        await drain();
        await freshSecond.up();
        await freshFirst.up();
        await drain();
        expect(peer.undo.length, undoCount + 1);
        expect(peer.document.frames[1].emitters[14 * 4].color, 0x405060);
        expect(peer.document.frames[1].emitters[9 * 4].color, 0);
        expect(
          peer.document.frames[0].emitters[9 * 4].color,
          0x102030,
          reason:
              'A fresh coherent twist edits only the currently displayed frame.',
        );
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await ble.dispose();
        await peer.close();
      }
    },
  );
}
