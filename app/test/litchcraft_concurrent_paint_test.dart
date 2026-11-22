import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'fixtures/litchcraft_v2_functional_peer.dart';

Future<void> finish(WidgetTester tester, Future<dynamic> work) async {
  var done = false;
  Object? failure;
  work.then(
    (_) => done = true,
    onError: (Object error) {
      failure = error;
      done = true;
    },
  );
  for (var i = 0; i < 2000 && !done; ++i) {
    await tester.pump(const Duration(milliseconds: 1));
  }
  expect(done, isTrue);
  if (failure != null) throw failure!;
}

void main() {
  setUp(() {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    SharedPreferences.setMockInitialValues({});
  });
  tearDown(() => debugDefaultTargetPlatformOverride = null);

  testWidgets('physical RGB and app effect on one emitter recover compatibly', (
    tester,
  ) async {
    final peer = LitchcraftV2FunctionalPeer();
    final service = LitchcraftV2FunctionalService(peer);
    final controller = LitchcraftController(service);
    await finish(tester, controller.enter());
    final gate = Completer<void>();
    peer.beforeDomainWork = () => gate.future;
    final applying = controller.applyAnimationEffect(
      square: 0,
      mask: 1,
      effect: LitchcraftEffectStyle.breathe,
    );
    await tester.pump();
    peer.physicalPaint(0, 0x123456);
    gate.complete();
    await finish(tester, applying);
    for (
      var i = 0;
      i < 4000 &&
          (controller.status != LitchcraftStatus.live || controller.pending);
      i++
    ) {
      await tester.pump(const Duration(milliseconds: 1));
      if (i % 100 == 0) await tester.runAsync(() async {});
    }
    expect(
      controller.status,
      LitchcraftStatus.live,
      reason: controller.errorMessage,
    );
    expect(
      sameLitchcraftDocument(
        peer.document,
        controller.documentSession.document,
      ),
      isTrue,
    );
    expect(peer.document.frames[0].emitters[0].color, 0x123456);
    expect(
      peer.document.frames[0].emitters[0].effect,
      LitchcraftEffectStyle.breathe,
    );
    controller.dispose();
    await finish(tester, service.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets('500ms canvas hold erases while Paint stays selected', (
    tester,
  ) async {
    final peer = LitchcraftV2FunctionalPeer()..paint(0, 0x123456);
    final service = LitchcraftV2FunctionalService(peer);
    final controller = LitchcraftController(service);
    await finish(tester, controller.enter());
    await tester.pumpWidget(
      MaterialApp(
        home: Center(
          child: SizedBox(
            width: 320,
            height: 320,
            child: LitchcraftCanvasView(
              canvas: controller.canvas,
              onBegin: controller.beginStroke,
              onPaint: controller.paintSquares,
              onEnd: controller.endStroke,
              onEraseSquare: (square) {
                controller.beginStroke(erasing: true);
                controller.paintSquares([square]);
                controller.endStroke();
              },
            ),
          ),
        ),
      ),
    );
    final origin = tester.getTopLeft(find.byType(LitchcraftCanvasView));
    final gesture = await tester.startGesture(origin + const Offset(20, 20));
    await tester.pump(const Duration(milliseconds: 500));
    for (var i = 0; i < 2000 && controller.pending; i++) {
      await tester.pump(const Duration(milliseconds: 1));
    }
    expect(controller.canvas.colors[0], 0);
    expect(peer.document.frames[0].emitters[0].color, 0);
    expect(controller.eraser, isFalse);
    await gesture.cancel();
    await tester.pumpWidget(const SizedBox());
    controller.dispose();
    await finish(tester, service.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets(
    'lost first ACK retires queued write then recovers exact accepted prefix',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      await finish(tester, controller.enter());
      final gate = Completer<void>();
      peer.paintReplyGate = gate.future;
      controller.beginStroke();
      controller.paintSquares([7]);
      controller.endStroke();
      await tester.pump();
      controller.beginStroke();
      controller.paintSquares([8]);
      controller.endStroke();
      peer.physicalPaint(0, 0x123456);
      for (
        var i = 0;
        i < 9000 && controller.status != LitchcraftStatus.error;
        i++
      ) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(controller.status, LitchcraftStatus.error);
      peer.paintReplyGate = null;
      for (
        var i = 0;
        i < 4000 &&
            (controller.status != LitchcraftStatus.live || controller.pending);
        i++
      ) {
        await tester.pump(const Duration(milliseconds: 1));
        if (i % 100 == 0) await tester.runAsync(() async {});
      }
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      expect(
        sameLitchcraftDocument(
          peer.document,
          controller.documentSession.document,
        ),
        isTrue,
      );
      expect(peer.document.frames[0].emitters[0].color, 0x123456);
      expect(peer.document.frames[0].emitters[28].color, 0xff7430);
      expect(peer.document.frames[0].emitters[32].color, 0xff7430);
      gate.complete();
      controller.dispose();
      await finish(tester, service.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final erase in [false, true]) {
    testWidgets('stroke no-op reversal, empty End, Undo; erase=$erase', (
      tester,
    ) async {
      final peer = LitchcraftV2FunctionalPeer();
      final initial = LitchcraftDocumentSession(peer.document)
        ..paintSelectedFrame(squares: [0, 1, 2], mask: 15, color: 0x123456)
        ..applyEffect(
          square: 1,
          mask: 15,
          effect: LitchcraftEffectStyle.breathe,
        );
      peer.document = initial.document;
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      await finish(tester, controller.enter());
      final start = peer.writes.length;
      controller.beginStroke(erasing: erase);
      controller.paintSquares([0, 1, 2]);
      await tester.pump(const Duration(milliseconds: 10));
      controller.paintSquares([2, 1, 0, 1, 2]);
      controller.endStroke();
      await finish(
        tester,
        Future<void>(() async {
          while (controller.pending) {
            await Future<void>.delayed(const Duration(milliseconds: 1));
          }
        }),
      );
      final writes = peer.writes
          .skip(start)
          .where((p) => p.first == 0x4d)
          .toList();
      final paints = writes.where((p) => p[2] == 3).toList();
      expect(paints.fold<int>(0, (sum, p) => sum + p.length - 17), 3);
      expect(writes.where((p) => p[2] == 4 && p.length == 17), isEmpty);
      expect(peer.undo, hasLength(1));
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          peer.document,
        ),
        isTrue,
      );
      expect(
        controller.eraser,
        isFalse,
        reason: 'stroke override never changes Paint tool',
      );
      if (erase) {
        expect(
          peer.document.frames[0].emitters[4].effect,
          LitchcraftEffectStyle.off,
        );
        expect(peer.document.frames[0].emitters[4].color, 0);
      }
      await finish(tester, controller.undo());
      expect(sameLitchcraftDocument(peer.document, initial.document), isTrue);
      controller.dispose();
      await finish(tester, service.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    });
  }

  testWidgets(
    'two queued taps preserve later overlay across physical acceptance',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      await finish(tester, controller.enter());
      final gate = Completer<void>();
      peer.beforeDomainWork = () => gate.future;
      controller.beginStroke();
      controller.paintSquares([7]);
      controller.endStroke();
      await tester.pump();
      controller.beginStroke();
      controller.paintSquares([8]);
      controller.endStroke();
      peer.physicalPaint(0, 0x123456);
      expect(
        controller.documentSession.document.frames[0].emitters[32].color,
        0xff7430,
      );
      gate.complete();
      for (var i = 0; i < 2000 && controller.pending; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(controller.status, LitchcraftStatus.live);
      expect(
        sameLitchcraftDocument(
          peer.document,
          controller.documentSession.document,
        ),
        isTrue,
      );
      expect(peer.document.frames[0].emitters[0].color, 0x123456);
      expect(peer.document.frames[0].emitters[28].color, 0xff7430);
      expect(peer.document.frames[0].emitters[32].color, 0xff7430);
      controller.dispose();
      await finish(tester, service.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'disjoint app paint during physical snapshot converges silently',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final service = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(service);
      await finish(tester, controller.enter());
      final statuses = <LitchcraftStatus>[];
      controller.addListener(() => statuses.add(controller.status));
      final gate = Completer<void>();
      var snapshotEntered = false;
      peer.beforeSnapshot = () async {
        snapshotEntered = true;
        await gate.future;
      };
      peer.physicalPaint(0, 0x123456);
      for (var i = 0; i < 100 && !snapshotEntered; ++i) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(snapshotEntered, isTrue);
      controller.beginStroke();
      controller.paintSquares([7]);
      controller.endStroke();
      gate.complete();
      for (var i = 0; i < 2000 && controller.pending; ++i) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      final app = controller.documentSession.document;
      final board = peer.document;
      controller.dispose();
      await finish(tester, service.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
      expect(
        statuses,
        isNot(contains(LitchcraftStatus.conflict)),
        reason: 'different squares have compatible intent',
      );
      expect(app.frames[0].emitters[0].color, 0x123456);
      expect(app.frames[0].emitters[28].color, 0xff7430);
      expect(sameLitchcraftDocument(app, board), isTrue);
    },
  );

  testWidgets('physical edit before app paint acceptance remains visible', (
    tester,
  ) async {
    final peer = LitchcraftV2FunctionalPeer();
    final service = LitchcraftV2FunctionalService(peer);
    final controller = LitchcraftController(service);
    await finish(tester, controller.enter());
    final gate = Completer<void>();
    peer.beforeDomainWork = () => gate.future;
    controller.beginStroke();
    controller.paintSquares([7]);
    controller.endStroke();
    await tester.pump(const Duration(milliseconds: 10));
    peer.physicalPaint(0, 0x123456);
    gate.complete();
    for (var i = 0; i < 2000 && controller.pending; ++i) {
      await tester.pump(const Duration(milliseconds: 1));
    }
    final app = controller.documentSession.document;
    final board = peer.document;
    controller.dispose();
    await finish(tester, service.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
    expect(
      sameLitchcraftDocument(app, board),
      isTrue,
      reason:
          'receipt revision includes physical work and cannot confirm app-only document',
    );
  });
}
