import 'dart:async';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:litch_mobile/src/widgets/litchcraft_motion_editor.dart';
import 'package:litch_mobile/src/widgets/litchcraft_tools.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'fixtures/litchcraft_v2_functional_peer.dart';

class _InterruptedPeer extends LitchcraftV2FunctionalPeer {
  bool failChunk = false, loseCommitAck = false;
  int commits = 0;
  final Set<int> committedRequests = {};
  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    if (failChunk &&
        value[0] == 0x4d &&
        (value[2] == LitchcraftV2Protocol.streamChunk ||
            value[2] == LitchcraftV2Protocol.replaceChunkOp)) {
      failChunk = false;
      throw StateError('interrupted native staging write');
    }
    await super.write(
      serviceId: serviceId,
      characteristicId: characteristicId,
      value: value,
      withResponse: withResponse,
    );
  }

  @override
  void respond(List<int> bytes) {
    if (bytes[0] == 0x4d &&
        (bytes[2] == LitchcraftV2Protocol.streamCommit ||
            bytes[2] == LitchcraftV2Protocol.replaceCommitOp)) {
      final request = bytes[7] | bytes[8] << 8;
      if (!committedRequests.add(request)) {
        if (!loseCommitAck) state(request);
        return;
      }
      commits++;
      final notifications = deliverNotifications;
      if (loseCommitAck) deliverNotifications = false;
      super.respond(bytes);
      deliverNotifications = notifications;
      return;
    }
    super.respond(bytes);
  }
}

Iterable<List<int>> _sharedCraftCommands(
  Iterable<ConnectionSessionPacket> writes,
) sync* {
  final assembler = ConnectionLitchcraftAssembler();
  for (final packet in writes) {
    if (packet.kind == ConnectionSessionKind.ordered &&
        packet.code == ConnectionSession.litchcraftModeOpcode) {
      yield packet.body;
    } else if (packet.kind == ConnectionSessionKind.object &&
        packet.code == ConnectionLitchcraftCodec.objectOpcode) {
      final command = assembler.accept(packet.body);
      if (command != null) yield command;
    }
  }
}

class _DelayedDraftStore extends LitchcraftArtworkStore {
  _DelayedDraftStore(super.preferences);
  final pending = Completer<LitchcraftLibrarySnapshot>();
  @override
  Future<LitchcraftLibrarySnapshot> load() => pending.future;
}

class _EntryWriteTrace implements AppTraceSink {
  _EntryWriteTrace(this.observe);
  final void Function(String, Map<String, Object?>) observe;
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) =>
      observe(event, fields);
}

Future<T> finish<T>(WidgetTester tester, Future<T> work) async {
  var done = false;
  Object? failure;
  work.then(
    (_) {
      done = true;
    },
    onError: (Object error) {
      failure = error;
      done = true;
    },
  );
  for (var i = 0; i < 2000 && !done; i++) {
    await tester.pump(const Duration(milliseconds: 1));
    if (i % 100 == 0) {
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
    }
  }
  expect(done, isTrue, reason: 'bounded native GATT operation');
  if (failure != null) throw failure!;
  return work;
}

Future<void> settle(
  WidgetTester tester,
  LitchcraftController controller,
) async {
  for (var i = 0; i < 1000; i++) {
    await tester.pump(const Duration(milliseconds: 1));
    if (!controller.pending) return;
  }
  fail('controller did not settle');
}

void main() {
  setUp(() {
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    SharedPreferences.setMockInitialValues(<String, Object>{});
  });
  tearDown(() => debugDefaultTargetPlatformOverride = null);

  for (final afterAppExit in [false, true]) {
    testWidgets(
      'physical advanced entry follows ${afterAppExit ? "app exit" : "inactive inspection"} without user recovery',
      (tester) async {
        final peer = LitchcraftV2FunctionalPeer()..active = afterAppExit;
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        if (afterAppExit) {
          await finish(tester, controller.enter());
          await finish(tester, controller.leave());
        } else {
          await finish(tester, controller.adoptActiveBoard());
        }
        for (var cycle = 0; cycle < 5; cycle++) {
          peer.physicalEnter();
          await settle(tester, controller);
          expect(controller.selected, isTrue);
          expect(controller.boardActive, isTrue);
          expect(controller.status, LitchcraftStatus.live);
          expect(controller.sessionId, peer.sessionId);
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              peer.document,
            ),
            isTrue,
          );
          peer.physicalExit();
          await settle(tester, controller);
          expect(controller.selected, isFalse);
          expect(controller.boardActive, isFalse);
        }
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'Android reuses safe calibration after failure in the same link',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      addTearDown(() async {
        await ble.dispose();
        await peer.close();
      });
      peer.beforeMtu = () async {
        throw StateError('transient MTU failure');
      };
      final token = ble.connectionSessionToken;
      expect(
        await ble.litchcraftPacketPayloadBytes(
          'board',
          connectionSessionToken: token,
        ),
        20,
      );
      peer.beforeMtu = null;
      expect(
        await ble.litchcraftPacketPayloadBytes(
          'board',
          connectionSessionToken: token,
        ),
        20,
      );
      expect(
        await ble.litchcraftPacketPayloadBytes(
          'board',
          connectionSessionToken: token,
        ),
        20,
      );
      expect(
        peer.mtuRequests,
        1,
        reason: 'A failed MTU attempt must not block the same link again',
      );
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'restored advanced draft exits after an in-flight compact Enter ACK',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()..active = false;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(
        ble,
        documentSession: LitchcraftDocumentSession(peer.document),
      );
      final gate = Completer<void>(), draft = Completer<void>();
      peer.enterReplyGate = gate.future;
      final entry = controller.enter(draftReady: draft.future);
      await tester.pump(const Duration(milliseconds: 20));
      expect(peer.active, isTrue);
      final leaving = controller.leave();
      gate.complete();
      await finish(tester, Future.wait([entry, leaving]));
      expect(peer.active, isFalse);
      expect(controller.boardActive, isFalse);
      expect(controller.selected, isFalse);
      expect(
        peer.writes.where(
          (p) => p[0] == 0x4c && p[1] == LitchcraftProtocol.exit,
        ),
        hasLength(1),
      );
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          peer.document,
        ),
        isTrue,
      );
      draft.complete();
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'advanced active first connection adopts authority without Enter',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      var adopted = false;
      await finish(
        tester,
        controller.adoptActiveBoard().then((value) => adopted = value),
      );
      expect(adopted, isTrue);
      expect(controller.selected, isTrue);
      expect(controller.status, LitchcraftStatus.live);
      expect(
        peer.writes.where(
          (p) => p[0] == 0x4c && p[1] == LitchcraftProtocol.enter,
        ),
        isEmpty,
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'advanced inspection and reconnect restore physical Paint Erase RGB and mask',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()
        ..brushErase = true
        ..brushColor = 0x123456
        ..brushMask = 5;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.adoptActiveBoard());
      expect(controller.eraser, isTrue);
      expect(controller.color, 0x123456);
      expect(controller.targetMask, 5);
      for (var cycle = 0; cycle < 5; cycle++) {
        await finish(tester, controller.setBrush(eraser: false));
        expect(peer.brushErase, isFalse);
        peer.physicalToggleEraser();
        expect(controller.eraser, isTrue);
        final connection = ble.connectionSessionToken;
        await finish(tester, ble.disconnect());
        expect(ble.isConnected, isFalse);
        expect(ble.connectionSessionToken, isNot(connection));
        for (var pass = 0; pass < 8; pass++) {
          await tester.runAsync(() async {});
          await tester.pump();
        }
        peer.brushErase = false;
        peer.brushColor = 0xabcdef;
        peer.brushMask = 15;
        ble.debugSetConnectedDevice('board');
        controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
        await settle(tester, controller);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.eraser, isFalse);
        expect(controller.color, 0xabcdef);
        expect(controller.targetMask, 15);
        peer.physicalToggleEraser();
        expect(controller.eraser, isTrue);
      }
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final bulk in [true, false]) {
    testWidgets(
      'interrupted ${bulk ? "bulk" : "small"} document delivery repairs without recovery controls',
      (tester) async {
        final peer = _InterruptedPeer()..supportsBulkDocument = bulk;
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        await finish(tester, controller.enter());
        final edited = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef);
        peer.failChunk = true;
        await finish(tester, controller.replaceDocument(edited.document));
        expect(controller.status, LitchcraftStatus.error);
        expect(peer.commits, 0);
        for (
          var i = 0;
          i < 1500 && controller.status != LitchcraftStatus.live;
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
        expect(sameLitchcraftDocument(peer.document, edited.document), isTrue);
        expect(peer.commits, 1);
        expect(controller.artworkDeliveryPending, isFalse);
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'lost live document commit receipt repairs without duplicate commit or conflict',
    (tester) async {
      final peer = _InterruptedPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final edited = LitchcraftDocumentSession(peer.document)
        ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef);
      peer.loseCommitAck = true;
      var settled = false;
      unawaited(
        controller.replaceDocument(edited.document).then((_) => settled = true),
      );
      for (
        var i = 0;
        i < 10000 && (!settled || controller.status != LitchcraftStatus.live);
        i++
      ) {
        await tester.pump(const Duration(milliseconds: 1));
        if (i % 100 == 0) await tester.runAsync(() async {});
      }
      expect(settled, isTrue);
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      expect(sameLitchcraftDocument(peer.document, edited.document), isTrue);
      expect(peer.commits, 1);
      expect(controller.artworkDeliveryPending, isFalse);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'draft restoration before Enter ACK preserves acknowledged session authority',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()..active = false;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final store = _DelayedDraftStore(await SharedPreferences.getInstance());
      final library = LitchcraftLibrary(controller, store: store);
      final saved = LitchcraftDocumentSession(peer.document)
        ..paintSelectedFrame(squares: [9], mask: 15, color: 0x123456);
      final ack = Completer<void>();
      peer.enterReplyGate = ack.future;
      final entry = controller.enter(draftReady: library.ready);
      for (var i = 0; i < 49 && !peer.active; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(peer.active, isTrue);
      store.pending.complete(
        LitchcraftLibrarySnapshot(
          draft: LitchcraftArtwork.animated(
            id: 'saved-draft',
            name: 'Saved draft',
            document: saved.document,
            createdAt: DateTime.utc(2026),
            modifiedAt: DateTime.utc(2026),
          ),
        ),
      );
      await finish(tester, library.ready);
      expect(controller.documentSession.replacementGeneration, greaterThan(0));
      ack.complete();
      await finish(tester, entry);
      final status = controller.status;
      final knownSession = controller.sessionId;
      final boardKnown = controller.documentSession.boardDocument != null;
      final sourcePreserved = sameLitchcraftDocument(
        controller.documentSession.document,
        saved.document,
      );
      controller.dispose();
      library.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
      expect(status, LitchcraftStatus.conflict);
      expect(knownSession, 42);
      expect(boardKnown, isTrue);
      expect(sourcePreserved, isTrue);
    },
  );

  testWidgets('Play selected before Enter ACK exits without waiting for disk', (
    tester,
  ) async {
    final peer = LitchcraftV2FunctionalPeer()..active = false;
    final ble = LitchcraftV2FunctionalService(peer);
    final controller = LitchcraftController(ble);
    final ack = Completer<void>(), disk = Completer<void>();
    peer.enterReplyGate = ack.future;
    var entrySettled = false, leaveSettled = false;
    final entry = controller
        .enter(draftReady: disk.future)
        .then((_) => entrySettled = true);
    for (var i = 0; i < 49 && !peer.active; i++) {
      await tester.pump(const Duration(milliseconds: 1));
    }
    expect(peer.active, isTrue);
    expect(
      controller.boardActive,
      isFalse,
      reason: 'Enter ACK is still withheld',
    );
    final leaving = controller.leave().then((_) => leaveSettled = true);
    ack.complete();
    for (var i = 0; i < 49 && !leaveSettled; i++) {
      await tester.pump(const Duration(milliseconds: 1));
    }
    final exitedBeforeDisk = !peer.active && entrySettled && leaveSettled;
    final opcodesBeforeDisk = peer.writes.map((p) => p[1]).toList();
    expect(disk.isCompleted, isFalse);
    disk.complete();
    await finish(tester, Future.wait([entry, leaving]));
    controller.dispose();
    await finish(tester, ble.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
    expect(exitedBeforeDisk, isTrue);
    expect(opcodesBeforeDisk, [
      LitchcraftProtocol.enter,
      LitchcraftProtocol.exit,
    ]);
  });

  for (final oldFirmware in [false, true]) {
    testWidgets(
      'advanced brush sync keeps artwork intact and tolerates legacy Busy=$oldFirmware',
      (tester) async {
        final peer = LitchcraftV2FunctionalPeer()
          ..rejectAdvancedTool = oldFirmware;
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        await finish(tester, controller.enter());
        final artwork = encodeLitchcraftAnimationDocument(peer.document).bytes;
        final revision = peer.revision;
        peer.writes.clear();
        await finish(
          tester,
          controller.setBrush(color: 0x123456, eraser: true, targetMask: 5),
        );
        expect(controller.color, 0x123456);
        expect(controller.eraser, isTrue);
        expect(controller.targetMask, 5);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.errorMessage, isNull);
        expect(peer.writes, hasLength(1));
        expect(peer.writes.single.skip(8), [1, 5, 0x12, 0x34, 0x56]);
        expect(peer.brushColor, oldFirmware ? 0xff7430 : 0x123456);
        expect(peer.brushErase, !oldFirmware);
        expect(peer.brushMask, oldFirmware ? 15 : 5);
        expect(peer.revision, revision);
        expect(encodeLitchcraftAnimationDocument(peer.document).bytes, artwork);
        expect(peer.undo, isEmpty);
        await tester.pump(const Duration(seconds: 1));
        expect(
          peer.writes,
          hasLength(1),
          reason: 'legacy Tool Busy does not schedule resync or retry',
        );
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets('physical Circle toggles the live app Paint and Erase control', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(390, 844);
    addTearDown(tester.view.reset);
    final peer = LitchcraftV2FunctionalPeer();
    final ble = LitchcraftV2FunctionalService(peer);
    final controller = LitchcraftController(ble);
    await finish(tester, controller.enter());
    final library = LitchcraftLibrary(controller);
    await finish(tester, library.ready);
    await library.dismissWelcome();
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final brush = find.byKey(
      const ValueKey<String>('craft-quick-brush-semantics'),
    );
    expect(tester.getSemantics(brush).label, 'Paint');

    peer.physicalToggleEraser();
    await tester.pump();
    expect(controller.eraser, isTrue);
    expect(tester.getSemantics(brush).label, 'Erase');
    expect(tester.getSemantics(brush).hint, 'Paint mode');

    peer.physicalToggleEraser();
    await tester.pump();
    expect(controller.eraser, isFalse);
    expect(tester.getSemantics(brush).label, 'Paint');
    expect(tester.getSemantics(brush).hint, 'Erase mode');

    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await finish(tester, ble.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets(
    'square effects update locally before the board ACK without a document reload',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      peer.writes.clear();

      final applyGate = Completer<void>();
      peer.effectReplyGate = applyGate.future;
      var applyDone = false;
      bool? applyAccepted;
      final applying = controller
          .applyAnimationEffect(
            square: 3,
            mask: 5,
            effect: LitchcraftEffectStyle.sparkle,
            effectDurationMs: 2000,
          )
          .then((accepted) {
            applyAccepted = accepted;
            applyDone = true;
          });

      final optimistic = controller.documentSession.document.frames.first;
      expect(optimistic.emitters[3 * 4].effect, LitchcraftEffectStyle.sparkle);
      expect(
        optimistic.emitters[3 * 4 + 2].effect,
        LitchcraftEffectStyle.sparkle,
      );
      expect(optimistic.emitters[3 * 4 + 1].effect, LitchcraftEffectStyle.off);
      for (var i = 0; i < 49 && peer.writes.isEmpty; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(applyDone, isFalse, reason: 'board acknowledgement is withheld');
      expect(peer.writes.map((packet) => packet[2]), [
        LitchcraftV2Protocol.effect,
      ]);
      expect(
        peer.writes.any((packet) => packet[2] == LitchcraftV2Protocol.snapshot),
        isFalse,
      );
      applyGate.complete();
      await finish(tester, applying);
      expect(applyAccepted, isTrue);

      peer.writes.clear();
      final removeGate = Completer<void>();
      peer.effectReplyGate = removeGate.future;
      var removeDone = false;
      bool? removeAccepted;
      final removing = controller
          .applyAnimationEffect(
            square: 3,
            mask: 5,
            effect: LitchcraftEffectStyle.off,
          )
          .then((accepted) {
            removeAccepted = accepted;
            removeDone = true;
          });
      final removed = controller.documentSession.document.frames.first;
      expect(removed.emitters[3 * 4].effect, LitchcraftEffectStyle.off);
      expect(removed.emitters[3 * 4 + 2].effect, LitchcraftEffectStyle.off);
      for (var i = 0; i < 49 && peer.writes.isEmpty; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(removeDone, isFalse, reason: 'board acknowledgement is withheld');
      expect(peer.writes.map((packet) => packet[2]), [
        LitchcraftV2Protocol.effect,
      ]);
      expect(
        peer.writes.any((packet) => packet[2] == LitchcraftV2Protocol.snapshot),
        isFalse,
      );
      removeGate.complete();
      await finish(tester, removing);
      expect(removeAccepted, isTrue);

      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'Play exit cancels draft preparation without waiting on disk or reentering later',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()..active = false;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final disk = Completer<void>();
      final entry = controller.enter(draftReady: disk.future);
      for (var i = 0; i < 49 && !peer.active; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(peer.active, isTrue);
      await finish(tester, controller.leave());
      expect(peer.active, isFalse);
      expect(controller.selected, isFalse);
      expect(disk.isCompleted, isFalse);
      final writes = peer.writes.length;
      disk.complete();
      await finish(tester, entry);
      expect(peer.writes.length, writes);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'first Litchcraft tap admits before disk restoration and preserves the saved draft',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final peer = LitchcraftV2FunctionalPeer()..active = false;
      final ble = LitchcraftV2FunctionalService(peer);
      expect(
        await finish(tester, ble.litchcraftControlAvailable('board')),
        isTrue,
      );
      int? writesAtIntent;
      final trace = _EntryWriteTrace((event, _) {
        if (event == 'litchcraft.entry_start') {
          writesAtIntent ??= peer.sharedWrites.length;
        }
      });
      final controller = LitchcraftController(ble, appTrace: trace);
      final store = _DelayedDraftStore(await SharedPreferences.getInstance());
      final library = LitchcraftLibrary(controller, store: store);
      final saved = LitchcraftDocumentSession(peer.document)
        ..paintSelectedFrame(squares: [9], mask: 15, color: 0x123456);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: LitchMainScaffold(
            bleService: ble,
            boardId: 'board',
            boards: const [],
            onDisconnect: () async {},
            craftController: controller,
            craftLibrary: library,
          ),
        ),
      );
      await tester.pump();
      // Passive startup may already have inspected the board before the tap.
      // Only that read-only request may precede the new admission intent.
      for (final packet in peer.writes.where(
        (packet) => packet[0] == 0x4c || packet[0] == 0x4d,
      )) {
        expect(packet.take(3), [0x4d, 2, LitchcraftV2Protocol.inspect]);
      }
      final writesAtTap = peer.writes.length;
      peer.beforeDomainWork = () =>
          Future<void>.delayed(const Duration(milliseconds: 30));
      await tester.tap(find.byKey(const ValueKey('main-destination-craft')));
      await tester.pump();
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        1,
      );
      for (var i = 0; i < 49; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      final admittedBeforeDisk = peer.active;
      expect(store.pending.isCompleted, isFalse);
      expect(writesAtIntent, isNotNull);
      final beforeDisk = peer.writes
          .skip(writesAtTap)
          .where((p) => p[0] == 0x4c || p[0] == 0x4d)
          .toList();
      for (final command in _sharedCraftCommands(
        peer.sharedWrites.take(writesAtIntent!),
      )) {
        expect(command.take(3), [0x4d, 2, LitchcraftV2Protocol.inspect]);
      }
      final issuedBeforeDisk = _sharedCraftCommands(
        peer.sharedWrites.skip(writesAtIntent!),
      ).toList();
      expect(
        issuedBeforeDisk,
        hasLength(1),
        reason: 'Only Enter may newly reach native after explicit intent',
      );
      expect(issuedBeforeDisk.single.take(2), [0x4c, LitchcraftProtocol.enter]);
      store.pending.complete(
        LitchcraftLibrarySnapshot(
          draft: LitchcraftArtwork.animated(
            id: 'saved-draft',
            name: 'Saved draft',
            document: saved.document,
            createdAt: DateTime.utc(2026),
            modifiedAt: DateTime.utc(2026),
          ),
        ),
      );
      await finish(tester, library.ready);
      await settle(tester, controller);
      expect(
        admittedBeforeDisk,
        isTrue,
        reason:
            'ops-before-disk=${peer.writes.map((packet) => (packet.take(3).toList(), packet.length)).toList()}',
      );
      expect(beforeDisk, hasLength(1));
      expect(beforeDisk.single.take(2), [0x4c, LitchcraftProtocol.enter]);
      expect(controller.status, LitchcraftStatus.conflict);
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          saved.document,
        ),
        isTrue,
      );
      expect(sameLitchcraftDocument(peer.document, saved.document), isFalse);
      await finish(tester, controller.resolveConflict(useBoard: false));
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      expect(
        sameLitchcraftDocument(peer.document, saved.document),
        isTrue,
        reason:
            'ops=${peer.writes.map((packet) => packet.length > 2 ? packet[2] : packet[1]).toList()} staged=${peer.staged.length}',
      );
      await tester.pumpWidget(const SizedBox());
      peer.beforeDomainWork = null;
      library.dispose();
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      await tester.pump(const Duration(seconds: 5));
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    testWidgets(
      '${platform.name} healthy cached-capability Litchcraft tap meets 50ms target',
      (tester) async {
        debugDefaultTargetPlatformOverride = platform;
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final peer = LitchcraftV2FunctionalPeer()..active = false;
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await finish(tester, library.ready);
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: LitchMainScaffold(
              bleService: ble,
              boardId: 'board',
              boards: const [],
              onDisconnect: () async {},
              craftController: controller,
              craftLibrary: library,
            ),
          ),
        );
        await tester.pumpAndSettle();
        peer.writes.clear();
        final capabilityFetches = ble.capabilityFetches;
        final tappedAt = tester.binding.clock.now();
        int? queuedAtMs;
        peer.beforeDomainWork = () {
          queuedAtMs ??= tester.binding.clock
              .now()
              .difference(tappedAt)
              .inMilliseconds;
          return Future<void>.delayed(const Duration(milliseconds: 30));
        };
        final snapshotGate = Completer<void>();
        peer.beforeSnapshot = () => snapshotGate.future;
        await tester.tap(find.byKey(const ValueKey('main-destination-craft')));
        await tester.pump();
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          1,
        );
        var elapsedMs = 0;
        while (!peer.active && elapsedMs < 100) {
          await tester.pump(const Duration(milliseconds: 1));
          elapsedMs++;
        }
        final admittedMs = elapsedMs;
        final firstPacket = peer.writes.first;
        snapshotGate.complete();
        await settle(tester, controller);
        await tester.pumpWidget(const SizedBox());
        peer.beforeDomainWork = null;
        library.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        await tester.pump(const Duration(seconds: 5));
        debugPrint(
          '[LITCHCRAFT_ENTRY_TIMING] platform=${platform.name} GATT_RTT=30ms admission=${admittedMs}ms page=next_frame',
        );
        expect(
          admittedMs,
          lessThan(50),
          reason:
              'healthy controlled target: tap to simulated board authority at the production GATT boundary',
        );
        expect(queuedAtMs, 0, reason: 'Enter starts in the tap event turn');
        expect(
          ble.capabilityFetches,
          capabilityFetches,
          reason: 'known capabilities are reused',
        );
        expect(firstPacket.take(2), [
          0x4c,
          LitchcraftProtocol.enter,
        ], reason: 'mode admission precedes document metadata');
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  for (final platform in [TargetPlatform.android, TargetPlatform.iOS]) {
    for (final adoptingFirst in [false, true]) {
      testWidgets(
        '${platform.name} explicit admission precedes slow artwork preparation and inactive adoption=$adoptingFirst',
        (tester) async {
          debugDefaultTargetPlatformOverride = platform;
          final peer = LitchcraftV2FunctionalPeer()..active = false;
          final ble = LitchcraftV2FunctionalService(peer);
          expect(
            await finish(tester, ble.litchcraftControlAvailable('board')),
            isTrue,
          );
          final artworkGate = Completer<void>();
          bool? activeWhenArtworkSizeRequested;
          ble.beforeArtworkPacketSize = () {
            activeWhenArtworkSizeRequested = peer.active;
            return artworkGate.future;
          };
          final controller = LitchcraftController(ble);
          Future<bool>? adoption;
          if (adoptingFirst) {
            adoption = controller.adoptActiveBoard();
            await tester.pump(const Duration(milliseconds: 50));
          }
          final entry = controller.enter();
          await tester.pump(const Duration(milliseconds: 49));
          final admittedWithin50ms = peer.active;
          // The connected shell already owns an activated shared session.
          // Hold the later artwork preparation across the reported seven seconds.
          await tester.pump(const Duration(milliseconds: 6951));
          artworkGate.complete();
          await finish(tester, entry);
          if (adoption != null) await adoption;
          expect(
            admittedWithin50ms,
            isTrue,
            reason:
                'the connected board must accept Enter before artwork preparation',
          );
          expect(activeWhenArtworkSizeRequested, isTrue);
          expect(
            peer.writes.where(
              (p) => p[0] == 0x4c && p[1] == LitchcraftProtocol.enter,
            ),
            hasLength(1),
          );
          expect(controller.status, LitchcraftStatus.live);
          debugPrint(
            '[LITCHCRAFT_ENTRY_TIMING] platform=${platform.name} inactive_adoption=$adoptingFirst artwork_preparation_delay=7000ms admitted_by=49ms Enter_count=1',
          );
          controller.dispose();
          await finish(tester, ble.dispose());
          await peer.close();
          debugDefaultTargetPlatformOverride = null;
        },
      );
    }
  }

  for (final frameCount in [1, 8]) {
    testWidgets(
      '$frameCount-frame artwork streams without per-chunk RTT and previews immediately',
      (tester) async {
        final peer = LitchcraftV2FunctionalPeer();
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        await finish(tester, controller.enter());
        final library = LitchcraftLibrary(controller);
        await finish(tester, library.ready);
        final edited = LitchcraftDocumentSession(
          staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint([0], 15, 0x13579b),
          ),
        );
        while (edited.document.frames.length < frameCount) {
          edited.addFrame();
        }
        final before = peer.writes.length;
        peer.beforeDomainReceipt = (_) =>
            Future<void>.delayed(const Duration(milliseconds: 30));
        final start = tester.binding.clock.now();
        final delivery = library.open(
          LitchcraftArtwork.animated(
            id: 'timed-art',
            name: 'Timed art',
            document: edited.document,
            createdAt: DateTime.utc(2026),
            modifiedAt: DateTime.utc(2026),
          ),
        );
        await tester.pump();
        expect(
          controller.documentSession.document.frames,
          hasLength(frameCount),
        );
        await finish(tester, delivery);
        final elapsed = tester.binding.clock.now().difference(start);
        expect(sameLitchcraftDocument(peer.document, edited.document), isTrue);
        expect(
          elapsed,
          lessThan(const Duration(milliseconds: 150)),
          reason:
              'two fixed admission/commit RTTs, never a round trip for each of 54 chunks',
        );
        final packets = peer.writes
            .skip(before)
            .where((p) => p[0] == 0x4d)
            .toList();
        expect(packets.where((p) => p[2] == 10), isEmpty);
        expect(
          packets.where((p) => p[2] == 15),
          hasLength(frameCount == 8 ? 54 : 7),
        );
        expect(packets.where((p) => p[2] == 16), hasLength(1));
        debugPrint(
          '[LITCHCRAFT_LIBRARY_OPEN_TIMING] frames=$frameCount RTT=30ms elapsed=${elapsed.inMilliseconds}ms chunks=${frameCount == 8 ? 54 : 7} chunk_ACKs=0 protocol_RTTs=3_including_once_per_link_probe preview=immediate',
        );
        library.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'old inactive V2 storage uses metadata probe before unknown mutation',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()
        ..active = false
        ..supportsBulkDocument = false;
      final ble = LitchcraftV2FunctionalService(peer);
      var mtu = 20;
      await finish(
        tester,
        ble
            .litchcraftPacketPayloadBytes(
              'board',
              connectionSessionToken: ble.connectionSessionToken,
            )
            .then((value) {
              mtu = value;
            }),
      );
      final link = LitchcraftLink(
        packets: peer.packets.stream,
        write: (bytes) => ble.writeLitchcraftV2Packet(
          'board',
          bytes,
          connectionSessionToken: ble.connectionSessionToken,
        ),
        writeBulkDocumentPacket: (bytes) =>
            ble.writeLitchcraftBulkDocumentPacket(
              'board',
              bytes,
              connectionSessionToken: ble.connectionSessionToken,
            ),
        onPacket: (_) {},
        onFailure: (_) {},
      );
      final client = LitchcraftV2DocumentClient(link);
      final encoded = encodeLitchcraftAnimationDocument(peer.document);
      await finish(
        tester,
        client
            .replace(
              session: 0,
              baseRevision: peer.revision,
              document: encoded.bytes,
              crc32: encoded.crc32,
              inactiveStorage: true,
              maximumPacketBytes: mtu,
            )
            .then((_) {}),
      );
      expect(peer.active, isFalse);
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.streamBegin),
        isEmpty,
      );
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.replaceCommitOp),
        hasLength(1),
      );
      await tester.runAsync(() async {
        await link.dispose();
        await ble.dispose();
        await peer.close();
      });
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'eight-frame board comparison is one high-MTU snapshot and one ordering barrier',
    (tester) async {
      final initial = LitchcraftDocumentSession(
        staticLitchcraftDocument(LitchcraftCanvas.blank()),
      );
      for (var i = 1; i < 8; i++) {
        initial.addFrame();
      }
      final peer = LitchcraftV2FunctionalPeer()..document = initial.document;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final written = peer.writes.length,
          notified = peer.notificationEvents.length;
      peer.beforeDomainWork = () =>
          Future<void>.delayed(const Duration(milliseconds: 30));
      final start = tester.binding.clock.now();
      peer.physicalPaint(0, 0x123456);
      await settle(tester, controller);
      expect(
        tester.binding.clock.now().difference(start).inMilliseconds,
        lessThanOrEqualTo(65),
      );
      expect(peer.writes.skip(written).map((p) => p[2]), [
        LitchcraftV2Protocol.snapshot,
        LitchcraftV2Protocol.inspect,
      ]);
      expect(
        peer.notificationEvents
            .skip(notified)
            .where((e) => e == LitchcraftV2Protocol.snapshotChunk),
        hasLength((12336 / 229).ceil()),
      );
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          peer.document,
        ),
        isTrue,
      );
      final count = peer.writes.length;
      final snapshotChunks = peer.notificationEvents
          .where((event) => event == LitchcraftV2Protocol.snapshotChunk)
          .length;
      await finish(tester, controller.coherentDocument());
      expect(
        peer.writes.length,
        count + 1,
        reason:
            'Comparing an already coherent document uses one ordering barrier.',
      );
      expect(peer.writes.last[2], LitchcraftV2Protocol.inspect);
      expect(
        peer.notificationEvents
            .where((event) => event == LitchcraftV2Protocol.snapshotChunk)
            .length,
        snapshotChunks,
        reason: 'A coherent comparison must not pull document bytes again.',
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final style in [
    LitchcraftEffectStyle.breathe,
    LitchcraftEffectStyle.rainbow,
  ]) {
    testWidgets(
      'paused ${style.name} mirrors board effect phase without polling',
      (tester) async {
        final document =
            LitchcraftDocumentSession(
              staticLitchcraftDocument(
                LitchcraftCanvas.blank().paint([0], 15, 0xff8040),
              ),
            )..applyEffect(
              square: 0,
              mask: 15,
              effect: style,
              effectDurationMs: 2000,
            );
        final peer = LitchcraftV2FunctionalPeer()
          ..document = document.document
          ..effectElapsedMs = 750;
        final ble = LitchcraftV2FunctionalService(peer);
        var now = 0;
        final controller = LitchcraftController(
          ble,
          monotonicMilliseconds: () => now,
        );
        await finish(tester, controller.enter());
        expect(controller.animationPlaying, isFalse);
        expect(controller.animationEffectElapsedMilliseconds, 750);
        final library = LitchcraftLibrary(controller);
        await finish(tester, library.ready);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        final color = tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors
            .first;
        final expected = composeLitchcraftAnimationDocument(
          peer.document,
          0,
          effectElapsedMs: controller.animationEffectElapsedMilliseconds!,
        ).colors.first;
        expect(color, expected);
        final count = peer.writes.length;
        now = 500;
        await tester.pump(const Duration(seconds: 2));
        expect(controller.animationEffectElapsedMilliseconds, 1250);
        expect(
          tester
              .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
              .canvas
              .colors
              .first,
          composeLitchcraftAnimationDocument(
            peer.document,
            0,
            effectElapsedMs: 1250,
          ).colors.first,
        );
        expect(
          peer.writes.length,
          count,
          reason:
              'Effect playback extrapolates the accepted board clock locally, without status polling.',
        );
        expect(
          peer.writes.where(
            (p) =>
                p[2] == LitchcraftV2Protocol.inspect &&
                p.length == 10 &&
                p[9] == 1,
          ),
          hasLength(1),
        );
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'old canvas peer caches rotation refusal and retains document fallback',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()
        ..supportsCanvasRotate = false
        ..supportsBulkDocument = false;
      peer.paint(0, 0x123456);
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      await finish(tester, controller.rotateArtwork(1));
      await finish(tester, controller.rotateArtwork(-1));
      expect(
        peer.writes.where(
          (p) => p[2] == LitchcraftV2Protocol.canvasEdit && p.length == 10,
        ),
        hasLength(2),
      );
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.replaceCommitOp),
        hasLength(2),
      );
      expect(
        sameLitchcraftDocument(
          peer.document,
          controller.documentSession.document,
        ),
        isTrue,
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'eight-frame rotation previews immediately and takes one acknowledged command',
    (tester) async {
      final initial = LitchcraftDocumentSession(
        staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 1, 0x123456),
        ),
      );
      for (var frame = 1; frame < 8; frame++) {
        initial.addFrame();
      }
      final peer = LitchcraftV2FunctionalPeer()
        ..document = initial.document
        ..selectedFrame = 3;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final expected = LitchcraftDocumentSession(peer.document)
        ..selectFrame(3)
        ..rotateSelectedFrame(1);
      final library = LitchcraftLibrary(controller);
      await finish(tester, library.ready);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      final written = peer.writes.length;
      peer.beforeDomainWork = () =>
          Future<void>.delayed(const Duration(milliseconds: 30));
      await tester.tap(find.byKey(const ValueKey('craft-rotate-artwork')));
      await tester.pump();
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          expected.document,
        ),
        isTrue,
        reason: 'Rotation preview belongs to the gesture before native I/O.',
      );
      final canvas = tester.widget<LitchcraftCanvasView>(
        find.byType(LitchcraftCanvasView),
      );
      expect(
        canvas.enabled,
        isTrue,
        reason: 'Pending rotation cannot disable painting.',
      );
      final fill = tester.widget<LitchcraftHoldAction>(
        find
            .ancestor(
              of: find.byKey(const ValueKey('craft-quick-fill')),
              matching: find.byType(LitchcraftHoldAction),
            )
            .first,
      );
      expect(
        fill.onTap,
        isNotNull,
        reason: 'Unrelated ordinary controls remain interactive.',
      );
      await tester.pump(const Duration(milliseconds: 60));
      expect(
        controller.pending,
        isFalse,
        reason:
            'One simulated30ms transport round trip fits within60ms; no chunk train or debounce.',
      );
      expect(
        peer.writes.skip(written).map((p) => p[2]),
        [LitchcraftV2Protocol.canvasEdit],
        reason:
            'Rotating one frame must not transfer all eight or reread the document.',
      );
      expect(sameLitchcraftDocument(peer.document, expected.document), isTrue);
      expect(peer.undo, hasLength(1));
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final erase in [false, true]) {
    testWidgets(
      'eight-frame ${erase ? 'Erase All hold' : 'Fill tap'} previews on first frame and uses one atomic command',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final initial = LitchcraftDocumentSession(
          staticLitchcraftDocument(
            LitchcraftCanvas(colors: List.filled(256, 0x112233)),
          ),
        );
        for (var frame = 1; frame < 8; frame++) {
          initial.addFrame();
        }
        final peer = LitchcraftV2FunctionalPeer()
          // Controlled old-peer fault proves the timing test rejects the
          // former whole-document transfer path, not merely wrong final RGB.
          ..supportsCanvasEdit = !const bool.fromEnvironment(
            'LITCHCRAFT_TEST_DISABLE_ATOMIC',
          )
          ..document = initial.document
          ..selectedFrame = 3;
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await finish(tester, library.ready);
        await finish(tester, controller.enter());
        await finish(
          tester,
          controller.setBrush(color: 0x345678, targetMask: 5),
        );
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        await settle(tester, controller);
        expect(controller.status, LitchcraftStatus.live);
        final nativeGate = Completer<void>(), ackGate = Completer<void>();
        peer.beforeDomainWork = () => nativeGate.future;
        peer.canvasEditReplyGate = ackGate.future;
        final count = peer.writes.length;
        final action = find.byKey(const ValueKey('craft-quick-fill'));
        if (erase) {
          await tester.longPress(action);
        } else {
          await tester.tap(action);
        }
        await tester.pump();
        final expected = List.generate(
          256,
          (index) => erase ? 0 : (index.isEven ? 0x345678 : 0x112233),
        );
        expect(
          controller.canvas.colors,
          expected,
          reason: 'controller immediately owns the whole-frame preview',
        );
        expect(
          tester
              .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
              .canvas
              .colors,
          expected,
          reason: 'first rendered app frame precedes any native write or ACK',
        );
        expect(find.byType(CircularProgressIndicator), findsNothing);
        expect(peer.writes.length, count);
        expect(peer.document.frames[3].emitters.first.color, 0x112233);
        nativeGate.complete();
        for (var turn = 0; turn < 20 && peer.writes.length == count; turn++) {
          await tester.pump(const Duration(milliseconds: 1));
        }
        expect(peer.writes.length, count + 1);
        expect(peer.writes.last.length, 19);
        expect(peer.writes.last[2], LitchcraftV2Protocol.canvasEdit);
        expect(
          peer.document.frames[3].emitters.map((e) => e.color),
          expected,
          reason: 'board authority changes in one request before delayed ACK',
        );
        expect(controller.pending, isTrue);
        expect(find.byType(CircularProgressIndicator), findsNothing);
        ackGate.complete();
        await settle(tester, controller);
        expect(controller.status, LitchcraftStatus.live);
        expect(
          peer.writes.length,
          count + 1,
          reason: 'no per-cell writes, transfer, or snapshot reread',
        );
        expect(peer.undo, hasLength(1));
        expect(peer.document.frames[2].emitters.first.color, 0x112233);
        expect(
          sameLitchcraftDocument(
            peer.document,
            controller.documentSession.document,
          ),
          isTrue,
        );
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  for (final dark in [false, true]) {
    for (final erase in [false, true]) {
      testWidgets(
        'checker ${dark ? 'dark' : 'light'} ${erase ? 'erase' : 'fill'} previews immediately and uses one minimum-ATT command',
        (tester) async {
          final peer = LitchcraftV2FunctionalPeer()
            ..document = staticLitchcraftDocument(
              LitchcraftCanvas(colors: List<int>.filled(256, 0x112233)),
            );
          final ble = LitchcraftV2FunctionalService(peer);
          final controller = LitchcraftController(ble);
          await finish(tester, controller.enter());
          await finish(
            tester,
            controller.setBrush(color: 0x345678, targetMask: 5),
          );
          if (erase) await finish(tester, controller.setBrush(eraser: true));
          peer.writes.clear();
          final ackGate = Completer<void>();
          peer.canvasEditReplyGate = ackGate.future;
          final operation = controller.fillChecker(dark: dark);
          await tester.pump();
          for (var square = 0; square < 64; square++) {
            final selected = ((square % 8) + (square ~/ 8)).isOdd == dark;
            expect(
              controller.canvas.colors.sublist(square * 4, square * 4 + 4),
              selected
                  ? <int>[
                      erase ? 0 : 0x345678,
                      0x112233,
                      erase ? 0 : 0x345678,
                      0x112233,
                    ]
                  : everyElement(0x112233),
            );
          }
          final commands = peer.writes.where(
            (packet) =>
                packet[2] == LitchcraftV2Protocol.canvasEdit &&
                packet.length == 19,
          );
          expect(commands, hasLength(1));
          expect(commands.single[18], 5 + (dark ? 1 : 0) + (erase ? 2 : 0));
          if (erase) expect(commands.single.sublist(15, 18), <int>[0, 0, 0]);
          expect(
            peer.writes.where(
              (packet) => packet[2] == LitchcraftV2Protocol.replaceCommitOp,
            ),
            isEmpty,
          );
          ackGate.complete();
          await finish(tester, operation);
          expect(
            sameLitchcraftDocument(
              peer.document,
              controller.documentSession.document,
            ),
            isTrue,
          );
          controller.dispose();
          await finish(tester, ble.dispose());
          await peer.close();
          debugDefaultTargetPlatformOverride = null;
        },
      );
    }
  }

  testWidgets(
    'older canvas peer rejects one pattern probe and retains document fallback',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()
        ..supportsCanvasPattern = false
        ..supportsBulkDocument = false;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      await finish(tester, controller.fillChecker(dark: false));
      expect(
        peer.writes.where(
          (packet) =>
              packet[2] == LitchcraftV2Protocol.canvasEdit &&
              packet.length == 10 &&
              packet[9] == 3,
        ),
        hasLength(1),
      );
      expect(
        peer.writes.where(
          (packet) => packet[2] == LitchcraftV2Protocol.replaceCommitOp,
        ),
        hasLength(1),
      );
      expect(
        sameLitchcraftDocument(
          peer.document,
          controller.documentSession.document,
        ),
        isTrue,
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'old V2 peer retains whole-document fallback after one cached Unsupported probe',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer()
        ..supportsCanvasEdit = false
        ..supportsBulkDocument = false;
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      await finish(tester, controller.fillAll());
      await finish(tester, controller.clear());
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.canvasEdit),
        hasLength(2),
      );
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.replaceCommitOp),
        hasLength(2),
      );
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.streamBegin),
        isEmpty,
      );
      expect(
        sameLitchcraftDocument(
          peer.document,
          controller.documentSession.document,
        ),
        isTrue,
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'atomic Fill Busy refusal retires preview and never replays on lift',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final before = peer.document;
      peer.physicalBrushHeld = true;
      await finish(tester, controller.fillAll());
      expect(controller.waitingForLift, isTrue);
      expect(controller.status, LitchcraftStatus.live);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, before),
        isTrue,
      );
      expect(peer.undo, isEmpty);
      final writes = peer.writes.length;
      peer.physicalBrushHeld = false;
      await tester.pump(const Duration(seconds: 1));
      expect(peer.writes.length, writes);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'held swatch selects Paint once and fills before ACK with one command and one Undo',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      await finish(tester, controller.setBrush(color: 0x123456, eraser: true));
      final gate = Completer<void>();
      peer.canvasEditReplyGate = gate.future;
      final count = peer.writes.length;
      final work = controller.fillAllWithColor(0xff1a00);
      await tester.pump();
      expect(controller.color, 0xff1a00);
      expect(controller.previousColor, 0x123456);
      expect(controller.eraser, isFalse);
      expect(
        controller.canvas.colors.every((color) => color == 0xff1a00),
        isTrue,
      );
      for (var turn = 0; turn < 20 && peer.writes.length == count; turn++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(peer.writes.length, count + 1);
      expect(peer.writes.last[2], LitchcraftV2Protocol.canvasEdit);
      expect(peer.undo, hasLength(1));
      gate.complete();
      await finish(tester, work);
      expect(controller.previousColor, 0x123456);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'lost atomic Fill ACK retries exact request without duplicate mutation or snapshot',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final count = peer.writes.length, revision = peer.revision;
      peer.deliverNotifications = false;
      final work = controller.fillAll();
      for (var turn = 0; turn < 20 && peer.writes.length == count; turn++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(peer.revision, revision + 1);
      expect(controller.pending, isTrue);
      final request = List<int>.of(peer.writes.last);
      peer.deliverNotifications = true;
      await tester.pump(const Duration(milliseconds: 260));
      await finish(tester, work);
      expect(peer.writes.length, count + 2);
      expect(peer.writes.last, request);
      expect(peer.revision, revision + 1);
      expect(peer.undo, hasLength(1));
      expect(controller.status, LitchcraftStatus.live);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'queued atomic fills preserve the newest preview and each accepted Undo boundary',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final gate = Completer<void>();
      peer.canvasEditReplyGate = gate.future;
      final first = controller.fillAllWithColor(0xff0000);
      await tester.pump();
      final second = controller.fillAllWithColor(0x00ff00);
      await tester.pump();
      expect(
        controller.canvas.colors.every((color) => color == 0x00ff00),
        isTrue,
      );
      gate.complete();
      await finish(tester, Future.wait([first, second]));
      expect(controller.status, LitchcraftStatus.live);
      expect(
        peer.document.frames[0].emitters.every((e) => e.color == 0x00ff00),
        isTrue,
      );
      expect(peer.undo, hasLength(2));
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'uncertain atomic Fill recovers by snapshot equality on the same shared session',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final revision = peer.revision;
      final subscriptions = peer.subscriptions;
      peer.deliverNotifications = false;
      unawaited(controller.fillAll());
      for (var turn = 0; turn < 20; turn++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      for (
        var turn = 0;
        turn < 100 && controller.status == LitchcraftStatus.live;
        turn++
      ) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      expect(controller.status, isNot(LitchcraftStatus.live));
      peer.deliverNotifications = true;
      final recoveryStart = peer.writes.length;
      final recovery = controller.retryConnection();
      // A canceled stream's native future resolves on the real event loop;
      // fake clock pumps alone cannot complete this cleanup barrier.
      await tester.runAsync(() async {
        await Future<void>.delayed(Duration.zero);
      });
      await finish(tester, recovery);
      expect(controller.status, LitchcraftStatus.live);
      expect(peer.subscriptions, subscriptions);
      expect(
        peer.writes
            .skip(recoveryStart)
            .where(
              (p) => p[0] == 0x4d && p[2] == LitchcraftV2Protocol.snapshot,
            ),
        isNotEmpty,
      );
      expect(peer.revision, revision + 1);
      expect(peer.undo, hasLength(1));
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.replaceBeginOp),
        isEmpty,
      );
      expect(
        sameLitchcraftDocument(
          peer.document,
          controller.documentSession.document,
        ),
        isTrue,
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final variation in [
    'unchanged',
    'palette changed',
    'late library restore',
  ]) {
    testWidgets(
      'background notification gap preserves Mac-expanded eight frames from coherent one-frame ancestor: $variation',
      (tester) async {
        debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
        final peer = LitchcraftV2FunctionalPeer()
          ..document = staticLitchcraftDocument(LitchcraftCanvas.blank());
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        await finish(tester, controller.enter());
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.documentSession.document.frames, hasLength(1));
        controller.didChangeAppLifecycleState(AppLifecycleState.paused);
        peer.deliverNotifications = false;
        final source = controller.documentSession.document;
        for (var frame = 1; frame < 8; frame++) {
          peer.physicalAdd();
        }
        peer.physicalPaint(3, 0xabcdef);
        final exactBoard = encodeLitchcraftAnimationDocument(
          peer.document,
        ).bytes;
        final boardRevision = peer.revision;
        LitchcraftLibrary? restoredLibrary;
        if (variation == 'palette changed') {
          controller.setFavoriteColors([
            0x123456,
            ...source.favoriteColors.skip(1),
          ]);
        } else if (variation == 'late library restore') {
          final store = LitchcraftArtworkStore(
            await SharedPreferences.getInstance(),
          );
          final draft = LitchcraftArtwork.animated(
            id: 'retained',
            name: 'Retained',
            document: source,
            createdAt: DateTime.utc(2026),
            modifiedAt: DateTime.utc(2026),
          );
          await finish(
            tester,
            store.save(LitchcraftLibrarySnapshot(draft: draft)),
          );
          restoredLibrary = LitchcraftLibrary(controller, store: store);
          await finish(tester, restoredLibrary.ready);
        }
        final written = peer.writes.length;
        final snapshotGate = Completer<void>();
        peer.beforeSnapshot = () => snapshotGate.future;
        peer.deliverNotifications = true;
        controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
        for (var i = 0; i < 30; i++) {
          await tester.pump(const Duration(milliseconds: 1));
        }
        // Timeline for a frame the stale app cannot yet address arrives while
        // the full document is downloading, as with physical/Mac frame edits.
        peer.physicalSelect(7, elapsed: 7250);
        snapshotGate.complete();
        for (var i = 0; i < 67; i++) {
          await tester.pump(const Duration(seconds: 1));
        }
        expect(
          controller.status,
          variation == 'palette changed'
              ? LitchcraftStatus.conflict
              : LitchcraftStatus.live,
          reason:
              '${controller.errorMessage}; ops=${peer.writes.skip(written).map((p) => p.take(9).toList()).toList()}',
        );
        expect(peer.document.frames, hasLength(8));
        expect(peer.revision, boardRevision);
        expect(
          encodeLitchcraftAnimationDocument(peer.document).bytes,
          exactBoard,
        );
        expect(peer.active, isTrue);
        expect(
          peer.writes
              .skip(written)
              .where(
                (p) =>
                    p[0] == 0x4d &&
                        p[2] == LitchcraftV2Protocol.replaceBeginOp ||
                    p[0] == 0x4c && p[1] == LitchcraftProtocol.exit,
              ),
          isEmpty,
        );
        if (variation != 'palette changed') {
          expect(controller.documentSession.document.frames, hasLength(8));
          expect(controller.animationSelectedFrameIndex, 7);
        }
        await tester.pump(const Duration(seconds: 67));
        expect(
          encodeLitchcraftAnimationDocument(peer.document).bytes,
          exactBoard,
        );
        restoredLibrary?.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'cold shell with stale persisted draft preserves live eight-frame board through background and resume',
    (tester) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final peer = LitchcraftV2FunctionalPeer();
      for (var i = 2; i < 8; i++) {
        peer.physicalAdd();
      }
      peer.physicalPaint(2, 0xabcdef);
      peer.revision = 19;
      final before = encodeLitchcraftAnimationDocument(peer.document);
      final ble = LitchcraftV2FunctionalService(peer);
      final store = LitchcraftArtworkStore(
        await SharedPreferences.getInstance(),
      );
      final draft = LitchcraftArtwork(
        id: 'stale',
        name: 'Stale',
        canvas: LitchcraftCanvas.blank().paint([4], 15, 0x123456),
        createdAt: DateTime.utc(2026),
        modifiedAt: DateTime.utc(2026),
      );
      await finish(tester, store.save(LitchcraftLibrarySnapshot(draft: draft)));
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller, store: store);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: LitchMainScaffold(
            bleService: ble,
            boardId: 'board',
            boards: const [],
            onDisconnect: () async {},
            craftController: controller,
            craftLibrary: library,
          ),
        ),
      );
      await finish(tester, library.ready);
      for (var i = 0; i < 500; i++) {
        await tester.pump(const Duration(milliseconds: 10));
      }
      expect(
        controller.status,
        LitchcraftStatus.conflict,
        reason: controller.errorMessage,
      );
      controller.didChangeAppLifecycleState(AppLifecycleState.paused);
      await finish(tester, library.flush());
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await settle(tester, controller);
      expect(peer.active, isTrue);
      expect(peer.revision, 19);
      expect(
        encodeLitchcraftAnimationDocument(peer.document).bytes,
        before.bytes,
      );
      expect(
        peer.writes.where(
          (p) =>
              p[0] == 0x4d && p[2] == LitchcraftV2Protocol.replaceBeginOp ||
              p[0] == 0x4c && p[1] == LitchcraftProtocol.exit,
        ),
        isEmpty,
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      final disposing = ble.dispose();
      await finish(tester, disposing);
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'initial page callback during active-board adoption cannot issue Exit',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await finish(tester, library.ready);
      final snapshotGate = Completer<void>();
      peer.beforeSnapshot = () => snapshotGate.future;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: LitchMainScaffold(
            bleService: ble,
            boardId: 'board',
            boards: const [],
            onDisconnect: () async {},
            craftController: controller,
            craftLibrary: library,
          ),
        ),
      );
      for (var i = 0; i < 30; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(controller.boardActive, isTrue);
      // Layout/programmatic positioning cannot request a mode change.
      expect(
        tester
            .widget<PageView>(find.byKey(const ValueKey('main-pages')))
            .onPageChanged,
        isNull,
      );
      snapshotGate.complete();
      for (var i = 0; i < 100; i++) {
        await tester.pump(const Duration(milliseconds: 10));
      }
      expect(
        peer.writes.where(
          (p) => p[0] == 0x4c && p[1] == LitchcraftProtocol.exit,
        ),
        isEmpty,
      );
      expect(peer.active, isTrue);
      expect(controller.status, LitchcraftStatus.live);
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        1,
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      final disposing = ble.dispose();
      await finish(tester, disposing);
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'clean-draft reselection during Exit ACK retains newer Litchcraft intent',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await finish(tester, library.ready);
      await finish(tester, controller.enter());
      await finish(tester, library.save('Clean canvas'));
      expect(library.dirty, isFalse);
      final before = encodeLitchcraftAnimationDocument(peer.document).bytes;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: LitchMainScaffold(
            bleService: ble,
            boardId: 'board',
            boards: const [],
            onDisconnect: () async {},
            craftController: controller,
            craftLibrary: library,
            initialPageIndex: 0,
            initialCraftModeResolved: true,
          ),
        ),
      );
      await tester.pumpAndSettle();
      peer.writes.clear();
      final exitGate = Completer<void>();
      var exitPending = false;
      peer.beforeExit = () {
        exitPending = true;
        return exitGate.future;
      };
      final pager = find.byKey(const ValueKey('main-pages'));
      Future<void> select(String label) async {
        await tester.tap(
          find.descendant(
            of: find.byType(NavigationBar),
            matching: find.byWidgetPredicate(
              (w) => w is NavigationDestination && w.label == label,
            ),
          ),
        );
        await tester.pump(const Duration(milliseconds: 50));
      }

      await select('Play');
      expect(exitPending, isTrue);
      expect(tester.widget<PageView>(pager).controller!.page, closeTo(0, .01));
      expect(find.byKey(const ValueKey('craft-leave-unsaved')), findsNothing);
      await select('Litchcraft');
      expect(tester.widget<PageView>(pager).controller!.page, closeTo(1, .01));
      exitGate.complete();
      await tester.pumpAndSettle();
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        1,
      );
      expect(tester.widget<PageView>(pager).controller!.page, 1);
      expect(controller.selected, isTrue);
      expect(controller.status, LitchcraftStatus.live);
      expect(peer.active, isTrue);
      expect(encodeLitchcraftAnimationDocument(peer.document).bytes, before);
      expect(
        peer.writes.where(
          (p) => p[0] == 0x4c && p[1] == LitchcraftProtocol.exit,
        ),
        hasLength(1),
      );
      expect(
        peer.writes.where(
          (p) => p[0] == 0x4c && p[1] == LitchcraftProtocol.enter,
        ),
        hasLength(1),
      );
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'healthy resume and physical exit reentry retain one Timeline observer without polling',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      final epoch = ble.connectionSessionToken;
      final subscriptions = peer.subscriptions;
      controller.didChangeAppLifecycleState(AppLifecycleState.paused);
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await settle(tester, controller);
      final afterResume = peer.writes.length;
      peer.physicalSelect(1, elapsed: 1375);
      await tester.pump();
      expect(controller.animationSelectedFrameIndex, 1);
      expect(controller.animationElapsedMilliseconds, 1375);
      await tester.pump(const Duration(seconds: 10));
      expect(peer.writes.length, afterResume);
      peer.revision++;
      peer.physicalExit();
      await tester.pump();
      expect(controller.status, LitchcraftStatus.offline);
      expect(controller.selected, isFalse);
      expect(
        peer.writes.length,
        afterResume,
        reason: 'inactive watermark does not fetch or replace',
      );
      await finish(tester, controller.enter());
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      final afterReentry = peer.writes.length;
      peer.physicalSelect(0, elapsed: 225);
      await tester.pump();
      expect(controller.animationSelectedFrameIndex, 0);
      expect(controller.animationElapsedMilliseconds, 225);
      peer.physicalSelect(1, elapsed: 1450);
      await tester.pump();
      expect(controller.animationSelectedFrameIndex, 1);
      expect(controller.animationElapsedMilliseconds, 1450);
      await tester.pump(const Duration(seconds: 10));
      expect(peer.writes.length, afterReentry);
      expect(peer.subscriptions, subscriptions);
      expect(subscriptions, 1);
      expect(ble.connectionSessionToken, epoch);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets(
    'physical Add buffers new frame Timeline until matching Snapshot and consumes it once',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      peer.physicalSelect(1);
      await tester.pump();
      final releaseSnapshot = Completer<void>();
      peer.beforeDomainWork = () => releaseSnapshot.future;
      peer.physicalAdd();
      await tester.pump();
      expect(controller.documentSession.document.frames, hasLength(2));
      expect(controller.animationSelectedFrameIndex, 1);
      releaseSnapshot.complete();
      await settle(tester, controller);
      peer.beforeDomainWork = null;
      expect(controller.documentSession.document.frames, hasLength(3));
      expect(controller.animationSelectedFrameIndex, 2);
      expect(controller.documentSession.selectedFrameIndex, 2);
      expect(controller.animationElapsedMilliseconds, 2000);
      expect(controller.animationPlaying, isFalse);
      expect(controller.canUndo, isTrue);
      expect(controller.canRedo, isFalse);
      expect(controller.documentSession.canUndo, isFalse);
      peer.physicalSelect(0, elapsed: 350);
      await tester.pump();
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await settle(tester, controller);
      expect(controller.animationSelectedFrameIndex, 0);
      expect(controller.animationElapsedMilliseconds, 350);
      await finish(tester, controller.undo());
      expect(controller.documentSession.document.frames, hasLength(2));
      expect(controller.canRedo, isTrue);
      expect(
        peer.writes.where((p) => p[2] == LitchcraftV2Protocol.replaceBeginOp),
        isEmpty,
      );
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final select in [true, false]) {
    testWidgets(
      'physical-held Busy on ${select ? 'Select' : 'Play'} stays live and only manual retry clears localized recovery',
      (tester) async {
        final semantics = tester.ensureSemantics();
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final peer = LitchcraftV2FunctionalPeer();
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await finish(tester, library.ready);
        await finish(tester, controller.enter());
        await tester.pumpWidget(
          MaterialApp(
            locale: const Locale('de'),
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        final animate = find.byKey(const ValueKey('craft-animate'));
        await tester.ensureVisible(animate);
        await tester.tap(animate);
        await tester.pumpAndSettle();
        final action = find.byKey(
          ValueKey(select ? 'craft-select-frame-1' : 'craft-preview'),
        );
        final before = peer.writes.length;
        final epoch = ble.connectionSessionToken;
        peer.physicalBrushHeld = true;
        await tester.tap(action);
        await settle(tester, controller);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.waitingForLift, isTrue);
        final strings = AppLocalizations.of(
          tester.element(find.byType(LitchcraftScreen)),
        );
        final motion = find.byKey(const ValueKey('craft-motion'));
        await tester.tap(motion);
        await tester.pump(const Duration(milliseconds: 300));
        await tester.pump(const Duration(milliseconds: 250));
        expect(
          tester
              .widget<LitchcraftEffectEditorPanel>(
                find.byType(LitchcraftEffectEditorPanel),
              )
              .enabled,
          isFalse,
          reason:
              'The Effects section stays navigable; its edits are disabled while a physical brush is held.',
        );
        expect(
          tester.getSemantics(motion).label,
          isNot(contains(strings.craftLiveMotionEditDisabled)),
        );
        expect(find.text(strings.craftBoardBusyRecovery), findsOneWidget);
        await tester.tap(animate);
        await tester.pump(const Duration(milliseconds: 300));
        expect(peer.writes.length, before + 1);
        peer.physicalBrushHeld = false;
        await tester.pump(const Duration(seconds: 10));
        expect(
          peer.writes.length,
          before + 1,
          reason: 'lift never replays the rejected command',
        );
        await tester.tap(action);
        await settle(tester, controller);
        expect(peer.writes.length, before + 2);
        expect(controller.waitingForLift, isFalse);
        expect(find.text(strings.craftBoardBusyRecovery), findsNothing);
        expect(controller.errorMessage, isNull);
        expect(
          select
              ? controller.animationSelectedFrameIndex == 1
              : controller.animationPlaying,
          isTrue,
        );
        expect(ble.connectionSessionToken, epoch);
        expect(peer.subscriptions, 1);
        await tester.pumpWidget(const SizedBox());
        semantics.dispose();
        library.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'Paint Busy ACK after pointer-up discards rejected preview and queued stroke without reconnect or replay',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await finish(tester, library.ready);
      await finish(tester, controller.enter());
      await tester.pumpWidget(
        MaterialApp(
          locale: const Locale('de'),
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      final before = peer.document;
      final epoch = ble.connectionSessionToken;
      final written = peer.writes.length;
      final reply = Completer<void>();
      peer.physicalBrushHeld = true;
      peer.paintReplyGate = reply.future;
      final rect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      final point = rect.topLeft + Offset(rect.width / 16, rect.height / 16);
      final finger = await tester.startGesture(point);
      await finger.moveBy(Offset(rect.width / 8, 0));
      await tester.pump(const Duration(milliseconds: 100));
      for (var i = 0; i < 10 && peer.writes.length == written; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(peer.writes.length, written + 1);
      expect(peer.writes.last[2], LitchcraftV2Protocol.paint);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, before),
        isFalse,
        reason: 'the production pointer produced optimistic paint before ACK',
      );
      await finger.up();
      await tester.pump();
      expect(controller.pending, isTrue);
      reply.complete();
      await settle(tester, controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.waitingForLift, isTrue);
      expect(sameLitchcraftDocument(peer.document, before), isTrue);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, before),
        isTrue,
      );
      expect(
        tester
            .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
            .canvas
            .colors,
        before.frames.first.emitters.map((e) => e.color),
      );
      expect(
        controller.canUndo,
        isFalse,
        reason: 'refused paint creates no accepted history',
      );
      final strings = AppLocalizations.of(
        tester.element(find.byType(LitchcraftScreen)),
      );
      expect(find.text(strings.craftBoardBusyRecovery), findsOneWidget);
      final refusalWrites = peer.writes.skip(written).map((p) => p[2]).toList();
      expect(
        refusalWrites,
        [
          LitchcraftV2Protocol.paint,
          LitchcraftV2Protocol.snapshot,
          LitchcraftV2Protocol.inspect,
        ],
        reason:
            'retire queued End; recover with one snapshot and its ordering barrier, never Replace',
      );
      peer.physicalBrushHeld = false;
      peer.paintReplyGate = null;
      await tester.pump(const Duration(seconds: 10));
      expect(
        peer.writes.length,
        written + 3,
        reason: 'lifting never replays rejected paint',
      );
      final retryRect = tester.getRect(
        find.byKey(const ValueKey('litchcraft-canvas')),
      );
      await tester.tapAt(
        retryRect.topLeft + Offset(retryRect.width / 16, retryRect.height / 16),
      );
      await settle(tester, controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.waitingForLift, isFalse);
      expect(controller.errorMessage, isNull);
      expect(find.text(strings.craftBoardBusyRecovery), findsNothing);
      expect(sameLitchcraftDocument(peer.document, before), isFalse);
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          peer.document,
        ),
        isTrue,
      );
      expect(controller.canUndo, isTrue);
      expect(
        peer.writes.skip(written).map((p) => p[2]),
        [
          LitchcraftV2Protocol.paint,
          LitchcraftV2Protocol.snapshot,
          LitchcraftV2Protocol.inspect,
          LitchcraftV2Protocol.paint,
        ],
        reason: 'A successful manual retry needs no redundant readback.',
      );
      expect(ble.connectionSessionToken, epoch);
      expect(peer.subscriptions, 1);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  testWidgets('Apple native payload is not reduced by a second ATT header', (
    tester,
  ) async {
    debugDefaultTargetPlatformOverride = TargetPlatform.iOS;
    final peer = LitchcraftV2FunctionalPeer()..nativeMtuResult = 244;
    final ble = LitchcraftV2FunctionalService(peer);
    final controller = LitchcraftController(ble);
    await finish(tester, controller.enter());
    expect(controller.status, LitchcraftStatus.live);
    expect(peer.notificationLengths, contains(244));
    expect(
      await ble.litchcraftPacketPayloadBytes(
        'board',
        connectionSessionToken: ble.connectionSessionToken,
      ),
      244,
    );
    final next = LitchcraftDocumentSession(peer.document)
      ..setSelectedFrameDuration(2100);
    await finish(tester, controller.replaceDocument(next.document));
    expect(peer.writes.any((bytes) => bytes.length == 244), isTrue);
    controller.dispose();
    await finish(tester, ble.dispose());
    await peer.close();
    debugDefaultTargetPlatformOverride = null;
  });

  testWidgets(
    'controller uses real epoch-safe v2 BLE writer and MTU247 payloads',
    (tester) async {
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      await finish(tester, controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(peer.mtuRequests, 1);
      expect(peer.notificationLengths, contains(244));
      expect(
        await ble.litchcraftPacketPayloadBytes(
          'board',
          connectionSessionToken: ble.connectionSessionToken,
        ),
        244,
      );
      final next = LitchcraftDocumentSession(peer.document)
        ..setSelectedFrameDuration(2100);
      await finish(tester, controller.replaceDocument(next.document));
      expect(peer.writes.any((bytes) => bytes.length == 244), isTrue);
      expect(
        peer.writes.every(
          (bytes) =>
              bytes.length <= 244 &&
              (bytes.first == 0x4d ||
                  bytes.first == 0x4c && bytes[1] == LitchcraftProtocol.enter),
        ),
        isTrue,
      );
      expect(sameLitchcraftDocument(peer.document, next.document), isTrue);
      final written = peer.writes.length;
      expect(
        () => ble.writeLitchcraftV2Packet(
          'board',
          LitchcraftV2Protocol.request(LitchcraftV2Protocol.inspect, 0, 80),
          connectionSessionToken: ble.connectionSessionToken - 1,
        ),
        throwsStateError,
      );
      expect(peer.writes.length, written);
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );

  for (final physical in [false, true]) {
    testWidgets(
      'screen ${physical ? 'physical adoption' : 'Paint'} Undo Redo uses board history',
      (tester) async {
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final peer = LitchcraftV2FunctionalPeer();
        final ble = LitchcraftV2FunctionalService(peer);
        final controller = LitchcraftController(ble);
        final library = LitchcraftLibrary(controller);
        await finish(tester, library.ready);
        await finish(tester, controller.enter());
        final initial = peer.document;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: controller, library: library),
            ),
          ),
        );
        await tester.pump();
        if (physical) {
          peer.physicalPaint(0, 0xabcdef);
        } else {
          final rect = tester.getRect(
            find.byKey(const ValueKey('litchcraft-canvas')),
          );
          await tester.tapAt(
            rect.topLeft + Offset(rect.width / 16, rect.height / 16),
          );
        }
        await settle(tester, controller);
        final painted = peer.document;
        expect(sameLitchcraftDocument(initial, painted), isFalse);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, painted),
          isTrue,
        );
        expect(controller.advancedBoardAuthorityLive, isTrue);
        expect(controller.canUndo, isTrue);
        if (physical) expect(library.canUndoAnimation, isFalse);
        await tester.ensureVisible(find.byKey(const ValueKey('craft-undo')));
        await tester.tap(find.byKey(const ValueKey('craft-undo')));
        await settle(tester, controller);
        expect(
          peer.writes.map((p) => p[2]),
          contains(LitchcraftV2Protocol.undo),
        );
        expect(sameLitchcraftDocument(peer.document, initial), isTrue);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, initial),
          isTrue,
        );
        expect(controller.canUndo, isFalse);
        expect(controller.canRedo, isTrue);
        expect(library.canRedoAnimation, isFalse);
        await tester.tap(find.byKey(const ValueKey('craft-redo')));
        await settle(tester, controller);
        expect(
          peer.writes.map((p) => p[2]),
          contains(LitchcraftV2Protocol.redo),
        );
        expect(sameLitchcraftDocument(peer.document, painted), isTrue);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, painted),
          isTrue,
        );
        expect(controller.canUndo, isTrue);
        expect(controller.canRedo, isFalse);
        await tester.pumpWidget(const SizedBox());
        library.dispose();
        controller.dispose();
        await finish(tester, ble.dispose());
        await peer.close();
        debugDefaultTargetPlatformOverride = null;
      },
    );
  }

  testWidgets(
    'screen advanced app-only Undo stays in the shared local history',
    (tester) async {
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final peer = LitchcraftV2FunctionalPeer();
      final ble = LitchcraftV2FunctionalService(peer);
      final controller = LitchcraftController(ble);
      final library = LitchcraftLibrary(controller);
      await finish(tester, library.ready);
      controller.restoreDocument(peer.document);
      library.setSelectedFrameDuration(2700);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      await tester.pump();
      await tester.ensureVisible(find.byKey(const ValueKey('craft-undo')));
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.pump();
      expect(library.selectedFrame.durationMs, 1000);
      expect(peer.writes, isEmpty);
      expect(library.canRedoAnimation, isTrue);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
      controller.dispose();
      await finish(tester, ble.dispose());
      await peer.close();
      debugDefaultTargetPlatformOverride = null;
    },
  );
}
