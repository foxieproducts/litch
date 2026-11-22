import '../support/memory_ota_target_store.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:litch_mobile/src/constants/ble_protocol.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/settings_state_wire_codec.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/litch_gatt_session.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import '../support/support_session_board.dart';

// Reuse the existing current receiver, retained Ordered outcomes and reverse
// Object grants. Only document operations below are scripted domain work.
class _CraftSessionBoard extends SupportSessionBoard {
  _CraftSessionBoard(this.peer) {
    onProductQuery = (_) => _publishProductState();
  }
  final LitchcraftV2FunctionalPeer peer;
  final _craft = ConnectionLitchcraftAssembler();

  void _publishProductState() {
    Uint8List fixture(String file, String row) {
      final line = File(
        '../test/fixtures/$file',
      ).readAsLinesSync().firstWhere((line) => line.startsWith('$row,'));
      final hex = line.substring(line.indexOf(',') + 1);
      return Uint8List.fromList([
        for (var i = 0; i < hex.length; i += 2)
          int.parse(hex.substring(i, i + 2), radix: 16),
      ]);
    }

    final home = fixture('home_state_wire_v1.csv', 'representative');
    for (var i = 0; i < 10; ++i) {
      emitState(4 + i, home.sublist(i * 10, i * 10 + 10), sequence: 1);
    }
    final settings = fixture('settings_state_wire_v3.csv', 'defaults');
    final identity = litchcraftAnimationCrc32(settings);
    for (var i = 0; i < SettingsStateWireCodec.fragmentCount; ++i) {
      emitState(
        SettingsStateWireCodec.fragmentCode(i),
        settings.sublist(
          i * 10,
          i * 10 + SettingsStateWireCodec.fragmentLength(i),
        ),
        sequence: identity == 0 ? 1 : identity,
      );
    }
  }

  @override
  int get sessionFeatures =>
      super.sessionFeatures | ConnectionSession.orderedModeFeature;

  @override
  Future<void> onAdmittedObject(ConnectionSessionPacket packet) async {
    if (packet.code != ConnectionLitchcraftCodec.objectOpcode) return;
    final bytes = _craft.accept(packet.body);
    if (bytes != null) await peer.writeDomain(bytes);
  }

  @override
  int orderedRejection(ConnectionSessionPacket packet) {
    if (packet.code != ConnectionSession.litchcraftModeOpcode) {
      return super.orderedRejection(packet);
    }
    if (packet.body[1] == LitchcraftProtocol.enter && peer.enterRejection != 0) {
      return peer.enterRejection;
    }
    unawaited(peer.writeDomain(packet.body));
    return 0;
  }

  @override
  Future<void> commandWrite(List<int> value) async {
    if ((jsonDecode(utf8.decode(value)) as Map)['cmd'] == 'info') {
      emitStatus('{"i":[3,3,${(1 << 10) | (1 << 12) | (1 << 14)}]}');
      return;
    }
    await super.commandWrite(value);
  }
}

/// Functional protocol peer at the native GATT boundary. Flutter transport,
/// controller and screen are real; this explicitly simulates board document
/// operations/history and does not claim firmware or radio verification.
class LitchcraftV2FunctionalPeer
    implements FlutterReactiveBle, LitchGattSessionFactory, LitchGattSession {
  LitchcraftV2FunctionalPeer() {
    final session = LitchcraftDocumentSession(
      staticLitchcraftDocument(LitchcraftCanvas.blank()),
    )..addFrame();
    document = session.document;
  }
  late final _session = _CraftSessionBoard(this);
  // Direct domain clients deliberately bypass GATT/session framing; consumer
  // journeys using LitchcraftV2FunctionalService cross the real app adapters.
  final packets = StreamController<List<int>>.broadcast(sync: true);
  final sharedWrites = <ConnectionSessionPacket>[];

  Future<void>? Function(List<int> bytes)? beforeDomainReceipt;

  void emitDomainPacket(List<int> bytes) {
    final wait = beforeDomainReceipt?.call(bytes);
    if (wait == null) {
      _deliverDomainPacket(bytes);
    } else {
      unawaited(wait.then((_) => _deliverDomainPacket(bytes)));
    }
  }

  void _deliverDomainPacket(List<int> bytes) {
    packets.add(bytes);
    if (_session.sessionActivated) {
      _session.emitSupport(
        ConnectionLitchcraftCodec.receiptCode,
        Uint8List.fromList(bytes),
      );
    }
  }

  Future<void> writeDomain(List<int> bytes) => write(
    serviceId: BleProtocol.baseService,
    characteristicId: BleProtocol.litchcraftCharacteristic,
    value: bytes,
    withResponse: true,
  );
  final writes = <List<int>>[];
  final notificationLengths = <int>[];
  final notificationEvents = <int>[];
  final undo = <LitchcraftAnimationDocument>[],
      redo = <LitchcraftAnimationDocument>[];
  final staged = <int>[];
  int sessionId = 42, revision = 1, mtuRequests = 0;
  int enterRejection = 0;
  int connectionPriorityRequests = 0;
  int subscriptions = 0, selectedFrame = 0, elapsedMs = 0, sampleId = 0;
  bool active = true, playing = false, physicalBrushHeld = false;
  bool rejectAdvancedTool = false, brushErase = false;
  int brushColor = 0xff7430, brushMask = 15;
  bool deliverNotifications = true;
  bool supportsCanvasEdit = true;
  bool supportsBulkDocument = true;
  final responseWrites = <bool>[];
  bool supportsCanvasRotate = true;
  bool supportsCanvasPattern = true;
  bool supportsEffectClock = true, effectClockSubscribed = false;
  int effectElapsedMs = 0;
  Future<void>? canvasEditReplyGate;
  Future<void>? effectReplyGate;
  Future<void>? frameEditReplyGate;
  Future<void>? historyReplyGate;
  final Map<int, List<int>> canvasEditReceipts = {};
  final Map<int, List<int>> editReceipts = {};
  int nativeMtuResult = 247;
  int? crc;
  LitchcraftAnimationDocument? strokeBefore;
  late LitchcraftAnimationDocument document;
  Future<void> Function()? beforeDomainWork;
  Future<void> Function()? beforeMtu;
  Future<void> Function()? beforeSnapshot;
  Future<void> Function()? beforeExit;
  Future<void>? paintReplyGate;
  Future<void>? enterReplyGate;
  Future<void>? brushEventGate;

  @override
  Future<int> requestMtu({required String deviceId, required int mtu}) async {
    mtuRequests++;
    await beforeMtu?.call();
    return nativeMtuResult;
  }

  @override
  Future<void> requestConnectionPriority({
    required String deviceId,
    required ConnectionPriority priority,
  }) async {
    connectionPriorityRequests++;
  }

  @override
  Stream<ConnectionStateUpdate> connectToDevice({
    required String id,
    Map<Uuid, List<Uuid>>? servicesWithCharacteristicsToDiscover,
    Duration? connectionTimeout,
  }) {
    final updates = _session.connectToDevice(id: id);
    scheduleMicrotask(
      () => _session.connections.last.add(
        ConnectionStateUpdate(
          deviceId: id,
          connectionState: DeviceConnectionState.connected,
          failure: null,
        ),
      ),
    );
    return updates;
  }

  @override
  Future<LitchGattSession> open(String deviceId) async => this;
  @override
  Stream<List<int>> subscribe({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) {
    if (characteristicId == BleProtocol.connectionSessionEgressCharacteristic) {
      subscriptions++;
    }
    return _session.subscribeToCharacteristic(
      QualifiedCharacteristic(
        deviceId: 'board',
        serviceId: serviceId,
        characteristicId: characteristicId,
      ),
    );
  }

  @override
  Stream<List<int>> subscribeToCharacteristic(
    QualifiedCharacteristic characteristic,
  ) => _session.subscribeToCharacteristic(characteristic);
  @override
  Future<List<int>> read({
    required Uuid serviceId,
    required Uuid characteristicId,
  }) async => [];
  @override
  Future<void> write({
    required Uuid serviceId,
    required Uuid characteristicId,
    required List<int> value,
    required bool withResponse,
  }) async {
    if (characteristicId != BleProtocol.litchcraftCharacteristic) {
      if (characteristicId ==
          BleProtocol.connectionSessionIngressCharacteristic) {
        final packet = ConnectionSessionCodec.decode(Uint8List.fromList(value));
        if (packet != null) sharedWrites.add(packet);
      }
      await _session.writeCharacteristicWithResponse(
        QualifiedCharacteristic(
          deviceId: 'board',
          serviceId: serviceId,
          characteristicId: characteristicId,
        ),
        value: value,
      );
      return;
    }
    // These gates control scripted domain work after shared admission. Native
    // BLE timing and actual firmware execution belong to native/HIL journeys.
    responseWrites.add(withResponse);
    if (withResponse) await beforeDomainWork?.call();
    if (value.length > 2 &&
        value[0] == 0x4d &&
        value[2] == LitchcraftV2Protocol.snapshot) {
      await beforeSnapshot?.call();
    }
    if (value.length > 1 &&
        value[0] == 0x4c &&
        value[1] == LitchcraftProtocol.exit) {
      await beforeExit?.call();
    }
    respond(value);
  }

  @override
  Future<void> writeCharacteristicWithResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) => write(
    serviceId: characteristic.serviceId,
    characteristicId: characteristic.characteristicId,
    value: value,
    withResponse: true,
  );

  @override
  Future<void> writeCharacteristicWithoutResponse(
    QualifiedCharacteristic characteristic, {
    required List<int> value,
  }) => write(
    serviceId: characteristic.serviceId,
    characteristicId: characteristic.characteristicId,
    value: value,
    withResponse: false,
  );

  void event(int kind, int request, [List<int> payload = const []]) {
    if (!deliverNotifications) return;
    final h = ByteData(13)
      ..setUint8(0, 0x4d)
      ..setUint8(1, 2)
      ..setUint8(2, kind)
      ..setUint32(3, sessionId, Endian.little)
      ..setUint32(7, revision, Endian.little)
      ..setUint16(11, request, Endian.little);
    final bytes = [...h.buffer.asUint8List(), ...payload];
    notificationLengths.add(bytes.length);
    notificationEvents.add(kind);
    emitDomainPacket(bytes);
  }

  void state(int request, {int status = 0}) =>
      event(LitchcraftV2Protocol.state, request, [
        status,
        (active ? 1 : 0) |
            (playing ? 2 : 0) |
            (undo.isNotEmpty ? 4 : 0) |
            (redo.isNotEmpty ? 8 : 0),
        document.frames.length,
        selectedFrame,
        3,
      ]);

  void physicalToggleEraser() {
    brushErase = !brushErase;
    tool();
  }

  void tool() {
    final header = ByteData(12)
      ..setUint8(0, 0x4c)
      ..setUint8(1, LitchcraftProtocol.brush)
      ..setUint32(2, sessionId, Endian.little)
      ..setUint32(6, revision, Endian.little)
      ..setUint16(10, 0, Endian.little);
    emitDomainPacket(<int>[
      ...header.buffer.asUint8List(),
      ...LitchcraftProtocol.rgb(brushColor),
      brushErase ? 1 : 0,
      brushMask,
    ]);
  }

  void gatedState(int request, Future<void>? gate, {int status = 0}) {
    final h = ByteData(13)
      ..setUint8(0, 0x4d)
      ..setUint8(1, 2)
      ..setUint8(2, 0x81)
      ..setUint32(3, sessionId, Endian.little)
      ..setUint32(7, revision, Endian.little)
      ..setUint16(11, request, Endian.little);
    final reply = <int>[
      ...h.buffer.asUint8List(),
      status,
      (active ? 1 : 0) |
          (playing ? 2 : 0) |
          (undo.isNotEmpty ? 4 : 0) |
          (redo.isNotEmpty ? 8 : 0),
      document.frames.length,
      selectedFrame,
      3,
    ];
    editReceipts[request] = reply;
    void deliver() {
      if (deliverNotifications) emitDomainPacket(reply);
    }

    if (gate == null) {
      deliver();
    } else {
      unawaited(gate.then((_) => deliver()));
    }
  }

  void timeline([int request = 0]) {
    sampleId = (sampleId % 0xffff) + 1;
    final low = ByteData(7)
      ..setUint16(0, sampleId, Endian.little)
      ..setUint8(2, playing ? 1 : 0)
      ..setUint32(3, elapsedMs & 0xffffffff, Endian.little);
    final high = ByteData(7)
      ..setUint16(0, sampleId, Endian.little)
      ..setUint8(2, selectedFrame)
      ..setUint32(3, elapsedMs >> 32, Endian.little);
    event(LitchcraftV2Protocol.timelineLow, request, low.buffer.asUint8List());
    event(
      LitchcraftV2Protocol.timelineHigh,
      request,
      high.buffer.asUint8List(),
    );
    if (effectClockSubscribed) {
      for (final part in [0, 1]) {
        final effect = ByteData(7)
          ..setUint16(0, sampleId, Endian.little)
          ..setUint8(2, 0)
          ..setUint32(
            3,
            (effectElapsedMs >> (part * 32)) & 0xffffffff,
            Endian.little,
          );
        event(
          part == 0
              ? LitchcraftV2Protocol.effectClockLow
              : LitchcraftV2Protocol.effectClockHigh,
          request,
          effect.buffer.asUint8List(),
        );
      }
    }
  }

  void physicalSelect(int frame, {bool play = false, int? elapsed}) {
    selectedFrame = frame;
    playing = play;
    elapsedMs =
        elapsed ??
        document.frames
            .take(frame)
            .fold(0, (sum, frame) => sum + frame.durationMs);
    state(0);
    timeline();
  }

  void physicalAdd() {
    undo.add(document);
    redo.clear();
    final session = LitchcraftDocumentSession(document)
      ..selectFrame(selectedFrame);
    session.addFrame();
    document = session.document;
    selectedFrame = session.selectedFrameIndex;
    playing = false;
    elapsedMs = document.frames
        .take(selectedFrame)
        .fold(0, (sum, frame) => sum + frame.durationMs);
    revision++;
    state(0);
    timeline();
    event(LitchcraftV2Protocol.changed, 0);
  }

  void physicalEnter() {
    if (!active) sessionId++;
    active = true;
    state(0);
  }

  void physicalExit() {
    active = false;
    playing = false;
    state(0);
  }

  void snapshot(int request) {
    final encoded = encodeLitchcraftAnimationDocument(document);
    state(request);
    final begin = ByteData(7)
      ..setUint8(0, 3)
      ..setUint16(1, encoded.bytes.length, Endian.little)
      ..setUint32(3, encoded.crc32, Endian.little);
    event(
      LitchcraftV2Protocol.snapshotBegin,
      request,
      begin.buffer.asUint8List(),
    );
    for (var offset = 0; offset < encoded.bytes.length; offset += 229) {
      event(LitchcraftV2Protocol.snapshotChunk, request, [
        offset & 255,
        offset >> 8,
        ...encoded.bytes.skip(offset).take(229),
      ]);
    }
    event(LitchcraftV2Protocol.snapshotEnd, request);
  }

  void paint(int square, int color) {
    final session = LitchcraftDocumentSession(document);
    session.replaceCanvas(session.canvas.paint([square], 15, color));
    document = session.document;
  }

  void physicalPaint(int square, int color) {
    undo.add(document);
    redo.clear();
    paint(square, color);
    revision++;
    state(0);
    event(LitchcraftV2Protocol.changed, 0);
  }

  void respond(List<int> bytes) {
    writes.add(List.of(bytes));
    if (bytes.first != 0x4d) {
      if (bytes[1] == LitchcraftProtocol.tool) {
        final request = bytes[6] | bytes[7] << 8;
        final header = ByteData(12)
          ..setUint8(0, 0x4c)
          ..setUint8(1, LitchcraftProtocol.state)
          ..setUint32(2, sessionId, Endian.little)
          ..setUint32(6, revision, Endian.little)
          ..setUint16(10, request, Endian.little);
        if (!rejectAdvancedTool) {
          brushErase = bytes[8] != 0;
          brushMask = bytes[9];
          brushColor = LitchcraftProtocol.readRgb(bytes, 10);
        }
        emitDomainPacket([
          ...header.buffer.asUint8List(),
          rejectAdvancedTool ? 1 : 0,
          active ? 1 : 0,
          3,
          0,
          0,
          brushErase ? 1 : 0,
          brushMask,
          0,
        ]);
        if (!rejectAdvancedTool) {
          header.setUint8(1, LitchcraftProtocol.brush);
          final brushEvent = <int>[
            ...header.buffer.asUint8List(),
            ...LitchcraftProtocol.rgb(brushColor),
            brushErase ? 1 : 0,
            brushMask,
          ];
          final gate = brushEventGate;
          if (gate == null) {
            emitDomainPacket(brushEvent);
          } else {
            unawaited(gate.then((_) => emitDomainPacket(brushEvent)));
          }
        }
        return;
      }
      if (bytes[1] == LitchcraftProtocol.enter ||
          bytes[1] == LitchcraftProtocol.exit) {
        active = bytes[1] == LitchcraftProtocol.enter;
        if (active && sessionId == 0) sessionId = 42;
        final request = bytes[6] | bytes[7] << 8;
        final ack = ByteData(12)
          ..setUint8(0, 0x4c)
          ..setUint8(1, LitchcraftProtocol.state)
          ..setUint32(2, sessionId, Endian.little)
          ..setUint32(6, revision, Endian.little)
          ..setUint16(10, request, Endian.little);
        final reply = <int>[
          ...ack.buffer.asUint8List(),
          0,
          active ? 1 : 0,
          3,
          0,
          0,
          0,
          15,
          0,
        ];
        final gate = bytes[1] == LitchcraftProtocol.enter
            ? enterReplyGate
            : null;
        if (gate == null) {
          emitDomainPacket(reply);
        } else {
          unawaited(gate.then((_) => emitDomainPacket(reply)));
        }
        return;
      }
      throw StateError('Advanced peer received unsupported static packet');
    }
    final data = ByteData.sublistView(Uint8List.fromList(bytes));
    final request = data.getUint16(7, Endian.little);
    final previousReceipt = editReceipts[request];
    // Stream chunks deliberately reuse the transfer id in the request field
    // and have no individual receipt. They must not be mistaken for a retry
    // of the Begin receipt that allocated that transfer id.
    if (bytes[2] != LitchcraftV2Protocol.streamChunk &&
        previousReceipt != null) {
      if (deliverNotifications) emitDomainPacket(previousReceipt);
      return;
    }
    switch (bytes[2]) {
      case LitchcraftV2Protocol.canvasEdit:
        if (!supportsCanvasEdit) {
          state(request, status: 4);
          return;
        }
        if (bytes.length == 9) {
          state(request);
          return;
        }
        if (bytes.length == 10) {
          final feature = bytes[9];
          state(
            request,
            status:
                (feature == 2 && supportsCanvasRotate) ||
                    (feature == 3 &&
                        supportsCanvasPattern &&
                        supportsCanvasRotate)
                ? 0
                : 3,
          );
          return;
        }
        final cached = canvasEditReceipts[request];
        if (cached != null) {
          if (deliverNotifications) emitDomainPacket(cached);
          return;
        }
        if (physicalBrushHeld) {
          state(request, status: 1);
          return;
        }
        if (data.getUint32(9, Endian.little) != revision ||
            bytes[13] != selectedFrame) {
          state(request, status: 9);
          return;
        }
        final previous = document;
        final edit = LitchcraftDocumentSession(document)
          ..selectFrame(bytes[13]);
        if (bytes[18] >= 5) {
          final dark = bytes[18].isEven;
          edit.paintSelectedFrame(
            squares: [
              for (var square = 0; square < 64; square++)
                if ((((square % 8) + (square ~/ 8)).isOdd) == dark) square,
            ],
            mask: bytes[14],
            color: bytes[15] << 16 | bytes[16] << 8 | bytes[17],
            clearEffects: bytes[18] >= 7,
          );
        } else if (bytes[18] >= 2) {
          edit.rotateSelectedFrame(bytes[18] - 1);
        } else if (bytes[18] == 1) {
          edit.clearSelectedFrame();
        } else {
          edit.paintSelectedFrame(
            squares: List.generate(64, (index) => index),
            mask: bytes[14],
            color: bytes[15] << 16 | bytes[16] << 8 | bytes[17],
          );
        }
        document = edit.document;
        playing = false;
        if (!sameLitchcraftDocument(previous, document)) {
          undo.add(previous);
          redo.clear();
          revision++;
        }
        final header = ByteData(13)
          ..setUint8(0, 0x4d)
          ..setUint8(1, 2)
          ..setUint8(2, 0x81)
          ..setUint32(3, sessionId, Endian.little)
          ..setUint32(7, revision, Endian.little)
          ..setUint16(11, request, Endian.little);
        final reply = [
          ...header.buffer.asUint8List(),
          0,
          (active ? 1 : 0) | (undo.isNotEmpty ? 4 : 0),
          document.frames.length,
          selectedFrame,
          3,
        ];
        canvasEditReceipts[request] = reply;
        void deliver() {
          if (deliverNotifications) emitDomainPacket(reply);
        }
        final gate = canvasEditReplyGate;
        if (gate == null) {
          deliver();
        } else {
          unawaited(gate.then((_) => deliver()));
        }
      case LitchcraftV2Protocol.inspect:
        if (bytes.length == 10 && bytes[9] == 2 && !supportsBulkDocument) {
          state(request, status: 3);
          return;
        }
        if (bytes.length == 10 && bytes[9] == 3 && !supportsBulkDocument) {
          state(request, status: 3);
          return;
        }
        if (bytes.length == 10 && bytes[9] == 1) {
          if (!supportsEffectClock) {
            state(request, status: 3);
            return;
          }
          effectClockSubscribed = true;
        }
        state(request);
        timeline(request);
        if (bytes.length == 9 || bytes[9] == 1) tool();
      case LitchcraftV2Protocol.timeline:
        if (physicalBrushHeld && bytes[9] != 0) {
          state(request, status: 1);
          return;
        }
        if (bytes[9] == 2) {
          selectedFrame = bytes[10];
          elapsedMs = document.frames
              .take(selectedFrame)
              .fold(0, (sum, frame) => sum + frame.durationMs);
        }
        playing = bytes[9] == 1;
        state(request);
        timeline(request);
      case LitchcraftV2Protocol.snapshot:
        snapshot(request);
      case LitchcraftV2Protocol.paint:
      case LitchcraftV2Protocol.effect:
        if (physicalBrushHeld) {
          final gate = paintReplyGate;
          if (gate == null) {
            state(request, status: 1);
          } else {
            unawaited(gate.then((_) => state(request, status: 1)));
          }
          return;
        }
        final effectCommand = bytes[2] == LitchcraftV2Protocol.effect;
        final flags = bytes[effectCommand ? 16 : 12];
        if (flags & 1 != 0) strokeBefore = document;
        final color = bytes[14] << 16 | bytes[15] << 8 | bytes[16];
        final previous = document;
        final session = LitchcraftDocumentSession(document)
          ..selectFrame(bytes[9]);
        if (effectCommand) {
          for (final square in bytes.skip(17)) {
            session.applyEffect(
              square: square,
              mask: bytes[13],
              effect: LitchcraftEffectStyle.fromId(bytes[10]),
              effectDurationMs: bytes[11] | bytes[12] << 8,
            );
          }
        } else {
          session.replaceCanvas(
            session.canvas.paint(bytes.skip(17), bytes[13], color),
          );
        }
        document = session.document;
        selectedFrame = bytes[9];
        playing = false;
        if (!sameLitchcraftDocument(previous, document)) revision++;
        if (flags & 2 != 0 && strokeBefore != null) {
          if (!sameLitchcraftDocument(strokeBefore!, document)) {
            undo.add(strokeBefore!);
            redo.clear();
          }
          strokeBefore = null;
        }
        final gate = effectCommand ? effectReplyGate : paintReplyGate;
        if (gate == null) {
          state(request);
        } else {
          unawaited(gate.then((_) => state(request)));
        }
      case LitchcraftV2Protocol.undo:
        if (physicalBrushHeld || undo.isEmpty) {
          gatedState(request, historyReplyGate, status: 1);
          return;
        }
        redo.add(document);
        document = undo.removeLast();
        selectedFrame = selectedFrame.clamp(0, document.frames.length - 1);
        playing = false;
        revision++;
        gatedState(request, historyReplyGate);
      case LitchcraftV2Protocol.redo:
        if (physicalBrushHeld || redo.isEmpty) {
          gatedState(request, historyReplyGate, status: 1);
          return;
        }
        undo.add(document);
        document = redo.removeLast();
        selectedFrame = selectedFrame.clamp(0, document.frames.length - 1);
        playing = false;
        revision++;
        gatedState(request, historyReplyGate);
      case LitchcraftV2Protocol.frameEdit:
        if (physicalBrushHeld) {
          gatedState(request, frameEditReplyGate, status: 1);
          return;
        }
        if (data.getUint32(9, Endian.little) != revision ||
            bytes[14] >= document.frames.length ||
            (bytes[13] == 0 && bytes[14] != selectedFrame)) {
          gatedState(request, frameEditReplyGate, status: 9);
          return;
        }
        final edit = LitchcraftDocumentSession(document)
          ..selectFrame(bytes[14]);
        switch (bytes[13]) {
          case 0:
            if (document.frames.length == 8) {
              gatedState(request, frameEditReplyGate, status: 3);
              return;
            }
            edit.addFrame();
          case 1:
            if (document.frames.length == 1) {
              gatedState(request, frameEditReplyGate, status: 3);
              return;
            }
            edit.deleteSelectedFrame();
          case 3:
            edit.setSelectedFrameDuration(data.getUint16(16, Endian.little));
          default:
            gatedState(request, frameEditReplyGate, status: 4);
            return;
        }
        if (!sameLitchcraftDocument(document, edit.document)) {
          undo.add(document);
          redo.clear();
          document = edit.document;
          revision++;
        }
        selectedFrame = edit.selectedFrameIndex;
        playing = false;
        gatedState(request, frameEditReplyGate);
      case LitchcraftV2Protocol.streamBegin:
        if (!supportsBulkDocument) {
          state(request, status: active ? 4 : 2);
          return;
        }
        staged.clear();
        crc = data.getUint32(15, Endian.little);
        state(request);
      case LitchcraftV2Protocol.streamChunk:
        staged.addAll(bytes.skip(13));
      case LitchcraftV2Protocol.replaceBeginOp:
        staged.clear();
        crc = data.getUint32(15, Endian.little);
        state(request);
      case LitchcraftV2Protocol.replaceChunkOp:
        staged.addAll(bytes.skip(13));
        state(request);
      case LitchcraftV2Protocol.streamCheckpoint:
        state(request);
      case LitchcraftV2Protocol.streamCommit:
      case LitchcraftV2Protocol.replaceCommitOp:
        undo.add(document);
        redo.clear();
        document = decodeLitchcraftAnimationDocument(staged, crc!);
        revision++;
        state(request);
      default:
        state(request);
    }
  }

  Future<void> close() async {
    await packets.close();
    await _session.packets.close();
    await _session.sessionPackets.close();
    for (final connection in _session.connections) {
      await connection.close();
    }
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

/// Domain integration facade for schema-3 document consumers. The controller's
/// current Store-root binding is intentionally outside this fixture's scope;
/// native shared Store journeys verify that product lifecycle. Document packets,
/// admission, retained results, calibration and reverse grants still cross the
/// production mobile transport and the existing shared receiver above.
class LitchcraftV2FunctionalService extends LitchBleService {
  LitchcraftV2FunctionalService(LitchcraftV2FunctionalPeer peer)
    : super(pendingOtaStore: MemoryOtaTargetStore(), ble: peer) {
    _transport = LitchBleService(
      pendingOtaStore: MemoryOtaTargetStore(),
      ble: peer,
      gattSessionFactory: peer,
    );
    debugSetConnectedDevice('board');
  }
  late final LitchBleService _transport;
  static const _capabilities = LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: LitchHardwareRevision.v3,
    capabilityMask: (1 << 10) | (1 << 12) | (1 << 14),
    explicitlyReported: true,
  );
  int capabilityFetches = 0;
  Future<void> Function()? beforeArtworkPacketSize;

  @override
  int get connectionSessionToken => _transport.connectionSessionToken;
  @override
  Stream<ConnectionStateUpdate> get connectionStream =>
      _transport.connectionStream;

  @override
  void debugSetConnectedDevice(String deviceId) {
    super.debugSetConnectedDevice(deviceId);
    _transport.debugSetConnectedDevice(deviceId);
    _transport.debugSetDeviceCapabilities(deviceId, _capabilities);
  }

  @override
  Object? latestSettingValue(String key) =>
      key == 'GAME_FIRST_PLAYER' ? 1 : false;
  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) => _capabilities;
  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async {
    capabilityFetches++;
    return _capabilities;
  }

  @override
  Future<ConnectionSession?> openConnectionSession(String deviceId) async =>
      null;
  @override
  Future<bool> litchcraftControlAvailable(String deviceId) =>
      _transport.litchcraftControlAvailable(deviceId);
  @override
  Stream<List<int>> litchcraftPackets(String deviceId) =>
      _transport.litchcraftPackets(deviceId);
  @override
  Future<int> litchcraftPacketPayloadBytes(
    String deviceId, {
    required int connectionSessionToken,
  }) async {
    await beforeArtworkPacketSize?.call();
    return _transport.litchcraftPacketPayloadBytes(
      deviceId,
      connectionSessionToken: connectionSessionToken,
    );
  }

  @override
  Future<void> writeLitchcraftPacket(
    String deviceId,
    List<int> bytes, {
    bool withResponse = true,
  }) => _transport.writeLitchcraftPacket(
    deviceId,
    bytes,
    withResponse: withResponse,
  );
  @override
  Future<void> writeLitchcraftV2Packet(
    String deviceId,
    List<int> bytes, {
    required int connectionSessionToken,
    bool Function()? isCurrent,
    Future<void>? cancellationFuture,
  }) => _transport.writeLitchcraftV2Packet(
    deviceId,
    bytes,
    connectionSessionToken: connectionSessionToken,
    isCurrent: isCurrent,
    cancellationFuture: cancellationFuture,
  );
  @override
  Future<void> writeLitchcraftBulkDocumentPacket(
    String deviceId,
    List<int> bytes, {
    required int connectionSessionToken,
  }) => _transport.writeLitchcraftBulkDocumentPacket(
    deviceId,
    bytes,
    connectionSessionToken: connectionSessionToken,
  );
  @override
  Future<void> disconnect() async {
    // Retire the facade and the production shared-session owner on the same link.
    await Future.wait([super.disconnect(), _transport.disconnect()]);
  }

  @override
  Future<void> dispose() async {
    await _transport.dispose();
    await super.dispose();
  }
}
