import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

LitchcraftAnimationDocument _artwork({
  bool colored = false,
  Iterable<int> favoriteColors = litchcraftDefaultFavoriteColors,
}) => LitchcraftAnimationDocument(
  topology: LitchcraftTopology.v3,
  favoriteColors: favoriteColors,
  playbackOnEntry: true,
  sparkleSeed: 23,
  frames: [
    LitchcraftAnimationFrame(
      durationMs: 350,
      emitters: List.generate(
        256,
        (index) => LitchcraftAnimatedEmitter(
          color: colored && index < 4 ? 0x123456 : 0,
        ),
      ),
    ),
  ],
);

void main() {
  test('black off frame is empty despite metadata and retained Undo', () {
    final board = LitchcraftDocumentSession(_artwork())
      ..paintSelectedFrame(squares: [0], mask: 15, color: 0x245678)
      ..clearSelectedFrame();
    expect(board.canUndo, isTrue);
    final phone = LitchcraftDocumentSession(_artwork(colored: true));
    expect(
      phone.observeBoard('board', board.document),
      LitchcraftDocumentResolution.sendApp,
    );
    expect(board.canUndo, isTrue);
    expect(board.document.frames.single.durationMs, 350);
    expect(board.document.sparkleSeed, 23);
    expect(board.document.playbackOnEntry, isTrue);
  });

  test(
    'cleared common board is empty after both sides retain different edits',
    () {
      final common = _artwork(colored: true);
      final board = LitchcraftDocumentSession(common)..clearSelectedFrame();
      final phone = LitchcraftDocumentSession(common)..confirm('board', common);
      phone.paintSelectedFrame(squares: [0], mask: 15, color: 0xabcdef);
      expect(phone.canUndo, isTrue);
      expect(board.canUndo, isTrue);
      expect(
        phone.observeBoard('board', board.document),
        LitchcraftDocumentResolution.sendApp,
      );
      expect(phone.document.frames.single.emitters.first.color, 0xabcdef);
      expect(phone.canUndo, isTrue);
    },
  );

  testWidgets('native paint clear keeps Undo while empty entry reads once', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues({});
    const id = 'virtual-litch-production';
    final native = (await tester.runAsync(
      () => VirtualLitchProcess.start(1, false, hardwareV3: true),
    ))!;
    var ble = VirtualLitchGattBle(
      native,
      negotiatedMtu: 517,
      operationIntervalMs: 8,
    );
    LitchBleService createService() => LitchBleService(
      ble: ble,
      gattSessionFactory: ReactiveBleGattSessionFactory(ble),
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    var service = createService();
    var controller = LitchcraftController(service);
    LitchcraftLibrary? library;
    var settled = false;
    Future<void> tick() async {
      await tester.pump();
      if (ble.hasPendingOperations) {
        await ble.flush(tester, maxOperations: 1);
      } else {
        await ble.command(tester, {
          'op': 'advance',
          'ms': 10,
        }, flushOperations: false);
        await tester.pump(const Duration(milliseconds: 10));
      }
    }

    Future<void> until(bool Function() done) async {
      for (var i = 0; i < 1500 && !done(); i++) {
        await tick();
      }
      expect(
        done(),
        isTrue,
        reason: '${controller.status}: ${controller.errorMessage}',
      );
    }

    Future<void> finish(Future<dynamic> work) async {
      var done = false;
      Object? failure;
      work.then<void>(
        (_) => done = true,
        onError: (Object error) {
          failure = error;
          done = true;
        },
      );
      await until(() => done);
      if (failure != null) throw failure!;
    }

    Future<Map<String, dynamic>> store() async =>
        (await ble.command(tester, {
              'op': 'store_frame',
              'index': 0,
            }))['store_frame']
            as Map<String, dynamic>;
    Future<void> settle() async {
      if (settled) return;
      settled = true;
      await tester.pumpWidget(const SizedBox.shrink());
      library?.dispose();
      controller.dispose();
      await finish(service.disconnect());
    }

    addTearDown(() async {
      await settle();
      await ble.disposeJourney(tester, service);
    });

    await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
    await ble.advance(tester, 1000);
    final seed = _artwork(colored: true);
    controller.restoreDocument(seed);
    await finish(service.connect(id));
    await finish(controller.enter());
    await finish(
      controller.transferFrameStoreForDiagnostics(
        frames: seed.frames,
        playbackOnEntry: true,
      ),
    );
    await finish(controller.enter());
    await finish(controller.setBrush(color: 0x245678));
    controller.beginStroke();
    controller.paintSquares([9]);
    controller.endStroke();
    await until(() => !controller.pending && !ble.hasPendingOperations);
    await finish(controller.clear());
    await until(() => controller.canUndo && !controller.pending);
    expect(
      controller.documentSession.document.frames.single.emitters.every(
        (emitter) =>
            emitter.color == 0 && emitter.effect == LitchcraftEffectStyle.off,
      ),
      isTrue,
    );
    // This slice optimizes clean saved-root proof; dirty-live admission remains
    // a separately gated journey. Saving never discards native Undo history.
    await ble.advance(tester, 5500);
    final cleared = await store();
    expect(cleared['dirty'], isFalse);
    expect(cleared['frame_count'], 1);
    expect(cleared['duration_ms'], 350);
    expect(cleared['sparkle_seed'], 23);
    expect(cleared['playback_on_entry'], isTrue);
    expect(controller.canUndo, isTrue);
    debugPrint(
      'EMPTY_ENTRY native paint→clear, Undo retained, clean one-frame '
      'black/off; next=enter with different phone artwork',
    );
    controller.dispose();
    await finish(service.disconnect());
    await finish(service.dispose());
    await finish(ble.close());

    ble = VirtualLitchGattBle(
      native,
      negotiatedMtu: 517,
      operationIntervalMs: 8,
    );
    service = createService();
    controller = LitchcraftController(service);
    library = LitchcraftLibrary(controller);
    await finish(library.ready);
    final selected = staticLitchcraftDocument(
      LitchcraftCanvas.blank().paint([7], 15, 0xabcdef),
    );
    controller.restoreDocument(selected);
    await finish(service.connect(id));
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    final transitions = <LitchcraftStatus>[];
    controller.addListener(() => transitions.add(controller.status));
    final from = ble.writes.length;
    List<Uint8List> requests() {
      final assembler = ConnectionLitchcraftAssembler();
      final packets = <Uint8List>[];
      for (final write in ble.writes.skip(from)) {
        if (write['channel'] != 'connection_session') continue;
        final shared = ConnectionSessionCodec.decode(
          Uint8List.fromList(write['bytes'] as List<int>),
        );
        if (shared?.kind != ConnectionSessionKind.object ||
            shared?.code != ConnectionLitchcraftCodec.objectOpcode) {
          continue;
        }
        final packet = assembler.accept(shared!.body);
        if (packet != null) packets.add(packet);
      }
      return packets;
    }

    final entering = controller.enter();
    await until(
      () => requests().any(
        (p) => p[2] == LitchcraftV2Protocol.frameStoreSnapshot,
      ),
    );
    final beforePayload = requests().takeWhile(
      (p) => p[2] != LitchcraftV2Protocol.frameStoreSnapshot,
    );
    expect(
      beforePayload.where((p) => p[2] == LitchcraftV2Protocol.inspect),
      hasLength(2),
      reason:
          'Reuse the entry Inspect once instead of repeating '
          'Inspect and State37 before the single-frame payload',
    );
    await finish(entering);
    await until(
      () =>
          !controller.boardArtworkUnresolved &&
          !controller.pending &&
          !ble.hasPendingOperations,
    );
    expect(controller.status, LitchcraftStatus.live);
    expect(transitions, isNot(contains(LitchcraftStatus.conflict)));
    expect(find.byKey(const ValueKey('craft-resolve-conflict')), findsNothing);
    expect(
      sameLitchcraftDocument(controller.documentSession.document, selected),
      isTrue,
    );
    final replaced = await store();
    expect((replaced['emitters'] as List)[28], [0xabcdef, 0, 0]);
    expect(
      requests().where((p) => p[2] == LitchcraftV2Protocol.frameStoreSnapshot),
      hasLength(1),
    );
    debugPrint(
      'EMPTY_ENTRY no chooser, one payload read, entry metadata reused; '
      'selected phone artwork committed paused; next=paint or explicit Play',
    );
    await settle();
  });

  testWidgets('native reconnect adopts board Clear after offline edit Undo', (
    tester,
  ) async {
    const id = 'virtual-litch-production';
    final native = (await tester.runAsync(
      () => VirtualLitchProcess.start(1, false, hardwareV3: true),
    ))!;
    final ble = VirtualLitchGattBle(
      native,
      negotiatedMtu: 517,
      operationIntervalMs: 8,
    );
    final service = LitchBleService(
      ble: ble,
      gattSessionFactory: ReactiveBleGattSessionFactory(ble),
      pendingOtaStore: MemoryOtaTargetStore(),
    );
    var controller = LitchcraftController(service);
    Future<void> tick() async {
      await tester.pump();
      if (ble.hasPendingOperations) {
        await ble.flush(tester, maxOperations: 1);
      } else {
        await ble.command(tester, {
          'op': 'advance',
          'ms': 10,
        }, flushOperations: false);
        await tester.pump(const Duration(milliseconds: 10));
      }
    }

    Future<void> until(bool Function() done) async {
      for (var i = 0; i < 1500 && !done(); i++) {
        await tick();
      }
      expect(
        done(),
        isTrue,
        reason: '${controller.status}: ${controller.errorMessage}',
      );
    }

    Future<void> finish(Future<dynamic> work) async {
      var done = false;
      Object? failure;
      work.then<void>(
        (_) => done = true,
        onError: (Object error) {
          failure = error;
          done = true;
        },
      );
      await until(() => done);
      if (failure != null) throw failure!;
    }

    Future<Map<String, dynamic>> store() async =>
        (await ble.command(tester, {
              'op': 'store_frame',
              'index': 0,
            }))['store_frame']
            as Map<String, dynamic>;
    var settled = false;
    Future<void> settle() async {
      if (settled) return;
      settled = true;
      controller.dispose();
      await finish(service.disconnect());
    }

    addTearDown(() async {
      await settle();
      await ble.disposeJourney(tester, service);
    });

    await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
    await ble.advance(tester, 1000);
    final seed = _artwork(colored: true);
    controller.restoreDocument(seed);
    await finish(service.connect(id));
    await finish(controller.enter());
    await finish(
      controller.transferFrameStoreForDiagnostics(
        frames: seed.frames,
        playbackOnEntry: true,
      ),
    );
    await finish(controller.enter());
    await until(() => !controller.pending && !ble.hasPendingOperations);
    // Matching frame/root metadata passively confirms C while retaining phone
    // palette A. The saved board root still owns its different palette B.
    controller.dispose();
    await finish(service.disconnect());
    controller = LitchcraftController(service);
    final paletteA = List<int>.of(litchcraftDefaultFavoriteColors)
      ..[0] = 0x13579b;
    controller.restoreDocument(
      _artwork(colored: true, favoriteColors: paletteA),
    );
    await finish(service.connect(id));
    await finish(controller.enter());
    await until(() => !controller.pending && !ble.hasPendingOperations);
    expect(controller.documentSession.document.favoriteColors, paletteA);
    expect((await store())['favorite_colors'], seed.favoriteColors);
    final common = controller.documentSession.document;
    expect(controller.documentSession.hasCommonDocument, isTrue);
    expect(isEmptyLitchcraftDocument(common), isFalse);
    await finish(service.disconnect());
    await until(() => controller.status == LitchcraftStatus.offline);
    await finish(controller.setBrush(color: 0xabcdef));
    controller.beginStroke();
    controller.paintSquares([9]);
    controller.endStroke();
    expect(
      sameLitchcraftDocument(controller.documentSession.document, common),
      isFalse,
    );
    await finish(controller.undo());
    expect(
      sameLitchcraftDocument(controller.documentSession.document, common),
      isTrue,
    );
    expect(controller.documentSession.hasLocalIntent, isTrue);
    expect(controller.documentSession.canRedo, isTrue);
    debugPrint(
      'EMPTY_COMMON phone offline edit→Undo returns exactly to confirmed C; '
      'retained history still reports local intent; next=board Clear',
    );

    // Existing production motion qualifier accepts this centered waveform.
    // Only sensor I2C/filtering is bypassed; the board owns the erase transaction.
    var x = -1085, y = -542, sign = 1;
    await ble.command(tester, {
      'op': 'motion',
      'x': x,
      'y': y,
      'z': 1000,
    }, flushOperations: false);
    for (final count in [7, 7, 7, 5]) {
      for (var i = 0; i < count; i++) {
        await ble.advance(tester, 40);
        x += sign * 310;
        y += sign * 155;
        await ble.command(tester, {
          'op': 'motion',
          'x': x,
          'y': y,
          'z': 1000,
        }, flushOperations: false);
      }
      sign = -sign;
    }
    await ble.advance(tester, 5500);
    final cleared = await store();
    expect(cleared['dirty'], isFalse);
    expect(cleared['favorite_colors'], seed.favoriteColors);
    expect(cleared['favorite_colors'], isNot(equals(paletteA)));
    expect(
      (cleared['emitters'] as List).every((e) => e[0] == 0 && e[1] == 0),
      isTrue,
    );
    final from = ble.writes.length;
    final transitions = <LitchcraftStatus>[];
    controller.addListener(() => transitions.add(controller.status));
    await finish(service.connect(id));
    await until(
      () =>
          controller.status == LitchcraftStatus.live &&
          !controller.boardArtworkUnresolved &&
          !controller.pending &&
          !ble.hasPendingOperations,
    );
    expect(controller.status, LitchcraftStatus.live);
    expect(transitions, isNot(contains(LitchcraftStatus.conflict)));
    expect(
      isEmptyLitchcraftDocument(controller.documentSession.document),
      isTrue,
      reason:
          'Returning to common C leaves the accepted board Clear authoritative',
    );
    expect(
      controller.documentSession.document.favoriteColors,
      paletteA,
      reason:
          'Automatic adoption preserves the phone palette during passive sync',
    );
    final assembler = ConnectionLitchcraftAssembler();
    final requests = <Uint8List>[];
    for (final write in ble.writes.skip(from)) {
      if (write['channel'] != 'connection_session') continue;
      final shared = ConnectionSessionCodec.decode(
        Uint8List.fromList(write['bytes'] as List<int>),
      );
      if (shared?.kind != ConnectionSessionKind.object ||
          shared?.code != ConnectionLitchcraftCodec.objectOpcode) {
        continue;
      }
      final packet = assembler.accept(shared!.body);
      if (packet != null) requests.add(packet);
    }
    expect(
      requests.where((p) => p[2] == LitchcraftV2Protocol.frameStoreBegin),
      isEmpty,
      reason: 'Reconnect must not refill the cleared board with C',
    );
    final after = await store();
    expect(cleared['root_generation'], isNotNull);
    expect(after['root_generation'], cleared['root_generation']);
    expect(after['emitters'], cleared['emitters']);
    expect(after['favorite_colors'], seed.favoriteColors);
    final shown =
        (await ble.command(tester, {'op': 'frame'}))['frame']
            as Map<String, dynamic>;
    expect(shown['litchcraft_active'], isTrue);
    expect(
      (shown['board'] as List).every(
        (square) => (square as List).every(
          (color) => (color as List).every((channel) => channel == 0),
        ),
      ),
      isTrue,
      reason: 'The accepted blank remains visible after reconnect',
    );
    debugPrint(
      'EMPTY_COMMON clean reconnect adopted board Clear, no chooser, '
      'no Store Begin/refill, root unchanged, phone palette A preserved '
      'and board palette B retained; next=paint or Undo on board',
    );
    await settle();
  });
}
