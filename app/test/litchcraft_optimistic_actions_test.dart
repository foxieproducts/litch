import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:shared_preferences/shared_preferences.dart';

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
  for (var i = 0; i < 2000 && !done; i++) {
    await tester.pump(const Duration(milliseconds: 1));
  }
  expect(done, isTrue, reason: 'bounded production GATT transaction');
  if (failure != null) throw failure!;
}

class _Rig {
  _Rig() {
    service = LitchcraftV2FunctionalService(peer);
    controller = LitchcraftController(service);
  }
  final peer = LitchcraftV2FunctionalPeer();
  late final LitchcraftV2FunctionalService service;
  late final LitchcraftController controller;
  Future<void> close(WidgetTester tester) async {
    controller.dispose();
    await finish(tester, service.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
  }

  List<int> operationsSince(int index) =>
      peer.writes.skip(index).map((p) => p[2]).toList();
}

void main() {
  setUp(() {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    SharedPreferences.setMockInitialValues({});
  });
  tearDown(() => debugDefaultTargetPlatformOverride = null);

  testWidgets('known Fill Undo and Redo preview before ACK with no snapshot', (
    tester,
  ) async {
    final rig = _Rig();
    await finish(tester, rig.controller.enter());
    final original = rig.peer.document;
    await finish(tester, rig.controller.fillAllWithColor(0x112233));
    final filled = rig.peer.document;
    for (final undo in [true, false]) {
      final gate = Completer<void>();
      rig.peer.historyReplyGate = gate.future;
      final start = rig.peer.writes.length;
      final work = undo ? rig.controller.undo() : rig.controller.redo();
      final expected = undo ? original : filled;
      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          expected,
        ),
        isTrue,
        reason:
            'the known inverse is visible in the command call, before native work',
      );
      await tester.pump(const Duration(milliseconds: 30));
      expect(rig.controller.pending, isTrue);
      expect(sameLitchcraftDocument(rig.peer.document, expected), isTrue);
      gate.complete();
      await finish(tester, work);
      expect(rig.operationsSince(start), [
        undo ? LitchcraftV2Protocol.undo : LitchcraftV2Protocol.redo,
      ]);
      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          expected,
        ),
        isTrue,
      );
    }
    await rig.close(tester);
  });

  testWidgets(
    'repeated known Undo and Redo stay optimistic and never download artwork',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      final documents = <LitchcraftAnimationDocument>[rig.peer.document];
      for (final color in <int>[0x112233, 0x445566, 0x778899]) {
        await finish(tester, rig.controller.fillAllWithColor(color));
        documents.add(rig.peer.document);
      }

      Future<void> verifyHistoryStep({
        required bool undo,
        required LitchcraftAnimationDocument expected,
      }) async {
        final gate = Completer<void>();
        rig.peer.historyReplyGate = gate.future;
        final start = rig.peer.writes.length;
        final work = undo ? rig.controller.undo() : rig.controller.redo();
        expect(
          sameLitchcraftDocument(
            rig.controller.documentSession.document,
            expected,
          ),
          isTrue,
          reason: 'every known history step previews before its board ACK',
        );
        await tester.pump(const Duration(milliseconds: 30));
        expect(sameLitchcraftDocument(rig.peer.document, expected), isTrue);
        gate.complete();
        await finish(tester, work);
        expect(rig.operationsSince(start), <int>[
          undo ? LitchcraftV2Protocol.undo : LitchcraftV2Protocol.redo,
        ]);
      }

      for (var index = documents.length - 2; index >= 0; --index) {
        await verifyHistoryStep(undo: true, expected: documents[index]);
      }
      for (var index = 1; index < documents.length; ++index) {
        await verifyHistoryStep(undo: false, expected: documents[index]);
      }
      await rig.close(tester);
    },
  );

  testWidgets(
    'painting during playback sends Paint immediately without a Pause round trip',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      rig.peer.playing = true;
      rig.peer.state(0);
      rig.peer.timeline();
      await tester.pump();
      expect(rig.controller.animationPlaying, isTrue);

      final reply = Completer<void>();
      rig.peer.paintReplyGate = reply.future;
      final start = rig.peer.writes.length;
      rig.controller.beginStroke();
      rig.controller.paintSquares(const <int>[7]);
      rig.controller.endStroke();
      await tester.pump(const Duration(milliseconds: 1));

      expect(rig.operationsSince(start), <int>[LitchcraftV2Protocol.paint]);
      expect(rig.peer.writes[start], hasLength(18));
      expect(rig.peer.playing, isFalse);
      reply.complete();
      for (var i = 0; i < 2000 && rig.controller.pending; ++i) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(rig.controller.pending, isFalse);
      await rig.close(tester);
    },
  );

  for (final action in ['duplicate', 'delete', 'duration']) {
    testWidgets(
      '$action previews immediately and commits one compact edit without readback',
      (tester) async {
        final rig = _Rig();
        rig.peer.paint(0, 0x112233);
        await finish(tester, rig.controller.enter());
        final expected = LitchcraftDocumentSession(rig.peer.document);
        final gate = Completer<void>();
        rig.peer.frameEditReplyGate = gate.future;
        final start = rig.peer.writes.length;
        late Future<bool> work;
        switch (action) {
          case 'duplicate':
            expected.addFrame();
            work = rig.controller.addAnimationFrame();
          case 'delete':
            expected.deleteSelectedFrame();
            work = rig.controller.deleteAnimationFrame();
          case 'duration':
            expected.setSelectedFrameDuration(1200);
            work = rig.controller.setAnimationFrameDuration(1200);
        }
        expect(
          sameLitchcraftDocument(
            rig.controller.documentSession.document,
            expected.document,
          ),
          isTrue,
        );
        expect(
          rig.controller.documentSession.selectedFrameIndex,
          expected.selectedFrameIndex,
        );
        await tester.pump(const Duration(milliseconds: 30));
        expect(rig.controller.pending, isTrue);
        expect(
          rig.controller.documentSession.selectedFrameIndex,
          expected.selectedFrameIndex,
          reason:
              'intermediate metadata cannot rewind the optimistic selection',
        );
        gate.complete();
        await finish(tester, work);
        expect(await work, isTrue);
        expect(rig.operationsSince(start), [LitchcraftV2Protocol.frameEdit]);
        expect(
          sameLitchcraftDocument(rig.peer.document, expected.document),
          isTrue,
        );
        await rig.close(tester);
      },
    );
  }

  testWidgets(
    'sole frame deletion clears immediately and retains one undoable frame',
    (tester) async {
      final rig = _Rig();
      final source = LitchcraftDocumentSession(rig.peer.document)
        ..deleteSelectedFrame();
      rig.peer.document = source.document;
      rig.peer.paint(4, 0x334455);
      await finish(tester, rig.controller.enter());
      final gate = Completer<void>();
      rig.peer.canvasEditReplyGate = gate.future;
      final start = rig.peer.writes.length;
      final work = rig.controller.deleteAnimationFrame();
      expect(rig.controller.documentSession.document.frames, hasLength(1));
      expect(rig.controller.canvas.colors.every((c) => c == 0), isTrue);
      await tester.pump(const Duration(milliseconds: 30));
      gate.complete();
      await finish(tester, work);
      expect(await work, isTrue);
      expect(rig.operationsSince(start), [LitchcraftV2Protocol.canvasEdit]);
      expect(rig.peer.undo, hasLength(1));
      await rig.close(tester);
    },
  );

  testWidgets(
    'playing frame duplicate captures displayed source with at most one Select',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      rig.peer.selectedFrame = 1;
      rig.peer.playing = true;
      rig.peer.elapsedMs = rig.peer.document.frames[0].durationMs;
      rig.peer.state(0);
      rig.peer.timeline();
      final gate = Completer<void>();
      rig.peer.frameEditReplyGate = gate.future;
      final start = rig.peer.writes.length;
      final work = rig.controller.addAnimationFrame();
      expect(rig.controller.documentSession.document.frames, hasLength(3));
      expect(rig.controller.documentSession.selectedFrameIndex, 2);
      await tester.pump(const Duration(milliseconds: 30));
      expect(rig.controller.documentSession.selectedFrameIndex, 2);
      gate.complete();
      await finish(tester, work);
      expect(await work, isTrue);
      expect(rig.operationsSince(start), [
        LitchcraftV2Protocol.timeline,
        LitchcraftV2Protocol.frameEdit,
      ]);
      await rig.close(tester);
    },
  );

  testWidgets(
    'duration commits its captured frame even when playback advances during drag',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      rig.peer.selectedFrame = 1;
      rig.peer.playing = true;
      rig.peer.elapsedMs = rig.peer.document.frames.first.durationMs;
      rig.peer.state(0);
      rig.peer.timeline();
      final target = rig.controller.captureAnimationFrameEditTarget()!;
      expect(target.frame, 1);
      final unchanged = rig.peer.document.frames.first.durationMs;
      rig.peer.selectedFrame = 0;
      rig.peer.elapsedMs = 0;
      rig.peer.state(0);
      rig.peer.timeline();
      final gate = Completer<void>();
      rig.peer.frameEditReplyGate = gate.future;
      final start = rig.peer.writes.length;
      final work = rig.controller.setAnimationFrameDuration(
        1300,
        target: target,
      );
      expect(
        rig.controller.documentSession.document.frames[1].durationMs,
        1300,
      );
      expect(
        rig.controller.documentSession.document.frames[0].durationMs,
        unchanged,
      );
      await tester.pump(const Duration(milliseconds: 20));
      gate.complete();
      await finish(tester, work);
      expect(await work, isTrue);
      expect(rig.peer.document.frames[1].durationMs, 1300);
      expect(rig.operationsSince(start), [
        LitchcraftV2Protocol.timeline,
        LitchcraftV2Protocol.frameEdit,
      ]);
      await rig.close(tester);
    },
  );

  testWidgets(
    'queued duplicate and duration preserve newest preview across older ACK',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      final gate = Completer<void>();
      rig.peer.frameEditReplyGate = gate.future;
      final duplicate = rig.controller.addAnimationFrame();
      await tester.pump(const Duration(milliseconds: 20));
      final duration = rig.controller.setAnimationFrameDuration(1500);
      expect(
        rig.controller.documentSession.document.frames[1].durationMs,
        1500,
      );
      expect(rig.controller.documentSession.selectedFrameIndex, 1);
      gate.complete();
      await finish(tester, Future.wait([duplicate, duration]));
      expect(await duplicate, isTrue);
      expect(await duration, isTrue);
      expect(
        rig.controller.documentSession.document.frames[1].durationMs,
        1500,
      );
      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          rig.peer.document,
        ),
        isTrue,
      );
      await rig.close(tester);
    },
  );

  testWidgets(
    'rejected duplicate retires only its preview and never replays on lift',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      final before = rig.peer.document;
      final gate = Completer<void>();
      rig.peer.physicalBrushHeld = true;
      rig.peer.frameEditReplyGate = gate.future;
      final work = rig.controller.addAnimationFrame();
      expect(rig.controller.documentSession.document.frames, hasLength(3));
      await tester.pump(const Duration(milliseconds: 20));
      gate.complete();
      await finish(tester, work);
      expect(await work, isFalse);
      expect(
        sameLitchcraftDocument(rig.controller.documentSession.document, before),
        isTrue,
      );
      expect(rig.controller.waitingForLift, isTrue);
      final start = rig.peer.writes.length;
      rig.peer.physicalBrushHeld = false;
      await tester.pump(const Duration(seconds: 1));
      expect(rig.peer.writes.length, start);
      await rig.close(tester);
    },
  );

  testWidgets(
    'captured frame target cannot shift behind a pending structural edit',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      final target = rig.controller.captureAnimationFrameEditTarget(frame: 1)!;
      final gate = Completer<void>();
      rig.peer.frameEditReplyGate = gate.future;
      final work = rig.controller.addAnimationFrame();
      final duplicate = rig.controller.documentSession.document;
      expect(
        await rig.controller.deleteAnimationFrame(target: target),
        isFalse,
      );
      expect(
        await rig.controller.setAnimationFrameDuration(700, target: target),
        isFalse,
      );
      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          duplicate,
        ),
        isTrue,
      );
      gate.complete();
      await finish(tester, work);
      await rig.close(tester);
    },
  );

  testWidgets('rejected predecessor preserves a newer queued duration draft', (
    tester,
  ) async {
    final rig = _Rig();
    await finish(tester, rig.controller.enter());
    final gate = Completer<void>();
    rig.peer.physicalBrushHeld = true;
    rig.peer.frameEditReplyGate = gate.future;
    final duplicate = rig.controller.addAnimationFrame();
    await tester.pump(const Duration(milliseconds: 20));
    final duration = rig.controller.setAnimationFrameDuration(1500);
    final newest = rig.controller.documentSession.document;
    gate.complete();
    await finish(tester, Future.wait([duplicate, duration]));
    expect(await duplicate, isFalse);
    expect(await duration, isFalse);
    expect(
      sameLitchcraftDocument(rig.controller.documentSession.document, newest),
      isTrue,
    );
    expect(rig.controller.status, LitchcraftStatus.conflict);
    expect(rig.peer.document.frames, hasLength(2));
    await rig.close(tester);
  });

  testWidgets('retired entry and old ACK cannot edit a newly opened document', (
    tester,
  ) async {
    final rig = _Rig();
    await finish(tester, rig.controller.enter());
    final target = rig.controller.captureAnimationFrameEditTarget()!;
    final gate = Completer<void>();
    rig.peer.frameEditReplyGate = gate.future;
    final work = rig.controller.addAnimationFrame();
    await tester.pump(const Duration(milliseconds: 20));
    rig.peer.active = false;
    rig.peer.state(0);
    final replacement = LitchcraftDocumentSession(rig.peer.document)
      ..selectFrame(0)
      ..setSelectedFrameDuration(1700);
    rig.controller.restoreDocument(replacement.document);
    gate.complete();
    await finish(tester, work);
    expect(await work, isFalse);
    expect(
      sameLitchcraftDocument(
        rig.controller.documentSession.document,
        replacement.document,
      ),
      isTrue,
    );
    final start = rig.peer.writes.length;
    expect(await rig.controller.deleteAnimationFrame(target: target), isFalse);
    expect(
      await rig.controller.setAnimationFrameDuration(700, target: target),
      isFalse,
    );
    expect(rig.peer.writes.length, start);
    await rig.close(tester);
  });

  testWidgets(
    'physical history after Fill invalidates the optimistic Undo proof',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      await finish(tester, rig.controller.fillAllWithColor(0x112233));
      final filled = rig.peer.document;
      rig.peer.physicalPaint(5, 0x445566);
      await finish(tester, rig.controller.coherentDocument());
      final physical = rig.controller.documentSession.document;
      final gate = Completer<void>();
      rig.peer.historyReplyGate = gate.future;
      final start = rig.peer.writes.length;
      final work = rig.controller.undo();
      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          physical,
        ),
        isTrue,
        reason: 'unknown board history must not guess the earlier Fill inverse',
      );
      await tester.pump(const Duration(milliseconds: 20));
      gate.complete();
      await finish(tester, work);
      expect(
        sameLitchcraftDocument(rig.controller.documentSession.document, filled),
        isTrue,
      );
      expect(
        rig.operationsSince(start),
        contains(LitchcraftV2Protocol.snapshot),
      );
      await rig.close(tester);
    },
  );

  testWidgets(
    'same-link re-entry repairs a missed board edit before showing live state',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());
      rig.peer.deliverNotifications = false;
      rig.peer.physicalPaint(5, 0x445566);
      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          rig.peer.document,
        ),
        isFalse,
        reason: 'reproduce the retained-screen notification gap',
      );

      rig.peer.deliverNotifications = true;
      final start = rig.peer.writes.length;
      await finish(tester, rig.controller.revalidateSelectedBoard());

      expect(
        sameLitchcraftDocument(
          rig.controller.documentSession.document,
          rig.peer.document,
        ),
        isTrue,
      );
      expect(
        rig.operationsSince(start),
        containsAllInOrder(<int>[
          LitchcraftV2Protocol.inspect,
          LitchcraftV2Protocol.snapshot,
        ]),
      );
      expect(rig.controller.status, LitchcraftStatus.live);
      await rig.close(tester);
    },
  );

  testWidgets(
    'schema3 domain editor interactions use compact work with no pacing delay',
    (tester) async {
      final rig = _Rig();
      await finish(tester, rig.controller.enter());

      Future<void> verify(
        String name,
        Future<dynamic> Function() action,
        int expectedOperation, {
        int? expectedMagic,
      }) async {
        final start = rig.peer.writes.length;
        var done = false;
        Object? failure;
        action().then(
          (_) => done = true,
          onError: (Object error) {
            failure = error;
            done = true;
          },
        );
        for (var elapsedMs = 0; elapsedMs < 50 && !done; elapsedMs++) {
          await tester.pump(const Duration(milliseconds: 1));
        }
        if (failure != null) throw failure!;
        expect(done, isTrue, reason: '$name has no internal >=50 ms pacing');
        final packets = rig.peer.writes.skip(start).toList();
        expect(
          packets,
          isNotEmpty,
          reason: '$name reaches the production GATT seam',
        );
        expect(
          packets,
          everyElement(hasLength(lessThanOrEqualTo(20))),
          reason: '$name stays in one minimum-ATT packet',
        );
        expect(
          packets.where(
            (packet) =>
                packet[0] == LitchcraftV2Protocol.magic &&
                <int>{
                  LitchcraftV2Protocol.replaceBeginOp,
                  LitchcraftV2Protocol.replaceChunkOp,
                  LitchcraftV2Protocol.replaceCommitOp,
                  LitchcraftV2Protocol.streamBegin,
                  LitchcraftV2Protocol.streamChunk,
                  LitchcraftV2Protocol.streamCommit,
                  LitchcraftV2Protocol.streamCheckpoint,
                }.contains(packet[2]),
          ),
          isEmpty,
          reason: '$name must never become a document transfer',
        );
        final packet = packets.last;
        expect(packet[0], expectedMagic ?? LitchcraftV2Protocol.magic);
        expect(
          packet[packet[0] == LitchcraftV2Protocol.magic ? 2 : 1],
          expectedOperation,
        );
      }

      await verify(
        'color/tool/mask',
        () => rig.controller.setBrush(color: 0x112233, targetMask: 5),
        LitchcraftProtocol.tool,
        expectedMagic: LitchcraftProtocol.magic,
      );
      await verify(
        'color swap',
        rig.controller.swapBrushColor,
        LitchcraftProtocol.tool,
        expectedMagic: LitchcraftProtocol.magic,
      );
      await verify(
        'Fill All',
        rig.controller.fillAll,
        LitchcraftV2Protocol.canvasEdit,
      );
      await finish(tester, rig.controller.setBrush(color: 0x445566));
      await verify(
        'light checker fill',
        () => rig.controller.fillChecker(dark: false),
        LitchcraftV2Protocol.canvasEdit,
      );
      await finish(tester, rig.controller.setBrush(color: 0x778899));
      await verify(
        'dark checker fill',
        () => rig.controller.fillChecker(dark: true),
        LitchcraftV2Protocol.canvasEdit,
      );
      await verify(
        'Clear All',
        rig.controller.clear,
        LitchcraftV2Protocol.canvasEdit,
      );
      await finish(
        tester,
        rig.controller.setBrush(eraser: false, color: 0x112233),
      );
      await finish(tester, rig.controller.fillAll());
      await verify('light checker erase', () async {
        await rig.controller.setBrush(eraser: true);
        await rig.controller.fillChecker(dark: false);
      }, LitchcraftV2Protocol.canvasEdit);
      await verify(
        'dark checker erase',
        () => rig.controller.fillChecker(dark: true),
        LitchcraftV2Protocol.canvasEdit,
      );
      await finish(
        tester,
        rig.controller.setBrush(eraser: false, color: 0x445566),
      );
      await finish(tester, rig.controller.fillChecker(dark: false));
      await verify(
        'Rotate',
        () => rig.controller.rotateArtwork(1),
        LitchcraftV2Protocol.canvasEdit,
      );
      await verify(
        'Effect',
        () => rig.controller.applyAnimationEffect(
          square: 7,
          mask: 3,
          effect: LitchcraftEffectStyle.breathe,
          effectDurationMs: 1200,
        ),
        LitchcraftV2Protocol.effect,
      );
      await verify(
        'Play',
        rig.controller.toggleAnimationPlayback,
        LitchcraftV2Protocol.timeline,
      );
      await verify(
        'Pause',
        rig.controller.toggleAnimationPlayback,
        LitchcraftV2Protocol.timeline,
      );
      await verify(
        'Select frame',
        () => rig.controller.selectAnimationFrame(1),
        LitchcraftV2Protocol.timeline,
      );
      await verify(
        'Add/copy frame',
        rig.controller.addAnimationFrame,
        LitchcraftV2Protocol.frameEdit,
      );
      await verify(
        'Frame duration',
        () => rig.controller.setAnimationFrameDuration(137),
        LitchcraftV2Protocol.frameEdit,
      );
      await verify(
        'Delete frame',
        rig.controller.deleteAnimationFrame,
        LitchcraftV2Protocol.frameEdit,
      );
      await verify('Undo', rig.controller.undo, LitchcraftV2Protocol.undo);
      await verify('Redo', rig.controller.redo, LitchcraftV2Protocol.redo);

      final paintStart = rig.peer.writes.length;
      rig.controller.beginStroke();
      rig.controller.paintSquares(const <int>[0, 7, 63]);
      rig.controller.endStroke();
      for (
        var elapsedMs = 0;
        elapsedMs < 50 && rig.peer.writes.length == paintStart;
        elapsedMs++
      ) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      final paintPackets = rig.peer.writes.skip(paintStart).toList();
      expect(paintPackets, isNotEmpty);
      expect(paintPackets, everyElement(hasLength(lessThanOrEqualTo(20))));
      expect(paintPackets.last[2], LitchcraftV2Protocol.paint);
      await rig.close(tester);
    },
  );
}
