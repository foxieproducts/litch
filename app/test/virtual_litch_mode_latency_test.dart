import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/home_board_snapshot.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/litch_control_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/widgets/home_chessboard.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const _boardId = 'virtual-litch-production';

class _Journey {
  _Journey(this.tester, this.ble)
    : service = LitchBleService(
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        pendingOtaStore: MemoryOtaTargetStore(),
      ) {
    controller = LitchcraftController(
      service,
      monotonicMilliseconds: () => now,
    );
    library = LitchcraftLibrary(controller);
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
  }

  bool _retired = false;
  final WidgetTester tester;
  final VirtualLitchGattBle ble;
  final LitchBleService service;
  late final LitchcraftController controller;
  late final LitchcraftLibrary library;
  int get now => tester.binding.clock.now().millisecondsSinceEpoch;

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

  Future<void> until(bool Function() done, {int budgetMs = 10000}) async {
    final start = now;
    while (!done() && now - start < budgetMs) {
      await tester.pump();
      if (!done()) await tick();
    }
    expect(done(), isTrue, reason: 'Mode journey exceeded $budgetMs ms');
  }

  Future<void> finish(Future<dynamic> work) async {
    var done = false;
    Object? failure;
    work.then<void>(
      (_) => done = true,
      onError: (Object error, StackTrace _) {
        failure = error;
        done = true;
      },
    );
    await until(() => done);
    if (failure != null) throw failure!;
  }

  Future<Map<String, dynamic>> frame() async =>
      (await ble.command(tester, {
            'op': 'frame',
          }, flushOperations: false))['frame']
          as Map<String, dynamic>;

  Future<Map<String, dynamic>> store([int index = 0]) async =>
      (await ble.command(tester, {
            'op': 'store_frame',
            'index': index,
          }, flushOperations: false))['store_frame']
          as Map<String, dynamic>;

  Future<void> boardMode(bool active) async {
    final start = now;
    var board = await frame();
    while (board['litchcraft_active'] != active && now - start < 10000) {
      await tick();
      board = await frame();
    }
    expect(board['litchcraft_active'], active);
    if (!active) expect(board['litchcraft_visible'], isFalse);
  }

  Future<void> mode(bool active, {int budgetMs = 10000}) async {
    final start = now;
    var board = await frame();
    while ((board['litchcraft_active'] != active ||
            controller.boardActive != active) &&
        now - start < budgetMs) {
      await tick();
      board = await frame();
    }
    expect(board['litchcraft_active'], active);
    expect(controller.boardActive, active, reason: controller.errorMessage);
    if (!active) expect(board['litchcraft_visible'], isFalse);
  }

  Future<void> connect() async {
    await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
    await ble.advance(tester, 1000);
    await finish(service.connect(_boardId));
    await finish(library.ready);
    ConnectionSession? session;
    await finish(
      service.openConnectionSession(_boardId).then((value) => session = value),
    );
    expect(session?.isActivated, isTrue);
    expect(session!.storeGuardEpochSupported, isTrue);
    final capabilities = service.deviceCapabilities(_boardId);
    expect(capabilities.explicitlyReported, isTrue);
    expect(capabilities.supportsConnectionSession, isTrue);
    expect(ble.discoveryCalls, greaterThan(0));
    expect(ble.qualifiedCalls, 0);
    expect(
      ble.writes.where((w) => w['channel'] == 'connection_session'),
      isNotEmpty,
    );
    expect(
      ble.writes
          .where((w) => w['channel'] == 'connection_session')
          .every((w) => (w['bytes'] as List<int>).first >> 4 == 4),
      isTrue,
      reason: 'This gate requires the current epoch, not the pre-session path',
    );
  }

  Future<void> seed({int frames = 2}) async {
    await finish(controller.enter());
    final source = LitchcraftDocumentSession(
      controller.documentSession.document,
    );
    while (source.document.frames.length < frames) {
      source.addFrame();
    }
    await finish(controller.replaceDocument(source.document));
    source.dispose();
    await until(
      () =>
          !controller.boardArtworkUnresolved &&
          !controller.pending &&
          controller.documentSession.document.frames.length == frames,
    );
    await finish(controller.selectAnimationFrame(0));
    expect(controller.usesFrameStoreDocument, isTrue);
    expect(
      controller.status,
      LitchcraftStatus.live,
      reason: controller.errorMessage,
    );
  }

  Future<void> button(int button, bool down) async {
    await ble.command(tester, {
      'op': 'button',
      'owner': 1,
      'button': button,
      'down': down,
    }, flushOperations: false);
    await ble.advance(tester, 30);
  }

  Future<void> chord() async {
    await button(5, true); // Physical X.
    await ble.advance(tester, 70);
    await button(6, true); // Physical O, inside the production chord window.
    await ble.advance(tester, 1500);
    await button(5, false);
    await button(6, false);
  }

  Future<void> paintAfterReturn({bool alreadyMounted = false}) async {
    expect(
      controller.status,
      LitchcraftStatus.live,
      reason: controller.errorMessage,
    );
    expect(
      controller.boardArtworkUnresolved,
      isFalse,
      reason: 'A confirmed document is required before remote paint',
    );
    if (!alreadyMounted) {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
    }
    await tester.pump();
    final selected = controller.documentSession.selectedFrameIndex;
    final rect = tester.getRect(find.byType(LitchcraftCanvasView));
    Offset at(int column) =>
        rect.topLeft +
        Offset(rect.width * (column + .5) / 8, rect.height * 3.5 / 8);
    final finger = await tester.startGesture(at(3));
    await tester.pump(const Duration(milliseconds: 16));
    await finger.moveTo(at(4));
    await tester.pump(const Duration(milliseconds: 16));
    await finger.up();
    await tester.pump();
    expect(
      controller.documentSession.document.frames[selected].emitters[108].color,
      controller.color,
    );
    await until(() => !controller.pending && !ble.hasPendingOperations);
    await ble.advance(tester, 5500);
    await until(() => !controller.pending && !ble.hasPendingOperations);
    final saved = await store(selected);
    expect(saved['dirty'], isFalse);
    final emitters = saved['emitters'] as List;
    expect((emitters[108] as List).first, controller.color);
    expect((emitters[112] as List).first, controller.color);
    expect(saved['selected_frame'], selected);
    expect(saved['playing'], isFalse);
    expect(
      controller.status,
      LitchcraftStatus.live,
      reason: controller.errorMessage,
    );
  }

  void checkpoint(String stage, Map<String, dynamic> board) {
    debugPrint(
      'MODE_ORDER $stage app_selected=${controller.selected} '
      'app_active=${controller.boardActive} native_active=${board['litchcraft_active']} '
      'app_eraser=${controller.eraser} native_eraser=${board['litchcraft_erasing']} '
      'occupancy=${board['occupancy']} display="${board['display']}" '
      'action=${board['required_action']} canvas_square27=${(board['board'] as List)[27]} next=paint-or-mode-change',
    );
  }

  Future<void> settle({bool expectStoreBatches = true}) async {
    if (_retired) return;
    _retired = true;
    await tester.pumpWidget(const SizedBox.shrink());
    library.dispose();
    controller.dispose();
    await ble.flush(tester);
    await finish(service.disconnect());
    expect(ble.qualifiedCalls, 0, reason: 'No UUID-only fallback is allowed');
    expect(ble.exactOperations, greaterThan(0));
    if (expectStoreBatches) {
      expect(
        ble.exactBatches,
        isNotEmpty,
        reason: 'Store bursts use the exact batch API',
      );
    }
  }

  Future<void> dispose() async {
    await settle();
    await ble.disposeJourney(tester, service);
  }
}

Iterable<LitchcraftV2Packet> _modePackets(
  Iterable<VirtualPacket> packets,
) sync* {
  final assembler = ConnectionLitchcraftAssembler();
  for (final packet in packets) {
    if (packet.channel != 'connection_session') continue;
    final shared = ConnectionSessionCodec.decode(
      Uint8List.fromList(packet.bytes),
    );
    if (shared?.kind != ConnectionSessionKind.object ||
        shared?.code != ConnectionLitchcraftCodec.receiptCode) {
      continue;
    }
    try {
      final bytes = assembler.accept(shared!.body);
      final domain = bytes == null ? null : LitchcraftV2Packet.decode(bytes);
      if (domain?.event == LitchcraftV2Protocol.frameStoreState) {
        yield domain!;
      }
    } on FormatException {
      // A capture may start after an unrelated response's first fragment.
    }
  }
}

void main() {
  setUp(() => SharedPreferences.setMockInitialValues({}));

  for (final choice in ['discard', 'keep', 'dismiss', 'large text back']) {
    testWidgets('cleared ongoing game Craft entry: $choice', (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchGattBle(native, negotiatedMtu: 247);
      final journey = _Journey(tester, ble);
      if (choice == 'large text back') {
        tester.view.physicalSize = const Size(844, 390);
      }
      addTearDown(() async {
        await journey.settle(expectStoreBatches: false);
        await journey.dispose();
      });
      await journey.connect();
      final opening = BigInt.parse('ffff00000000ffff', radix: 16);
      Future<void> hall(BigInt occupancy, int ms) async {
        await ble.command(tester, {
          'op': 'hall',
          'mask': occupancy.toRadixString(16).padLeft(16, '0'),
        }, flushOperations: false);
        await ble.advance(tester, ms);
      }

      await hall(opening, 1000);
      await hall(opening & ~(BigInt.one << 52), 300);
      await hall((opening & ~(BigInt.one << 52)) | (BigInt.one << 36), 2500);
      await journey.until(
        () =>
            journey.service.latestHomeState?.position.sideToMove.name ==
            'black',
      );
      final gameFen = journey.service.latestHomeState!.position.fen;
      await hall(BigInt.zero, 500);
      await journey.until(
        () =>
            journey.service.latestHomeState?.physicalStatus.occupiedSquares ==
            BigInt.zero,
      );
      debugPrint(
        'GAME_ENTRY played e2→e4 through Hall, cleared all pieces; game="$gameFen" next=choose board discard or local app',
      );
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(
              textScaler: TextScaler.linear(
                choice == 'large text back' ? 2 : 1,
              ),
            ),
            child: child!,
          ),
          home: Scaffold(
            body: LitchcraftScreen(
              controller: journey.controller,
              library: journey.library,
            ),
          ),
        ),
      );
      final from = ble.writes.length;
      await journey.finish(journey.controller.enter());
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(
        find.byKey(const ValueKey('craft-game-entry-choice')),
        findsOneWidget,
      );
      expect(
        find.byKey(const ValueKey('craft-game-entry-cancel')),
        findsNothing,
      );
      if (choice == 'large text back') {
        await tester.binding.handlePopRoute();
      } else if (choice == 'dismiss') {
        await tester.tapAt(const Offset(4, 4));
      } else {
        await tester.tap(find.byKey(ValueKey('craft-game-entry-$choice')));
      }
      await tester.pump();
      await journey.until(
        () => find
            .byKey(const ValueKey('craft-game-entry-choice'))
            .evaluate()
            .isEmpty,
      );
      if (choice == 'discard') {
        await journey.until(() => journey.controller.boardActive);
        await journey.mode(true);
        expect(journey.service.latestHomeState!.freshSetup, isTrue);
        expect(
          journey.service.latestHomeState!.physicalStatus.occupiedSquares,
          BigInt.zero,
        );
      } else {
        await journey.boardMode(false);
        expect(journey.service.latestHomeState!.position.fen, gameFen);
        expect(journey.controller.boardActive, isFalse);
        if (choice == 'keep') {
          await journey.finish(journey.service.disconnect());
          await journey.finish(journey.service.connect(_boardId));
          await journey.finish(journey.service.openConnectionSession(_boardId));
          await journey.finish(journey.controller.revalidateSelectedBoard());
          await journey.until(() => !ble.hasPendingOperations);
          final canvas = tester.getRect(find.byType(LitchcraftCanvasView));
          await tester.tapAt(canvas.center);
          await tester.pump();
          expect(journey.controller.hasMarks, isTrue);
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 300));
          await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
          await tester.pump();
          await tester.tap(find.byKey(const ValueKey('craft-preview')));
          await tester.pump(const Duration(milliseconds: 300));
          expect(
            find.byKey(const ValueKey('craft-animation-playing-indicator')),
            findsOneWidget,
          );
          expect(journey.controller.animationPlaying, isFalse);
          await tester.tap(find.byKey(const ValueKey('craft-preview')));
          await tester.pump();
        }
      }
      final sharedWrites = ble.writes
          .skip(from)
          .where((w) => w['channel'] == 'connection_session')
          .map(
            (w) => ConnectionSessionCodec.decode(
              Uint8List.fromList(w['bytes'] as List<int>),
            ),
          )
          .whereType<ConnectionSessionPacket>()
          .toList();
      final ordered = sharedWrites.where(
        (p) => p.kind == ConnectionSessionKind.ordered,
      );
      final resets = ordered.where(
        (p) =>
            p.code == LitchControlProtocol.connectionActionOpcode &&
            p.body[1] == 3,
      );
      expect(resets, hasLength(choice == 'discard' ? 1 : 0));
      final assembler = ConnectionLitchcraftAssembler();
      final modeRequests = <Uint8List>[];
      for (final packet in sharedWrites) {
        final domain =
            packet.kind == ConnectionSessionKind.ordered &&
                packet.code == ConnectionSession.litchcraftModeOpcode
            ? packet.body
            : packet.kind == ConnectionSessionKind.object &&
                  packet.code == ConnectionLitchcraftCodec.objectOpcode
            ? assembler.accept(packet.body)
            : null;
        if (domain != null &&
            domain.length == 8 &&
            domain[0] == LitchcraftProtocol.magic &&
            domain[1] == LitchcraftProtocol.enter) {
          modeRequests.add(domain);
        }
      }
      expect(modeRequests, hasLength(choice == 'discard' ? 1 : 0));
      journey.checkpoint('ongoing game choice=$choice', await journey.frame());
      await journey.settle(expectStoreBatches: false);
    });
  }

  for (final mtu in [23, 247]) {
    testWidgets(
      'modern Store Tool and Exit preempt upload at MTU $mtu',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(
          native,
          negotiatedMtu: mtu,
          operationIntervalMs: 20,
        );
        final journey = _Journey(tester, ble);
        addTearDown(journey.dispose);
        await journey.connect();
        await journey.seed(frames: 8);
        final controller = journey.controller;

        var uploadCompleted = true;
        Future<void> upload(int square) async {
          final source = LitchcraftDocumentSession(
            controller.documentSession.document,
          )..paintSelectedFrame(squares: [square], mask: 15, color: 0xabcdef);
          uploadCompleted = false;
          final work = controller.replaceDocument(source.document);
          work.then<void>(
            (_) => uploadCompleted = true,
            onError: (Object _, StackTrace _) => uploadCompleted = true,
          );
          source.dispose();
          work.ignore();
          await journey.until(() => ble.pendingExactBatches > 0);
          expect(
            uploadCompleted,
            isFalse,
            reason: 'The oracle must observe outstanding artwork work',
          );
        }

        Future<void> measureTool(
          String label,
          bool erase, {
          bool duringBatch = false,
        }) async {
          final singleWritesBefore = ble.singleWrites.length;
          if (duringBatch) expect(ble.pendingExactBatches, greaterThan(0));
          final start = journey.now;
          final work = controller.setBrush(eraser: erase);
          expect(
            controller.eraser,
            erase,
            reason: 'Tool feedback is immediate',
          );
          work.ignore();
          if (duringBatch) {
            await journey.until(
              () => ble.singleWrites.skip(singleWritesBefore).any((write) {
                final packet = ConnectionSessionCodec.decode(
                  Uint8List.fromList(write.value),
                );
                return write.batchPending &&
                    !write.withResponse &&
                    packet?.kind == ConnectionSessionKind.ordered &&
                    packet?.code == ConnectionSession.litchcraftToolOpcode;
              }),
            );
            expect(
              uploadCompleted,
              isFalse,
              reason:
                  'Interactive admission cannot wait for the whole transfer',
            );
          }
          var board = await journey.frame();
          while (board['litchcraft_erasing'] != erase &&
              journey.now - start < 1000) {
            await journey.tick();
            board = await journey.frame();
          }
          final acceptedMs = journey.now - start;
          expect(board['litchcraft_erasing'], erase);
          await journey.finish(work);
          debugPrint(
            'SIMULATED_DIAGNOSTIC epoch=4 mtu=$mtu action=$label '
            'receiver_ms=$acceptedMs terminal_ms=${journey.now - start}',
          );
        }

        await upload(7);
        await measureTool('erase-during-upload', true, duringBatch: true);
        final olderTool = ble.emitted.lastWhere((packet) {
          if (packet.channel != 'connection_session') return false;
          final shared = ConnectionSessionCodec.decode(
            Uint8List.fromList(packet.bytes),
          );
          return shared?.kind == ConnectionSessionKind.state &&
              shared?.code == ConnectionSession.litchcraftStateCode;
        });
        await measureTool('paint-during-upload', false);
        ble.deliver(olderTool);
        await tester.pump();
        expect(
          controller.eraser,
          isFalse,
          reason: 'Old shared Tool State cannot rewind the latest selection',
        );
        await journey.until(
          () => !controller.pending && !ble.hasPendingOperations,
        );

        // A physical O tap is Redo; Paint/Erase belongs to its one-second hold.
        // Do not drain SDK work while down: its per-operation clock advances
        // can turn a nominal 30ms press into a hold at the minimum MTU.
        final tapStarted = (await journey.frame())['now_ms'] as int;
        for (final down in [true, false]) {
          await ble.command(tester, {
            'op': 'button',
            'owner': 1,
            'button': 6,
            'down': down,
          }, flushOperations: false);
          await ble.command(tester, {
            'op': 'advance',
            'ms': 30,
          }, flushOperations: false);
        }
        final tapElapsed =
            ((await journey.frame())['now_ms'] as int) - tapStarted;
        expect(
          tapElapsed,
          60,
          reason: 'Both raw edges receive the real 20ms debounce',
        );
        await ble.flush(tester);
        await ble.advance(tester, 800);
        expect(controller.eraser, isFalse);
        expect((await journey.frame())['litchcraft_erasing'], isFalse);
        journey.checkpoint('physical-O-tap-keeps-Paint', await journey.frame());
        await journey.button(6, true);
        await ble.advance(tester, 1000);
        await journey.until(() => controller.eraser);
        expect((await journey.frame())['litchcraft_erasing'], isTrue);
        await journey.button(6, false);
        await ble.advance(tester, 800);
        expect(
          controller.eraser,
          isTrue,
          reason: 'Release preserves the hold-selected Erase tool',
        );
        journey.checkpoint(
          'physical-O-hold-selects-Erase',
          await journey.frame(),
        );
        await journey.finish(controller.setBrush(eraser: false));

        await upload(8);
        final draft = controller.documentSession.document;
        final leaving = controller.leave();
        leaving.ignore();
        final start = journey.now;
        await journey.boardMode(false);
        final exitReceiverMs = journey.now - start;
        debugPrint(
          'SIMULATED_DIAGNOSTIC epoch=4 mtu=$mtu action=exit-during-upload receiver_ms=$exitReceiverMs',
        );
        await journey.finish(leaving);
        ble.deliver(olderTool);
        await tester.pump();
        expect(
          controller.boardActive,
          isFalse,
          reason: 'Old mode State cannot reverse departure',
        );
        expect(
          sameLitchcraftDocument(controller.documentSession.document, draft),
          isTrue,
        );
        final reenterStart = journey.now;
        final entering = controller.enter();
        entering.ignore();
        await journey.boardMode(true);
        final enterReceiverMs = journey.now - reenterStart;
        debugPrint(
          'SIMULATED_DIAGNOSTIC epoch=4 mtu=$mtu action=reenter receiver_ms=$enterReceiverMs',
        );
        await journey.finish(entering);
        await journey.mode(true);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, draft),
          isTrue,
        );
        expect(
          controller.status,
          LitchcraftStatus.conflict,
          reason:
              'Exit interrupted a replacement before adoption; the draft needs its existing explicit source recovery',
        );
        await journey.finish(controller.resolveConflict(useBoard: false));
        await journey.until(
          () => !controller.pending && !controller.boardArtworkUnresolved,
        );
        await journey.paintAfterReturn();
        journey.checkpoint('upload-exit-return-mtu$mtu', await journey.frame());
        await journey.settle();
        // Physical acceptance remains <50 ms on the current app/board pair.
        // This sequential SDK pipe does not model native queue/radio timing.
        debugPrint(
          'PHYSICAL_GATE pending: Tool/Enter/Exit receiver start <50ms; '
          'native Android/Swift admission suites cover batch/control scheduling',
        );
      },
      variant: TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'modern Store occupied refusal, retry and physical mode following',
    (tester) async {
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
      final journey = _Journey(tester, ble);
      addTearDown(journey.dispose);
      await journey.connect();
      await ble.command(tester, {'op': 'hall', 'mask': '0000000000000001'});
      final draft = journey.controller.documentSession.document;
      await journey.finish(journey.controller.enter());
      await journey.mode(false);
      expect(journey.controller.status, LitchcraftStatus.boardNotEmpty);
      expect(
        sameLitchcraftDocument(
          journey.controller.documentSession.document,
          draft,
        ),
        isTrue,
      );
      expect((await journey.frame())['occupancy'], '0000000000000001');
      await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
      await ble.advance(tester, 1000);
      await journey.seed();
      await journey.finish(
        journey.controller.setBrush(color: 0xff7430, targetMask: 15),
      );
      await journey.finish(journey.controller.leave());
      await journey.mode(false);
      expect(journey.controller.selected, isFalse);
      expect(
        sameLitchcraftDocument(
          journey.controller.documentSession.document,
          draft,
        ),
        isFalse,
        reason: 'The explicit two-frame seed replaces only the fixture blank',
      );
      final saved = journey.controller.documentSession.document;
      var resolved = false;
      final pages = <int>[];
      await tester.pumpWidget(
        MaterialApp(
          home: LitchMainScaffold(
            bleService: journey.service,
            boardId: _boardId,
            boards: const [],
            onDisconnect: journey.service.disconnect,
            craftController: journey.controller,
            craftLibrary: journey.library,
            initialPageIndex: 1,
            onModeResolved: () => resolved = true,
            onPageSelected: pages.add,
            loadAppConsolePreference: () async => false,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
          ),
        ),
      );
      await journey.until(
        () => resolved && find.byType(NavigationBar).evaluate().isNotEmpty,
      );
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        0,
      );
      final emittedBefore = ble.emitted.length;
      await journey.chord();
      await journey.boardMode(true);
      final modePackets = _modePackets(ble.emitted.skip(emittedBefore));
      debugPrint(
        'PHYSICAL_MODE entry raw=${modePackets.where((p) => p.requestId == 0).map((p) => {'event': p.event, 'session': p.session, 'revision': p.revision, 'payload': p.payload}).toList()}',
      );
      expect(
        modePackets.any((p) => p.requestId == 0 && p.payload[1] & 1 != 0),
        isTrue,
      );
      await journey.mode(true);
      await journey.until(
        () => journey.controller.selected && !journey.controller.pending,
      );
      expect(
        sameLitchcraftDocument(
          journey.controller.documentSession.document,
          saved,
        ),
        isTrue,
      );
      await journey.until(() => pages.contains(0));
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        1,
      );
      journey.checkpoint('physical-entry-followed', await journey.frame());
      await journey.paintAfterReturn(alreadyMounted: true);
      final painted = journey.controller.documentSession.document;
      await journey.chord();
      await journey.mode(false);
      await journey.until(() => pages.last == 1);
      expect(
        tester.widget<NavigationBar>(find.byType(NavigationBar)).selectedIndex,
        0,
      );
      expect(
        sameLitchcraftDocument(
          journey.controller.documentSession.document,
          painted,
        ),
        isTrue,
      );
      journey.checkpoint('physical-exit-followed', await journey.frame());

      // A real completed move establishes the game exception. Occupancy alone
      // is not evidence of an in-progress game, and Craft brush pieces must
      // still permit the ordinary Exit paths above.
      final mapping = ((await journey.frame())['logical_to_physical'] as List)
          .cast<int>();
      Future<void> hall(BigInt logical) => ble.command(tester, {
        'op': 'hall',
        'mask': List.generate(64, (square) => square)
            .where(
              (square) => (logical & (BigInt.one << square)) != BigInt.zero,
            )
            .fold<BigInt>(
              BigInt.zero,
              (physical, square) => physical | (BigInt.one << mapping[square]),
            )
            .toRadixString(16)
            .padLeft(16, '0'),
      });
      final opening = BigInt.parse('FFFF00000000FFFF', radix: 16);
      await hall(opening);
      await ble.advance(tester, 2500);
      final lifted = opening & ~(BigInt.one << 52); // e2.
      await hall(lifted);
      await ble.advance(tester, 30);
      await hall(lifted | (BigInt.one << 36)); // e4.
      await ble.advance(tester, 2550);
      await journey.finish(
        journey.service.requestCurrentHomeStateSnapshot(_boardId),
      );
      await journey.until(
        () =>
            journey.service.latestHomeState?.position.fen.contains(
              '4P3/8/PPPP1PPP/RNBQKBNR b',
            ) ==
            true,
      );
      final gameFen = journey.service.latestHomeState!.position.fen;
      final playBoard = find.byType(HomeChessboard);
      await journey.until(
        () =>
            playBoard.evaluate().isNotEmpty &&
            tester.widget<HomeChessboard>(playBoard).snapshot.position?.fen ==
                gameFen,
      );
      final position = tester.widget<HomeChessboard>(playBoard).snapshot.position!;
      expect(position.pieces[52], isNull, reason: 'Play shows e2 empty');
      expect(position.pieces[36], 'P', reason: 'Play shows the white pawn on e4');
      expect(position.sideToMove, HomeSide.black);
      final gameFrame = await journey.frame();
      final gameStore = await journey.store();
      await tester.tap(find.byKey(const ValueKey('main-destination-craft')));
      await journey.until(
        () =>
            !journey.controller.pending &&
            journey.controller.status != LitchcraftStatus.connecting,
      );
      expect(
        journey.controller.status,
        LitchcraftStatus.boardNotEmpty,
        reason: journey.controller.errorMessage,
      );
      final occupiedRefusal = await journey.frame();
      expect(occupiedRefusal['litchcraft_active'], isFalse);
      expect(occupiedRefusal['occupancy'], gameFrame['occupancy']);
      expect(journey.service.latestHomeState!.position.fen, gameFen);
      // Current firmware reports occupancy first. Even after the pieces are
      // removed, the real resumable game must independently prevent admission.
      await hall(BigInt.zero);
      await tester.tap(find.byKey(const ValueKey('main-destination-play')));
      await journey.until(() => !journey.controller.selected);
      await tester.tap(find.byKey(const ValueKey('main-destination-craft')));
      await journey.until(
        () => journey.controller.status == LitchcraftStatus.gameActive,
      );
      expect(
        find.text(
          'Your game stays on the board. Finish it in Play to paint live.',
        ),
        findsOneWidget,
      );
      expect(find.textContaining('Board controls unavailable'), findsNothing);
      final refused = await journey.frame();
      expect(refused['litchcraft_active'], isFalse);
      expect(refused['occupancy'], '0000000000000000');
      expect(refused['litchcraft_session'], gameFrame['litchcraft_session']);
      expect(refused['litchcraft_revision'], gameFrame['litchcraft_revision']);
      expect(
        (await journey.store())['root_generation'],
        gameStore['root_generation'],
      );
      expect(
        sameLitchcraftDocument(
          journey.controller.documentSession.document,
          painted,
        ),
        isTrue,
      );
      expect(
        find.byKey(const ValueKey('craft-game-entry-choice')),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const ValueKey('craft-game-entry-keep')));
      await journey.until(
        () => find
            .byKey(const ValueKey('craft-game-entry-choice'))
            .evaluate()
            .isEmpty,
      );
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const ValueKey('main-destination-play')));
      await journey.until(
        () =>
            !journey.controller.selected &&
            tester
                    .widget<NavigationBar>(find.byType(NavigationBar))
                    .selectedIndex ==
                0,
      );
      await journey.finish(
        journey.service.requestCurrentHomeStateSnapshot(_boardId),
      );
      expect(journey.service.latestHomeState!.position.fen, gameFen);
      journey.checkpoint('active-game-refused-and-returned-to-Play', refused);
      await journey.settle();
    },
    variant: TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final action in ['add', 'delete']) {
    testWidgets(
      'modern Store pending $action departs and reenters without late replay',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
        final journey = _Journey(tester, ble);
        addTearDown(journey.dispose);
        await journey.connect();
        await journey.seed();
        await journey.finish(
          journey.controller.setBrush(color: 0xff7430, targetMask: 15),
        );
        await ble.command(tester, {'op': 'store_worker', 'enabled': false});
        final before = await journey.store();
        final writesBefore = ble.writes.length;
        final prior = journey.controller.documentSession.document;
        final edit = action == 'add'
            ? journey.controller.addAnimationFrame()
            : journey.controller.deleteAnimationFrame();
        bool? accepted;
        final terminal = edit.then<Object?>((value) {
          accepted = value;
          return null;
        }, onError: (Object error, StackTrace _) => error);
        // Both source/neighbor frames are cached in this seed. Layout accepts
        // in live RAM; the paused worker holds its separate durable publication.
        await journey.finish(terminal);
        final held = await journey.store();
        expect(before['dirty'], isFalse);
        expect(accepted, isTrue);
        expect(held['dirty'], isTrue);
        expect(held['live_revision'], (before['live_revision'] as int) + 1);
        expect(
          journey.controller.documentSession.document.frames.length,
          prior.frames.length + (action == 'add' ? 1 : -1),
        );
        Set<String> frameEdits() => ble.writes
            .skip(writesBefore)
            .where((write) => write['channel'] == 'connection_session')
            .map(
              (write) => ConnectionSessionCodec.decode(
                Uint8List.fromList(write['bytes'] as List<int>),
              ),
            )
            .whereType<ConnectionSessionPacket>()
            .where(
              (packet) =>
                  packet.kind == ConnectionSessionKind.ordered &&
                  packet.code ==
                      ConnectionSession.litchcraftStoreFrameEditOpcode,
            )
            .map((packet) => '${packet.token}:${packet.sequence}')
            .toSet();
        final originalEdit = frameEdits();
        expect(originalEdit, hasLength(1));
        expect(held['root_generation'], before['root_generation']);
        expect(held['frame_count'], before['frame_count']);
        debugPrint(
          'MODE_ORDER $action held live_revision=${held['live_revision']} '
          'dirty=${held['dirty']} durable_frames=${held['frame_count']} '
          'root=${held['root_generation']} next=Exit with save worker paused',
        );
        final intended = journey.controller.documentSession.document;
        expect(sameLitchcraftDocument(intended, prior), isFalse);
        await journey.finish(journey.controller.leave());
        await journey.mode(false);
        expect(
          sameLitchcraftDocument(
            journey.controller.documentSession.document,
            intended,
          ),
          isTrue,
        );
        await ble.command(tester, {'op': 'store_worker', 'enabled': true});
        Object? completion;
        await journey.finish(terminal.then((value) => completion = value));
        expect(
          completion,
          isNull,
          reason: 'An accepted live edit survives departure',
        );
        await ble.advance(tester, 500);
        final published = await journey.store();
        expect(published['structural_pending'], isFalse);
        expect(published['dirty'], isFalse);
        expect(published['frame_count'], intended.frames.length);
        expect(
          published['root_generation'],
          (before['root_generation'] as int) + 1,
          reason: 'The accepted layout publishes exactly once after departure',
        );
        debugPrint(
          'MODE_ORDER $action departed dirty=${published['dirty']} '
          'durable_frames=${published['frame_count']} '
          'root=${published['root_generation']} next=reenter without replay',
        );
        expect((await journey.frame())['litchcraft_active'], isFalse);
        await journey.finish(journey.controller.enter());
        await journey.mode(true);
        await journey.until(
          () => !journey.controller.pending && !ble.hasPendingOperations,
        );
        expect(
          sameLitchcraftDocument(
            journey.controller.documentSession.document,
            intended,
          ),
          isTrue,
        );
        expect(journey.controller.status, LitchcraftStatus.live);
        expect(
          (await journey.store())['root_generation'],
          published['root_generation'],
          reason: 'Reentry does not republish the already accepted layout',
        );
        await journey.until(
          () =>
              !journey.controller.pending &&
              !journey.controller.boardArtworkUnresolved,
        );
        expect(
          sameLitchcraftDocument(
            journey.controller.documentSession.document,
            intended,
          ),
          isTrue,
        );
        await journey.paintAfterReturn();
        expect(
          frameEdits(),
          originalEdit,
          reason:
              'Returning may not repeat an accepted layout edit under a new identity',
        );
        journey.checkpoint('$action-depart-return', await journey.frame());
        await journey.settle();
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  for (final mate in [true, false]) {
    testWidgets(
      'Play shows ${mate ? "Fool’s Mate" : "a threefold draw"} and reopens the game after Undo',
      (tester) async {
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = VirtualLitchGattBle(native, operationIntervalMs: 20);
        final journey = _Journey(tester, ble);
        addTearDown(journey.dispose);
        await journey.connect();
        await tester.pumpWidget(
          MaterialApp(
            home: LitchMainScaffold(
              bleService: journey.service,
              boardId: _boardId,
              boards: const [],
              onDisconnect: journey.service.disconnect,
              craftController: journey.controller,
              craftLibrary: journey.library,
              loadAppConsolePreference: () async => false,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );
        final playBoard = find.byType(HomeChessboard);
        final mapping = ((await journey.frame())['logical_to_physical'] as List)
            .cast<int>();
        var occupancy = BigInt.parse('ffff00000000ffff', radix: 16);
        Future<void> observe(int delayMs) async {
          var physical = BigInt.zero;
          for (var square = 0; square < 64; square++) {
            if ((occupancy & (BigInt.one << square)) != BigInt.zero) {
              physical |= BigInt.one << mapping[square];
            }
          }
          await ble.command(tester, {
            'op': 'hall',
            'mask': physical.toRadixString(16).padLeft(16, '0'),
          });
          await ble.advance(tester, delayMs);
        }

        Future<void> move(int from, int to) async {
          occupancy &= ~(BigInt.one << from);
          await observe(40);
          occupancy |= BigInt.one << to;
          await observe(2200);
        }

        await observe(2500);
        await journey.until(() => playBoard.evaluate().isNotEmpty);
        // f3 e5 g4 Qh4#, or two repetitions of Nf3 Nf6 Ng1 Ng8.
        const knightCycle = [(62, 45), (6, 21), (45, 62), (21, 6)];
        final moves = mate
            ? [(53, 45), (12, 28), (54, 38), (3, 39)]
            : [...knightCycle, ...knightCycle];
        for (final (index, (from, to)) in moves.indexed) {
          await move(from, to);
          if (index == 1) {
            await journey.until(
              () => find.text('Classic · Move 2').evaluate().isNotEmpty,
            );
          }
        }
        await journey.finish(
          journey.service.requestCurrentHomeStateSnapshot(_boardId),
        );
        HomeBoardSnapshot onPlay() =>
            tester.widget<HomeChessboard>(playBoard).snapshot;
        await journey.until(() => onPlay().gameStatus?.isFinished == true);
        final result = onPlay().gameStatus!;
        expect(result.outcome, mate ? 'checkmate' : 'draw');
        expect(result.winner, mate ? 'black' : 'none');
        expect(find.text(mate ? 'P2 wins' : 'Draw'), findsOneWidget);
        final finalMoveNumber = mate ? 3 : 5;
        expect(onPlay().position?.fullmoveNumber, finalMoveNumber);

        final undo = find.byKey(const ValueKey('control:undo'));
        await tester.ensureVisible(undo);
        await tester.tap(undo);
        await journey.until(
          () => journey.service.latestHomeState?.undoPending == true,
        );
        await move(moves.last.$2, moves.last.$1);
        await journey.finish(
          journey.service.requestCurrentHomeStateSnapshot(_boardId),
        );
        final page = Scrollable.of(
          tester.element(playBoard),
          axis: Axis.vertical,
        );
        page.position.jumpTo(page.position.minScrollExtent);
        await tester.pump();
        await journey.until(
          () =>
              onPlay().gameStatus?.isFinished == false &&
              onPlay().physicalPositionMatches == true &&
              find.text('P2 to move').evaluate().isNotEmpty,
        );
        expect(onPlay().position?.sideToMove, HomeSide.black);
        expect(find.text('P2 to move'), findsOneWidget);
        expect(onPlay().position?.fullmoveNumber, finalMoveNumber - 1);
        expect(find.text('Classic · Move ${finalMoveNumber - 1}'), findsOneWidget);
        await journey.settle();
      },
      variant: TargetPlatformVariant({
        TargetPlatform.android,
        TargetPlatform.iOS,
      }),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
