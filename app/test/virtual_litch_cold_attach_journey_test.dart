import 'dart:async';
import 'dart:io';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart'
    show ConnectionStateUpdate, DeviceConnectionState;
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litch_board.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/litchcraft_artwork.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/screens/connection_gate_screen.dart';
import 'package:litch_mobile/src/screens/home_screen.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_frame_store.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'support/litchcraft_frame_strip_gestures.dart';
import 'support/litchcraft_packet_observers.dart';
import 'support/memory_ota_target_store.dart';
import 'virtual_litch/native_process.dart';
import 'virtual_litch/sdk_ble.dart';

const _boardId = 'virtual-litch-production';

Iterable<Uint8List> _craftFrames(Iterable<List<int>> packets) sync* {
  final assembler = ConnectionLitchcraftAssembler();
  for (final bytes in packets) {
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(bytes));
    if (packet?.kind == ConnectionSessionKind.ordered &&
        packet?.code == ConnectionSession.litchcraftModeOpcode) {
      yield packet!.body;
      continue;
    }
    if (packet?.kind != ConnectionSessionKind.object ||
        packet?.code != ConnectionLitchcraftCodec.objectOpcode) {
      continue;
    }
    final frame = assembler.accept(packet!.body);
    if (frame != null) yield frame;
  }
}

// Identify complete Store body Objects, not Begin or a native batch size.
// This high-MTU fixture's client shapes each chunk to one shared Object.
ConnectionSessionPacket? _storeBodyObject(List<int> bytes) {
  final packet = ConnectionSessionCodec.decode(Uint8List.fromList(bytes));
  if (packet?.kind != ConnectionSessionKind.object ||
      packet?.code != ConnectionLitchcraftCodec.objectOpcode) {
    return null;
  }
  final body = packet!.body;
  return body.length >= 7 &&
          body[0] == ConnectionLitchcraftCodec.version &&
          body[1] ==
              (ConnectionLitchcraftCodec.beginFlag |
                  ConnectionLitchcraftCodec.endFlag) &&
          body[4] == LitchcraftV2Protocol.magic &&
          body[5] == LitchcraftV2Protocol.version &&
          body[6] == LitchcraftFrameStoreProtocol.storeChunk
      ? packet
      : null;
}

int? _objectGrantBeforeBody(
  Iterable<VirtualPacket> emitted,
  ConnectionSessionPacket firstBody,
) {
  int? grant;
  for (final raw in emitted) {
    if (raw.channel != 'connection_session') continue;
    final packet = ConnectionSessionCodec.decode(Uint8List.fromList(raw.bytes));
    if (packet?.kind != ConnectionSessionKind.control) continue;
    final data = ByteData.sublistView(packet!.body);
    int? next;
    if (packet.code == ConnectionSessionControl.opened &&
        packet.body[7] == 1 &&
        data.getUint32(0, Endian.little) == firstBody.token) {
      next = packet.body[6];
    } else if (packet.code == ConnectionSessionControl.progress &&
        packet.token == firstBody.token &&
        packet.sequence < firstBody.sequence) {
      next = data.getUint32(4, Endian.little);
    }
    if (next != null && (grant == null || next > grant)) grant = next;
  }
  return grant;
}

// Only the cold-attach scenarios control the platform MTU callback.
class _StartupBle extends VirtualLitchGattBle {
  _StartupBle(super.native, {required super.negotiatedMtu});

  Completer<int>? mtuResult;
  bool failMtu = false;
  int mtuRequests = 0;
  bool holdInspectReplies = false;
  final inspectReplies = <VirtualPacket>[];

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final envelope = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdInspectReplies &&
        envelope != null &&
        envelope.kind == ConnectionSessionKind.object &&
        envelope.code == ConnectionLitchcraftCodec.receiptCode &&
        envelope.body.length > 4 &&
        envelope.body[0] == ConnectionLitchcraftCodec.version &&
        envelope.body[1] ==
            (ConnectionLitchcraftCodec.beginFlag |
                ConnectionLitchcraftCodec.endFlag)) {
      final reply = LitchcraftV2Packet.decode(envelope.body.sublist(4));
      if (reply != null &&
          reply.event == LitchcraftV2Protocol.state &&
          reply.requestId != 0) {
        inspectReplies.add(packet);
        return;
      }
    }
    super.deliver(packet, lateCallback: lateCallback);
  }

  void releaseInspectReplies() {
    holdInspectReplies = false;
    final held = List<VirtualPacket>.of(inspectReplies);
    inspectReplies.clear();
    for (final packet in held) {
      deliver(packet);
    }
  }

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) {
    ++mtuRequests;
    if (failMtu) {
      return Future.error(StateError('controlled MTU callback deadline'));
    }
    return mtuResult?.future.then(mtuCallbackValue) ??
        super.requestMtu(deviceId: deviceId, mtu: mtu);
  }
}

// Native info writes and replies stay real; only the iOS callback readiness
// boundary drops responses before the notification listener becomes usable.
class _NotificationStartupBle extends _StartupBle {
  _NotificationStartupBle(super.native) : super(negotiatedMtu: 247);
  bool notificationReady = true;
  int droppedCapabilityReplies = 0;

  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    if (!notificationReady && packet.channel == 'ota_control') {
      droppedCapabilityReplies++;
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }
}

// Only OS discovery and optional capability completion are controlled here.
// Replies, connect/disconnect, mode, session and Store remain production-owned.
class _LifecycleService extends LitchBleService {
  _LifecycleService(_StartupBle ble, {super.appTrace})
    : super(
        ble: ble,
        gattSessionFactory: ReactiveBleGattSessionFactory(ble),
        pendingOtaStore: MemoryOtaTargetStore(),
      );
  final _boards = StreamController<List<LitchBoard>>.broadcast(sync: true);
  Completer<void>? capabilityGate;
  bool discoverBoard = false;
  int capabilityCalls = 0;
  Completer<void>? orientationGate;
  int orientationReads = 0;

  @override
  Object? latestSettingValue(String key) =>
      key == 'BOARD_ROTATE' && orientationGate != null
      ? null
      : super.latestSettingValue(key);

  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    final reading = super.readSettingValue(deviceId: deviceId, key: key);
    reading.ignore();
    if (key == 'BOARD_ROTATE' && orientationGate != null) {
      ++orientationReads;
      await orientationGate!.future;
    }
    return reading;
  }

  @override
  Stream<List<LitchBoard>> get boardsStream => _boards.stream;
  @override
  BleAvailability get bleAvailability => BleAvailability.ready;
  @override
  Future<void> startScan() async {
    if (!discoverBoard) return;
    _boards.add(const [
      LitchBoard(id: _boardId, name: 'Foxie Store', rssi: -40),
    ]);
  }

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    ++capabilityCalls;
    await capabilityGate?.future;
    return super.fetchDeviceCapabilities(deviceId: deviceId, refresh: refresh);
  }

  @override
  Future<void> dispose() async {
    await super.dispose();
    await _boards.close();
  }
}

// Existing SDK receipt hold, shared by the mixed mounted journey. Only the
// genuine Paint checkpoint is delayed; native application and State stay real.
class _WorkloadBle extends _StartupBle {
  _WorkloadBle(super.native) : super(negotiatedMtu: 247);
  bool holdPaint = false;
  bool holdObjectProgress = false;
  int batchesFrom = 0;
  final objectProgress = <VirtualPacket>[];
  void releaseObjectProgress() {
    holdObjectProgress = false;
    final pending = List<VirtualPacket>.of(objectProgress);
    objectProgress.clear();
    for (final packet in pending) {
      deliver(packet);
    }
  }

  final paintReceipts = <VirtualPacket>[];
  @override
  void deliver(VirtualPacket packet, {bool lateCallback = false}) {
    final p = packet.channel == 'connection_session'
        ? ConnectionSessionCodec.decode(Uint8List.fromList(packet.bytes))
        : null;
    if (holdObjectProgress &&
        p?.kind == ConnectionSessionKind.control &&
        p?.code == ConnectionSessionControl.progress &&
        (() {
          final producer = exactBatches
              .skip(batchesFrom)
              .expand((batch) => batch.values)
              .map(_storeBodyObject)
              .whereType<ConnectionSessionPacket>()
              .take(LitchcraftFrameStoreClient.maximumNativeBatchPackets)
              .toList();
          // Let earlier real credits cross the smaller receiver window.
          // Withhold admission only once the complete first producer can
          // return natively; Begin's earlier grant must still pass.
          return producer.length ==
                  LitchcraftFrameStoreClient.maximumNativeBatchPackets &&
              producer.first.token == p?.token &&
              producer.last.sequence <= p!.sequence;
        })()) {
      objectProgress.add(packet);
      return;
    }
    if (holdPaint &&
        p?.kind == ConnectionSessionKind.control &&
        p?.code == ConnectionSessionControl.checkpoint &&
        writes.any((w) {
          if (w['channel'] != 'connection_session') return false;
          final q = ConnectionSessionCodec.decode(
            Uint8List.fromList(w['bytes'] as List<int>),
          );
          return q?.kind == ConnectionSessionKind.ordered &&
              q?.code == ConnectionSession.litchcraftPaintOpcode &&
              q?.token == p?.token &&
              q!.sequence <= p!.sequence;
        })) {
      paintReceipts.add(packet);
      return;
    }
    super.deliver(packet, lateCallback: lateCallback);
  }

  void releasePaint() {
    holdPaint = false;
    final pending = List<VirtualPacket>.of(paintReceipts);
    paintReceipts.clear();
    for (final packet in pending) {
      deliver(packet);
    }
  }
}

class _WorkloadTrace implements AppTraceSink {
  final events = <({String event, Map<String, Object?> fields})>[];
  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    events.add((event: event, fields: Map.of(fields)));
    if (event == 'litchcraft.object_submission' ||
        event == 'litchcraft.object_transfer') {
      debugPrint(
        'CRAFT_WORKLOAD_TRACE ${jsonEncode({'event': event, ...fields})}',
      );
    }
  }
}

void main() {
  testWidgets(
    'current Store adoption survives iOS capability notification startup loss',
    (tester) async {
      SharedPreferences.setMockInitialValues({'enable_app_console': false});
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      var ble = _NotificationStartupBle(native);
      var service = _LifecycleService(ble);
      var controller = LitchcraftController(service);
      Future<void> tick() async {
        await tester.pump();
        if (ble.hasPendingOperations) {
          await ble.flush(tester, maxOperations: 1);
        } else {
          await ble.advance(tester, 10);
        }
      }

      Future<void> until(bool Function() done) async {
        final start = tester.binding.clock.now();
        while (!done() &&
            tester.binding.clock.now().difference(start) <
                const Duration(seconds: 8)) {
          await tick();
        }
        expect(done(), isTrue, reason: 'current native startup must settle');
      }

      Future<T> finish<T>(Future<T> work) async {
        var done = false;
        T? result;
        Object? failure;
        work.then(
          (value) {
            result = value;
            done = true;
          },
          onError: (Object error, StackTrace _) {
            failure = error;
            done = true;
          },
        );
        await until(() => done);
        if (failure != null) throw failure!;
        return result as T;
      }

      var cleanupStarted = false;
      Future<void> cleanupJourney() async {
        cleanupStarted = true;
        ble.notificationReady = true;
        controller.dispose();
        try {
          await finish(service.dispose());
          await ble.flush(tester);
          await finish(ble.close());
        } finally {
          await tester.runAsync(native.close);
        }
      }

      addTearDown(() {
        if (!cleanupStarted) return cleanupJourney();
      });
      try {
        await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
        await ble.advance(tester, 1000);
        await finish(service.connect(_boardId));
        await finish(controller.enter());
        final saved = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([63], 15, 0x123456),
        );
        await finish(controller.replaceDocument(saved));
        await until(
          () => !controller.boardArtworkUnresolved && !controller.pending,
        );
        await finish(service.disconnect());
        controller.dispose();
        await finish(service.dispose());
        await ble.flush(tester);
        await finish(ble.close());

        final trace = _WorkloadTrace();
        ble = _NotificationStartupBle(native)..notificationReady = false;
        service = _LifecycleService(ble, appTrace: trace);
        controller = LitchcraftController(service);
        final connection = service.connect(_boardId);
        await until(() => service.isConnected);
        final adoption = controller.adoptActiveBoard();
        await until(() => ble.droppedCapabilityReplies == 1);
        await tester.pump(const Duration(milliseconds: 400));
        expect(controller.boardActive, isFalse);
        ble.notificationReady = true;
        expect(await finish(connection), isTrue);
        expect(await finish(adoption), isTrue);
        await until(
          () => !controller.boardArtworkUnresolved && !controller.pending,
        );

        final capabilities = service.deviceCapabilities(_boardId);
        expect(capabilities.contractVersion, 3);
        expect(capabilities.supportsLitchcraftAnimation, isTrue);
        expect(controller.selected, isTrue);
        expect(controller.boardActive, isTrue);
        expect(controller.status, LitchcraftStatus.live);
        expect(
          sameLitchcraftDocument(controller.documentSession.document, saved),
          isTrue,
        );
        expect(
          trace.events
              .where((event) => event.event == 'ble.capability_result')
              .map((event) => event.fields['outcome']),
          ['timeout', 'reported'],
        );
        final craftCommands = _craftFrames(
          ble.writes
              .where((write) => write['channel'] == 'connection_session')
              .map((write) => write['bytes'] as List<int>),
        ).toList();
        final modeInspections = craftCommands.where(isLitchcraftModeDiscovery);
        expect(modeInspections, hasLength(1));
        expect(
          craftCommands.where(isLitchcraftSourceClockInspection),
          hasLength(lessThanOrEqualTo(1)),
        );
        expect(
          craftCommands.where(
            (packet) =>
                packet[0] == LitchcraftProtocol.magic &&
                packet[1] == LitchcraftProtocol.enter,
          ),
          isEmpty,
        );
        expect(
          craftCommands.where(
            (packet) =>
                packet.length == 10 &&
                packet[0] == LitchcraftV2Protocol.magic &&
                packet[2] == LitchcraftV2Protocol.inspect &&
                packet[9] == 7,
          ),
          isNotEmpty,
        );
        expect(controller.usesFrameStoreDocument, isTrue);
        await finish(controller.setBrush(color: 0x66cc44));
        controller.beginStroke();
        controller.paintSquares([7]);
        controller.endStroke();
        await until(
          () => !controller.pending && !controller.boardArtworkUnresolved,
        );
        Future<Map<String, dynamic>> readStored() async =>
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        // Applied paint is live RAM; this existing diagnostic exposes frame
        // bytes only after ReadCleanStoredFrame permits durable readback.
        var board = await readStored();
        for (
          var attempt = 0;
          attempt < 80 &&
              (board['dirty'] != false || board['emitters'] is! List);
          attempt++
        ) {
          await ble.advance(tester, 100);
          board = await readStored();
        }
        expect(controller.status, LitchcraftStatus.live);
        expect(board['dirty'], isFalse);
        expect(board['emitters'], isA<List<dynamic>>());
        expect((board['emitters'] as List<dynamic>)[28][0], 0x66cc44);
        expect(
          controller.documentSession.document.frames[0].emitters[28].color,
          0x66cc44,
        );
      } finally {
        await cleanupJourney();
      }
    },
    variant: TargetPlatformVariant.only(TargetPlatform.iOS),
  );

  for (final active in [true, false]) {
    testWidgets(
      'passive native mode ${active ? 'Craft' : 'Play'} precedes local draft and orientation',
      (tester) async {
        SharedPreferences.setMockInitialValues({
          'enable_app_console': false,
          'litchcraft_discovery_dismissed': true,
        });
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        var ble = _StartupBle(native, negotiatedMtu: 247);
        var service = _LifecycleService(ble)..discoverBoard = true;
        var controller = LitchcraftController(service);
        LitchcraftLibrary? library;
        final draftGate = Completer<LitchcraftArtworkStore>();
        final orientation = Completer<void>();
        Completer<void>? freshOrientation;
        Future<void>? readinessRepair;
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
          final start = tester.binding.clock.now();
          while (!done() &&
              tester.binding.clock.now().difference(start) <
                  const Duration(seconds: 8)) {
            await tick();
          }
          expect(
            done(),
            isTrue,
            reason:
                'active=$active inspected=${controller.inspectedBoardActive} status=${controller.status} orientation_reads=${service.orientationReads} error=${controller.errorMessage}',
          );
        }

        Future<void> finish(Future<dynamic> work) async {
          var done = false;
          Object? failure;
          unawaited(
            work.then<void>(
              (_) => done = true,
              onError: (Object error, StackTrace _) {
                failure = error;
                done = true;
              },
            ),
          );
          await until(() => done);
          if (failure != null) throw failure!;
        }

        late LitchcraftArtworkStore store;
        var cleaned = false;
        Future<void> cleanup() async {
          if (cleaned) return;
          cleaned = true;
          if (!draftGate.isCompleted) draftGate.complete(store);
          if (!orientation.isCompleted) orientation.complete();
          if (freshOrientation case final held?) {
            if (!held.isCompleted) held.complete();
          }
          ble.releaseInspectReplies();
          if (readinessRepair case final repair?) {
            await finish(repair.catchError((Object _) {}));
          }
          await tester.pumpWidget(const SizedBox.shrink());
          if (library != null) await finish(library.ready);
          library?.dispose();
          controller.dispose();
          try {
            await finish(service.dispose());
            await ble.flush(tester);
            await finish(ble.close());
          } finally {
            await tester.runAsync(native.close);
          }
        }

        addTearDown(cleanup);
        store = LitchcraftArtworkStore(await SharedPreferences.getInstance());
        await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
        await ble.advance(tester, 1000);
        await finish(service.connect(_boardId));
        await finish(controller.enter());
        final boardArtwork = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([63], 15, 0x123456),
        );
        await finish(controller.replaceDocument(boardArtwork));
        await until(
          () => !controller.pending && !controller.boardArtworkUnresolved,
        );
        if (!active) await finish(controller.leave());
        await finish(service.disconnect());
        controller.dispose();
        await finish(service.dispose());
        await ble.flush(tester);
        await finish(ble.close());

        final localArtwork = LitchcraftArtwork(
          id: 'local-draft',
          name: 'Saved phone draft',
          canvas: LitchcraftCanvas.blank().paint([0], 15, 0xabcdef),
          createdAt: DateTime.utc(2026),
          modifiedAt: DateTime.utc(2026),
        );
        await store.save(
          LitchcraftLibrarySnapshot(
            artworks: [localArtwork],
            draft: localArtwork,
            draftOrigin: LitchcraftDraftOrigin.saved,
            dismissedWelcome: true,
          ),
        );
        ble = _StartupBle(native, negotiatedMtu: 247);
        service = _LifecycleService(ble)
          ..discoverBoard = true
          ..orientationGate = orientation;
        controller = LitchcraftController(service);
        library = LitchcraftLibrary(
          controller,
          storeLoader: () => draftGate.future,
        );
        await finish(service.connect(_boardId));
        final writesFrom = ble.writes.length;
        expect((await ble.frame(tester))['litchcraft_active'], active);
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: controller,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );
        await until(() => controller.inspectedBoardActive == active);
        await until(() => find.byType(NavigationBar).evaluate().isNotEmpty);
        final modeWrites = _craftFrames(
          ble.writes
              .skip(writesFrom)
              .where((write) => write['channel'] == 'connection_session')
              .map((write) => write['bytes'] as List<int>),
        ).toList();
        final inspections = modeWrites.where(
          (packet) =>
              (packet.length >= 3 &&
                  packet[0] == LitchcraftV2Protocol.magic &&
                  packet[1] == LitchcraftV2Protocol.version &&
                  packet[2] == LitchcraftV2Protocol.inspect) ||
              (packet.length >= 2 &&
                  packet[0] == LitchcraftProtocol.magic &&
                  packet[1] == LitchcraftProtocol.inspect),
        );
        expect(
          inspections,
          hasLength(1),
          reason: 'Mode must come from one genuine native inspection',
        );
        expect(
          modeWrites,
          hasLength(1),
          reason: 'No artwork or mode mutation is allowed before prerequisites',
        );
        expect(service.orientationReads, 0);
        expect(draftGate.isCompleted, isFalse);
        expect(find.byType(NavigationBar), findsOneWidget);
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          active ? 1 : 0,
        );
        if (active) {
          expect(controller.boardArtworkUnresolved, isTrue);
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .enabled,
            isFalse,
          );
          await until(
            () => find.text('Preparing artwork…').evaluate().isNotEmpty,
          );
          draftGate.complete(store);
          await until(() => service.orientationReads == 1);
          expect(controller.boardArtworkUnresolved, isTrue);
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .enabled,
            isFalse,
          );
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              localArtwork.document,
            ),
            isTrue,
          );
          await tester.tap(find.byKey(const ValueKey('main-destination-play')));
          await until(() => !controller.boardActive);
          expect((await ble.frame(tester))['litchcraft_active'], isFalse);
        } else {
          expect(controller.boardActive, isFalse);
        }
        // A retained controller reconnects before the old prerequisite returns.
        // This new current-link inactive proof must not wait on that old read.
        await finish(service.disconnect());
        await finish(service.connect(_boardId));
        await until(() => controller.inspectedBoardActive == false);
        // Native mode proof can follow the tick's Flutter frame. Present its
        // current-link gate before tapping, with the old prerequisites held.
        final setup = find.byKey(const ValueKey('main-destination-setup'));
        await until(() => setup.hitTestable().evaluate().isNotEmpty);
        expect(orientation.isCompleted, isFalse);
        expect(draftGate.isCompleted, active);
        await tester.tap(setup.hitTestable());
        await tester.pump();
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          2,
        );
        if (!draftGate.isCompleted) draftGate.complete(store);
        orientation.complete();
        await finish(library.ready);
        await ble.flush(tester);
        for (var i = 0; i < 20; ++i) {
          await tick();
        }
        expect(
          tester
              .widget<NavigationBar>(find.byType(NavigationBar))
              .selectedIndex,
          2,
        );
        expect(controller.inspectedBoardActive, isFalse);
        expect(controller.selected, isFalse);
        expect(
          sameLitchcraftDocument(
            controller.documentSession.document,
            localArtwork.document,
          ),
          isTrue,
        );
        final commands = _craftFrames(
          ble.writes
              .skip(writesFrom)
              .where((write) => write['channel'] == 'connection_session')
              .map((write) => write['bytes'] as List<int>),
        );
        expect(
          commands.where(
            (packet) =>
                packet.length >= 2 &&
                packet[0] == LitchcraftProtocol.magic &&
                packet[1] == LitchcraftProtocol.enter,
          ),
          isEmpty,
        );
        final stored =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        expect(
          (stored['emitters'] as List<dynamic>).map((emitter) => emitter[0]),
          boardArtwork.frames[0].emitters.map((emitter) => emitter.color),
        );
        expect(stored['active'], isFalse);
        if (active) {
          // Complete a deliberate return too: the saved nonblank phone draft
          // and different board artwork legitimately require a source choice.
          await tester.tap(
            find.byKey(const ValueKey('main-destination-craft')),
          );
          await until(() => controller.status == LitchcraftStatus.conflict);
          await until(
            () => find
                .byKey(const ValueKey('craft-resolve-conflict'))
                .evaluate()
                .isNotEmpty,
          );
          await tester.tap(
            find.byKey(const ValueKey('craft-resolve-conflict')),
          );
          await until(
            () => find
                .byKey(const ValueKey('craft-use-app-artwork'))
                .hitTestable()
                .evaluate()
                .isNotEmpty,
          );
          await tester.tap(find.byKey(const ValueKey('craft-use-app-artwork')));
          await until(
            () =>
                controller.status == LitchcraftStatus.live &&
                !controller.boardArtworkUnresolved &&
                controller.fullAnimationSyncDirection == null,
          );
          // Native completion followed the tick's frame. Present the editor's
          // new state before inspecting its canvas readiness.
          await tester.pump();
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .enabled,
            isTrue,
          );
          final completed =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(
            (completed['emitters'] as List<dynamic>).map(
              (emitter) => emitter[0],
            ),
            localArtwork.canvas.colors,
          );
          expect(completed['active'], isTrue);
          // A real physical edit invalidates the first compact inspection
          // while orientation is held. Retrying must keep the same embargo
          // while its required fresh Inspect reply is still in flight.
          freshOrientation = Completer<void>();
          service.orientationGate = freshOrientation;
          final readsBeforeRepair = service.orientationReads;
          readinessRepair = controller.retryConnection();
          readinessRepair.ignore();
          await until(() => service.orientationReads > readsBeforeRepair);
          await ble.command(tester, {'op': 'hall', 'mask': '0000002000000000'});
          await ble.advance(tester, 30);
          await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
          await ble.advance(tester, 400);
          ble.holdInspectReplies = true;
          freshOrientation.complete();
          await until(() => ble.inspectReplies.isNotEmpty);
          expect(
            controller.boardArtworkUnresolved,
            isTrue,
            reason: 'Fresh Inspect cannot reopen edits before rich authority',
          );
          expect(
            tester
                .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
                .enabled,
            isFalse,
          );
          final protectedDraft = controller.documentSession.document;
          final writesBeforeGuardChecks = ble.writes.length;
          controller.beginStroke();
          controller.paintSquares([2]);
          controller.endStroke();
          await finish(controller.rotateArtwork(1));
          await finish(controller.toggleAnimationPlayback());
          expect(
            sameLitchcraftDocument(
              controller.documentSession.document,
              protectedDraft,
            ),
            isTrue,
          );
          final orderedMutations = ble.writes
              .skip(writesBeforeGuardChecks)
              .where((write) => write['channel'] == 'connection_session')
              .map(
                (write) => ConnectionSessionCodec.decode(
                  Uint8List.fromList(write['bytes'] as List<int>),
                ),
              )
              .whereType<ConnectionSessionPacket>()
              .where((packet) => packet.kind == ConnectionSessionKind.ordered);
          expect(
            orderedMutations,
            isEmpty,
            reason:
                'Programmatic Paint/rotate/Play also obey unresolved authority',
          );
          ble.releaseInspectReplies();
          await finish(readinessRepair);
          await until(
            () => !controller.pending && !controller.boardArtworkUnresolved,
          );
          expect(
            controller.status,
            anyOf(LitchcraftStatus.live, LitchcraftStatus.conflict),
          );
        }
        await cleanup();
      },
      variant: TargetPlatformVariant({TargetPlatform.android}),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }

  testWidgets(
    'closed native Store capacity keeps rapid Craft edits and Exit recoverable',
    (tester) async {
      const seed = 0xc105ed;
      SharedPreferences.setMockInitialValues({
        'enable_app_console': false,
        'litchcraft_discovery_dismissed': true,
      });
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _StartupBle(native, negotiatedMtu: 247);
      final trace = _WorkloadTrace();
      final service = _LifecycleService(ble, appTrace: trace)
        ..discoverBoard = true;
      final controller = LitchcraftController(service, appTrace: trace);
      final library = LitchcraftLibrary(controller);
      var cleaned = false;
      var primaryFailure = false;
      Object? cleanupFailure;
      StackTrace? cleanupStack;
      final observedWork = <Future<void>>[];
      void observe(Future<void> work) {
        observedWork.add(
          work.then<void>((_) {}, onError: (Object _, StackTrace _) {}),
        );
      }

      void recordCleanupFailure(String step, Object error, StackTrace stack) {
        cleanupFailure ??= error;
        cleanupStack ??= stack;
        debugPrint(
          'CRAFT_CAPACITY_CLEANUP step=$step kind=${error.runtimeType}',
        );
      }

      Future<void> tick() async {
        await tester.pump();
        if (ble.hasPendingOperations) {
          try {
            await ble.flush(tester, maxOperations: 1);
          } catch (error, stack) {
            if (!cleaned) rethrow;
            // The failed SDK operation has been dequeued. Keep joining the
            // observed producers before retiring their native process.
            recordCleanupFailure('sdk-drain', error, stack);
          }
        } else {
          await ble.command(tester, {
            'op': 'advance',
            'ms': 10,
          }, flushOperations: false);
          await tester.pump(const Duration(milliseconds: 10));
        }
      }

      Future<void> until(bool Function() done) async {
        final start = tester.binding.clock.now();
        while (!done() &&
            tester.binding.clock.now().difference(start) <
                const Duration(seconds: 20)) {
          await tick();
        }
        expect(
          done(),
          isTrue,
          reason:
              'status=${controller.status} pending=${controller.pending} '
              'error=${controller.errorMessage}',
        );
      }

      Future<void> finish(Future<dynamic> work) async {
        var done = false;
        Object? failure;
        unawaited(
          work.then<void>(
            (_) => done = true,
            onError: (Object error, StackTrace _) {
              failure = error;
              done = true;
            },
          ),
        );
        await until(() => done);
        if (failure != null) throw failure!;
      }

      Future<Map<String, dynamic>> store(int index) async =>
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': index,
              }))['store_frame']
              as Map<String, dynamic>;
      Iterable<ConnectionSessionPacket> packets(
        Iterable<List<int>> values,
      ) sync* {
        for (final bytes in values) {
          final packet = ConnectionSessionCodec.decode(
            Uint8List.fromList(bytes),
          );
          if (packet != null) yield packet;
        }
      }

      Future<void> cleanup() async {
        if (cleaned) return;
        cleaned = true;
        Future<void> step(String name, Future<void> Function() action) async {
          try {
            await action();
          } catch (error, stack) {
            recordCleanupFailure(name, error, stack);
          }
        }

        await step('resume-ingress', () async {
          await ble.command(tester, {
            'op': 'store_ingress',
            'enabled': true,
          }, flushOperations: false);
        });
        await step('join-producers', () => finish(Future.wait(observedWork)));
        await step('drain-native', () async {
          await ble.flush(tester);
          expect(native.pendingRequestCount, 0);
        });
        await step('unmount', () => tester.pumpWidget(const SizedBox.shrink()));
        await step('library-flush', () => finish(library.flush()));
        library.dispose();
        controller.dispose();
        await step('service-dispose', () => finish(service.dispose()));
        await step('sdk-flush', () => ble.flush(tester));
        await step('sdk-close', () => finish(ble.close()));
        await step('native-close', () async {
          await tester.runAsync(native.close);
        });
        if (!primaryFailure && cleanupFailure != null) {
          Error.throwWithStackTrace(cleanupFailure!, cleanupStack!);
        }
      }

      addTearDown(cleanup);
      await finish(library.ready);
      await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
      await ble.advance(tester, 1000);
      await finish(service.connect(_boardId));
      await finish(controller.enter());
      await finish(
        controller.replaceDocument(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        ),
      );
      await finish(controller.enter());
      await until(
        () => controller.status == LitchcraftStatus.live && !controller.pending,
      );
      final originalRoot = (await store(0))['root_generation'];
      ConnectionSession? session;
      await finish(
        service.openConnectionSession(_boardId).then((s) => session = s),
      );
      final metadata = controller.documentSession.document;
      final document = LitchcraftAnimationDocument(
        frames: List.generate(
          32,
          (frame) => LitchcraftAnimationFrame(
            durationMs: 100 + frame * 10,
            emitters: List.generate(
              256,
              (emitter) => LitchcraftAnimatedEmitter(
                color: (seed + frame * 123457 + emitter * 397) & 0xffffff,
              ),
            ),
          ),
        ),
        topology: metadata.topology,
        favoriteColors: metadata.favoriteColors,
        sparkleSeed: seed,
        playbackOnEntry: false,
      );
      final batchesFrom = ble.exactBatches.length;
      final writesFrom = ble.writes.length;
      final emittedFrom = ble.emitted.length;
      var uploadDone = false;
      final upload = controller.replaceDocument(document);
      observe(upload);
      unawaited(
        upload.then<void>(
          (_) => uploadDone = true,
          onError: (Object _, StackTrace _) => uploadDone = true,
        ),
      );
      // The real client submits this body batch after Begin's Object admission.
      // Pause its larger body here; emitted Progress below must prove the
      // actual closed window, regardless of any prefix already consumed.
      Iterable<ConnectionSessionPacket> nativeBodyObjects() => ble.exactBatches
          .skip(batchesFrom)
          .expand((batch) => batch.values)
          .map(_storeBodyObject)
          .whereType<ConnectionSessionPacket>();
      await until(() => nativeBodyObjects().isNotEmpty);
      final firstBody = nativeBodyObjects().first;
      final initialGrant = _objectGrantBeforeBody(ble.emitted, firstBody);
      expect(initialGrant, isNotNull);
      await ble.command(tester, {
        'op': 'store_ingress',
        'enabled': false,
      }, flushOperations: false);
      List<ConnectionSessionPacket> bodyObjects() =>
          packets(
                ble.writes
                    .skip(writesFrom)
                    .where((write) => write['channel'] == 'connection_session')
                    .map((write) => write['bytes'] as List<int>),
              )
              .where(
                (packet) =>
                    packet.kind == ConnectionSessionKind.object &&
                    packet.token == firstBody.token &&
                    packet.sequence >= firstBody.sequence,
              )
              .toList();
      ConnectionSessionPacket? closedProgress() {
        final issued = bodyObjects();
        if (issued.isEmpty || issued.last.sequence < initialGrant!) return null;
        for (final packet in packets(
          ble.emitted
              .skip(emittedFrom)
              .where((packet) => packet.channel == 'connection_session')
              .map((packet) => packet.bytes),
        ).toList().reversed) {
          if (packet.kind != ConnectionSessionKind.control ||
              packet.code != ConnectionSessionControl.progress ||
              packet.token != firstBody.token) {
            continue;
          }
          final data = ByteData.sublistView(packet.body);
          if (packet.sequence == issued.last.sequence &&
              data.getUint32(0, Endian.little) == packet.sequence &&
              data.getUint32(4, Endian.little) == packet.sequence &&
              data.getUint16(8, Endian.little) == 0) {
            return packet;
          }
        }
        return null;
      }

      await until(() => closedProgress() != null);
      final closed = closedProgress()!;
      final issuedAtClosure = bodyObjects().length;
      expect(uploadDone, isFalse);
      // Advance native/UI time and the recovery owner's real Stopwatch. All
      // issued Objects are accepted; this wait is capacity pressure, not loss.
      await ble.advance(tester, 2500);
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 2200)),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(session!.isActivated, isTrue);
      expect(service.isConnected, isTrue);
      expect(uploadDone, isFalse);
      expect(bodyObjects(), hasLength(issuedAtClosure));
      await finish(session!.heartbeat());
      debugPrint(
        'CRAFT_CAPACITY accepted=${closed.sequence} grant=${closed.sequence} '
        'issued=$issuedAtClosure wait_ms=2500 session=active next=edit-or-Exit',
      );

      final brush = controller.setBrush(
        color: 0x12ab34,
        eraser: false,
        targetMask: 15,
      );
      observe(brush);
      brush.ignore();
      controller.beginStroke();
      controller.paintSquares([0]);
      controller.endStroke();
      controller.beginStroke(erasing: true);
      controller.paintSquares([1]);
      controller.endStroke();
      final play = controller.toggleAnimationPlayback();
      observe(play);
      play.ignore();
      expect(controller.animationPlaybackPending, isTrue);
      final intended = controller.documentSession.document;
      expect(
        intended.frames[0].emitters.take(4).map((e) => e.color),
        everyElement(0x12ab34),
      );
      expect(
        intended.frames[0].emitters.skip(4).take(4).map((e) => e.color),
        everyElement(0),
      );
      final departure = controller.leave();
      observe(departure);
      try {
        await finish(departure);
      } catch (error, stack) {
        primaryFailure = true;
        final writtenCraft = _craftFrames(
          ble.writes
              .skip(writesFrom)
              .where((write) => write['channel'] == 'connection_session')
              .map((write) => write['bytes'] as List<int>),
        ).toList();
        final exits = writtenCraft
            .where(
              (packet) =>
                  packet.length >= 8 &&
                  packet[0] == LitchcraftProtocol.magic &&
                  packet[1] == LitchcraftProtocol.exit,
            )
            .toList();
        final exitRequests = exits
            .map(
              (packet) =>
                  ByteData.sublistView(packet).getUint16(6, Endian.little),
            )
            .toSet();
        final replies =
            _craftFrames(
                  ble.emitted
                      .skip(emittedFrom)
                      .where((packet) => packet.channel == 'connection_session')
                      .map((packet) => packet.bytes),
                )
                .map(LitchcraftPacket.decode)
                .whereType<LitchcraftPacket>()
                .where(
                  (packet) =>
                      packet.event == LitchcraftProtocol.state &&
                      exitRequests.contains(packet.requestId),
                )
                .length;
        final cancels =
            packets(
                  ble.writes
                      .skip(writesFrom)
                      .where(
                        (write) => write['channel'] == 'connection_session',
                      )
                      .map((write) => write['bytes'] as List<int>),
                )
                .where(
                  (packet) =>
                      packet.kind == ConnectionSessionKind.ordered &&
                      packet.code == LitchcraftFrameStoreProtocol.storeCancel,
                )
                .length;
        debugPrint(
          'CRAFT_CAPACITY_EXIT_FAILURE ordered_cancel22=$cancels '
          'written_exit=${exits.length} emitted_exit_state=$replies '
          'exit_start=${trace.events.where((e) => e.event == 'litchcraft.exit_start').length} '
          'exit_ack=${trace.events.where((e) => e.event == 'litchcraft.exit_acknowledged').length} '
          'upload_done=$uploadDone kind=${error.runtimeType}',
        );
        Error.throwWithStackTrace(error, stack);
      }
      final leavingWrites = packets(
        ble.writes
            .skip(writesFrom)
            .where((write) => write['channel'] == 'connection_session')
            .map((write) => write['bytes'] as List<int>),
      ).toList();
      expect(
        leavingWrites.where(
          (packet) =>
              packet.kind == ConnectionSessionKind.ordered &&
              packet.code == LitchcraftFrameStoreProtocol.storeCancel,
        ),
        isNotEmpty,
        reason: 'Real active transfer Cancel must bypass the full bulk FIFO',
      );
      expect((await ble.frame(tester))['litchcraft_active'], isFalse);
      expect(controller.animationPlaying, isFalse);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, intended),
        isTrue,
      );
      await ble.command(tester, {'op': 'store_ingress', 'enabled': true});
      await until(() => uploadDone);
      expect(
        (await store(0))['root_generation'],
        originalRoot,
        reason: 'Resuming cancelled ingress must not publish stale artwork',
      );
      await finish(controller.enter());
      await until(
        () =>
            controller.status == LitchcraftStatus.live &&
            !controller.pending &&
            !controller.boardArtworkUnresolved &&
            controller.fullAnimationSyncDirection == null,
      );
      expect(controller.boardArtworkUnresolved, isFalse);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, intended),
        isTrue,
      );
      for (var index = 0; index < intended.frames.length; ++index) {
        final frame = await store(index);
        expect(frame['frame_count'], 32);
        expect(frame['duration_ms'], intended.frames[index].durationMs);
        expect(frame['emitters'], [
          for (final emitter in intended.frames[index].emitters)
            [emitter.color, emitter.effect.id, emitter.effectDurationMs],
        ]);
      }
      final committedRoot = (await store(0))['root_generation'];
      await finish(service.disconnect());
      await finish(service.connect(_boardId));
      await finish(controller.enter());
      await until(
        () =>
            controller.status == LitchcraftStatus.live &&
            !controller.pending &&
            !controller.boardArtworkUnresolved &&
            controller.fullAnimationSyncDirection == null,
      );
      expect(controller.boardArtworkUnresolved, isFalse);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, intended),
        isTrue,
      );
      expect((await store(0))['root_generation'], committedRoot);
      expect(
        (await store(0))['playing'],
        isFalse,
        reason: 'Cancelled Play must not restart through reconnect',
      );
      expect((await ble.frame(tester))['litchcraft_active'], isTrue);
      debugPrint(
        'CRAFT_CAPACITY cancelled-root=$originalRoot recovered-root=$committedRoot '
        'frames=32 chooser=none reconnect=clean next=paint-or-Play',
      );
      await cleanup();
    },
    variant: TargetPlatformVariant({TargetPlatform.android}),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  testWidgets(
    'seeded mixed Craft workload preserves order across physical Undo and reconnect',
    (tester) async {
      const seed = 0xc4a7;
      SharedPreferences.setMockInitialValues({
        'enable_app_console': false,
        'litchcraft_discovery_dismissed': true,
      });
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final native = (await tester.runAsync(
        () => VirtualLitchProcess.start(1, false, hardwareV3: true),
      ))!;
      final ble = _WorkloadBle(native);
      final trace = _WorkloadTrace();
      final service = _LifecycleService(ble, appTrace: trace)
        ..discoverBoard = true;
      final controller = LitchcraftController(service, appTrace: trace);
      final library = LitchcraftLibrary(controller);
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
        final start = tester.binding.clock.now();
        while (!done() &&
            tester.binding.clock.now().difference(start) <
                const Duration(seconds: 20)) {
          await tick();
        }
        expect(
          done(),
          isTrue,
          reason:
              'seed=$seed status=${controller.status} pending=${controller.pending} error=${controller.errorMessage}',
        );
      }

      Future<void> finish(Future<dynamic> work) async {
        var done = false;
        Object? failure;
        work.then<void>(
          (_) => done = true,
          onError: (Object e, StackTrace _) {
            failure = e;
            done = true;
          },
        );
        await until(() => done);
        if (failure != null) throw failure!;
      }

      Future<Map<String, dynamic>> store(int index) async =>
          (await ble.command(tester, {
                'op': 'store_frame',
                'index': index,
              }))['store_frame']
              as Map<String, dynamic>;
      Future<void> cleanParity() async {
        await ble.advance(tester, 6000);
        await until(() => !controller.pending && !ble.hasPendingOperations);
        final doc = controller.documentSession.document;
        for (var i = 0; i < doc.frames.length; ++i) {
          final frame = await store(i);
          expect(frame['dirty'], isFalse);
          expect(frame['frame_count'], doc.frames.length);
          expect(frame['duration_ms'], doc.frames[i].durationMs);
          expect(frame['emitters'], [
            for (final e in doc.frames[i].emitters)
              [e.color, e.effect.id, e.effectDurationMs],
          ]);
          expect(frame['favorite_colors'], doc.favoriteColors);
          expect(frame['topology'], doc.topology.id);
          expect(frame['sparkle_seed'], doc.sparkleSeed);
          expect(
            frame['selected_frame'],
            controller.documentSession.selectedFrameIndex,
          );
          expect(frame['active'], isTrue);
        }
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.selected, isTrue);
        expect(controller.boardArtworkUnresolved, isFalse);
        expect(find.text('Choose artwork'), findsNothing);
      }

      Future<void> openFrames() async {
        if (find
            .byKey(const ValueKey('craft-add-frame'))
            .hitTestable()
            .evaluate()
            .isEmpty) {
          await tester.tap(find.byKey(const ValueKey('craft-animate')));
          await tester.pump(const Duration(milliseconds: 300));
        }
      }

      Future<void> checkpoint(String action) async {
        final frame = await ble.frame(tester);
        debugPrint(
          'CRAFT_WORKLOAD seed=$seed action=$action occupancy=${frame['occupancy']} active=${frame['litchcraft_active']} frames=${controller.documentSession.document.frames.length} selected=${controller.documentSession.selectedFrameIndex} pending=${controller.pending} next=continued-editor',
        );
      }

      var cleaned = false;
      Future<dynamic>? uploadForCleanup;
      Future<void> cleanup() async {
        if (cleaned) return;
        cleaned = true;
        ble.releaseObjectProgress();
        ble.releasePaint();
        ble.holdPhysicalStoreObjects = false;
        ble.heldChannels.clear();
        ble.release('connection_session');
        if (uploadForCleanup != null) {
          try {
            await finish(uploadForCleanup);
          } catch (_) {
            /* Keep transport cleanup unconditional. */
          }
        }
        await tester.pumpWidget(const SizedBox.shrink());
        await finish(library.flush());
        library.dispose();
        controller.dispose();
        try {
          await finish(service.dispose());
          await ble.flush(tester);
          await finish(ble.close());
        } finally {
          await tester.runAsync(native.close);
        }
      }

      addTearDown(cleanup);
      await finish(library.ready);
      await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
      await ble.advance(tester, 1000);
      await finish(service.connect(_boardId));
      await finish(controller.enter());
      await finish(
        controller.replaceDocument(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        ),
      );
      await finish(controller.enter());
      await tester.pumpWidget(
        MaterialApp(
          home: ConnectionGateScreen(
            bleService: service,
            craftController: controller,
            craftLibrary: library,
            firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
          ),
        ),
      );
      await until(
        () =>
            find.byType(LitchcraftCanvasView).evaluate().isNotEmpty &&
            controller.status == LitchcraftStatus.live,
      );
      final workloadWritesFrom = ble.writes.length;
      trace.events.clear();
      final metadata = controller.documentSession.document;
      final document = LitchcraftAnimationDocument(
        frames: List.generate(
          16,
          (f) => LitchcraftAnimationFrame(
            durationMs: 500 + f * 50,
            emitters: List.generate(
              256,
              (e) => LitchcraftAnimatedEmitter(
                color: (seed * 97 + f * 123457 + e * 397) & 0xffffff,
              ),
            ),
          ),
        ),
        topology: metadata.topology,
        favoriteColors: metadata.favoriteColors,
        sparkleSeed: seed,
        playbackOnEntry: false,
      );
      ConnectionSession? uploadSession;
      await finish(
        service.openConnectionSession(_boardId).then((s) => uploadSession = s),
      );
      final nativeCapacity = uploadSession!.objectSelectedCapacity;
      final producerWindow = LitchcraftFrameStoreClient.semanticWindowChunksFor(
        maximumPacketBytes:
            ConnectionLitchcraftCodec.maximumSingleObjectPacketBytes(
              maximumBodyBytes: uploadSession!.objectMaximumBody,
            ),
        sharedSession: true,
      );
      expect(nativeCapacity, greaterThan(0));
      expect(producerWindow, greaterThan(nativeCapacity));
      ble.batchesFrom = ble.exactBatches.length;
      ble.holdObjectProgress = true;
      final upload = uploadForCleanup = controller.replaceDocument(document);
      upload.ignore();
      var uploadDone = false;
      upload.then<void>(
        (_) => uploadDone = true,
        onError: (Object _, StackTrace _) {},
      );
      List<ConnectionSessionPacket> nativeBodyObjects() => ble.exactBatches
          .skip(ble.batchesFrom)
          .expand((batch) => batch.values)
          .map(_storeBodyObject)
          .whereType<ConnectionSessionPacket>()
          .toList();
      await until(
        () =>
            ble.objectProgress.isNotEmpty &&
            ble.pendingExactBatches == 0 &&
            nativeBodyObjects().length == producerWindow,
      );
      final beforeAcceptance = nativeBodyObjects();
      final firstBody = beforeAcceptance.first;
      final initialGrant = _objectGrantBeforeBody(ble.emitted, firstBody);
      expect(initialGrant, isNotNull);
      expect(beforeAcceptance.last.sequence, greaterThan(initialGrant!));
      expect(beforeAcceptance.map((packet) => packet.token),
          everyElement(firstBody.token));
      expect(beforeAcceptance.map((packet) => packet.sequence), [
        for (var index = 0; index < producerWindow; ++index)
          firstBody.sequence + index,
      ]);
      for (final batch in ble.exactBatches.skip(ble.batchesFrom)) {
        if (batch.values.any((bytes) => _storeBodyObject(bytes) != null)) {
          expect(batch.values.length, lessThanOrEqualTo(nativeCapacity));
        }
      }
      expect(uploadDone, isFalse);
      expect(
        controller.fullAnimationSyncDirection,
        LitchcraftFullSyncDirection.sendingToBoard,
      );
      final priorCommands = _craftFrames(
        ble.writes
            .skip(workloadWritesFrom)
            .where((w) => w['channel'] == 'connection_session')
            .map((w) => w['bytes'] as List<int>),
      );
      expect(
        priorCommands.where(
          (p) =>
              p.length > 2 &&
              p[0] == LitchcraftV2Protocol.magic &&
              p[1] == LitchcraftV2Protocol.version &&
              p[2] == LitchcraftV2Protocol.frameStoreCommit,
        ),
        isEmpty,
        reason:
            'Actual controller/client Commit cannot overtake the native-returned producer window awaiting admission',
      );
      ble.releaseObjectProgress();
      await finish(upload);
      await finish(controller.enter());
      await cleanParity();
      expect(
        trace.events.where(
          (e) =>
              e.event == 'litchcraft.frame_store_delivery' &&
              e.fields['accepted'] == true &&
              e.fields['frames'] == 16 &&
              e.fields['resident_hit'] == false,
        ),
        isNotEmpty,
      );
      expect(
        trace.events.where((e) => e.event == 'litchcraft.object_submission'),
        isNotEmpty,
      );
      final rows = trace.events
          .where((e) => e.event == 'litchcraft.object_submission')
          .toList();
      expect(rows.first.fields['count'], 48);
      expect(rows[1].fields['count'], 6);
      expect(rows[1].fields['first'], (rows.first.fields['last'] as int) + 1);
      expect(rows[1].fields['life'], rows.first.fields['life']);
      expect(rows[1].fields['request'], rows.first.fields['request']);
      final mapping = trace.events.singleWhere(
        (e) => e.event == 'litchcraft.object_transfer',
      );
      expect(mapping.fields['submissions'], rows.length);
      expect(
        mapping.fields['objects'],
        rows.fold<int>(0, (total, row) => total + (row.fields['count'] as int)),
      );

      await checkpoint('nonresident-upload');
      await openFrames();
      await settleCraftFilmstripScroll(tester);
      final eighth = await revealCraftFilmstripFrame(
        tester,
        index: 8,
        frameCount: controller.documentSession.document.frames.length,
        selectedFrame: () => controller.documentSession.selectedFrameIndex,
      );
      await tester.tap(eighth);
      await tester.pump();
      await settleCraftFilmstripScroll(tester);
      await until(
        () =>
            controller.documentSession.selectedFrameIndex == 8 &&
            !controller.pending,
      );
      await finish(controller.setBrush(color: 0x3498db, targetMask: 15));
      final before = controller.documentSession.document;
      final rect = tester.getRect(find.byType(LitchcraftCanvasView));
      Offset at(int x, int y) =>
          rect.topLeft +
          Offset(rect.width * (x + .5) / 8, rect.height * (y + .5) / 8);
      ble.holdPaint = true;
      final finger = await tester.startGesture(at(0, 0));
      await finger.moveTo(at(1, 0));
      await tester.pump(const Duration(milliseconds: 16));
      await until(() => ble.paintReceipts.isNotEmpty);
      await finger.moveTo(at(7, 0));
      await tester.pump(const Duration(milliseconds: 16));
      await finger.moveTo(at(7, 1));
      await tester.pump(const Duration(milliseconds: 16));
      await finger.moveTo(at(0, 1));
      await finger.up();
      await tester.pump();
      expect(controller.pending, isTrue);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, before),
        isFalse,
      );
      ble.holdPhysicalStoreObjects = true;
      await ble.command(tester, {
        'op': 'button',
        'owner': 1,
        'button': 5,
        'down': true,
      }, flushOperations: false);
      await ble.advance(tester, 20);
      await ble.command(tester, {
        'op': 'button',
        'owner': 1,
        'button': 5,
        'down': false,
      }, flushOperations: false);
      await ble.advance(tester, 20);
      ble.holdPhysicalStoreObjects = false;
      ble.release('connection_session');
      ble.releasePaint();
      await until(() => !controller.pending && !ble.hasPendingOperations);
      expect(
        sameLitchcraftDocument(controller.documentSession.document, before),
        isTrue,
      );
      await cleanParity();
      await checkpoint('released-drag-physical-X');
      await openFrames();
      await tester.tap(find.byKey(const ValueKey('craft-add-frame')));
      await until(
        () =>
            controller.documentSession.document.frames.length == 17 &&
            !controller.pending,
      );
      final added = controller.documentSession.selectedFrameIndex;
      final second = await tester.startGesture(at(3, 3));
      await second.moveTo(at(4, 3));
      await tester.pump(const Duration(milliseconds: 16));
      await second.up();
      await until(() => !controller.pending);
      final drawn = controller.documentSession.document;
      await openFrames();
      await settleCraftFilmstripScroll(tester);
      final addedTile = await revealCraftFilmstripFrame(
        tester,
        index: added,
        frameCount: controller.documentSession.document.frames.length,
        selectedFrame: () => controller.documentSession.selectedFrameIndex,
      );
      await tester.tap(addedTile);
      await tester.pump();
      await settleCraftFilmstripScroll(tester);
      // Existing filmstrip long press owns Delete confirmation.
      await tester.longPress(addedTile);
      await until(
        () => find
            .byKey(const ValueKey('craft-confirm-delete-frame'))
            .evaluate()
            .isNotEmpty,
      );
      await tester.tap(
        find.byKey(const ValueKey('craft-confirm-delete-frame')),
      );
      await until(
        () =>
            controller.documentSession.document.frames.length == 16 &&
            !controller.pending,
      );
      final deleted = controller.documentSession.document;
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await until(
        () =>
            !controller.pending &&
            controller.documentSession.document.frames.length == 17,
      );
      expect(
        sameLitchcraftDocument(controller.documentSession.document, drawn),
        isTrue,
      );
      await tester.tap(find.byKey(const ValueKey('craft-redo')));
      await until(
        () =>
            !controller.pending &&
            controller.documentSession.document.frames.length == 16,
      );
      expect(
        sameLitchcraftDocument(controller.documentSession.document, deleted),
        isTrue,
      );
      await cleanParity();
      await checkpoint('Add-draw-Delete-Undo-Redo');
      await openFrames();
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await until(() => controller.animationPlaying);
      await ble.advance(tester, 1300);
      await tester.tap(find.byKey(const ValueKey('craft-preview')));
      await until(() => !controller.animationPlaying && !controller.pending);
      await cleanParity();
      await checkpoint('Play-Pause');
      final intended = controller.documentSession.document;
      final selected = controller.documentSession.selectedFrameIndex;
      final writesFrom = workloadWritesFrom;
      await tester.tap(
        find.byKey(const ValueKey('connected-board-status-action')),
      );
      await until(() => find.text('Disconnect').evaluate().isNotEmpty);
      await tester.tap(find.text('Disconnect'));
      await until(() => !service.isConnected);
      final offlineStatus = find.byKey(
        const ValueKey('disconnected-board-status-action'),
      );
      await until(() => offlineStatus.evaluate().isNotEmpty);
      await tester.tap(offlineStatus);
      await finish(service.startScan());
      await until(() => find.text('Connect').evaluate().isNotEmpty);
      await tester.tap(find.text('Connect'));
      await until(
        () =>
            service.isConnected &&
            controller.status == LitchcraftStatus.live &&
            controller.inspectedBoardActive == true,
      );
      await cleanParity();
      expect(
        sameLitchcraftDocument(controller.documentSession.document, intended),
        isTrue,
      );
      expect(controller.documentSession.selectedFrameIndex, selected);
      final commands = _craftFrames(
        ble.writes
            .skip(writesFrom)
            .where((w) => w['channel'] == 'connection_session')
            .map((w) => w['bytes'] as List<int>),
      );
      expect(
        commands.where(
          (p) =>
              p[0] == LitchcraftProtocol.magic &&
              p[1] == LitchcraftProtocol.exit,
        ),
        isEmpty,
      );
      final sessionFuture = service.openConnectionSession(_boardId);
      ConnectionSession? session;
      await finish(sessionFuture.then((s) => session = s));
      final client = createConnectionLitchcraftFrameStoreClient(
        session: session!,
        monotonicMilliseconds: () => 0,
        appTrace: trace,
      );
      var request = 60000;
      LitchcraftFrameStoreState? state;
      await finish(
        client
            .inspect(session: controller.sessionId, request: ++request)
            .then((s) => state = s),
      );
      LitchcraftFrameStoreSnapshot? readback;
      await finish(
        client
            .snapshot(
              session: state!.session,
              generation: state!.generation,
              allocateRequest: () => ++request,
            )
            .then((s) => readback = s),
      );
      expect(sameLitchcraftDocument(readback!.document, intended), isTrue);
      expect(readback!.selectedFrame, selected);
      expect(controller.pending, isFalse);
      await cleanParity();
      await checkpoint('normal-reconnect-fresh-full-readback');
      await cleanup();
    },
    variant: TargetPlatformVariant({
      TargetPlatform.android,
      TargetPlatform.iOS,
    }),
    skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
  );

  for (final console in [true, false]) {
    testWidgets(
      'manual Craft reconnect preserves active Store with Console $console',
      (tester) async {
        SharedPreferences.setMockInitialValues({
          'enable_app_console': console,
          'litchcraft_discovery_dismissed': true,
        });
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        final ble = _StartupBle(native, negotiatedMtu: 247);
        final service = _LifecycleService(ble)..discoverBoard = true;
        final controller = LitchcraftController(service);
        final library = LitchcraftLibrary(controller);
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
          final start = tester.binding.clock.now();
          while (!done() &&
              tester.binding.clock.now().difference(start) <
                  const Duration(seconds: 10)) {
            await tick();
          }
          expect(
            done(),
            isTrue,
            reason:
                'status=${controller.status} active=${controller.boardActive} '
                'selected=${controller.selected} inspected=${controller.inspectedBoardActive} '
                'error=${controller.errorMessage}',
          );
        }

        Future<void> finish(Future<dynamic> work) async {
          var done = false;
          Object? failure;
          unawaited(
            work.then<void>(
              (_) => done = true,
              onError: (Object error, StackTrace _) {
                failure = error;
                done = true;
              },
            ),
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
        List<Uint8List> commandsSince(int from) => [
          for (final write in ble.writes.skip(from))
            if (write['channel'] == 'litchcraft')
              Uint8List.fromList(write['bytes'] as List<int>),
          ..._craftFrames(
            ble.writes
                .skip(from)
                .where((write) => write['channel'] == 'connection_session')
                .map((write) => write['bytes'] as List<int>),
          ),
        ];
        int exitsSince(int from) => commandsSince(from)
            .where(
              (packet) =>
                  packet[0] == LitchcraftProtocol.magic &&
                  packet[1] == LitchcraftProtocol.exit,
            )
            .length;
        bool getCraftVisible() =>
            find
                .byKey(const ValueKey('main-destination-craft'))
                .evaluate()
                .isNotEmpty &&
            tester
                    .widget<NavigationBar>(find.byType(NavigationBar))
                    .selectedIndex ==
                1;
        var phoneClosed = false;
        Future<void> closePhone() async {
          if (phoneClosed) return;
          phoneClosed = true;
          service.capabilityGate?.complete();
          service.capabilityGate = null;
          ble.heldChannels.clear();
          ble.release('connection_session');
          try {
            await tester.pumpWidget(const SizedBox.shrink());
            await finish(library.flush());
            library.dispose();
            controller.dispose();
            await finish(service.dispose());
            await ble.flush(tester);
            await finish(ble.close());
          } finally {
            await tester.runAsync(native.close);
          }
        }

        addTearDown(closePhone);
        await finish(library.ready);
        await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
        await ble.advance(tester, 1000);
        await finish(service.connect(_boardId));
        await finish(controller.enter());
        await finish(
          controller.replaceDocument(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint([0, 63], 15, 0x12ab34),
            ),
          ),
        );
        await finish(controller.enter());
        await ble.advance(tester, 2000);
        await until(
          () =>
              controller.status == LitchcraftStatus.live &&
              !controller.pending &&
              !controller.boardArtworkUnresolved,
        );
        // Real Hall input creates retained Store history, not a fake mode bit.
        await ble.command(tester, {'op': 'hall', 'mask': '0000000000000002'});
        await ble.advance(tester, 30);
        await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
        await ble.advance(tester, 6000);
        await until(() => controller.canUndo && !controller.pending);
        final before = await store();
        final beforeFrame = await ble.frame(tester);
        expect(before['frame_count'], 1);
        expect(before['active'], isTrue);
        expect(before['dirty'], isFalse);
        await tester.pumpWidget(
          MaterialApp(
            home: ConnectionGateScreen(
              bleService: service,
              craftController: controller,
              craftLibrary: library,
              firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
            ),
          ),
        );
        // Cold mounting follows authoritative Craft before any manual cycle.
        await until(
          () => getCraftVisible() && controller.status == LitchcraftStatus.live,
        );
        expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
        for (var cycle = 0; cycle < 3; ++cycle) {
          final writesFrom = ble.writes.length;
          final connectionsBefore = ble.connections.length;
          await tester.tap(
            find.byKey(const ValueKey('connected-board-status-action')),
          );
          await until(() => find.text('Disconnect').evaluate().isNotEmpty);
          await tester.tap(find.text('Disconnect'));
          await until(() => !service.isConnected);
          await ble.flush(tester);
          expect((await ble.frame(tester))['litchcraft_active'], isTrue);
          expect(exitsSince(writesFrom), 0);
          await tester.tap(
            find.byKey(const ValueKey('disconnected-board-status-action')),
          );
          await until(
            () => find
                .byKey(const ValueKey('connection-gate-content'))
                .evaluate()
                .isNotEmpty,
          );
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(exitsSince(writesFrom), 0);
          service.capabilityGate = Completer<void>();
          final capabilityCalls = service.capabilityCalls;
          await service.startScan();
          await until(() => find.text('Connect').evaluate().isNotEmpty);
          await tester.tap(find.text('Connect'));
          await until(
            () =>
                service.isConnected &&
                service.capabilityCalls > capabilityCalls,
          );
          // Hold real capability completion while the retained shell rebuilds.
          for (var step = 0; step < 12; ++step) {
            await tick();
          }
          expect(
            exitsSince(writesFrom),
            0,
            reason:
                'Transport readiness must not become an explicit Play/Exit intent',
          );
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          ble.heldChannels.add('connection_session');
          service.capabilityGate!.complete();
          service.capabilityGate = null;
          await until(() => ble.held.isNotEmpty);
          for (var step = 0; step < 5; ++step) {
            await tick();
          }
          expect(exitsSince(writesFrom), 0);
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          ble.heldChannels.remove('connection_session');
          ble.release('connection_session');
          await until(
            () =>
                controller.inspectedBoardActive == true &&
                controller.status == LitchcraftStatus.live &&
                getCraftVisible(),
          );
          await ble.advance(tester, 3000);
          expect(
            ble.connections.length,
            connectionsBefore + 1,
            reason: 'Healthy reconnect must not retire and reconnect again',
          );
          expect(exitsSince(writesFrom), 0);
          expect(controller.canUndo, isTrue);
          final after = await store();
          for (final key in [
            'root_generation',
            'frame_count',
            'live_generation',
            'live_revision',
            'emitters',
            'favorite_colors',
            'playing',
            'active',
          ]) {
            expect(
              after[key],
              before[key],
              reason: 'Reconnect retains Store $key',
            );
          }
          final frame = await ble.frame(tester);
          expect(
            frame['litchcraft_session'],
            beforeFrame['litchcraft_session'],
          );
          expect(frame['litchcraft_active'], isTrue);
          debugPrint(
            'CRAFT_MANUAL_RECONNECT console=$console cycle=$cycle '
            'occupancy=${frame['occupancy']} mode=Craft active=${frame['litchcraft_active']} '
            'root=${after['root_generation']} frames=${after['frame_count']} '
            'undo=${controller.canUndo} exits=${exitsSince(writesFrom)} '
            'connections=${ble.connections.length} next=paint-or-explicit-Play',
          );
        }
        final departureFrom = ble.writes.length;
        await tester.tap(find.byKey(const ValueKey('main-destination-play')));
        await until(() => !controller.boardActive && !controller.selected);
        expect(
          exitsSince(departureFrom),
          1,
          reason: 'A genuine Play tap still owns one acknowledged Exit',
        );
        expect((await store())['emitters'], before['emitters']);
        expect((await ble.frame(tester))['litchcraft_active'], isFalse);
        await closePhone();
      },
      variant: TargetPlatformVariant.only(TargetPlatform.iOS),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
  for (final scenario in [
    (
      mtu: 23,
      delayed: false,
      failed: false,
      exitedWhileAbsent: false,
      retained: false,
    ),
    (
      mtu: 247,
      delayed: false,
      failed: false,
      exitedWhileAbsent: false,
      retained: false,
    ),
    (
      mtu: 247,
      delayed: true,
      failed: false,
      exitedWhileAbsent: false,
      retained: false,
    ),
    (
      mtu: 23,
      delayed: false,
      failed: true,
      exitedWhileAbsent: false,
      retained: false,
    ),
    (
      mtu: 247,
      delayed: false,
      failed: false,
      exitedWhileAbsent: true,
      retained: false,
    ),
    (
      mtu: 247,
      delayed: false,
      failed: false,
      exitedWhileAbsent: true,
      retained: true,
    ),
  ]) {
    final mtu = scenario.mtu;
    final delayed = scenario.delayed;
    final exitedWhileAbsent = scenario.exitedWhileAbsent;
    final retained = scenario.retained;
    testWidgets(
      exitedWhileAbsent
          ? 'app ${retained ? 'background link loss' : 'termination'} then raw physical Exit${retained ? ' with 21 completed strokes' : ''}: Craft Play navigation keeps board controls available'
          : 'cold active Litchcraft attach: actual capabilities, MTU $mtu, delayed=$delayed, failed=${scenario.failed}',
      (tester) async {
        SharedPreferences.setMockInitialValues({});
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final native = (await tester.runAsync(
          () => VirtualLitchProcess.start(1, false, hardwareV3: true),
        ))!;
        var ble = _StartupBle(native, negotiatedMtu: mtu);
        LitchBleService createService() => retained
            ? _LifecycleService(ble)
            : LitchBleService(
                ble: ble,
                gattSessionFactory: ReactiveBleGattSessionFactory(ble),
                pendingOtaStore: MemoryOtaTargetStore(),
              );
        var service = createService();
        var controller = LitchcraftController(service);
        LitchcraftLibrary? library;
        var phoneDisposed = false;
        var expectHealthyNavigation = false;
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
          if (expectHealthyNavigation) {
            expect(
              controller.status,
              isNot(LitchcraftStatus.error),
              reason: controller.errorMessage,
            );
            expect(
              find.textContaining('Board controls unavailable'),
              findsNothing,
            );
          }
        }

        Future<void> until(bool Function() done) async {
          final start = tester.binding.clock.now();
          while (!done() &&
              tester.binding.clock.now().difference(start) <
                  const Duration(seconds: 15)) {
            await tick();
          }
          if (!done()) {
            final response = await ble.command(tester, {
              'op': 'frame',
            }, flushOperations: false);
            final frame = response['frame'] as Map<String, dynamic>;
            debugPrint(
              'ABSENT_EXIT_FAILURE nativeActive=${frame['litchcraft_active']} session=${frame['litchcraft_session']} requiredAction=${frame['required_action']} controller=${controller.status} selected=${controller.selected}',
            );
          }
          expect(
            done(),
            isTrue,
            reason:
                '${controller.errorMessage}; status=${controller.status} active=${controller.boardActive} selected=${controller.selected} inspected=${controller.inspectedBoardActive} session=${controller.sessionId}',
          );
        }

        Future<void> finish(Future<dynamic> work) async {
          var done = false;
          Object? failure;
          unawaited(
            work.then<void>(
              (_) => done = true,
              onError: (Object error, StackTrace _) {
                failure = error;
                done = true;
              },
            ),
          );
          await until(() => done);
          if (failure != null) throw failure!;
        }

        addTearDown(() async {
          try {
            await tester.pumpWidget(const SizedBox.shrink());
            if (!phoneDisposed) {
              library?.dispose();
              controller.dispose();
            }
            await finish(service.dispose());
            await ble.flush(tester);
            await finish(ble.close());
          } finally {
            await tester.runAsync(native.close);
          }
        });

        // Seed a genuine healthy Store through the existing app/firmware
        // owners, then destroy the phone while leaving physical mode active.
        await ble.command(tester, {'op': 'hall', 'mask': '0000000000000000'});
        await ble.advance(tester, 1000);
        if (retained) {
          library = LitchcraftLibrary(controller);
          await finish(library.ready);
        }
        await finish(service.connect(_boardId));
        await finish(controller.enter());
        var artwork = LitchcraftCanvas.blank().paint([0, 63], 15, 0x12ab34);
        await finish(
          retained
              ? controller.replaceDocument(staticLitchcraftDocument(artwork))
              : controller.transferFrameStoreForDiagnostics(
                  frames: staticLitchcraftDocument(artwork).frames,
                  playbackOnEntry: false,
                ),
        );
        await finish(controller.enter());
        expect(controller.boardActive, isTrue);
        await ble.advance(tester, 2000);
        final stored =
            (await ble.command(tester, {
                  'op': 'store_frame',
                  'index': 0,
                }))['store_frame']
                as Map<String, dynamic>;
        expect(stored['dirty'], isFalse);
        expect(stored['active'], isTrue);
        expect(stored['playing'], isFalse);
        expect(
          (stored['emitters'] as List<dynamic>).map((emitter) => emitter[0]),
          artwork.colors,
          reason: 'The native Store must contain the unfamiliar artwork seed',
        );
        if (retained) {
          await finish(controller.setBrush(color: 0x654321, targetMask: 15));
          for (var square = 1; square <= 21; ++square) {
            await ble.command(tester, {
              'op': 'hall',
              'mask': (BigInt.one << square).toRadixString(16).padLeft(16, '0'),
            });
            await ble.advance(tester, 30);
            await ble.command(tester, {
              'op': 'hall',
              'mask': '0000000000000000',
            });
            // A release longer than the board's 350 ms stroke grace creates
            // a distinct real history operation rather than one long stroke.
            await ble.advance(tester, 400);
          }
          // Physical strokes use the normal deferred Store persistence owner.
          await ble.advance(tester, 6000);
          final painted =
              (await ble.command(tester, {
                    'op': 'store_frame',
                    'index': 0,
                  }))['store_frame']
                  as Map<String, dynamic>;
          expect(painted['dirty'], isFalse);
          artwork = LitchcraftCanvas(
            colors: (painted['emitters'] as List<dynamic>).map(
              (emitter) => emitter[0] as int,
            ),
            topology: artwork.topology,
          );
        }
        if (retained) {
          await tester.pumpWidget(
            MaterialApp(
              home: ConnectionGateScreen(
                bleService: service,
                craftController: controller,
                craftLibrary: library,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
              ),
            ),
          );
          await until(
            () =>
                find.byType(NavigationBar).evaluate().isNotEmpty &&
                tester
                        .widget<NavigationBar>(find.byType(NavigationBar))
                        .selectedIndex ==
                    1,
          );
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.inactive,
          );
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.hidden,
          );
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.paused,
          );
          await until(() => !service.appForegroundActive);
          expect(
            service.isConnected,
            isTrue,
            reason: 'Backgrounding preserves a healthy native connection',
          );
          // Model actual OS/radio loss, not a manual Disconnect intent. Native
          // owns retirement; the SDK event tells the retained app it happened.
          await ble.command(tester, {
            'op': 'connect',
            'connected': false,
          }, flushOperations: false);
          ble.connections.last.add(
            const ConnectionStateUpdate(
              deviceId: _boardId,
              connectionState: DeviceConnectionState.disconnected,
              failure: null,
            ),
          );
          await until(() => !service.isConnected);
          await ble.flush(tester);
        } else {
          controller.dispose();
          phoneDisposed = true;
          await finish(service.dispose());
          await ble.flush(tester);
          await finish(ble.close());
        }
        expect((await ble.frame(tester))['litchcraft_active'], isTrue);
        int? absentExitSession;
        if (exitedWhileAbsent) {
          // The phone has gone away: only raw physical controls may exit mode.
          for (final button in [5, 6]) {
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': button,
              'down': true,
            }, flushOperations: false);
            await ble.advance(tester, 30);
          }
          await ble.advance(tester, 1500);
          for (final button in [5, 6]) {
            await ble.command(tester, {
              'op': 'button',
              'owner': 1,
              'button': button,
              'down': false,
            }, flushOperations: false);
            await ble.advance(tester, 30);
          }
          await ble.advance(tester, 2000);
          final exited = await ble.frame(tester);
          expect(exited['litchcraft_active'], isFalse);
          absentExitSession = exited['litchcraft_session'] as int;
        }

        if (!retained) ble = _StartupBle(native, negotiatedMtu: mtu);
        if (delayed) ble.mtuResult = Completer<int>();
        ble.failMtu = scenario.failed;
        if (!retained) {
          service = createService();
          controller = LitchcraftController(service);
          library = LitchcraftLibrary(controller);
          phoneDisposed = false;
        } else {
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.hidden,
          );
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.inactive,
          );
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
          await tester.pump();
        }
        var modeResolved = false;
        final selectedPages = <int>[];
        await finish(service.connect(_boardId));
        if (!retained) {
          await tester.pumpWidget(
            MaterialApp(
              home: LitchMainScaffold(
                bleService: service,
                boardId: _boardId,
                boards: const [],
                onDisconnect: service.disconnect,
                craftController: controller,
                craftLibrary: library,
                onModeResolved: () => modeResolved = true,
                onPageSelected: selectedPages.add,
                loadAppConsolePreference: () async => false,
                firmwarePageBuilder: (_, _, _, _, _) => const SizedBox.shrink(),
              ),
            ),
          );
        }
        if (retained) {
          await until(() => controller.inspectedBoardActive != null);
          modeResolved = true;
        }
        if (delayed) {
          await ble.advance(tester, 8000);
          // This fixture mounts the mode-owning scaffold directly. The gate
          // owns connection progress and Cancel; this seam owns admission.
          expect(find.byType(HomeScreen, skipOffstage: false), findsNothing);
          expect(controller.inspectedBoardActive, isNull);
          expect(modeResolved, isFalse);
          expect(ble.writes, isEmpty);
          expect(ble.connections, hasLength(1));
          expect(ble.mtuRequests, 1);
          ble.mtuResult!.complete(mtu);
        }
        if (exitedWhileAbsent) {
          await until(
            () => modeResolved && find.byType(HomeScreen).evaluate().isNotEmpty,
          );
          expect(controller.inspectedBoardActive, isFalse);
          expect(controller.boardActive, isFalse);
          expect(controller.selected, isFalse);
          final reconnected = await ble.frame(tester);
          expect(reconnected['litchcraft_active'], isFalse);
          expect(
            reconnected['litchcraft_session'],
            absentExitSession,
            reason: 'Passive restoration must not send a speculative Enter',
          );
          debugPrint(
            'LIFECYCLE_MODE absent-exit-followed retained=$retained '
            'occupancy=${reconnected['occupancy']} mode=Play '
            'display="${reconnected['display']}" action=${reconnected['required_action']} '
            'app_status=${controller.status} next=explicit-Craft',
          );
          expectHealthyNavigation = true;
          for (var cycle = 0; cycle < 2; ++cycle) {
            final repliesFrom = ble.emitted.length;
            final writesFrom = ble.writes.length;
            await tester.tap(
              find.byKey(const ValueKey('main-destination-craft')),
            );
            if (cycle == 0) {
              // The player saw the board follow Enter before choosing Play.
              // Sample real firmware without draining unrelated Store work.
              final started = tester.binding.clock.now();
              var active = false;
              while (!active &&
                  tester.binding.clock.now().difference(started) <
                      const Duration(seconds: 5)) {
                await tick();
                final response = await ble.command(tester, {
                  'op': 'frame',
                }, flushOperations: false);
                active =
                    (response['frame']
                        as Map<String, dynamic>)['litchcraft_active'] ==
                    true;
              }
              expect(active, isTrue);
              debugPrint(
                'LIFECYCLE_MODE Enter-observed status=${controller.status} pending=${controller.pending} artworkUnresolved=${controller.boardArtworkUnresolved} next=immediate-Play',
              );
            } else {
              await until(
                () =>
                    controller.status == LitchcraftStatus.live &&
                    !controller.boardArtworkUnresolved &&
                    !controller.pending,
              );
              expect(controller.canvas.colors, artwork.colors);
            }
            if (retained && cycle == 0) {
              final entryPackets =
                  _craftFrames(
                        ble.emitted
                            .skip(repliesFrom)
                            .where(
                              (packet) =>
                                  packet.channel == 'connection_session',
                            )
                            .map((packet) => packet.bytes),
                      )
                      .map(LitchcraftPacket.decode)
                      .whereType<LitchcraftPacket>()
                      .toList();
              final ackIndex = entryPackets.indexWhere(
                (packet) =>
                    packet.event == LitchcraftProtocol.state &&
                    packet.requestId != 0 &&
                    packet.payload[1] & 1 != 0,
              );
              expect(ackIndex, greaterThanOrEqualTo(0));
              final ack = entryPackets[ackIndex];
              final resyncIndex = entryPackets.indexWhere(
                (packet) =>
                    packet.event == LitchcraftProtocol.resyncRequired &&
                    packet.requestId == ack.requestId &&
                    packet.session == ack.session &&
                    packet.revision == ack.revision,
              );
              expect(resyncIndex, greaterThan(ackIndex));
              expect(controller.boardActive, isTrue);
              expect(controller.selected, isTrue);
            }
            final unavailable = find
                .textContaining('Board controls unavailable')
                .evaluate()
                .isNotEmpty;
            await tester.tap(
              find.byKey(const ValueKey('main-destination-play')),
            );
            if (retained && cycle == 0) {
              await tester.pump();
              await ble.flush(tester);
              final afterPlay = await ble.frame(tester);
              final states =
                  _craftFrames(
                        ble.emitted
                            .skip(repliesFrom)
                            .where(
                              (packet) =>
                                  packet.channel == 'connection_session',
                            )
                            .map((packet) => packet.bytes),
                      )
                      .where(
                        (packet) =>
                            packet[0] == LitchcraftProtocol.magic &&
                            packet[1] == LitchcraftProtocol.state &&
                            (packet[10] != 0 || packet[11] != 0),
                      )
                      .toList();
              final exits =
                  _craftFrames(
                        ble.writes
                            .skip(writesFrom)
                            .where(
                              (write) =>
                                  write['channel'] == 'connection_session',
                            )
                            .map((write) => write['bytes'] as List<int>),
                      )
                      .where(
                        (packet) =>
                            packet[0] == LitchcraftProtocol.magic &&
                            packet[1] == LitchcraftProtocol.exit,
                      )
                      .length;
              debugPrint(
                'HISTORY_MODE_ACK states=${states.map((packet) => {"status": packet[12], "active": packet[13] & 1, "undo": packet[15], "redo": packet[16], "decoded": LitchcraftPacket.decode(packet) != null}).toList()} unavailable=$unavailable app_active=${controller.boardActive} selected=${controller.selected} native_active=${afterPlay['litchcraft_active']} Exit_writes=$exits error=${controller.errorMessage}',
              );
              expect(states.where((packet) => packet[13] & 1 != 0), isNotEmpty);
              expect(states.where((packet) => packet[13] & 1 == 0), isNotEmpty);
              expect(states.map((packet) => packet[15]), everyElement(21));
              expect(
                states.every(
                  (packet) => LitchcraftPacket.decode(packet) != null,
                ),
                isTrue,
              );
              expect(exits, 1);
              expect(afterPlay['litchcraft_active'], isFalse);
            }
            expect(unavailable, isFalse);
            await until(
              () =>
                  !controller.boardActive &&
                  !controller.selected &&
                  !controller.pending,
            );
            final exited = await ble.frame(tester);
            expect(exited['litchcraft_active'], isFalse);
            debugPrint(
              'LIFECYCLE_MODE explicit-Play cycle=$cycle retained=$retained '
              'occupancy=${exited['occupancy']} native_active=${exited['litchcraft_active']} '
              'display="${exited['display']}" action=${exited['required_action']} '
              'app_status=${controller.status} next=explicit-Craft',
            );
          }
          await tester.tap(
            find.byKey(const ValueKey('main-destination-craft')),
          );
        }
        await until(
          () =>
              modeResolved &&
              (retained
                  ? tester
                            .widget<NavigationBar>(find.byType(NavigationBar))
                            .selectedIndex ==
                        1
                  : selectedPages.contains(0)) &&
              controller.boardActive &&
              controller.status == LitchcraftStatus.live &&
              !controller.boardArtworkUnresolved,
        );
        expect(service.deviceCapabilities(_boardId).explicitlyReported, isTrue);
        expect(ble.connections, hasLength(retained ? 2 : 1));
        expect(ble.mtuRequests, retained ? 2 : 1);
        expect(ble.discoveryCalls, retained ? 2 : 1);
        expect(ble.exactOperations, greaterThan(0));
        expect(ble.qualifiedCalls, 0);
        expect(
          controller.canvas.colors,
          artwork.colors,
          reason: 'A fresh phone must actually read unfamiliar board artwork',
        );
        expect(controller.documentSession.canvas.colors, artwork.colors);
        expect((await ble.frame(tester))['litchcraft_active'], isTrue);
        await tester.pumpWidget(const SizedBox.shrink());
        await finish(library!.flush());
        library.dispose();
        controller.dispose();
        phoneDisposed = true;
        await finish(service.disconnect());
        if (scenario.failed) {
          ble.failMtu = false;
          await finish(service.connect(_boardId));
          await finish(service.openConnectionSession(_boardId));
          expect(ble.mtuRequests, 2);
          expect(ble.discoveryCalls, 2);
          expect(ble.connections, hasLength(2));
          expect(ble.qualifiedCalls, 0);
          await ble.advance(tester, 1000);
          await finish(service.disconnect());
        }
      },
      variant: exitedWhileAbsent
          ? TargetPlatformVariant({TargetPlatform.android, TargetPlatform.iOS})
          : TargetPlatformVariant.only(TargetPlatform.android),
      skip: Platform.environment['LITCH_VIRTUAL_BRIDGE'] == null,
    );
  }
}
