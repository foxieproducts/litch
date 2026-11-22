import 'support/memory_ota_target_store.dart';
import 'support/litchcraft_packet_observers.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_reactive_ble/flutter_reactive_ble.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:litch_mobile/l10n/generated/app_localizations.dart';
import 'package:litch_mobile/src/controllers/litchcraft_controller.dart';
import 'package:litch_mobile/src/controllers/litchcraft_library.dart';
import 'package:litch_mobile/src/models/litch_device_capabilities.dart';
import 'package:litch_mobile/src/models/litch_piece_color.dart';
import 'package:litch_mobile/src/models/litchcraft_state.dart';
import 'package:litch_mobile/src/models/litchcraft_animation.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_codec.dart';
import 'package:litch_mobile/src/models/litchcraft_animation_renderer.dart';
import 'package:litch_mobile/src/models/litchcraft_document_session.dart';
import 'package:litch_mobile/src/models/litchcraft_bouncing_balls_preset.dart';
import 'package:litch_mobile/src/models/litchcraft_surprise_presets.dart';
import 'package:litch_mobile/src/models/litchcraft_wire_codec.dart';
import 'package:litch_mobile/src/models/home_state_notification.dart';
import 'package:litch_mobile/src/models/litch_control_action.dart';
import 'package:litch_mobile/src/services/litch_ble_service.dart';
import 'package:litch_mobile/src/services/app_trace_recorder.dart';
import 'package:litch_mobile/src/services/litchcraft_artwork_store.dart';
import 'package:litch_mobile/src/services/connection_session.dart';
import 'package:litch_mobile/src/services/connection_session_codec.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_codec.dart';
import 'package:litch_mobile/src/services/connection_litchcraft_frame_store.dart';
import 'package:litch_mobile/src/services/litchcraft_frame_store_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_protocol.dart';
import 'package:litch_mobile/src/services/litchcraft_phone_shake.dart';
import 'package:litch_mobile/src/screens/litchcraft_screen.dart';
import 'package:litch_mobile/src/widgets/litchcraft_canvas.dart';
import 'package:shared_preferences/shared_preferences.dart';

class _EntryProbeTrace implements AppTraceSink {
  final failures = <Map<String, Object?>>[];
  final recoveries = <Map<String, Object?>>[];
  final records = <Map<String, Object?>>[];

  @override
  void record(String event, {Map<String, Object?> fields = const {}}) {
    records.add({'event': event, ...fields});
    if (event == 'litchcraft.transport_failed') {
      failures.add(Map.of(fields));
    } else if (event == 'litchcraft.recovery_scheduled') {
      recoveries.add(Map.of(fields));
    }
  }
}

class _CountingArtworkStore extends LitchcraftArtworkStore {
  _CountingArtworkStore(super.preferences);

  int saves = 0;

  @override
  Future<void> save(LitchcraftLibrarySnapshot snapshot) {
    saves++;
    return super.save(snapshot);
  }
}

class _UnusedSdk implements FlutterReactiveBle {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _Request {
  _Request(List<int> bytes, {this.withResponse = true})
    : bytes = List.unmodifiable(bytes);
  final List<int> bytes;
  final bool withResponse;
  int get opcode => bytes[1];
  int get session => ByteData.sublistView(
    Uint8List.fromList(bytes),
  ).getUint32(2, Endian.little);
  int get id => bytes[6] | (bytes[7] << 8);
  List<int> get payload => bytes.sublist(8);
}

/// Scripted protocol peer only. Hall/firmware domain behavior is deliberately
/// not reimplemented here: tests explicitly supply authoritative frames.
class _Ble extends LitchBleService {
  _Ble() : super(pendingOtaStore: MemoryOtaTargetStore(), ble: _UnusedSdk());
  final connections = StreamController<ConnectionStateUpdate>.broadcast(
    sync: true,
  );
  final packets = StreamController<List<int>>.broadcast(sync: true);
  final homes = StreamController<HomeStateNotification>.broadcast(sync: true);
  final settings = StreamController<String>.broadcast(sync: true);
  HomeStateNotification? home;
  Future<bool> Function()? onHomeRefresh;
  Future<LitchControlEvent> Function(LitchControlAction)? onControlAction;
  @override
  Future<bool> requestCurrentHomeStateSnapshot(String deviceId) async =>
      await onHomeRefresh?.call() ?? false;
  @override
  Future<LitchControlEvent> dispatchControlAction({
    required String deviceId,
    required LitchControlAction action,
    LitchControlOwner? owner,
    bool? enabled,
    int undoCount = 1,
  }) =>
      onControlAction?.call(action) ??
      super.dispatchControlAction(
        deviceId: deviceId,
        action: action,
        owner: owner,
        enabled: enabled,
        undoCount: undoCount,
      );
  @override
  Stream<HomeStateNotification> get homeStateStream => homes.stream;
  @override
  Stream<String> get settingsChangedStream => settings.stream;
  @override
  HomeStateNotification? get latestHomeState => home;
  final requests = <_Request>[];
  final bulkFrames = <List<int>>[];
  final settingReads = <String>[];
  Object? cachedFirstPlayer = 1;
  Object? cachedBoardRotate = false;
  Future<Object?> Function()? onReadFirstPlayer;
  Future<Object?> Function()? onReadBoardRotate;
  bool connected = true, supported = true, active = true, waiting = false;
  LitchHardwareRevision hardwareRevision = LitchHardwareRevision.v3;
  int connectionToken = 0;
  @override
  int get connectionSessionToken => connectionToken;
  int colorPickPhase = LitchcraftProtocol.colorPickIdle;
  String deviceId = 'scripted-board';
  bool bulkReplaceSupported = true;
  bool inspectSupported = false;
  bool paletteSyncSupported = false;
  bool shakeEraseSupported = true;
  int litchcraftSubscriptionCount = 0;
  int session = 41,
      revision = 10,
      undoCount = 0,
      redoCount = 0,
      enterStatus = 0;
  int brushColor = 0xff7430, brushMask = 15;
  bool brushErase = false;
  LitchcraftCanvas artwork = LitchcraftCanvas.blank();
  void Function(_Request)? onSnapshot;
  void Function(_Request)? onInspect;
  Future<void> Function(_Request)? onMutation;
  Future<void> Function(List<List<int>> frames, int request)? onBulk;

  @override
  Stream<ConnectionStateUpdate> get connectionStream => connections.stream;
  @override
  bool get isConnected => connected;
  @override
  String? get connectedId => connected ? deviceId : null;
  // This base peer scripts domain responses. Shared-session lifecycle cases
  // opt into _AdvancedBle's existing current receiver below.
  @override
  Future<bool> litchcraftControlAvailable(String deviceId) async => supported;
  @override
  Future<ConnectionSession?> openConnectionSession(String deviceId) async =>
      null;
  @override
  Object? latestSettingValue(String key) => switch (key) {
    'GAME_FIRST_PLAYER' => cachedFirstPlayer,
    'BOARD_ROTATE' => cachedBoardRotate,
    _ => null,
  };
  @override
  Future<Object?> readSettingValue({
    required String deviceId,
    required String key,
  }) async {
    expectSync(deviceId, 'scripted-board');
    settingReads.add(key);
    return switch (key) {
      'GAME_FIRST_PLAYER' => await onReadFirstPlayer?.call(),
      'BOARD_ROTATE' => await onReadBoardRotate?.call(),
      _ => null,
    };
  }

  @override
  Stream<List<int>> litchcraftPackets(String deviceId) {
    litchcraftSubscriptionCount += 1;
    return packets.stream;
  }

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async => supported
      ? LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: hardwareRevision,
          capabilityMask:
              LitchDeviceCapabilities.litchcraftBit |
              (bulkReplaceSupported
                  ? LitchDeviceCapabilities.litchcraftBulkBit
                  : 0) |
              (inspectSupported
                  ? LitchDeviceCapabilities.litchcraftInspectBit
                  : 0) |
              (paletteSyncSupported
                  ? LitchDeviceCapabilities.litchcraftPaletteBit
                  : 0) |
              (shakeEraseSupported
                  ? LitchDeviceCapabilities.litchcraftShakeEraseBit
                  : 0),
          explicitlyReported: true,
        )
      : LitchDeviceCapabilities.unavailable;

  @override
  Future<void> writeLitchcraftPacket(
    String deviceId,
    List<int> bytes, {
    bool withResponse = true,
  }) async {
    final request = _Request(bytes, withResponse: withResponse);
    requests.add(request);
    if (request.opcode == LitchcraftProtocol.snapshot) {
      if (onSnapshot case final handler?) {
        handler(request);
      } else {
        snapshot(request.id);
      }
      return;
    }
    if (request.opcode == LitchcraftProtocol.inspect) {
      if (onInspect case final handler?) {
        handler(request);
      } else {
        state(request.id);
        colorPick(colorPickPhase);
      }
      return;
    }
    if (onMutation case final handler?) {
      await handler(request);
      return;
    }
    if (request.opcode == LitchcraftProtocol.enter) {
      if (enterStatus != 0) {
        state(request.id, status: enterStatus);
        return;
      }
      if (!active) {
        if (session == 0) session = 1;
        active = true;
        ++revision;
      }
    } else if (request.opcode == LitchcraftProtocol.exit) {
      active = false;
      ++revision;
    } else if (request.opcode == LitchcraftProtocol.shakeErase) {
      if (artwork.hasMarks) {
        artwork = LitchcraftCanvas.blank(topology: artwork.topology);
        ++revision;
        emit(
          LitchcraftProtocol.gesture,
          atRevision: revision,
          payload: const <int>[LitchcraftProtocol.gestureShakeErase, 1, 0],
        );
      }
    }
    state(request.id);
  }

  @override
  Future<void> writeLitchcraftBulkArtwork(
    String deviceId,
    int sessionId,
    int request,
    List<int> colors,
  ) async {
    final frames = LitchcraftProtocol.bulkReplaceFrames(
      session: sessionId,
      request: request,
      colors: colors,
      maximumPacketBytes: 244,
    );
    bulkFrames.addAll(frames);
    if (onBulk case final handler?) {
      await handler(frames, request);
      return;
    }
    artwork = LitchcraftCanvas(colors: colors, topology: artwork.topology);
    ++revision;
    state(request);
  }

  List<int> statePayload({int status = 0}) => [
    status,
    (active ? 1 : 0) | (waiting ? 2 : 0),
    artwork.topology.id,
    undoCount,
    redoCount,
    brushErase ? 1 : 0,
    brushMask,
    0,
  ];

  void emit(
    int event, {
    int? sessionId,
    int? atRevision,
    int requestId = 0,
    List<int> payload = const [],
  }) {
    final bytes = Uint8List(12 + payload.length);
    final data = ByteData.sublistView(bytes);
    bytes[0] = LitchcraftProtocol.magic;
    bytes[1] = event;
    data.setUint32(2, sessionId ?? session, Endian.little);
    data.setUint32(6, atRevision ?? revision, Endian.little);
    data.setUint16(10, requestId, Endian.little);
    bytes.setRange(12, bytes.length, payload);
    packets.add(bytes);
  }

  void state(int id, {int status = 0, int? atRevision, int? sessionId}) => emit(
    LitchcraftProtocol.state,
    requestId: id,
    atRevision: atRevision,
    sessionId: sessionId,
    payload: statePayload(status: status),
  );

  void colorPick(int phase) =>
      emit(LitchcraftProtocol.colorPick, payload: <int>[phase]);

  void begin(int id, {int? atRevision, int? sessionId}) => emit(
    LitchcraftProtocol.snapshotBegin,
    requestId: id,
    atRevision: atRevision,
    sessionId: sessionId,
    payload: statePayload(),
  );

  void chunks(
    int id,
    LitchcraftCanvas value, {
    int from = 0,
    int until = 256,
    int? atRevision,
    int? sessionId,
  }) {
    for (var offset = from; offset < until; offset += 2) {
      emit(
        LitchcraftProtocol.snapshotChunk,
        requestId: id,
        atRevision: atRevision,
        sessionId: sessionId,
        payload: [
          ...LitchcraftProtocol.u16(offset),
          ...value.colors
              .skip(offset)
              .take((until - offset).clamp(0, 2))
              .expand(LitchcraftProtocol.rgb),
        ],
      );
    }
  }

  void snapshot(int id) {
    state(id);
    begin(id);
    emit(
      LitchcraftProtocol.brush,
      requestId: id,
      payload: [
        ...LitchcraftProtocol.rgb(brushColor),
        brushErase ? 1 : 0,
        brushMask,
      ],
    );
    chunks(id, artwork);
    emit(LitchcraftProtocol.snapshotEnd, requestId: id);
    colorPick(colorPickPhase);
  }

  void connection(bool value) {
    final bool wasConnected = connected;
    connected = value;
    if (value && !wasConnected) ++connectionToken;
    connections.add(
      ConnectionStateUpdate(
        deviceId: 'scripted-board',
        connectionState: value
            ? DeviceConnectionState.connected
            : DeviceConnectionState.disconnected,
        failure: null,
      ),
    );
  }

  Future<void> shutdown() async {
    await homes.close();
    await settings.close();
    await connections.close();
    await packets.close();
    await super.dispose();
  }
}

class _TestFrameResident {
  const _TestFrameResident({
    required this.rootSlot,
    required this.generation,
    required this.document,
    required this.body,
  });

  final int rootSlot;
  final int generation;
  final LitchcraftAnimationDocument document;
  final List<int> body;
}

class _AdvancedBle extends _Ble {
  int packetPayloadBytes = 20;
  bool forceMinimumConnectionPacket = false;
  bool connectionSessionSupported = false;
  bool animationSupported = true;
  bool connectionSessionAvailable = true;
  bool admitStoreReadsBeforeResponse = false;
  bool simulateMissingStoreRoot = false;
  StreamController<List<int>>? _connectionPackets;
  ConnectionSession? _connectionSession;
  List<int>? _lastStoreReceipt;
  Uint8List? _lastPhysicalStoreEdit;
  Uint8List? _lastPhysicalStoreBatch;
  int physicalStoreEditRevision = 0;
  int _reverseObjectSequence = 0;
  int _connectionLitchcraftStateSequence = 0;

  @override
  Future<int> litchcraftPacketPayloadBytes(
    String deviceId, {
    required int connectionSessionToken,
  }) async => packetPayloadBytes;
  @override
  Future<void> writeLitchcraftV2Packet(
    String deviceId,
    List<int> bytes, {
    required int connectionSessionToken,
    bool Function()? isCurrent,
    Future<void>? cancellationFuture,
  }) {
    expectSync(connectionSessionToken, connectionToken);
    return writeLitchcraftPacket(deviceId, bytes);
  }

  @override
  Future<void> writeLitchcraftBulkDocumentPacket(
    String deviceId,
    List<int> bytes, {
    required int connectionSessionToken,
  }) {
    expectSync(connectionSessionToken, connectionToken);
    return writeLitchcraftPacket(deviceId, bytes, withResponse: false);
  }

  _AdvancedBle() {
    final local = LitchcraftDocumentSession(
      staticLitchcraftDocument(
        LitchcraftCanvas.blank().paint([0], 15, 0x123456),
      ),
    )..addFrame();
    document = local.document;
  }
  late LitchcraftAnimationDocument document;
  final operations = <int>[];
  final orderedOperations = <int>[];
  final orderedPaintSquareCounts = <int>[];
  final orderedPaintBodies = <List<int>>[];
  final replacementSessions = <int>[];
  final staged = <int>[];
  final timelineFrames = <int>[];
  Future<void> Function()? beforeSnapshot;
  Future<void> Function()? beforeCommit;
  Future<void> Function()? beforeFrameStoreCommit;
  Future<void> Function()? beforeFrameStoreInspect;
  void Function()? onConnectionLitchcraftStateQuery;
  void Function()? onStoreAuthorityQuery;
  Future<void> Function()? beforeFrameStoreResidents;
  Future<void> Function()? beforeFrameStoreSelect;
  Completer<void>? deferredFrameStoreRoot;
  bool frameStoreCommitAcceptedPending = false;
  bool frameStoreCommitIncludesResidents = true;
  bool rejectFrameStoreCommit = false;
  bool stalePausedCommitReceipt = false;
  Future<void> Function()? beforeFrameStoreChunk;
  bool dropFrameStoreCheckpointReceipt = false;
  void Function()? onFrameStoreCheckpoint;
  Future<void> Function()? beforeFrameStoreSnapshot;
  int? rejectNextFrameStoreSnapshotStatus;
  Future<void> Function()? beforeFrameStorePreview;
  Future<void> Function()? beforeOrderedTimeline;
  Future<void> Function()? beforeOrderedToolCheckpoint;
  Future<void> Function()? beforeOrderedPaint;
  Future<void> Function()? beforeOrderedCanvas;
  Future<void> Function()? afterOrderedCanvasBeforeCheckpoint;
  final orderedCanvasBodies = <List<int>>[];
  Future<void> Function()? afterOrderedPaintBeforeCheckpoint;
  Future<void> Function()? beforePaint;
  int? stagedCrc;
  int snapshotCount = 0;
  int frameStoreSnapshotTransactionCount = 0;
  int frameStoreLiveLayoutCount = 0, frameStoreLiveFrameCount = 0;
  int frameStorePreviewCount = 0, frameStorePreviewBodyBytes = 0;
  int frameStoreInspectCount = 0;
  int storeAuthorityQueryCount = 0;
  int? dropStoreAuthorityQueryAt;
  final effectClockInspectRequests = <int>[];
  final inspectionPackets = <Uint8List>[];
  final frameStoreInspectCapabilities = <int>[];
  int paintCount = 0, effectCount = 0, addFrameCount = 0, deleteFrameCount = 0;
  int frameStoreGeneration = 0;
  int frameStoreLiveRevision = 1;
  int? guardedStoreRevision;
  int? guardedStoreGeneration;
  final storeUndo = <LitchcraftAnimationDocument>[];
  final storeRedo = <LitchcraftAnimationDocument>[];
  LitchcraftAnimationDocument? storeStrokeBefore;
  int storeAuthorityStateSequence = 0;
  Uint8List? _lastStoreAuthorityBody;
  bool storeLiveDirty = false;
  int boardBootNonce = 0x123456789abcdef;
  bool delayStoreAuthorityAfterOrderedPaint = false;
  bool rejectNextAdvancedPaintBusy = false;
  bool dropNextStoreAuthorityQuery = false;
  bool dropStoreAuthorityQueries = false;
  int frameStoreFrameCount = 0;
  int frameStoreBodyBytes = 0;
  bool frameStoreRgbWireCapable = false;
  bool frameStoreTransferRgbWire = false;
  int frameStoreCursor = 0;
  int frameStoreTransfer = 0;
  int frameStoreTopology = 3;
  final frameStoreBody = <int>[];
  final frameStoreStagedBody = <int>[];
  final _frameStoreResidents = <_TestFrameResident>[];
  bool fragmentStoreReceipts = false;
  int frameStoreSelectedRoot = 0;
  int frameStoreSparkleSeed = 1;
  List<int> frameStoreFavorites = List<int>.of(litchcraftDefaultFavoriteColors);
  bool frameStorePredecessorDeltas = true;
  bool frameStoreHasArtwork = false;
  bool frameStoreActive = false;
  bool frameStorePlaying = false;
  bool frameStorePlaybackOnEntry = false;
  bool frameStoreSupported = true;
  int reportedSchema = LitchcraftV2Protocol.schema;
  final editStrokes = <int>[];
  final editFlags = <int>[];
  bool rejectDocumentMutationBusy = false;
  bool rejectTimelineBusy = false;
  int? inspectStatus;
  int? rejectNextOrderedTimelineReason;
  bool rejectReplaceBeginBusy = false;
  int? rejectNextEffectStatus;
  Future<void> Function()? beforeEffect;
  bool playing = false;
  int selectedFrame = 0;
  int elapsedMilliseconds = 0;

  void publishStoreAuthority() {
    final body = Uint8List(10);
    final data = ByteData.sublistView(body);
    body[0] = 1;
    body[1] = storeLiveDirty ? 1 : 0;
    data.setUint32(2, frameStoreGeneration, Endian.little);
    data.setUint32(6, frameStoreLiveRevision, Endian.little);
    final previous = _lastStoreAuthorityBody;
    if (previous == null || !listEquals(previous, body)) {
      _lastStoreAuthorityBody = Uint8List.fromList(body);
      storeAuthorityStateSequence += 1;
    }
    _connectionPackets?.add(
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: ConnectionSession.litchcraftStoreAuthorityStateCode,
          token: 0x80000001,
          sequence: storeAuthorityStateSequence,
          body: body,
        ),
      ),
    );
  }

  void persistStore() {
    frameStoreBody
      ..clear()
      ..addAll(_frameStoreBody(document));
    storeLiveDirty = false;
    frameStoreGeneration += 1;
    publishStoreAuthority();
  }

  @override
  Future<LitchDeviceCapabilities> fetchDeviceCapabilities({
    required String deviceId,
    bool refresh = false,
  }) async => LitchDeviceCapabilities(
    contractVersion: 3,
    hardwareRevision: hardwareRevision,
    capabilityMask:
        (1 << 10) |
        (1 << 12) |
        (paletteSyncSupported
            ? LitchDeviceCapabilities.litchcraftPaletteBit
            : 0) |
        (animationSupported ? 1 << 14 : 0),
    explicitlyReported: true,
  );

  @override
  LitchDeviceCapabilities deviceCapabilities(String deviceId) =>
      connectionSessionSupported
      ? LitchDeviceCapabilities(
          contractVersion: 3,
          hardwareRevision: hardwareRevision,
          capabilityMask:
              (1 << 10) |
              (1 << 12) |
              (paletteSyncSupported
                  ? LitchDeviceCapabilities.litchcraftPaletteBit
                  : 0) |
              (animationSupported ? 1 << 14 : 0),
          explicitlyReported: true,
        )
      : super.deviceCapabilities(deviceId);

  @override
  Future<ConnectionSession?> openConnectionSession(String deviceId) async {
    if (!connectionSessionSupported || !connectionSessionAvailable) return null;
    if (!simulateMissingStoreRoot && !frameStoreHasArtwork) {
      // Current V3 enters with Store authority. Keep the ordinary modern
      // peer's artwork in that Store; missing-root tests opt out explicitly.
      frameStoreHasArtwork = true;
      frameStoreGeneration = 1;
      frameStoreFrameCount = document.frames.length;
      frameStoreActive = active;
      frameStoreBody
        ..clear()
        ..addAll(_frameStoreBody(document));
    }
    final existing = _connectionSession;
    if (existing != null) return existing;
    final selectedMaximum = forceMinimumConnectionPacket
        ? ConnectionSessionCodec.minimumPacketBytes
        : (packetPayloadBytes < 244 ? 244 : packetPayloadBytes);
    final packets = StreamController<List<int>>(sync: true);
    _connectionPackets = packets;
    const connectionToken = 0x41;
    const orderedToken = 0x42;
    const objectToken = 0x43;
    const reverseObjectToken = 0x80000002;
    var objectAcceptedThrough = 0;
    final objectAssembler = ConnectionLitchcraftAssembler();

    Uint8List control(
      int code, {
      int token = connectionToken,
      int sequence = 0,
      Uint8List? body,
    }) => ConnectionSessionCodec.encode(
      ConnectionSessionPacket(
        kind: ConnectionSessionKind.control,
        code: code,
        token: token,
        sequence: sequence,
        body: body ?? Uint8List(0),
      ),
    );

    void orderedProgress(int sequence) {
      final body = Uint8List(10);
      final data = ByteData.sublistView(body);
      data.setUint32(0, sequence, Endian.little);
      data.setUint32(4, sequence + 8, Endian.little);
      packets.add(
        control(
          ConnectionSessionControl.progress,
          token: orderedToken,
          sequence: sequence,
          body: body,
        ),
      );
    }

    Future<void> send(Uint8List bytes) async {
      final packet = ConnectionSessionCodec.decode(
        bytes,
        packetLimit: selectedMaximum,
      )!;
      if (packet.kind == ConnectionSessionKind.object) {
        expectSync(packet.token, objectToken);
        if (packet.code == ConnectionLitchcraftCodec.objectOpcode) {
          final completed = objectAssembler.accept(packet.body);
          if (completed != null) {
            final handled = writeLitchcraftPacket(
              deviceId,
              completed,
              withResponse: false,
            );
            if (admitStoreReadsBeforeResponse &&
                completed[2] == LitchcraftV2Protocol.frameStoreSnapshot) {
              // Production acknowledges Object admission before the storage
              // worker's read response; holding a UI read is not lost credit.
              unawaited(handled);
            } else {
              await handled;
            }
          }
        } else {
          expectSync(packet.code, packet.body[2]);
          await writeLitchcraftPacket(
            deviceId,
            packet.body,
            withResponse: false,
          );
        }
        objectAcceptedThrough = packet.sequence;
        final progress = Uint8List(10);
        final progressData = ByteData.sublistView(progress);
        progressData.setUint32(0, objectAcceptedThrough, Endian.little);
        progressData.setUint32(
          4,
          objectAcceptedThrough + ConnectionSessionCodec.maximumWindow,
          Endian.little,
        );
        packets.add(
          control(
            ConnectionSessionControl.progress,
            token: objectToken,
            sequence: objectAcceptedThrough,
            body: progress,
          ),
        );
        return;
      }
      if (packet.kind == ConnectionSessionKind.ordered) {
        expectSync(packet.token, orderedToken);
        orderedOperations.add(packet.code);
        if (packet.code == ConnectionSession.litchcraftToolOpcode) {
          expectSync(packet.body.length, 5);
          expectSync(packet.body[0], inInclusiveRange(0, 1));
          expectSync(packet.body[1], inInclusiveRange(1, 15));
          brushErase = packet.body[0] != 0;
          brushMask = packet.body[1];
          brushColor = LitchcraftProtocol.readRgb(packet.body, 2);
          final strokeBefore = storeStrokeBefore;
          if (strokeBefore != null &&
              !sameLitchcraftDocument(strokeBefore, document)) {
            storeUndo.add(strokeBefore);
            storeRedo.clear();
          }
          storeStrokeBefore = null;
          await beforeOrderedToolCheckpoint?.call();
          final checkpoint = Uint8List(10);
          ByteData.sublistView(
            checkpoint,
          ).setUint32(0, frameStoreLiveRevision, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: orderedToken,
              sequence: packet.sequence,
              body: checkpoint,
            ),
          );
          orderedProgress(packet.sequence);
          return;
        }
        if (packet.code == ConnectionSession.litchcraftStoreGuardOpcode) {
          final body = ByteData.sublistView(packet.body);
          final valid =
              packet.body.length == 8 &&
              body.getUint32(0, Endian.little) == frameStoreGeneration &&
              body.getUint32(4, Endian.little) == frameStoreLiveRevision;
          guardedStoreRevision = valid ? frameStoreLiveRevision : null;
          guardedStoreGeneration = valid ? frameStoreGeneration : null;
          if (!valid) {
            packets.add(
              control(
                ConnectionSessionControl.outcome,
                token: orderedToken,
                sequence: packet.sequence,
                body: Uint8List.fromList(<int>[1, 9, 0]),
              ),
            );
          }
          final checkpoint = Uint8List(10);
          final data = ByteData.sublistView(checkpoint);
          data.setUint32(0, frameStoreLiveRevision, Endian.little);
          if (!valid) data.setUint16(8, 1, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: orderedToken,
              sequence: packet.sequence,
              body: checkpoint,
            ),
          );
          orderedProgress(packet.sequence);
          return;
        }
        if (packet.code == 9) {
          orderedCanvasBodies.add(packet.body.toList());
          await beforeOrderedCanvas?.call();
          final frame = packet.body[0];
          final mask = packet.body[1];
          final color = LitchcraftProtocol.readRgb(packet.body, 2);
          final action = packet.body[5];
          final valid =
              packet.body.length == 6 &&
              frame == selectedFrame &&
              guardedStoreRevision == frameStoreLiveRevision &&
              guardedStoreGeneration == frameStoreGeneration &&
              storeStrokeBefore == null &&
              mask > 0 &&
              mask <= 15 &&
              (action == 0 || action == 1 || action >= 5 && action <= 8) &&
              (action != 1 || mask == 15 && color == 0) &&
              (action < 7 || color == 0);
          if (valid) {
            final edit = LitchcraftDocumentSession(document)
              ..selectFrame(frame);
            final erase = action == 1 || action >= 7;
            final squares = <int>[
              for (var square = 0; square < 64; square++)
                if (action < 5 ||
                    (((square % 8) + (square ~/ 8)).isOdd) ==
                        (action == 6 || action == 8))
                  square,
            ];
            edit.paintSelectedFrame(
              squares: squares,
              mask: mask,
              color: erase ? 0 : color,
              clearEffects: erase,
            );
            if (!sameLitchcraftDocument(document, edit.document)) {
              storeUndo.add(document);
              storeRedo.clear();
              document = edit.document;
              frameStoreLiveRevision++;
              guardedStoreRevision = frameStoreLiveRevision;
              storeLiveDirty = true;
              publishStoreAuthority();
            }
            frameStorePlaying = false;
            if (action == 0) {
              brushErase = false;
              brushMask = mask;
              brushColor = color;
            }
          } else {
            packets.add(
              control(
                ConnectionSessionControl.outcome,
                token: orderedToken,
                sequence: packet.sequence,
                body: Uint8List.fromList(<int>[1, 9, 0]),
              ),
            );
          }
          await afterOrderedCanvasBeforeCheckpoint?.call();
          final checkpoint = Uint8List(10);
          final data = ByteData.sublistView(checkpoint);
          data.setUint32(0, frameStoreLiveRevision, Endian.little);
          if (!valid) data.setUint16(8, 1, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: orderedToken,
              sequence: packet.sequence,
              body: checkpoint,
            ),
          );
          orderedProgress(packet.sequence);
          return;
        }
        if (packet.code == ConnectionSession.litchcraftUndoOpcode ||
            packet.code == ConnectionSession.litchcraftRedoOpcode) {
          final undo = packet.code == ConnectionSession.litchcraftUndoOpcode;
          final source = undo ? storeUndo : storeRedo;
          final destination = undo ? storeRedo : storeUndo;
          final valid =
              packet.body.isEmpty &&
              guardedStoreRevision == frameStoreLiveRevision &&
              source.isNotEmpty;
          if (valid) {
            destination.add(document);
            document = source.removeLast();
            frameStoreLiveRevision++;
            guardedStoreRevision = frameStoreLiveRevision;
            frameStorePlaying = false;
            storeLiveDirty = true;
            publishStoreAuthority();
          } else {
            packets.add(
              control(
                ConnectionSessionControl.outcome,
                token: orderedToken,
                sequence: packet.sequence,
                body: Uint8List.fromList(<int>[1, source.isEmpty ? 5 : 9, 0]),
              ),
            );
          }
          final checkpoint = Uint8List(10);
          final data = ByteData.sublistView(checkpoint);
          data.setUint32(0, frameStoreLiveRevision, Endian.little);
          if (!valid) data.setUint16(8, 1, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: orderedToken,
              sequence: packet.sequence,
              body: checkpoint,
            ),
          );
          orderedProgress(packet.sequence);
          return;
        }
        if (packet.code == ConnectionSession.litchcraftTimelineOpcode) {
          await beforeOrderedTimeline?.call();
          final reason = frameStoreCommitAcceptedPending
              ? 1
              : storeStrokeBefore != null && packet.body[0] != 0
              ? 1
              : rejectNextOrderedTimelineReason;
          if (!frameStoreCommitAcceptedPending) {
            rejectNextOrderedTimelineReason = null;
          }
          if (reason == null) {
            expectSync(packet.body.length, 6);
            expectSync(
              ByteData.sublistView(packet.body).getUint32(2, Endian.little),
              frameStoreGeneration,
            );
            switch (packet.body[0]) {
              case 0:
                frameStorePlaying = false;
                selectedFrame = packet.body[1];
              case 1:
                frameStorePlaying = true;
              case 2:
                frameStorePlaying = false;
                selectedFrame = packet.body[1];
            }
            connectionLitchcraftState(
              sequence: _connectionLitchcraftStateSequence + 1,
              selectedFrame: selectedFrame,
              frameElapsedMilliseconds: 0,
              playing: frameStorePlaying,
            );
          } else {
            final outcome = Uint8List.fromList([1, reason & 0xff, reason >> 8]);
            packets.add(
              control(
                ConnectionSessionControl.outcome,
                token: orderedToken,
                sequence: packet.sequence,
                body: outcome,
              ),
            );
          }
          final checkpoint = Uint8List(10);
          final checkpointData = ByteData.sublistView(checkpoint);
          checkpointData.setUint32(0, frameStoreGeneration, Endian.little);
          if (reason != null) checkpointData.setUint16(8, 1, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: orderedToken,
              sequence: packet.sequence,
              body: checkpoint,
            ),
          );
          return;
        }
        if (packet.code == ConnectionSession.litchcraftPaintOpcode ||
            packet.code == ConnectionSession.litchcraftEffectOpcode) {
          await beforeOrderedPaint?.call();
          final frameValid = packet.body[0] == selectedFrame;
          final guardValid = guardedStoreRevision == frameStoreLiveRevision;
          if (!frameValid || !guardValid) {
            packets.add(
              control(
                ConnectionSessionControl.outcome,
                token: orderedToken,
                sequence: packet.sequence,
                body: Uint8List.fromList(<int>[1, frameValid ? 9 : 3, 0]),
              ),
            );
            final checkpoint = Uint8List(10);
            final data = ByteData.sublistView(checkpoint);
            data.setUint32(0, frameStoreLiveRevision, Endian.little);
            data.setUint16(8, 1, Endian.little);
            packets.add(
              control(
                ConnectionSessionControl.checkpoint,
                token: orderedToken,
                sequence: packet.sequence,
                body: checkpoint,
              ),
            );
            orderedProgress(packet.sequence);
            return;
          }
          final before = document;
          final frame = packet.body[0];
          final mask = packet.body[4];
          final squares = packet.body.skip(8);
          final flags =
              packet.body[packet.code == ConnectionSession.litchcraftPaintOpcode
                  ? 3
                  : 7];
          if (flags & 1 != 0) storeStrokeBefore = document;
          final edit = LitchcraftDocumentSession(document)..selectFrame(frame);
          if (packet.code == ConnectionSession.litchcraftPaintOpcode) {
            paintCount++;
            orderedPaintSquareCounts.add(squares.length);
            orderedPaintBodies.add(packet.body.toList());
            for (final square in squares) {
              final prior = edit.document;
              edit.paintSelectedFrame(
                squares: [square],
                mask: mask,
                color:
                    packet.body[5] << 16 | packet.body[6] << 8 | packet.body[7],
                history: false,
              );
              if (!sameLitchcraftDocument(prior, edit.document)) {
                frameStoreLiveRevision++;
              }
            }
          } else {
            effectCount++;
            for (final square in squares) {
              final prior = edit.document;
              edit.applyEffect(
                square: square,
                mask: mask,
                effect: LitchcraftEffectStyle.fromId(packet.body[1]),
                effectDurationMs: packet.body[2] | packet.body[3] << 8,
              );
              if (!sameLitchcraftDocument(prior, edit.document)) {
                frameStoreLiveRevision++;
              }
            }
          }
          document = edit.document;
          if (!sameLitchcraftDocument(before, document)) {
            guardedStoreRevision = frameStoreLiveRevision;
            storeLiveDirty = true;
            if (!delayStoreAuthorityAfterOrderedPaint) {
              publishStoreAuthority();
            }
          }
          if (flags & 2 != 0) {
            final strokeBefore = storeStrokeBefore;
            if (strokeBefore != null &&
                !sameLitchcraftDocument(strokeBefore, document)) {
              storeUndo.add(strokeBefore);
              storeRedo.clear();
            }
            storeStrokeBefore = null;
          }
          selectedFrame = edit.selectedFrameIndex;
          frameStorePlaying = false;
          await afterOrderedPaintBeforeCheckpoint?.call();
          final checkpoint = Uint8List(10);
          ByteData.sublistView(
            checkpoint,
          ).setUint32(0, frameStoreLiveRevision, Endian.little);
          packets.add(
            control(
              ConnectionSessionControl.checkpoint,
              token: orderedToken,
              sequence: packet.sequence,
              body: checkpoint,
            ),
          );
          orderedProgress(packet.sequence);
          return;
        }
        expectSync(packet.code, LitchcraftV2Protocol.frameStoreCancel);
        await writeLitchcraftPacket(deviceId, packet.body);
        final checkpoint = Uint8List(10);
        ByteData.sublistView(
          checkpoint,
        ).setUint32(0, frameStoreGeneration, Endian.little);
        packets.add(
          control(
            ConnectionSessionControl.checkpoint,
            token: orderedToken,
            sequence: packet.sequence,
            body: checkpoint,
          ),
        );
        return;
      }
      switch (packet.code) {
        case ConnectionSessionControl.hello:
          final welcomeBody = Uint8List.fromList(packet.body);
          ByteData.sublistView(
            welcomeBody,
          ).setUint16(8, selectedMaximum, Endian.little);
          final identityBody = Uint8List(10);
          final identityData = ByteData.sublistView(identityBody);
          identityData.setUint64(0, boardBootNonce, Endian.little);
          identityData.setUint16(
            8,
            ConnectionSession.orderedFeature |
                ConnectionSession.stateFeature |
                ConnectionSession.objectFeature |
                ConnectionSession.reverseObjectFeature |
                ConnectionSession.storeGuardEpochFeature,
            Endian.little,
          );
          packets.add(
            control(ConnectionSessionControl.welcome, body: welcomeBody),
          );
          packets.add(
            control(ConnectionSessionControl.identity, body: identityBody),
          );
        case ConnectionSessionControl.ready:
          packets.add(
            control(
              ConnectionSessionControl.activated,
              body: Uint8List.fromList(packet.body),
            ),
          );
          final stateOpen = Uint8List(8);
          final stateOpenData = ByteData.sublistView(stateOpen);
          stateOpenData.setUint16(
            0,
            ConnectionSession.litchcraftStateTopic,
            Endian.little,
          );
          stateOpen[2] = 1;
          stateOpen[3] = ConnectionSession.stateCapacity;
          stateOpenData.setUint16(
            4,
            ConnectionSession.stateMaximumBody,
            Endian.little,
          );
          packets.add(
            control(
              ConnectionSessionControl.open,
              sequence: 1,
              body: stateOpen,
            ),
          );
          final reverseObjectOpen = Uint8List(8);
          final reverseObjectOpenData = ByteData.sublistView(reverseObjectOpen);
          reverseObjectOpenData.setUint16(
            0,
            ConnectionSession.litchcraftReverseObjectTopic,
            Endian.little,
          );
          reverseObjectOpen[2] = 1;
          reverseObjectOpen[3] = ConnectionSession.reverseObjectCapacity;
          reverseObjectOpenData.setUint16(
            4,
            selectedMaximum - 10,
            Endian.little,
          );
          reverseObjectOpenData.setUint16(
            6,
            ConnectionSession.expectedFeedbackMs,
            Endian.little,
          );
          packets.add(
            control(
              ConnectionSessionControl.open,
              sequence: 2,
              body: reverseObjectOpen,
            ),
          );
        case ConnectionSessionControl.open:
          final lane = packet.body[2];
          final openedBody = Uint8List(10);
          final openedData = ByteData.sublistView(openedBody);
          openedData.setUint32(
            0,
            lane == 0 ? orderedToken : objectToken,
            Endian.little,
          );
          openedData.setUint16(
            4,
            lane == 0 ? 10 : selectedMaximum - 10,
            Endian.little,
          );
          openedBody[6] = lane == 0 ? 8 : ConnectionSessionCodec.maximumWindow;
          openedBody[7] = lane;
          packets.add(
            control(
              ConnectionSessionControl.opened,
              sequence: packet.sequence,
              body: openedBody,
            ),
          );
          if (lane == 0) {
            final progress = Uint8List(10);
            ByteData.sublistView(progress).setUint32(4, 8, Endian.little);
            packets.add(
              control(
                ConnectionSessionControl.progress,
                token: orderedToken,
                body: progress,
              ),
            );
          }
        case ConnectionSessionControl.opened:
          expectSync(packet.token, connectionToken);
          expectSync(
            ByteData.sublistView(packet.body).getUint32(0, Endian.little),
            packet.sequence == 1 ? 0x80000001 : reverseObjectToken,
          );
          break;
        case ConnectionSessionControl.progress:
          if (packet.token == reverseObjectToken) break;
          fail('Unexpected client Progress token ${packet.token}');
        case ConnectionSessionControl.query:
          if (packet.token == objectToken) {
            if (packet.sequence != 0) {
              final receipt = _lastStoreReceipt;
              if (receipt != null &&
                  ByteData.sublistView(
                        Uint8List.fromList(receipt),
                      ).getUint16(11, Endian.little) ==
                      packet.sequence) {
                packets.add(
                  ConnectionSessionCodec.encode(
                    ConnectionSessionPacket(
                      kind: ConnectionSessionKind.state,
                      code: receipt[2],
                      token: objectToken,
                      sequence: packet.sequence,
                      body: Uint8List.fromList(receipt),
                    ),
                  ),
                );
              }
              break;
            }
            final progress = Uint8List(10);
            final progressData = ByteData.sublistView(progress);
            progressData.setUint32(0, objectAcceptedThrough, Endian.little);
            progressData.setUint32(
              4,
              objectAcceptedThrough + ConnectionSessionCodec.maximumWindow,
              Endian.little,
            );
            packets.add(
              control(
                ConnectionSessionControl.progress,
                token: objectToken,
                sequence: objectAcceptedThrough,
                body: progress,
              ),
            );
          } else if (packet.token == 0x80000001) {
            if (packet.sequence ==
                ConnectionSession.litchcraftStoreAuthorityStateCode) {
              storeAuthorityQueryCount++;
              if (dropStoreAuthorityQueries) break;
              if (dropStoreAuthorityQueryAt == storeAuthorityQueryCount) {
                dropStoreAuthorityQueryAt = null;
                break;
              }
              if (dropNextStoreAuthorityQuery) {
                dropNextStoreAuthorityQuery = false;
                break;
              }
              onStoreAuthorityQuery?.call();
              publishStoreAuthority();
              break;
            }
            if (packet.sequence ==
                ConnectionSession.litchcraftStoreEditStateCode) {
              final edit = _lastPhysicalStoreEdit;
              if (edit != null) packets.add(edit);
              break;
            }
            if (onConnectionLitchcraftStateQuery case final query?) {
              query();
              break;
            }
            connectionLitchcraftState(
              sequence: ++_connectionLitchcraftStateSequence,
              selectedFrame: selectedFrame,
              frameElapsedMilliseconds: 0,
              playing: frameStoreGeneration != 0 ? frameStorePlaying : playing,
            );
          }
        default:
          break;
      }
    }

    final sessionOwner = ConnectionSession(
      packets: packets.stream,
      send: send,
      clientNonce: 0x102030405060708,
      packetMaximum: selectedMaximum,
    );
    _connectionSession = sessionOwner;
    await sessionOwner.activate();
    return sessionOwner;
  }

  void installSavedStoreDocument(
    LitchcraftAnimationDocument saved, {
    int generation = 1,
  }) {
    document = saved;
    frameStoreGeneration = generation;
    frameStoreFrameCount = saved.frames.length;
    frameStoreTopology = saved.topology.id;
    frameStoreHasArtwork = true;
    frameStoreActive = active;
    frameStorePlaying = false;
    storeLiveDirty = false;
    selectedFrame = 0;
    frameStoreBody
      ..clear()
      ..addAll(_frameStoreBody(saved));
    _frameStoreResidents.clear();
  }

  Future<void> retireSharedConnection() async {
    connection(false);
    await _connectionSession?.dispose();
    await _connectionPackets?.close();
    _connectionSession = null;
    _connectionPackets = null;
    _reverseObjectSequence = 0;
    _connectionLitchcraftStateSequence = 0;
    storeAuthorityStateSequence = 0;
    _lastStoreAuthorityBody = null;
    _lastStoreReceipt = null;
  }

  @override
  Future<void> shutdown() async {
    await _connectionSession?.dispose();
    await _connectionPackets?.close();
    await super.shutdown();
  }

  void advancedEvent(int event, int request, [List<int> payload = const []]) {
    final header = ByteData(13)
      ..setUint8(0, 0x4d)
      ..setUint8(1, 2)
      ..setUint8(2, event)
      ..setUint32(3, session, Endian.little)
      ..setUint32(7, revision, Endian.little)
      ..setUint16(11, request, Endian.little);
    final response = <int>[...header.buffer.asUint8List(), ...payload];
    if (connectionSessionSupported &&
        event == LitchcraftV2Protocol.frameStoreCheckpointReceipt) {
      _emitStoreReceipt(response, request);
    } else {
      packets.add(response);
    }
  }

  void _emitStoreReceipt(List<int> response, int request) {
    _lastStoreReceipt = List<int>.from(response);
    final fragments = fragmentStoreReceipts
        ? ConnectionLitchcraftCodec.encodePacket(
            Uint8List.fromList(response),
            maximumBodyBytes: forceMinimumConnectionPacket ? 10 : 234,
          )
        : <Uint8List>[Uint8List.fromList(response)];
    for (final fragment in fragments) {
      _reverseObjectSequence += 1;
      _connectionPackets!.add(
        ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.object,
            code: fragmentStoreReceipts
                ? ConnectionLitchcraftCodec.receiptCode
                : response[2],
            token: 0x80000002,
            sequence: _reverseObjectSequence,
            body: fragment,
          ),
        ),
      );
    }
  }

  void physicalStorePaint({
    required int square,
    required int mask,
    required int color,
    bool clearEffects = false,
    int revisionStep = 1,
    bool emit = true,
  }) {
    if (frameStoreCommitAcceptedPending) return;
    physicalStoreEditRevision += revisionStep - 1;
    physicalStoreBatch(
      records: [
        (square: square, mask: mask, color: color, clearEffects: clearEffects),
      ],
      emit: emit,
    );
    final body = Uint8List(10);
    ByteData.sublistView(
      body,
    ).setUint32(0, frameStoreGeneration, Endian.little);
    body[4] = selectedFrame;
    body[5] = square;
    body[6] = mask | (clearEffects ? 0x10 : 0);
    body[7] = color >> 16;
    body[8] = color >> 8 & 0xff;
    body[9] = color & 0xff;
    final packet = ConnectionSessionCodec.encode(
      ConnectionSessionPacket(
        kind: ConnectionSessionKind.state,
        code: ConnectionSession.litchcraftStoreEditStateCode,
        token: 0x80000001,
        sequence: physicalStoreEditRevision,
        body: body,
      ),
    );
    _lastPhysicalStoreEdit = packet;
    if (emit) _connectionPackets!.add(packet);
  }

  void emitDelayedPhysicalStoreEdit() {
    final edit = _lastPhysicalStoreEdit;
    if (edit != null) _connectionPackets!.add(edit);
  }

  void physicalStoreBatch({
    required List<({int square, int mask, int color, bool clearEffects})>
    records,
    bool overflow = false,
    bool emitLatestStateFirst = false,
    bool emit = true,
    int? footprintOverride,
  }) {
    if (records.isEmpty || records.length > 10) {
      throw ArgumentError.value(records.length, 'records');
    }
    if (footprintOverride != null && records.length != 1) {
      throw ArgumentError.value(records.length, 'records');
    }
    final liveRevisions = <int>[];
    for (final record in records) {
      final squares = footprintOverride == null
          ? <int>[record.square]
          : <int>[
              for (var square = 0; square < 64; square++)
                if (footprintOverride & (1 << square) != 0) square,
            ];
      final edit = LitchcraftDocumentSession(document)
        ..selectFrame(selectedFrame)
        ..paintSelectedFrame(
          squares: squares,
          mask: record.mask,
          color: record.color,
          clearEffects: record.clearEffects,
        );
      document = edit.document;
      physicalStoreEditRevision++;
      frameStoreLiveRevision++;
      liveRevisions.add(frameStoreLiveRevision);
      storeLiveDirty = true;
    }
    publishStoreAuthority();
    final response = Uint8List(15 + records.length * 21);
    final data = ByteData.sublistView(response);
    response[0] = LitchcraftV2Protocol.magic;
    response[1] = LitchcraftV2Protocol.version;
    response[2] = LitchcraftV2Protocol.physicalStoreBatch;
    data.setUint32(3, session, Endian.little);
    data.setUint32(7, physicalStoreEditRevision, Endian.little);
    data.setUint16(
      11,
      physicalStoreEditRevision & 0xffff == 0
          ? 1
          : physicalStoreEditRevision & 0xffff,
      Endian.little,
    );
    response[13] = overflow ? 1 : 0;
    response[14] = records.length;
    for (var index = 0; index < records.length; index++) {
      final record = records[index];
      final offset = 15 + index * 21;
      data.setUint32(offset, frameStoreGeneration, Endian.little);
      response[offset + 4] = selectedFrame;
      response[offset + 5] = record.mask | (record.clearEffects ? 0x10 : 0);
      response[offset + 6] = record.color >> 16;
      response[offset + 7] = record.color >> 8 & 0xff;
      response[offset + 8] = record.color & 0xff;
      data.setUint64(
        offset + 9,
        footprintOverride ?? 1 << record.square,
        Endian.little,
      );
      data.setUint32(offset + 17, liveRevisions[index], Endian.little);
    }
    if (emitLatestStateFirst) {
      final latest = records.last;
      final body = Uint8List(10);
      ByteData.sublistView(
        body,
      ).setUint32(0, frameStoreGeneration, Endian.little);
      body[4] = selectedFrame;
      body[5] = latest.square;
      body[6] = latest.mask | (latest.clearEffects ? 0x10 : 0);
      body[7] = latest.color >> 16;
      body[8] = latest.color >> 8 & 0xff;
      body[9] = latest.color & 0xff;
      final packet = ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: ConnectionSession.litchcraftStoreEditStateCode,
          token: 0x80000001,
          sequence: physicalStoreEditRevision,
          body: body,
        ),
      );
      _lastPhysicalStoreEdit = packet;
      _connectionPackets!.add(packet);
    }
    _lastPhysicalStoreBatch = response;
    // Production LitchBleService routes this typed reverse Object directly
    // into the one Litchcraft packet stream after validating its body.
    if (emit) {
      _emitStoreReceipt(response, data.getUint16(11, Endian.little));
      packets.add(response);
    }
  }

  void emitDelayedPhysicalStoreBatch() {
    final batch = _lastPhysicalStoreBatch;
    if (batch != null) packets.add(batch);
  }

  void connectionLitchcraftState({
    required int sequence,
    required int selectedFrame,
    required int frameElapsedMilliseconds,
    required bool playing,
    bool transferSuspended = false,
  }) {
    if (sequence > _connectionLitchcraftStateSequence) {
      _connectionLitchcraftStateSequence = sequence;
    }
    final body = Uint8List(10);
    body[0] = selectedFrame;
    body[1] = document.frames.length == 256 ? 0 : document.frames.length;
    ByteData.sublistView(
      body,
    ).setUint16(2, frameElapsedMilliseconds, Endian.little);
    body[4] =
        (active ? 1 : 0) | (playing ? 2 : 0) | (transferSuspended ? 16 : 0);
    body[5] = brushMask;
    body[6] = brushColor >> 16;
    body[7] = brushColor >> 8 & 0xff;
    body[8] = brushColor & 0xff;
    _connectionPackets!.add(
      ConnectionSessionCodec.encode(
        ConnectionSessionPacket(
          kind: ConnectionSessionKind.state,
          code: ConnectionSession.litchcraftStateCode,
          token: 0x80000001,
          sequence: sequence,
          body: body,
        ),
      ),
    );
  }

  void advancedState(int request, {int status = 0}) =>
      advancedEvent(LitchcraftV2Protocol.state, request, [
        status,
        (active ? 1 : 0) | (playing ? 2 : 0),
        reportedSchema == 0 ? 0 : document.frames.length,
        reportedSchema == 0 ? 0 : selectedFrame,
        reportedSchema,
      ]);

  void frameStoreState(
    int request, {
    int status = 0,
    bool includeResidents = false,
  }) {
    if (includeResidents &&
        _frameStoreResidents.isEmpty &&
        frameStoreHasArtwork) {
      _frameStoreResidents.add(
        _TestFrameResident(
          rootSlot: frameStoreSelectedRoot,
          generation: frameStoreGeneration,
          document: document,
          body: List<int>.of(frameStoreBody),
        ),
      );
    }
    final header = ByteData(13)
      ..setUint8(0, LitchcraftV2Protocol.magic)
      ..setUint8(1, LitchcraftV2Protocol.version)
      ..setUint8(2, LitchcraftV2Protocol.frameStoreState)
      ..setUint32(3, session, Endian.little)
      ..setUint32(7, frameStoreGeneration, Endian.little)
      ..setUint16(11, request, Endian.little);
    final count = includeResidents ? _frameStoreResidents.length : 0;
    final payload = ByteData(includeResidents ? 8 + count * 45 : 6)
      ..setUint8(0, status)
      ..setUint8(
        1,
        (active ? 1 : 0) |
            (frameStorePlaying ? 2 : 0) |
            (frameStoreHasArtwork ? 4 : 0) |
            (frameStoreRgbWireCapable ? 0x20 : 0) |
            ((frameStoreTopology - 1) << 3),
      )
      ..setUint16(
        2,
        frameStoreHasArtwork ? frameStoreFrameCount : 0,
        Endian.little,
      )
      ..setUint16(4, selectedFrame, Endian.little);
    if (includeResidents) {
      payload
        // A scripted peer may start with artwork metadata but no resident
        // fixture. Keep that synthetic empty listing internally coherent.
        ..setUint8(
          6,
          count == 0 || !frameStoreHasArtwork ? 0xff : frameStoreSelectedRoot,
        )
        ..setUint8(7, count);
    }
    for (
      var residentIndex = 0;
      includeResidents && residentIndex < count;
      residentIndex++
    ) {
      final resident = _frameStoreResidents[residentIndex];
      final residentDocument = resident.document;
      final offset = 8 + residentIndex * 45;
      payload.setUint8(offset, resident.rootSlot);
      payload.setUint32(offset + 1, resident.generation, Endian.little);
      payload.setUint32(
        offset + 5,
        litchcraftAnimationCrc32(resident.body),
        Endian.little,
      );
      payload.setUint32(offset + 9, resident.body.length, Endian.little);
      payload.setUint16(
        offset + 13,
        residentDocument.frames.length,
        Endian.little,
      );
      payload.setUint8(offset + 15, residentDocument.topology.id);
      payload.setUint8(
        offset + 16,
        1 | (residentDocument.playbackOnEntry ? 2 : 0) | 4,
      );
      payload.setUint32(
        offset + 17,
        residentDocument.sparkleSeed,
        Endian.little,
      );
      for (var index = 0; index < 8; index++) {
        final color = residentDocument.favoriteColors[index];
        payload.setUint8(offset + 21 + index * 3, color >> 16);
        payload.setUint8(offset + 22 + index * 3, color >> 8 & 0xff);
        payload.setUint8(offset + 23 + index * 3, color & 0xff);
      }
    }
    final response = <int>[
      ...header.buffer.asUint8List(),
      ...payload.buffer.asUint8List(),
    ];
    if (connectionSessionSupported) {
      _emitStoreReceipt(response, request);
    } else {
      packets.add(response);
    }
  }

  void frameStoreSnapshot(
    int request,
    int cursor, {
    int? frame,
    bool live = false,
    bool layout = false,
  }) {
    final body = layout
        ? Uint8List(document.frames.length * 6)
        : frame == null
        ? frameStoreBody
        : _frameStoreBody(
            LitchcraftAnimationDocument(
              frames: [document.frames[frame]],
              topology: document.topology,
              favoriteColors: document.favoriteColors,
              playbackOnEntry: document.playbackOnEntry,
              sparkleSeed: document.sparkleSeed,
            ),
          );
    if (layout) {
      final rows = ByteData.sublistView(body as Uint8List);
      for (var i = 0; i < document.frames.length; i++) {
        final signature = LitchcraftFrameStoreFrameSignature.forFrame(
          document.frames[i],
        );
        rows.setUint16(i * 6, signature.bytes, Endian.little);
        rows.setUint32(i * 6 + 2, signature.crc32, Endian.little);
      }
    }
    if (frame != null && cursor == 0) frameStorePreviewBodyBytes += body.length;
    if (frameStoreCommitAcceptedPending) {
      final response = Uint8List(60);
      final data = ByteData.sublistView(response);
      response[0] = LitchcraftV2Protocol.magic;
      response[1] = LitchcraftV2Protocol.version;
      response[2] = LitchcraftV2Protocol.frameStoreSnapshotChunk;
      data.setUint32(3, session, Endian.little);
      data.setUint32(7, frameStoreGeneration, Endian.little);
      data.setUint16(11, request, Endian.little);
      response[13] = 10;
      _emitStoreReceipt(response, request);
      return;
    }
    final bodyCrc32 = litchcraftAnimationCrc32(body);
    while (cursor < body.length) {
      final header = live ? 78 : 60;
      final count = (body.length - cursor).clamp(0, 234 - header);
      final response = Uint8List(header + count);
      final data = ByteData.sublistView(response);
      response[0] = LitchcraftV2Protocol.magic;
      response[1] = LitchcraftV2Protocol.version;
      response[2] = LitchcraftV2Protocol.frameStoreSnapshotChunk;
      data.setUint32(3, session, Endian.little);
      data.setUint32(7, frameStoreGeneration, Endian.little);
      data.setUint16(11, request, Endian.little);
      response[14] =
          (active ? 1 : 0) |
          (frameStorePlaying ? 2 : 0) |
          4 |
          (live ? 64 | (layout ? 128 : 32) : (frame == null ? 8 : 32)) |
          (document.playbackOnEntry ? 16 : 0);
      response[15] = document.topology.id;
      data.setUint16(16, document.frames.length, Endian.little);
      data.setUint16(18, frame ?? selectedFrame, Endian.little);
      data.setUint32(20, body.length, Endian.little);
      data.setUint32(24, bodyCrc32, Endian.little);
      data.setUint32(28, cursor, Endian.little);
      data.setUint32(32, document.sparkleSeed, Endian.little);
      for (var index = 0; index < document.favoriteColors.length; index++) {
        final color = document.favoriteColors[index];
        response[36 + index * 3] = color >> 16;
        response[37 + index * 3] = color >> 8;
        response[38 + index * 3] = color;
      }
      if (live) {
        data.setUint32(60, frameStoreLiveRevision, Endian.little);
        data.setUint32(64, 1, Endian.little);
      }
      response.setRange(header, response.length, body, cursor);
      _emitStoreReceipt(response, request);
      cursor += count;
    }
  }

  void frameStoreResidents(int request, {int status = 0}) {
    if (_frameStoreResidents.isEmpty && frameStoreHasArtwork) {
      _frameStoreResidents.add(
        _TestFrameResident(
          rootSlot: frameStoreSelectedRoot,
          generation: frameStoreGeneration,
          document: document,
          body: List<int>.of(frameStoreBody),
        ),
      );
    }
    final residents = _frameStoreResidents;
    final count = residents.length;
    final response = Uint8List(16 + count * 45);
    final data = ByteData.sublistView(response);
    response[0] = LitchcraftV2Protocol.magic;
    response[1] = LitchcraftV2Protocol.version;
    response[2] = LitchcraftV2Protocol.frameStoreResidentsState;
    data.setUint32(3, session, Endian.little);
    data.setUint32(7, frameStoreGeneration, Endian.little);
    data.setUint16(11, request, Endian.little);
    response[13] = status;
    response[14] = frameStoreHasArtwork ? frameStoreSelectedRoot : 0xff;
    response[15] = count;
    for (
      var residentIndex = 0;
      residentIndex < residents.length;
      residentIndex++
    ) {
      final resident = residents[residentIndex];
      final residentDocument = resident.document;
      final offset = 16 + residentIndex * 45;
      response[offset] = resident.rootSlot;
      data.setUint32(offset + 1, resident.generation, Endian.little);
      data.setUint32(
        offset + 5,
        litchcraftAnimationCrc32(resident.body),
        Endian.little,
      );
      data.setUint32(offset + 9, resident.body.length, Endian.little);
      data.setUint16(
        offset + 13,
        residentDocument.frames.length,
        Endian.little,
      );
      response[offset + 15] = residentDocument.topology.id;
      response[offset + 16] =
          1 | (residentDocument.playbackOnEntry ? 2 : 0) | 4;
      data.setUint32(offset + 17, residentDocument.sparkleSeed, Endian.little);
      for (var index = 0; index < 8; index++) {
        final color = residentDocument.favoriteColors[index];
        response[offset + 21 + index * 3] = color >> 16;
        response[offset + 22 + index * 3] = color >> 8;
        response[offset + 23 + index * 3] = color;
      }
    }
    _emitStoreReceipt(response, request);
  }

  void timeline([int request = 0]) {
    final low = elapsedMilliseconds & 0xffffffff;
    final high = elapsedMilliseconds >> 32;
    advancedEvent(LitchcraftV2Protocol.timelineLow, request, [
      1,
      0,
      playing ? 1 : 0,
      low & 255,
      low >> 8 & 255,
      low >> 16 & 255,
      low >> 24 & 255,
    ]);
    advancedEvent(LitchcraftV2Protocol.timelineHigh, request, [
      1,
      0,
      selectedFrame,
      high & 255,
      high >> 8 & 255,
      high >> 16 & 255,
      high >> 24 & 255,
    ]);
  }

  @override
  Future<void> writeLitchcraftPacket(
    String deviceId,
    List<int> bytes, {
    bool withResponse = true,
  }) async {
    if (bytes.first != 0x4d) {
      // The compact State receipt reports the board's accepted tool, including
      // on schema-3 firmware. Keep this scripted peer at that production seam.
      if (bytes[1] == LitchcraftProtocol.tool) {
        brushErase = bytes[8] != 0;
        brushMask = bytes[9];
        brushColor = LitchcraftProtocol.readRgb(bytes, 10);
      }
      return super.writeLitchcraftPacket(
        deviceId,
        bytes,
        withResponse: withResponse,
      );
    }
    final data = ByteData.sublistView(Uint8List.fromList(bytes));
    final operation = bytes[2], request = data.getUint16(7, Endian.little);
    operations.add(operation);
    switch (operation) {
      case LitchcraftV2Protocol.inspect:
        inspectionPackets.add(Uint8List.fromList(bytes));
        if (bytes.length == 10 && bytes[9] == 1) {
          effectClockInspectRequests.add(request);
        }
        if (bytes.length == 10 && bytes[9] >= 4) {
          frameStoreInspectCount++;
          frameStoreInspectCapabilities.add(bytes[9]);
          await beforeFrameStoreInspect?.call();
          if (frameStoreSupported && bytes[9] == 7) {
            frameStoreState(
              request,
              status: frameStoreCommitAcceptedPending ? 10 : 0,
              includeResidents: !frameStoreCommitAcceptedPending,
            );
          } else {
            frameStoreState(request, status: 4);
          }
          return;
        }
        if (inspectStatus case final status?) {
          advancedState(request, status: status);
          return;
        }
        advancedState(request);
        timeline(request);
      case LitchcraftV2Protocol.timeline:
        if (rejectTimelineBusy) {
          advancedState(request, status: 1);
          return;
        }
        final action = bytes[9], frame = bytes[10];
        timelineFrames.add(frame);
        switch (action) {
          case 0:
            playing = false;
          case 1:
            playing = true;
          case 2:
            playing = false;
            selectedFrame = frame;
        }
        advancedState(request);
        timeline(request);
      case LitchcraftV2Protocol.effect:
        await beforeEffect?.call();
        if (!connected) return;
        if (rejectNextEffectStatus case final status?) {
          rejectNextEffectStatus = null;
          advancedState(request, status: status);
          return;
        }
        if (rejectDocumentMutationBusy) {
          advancedState(request, status: 1);
          return;
        }
        effectCount++;
        editStrokes.add(bytes[14] | bytes[15] << 8);
        editFlags.add(bytes[16]);
        final session = LitchcraftDocumentSession(document)
          ..selectFrame(bytes[9]);
        session.applyEffect(
          square: bytes[17],
          mask: bytes[13],
          effect: LitchcraftEffectStyle.fromId(bytes[10]),
          effectDurationMs: bytes[11] | bytes[12] << 8,
        );
        document = session.document;
        selectedFrame = session.selectedFrameIndex;
        playing = false;
        revision++;
        advancedState(request);
      case LitchcraftV2Protocol.paint:
        await beforePaint?.call();
        if (!connected) return;
        if (rejectNextAdvancedPaintBusy) {
          rejectNextAdvancedPaintBusy = false;
          advancedState(request, status: 1);
          return;
        }
        paintCount++;
        editStrokes.add(bytes[10] | bytes[11] << 8);
        editFlags.add(bytes[12]);
        final session = LitchcraftDocumentSession(document)
          ..selectFrame(bytes[9])
          ..paintSelectedFrame(
            squares: bytes.skip(17),
            mask: bytes[13],
            color: bytes[14] << 16 | bytes[15] << 8 | bytes[16],
          );
        document = session.document;
        selectedFrame = session.selectedFrameIndex;
        playing = false;
        revision++;
        advancedState(request);
      case LitchcraftV2Protocol.frameEdit:
        if (rejectDocumentMutationBusy) {
          advancedState(request, status: 1);
          return;
        }
        final baseRevision = data.getUint32(9, Endian.little);
        final action = bytes[13], source = bytes[14];
        if (baseRevision != revision ||
            (action == 0 && source != selectedFrame)) {
          advancedState(request, status: baseRevision != revision ? 9 : 3);
          return;
        }
        final session = LitchcraftDocumentSession(document)
          ..selectFrame(source);
        switch (action) {
          case 0:
            addFrameCount++;
            session.addFrame();
          case 1:
            deleteFrameCount++;
            session.deleteSelectedFrame();
        }
        document = session.document;
        selectedFrame = session.selectedFrameIndex;
        playing = false;
        revision++;
        advancedState(request);
      case LitchcraftV2Protocol.snapshot:
        snapshotCount++;
        await beforeSnapshot?.call();
        final encoded = encodeLitchcraftAnimationDocument(document);
        advancedState(request);
        final begin = ByteData(7)
          ..setUint8(0, 3)
          ..setUint16(1, encoded.bytes.length, Endian.little)
          ..setUint32(3, encoded.crc32, Endian.little);
        advancedEvent(
          LitchcraftV2Protocol.snapshotBegin,
          request,
          begin.buffer.asUint8List(),
        );
        for (var offset = 0; offset < encoded.bytes.length; offset += 5) {
          advancedEvent(LitchcraftV2Protocol.snapshotChunk, request, [
            offset & 255,
            offset >> 8,
            ...encoded.bytes.skip(offset).take(5),
          ]);
        }
        advancedEvent(LitchcraftV2Protocol.snapshotEnd, request);
      case LitchcraftV2Protocol.canvasEdit:
        advancedState(request, status: 4);
      case LitchcraftV2Protocol.streamBegin:
      case LitchcraftV2Protocol.replaceBeginOp:
        replacementSessions.add(data.getUint32(3, Endian.little));
        if (rejectReplaceBeginBusy) {
          advancedState(request, status: 1);
          return;
        }
        staged.clear();
        stagedCrc = data.getUint32(15, Endian.little);
        advancedState(request);
      case LitchcraftV2Protocol.streamChunk:
        staged.addAll(bytes.skip(13));
      case LitchcraftV2Protocol.replaceChunkOp:
        staged.addAll(bytes.skip(13));
        advancedState(request);
      case LitchcraftV2Protocol.streamCommit:
      case LitchcraftV2Protocol.replaceCommitOp:
        await beforeCommit?.call();
        document = decodeLitchcraftAnimationDocument(staged, stagedCrc!);
        reportedSchema = LitchcraftV2Protocol.schema;
        // Successful normal replacement returns to the editable, paused
        // schema-3 authority; native tests own durable stream retirement.
        frameStorePlaying = false;
        frameStoreHasArtwork = false;
        frameStoreGeneration = 0;
        frameStoreFrameCount = 0;
        _frameStoreResidents.clear();
        playing = false;
        revision++;
        advancedState(request);
      case LitchcraftV2Protocol.frameStoreBegin:
        frameStoreTransfer = request;
        frameStoreBodyBytes = data.getUint32(13, Endian.little);
        frameStoreFrameCount = data.getUint16(21, Endian.little);
        frameStoreTopology = bytes[23];
        frameStoreActive = bytes[24] & 1 != 0;
        frameStorePlaybackOnEntry = bytes[24] & 2 != 0;
        frameStorePredecessorDeltas = bytes[24] & 4 != 0;
        frameStoreTransferRgbWire = bytes[24] & 8 != 0;
        expectSync(
          frameStoreTransferRgbWire && !frameStoreRgbWireCapable,
          isFalse,
        );
        frameStoreSparkleSeed = data.getUint32(25, Endian.little);
        frameStoreFavorites = <int>[
          for (var offset = 29; offset < 53; offset += 3)
            bytes[offset] << 16 | bytes[offset + 1] << 8 | bytes[offset + 2],
        ];
        frameStoreCursor = 0;
        frameStoreStagedBody.clear();
      // A successful Begin is optimistic and silent. Only malformed,
      // conflicting, stale, or busy admission produces an immediate state.
      case LitchcraftV2Protocol.frameStoreChunk:
        await beforeFrameStoreChunk?.call();
        expectSync(data.getUint32(9, Endian.little), frameStoreCursor);
        frameStoreCursor += bytes.length - 13;
        frameStoreStagedBody.addAll(bytes.skip(13));
      case LitchcraftV2Protocol.frameStoreCheckpoint:
        expectSync(data.getUint32(9, Endian.little), frameStoreCursor);
        onFrameStoreCheckpoint?.call();
        if (dropFrameStoreCheckpointReceipt) return;
        advancedEvent(
          LitchcraftV2Protocol.frameStoreCheckpointReceipt,
          request,
          [0, ...bytes.sublist(9, 13)],
        );
      case LitchcraftV2Protocol.frameStoreCommit:
        if (rejectFrameStoreCommit) {
          frameStoreState(request, status: 9);
          return;
        }
        expectSync(request, frameStoreTransfer);
        expectSync(data.getUint32(9, Endian.little), frameStoreBodyBytes);
        expectSync(frameStoreCursor, frameStoreBodyBytes);
        final incomingDocument = LitchcraftAnimationDocument(
          frames: _decodeFrameStoreBody(
            frameStoreStagedBody,
            enableRgbOnly: frameStoreTransferRgbWire,
          ),
          topology: LitchcraftTopology.fromId(frameStoreTopology),
          favoriteColors: frameStoreFavorites,
          playbackOnEntry: frameStorePlaybackOnEntry,
          sparkleSeed: frameStoreSparkleSeed,
        );
        final nextGeneration =
            _frameStoreResidents.fold<int>(
              frameStoreGeneration,
              (value, resident) =>
                  resident.generation > value ? resident.generation : value,
            ) +
            1;
        final occupied = _frameStoreResidents
            .map((resident) => resident.rootSlot)
            .toSet();
        final targetRoot = occupied.length < 2
            ? (occupied.contains(0) ? 1 : 0)
            : frameStoreSelectedRoot ^ 1;
        void publishRoot() {
          frameStoreHasArtwork = true;
          document = incomingDocument;
          frameStoreBody
            ..clear()
            ..addAll(
              frameStoreTransferRgbWire
                  ? _frameStoreBody(incomingDocument)
                  : frameStoreStagedBody,
            );
          reportedSchema = 0;
          frameStoreLiveRevision++;
          storeLiveDirty = false;
          active = frameStoreActive;
          frameStorePlaying = false;
          playing = false;
          selectedFrame = 0;
          _frameStoreResidents.removeWhere(
            (resident) => resident.rootSlot == targetRoot,
          );
          _frameStoreResidents.add(
            _TestFrameResident(
              rootSlot: targetRoot,
              generation: nextGeneration,
              document: document,
              body: List<int>.of(frameStoreBody),
            ),
          );
          frameStoreSelectedRoot = targetRoot;
          frameStoreGeneration = nextGeneration;
          frameStoreCommitAcceptedPending = false;
          publishStoreAuthority();
        }
        if (deferredFrameStoreRoot != null) {
          frameStoreCommitAcceptedPending = true;
          unawaited(() async {
            await deferredFrameStoreRoot!.future;
            await beforeFrameStoreCommit?.call();
            publishRoot();
            if (stalePausedCommitReceipt) frameStorePlaying = false;
            frameStoreState(
              request,
              includeResidents: frameStoreCommitIncludesResidents,
            );
            if (stalePausedCommitReceipt) frameStorePlaying = true;
          }());
        } else {
          publishRoot();
          await beforeFrameStoreCommit?.call();
          frameStoreState(
            request,
            includeResidents: frameStoreCommitIncludesResidents,
          );
        }
      case LitchcraftV2Protocol.frameStoreCancel:
        if (frameStoreCommitAcceptedPending) {
          frameStoreState(request, status: 1);
        } else {
          frameStoreTransfer = 0;
          frameStoreState(request);
        }
      case LitchcraftV2Protocol.frameStoreSnapshot:
        expectSync(data.getUint32(9, Endian.little), frameStoreGeneration);
        if (bytes.length == 23) {
          final selector = data.getUint16(17, Endian.little);
          final layout = selector == 0xfffe;
          if (data.getUint32(13, Endian.little) == 0) {
            if (layout) {
              frameStoreLiveLayoutCount++;
            } else {
              frameStoreLiveFrameCount++;
            }
          }
          if (layout) {
            await beforeFrameStoreSnapshot?.call();
          } else {
            await beforeFrameStorePreview?.call();
          }
          if (rejectNextFrameStoreSnapshotStatus case final status?) {
            rejectNextFrameStoreSnapshotStatus = null;
            frameStoreState(request, status: status);
            return;
          }
          if (data.getUint32(9, Endian.little) != frameStoreGeneration ||
              data.getUint32(19, Endian.little) != frameStoreLiveRevision) {
            frameStoreState(request, status: 9);
            return;
          }
          frameStoreSnapshot(
            request,
            data.getUint32(13, Endian.little),
            frame: layout
                ? null
                : selector == 0xffff
                ? selectedFrame
                : selector,
            live: true,
            layout: layout,
          );
        } else if (bytes.length == 19) {
          if (data.getUint32(13, Endian.little) == 0) frameStorePreviewCount++;
          await beforeFrameStorePreview?.call();
          final selector = data.getUint16(17, Endian.little);
          frameStoreSnapshot(
            request,
            data.getUint32(13, Endian.little),
            frame: selector == 0xffff ? selectedFrame : selector,
          );
        } else {
          if (data.getUint32(13, Endian.little) == 0) {
            frameStoreSnapshotTransactionCount++;
          }
          await beforeFrameStoreSnapshot?.call();
          if (rejectNextFrameStoreSnapshotStatus case final status?) {
            rejectNextFrameStoreSnapshotStatus = null;
            frameStoreState(request, status: status);
            return;
          }
          frameStoreSnapshot(request, data.getUint32(13, Endian.little));
        }
      case LitchcraftV2Protocol.frameStoreResidents:
        await beforeFrameStoreResidents?.call();
        frameStoreResidents(
          request,
          status: frameStoreCommitAcceptedPending ? 10 : 0,
        );
      case LitchcraftV2Protocol.frameStoreSelect:
        await beforeFrameStoreSelect?.call();
        final requested = _frameStoreResidents.where(
          (resident) =>
              resident.rootSlot == bytes[9] &&
              resident.generation == data.getUint32(10, Endian.little) &&
              litchcraftAnimationCrc32(resident.body) ==
                  data.getUint32(14, Endian.little),
        );
        if (!frameStoreHasArtwork || bytes.length != 18 || requested.isEmpty) {
          frameStoreResidents(request, status: 9);
        } else {
          final resident = requested.single;
          frameStoreSelectedRoot = resident.rootSlot;
          frameStoreGeneration = resident.generation;
          document = resident.document;
          frameStoreLiveRevision++;
          storeLiveDirty = false;
          publishStoreAuthority();
          reportedSchema = 0;
          frameStoreBody
            ..clear()
            ..addAll(resident.body);
          frameStoreFrameCount = resident.document.frames.length;
          frameStoreTopology = resident.document.topology.id;
          frameStoreActive = true;
          frameStorePlaying = false;
          frameStoreResidents(request);
        }
      default:
        advancedState(request);
    }
  }
}

Future<T> _drive<T>(Future<T> future, {int maxPumps = 600}) async {
  var completed = false;
  T? result;
  Object? failure;
  future.then(
    (value) {
      result = value;
      completed = true;
    },
    onError: (Object error) {
      failure = error;
      completed = true;
    },
  );
  for (var i = 0; i < maxPumps && !completed; ++i) {
    await Future<void>.delayed(Duration.zero);
  }
  expect(
    completed,
    isTrue,
    reason: 'the scripted transport should settle without wall-clock waits',
  );
  if (failure != null) throw failure!;
  return result as T;
}

Future<void> _idle(LitchcraftController controller) async {
  for (var i = 0; i < 600; ++i) {
    await Future<void>.delayed(Duration.zero);
    if (!controller.pending) return;
  }
  fail('controller did not finish its serialized work');
}

Future<void> _withCurrentStore(
  Future<void> Function(_AdvancedBle peer, LitchcraftController owner) run,
) async {
  final peer = _AdvancedBle()
    ..connectionSessionSupported = true
    ..reportedSchema = 0;
  final owner = LitchcraftController(peer);
  try {
    await run(peer, owner);
  } finally {
    owner.dispose();
    await peer.shutdown();
  }
}

Iterable<Uint8List> _inspectionPacketsInOperationWindow(
  _AdvancedBle peer, {
  int operationStart = 0,
}) {
  final count = peer.operations
      .skip(operationStart)
      .where((operation) => operation == LitchcraftV2Protocol.inspect)
      .length;
  // Existing operation clears and offsets define each assertion's phase.
  return peer.inspectionPackets.skip(peer.inspectionPackets.length - count);
}

int _modeInspectCount(_AdvancedBle peer) => _inspectionPacketsInOperationWindow(
  peer,
).where(isLitchcraftModeDiscovery).length;

void _expectAtMostOneSourceClockInspect(
  _AdvancedBle peer, {
  int operationStart = 0,
  int sampleStart = 0,
}) {
  final inspects = _inspectionPacketsInOperationWindow(
    peer,
    operationStart: operationStart,
  ).toList();
  expect(inspects.where(isLitchcraftModeDiscovery), isEmpty);
  expect(inspects.every(isLitchcraftSourceClockInspection), isTrue);
  expect(inspects, hasLength(lessThanOrEqualTo(1)));
  final samples = peer.effectClockInspectRequests.skip(sampleStart);
  final requests = inspects.map(
    (packet) => ByteData.sublistView(packet).getUint16(7, Endian.little),
  );
  expect(samples, unorderedEquals(requests));
  expect(samples, everyElement(greaterThan(0)));
}

Future<void> _enterUsingBoard(LitchcraftController controller) async {
  await _drive(controller.enter());
  expect(
    controller.status,
    LitchcraftStatus.live,
    reason: 'an unedited app must adopt the board without a choice',
  );
}

Future<void> _until(bool Function() reached) async {
  for (var i = 0; i < 600 && !reached(); ++i) {
    await Future<void>.delayed(Duration.zero);
  }
  expect(reached(), isTrue, reason: 'expected scripted transport checkpoint');
}

Future<void> _untilBackgroundRepair(bool Function() reached) async {
  for (var i = 0; i < 100 && !reached(); ++i) {
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
  expect(reached(), isTrue, reason: 'background Store repair must converge');
}

Future<void> _pumpTransport(WidgetTester tester) async {
  for (var i = 0; i < 20; ++i) {
    await tester.pump(const Duration(milliseconds: 1));
  }
}

Uint8List _frameStoreBody(LitchcraftAnimationDocument document) {
  final body = BytesBuilder(copy: false);
  LitchcraftAnimationFrame? previous;
  for (var index = 0; index < document.frames.length; index++) {
    final frame = document.frames[index];
    final encoded = encodeLitchcraftWireFrame(
      previous: index % LitchcraftFrameStoreProtocol.keyframeInterval == 0
          ? null
          : previous,
      frame: frame,
    );
    final record = Uint8List(6 + encoded.length);
    final data = ByteData.sublistView(record);
    data.setUint16(0, encoded.length, Endian.little);
    data.setUint32(2, litchcraftAnimationCrc32(encoded), Endian.little);
    record.setRange(6, record.length, encoded);
    body.add(record);
    previous = frame;
  }
  return body.takeBytes();
}

List<LitchcraftAnimationFrame> _decodeFrameStoreBody(
  List<int> body, {
  bool enableRgbOnly = false,
}) {
  final frames = <LitchcraftAnimationFrame>[];
  LitchcraftAnimationFrame? previous;
  for (var offset = 0; offset < body.length;) {
    final bytes = Uint8List.fromList(body);
    final length = ByteData.sublistView(bytes).getUint16(offset, Endian.little);
    final encoded = bytes.sublist(offset + 6, offset + 6 + length);
    final frame = decodeLitchcraftWireFrame(
      previous: previous,
      bytes: encoded,
      enableRgbOnly: enableRgbOnly,
    );
    frames.add(frame);
    previous = frame;
    offset += 6 + length;
  }
  return frames;
}

void _usePhoneViewport(WidgetTester tester) {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(390, 844);
  addTearDown(tester.view.reset);
}

void main() {
  for (final localKind in ['static', 'effect', 'multiple frames']) {
    test(
      'switching from a Store board to V1 ESP32-C3 preserves artwork ($localKind)',
      () async {
        await _withCurrentStore((peer, owner) async {
          final canvas = LitchcraftCanvas.blank().paint([0, 63], 15, 0x123456);
          peer.installSavedStoreDocument(staticLitchcraftDocument(canvas));
          await _enterUsingBoard(owner);
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          expect(owner.usesFrameStoreDocument, isTrue);
          expect(owner.documentSession.isAdvanced, isFalse);

          await peer.retireSharedConnection();
          if (localKind == 'effect') {
            owner.documentSession.applyEffect(
              square: 0,
              mask: 15,
              effect: LitchcraftEffectStyle.breathe,
              effectDurationMs: 1000,
            );
          } else if (localKind == 'multiple frames') {
            owner.documentSession.addFrame();
          }
          final local = owner.documentSession.document;
          peer
            ..deviceId = 'v1-static-artwork'
            ..hardwareRevision = LitchHardwareRevision.v1
            ..animationSupported = false
            ..frameStoreSupported = false
            ..artwork = canvas
            ..session = 73
            ..revision = 1;
          peer.requests.clear();
          peer.operations.clear();
          peer.connection(true);
          await _drive(owner.settleModeResolution());
          expect(peer._connectionSession?.isActivated, isTrue);
          expect(
            peer.operations,
            isEmpty,
            reason: 'Static destination never receives V2/Store requests.',
          );
          if (localKind != 'static') {
            expect(owner.status, LitchcraftStatus.unsupported);
            expect(
              sameLitchcraftDocument(owner.documentSession.document, local),
              isTrue,
            );
            expect(
              peer.requests.where(
                (request) => request.opcode == LitchcraftProtocol.paint,
              ),
              isEmpty,
            );
            await _drive(
              owner.replaceDocument(staticLitchcraftDocument(canvas)),
            );
            await _drive(owner.revalidateSelectedBoard());
            expect(owner.status, LitchcraftStatus.live);
            expect(owner.usesFrameStoreDocument, isFalse);
            expect(owner.documentSession.canvas, canvas);
            expect(peer.operations, isEmpty);
            return;
          }
          expect(owner.status, LitchcraftStatus.live);
          expect(owner.usesFrameStoreDocument, isFalse);
          expect(owner.documentSession.canvas, canvas);
          await _drive(owner.revalidateSelectedBoard());
          expect(
            peer.requests.map((request) => request.opcode),
            contains(LitchcraftProtocol.inspect),
          );
          expect(peer.operations, isEmpty);
          peer.onMutation = (request) async {
            if (request.opcode == LitchcraftProtocol.paint) {
              for (final square in request.payload.skip(3)) {
                peer.artwork = peer.artwork.paint(
                  [square],
                  peer.brushMask,
                  peer.brushColor,
                );
                ++peer.revision;
                peer.emit(
                  LitchcraftProtocol.cell,
                  requestId: request.id,
                  payload: [
                    square,
                    peer.brushMask,
                    ...LitchcraftProtocol.rgb(peer.brushColor),
                  ],
                );
              }
            }
            peer.state(request.id);
          };
          owner.beginStroke();
          owner.paintSquares([7]);
          owner.endStroke();
          await _idle(owner);
          expect(owner.status, LitchcraftStatus.live);
          expect(owner.documentSession.canvas, peer.artwork);
          expect(peer.artwork.colors[28], peer.brushColor);
          expect(peer.operations, isEmpty);
        });
      },
    );
  }

  for (final restored in ['same', 'different', 'blank']) {
    test(
      'current active restored $restored artwork preserves the selected phone source after reboot',
      () async {
        await _withCurrentStore((peer, owner) async {
          peer.installSavedStoreDocument(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint([0, 63], 4, 0x123456),
            ),
          );
          await _drive(owner.enter());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          final cached = owner.documentSession.document;
          await peer.retireSharedConnection();
          // This is the user's selected phone source, not an untouched blank app.
          owner.restoreDocument(cached);
          peer.boardBootNonce++;
          peer.session = 73;
          peer.revision = 1;
          final durable = restored == 'same'
              ? cached
              : staticLitchcraftDocument(
                  restored == 'different'
                      ? LitchcraftCanvas.blank().paint([7], 2, 0xabcdef)
                      : LitchcraftCanvas.blank(),
                );
          peer.installSavedStoreDocument(durable, generation: 4);
          peer.requests.clear();
          peer.operations.clear();
          peer.connection(true);
          await _drive(owner.settleModeResolution());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          expect(owner.sessionId, 73);
          expect(owner.boardActive, isTrue);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, cached),
            isTrue,
          );
          expect(
            peer.requests,
            isEmpty,
            reason: 'an active restored board needs no Enter',
          );
          if (restored == 'different') {
            expect(owner.status, LitchcraftStatus.conflict);
            expect(sameLitchcraftDocument(peer.document, durable), isTrue);
            expect(
              peer.operations,
              isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
            );
            await _drive(owner.resolveConflict(useBoard: true));
            expect(
              sameLitchcraftDocument(owner.documentSession.document, durable),
              isTrue,
            );
          } else {
            expect(owner.status, LitchcraftStatus.live);
            expect(sameLitchcraftDocument(peer.document, cached), isTrue);
            expect(
              peer.operations.contains(LitchcraftV2Protocol.frameStoreCommit),
              restored == 'blank',
            );
          }
        });
        if (restored == 'same') {
          for (final replacement in ['New artwork', 'Clear canvas']) {
            await _withCurrentStore((peer, owner) async {
              final durable = staticLitchcraftDocument(
                LitchcraftCanvas.blank().paint([0, 63], 4, 0x123456),
              );
              peer.installSavedStoreDocument(durable);
              await _drive(owner.enter());
              await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
              expect(owner.documentSession.hasCommonDocument, isTrue);
              final boot = peer.boardBootNonce;
              final mode = owner.sessionId;
              await peer.retireSharedConnection();
              if (replacement == 'New artwork') {
                await _drive(
                  owner.replaceDocument(
                    staticLitchcraftDocument(LitchcraftCanvas.blank()),
                    newCanvas: true,
                  ),
                );
              } else {
                await _drive(owner.clear());
                expect(owner.canUndo, isTrue);
                await _drive(owner.undo());
                expect(
                  sameLitchcraftDocument(
                    owner.documentSession.document,
                    durable,
                  ),
                  isTrue,
                );
                expect(owner.canRedo, isTrue);
                await _drive(owner.redo());
              }
              final phone = owner.documentSession.document;
              expect(isEmptyLitchcraftDocument(phone), isTrue);
              peer.requests.clear();
              peer.operations.clear();
              peer.connection(true);
              await _drive(owner.settleModeResolution());
              await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
              expect(peer.boardBootNonce, boot);
              expect(owner.sessionId, mode);
              expect(
                owner.status,
                LitchcraftStatus.conflict,
                reason:
                    '$replacement offline must preserve both sources on warm reconnect',
              );
              expect(sameLitchcraftDocument(peer.document, durable), isTrue);
              expect(
                sameLitchcraftDocument(owner.documentSession.document, phone),
                isTrue,
              );
              expect(
                peer.operations,
                isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
              );
              await _drive(
                owner.resolveConflict(useBoard: replacement == 'New artwork'),
              );
              expect(owner.status, LitchcraftStatus.live);
              expect(
                sameLitchcraftDocument(
                  peer.document,
                  replacement == 'New artwork' ? durable : phone,
                ),
                isTrue,
              );
              expect(
                sameLitchcraftDocument(
                  owner.documentSession.document,
                  peer.document,
                ),
                isTrue,
              );
              if (replacement == 'New artwork') {
                // An ordinary offline field edit still has a proven ancestor.
                await peer.retireSharedConnection();
                owner.documentSession.setSelectedFrameDuration(2400);
                peer.connection(true);
                await _drive(owner.settleModeResolution());
                await _untilBackgroundRepair(
                  () => !owner.boardArtworkUnresolved,
                );
                expect(owner.status, LitchcraftStatus.live);
                expect(peer.document.frames.single.durationMs, 2400);
                expect(
                  sameLitchcraftDocument(
                    owner.documentSession.document,
                    peer.document,
                  ),
                  isTrue,
                );
              }
            });
          }
        }
      },
    );
  }

  test(
    'known resident conflict survives the same mode in a new boot',
    () async {
      await _withCurrentStore((peer, owner) async {
        await _drive(owner.enter());
        final selected = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x12ab34),
        );
        final different = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([7], 15, 0xabcdef),
        );
        await _drive(owner.replaceDocument(selected));
        await _drive(owner.replaceDocument(different));
        await _drive(owner.replaceDocument(selected));
        expect(owner.documentSession.hasLocalIntent, isFalse);
        expect(owner.documentSession.hasCommonDocument, isTrue);
        final source = owner.documentSession.replacementGeneration;
        final mode = owner.sessionId;
        final frame = owner.documentSession.selectedFrameIndex;
        final external = createConnectionLitchcraftFrameStoreClient(
          session: peer._connectionSession!,
          monotonicMilliseconds: () => 0,
        );
        final residents = await _drive(
          external.residents(session: mode, request: 0xfff0),
        );
        final other = residents.residents.singleWhere(
          (resident) => resident.matchesContent(
            LitchcraftFrameStoreProtocol.identify(different),
          ),
        );
        // A separately accepted typed Select changes only the board source.
        // The controller's coherent read recognizes both known residents.
        await _drive(
          external.selectResident(
            session: mode,
            request: 0xfff1,
            resident: other,
          ),
        );
        await _untilBackgroundRepair(
          () => owner.status == LitchcraftStatus.conflict,
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.document, selected),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, different), isTrue);
        final boot = peer.boardBootNonce;
        await peer.retireSharedConnection();
        peer.boardBootNonce++;
        peer.session = mode;
        peer.revision = 1;
        peer.operations.clear();
        peer.connection(true);
        await _drive(owner.settleModeResolution());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(peer.boardBootNonce, isNot(boot));
        expect(owner.sessionId, mode);
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.documentSession.replacementGeneration, source);
        expect(owner.documentSession.selectedFrameIndex, frame);
        expect(owner.documentSession.hasCommonDocument, isFalse);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, selected),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, different), isTrue);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );
      });
    },
  );

  for (final restored in ['equal', 'different', 'blank']) {
    test(
      'same-mode new-boot $restored artwork preserves retained phone lineage',
      () async {
        await _withCurrentStore((peer, owner) async {
          peer.installSavedStoreDocument(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint([0, 63], 4, 0x123456),
            ),
          );
          await _drive(owner.enter());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          final cached = owner.documentSession.document;
          final mode = owner.sessionId;
          final source = owner.documentSession.replacementGeneration;
          final boot = peer.boardBootNonce;
          final reads = peer.frameStoreSnapshotTransactionCount;
          final liveReads = peer.frameStoreLiveLayoutCount;
          await peer.retireSharedConnection();
          peer.boardBootNonce++;
          peer.session = mode;
          peer.revision = 1;
          final durable = restored == 'equal'
              ? cached
              : staticLitchcraftDocument(
                  restored == 'different'
                      ? LitchcraftCanvas.blank().paint([7], 2, 0xabcdef)
                      : LitchcraftCanvas.blank(),
                );
          peer.installSavedStoreDocument(durable, generation: 4);
          peer.requests.clear();
          peer.operations.clear();
          peer.connection(true);
          await _drive(owner.settleModeResolution());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          expect(peer.boardBootNonce, isNot(boot));
          expect(owner.sessionId, mode);
          expect(owner.documentSession.replacementGeneration, source);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, cached),
            isTrue,
          );
          expect(peer.requests, isEmpty);
          if (restored == 'different') {
            expect(owner.status, LitchcraftStatus.conflict);
            expect(sameLitchcraftDocument(peer.document, durable), isTrue);
            expect(
              peer.operations,
              isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
            );
          } else {
            expect(owner.status, LitchcraftStatus.live);
            expect(sameLitchcraftDocument(peer.document, cached), isTrue);
            expect(
              peer.operations.contains(LitchcraftV2Protocol.frameStoreCommit),
              restored == 'blank',
            );
            if (restored == 'equal') {
              expect(peer.frameStoreSnapshotTransactionCount, reads);
              expect(peer.frameStoreLiveLayoutCount, liveReads);
            }
          }
        });
      },
    );
  }

  test(
    'every discrete palette choice retains the immediately previous color',
    () async {
      final ble = _Ble()..connected = false;
      final controller = LitchcraftController(ble);
      await controller.setBrush(color: 0x123456);
      await controller.selectPaletteColor(0x00ff00);
      expect(controller.color, 0x00ff00);
      expect(controller.previousColor, 0x123456);
      await controller.selectPaletteColor(0x0000ff);
      expect(controller.color, 0x0000ff);
      expect(controller.previousColor, 0x00ff00);
      await controller.swapBrushColor();
      expect(controller.color, 0x00ff00);
      expect(controller.previousColor, 0x0000ff);
      controller.dispose();
      await ble.dispose();
    },
  );

  test(
    'continuous color preview retains its pre-gesture color exactly once',
    () async {
      final ble = _Ble()..connected = false;
      final controller = LitchcraftController(ble);
      await controller.setBrush(color: 0x123456);

      controller.beginBrushColorPreview();
      await controller.previewBrushColor(0x234567);
      await controller.previewBrushColor(0x345678);
      expect(controller.color, 0x345678);
      expect(controller.previousColor, 0xff7430);

      await controller.commitBrushColorPreview(0x456789);
      expect(controller.color, 0x456789);
      expect(controller.previousColor, 0x123456);

      controller.dispose();
      await ble.dispose();
    },
  );

  test(
    'continuous color preview stays optimistic and coalesces blocked board writes',
    () async {
      final ble = _Ble();
      final controller = LitchcraftController(ble);
      await _drive(controller.enter());
      ble.requests.clear();
      final firstWrite = Completer<void>();
      var toolWrites = 0;
      ble.onMutation = (request) async {
        if (request.opcode != LitchcraftProtocol.tool) {
          ble.state(request.id);
          return;
        }
        ++toolWrites;
        if (toolWrites == 1) await firstWrite.future;
        ble.brushColor = LitchcraftProtocol.readRgb(request.payload, 2);
        ble.state(request.id);
      };

      controller.beginBrushColorPreview();
      for (var sample = 1; sample <= 20; ++sample) {
        unawaited(controller.previewBrushColor(0x110000 + sample));
      }
      await _until(() => toolWrites == 1);
      expect(controller.color, 0x110014);
      expect(toolWrites, 1, reason: 'ACK latency must not queue every sample');

      unawaited(controller.commitBrushColorPreview(0x223344));
      expect(controller.color, 0x223344);
      firstWrite.complete();
      await _idle(controller);
      expect(toolWrites, 2, reason: 'only the newest replacement is delivered');
      expect(ble.brushColor, 0x223344);

      controller.dispose();
      await ble.shutdown();
    },
  );
  for (final retirement in ['disconnect', 'leave']) {
    test('held color preview releases ownership after $retirement', () async {
      final ble = _Ble();
      final controller = LitchcraftController(ble);
      await _drive(controller.enter());
      await _drive(controller.previewBrushColor(0x123456));
      for (var turn = 0; turn < 5; turn++) {
        await Future<void>.delayed(Duration.zero);
      }
      if (retirement == 'disconnect') {
        ble.connection(false);
        await _idle(controller);
        ble.connection(true);
      } else {
        await _drive(controller.leave());
      }
      await _drive(controller.enter());
      ++ble.revision;
      ble.emit(
        LitchcraftProtocol.brush,
        payload: [...LitchcraftProtocol.rgb(0xabcdef), 0, 15],
      );
      expect(
        controller.color,
        0xabcdef,
        reason: 'A retired finger gesture cannot hide the next board brush',
      );
      controller.dispose();
      await ble.shutdown();
    });
  }

  for (final preview in [true, false]) {
    test(
      '${preview ? "final hue" : "direct color"} commit captures Tool before queued physical brush State',
      () async {
        final ble = _Ble();
        final controller = LitchcraftController(ble);
        await _drive(controller.enter());
        ble.onMutation = (request) async {
          if (request.opcode == LitchcraftProtocol.tool) {
            ble.brushColor = LitchcraftProtocol.readRgb(request.payload, 2);
            ble.brushErase = request.payload[0] != 0;
            ble.brushMask = request.payload[1];
          }
          ble.state(request.id);
        };
        await _drive(
          preview
              ? controller.previewBrushColor(0x123456)
              : controller.setBrush(color: 0x123456),
        );
        for (var turn = 0; turn < 5; turn++) {
          await Future<void>.delayed(Duration.zero);
        }
        var observed = false;
        void queuePhysicalState() {
          if (observed) return;
          observed = true;
          // A real passive brush packet at the async _setBrush yield. No receipt
          // is manufactured; the scripted SDK returns the normal Tool receipt.
          scheduleMicrotask(() {
            ++ble.revision;
            ble.emit(
              LitchcraftProtocol.brush,
              payload: [...LitchcraftProtocol.rgb(0xabcdef), 0, 15],
            );
          });
        }

        controller.addListener(queuePhysicalState);
        await _drive(controller.commitBrushColorPreview(0x225588));
        controller.removeListener(queuePhysicalState);
        await _until(() => ble.brushColor == 0x225588);
        expect(
          LitchcraftProtocol.readRgb(ble.requests.last.payload, 2),
          0x225588,
        );
        expect(controller.color, 0x225588);
        controller.dispose();
        await ble.shutdown();
      },
    );
  }
  TestWidgetsFlutterBinding.ensureInitialized();
  group('whole-document controller', () {
    late _AdvancedBle peer;
    late LitchcraftController owner;
    setUp(() {
      peer = _AdvancedBle();
      owner = LitchcraftController(peer);
    });
    tearDown(() async {
      owner.dispose();
      await peer.shutdown();
    });

    for (final frameCount in <int>[1, 2]) {
      testWidgets(
        'effected Store color-pick result survives lift and newer State frames=$frameCount',
        (tester) async {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          owner.dispose();
          expect(
            LitchcraftController.physicalColorPickedDisplayDuration,
            const Duration(seconds: 3),
          );
          final source =
              LitchcraftDocumentSession(
                staticLitchcraftDocument(
                  LitchcraftCanvas.blank().paint(<int>[9], 15, 0x194b7d),
                ),
              )..applyEffect(
                square: 9,
                mask: 15,
                effect: LitchcraftEffectStyle.flash,
                effectDurationMs: 1000,
              );
          if (frameCount == 2) source.addFrame();
          peer.document = source.document;
          peer.connectionSessionSupported = true;
          final widgetClock = Zone.current;
          // Protocol setup needs real async work. Its packet callbacks retain
          // that zone, so return the notice timer to the widget clock only.
          final noticeClock = ZoneSpecification(
            createTimer: (self, parent, zone, duration, callback) =>
                duration ==
                    LitchcraftController.physicalColorPickedDisplayDuration
                ? widgetClock.createTimer(duration, callback)
                : parent.createTimer(zone, duration, callback),
          );
          await tester.runAsync(
            () => runZoned(() async {
              owner = LitchcraftController(peer);
              await _enterUsingBoard(owner);
            }, zoneSpecification: noticeClock),
          );
          expect(owner.usesFrameStoreDocument, isTrue);
          expect(owner.documentSession.isAdvanced, isTrue);
          expect(owner.documentSession.document.frames, hasLength(frameCount));
          final library = LitchcraftLibrary(owner);
          await tester.runAsync(() => library.ready);
          await tester.pumpWidget(
            MaterialApp(
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              home: Scaffold(
                body: LitchcraftScreen(controller: owner, library: library),
              ),
            ),
          );
          const pickedNotice =
              'Color picked. Move the piece to paint somewhere else.';
          peer.emit(
            LitchcraftProtocol.colorPick,
            sessionId: peer.session + 1,
            payload: const <int>[LitchcraftProtocol.colorPickPicked],
          );
          await tester.pump();
          expect(owner.physicalColorPicked, isFalse);

          // Exact post-Object domain sequence: retained current tool State can
          // overtake picked/lift events at an older artwork revision. The
          // native Hall journey owns physical behavior; this peer supplies bytes.
          const captured = 0x0c253e;
          peer.brushColor = captured;
          peer.brushMask = 2;
          peer.connectionLitchcraftState(
            sequence: 100,
            selectedFrame: 0,
            frameElapsedMilliseconds: 0,
            playing: false,
          );
          peer.emit(
            LitchcraftProtocol.brush,
            payload: <int>[...LitchcraftProtocol.rgb(captured), 0, 2],
          );
          peer.state(0, atRevision: peer.revision + 1);
          peer.colorPick(LitchcraftProtocol.colorPickPicked);
          peer.emit(
            LitchcraftProtocol.colorPick,
            atRevision: peer.revision + 1,
            payload: const <int>[LitchcraftProtocol.colorPickIdle],
          );
          await tester.pump();
          expect(owner.color, captured);
          expect(owner.targetMask, 2);
          expect(owner.physicalColorPicking, isFalse);
          expect(owner.physicalColorPicked, isTrue);
          expect(find.text(pickedNotice), findsOneWidget);
          await tester.pump(const Duration(seconds: 1));
          expect(find.text(pickedNotice), findsOneWidget);
          await tester.pump(const Duration(milliseconds: 1999));
          expect(find.text(pickedNotice), findsOneWidget);
          await tester.pump(const Duration(milliseconds: 2));
          expect(owner.physicalColorPicked, isFalse);
          expect(find.text(pickedNotice), findsNothing);

          final retiredSession = peer.session;
          await tester.runAsync(() => _drive(owner.leave()));
          peer.colorPick(LitchcraftProtocol.colorPickPicked);
          await tester.pump();
          expect(owner.physicalColorPicked, isFalse);
          peer.session++;
          await tester.runAsync(
            () => runZoned(
              () => _enterUsingBoard(owner),
              zoneSpecification: noticeClock,
            ),
          );
          peer.emit(
            LitchcraftProtocol.colorPick,
            sessionId: retiredSession,
            payload: const <int>[LitchcraftProtocol.colorPickPicked],
          );
          await tester.pump();
          expect(owner.physicalColorPicked, isFalse);
          peer.colorPick(LitchcraftProtocol.colorPickPicked);
          peer.colorPick(LitchcraftProtocol.colorPickIdle);
          await tester.pump();
          expect(owner.physicalColorPicked, isTrue);
          peer.connection(false);
          await tester.pump();
          peer.colorPick(LitchcraftProtocol.colorPickPicked);
          await tester.pump();
          expect(owner.physicalColorPicked, isFalse);
          expect(find.text(pickedNotice), findsNothing);
          await tester.pumpWidget(const SizedBox());
          library.dispose();
        },
      );
    }

    for (final smallPackets in [false, true]) {
      testWidgets(
        'UI108 chooser Use app has visible body and root progress minimum=$smallPackets',
        (tester) async {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          tester.view.physicalSize = const Size(390, 844);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          peer.connectionSessionSupported = true;
          peer.forceMinimumConnectionPacket = smallPackets;
          peer.fragmentStoreReceipts = smallPackets;
          peer.dropFrameStoreCheckpointReceipt = smallPackets;
          peer.frameStoreRgbWireCapable = true;
          peer.reportedSchema = 0;
          final library = LitchcraftLibrary(owner);
          await tester.runAsync(() => library.ready);
          final incoming = LitchcraftAnimationDocument(
            frames: smallPackets
                ? LitchcraftBouncingBallsPreset.decode().frames
                      .take(32)
                      .toList()
                : LitchcraftBouncingBallsPreset.decode().frames,
            topology: owner.canvas.topology,
            favoriteColors: peer.document.favoriteColors,
            playbackOnEntry: true,
          );
          owner.restoreDocument(incoming);
          await tester.runAsync(() => _drive(owner.enter()));
          expect(owner.status, LitchcraftStatus.conflict);
          final oldBoard = peer.document;
          final rootGate = Completer<void>();
          peer.deferredFrameStoreRoot = rootGate;
          await tester.pumpWidget(
            MaterialApp(
              localizationsDelegates: AppLocalizations.localizationsDelegates,
              supportedLocales: AppLocalizations.supportedLocales,
              home: Scaffold(
                body: LitchcraftScreen(controller: owner, library: library),
              ),
            ),
          );
          await tester.pump();
          await tester.tap(
            find.byKey(const ValueKey('craft-resolve-conflict')),
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 300));
          await _pumpTransport(tester);
          final app = find.byKey(const ValueKey('craft-use-app-artwork'));
          await tester.ensureVisible(app);
          await tester.tap(app);
          await tester.pump();
          for (
            var i = 0;
            i < 2000 &&
                !(smallPackets
                    ? peer.operations.contains(
                        LitchcraftV2Protocol.frameStoreCheckpoint,
                      )
                    : peer.frameStoreCommitAcceptedPending);
            i++
          ) {
            await tester.pump(const Duration(milliseconds: 1));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
          expect(
            smallPackets
                ? peer.operations.contains(
                    LitchcraftV2Protocol.frameStoreCheckpoint,
                  )
                : peer.frameStoreCommitAcceptedPending,
            isTrue,
          );
          await tester.pump(const Duration(milliseconds: 700));
          final progress = find.byKey(
            const ValueKey('craft-full-animation-sync'),
          );
          debugPrint(
            'UI108_SYNC phase=${smallPackets ? 'body-checkpoint-hold' : 'publication-hold'} t_ms=700 frames=${incoming.frames.length} '
            'status=${owner.status.name} fullDirection=${owner.fullAnimationSyncDirection} '
            'fullProgress=${owner.fullAnimationSyncProgress} rootPending=${owner.storeRootPublicationPending} '
            'bars=${progress.evaluate().length} chooseArtwork=${find.text("Choose artwork").evaluate().length} '
            'boardUnchanged=${sameLitchcraftDocument(oldBoard, peer.document)} connected=${peer.isConnected}',
          );
          final wasVisible = progress.evaluate().isNotEmpty;
          if (smallPackets) {
            expect(owner.fullAnimationSyncProgress, inExclusiveRange(0, .99));
            peer.dropFrameStoreCheckpointReceipt = false;
            final checkpoint = ByteData(5)
              ..setUint32(1, peer.frameStoreCursor, Endian.little);
            peer.advancedEvent(
              LitchcraftV2Protocol.frameStoreCheckpointReceipt,
              peer.frameStoreTransfer,
              checkpoint.buffer.asUint8List(),
            );
            for (
              var i = 0;
              i < 2000 && !peer.frameStoreCommitAcceptedPending;
              i++
            ) {
              await tester.pump(const Duration(milliseconds: 1));
              await tester.runAsync(() => Future<void>.delayed(Duration.zero));
            }
            expect(peer.frameStoreCommitAcceptedPending, isTrue);
          }
          await tester.pump(const Duration(milliseconds: 700));
          final rootWasVisible = progress.evaluate().isNotEmpty;
          final shownProgress = rootWasVisible
              ? tester.widget<LinearProgressIndicator>(progress).value
              : null;
          debugPrint(
            'UI108_SYNC phase=root-hold status=${owner.status.name} direction=${owner.fullAnimationSyncDirection} '
            'progress=${owner.fullAnimationSyncProgress} rootPending=${owner.storeRootPublicationPending} '
            'bars=${progress.evaluate().length} shownProgress=$shownProgress boardUnchanged=${sameLitchcraftDocument(oldBoard, peer.document)}',
          );
          expect(owner.fullAnimationSyncProgress, .99);
          expect(sameLitchcraftDocument(oldBoard, peer.document), isTrue);
          rootGate.complete();
          for (
            var i = 0;
            i < 1000 && owner.status != LitchcraftStatus.live;
            i++
          ) {
            await tester.pump(const Duration(milliseconds: 1));
            await tester.runAsync(() => Future<void>.delayed(Duration.zero));
          }
          await tester.pump();
          debugPrint(
            'UI108_SYNC phase=settled status=${owner.status.name} frames=${peer.document.frames.length} '
            'matched=${sameLitchcraftDocument(peer.document, incoming)} direction=${owner.fullAnimationSyncDirection} error=${owner.errorMessage}',
          );
          expect(owner.status, LitchcraftStatus.live);
          expect(sameLitchcraftDocument(peer.document, incoming), isTrue);
          expect(owner.frameStoreArtworkReadPending, isFalse);
          expect(owner.errorMessage, isNull);
          expect(
            find.byKey(const ValueKey('craft-resolve-conflict')),
            findsNothing,
          );
          expect(find.text('Choose artwork'), findsNothing);
          await tester.pumpWidget(const SizedBox());
          await tester.runAsync(() => library.flush());
          library.dispose();
          expect(
            rootWasVisible,
            isTrue,
            reason: 'Keep sending visible until durable root publishes.',
          );
          expect(shownProgress, .99);
          expect(
            wasVisible,
            isTrue,
            reason:
                'After Use app starts a full Bouncing Balls transfer lasting over500ms, show determinate sending progress in the actual canvas.',
          );
        },
      );
    }

    testWidgets(
      'UI108 stopped filmstrip preserves held browsing and minimally reveals taps',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        await tester.runAsync(() async {
          await _drive(owner.enter());
          await _drive(
            owner.replaceDocument(
              LitchcraftAnimationDocument(
                frames: List.generate(
                  32,
                  (index) => LitchcraftAnimationFrame(
                    emitters: peer.document.frames.first.emitters,
                    durationMs: 50 + index,
                  ),
                ),
                topology: peer.document.topology,
                favoriteColors: peer.document.favoriteColors,
              ),
            ),
          );
          await _drive(owner.selectAnimationFrame(4));
        });
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.animationPlaying, isFalse);
        final library = LitchcraftLibrary(owner);
        await tester.runAsync(() => library.ready);
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.pump();
        final animate = find.byKey(const ValueKey('craft-animate'));
        await tester.ensureVisible(animate);
        await tester.tap(animate);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        final viewport = find.byKey(
          const ValueKey('craft-frame-scroll-viewport'),
        );
        ScrollPosition position() => tester
            .state<ScrollableState>(
              find.descendant(of: viewport, matching: find.byType(Scrollable)),
            )
            .position;
        final initial = position().pixels;
        final start = tester.getCenter(viewport);
        final drag = await tester.startGesture(start);
        await drag.moveTo(start + const Offset(-40, 0));
        await tester.pump(const Duration(milliseconds: 50));
        await drag.moveTo(start + const Offset(-85, 0));
        await tester.pump(const Duration(milliseconds: 50));
        final held = position().pixels;
        expect(held, greaterThan(initial + 30));
        expect(
          owner.animationSelectedFrameIndex,
          4,
          reason: 'Browsing does not select or send a frame.',
        );
        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: 1,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await _pumpTransport(tester);
        expect(owner.animationSelectedFrameIndex, 4);
        final afterAuthority = position().pixels;
        await drag.moveTo(start + const Offset(-140, 0));
        await tester.pump(const Duration(milliseconds: 50));
        final afterNextPoint = position().pixels;
        await tester.pump(const Duration(milliseconds: 200));
        await drag.up();
        await tester.pump(const Duration(milliseconds: 200));
        final beforeTap = position().pixels;
        final visible = [
          for (var index = 0; index < 32; index++)
            if (index != owner.animationSelectedFrameIndex &&
                find
                    .byKey(ValueKey('craft-select-frame-$index'))
                    .hitTestable()
                    .evaluate()
                    .isNotEmpty)
              index,
        ];
        expect(visible, isNotEmpty);
        final tappedFrame = visible.last;
        final tappedThumbnail = find.byKey(
          ValueKey('craft-select-frame-$tappedFrame'),
        );
        final viewportBounds = tester.getRect(viewport);
        final beforeTapBounds = tester.getRect(tappedThumbnail);
        final clippedWidth =
            beforeTapBounds.width -
            beforeTapBounds.intersect(viewportBounds).width;
        expect(clippedWidth, greaterThan(0));
        final receipt = Completer<void>();
        peer.beforeOrderedTimeline = () => receipt.future;
        await tester.tap(tappedThumbnail);
        await tester.pump();
        final afterTap = position().pixels;
        final afterTapBounds = tester.getRect(tappedThumbnail);
        expect(owner.animationSelectedFrameIndex, tappedFrame);
        expect(owner.animationPlaying, isFalse);
        receipt.complete();
        await _pumpTransport(tester);
        for (var i = 0; i < 100 && peer.selectedFrame != tappedFrame; i++) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
        await _pumpTransport(tester);
        final afterReceipt = position().pixels;
        expect(peer.selectedFrame, tappedFrame);
        final fullyContained = List.generate(32, (index) => index).where((
          index,
        ) {
          final candidate = find.byKey(ValueKey('craft-select-frame-$index'));
          if (index == tappedFrame ||
              candidate.hitTestable().evaluate().isEmpty) {
            return false;
          }
          final bounds = tester.getRect(candidate);
          return bounds.left >= viewportBounds.left &&
              bounds.right <= viewportBounds.right;
        }).toList();
        expect(fullyContained, isNotEmpty);
        final companionFrame = fullyContained.first;
        final companion = find.byKey(
          ValueKey('craft-select-frame-$companionFrame'),
        );
        final companionBounds = tester.getRect(companion);
        expect(companionBounds.left, greaterThanOrEqualTo(viewportBounds.left));
        expect(companionBounds.right, lessThanOrEqualTo(viewportBounds.right));
        final companionReceipt = Completer<void>();
        peer.beforeOrderedTimeline = () => companionReceipt.future;
        await tester.tap(companion);
        await tester.pump();
        final afterVisibleTap = position().pixels;
        expect(owner.animationSelectedFrameIndex, companionFrame);
        expect(companionFrame, isNot(tappedFrame));
        companionReceipt.complete();
        await _pumpTransport(tester);
        for (var i = 0; i < 100 && peer.selectedFrame != companionFrame; i++) {
          await tester.pump(const Duration(milliseconds: 10));
          await tester.runAsync(() => Future<void>.delayed(Duration.zero));
        }
        await _pumpTransport(tester);
        final afterVisibleReceipt = position().pixels;
        debugPrint(
          'UI108_STRIP initial=$initial held=$held afterAuthority=$afterAuthority '
          'afterNextPoint=$afterNextPoint beforeTap=$beforeTap afterTap=$afterTap '
          'afterReceipt=$afterReceipt tapped=$tappedFrame companion=$companionFrame boardFrame=${peer.selectedFrame}',
        );
        expect(peer.selectedFrame, companionFrame);
        await tester.pumpWidget(const SizedBox());
        await tester.runAsync(() => library.flush());
        library.dispose();
        expect(
          afterAuthority,
          closeTo(held, .01),
          reason:
              'A stopped board frame update cannot move a finger-owned strip.',
        );
        expect(
          afterNextPoint,
          closeTo(held + 55, .01),
          reason:
              'The next held drag point continues from the finger-owned offset.',
        );
        expect(
          afterTapBounds.left,
          greaterThanOrEqualTo(viewportBounds.left - .5),
        );
        expect(
          afterTapBounds.right,
          lessThanOrEqualTo(viewportBounds.right + .5),
        );
        expect(
          (afterTap - beforeTap).abs(),
          closeTo(clippedWidth, .5),
          reason: 'A clipped thumbnail tap scrolls only enough to reveal it.',
        );
        expect(
          afterReceipt,
          closeTo(afterTap, .01),
          reason:
              'The asynchronous board receipt cannot move the reveal further.',
        );
        expect(
          afterVisibleTap,
          closeTo(afterReceipt, .01),
          reason:
              'Selecting a different fully visible frame preserves browsing.',
        );
        expect(
          afterVisibleReceipt,
          closeTo(afterVisibleTap, .01),
          reason: 'Its asynchronous receipt also preserves the viewport.',
        );
      },
    );

    testWidgets(
      'refused shared-session entry never starts retired inactive artwork delivery',
      (tester) async {
        peer.connectionSessionSupported = true;
        peer.active = false;
        peer.enterStatus = 8;
        final trace = _EntryProbeTrace();
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        await tester.runAsync(() => _drive(owner.enter()));
        expect(owner.status, LitchcraftStatus.boardNotEmpty);
        // A restored/edited phone draft is observed in the widget fake zone,
        // so its trailing delivery deadline is advanced by tester.pump.
        owner.documentSession.addFrame();
        peer.inspectStatus = 4;
        expect(owner.boardActive, isFalse);
        expect(peer.operations, isEmpty);

        // Actual phone ordering: refused Enter, then the background delivery
        // deadline. Current Store-only firmware refuses retired v2 storage.
        await tester.pump(const Duration(milliseconds: 601));
        await _pumpTransport(tester);
        expect(
          peer.operations,
          isEmpty,
          reason: 'Inactive modern storage must use the Store owner.',
        );
        expect(owner.status, LitchcraftStatus.boardNotEmpty);
        expect(owner.canSaveArtworkToBoard, isFalse);
        expect(trace.failures, isEmpty);
        expect(trace.recoveries, isEmpty);
        expect(peer.isConnected, isTrue);
      },
    );

    test(
      'Tool readback preserves an intervening physical brush selection',
      () async {
        peer.connectionSessionSupported = true;
        await _enterUsingBoard(owner);
        final previousColor = owner.previousColor;
        for (final color in [0xe71800, 0xcf3000, 0x445566]) {
          peer.brushColor = color;
          peer.connectionLitchcraftState(
            sequence: peer._connectionLitchcraftStateSequence + 1,
            selectedFrame: 0,
            frameElapsedMilliseconds: 0,
            playing: false,
          );
          expect(owner.color, color);
          expect(owner.previousColor, previousColor);
        }
        peer.beforeOrderedToolCheckpoint = () async {
          // The board can change back to the old RGB after applying App Tool.
          // Equal values do not identify the ordering of two accepted selections.
          peer.brushColor = 0x445566;
          peer.connectionLitchcraftState(
            sequence: peer._connectionLitchcraftStateSequence + 1,
            selectedFrame: 0,
            frameElapsedMilliseconds: 0,
            playing: false,
          );
        };
        await _drive(owner.selectPaletteColor(0x339966));
        expect(peer.brushColor, 0x445566);
        expect(
          owner.color,
          0x445566,
          reason:
              'Current board truth wins after ambiguous pre-ACK observation',
        );
        expect(owner.status, LitchcraftStatus.live);
        await _drive(owner.leave());
        final detachedColor = owner.color;
        peer.brushColor = 0xaabbcc;
        peer.connectionLitchcraftState(
          sequence: peer._connectionLitchcraftStateSequence + 1,
          selectedFrame: 0,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await _idle(owner);
        expect(owner.boardActive, isFalse);
        expect(owner.color, detachedColor);
      },
    );

    test(
      'foreground notices a missed advanced physical Exit without admission',
      () async {
        await _drive(owner.enter());
        peer.requests.clear();
        final snapshots = peer.snapshotCount;
        peer.active = false;
        owner.didChangeAppLifecycleState(AppLifecycleState.resumed);
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.selected, isFalse);
        expect(owner.boardActive, isFalse);
        expect(peer.requests, isEmpty);
        expect(peer.snapshotCount, snapshots);
      },
    );

    test(
      'explicit retry re-enters when cached active mode was lost on the link',
      () async {
        peer.inspectSupported = true;
        await _drive(owner.enter());
        peer.requests.clear();

        // The BLE link remains connected, but the board has left Litchcraft
        // without delivering the State notification. The controller still
        // has its old active cache, which is the production retry seam.
        peer.active = false;
        ++peer.revision;

        await _drive(owner.retryConnection());

        expect(
          peer.requests.map((request) => request.opcode),
          contains(LitchcraftProtocol.enter),
          reason: 'an explicit retry must restore board mode before commands',
        );
        expect(owner.boardActive, isTrue);
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'reconnect equality needs Inspect only and never static admission',
      () async {
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.live);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
        expect(peer.snapshotCount, 1);
        peer.requests.clear();
        peer.connection(false);
        peer.connection(true);
        await Future<void>.delayed(Duration.zero);
        await _drive(owner.settleModeResolution());
        expect(peer.snapshotCount, 1);
        expect(peer.requests, isEmpty);
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'physical Exit during the first Store authority read retires passive admission',
      () async {
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = peer.document.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStoreBody.addAll(_frameStoreBody(peer.document));
        peer.beforeFrameStoreInspect = () async {
          peer.dropNextStoreAuthorityQuery = true;
        };
        final draft = owner.documentSession.document;
        final observedModes = <bool>[];
        owner.addListener(() => observedModes.add(owner.boardActive));
        final adopting = owner.adoptActiveBoard();
        try {
          await _until(
            () =>
                owner.inspectedBoardActive == true &&
                owner.boardArtworkUnresolved &&
                peer.frameStoreInspectCount != 0 &&
                !peer.dropNextStoreAuthorityQuery,
          );
          final operations = peer.operations.length;
          peer.active = false;
          peer.revision++;
          peer.advancedState(0);

          expect(owner.inspectedBoardActive, isFalse);
          expect(owner.boardActive, isFalse);
          expect(owner.selected, isFalse);
          expect(owner.boardArtworkUnresolved, isFalse);
          expect(observedModes, containsAllInOrder(<bool>[true, false]));

          peer.frameStoreActive = false;
          peer.publishStoreAuthority();
          await _drive(adopting);
          expect(owner.status, LitchcraftStatus.offline);
          expect(owner.inspectedBoardActive, isFalse);
          expect(peer.operations, hasLength(operations));
          expect(
            sameLitchcraftDocument(owner.documentSession.document, draft),
            isTrue,
          );
        } finally {
          peer.publishStoreAuthority();
          await _drive(adopting);
        }
      },
    );

    test(
      'physical Exit before clean Store authority does not depend on artwork schema',
      () async {
        final release = Completer<void>();
        addTearDown(() {
          if (!release.isCompleted) release.complete();
        });
        peer.beforeFrameStoreSnapshot = () => release.future;
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = peer.document.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.storeLiveDirty = true;
        peer.frameStoreBody.addAll(_frameStoreBody(peer.document));

        await _drive(owner.adoptActiveBoard());
        expect(owner.boardActive, isTrue);
        expect(owner.boardArtworkUnresolved, isTrue);
        expect(owner.documentSession.isAdvanced, isFalse);
        expect(
          owner.usesFrameStoreDocument,
          isTrue,
          reason: 'Store metadata admits the existing dirty live read owner',
        );
        final draft = owner.documentSession.document;
        final operations = peer.operations.length;
        final observedModes = <bool>[];
        owner.addListener(() => observedModes.add(owner.boardActive));

        peer.active = false;
        peer.revision++;
        peer.advancedState(0);

        expect(owner.boardActive, isFalse);
        expect(owner.selected, isFalse);
        expect(owner.boardArtworkUnresolved, isFalse);
        expect(observedModes, contains(false));

        peer.frameStoreActive = false;
        peer.storeLiveDirty = false;
        release.complete();
        peer.publishStoreAuthority();
        await _idle(owner);
        expect(owner.inspectedBoardActive, isFalse);
        expect(owner.status, LitchcraftStatus.offline);
        expect(peer.operations, hasLength(operations));
        expect(
          sameLitchcraftDocument(owner.documentSession.document, draft),
          isTrue,
        );
      },
    );

    test(
      'fresh phone downloads active Store authority without legacy Snapshot',
      () async {
        peer.connectionSessionSupported = true;
        peer.paletteSyncSupported = true;
        peer.reportedSchema = 0;
        peer.document = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x123456),
        );
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = peer.document.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStorePlaying = true;
        peer.frameStoreBody.addAll(_frameStoreBody(peer.document));
        peer.onMutation = (request) async {
          peer.state(
            request.id,
            status: request.opcode == LitchcraftProtocol.paletteChunk ? 1 : 0,
          );
        };

        await _drive(owner.enter());
        await _idle(owner);

        expect(owner.status, LitchcraftStatus.live);
        expect(owner.usesFrameStoreDocument, isTrue);
        expect(owner.animationPlaying, isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
        expect(peer.snapshotCount, 0);
        expect(
          peer.operations,
          contains(LitchcraftV2Protocol.frameStoreSnapshot),
        );
        expect(
          peer.requests.where(
            (request) => request.opcode == LitchcraftProtocol.paletteChunk,
          ),
          isEmpty,
          reason:
              'Store metadata owns favorite colors; compact palette writes '
              'are a conflicting legacy authority',
        );

        peer.operations.clear();
        final coherent = await _drive(owner.coherentDocument());
        expect(sameLitchcraftDocument(coherent, peer.document), isTrue);
        expect(
          peer.operations,
          isEmpty,
          reason:
              'a reconstructed Store document is already coherent and must '
              'not open the retired schema-3 snapshot path',
        );

        peer.beforeFrameStoreCommit = () async {
          peer.emit(
            LitchcraftProtocol.cell,
            sessionId: owner.sessionId,
            atRevision: peer.revision + 2,
            payload: const <int>[0, 15, 1, 2, 3],
          );
        };
        await _drive(
          owner.transferFrameStoreForDiagnostics(
            frames: coherent.frames,
            playbackOnEntry: false,
          ),
        );
        expect(owner.status, LitchcraftStatus.live);
        expect(
          peer.requests.where(
            (request) => request.opcode == LitchcraftProtocol.snapshot,
          ),
          isEmpty,
          reason:
              'compact revision gaps cannot schedule a legacy Snapshot after '
              'a Store commit',
        );
      },
    );

    for (final reportedSchema in <int>[0, 3]) {
      test(
        'active shared session schema $reportedSchema with no Store root rejects old artwork fallback',
        () async {
          peer.connectionSessionSupported = true;
          peer.simulateMissingStoreRoot = true;
          peer.reportedSchema = reportedSchema;
          peer.frameStoreHasArtwork = false;

          await _drive(owner.enter());

          expect(owner.status, LitchcraftStatus.error);
          expect(owner.usesFrameStoreDocument, isTrue);
          expect(owner.errorMessage, contains('Board artwork is unavailable'));
          expect(peer.frameStoreInspectCount, greaterThan(0));
          expect(peer.frameStoreInspectCapabilities, everyElement(7));
          expect(peer.snapshotCount, 0);
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.snapshot)),
          );
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.replaceBeginOp)),
          );
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.replaceCommitOp)),
          );
        },
      );
    }

    testWidgets(
      'missing Store artwork stays visible and offers an accessible retry',
      (tester) async {
        _usePhoneViewport(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        peer.connectionSessionSupported = true;
        peer.simulateMissingStoreRoot = true;
        peer.reportedSchema = 0;
        peer.frameStoreHasArtwork = false;
        final library = LitchcraftLibrary(owner);
        await library.ready;
        await tester.runAsync(() => _drive(owner.enter()));
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(
          find.textContaining('Could not read artwork from your Litch'),
          findsOneWidget,
        );
        final card = tester.widget<Semantics>(
          find.byKey(const ValueKey('craft-status-card')),
        );
        expect(card.properties.liveRegion, isTrue);
        expect(find.byKey(const ValueKey('craft-reconnect')), findsOneWidget);

        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      },
    );

    testWidgets(
      'Store controls recovery stays actionable after a quiet Retry without a shared session',
      (tester) async {
        _usePhoneViewport(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        peer.connectionSessionSupported = true;
        await tester.runAsync(() => _drive(owner.enter()));
        final library = LitchcraftLibrary(owner);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );

        peer.connection(false);
        // Keep the current epoch3 profile; activation itself is unavailable.
        peer.connectionSessionAvailable = false;
        peer.connection(true);
        await tester.runAsync(() => _idle(owner));
        await tester.pumpAndSettle();
        expect(find.byKey(const ValueKey('craft-status-card')), findsOneWidget);
        expect(find.byKey(const ValueKey('craft-reconnect')), findsOneWidget);
        expect(
          find.textContaining('Could not read artwork from your Litch'),
          findsOneWidget,
        );
        expect(find.byTooltip('Try again'), findsOneWidget);

        peer.cachedBoardRotate = null;
        final orientation = Completer<Object?>();
        final readingOrientation = Completer<void>();
        peer.onReadBoardRotate = () {
          readingOrientation.complete();
          return orientation.future;
        };
        await tester.runAsync(() async {
          await tester.tap(find.byKey(const ValueKey('craft-reconnect')));
          await _until(() => readingOrientation.isCompleted);
        });
        await tester.pump();
        expect(owner.status, LitchcraftStatus.connecting);
        expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
        await tester.pump(const Duration(milliseconds: 499));
        expect(find.byKey(const ValueKey('craft-status-card')), findsNothing);
        await tester.pump(const Duration(milliseconds: 1));
        expect(find.byKey(const ValueKey('craft-status-card')), findsOneWidget);
        expect(find.text('Preparing artwork…'), findsOneWidget);
        expect(
          find.byKey(const ValueKey('craft-sync-activity')),
          findsOneWidget,
        );
        expect(
          tester
              .widget<Semantics>(
                find.byKey(const ValueKey('craft-status-card')),
              )
              .properties
              .liveRegion,
          isTrue,
        );

        orientation.complete(false);
        await tester.runAsync(
          () => _until(() => owner.connectionRecoveryRequired),
        );
        await tester.pump();
        expect(owner.status, LitchcraftStatus.error);
        expect(find.byKey(const ValueKey('craft-status-card')), findsOneWidget);
        expect(find.byKey(const ValueKey('craft-reconnect')), findsOneWidget);
        expect(find.byTooltip('Try again'), findsOneWidget);
        expect(
          find.textContaining('Could not read artwork from your Litch'),
          findsOneWidget,
        );
        expect(find.byKey(const ValueKey('craft-sync-activity')), findsNothing);

        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      },
    );

    test(
      'retry adopts a Store root that appears after the missing-root error',
      () async {
        peer.connectionSessionSupported = true;
        peer.simulateMissingStoreRoot = true;
        peer.reportedSchema = 0;
        peer.frameStoreHasArtwork = false;
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.error);

        peer.document = staticLitchcraftDocument(LitchcraftCanvas.blank());
        peer.frameStoreGeneration = 1;
        peer.frameStoreFrameCount = 1;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStoreBody
          ..clear()
          ..addAll(_frameStoreBody(peer.document));
        peer.publishStoreAuthority();
        await _drive(owner.retryConnection());

        expect(owner.status, LitchcraftStatus.live);
        expect(owner.storeArtworkUnavailable, isFalse);
        expect(peer.frameStoreInspectCount, greaterThan(1));
        expect(peer.snapshotCount, 0);
      },
    );

    test(
      'Store session loss on reconnect cannot turn an unverified canvas live',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.usesFrameStoreDocument, isTrue);

        peer.connection(false);
        peer.connectionSessionSupported = false;
        peer.connection(true);
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.error);
        expect(owner.storeArtworkUnavailable, isTrue);
        expect(owner.connectionRecoveryRequired, isTrue);

        await _drive(owner.retryConnection());
        expect(owner.status, LitchcraftStatus.error);
        expect(owner.storeArtworkUnavailable, isTrue);
      },
    );

    test(
      'selected Store page reacquires mode after an unnoticed board reboot',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.live);
        peer.requests.clear();
        peer.session = 0;
        peer.revision = 0;
        peer.active = false;
        peer.frameStoreActive = false;
        peer.frameStoreHasArtwork = false;
        peer.frameStoreGeneration = 0;
        peer.frameStoreFrameCount = 0;
        peer.simulateMissingStoreRoot = true;

        await _drive(owner.revalidateSelectedBoard());

        expect(
          peer.requests.map((request) => request.opcode),
          contains(LitchcraftProtocol.enter),
          reason: 'a selected page must re-enter after reset authority is seen',
        );
        expect(owner.status, LitchcraftStatus.error);
        expect(owner.storeArtworkUnavailable, isTrue);
      },
    );

    test(
      'matching Store resident reconnect refreshes authority without artwork read',
      () async {
        final trace = _EntryProbeTrace();
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        peer.document = owner.documentSession.document;
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = peer.document.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStorePlaying = true;
        peer.frameStoreBody.addAll(_frameStoreBody(peer.document));

        await _drive(owner.enter());
        await _idle(owner);

        expect(owner.status, LitchcraftStatus.live);
        expect(owner.usesFrameStoreDocument, isTrue);
        expect(owner.animationPlaying, isTrue);
        expect(peer.snapshotCount, 0);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSnapshot)),
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );

        peer.operations.clear();
        peer.requests.clear();
        await _drive(owner.revalidateSelectedBoard());
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.animationPlaying, isTrue);
        expect(peer.frameStoreSnapshotTransactionCount, 0);
        expect(
          peer.requests.where(
            (request) => request.opcode == LitchcraftProtocol.snapshot,
          ),
          isEmpty,
          reason:
              'foreground Store refresh must stay on shared metadata authority',
        );
        final phases = trace.records.where(
          (record) =>
              record['event'] == 'litchcraft.store_entry_phase' &&
              record['stage'] == 'complete',
        );
        expect(
          phases.map((record) => record['phase']),
          containsAll(['inspect', 'fresh_state37', 'canonical_prepare']),
        );
        expect(phases.first['entry_kind'], 'cold');
        for (final phase in phases) {
          expect(phase['elapsed_ms'], greaterThanOrEqualTo(0));
        }
        final reads = trace.records.where(
          (record) => record['event'] == 'litchcraft.frame_store_snapshot_read',
        );
        expect(reads, isNotEmpty);
        for (final read in reads) {
          expect(read['read_kind'], 'entry_metadata');
          expect(read['body_bytes'], 0);
          expect(read['chunks'], 0);
        }
      },
    );

    test(
      'favorite palette difference does not invent an artwork conflict',
      () async {
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        owner.restoreDocument(peer.document);
        final favorites = List<int>.of(peer.document.favoriteColors)
          ..[0] = 0xabcdef;
        owner.setFavoriteColors(favorites);
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = peer.document.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStoreBody.addAll(_frameStoreBody(peer.document));

        await _drive(owner.enter());

        expect(owner.status, LitchcraftStatus.live);
        expect(owner.documentSession.document.favoriteColors, favorites);
        expect(peer.document.favoriteColors, isNot(favorites));
        expect(peer.frameStoreSnapshotTransactionCount, 0);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );

        peer.physicalStorePaint(square: 7, mask: 15, color: 0x44cc55);
        await _idle(owner);
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0xff7711,
          emit: false,
        );
        peer.physicalStorePaint(square: 9, mask: 15, color: 0x1188ee);
        await Future<void>.delayed(const Duration(milliseconds: 180));
        expect(
          peer.frameStoreSnapshotTransactionCount,
          0,
          reason:
              'an unsaved physical frame cannot be repaired from the saved root',
        );
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _idle(owner);

        expect(owner.status, LitchcraftStatus.live);
        expect(owner.documentSession.document.favoriteColors, favorites);
        expect(peer.document.favoriteColors, isNot(favorites));
        expect(
          owner.documentSession.document.frames[0].emitters[32].color,
          0xff7711,
        );
        expect(
          owner.documentSession.document.frames[0].emitters[36].color,
          0x1188ee,
        );
      },
    );

    test(
      'unsaved Hall paint cannot trigger a saved-resident chooser on entry',
      () async {
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        final saved = peer.document;
        final local = LitchcraftDocumentSession(saved)
          ..paintSelectedFrame(
            squares: const <int>[8],
            mask: 15,
            color: 0xaabbcc,
          );
        owner.restoreDocument(local.document);
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = saved.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStoreBody.addAll(_frameStoreBody(saved));
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x44cc55,
          emit: false,
        );

        await _drive(owner.enter());
        await _untilBackgroundRepair(
          () =>
              !owner.boardArtworkUnresolved &&
              owner.status == LitchcraftStatus.conflict,
        );
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            local.document,
          ),
          isTrue,
          reason: 'unknown live Hall edits cannot overwrite the phone draft',
        );
        expect(owner.conflictBoardPreview?.colors[28], 0x44cc55);
        expect(
          owner.conflictBoardPreview?.colors[32],
          saved.frames[0].emitters[32].color,
        );
        expect(peer.frameStoreLiveLayoutCount, 1);
        expect(peer.frameStoreSnapshotTransactionCount, 0);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSelect)),
          reason:
              'the source choice must use live bytes, never a saved resident',
        );
        expect(
          peer.frameStoreBody,
          _frameStoreBody(saved),
          reason: 'the scripted saved root stays older than live Hall paint',
        );
      },
    );

    test('mismatched Store resident asks before reading artwork', () async {
      peer.connectionSessionSupported = true;
      peer.reportedSchema = 0;
      owner.restoreDocument(owner.documentSession.document);
      final boardSession = LitchcraftDocumentSession(peer.document)
        ..selectFrame(0)
        ..paintSelectedFrame(
          squares: const <int>[7],
          mask: 15,
          color: 0xabcdef,
        );
      peer.document = boardSession.document;
      peer.frameStoreGeneration = 4;
      peer.frameStoreFrameCount = peer.document.frames.length;
      peer.frameStoreHasArtwork = true;
      peer.frameStoreActive = true;
      peer.frameStorePlaying = true;
      peer.frameStoreBody.addAll(_frameStoreBody(peer.document));

      await _drive(owner.enter());
      await Future<void>.delayed(Duration.zero);

      expect(owner.status, LitchcraftStatus.conflict);
      expect(peer.snapshotCount, 0);
      expect(
        peer.operations,
        isNot(contains(LitchcraftV2Protocol.frameStoreSnapshot)),
        reason: 'the user must choose before a board-artwork read',
      );

      await _drive(owner.resolveConflict(useBoard: false));
      await _idle(owner);
      expect(peer.snapshotCount, 0);
      expect(peer.operations, contains(LitchcraftV2Protocol.frameStoreCommit));
      expect(owner.status, LitchcraftStatus.live);
    });

    test('late Store Inspect cannot restore a retired app draft', () async {
      peer.connectionSessionSupported = true;
      peer.reportedSchema = 0;
      peer.frameStoreGeneration = 4;
      peer.frameStoreFrameCount = peer.document.frames.length;
      peer.frameStoreHasArtwork = true;
      peer.frameStoreActive = true;
      peer.frameStoreBody.addAll(_frameStoreBody(peer.document));
      final inspectStarted = Completer<void>();
      final releaseInspect = Completer<void>();
      peer.beforeFrameStoreInspect = () async {
        inspectStarted.complete();
        await releaseInspect.future;
      };

      final entry = owner.enter();
      await inspectStarted.future;
      final replacement = staticLitchcraftDocument(
        LitchcraftCanvas.blank().paint(<int>[0], 15, 0x123456),
      );
      owner.restoreDocument(replacement);
      releaseInspect.complete();
      await _drive(entry);
      expect(owner.status, LitchcraftStatus.offline);
      expect(owner.documentSession.document, same(replacement));
      expect(peer.frameStoreSnapshotTransactionCount, 0);
    });

    Future<LitchcraftAnimationDocument> previewConflict() async {
      peer.connectionSessionSupported = true;
      peer.reportedSchema = 0;
      owner.restoreDocument(owner.documentSession.document);
      final physical = LitchcraftDocumentSession(peer.document)..selectFrame(1);
      physical.paintSelectedFrame(squares: <int>[7], mask: 15, color: 0xabcdef);
      peer.document = physical.document;
      peer.frameStoreGeneration = 4;
      peer.frameStoreFrameCount = peer.document.frames.length;
      peer.frameStoreHasArtwork = true;
      peer.frameStoreActive = true;
      peer.frameStoreBody.addAll(_frameStoreBody(peer.document));
      await _drive(owner.enter());
      expect(owner.status, LitchcraftStatus.conflict);
      expect(owner.frameStoreArtworkReadPending, isTrue);
      expect(peer.frameStoreSnapshotTransactionCount, 0);
      return physical.document;
    }

    test(
      'Use app closes an interrupted stroke before replacement body',
      () async {
        await previewConflict();
        final draft = owner.documentSession.document;
        peer.storeStrokeBefore = peer.document;
        peer.orderedOperations.clear();
        peer.beforeFrameStoreChunk = () async {
          expect(
            peer.orderedOperations,
            contains(ConnectionSession.litchcraftToolOpcode),
            reason: 'existing Tool closes interrupted history without painting',
          );
          expect(peer.storeStrokeBefore, isNull);
        };
        await _drive(owner.resolveConflict(useBoard: false));
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.live);
        expect(sameLitchcraftDocument(peer.document, draft), isTrue);
      },
    );

    test(
      'Use app retires a delayed Tool when its local source changes',
      () async {
        await previewConflict();
        final toolEntered = Completer<void>();
        final releaseTool = Completer<void>();
        peer.beforeOrderedToolCheckpoint = () async {
          toolEntered.complete();
          await releaseTool.future;
        };
        final replacement = owner.resolveConflict(useBoard: false);
        final expected = expectLater(
          replacement,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await _drive(toolEntered.future);
        final newer = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint(<int>[5], 15, 0x123456),
        );
        owner.restoreDocument(newer);
        peer.operations.clear();
        releaseTool.complete();
        await _drive(expected);
        expect(owner.documentSession.document, same(newer));
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreBegin)),
        );
        expect(owner.status, LitchcraftStatus.conflict);
      },
    );

    test(
      'cold 256-frame chooser reads only the current frame before Board choice',
      () async {
        final first = peer.document.frames.first;
        peer.document = LitchcraftAnimationDocument(
          frames: List.generate(
            256,
            (index) => LitchcraftAnimationFrame(
              durationMs: 100 + index,
              emitters: first.emitters,
            ),
          ),
          topology: peer.document.topology,
          favoriteColors: peer.document.favoriteColors,
          playbackOnEntry: false,
          sparkleSeed: peer.document.sparkleSeed,
        );
        await previewConflict();
        peer.selectedFrame = 108;
        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: 108,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await pumpEventQueue(times: 10);
        final draft = owner.documentSession.document;
        await _drive(owner.prepareConflictBoardPreview());
        expect(owner.conflictBoardPreview, isNotNull);
        expect(
          peer.frameStoreSnapshotTransactionCount,
          0,
          reason: 'a thumbnail must not download the whole 256-frame artwork',
        );
        expect(peer.frameStorePreviewCount, 1);
        expect(
          peer.frameStorePreviewBodyBytes,
          lessThanOrEqualTo(litchcraftWireMaximumFrameBytes + 6),
        );
        expect(owner.documentSession.document, same(draft));
        expect(owner.fullAnimationSyncDirection, isNull);
        await _drive(owner.resolveConflict(useBoard: true));
        expect(peer.frameStoreSnapshotTransactionCount, 1);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
      },
    );

    test(
      'cached conflict preview freezes its frame while Board adoption keeps newer Pause',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint(<int>[7], 15, 0x123456),
        );
        final second = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint(<int>[7], 15, 0xabcdef),
        );
        final physical = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            first.frames.first,
            second.frames.first,
          ],
          topology: first.topology,
          favoriteColors: first.favoriteColors,
          playbackOnEntry: false,
          sparkleSeed: first.sparkleSeed,
        );
        await _drive(owner.replaceDocument(physical));
        await _idle(owner);
        peer.selectedFrame = 0;
        // A separately accepted physical paint/Undo can retain these cached
        // bytes with newer live authority. The old common receipt no longer
        // permits automatic replacement by the subsequently selected draft.
        peer.frameStoreLiveRevision += 2;
        final draft = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint(<int>[2], 15, 0x22cc44),
        );
        owner.restoreDocument(draft);
        await _drive(owner.revalidateSelectedBoard());
        expect(owner.status, LitchcraftStatus.conflict);
        final fullReads = peer.frameStoreSnapshotTransactionCount;
        final frameReads = peer.frameStorePreviewCount;
        await _drive(owner.prepareConflictBoardPreview());
        expect(owner.conflictBoardPreview!.colors[28], 0x123456);
        expect(peer.frameStorePreviewCount, frameReads);
        expect(peer.frameStoreSnapshotTransactionCount, fullReads);
        expect(owner.documentSession.document, same(draft));

        peer.selectedFrame = 1;
        peer.frameStorePlaying = true;
        peer.connectionLitchcraftState(
          sequence: peer._connectionLitchcraftStateSequence + 1,
          selectedFrame: 1,
          frameElapsedMilliseconds: 0,
          playing: true,
        );
        await pumpEventQueue(times: 10);
        expect(
          owner.conflictBoardPreview!.colors[28],
          0x123456,
          reason: 'a cached thumbnail freezes just like a cold one-frame read',
        );
        expect(owner.documentSession.document, same(draft));
        expect(peer.frameStorePreviewCount, frameReads);
        expect(peer.frameStoreSnapshotTransactionCount, fullReads);

        peer.frameStorePlaying = false;
        peer.connectionLitchcraftState(
          sequence: peer._connectionLitchcraftStateSequence + 1,
          selectedFrame: 1,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await pumpEventQueue(times: 10);
        expect(owner.conflictBoardPreview!.colors[28], 0x123456);
        await _drive(owner.resolveConflict(useBoard: true));
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.animationSelectedFrameIndex, 1);
        expect(owner.animationPlaying, isFalse);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, physical),
          isTrue,
        );
        expect(peer.frameStoreSnapshotTransactionCount, fullReads);
      },
    );

    test(
      'Store conflict preview coalesces one-frame reads; Board choice reads the document',
      () async {
        final physical = await previewConflict();
        final draft = owner.documentSession.document;
        final common = owner.documentSession.boardDocument;
        final selected = owner.documentSession.selectedFrameIndex;
        final release = Completer<void>();
        peer.beforeFrameStorePreview = () => release.future;
        final first = owner.prepareConflictBoardPreview();
        expect(owner.prepareConflictBoardPreview(), same(first));
        await _until(() => peer.frameStorePreviewCount == 1);
        expect(owner.conflictBoardPreviewLoading, isTrue);
        expect(owner.conflictBoardPreview, isNull);
        expect(owner.documentSession.document, same(draft));
        final choose = owner.resolveConflict(useBoard: true);
        release.complete();
        expect(owner.documentSession.boardDocument, same(common));
        await _drive(first);
        await _drive(choose);
        expect(peer.frameStoreSnapshotTransactionCount, 1);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, physical),
          isTrue,
        );
        expect(owner.conflictBoardPreview, isNull);
        expect(owner.conflictBoardPreviewLoading, isFalse);
        expect(owner.documentSession.selectedFrameIndex, selected);
      },
    );

    test(
      'Store Board choice preserves newer runtime Pause while authority waits',
      () async {
        final physical = await previewConflict();
        await _drive(owner.prepareConflictBoardPreview());
        expect(peer.frameStoreSnapshotTransactionCount, 0);
        peer.frameStorePlaying = true;
        // Hold the final choice authority fence, after the newly separate
        // full-document read. Its preceding Inspect is stale relative to Pause.
        peer.dropStoreAuthorityQueryAt = peer.storeAuthorityQueryCount + 3;
        final choice = owner.resolveConflict(useBoard: true);
        await _until(() => peer.dropStoreAuthorityQueryAt == null);
        peer.frameStorePlaying = false;
        peer.selectedFrame = 1;
        peer.connectionLitchcraftState(
          sequence: peer._connectionLitchcraftStateSequence + 1,
          selectedFrame: 1,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await Future<void>.delayed(Duration.zero);
        expect(owner.animationPlaying, isFalse);
        peer.publishStoreAuthority();
        await _drive(choice);
        debugPrint(
          'CONFLICT_CHOICE newerRuntime=Pause playing=${owner.animationPlaying} '
          'selected=${owner.animationSelectedFrameIndex} '
          'bodyReads=${peer.frameStoreSnapshotTransactionCount} next=Edit',
        );
        expect(
          owner.animationPlaying,
          isFalse,
          reason: 'Choice Inspect must not overwrite a newer runtime Pause',
        );
        expect(owner.animationSelectedFrameIndex, 1);
        expect(peer.frameStoreSnapshotTransactionCount, 1);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer._connectionSession?.isActivated, isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, physical),
          isTrue,
        );
      },
    );

    test(
      'Store conflict preview freezes the current frame without streaming playback or selecting the draft',
      () async {
        final physical = await previewConflict();
        final draft = owner.documentSession.document;
        final selected = owner.documentSession.selectedFrameIndex;
        await _drive(owner.prepareConflictBoardPreview());
        expect(
          owner.conflictBoardPreview!.colors[28],
          physical.frames[0].emitters[28].color,
        );
        peer.selectedFrame = 1;
        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: 1,
          frameElapsedMilliseconds: 0,
          playing: true,
        );
        await pumpEventQueue(times: 10);
        expect(
          owner.conflictBoardPreview!.colors[28],
          physical.frames[0].emitters[28].color,
        );
        expect(peer.frameStorePreviewCount, 1);
        owner.retireConflictBoardPreview();
        await _drive(owner.prepareConflictBoardPreview());
        expect(
          owner.conflictBoardPreview!.colors[28],
          physical.frames[1].emitters[28].color,
        );
        expect(peer.frameStorePreviewCount, 2);
        expect(peer.frameStoreSnapshotTransactionCount, 0);
        expect(owner.documentSession.document, same(draft));
        expect(owner.documentSession.selectedFrameIndex, selected);
      },
    );

    test(
      'Store conflict preview invalidates on live authority and retries without adoption',
      () async {
        await previewConflict();
        final draft = owner.documentSession.document;
        final observed = <bool>[];
        owner.addListener(
          () => observed.add(owner.conflictBoardPreview != null),
        );
        await _drive(owner.prepareConflictBoardPreview());
        expect(observed.last, isTrue);
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        await pumpEventQueue(times: 10);
        expect(owner.conflictBoardPreview, isNull);
        expect(observed.last, isFalse);
        expect(owner.conflictBoardPreviewFailed, isTrue);
        expect(owner.documentSession.document, same(draft));
        await expectLater(
          _drive(owner.resolveConflict(useBoard: true)),
          throwsStateError,
        );
        expect(owner.status, LitchcraftStatus.conflict);
        expect(peer._connectionSession?.isActivated, isTrue);
        peer.persistStore();
        await _drive(owner.prepareConflictBoardPreview());
        expect(owner.conflictBoardPreview!.colors[32], 0x31c7ff);
        expect(owner.documentSession.document, same(draft));
        await _drive(owner.resolveConflict(useBoard: true));
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
      },
    );

    for (final retirement in <String>[
      'dismiss',
      'app',
      'source',
      'disconnect',
    ]) {
      test(
        'Store conflict preview retires $retirement before a late read',
        () async {
          await previewConflict();
          final draft = owner.documentSession.document;
          final release = Completer<void>();
          peer.beforeFrameStorePreview = () => release.future;
          final read = owner.prepareConflictBoardPreview();
          await _until(() => peer.frameStorePreviewCount == 1);
          Future<void>? choice;
          final newer = staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint([63], 15, 0x123456),
          );
          if (retirement == 'dismiss') {
            owner.retireConflictBoardPreview();
          } else if (retirement == 'app') {
            choice = owner.resolveConflict(useBoard: false);
          } else if (retirement == 'source') {
            owner.restoreDocument(newer);
          } else {
            peer.connection(false);
          }
          await _drive(read);
          expect(owner.conflictBoardPreviewLoading, isFalse);
          expect(owner.conflictBoardPreview, isNull);
          release.complete();
          await pumpEventQueue(times: 20);
          if (choice != null) await _drive(choice);
          expect(owner.conflictBoardPreview, isNull);
          expect(
            owner.documentSession.document,
            same(retirement == 'source' ? newer : draft),
          );
          expect(
            peer.orderedOperations,
            isNot(contains(LitchcraftFrameStoreProtocol.storeCancel)),
          );
          if (retirement != 'disconnect') {
            expect(peer._connectionSession?.isActivated, isTrue);
            expect(owner.status, isNot(LitchcraftStatus.error));
          }
        },
      );
    }

    test('choosing board reads mismatched Store artwork once', () async {
      peer.connectionSessionSupported = true;
      peer.reportedSchema = 0;
      owner.restoreDocument(owner.documentSession.document);
      final boardSession = LitchcraftDocumentSession(peer.document)
        ..selectFrame(0)
        ..paintSelectedFrame(
          squares: const <int>[7],
          mask: 15,
          color: 0xabcdef,
        );
      peer.document = boardSession.document;
      peer.frameStoreGeneration = 4;
      peer.frameStoreFrameCount = peer.document.frames.length;
      peer.frameStoreHasArtwork = true;
      peer.frameStoreActive = true;
      peer.frameStorePlaying = true;
      peer.frameStoreBody.addAll(_frameStoreBody(peer.document));

      await _drive(owner.enter());
      await Future<void>.delayed(Duration.zero);
      expect(owner.frameStoreArtworkReadPending, isTrue);
      expect(peer.snapshotCount, 0);

      final heldSnapshot = Completer<void>();
      peer.beforeFrameStoreSnapshot = () => heldSnapshot.future;
      final resolution = owner.resolveConflict(useBoard: true);
      await _until(
        () =>
            owner.conflictBoardPreviewLoading &&
            peer.frameStoreSnapshotTransactionCount == 1,
      );
      expect(owner.conflictBoardPreview, isNotNull);
      peer.dropNextStoreAuthorityQuery = true;
      heldSnapshot.complete();
      await Future<void>.delayed(const Duration(milliseconds: 160));
      await _drive(resolution);
      await _idle(owner);

      expect(peer.frameStoreSnapshotTransactionCount, 1);
      expect(peer.dropNextStoreAuthorityQuery, isFalse);
      expect(owner.fullAnimationSyncDirection, isNull);
      expect(owner.conflictBoardPreviewLoading, isFalse);
      expect(owner.frameStoreArtworkReadPending, isFalse);
      expect(owner.status, LitchcraftStatus.live);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, peer.document),
        isTrue,
      );
    });

    test(
      'failed Use board read keeps the choice available for retry',
      () async {
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        owner.restoreDocument(owner.documentSession.document);
        final boardSession = LitchcraftDocumentSession(peer.document)
          ..selectFrame(0)
          ..paintSelectedFrame(
            squares: const <int>[7],
            mask: 15,
            color: 0xabcdef,
          );
        peer.document = boardSession.document;
        peer.frameStoreGeneration = 4;
        peer.frameStoreFrameCount = peer.document.frames.length;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreActive = true;
        peer.frameStorePlaying = true;
        peer.frameStoreBody.addAll(_frameStoreBody(peer.document));

        await _drive(owner.enter());
        await Future<void>.delayed(Duration.zero);
        expect(owner.status, LitchcraftStatus.conflict);
        peer.beforeFrameStoreSnapshot = () async {
          throw StateError('controlled board artwork read failure');
        };
        await expectLater(
          _drive(owner.resolveConflict(useBoard: true)),
          throwsStateError,
        );
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.frameStoreArtworkReadPending, isTrue);
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            boardSession.document,
          ),
          isFalse,
        );

        peer.beforeFrameStoreSnapshot = null;
        await _drive(owner.resolveConflict(useBoard: true));
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.frameStoreArtworkReadPending, isFalse);
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            boardSession.document,
          ),
          isTrue,
        );
      },
    );

    test(
      'shared-session board without Store support requests a firmware update',
      () async {
        peer.connectionSessionSupported = true;
        peer.simulateMissingStoreRoot = true;
        peer.frameStoreSupported = false;
        await _drive(owner.enter());
        expect(peer._connectionSession?.isActivated, isTrue);
        expect(owner.status, LitchcraftStatus.unsupported);
        expect(owner.animationPlaying, isFalse);
        expect(peer.frameStoreGeneration, 0);
        expect(peer.snapshotCount, 0);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.replaceBeginOp)),
        );
      },
    );

    LitchcraftAnimationDocument largeDraft({bool playbackOnEntry = false}) =>
        LitchcraftAnimationDocument(
          frames: List.generate(
            32,
            (index) => LitchcraftAnimationFrame(
              emitters: peer.document.frames.first.emitters,
              durationMs: 50 + index,
            ),
          ),
          topology: peer.document.topology,
          favoriteColors: peer.document.favoriteColors,
          playbackOnEntry: playbackOnEntry,
        );

    for (final frameCount in [32, 256]) {
      test(
        'unchanged Rainbow $frameCount Play to Craft stays quiet without encoding or artwork IO',
        () async {
          SharedPreferences.setMockInitialValues(<String, Object>{});
          final trace = _EntryProbeTrace();
          owner.dispose();
          owner = LitchcraftController(peer, appTrace: trace);
          final store = _CountingArtworkStore(
            await SharedPreferences.getInstance(),
          );
          final library = LitchcraftLibrary(owner, store: store);
          await library.ready;
          addTearDown(library.dispose);
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          final preset = LitchcraftSurprisePreset.rainbowRibbon;
          await _drive(
            owner.replaceDocument(
              LitchcraftAnimationDocument(
                frames: preset.createFrames().take(frameCount),
                topology: peer.document.topology,
                favoriteColors: peer.document.favoriteColors,
                sparkleSeed: preset.sparkleSeed,
              ),
            ),
          );
          await library.flush();
          final savesBefore = store.saves;
          final session = peer._connectionSession!;
          final layoutsBefore = peer.frameStoreLiveLayoutCount;
          final snapshotsBefore = peer.frameStoreSnapshotTransactionCount;
          final readingCues = <bool>[];
          owner.addListener(() => readingCues.add(owner.boardArtworkLoading));
          trace.records.clear();
          peer.operations.clear();
          final inspectBefore = peer.frameStoreInspectCount;
          final authorityBefore = peer.storeAuthorityQueryCount;
          for (var turn = 0; turn < 3; turn++) {
            await _drive(owner.leave());
            peer.session++;
            await _drive(owner.enter());
            await _idle(owner);
            await library.flush();
          }
          expect(owner.status, LitchcraftStatus.live);
          expect(owner.boardActive, isTrue);
          expect(session.isActivated, isTrue);
          expect(peer.frameStoreLiveLayoutCount, layoutsBefore);
          expect(peer.frameStoreSnapshotTransactionCount, snapshotsBefore);
          expect(
            readingCues,
            isNot(contains(true)),
            reason: 'metadata validation must not claim an artwork read',
          );
          expect(
            trace.records.where(
              (record) =>
                  record['event'] == 'litchcraft.store_entry_phase' &&
                  record['phase'] == 'canonical_prepare',
            ),
            isEmpty,
            reason:
                'the immutable uploaded content already has a canonical identity',
          );
          expect(
            peer.operations,
            isNot(
              anyElement(
                isIn([
                  LitchcraftV2Protocol.frameStoreSnapshot,
                  LitchcraftV2Protocol.frameStoreBegin,
                  LitchcraftV2Protocol.frameStoreChunk,
                  LitchcraftV2Protocol.frameStoreCommit,
                ]),
              ),
            ),
          );
          expect(peer.frameStoreInspectCount - inspectBefore, 3);
          expect(peer.storeAuthorityQueryCount - authorityBefore, 3);
          expect(
            store.saves,
            savesBefore,
            reason: 'navigation does not change the local durable draft',
          );
          expect(
            sameLitchcraftDocument(
              owner.documentSession.document,
              peer.document,
            ),
            isTrue,
          );
        },
      );
    }

    testWidgets(
      'warm Store metadata validation does not show a read or failure card',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        peer.connectionSessionSupported = true;
        final library = LitchcraftLibrary(owner);
        await tester.runAsync(() async {
          await library.ready;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
        });
        final started = Completer<void>();
        final release = Completer<void>();
        addTearDown(() {
          if (!release.isCompleted) release.complete();
        });
        peer.beforeFrameStoreInspect = () async {
          started.complete();
          await release.future;
        };
        late Future<void> refresh;
        await tester.runAsync(() async {
          refresh = owner.revalidateSelectedBoard();
          await _until(() => started.isCompleted);
        });
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.pump();
        expect(
          owner.boardArtworkUnresolved,
          isTrue,
          reason: 'editing still waits for authoritative metadata',
        );
        expect(
          find.textContaining('Reading artwork from your Litch'),
          findsNothing,
        );
        expect(find.textContaining('Board controls unavailable'), findsNothing);
        expect(find.byKey(const ValueKey('craft-sync-activity')), findsNothing);
        release.complete();
        await tester.runAsync(() => _drive(refresh));
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      },
    );

    testWidgets(
      'exhausted Store authority query keeps healthy controls and offers retry',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final trace = _EntryProbeTrace();
        late LitchcraftLibrary library;
        await tester.runAsync(() async {
          owner.dispose();
          owner = LitchcraftController(peer, appTrace: trace);
          library = LitchcraftLibrary(owner);
        });
        peer.connectionSessionSupported = true;
        addTearDown(library.dispose);
        await tester.runAsync(() async {
          await library.ready;
          await owner.enter().timeout(const Duration(seconds: 6));
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          await _drive(owner.replaceDocument(largeDraft()));
          await library.flush();
        });
        final shared = peer._connectionSession!;
        final queryStart = peer.storeAuthorityQueryCount;
        final bodyReads = peer.frameStoreSnapshotTransactionCount;
        peer.operations.clear();
        trace.records.clear();
        peer.dropStoreAuthorityQueries = true;
        late Future<void> refresh;
        await tester.runAsync(() async {
          refresh = owner.revalidateSelectedBoard();
          await _until(() => peer.storeAuthorityQueryCount > queryStart);
          final runtime = await shared
              .readState(code: ConnectionSession.litchcraftStateCode)
              .timeout(const Duration(seconds: 3));
          expect(
            runtime.code,
            ConnectionSession.litchcraftStateCode,
            reason:
                'only State37 is missing; the typed State stream remains healthy',
          );
        });
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.pump();
        expect(
          find.textContaining('Reading artwork from your Litch'),
          findsNothing,
        );
        expect(
          find.byKey(const ValueKey('craft-reconnect')),
          findsNothing,
          reason: 'an ordinary in-flight metadata check stays quiet',
        );
        await tester.runAsync(
          () => refresh.timeout(const Duration(seconds: 3)),
        );
        await tester.pump();
        expect(
          owner.connectionRecoveryRequired,
          isTrue,
          reason:
              'exhausted bounded authority queries need an actionable recovery',
        );
        expect(peer.storeAuthorityQueryCount - queryStart, greaterThan(1));
        expect(shared.isActivated, isTrue);
        expect(identical(peer._connectionSession, shared), isTrue);
        expect(
          find.textContaining('Board controls unavailable'),
          findsOneWidget,
        );
        expect(find.byKey(const ValueKey('craft-reconnect')), findsOneWidget);
        expect(find.byKey(const ValueKey('craft-sync-activity')), findsNothing);
        peer.dropStoreAuthorityQueries = false;
        await tester.runAsync(() async {
          await tester.tap(find.byKey(const ValueKey('craft-reconnect')));
          await owner.settleModeResolution();
        });
        await tester.pump();
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.boardArtworkUnresolved, isFalse);
        expect(shared.isActivated, isTrue);
        expect(identical(peer._connectionSession, shared), isTrue);
        expect(peer.frameStoreSnapshotTransactionCount, bodyReads);
        expect(
          trace.records.where(
            (record) =>
                record['event'] == 'litchcraft.store_entry_phase' &&
                record['phase'] == 'canonical_prepare',
          ),
          isEmpty,
        );
        expect(
          peer.operations,
          isNot(
            anyElement(
              isIn([
                LitchcraftV2Protocol.frameStoreSnapshot,
                LitchcraftV2Protocol.frameStoreBegin,
                LitchcraftV2Protocol.frameStoreChunk,
                LitchcraftV2Protocol.frameStoreCommit,
              ]),
            ),
          ),
        );
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );

    test(
      'Exit after live Snapshot retires the held resident proof without follow-up requests',
      () async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        final store = _CountingArtworkStore(
          await SharedPreferences.getInstance(),
        );
        final library = LitchcraftLibrary(owner, store: store);
        addTearDown(library.dispose);
        await library.ready;
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.leave());
        final draft = owner.documentSession.document;
        final changed = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: [11], mask: 15, color: 0xabcdef);
        peer.document = changed.document;
        peer.frameStoreLiveRevision++;
        peer.frameStoreGeneration++;
        peer.storeLiveDirty = true;
        final held = Completer<void>();
        final release = Completer<void>();
        addTearDown(() {
          if (!release.isCompleted) release.complete();
        });
        var inspections = 0;
        peer.beforeFrameStoreInspect = () async {
          if (++inspections != 2) return;
          held.complete();
          await release.future;
        };
        await _drive(owner.enter());
        await _untilBackgroundRepair(() => held.isCompleted);
        await library.flush();
        final saves = store.saves;
        await _drive(owner.leave());
        final queries = peer.storeAuthorityQueryCount;
        final operations = List<int>.of(peer.operations);
        release.complete();
        await _idle(owner);
        await pumpEventQueue(times: 20);
        await library.flush();
        expect(
          peer.storeAuthorityQueryCount,
          queries,
          reason:
              'a completed retired Inspect cannot authorize another State37 query',
        );
        expect(peer.operations, operations);
        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.documentSession.document, same(draft));
        expect(store.saves, saves);
      },
    );

    testWidgets(
      'open Craft choice retires during revalidation and reopens with fresh artwork',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final navigator = GlobalKey<NavigatorState>();
        final trace = _EntryProbeTrace();
        late LitchcraftLibrary library;
        late _CountingArtworkStore store;
        late LitchcraftAnimationDocument draft;
        var libraryDisposed = false;
        await tester.runAsync(() async {
          owner.dispose();
          owner = LitchcraftController(peer, appTrace: trace);
          peer.connectionSessionSupported = true;
          draft = largeDraft();
          owner.restoreDocument(draft);
          store = _CountingArtworkStore(await SharedPreferences.getInstance());
          library = LitchcraftLibrary(owner, store: store);
          await library.ready;
          await _drive(owner.enter());
          await _drive(owner.prepareConflictBoardPreview());
        });
        addTearDown(() {
          if (!libraryDisposed) library.dispose();
        });
        expect(owner.status, LitchcraftStatus.conflict);
        await tester.pumpWidget(
          MaterialApp(
            navigatorKey: navigator,
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        await _pumpTransport(tester);
        final choice = find.byKey(const ValueKey('craft-use-board-artwork'));
        expect(choice, findsOneWidget);
        expect(tester.widget<InkWell>(choice).onTap, isNotNull);
        expect(owner.conflictBoardPreview, isNotNull);
        expect(owner.conflictBoardPreview?.colors[44], isNot(0xabcdef));
        final oldChoiceRoute = ModalRoute.of(tester.element(choice))!;
        // Another modal is independently owned, even when stacked above the
        // old sheet. Retiring the Craft route must leave this route current.
        unawaited(
          showDialog<void>(
            context: navigator.currentContext!,
            builder: (_) => const AlertDialog(title: Text('Other modal')),
          ),
        );
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));

        final release = Completer<void>();
        final started = Completer<void>();
        addTearDown(() {
          if (!release.isCompleted) release.complete();
        });
        late Future<void> refresh;
        final changed = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: [11], mask: 15, color: 0xabcdef);
        peer.document = changed.document;
        peer.frameStoreLiveRevision++;
        peer.storeLiveDirty = true;
        peer.admitStoreReadsBeforeResponse = true;
        peer.beforeFrameStoreSnapshot = () async {
          started.complete();
          await release.future;
        };
        refresh = owner.revalidateSelectedBoard();
        for (var turn = 0; turn < 100 && !started.isCompleted; turn++) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 10)),
          );
          await tester.pump(const Duration(milliseconds: 1));
        }
        expect(
          started.isCompleted,
          isTrue,
          reason:
              'held live read: status=${owner.status} pending=${owner.pending} '
              'unresolved=${owner.boardArtworkUnresolved} error=${owner.errorMessage} '
              'operations=${peer.operations}',
        );
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 600));
        expect(oldChoiceRoute.isActive, isFalse);
        expect(find.text('Other modal'), findsOneWidget);
        expect(
          choice,
          findsNothing,
          reason:
              'stale board cards must not stay tappable while the fresh live read is held',
        );
        expect(owner.documentSession.document, same(draft));
        expect(owner.boardArtworkLoading, isTrue);
        expect(
          find.byKey(const ValueKey('craft-sync-activity')),
          findsOneWidget,
        );

        release.complete();
        await _pumpTransport(tester);
        await refresh;
        for (
          var turn = 0;
          turn < 100 && owner.status != LitchcraftStatus.conflict;
          turn++
        ) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 10)),
          );
          await tester.pump(const Duration(milliseconds: 1));
        }
        expect(
          owner.status,
          LitchcraftStatus.conflict,
          reason:
              'proof unresolved=${owner.boardArtworkUnresolved} error=${owner.errorMessage} '
              'frames=${owner.documentSession.document.frames.length} local=${owner.documentSession.hasLocalIntent} '
              'trace=${trace.records.where((row) => row["event"].toString().contains("physical_store")).toList()}',
        );
        await tester.pump();
        expect(find.text('Other modal'), findsOneWidget);
        expect(choice, findsNothing);
        navigator.currentState!.pop();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        await tester.pump(const Duration(milliseconds: 300));
        expect(choice, findsOneWidget);
        expect(owner.conflictBoardPreview?.colors[44], 0xabcdef);
        expect(owner.documentSession.document, same(draft));
        await tester.ensureVisible(choice);
        await tester.tap(choice);
        await tester.pump(const Duration(milliseconds: 300));
        for (
          var turn = 0;
          turn < 100 && owner.status != LitchcraftStatus.live;
          turn++
        ) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 10)),
          );
          await tester.pump();
        }
        expect(owner.status, LitchcraftStatus.live);
        expect(
          owner.documentSession.document.frames.first.emitters[44].color,
          0xabcdef,
        );
        expect(peer.storeLiveDirty, isTrue);
        await tester.runAsync(() async {
          final saved = await store.load();
          final preserved = await saved.previousDraft!.resolve();
          expect(sameLitchcraftDocument(preserved.document, draft), isTrue);
        });
        await tester.pump(const Duration(milliseconds: 300));
        expect(choice, findsNothing);

        Future<void> refreshMountedChoice() async {
          final refreshing = owner.revalidateSelectedBoard();
          var refreshed = false;
          refreshing.then((_) => refreshed = true);
          for (
            var turn = 0;
            turn < 100 && (!refreshed || owner.boardArtworkUnresolved);
            turn++
          ) {
            await tester.runAsync(
              () => Future<void>.delayed(const Duration(milliseconds: 10)),
            );
            await tester.pump(const Duration(milliseconds: 10));
          }
          expect(refreshed, isTrue);
          expect(owner.boardArtworkUnresolved, isFalse);
          await tester.pump(const Duration(milliseconds: 300));
        }

        void divergeArtwork(int localColor, int boardColor) {
          owner.documentSession.paintSelectedFrame(
            squares: [11],
            mask: 15,
            color: localColor,
          );
          final changed = LitchcraftDocumentSession(peer.document)
            ..paintSelectedFrame(squares: [11], mask: 15, color: boardColor);
          peer.document = changed.document;
          peer.frameStoreLiveRevision++;
        }

        peer.beforeFrameStoreSnapshot = null;
        divergeArtwork(0x112233, 0x445566);
        await refreshMountedChoice();
        expect(owner.status, LitchcraftStatus.conflict);
        await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
        await tester.pump(const Duration(milliseconds: 300));
        expect(choice, findsOneWidget);

        // This open choice becomes unnecessary: fresh live board bytes now
        // exactly equal the phone. Its reopen intent must end with this proof.
        peer.document = owner.documentSession.document;
        peer.frameStoreLiveRevision++;
        await refreshMountedChoice();
        expect(owner.status, LitchcraftStatus.live);
        expect(choice, findsNothing);
        divergeArtwork(0x778899, 0xaabbcc);
        await refreshMountedChoice();
        expect(owner.status, LitchcraftStatus.conflict);
        expect(
          choice,
          findsNothing,
          reason:
              'an unrelated later conflict must not reuse the settled reopen intent',
        );
        final manualChoice = find.byKey(
          const ValueKey('craft-resolve-conflict'),
        );
        expect(manualChoice, findsOneWidget);
        await tester.tap(manualChoice);
        await tester.pump(const Duration(milliseconds: 300));
        expect(choice, findsOneWidget);
        navigator.currentState!.pop();
        await tester.pump(const Duration(milliseconds: 300));
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
        libraryDisposed = true;
        owner.dispose();
      },
    );

    test(
      'dirty revalidation replaces an obsolete choice while preserving the local draft',
      () async {
        peer.connectionSessionSupported = true;
        final draft = largeDraft();
        owner.restoreDocument(draft);
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.conflict);
        await _drive(owner.prepareConflictBoardPreview());
        final oldPreview = owner.conflictBoardPreview;
        expect(oldPreview, isNotNull);
        final savedBody = List<int>.of(peer.frameStoreBody);
        final layouts = peer.frameStoreLiveLayoutCount;
        final changed = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: [11], mask: 15, color: 0xabcdef);
        peer.document = changed.document;
        peer.frameStoreLiveRevision++;
        peer.storeLiveDirty = true;
        await _drive(owner.revalidateSelectedBoard());
        await _untilBackgroundRepair(
          () => peer.frameStoreLiveLayoutCount == layouts + 1,
        );
        await _untilBackgroundRepair(
          () => owner.status == LitchcraftStatus.conflict,
        );
        expect(owner.boardArtworkUnresolved, isFalse);
        expect(owner.documentSession.document, same(draft));
        expect(owner.conflictBoardPreview?.colors[44], 0xabcdef);
        expect(peer.frameStoreBody, savedBody);
        expect(peer.storeLiveDirty, isTrue);
        expect(peer._connectionSession!.isActivated, isTrue);
      },
    );

    test(
      'newer dirty authority reinspects once and never waits for another save',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        owner.beginStroke();
        owner.paintSquares([9]);
        owner.endStroke();
        await _idle(owner);
        final shared = peer._connectionSession!;

        void advanceSavedRoot() {
          // Supply a completed predecessor save between the actual Inspect
          // response and State37 query. Current live edits remain unsaved.
          peer.frameStoreGeneration++;
          for (
            var index = 0;
            index < peer._frameStoreResidents.length;
            index++
          ) {
            final resident = peer._frameStoreResidents[index];
            if (resident.rootSlot != peer.frameStoreSelectedRoot) continue;
            peer._frameStoreResidents[index] = _TestFrameResident(
              rootSlot: resident.rootSlot,
              generation: peer.frameStoreGeneration,
              document: resident.document,
              body: resident.body,
            );
          }
        }

        var inspections = peer.frameStoreInspectCount;
        var layouts = peer.frameStoreLiveLayoutCount;
        final savedReads = peer.frameStoreSnapshotTransactionCount;
        peer.onStoreAuthorityQuery = () {
          peer.onStoreAuthorityQuery = null;
          advanceSavedRoot();
        };
        await _drive(owner.revalidateSelectedBoard());
        expect(
          peer.frameStoreInspectCount - inspections,
          2,
          reason: 'dirty generation mismatch must refresh metadata once',
        );
        expect(owner.boardArtworkUnresolved, isFalse);
        expect(
          peer.frameStoreLiveLayoutCount,
          layouts,
          reason: 'exact confirmed live bytes still need no artwork read',
        );
        expect(peer.frameStoreSnapshotTransactionCount, savedReads);

        inspections = peer.frameStoreInspectCount;
        layouts = peer.frameStoreLiveLayoutCount;
        peer.onStoreAuthorityQuery = () {
          peer.onStoreAuthorityQuery = null;
          advanceSavedRoot();
          final changed = LitchcraftDocumentSession(peer.document)
            ..paintSelectedFrame(squares: [11], mask: 15, color: 0xabcdef);
          peer.document = changed.document;
          peer.frameStoreLiveRevision++;
        };
        await _drive(owner.revalidateSelectedBoard());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(
          peer.frameStoreInspectCount - inspections,
          3,
          reason:
              'one metadata retry, then existing live repair resident inspection',
        );
        expect(peer.frameStoreLiveLayoutCount, layouts + 1);
        expect(
          owner.documentSession.document.frames.first.emitters[44].color,
          0xabcdef,
        );
        expect(
          peer.storeLiveDirty,
          isTrue,
          reason: 'live recovery cannot depend on another completed save',
        );
        expect(peer.frameStoreSnapshotTransactionCount, savedReads);

        inspections = peer.frameStoreInspectCount;
        layouts = peer.frameStoreLiveLayoutCount;
        peer.onStoreAuthorityQuery = advanceSavedRoot;
        await _drive(owner.revalidateSelectedBoard());
        peer.onStoreAuthorityQuery = null;
        expect(peer.frameStoreInspectCount - inspections, 2);
        expect(
          owner.connectionRecoveryRequired,
          isTrue,
          reason:
              'a second mismatch must expose recovery rather than wait indefinitely',
        );
        expect(peer.frameStoreLiveLayoutCount, layouts);
        expect(shared.isActivated, isTrue);
      },
    );

    for (final result in [
      'adopt',
      'conflict',
      'storage failure',
      'departure',
    ]) {
      test(
        'unknown dirty Store entry reads live bytes before save: $result',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await _drive(owner.leave());
          if (result == 'conflict') {
            owner.documentSession.paintSelectedFrame(
              squares: [9],
              mask: 15,
              color: 0x332211,
            );
          }
          final draft = owner.documentSession.document;
          final savedBody = List<int>.of(peer.frameStoreBody);
          final savedGeneration = peer.frameStoreGeneration;
          // The peer bypasses Hall input and flash: supply a missed accepted live
          // revision while its committed body/CRC still describes the predecessor.
          final board = LitchcraftDocumentSession(peer.document)
            ..paintSelectedFrame(squares: [9], mask: 15, color: 0xabcdef);
          peer.document = board.document;
          peer.frameStoreLiveRevision++;
          peer.storeLiveDirty = true;
          final release = Completer<void>();
          addTearDown(() {
            if (!release.isCompleted) release.complete();
          });
          final layouts = peer.frameStoreLiveLayoutCount;
          final savedReads = peer.frameStoreSnapshotTransactionCount;
          final readProgress = <double>[];
          void recordProgress() {
            if (owner.fullAnimationSyncDirection ==
                LitchcraftFullSyncDirection.readingFromBoard) {
              readProgress.add(owner.fullAnimationSyncProgress!);
            }
          }

          owner.addListener(recordProgress);
          peer.beforeFrameStoreSnapshot = () => release.future;
          if (result == 'storage failure') {
            peer.rejectNextFrameStoreSnapshotStatus = 10;
          }
          await _drive(owner.enter());
          await _until(() => peer.frameStoreLiveLayoutCount == layouts + 1);
          expect(owner.boardArtworkUnresolved, isTrue);
          expect(
            owner.boardArtworkLoading,
            isTrue,
            reason: 'an actual live read owns the reading cue',
          );
          expect(owner.documentSession.document, same(draft));
          final shared = peer._connectionSession!;
          final leaving = result == 'departure' ? owner.leave() : null;
          if (leaving != null) expect(owner.boardArtworkLoading, isFalse);
          release.complete();
          if (leaving != null) await _drive(leaving);
          await _untilBackgroundRepair(
            () =>
                !owner.boardArtworkLoading &&
                (result == 'storage failure'
                    ? owner.connectionRecoveryRequired
                    : result == 'departure'
                    ? !owner.boardActive
                    : !owner.boardArtworkUnresolved),
          );
          owner.removeListener(recordProgress);
          if (result == 'adopt' || result == 'conflict') {
            expect(readProgress, isNotEmpty);
            expect(readProgress.last, .99);
            expect(readProgress, contains(inExclusiveRange(0.0, .99)));
          } else {
            expect(readProgress, isEmpty);
          }
          expect(owner.fullAnimationSyncDirection, isNull);
          expect(shared.isActivated, isTrue);
          expect(
            peer.frameStoreBody,
            savedBody,
            reason:
                'held/failed persistence cannot be required for live recovery',
          );
          expect(peer.frameStoreGeneration, savedGeneration);
          expect(peer.storeLiveDirty, isTrue);
          expect(
            peer.frameStoreSnapshotTransactionCount,
            savedReads,
            reason: 'the saved root is not the live artwork',
          );
          if (result == 'adopt') {
            expect(owner.status, LitchcraftStatus.live);
            expect(
              owner.documentSession.document.frames.first.emitters[36].color,
              0xabcdef,
            );
          } else if (result == 'conflict') {
            expect(owner.status, LitchcraftStatus.conflict);
            expect(owner.documentSession.document, same(draft));
            expect(owner.conflictBoardPreview?.colors[36], 0xabcdef);
          } else if (result == 'storage failure') {
            expect(owner.status, LitchcraftStatus.error);
            expect(owner.documentSession.document, same(draft));
            await _drive(owner.retryConnection());
            await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
            expect(owner.status, LitchcraftStatus.live);
            expect(
              owner.documentSession.document.frames.first.emitters[36].color,
              0xabcdef,
            );
          } else {
            expect(owner.status, LitchcraftStatus.offline);
            expect(owner.documentSession.document, same(draft));
          }
        },
      );
    }

    for (final reboot in [false, true]) {
      test(
        'dirty confirmed content requires the same board boot on reconnect reboot=$reboot',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          owner.beginStroke();
          owner.paintSquares([9]);
          owner.endStroke();
          await _idle(owner);
          final confirmed = owner.documentSession.document;
          final layouts = peer.frameStoreLiveLayoutCount;
          final release = Completer<void>();
          addTearDown(() {
            if (!release.isCompleted) release.complete();
          });
          if (reboot) peer.beforeFrameStoreSnapshot = () => release.future;
          expect(peer.storeLiveDirty, isTrue);
          peer.connection(false);
          await _idle(owner);
          // Model the BLE owner's new activation. The scripted peer supplies
          // exact boot identity; firmware dirty-state persistence is not mocked.
          await peer._connectionSession!.dispose();
          await peer._connectionPackets!.close();
          peer._connectionSession = null;
          peer._reverseObjectSequence = 0;
          peer._lastStoreReceipt = null;
          if (reboot) peer.boardBootNonce++;
          peer.connection(true);
          await Future<void>.delayed(Duration.zero);
          await _drive(owner.settleModeResolution());
          expect(
            owner.boardArtworkUnresolved,
            reboot,
            reason:
                'same generation/revision in a different boot is no live proof',
          );
          expect(owner.documentSession.document, same(confirmed));
          if (reboot) {
            await _until(() => peer.frameStoreLiveLayoutCount == layouts + 1);
            expect(owner.boardArtworkLoading, isTrue);
            release.complete();
            await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
            expect(
              sameLitchcraftDocument(
                owner.documentSession.document,
                peer.document,
              ),
              isTrue,
            );
          } else {
            expect(owner.boardArtworkLoading, isFalse);
            expect(
              peer.frameStoreLiveLayoutCount,
              layouts,
              reason:
                  'the exact same-boot confirmed revision needs no body read',
            );
          }
        },
      );
    }

    test(
      'canonical identity ignores palette and selection but detects changed frame content',
      () async {
        final trace = _EntryProbeTrace();
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.leave());
        owner.documentSession.selectFrame(17);
        owner.documentSession.setFavoriteColors([
          0xabcdef,
          ...owner.documentSession.document.favoriteColors.skip(1),
        ]);
        trace.records.clear();
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.live);
        expect(
          trace.records.where(
            (record) =>
                record['event'] == 'litchcraft.store_entry_phase' &&
                record['phase'] == 'canonical_prepare',
          ),
          isEmpty,
        );

        await _drive(owner.leave());
        final changedFrame = owner.documentSession.selectedFrameIndex;
        owner.documentSession.setSelectedFrameDuration(456);
        final draft = owner.documentSession.document;
        trace.records.clear();
        final writesFrom = peer.operations.length;
        await _drive(owner.enter());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          owner.documentSession.document,
          same(draft),
          reason: 'unchanged board ancestry preserves the changed local frame',
        );
        expect(sameLitchcraftDocument(peer.document, draft), isTrue);
        expect(peer.document.frames[changedFrame].durationMs, 456);
        expect(
          peer.operations
              .skip(writesFrom)
              .where(
                (opcode) => opcode == LitchcraftV2Protocol.frameStoreCommit,
              ),
          hasLength(1),
          reason: 'Changed frame bytes must get a new identity and real Commit',
        );
        expect(
          trace.records.where(
            (record) =>
                record['event'] == 'litchcraft.store_entry_phase' &&
                record['phase'] == 'canonical_prepare' &&
                record['stage'] == 'complete',
          ),
          hasLength(1),
        );
      },
    );

    for (final result in ['success', 'failure', 'departure']) {
      test('actual Store artwork read owns its cue until $result', () async {
        peer.connectionSessionSupported = true;
        peer.document = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([9], 15, 0x4488cc),
        );
        final release = Completer<void>();
        addTearDown(() {
          if (!release.isCompleted) release.complete();
        });
        peer.beforeFrameStoreSnapshot = () async {
          await release.future;
          if (result == 'failure') {
            throw StateError('controlled artwork read failure');
          }
        };
        await _drive(owner.enter());
        await _until(() => owner.boardArtworkLoading);
        expect(owner.boardArtworkUnresolved, isTrue);
        final leaving = result == 'departure' ? owner.leave() : null;
        if (leaving != null) {
          expect(
            owner.boardArtworkLoading,
            isFalse,
            reason:
                'the old read generation loses its cue immediately on departure',
          );
        }
        release.complete();
        if (leaving != null) await _drive(leaving);
        await _until(() => !owner.boardArtworkLoading);
        await _idle(owner);
        expect(
          owner.status,
          result == 'failure'
              ? LitchcraftStatus.error
              : result == 'departure'
              ? LitchcraftStatus.offline
              : LitchcraftStatus.live,
        );
        if (result == 'success') {
          expect(
            sameLitchcraftDocument(
              owner.documentSession.document,
              peer.document,
            ),
            isTrue,
          );
        }
      });
    }

    test(
      'post-New read barrier keeps accepted paint and finger-up on the live connection',
      () async {
        peer.connectionSessionSupported = true;
        final trace = _EntryProbeTrace();
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        await _drive(owner.enter());
        final session = peer._connectionSession!;
        final readsBefore = peer.frameStoreLiveLayoutCount;
        owner.beginStroke();
        owner.paintSquares([7]);
        await _until(() => peer.paintCount == 1);
        await _idle(owner);
        expect(peer.storeStrokeBefore, isNotNull);
        // The native InstantNew journey proves the exact firmware response.
        // Replay that pending-save result through the real decoder/controller
        // while an accepted app stroke still awaits finger-up.
        peer.rejectNextFrameStoreSnapshotStatus = 1;
        peer.frameStoreLiveRevision++;
        peer.storeLiveDirty = true;
        peer.publishStoreAuthority();
        await _untilBackgroundRepair(
          () => peer.frameStoreLiveLayoutCount > readsBefore,
        );
        owner.endStroke();
        await _idle(owner);
        expect(trace.failures, isEmpty);
        expect(trace.recoveries, isEmpty);
        await _untilBackgroundRepair(
          () => peer.frameStoreLiveLayoutCount > readsBefore + 1,
        );
        await _idle(owner);
        expect(peer.storeStrokeBefore, isNull);
        expect(owner.status, LitchcraftStatus.live);
        expect(identical(peer._connectionSession, session), isTrue);
        expect(session.isActivated, isTrue);
        expect(peer.document.frames.first.emitters[28].color, owner.color);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
      },
    );

    test(
      'Store refusal during re-entry cannot retire healthy shared connection',
      () async {
        peer.connectionSessionSupported = true;
        final trace = _EntryProbeTrace();
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.leave());
        peer.session++;
        await _drive(owner.enter());
        final session = peer._connectionSession!;
        final readsBefore = peer.frameStoreLiveLayoutCount;
        // The phone ring omitted the Store status. Exercise a controlled
        // stale-mode response through the real Store decoder, not an SDK error.
        peer.rejectNextFrameStoreSnapshotStatus = 2;
        peer.frameStoreLiveRevision++;
        peer.storeLiveDirty = true;
        peer.publishStoreAuthority();
        await _untilBackgroundRepair(
          () => peer.frameStoreLiveLayoutCount > readsBefore,
        );
        await Future<void>.delayed(const Duration(milliseconds: 800));
        expect(
          trace.failures,
          isNotEmpty,
          reason:
              'controlled read rejection reached the reported recovery seam',
        );
        expect(
          session.isActivated,
          isTrue,
          reason: 'artwork rejection cannot dispose the BLE-owned session',
        );
        expect(peer.connected, isTrue);
        expect(identical(peer._connectionSession, session), isTrue);
      },
    );

    for (final queuedBeforeExit in <bool>[false, true]) {
      test(
        'acknowledged Exit retires late Store history repair queuedBefore=$queuedBeforeExit',
        () async {
          peer.connectionSessionSupported = true;
          final trace = _EntryProbeTrace();
          owner.dispose();
          owner = LitchcraftController(peer, appTrace: trace);
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          final session = peer._connectionSession!;
          final readsBefore = peer.frameStoreLiveLayoutCount;
          // Equivalent controlled fault: the exact phone rejection status was
          // not recorded. Status2 represents a retired mode read, not BLE loss.
          peer.beforeFrameStoreSnapshot = () async {
            throw const LitchcraftRejected(2);
          };
          void delayedAuthority() {
            peer.frameStoreLiveRevision++;
            peer.storeLiveDirty = true;
            peer.publishStoreAuthority();
          }

          if (queuedBeforeExit) delayedAuthority();
          await _drive(owner.leave());
          expect(owner.selected, isFalse);
          expect(owner.boardActive, isFalse);
          if (!queuedBeforeExit) delayedAuthority();
          // A single late Timeline half exercises the real link recovery timer.
          peer.revision += 6;
          peer.advancedEvent(LitchcraftV2Protocol.timelineLow, 0, <int>[
            1,
            0,
            0,
            0,
            0,
            0,
            0,
          ]);
          await Future<void>.delayed(const Duration(milliseconds: 1600));
          expect(
            trace.failures,
            isEmpty,
            reason: 'retired mode repair cannot fail the healthy shared link',
          );
          expect(trace.recoveries, isEmpty);
          expect(
            peer.frameStoreLiveLayoutCount,
            readsBefore,
            reason: 'Exit retires queued and newly arriving Store readback',
          );
          expect(peer.connected, isTrue);
          expect(identical(peer._connectionSession, session), isTrue);
          expect(session.isActivated, isTrue);
          expect(owner.selected, isFalse);
          expect(owner.boardActive, isFalse);
          expect(owner.status, LitchcraftStatus.offline);
        },
      );
    }

    for (final differentDraft in <bool>[false, true]) {
      test(
        'saved256 inactive startup retains ${differentDraft ? 'different' : 'same'} draft without recovery then admits explicit entry',
        () async {
          // Match the retained V3 release trace: boot recovered saved artwork,
          // while the volatile mode session is inactive/zero. This scripted
          // protocol peer bypasses firmware boot and flash persistence; the
          // production controller, typed session and codecs remain unchanged.
          final saved = LitchcraftAnimationDocument(
            frames: LitchcraftBouncingBallsPreset.decode().frames,
            topology: peer.document.topology,
            favoriteColors: peer.document.favoriteColors,
          );
          peer
            ..connectionSessionSupported = true
            ..reportedSchema = 0
            ..document = saved
            ..active = false
            ..session = 0
            ..revision = 0
            ..frameStoreGeneration = 192
            ..frameStoreFrameCount = 256
            ..frameStoreTopology = saved.topology.id
            ..frameStoreHasArtwork = true;
          peer.frameStoreBody.addAll(_frameStoreBody(saved));
          final draft = LitchcraftDocumentSession(saved);
          if (differentDraft) draft.setSelectedFrameDuration(2000);
          draft.selectFrame(73);
          final retained = draft.document;
          final trace = _EntryProbeTrace();
          owner.dispose();
          owner = LitchcraftController(
            peer,
            documentSession: draft,
            appTrace: trace,
          );

          final adopted = await _drive(owner.adoptActiveBoard());

          expect(
            trace.failures,
            isEmpty,
            reason:
                'Inactive saved-root observation must not throw: '
                '${owner.errorMessage}',
          );
          expect(trace.recoveries, isEmpty);
          expect(adopted, isFalse);
          expect(owner.status, LitchcraftStatus.offline);
          expect(owner.selected, isFalse);
          expect(owner.boardActive, isFalse);
          expect(owner.frameStoreArtworkReadPending, isFalse);
          expect(owner.pending, isFalse);
          expect(owner.errorMessage, isNull);
          expect(owner.documentSession.document, same(retained));
          expect(owner.documentSession.selectedFrameIndex, 73);
          expect(peer.active, isFalse);
          expect(peer.session, 0);
          expect(peer.frameStoreGeneration, 192);
          expect(peer.document, same(saved));
          expect(peer._connectionSession!.isActivated, isTrue);
          expect(peer.requests, isEmpty);
          expect(peer.operations, <int>[LitchcraftV2Protocol.inspect]);
          expect(peer.frameStoreInspectCount, 0);
          expect(peer.frameStoreSnapshotTransactionCount, 0);
          expect(peer.frameStorePreviewCount, 0);
          expect(peer.snapshotCount, 0);

          await _drive(owner.enter());
          await _idle(owner);

          expect(peer.session, greaterThan(0));
          expect(peer.active, isTrue);
          expect(owner.boardActive, isTrue);
          expect(owner.selected, isTrue);
          expect(
            owner.status,
            differentDraft ? LitchcraftStatus.conflict : LitchcraftStatus.live,
          );
          expect(owner.documentSession.document, same(retained));
          expect(peer.document, same(saved));
          expect(peer.frameStoreGeneration, 192);
          expect(peer.frameStoreInspectCount, 1);
          expect(
            peer.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
            hasLength(1),
          );
          expect(
            peer.operations,
            isNot(
              anyElement(
                isIn(<int>[
                  LitchcraftV2Protocol.frameStoreBegin,
                  LitchcraftV2Protocol.frameStoreChunk,
                  LitchcraftV2Protocol.frameStoreCommit,
                ]),
              ),
            ),
          );
          expect(trace.failures, isEmpty);
          expect(trace.recoveries, isEmpty);
        },
      );
    }

    for (final matchingDraft in <bool>[true, false]) {
      test(
        'active saved256 cold ${matchingDraft ? 'resident' : 'snapshot'} admission replays the original playback anchor',
        () async {
          final saved = LitchcraftAnimationDocument(
            frames: LitchcraftBouncingBallsPreset.decode().frames,
            topology: peer.document.topology,
            favoriteColors: peer.document.favoriteColors,
          );
          peer
            ..connectionSessionSupported = true
            ..reportedSchema = 0
            ..document = saved
            ..session = 42
            ..frameStoreGeneration = 192
            ..frameStoreFrameCount = 256
            ..frameStoreTopology = saved.topology.id
            ..frameStoreHasArtwork = true
            ..frameStoreActive = true
            ..frameStorePlaying = true
            ..playing = true
            ..selectedFrame = 73;
          peer.frameStoreBody.addAll(_frameStoreBody(saved));
          var now = 100;
          var inspectionStarted = false;
          var replayRequested = false;
          var holdReplay = true;
          peer.onConnectionLitchcraftStateQuery = () {
            if (inspectionStarted && holdReplay) {
              replayRequested = true;
              return;
            }
            // State Query replays the same retained publication, not a new
            // clock sample. Duplicate delivery keeps its first observed time.
            peer.connectionLitchcraftState(
              sequence: 9,
              selectedFrame: 73,
              frameElapsedMilliseconds: 17,
              playing: true,
            );
          };
          peer.beforeFrameStoreInspect = () async {
            inspectionStarted = true;
            now = 115;
          };
          owner.dispose();
          owner = LitchcraftController(
            peer,
            documentSession: LitchcraftDocumentSession(
              matchingDraft
                  ? saved
                  : staticLitchcraftDocument(LitchcraftCanvas.blank()),
              localIntent: matchingDraft,
            ),
            monotonicMilliseconds: () => now,
          );

          expect(await _drive(owner.adoptActiveBoard()), isTrue);
          await _until(() => !owner.boardArtworkUnresolved);
          await _idle(owner);

          expect(owner.status, LitchcraftStatus.live);
          expect(owner.boardActive, isTrue);
          expect(owner.animationPlaying, isTrue);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, saved),
            isTrue,
          );
          expect(
            peer.frameStoreSnapshotTransactionCount,
            matchingDraft ? 0 : 1,
          );
          expect(
            owner.animationSelectedFrameIndex,
            73,
            reason: 'Pending timing replay must not flash frame zero',
          );
          expect(owner.animationElapsedMilliseconds, 73 * 50);
          expect(replayRequested, isTrue);
          holdReplay = false;
          peer.onConnectionLitchcraftStateQuery!();
          expect(owner.animationSelectedFrameIndex, 73);
          expect(owner.animationElapsedMilliseconds, 73 * 50 + 17 + 15);
          expect(
            peer.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
            isEmpty,
          );

          now = 135;
          peer.onConnectionLitchcraftStateQuery!();
          expect(owner.animationSelectedFrameIndex, 74);
          expect(
            owner.animationElapsedMilliseconds,
            73 * 50 + 17 + 35,
            reason: 'Duplicate replay must not reset the admitted anchor',
          );
        },
      );
    }

    for (final replaceAgain in <bool>[false, true]) {
      test(
        replaceAgain
            ? 'pending Enter commits only the latest one-frame Open'
            : 'pending Enter Open settles a healthy Store root live and paused',
        () async {
          peer.connectionSessionSupported = true;
          // Supply a healthy board-owned root; this peer bypasses firmware boot.
          peer.reportedSchema = 0;
          peer.frameStoreGeneration = 4;
          peer.frameStoreFrameCount = peer.document.frames.length;
          peer.frameStoreHasArtwork = true;
          peer.frameStoreActive = true;
          peer.frameStoreBody.addAll(_frameStoreBody(peer.document));
          final admissionStarted = Completer<void>();
          final releaseAdmission = Completer<void>();
          addTearDown(() {
            if (!releaseAdmission.isCompleted) releaseAdmission.complete();
          });
          peer.onMutation = (request) async {
            expect(request.opcode, LitchcraftProtocol.enter);
            admissionStarted.complete();
            await releaseAdmission.future;
            peer.onMutation = null;
            peer.state(request.id);
          };
          final committed = <LitchcraftAnimationDocument>[];
          peer.beforeFrameStoreCommit = () async {
            committed.add(peer.document);
          };
          final animation = LitchcraftAnimationDocument(
            frames: List<LitchcraftAnimationFrame>.filled(
              256,
              peer.document.frames.first,
            ),
            topology: peer.document.topology,
            favoriteColors: peer.document.favoriteColors,
            playbackOnEntry: true,
            sparkleSeed: peer.document.sparkleSeed,
          );
          final newest = replaceAgain
              ? staticLitchcraftDocument(
                  LitchcraftCanvas.blank().paint([8], 15, 0x44cc55),
                )
              : animation;

          final entering = owner.enter(draftReady: Future<void>.value());
          await _drive(admissionStarted.future);
          final firstOpen = owner.replaceDocument(animation);
          final latestOpen = replaceAgain
              ? owner.replaceDocument(newest)
              : firstOpen;
          expect(owner.documentSession.document, same(newest));
          expect(owner.animationPlaying, isFalse);
          expect(committed, isEmpty);
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
            reason: 'Open cannot commit before mode admission is acknowledged',
          );

          final settling = Future.wait<void>(<Future<void>>[
            entering,
            firstOpen,
            if (replaceAgain) latestOpen,
          ]);
          releaseAdmission.complete();
          await _drive(settling);
          await _idle(owner);

          expect(peer._connectionSession?.isActivated, isTrue);
          expect(
            peer.operations.where(
              (operation) => operation == LitchcraftV2Protocol.frameStoreCommit,
            ),
            hasLength(1),
          );
          expect(committed, hasLength(1));
          expect(sameLitchcraftDocument(committed.single, newest), isTrue);
          expect(peer.frameStoreFrameCount, newest.frames.length);
          expect(peer.frameStoreBody, _frameStoreBody(newest));
          expect(sameLitchcraftDocument(peer.document, newest), isTrue);
          expect(
            sameLitchcraftDocument(
              owner.documentSession.document,
              peer.document,
            ),
            isTrue,
          );
          expect(owner.status, LitchcraftStatus.live);
          expect(owner.frameStoreArtworkReadPending, isFalse);
          expect(owner.boardArtworkUnresolved, isFalse);
          expect(owner.animationPlaying, isFalse);
          expect(peer.frameStorePlaying, isFalse);
          expect(owner.pending, isFalse);
        },
      );
    }

    test(
      'normal 32-frame artwork loads Store in background and Play stays Store',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final document = largeDraft();
        peer.operations.clear();
        await _drive(owner.replaceDocument(document));
        expect(owner.usesFrameStoreDocument, isTrue);
        expect(owner.animationPlaying, isFalse);
        expect(owner.documentSession.document, same(document));
        expect(peer.frameStoreFrameCount, 32);
        expect(
          peer.operations,
          contains(LitchcraftV2Protocol.frameStoreCommit),
        );
        await _drive(owner.toggleAnimationPlayback());
        await _drive(owner.toggleAnimationPlayback());
        expect(peer.frameStoreFrameCount, 32);
        peer.operations.clear();
        peer.orderedOperations.clear();
        peer.paintCount = 0;
        await owner.selectAnimationFrame(31);
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftPaintOpcode),
          reason: 'Store paint uses the shared ordered lane immediately',
        );
        expect(peer.paintCount, 1);
        expect(peer.operations, isEmpty);
        expect(
          owner.documentSession.document.frames.last.emitters[28].color,
          owner.color,
        );
        expect(
          peer.document.frames.last.emitters[28].color,
          owner.color,
          reason:
              'the ordered packet reaches board authority without a Store upload',
        );
      },
    );

    test(
      'Store emitter batch refuses a coalesced later foreign live revision',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.setBrush(color: 0x224466, targetMask: 5));
        final before = peer.document;
        final liveBefore = peer.frameStoreLiveRevision;
        var interrupted = false;
        LitchcraftAnimationDocument? acceptedLocal;
        peer.afterOrderedPaintBeforeCheckpoint = () async {
          if (interrupted) return;
          interrupted = true;
          acceptedLocal = peer.document;
          // Existing scripted peer fault seam: accept real controller Paint,
          // then coalesce a later physical revision into its terminal receipt.
          // Its typed reverse physical batch is delayed, not manufactured.
          peer.physicalStorePaint(
            square: 8,
            mask: 15,
            color: 0x31c7ff,
            emit: false,
          );
        };
        owner.beginStroke();
        owner.paintSquares([7, 9]);
        owner.endStroke();
        await _idle(owner);
        expect(interrupted, isTrue);
        expect(peer.frameStoreLiveRevision, liveBefore + 3);
        expect(peer.document.frames.first.emitters[28].color, 0x224466);
        expect(peer.document.frames.first.emitters[36].color, 0x224466);
        expect(peer.document.frames.first.emitters[32].color, 0x31c7ff);
        debugPrint(
          'STORE_EMITTER foreign savedG=${peer.frameStoreGeneration} '
          'startLiveR=$liveBefore localDelta=2 terminalLiveR=${peer.frameStoreLiveRevision} '
          'confirmedLocal=${sameLitchcraftDocument(owner.documentSession.boardDocument!, acceptedLocal!)} '
          'boardRGB8=${peer.document.frames.first.emitters[32].color} '
          'appConfirmedRGB8=${owner.documentSession.boardDocument!.frames.first.emitters[32].color} '
          'next=wait-for-physical-batch',
        );
        expect(
          sameLitchcraftDocument(
            owner.documentSession.boardDocument!,
            acceptedLocal!,
          ),
          isFalse,
          reason:
              'A terminal revision beyond the exact two-square result cannot '
              'confirm local bytes that omit the later board edit',
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.boardDocument!, before),
          isTrue,
          reason:
              'Keep the proven common artwork while the foreign batch is missing',
        );
        peer.afterOrderedPaintBeforeCheckpoint = null;
        peer.emitDelayedPhysicalStoreBatch();
        peer.persistStore();
        await _untilBackgroundRepair(
          () => sameLitchcraftDocument(
            owner.documentSession.document,
            peer.document,
          ),
        );
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.canvas.colors[28], 0x224466);
        expect(owner.canvas.colors[36], 0x224466);
        expect(owner.canvas.colors[32], 0x31c7ff);
      },
    );

    for (final erase in [false, true]) {
      test(
        'Store emitter batch counts distinct changes and empty close erase=$erase',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          final draft = LitchcraftDocumentSession(largeDraft());
          if (erase) {
            draft.paintSelectedFrame(
              squares: [9, 13],
              mask: 5,
              color: 0x112233,
            );
            for (final square in [9, 11]) {
              draft.applyEffect(
                square: square,
                mask: 5,
                effect: LitchcraftEffectStyle.breathe,
                effectDurationMs: 1600,
              );
            }
          } else {
            draft.paintSelectedFrame(squares: [7], mask: 5, color: 0x224466);
          }
          await _drive(owner.replaceDocument(draft.document));
          await _drive(owner.setBrush(color: 0x224466, targetMask: 5));
          owner.beginStroke();
          owner.paintSquares([1]);
          owner.endStroke();
          await _idle(owner);
          await _drive(
            owner.setBrush(color: 0x224466, eraser: erase, targetMask: 5),
          );
          final before = peer.document;
          final historyBefore = peer.storeUndo.length;
          final liveBefore = peer.frameStoreLiveRevision;
          final savedBefore = peer.frameStoreGeneration;
          expect(
            liveBefore,
            isNot(savedBefore),
            reason:
                'The next batch starts at confirmed live authority, not Saved G',
          );
          final expected = LitchcraftDocumentSession(before)
            ..paintSelectedFrame(
              squares: [7, 9, 11, 13],
              mask: 5,
              color: erase ? 0 : 0x224466,
              clearEffects: erase,
            );
          peer.orderedPaintSquareCounts.clear();
          final effectsBefore = peer.effectCount;
          owner.beginStroke();
          owner.paintSquares([7, 9, 11, 13, 9]);
          await _idle(owner);
          final expectedDelta = erase ? 4 : 3;
          expect(peer.frameStoreLiveRevision, liveBefore + expectedDelta);
          expect(peer.frameStoreGeneration, savedBefore);
          expect(peer.orderedPaintSquareCounts, [2, 1]);
          expect(peer.effectCount - effectsBefore, erase ? 1 : 0);
          expect(
            sameLitchcraftDocument(peer.document, expected.document),
            isTrue,
          );
          expect(
            sameLitchcraftDocument(
              owner.documentSession.boardDocument!,
              expected.document,
            ),
            isTrue,
          );
          // Duplicate visits and the input no-op do not create extra writes;
          // release still closes the accepted stroke with its empty Paint.
          owner.paintSquares([7, 9, 11, 13, 9]);
          await _idle(owner);
          expect(peer.orderedPaintSquareCounts, [2, 1]);
          owner.endStroke();
          await _idle(owner);
          expect(peer.orderedPaintSquareCounts, [2, 1, 0]);
          expect(peer.frameStoreLiveRevision, liveBefore + expectedDelta);
          expect(peer.storeUndo, hasLength(historyBefore + 1));
          debugPrint(
            'STORE_EMITTER valid erase=$erase savedG=$savedBefore '
            'startLiveR=$liveBefore distinctDelta=$expectedDelta '
            'terminalLiveR=${peer.frameStoreLiveRevision} '
            'paintCounts=${peer.orderedPaintSquareCounts} effectPackets=${peer.effectCount - effectsBefore} '
            'result=exact-document next=Undo',
          );
          await _drive(owner.undo());
          expect(sameLitchcraftDocument(peer.document, before), isTrue);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, before),
            isTrue,
          );
        },
      );
    }

    test(
      'held 13-square Store drag delivers each square before pointer release',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await owner.selectAnimationFrame(31);
        peer.orderedPaintSquareCounts.clear();
        final squares = List<int>.generate(13, (index) => index + 1);

        owner.beginStroke();
        owner.paintSquares(squares);
        await _until(
          () =>
              peer.orderedPaintSquareCounts.fold<int>(0, (a, b) => a + b) == 13,
        );
        expect(peer.orderedPaintSquareCounts, [2, 2, 2, 2, 2, 2, 1]);
        for (final square in squares) {
          expect(
            peer.document.frames.last.emitters[square * 4].color,
            owner.color,
            reason: 'the board receives the crossed square while held',
          );
        }
        owner.endStroke();
        await _idle(owner);
      },
    );

    testWidgets(
      'one long held canvas move reports all crossed squares before release',
      (tester) async {
        final visited = <int>{};
        var begun = 0;
        var ended = 0;
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: Center(
                child: SizedBox.square(
                  dimension: 320,
                  child: LitchcraftCanvasView(
                    canvas: LitchcraftCanvas.blank(),
                    onBegin: () => begun++,
                    onPaint: visited.addAll,
                    onEnd: () => ended++,
                  ),
                ),
              ),
            ),
          ),
        );
        const start = Offset(20, 20);
        const finish = Offset(300, 270);
        final expected = litchcraftSquaresAlongSegment(
          start,
          finish,
          const Size.square(320),
        ).toSet();
        expect(expected.length, greaterThanOrEqualTo(13));
        final topLeft = tester.getTopLeft(
          find.byKey(const ValueKey('litchcraft-canvas')),
        );
        final pointer = await tester.startGesture(topLeft + start);
        await pointer.moveTo(topLeft + finish);
        await tester.pump();
        expect(begun, 1);
        expect(visited, expected);
        expect(ended, 0);
        await pointer.up();
        expect(ended, 1);
      },
    );

    test(
      'Store Canvas Fill and Erase use one mutation and preserve metadata',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final draft = LitchcraftDocumentSession(largeDraft())..selectFrame(31);
        draft.applyEffect(
          square: 7,
          mask: 15,
          effect: LitchcraftEffectStyle.breathe,
          effectDurationMs: 1600,
        );
        await _drive(owner.replaceDocument(draft.document));
        await _drive(owner.selectAnimationFrame(31));
        await _drive(owner.setBrush(color: 0x224466, targetMask: 5));
        final before = peer.document;
        final revision = peer.frameStoreLiveRevision;
        peer.orderedOperations.clear();
        peer.operations.clear();
        final release = Completer<void>();
        peer.beforeOrderedCanvas = () => release.future;
        final filling = owner.fillAll();
        expect(
          owner.documentSession.document.frames[31].emitters[28].color,
          0x224466,
        );
        release.complete();
        await _drive(filling);
        peer.beforeOrderedCanvas = null;
        final filled = peer.document;
        expect(peer.orderedOperations, <int>[8, 9]);
        expect(peer.orderedCanvasBodies.single, <int>[
          31,
          5,
          0x22,
          0x44,
          0x66,
          0,
        ]);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
        expect(peer.frameStoreLiveRevision, revision + 1);
        expect(peer.storeUndo, hasLength(1));
        expect(peer.operations, isEmpty);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, filled),
          isTrue,
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.boardDocument!, filled),
          isTrue,
        );
        for (var index = 0; index < 256; index++) {
          final old = before.frames[31].emitters[index];
          final next = filled.frames[31].emitters[index];
          expect(
            next.color,
            5 & (1 << (index % 4)) != 0 ? 0x224466 : old.color,
          );
          expect(next.effect, old.effect);
          expect(next.effectDurationMs, old.effectDurationMs);
        }
        expect(filled.frames[31].durationMs, before.frames[31].durationMs);
        expect(filled.frames.first, same(before.frames.first));
        await _drive(owner.fillAll());
        expect(peer.frameStoreLiveRevision, revision + 1);
        expect(
          peer.storeUndo,
          hasLength(1),
          reason: 'no-op Fill has no history boundary',
        );
        await _drive(owner.undo());
        expect(sameLitchcraftDocument(peer.document, before), isTrue);
        await _drive(owner.redo());
        expect(sameLitchcraftDocument(peer.document, filled), isTrue);
        peer.orderedOperations.clear();
        await _drive(owner.clear());
        final cleared = peer.document;
        expect(peer.orderedOperations.where((op) => op != 2), <int>[8, 9]);
        expect(peer.orderedCanvasBodies.last, <int>[31, 15, 0, 0, 0, 1]);
        expect(peer.brushMask, 5);
        expect(peer.brushColor, 0x224466);
        expect(peer.brushErase, isFalse);
        expect(owner.targetMask, 5);
        expect(owner.color, 0x224466);
        expect(
          cleared.frames[31].emitters.every(
            (e) =>
                e.color == 0 &&
                e.effect == LitchcraftEffectStyle.off &&
                e.effectDurationMs == 0,
          ),
          isTrue,
        );
        expect(cleared.frames[31].durationMs, filled.frames[31].durationMs);
        expect(cleared.frames.first, same(filled.frames.first));
        final clearRevision = peer.frameStoreLiveRevision;
        final clearUndoCount = peer.storeUndo.length;
        await _drive(owner.clear());
        expect(peer.frameStoreLiveRevision, clearRevision);
        expect(peer.storeUndo, hasLength(clearUndoCount));
        await _drive(owner.undo());
        expect(sameLitchcraftDocument(peer.document, filled), isTrue);
        await _drive(owner.redo());
        expect(sameLitchcraftDocument(peer.document, cleared), isTrue);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    test(
      'Store Canvas held swatch carries tool and checker stays atomic',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        await _drive(owner.setBrush(eraser: true, targetMask: 2));
        peer.orderedOperations.clear();
        await _drive(owner.fillAllWithColor(0xaabbcc));
        expect(peer.orderedOperations, <int>[8, 9]);
        expect(peer.brushErase, isFalse);
        expect(peer.brushColor, 0xaabbcc);
        expect(peer.brushMask, 2);
        expect(owner.eraser, isFalse);
        peer.orderedOperations.clear();
        await _drive(owner.fillChecker(dark: true));
        expect(peer.orderedOperations, <int>[8, 9]);
        expect(peer.orderedCanvasBodies.last, <int>[
          31,
          2,
          0xaa,
          0xbb,
          0xcc,
          6,
        ]);
        await _drive(owner.selectAnimationFrame(0));
        await _drive(owner.setBrush(color: 0x3d3e3f, targetMask: 2));
        await _drive(owner.fillChecker(dark: false));
        expect(peer.orderedCanvasBodies.last, <int>[0, 2, 61, 62, 63, 5]);
        final canonical = ConnectionSessionCodec.encode(
          ConnectionSessionPacket(
            kind: ConnectionSessionKind.ordered,
            code: ConnectionSession.litchcraftStoreCanvasOpcode,
            token: 7,
            sequence: 1,
            body: Uint8List.fromList(peer.orderedCanvasBodies.last),
          ),
        );
        expect(
          canonical
              .map((byte) => byte.toRadixString(16).padLeft(2, '0'))
              .join(),
          // Epoch4 changes the envelope byte; Canvas semantics stay exact.
          '4309070000000100000000023d3e3f05',
        );
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    for (final drift in <String>['generation', 'revision', 'frame']) {
      test('Store Canvas $drift rejection retires only its preview', () async {
        peer.connectionSessionSupported = true;
        peer.forceMinimumConnectionPacket = true;
        peer.fragmentStoreReceipts = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()), maxPumps: 6000);
        await _drive(owner.selectAnimationFrame(31));
        final before = peer.document;
        peer.beforeOrderedCanvas = () async {
          switch (drift) {
            case 'generation':
              peer.frameStoreGeneration++;
            case 'revision':
              peer.frameStoreLiveRevision++;
            case 'frame':
              peer.selectedFrame = 0;
          }
        };
        await _drive(owner.fillAll());
        expect(peer.orderedCanvasBodies, hasLength(1));
        expect(sameLitchcraftDocument(peer.document, before), isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, before),
          isTrue,
        );
        expect(peer.storeUndo, isEmpty);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer._connectionSession!.packetMaximum, 20);
      });
    }

    test(
      'Store Canvas captures the playing frame before slow admission',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());
        final selected = owner.animationSelectedFrameIndex;
        final entered = Completer<void>();
        final release = Completer<void>();
        peer.beforeOrderedTimeline = () async {
          entered.complete();
          await release.future;
          peer.selectedFrame = (selected + 1) % peer.document.frames.length;
        };
        peer.orderedOperations.clear();
        final filling = owner.fillAll();
        expect(owner.animationPlaying, isFalse);
        expect(owner.animationSelectedFrameIndex, selected);
        await _drive(entered.future);
        release.complete();
        await _drive(filling);
        expect(peer.orderedOperations, <int>[2, 8, 9]);
        expect(peer.selectedFrame, selected);
        expect(peer.frameStorePlaying, isFalse);
        expect(peer.orderedCanvasBodies.single.first, selected);
        expect(
          peer.document.frames[selected].emitters.map((e) => e.color),
          everyElement(owner.color),
        );
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    test(
      'Store Canvas old receipt cannot overwrite a replacement source',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final applied = Completer<void>();
        final release = Completer<void>();
        peer.afterOrderedCanvasBeforeCheckpoint = () async {
          applied.complete();
          await release.future;
        };
        final fill = owner.fillAll();
        final cancelledFill = expectLater(
          fill,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await _drive(applied.future);
        final newer = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([17], 15, 0x987654),
        );
        final replacement = owner.replaceDocument(newer);
        expect(owner.documentSession.document, same(newer));
        release.complete();
        await _drive(replacement);
        await cancelledFill;
        await _idle(owner);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, newer),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, newer), isTrue);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    test(
      '64-square Store paint and erase pack pairs with one stroke Undo',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await owner.selectAnimationFrame(31);
        final before = peer.document;
        final squares = List<int>.generate(64, (index) => index);
        peer.orderedPaintSquareCounts.clear();

        owner.beginStroke();
        owner.paintSquares(squares);
        owner.endStroke();
        await _idle(owner);

        expect(peer.orderedPaintSquareCounts, everyElement(2));
        expect(peer.orderedPaintSquareCounts.length, 32);
        expect(sameLitchcraftDocument(peer.document, before), isFalse);
        for (final square in squares) {
          expect(
            peer.document.frames.last.emitters[square * 4].color,
            owner.color,
          );
        }
        expect(owner.documentSession.canUndo, isTrue);
        final painted = peer.document;
        await owner.setBrush(eraser: true);
        peer.orderedPaintSquareCounts.clear();
        owner.beginStroke();
        owner.paintSquares(squares);
        owner.endStroke();
        await _idle(owner);
        expect(peer.orderedPaintSquareCounts, everyElement(2));
        expect(peer.orderedPaintSquareCounts.length, 32);
        for (final square in squares) {
          expect(peer.document.frames.last.emitters[square * 4].color, 0);
        }
        expect(peer.storeUndo.length, 2);
        expect(owner.documentSession.canUndo, isTrue);
        await _drive(owner.undo());
        expect(sameLitchcraftDocument(peer.document, painted), isTrue);
      },
    );

    test(
      'accepted Store paint survives later canvas cache notifications',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        final acceptedColor = owner.color;

        owner.beginStroke();
        owner.paintSquares(const <int>[7]);
        owner.endStroke();
        await _idle(owner);
        expect(peer.document.frames.last.emitters[28].color, owner.color);

        await _drive(owner.setBrush(color: 0xcc4411, targetMask: 1));
        await _drive(owner.selectAnimationFrame(0));
        await _drive(owner.selectAnimationFrame(31));
        expect(owner.canvas.colors[28], acceptedColor);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
          reason:
              'a stale board presentation cache cannot rewrite accepted bytes',
        );
      },
    );

    test('Store paint freezes playback before two transport turns', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      await _drive(owner.replaceDocument(largeDraft()));
      await _drive(owner.toggleAnimationPlayback());
      expect(peer.frameStorePlaying, isTrue);
      peer.beforeOrderedPaint = () async {
        await Future<void>.delayed(const Duration(milliseconds: 20));
        if (peer.frameStorePlaying) peer.selectedFrame = 1;
        await Future<void>.delayed(const Duration(milliseconds: 20));
        if (peer.frameStorePlaying) peer.selectedFrame = 2;
      };

      owner.beginStroke();
      owner.paintSquares(const <int>[7]);
      owner.endStroke();
      await Future<void>.delayed(const Duration(milliseconds: 60));
      await _idle(owner);

      expect(peer.frameStorePlaying, isFalse);
      expect(peer.selectedFrame, 0);
      expect(peer.paintCount, 1);
      expect(peer.document.frames.first.emitters[28].color, owner.color);
    });

    test(
      'queued Store strokes keep the first playback frame selection',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final before = peer.document;
        peer.orderedOperations.clear();
        peer.operations.clear();
        final playEntered = Completer<void>();
        final releasePlay = Completer<void>();
        peer.beforeOrderedTimeline = () async {
          peer.beforeOrderedTimeline = null;
          playEntered.complete();
          await releasePlay.future;
          // Playback reaches another frame before the held Play receipt returns.
          peer.selectedFrame = 1;
        };

        final play = owner.toggleAnimationPlayback();
        await _drive(playEntered.future);
        owner.beginStroke();
        owner.paintSquares(const <int>[7]);
        owner.endStroke();
        owner.beginStroke();
        owner.paintSquares(const <int>[8]);
        owner.endStroke();
        expect(peer.paintCount, 0);

        releasePlay.complete();
        await _drive(play);
        await _idle(owner);

        expect(
          peer.orderedOperations.take(4).toList(),
          <int>[
            ConnectionSession.litchcraftTimelineOpcode,
            ConnectionSession.litchcraftTimelineOpcode,
            ConnectionSession.litchcraftStoreGuardOpcode,
            ConnectionSession.litchcraftPaintOpcode,
          ],
          reason: 'the first stroke must select before its Guard and Paint',
        );
        expect(peer.selectedFrame, 0);
        expect(peer.document.frames.first.emitters[28].color, owner.color);
        expect(peer.document.frames.first.emitters[32].color, owner.color);
        expect(
          peer.document.frames[1].emitters[28].color,
          before.frames[1].emitters[28].color,
        );
        expect(owner.waitingForLift, isFalse);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          peer.operations,
          isEmpty,
          reason: 'no Store repair upload is needed',
        );
        expect(peer.storeUndo, hasLength(2));
        final afterFirstUndo = LitchcraftDocumentSession(before)
          ..paintSelectedFrame(
            squares: const <int>[7],
            mask: 15,
            color: owner.color,
          );
        await _drive(owner.undo());
        expect(
          sameLitchcraftDocument(peer.document, afterFirstUndo.document),
          isTrue,
        );
        await _drive(owner.undo());
        expect(sameLitchcraftDocument(peer.document, before), isTrue);
      },
    );

    test(
      'Store Clear owns the visible playback frame through its receipt',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        await _drive(owner.toggleAnimationPlayback());
        await Future<void>.delayed(const Duration(milliseconds: 90));
        final visible = owner.animationSelectedFrameIndex;
        expect(visible, isNot(31));
        peer.beforeOrderedPaint = () async {
          peer.beforeOrderedPaint = null;
          await Future<void>.delayed(const Duration(milliseconds: 20));
          if (peer.frameStorePlaying) peer.selectedFrame = 1;
          await Future<void>.delayed(const Duration(milliseconds: 20));
          if (peer.frameStorePlaying) peer.selectedFrame = 2;
        };

        final clear = owner.clear();
        await Future<void>.delayed(const Duration(milliseconds: 60));
        await _drive(clear);
        expect(peer.frameStorePlaying, isFalse);
        expect(peer.selectedFrame, visible);
        expect(peer.document.frames[visible].emitters.first.color, 0);
        expect(peer.document.frames.last.emitters.first.color, isNot(0));
        expect(
          owner.documentSession.document.frames[visible].emitters.first.color,
          0,
        );
      },
    );

    test(
      'unknown Store history rejection preserves a later local intent',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        expect(owner.documentSession.canUndo, isFalse);
        final toolEntered = Completer<void>();
        final releaseTool = Completer<void>();
        peer.beforeOrderedToolCheckpoint = () async {
          toolEntered.complete();
          await releaseTool.future;
        };
        final tool = owner.setBrush(color: 0x778899);
        await _drive(toolEntered.future);
        final unknown = owner.undo();
        await pumpEventQueue(times: 10);
        owner.documentSession.paintSelectedFrame(
          squares: [9],
          mask: 15,
          color: 0xabcdef,
        );
        final intended = owner.documentSession.document;
        releaseTool.complete();
        await _drive(tool);
        await _drive(unknown);
        await _idle(owner);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, intended),
          isTrue,
        );
        expect(peer.storeUndo, isEmpty);
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftUndoOpcode),
        );
      },
    );

    for (final replaceSource in <bool>[false, true]) {
      test(
        'unknown Store history gap retires on source or departure $replaceSource',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await _drive(owner.fillAllWithColor(0x224466));
          // The native cold-Board journey establishes this same unknown journal
          // through real reconnect/adoption. This focused lifetime test retires
          // only the phone's inverse so it can hold the genuine authority gap.
          owner.documentSession.retireLocalHistory();
          peer.physicalStorePaint(
            square: 7,
            mask: 15,
            color: 0x31c7ff,
            emit: false,
          );
          await Future<void>.delayed(Duration.zero);
          peer.orderedOperations.clear();
          expect(owner.historyRecoveryPending, isFalse);
          var settled = false;
          final history = owner.undo().whenComplete(() => settled = true);
          await pumpEventQueue(times: 10);
          expect(settled, isFalse);
          expect(owner.historyRecoveryPending, isTrue);
          expect(
            peer.orderedOperations,
            isNot(contains(ConnectionSession.litchcraftUndoOpcode)),
          );
          final newer = staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint([63], 15, 0xabcdef),
          );
          if (replaceSource) {
            owner.restoreDocument(newer);
          } else {
            await _drive(owner.leave());
          }
          await _until(() => settled);
          await _drive(history);
          expect(owner.historyRecoveryPending, isFalse);
          expect(
            peer.orderedOperations,
            isNot(contains(ConnectionSession.litchcraftUndoOpcode)),
          );
          if (replaceSource) {
            expect(
              sameLitchcraftDocument(owner.documentSession.document, newer),
              isTrue,
            );
            expect(peer._connectionSession?.isActivated, isTrue);
            expect(owner.status, isNot(LitchcraftStatus.error));
          }
        },
      );
    }

    test(
      'Store paint Undo reaches board authority without a full upload',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await owner.selectAnimationFrame(31);
        final before = peer.document;
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        expect(sameLitchcraftDocument(peer.document, before), isFalse);
        final painted = peer.document;

        await owner.selectAnimationFrame(0);
        expect(owner.animationSelectedFrameIndex, 0);

        peer.orderedOperations.clear();
        peer.operations.clear();
        final undo = owner.undo();
        expect(owner.animationSelectedFrameIndex, 31);
        expect(
          owner.canvas.colors[28],
          before.frames.last.emitters[28].color,
          reason: 'cross-frame Undo previews the inverse before Timeline ACK',
        );
        await _drive(undo);
        await _idle(owner);
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftUndoOpcode),
          reason: 'live Store Undo must use the board-owned ordered action',
        );
        expect(
          sameLitchcraftDocument(peer.document, before),
          isTrue,
          reason: 'the board must show the pre-paint frame after Undo',
        );
        expect(
          peer.operations,
          isEmpty,
          reason: 'Undo cannot reupload artwork',
        );
        peer.orderedOperations.clear();
        await _drive(owner.redo());
        await _idle(owner);
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftRedoOpcode),
        );
        expect(sameLitchcraftDocument(peer.document, painted), isTrue);
        expect(
          peer.operations,
          isEmpty,
          reason: 'Redo cannot reupload artwork',
        );
      },
    );

    test(
      'fast Store paint then Undo waits for paint receipt and reaches board',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final before = peer.document;
        final paintEntered = Completer<void>();
        final releasePaint = Completer<void>();
        peer.beforeOrderedPaint = () async {
          if (!paintEntered.isCompleted) paintEntered.complete();
          await releasePaint.future;
        };
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _drive(paintEntered.future);
        final undo = owner.undo();
        expect(owner.canvas.colors[28], before.frames.first.emitters[28].color);
        releasePaint.complete();
        await _drive(undo);
        await _idle(owner);
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftUndoOpcode),
        );
        expect(sameLitchcraftDocument(peer.document, before), isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, before),
          isTrue,
        );
      },
    );

    for (final interruption in ['foreign revision', 'dispose']) {
      test(
        'deferred independent Store tap rejects $interruption predecessor',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          final entered = Completer<void>();
          final release = Completer<void>();
          peer.afterOrderedPaintBeforeCheckpoint = () async {
            peer.afterOrderedPaintBeforeCheckpoint = null;
            entered.complete();
            await release.future;
          };
          peer.orderedOperations.clear();
          peer.orderedPaintBodies.clear();
          owner.beginStroke();
          owner.paintSquares([7]);
          owner.endStroke();
          await _drive(entered.future);
          final acceptedFirst = peer.document;
          expect(peer.storeLiveDirty, isTrue);
          expect(owner.documentSession.boardDocument, isNot(acceptedFirst));
          owner.beginStroke();
          owner.paintSquares([9]);
          owner.endStroke();
          expect(owner.pending, isTrue);
          expect(
            owner.documentSession.document.frames.first.emitters[36].color,
            owner.color,
          );
          if (interruption == 'foreign revision') {
            // Production-equivalent scripted peer accepts a physical edit after
            // the first tap, before that tap's genuine coalesced receipt.
            peer.physicalStorePaint(
              square: 8,
              mask: 15,
              color: 0x31c7ff,
              emit: false,
            );
          } else {
            owner.dispose();
          }
          release.complete();
          await _idle(owner);
          expect(
            peer.orderedPaintBodies,
            hasLength(1),
            reason:
                'An unproven or departed predecessor must not admit tap two',
          );
          expect(
            peer.orderedOperations.where(
              (op) => op == ConnectionSession.litchcraftStoreGuardOpcode,
            ),
            hasLength(1),
          );
          expect(
            peer.document.frames.first.emitters[36].color,
            acceptedFirst.frames.first.emitters[36].color,
          );
          if (interruption == 'foreign revision') {
            expect(peer.document.frames.first.emitters[32].color, 0x31c7ff);
            expect(
              owner.documentSession.document.frames.first.emitters[36].color,
              owner.color,
              reason:
                  'Refusal preserves the later local draft without sending it',
            );
          }
        },
      );
    }

    test(
      'accepted Store prefix closes its stroke after an authority gap',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final before = peer.document;

        owner.beginStroke();
        owner.paintSquares([7]);
        await _idle(owner);
        expect(peer.storeStrokeBefore, isNotNull);
        expect(peer.storeUndo, isEmpty);

        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 8, mask: 15, color: 0x31c7ff, clearEffects: false),
          ],
          emit: false,
        );
        owner.endStroke();
        await _idle(owner);
        expect(peer.storeStrokeBefore, isNotNull);

        peer.emitDelayedPhysicalStoreBatch();
        await _until(() => peer.storeStrokeBefore == null);
        await _idle(owner);
        expect(
          peer.orderedPaintSquareCounts.where((count) => count == 0),
          hasLength(1),
          reason: 'the accepted prefix needs exactly one terminal End',
        );
        expect(peer.storeUndo, hasLength(1));
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            owner.documentSession.boardDocument!,
          ),
          isTrue,
        );
        final undo = owner.undo();
        await _until(
          () => peer.orderedOperations.contains(
            ConnectionSession.litchcraftUndoOpcode,
          ),
        );
        // The fake has no native persistence clock. Publish the genuinely
        // changed dirty Undo root before awaiting this same retained intent.
        peer.persistStore();
        await _untilBackgroundRepair(() => !owner.pending);
        await _drive(undo);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
        expect(
          peer.document.frames.first.emitters[28].color,
          before.frames.first.emitters[28].color,
        );
      },
    );

    test(
      'Store Clear rejects while an earlier refused paint is recovering',
      () async {
        await _drive(owner.enter());
        final snapshotEntered = Completer<void>();
        final releaseSnapshot = Completer<void>();
        peer.beforeSnapshot = () async {
          snapshotEntered.complete();
          await releaseSnapshot.future;
        };
        peer.rejectNextAdvancedPaintBusy = true;
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        for (var turn = 0; turn < 100 && !snapshotEntered.isCompleted; turn++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        expect(
          snapshotEntered.isCompleted,
          isTrue,
          reason:
              'ordered=${peer.orderedOperations} '
              'snapshots=${peer.snapshotCount} status=${owner.status} '
              'pending=${owner.pending} waiting=${owner.waitingForLift} '
              'operations=${peer.operations}',
        );

        for (var frame = 0; frame < 8; frame++) {
          owner.documentSession.addFrame();
        }
        expect(owner.usesFrameStoreDocument, isTrue);
        expect(owner.waitingForLift, isTrue);
        expect(owner.status, LitchcraftStatus.live);

        var settled = false;
        final clearResult = owner.clear().then<void>(
          (_) => fail('Clear reported success during rejected-paint recovery'),
          onError: (Object error) {
            expect(error, isA<StateError>());
            settled = true;
          },
        );
        for (var turn = 0; turn < 100 && !settled; turn++) {
          await Future<void>.delayed(Duration.zero);
        }
        expect(
          settled,
          isTrue,
          reason: 'Clear must reject before recovery ends',
        );
        await expectLater(owner.fillAll(), throwsStateError);
        expect(peer.paintCount, 0);
        releaseSnapshot.complete();
        await _drive(clearResult);
        await _idle(owner);
      },
    );

    test(
      'rejected cross-frame Store Undo restores frame and artwork',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        final painted = peer.document;
        await _drive(owner.selectAnimationFrame(0));
        final selectedRoot = peer.frameStoreSelectedRoot;
        peer.rejectNextOrderedTimelineReason = 1;

        final undo = owner.undo();
        expect(owner.animationSelectedFrameIndex, 31);
        await _drive(undo);
        await _idle(owner);

        expect(owner.animationSelectedFrameIndex, 0);
        expect(peer.frameStoreSelectedRoot, selectedRoot);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, painted),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, painted), isTrue);
        expect(
          owner.canvas.colors[28],
          painted.frames.first.emitters[28].color,
        );
      },
    );

    testWidgets('mounted Store Undo and Redo buttons change the board frame', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      peer.connectionSessionSupported = true;
      final library = LitchcraftLibrary(owner);
      await library.ready;
      await tester.runAsync(() async {
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
      });
      await tester.runAsync(() async {
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
      });
      final painted = peer.document;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: owner, library: library),
          ),
        ),
      );
      await tester.pump();
      await tester.ensureVisible(find.byKey(const ValueKey('craft-undo')));
      expect(owner.usesFrameStoreDocument, isTrue);
      expect(owner.storeReplacementUnresolved, isFalse);
      expect(owner.pending, isFalse);
      expect(owner.canUndo, isTrue);
      expect(
        tester
            .widget<IconButton>(find.byKey(const ValueKey('craft-undo')))
            .onPressed,
        isNotNull,
      );
      peer.orderedOperations.clear();
      await tester.tap(find.byKey(const ValueKey('craft-undo')));
      await tester.runAsync(() async {
        await Future<void>.delayed(const Duration(milliseconds: 10));
      });
      for (
        var tick = 0;
        tick < 100 &&
            !peer.orderedOperations.contains(
              ConnectionSession.litchcraftUndoOpcode,
            );
        tick++
      ) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      await _pumpTransport(tester);
      await tester.pump();
      expect(
        peer.orderedOperations,
        contains(ConnectionSession.litchcraftUndoOpcode),
      );
      expect(sameLitchcraftDocument(peer.document, painted), isFalse);
      peer.orderedOperations.clear();
      await tester.tap(find.byKey(const ValueKey('craft-redo')));
      await tester.runAsync(() async {
        await Future<void>.delayed(const Duration(milliseconds: 10));
      });
      for (
        var tick = 0;
        tick < 100 &&
            !peer.orderedOperations.contains(
              ConnectionSession.litchcraftRedoOpcode,
            );
        tick++
      ) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      await _pumpTransport(tester);
      await tester.pump();
      expect(
        peer.orderedOperations,
        contains(ConnectionSession.litchcraftRedoOpcode),
      );
      expect(sameLitchcraftDocument(peer.document, painted), isTrue);
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
    });

    test(
      'dense animation progress keeps editor notifications bounded',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final document = LitchcraftAnimationDocument(
          frames: LitchcraftBouncingBallsPreset.decode().frames,
          topology: peer.document.topology,
          favoriteColors: peer.document.favoriteColors,
        );
        var progressNotifications = 0;
        void countProgress() {
          if (owner.fullAnimationSyncDirection ==
              LitchcraftFullSyncDirection.sendingToBoard) {
            progressNotifications++;
          }
        }

        owner.addListener(countProgress);
        peer.operations.clear();
        await _drive(owner.replaceDocument(document));
        owner.removeListener(countProgress);
        expect(
          peer.operations
              .where(
                (operation) =>
                    operation == LitchcraftV2Protocol.frameStoreChunk,
              )
              .length,
          greaterThan(800),
        );
        expect(
          progressNotifications,
          inInclusiveRange(2, 110),
          reason: 'dense Store transfer must not rebuild the editor per packet',
        );
      },
    );

    test(
      'physical Store edit between Guard and Paint rejects stale app paint',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        var interrupted = false;
        peer.beforeOrderedPaint = () async {
          if (interrupted) return;
          interrupted = true;
          peer.physicalStorePaint(square: 7, mask: 15, color: 0x31c7ff);
        };
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        expect(interrupted, isTrue);
        expect(peer.paintCount, 0);
        expect(peer.document.frames.first.emitters[28].color, 0x31c7ff);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          owner.canvas.colors[28],
          owner.color,
          reason: 'the local intent stays visible while authority is repaired',
        );
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.conflict);
        expect(peer.document.frames.first.emitters[28].color, 0x31c7ff);
        expect(owner.canvas.colors[28], owner.color);
        expect(
          owner.boardCanvas.colors[28],
          0x31c7ff,
          reason: 'the chooser must preview the board artwork distinctly',
        );
      },
    );

    test(
      'disjoint Hall edit during guarded paint catches up without a chooser',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        var interrupted = false;
        peer.beforeOrderedPaint = () async {
          if (interrupted) return;
          interrupted = true;
          peer.physicalStorePaint(square: 8, mask: 15, color: 0x31c7ff);
        };
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        expect(peer.paintCount, 0);
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _until(
          () => peer.document.frames.first.emitters[28].color == owner.color,
        );
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.document.frames.first.emitters[28].color, owner.color);
        expect(peer.document.frames.first.emitters[32].color, 0x31c7ff);
        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          0x31c7ff,
        );
      },
    );

    test(
      'State authority ahead of a missing Hall packet cannot bless app paint',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final firstColor = owner.color;
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 9, mask: 15, color: 0x6677aa, clearEffects: false),
          ],
          emit: false,
        );
        await _idle(owner);
        final acceptedBefore = peer.paintCount;
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        expect(peer.paintCount, acceptedBefore);
        expect(
          peer.document.frames.first.emitters[28].color,
          isNot(owner.color),
        );
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _untilBackgroundRepair(
          () => peer.document.frames.first.emitters[28].color == owner.color,
        );
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          0x31c7ff,
        );
        await owner.setBrush(color: 0xcc4411);
        owner.beginStroke();
        owner.paintSquares([9]);
        owner.endStroke();
        await _idle(owner);
        expect(peer.document.frames.first.emitters[28].color, firstColor);
        expect(peer.document.frames.first.emitters[36].color, 0xcc4411);
      },
    );

    test(
      'two guarded catch-up squares survive delayed post-ACK State',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        await _idle(owner);
        peer.delayStoreAuthorityAfterOrderedPaint = true;
        owner.beginStroke();
        owner.paintSquares([7, 9]);
        owner.endStroke();
        await _idle(owner);
        expect(peer.paintCount, 0);
        peer.persistStore();
        await _untilBackgroundRepair(
          () => peer.document.frames.first.emitters[36].color == owner.color,
        );
        expect(peer.document.frames.first.emitters[28].color, owner.color);
        expect(peer.document.frames.first.emitters[32].color, 0x31c7ff);
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'Store save generation catches up beneath a previewed fast stroke',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final before = peer.document;
        final beforeGeneration = peer.frameStoreGeneration;

        owner.beginStroke();
        owner.paintSquares([7, 9]);
        peer.persistStore();
        owner.endStroke();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _idle(owner);

        expect(peer.frameStoreGeneration, beforeGeneration + 1);
        expect(peer.document.frames.first.emitters[28].color, owner.color);
        expect(peer.document.frames.first.emitters[36].color, owner.color);
        expect(sameLitchcraftDocument(peer.document, before), isFalse);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
      },
    );

    test(
      'Clear waits for Store authority and keeps its original frame and tool',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        peer.operations.clear();
        final marked = peer.document.frames.last.emitters.first.color;
        expect(marked, isNot(0));

        // State 37 arrives before the physical edit batch, so Guard cannot
        // certify this Clear when the finger is lifted.
        peer.selectedFrame = 0;
        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 7, mask: 15, color: 0x31c7ff, clearEffects: false),
          ],
          emit: false,
        );
        final clear = owner.clear();
        var clearCompleted = false;
        clear.then((_) => clearCompleted = true);
        await _idle(owner);
        await Future<void>.delayed(Duration.zero);
        expect(peer.document.frames.last.emitters.first.color, marked);
        expect(
          owner.documentSession.document.frames.last.emitters.first.color,
          0,
        );
        expect(
          clearCompleted,
          isFalse,
          reason: 'local Clear is not a confirmed live board edit',
        );

        await _drive(owner.setBrush(color: 0xcc4411, targetMask: 1));
        await _drive(owner.selectAnimationFrame(0));
        peer.emitDelayedPhysicalStoreBatch();
        await _untilBackgroundRepair(
          () => peer.document.frames.last.emitters.first.color == 0,
        );
        await _drive(clear);
        await _idle(owner);
        expect(peer.document.frames.first.emitters[28].color, 0x31c7ff);
        expect(
          peer.document.frames.last.emitters
              .take(4)
              .every((emitter) => emitter.color == 0),
          isTrue,
        );
        expect(
          owner.documentSession.document.frames.last.emitters.first.color,
          0,
        );
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftStoreCanvasOpcode),
        );
        expect(
          peer.operations,
          isEmpty,
          reason: 'recovery uses ordered edits, not a Store upload',
        );
        expect(clearCompleted, isTrue);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
        expect(peer.orderedCanvasBodies, hasLength(1));
        expect(peer.orderedCanvasBodies.single, <int>[31, 15, 0, 0, 0, 1]);
        expect(peer.brushColor, 0xcc4411);
        expect(peer.brushMask, 1);
      },
    );

    test(
      'blocked Store Clear fails within its deadline and later repair keeps intent',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        var failedReads = 0;
        peer.beforeFrameStoreSnapshot = () async {
          failedReads++;
          peer.beforeFrameStoreSnapshot = null;
          throw StateError('controlled Store repair failure');
        };
        final clear = owner.clear();
        peer.persistStore();

        await expectLater(clear, throwsA(isA<TimeoutException>()));
        expect(peer.document.frames.last.emitters.first.color, isNot(0));
        expect(
          owner.documentSession.document.frames.last.emitters.first.color,
          0,
        );

        await _untilBackgroundRepair(
          () => peer.document.frames.last.emitters.first.color == 0,
        );
        expect(failedReads, 1);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
        expect(peer.orderedCanvasBodies, hasLength(1));
        expect(peer.orderedCanvasBodies.single, <int>[31, 15, 0, 0, 0, 1]);
      },
      timeout: const Timeout(Duration(seconds: 5)),
    );

    for (final canvasLater in <bool>[true, false]) {
      test(
        'blocked Store Canvas orders Undo before a later edit $canvasLater',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          final draft = LitchcraftDocumentSession(largeDraft())
            ..selectFrame(31);
          draft.applyEffect(
            square: 9,
            mask: 5,
            effect: LitchcraftEffectStyle.breathe,
            effectDurationMs: 1600,
          );
          await _drive(owner.replaceDocument(draft.document));
          await _drive(owner.selectAnimationFrame(31));
          peer.selectedFrame = 0;
          peer.physicalStorePaint(
            square: 7,
            mask: 15,
            color: 0x31c7ff,
            emit: false,
          );
          final beforeA = peer.document;
          final first = owner.clear();
          await _idle(owner);
          expect(owner.historyRecoveryPending, isFalse);
          var undoDone = false;
          final undo = owner.undo().then<void>((_) {
            undoDone = true;
          });
          await _idle(owner);
          expect(owner.historyRecoveryPending, isTrue);
          Future<void>? later;
          if (canvasLater) {
            later = owner.fillAllWithColor(0x224466);
          } else {
            await _drive(owner.setBrush(color: 0x224466, targetMask: 5));
            owner.beginStroke();
            owner.paintSquares(<int>[9]);
            owner.endStroke();
          }
          await _idle(owner);
          peer.emitDelayedPhysicalStoreBatch();
          await _drive(first, maxPumps: 6000);
          await _untilBackgroundRepair(
            () => peer.orderedOperations.contains(6),
          );
          // Native reverse history/persistence are separately covered by the
          // real bridge. This fake supplies the normal durable recovery root.
          peer.persistStore();
          await _untilBackgroundRepair(() => undoDone);
          await _drive(undo, maxPumps: 6000);
          expect(owner.historyRecoveryPending, isFalse);
          if (later != null) await _drive(later, maxPumps: 6000);
          final expected = patchLitchcraftEmitters(
            beforeA,
            frame: 31,
            squares: canvasLater ? List<int>.generate(64, (i) => i) : <int>[9],
            mask: canvasLater ? 15 : 5,
            color: 0x224466,
          );
          await _untilBackgroundRepair(
            () => sameLitchcraftDocument(peer.document, expected),
          );
          expect(
            peer.orderedOperations.where((op) => op == 9 || op == 6),
            canvasLater ? <int>[9, 6, 9] : <int>[9, 6],
          );
          expect(
            sameLitchcraftDocument(owner.documentSession.document, expected),
            isTrue,
          );
          expect(peer.effectCount, 0);
          expect(
            peer.orderedPaintSquareCounts,
            canvasLater ? isEmpty : <int>[1, 1, 0],
          );
          if (!canvasLater) {
            expect(
              peer.orderedPaintBodies
                  .where((body) => body.length > 8)
                  .map((body) => <int>[body[0], body[4], ...body.skip(8)]),
              <List<int>>[
                <int>[31, 1, 9],
                <int>[31, 4, 9],
              ],
            );
            expect(peer.orderedPaintBodies.last, hasLength(8));
            expect(peer.orderedPaintBodies.last[3] & 2, 2);
          }
        },
      );
    }

    for (final redoSecond in <bool>[false, true]) {
      test(
        'Store Canvas barrier chains two history intents $redoSecond',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await _drive(owner.selectAnimationFrame(31));
          final before = peer.document;
          final gate = Completer<void>();
          peer.beforeOrderedCanvas = () => gate.future;
          final first = owner.clear();
          await _until(() => peer.orderedCanvasBodies.isNotEmpty);
          final second = owner.fillAllWithColor(0x224466);
          await Future<void>.delayed(Duration.zero);
          final afterSecond = owner.documentSession.document;
          var historyDone = 0;
          final undo = owner.undo().then<void>((_) {
            historyDone++;
          });
          final next = (redoSecond ? owner.redo() : owner.undo()).then<void>((
            _,
          ) {
            historyDone++;
          });
          gate.complete();
          await _drive(
            Future.wait(<Future<void>>[first, second]),
            maxPumps: 6000,
          );
          await _untilBackgroundRepair(
            () => peer.orderedOperations.contains(6),
          );
          peer.persistStore();
          await _untilBackgroundRepair(
            () =>
                peer.orderedOperations
                    .where((op) => op == 6 || op == 7)
                    .length ==
                2,
          );
          peer.persistStore();
          await _untilBackgroundRepair(() => historyDone == 2);
          await _drive(Future.wait(<Future<void>>[undo, next]));
          expect(
            peer.orderedOperations.where((op) => op == 9 || op == 6 || op == 7),
            <int>[9, 9, 6, redoSecond ? 7 : 6],
          );
          final expected = redoSecond ? afterSecond : before;
          expect(sameLitchcraftDocument(peer.document, expected), isTrue);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, expected),
            isTrue,
          );
          expect(peer.paintCount, 0);
          expect(peer.effectCount, 0);
        },
      );
    }

    for (final ending in <String>[
      'leave',
      'replace',
      'disconnect',
      'dispose',
    ]) {
      test(
        'Store Canvas history barrier cancels followers on $ending',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          peer.physicalStorePaint(
            square: 7,
            mask: 15,
            color: 0x31c7ff,
            emit: false,
          );
          final first = owner.clear();
          await _idle(owner);
          final history = owner.undo();
          final later = owner.fillAllWithColor(0x224466);
          final cancelled = <Future<void>>[
            for (final work in <Future<void>>[first, history, later])
              expectLater(
                work.timeout(const Duration(milliseconds: 500)),
                throwsA(isA<LitchcraftTransferCancelled>()),
              ),
          ];
          await _idle(owner);
          switch (ending) {
            case 'leave':
              await _drive(owner.leave());
            case 'replace':
              await _drive(
                owner.replaceDocument(
                  staticLitchcraftDocument(LitchcraftCanvas.blank()),
                ),
              );
            case 'disconnect':
              peer.connection(false);
            case 'dispose':
              owner.dispose();
          }
          await Future.wait(cancelled);
          expect(peer.orderedCanvasBodies, isEmpty);
          expect(peer.orderedOperations, isNot(contains(6)));
          expect(peer.paintCount, 0);
          expect(peer.effectCount, 0);
        },
      );
    }

    test(
      'Store Canvas rejected predecessor cannot Undo an older native action',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        await _drive(owner.fillAllWithColor(0x224466));
        peer.orderedOperations.clear();
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final before = peer.document;
        final clearing = owner.clear();
        await _idle(owner);
        final declined = expectLater(
          owner.undo(),
          throwsA(isA<LitchcraftResyncRequired>()),
        );
        peer.beforeOrderedCanvas = () async {
          peer.selectedFrame = 0;
        };
        peer.emitDelayedPhysicalStoreBatch();
        await _drive(clearing, maxPumps: 6000);
        await declined;
        expect(peer.orderedOperations, isNot(contains(6)));
        expect(peer.storeUndo, hasLength(1));
        expect(sameLitchcraftDocument(peer.document, before), isTrue);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    test(
      'Store Canvas history repair fences hidden post-terminal physical revision',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final first = owner.clear();
        await _idle(owner);
        final history = owner.undo();
        final later = owner.fillAllWithColor(0x224466);
        final cancelled = <Future<void>>[
          for (final work in <Future<void>>[history, later])
            expectLater(work, throwsA(isA<LitchcraftTransferCancelled>())),
        ];
        peer.emitDelayedPhysicalStoreBatch();
        await _drive(first, maxPumps: 6000);
        await _untilBackgroundRepair(() => peer.orderedOperations.contains(6));
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0x77aaee,
          emit: false,
        );
        peer.persistStore();
        await _untilBackgroundRepair(
          () => owner.status == LitchcraftStatus.conflict,
        );
        await Future.wait(cancelled);
        expect(peer.document.frames[0].emitters[32].color, 0x77aaee);
        expect(
          owner.documentSession.boardDocument!.frames[0].emitters[32].color,
          0x77aaee,
        );
        expect(
          owner.documentSession.document.frames[31].emitters.first.color,
          0x224466,
        );
        expect(peer.orderedCanvasBodies, hasLength(1));
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    testWidgets(
      'dirty live foreign conflict previews and adopts through the mounted chooser',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        tester.view.physicalSize = const Size(390, 844);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.runAsync(() async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await _drive(owner.selectAnimationFrame(31));
          peer.selectedFrame = 0;
          peer.physicalStorePaint(
            square: 7,
            mask: 15,
            color: 0x31c7ff,
            emit: false,
          );
          final first = owner.clear();
          await _idle(owner);
          final history = owner.undo();
          final later = owner.fillAllWithColor(0x224466);
          final cancelled = <Future<void>>[
            for (final work in <Future<void>>[history, later])
              expectLater(work, throwsA(isA<LitchcraftTransferCancelled>())),
          ];
          peer.emitDelayedPhysicalStoreBatch();
          await _drive(first, maxPumps: 6000);
          await _untilBackgroundRepair(
            () => peer.orderedOperations.contains(6),
          );
          peer.selectedFrame = 0;
          peer.physicalStorePaint(
            square: 8,
            mask: 15,
            color: 0x77aaee,
            emit: false,
          );
          // The Store is still dirty; no quiet save is supplied by this fixture.
          await _untilBackgroundRepair(
            () => owner.status == LitchcraftStatus.conflict,
          );
          await Future.wait(cancelled);
          expect(peer.document.frames[0].emitters[32].color, 0x77aaee);
          expect(
            owner.documentSession.boardDocument!.frames[0].emitters[32].color,
            0x77aaee,
          );
          expect(
            owner.documentSession.document.frames[31].emitters.first.color,
            0x224466,
          );
          expect(peer.orderedCanvasBodies, hasLength(1));
          expect(peer.paintCount, 0);
          expect(peer.effectCount, 0);
        });
        final draft = owner.documentSession.document;
        final physical = peer.document;
        expect(peer.storeLiveDirty, isTrue);
        expect(owner.conflictBoardPreview, isNotNull);
        final fullReads = peer.frameStoreSnapshotTransactionCount;
        final layoutReads = peer.frameStoreLiveLayoutCount;
        late Directory storageDirectory;
        late LitchcraftArtworkStore store;
        await tester.runAsync(() async {
          storageDirectory = await Directory.systemTemp.createTemp(
            'litch_dirty_chooser_',
          );
          store = await LitchcraftArtworkStore.open(
            preferences: await SharedPreferences.getInstance(),
            directory: storageDirectory,
          );
        });
        addTearDown(() async => storageDirectory.delete(recursive: true));
        final library = LitchcraftLibrary(owner, store: store);
        await tester.runAsync(() => library.ready);
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.pump();
        await tester.tap(find.byKey(const ValueKey('craft-resolve-conflict')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        expect(
          owner.documentSession.document,
          same(draft),
          reason: 'opening the chooser preserves the local draft',
        );
        expect(owner.conflictBoardPreview, isNotNull);
        expect(owner.conflictBoardPreviewFailed, isFalse);
        expect(
          peer.frameStoreLiveLayoutCount,
          layoutReads,
          reason: 'opening reuses the exact verified live result',
        );
        final boardChoice = find.byKey(
          const ValueKey('craft-use-board-artwork'),
        );
        await tester.ensureVisible(boardChoice);
        await tester.pump();
        expect(tester.getCenter(boardChoice).dy, lessThan(844));
        await tester.tap(boardChoice);
        await tester.pump(const Duration(milliseconds: 300));
        for (var i = 0; i < 100 && owner.status != LitchcraftStatus.live; ++i) {
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 10)),
          );
          await tester.pump();
        }
        expect(
          owner.status,
          LitchcraftStatus.live,
          reason:
              'mounted choice must finish: saveFailed=${library.saveFailed} '
              'saveError=${library.lastSaveError} previewFailed=${owner.conflictBoardPreviewFailed}',
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.document, physical),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, physical), isTrue);
        expect(
          peer.storeLiveDirty,
          isTrue,
          reason: 'choice finishes before persistence',
        );
        expect(
          peer.frameStoreSnapshotTransactionCount,
          fullReads,
          reason: 'no saved full-artwork read',
        );
        expect(owner.pending, isFalse);
        await tester.runAsync(() async {
          final saved = await store.load();
          expect(saved.previousDraft, isNotNull);
          final preserved = await saved.previousDraft!.resolve();
          expect(
            sameLitchcraftDocument(preserved.document, draft),
            isTrue,
            reason: 'Use board durably preserves the former app draft',
          );
        });
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      },
    );

    test('Store Canvas fast Undo previews before the Fill receipt', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      await _drive(owner.replaceDocument(largeDraft()));
      final before = peer.document;
      final release = Completer<void>();
      peer.beforeOrderedCanvas = () => release.future;
      final filling = owner.fillAllWithColor(0x224466);
      await _until(() => peer.orderedCanvasBodies.isNotEmpty);
      final undoing = owner.undo();
      expect(
        sameLitchcraftDocument(owner.documentSession.document, before),
        isTrue,
        reason: 'healthy Undo retains immediate known preview ownership',
      );
      expect(peer.orderedOperations, isNot(contains(6)));
      release.complete();
      await _drive(Future.wait(<Future<void>>[filling, undoing]));
      expect(peer.orderedOperations.where((op) => op == 9 || op == 6), <int>[
        9,
        6,
      ]);
      expect(sameLitchcraftDocument(peer.document, before), isTrue);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, before),
        isTrue,
      );
    });

    for (final brushBeforeSecond in <bool>[true, false]) {
      test(
        'blocked Store Canvas orders a tool between two Fills $brushBeforeSecond',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await _drive(owner.setBrush(color: 0x224466));
          peer.orderedOperations.clear();
          final operationStart = peer.operations.length;
          final fullReads = peer.frameStoreSnapshotTransactionCount;
          peer.physicalStorePaint(
            square: 7,
            mask: 15,
            color: 0x31c7ff,
            emit: false,
          );
          final first = owner.fillAll();
          await _idle(owner);
          if (brushBeforeSecond) await _drive(owner.setBrush(color: 0xcc4411));
          final second = owner.fillAllWithColor(0x778899);
          await _idle(owner);
          if (!brushBeforeSecond) await _drive(owner.setBrush(color: 0xcc4411));
          peer.emitDelayedPhysicalStoreBatch();
          await _drive(
            Future.wait(<Future<void>>[first, second]),
            maxPumps: 6000,
          );
          debugPrint(
            'BLOCKED_FILL_TOOL beforeSecond=$brushBeforeSecond '
            'board=${peer.brushColor.toRadixString(16)} '
            'app=${owner.color.toRadixString(16)} '
            'ordered=${peer.orderedOperations} '
            'fills=${peer.orderedCanvasBodies.map((body) => LitchcraftProtocol.readRgb(body, 2).toRadixString(16)).toList()}',
          );
          await _until(
            () => peer.brushColor == (brushBeforeSecond ? 0x778899 : 0xcc4411),
          );
          expect(
            peer.orderedCanvasBodies.map(
              (body) => LitchcraftProtocol.readRgb(body, 2),
            ),
            <int>[0x224466, 0x778899],
          );
          expect(peer.brushColor, brushBeforeSecond ? 0x778899 : 0xcc4411);
          expect(owner.color, peer.brushColor);
          expect(
            peer.orderedOperations.where((op) => op == 9 || op == 5),
            brushBeforeSecond ? <int>[9, 9] : <int>[9, 9, 5],
            reason: 'Only a Tool chosen after both Fills may follow them',
          );
          expect(peer.paintCount, 0);
          expect(peer.effectCount, 0);
          expect(
            peer.operations.skip(operationStart),
            isNot(
              anyOf(
                contains(LitchcraftV2Protocol.frameStoreBegin),
                contains(LitchcraftV2Protocol.replaceBeginOp),
              ),
            ),
            reason: 'Fill and Tool remain incremental ordered commands',
          );
          expect(peer.frameStoreSnapshotTransactionCount, fullReads);
        },
      );
    }

    test(
      'blocked Store Canvas intents keep accepted invocation order',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final clearing = owner.clear();
        await _idle(owner);
        final filling = owner.fillAllWithColor(0x224466);
        await _idle(owner);
        expect(peer.orderedCanvasBodies, isEmpty);
        peer.emitDelayedPhysicalStoreBatch();
        await _drive(
          Future.wait(<Future<void>>[clearing, filling]),
          maxPumps: 6000,
        );
        expect(peer.orderedCanvasBodies, <List<int>>[
          <int>[31, 15, 0, 0, 0, 1],
          <int>[31, 15, 0x22, 0x44, 0x66, 0],
        ]);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
        expect(
          peer.document.frames[31].emitters.every((e) => e.color == 0x224466),
          isTrue,
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
      },
    );

    test('blocked Store Canvas releases later closed ordinary paint', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      await _drive(owner.replaceDocument(largeDraft()));
      await _drive(owner.selectAnimationFrame(31));
      peer.selectedFrame = 0;
      peer.physicalStorePaint(
        square: 7,
        mask: 15,
        color: 0x31c7ff,
        emit: false,
      );
      final clearing = owner.clear();
      await _idle(owner);
      await _drive(owner.setBrush(color: 0xcc4411, targetMask: 1));
      owner.beginStroke();
      owner.paintSquares(<int>[9]);
      owner.endStroke();
      await _idle(owner);
      peer.emitDelayedPhysicalStoreBatch();
      await _drive(clearing, maxPumps: 6000);
      await _untilBackgroundRepair(
        () => peer.document.frames[31].emitters[36].color == 0xcc4411,
      );
      expect(peer.orderedCanvasBodies, <List<int>>[
        <int>[31, 15, 0, 0, 0, 1],
      ]);
      expect(peer.document.frames[31].emitters[0].color, 0);
      expect(peer.document.frames[31].emitters[37].color, 0);
      expect(peer.document.frames[0].emitters[28].color, 0x31c7ff);
      expect(
        peer.orderedPaintSquareCounts,
        <int>[1, 0],
        reason:
            'one later ordinary square plus its existing empty stroke close',
      );
      expect(peer.effectCount, 0);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, peer.document),
        isTrue,
      );
    });

    test(
      'blocked Store Canvas and followers cancel promptly on dispose',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final clearing = owner.clear();
        final filling = owner.fillAllWithColor(0x224466);
        final cancelled = <Future<void>>[
          for (final work in <Future<void>>[clearing, filling])
            expectLater(
              work.timeout(const Duration(milliseconds: 50)),
              throwsA(isA<LitchcraftTransferCancelled>()),
            ),
        ];
        await _idle(owner);
        owner.dispose();
        await Future.wait(cancelled);
        expect(peer.orderedCanvasBodies, isEmpty);
      },
    );

    test('blocked Store Canvas Undo waits every earlier authority gap', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      final draft = LitchcraftDocumentSession(largeDraft())
        ..applyEffect(
          square: 9,
          mask: 5,
          effect: LitchcraftEffectStyle.breathe,
          effectDurationMs: 1600,
        );
      await _drive(owner.replaceDocument(draft.document));
      await _drive(owner.selectAnimationFrame(31));
      peer.selectedFrame = 0;
      peer.physicalStorePaint(
        square: 7,
        mask: 15,
        color: 0x31c7ff,
        emit: false,
      );
      final first = owner.clear();
      await _idle(owner);
      await _drive(owner.selectAnimationFrame(0));
      final second = owner.fillAllWithColor(0x224466);
      await _idle(owner);
      var undoCompleted = false;
      final undo = owner.undo().then((_) => undoCompleted = true);
      LitchcraftAnimationDocument? beforeSecond;
      peer.beforeOrderedTimeline = () async {
        if (peer.orderedCanvasBodies.length != 1 || beforeSecond != null) {
          return;
        }
        // First Canvas was confirmed on frame31. The successor's captured-frame
        // seek exposes a later physical revision before its Fill can be guarded.
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0x77aabb,
          emit: false,
        );
        beforeSecond = peer.document;
      };
      peer.emitDelayedPhysicalStoreBatch();
      await _drive(first, maxPumps: 6000);
      await _idle(owner);
      expect(beforeSecond, isNotNull);
      expect(peer.orderedCanvasBodies, hasLength(1));
      debugPrint(
        'CANVAS_TWO_GAPS first-accepted selected=${peer.selectedFrame} '
        'liveR=${peer.frameStoreLiveRevision} canvasCount=${peer.orderedCanvasBodies.length} '
        'undoCompleted=$undoCompleted next=release-second-physical-batch',
      );
      expect(
        undoCompleted,
        isFalse,
        reason: 'Undo must still await the second earlier Canvas',
      );
      expect(peer.orderedOperations, isNot(contains(6)));
      peer.emitDelayedPhysicalStoreBatch();
      await _untilBackgroundRepair(() => peer.orderedOperations.contains(6));
      peer.persistStore();
      await _untilBackgroundRepair(() => undoCompleted);
      await _drive(Future.wait(<Future<void>>[second, undo]), maxPumps: 6000);
      expect(peer.orderedOperations.where((op) => op == 9 || op == 6), <int>[
        9,
        9,
        6,
      ]);
      expect(sameLitchcraftDocument(peer.document, beforeSecond!), isTrue);
      debugPrint(
        'CANVAS_TWO_GAPS undo-complete liveR=${peer.frameStoreLiveRevision} '
        'operations=${peer.orderedOperations.where((op) => op == 9 || op == 6).toList()} '
        'document=second-gap-preimage next=save-and-confirm',
      );
      peer.persistStore();
      await _untilBackgroundRepair(
        () => sameLitchcraftDocument(
          owner.documentSession.document,
          peer.document,
        ),
      );
      expect(peer.paintCount, 0);
      expect(peer.effectCount, 0);
    });

    test(
      'blocked Store Canvas orders the first Undo after its receipt',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(31));
        final before = peer.document.frames[31];
        peer.selectedFrame = 0;
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final clearing = owner.clear();
        await _idle(owner);
        var undoDone = false;
        final undoing = owner.undo().then<void>((_) {
          undoDone = true;
        });
        await _idle(owner);
        expect(
          peer.orderedOperations,
          isNot(contains(ConnectionSession.litchcraftUndoOpcode)),
        );
        peer.emitDelayedPhysicalStoreBatch();
        await _drive(clearing, maxPumps: 6000);
        await _untilBackgroundRepair(() => peer.orderedOperations.contains(6));
        peer.persistStore();
        await _untilBackgroundRepair(() => undoDone);
        await _drive(undoing, maxPumps: 6000);
        await _untilBackgroundRepair(
          () =>
              peer.document.frames[31].emitters.first.color ==
              before.emitters.first.color,
        );
        // This fake does not synthesize native history reverse Objects or an
        // async flash save. Publish the normal persisted root before repair.
        peer.persistStore();
        await _untilBackgroundRepair(
          () => sameLitchcraftDocument(
            owner.documentSession.document,
            peer.document,
          ),
        );
        expect(peer.orderedCanvasBodies, hasLength(1));
        expect(peer.orderedOperations.where((op) => op == 9 || op == 6), <int>[
          9,
          6,
        ]);
        expect(peer.document.frames[0].emitters[28].color, 0x31c7ff);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    test(
      'blocked Store Canvas Fill preserves a newer brush selection',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.setBrush(color: 0x224466, targetMask: 5));
        peer.physicalStorePaint(
          square: 7,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final filling = owner.fillAll();
        await _idle(owner);
        await _drive(owner.setBrush(color: 0xcc4411, targetMask: 1));
        peer.emitDelayedPhysicalStoreBatch();
        await _drive(filling, maxPumps: 6000);
        await _idle(owner);
        expect(peer.orderedCanvasBodies.single, <int>[
          0,
          5,
          0x22,
          0x44,
          0x66,
          0,
        ]);
        expect(peer.brushColor, 0xcc4411);
        expect(peer.brushMask, 1);
        expect(owner.color, 0xcc4411);
        expect(owner.targetMask, 1);
        expect(peer.paintCount, 0);
        expect(peer.effectCount, 0);
      },
    );

    test('blocked Store Clear cancels when the user leaves', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      await _drive(owner.replaceDocument(largeDraft()));
      peer.physicalStorePaint(
        square: 7,
        mask: 15,
        color: 0x31c7ff,
        emit: false,
      );
      final clear = owner.clear();
      final cancelled = expectLater(
        clear,
        throwsA(isA<LitchcraftTransferCancelled>()),
      );
      await _idle(owner);
      await _drive(owner.leave());
      await cancelled;
    });

    test(
      'Hall edit before Paint ACK stays in the confirmed Store document',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        var interrupted = false;
        peer.afterOrderedPaintBeforeCheckpoint = () async {
          if (interrupted) return;
          interrupted = true;
          peer.physicalStorePaint(square: 8, mask: 15, color: 0x31c7ff);
        };
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        expect(interrupted, isTrue);
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          owner.documentSession.document.frames.first.emitters[28].color,
          owner.color,
        );
        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          0x31c7ff,
        );
        expect(peer.document.frames.first.emitters[32].color, 0x31c7ff);
      },
    );

    test(
      'known resident artwork switches without retransmitting its frame body',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final document = largeDraft();
        await _drive(owner.replaceDocument(document));
        final residentGeneration = peer.frameStoreGeneration;
        peer.operations.clear();
        peer.effectClockInspectRequests.clear();

        await _drive(owner.replaceDocument(document));

        expect(owner.documentSession.document, same(document));
        expect(peer.frameStoreGeneration, residentGeneration);
        expect(
          peer.operations.where(
            (operation) => operation != LitchcraftV2Protocol.inspect,
          ),
          equals(<int>[LitchcraftV2Protocol.frameStoreSelect]),
          reason:
              'an exact resident uses one Select; only source-clock sampling '
              'may follow in the background',
        );
        _expectAtMostOneSourceClockInspect(peer);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreBegin)),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreChunk)),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );
      },
    );

    for (final frameCount in <int>[1, 32]) {
      test(
        'opening artwork stops both players before Store body frameCount=$frameCount',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await _drive(owner.toggleAnimationPlayback());
          expect(peer.frameStorePlaying, isTrue);
          final selected = LitchcraftAnimationDocument(
            frames: List.generate(
              frameCount,
              (index) => LitchcraftAnimationFrame(
                emitters: staticLitchcraftDocument(
                  LitchcraftCanvas.blank().paint([index % 64], 15, 0x44cc55),
                ).frames.single.emitters,
                durationMs: 100 + index,
              ),
            ),
            topology: peer.document.topology,
            favoriteColors: peer.document.favoriteColors,
            playbackOnEntry: true,
          );
          final entered = Completer<void>();
          final release = Completer<void>();
          peer.beforeFrameStoreChunk = () async {
            peer.beforeFrameStoreChunk = null;
            entered.complete();
            await release.future;
          };
          final delivery = owner.replaceDocument(selected);
          try {
            expect(owner.documentSession.document, same(selected));
            expect(owner.animationPlaying, isFalse);
            await _until(() => entered.isCompleted);
            expect(
              peer.frameStorePlaying,
              isFalse,
              reason: 'Pause must be acknowledged before staging artwork',
            );
            expect(owner.animationSelectedFrameIndex, 0);
            expect(owner.animationPlaying, isFalse);
          } finally {
            release.complete();
            await _drive(delivery);
          }
          expect(sameLitchcraftDocument(peer.document, selected), isTrue);
          expect(peer.frameStorePlaying, isFalse);
          expect(
            owner.animationPlaying,
            isFalse,
            reason: 'Opening artwork never resumes playback automatically',
          );
        },
      );
    }

    test(
      'opening static artwork while Store plays retires the animation authority',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final seed = largeDraft();
        final animation = LitchcraftAnimationDocument(
          frames: List<LitchcraftAnimationFrame>.filled(256, seed.frames.first),
          topology: seed.topology,
          favoriteColors: seed.favoriteColors,
          playbackOnEntry: true,
          sparkleSeed: seed.sparkleSeed,
        );
        await _drive(owner.replaceDocument(animation));
        await _drive(owner.toggleAnimationPlayback());
        expect(peer.frameStorePlaying, isTrue);
        final still = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x44cc55),
        );
        peer.operations.clear();

        final switching = owner.replaceDocument(still);
        expect(owner.documentSession.document, same(still));
        expect(owner.animationPlaying, isFalse);
        await _drive(switching);

        expect(
          peer.operations,
          contains(LitchcraftV2Protocol.frameStoreCommit),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.replaceBeginOp)),
        );
        expect(sameLitchcraftDocument(peer.document, still), isTrue);
        expect(peer.playing, isFalse);
      },
    );

    test(
      'rejected still upload retains old Store artwork after pausing playback',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final animation = largeDraft(playbackOnEntry: true);
        await _drive(owner.replaceDocument(animation));
        await _drive(owner.toggleAnimationPlayback());
        expect(peer.frameStorePlaying, isTrue);
        final priorGeneration = peer.frameStoreGeneration;
        final priorRoot = peer.frameStoreSelectedRoot;
        final priorDocument = peer.document;
        final still = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x44cc55),
        );
        peer.rejectFrameStoreCommit = true;
        peer.operations.clear();

        await expectLater(
          owner.replaceDocument(still),
          throwsA(isA<LitchcraftRejected>()),
        );

        expect(peer.operations, contains(LitchcraftV2Protocol.frameStoreBegin));
        expect(
          peer.operations,
          contains(LitchcraftV2Protocol.frameStoreCommit),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.replaceBeginOp)),
        );
        expect(peer.frameStoreGeneration, priorGeneration);
        expect(peer.frameStoreSelectedRoot, priorRoot);
        expect(sameLitchcraftDocument(peer.document, priorDocument), isTrue);
        expect(
          peer.frameStorePlaying,
          isFalse,
          reason: 'A rejected source load must not resume old playback',
        );
        expect(
          sameLitchcraftDocument(owner.documentSession.document, still),
          isTrue,
        );
        peer.orderedOperations.clear();
        await owner.toggleAnimationPlayback();
        expect(
          peer.orderedOperations,
          isNot(contains(ConnectionSession.litchcraftTimelineOpcode)),
          reason: 'the rejected local still cannot control the old board root',
        );
        peer.orderedOperations.clear();
        owner.paintSquares([7]);
        owner.endStroke();
        await pumpEventQueue(times: 20);
        expect(
          peer.orderedOperations,
          isNot(contains(ConnectionSession.litchcraftStoreGuardOpcode)),
          reason: 'the local still cannot guard and paint the old Store root',
        );
        expect(peer.orderedOperations, isNot(contains(3)));
        expect(sameLitchcraftDocument(peer.document, priorDocument), isTrue);
        expect(
          owner.documentSession.document.frames.single.emitters[7 * 4].color,
          owner.color,
          reason: 'the rejected artwork keeps the local paint intent',
        );
        await owner.undo();
        expect(owner.canvas.colors[7 * 4], 0);
        await owner.redo();
        expect(owner.canvas.colors[7 * 4], owner.color);
        final oldBoardWrites = peer.requests.length;
        final oldBulkWrites = peer.bulkFrames.length;
        await owner.fillAll();
        await owner.clear();
        await owner.eraseDisplayedFrameByPhone();
        expect(peer.requests.length, oldBoardWrites);
        expect(peer.bulkFrames.length, oldBulkWrites);
        expect(sameLitchcraftDocument(peer.document, priorDocument), isTrue);
        expect(owner.canvas.hasMarks, isFalse);
        peer.physicalStorePaint(square: 9, mask: 15, color: 0x55aaff);
        await pumpEventQueue(times: 20);
        expect(owner.canvas.colors[9 * 4], 0);
        expect(
          owner.documentSession.document.frames.single.emitters[9 * 4].color,
          0,
        );
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.documentSession.hasLocalIntent, isTrue);
        peer.persistStore();
        await _drive(owner.prepareConflictBoardPreview());
        expect(
          owner.conflictBoardPreview!.colors[9 * 4],
          0x55aaff,
          reason:
              'the unresolved root is observed through a fresh verified preview',
        );
        expect(
          owner.canvas.colors[9 * 4],
          0,
          reason:
              'the native paint must not select or alter the rejected draft',
        );
      },
    );

    test(
      'paint during Store handoff queues a frame delta after one Commit',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final animation = LitchcraftAnimationDocument(
          frames: LitchcraftBouncingBallsPreset.decode().frames,
          topology: peer.document.topology,
          favoriteColors: peer.document.favoriteColors,
        );
        final chunkEntered = Completer<void>();
        final releaseChunk = Completer<void>();
        peer.beforeFrameStoreChunk = () async {
          peer.beforeFrameStoreChunk = null;
          chunkEntered.complete();
          await releaseChunk.future;
        };

        peer.operations.clear();
        peer.orderedOperations.clear();
        peer.orderedPaintBodies.clear();
        final delivery = owner.replaceDocument(animation);
        await chunkEntered.future;
        owner.paintSquares([7]);
        owner.endStroke();
        final intended = owner.documentSession.document;
        expect(intended.frames.first.emitters[7 * 4].color, owner.color);
        expect(sameLitchcraftDocument(intended, animation), isFalse);
        final queuedUndo = owner.undo();
        expect(
          sameLitchcraftDocument(owner.documentSession.document, animation),
          isTrue,
        );
        await owner.toggleAnimationPlayback();
        expect(owner.animationPlaybackPending, isTrue);
        final catchUpEntered = Completer<void>();
        final releaseCatchUp = Completer<void>();
        peer.beforeOrderedPaint = () async {
          peer.beforeOrderedPaint = null;
          catchUpEntered.complete();
          await releaseCatchUp.future;
        };
        expect(peer.orderedPaintBodies, isEmpty);
        releaseChunk.complete();
        await _drive(delivery);
        await _until(() => catchUpEntered.isCompleted);
        expect(sameLitchcraftDocument(peer.document, animation), isTrue);
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(owner.fullAnimationSyncDirection, isNull);
        releaseCatchUp.complete();
        await _drive(queuedUndo);
        await _until(() => peer.frameStorePlaying);
        await _idle(owner);
        expect(sameLitchcraftDocument(peer.document, animation), isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, animation),
          isTrue,
        );
        expect(owner.animationPlaying, isTrue);
        expect(
          peer.orderedOperations.where(
            (op) => op == ConnectionSession.litchcraftUndoOpcode,
          ),
          hasLength(1),
        );
        await _drive(owner.redo());
        await _untilBackgroundRepair(
          () => sameLitchcraftDocument(peer.document, intended),
        );
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(peer.document.frames.length, 256);
        await _idle(owner);
        expect(
          peer.orderedPaintBodies.where((body) => body.length > 8),
          hasLength(1),
        );
        expect(peer.orderedPaintBodies.every((body) => body[0] == 0), isTrue);
        expect(
          peer.orderedOperations.where(
            (op) => op == ConnectionSession.litchcraftRedoOpcode,
          ),
          hasLength(1),
        );
        expect(
          peer.operations.where(
            (op) => op == LitchcraftV2Protocol.frameStoreBegin,
          ),
          hasLength(1),
        );
        expect(
          peer.operations.where(
            (op) => op == LitchcraftV2Protocol.frameStoreCommit,
          ),
          hasLength(1),
        );
        expect(
          sameLitchcraftDocument(
            owner.documentSession.boardDocument!,
            intended,
          ),
          isTrue,
        );
      },
    );

    test(
      'failed queued Store delta preserves the draft through reconnect',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final still = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x44cc55),
        );
        final chunkEntered = Completer<void>();
        final releaseChunk = Completer<void>();
        peer.beforeFrameStoreChunk = () async {
          peer.beforeFrameStoreChunk = null;
          chunkEntered.complete();
          await releaseChunk.future;
        };
        peer.beforeOrderedPaint = () async {
          peer.connection(false);
          throw StateError('controlled queued Paint write failure');
        };

        final delivery = owner.replaceDocument(still);
        await chunkEntered.future;
        owner.paintSquares([7]);
        owner.endStroke();
        final intended = owner.documentSession.document;
        releaseChunk.complete();
        await _drive(delivery);
        await _until(() => !peer.connected);
        await _idle(owner);
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(sameLitchcraftDocument(peer.document, still), isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, intended),
          isTrue,
        );

        peer.beforeOrderedPaint = null;
        await peer.retireSharedConnection();
        peer.connection(true);
        await _drive(owner.settleModeResolution());
        if (owner.connectionRecoveryRequired) {
          await _drive(owner.retryConnection());
        }
        await _untilBackgroundRepair(
          () => sameLitchcraftDocument(peer.document, intended),
        );
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(sameLitchcraftDocument(peer.document, intended), isTrue);
      },
    );

    test(
      'frame selection during Store upload survives Commit and reaches board',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final edit = LitchcraftDocumentSession(largeDraft())
          ..paintSelectedFrame(squares: [11], mask: 15, color: 0x44bbcc);
        final selected = edit.document;
        final chunkEntered = Completer<void>();
        final releaseChunk = Completer<void>();
        peer.beforeFrameStoreChunk = () async {
          peer.beforeFrameStoreChunk = null;
          chunkEntered.complete();
          await releaseChunk.future;
        };

        final pendingRoot = Completer<void>();
        peer.deferredFrameStoreRoot = pendingRoot;
        peer.orderedPaintBodies.clear();
        final delivery = owner.replaceDocument(selected);
        await chunkEntered.future;
        await owner.selectAnimationFrame(2);
        expect(owner.documentSession.selectedFrameIndex, 2);
        owner.paintSquares([6]);
        owner.endStroke();
        final firstStroke = owner.documentSession.document;
        owner.paintSquares([7]);
        final intended = owner.documentSession.document;
        releaseChunk.complete();
        await _until(() => peer.frameStoreCommitAcceptedPending);
        await Future<void>.delayed(const Duration(milliseconds: 800));
        expect(peer.orderedPaintBodies, isEmpty);
        expect(owner.storeReplacementUnresolved, isTrue);
        pendingRoot.complete();
        await _drive(delivery);
        await _untilBackgroundRepair(
          () => sameLitchcraftDocument(peer.document, intended),
        );
        await _until(() => peer.selectedFrame == 2);
        expect(owner.documentSession.selectedFrameIndex, 2);
        owner.endStroke();
        await _idle(owner);
        final paints = peer.orderedPaintBodies
            .where((body) => body.length > 8)
            .toList();
        expect(paints, hasLength(2));
        expect(paints.map((body) => body.sublist(8)), [
          [6],
          [7],
        ]);
        expect(paints[0].sublist(1, 3), isNot(paints[1].sublist(1, 3)));
        expect(peer.orderedPaintBodies.every((body) => body[0] == 2), isTrue);
        await _drive(owner.undo());
        expect(sameLitchcraftDocument(peer.document, firstStroke), isTrue);
        await _drive(owner.redo());
        expect(sameLitchcraftDocument(peer.document, intended), isTrue);

        // A separate selected-frame handoff has no stroke predecessor to
        // incidentally keep Play behind the selection's ordered receipt.
        final next = LitchcraftDocumentSession(intended)
          ..paintSelectedFrame(squares: [13], mask: 15, color: 0x55aaff);
        final nextChunk = Completer<void>();
        final releaseNextChunk = Completer<void>();
        peer.deferredFrameStoreRoot = null;
        peer.beforeFrameStoreChunk = () async {
          peer.beforeFrameStoreChunk = null;
          nextChunk.complete();
          await releaseNextChunk.future;
        };
        peer.orderedPaintBodies.clear();
        final nextDelivery = owner.replaceDocument(next.document);
        await nextChunk.future;
        await owner.selectAnimationFrame(2);
        await owner.toggleAnimationPlayback();
        releaseNextChunk.complete();
        await _drive(nextDelivery);
        await _until(() => peer.frameStorePlaying);
        await _idle(owner);
        expect(peer.selectedFrame, 2);
        expect(owner.animationPlaying, isTrue);
        expect(peer.frameStorePlaying, isTrue);
        expect(peer.orderedPaintBodies, isEmpty);
        expect(sameLitchcraftDocument(peer.document, next.document), isTrue);
      },
    );

    test(
      'rejected newer Store cannot select a frame on an older pending root',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        final pendingRoot = Completer<void>();
        peer.deferredFrameStoreRoot = pendingRoot;
        final older = largeDraft();
        final first = owner.replaceDocument(older);
        unawaited(first.catchError((Object _) {}));
        await _until(() => peer.frameStoreCommitAcceptedPending);
        await Future<void>.delayed(const Duration(milliseconds: 800));
        final edit = LitchcraftDocumentSession(largeDraft())
          ..paintSelectedFrame(squares: [11], mask: 15, color: 0x44bbcc);
        final latest = owner.replaceDocument(edit.document);
        unawaited(latest.catchError((Object _) {}));
        final selection = owner.selectAnimationFrame(2);
        peer.rejectFrameStoreCommit = true;
        pendingRoot.complete();
        try {
          await _drive(first);
        } on LitchcraftTransferCancelled {
          // The newer local artwork retired the old caller.
        }
        await expectLater(latest, throwsA(isA<LitchcraftRejected>()));
        await _drive(selection);

        expect(
          peer.orderedOperations,
          isNot(contains(ConnectionSession.litchcraftTimelineOpcode)),
        );
        expect(sameLitchcraftDocument(peer.document, older), isTrue);
        expect(owner.documentSession.selectedFrameIndex, 2);
      },
      timeout: const Timeout(Duration(seconds: 15)),
    );

    test(
      'newer artwork waits for an accepted pending Store root',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        final pendingRoot = Completer<void>();
        peer.deferredFrameStoreRoot = pendingRoot;
        final first = owner.replaceDocument(largeDraft());
        unawaited(first.catchError((Object _) {}));
        await _until(() => peer.frameStoreCommitAcceptedPending);
        await Future<void>.delayed(const Duration(milliseconds: 800));
        final latestDocument = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([12], 15, 0x45ccee),
        );
        peer.operations.clear();

        final latest = owner.replaceDocument(latestDocument);
        expect(owner.documentSession.document, same(latestDocument));
        await pumpEventQueue(times: 5);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreBegin)),
          reason: 'new Begin must wait for the accepted old root',
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSelect)),
        );

        pendingRoot.complete();
        try {
          await _drive(first);
        } on LitchcraftTransferCancelled {
          // The newer local selection retires the older caller, even if the
          // board had already accepted its Commit.
        }
        await _drive(latest);
        expect(sameLitchcraftDocument(peer.document, latestDocument), isTrue);
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            latestDocument,
          ),
          isTrue,
        );
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(owner.storePlaybackFollowsLocal, isTrue);
      },
      timeout: const Timeout(Duration(seconds: 15)),
    );

    test(
      'superseded artwork cannot select a resident after delayed listing',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final older = largeDraft();
        final newer = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([21], 15, 0x66cc44),
        );
        final body = _frameStoreBody(older);
        peer._frameStoreResidents.add(
          _TestFrameResident(
            rootSlot: 1,
            generation: 4,
            document: older,
            body: body,
          ),
        );
        peer.frameStoreGeneration = 4;
        peer.frameStoreSelectedRoot = 1;
        peer.frameStoreActive = true;
        peer.frameStoreHasArtwork = true;
        peer.frameStoreFrameCount = older.frames.length;
        // Current Inspect carries the resident listing. A genuinely newer
        // authority invalidates the entry handoff before the fresh decision.
        final listingEntered = Completer<void>();
        final releaseListing = Completer<void>();
        peer.beforeFrameStoreInspect = () async {
          peer.beforeFrameStoreInspect = null;
          listingEntered.complete();
          await releaseListing.future;
        };
        peer.publishStoreAuthority();

        final first = owner.replaceDocument(older);
        unawaited(first.catchError((Object _) {}));
        for (var pump = 0; pump < 600 && !listingEntered.isCompleted; pump++) {
          await Future<void>.delayed(Duration.zero);
        }
        expect(
          listingEntered.isCompleted,
          isTrue,
          reason: 'a fresh resident listing should be held: ${peer.operations}',
        );
        final latest = owner.replaceDocument(newer);
        releaseListing.complete();
        try {
          await _drive(first);
        } on LitchcraftTransferCancelled {
          // A newer local choice cancels the resident switch before admission.
        }
        await _drive(latest);

        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSelect)),
          reason: 'the older resident was never chosen on the board',
        );
        expect(sameLitchcraftDocument(peer.document, newer), isTrue);
      },
    );

    test(
      'newer document retires a missing precommit Store checkpoint',
      () async {
        peer.connectionSessionSupported = true;
        peer.forceMinimumConnectionPacket = true;
        peer.fragmentStoreReceipts = true;
        await _drive(owner.enter(), maxPumps: 10000);
        final checkpoint = Completer<void>();
        peer.dropFrameStoreCheckpointReceipt = true;
        peer.onFrameStoreCheckpoint = () {
          if (!checkpoint.isCompleted) checkpoint.complete();
        };
        final seed = largeDraft();
        final first = owner.replaceDocument(
          LitchcraftAnimationDocument(
            frames: List.generate(
              256,
              (index) => LitchcraftAnimationFrame(
                emitters: seed.frames.first.emitters,
                durationMs: 50 + index,
              ),
            ),
            topology: seed.topology,
            favoriteColors: seed.favoriteColors,
          ),
        );
        final cancelled = expectLater(
          first,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await checkpoint.future.timeout(const Duration(seconds: 3));
        final oldTransfer = peer.frameStoreTransfer;
        peer.dropFrameStoreCheckpointReceipt = false;
        final latestDocument = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([5], 15, 0x234567),
        );
        final latest = owner.replaceDocument(latestDocument);
        await cancelled.timeout(const Duration(milliseconds: 300));
        await _drive(latest, maxPumps: 10000);
        peer.advancedEvent(
          LitchcraftV2Protocol.frameStoreCheckpointReceipt,
          oldTransfer,
          [0, 0, 0, 0, 0],
        );
        await Future<void>.delayed(Duration.zero);
        expect(sameLitchcraftDocument(peer.document, latestDocument), isTrue);
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            latestDocument,
          ),
          isTrue,
        );
      },
    );

    for (final retireByDispose in [false, true]) {
      test(
        'idle same-link Store precommit retirement dispose=$retireByDispose',
        () async {
          peer.connectionSessionSupported = true;
          peer.forceMinimumConnectionPacket = true;
          peer.fragmentStoreReceipts = true;
          await _drive(owner.enter());
          final checkpoint = Completer<void>();
          peer.dropFrameStoreCheckpointReceipt = true;
          peer.onFrameStoreCheckpoint = () {
            if (!checkpoint.isCompleted) checkpoint.complete();
          };
          final seed = largeDraft();
          final transfer = owner.replaceDocument(
            LitchcraftAnimationDocument(
              frames: List.generate(
                256,
                (index) => LitchcraftAnimationFrame(
                  emitters: seed.frames.first.emitters,
                  durationMs: 50 + index,
                ),
              ),
              topology: seed.topology,
              favoriteColors: seed.favoriteColors,
            ),
          );
          final cancelled = expectLater(
            transfer,
            throwsA(isA<LitchcraftTransferCancelled>()),
          );
          await checkpoint.future;
          final oldTransfer = peer.frameStoreTransfer;
          final commits = peer.operations
              .where((op) => op == LitchcraftV2Protocol.frameStoreCommit)
              .length;
          if (retireByDispose) {
            owner.dispose();
          } else {
            await _drive(owner.leave());
          }
          await cancelled.timeout(const Duration(milliseconds: 300));
          expect(peer.connected, isTrue);
          expect(
            peer.operations
                .where((op) => op == LitchcraftV2Protocol.frameStoreCommit)
                .length,
            commits,
          );
          final confirmedBeforeLate = owner.documentSession.boardDocument;
          peer.advancedEvent(
            LitchcraftV2Protocol.frameStoreCheckpointReceipt,
            oldTransfer,
            [0, 0, 0, 0, 0],
          );
          await Future<void>.delayed(Duration.zero);
          expect(
            owner.documentSession.boardDocument,
            same(confirmedBeforeLate),
          );
          if (!retireByDispose) {
            peer.dropFrameStoreCheckpointReceipt = false;
            await _drive(owner.enter(), maxPumps: 12000);
            await _drive(
              owner.replaceDocument(
                staticLitchcraftDocument(
                  LitchcraftCanvas.blank().paint([6], 15, 0x345678),
                ),
              ),
              maxPumps: 12000,
            );
            expect(owner.status, LitchcraftStatus.live);
          }
        },
      );
    }

    test(
      'newer static selection still retires Store while first switch is pending',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final seed = largeDraft();
        await _drive(
          owner.replaceDocument(
            LitchcraftAnimationDocument(
              frames: List<LitchcraftAnimationFrame>.filled(
                256,
                seed.frames.first,
              ),
              topology: seed.topology,
              favoriteColors: seed.favoriteColors,
              playbackOnEntry: true,
              sparkleSeed: seed.sparkleSeed,
            ),
          ),
        );
        await _drive(owner.toggleAnimationPlayback());
        expect(peer.frameStorePlaying, isTrue);
        final previousBoardDocument = peer.document;
        final firstStill = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0], 15, 0x44cc55),
        );
        final newestStill = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([1], 15, 0xcc4455),
        );
        final firstChunkEntered = Completer<void>();
        final releaseFirstChunk = Completer<void>();
        final committedDocuments = <LitchcraftAnimationDocument>[];
        peer.operations.clear();
        peer.beforeFrameStoreCommit = () async {
          committedDocuments.add(peer.document);
        };
        peer.beforeFrameStoreChunk = () async {
          if (!firstChunkEntered.isCompleted) {
            firstChunkEntered.complete();
            await releaseFirstChunk.future;
          }
        };

        final firstSwitch = owner.replaceDocument(firstStill);
        unawaited(firstSwitch.catchError((Object _) {}));
        await firstChunkEntered.future;
        final newerSwitch = owner.replaceDocument(newestStill);
        expect(owner.documentSession.document, same(newestStill));
        expect(owner.animationPlaying, isFalse);
        expect(
          sameLitchcraftDocument(peer.document, previousBoardDocument),
          isTrue,
          reason: 'a held inactive transfer cannot change the displayed root',
        );
        releaseFirstChunk.complete();
        await expectLater(
          firstSwitch,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await _drive(newerSwitch);

        expect(
          sameLitchcraftDocument(peer.document, newestStill),
          isTrue,
          reason:
              'operations=${peer.operations} status=${owner.status} '
              'store=${peer.frameStorePlaying} revision=${peer.revision}',
        );
        expect(peer.frameStorePlaying, isFalse);
        expect(peer.playing, isFalse);
        expect(
          committedDocuments.any(
            (document) => sameLitchcraftDocument(document, firstStill),
          ),
          isFalse,
          reason: 'a superseded draft must never reach the displayed root',
        );
      },
    );

    test(
      'two distinct 256-frame residents alternate A-B-A-B with no replacement',
      () async {
        peer.connectionSessionSupported = true;
        peer.fragmentStoreReceipts = true;
        await _drive(owner.enter());
        final seed = largeDraft();
        final first = LitchcraftAnimationDocument(
          frames: List<LitchcraftAnimationFrame>.generate(
            256,
            (index) => LitchcraftAnimationFrame(
              emitters: seed.frames.first.emitters,
              durationMs: 50 + index,
            ),
          ),
          topology: seed.topology,
          favoriteColors: seed.favoriteColors,
          playbackOnEntry: true,
          sparkleSeed: seed.sparkleSeed,
        );
        final second = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            LitchcraftAnimationFrame(
              emitters: first.frames.first.emitters,
              durationMs: 137,
            ),
            ...first.frames.skip(1),
          ],
          topology: first.topology,
          favoriteColors: first.favoriteColors,
          playbackOnEntry: first.playbackOnEntry,
          sparkleSeed: first.sparkleSeed,
        );
        await _drive(owner.replaceDocument(first));
        final firstGeneration = peer.frameStoreGeneration;
        await _drive(owner.replaceDocument(second));
        final secondGeneration = peer.frameStoreGeneration;
        expect(secondGeneration, greaterThan(firstGeneration));

        for (final target in <LitchcraftAnimationDocument>[first, second]) {
          peer.operations.clear();
          peer.effectClockInspectRequests.clear();
          await _drive(owner.replaceDocument(target));
          expect(
            sameLitchcraftDocument(peer.document, target),
            isTrue,
            reason: 'the selected resident becomes board authority',
          );
          expect(
            peer.operations.where(
              (operation) => operation != LitchcraftV2Protocol.inspect,
            ),
            equals(<int>[LitchcraftV2Protocol.frameStoreSelect]),
            reason:
                'each A/B switch uses one Select without resident discovery '
                'or replacement; source-clock sampling may follow',
          );
          _expectAtMostOneSourceClockInspect(peer);
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.frameStoreBegin)),
          );
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.frameStoreChunk)),
          );
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
          );
        }
        expect(peer.frameStoreGeneration, secondGeneration);
      },
    );

    test(
      'passive entry never selects an unchosen older Store resident',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = peer.document;
        final edit = LitchcraftDocumentSession(first)
          ..paintSelectedFrame(
            squares: const <int>[7],
            mask: 15,
            color: 0xabcdef,
          );
        final second = edit.document;
        await _drive(owner.replaceDocument(first));
        await _drive(owner.replaceDocument(second));
        expect(sameLitchcraftDocument(peer.document, second), isTrue);

        owner.dispose();
        owner = LitchcraftController(peer)..restoreDocument(first);
        peer.operations.clear();
        await _drive(owner.enter());

        expect(owner.status, LitchcraftStatus.conflict);
        expect(sameLitchcraftDocument(peer.document, second), isTrue);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSelect)),
          reason: 'Only an explicit artwork choice may switch the active root.',
        );
      },
    );

    test('minimum ATT payload transfers Store and selects a resident', () async {
      peer.connectionSessionSupported = true;
      peer.forceMinimumConnectionPacket = true;
      peer.fragmentStoreReceipts = true;
      await _drive(owner.enter());
      final document = largeDraft();
      await _drive(owner.replaceDocument(document), maxPumps: 6000);
      expect(peer.frameStoreGeneration, greaterThan(0));
      expect(sameLitchcraftDocument(peer.document, document), isTrue);
      peer.operations.clear();
      await _drive(
        owner.replaceDocument(
          LitchcraftAnimationDocument(
            frames: <LitchcraftAnimationFrame>[
              document.frames.first,
              ...document.frames.skip(1),
              document.frames.first,
            ],
            topology: document.topology,
            favoriteColors: document.favoriteColors,
            playbackOnEntry: document.playbackOnEntry,
            sparkleSeed: document.sparkleSeed,
          ),
        ),
      );
      peer.operations.clear();
      peer.effectClockInspectRequests.clear();
      await _drive(owner.replaceDocument(document));
      await _idle(owner);
      expect(
        peer.operations,
        <int>[
          LitchcraftV2Protocol.frameStoreSelect,
          LitchcraftV2Protocol.inspect,
        ],
        reason:
            'Resident selection reanchors its independent effect clock once',
      );
      expect(
        peer.effectClockInspectRequests,
        hasLength(1),
        reason:
            'The source refresh opts into the paired effect sample with payload [1]',
      );
      expect(peer.effectClockInspectRequests.single, greaterThan(0));
      expect(sameLitchcraftDocument(peer.document, document), isTrue);
    });

    test(
      'stale resident identity repairs immediately through authoritative upload',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = largeDraft();
        final second = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            LitchcraftAnimationFrame(
              emitters: first.frames.first.emitters,
              durationMs: first.frames.first.durationMs + 1,
            ),
            ...first.frames.skip(1),
          ],
          topology: first.topology,
          favoriteColors: first.favoriteColors,
          playbackOnEntry: first.playbackOnEntry,
          sparkleSeed: first.sparkleSeed,
        );
        await _drive(owner.replaceDocument(first));
        await _drive(owner.replaceDocument(second));
        peer._frameStoreResidents.removeWhere(
          (resident) => sameLitchcraftDocument(resident.document, first),
        );
        peer.operations.clear();

        await _drive(owner.replaceDocument(first));

        expect(sameLitchcraftDocument(peer.document, first), isTrue);
        expect(
          peer.operations.take(3),
          equals(<int>[
            LitchcraftV2Protocol.frameStoreSelect,
            LitchcraftV2Protocol.inspect,
            LitchcraftV2Protocol.frameStoreBegin,
          ]),
          reason:
              'a rejected cached Select falls straight through to one '
              'authoritative replacement without a duplicate resident read',
        );
        expect(
          peer.operations,
          containsAllInOrder(<int>[
            LitchcraftV2Protocol.frameStoreBegin,
            LitchcraftV2Protocol.frameStoreCommit,
          ]),
          reason:
              'the missing resident is restored by one upload and its receipt',
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
        );
      },
    );

    test(
      'Commit resident proof preserves cached selection without another lookup',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = largeDraft();
        final changed = LitchcraftDocumentSession(first)
          ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef);
        peer.operations.clear();
        await _drive(owner.replaceDocument(first));
        await _drive(owner.replaceDocument(changed.document));
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
        );
        final inspections = peer.frameStoreInspectCount;
        peer.operations.clear();
        await _drive(owner.replaceDocument(first));
        expect(peer.frameStoreInspectCount, inspections);
        expect(
          peer.operations,
          contains(LitchcraftV2Protocol.frameStoreSelect),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreBegin)),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
        );
        expect(
          sameLitchcraftDocument(peer.document, owner.documentSession.document),
          isTrue,
        );
      },
    );

    test(
      'Commit without resident listing invalidates overwritten cached proof',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = largeDraft();
        final second = LitchcraftDocumentSession(first)
          ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef);
        final third = LitchcraftDocumentSession(first)
          ..paintSelectedFrame(squares: [8], mask: 15, color: 0x123abc);
        await _drive(owner.replaceDocument(first));
        await _drive(owner.replaceDocument(second.document));
        peer.frameStoreCommitIncludesResidents = false;
        await _drive(owner.replaceDocument(third.document));
        expect(
          peer._frameStoreResidents.any(
            (resident) => sameLitchcraftDocument(resident.document, first),
          ),
          isFalse,
        );
        final inspections = peer.frameStoreInspectCount;
        peer.operations.clear();

        await _drive(owner.replaceDocument(first));

        expect(peer.frameStoreInspectCount, greaterThan(inspections));
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSelect)),
          reason: 'An overwritten cached identity must not be selected',
        );
        expect(peer.operations, contains(LitchcraftV2Protocol.frameStoreBegin));
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
        );
        expect(sameLitchcraftDocument(peer.document, first), isTrue);
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'resident metadata in Inspect removes the cold Residents round trip',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        peer.operations.clear();

        await _drive(owner.replaceDocument(largeDraft()), maxPumps: 6000);

        final beginIndex = peer.operations.indexOf(
          LitchcraftV2Protocol.frameStoreBegin,
        );
        expect(beginIndex, greaterThanOrEqualTo(0));
        expect(
          peer.operations.take(beginIndex),
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
          reason:
              'the capability Inspect already carries resident metadata; '
              'the upload must not wait for a second pre-transfer board read',
        );
      },
    );

    test(
      'schema-0 entry hands its one Store metadata read to the first upload',
      () async {
        peer.connectionSessionSupported = true;
        peer.reportedSchema = 0;
        peer.document = owner.documentSession.document;
        await _drive(owner.enter());
        expect(peer.frameStoreInspectCount, 1);
        expect(peer.frameStoreSnapshotTransactionCount, 0);

        await _drive(owner.replaceDocument(largeDraft()), maxPumps: 6000);

        final beginIndex = peer.operations.indexOf(
          LitchcraftV2Protocol.frameStoreBegin,
        );
        expect(beginIndex, greaterThanOrEqualTo(0));
        expect(
          peer.frameStoreInspectCount,
          1,
          reason: 'entry metadata remains authoritative until the first Begin',
        );
        expect(
          peer.operations.take(beginIndex),
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
        );
        expect(peer.frameStoreSnapshotTransactionCount, 0);
      },
    );

    test(
      'Balls256 bulk load keeps local frame selection responsive before Commit',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final document = LitchcraftAnimationDocument(
          frames: LitchcraftBouncingBallsPreset.decode().frames,
          topology: peer.document.topology,
          favoriteColors: peer.document.favoriteColors,
          playbackOnEntry: true,
        );
        final firstChunkEntered = Completer<void>();
        final releaseFirstChunk = Completer<void>();
        var first = true;
        peer.beforeFrameStoreChunk = () async {
          if (!first) return;
          first = false;
          firstChunkEntered.complete();
          await releaseFirstChunk.future;
        };
        peer.operations.clear();
        peer.orderedOperations.clear();

        final delivery = owner.replaceDocument(document);
        await firstChunkEntered.future;
        expect(owner.documentSession.document, same(document));
        expect(owner.usesFrameStoreDocument, isTrue);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );

        await _drive(owner.selectAnimationFrame(255));
        expect(owner.documentSession.selectedFrameIndex, 255);
        await _drive(owner.setBrush(color: 0x22cc88));
        expect(owner.color, 0x22cc88);
        expect(owner.pending, isFalse);
        expect(
          peer.orderedOperations,
          [ConnectionSession.litchcraftToolOpcode],
          reason: 'Tool selection stays available while the artwork is staged',
        );
        expect(peer.brushColor, 0x22cc88);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );

        releaseFirstChunk.complete();
        await _drive(delivery);
        expect(peer.frameStoreFrameCount, 256);
        expect(peer.frameStoreTransferRgbWire, isFalse);
        expect(peer.frameStoreBodyBytes, greaterThan(160000));
        expect(peer.frameStoreGeneration, 2);
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isTrue);
        expect(
          peer.orderedOperations,
          contains(ConnectionSession.litchcraftTimelineOpcode),
        );
      },
    );

    test('RGB Store capability compresses only the wire body', () async {
      peer.connectionSessionSupported = true;
      peer.frameStoreRgbWireCapable = true;
      await _drive(owner.enter());
      final document = LitchcraftAnimationDocument(
        frames: LitchcraftBouncingBallsPreset.decode().frames,
        topology: peer.document.topology,
        favoriteColors: peer.document.favoriteColors,
        playbackOnEntry: true,
      );

      await _drive(owner.replaceDocument(document));

      expect(peer.frameStoreTransferRgbWire, isTrue);
      expect(peer.frameStoreBodyBytes, 122791);
      expect(peer.frameStoreBody.length, 196545);
      expect(peer.frameStoreBody, _frameStoreBody(document));
      expect(peer.frameStoreFrameCount, 256);
      expect(owner.pending, isFalse);
    });

    test('playback-on-entry artwork stays paused until explicit Play', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      final paused = largeDraft();
      final document = LitchcraftAnimationDocument(
        frames: paused.frames,
        topology: paused.topology,
        favoriteColors: paused.favoriteColors,
        playbackOnEntry: true,
        sparkleSeed: paused.sparkleSeed,
      );

      await _drive(owner.replaceDocument(document));

      expect(owner.usesFrameStoreDocument, isTrue);
      expect(owner.animationPlaying, isFalse);
      expect(peer.frameStoreFrameCount, 32);
      expect(peer.frameStorePlaying, isFalse);
      expect(peer.document.playbackOnEntry, isTrue);
      await _drive(owner.toggleAnimationPlayback());
      expect(owner.animationPlaying, isTrue);
      expect(peer.frameStorePlaying, isTrue);

      peer.playing = false;
      peer.revision = peer.frameStoreGeneration - 1;
      peer.timeline();
      await _idle(owner);
      expect(
        owner.animationPlaying,
        isTrue,
        reason:
            'a delayed pre-Store paused Timeline cannot override the committed generation',
      );

      peer.revision = peer.frameStoreGeneration;
      peer.timeline();
      await _idle(owner);
      expect(
        owner.animationPlaying,
        isTrue,
        reason:
            'Legacy Timeline has no Store frame-local anchor, even at the matching generation',
      );
      peer.connectionLitchcraftState(
        sequence: peer._connectionLitchcraftStateSequence + 1,
        selectedFrame: 0,
        frameElapsedMilliseconds: 0,
        playing: false,
      );
      await _idle(owner);
      expect(
        owner.animationPlaying,
        isFalse,
        reason: 'The shared runtime State remains Store playback authority',
      );
    });

    test(
      'connected transport loss keeps last committed playback visible',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final source = largeDraft();
        final playing = LitchcraftAnimationDocument(
          frames: source.frames,
          topology: source.topology,
          favoriteColors: source.favoriteColors,
          playbackOnEntry: true,
          sparkleSeed: source.sparkleSeed,
        );
        await _drive(owner.replaceDocument(playing));
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isTrue);

        peer.beforeOrderedTimeline = () async {
          throw StateError('controlled native command transport loss');
        };
        await owner.toggleAnimationPlayback().timeout(
          const Duration(seconds: 5),
        );
        expect(peer.connected, isTrue);
        expect(peer.frameStorePlaying, isTrue);
        expect(owner.status, LitchcraftStatus.error);
        expect(owner.animationPlaying, isTrue);
      },
    );

    test(
      'retained State Query cannot begin a second clock correction',
      () async {
        owner.dispose();
        await peer.shutdown();
        var now = 0;
        peer = _AdvancedBle();
        owner = LitchcraftController(peer, monotonicMilliseconds: () => now);
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());
        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: 8,
          frameElapsedMilliseconds: 25,
          playing: true,
        );
        await Future<void>.delayed(Duration.zero);
        now = 65000;
        final beforeQuery = owner.animationElapsedMilliseconds;
        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: 8,
          frameElapsedMilliseconds: 25,
          playing: true,
        );
        await Future<void>.delayed(Duration.zero);
        expect(owner.animationElapsedMilliseconds, beforeQuery);
        now = 69000;
        expect(
          owner.animationElapsedMilliseconds,
          beforeQuery + 4000,
          reason: 'An identical retained clock sample cannot start a new slew',
        );
        expect(owner.animationPlaying, isTrue);
      },
    );

    test(
      'same-playback clock samples are rate-limited and slewed without frame jumps',
      () async {
        owner.dispose();
        await peer.shutdown();
        var now = 0;
        peer = _AdvancedBle();
        owner = LitchcraftController(peer, monotonicMilliseconds: () => now);
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final paused = largeDraft();
        final document = LitchcraftAnimationDocument(
          frames: paused.frames,
          topology: paused.topology,
          favoriteColors: paused.favoriteColors,
          playbackOnEntry: true,
          sparkleSeed: paused.sparkleSeed,
        );
        await _drive(owner.replaceDocument(document));
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isTrue);
        peer.connectionLitchcraftState(
          sequence: 10,
          selectedFrame: 8,
          frameElapsedMilliseconds: 25,
          playing: true,
        );
        await Future<void>.delayed(Duration.zero);
        final before = owner.animationElapsedMilliseconds;
        await _idle(owner);
        expect(peer.effectClockInspectRequests, isNotEmpty);
        final effectRequest = peer.effectClockInspectRequests.last;
        peer.advancedEvent(LitchcraftV2Protocol.effectClockLow, effectRequest, [
          1,
          0,
          0,
          100,
          0,
          0,
          0,
        ]);
        peer.advancedEvent(
          LitchcraftV2Protocol.effectClockHigh,
          effectRequest,
          [1, 0, 0, 0, 0, 0, 0],
        );
        await Future<void>.delayed(Duration.zero);

        for (var frame = 9; frame < 20; ++frame) {
          peer.connectionLitchcraftState(
            sequence: frame + 2,
            selectedFrame: frame,
            frameElapsedMilliseconds: 25,
            playing: true,
          );
        }
        await Future<void>.delayed(Duration.zero);
        final after = owner.animationElapsedMilliseconds;

        expect(after, greaterThanOrEqualTo(before));
        expect(
          after - before,
          lessThan(200),
          reason:
              '20 Hz board observations must not turn BLE delivery jitter into visible frame skips',
        );

        now = const Duration(minutes: 1).inMilliseconds;
        final beforeSync = owner.animationElapsedMilliseconds;
        final cycleMilliseconds = document.frames.fold<int>(
          0,
          (total, frame) => total + frame.durationMs,
        );
        final boardPhase = (beforeSync + 20) % cycleMilliseconds;
        final boardFrame = litchcraftAnimationFrameIndexAt(
          document,
          boardPhase,
        );
        final boardFrameStart = litchcraftAnimationFrameStartOrNull(
          document,
          boardFrame,
        )!;
        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: boardFrame,
          frameElapsedMilliseconds: boardPhase - boardFrameStart,
          playing: true,
        );
        await Future<void>.delayed(Duration.zero);
        expect(
          owner.animationElapsedMilliseconds,
          beforeSync,
          reason: 'a periodic phase sample must never jump the playhead',
        );

        var previousElapsed = beforeSync;
        for (var step = 1; step <= 40; step++) {
          now = const Duration(minutes: 1).inMilliseconds + step * 100;
          final elapsed = owner.animationElapsedMilliseconds;
          expect(
            elapsed - previousElapsed,
            anyOf(100, 101),
            reason:
                'a 20 ms phase correction is repaid as a 0.5% speed change, not a skipped frame',
          );
          previousElapsed = elapsed;
        }
        expect(owner.animationElapsedMilliseconds, beforeSync + 4020);
        peer.connectionLitchcraftState(
          sequence: 101,
          selectedFrame: 8,
          frameElapsedMilliseconds: 25,
          playing: true,
          transferSuspended: true,
        );
        await Future<void>.delayed(Duration.zero);
        final heldFrameClock = owner.animationElapsedMilliseconds;
        final heldEffectClock = owner.animationEffectElapsedMilliseconds;
        expect(heldEffectClock, isNotNull);
        expect(owner.animationPlaying, isTrue);
        now += 5000;
        expect(owner.animationElapsedMilliseconds, heldFrameClock);
        expect(owner.animationEffectElapsedMilliseconds, heldEffectClock);
        peer.connectionLitchcraftState(
          sequence: 102,
          selectedFrame: 8,
          frameElapsedMilliseconds: 25,
          playing: true,
        );
        await Future<void>.delayed(Duration.zero);
        expect(owner.animationElapsedMilliseconds, heldFrameClock);
        now += 25;
        expect(owner.animationElapsedMilliseconds, heldFrameClock + 25);
        if (heldEffectClock != null) {
          expect(
            owner.animationEffectElapsedMilliseconds,
            heldEffectClock + 25,
          );
        }
        peer.connectionLitchcraftState(
          sequence: 103,
          selectedFrame: 8,
          frameElapsedMilliseconds: 50,
          playing: true,
          transferSuspended: true,
        );
        await Future<void>.delayed(Duration.zero);
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isFalse);
        peer.connectionLitchcraftState(
          sequence: 104,
          selectedFrame: owner.documentSession.selectedFrameIndex,
          frameElapsedMilliseconds: 50,
          playing: false,
        );
        await Future<void>.delayed(Duration.zero);
        final explicitlyPaused = owner.animationElapsedMilliseconds;
        now += 5000;
        expect(owner.animationPlaying, isFalse);
        expect(owner.animationElapsedMilliseconds, explicitlyPaused);
      },
    );

    test(
      'leaving during a dense Store upload cancels stale frames before Commit',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final document = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            for (var index = 0; index < 32; index++)
              LitchcraftAnimationFrame(
                durationMs: 50,
                emitters: <LitchcraftAnimatedEmitter>[
                  for (var emitter = 0; emitter < 256; emitter++)
                    LitchcraftAnimatedEmitter(
                      color:
                          (((index + emitter * 11) & 0xff) << 16) |
                          (((index + emitter * 17) & 0xff) << 8) |
                          ((index + emitter * 23) & 0xff),
                    ),
                ],
              ),
          ],
          topology: peer.document.topology,
          favoriteColors: peer.document.favoriteColors,
          playbackOnEntry: true,
        );
        final firstChunkEntered = Completer<void>();
        final releaseFirstChunk = Completer<void>();
        var first = true;
        peer.beforeFrameStoreChunk = () async {
          if (!first) return;
          first = false;
          firstChunkEntered.complete();
          await releaseFirstChunk.future;
        };
        peer.operations.clear();
        peer.orderedOperations.clear();

        final delivery = owner.replaceDocument(document);
        await firstChunkEntered.future;
        final leaving = owner.leave();
        releaseFirstChunk.complete();

        await expectLater(
          delivery.timeout(const Duration(seconds: 1)),
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await _drive(leaving);
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );
        expect(
          peer.orderedOperations.where(
            (operation) => operation == LitchcraftV2Protocol.frameStoreCancel,
          ),
          hasLength(1),
        );
        expect(peer.frameStoreTransfer, 0);
        expect(owner.boardActive, isFalse);
      },
    );

    test(
      'resident switch preempts dense replacement after one Object window',
      () async {
        final trace = _EntryProbeTrace();
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = largeDraft();
        final second = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            LitchcraftAnimationFrame(
              emitters: first.frames.first.emitters,
              durationMs: first.frames.first.durationMs + 1,
            ),
            ...first.frames.skip(1),
          ],
          topology: first.topology,
          favoriteColors: first.favoriteColors,
          playbackOnEntry: first.playbackOnEntry,
          sparkleSeed: first.sparkleSeed,
        );
        await _drive(owner.replaceDocument(first));
        await _drive(owner.replaceDocument(second));
        final replacement = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            for (var index = 0; index < 32; index++)
              LitchcraftAnimationFrame(
                durationMs: 50,
                emitters: <LitchcraftAnimatedEmitter>[
                  for (var emitter = 0; emitter < 256; emitter++)
                    LitchcraftAnimatedEmitter(
                      color:
                          (((index + emitter * 11) & 0xff) << 16) |
                          (((index + emitter * 17) & 0xff) << 8) |
                          ((index + emitter * 23) & 0xff),
                    ),
                ],
              ),
          ],
          topology: first.topology,
          favoriteColors: first.favoriteColors,
          playbackOnEntry: true,
          sparkleSeed: first.sparkleSeed,
        );
        final firstChunkEntered = Completer<void>();
        final releaseFirstChunk = Completer<void>();
        var hold = true;
        peer.beforeFrameStoreChunk = () async {
          if (!hold) return;
          hold = false;
          firstChunkEntered.complete();
          await releaseFirstChunk.future;
        };
        peer.operations.clear();
        peer.orderedOperations.clear();
        peer.effectClockInspectRequests.clear();

        final traceStart = trace.records.length;
        final delivery = owner.replaceDocument(replacement);
        await firstChunkEntered.future;
        // Earlier source sampling belongs to the held replacement's trace.
        final residentOperationStart = peer.operations.length;
        final residentSampleStart = peer.effectClockInspectRequests.length;
        final switching = owner.replaceDocument(first);
        expect(
          owner.documentSession.document,
          same(first),
          reason: 'the app projects the requested resident immediately',
        );
        releaseFirstChunk.complete();

        await expectLater(
          delivery.timeout(const Duration(seconds: 1)),
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        await _drive(switching);
        expect(sameLitchcraftDocument(peer.document, first), isTrue);
        expect(
          peer.operations.where(
            (operation) => operation == LitchcraftV2Protocol.frameStoreChunk,
          ),
          hasLength(LitchcraftFrameStoreClient.maximumNativeBatchPackets),
        );
        final submissions = trace.records
            .skip(traceStart)
            .where((row) => row['event'] == 'litchcraft.object_submission')
            .toList();
        expect(
          submissions.map((row) => row['count']),
          unorderedEquals(<int>[
            LitchcraftFrameStoreClient.maximumNativeBatchPackets,
            LitchcraftFrameStoreClient.highMtuWindowChunks -
                LitchcraftFrameStoreClient.maximumNativeBatchPackets,
          ]),
          reason: 'the existing 54-Object window queues both producers',
        );
        expect(submissions.map((row) => row['request']).toSet(), hasLength(1));
        final issued = submissions.singleWhere(
          (row) =>
              row['count'] ==
              LitchcraftFrameStoreClient.maximumNativeBatchPackets,
        );
        final unissued = submissions.singleWhere(
          (row) =>
              row['count'] !=
              LitchcraftFrameStoreClient.maximumNativeBatchPackets,
        );
        expect(issued['first'], isNotNull);
        expect(issued['outcome'], 'accepted');
        expect(unissued['first'], isNull);
        expect(unissued['outcome'], 'cancelled');
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
        );
        expect(
          peer.operations
              .where((operation) => operation != LitchcraftV2Protocol.inspect)
              .last,
          LitchcraftV2Protocol.frameStoreSelect,
          reason: 'Select terminates the transfer; clock sampling may follow',
        );
        expect(
          peer.operations.where(
            (operation) => operation == LitchcraftV2Protocol.frameStoreSelect,
          ),
          hasLength(1),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreResidents)),
        );
        _expectAtMostOneSourceClockInspect(
          peer,
          operationStart: residentOperationStart,
          sampleStart: residentSampleStart,
        );
        expect(
          peer.orderedOperations.where(
            (operation) => operation == LitchcraftV2Protocol.frameStoreCancel,
          ),
          hasLength(1),
        );
      },
    );

    test(
      'physical Store paint updates the app through the reliable reverse batch',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.usesFrameStoreDocument, isTrue);
        expect(peer._connectionSession!.stateStreamToken, 0x80000001);
        expect(
          owner.documentSession.boardDocument,
          isNotNull,
          reason: 'the Store Commit receipt establishes board authority',
        );
        final observedStates = <ConnectionSessionPacket>[];
        final stateSubscription = peer._connectionSession!.states.listen(
          observedStates.add,
        );

        peer.physicalStorePaint(square: 8, mask: 15, color: 0x31c7ff);
        await _idle(owner);
        await stateSubscription.cancel();
        expect(
          observedStates.map((packet) => packet.code),
          contains(ConnectionSession.litchcraftStoreEditStateCode),
        );

        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          0x31c7ff,
          reason: 'a physical square reaches app authority without a snapshot',
        );
        final readsBefore = peer.frameStoreSnapshotTransactionCount;
        owner.beginStroke();
        owner.paintSquares([9]);
        owner.endStroke();
        await _idle(owner);
        expect(
          peer.document.frames.first.emitters[36].color,
          owner.color,
          reason:
              'the complete reverse patch and authority permit the next guarded app stroke',
        );
        expect(
          peer.frameStoreSnapshotTransactionCount,
          readsBefore,
          reason: 'a dirty Store is never read as the live frame',
        );
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _untilBackgroundRepair(
          () => peer.document.frames.first.emitters[36].color == owner.color,
        );
        await _idle(owner);
        expect(
          peer.document.frames.first.emitters[36].color,
          owner.color,
          reason: 'the accepted tap remains intact after persistence',
        );
        expect(
          peer.frameStoreSnapshotTransactionCount,
          readsBefore,
          reason: 'a complete reverse batch needs no bulk artwork repair',
        );
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'physical Store revision gap repairs from one coherent snapshot',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());

        final syncDirections = <LitchcraftFullSyncDirection>[];
        void recordSync() {
          final direction = owner.fullAnimationSyncDirection;
          if (direction != null) syncDirections.add(direction);
        }

        owner.addListener(recordSync);
        peer.physicalStorePaint(square: 7, mask: 15, color: 0x44cc55);
        await _idle(owner);
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0xff7711,
          emit: false,
        );
        peer.physicalStorePaint(square: 9, mask: 15, color: 0x1188ee);
        peer.persistStore();
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _idle(owner);

        final frame = owner.documentSession.document.frames.first;
        expect(
          frame.emitters[32].color,
          0xff7711,
          reason: 'the missed physical revision is restored by Store repair',
        );
        expect(frame.emitters[36].color, 0x1188ee);
        expect(owner.status, LitchcraftStatus.live);
        owner.removeListener(recordSync);
        expect(syncDirections, isEmpty, reason: 'background repair is silent');
      },
    );

    test(
      'physical Store repair cannot roll back a later Pause',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isTrue);

        final snapshotStarted = Completer<void>();
        final finishSnapshot = Completer<void>();
        var reads = 0;
        peer.beforeFrameStoreSnapshot = () async {
          if (++reads != 1) return;
          snapshotStarted.complete();
          await finishSnapshot.future;
        };
        peer.physicalStorePaint(square: 7, mask: 15, color: 0x44cc55);
        await _idle(owner);
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0xff7711,
          emit: false,
        );
        peer.physicalStorePaint(square: 9, mask: 15, color: 0x1188ee);
        peer.persistStore();
        await snapshotStarted.future;
        peer.frameStorePlaying = false;
        peer.connectionLitchcraftState(
          sequence: 500,
          selectedFrame: 0,
          frameElapsedMilliseconds: 25,
          playing: false,
        );
        await _until(() => !owner.animationPlaying);
        finishSnapshot.complete();
        await _until(
          () =>
              owner.documentSession.document.frames.first.emitters[32].color ==
              0xff7711,
        );
        expect(owner.animationPlaying, isFalse);
        expect(
          owner.animationElapsedMilliseconds,
          25,
          reason: 'a newer paused nonzero phase must survive repair',
        );
        expect(owner.status, LitchcraftStatus.live);
      },
      timeout: const Timeout(Duration(seconds: 60)),
    );

    test(
      'reliable physical footprint batch closes a retained-State gap without downloading artwork',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());

        peer.physicalStorePaint(square: 7, mask: 15, color: 0x44cc55);
        await _idle(owner);
        final snapshotsBefore = peer.operations
            .where(
              (operation) =>
                  operation == LitchcraftV2Protocol.frameStoreSnapshot,
            )
            .length;
        peer.physicalStoreBatch(
          emitLatestStateFirst: true,
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 8, mask: 15, color: 0x000000, clearEffects: true),
            (square: 9, mask: 15, color: 0x000000, clearEffects: true),
            (square: 10, mask: 15, color: 0x000000, clearEffects: true),
          ],
        );
        await Future<void>.delayed(const Duration(milliseconds: 180));
        await _idle(owner);

        final frame = owner.documentSession.document.frames.first;
        for (final square in <int>[8, 9, 10]) {
          expect(frame.emitters[square * 4].color, 0);
          expect(frame.emitters[square * 4].effect, LitchcraftEffectStyle.off);
        }
        expect(
          peer.operations
              .where(
                (operation) =>
                    operation == LitchcraftV2Protocol.frameStoreSnapshot,
              )
              .length,
          snapshotsBefore,
          reason:
              'the reliable batch must satisfy the coalesced State gap before its 121 ms snapshot deadline',
        );
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.errorMessage, isNull);
      },
    );

    test(
      'one physical brush footprint paints the phone without 64 rebuilds',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        var updates = 0;
        owner.documentSession.addListener(() => updates++);

        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 0, mask: 15, color: 0x1288cc, clearEffects: false),
          ],
          footprintOverride: (1 << 64) - 1,
        );
        await _idle(owner);

        expect(
          owner.documentSession.document.frames.first.emitters[0].color,
          0x1288cc,
        );
        expect(
          owner.documentSession.document.frames.first.emitters[63 * 4].color,
          0x1288cc,
        );
        expect(
          updates,
          lessThan(8),
          reason: 'one brush event is one projection',
        );
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'twelve rapid board marks project in two bounded phone updates',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        var updates = 0;
        owner.documentSession.addListener(() => updates++);
        final snapshotsBefore = peer.operations
            .where((op) => op == LitchcraftV2Protocol.frameStoreSnapshot)
            .length;

        final marks = <({int square, int mask, int color, bool clearEffects})>[
          for (var square = 0; square < 12; square++)
            (
              square: square,
              mask: 15,
              color: 0x128800 + square,
              clearEffects: false,
            ),
        ];
        peer.physicalStoreBatch(records: marks.take(10).toList());
        peer.physicalStoreBatch(records: marks.skip(10).toList());
        await _idle(owner);

        for (var square = 0; square < 12; square++) {
          expect(
            owner
                .documentSession
                .document
                .frames
                .first
                .emitters[square * 4]
                .color,
            0x128800 + square,
          );
        }
        expect(
          updates,
          lessThan(8),
          reason: 'each reverse batch projects once',
        );
        expect(
          peer.operations
              .where((op) => op == LitchcraftV2Protocol.frameStoreSnapshot)
              .length,
          snapshotsBefore,
        );
        owner.paintSquares(<int>[20]);
        owner.endStroke();
        await _idle(owner);
        expect(peer.document.frames[0].emitters[20 * 4].color, 0xff7430);
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'a physical mark is followed by an immediate guarded phone mark',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final snapshotsBefore = peer.operations
            .where((op) => op == LitchcraftV2Protocol.frameStoreSnapshot)
            .length;

        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 7, mask: 15, color: 0x1288cc, clearEffects: false),
          ],
        );
        await _idle(owner);
        owner.paintSquares(<int>[3]);
        owner.endStroke();
        await _idle(owner);

        expect(peer.document.frames[0].emitters[7 * 4].color, 0x1288cc);
        expect(peer.document.frames[0].emitters[3 * 4].color, 0xff7430);
        expect(
          owner.documentSession.document.frames[0].emitters[3 * 4].color,
          0xff7430,
        );
        expect(
          peer.operations
              .where((op) => op == LitchcraftV2Protocol.frameStoreSnapshot)
              .length,
          snapshotsBefore,
          reason: 'a complete physical batch proves the next paint base',
        );
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      'physical frame change waits for an active phone stroke boundary',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        owner.beginStroke();
        owner.paintSquares(<int>[2]);
        await _idle(owner);

        peer.selectedFrame = 1;
        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 7, mask: 15, color: 0x1288cc, clearEffects: false),
          ],
        );
        await Future<void>.delayed(Duration.zero);
        expect(owner.documentSession.selectedFrameIndex, 0);
        expect(owner.animationSelectedFrameIndex, 0);
        owner.paintSquares(<int>[3]);
        expect(
          owner.documentSession.document.frames[0].emitters[3 * 4].color,
          0xff7430,
        );
        expect(
          owner.documentSession.document.frames[1].emitters[3 * 4].color,
          isNot(0xff7430),
        );

        owner.endStroke();
        expect(owner.documentSession.selectedFrameIndex, 1);
        expect(
          owner.documentSession.document.frames[1].emitters[7 * 4].color,
          0x1288cc,
        );
        await _idle(owner);
        expect(peer.document.frames[1].emitters[3 * 4].color, isNot(0xff7430));
      },
    );

    testWidgets(
      'held phone pointer keeps the mounted canvas on its stroke frame',
      (tester) async {
        SharedPreferences.setMockInitialValues(<String, Object>{});
        peer.connectionSessionSupported = true;
        await tester.runAsync(() async {
          await _drive(owner.enter());
          final draft = LitchcraftDocumentSession(largeDraft())
            ..selectFrame(0)
            ..paintSelectedFrame(
              squares: const <int>[0],
              mask: 15,
              color: 0x112233,
            )
            ..selectFrame(1)
            ..paintSelectedFrame(
              squares: const <int>[0],
              mask: 15,
              color: 0x445566,
            );
          await _drive(owner.replaceDocument(draft.document));
        });
        final library = LitchcraftLibrary(owner);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: LitchcraftScreen(controller: owner, library: library),
            ),
          ),
        );
        await tester.pump();
        final square = find.byKey(const ValueKey('craft-square-2'));
        final pointer = await tester.startGesture(tester.getCenter(square));
        await pointer.moveBy(const Offset(20, 0));
        await tester.pump();
        expect(owner.documentSession.selectedFrameIndex, 0);

        peer.selectedFrame = 1;
        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 7, mask: 15, color: 0x1288cc, clearEffects: false),
          ],
        );
        await tester.pump();
        final held = tester.widget<LitchcraftCanvasView>(
          find.byType(LitchcraftCanvasView),
        );
        expect(held.canvas.colors[0], 0x112233);
        expect(owner.animationSelectedFrameIndex, 0);

        await pointer.up();
        await tester.pump();
        final released = tester.widget<LitchcraftCanvasView>(
          find.byType(LitchcraftCanvasView),
        );
        expect(owner.documentSession.selectedFrameIndex, 1);
        expect(released.canvas.colors[0], 0x445566);
        peer.connectionLitchcraftState(
          sequence: 4,
          selectedFrame: 1,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await tester.pump();
        expect(
          tester
              .widget<LitchcraftCanvasView>(find.byType(LitchcraftCanvasView))
              .canvas
              .colors[0],
          0x445566,
        );
        await tester.pumpWidget(const SizedBox.shrink());
        library.dispose();
      },
    );

    for (final intent in ['Play', 'Pause', 'failed Undo']) {
      test(
        'pending current Undo admits later $intent only after its edit position',
        () async {
          peer.connectionSessionSupported = true;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          final paintEntered = Completer<void>();
          final releasePaint = Completer<void>();
          addTearDown(() {
            if (!releasePaint.isCompleted) releasePaint.complete();
          });
          peer.afterOrderedPaintBeforeCheckpoint = () async {
            if (paintEntered.isCompleted) return;
            paintEntered.complete();
            await releasePaint.future;
          };
          owner.beginStroke();
          owner.paintSquares([7]);
          owner.endStroke();
          await paintEntered.future;
          // This known Undo has an optimistic inverse, but is still waiting for
          // the actual preceding Paint receipt before reserving the command tail.
          final undo = owner.undo();
          final play = owner.toggleAnimationPlayback();
          final pause = intent == 'Pause'
              ? owner.toggleAnimationPlayback()
              : null;
          if (intent == 'failed Undo') peer.storeUndo.clear();
          await pumpEventQueue(times: 5);
          expect(
            peer.orderedOperations,
            isNot(contains(ConnectionSession.litchcraftTimelineOpcode)),
          );
          expect(owner.animationPlaying, intent != 'Pause');
          releasePaint.complete();
          await _drive(Future.wait<void>([undo, play, ?pause]));
          await _idle(owner);
          final undoIndex = peer.orderedOperations.indexOf(
            ConnectionSession.litchcraftUndoOpcode,
          );
          final timelineIndex = peer.orderedOperations.indexOf(
            ConnectionSession.litchcraftTimelineOpcode,
          );
          expect(undoIndex, greaterThanOrEqualTo(0));
          if (intent == 'failed Undo') {
            expect(
              timelineIndex,
              -1,
              reason: 'a failed predecessor cannot leave an orphan Play',
            );
            expect(owner.animationPlaying, isFalse);
          } else {
            expect(
              timelineIndex,
              greaterThan(undoIndex),
              reason: 'native Undo must apply before the later Timeline intent',
            );
            expect(
              peer.orderedOperations.where(
                (opcode) =>
                    opcode == ConnectionSession.litchcraftTimelineOpcode,
              ),
              hasLength(1),
            );
            expect(peer.frameStorePlaying, intent == 'Play');
            expect(owner.animationPlaying, intent == 'Play');
          }
        },
      );
    }

    test('late exact Undo echoes cannot pause a newer accepted Play', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      await _drive(owner.replaceDocument(largeDraft()));
      owner.beginStroke();
      owner.paintSquares([for (var square = 0; square < 32; square++) square]);
      owner.endStroke();
      await _idle(owner);
      await _drive(owner.undo());
      final inverse = owner.documentSession.document;
      final inverseRevision = peer.frameStoreLiveRevision;
      expect(owner.boardArtworkUnresolved, isFalse);
      final timelineEntered = Completer<void>();
      final releaseTimeline = Completer<void>();
      addTearDown(() {
        if (!releaseTimeline.isCompleted) releaseTimeline.complete();
      });
      peer.beforeOrderedTimeline = () async {
        timelineEntered.complete();
        await releaseTimeline.future;
      };
      final playing = owner.toggleAnimationPlayback();
      await timelineEntered.future;
      expect(owner.animationPlaying, isTrue);
      final reads = peer.frameStoreLiveLayoutCount;
      void echo({
        required int cursor,
        required int firstSquare,
        required int count,
        required bool overflow,
      }) {
        // Controlled delivery of the production reverse batch wire shape.
        // Stream cursor26/32 differs from the one known Undo live revision.
        final bytes = Uint8List(15 + count * 21);
        final data = ByteData.sublistView(bytes);
        bytes[0] = LitchcraftV2Protocol.magic;
        bytes[1] = LitchcraftV2Protocol.version;
        bytes[2] = LitchcraftV2Protocol.physicalStoreBatch;
        data.setUint32(3, peer.session, Endian.little);
        data.setUint32(7, cursor, Endian.little);
        data.setUint16(11, cursor, Endian.little);
        peer.physicalStoreEditRevision = cursor;
        bytes[13] = overflow ? 1 : 0;
        bytes[14] = count;
        for (var index = 0; index < count; index++) {
          final square = firstSquare + index;
          final offset = 15 + index * 21;
          final color = inverse.frames[0].emitters[square * 4].color;
          data.setUint32(offset, peer.frameStoreGeneration, Endian.little);
          bytes[offset + 4] = 0;
          bytes[offset + 5] = 15;
          bytes[offset + 6] = color >> 16;
          bytes[offset + 7] = color >> 8 & 0xff;
          bytes[offset + 8] = color & 0xff;
          data.setUint64(offset + 9, 1 << square, Endian.little);
          data.setUint32(offset + 17, inverseRevision, Endian.little);
        }
        peer._emitStoreReceipt(bytes, cursor);
        peer.packets.add(bytes);
      }

      echo(cursor: 26, firstSquare: 16, count: 10, overflow: true);
      expect(
        owner.animationPlaying,
        isTrue,
        reason: 'an exact own inverse is content proof, not a physical Pause',
      );
      expect(owner.boardArtworkUnresolved, isFalse);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, inverse),
        isTrue,
      );
      releaseTimeline.complete();
      await _drive(playing);
      expect(peer.frameStorePlaying, isTrue);
      expect(owner.animationPlaying, isTrue);
      echo(cursor: 32, firstSquare: 26, count: 6, overflow: false);
      expect(owner.animationPlaying, isTrue);
      expect(peer.frameStoreLiveLayoutCount, reads);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, inverse),
        isTrue,
      );

      peer.beforeOrderedTimeline = null;
      peer.selectedFrame = 1;
      peer.frameStorePlaying = false;
      peer.physicalStoreBatch(
        records: [(square: 7, mask: 15, color: 0x66cc44, clearEffects: false)],
      );
      expect(
        owner.animationPlaying,
        isFalse,
        reason: 'a genuine later physical mark still owns the Pause',
      );
      expect(owner.animationSelectedFrameIndex, 1);
      expect(
        owner.documentSession.document.frames[1].emitters[28].color,
        0x66cc44,
      );
    });

    test(
      'confirmed Store inverse does not excuse a foreign overflow batch',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        owner.beginStroke();
        owner.paintSquares(<int>[7]);
        owner.endStroke();
        await _idle(owner);
        await _drive(owner.undo());
        expect(owner.documentSession.canRedo, isTrue);

        final repairStarted = Completer<void>();
        final releaseRepair = Completer<void>();
        peer.beforeFrameStoreSnapshot = () async {
          if (!repairStarted.isCompleted) repairStarted.complete();
          await releaseRepair.future;
        };
        peer.physicalStoreBatch(
          records: <({int square, int mask, int color, bool clearEffects})>[
            (square: 9, mask: 15, color: 0x1288cc, clearEffects: false),
          ],
          overflow: true,
        );
        await repairStarted.future;
        expect(owner.documentSession.canRedo, isFalse);
        peer.orderedOperations.clear();
        final redo = owner.redo();
        await pumpEventQueue(times: 10);
        expect(
          peer.orderedOperations,
          isNot(contains(ConnectionSession.litchcraftRedoOpcode)),
          reason: 'A newer foreign revision must retain the repair fence',
        );
        // Production End of the changed foreign stroke truncates native Redo.
        // This explicit completed-journal fixture supplies that existing state;
        // a fresh typed NoHistory refusal must preserve the foreign blue art.
        peer.storeRedo.clear();
        releaseRepair.complete();
        peer.beforeFrameStoreSnapshot = null;
        peer.persistStore();
        await _untilBackgroundRepair(() => !owner.pending);
        await _drive(redo);
        await _idle(owner);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
          reason: 'Normal coherent repair still adopts foreign paint',
        );
      },
    );

    test(
      'overflow repair stays fenced across a later contiguous batch',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        var snapshotAttempts = 0;
        peer.beforeFrameStoreSnapshot = () async {
          snapshotAttempts += 1;
          if (snapshotAttempts == 1) {
            throw StateError('controlled first repair read failure');
          }
        };
        peer.physicalStorePaint(
          square: 40,
          mask: 15,
          color: 0x1288cc,
          emit: false,
        );
        final marks = <({int square, int mask, int color, bool clearEffects})>[
          for (var square = 0; square < 16; square++)
            (
              square: square,
              mask: 15,
              color: 0x118800 + square,
              clearEffects: false,
            ),
        ];
        peer.physicalStoreBatch(
          records: marks.take(10).toList(),
          overflow: true,
        );
        peer.physicalStoreBatch(records: marks.skip(10).toList());
        peer.persistStore();
        for (
          var attempt = 0;
          attempt < 50 && snapshotAttempts == 0;
          attempt++
        ) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        expect(
          snapshotAttempts,
          1,
          reason: 'overflow must still require repair after the next batch',
        );
        expect(
          owner.documentSession.document.frames[0].emitters[40 * 4].color,
          isNot(0x1288cc),
        );
        owner.paintSquares(<int>[30]);
        owner.endStroke();
        await Future<void>.delayed(const Duration(milliseconds: 20));
        expect(
          peer.document.frames[0].emitters[30 * 4].color,
          isNot(0xff7430),
          reason: 'incomplete physical history cannot become a guarded base',
        );
        await Future<void>.delayed(const Duration(milliseconds: 500));
        await _idle(owner);
        expect(snapshotAttempts, greaterThanOrEqualTo(2));
        expect(
          owner.documentSession.document.frames[0].emitters[40 * 4].color,
          0x1288cc,
        );
      },
    );

    test(
      'physical Store gap retries silently when its first snapshot fails',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());

        var snapshotAttempts = 0;
        peer.beforeFrameStoreSnapshot = () async {
          snapshotAttempts += 1;
          if (snapshotAttempts == 1) {
            throw StateError('controlled background snapshot interruption');
          }
        };
        peer.physicalStorePaint(square: 7, mask: 15, color: 0x44cc55);
        await _idle(owner);
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0xff7711,
          emit: false,
        );
        peer.physicalStorePaint(square: 9, mask: 15, color: 0x1188ee);
        peer.persistStore();

        for (
          var attempt = 0;
          attempt < 100 && snapshotAttempts < 2;
          ++attempt
        ) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        expect(snapshotAttempts, greaterThanOrEqualTo(2));
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.errorMessage, isNull);
      },
    );

    for (final useBoard in <bool>[false, true]) {
      test('failed 256-frame Store load keeps a persistent choice and retries '
          '${useBoard ? 'Board' : 'App'}', () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final previousBoard = peer.document;
        final document = LitchcraftAnimationDocument(
          frames: List.generate(
            256,
            (index) => LitchcraftAnimationFrame(
              emitters: previousBoard.frames.first.emitters,
              durationMs: 50 + index,
            ),
          ),
          topology: previousBoard.topology,
          favoriteColors: previousBoard.favoriteColors,
        );
        // A controlled pre-Commit interruption at the production typed Store
        // writer; this does not reproduce the physical history backlog.
        peer.beforeFrameStoreChunk = () async {
          throw StateError('controlled pre-Commit failure');
        };
        await expectLater(
          _drive(owner.replaceDocument(document)),
          throwsStateError,
        );
        expect(owner.documentSession.document, same(document));
        expect(owner.animationPlaying, isFalse);
        expect(peer.frameStoreGeneration, 1);
        expect(owner.fullAnimationSyncDirection, isNull);
        expect(owner.pending, isFalse);
        expect(
          owner.status,
          LitchcraftStatus.conflict,
          reason: 'drained progress must not make different artwork look Live',
        );
        expect(owner.frameStoreArtworkReadPending, isTrue);
        expect(owner.storeReplacementUnresolved, isTrue);
        expect(
          peer.operations.where(
            (operation) => operation == LitchcraftV2Protocol.frameStoreCancel,
          ),
          hasLength(1),
        );
        await expectLater(
          _drive(owner.resolveConflict(useBoard: false)),
          throwsStateError,
        );
        expect(owner.documentSession.document, same(document));
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.frameStoreArtworkReadPending, isTrue);
        // A clean authority notification cannot silently adopt the old root.
        peer.persistStore();
        await pumpEventQueue(times: 100);
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.documentSession.document, same(document));

        peer.beforeFrameStoreChunk = null;
        await _drive(owner.resolveConflict(useBoard: useBoard), maxPumps: 6000);
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.frameStoreArtworkReadPending, isFalse);
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            useBoard ? previousBoard : document,
          ),
          isTrue,
        );
        expect(
          sameLitchcraftDocument(peer.document, owner.documentSession.document),
          isTrue,
        );
        expect(peer.frameStorePlaying, isFalse);
      });
    }

    for (final failSelect in <bool>[false, true]) {
      test('failed Store ${failSelect ? 'cached Select' : 'Inspect'} '
          'retains a choice without guessed board metadata', () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final first = largeDraft();
        await _drive(owner.replaceDocument(first));
        final edit = LitchcraftDocumentSession(first)
          ..setSelectedFrameDuration(2000);
        final second = edit.document;
        if (failSelect) await _drive(owner.replaceDocument(second));
        peer.beforeFrameStoreInspect = failSelect
            ? null
            : () async => throw StateError('controlled Inspect failure');
        peer.beforeFrameStoreSelect = failSelect
            ? () async => throw StateError('controlled cached Select failure')
            : null;
        final local = failSelect ? first : second;
        await expectLater(
          _drive(owner.replaceDocument(local)),
          throwsStateError,
        );
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.frameStoreArtworkReadPending, isTrue);
        expect(owner.documentSession.document, same(local));
        peer.beforeFrameStoreInspect = null;
        peer.beforeFrameStoreSelect = null;
        await _drive(owner.resolveConflict(useBoard: false));
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.frameStoreArtworkReadPending, isFalse);
        expect(owner.storeReplacementUnresolved, isFalse);
        expect(sameLitchcraftDocument(peer.document, local), isTrue);
      });
    }

    test(
      'failed Store replacement on a retired session enters recovery',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final local = largeDraft();
        peer.beforeFrameStoreChunk = () async {
          await peer._connectionSession!.dispose();
          throw StateError('controlled retired shared session');
        };
        await expectLater(
          _drive(owner.replaceDocument(local)),
          throwsStateError,
        );
        expect(owner.status, LitchcraftStatus.error);
        expect(owner.documentSession.document, same(local));
        expect(owner.storeReplacementUnresolved, isTrue);
        expect(owner.errorMessage, contains('Connection session disposed'));
        // The production BLE owner reconnects before reactivation. This peer
        // has no native reconnect adapter; this regression covers the changed
        // failure-to-recovery handoff, not that unchanged physical reconnect.
        for (final clearBeforeUnwind in [true, false]) {
          await _withCurrentStore((warmPeer, warmOwner) async {
            final durable = staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint([0, 63], 4, 0x123456),
            );
            warmPeer.installSavedStoreDocument(durable);
            await _drive(warmOwner.enter());
            await _untilBackgroundRepair(
              () => !warmOwner.boardArtworkUnresolved,
            );
            final opening = LitchcraftDocumentSession(durable)
              ..paintSelectedFrame(squares: [7], mask: 15, color: 0xabcdef);
            final interrupted = TimeoutException(
              'controlled pre-Commit Open failure',
            );
            warmPeer.beforeFrameStoreChunk = () async {
              warmPeer.connection(false);
              if (clearBeforeUnwind) await warmOwner.clear();
              throw interrupted;
            };
            await expectLater(
              _drive(warmOwner.replaceDocument(opening.document)),
              throwsA(same(interrupted)),
            );
            if (!clearBeforeUnwind) await _drive(warmOwner.clear());
            final phone = warmOwner.documentSession.document;
            expect(isEmptyLitchcraftDocument(phone), isTrue);
            expect(sameLitchcraftDocument(warmPeer.document, durable), isTrue);
            warmPeer.beforeFrameStoreChunk = null;
            await warmPeer.retireSharedConnection();
            warmPeer.operations.clear();
            warmPeer.connection(true);
            await _drive(warmOwner.settleModeResolution());
            await _untilBackgroundRepair(
              () => !warmOwner.boardArtworkUnresolved,
            );
            expect(
              warmOwner.status,
              LitchcraftStatus.conflict,
              reason:
                  'Offline Clear ${clearBeforeUnwind ? 'before' : 'after'} Open failure unwinds retires its automatic replacement permission.',
            );
            expect(warmOwner.storeReplacementUnresolved, isFalse);
            expect(sameLitchcraftDocument(warmPeer.document, durable), isTrue);
            expect(
              sameLitchcraftDocument(warmOwner.documentSession.document, phone),
              isTrue,
            );
            expect(
              warmPeer.operations,
              isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
            );
          });
        }
      },
    );

    test(
      'inactive Store Inspect enters mode recovery and retains the draft',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final edit = LitchcraftDocumentSession(largeDraft())
          ..setSelectedFrameDuration(2000);
        final local = edit.document;
        // Store Inspect can report a real mode exit before ordinary mode State
        // reaches the app. Its OK result still proves active=false.
        peer.active = false;
        await expectLater(
          _drive(owner.replaceDocument(local)),
          throwsStateError,
        );
        expect(owner.status, LitchcraftStatus.error);
        expect(
          owner.frameStoreArtworkReadPending,
          isFalse,
          reason: 'an inactive mode cannot service an artwork choice',
        );
        expect(owner.documentSession.document, same(local));
        expect(owner.storeReplacementUnresolved, isTrue);
      },
    );

    testWidgets('failed Store load shows recovery after many idle minutes', (
      tester,
    ) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      tester.view.physicalSize = const Size(390, 844);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      peer.connectionSessionSupported = true;
      await tester.runAsync(() async {
        await _drive(owner.enter());
        peer.beforeFrameStoreChunk = () async {
          throw StateError('controlled pre-Commit failure');
        };
        await expectLater(
          _drive(owner.replaceDocument(largeDraft())),
          throwsStateError,
        );
      });
      final library = LitchcraftLibrary(owner);
      await tester.runAsync(() => library.ready);
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(controller: owner, library: library),
          ),
        ),
      );
      await tester.pump();
      expect(find.text('Choose artwork'), findsOneWidget);
      final action = find.byKey(const ValueKey('craft-resolve-conflict'));
      expect(action, findsOneWidget);
      expect(tester.getSize(action).height, greaterThanOrEqualTo(48));
      expect(
        find.byKey(const ValueKey('craft-use-app-artwork')),
        findsNothing,
        reason: 'a failure must not open the chooser without a user action',
      );
      await tester.pump(const Duration(minutes: 3));
      expect(find.text('Choose artwork'), findsOneWidget);
      expect(action, findsOneWidget);
      expect(owner.fullAnimationSyncDirection, isNull);
      expect(owner.status, LitchcraftStatus.conflict);
      await tester.pumpWidget(const SizedBox());
      library.dispose();
    });

    test(
      'existing Store Play and Pause project before ACK and roll back refusal',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isTrue);
        expect(peer.frameStorePlaying, isTrue);

        final pauseReceipt = Completer<void>();
        peer.beforeOrderedTimeline = () => pauseReceipt.future;
        var pauseCompleted = false;
        final pause = owner.toggleAnimationPlayback().whenComplete(() {
          pauseCompleted = true;
        });
        expect(
          owner.animationPlaying,
          isFalse,
          reason: 'the local control must respond before the BLE checkpoint',
        );
        await pumpEventQueue(times: 10);
        expect(pauseCompleted, isFalse);
        pauseReceipt.complete();
        await _drive(pause);
        expect(peer.frameStorePlaying, isFalse);

        peer.beforeOrderedTimeline = null;
        peer.rejectNextOrderedTimelineReason = 1;
        final rejectedPlay = owner.toggleAnimationPlayback();
        expect(owner.animationPlaying, isTrue);
        await _drive(rejectedPlay);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          owner.animationPlaying,
          isFalse,
          reason: 'a refused newest intent restores the accepted board state',
        );
      },
    );

    test(
      'Store manual frame intent survives feedback until explicit Play',
      () async {
        peer.connectionSessionSupported = true;
        var now = 1000;
        owner.dispose();
        owner = LitchcraftController(peer, monotonicMilliseconds: () => now);
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        await _drive(owner.selectAnimationFrame(7));
        final frameSevenStart = litchcraftAnimationFrameStartOrNull(
          owner.documentSession.document,
          7,
        )!;
        expect(owner.animationSelectedFrameIndex, 7);
        expect(owner.animationElapsedMilliseconds, frameSevenStart);

        await _drive(owner.toggleAnimationPlayback());
        now += 25;
        expect(owner.animationPlaying, isTrue);
        expect(owner.animationSelectedFrameIndex, 7);
        expect(owner.animationElapsedMilliseconds, frameSevenStart + 25);

        final selectionReceipt = Completer<void>();
        peer.beforeOrderedTimeline = () => selectionReceipt.future;
        final predecessorSelection = owner.selectAnimationFrame(4);
        final selection = owner.selectAnimationFrame(12);
        final frameTwelveStart = litchcraftAnimationFrameStartOrNull(
          owner.documentSession.document,
          12,
        )!;
        expect(owner.animationPlaying, isFalse);
        expect(owner.animationSelectedFrameIndex, 12);
        expect(owner.animationElapsedMilliseconds, frameTwelveStart);
        selectionReceipt.complete();
        await _drive(predecessorSelection);
        await _drive(selection);
        expect(peer.selectedFrame, 12);

        peer.connectionLitchcraftState(
          sequence: 100,
          selectedFrame: 4,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await pumpEventQueue(times: 10);
        expect(
          owner.animationSelectedFrameIndex,
          12,
          reason: 'Paused board feedback cannot replace a manual app choice',
        );
        expect(owner.animationElapsedMilliseconds, frameTwelveStart);
        peer.connectionLitchcraftState(
          sequence: 101,
          selectedFrame: 7,
          frameElapsedMilliseconds: 0,
          playing: true,
        );
        await pumpEventQueue(times: 10);
        expect(owner.animationPlaying, isFalse);
        expect(owner.animationSelectedFrameIndex, 12);
        owner.beginStroke();
        owner.paintSquares(<int>[7]);
        owner.endStroke();
        await _idle(owner);
        expect(owner.animationSelectedFrameIndex, 12);
        expect(peer.document.frames[12].emitters[28].color, owner.color);
        expect(
          sameLitchcraftDocument(peer.document, owner.documentSession.document),
          isTrue,
        );

        await _drive(owner.toggleAnimationPlayback());
        now += owner.documentSession.document.frames[12].durationMs + 1;
        expect(owner.animationSelectedFrameIndex, 13);
        peer.connectionLitchcraftState(
          sequence: peer._connectionLitchcraftStateSequence + 1,
          selectedFrame: 13,
          frameElapsedMilliseconds: 0,
          playing: false,
        );
        await _until(() => !owner.animationPlaying);
        expect(owner.animationSelectedFrameIndex, 13);
      },
    );

    test(
      'reconnect preserves normal large draft and does not upload automatically',
      () async {
        await _drive(owner.enter());
        final document = largeDraft();
        await _drive(owner.replaceDocument(document));
        peer.connection(false);
        peer.connection(true);
        await Future<void>.delayed(Duration.zero);
        await _drive(owner.settleModeResolution());
        expect(owner.documentSession.document, same(document));
        expect(peer.frameStoreGeneration, 0);
        expect(owner.status, LitchcraftStatus.live);
      },
    );

    test(
      '256-frame artwork remains paused through Commit confirmation',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        final receipt = Completer<void>();
        peer.beforeFrameStoreCommit = () => receipt.future;
        var confirmed = false;
        final delivery = owner.runRepeated256FrameDiagnostic().then((result) {
          confirmed = true;
          return result;
        });
        expect(owner.animationPlaying, isFalse);
        expect(owner.frameStorePlaybackRequested, isFalse);
        expect(owner.pending, isFalse);
        await pumpEventQueue(times: 20);
        expect(peer.frameStoreGeneration, 2);
        expect(confirmed, isFalse);
        expect(owner.animationPlaying, isFalse);
        expect(owner.frameStorePlaybackRequested, isFalse);
        expect(owner.pending, isFalse);
        expect(owner.status, LitchcraftStatus.live);
        receipt.complete();
        final result = await _drive(delivery);
        expect(result.transfer.state.playing, isFalse);
        expect(owner.animationPlaying, isFalse);
        expect(peer.document.playbackOnEntry, isTrue);
        expect(owner.frameStorePlaybackRequested, isFalse);
        expect(confirmed, isTrue);
      },
    );

    test(
      'late Store root keeps old artwork visible until durable adoption',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        peer.physicalStorePaint(
          square: 8,
          mask: 15,
          color: 0x31c7ff,
          emit: false,
        );
        final oldDocument = peer.document;
        final replacement = largeDraft(playbackOnEntry: true);
        final root = Completer<void>();
        peer.deferredFrameStoreRoot = root;
        var confirmed = false;
        final delivery = owner.replaceDocument(replacement).then((result) {
          confirmed = true;
          return result;
        });
        for (var i = 0; i < 500 && !peer.frameStoreCommitAcceptedPending; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        await _untilBackgroundRepair(() => owner.storeRootPublicationPending);
        expect(
          peer.frameStoreCommitAcceptedPending,
          isTrue,
          reason:
              'operations=${peer.operations.length} last=${peer.operations.isEmpty ? null : peer.operations.last}',
        );
        expect(confirmed, isFalse);
        expect(owner.pending, isFalse);
        expect(owner.storeRootPublicationPending, isTrue);
        expect(owner.animationPlaying, isFalse);
        expect(peer.document, same(oldDocument));
        expect(
          owner.fullAnimationSyncDirection,
          LitchcraftFullSyncDirection.sendingToBoard,
        );
        expect(owner.fullAnimationSyncProgress, 0.99);

        peer.frameStorePlaying = false;
        peer.playing = false;
        peer.physicalStorePaint(square: 8, mask: 15, color: 0x31c7ff);
        peer.emitDelayedPhysicalStoreEdit();
        peer.emitDelayedPhysicalStoreBatch();
        await _idle(owner);
        expect(owner.animationPlaying, isFalse);
        expect(peer.document, same(oldDocument));
        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          replacement.frames.first.emitters[32].color,
          reason: 'a delayed old-generation paint cannot alter the new draft',
        );
        expect(
          owner.documentSession.document.frames.first.emitters[36].color,
          replacement.frames.first.emitters[36].color,
          reason: 'a delayed old-generation batch cannot alter the new draft',
        );
        expect(owner.fullAnimationSyncProgress, 0.99);

        root.complete();
        await _drive(delivery);
        expect(confirmed, isTrue);
        expect(owner.fullAnimationSyncDirection, isNull);
        expect(owner.storeRootPublicationPending, isFalse);
        expect(peer.document, isNot(same(oldDocument)));
        await _idle(owner);
        expect(
          peer.operations
              .where(
                (operation) =>
                    operation == LitchcraftV2Protocol.frameStoreBegin,
              )
              .length,
          1,
          reason: 'old-generation paint must not trigger a second transfer',
        );
        peer.physicalStorePaint(square: 8, mask: 15, color: 0x31c7ff);
        await _idle(owner);
        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          0x31c7ff,
          reason: 'physical paint after durable adoption reaches the app',
        );
        expect(owner.animationPlaying, isFalse);
      },
      timeout: const Timeout(Duration(seconds: 60)),
    );

    test(
      'single-frame root retry keeps a sending cue without Play',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        final oldDocument = peer.document;
        final first = oldDocument.frames.first;
        final replacement = LitchcraftAnimationDocument(
          frames: <LitchcraftAnimationFrame>[
            LitchcraftAnimationFrame(
              emitters: first.emitters,
              durationMs: first.durationMs + 1,
            ),
          ],
          topology: oldDocument.topology,
          favoriteColors: oldDocument.favoriteColors,
        );
        final root = Completer<void>();
        peer.deferredFrameStoreRoot = root;
        final delivery = owner.replaceDocument(replacement);
        for (var i = 0; i < 500 && !peer.frameStoreCommitAcceptedPending; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        await _untilBackgroundRepair(() => owner.storeRootPublicationPending);
        expect(peer.frameStoreCommitAcceptedPending, isTrue);
        expect(peer.document, same(oldDocument));
        expect(owner.storeRootPublicationPending, isTrue);
        expect(owner.fullAnimationSyncDirection, isNull);
        expect(owner.animationPlaybackPending, isFalse);
        root.complete();
        await _drive(delivery);
        expect(owner.storeRootPublicationPending, isFalse);
        expect(peer.document.frames.first.durationMs, first.durationMs + 1);
      },
      timeout: const Timeout(Duration(seconds: 60)),
    );

    test(
      'lost pending Store receipt enters recovery without false playback',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        peer.deferredFrameStoreRoot = Completer<void>();
        final delivery = owner.replaceDocument(
          largeDraft(playbackOnEntry: true),
        );
        unawaited(delivery.catchError((Object _) {}));
        for (var i = 0; i < 500 && !peer.frameStoreCommitAcceptedPending; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        await _untilBackgroundRepair(() => owner.storeRootPublicationPending);
        expect(peer.frameStoreCommitAcceptedPending, isTrue);
        expect(owner.animationPlaying, isFalse);
        await peer._connectionSession!.dispose();
        await expectLater(
          delivery,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        expect(owner.animationPlaying, isFalse);
        expect(owner.status, LitchcraftStatus.error);
        final retainedDraft = owner.documentSession.document;
        final confirmedBeforeLate = owner.documentSession.boardDocument;
        peer.connection(false);
        await _idle(owner);
        peer.operations.clear();
        peer.deferredFrameStoreRoot!.complete();
        await _until(() => !peer.frameStoreCommitAcceptedPending);
        await pumpEventQueue(times: 10);
        final lateReceipt = LitchcraftFrameStoreState.fromPacket(
          LitchcraftV2Packet.decode(peer._lastStoreReceipt!)!,
        );
        expect(lateReceipt.residentListing, isNotNull);
        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.documentSession.document, same(retainedDraft));
        expect(
          owner.documentSession.boardDocument,
          same(confirmedBeforeLate),
          reason:
              'Disconnected Commit proof cannot populate live cache or '
              'confirm the retired transfer',
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreSelect)),
        );
        expect(
          peer.operations,
          isNot(contains(LitchcraftV2Protocol.frameStoreBegin)),
        );
      },
      timeout: const Timeout(Duration(seconds: 60)),
    );

    test(
      'same-link Exit retires pending Commit receipt before a new mode session',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        peer.deferredFrameStoreRoot = Completer<void>();
        final delivery = owner.replaceDocument(largeDraft());
        unawaited(delivery.catchError((Object _) {}));
        for (var i = 0; i < 500 && !peer.frameStoreCommitAcceptedPending; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        await Future<void>.delayed(const Duration(seconds: 1));
        expect(peer.frameStoreCommitAcceptedPending, isTrue);
        await _drive(owner.leave());
        await expectLater(
          delivery,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        expect(owner.animationPlaying, isFalse);
        expect(owner.status, LitchcraftStatus.offline);
        expect(peer.connected, isTrue);
      },
      timeout: const Timeout(Duration(seconds: 60)),
    );

    test(
      'Exit during Commit receipt waits cancels watcher created after Exit began',
      () async {
        peer.connectionSessionSupported = true;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
        );
        await _drive(owner.enter());
        peer.deferredFrameStoreRoot = Completer<void>();
        final delivery = owner.replaceDocument(largeDraft());
        unawaited(delivery.catchError((Object _) {}));
        for (var i = 0; i < 500 && !peer.frameStoreCommitAcceptedPending; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        expect(peer.frameStoreCommitAcceptedPending, isTrue);
        final leaving = owner.leave();
        await leaving.timeout(const Duration(seconds: 2));
        await expectLater(
          delivery.timeout(const Duration(seconds: 2)),
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        expect(owner.status, LitchcraftStatus.offline);
        expect(peer.connected, isTrue);
      },
      timeout: const Timeout(Duration(seconds: 10)),
    );

    test(
      'Hall paint before Commit rejection remains a conflict with replacement',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final replacement = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: <int>[8], mask: 15, color: 0xaabbcc);
        final heldChunk = Completer<void>();
        peer.beforeFrameStoreChunk = () => heldChunk.future;
        peer.rejectFrameStoreCommit = true;
        peer.operations.clear();
        final delivery = owner.replaceDocument(replacement.document);
        unawaited(delivery.catchError((Object _) {}));
        await _until(
          () => peer.operations.contains(LitchcraftV2Protocol.frameStoreChunk),
        );
        await owner.toggleAnimationPlayback();
        expect(owner.animationPlaybackPending, isTrue);
        expect(owner.animationPlaying, isFalse);

        peer.physicalStorePaint(square: 8, mask: 15, color: 0x31c7ff);
        await pumpEventQueue(times: 5);
        heldChunk.complete();
        await expectLater(delivery, throwsA(isA<LitchcraftRejected>()));
        expect(
          owner.documentSession.document.frames.first.emitters[32].color,
          0xaabbcc,
          reason: 'the rejected replacement retains its own local intent',
        );
        expect(
          owner.documentSession.boardDocument!.frames.first.emitters[32].color,
          0x31c7ff,
          reason: 'the old resident keeps the accepted Hall paint',
        );
        expect(owner.status, LitchcraftStatus.conflict);
        expect(owner.animationPlaybackPending, isFalse);
        expect(peer.frameStorePlaying, isFalse);
      },
    );

    test('Play during held Store Chunk waits for the new artwork', () async {
      peer.connectionSessionSupported = true;
      await _drive(owner.enter());
      await _drive(owner.replaceDocument(largeDraft()));
      final replacement = LitchcraftDocumentSession(peer.document)
        ..paintSelectedFrame(squares: <int>[8], mask: 15, color: 0xaabbcc);
      final heldChunk = Completer<void>();
      peer.beforeFrameStoreChunk = () => heldChunk.future;
      peer.operations.clear();
      final delivery = owner.replaceDocument(replacement.document);
      await _until(
        () => peer.operations.contains(LitchcraftV2Protocol.frameStoreChunk),
      );
      expect(
        owner.fullAnimationSyncDirection,
        LitchcraftFullSyncDirection.sendingToBoard,
      );
      expect(owner.fullAnimationSyncProgress, isNotNull);
      await owner.toggleAnimationPlayback();
      expect(owner.animationPlaybackPending, isTrue);
      expect(owner.animationPlaying, isFalse);
      expect(peer.frameStorePlaying, isFalse);
      expect(
        peer.orderedOperations,
        isNot(contains(ConnectionSession.litchcraftTimelineOpcode)),
      );

      heldChunk.complete();
      await _drive(delivery);
      expect(owner.fullAnimationSyncDirection, isNull);
      expect(owner.fullAnimationSyncProgress, isNull);
      await _until(() => peer.frameStorePlaying);
      expect(owner.animationPlaybackPending, isFalse);
      expect(owner.animationPlaying, isTrue);
      expect(peer.frameStoreGeneration, 3);
    });

    test(
      'rejected queued Play stays visible until an explicit retry',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final replacement = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: <int>[8], mask: 15, color: 0xaabbcc);
        final heldChunk = Completer<void>();
        peer.beforeFrameStoreChunk = () => heldChunk.future;
        peer.operations.clear();
        final delivery = owner.replaceDocument(replacement.document);
        await _until(
          () => peer.operations.contains(LitchcraftV2Protocol.frameStoreChunk),
        );
        await owner.toggleAnimationPlayback();
        expect(owner.animationPlaybackPending, isTrue);
        peer.rejectNextOrderedTimelineReason = 9;
        heldChunk.complete();
        await _drive(delivery);
        await _until(() => owner.animationPlaybackFailed);
        expect(owner.animationPlaybackPending, isFalse);
        expect(owner.animationPlaying, isFalse);
        expect(peer.frameStorePlaying, isFalse);

        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaybackFailed, isFalse);
        expect(owner.animationPlaying, isTrue);
        expect(peer.frameStorePlaying, isTrue);
      },
    );

    test(
      'queued Play is retired on disconnect before Store admission',
      () async {
        peer.connectionSessionSupported = true;
        await _drive(owner.enter());
        await _drive(owner.replaceDocument(largeDraft()));
        final replacement = LitchcraftDocumentSession(peer.document)
          ..paintSelectedFrame(squares: <int>[8], mask: 15, color: 0xaabbcc);
        final heldChunk = Completer<void>();
        peer.beforeFrameStoreChunk = () => heldChunk.future;
        peer.operations.clear();
        final delivery = owner.replaceDocument(replacement.document);
        unawaited(delivery.catchError((Object _) {}));
        await _until(
          () => peer.operations.contains(LitchcraftV2Protocol.frameStoreChunk),
        );
        await owner.toggleAnimationPlayback();
        expect(owner.animationPlaybackPending, isTrue);
        peer.connection(false);
        heldChunk.complete();
        await expectLater(
          delivery,
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        expect(owner.animationPlaybackPending, isFalse);
        expect(owner.animationPlaying, isFalse);
        expect(peer.frameStorePlaying, isFalse);
      },
    );

    test(
      'Play intent waits for durable root instead of starting old artwork',
      () async {
        peer.connectionSessionSupported = true;
        peer.stalePausedCommitReceipt = true;
        var now = 0;
        owner.dispose();
        owner = LitchcraftController(
          peer,
          frameStoreResponseTimeout: const Duration(milliseconds: 100),
          monotonicMilliseconds: () => now,
        );
        await _drive(owner.enter());
        final root = Completer<void>();
        peer.deferredFrameStoreRoot = root;
        final delivery = owner.replaceDocument(largeDraft());
        for (var i = 0; i < 500 && !peer.frameStoreCommitAcceptedPending; i++) {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        }
        await _untilBackgroundRepair(() => owner.storeRootPublicationPending);
        expect(peer.frameStoreCommitAcceptedPending, isTrue);
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaybackPending, isTrue);
        expect(peer.frameStorePlaying, isFalse);
        expect(owner.animationPlaying, isFalse);
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaybackPending, isFalse);
        expect(peer.frameStorePlaying, isFalse);
        expect(owner.animationPlaying, isFalse);
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaybackPending, isTrue);
        expect(peer.frameStorePlaying, isFalse);
        expect(
          peer.orderedOperations,
          isNot(contains(ConnectionSession.litchcraftTimelineOpcode)),
        );
        expect(root.isCompleted, isFalse);
        root.complete();
        await _drive(delivery);
        await _until(() => peer.frameStorePlaying);
        now = 120;
        final afterReceipt = owner.animationElapsedMilliseconds;
        now = 240;
        expect(owner.animationPlaying, isTrue);
        expect(owner.animationElapsedMilliseconds, greaterThan(afterReceipt));
      },
      timeout: const Timeout(Duration(seconds: 60)),
    );

    test('late Store body cannot roll back a confirmed Pause', () async {
      peer.connectionSessionSupported = true;
      peer.admitStoreReadsBeforeResponse = true;
      peer.reportedSchema = 0;
      peer.document = largeDraft();
      peer.frameStoreGeneration = 7;
      peer.frameStoreFrameCount = peer.document.frames.length;
      peer.frameStoreTopology = peer.document.topology.id;
      peer.frameStoreHasArtwork = true;
      peer.frameStoreActive = true;
      peer.frameStorePlaying = true;
      peer.active = true;
      peer.frameStoreBody.addAll(_frameStoreBody(peer.document));
      await _drive(owner.enter());
      await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
      expect(owner.animationPlaying, isTrue);
      await _drive(owner.toggleAnimationPlayback());
      expect(owner.animationPlaying, isFalse);
      expect(peer.frameStorePlaying, isFalse);

      // Pause requires a coherent current source. A subsequent missed Hall
      // edit needs genuine live bytes; its later body cannot restart playback.
      peer.physicalStorePaint(
        square: 7,
        mask: 15,
        color: 0x66cc44,
        emit: false,
      );
      final snapshotStarted = Completer<void>();
      final finishSnapshot = Completer<void>();
      addTearDown(() {
        if (!finishSnapshot.isCompleted) finishSnapshot.complete();
      });
      peer.beforeFrameStoreSnapshot = () async {
        if (snapshotStarted.isCompleted) return;
        snapshotStarted.complete();
        await finishSnapshot.future;
      };
      await _drive(owner.revalidateSelectedBoard());
      await snapshotStarted.future;
      expect(owner.status, LitchcraftStatus.live);
      expect(owner.boardArtworkLoading, isTrue);
      expect(owner.boardArtworkUnresolved, isTrue);
      expect(owner.pending, isTrue);
      expect(owner.animationPlaybackPending, isFalse);
      expect(owner.animationPlaying, isFalse);
      expect(peer.frameStorePlaying, isFalse);
      final retained = owner.documentSession.document;
      final timelines = peer.orderedOperations
          .where(
            (operation) =>
                operation == ConnectionSession.litchcraftTimelineOpcode,
          )
          .length;
      owner.beginStroke();
      owner.paintSquares([9]);
      owner.endStroke();
      await _drive(owner.toggleAnimationPlayback());
      expect(owner.documentSession.document, same(retained));
      expect(owner.animationPlaybackPending, isFalse);
      expect(
        peer.orderedOperations.where(
          (operation) =>
              operation == ConnectionSession.litchcraftTimelineOpcode,
        ),
        hasLength(timelines),
      );

      finishSnapshot.complete();
      await _until(
        () => sameLitchcraftDocument(
          owner.documentSession.document,
          peer.document,
        ),
      );
      expect(owner.status, LitchcraftStatus.live);
      expect(owner.animationPlaying, isFalse);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, peer.document),
        isTrue,
      );
    });

    test(
      '32-frame trace retains delivery identity and every timing phase',
      () async {
        final Directory traceDirectory = await Directory.systemTemp.createTemp(
          'litch_frame_store_trace_',
        );
        final AppTraceRecorder trace = await AppTraceRecorder.open(
          traceDirectory,
        );
        owner.dispose();
        owner = LitchcraftController(peer, appTrace: trace);
        try {
          peer.connectionSessionSupported = true;
          peer.packetPayloadBytes = 512;
          await _drive(owner.enter());
          await _drive(owner.replaceDocument(largeDraft()));
          await trace.flush();
          final AppTraceSnapshot snapshot = await trace.snapshot();
          final List<Map<String, Object?>> timingRecords = snapshot.jsonl
              .split('\n')
              .where((String line) => line.isNotEmpty)
              .map((String line) => jsonDecode(line) as Map<String, Object?>)
              .where(
                (Map<String, Object?> record) =>
                    record['event'] == 'litchcraft.frame_store_timing',
              )
              .toList();
          expect(timingRecords, hasLength(1));
          final Map<String, Object?> timing = timingRecords.single;
          for (final String field in <String>[
            'queue_wait_ms',
            'setup_ms',
            'preparation_ms',
            'begin_write_ms',
            'bulk_write_ms',
            'checkpoint_ms',
            'commit_write_ms',
            'receipt_wait_ms',
            'transfer_ms',
            'total_ms',
            'request_total_ms',
          ]) {
            expect(timing[field], isA<int>(), reason: field);
          }
          expect(timing['fields_omitted'], isNull);
          final delivery = snapshot.jsonl
              .split('\n')
              .where((line) => line.isNotEmpty)
              .map((line) => jsonDecode(line) as Map<String, Object?>)
              .singleWhere(
                (record) =>
                    record['event'] == 'litchcraft.frame_store_delivery',
              );
          expect(delivery['fields_omitted'], isNull);
          expect(delivery['frames'], 32);
          expect(delivery['resident_hit'], isFalse);
          expect(delivery['canonical_bytes'], greaterThan(0));
          expect(delivery['canonical_crc32'], isA<int>());
          expect(delivery['generation'], timing['generation']);
          expect(delivery['session'], timing['session']);
          expect(snapshot.droppedEvents, 0);
        } finally {
          await trace.close();
          await traceDirectory.delete(recursive: true);
        }
      },
    );

    test(
      'Store uses negotiated ATT517 without old document packet bounds',
      () async {
        peer.connectionSessionSupported = true;
        peer.packetPayloadBytes = 512;
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.live);
        peer.operations.clear();

        final partialBatch = Completer<void>();
        final releaseBatch = Completer<void>();
        var chunkWrites = 0;
        peer.beforeFrameStoreChunk = () async {
          if (++chunkWrites == 4) {
            partialBatch.complete();
            await releaseBatch.future;
          }
        };
        addTearDown(() {
          if (!releaseBatch.isCompleted) releaseBatch.complete();
        });
        var transferCompleted = false;
        final sending = owner.runRepeated256FrameDiagnostic().then((result) {
          transferCompleted = true;
          return result;
        });
        await _drive(partialBatch.future);
        await Future<void>.value();
        expect(transferCompleted, isFalse);
        expect(
          owner.fullAnimationSyncDirection,
          LitchcraftFullSyncDirection.sendingToBoard,
        );
        expect(owner.fullAnimationSyncProgress, greaterThan(0));
        expect(owner.fullAnimationSyncProgress, lessThan(.99));
        releaseBatch.complete();
        final result = await _drive(sending);
        peer.beforeFrameStoreChunk = null;

        expect(result.transfer.bodyBytes, 11264);
        expect(result.transfer.chunkPackets, 24);
        expect(result.transfer.checkpoints, 0);
        expect(result.transfer.writes, 26);
        expect(result.transfer.state.frameCount, 256);
        expect(result.transfer.state.playing, isFalse);
        expect(peer.frameStoreGeneration, 2);
        expect(peer.frameStoreFrameCount, 256);
        expect(peer.frameStoreCursor, 11264);
        expect(
          peer.operations.where(
            (operation) => operation == LitchcraftV2Protocol.frameStoreChunk,
          ),
          hasLength(24),
        );

        peer.operations.clear();
        final sparse = await _drive(owner.runSparse256FrameDiagnostic());
        expect(sparse.transfer.state.frameCount, 256);
        expect(sparse.transfer.bodyBytes, 13000);
        expect(sparse.transfer.chunkPackets, 27);
        expect(sparse.transfer.writes, 29);
        expect(sparse.transfer.checkpoints, 0);

        peer.operations.clear();
        final dense = await _drive(owner.runDense256FrameDiagnostic());
        expect(dense.transfer.state.frameCount, 256);
        expect(dense.transfer.bodyBytes, greaterThan(100000));
        expect(dense.transfer.chunkPackets, 406);
        expect(dense.transfer.checkpoints, 0);
        expect(
          dense.transfer.writes,
          dense.transfer.chunkPackets + dense.transfer.checkpoints + 2,
        );
      },
    );

    test(
      'dense 256-frame Store uses one final barrier on the shared high-MTU path',
      () async {
        peer.connectionSessionSupported = true;
        peer.packetPayloadBytes = 244;
        await _drive(owner.enter());
        final result = await _drive(owner.runDense256FrameDiagnostic());

        expect(result.transfer.state.frameCount, 256);
        expect(result.transfer.bodyBytes, 196545);
        expect(result.transfer.chunkPackets, greaterThan(800));
        expect(result.transfer.checkpoints, 0);
        expect(result.transfer.writes, result.transfer.chunkPackets + 2);
      },
    );

    test(
      'document protocol stores inactive artwork without entering during a game',
      () async {
        peer.active = false;
        expect(await _drive(owner.adoptActiveBoard()), isFalse);
        peer.enterStatus = 1;
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.gameActive);
        expect(owner.canSaveArtworkToBoard, isTrue);
        final sessionBefore = peer.session;
        final legacyRevisionBefore = peer.revision;
        owner.documentSession.applyEffect(
          square: 9,
          mask: 5,
          effect: LitchcraftEffectStyle.rainbow,
        );
        final source = owner.documentSession.document;

        expect(await _drive(owner.saveArtworkToBoard()), isTrue);

        expect(peer.replacementSessions, <int>[0]);
        expect(peer.active, isFalse);
        expect(peer.session, sessionBefore);
        expect(peer.revision, legacyRevisionBefore + 1);
        expect(owner.status, LitchcraftStatus.gameActive);
        expect(owner.animationBoardAuthorityLive, isFalse);
        expect(sameLitchcraftDocument(peer.document, source), isTrue);
      },
    );

    test(
      'document protocol refreshes game revision before inactive artwork storage',
      () async {
        peer.active = false;
        expect(await _drive(owner.adoptActiveBoard()), isFalse);
        peer.enterStatus = 1;
        await _drive(owner.enter());
        expect(owner.status, LitchcraftStatus.gameActive);
        final cachedRevision = peer.revision;
        peer.revision++;
        owner.documentSession.applyEffect(
          square: 18,
          mask: 15,
          effect: LitchcraftEffectStyle.breathe,
        );
        final source = owner.documentSession.document;

        expect(await _drive(owner.saveArtworkToBoard()), isTrue);

        expect(peer.replacementSessions, <int>[0]);
        expect(peer.revision, cachedRevision + 2);
        expect(owner.status, LitchcraftStatus.gameActive);
        expect(owner.animationBoardAuthorityLive, isFalse);
        expect(sameLitchcraftDocument(peer.document, source), isTrue);
      },
    );

    test(
      'background inactive delivery asks before reading stale schema-3 artwork',
      () async {
        peer.active = false;
        expect(await _drive(owner.adoptActiveBoard()), isFalse);
        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.canSaveArtworkToBoard, isTrue);

        owner.documentSession.applyEffect(
          square: 21,
          mask: 15,
          effect: LitchcraftEffectStyle.rainbow,
        );
        await Future<void>.delayed(const Duration(milliseconds: 650));
        await _idle(owner);

        expect(owner.status, LitchcraftStatus.conflict);
        expect(
          peer.snapshotCount,
          0,
          reason: 'background delivery must not read board artwork implicitly',
        );

        final local = owner.documentSession.document;
        await _drive(owner.resolveConflict(useBoard: false));
        expect(peer.snapshotCount, 0);
        expect(sameLitchcraftDocument(peer.document, local), isTrue);
        expect(owner.status, LitchcraftStatus.offline);
      },
    );

    test(
      'live Timeline is authoritative, extrapolates locally, and never polls',
      () async {
        peer.selectedFrame = 1;
        peer.playing = true;
        peer.elapsedMilliseconds = 1500;
        await _drive(owner.enter());
        expect(owner.animationBoardAuthorityLive, isTrue);
        expect(owner.animationPlaying, isTrue);
        expect(owner.animationSelectedFrameIndex, 1);
        expect(owner.documentSession.selectedFrameIndex, 1);
        final before = owner.animationElapsedMilliseconds;
        await Future<void>.delayed(Duration.zero);
        expect(
          owner.animationElapsedMilliseconds,
          greaterThanOrEqualTo(before),
        );
        final timelineOperations = peer.operations
            .where((operation) => operation == LitchcraftV2Protocol.timeline)
            .length;
        peer.selectedFrame = 0;
        peer.playing = false;
        peer.elapsedMilliseconds = 900;
        peer.timeline();
        await _until(
          () =>
              !owner.animationPlaying && owner.animationSelectedFrameIndex == 0,
        );
        expect(owner.documentSession.selectedFrameIndex, 0);
        expect(
          peer.operations
              .where((operation) => operation == LitchcraftV2Protocol.timeline)
              .length,
          timelineOperations,
          reason: 'no BLE polling',
        );
      },
    );

    test(
      'rapid animated-artwork taps confirm accepted prefixes without a false conflict',
      () async {
        await _drive(owner.enter());
        final initialSnapshots = peer.snapshotCount;
        final releaseFirstAck = Completer<void>();
        var paintAttempts = 0;
        peer.beforePaint = () async {
          paintAttempts++;
          if (paintAttempts == 1) await releaseFirstAck.future;
        };

        owner.beginStroke();
        owner.paintSquares(const <int>[7]);
        owner.endStroke();
        owner.beginStroke();
        owner.paintSquares(const <int>[8]);
        owner.endStroke();
        final latest = owner.documentSession.document;
        await _until(() => paintAttempts == 1);
        releaseFirstAck.complete();
        await _idle(owner);

        expect(paintAttempts, 2);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, latest),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, latest), isTrue);
        expect(
          peer.snapshotCount,
          initialSnapshots,
          reason:
              'accepted taps are exact evidence; no per-tap snapshot is needed',
        );
      },
    );

    test(
      'board selection presents its immutable frame without replacing it',
      () async {
        final distinct = LitchcraftDocumentSession(peer.document)
          ..selectFrame(1)
          ..replaceCanvas(LitchcraftCanvas.blank().paint([0], 15, 0x2468ac));
        peer.document = distinct.document;
        await _drive(owner.enter());
        final before = encodeLitchcraftAnimationDocument(
          owner.documentSession.document,
        ).bytes;
        peer.selectedFrame = 1;
        peer.playing = false;
        peer.advancedState(0);
        await _until(() => owner.animationSelectedFrameIndex == 1);
        expect(owner.canvas.colors[0], 0x2468ac);
        expect(
          encodeLitchcraftAnimationDocument(
            owner.documentSession.document,
          ).bytes,
          before,
          reason: 'selection is presentation, never an RGB document edit',
        );
        final replacements = peer.operations
            .where(
              (op) =>
                  (op == LitchcraftV2Protocol.streamBegin ||
                  op == LitchcraftV2Protocol.replaceBeginOp),
            )
            .length;
        peer.revision++;
        peer.advancedEvent(LitchcraftV2Protocol.changed, 0);
        await _drive(owner.coherentDocument());
        peer.connection(false);
        peer.connection(true);
        await _drive(owner.settleModeResolution());
        expect(
          peer.operations.where(
            (op) =>
                (op == LitchcraftV2Protocol.streamBegin ||
                op == LitchcraftV2Protocol.replaceBeginOp),
          ),
          hasLength(replacements),
          reason:
              'Changed/reconnect must not turn a board selection into Replace',
        );
      },
    );

    test(
      'runtime State fences stale revisions and treats newer State as a watermark',
      () async {
        peer.selectedFrame = 1;
        peer.playing = true;
        peer.elapsedMilliseconds = 1500;
        await _drive(owner.enter());
        final current = peer.revision;
        peer.revision = current - 1;
        peer.selectedFrame = 0;
        peer.playing = false;
        peer.advancedState(0);
        peer.revision = current;
        expect(owner.animationSelectedFrameIndex, 1);
        expect(owner.animationPlaying, isTrue);
        peer.revision = current + 1;
        peer.advancedState(0);
        expect(owner.animationSelectedFrameIndex, 1);
        expect(owner.animationPlaying, isTrue);
        await _drive(owner.coherentDocument());
        expect(owner.animationSelectedFrameIndex, 0);
        expect(owner.animationPlaying, isFalse);
      },
    );

    test(
      'newer unsolicited inactive State detaches before it can request a snapshot',
      () async {
        await _drive(owner.enter());
        final operations = peer.operations.length;
        peer.revision++;
        peer.active = false;
        peer.advancedState(0);
        await _idle(owner);

        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.selected, isFalse);
        expect(owner.animationBoardAuthorityLive, isFalse);
        expect(
          peer.operations.skip(operations),
          isNot(contains(LitchcraftV2Protocol.snapshot)),
          reason: 'physical Exit must cancel sync before the newer watermark',
        );
        expect(
          peer.operations.skip(operations),
          isNot(
            anyOf(
              contains(LitchcraftV2Protocol.streamBegin),
              contains(LitchcraftV2Protocol.replaceBeginOp),
            ),
          ),
          reason: 'a detached app must never upload over the physical exit',
        );
      },
    );

    test(
      'compact physical Exit notifies advanced artwork before any later packet',
      () async {
        await _drive(owner.enter());
        final operations = peer.operations.length;
        final observedModes = <bool>[];
        owner.addListener(() => observedModes.add(owner.boardActive));

        peer.active = false;
        peer.state(0, atRevision: peer.revision - 1);
        expect(owner.boardActive, isTrue);
        expect(observedModes, isEmpty);
        peer.revision++;
        peer.state(0);

        expect(owner.boardActive, isFalse);
        expect(owner.selected, isFalse);
        expect(
          observedModes,
          contains(false),
          reason: 'Play routing cannot wait for another artwork notification',
        );
        peer.advancedState(0);
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.inspectedBoardActive, isFalse);
        expect(peer.operations, hasLength(operations));
      },
    );

    test(
      'derived playback frame crosses a boundary without a Timeline poll',
      () async {
        final timed = LitchcraftDocumentSession(peer.document)
          ..setSelectedFrameDuration(100)
          ..selectFrame(1)
          ..setSelectedFrameDuration(100);
        peer.document = timed.document;
        peer.selectedFrame = 0;
        peer.playing = true;
        peer.elapsedMilliseconds = 0;
        await _drive(owner.enter());
        final operations = peer.operations.length;
        await Future<void>.delayed(const Duration(milliseconds: 120));
        expect(owner.animationSelectedFrameIndex, 1);
        await _drive(owner.toggleAnimationPlayback());
        expect(peer.timelineFrames.last, 1);
        expect(peer.operations, hasLength(operations + 1));
      },
    );

    test(
      'play, pause, selection, and painting preserve board authority',
      () async {
        await _drive(owner.enter());
        await _drive(owner.toggleAnimationPlayback());
        expect(peer.playing, isTrue);
        await _drive(owner.selectAnimationFrame(1));
        expect(peer.playing, isFalse);
        expect(peer.selectedFrame, 1);
        peer.playing = true;
        peer.timeline();
        await _until(() => owner.animationPlaying);
        owner.beginStroke();
        owner.paintSquares([0]);
        owner.endStroke();
        await _idle(owner);
        final timeline = peer.operations.where(
          (operation) => operation == LitchcraftV2Protocol.timeline,
        );
        expect(
          timeline,
          hasLength(2),
          reason:
              'Paint pauses atomically and must not wait behind a third Timeline command',
        );
        expect(peer.playing, isFalse);
      },
    );

    test(
      'live effect and frame controls mutate through board authority',
      () async {
        await _drive(owner.enter());

        await _drive(
          owner.applyAnimationEffect(
            square: 3,
            mask: 5,
            effect: LitchcraftEffectStyle.breathe,
            effectDurationMs: 1200,
          ),
        );

        expect(peer.effectCount, 1);
        expect(
          owner.documentSession.document.frames[0].emitters[12].effect,
          LitchcraftEffectStyle.breathe,
        );
        expect(
          owner.documentSession.document.frames[0].emitters[14].effect,
          LitchcraftEffectStyle.breathe,
        );

        final initialFrames = owner.documentSession.document.frames.length;
        await _drive(owner.addAnimationFrame());
        expect(peer.addFrameCount, 1);
        expect(owner.documentSession.document.frames.length, initialFrames + 1);
        expect(owner.animationSelectedFrameIndex, 1);

        await _drive(owner.deleteAnimationFrame());
        expect(peer.deleteFrameCount, 1);
        expect(owner.documentSession.document.frames.length, initialFrames);
        expect(owner.animationSelectedFrameIndex, 0);
      },
    );

    test('live erase clears effect metadata in one V2 stroke', () async {
      final edited = LitchcraftDocumentSession(peer.document)
        ..selectFrame(0)
        ..applyEffect(
          square: 0,
          mask: 1,
          effect: LitchcraftEffectStyle.flash,
          effectDurationMs: 1700,
        );
      peer.document = edited.document;
      await _drive(owner.enter());
      await _drive(owner.setBrush(eraser: true, targetMask: 1));

      owner.beginStroke();
      owner.paintSquares(const <int>[0]);
      owner.endStroke();
      await _idle(owner);

      expect(peer.effectCount, 1);
      expect(peer.paintCount, 1);
      expect(peer.editStrokes, hasLength(2));
      expect(peer.editStrokes[0], peer.editStrokes[1]);
      expect(peer.editFlags, <int>[1, 2]);
      final emitter = peer.document.frames.first.emitters.first;
      expect(emitter.color, 0);
      expect(emitter.effect, LitchcraftEffectStyle.off);
      expect(emitter.effectDurationMs, 0);
      expect(
        sameLitchcraftDocument(peer.document, owner.documentSession.document),
        isTrue,
      );
    });

    test(
      'live Fill All and Clear All replace one selected frame atomically',
      () async {
        final edited = LitchcraftDocumentSession(peer.document)
          ..selectFrame(1)
          ..applyEffect(
            square: 7,
            mask: 5,
            effect: LitchcraftEffectStyle.rainbow,
          );
        peer.document = edited.document;
        peer.selectedFrame = 1;
        await _drive(owner.enter());
        await _drive(
          owner.setBrush(color: 0x345678, eraser: true, targetMask: 5),
        );
        final untouched = owner.documentSession.document.frames.first;
        final selectedBefore = owner.documentSession.document.frames[1];
        final replacementsBefore = peer.operations
            .where(
              (operation) =>
                  (operation == LitchcraftV2Protocol.streamBegin ||
                  operation == LitchcraftV2Protocol.replaceBeginOp),
            )
            .length;

        await _drive(owner.fillAll());

        final filled = peer.document.frames[1].emitters;
        for (var square = 0; square < 64; square++) {
          expect(filled[square * 4].color, 0x345678);
          expect(filled[square * 4 + 2].color, 0x345678);
          expect(
            filled[square * 4 + 1].color,
            selectedBefore.emitters[square * 4 + 1].color,
          );
          expect(
            filled[square * 4 + 3].color,
            selectedBefore.emitters[square * 4 + 3].color,
          );
        }
        expect(filled[7 * 4].effect, LitchcraftEffectStyle.rainbow);
        expect(
          peer.document.frames.first.emitters.map((emitter) => emitter.color),
          untouched.emitters.map((emitter) => emitter.color),
        );
        expect(
          peer.operations
              .where(
                (operation) =>
                    (operation == LitchcraftV2Protocol.streamBegin ||
                    operation == LitchcraftV2Protocol.replaceBeginOp),
              )
              .length,
          replacementsBefore + 1,
        );

        await _drive(owner.clear());

        expect(
          peer.document.frames[1].emitters.every(
            (emitter) =>
                emitter.color == 0 &&
                emitter.effect == LitchcraftEffectStyle.off &&
                emitter.effectDurationMs == 0,
          ),
          isTrue,
        );
        expect(
          peer.document.frames.first.emitters.map((emitter) => emitter.color),
          untouched.emitters.map((emitter) => emitter.color),
        );
        expect(
          peer.operations
              .where(
                (operation) =>
                    (operation == LitchcraftV2Protocol.streamBegin ||
                    operation == LitchcraftV2Protocol.replaceBeginOp),
              )
              .length,
          replacementsBefore + 2,
        );
      },
    );

    test(
      'live rotation pauses and transforms the displayed effect frame',
      () async {
        final edited = LitchcraftDocumentSession(peer.document)
          ..selectFrame(1)
          ..applyEffect(
            square: 0,
            mask: 1,
            effect: LitchcraftEffectStyle.flash,
            effectDurationMs: 1700,
          );
        peer.document = edited.document;
        await _drive(owner.enter());
        await _drive(owner.selectAnimationFrame(1));
        peer.elapsedMilliseconds =
            owner.documentSession.document.frames[0].durationMs;
        await _drive(owner.toggleAnimationPlayback());
        expect(owner.animationPlaying, isTrue);
        expect(owner.animationSelectedFrameIndex, 1);
        final untouched = owner.documentSession.document.frames[0];
        final replacementsBefore = peer.operations
            .where(
              (operation) =>
                  (operation == LitchcraftV2Protocol.streamBegin ||
                  operation == LitchcraftV2Protocol.replaceBeginOp),
            )
            .length;

        expect(await _drive(owner.rotateArtwork(-1)), isTrue);

        expect(peer.playing, isFalse);
        expect(peer.selectedFrame, 1);
        expect(owner.documentSession.document.frames[0], untouched);
        const destinationEmitter = 56 * 4 + 3;
        final emitter = owner
            .documentSession
            .document
            .frames[1]
            .emitters[destinationEmitter];
        expect(emitter.effect, LitchcraftEffectStyle.flash);
        expect(emitter.effectDurationMs, 1700);
        expect(
          sameLitchcraftDocument(peer.document, owner.documentSession.document),
          isTrue,
        );
        expect(
          peer.operations
              .where(
                (operation) =>
                    (operation == LitchcraftV2Protocol.streamBegin ||
                    operation == LitchcraftV2Protocol.replaceBeginOp),
              )
              .length,
          replacementsBefore + 1,
          reason: 'one gesture has one board replacement boundary',
        );
      },
    );

    test('busy rotation refuses without local edit or success', () async {
      await _drive(owner.enter());
      final before = owner.documentSession.document;
      final replacementsBefore = peer.operations
          .where(
            (operation) =>
                (operation == LitchcraftV2Protocol.streamBegin ||
                operation == LitchcraftV2Protocol.replaceBeginOp),
          )
          .length;
      peer.rejectTimelineBusy = true;

      expect(await _drive(owner.rotateArtwork(1)), isFalse);

      expect(owner.waitingForLift, isTrue);
      expect(
        sameLitchcraftDocument(owner.documentSession.document, before),
        isTrue,
      );
      expect(
        peer.operations
            .where(
              (operation) =>
                  (operation == LitchcraftV2Protocol.streamBegin ||
                  operation == LitchcraftV2Protocol.replaceBeginOp),
            )
            .length,
        replacementsBefore,
      );
    });

    test('full-turn rotation is a no-op without board history', () async {
      await _drive(owner.enter());
      final before = owner.documentSession.document;
      final replacementsBefore = peer.operations
          .where(
            (operation) =>
                (operation == LitchcraftV2Protocol.streamBegin ||
                operation == LitchcraftV2Protocol.replaceBeginOp),
          )
          .length;

      expect(await _drive(owner.rotateArtwork(4)), isFalse);

      expect(
        sameLitchcraftDocument(owner.documentSession.document, before),
        isTrue,
      );
      expect(
        peer.operations
            .where(
              (operation) =>
                  (operation == LitchcraftV2Protocol.streamBegin ||
                  operation == LitchcraftV2Protocol.replaceBeginOp),
            )
            .length,
        replacementsBefore,
      );
    });

    test(
      'late Busy preserves one rotated source for deliberate repair',
      () async {
        final edited = LitchcraftDocumentSession(peer.document)
          ..selectFrame(1)
          ..applyEffect(
            square: 0,
            mask: 1,
            effect: LitchcraftEffectStyle.flash,
            effectDurationMs: 1700,
          );
        peer.document = edited.document;
        await _drive(owner.enter());
        await _drive(owner.selectAnimationFrame(1));
        final confirmed = peer.document;
        peer.rejectReplaceBeginBusy = true;

        expect(await _drive(owner.rotateArtwork(1)), isFalse);

        expect(owner.status, LitchcraftStatus.error);
        expect(
          sameLitchcraftDocument(peer.document, confirmed),
          isTrue,
          reason: 'Busy left board authority at the confirmed document',
        );
        final local = owner.documentSession.document;
        expect(sameLitchcraftDocument(local, confirmed), isFalse);
        const onceRotatedEmitter = 7 * 4 + 1;
        expect(
          local.frames[1].emitters[onceRotatedEmitter].effect,
          LitchcraftEffectStyle.flash,
        );

        peer.rejectReplaceBeginBusy = false;
        await _drive(owner.retryConnection());

        expect(owner.status, LitchcraftStatus.live);
        expect(sameLitchcraftDocument(peer.document, local), isTrue);
        expect(
          peer.document.frames[1].emitters[onceRotatedEmitter].effect,
          LitchcraftEffectStyle.flash,
          reason: 'repair sends retained source without rotating it again',
        );
      },
    );

    test(
      'live document controls preserve authority across Busy and retry after lift',
      () async {
        await _drive(owner.enter());
        final before = owner.documentSession.document;
        peer.rejectDocumentMutationBusy = true;

        await _drive(
          owner.applyAnimationEffect(
            square: 3,
            mask: 1,
            effect: LitchcraftEffectStyle.flash,
          ),
        );
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.waitingForLift, isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, before),
          isTrue,
          reason: 'a Busy refusal cannot become an optimistic local edit',
        );

        peer.rejectDocumentMutationBusy = false;
        await _drive(owner.addAnimationFrame());
        expect(owner.waitingForLift, isFalse);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.addFrameCount, 1);
        expect(owner.documentSession.document.frames, hasLength(3));
      },
    );

    test(
      'frame edits latch the displayed frame before playback crosses',
      () async {
        final distinct = LitchcraftDocumentSession(peer.document)
          ..selectFrame(1)
          ..replaceCanvas(LitchcraftCanvas.blank().paint([0], 15, 0xabcdef))
          ..selectFrame(0)
          ..setSelectedFrameDuration(100)
          ..selectFrame(1)
          ..setSelectedFrameDuration(100);
        peer.document = distinct.document;
        peer.selectedFrame = 0;
        peer.playing = true;
        peer.elapsedMilliseconds = 150;
        await _drive(owner.enter());
        expect(owner.animationSelectedFrameIndex, 1);

        expect(await _drive(owner.addAnimationFrame()), isTrue);
        expect(peer.addFrameCount, 1);
        expect(peer.selectedFrame, 2);
        expect(
          peer.document.frames[2].emitters.first.color,
          0xabcdef,
          reason: 'Add copied the displayed frame after selecting it on board',
        );
      },
    );

    test(
      'delete token keeps dialog target exact while board selection changes',
      () async {
        final distinct = LitchcraftDocumentSession(peer.document)
          ..selectFrame(1)
          ..replaceCanvas(LitchcraftCanvas.blank().paint([0], 15, 0xabcdef))
          ..selectFrame(0);
        peer.document = distinct.document;
        await _drive(owner.enter());
        final target = await _drive(owner.prepareAnimationFrameEdit());
        expect(target?.frame, 0);

        peer.selectedFrame = 1;
        peer.advancedState(0);
        peer.timeline();
        await _until(() => owner.animationSelectedFrameIndex == 1);
        expect(
          await _drive(owner.deleteAnimationFrame(target: target)),
          isTrue,
        );
        expect(peer.deleteFrameCount, 1);
        expect(peer.document.frames, hasLength(1));
        expect(peer.document.frames.single.emitters.first.color, 0xabcdef);
      },
    );

    test(
      'effect reports false on revision refusal and reconnect cancellation',
      () async {
        await _drive(owner.enter());
        peer.rejectNextEffectStatus = 9;
        expect(
          await _drive(
            owner.applyAnimationEffect(
              square: 2,
              mask: 1,
              effect: LitchcraftEffectStyle.flash,
            ),
          ),
          isFalse,
        );
        expect(peer.effectCount, 0);

        owner.dispose();
        await peer.shutdown();
        peer = _AdvancedBle();
        owner = LitchcraftController(peer);
        await _drive(owner.enter());
        peer.beforeEffect = () async => peer.connection(false);
        expect(
          await _drive(
            owner.applyAnimationEffect(
              square: 2,
              mask: 1,
              effect: LitchcraftEffectStyle.flash,
            ),
          ),
          isFalse,
        );
        expect(owner.status, LitchcraftStatus.offline);
        expect(peer.effectCount, 0);
      },
    );

    test(
      'same RGB different effects on both sides require whole document choice',
      () async {
        await _drive(owner.enter());
        peer.connection(false);
        owner.documentSession.applyEffect(
          square: 0,
          mask: 1,
          effect: LitchcraftEffectStyle.flash,
        );
        final board = LitchcraftDocumentSession(peer.document)
          ..applyEffect(
            square: 0,
            mask: 1,
            effect: LitchcraftEffectStyle.breathe,
          )
          ..selectFrame(1)
          ..replaceCanvas(LitchcraftCanvas.blank().paint([0], 15, 0xabcdef));
        peer.document = board.document;
        peer.selectedFrame = 1;
        peer.revision++;
        peer.connection(true);
        await Future<void>.delayed(Duration.zero);
        await _drive(owner.settleModeResolution());
        expect(owner.status, LitchcraftStatus.conflict);
        expect(
          owner.boardCanvas.colors.first,
          0xabcdef,
          reason:
              'the conflict preview must show the selected physical document frame, not the stale static canvas',
        );
        expect(
          owner.documentSession.document.frames[0].emitters[0].effect,
          LitchcraftEffectStyle.flash,
        );
        expect(
          peer.operations,
          isNot(
            anyOf(
              contains(LitchcraftV2Protocol.streamBegin),
              contains(LitchcraftV2Protocol.replaceBeginOp),
            ),
          ),
        );
      },
    );

    test(
      'Changed snapshot preserves a local effect edited while bytes are pending',
      () async {
        await _drive(owner.enter());
        final board = LitchcraftDocumentSession(peer.document)
          ..setSelectedFrameDuration(2200);
        peer.document = board.document;
        peer.revision++;
        peer.beforeSnapshot = () async {
          owner.documentSession.applyEffect(
            square: 0,
            mask: 1,
            effect: LitchcraftEffectStyle.rainbow,
          );
        };
        peer.advancedEvent(LitchcraftV2Protocol.changed, 0);
        await _drive(owner.coherentDocument());
        expect(
          owner.documentSession.document.frames[0].emitters[0].effect,
          LitchcraftEffectStyle.rainbow,
        );
        expect(owner.status, LitchcraftStatus.conflict);
      },
    );

    test(
      'source replacement during snapshot cannot adopt old board into new work',
      () async {
        final replacement = LitchcraftDocumentSession(peer.document)
          ..setSelectedFrameDuration(3100);
        peer.beforeSnapshot = () async {
          owner.restoreDocument(replacement.document);
        };
        await expectLater(
          _drive(owner.enter()),
          throwsA(isA<LitchcraftTransferCancelled>()),
        );
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            replacement.document,
          ),
          isTrue,
        );
        expect(owner.documentSession.boardDocument, isNull);
        expect(owner.status, LitchcraftStatus.offline);
      },
    );

    test(
      'interrupted replacement completion cannot replace newer local source',
      () async {
        await _drive(owner.enter());
        final upload = LitchcraftDocumentSession(peer.document)
          ..setSelectedFrameDuration(1800);
        final newer = LitchcraftDocumentSession(peer.document)
          ..setSelectedFrameDuration(3900);
        peer.beforeCommit = () async {
          owner.restoreDocument(newer.document);
        };
        // Source cancellation is handled by the ordered controller lane;
        // it must preserve the newer source without surfacing transport failure.
        await _drive(owner.replaceDocument(upload.document));
        expect(
          sameLitchcraftDocument(
            owner.documentSession.document,
            newer.document,
          ),
          isTrue,
        );
        expect(
          sameLitchcraftDocument(
            owner.documentSession.boardDocument!,
            newer.document,
          ),
          isFalse,
        );
        expect(owner.documentSession.canUndo, isFalse);
      },
    );

    test(
      'disconnect during snapshot preserves complete source through repair',
      () async {
        await _drive(owner.enter());
        peer.requests.clear();
        final previous = owner.documentSession.document;
        peer.revision++;
        peer.beforeSnapshot = () async {
          peer.connection(false);
        };
        peer.advancedEvent(LitchcraftV2Protocol.changed, 0);
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.offline);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, previous),
          isTrue,
        );
        peer.beforeSnapshot = null;
        peer.connection(true);
        await Future<void>.delayed(Duration.zero);
        await _drive(owner.settleModeResolution());
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.requests, isEmpty);
      },
    );

    test('inactive board on reconnect is never admitted', () async {
      await _drive(owner.enter());
      peer.requests.clear();
      peer.connection(false);
      peer.active = false;
      peer.connection(true);
      await Future<void>.delayed(Duration.zero);
      await _drive(owner.settleModeResolution());
      expect(owner.status, LitchcraftStatus.offline);
      expect(peer.requests, isEmpty);
    });

    test('one-sided board change adopts all frames without upload', () async {
      await _drive(owner.enter());
      peer.connection(false);
      final board = LitchcraftDocumentSession(peer.document)
        ..setSelectedFrameDuration(2400);
      peer.document = board.document;
      peer.revision++;
      peer.connection(true);
      await Future<void>.delayed(Duration.zero);
      await _drive(owner.settleModeResolution());
      expect(
        sameLitchcraftDocument(owner.documentSession.document, peer.document),
        isTrue,
      );
      expect(
        peer.operations,
        isNot(
          anyOf(
            contains(LitchcraftV2Protocol.streamBegin),
            contains(LitchcraftV2Protocol.replaceBeginOp),
          ),
        ),
      );
    });

    test(
      'one-sided offline app change sends full document on equal board authority',
      () async {
        await _drive(owner.enter());
        peer.connection(false);
        owner.documentSession.setSelectedFrameDuration(2400);
        peer.connection(true);
        await Future<void>.delayed(Duration.zero);
        await _drive(owner.settleModeResolution());
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
        expect(
          peer.operations,
          anyOf(
            contains(LitchcraftV2Protocol.streamCommit),
            contains(LitchcraftV2Protocol.replaceCommitOp),
          ),
        );
      },
    );
  });
  late _Ble ble;
  late LitchcraftController controller;
  setUp(() {
    ble = _Ble();
    controller = LitchcraftController(
      ble,
      physicalColorPickedDuration: const Duration(milliseconds: 20),
      physicalShakeEraseDuration: const Duration(milliseconds: 20),
    );
  });
  tearDown(() async {
    controller.dispose();
    await ble.shutdown();
  });

  for (final fault in [
    'stale before reset',
    'departure before reset',
    'rejected',
    'disconnect',
    'stale connection',
    'departure',
    'occupied after acknowledgement',
    'unconfirmed setup',
    'duplicate',
  ]) {
    test('game entry discard guard: $fault', () async {
      HomeStateNotification home(
        int sequence, {
        bool setup = false,
        bool occupied = false,
      }) => HomeStateNotification.tryParse(
        jsonEncode({
          'event': 'home_state',
          'v': 1,
          'seq': sequence,
          'a': 'corr',
          'fen': setup
              ? 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
              : 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1',
          'occ': occupied ? '0000000000000001' : '0000000000000000',
          'g': 'c',
          'duck': '--',
          'out': 'n',
          'win': 'n',
          'rot': 0,
          'white': 1,
          'pm': setup ? 1 : 0,
          'fs': setup ? 1 : 0,
          'cl': [0, 0, 0, 'w', 0, 0, 0, setup ? 'y' : 'n'],
        }),
      )!;
      ble.active = false;
      ble.home = home(10);
      var refreshes = 0;
      ble.onHomeRefresh = () async {
        refreshes++;
        if (refreshes == 2) {
          if (fault == 'stale before reset') ble.connectionToken++;
          if (fault == 'departure before reset') await controller.leave();
        }
        return true;
      };
      final firstEntry = controller.enter();
      final secondEntry = controller.enter();
      await _drive(Future.wait([firstEntry, secondEntry]));
      final choice = controller.gameEntryChoice;
      expect(choice, isNotNull);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        isEmpty,
      );
      var resets = 0;
      ble.onControlAction = (action) async {
        expect(action, LitchControlAction.newGame);
        resets++;
        if (fault == 'disconnect') ble.connection(false);
        if (fault == 'stale connection') ble.connectionToken++;
        if (fault == 'departure') await controller.leave();
        if (fault == 'rejected') {
          return const LitchControlEvent(
            action: LitchControlAction.newGame,
            phase: LitchControlPhase.rejected,
            reason: LitchMutationReason.busy,
          );
        }
        ble.home = home(
          11,
          setup: fault != 'unconfirmed setup',
          occupied: fault == 'occupied after acknowledgement',
        );
        return const LitchControlEvent(
          action: LitchControlAction.newGame,
          phase: LitchControlPhase.applied,
        );
      };
      final first = controller.discardGameAndEnter(choice!);
      final second = controller.discardGameAndEnter(choice);
      await _drive(Future.wait([first, second]));
      final expectedResets = fault.endsWith('before reset') ? 0 : 1;
      expect(resets, expectedResets);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(fault == 'duplicate' ? 1 : 0),
      );
      expect(controller.boardActive, fault == 'duplicate');
      if (fault != 'duplicate') {
        expect(
          await controller.discardGameAndEnter(choice),
          isFalse,
          reason: 'A retired choice cannot reset another lifetime',
        );
        expect(resets, expectedResets);
      }
    });
  }

  test(
    'active-game refusal fetches fresh Home without a prior snapshot',
    () async {
      ble.active = false;
      ble.enterStatus = 1;
      ble.home = null;
      var refreshes = 0;
      ble.onHomeRefresh = () async {
        refreshes++;
        ble.home = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":11,"a":"corr","fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1","occ":"0000000000000000","g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":0,"cl":[0,0,0,"b",0,0,0,"n"]}',
        )!;
        return true;
      };
      await _drive(controller.enter());
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(1),
      );
      expect(refreshes, 1);
      expect(controller.gameEntryChoice, isNotNull);
      final empty = ble.home!;
      controller.keepGameForLocalArtwork(controller.gameEntryChoice!);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(1),
      );
      ble.home = HomeStateNotification(
        sequence: 9,
        action: empty.action,
        position: empty.position,
        gameStatus: empty.gameStatus,
        physicalStatus: empty.physicalStatus.copyWith(
          occupiedSquares: BigInt.one,
        ),
        freshSetup: empty.freshSetup,
        vexMoveFirst: empty.vexMoveFirst,
        clockStatus: empty.clockStatus,
      );
      await _drive(controller.enter());
      expect(
        refreshes,
        2,
        reason: 'Typed refusal refreshes a stale occupied cache too',
      );
      expect(controller.gameEntryChoice, isNotNull);
      controller.keepGameForLocalArtwork(controller.gameEntryChoice!);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(2),
      );
    },
  );

  testWidgets(
    'unconfirmed game reset exposes persistent Retry and recovers through fresh Setup',
    (tester) async {
      SharedPreferences.setMockInitialValues({});
      ble.active = false;
      ble.home = HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":10,"a":"corr","fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1","occ":"0000000000000000","g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":0,"cl":[0,0,0,"b",0,0,0,"n"]}',
      )!;
      var refreshes = 0, resets = 0;
      ble.onHomeRefresh = () async => ![3, 4].contains(++refreshes);
      ble.onControlAction = (action) async {
        resets++;
        ble.home = HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":11,"a":"corr","fen":"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1","occ":"0000000000000000","g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":1,"fs":1,"cl":[0,0,0,"w",0,0,0,"y"]}',
        )!;
        return const LitchControlEvent(
          action: LitchControlAction.newGame,
          phase: LitchControlPhase.applied,
        );
      };
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      controller.enter();
      await _pumpTransport(tester);
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const ValueKey('craft-game-entry-discard')));
      await _pumpTransport(tester);
      await tester.pump(const Duration(seconds: 4));
      expect(controller.status, LitchcraftStatus.error);
      expect(resets, 1);
      final retry = find.byKey(const ValueKey('craft-game-entry-retry'));
      expect(retry, findsOneWidget);
      await tester.tap(retry);
      await _pumpTransport(tester);
      expect(
        retry,
        findsOneWidget,
        reason: 'An unavailable fresh Home keeps recovery actionable',
      );
      expect(controller.boardActive, isFalse);
      expect(resets, 1);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        isEmpty,
      );
      await tester.tap(retry);
      await _pumpTransport(tester);
      expect(controller.boardActive, isTrue);
      expect(
        resets,
        1,
        reason: 'Fresh Setup permits admission without repeating NewGame',
      );
      expect(ble.connectionSessionToken, 0);
      expect(ble.isConnected, isTrue);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(1),
      );
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
    },
  );

  testWidgets('visible game-entry Retry reuses its fresh ongoing-game Home', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues({});
    ble.active = false;
    ble.enterStatus = 1;
    ble.home = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":10,"a":"corr","fen":"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1","occ":"0000000000000000","g":"c","duck":"--","out":"n","win":"n","rot":0,"white":1,"pm":0,"fs":0,"cl":[0,0,0,"b",0,0,0,"n"]}',
    )!;
    var refreshes = 0, resets = 0;
    ble.onHomeRefresh = () async => ++refreshes <= 3;
    ble.onControlAction = (action) async {
      resets++;
      return const LitchControlEvent(
        action: LitchControlAction.newGame,
        phase: LitchControlPhase.rejected,
        reason: LitchMutationReason.busy,
      );
    };
    final library = LitchcraftLibrary(controller);
    await library.ready;
    try {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: LitchcraftScreen(controller: controller, library: library),
          ),
        ),
      );
      controller.enter();
      await _pumpTransport(tester);
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const ValueKey('craft-game-entry-discard')));
      await _pumpTransport(tester);
      await tester.pump(const Duration(seconds: 4));
      expect(resets, 1);
      expect(refreshes, 2);
      final retry = find.byKey(const ValueKey('craft-game-entry-retry'));
      expect(retry, findsOneWidget);
      await tester.tap(retry);
      await _pumpTransport(tester);
      await tester.pump(const Duration(milliseconds: 300));
      expect(
        find.byKey(const ValueKey('craft-game-entry-choice')),
        findsOneWidget,
        reason:
            'One verified retry Home must preserve the choice when a redundant read would be unavailable',
      );
      expect(refreshes, 3, reason: 'Retry consumes its fresh Home once');
      expect(resets, 1, reason: 'Retry must not repeat the rejected reset');
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        isEmpty,
      );
      await tester.binding.handlePopRoute();
      await _pumpTransport(tester);
      expect(controller.gameEntryChoice, isNull);
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      library.dispose();
    }
  });

  test(
    'unsupported board never flattens an animated local draft into old still transport',
    () async {
      await _enterUsingBoard(controller);
      final work = LitchcraftDocumentSession(
        staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([3], 15, 0xfedcba),
        ),
      )..addFrame();
      final requestsBefore = ble.requests.length;
      final bulkBefore = ble.bulkFrames.length;
      await controller.replaceDocument(work.document);
      ble.connection(false);
      ble.connection(true);
      await Future<void>.delayed(Duration.zero);
      await _drive(controller.settleModeResolution());
      expect(controller.status, LitchcraftStatus.unsupported);
      expect(ble.requests.length, requestsBefore);
      expect(ble.bulkFrames.length, bulkBefore);
      expect(
        sameLitchcraftDocument(
          controller.documentSession.document,
          work.document,
        ),
        isTrue,
      );
    },
  );

  testWidgets('continuous phone shake requests the board gesture once', (
    tester,
  ) async {
    SharedPreferences.setMockInitialValues(<String, Object>{});
    ble.artwork = LitchcraftCanvas.blank().paint(const <int>[4], 15, 0x336699);
    await tester.runAsync(() => _enterUsingBoard(controller));
    final samples = StreamController<LitchcraftMotionSample>.broadcast(
      sync: true,
    );
    var now = 0;
    var triggers = 0;
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(
            controller: controller,
            library: library,
            phoneMotionSamples: samples.stream,
            monotonicMilliseconds: () => now,
            phoneShakeAction: () async => triggers++,
          ),
        ),
      ),
    );
    expect(controller.selected, isTrue);
    expect(controller.status, LitchcraftStatus.live);
    expect(samples.hasListener, isTrue);
    for (var sample = 0; sample <= 24; sample++) {
      now = sample * 50;
      final withinLobe = now % 250;
      final acceleration = withinLobe >= 200
          ? 0.0
          : (now ~/ 250).isEven
          ? 15.0
          : -15.0;
      samples.add(LitchcraftMotionSample(acceleration, 0, 0));
      await tester.pump();
    }
    expect(triggers, 1);

    for (var sample = 25; sample <= 50; sample++) {
      now = sample * 50;
      final acceleration = (now ~/ 250).isEven ? 15.0 : -15.0;
      samples.add(LitchcraftMotionSample(acceleration, 0, 0));
    }
    await tester.pump();
    expect(triggers, 1, reason: 'continued shaking remains one episode');
    await tester.pumpWidget(const SizedBox());
    await samples.close();
    library.dispose();
  });

  testWidgets(
    'phone shake cancels during a pointer, color pick, and visible tray',
    (tester) async {
      _usePhoneViewport(tester);
      SharedPreferences.setMockInitialValues(<String, Object>{});
      ble.artwork = LitchcraftCanvas.blank().paint(
        const <int>[4],
        15,
        0x336699,
      );
      await tester.runAsync(() => _enterUsingBoard(controller));
      final samples = StreamController<LitchcraftMotionSample>.broadcast(
        sync: true,
      );
      var now = 0;
      var triggers = 0;
      final library = LitchcraftLibrary(controller);
      await library.ready;
      await tester.pumpWidget(
        MaterialApp(
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Scaffold(
            body: LitchcraftScreen(
              controller: controller,
              library: library,
              phoneMotionSamples: samples.stream,
              monotonicMilliseconds: () => now,
              phoneShakeAction: () async => triggers++,
            ),
          ),
        ),
      );

      Future<void> emitShake(int startMs) async {
        for (var elapsed = 0; elapsed <= 1200; elapsed += 50) {
          now = startMs + elapsed;
          final withinLobe = elapsed % 250;
          final acceleration = withinLobe >= 200
              ? 0.0
              : (elapsed ~/ 250).isEven
              ? 15.0
              : -15.0;
          samples.add(LitchcraftMotionSample(acceleration, 0, 0));
          await tester.pump();
        }
      }

      final pointer = await tester.startGesture(const Offset(200, 200));
      await emitShake(0);
      expect(triggers, 0, reason: 'an active pointer owns the canvas gesture');
      await pointer.up();
      await _pumpTransport(tester);

      await tester.tap(find.byKey(const ValueKey('craft-inspector-hex')));
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-hex-pad')), findsOneWidget);
      await emitShake(1300);
      expect(triggers, 0, reason: 'a visible controls tray blocks shaking');
      await tester.tap(find.byKey(const ValueKey('craft-hex-apply')));
      await tester.pumpAndSettle();
      expect(find.byKey(const ValueKey('craft-hex-pad')), findsNothing);

      ble.colorPick(LitchcraftProtocol.colorPickPicking);
      await tester.runAsync(
        () => _until(() => controller.physicalColorPicking),
      );
      await tester.pump();
      await emitShake(2600);
      expect(triggers, 0, reason: 'physical color picking blocks shaking');
      ble.colorPick(LitchcraftProtocol.colorPickIdle);
      await tester.runAsync(
        () => _until(() => !controller.physicalColorPicking),
      );
      await tester.pump();

      await emitShake(3900);
      expect(triggers, 1, reason: 'a fresh idle shake is accepted');

      await tester.pumpWidget(const SizedBox());
      await samples.close();
      library.dispose();
    },
  );

  testWidgets('shake guidance is localized and stays outside Litchcraft', (
    tester,
  ) async {
    final oneSecond = {
      'de': RegExp(r'\b(?:1|eine) Sekunde\b'),
      'en': RegExp(r'\b(?:1|one) second\b'),
      'es': RegExp(r'\b(?:1|un) segundo\b'),
      'fr': RegExp(r'\b(?:1|une) seconde\b'),
      'it': RegExp(r'\b(?:1|un) secondo\b'),
    };
    final localizedHeadings = <String>{};
    for (final locale in AppLocalizations.supportedLocales) {
      late AppLocalizations strings;
      await tester.pumpWidget(
        MaterialApp(
          locale: locale,
          localizationsDelegates: AppLocalizations.localizationsDelegates,
          supportedLocales: AppLocalizations.supportedLocales,
          home: Builder(
            builder: (context) {
              strings = AppLocalizations.of(context);
              return const SizedBox();
            },
          ),
        ),
      );
      localizedHeadings.add(strings.craftShakeEraseHeading);
      final duration = oneSecond[locale.languageCode]!;
      expect(strings.craftShakeEraseHelp, contains(duration));
      expect(strings.craftIdleTipShake, contains(duration));
    }
    expect(localizedHeadings, hasLength(greaterThanOrEqualTo(5)));

    SharedPreferences.setMockInitialValues(<String, Object>{});
    ble.shakeEraseSupported = false;
    await tester.runAsync(() => _enterUsingBoard(controller));
    final library = LitchcraftLibrary(controller);
    await library.ready;
    await tester.pumpWidget(
      MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Scaffold(
          body: LitchcraftScreen(controller: controller, library: library),
        ),
      ),
    );
    final strings = AppLocalizations.of(
      tester.element(find.byType(LitchcraftScreen)),
    );
    expect(find.byKey(const ValueKey('craft-board-controls')), findsNothing);
    expect(find.text(strings.craftShakeEraseHeading), findsNothing);
    expect(find.text(strings.craftShakeEraseHelp), findsNothing);
    await tester.pumpWidget(const SizedBox());
    library.dispose();
  });

  test('phone erase uses the board-owned gesture transaction', () async {
    ble.artwork = LitchcraftCanvas.blank().paint(const <int>[4], 15, 0x336699);
    await _enterUsingBoard(controller);

    await _drive(controller.eraseDisplayedFrameByPhone());

    expect(
      ble.requests.where(
        (request) => request.opcode == LitchcraftProtocol.shakeErase,
      ),
      hasLength(1),
    );
    expect(ble.artwork.hasMarks, isFalse);
    expect(controller.physicalShakeErased, isTrue);
  });

  test('publishes physical color-pick progress and readable result', () async {
    expect(
      LitchcraftController.physicalColorPickedDisplayDuration,
      const Duration(seconds: 3),
    );
    await _enterUsingBoard(controller);
    expect(controller.status, LitchcraftStatus.live);

    ble.colorPick(LitchcraftProtocol.colorPickPicking);
    await _until(() => controller.physicalColorPicking);
    expect(controller.physicalColorPicking, isTrue);
    expect(controller.physicalColorPicked, isFalse);

    ble.colorPick(LitchcraftProtocol.colorPickPicked);
    await _until(() => controller.physicalColorPicked);
    expect(controller.physicalColorPicking, isFalse);
    expect(controller.physicalColorPicked, isTrue);

    await Future<void>.delayed(const Duration(milliseconds: 30));
    expect(
      controller.physicalColorPicked,
      isFalse,
      reason: 'the picked hint expires even while the piece stays down',
    );
    ble.colorPick(LitchcraftProtocol.colorPickPicked);
    await Future<void>.delayed(Duration.zero);
    expect(
      controller.physicalColorPicked,
      isFalse,
      reason: 'a repeated Picked status cannot restart the hint',
    );
    ble.colorPick(LitchcraftProtocol.colorPickIdle);
    await Future<void>.delayed(Duration.zero);
    expect(controller.physicalColorPicked, isFalse);

    ble.colorPick(LitchcraftProtocol.colorPickPicking);
    ble.colorPick(LitchcraftProtocol.colorPickPicked);
    ble.colorPick(LitchcraftProtocol.colorPickIdle);
    await Future<void>.delayed(const Duration(milliseconds: 10));
    expect(controller.physicalColorPicking, isFalse);
    expect(controller.physicalColorPicked, isTrue);
    await Future<void>.delayed(const Duration(milliseconds: 20));
    expect(controller.physicalColorPicked, isFalse);
  });

  testWidgets(
    'validated shake erase reaches status and rejects stale duplicate or disconnected events',
    (tester) async {
      SharedPreferences.setMockInitialValues(<String, Object>{});
      await tester.runAsync(() => _enterUsingBoard(controller));
      final library = LitchcraftLibrary(controller);
      await library.ready;
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

      ble.emit(
        LitchcraftProtocol.gesture,
        sessionId: controller.sessionId + 1,
        atRevision: controller.revision + 1,
        payload: const <int>[LitchcraftProtocol.gestureShakeErase, 1, 0],
      );
      await tester.pump();
      expect(controller.physicalShakeErased, isFalse);

      final revision = controller.revision + 1;
      ble.revision = revision;
      ble.emit(
        LitchcraftProtocol.gesture,
        sessionId: controller.sessionId,
        atRevision: revision,
        payload: const <int>[LitchcraftProtocol.gestureShakeErase, 2, 0],
      );
      await tester.pump();
      expect(controller.physicalShakeErased, isTrue);
      expect(
        find.text('Erased all squares. To undo, press Undo.'),
        findsOneWidget,
      );

      await tester.runAsync(() async {
        await Future<void>.delayed(const Duration(milliseconds: 15));
        ble.emit(
          LitchcraftProtocol.gesture,
          sessionId: controller.sessionId,
          atRevision: revision,
          payload: const <int>[LitchcraftProtocol.gestureShakeErase, 2, 0],
        );
        await Future<void>.delayed(const Duration(milliseconds: 15));
      });
      await tester.pump();
      expect(
        controller.physicalShakeErased,
        isFalse,
        reason: 'a duplicate cannot extend transient feedback expiry',
      );

      final priorityRevision = controller.revision + 1;
      ble.revision = priorityRevision;
      ble.emit(
        LitchcraftProtocol.gesture,
        sessionId: controller.sessionId,
        atRevision: priorityRevision,
        payload: const <int>[LitchcraftProtocol.gestureShakeErase, 4, 0],
      );
      await tester.pump();
      expect(controller.physicalShakeErased, isTrue);
      ble.packets.addError(StateError('notification transport failed'));
      await tester.pump();
      expect(controller.status, LitchcraftStatus.error);
      expect(
        find.text('Erased all squares. To undo, press Undo.'),
        findsNothing,
        reason: 'connection failure feedback has priority over gesture success',
      );

      ble.connection(false);
      await tester.pump();
      ble.emit(
        LitchcraftProtocol.gesture,
        sessionId: controller.sessionId,
        atRevision: revision + 1,
        payload: const <int>[LitchcraftProtocol.gestureShakeErase, 3, 0],
      );
      await tester.pump();
      expect(controller.physicalShakeErased, isFalse);
      expect(
        find.text('Erased all squares. To undo, press Undo.'),
        findsNothing,
      );

      await tester.pumpWidget(const SizedBox());
      library.dispose();
    },
  );

  test('syncs eight artwork favorites in bounded atomic chunks', () async {
    ble.paletteSyncSupported = true;
    controller.setFavoriteColors(const <int>[
      0x010203,
      0x040506,
      0x070809,
      0x0a0b0c,
      0x0d0e0f,
      0x101112,
      0x131415,
      0x161718,
    ]);
    await _enterUsingBoard(controller);
    await _idle(controller);

    final writes = ble.requests
        .where((request) => request.opcode == LitchcraftProtocol.paletteChunk)
        .toList();
    expect(writes, hasLength(3));
    expect(
      writes.map((request) => request.payload.take(2).toList()),
      <List<int>>[
        <int>[0, 3],
        <int>[3, 3],
        <int>[6, 2],
      ],
    );
    expect(writes.every((request) => request.bytes.length <= 20), isTrue);

    ble.requests.clear();
    controller.setFavoriteColors(const <int>[
      0x161718,
      0x010203,
      0x040506,
      0x070809,
      0x0a0b0c,
      0x0d0e0f,
      0x101112,
      0x131415,
    ]);
    await _idle(controller);
    expect(
      ble.requests.where(
        (request) => request.opcode == LitchcraftProtocol.paletteChunk,
      ),
      hasLength(3),
    );
  });

  test(
    'rapid taps retain one edit lineage when reconnect interrupts a burst',
    () async {
      ble.inspectSupported = true;
      await _drive(controller.enter());
      final blocked = Completer<void>();
      var writes = 0;
      ble.onMutation = (request) async {
        if (request.opcode == LitchcraftProtocol.paint) {
          if (++writes == 1) await blocked.future;
          for (final square in request.payload.skip(3)) {
            ble.artwork = ble.artwork.paint([square], 15, 0xff7430);
            ++ble.revision;
          }
          // Accepted paint, lost Cell and ACK notifications, then link loss.
          ble.connection(false);
          return;
        }
        ble.state(request.id);
      };
      for (var square = 0; square < 32; square++) {
        controller.beginStroke();
        controller.paintSquares([square]);
        controller.endStroke();
      }
      final latest = controller.canvas;
      await _until(() => writes == 1);
      blocked.complete();
      await _idle(controller);
      ble.onMutation = null;
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, latest);
      expect(ble.artwork, latest);
      expect(
        writes,
        1,
        reason: 'old-generation queued taps never write after reconnect',
      );
    },
  );

  test(
    'older paint echoes cannot overwrite a newer tap on the same square',
    () async {
      await _drive(controller.enter());
      final blocked = Completer<void>();
      var writes = 0;
      ble.onMutation = (request) async {
        if (request.opcode == LitchcraftProtocol.tool) {
          ble.brushColor = LitchcraftProtocol.readRgb(request.payload, 2);
        }
        if (request.opcode == LitchcraftProtocol.paint) {
          if (++writes == 1) await blocked.future;
          for (final square in request.payload.skip(3)) {
            ble.artwork = ble.artwork.paint([square], 15, ble.brushColor);
            ++ble.revision;
            ble.emit(
              LitchcraftProtocol.cell,
              requestId: request.id,
              payload: [square, 15, ...LitchcraftProtocol.rgb(ble.brushColor)],
            );
            expect(
              controller.canvas.colors[square * 4],
              0x123456,
              reason:
                  'newer local tap stays visible while older accepted paint is echoed',
            );
          }
        }
        ble.state(request.id);
      };
      controller.paintSquares([7]);
      controller.endStroke();
      await _until(() => writes == 1);
      final tool = controller.setBrush(color: 0x123456);
      controller.paintSquares([7]);
      controller.endStroke();
      expect(controller.canvas.colors[28], 0x123456);
      blocked.complete();
      await _drive(tool);
      await _idle(controller);
      expect(controller.canvas, ble.artwork);
    },
  );

  test(
    'foreground current metadata refresh preserves the latest admitted tap through its receipt',
    () async {
      await _withCurrentStore((peer, owner) async {
        await _drive(owner.enter());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        owner.beginStroke();
        owner.paintSquares([7]);
        owner.endStroke();
        await _idle(owner);
        final paintStarted = Completer<void>();
        final releasePaint = Completer<void>();
        addTearDown(() {
          if (!releasePaint.isCompleted) releasePaint.complete();
        });
        peer.afterOrderedPaintBeforeCheckpoint = () async {
          if (paintStarted.isCompleted) return;
          paintStarted.complete();
          await releasePaint.future;
        };
        owner.beginStroke();
        owner.paintSquares([8]);
        owner.endStroke();
        final latest = owner.documentSession.document;
        expect(latest.frames[0].emitters[28].color, owner.color);
        expect(latest.frames[0].emitters[32].color, owner.color);
        await paintStarted.future;
        final reads = peer.frameStoreSnapshotTransactionCount;
        final layouts = peer.frameStoreLiveLayoutCount;
        final refreshing = owner.revalidateSelectedBoard();
        expect(
          sameLitchcraftDocument(owner.documentSession.document, latest),
          isTrue,
        );
        expect(
          owner.boardArtworkUnresolved,
          isTrue,
          reason:
              'the refresh gates new taps, not already-admitted local intent',
        );
        owner.beginStroke();
        owner.paintSquares([9]);
        owner.endStroke();
        expect(
          sameLitchcraftDocument(owner.documentSession.document, latest),
          isTrue,
          reason:
              'the public unresolved gate rejects new input without losing accepted taps',
        );
        releasePaint.complete();
        await _drive(refreshing);
        await _idle(owner);
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.boardArtworkUnresolved, isFalse);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, latest),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, latest), isTrue);
        expect(peer.frameStoreSnapshotTransactionCount, reads);
        expect(peer.frameStoreLiveLayoutCount, layouts);
        expect(peer.orderedPaintBodies, hasLength(2));
      });
    },
  );

  testWidgets(
    'lost snapshot end recovers within three seconds without a chooser',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      var reads = 0;
      ble.onSnapshot = (request) {
        if (++reads == 1) {
          ble.state(request.id);
          ble.begin(request.id);
          ble.chunks(request.id, ble.artwork);
        } else {
          ble.snapshot(request.id);
        }
      };
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await _pumpTransport(tester);
      await tester.pump(const Duration(seconds: 2));
      await _pumpTransport(tester);
      await tester.pump(const Duration(milliseconds: 500));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await _pumpTransport(tester);
      expect(
        controller.pending,
        isFalse,
        reason:
            'complete pixel coverage plus a lost end must not stall for 20 seconds',
      );
      expect(controller.status, LitchcraftStatus.live);
      expect(reads, 2);
    },
  );

  testWidgets(
    'lost tap ACK with BLE still connected retains the accepted prefix',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble()..inspectSupported = true;
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      int? acceptedRequest;
      final attempted = <int>[];
      ble.onMutation = (request) async {
        attempted.add(request.id);
        if (acceptedRequest == null) {
          acceptedRequest = request.id;
          ble.artwork = ble.artwork.paint(
            request.payload.skip(3),
            15,
            0xff7430,
          );
          ++ble.revision;
        }
        // The same request may replay; its mutation is accepted exactly once.
      };
      for (var square = 0; square < 32; square++) {
        controller.paintSquares([square]);
        controller.endStroke();
      }
      final latest = controller.canvas;
      await _pumpTransport(tester);
      await tester.pump(const Duration(milliseconds: 250));
      await _pumpTransport(tester);
      await tester.pump(const Duration(milliseconds: 750));
      await _pumpTransport(tester);
      await tester.pump(const Duration(seconds: 6));
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.error);
      expect(ble.connected, isTrue);
      expect(attempted.toSet(), {acceptedRequest});
      ble.onMutation = null;
      await tester.pump(const Duration(milliseconds: 500));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, latest);
      expect(ble.artwork, latest);
    },
  );

  for (final useBoard in [false, true]) {
    test(
      'conflict choice retires rejected pending paint (useBoard=$useBoard)',
      () async {
        await _drive(controller.enter());
        ble.onMutation = (request) async {
          ble.connection(false); // Paint was never accepted.
        };
        controller.paintSquares([7]);
        controller.endStroke();
        await _idle(controller);
        final app = controller.canvas;
        ble.artwork = ble.artwork.paint([63], 15, 0x112233);
        ++ble.revision;
        ble.onMutation = null;
        ble.connection(true);
        await _idle(controller);
        expect(controller.status, LitchcraftStatus.conflict);
        await _drive(controller.resolveConflict(useBoard: useBoard));
        final selected = useBoard ? ble.artwork : app;
        ble.artwork = selected.paint([0], 15, 0xabcdef);
        ++ble.revision;
        ble.emit(LitchcraftProtocol.cell, payload: [0, 15, 0xab, 0xcd, 0xef]);
        expect(controller.canvas, ble.artwork);
        expect(await _drive(controller.coherentSnapshot()), ble.artwork);
      },
    );
  }

  testWidgets(
    'legacy snapshot progress may exceed idle timeout without failing',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      _Request? read;
      ble.onSnapshot = (request) {
        read = request;
        ble.state(request.id);
        ble.begin(request.id);
      };
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await _pumpTransport(tester);
      for (var offset = 254; offset >= 0; offset -= 2) {
        await tester.pump(const Duration(milliseconds: 30));
        ble.chunks(read!.id, ble.artwork, from: offset, until: offset + 2);
        if (offset % 16 == 0) {
          ble.begin(read!.id); // Duplicate begin must not discard coverage.
          ble.chunks(read!.id, ble.artwork, from: offset, until: offset + 2);
        }
      }
      ble.emit(LitchcraftProtocol.snapshotEnd, requestId: read!.id);
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.pending, isFalse);
    },
  );

  for (final delivery in [
    'ordered',
    'ack-first',
    'duplicate',
    'dropped-cells',
  ]) {
    test(
      '64 rapid taps preserve their preview through $delivery notifications',
      () async {
        await _drive(controller.enter());
        final release = Completer<void>();
        var commands = 0;
        var previewPreserved = true;
        ble.onMutation = (request) async {
          if (request.opcode != LitchcraftProtocol.paint) {
            ble.state(request.id);
            return;
          }
          if (++commands == 1) await release.future;
          final square = request.payload[3];
          ble.artwork = ble.artwork.paint([square], 15, 0xff7430);
          ++ble.revision;
          void echo() => ble.emit(
            LitchcraftProtocol.cell,
            requestId: request.id,
            payload: [square, 15, 0xff, 0x74, 0x30],
          );
          if (delivery == 'ack-first') ble.state(request.id);
          if (delivery != 'dropped-cells') echo();
          if (delivery == 'duplicate') {
            echo();
            ble.emit(
              LitchcraftProtocol.cell,
              atRevision: ble.revision - 1,
              requestId: request.id,
              payload: [square, 15, 0, 0, 0],
            );
          }
          ble.state(request.id);
          previewPreserved &= controller.canvas.colors.every(
            (color) => color == 0xff7430,
          );
        };
        for (var square = 0; square < 64; square++) {
          controller.paintSquares([square]);
          controller.endStroke();
          expect(
            controller.canvas.colors[square * 4],
            0xff7430,
            reason: 'tap preview is synchronous, before any ACK',
          );
        }
        await _until(() => commands == 1);
        release.complete();
        await _idle(controller);
        expect(previewPreserved, isTrue);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.canvas, ble.artwork);
        expect(ble.artwork.colors, everyElement(0xff7430));
        expect(commands, 64, reason: 'each tap has one accepted stroke');
        expect(ble.bulkFrames, isEmpty, reason: 'no false fork or replacement');
      },
    );
  }

  testWidgets(
    'tap previews immediately and sends without waiting for stroke timer',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      ble.requests.clear();
      controller.paintSquares([7]);
      expect(controller.canvas.colors[28], 0xff7430);
      controller.endStroke();
      await tester.pump(const Duration(milliseconds: 1));
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        hasLength(1),
        reason:
            'completed tap dispatches in the next event turn; no 25ms flush wait',
      );
      await _pumpTransport(tester);
    },
  );

  testWidgets(
    'held drag dispatches its first compact paint on the next event turn',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      ble.requests.clear();

      controller.beginStroke();
      controller.paintSquares([7]);
      await tester.pump(const Duration(milliseconds: 1));

      final paints = ble.requests
          .where((request) => request.opcode == LitchcraftProtocol.paint)
          .toList();
      expect(paints, hasLength(1));
      expect(paints.single.bytes.length, lessThanOrEqualTo(20));
      expect(
        paints.single.payload.skip(3),
        <int>[7],
        reason: 'a held drag is not paced behind an artificial timer',
      );
      controller.endStroke();
      await _pumpTransport(tester);
    },
  );

  for (final recovery in [
    'same predecessor',
    'changed revision',
    'changed mode',
    'changed boot',
    'changed resident',
  ]) {
    test(
      'current editable replacement and failed following paint retain every admitted tap on reconnect ($recovery)',
      () async {
        await _withCurrentStore((peer, owner) async {
          await _drive(owner.enter());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          await peer.retireSharedConnection();
          peer.connection(true);
          await _drive(owner.settleModeResolution());
          expect(owner.boardArtworkUnresolved, isFalse);
          expect(owner.status, LitchcraftStatus.live);
          final draft = LitchcraftDocumentSession(
            owner.documentSession.document,
          )..paintSelectedFrame(squares: [0], mask: 15, color: 0x66cc44);
          final chunkStarted = Completer<void>();
          final releaseChunk = Completer<void>();
          addTearDown(() {
            if (!releaseChunk.isCompleted) releaseChunk.complete();
          });
          peer.beforeFrameStoreChunk = () async {
            if (chunkStarted.isCompleted) return;
            chunkStarted.complete();
            await releaseChunk.future;
          };
          final replacing = owner.replaceDocument(draft.document);
          await chunkStarted.future;
          expect(
            owner.boardArtworkUnresolved,
            isFalse,
            reason:
                'a proven current predecessor permits editable local delivery',
          );
          owner.beginStroke();
          owner.paintSquares([7]);
          owner.endStroke();
          final stagedTap = owner.documentSession.document;
          expect(stagedTap.frames[0].emitters[28].color, owner.color);
          releaseChunk.complete();
          await _drive(replacing);
          await _untilBackgroundRepair(
            () =>
                !owner.storeReplacementUnresolved &&
                sameLitchcraftDocument(peer.document, stagedTap),
          );
          expect(
            sameLitchcraftDocument(owner.documentSession.document, stagedTap),
            isTrue,
            reason: 'Commit and its queued delta retain the staging-time tap',
          );
          // This journey qualifies reconnect from a clean saved predecessor.
          // Queued paint is now a live delta, so finish its normal save first.
          peer.installSavedStoreDocument(
            stagedTap,
            generation: peer.frameStoreGeneration + 1,
          );
          peer.publishStoreAuthority();
          await _idle(owner);
          peer.beforeOrderedPaint = () async {
            peer.connection(false);
            throw StateError('controlled native Paint write failure');
          };
          owner.beginStroke();
          owner.paintSquares([8]);
          owner.endStroke();
          final latest = owner.documentSession.document;
          expect(latest.frames[0].emitters[32].color, owner.color);
          await _until(() => !peer.connected);
          await _idle(owner);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, latest),
            isTrue,
          );
          expect(sameLitchcraftDocument(peer.document, stagedTap), isTrue);
          peer.beforeOrderedPaint = null;
          await peer.retireSharedConnection();
          switch (recovery) {
            case 'changed revision':
              // A separately accepted paint/Undo can leave identical bytes with
              // a newer live revision; the old receipt cannot prove that path.
              peer.frameStoreLiveRevision++;
            case 'changed mode':
              peer.session++;
            case 'changed boot':
              peer.boardBootNonce++;
            case 'changed resident':
              // Controlled inconsistent metadata must not let a retained live
              // revision certify a different selected resident's bytes.
              final other = LitchcraftDocumentSession(stagedTap)
                ..paintSelectedFrame(squares: [9], mask: 15, color: 0xabcdef);
              peer.installSavedStoreDocument(
                other.document,
                generation: peer.frameStoreGeneration,
              );
          }
          final boardBeforeRecovery = peer.document;
          final recoveryWritesFrom = peer.operations.length;
          final statuses = <LitchcraftStatus>[];
          owner.addListener(() => statuses.add(owner.status));
          peer.connection(true);
          await _drive(owner.settleModeResolution());
          if (owner.connectionRecoveryRequired) {
            await _drive(owner.retryConnection());
          }
          await _untilBackgroundRepair(
            () =>
                !owner.boardArtworkUnresolved &&
                !owner.storeReplacementUnresolved,
          );
          expect(
            sameLitchcraftDocument(owner.documentSession.document, latest),
            isTrue,
            reason: 'Every recovery retains the newest admitted phone tap',
          );
          if (recovery == 'same predecessor') {
            expect(owner.status, LitchcraftStatus.live);
            expect(statuses, isNot(contains(LitchcraftStatus.conflict)));
            expect(sameLitchcraftDocument(peer.document, latest), isTrue);
            expect(
              peer.operations
                  .skip(recoveryWritesFrom)
                  .where(
                    (opcode) => opcode == LitchcraftV2Protocol.frameStoreCommit,
                  ),
              hasLength(1),
              reason:
                  'The existing Store owner commits the preserved draft once',
            );
            owner.beginStroke();
            owner.paintSquares([10]);
            owner.endStroke();
            await _idle(owner);
            expect(owner.status, LitchcraftStatus.live);
            expect(
              sameLitchcraftDocument(
                peer.document,
                owner.documentSession.document,
              ),
              isTrue,
              reason: 'Ordinary painting remains usable after recovery',
            );
            expect(peer.document.frames.first.emitters[40].color, owner.color);
            await _drive(owner.undo());
            expect(owner.status, LitchcraftStatus.live);
            expect(sameLitchcraftDocument(peer.document, latest), isTrue);
            expect(
              sameLitchcraftDocument(owner.documentSession.document, latest),
              isTrue,
              reason: 'Undo after recovery returns to the preserved drawing',
            );
          } else {
            expect(owner.status, LitchcraftStatus.conflict);
            expect(
              sameLitchcraftDocument(peer.document, boardBeforeRecovery),
              isTrue,
            );
            expect(
              peer.operations.skip(recoveryWritesFrom),
              isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
              reason:
                  'Unproven predecessor metadata cannot replace board artwork',
            );
          }
        });
      },
    );
  }

  for (final inspect in [false, true]) {
    test(
      'reconnect reconciles one-sided changes silently (inspect=$inspect)',
      () async {
        ble.inspectSupported = inspect;
        await _drive(controller.enter());
        final statuses = <LitchcraftStatus>[];
        controller.addListener(() => statuses.add(controller.status));
        for (var cycle = 0; cycle < 40; cycle++) {
          ble.connection(false);
          if (cycle.isEven) {
            ble.artwork = ble.artwork.paint([cycle % 64], 15, cycle + 1);
            ++ble.revision;
          } else {
            await controller.replaceCanvas(
              controller.canvas.paint([cycle % 64], 15, cycle + 1),
            );
          }
          ble.connection(true);
          ble.connection(true);
          await _idle(controller);
          expect(
            controller.status,
            LitchcraftStatus.live,
            reason: 'cycle $cycle',
          );
          expect(controller.canvas.colors, ble.artwork.colors);
        }
        expect(statuses, isNot(contains(LitchcraftStatus.conflict)));
      },
    );
  }

  test(
    'fresh unedited canvas adopts authoritative board without a choice',
    () async {
      ble.artwork = ble.artwork.paint([63], 15, 0x123456);
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  testWidgets(
    'same-generation notification failure repairs without false conflict',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);
      ++ble.revision;
      ble.emit(LitchcraftProtocol.cell, payload: [4, 15, 0x12, 0x34, 0x56]);
      await tester.pump();
      final lastCanvas = controller.canvas;
      ble.packets.addError(StateError('notification channel lost'));
      await tester.pump();
      ble.artwork = lastCanvas.paint([5], 15, 0xabcdef);
      ++ble.revision;
      await tester.pump(const Duration(seconds: 2));
      await tester.runAsync(() async {
        await Future<void>.delayed(Duration.zero);
      });
      await _pumpTransport(tester);
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason:
            '${controller.errorMessage}; requests=${ble.requests.map((r) => r.opcode).toList()}; pending=${controller.pending}; subs=${ble.litchcraftSubscriptionCount}',
      );
      expect(controller.canvas, ble.artwork);
      expect(ble.litchcraftSubscriptionCount, 2);
    },
  );

  testWidgets(
    'automatic recovery replaces a link after an incomplete canvas snapshot',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble()..inspectSupported = true;
      final expected = LitchcraftCanvas.blank().paint(<int>[7], 15, 0x2764d8);
      ble.artwork = expected;
      var snapshots = 0;
      ble.onSnapshot = (request) {
        if (++snapshots == 1) {
          ble.state(request.id);
          ble.begin(request.id);
          ble.emit(LitchcraftProtocol.snapshotEnd, requestId: request.id);
        } else {
          ble.snapshot(request.id);
        }
      };
      controller = LitchcraftController(ble);
      controller.restoreDraft(expected);

      controller.enter();
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.error);
      expect(ble.litchcraftSubscriptionCount, 1);

      await tester.pump(const Duration(seconds: 2));
      await tester.runAsync(() async {
        await Future<void>.delayed(Duration.zero);
      });
      await _pumpTransport(tester);

      expect(
        ble.litchcraftSubscriptionCount,
        greaterThanOrEqualTo(2),
        reason:
            'a failed canvas stream must not be reused by automatic recovery',
      );
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, expected);
      expect(
        ble.requests.where(
          (request) => request.opcode == LitchcraftProtocol.enter,
        ),
        hasLength(1),
      );
      controller.dispose();
      controller = LitchcraftController(ble);
    },
  );

  testWidgets(
    'board-rotation transient failure retries while page remains selected',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      ble.cachedBoardRotate = null;
      ble.onReadBoardRotate = () async => null;
      controller.enter();
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.error);
      ble.onReadBoardRotate = () async => true;
      await tester.pump(const Duration(seconds: 2));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await _pumpTransport(tester);
      expect(
        controller.status,
        LitchcraftStatus.live,
        reason: controller.errorMessage,
      );
      expect(controller.flipped, isFalse);
    },
  );

  testWidgets(
    'repeated physical undo invalidations recover with no false conflict',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);
      var snapshots = 0;
      ble.onSnapshot = (request) {
        if (++snapshots <= 4) {
          ++ble.revision;
          ble.state(request.id);
          ble.emit(LitchcraftProtocol.resyncRequired);
        } else {
          ble.snapshot(request.id);
        }
      };
      ++ble.revision;
      ble.artwork = LitchcraftCanvas.blank().paint([2], 15, 0xffcc00);
      ble.emit(LitchcraftProtocol.resyncRequired);
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.error);
      await tester.pump(const Duration(seconds: 1));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(snapshots, 5);
    },
  );

  testWidgets(
    'failed changed-authority snapshot cannot bless stale cached artwork',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble()..inspectSupported = true;
      controller = LitchcraftController(ble);
      controller.enter();
      await _pumpTransport(tester);
      final oldCanvas = controller.canvas;
      final oldRevision = controller.revision;

      ble.artwork = LitchcraftCanvas.blank().paint([9], 15, 0x123456);
      ++ble.revision;
      var snapshots = 0;
      ble.onSnapshot = (request) {
        if (++snapshots <= 2) {
          ble.state(request.id);
          ble.emit(LitchcraftProtocol.resyncRequired);
        } else {
          ble.snapshot(request.id);
        }
      };
      ble.connection(false);
      ble.connection(true);
      // Reconnect now waits for the real stream cancellation event turn.
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.error);
      expect(controller.canvas, oldCanvas);
      expect(controller.revision, oldRevision);

      await tester.pump(const Duration(seconds: 1));
      await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.revision, ble.revision);
      expect(snapshots, 3);
      expect(
        ble.requests.map((request) => request.opcode).toList().sublist(2),
        [
          LitchcraftProtocol.inspect,
          LitchcraftProtocol.snapshot,
          LitchcraftProtocol.snapshot,
          LitchcraftProtocol.inspect,
          LitchcraftProtocol.snapshot,
        ],
      );
    },
  );

  testWidgets('capability transient fallback is retried without manual entry', (
    tester,
  ) async {
    controller.dispose();
    await ble.shutdown();
    ble = _Ble();
    controller = LitchcraftController(ble);
    ble.supported = false;
    controller.enter();
    await tester.pumpAndSettle();
    ble.supported = true;
    await tester.pump(const Duration(seconds: 2));
    await tester.pumpAndSettle();
    expect(controller.status, LitchcraftStatus.live);
  });

  testWidgets('last authoritative piece removal admits selected detached page', (
    tester,
  ) async {
    controller.dispose();
    await ble.shutdown();
    ble = _Ble();
    controller = LitchcraftController(ble);
    ble.enterStatus = 8;
    ble.active = false;
    controller.enter();
    await tester.pumpAndSettle();
    expect(controller.status, LitchcraftStatus.boardNotEmpty);
    ble.enterStatus = 0;
    ble.home = HomeStateNotification.tryParse(
      '{"event":"home_state","v":1,"seq":7,"a":"idle","fen":"8/8/8/8/8/8/8/8 w - - 0 1","occ":"0000000000000000","g":"u","duck":"--","out":"n","win":"n","rot":0,"pm":1,"fs":1,"white":1}',
    );
    expect(ble.home, isNotNull);
    ble.homes.add(ble.home!);
    await tester.pump(const Duration(milliseconds: 250));
    await tester.pumpAndSettle();
    expect(controller.status, LitchcraftStatus.live);
  });

  testWidgets(
    'removing every piece from an in-progress game never auto-enters Litchcraft',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      ble.enterStatus = 8;
      ble.active = false;
      controller.enter();
      await _pumpTransport(tester);
      expect(controller.status, LitchcraftStatus.boardNotEmpty);
      HomeStateNotification state(
        int sequence,
        String occupancy, {
        bool fresh = false,
      }) {
        final fen = fresh
            ? 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
            : 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1';
        return HomeStateNotification.tryParse(
          '{"event":"home_state","v":1,"seq":$sequence,"a":"idle","fen":"$fen","occ":"$occupancy","g":"c","duck":"--","out":"n","win":"n","rot":0,"pm":1,"fs":${fresh ? 1 : 0},"white":1}',
        )!;
      }

      ble.home = state(10, '0000000000000001');
      ble.homes.add(ble.home!);
      ble.home = state(11, '0000000000000000');
      ble.homes.add(ble.home!);
      await tester.pump(const Duration(seconds: 1));
      await _pumpTransport(tester);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(1),
      );
      expect(controller.boardActive, isFalse);
      expect(controller.status, LitchcraftStatus.boardNotEmpty);
      // Fresh setup can end while the removal stability timer is pending.
      ble.home = state(12, '0000000000000000', fresh: true);
      ble.homes.add(ble.home!);
      await tester.pump(const Duration(milliseconds: 100));
      ble.home = state(13, '0000000000000000');
      ble.homes.add(ble.home!);
      await tester.pump(const Duration(milliseconds: 100));
      await _pumpTransport(tester);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(1),
      );
      // Explicit selection still requests firmware admission, which can refuse
      // the running game; the auto-entry guard does not replace board authority.
      ble.enterStatus = 1;
      controller.enter();
      await _pumpTransport(tester);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(2),
      );
      expect(controller.status, LitchcraftStatus.gameActive);
    },
  );

  testWidgets(
    'empty occupancy never auto-enters an active game and stale empty refusal is bounded',
    (tester) async {
      controller.dispose();
      await ble.shutdown();
      ble = _Ble();
      controller = LitchcraftController(ble);
      ble.enterStatus = 8;
      ble.active = false;
      controller.enter();
      await _pumpTransport(tester);
      HomeStateNotification home(
        int sequence,
        int placement,
      ) => HomeStateNotification.tryParse(
        '{"event":"home_state","v":1,"seq":$sequence,"a":"idle","fen":"8/8/8/8/8/8/8/8 w - - 0 1","occ":"0000000000000000","g":"u","duck":"--","out":"n","win":"n","rot":0,"pm":$placement,"fs":$placement,"white":1}',
      )!;
      ble.home = home(1, 0);
      ble.homes.add(ble.home!);
      await tester.pump(const Duration(seconds: 1));
      await _pumpTransport(tester);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(1),
      );
      ble.home = home(2, 1);
      ble.homes.add(ble.home!);
      await tester.pump(const Duration(seconds: 1));
      await _pumpTransport(tester);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(2),
      );
      for (var sequence = 3; sequence < 7; sequence++) {
        ble.home = home(sequence, 1);
        ble.homes.add(ble.home!);
        await tester.pump(const Duration(seconds: 2));
        await _pumpTransport(tester);
        expect(controller.status, LitchcraftStatus.boardNotEmpty);
        expect(
          ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
          hasLength(2),
          reason:
              'Unchanged empty heartbeats must not repeat a refused admission.',
        );
      }
      await tester.pump(const Duration(seconds: 10));
      await _pumpTransport(tester);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.enter),
        hasLength(2),
      );
    },
  );

  for (final initialRotation in [false, true]) {
    testWidgets(
      'BOARD_ROTATE=$initialRotation presents physical Litchcraft corners through read, change and reconnect',
      (tester) async {
        _usePhoneViewport(tester);
        SharedPreferences.setMockInitialValues(<String, Object>{});
        ble.cachedBoardRotate = null;
        var boardRotation = initialRotation;
        ble.onReadBoardRotate = () async => boardRotation;
        final artwork = LitchcraftCanvas.blank()
            .paint([0], 15, 0xff0000) // a8
            .paint([7], 15, 0x00ff00) // h8
            .paint([56], 15, 0x0000ff) // a1
            .paint([63], 15, 0xffff00) // h1
            .paint([9], 1, 0xab34ef) // b7 left emitter
            .paint([9], 2, 0x1234cd); // b7 top emitter
        ble.artwork = artwork;
        await tester.runAsync(() => _enterUsingBoard(controller));
        expect(ble.settingReads, ['BOARD_ROTATE']);
        final library = LitchcraftLibrary(controller);
        await library.ready;
        await tester.pumpWidget(
          MaterialApp(
            localizationsDelegates: AppLocalizations.localizationsDelegates,
            supportedLocales: AppLocalizations.supportedLocales,
            home: Scaffold(
              body: RepaintBoundary(
                key: const ValueKey('orientation-capture'),
                child: LitchcraftScreen(
                  controller: controller,
                  library: library,
                ),
              ),
            ),
          ),
        );
        await tester.pumpAndSettle();

        Future<void> expectPhysicalCorners(bool rotated) async {
          // BOARD_ROTATE compensates the board's physical half-turn. The
          // viewer keeps a8 far-left and a1 near-left in both hardware poses.
          const corners = [0, 7, 56, 63];
          final canvas = find.byType(LitchcraftCanvasView);
          final rect = tester.getRect(canvas);
          final positions = [
            rect.topLeft + Offset(rect.width / 16, rect.height / 16),
            rect.topRight + Offset(-rect.width / 16, rect.height / 16),
            rect.bottomLeft + Offset(rect.width / 16, -rect.height / 16),
            rect.bottomRight - Offset(rect.width / 16, rect.height / 16),
          ];
          for (var corner = 0; corner < 4; corner++) {
            expect(
              tester.getCenter(
                find.byKey(ValueKey('craft-square-${corners[corner]}')),
              ),
              positions[corner],
              reason: 'BOARD_ROTATE=$rotated physical corner $corner',
            );
          }
          final thumbnail = tester.widget<LitchcraftThumbnail>(
            find.descendant(
              of: canvas,
              matching: find.byType(LitchcraftThumbnail),
            ),
          );
          expect(thumbnail.flipped, isFalse);
          expect(thumbnail.canvas.colors, artwork.colors);
          expect(controller.canvas.colors, artwork.colors);
          final capture = find.byKey(const ValueKey('orientation-capture'));
          final boundary = tester.renderObject<RenderRepaintBoundary>(capture);
          final origin = tester.getTopLeft(capture);
          final emitterSquare = tester.getRect(
            find.byKey(const ValueKey('craft-square-9')),
          );
          await tester.runAsync(() async {
            final image = await boundary.toImage();
            final pixels = (await image.toByteData(
              format: ui.ImageByteFormat.rawRgba,
            ))!;
            int rgbAt(Offset position) {
              final local = position - origin;
              final index =
                  (local.dy.floor() * image.width + local.dx.floor()) * 4;
              return pixels.getUint8(index) << 16 |
                  pixels.getUint8(index + 1) << 8 |
                  pixels.getUint8(index + 2);
            }

            for (var corner = 0; corner < 4; corner++) {
              expect(
                rgbAt(positions[corner]),
                artwork.colors[corners[corner] * 4],
                reason:
                    'painted corner pixels must match the accessible square',
              );
            }
            const direction = -1.0;
            expect(
              rgbAt(
                emitterSquare.center +
                    Offset(direction * emitterSquare.width / 4, 0),
              ),
              litchcraftPreviewColor(0xab34ef).toARGB32() & 0xffffff,
              reason:
                  'the logical left emitter follows the same 180-degree view',
            );
            expect(
              rgbAt(
                emitterSquare.center +
                    Offset(0, direction * emitterSquare.height / 4),
              ),
              litchcraftPreviewColor(0x1234cd).toARGB32() & 0xffffff,
              reason:
                  'the logical top emitter follows the same 180-degree view',
            );
            image.dispose();
          });
        }

        await expectPhysicalCorners(initialRotation);
        final document = encodeLitchcraftAnimationDocument(
          controller.documentSession.document,
        ).bytes;
        boardRotation = !initialRotation;
        ble.cachedBoardRotate = boardRotation;
        ble.settings.add('BOARD_ROTATE');
        await tester.pumpAndSettle();
        await expectPhysicalCorners(boardRotation);
        ble.cachedFirstPlayer = 2;
        ble.settings.add('GAME_FIRST_PLAYER');
        await tester.pumpAndSettle();
        await expectPhysicalCorners(boardRotation);

        await tester.runAsync(() async {
          ble.connection(false);
          boardRotation = initialRotation;
          ble.cachedBoardRotate = null;
          ble.connection(true);
          await _idle(controller);
        });
        await tester.pumpAndSettle();
        expect(controller.status, LitchcraftStatus.live);
        expect(ble.settingReads, ['BOARD_ROTATE', 'BOARD_ROTATE']);
        await expectPhysicalCorners(initialRotation);
        expect(
          encodeLitchcraftAnimationDocument(
            controller.documentSession.document,
          ).bytes,
          document,
          reason: 'presentation changes preserve serialized artwork identity',
        );
        expect(ble.bulkFrames, isEmpty);
        await tester.runAsync(
          () => _drive(controller.setBrush(color: 0x56789a)),
        );
        await tester.pumpAndSettle();
        final canvasRect = tester.getRect(find.byType(LitchcraftCanvasView));
        await tester.runAsync(() async {
          await tester.tapAt(
            canvasRect.topLeft +
                Offset(canvasRect.width / 16, canvasRect.height / 16),
          );
          await _idle(controller);
        });
        await tester.pumpAndSettle();
        final painted = ble.requests.where(
          (r) => r.opcode == LitchcraftProtocol.paint,
        );
        expect(painted, isNotEmpty);
        expect(
          painted.expand((r) => r.payload.skip(3)).toSet(),
          {0},
          reason: 'upper-left pointer uses the displayed logical square on BLE',
        );
        await tester.pumpWidget(const SizedBox());
        library.dispose();
      },
    );
  }

  test(
    'live admission waits for board rotation before enabling paint',
    () async {
      ble.cachedBoardRotate = null;
      final orientation = Completer<Object?>();
      ble.onReadBoardRotate = () => orientation.future;
      ble.artwork = LitchcraftCanvas.blank().paint([0, 63], 2, 0x123456);
      final entering = controller.enter();
      await _until(() => ble.settingReads.isNotEmpty);
      expect(controller.status, LitchcraftStatus.connecting);
      expect(controller.pending, isTrue);
      expect(ble.requests.map((request) => request.opcode), [
        LitchcraftProtocol.enter,
      ]);
      orientation.complete(true);
      await _drive(entering);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.flipped, isFalse);
      expect(controller.canvas, ble.artwork);

      // A late setting read from a lost connection cannot re-admit the page.
      ble.connection(false);
      final lateOrientation = Completer<Object?>();
      ble.onReadBoardRotate = () => lateOrientation.future;
      final requestCount = ble.requests.length;
      ble.connection(true);
      await _until(() => ble.settingReads.length == 2);
      ble.connection(false);
      lateOrientation.complete(false);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.offline);
      expect(controller.flipped, isFalse);
      expect(ble.requests.length, requestCount);
    },
  );

  test(
    'board rotation updates the live Litchcraft canvas orientation',
    () async {
      await _drive(controller.enter());
      expect(controller.flipped, isFalse);

      ble.cachedBoardRotate = true;
      ble.settings.add('BOARD_ROTATE');
      await Future<void>.delayed(Duration.zero);

      expect(controller.flipped, isFalse);
      expect(controller.status, LitchcraftStatus.live);
    },
  );

  test(
    'invalid board rotation admits mode but cannot enable paint or replace a local draft',
    () async {
      ble.cachedBoardRotate = '9';
      ble.onReadBoardRotate = () async => 2;
      final draft = LitchcraftCanvas.blank().paint([7], 8, 0x345678);
      controller.restoreDraft(draft);
      await _drive(controller.enter());
      expect(ble.settingReads, ['BOARD_ROTATE']);
      expect(controller.status, LitchcraftStatus.error);
      expect(controller.canvas, draft);
      expect(ble.requests.map((request) => request.opcode), [
        LitchcraftProtocol.enter,
      ]);
    },
  );

  test(
    'failed orientation read keeps artwork after mode admission and disables paint',
    () async {
      ble.cachedBoardRotate = null;
      ble.onReadBoardRotate = () async =>
          throw StateError('setting read failed');
      final draft = LitchcraftCanvas.blank().paint([56], 1, 0x345678);
      controller.restoreDraft(draft);
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.error);
      expect(controller.canvas, draft);
      expect(controller.pending, isFalse);
      expect(ble.requests.map((request) => request.opcode), [
        LitchcraftProtocol.enter,
      ]);
    },
  );

  test(
    'offline strokes retain all square addresses and bounded undo units',
    () async {
      ble.connected = false;
      await _drive(controller.enter());
      await controller.setBrush(color: 0x123456);
      controller.beginStroke();
      for (var square = 0; square < 64; ++square) {
        controller.paintSquares([square]);
      }
      controller.endStroke();
      expect(controller.canvas.colors, everyElement(0x123456));
      await controller.undo();
      expect(controller.canvas.hasMarks, isFalse);
      expect(
        controller.canUndo,
        isFalse,
        reason: '64 cells in one drag are one undo',
      );
      await controller.redo();
      expect(controller.canvas.colors, everyElement(0x123456));
      expect(ble.requests, isEmpty);
    },
  );

  test('light and dark checker fills remain independently editable', () async {
    ble.connected = false;
    await _drive(controller.enter());
    await controller.setBrush(color: 0x123456);
    await controller.fillChecker(dark: false);
    await controller.setBrush(color: 0xabcdef);
    await controller.fillChecker(dark: true);
    for (var square = 0; square < 64; ++square) {
      final dark = ((square % 8) + (square ~/ 8)).isOdd;
      expect(
        controller.canvas.colors.sublist(square * 4, square * 4 + 4),
        everyElement(dark ? 0xabcdef : 0x123456),
      );
    }
  });

  test('brush color swap exchanges the current and previous colors', () async {
    ble.connected = false;
    expect(controller.canSwapBrushColor, isFalse);

    await controller.setBrush(color: 0x123456);
    expect(controller.color, 0x123456);
    expect(controller.previousColor, 0xff7430);
    expect(controller.canSwapBrushColor, isTrue);

    await controller.setBrush(color: 0xabcdef);
    await controller.swapBrushColor();
    expect(controller.color, 0x123456);
    expect(controller.previousColor, 0xabcdef);

    await controller.swapBrushColor();
    expect(controller.color, 0xabcdef);
    expect(controller.previousColor, 0x123456);

    await controller.setBrush(eraser: true);
    await controller.swapBrushColor();
    expect(controller.color, 0x123456);
    expect(controller.eraser, isTrue);
    controller.paintSquares(<int>[0]);
    expect(controller.canvas.colors.take(4), everyElement(0));
  });

  test(
    'unsolicited physical Tool colors preserve last color across packets',
    () async {
      await _drive(controller.enter());
      await _drive(controller.setBrush(color: 0x123456));
      final previous = controller.previousColor;
      for (final color in [0xe71800, 0xcf3000, 0xb74800]) {
        ble.brushColor = color;
        ble.revision++;
        ble.emit(
          LitchcraftProtocol.brush,
          payload: [...LitchcraftProtocol.rgb(color), 0, 15],
        );
        await Future<void>.delayed(Duration.zero);
        expect(controller.color, color);
        expect(controller.previousColor, previous);
      }
      await _drive(controller.swapBrushColor());
      expect(controller.color, previous);
    },
  );

  test(
    'repeated offline whole-canvas action does not add a no-op undo',
    () async {
      ble.connected = false;
      await _drive(controller.enter());
      await controller.setBrush(color: 0x123456, targetMask: 15);
      await controller.fillAll();
      await controller.fillAll();

      await controller.undo();

      expect(controller.canvas.hasMarks, isFalse);
      expect(controller.canUndo, isFalse);
    },
  );

  test(
    'live whole-canvas actions commit atomically over negotiated bulk',
    () async {
      await _drive(controller.enter());
      await _drive(controller.setBrush(color: 0x123456, targetMask: 15));
      ble.requests.clear();
      ble.bulkFrames.clear();

      await _drive(controller.fillAll());

      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        isEmpty,
        reason: 'a full-canvas action must not animate as paint packet chunks',
      );
      expect(ble.bulkFrames, hasLength(4));
      expect(controller.canvas.colors, everyElement(0x123456));

      ble.requests.clear();
      ble.bulkFrames.clear();
      await _drive(controller.setBrush(color: 0xabcdef, targetMask: 15));
      ble.requests.clear();
      await _drive(controller.fillChecker(dark: true));

      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        isEmpty,
      );
      expect(ble.bulkFrames, hasLength(4));
      for (var square = 0; square < 64; ++square) {
        final dark = ((square % 8) + (square ~/ 8)).isOdd;
        expect(
          controller.canvas.colors.sublist(square * 4, square * 4 + 4),
          everyElement(dark ? 0xabcdef : 0x123456),
        );
      }
    },
  );

  test(
    'queued whole-canvas actions derive from the last accepted canvas',
    () async {
      await _drive(controller.enter());
      await _drive(controller.setBrush(color: 0x123456, targetMask: 15));
      final firstStarted = Completer<void>();
      final releaseFirst = Completer<void>();
      var transfers = 0;
      ble.onBulk = (frames, request) async {
        final colors = List<int>.filled(256, 0);
        for (final frame in frames) {
          final offset = frame[8] | (frame[9] << 8);
          final count = frame[11];
          for (var index = 0; index < count; ++index) {
            colors[offset + index] = LitchcraftProtocol.readRgb(
              frame,
              12 + index * 3,
            );
          }
        }
        if (++transfers == 1) {
          firstStarted.complete();
          await releaseFirst.future;
        }
        ble.artwork = LitchcraftCanvas(colors: colors);
        ++ble.revision;
        ble.state(request);
      };

      final first = controller.fillAll();
      await firstStarted.future;
      final brush = controller.setBrush(color: 0xabcdef, targetMask: 15);
      final second = controller.fillChecker(dark: true);
      releaseFirst.complete();
      await _drive(Future.wait<void>([first, brush, second]));

      expect(transfers, 2);
      for (var square = 0; square < 64; ++square) {
        final dark = ((square % 8) + (square ~/ 8)).isOdd;
        expect(
          controller.canvas.colors.sublist(square * 4, square * 4 + 4),
          everyElement(dark ? 0xabcdef : 0x123456),
        );
      }
    },
  );

  test(
    'live whole-canvas actions commit atomically over negotiated bulk',
    () async {
      await _drive(controller.enter());
      await _drive(controller.setBrush(color: 0x123456, targetMask: 15));
      ble.requests.clear();
      ble.bulkFrames.clear();

      await _drive(controller.fillAll());

      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        isEmpty,
        reason: 'a full-canvas action must not animate as paint packet chunks',
      );
      expect(ble.bulkFrames, hasLength(4));
      expect(controller.canvas.colors, everyElement(0x123456));

      ble.requests.clear();
      ble.bulkFrames.clear();
      await _drive(controller.setBrush(color: 0xabcdef, targetMask: 15));
      ble.requests.clear();
      await _drive(controller.fillChecker(dark: true));

      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        isEmpty,
      );
      expect(ble.bulkFrames, hasLength(4));
      for (var square = 0; square < 64; ++square) {
        final dark = ((square % 8) + (square ~/ 8)).isOdd;
        expect(
          controller.canvas.colors.sublist(square * 4, square * 4 + 4),
          everyElement(dark ? 0xabcdef : 0x123456),
        );
      }
    },
  );

  test(
    'eraser color changes and clear form sensible offline history boundaries',
    () async {
      ble.connected = false;
      await controller.setBrush(color: 0x123456);
      controller.paintSquares([0, 63]);
      await controller.setBrush(eraser: true);
      controller.paintSquares([0]);
      controller.endStroke();
      expect(controller.canvas.colors[0], 0);
      await controller.setBrush(color: 0xabcdef);
      expect(controller.eraser, isFalse);
      await controller.undo();
      expect(controller.canvas.colors[0], 0x123456);
      await controller.clear();
      expect(controller.canvas.hasMarks, isFalse);
      await controller.undo();
      expect(controller.canvas.colors[0], 0x123456);
      expect(controller.canvas.colors[252], 0x123456);
    },
  );

  test(
    'legacy firmware keeps local artwork usable and sends no art commands',
    () async {
      ble.supported = false;
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.unsupported);
      controller.paintSquares([7]);
      controller.endStroke();
      expect(controller.hasMarks, isTrue);
      expect(
        (await controller.coherentSnapshot()).colors[28],
        controller.color,
      );
      expect(ble.requests, isEmpty);
    },
  );

  test(
    'active-game rejection preserves detached draft and never uploads',
    () async {
      ble.active = false;
      ble.enterStatus = 1;
      final draft = LitchcraftCanvas.blank().paint([7, 56], 15, 0x123456);
      controller.restoreDraft(draft);
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.gameActive);
      expect(controller.canvas, draft);
      expect(ble.requests.map((r) => r.opcode), [LitchcraftProtocol.enter]);
    },
  );

  test(
    'non-empty-board rejection is immediate and distinct from a game',
    () async {
      ble.active = false;
      ble.enterStatus = 8;
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.boardNotEmpty);
      expect(ble.requests.map((r) => r.opcode), [LitchcraftProtocol.enter]);
    },
  );

  test(
    'snapshot restores exact asymmetric colors on each supported topology',
    () async {
      for (final topology in LitchcraftTopology.values) {
        ble.artwork = LitchcraftCanvas(
          colors: List.generate(256, (i) => (i * 31337 + 0x123456) & 0xffffff),
          topology: topology,
        );
        await _enterUsingBoard(controller);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.canvas, ble.artwork);
        expect(controller.canvas.colors[0], 0x123456);
        expect(controller.canvas.colors[255], ble.artwork.colors[255]);
      }
    },
  );

  test(
    'history count greater than one still establishes live session',
    () async {
      ble.undoCount = 2;
      ble.redoCount = 3;
      final entering = controller.enter();
      await Future<void>.delayed(Duration.zero);
      await _drive(entering);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canUndo, isTrue);
      expect(controller.canRedo, isTrue);
    },
  );

  test(
    'partial snapshot stays hidden and later Hall delta wins after coherent commit',
    () async {
      await _drive(controller.enter());
      final old = controller.canvas;
      final replacement = LitchcraftCanvas.blank().paint(
        [0, 7, 56, 63],
        15,
        0x123456,
      );
      ble.artwork = replacement;
      _Request? read;
      ble.onSnapshot = (request) {
        read = request;
        ble.state(request.id);
        ble.begin(request.id);
        ble.chunks(request.id, replacement, until: 2);
      };
      final saving = controller.coherentSnapshot();
      await _until(() => read != null);
      expect(read, isNotNull);
      expect(
        controller.canvas,
        old,
        reason: 'one received chunk is not finished artwork',
      );
      ble.emit(
        LitchcraftProtocol.cell,
        atRevision: 11,
        payload: [0, 15, 0xab, 0xcd, 0xef],
      );
      ble.chunks(read!.id, replacement, from: 2);
      ble.emit(LitchcraftProtocol.snapshotEnd, requestId: read!.id);
      final saved = await _drive(saving);
      expect(saved.colors.take(4), everyElement(0xabcdef));
      expect(saved.colors[28], 0x123456);
      expect(controller.revision, 11);
      expect(controller.boardMarkCount, 1);
    },
  );

  test(
    'duplicate chunks are idempotent but conflicting chunks reject atomically',
    () async {
      await _drive(controller.enter());
      final original = controller.canvas;
      ble.onSnapshot = (request) {
        ble.state(request.id);
        ble.begin(request.id);
        ble.chunks(request.id, ble.artwork, until: 2);
        ble.chunks(request.id, ble.artwork, until: 2);
        ble.chunks(request.id, ble.artwork.paint([0], 15, 0x123456), until: 2);
      };
      final failed = expectLater(
        controller.coherentSnapshot(),
        throwsStateError,
      );
      await _idle(controller);
      await failed;
      expect(controller.status, LitchcraftStatus.error);
      expect(controller.canvas, original);
    },
  );

  test(
    'missing snapshot chunk cannot become a saved partial picture',
    () async {
      await _drive(controller.enter());
      ble.onSnapshot = (request) {
        ble.state(request.id);
        ble.begin(request.id);
        ble.chunks(request.id, ble.artwork, until: 254);
        ble.emit(LitchcraftProtocol.snapshotEnd, requestId: request.id);
      };
      final failed = expectLater(
        controller.coherentSnapshot(),
        throwsStateError,
      );
      await _idle(controller);
      await failed;
      expect(controller.status, LitchcraftStatus.error);
      expect(controller.canvas.hasMarks, isFalse);
    },
  );

  test(
    'delayed snapshot from a previous request cannot replace current read',
    () async {
      await _drive(controller.enter());
      final current = LitchcraftCanvas.blank().paint([7], 15, 0xabcdef);
      ble.onSnapshot = (request) {
        ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x123456);
        ble.snapshot(request.id - 1);
        ble.artwork = current;
        ble.snapshot(request.id);
      };
      final saved = await _drive(controller.coherentSnapshot());
      expect(
        saved,
        current,
        reason: 'request identity belongs to the coherent snapshot',
      );
    },
  );

  test(
    'duplicate stale and wrong-session deltas cannot overwrite newer Hall paint',
    () async {
      await _drive(controller.enter());
      ble.emit(
        LitchcraftProtocol.cell,
        atRevision: 11,
        payload: [63, 15, 1, 2, 3],
      );
      ble.emit(
        LitchcraftProtocol.cell,
        atRevision: 11,
        payload: [63, 15, 9, 9, 9],
      );
      ble.emit(
        LitchcraftProtocol.cell,
        atRevision: 10,
        payload: [63, 15, 9, 9, 9],
      );
      ble.emit(
        LitchcraftProtocol.cell,
        atRevision: 12,
        sessionId: 40,
        payload: [63, 15, 9, 9, 9],
      );
      expect(controller.canvas.colors[252], 0x010203);
      expect(controller.revision, 11);
      expect(controller.boardMarkCount, 1);
    },
  );

  test(
    'stale state cannot undo current brush or end the live session',
    () async {
      await _drive(controller.enter());
      ble.emit(
        LitchcraftProtocol.brush,
        atRevision: 11,
        payload: [1, 2, 3, 1, 4],
      );
      ble.active = false;
      ble.brushErase = false;
      ble.brushMask = 15;
      ble.state(0, atRevision: 10);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.eraser, isTrue);
      expect(controller.targetMask, 4);
    },
  );

  test(
    'revision gap resyncs authoritative artwork instead of guessing omitted cells',
    () async {
      await _drive(controller.enter());
      ble.artwork = LitchcraftCanvas.blank().paint([1, 63], 15, 0x123456);
      ble.revision = 12;
      ble.emit(LitchcraftProtocol.cell, payload: [63, 15, 0x12, 0x34, 0x56]);
      await _idle(controller);
      expect(controller.canvas, ble.artwork);
      expect(controller.revision, 12);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.snapshot),
        hasLength(2),
      );
    },
  );

  test(
    'repeated duplicate connected events keep the live canvas and do not restart synchronization',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint([0, 63], 15, 0x123456);
      await _enterUsingBoard(controller);
      final statuses = <LitchcraftStatus>[];
      controller.addListener(() => statuses.add(controller.status));
      for (var repeat = 0; repeat < 40; ++repeat) {
        ble.connection(true);
        await _idle(controller);
      }
      expect(statuses.every((s) => s == LitchcraftStatus.live), isTrue);
      expect(controller.canvas, ble.artwork);
      expect(ble.requests.map((r) => r.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
      ]);
    },
  );

  test(
    'connected events during admission coalesce into the current handshake',
    () async {
      _Request? pending;
      ble.onSnapshot = (request) => pending = request;
      final entering = controller.enter();
      await _until(() => pending != null);
      for (var repeat = 0; repeat < 5; ++repeat) {
        ble.connection(true);
      }
      ble.onSnapshot = null;
      ble.snapshot(pending!.id);
      await _drive(entering);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(ble.requests.map((request) => request.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
      ]);
    },
  );

  test(
    'foreground refresh preserves the live canvas while snapshots are pending',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint([0, 63], 15, 0x123456);
      await _enterUsingBoard(controller);
      final coherent = controller.canvas;
      final statuses = <LitchcraftStatus>[];
      controller.addListener(() => statuses.add(controller.status));
      for (var repeat = 0; repeat < 20; ++repeat) {
        _Request? pending;
        ble.onSnapshot = (request) => pending = request;
        controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
        await _until(() => pending != null);
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.canvas, coherent);
        ble.snapshot(pending!.id);
        await _idle(controller);
      }
      expect(statuses.every((s) => s == LitchcraftStatus.live), isTrue);
    },
  );

  test(
    'latest coherent physical edits become the baseline for later app-only edits',
    () async {
      await _drive(controller.enter());
      ble.artwork = ble.artwork.paint([0], 15, 0x112233);
      ++ble.revision;
      ble.emit(LitchcraftProtocol.cell, payload: [0, 15, 0x11, 0x22, 0x33]);
      ble.state(0);
      expect(controller.canvas, ble.artwork);
      ble.connection(false);
      final edited = controller.canvas.paint([7], 15, 0xabcdef);
      await controller.replaceCanvas(edited);
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.status, LitchcraftStatus.live);
      expect(ble.artwork, edited);
    },
  );

  test(
    'both sides making identical edits reconnect without uploading or choosing',
    () async {
      await _drive(controller.enter());
      ble.connection(false);
      final edited = controller.canvas.paint([7], 15, 0xabcdef);
      await controller.replaceCanvas(edited);
      ble.artwork = edited;
      ++ble.revision;
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, edited);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'another board cannot inherit the previous board common ancestor',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
      await _enterUsingBoard(controller);
      final retained = controller.canvas;
      ble.connection(false);
      ble.deviceId = 'another-board';
      ble.artwork = LitchcraftCanvas.blank().paint([63], 15, 0xabcdef);
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.conflict);
      expect(controller.canvas, retained);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'app-only offline edits automatically replace the unchanged board canvas',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
      await _enterUsingBoard(controller);
      ble.connection(false);
      final edited = controller.canvas.paint([7], 15, 0xabcdef);
      await controller.replaceCanvas(edited);
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(ble.artwork, edited);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, edited);
      expect(ble.artwork, edited);
      expect(ble.bulkFrames, hasLength(4));
    },
  );

  test(
    'local edits reverted to baseline do not conflict with board-only changes',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
      await _enterUsingBoard(controller);
      final baseline = controller.canvas;
      ble.connection(false);
      await controller.replaceCanvas(baseline.paint([7], 15, 0xabcdef));
      await controller.replaceCanvas(baseline);
      ble.artwork = baseline.paint([63], 15, 0x654321);
      ++ble.revision;
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'restored identical RGB artwork ignores topology and orientation metadata',
    () async {
      final colors = List<int>.generate(256, (i) => i * 12345);
      controller.restoreDraft(
        LitchcraftCanvas(colors: colors, topology: LitchcraftTopology.v2),
      );
      ble.artwork = LitchcraftCanvas(colors: colors);
      ble.cachedBoardRotate = true;
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.flipped, isFalse);
      expect(controller.canvas.topology, LitchcraftTopology.v3);
      expect(controller.canvas.colors, colors);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'first live entry adopts board artwork when the app has no authored draft',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(controller.boardCanvas, ble.artwork);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'first live entry sends the app artwork when the board canvas is empty',
    () async {
      final draft = LitchcraftCanvas.blank().paint([0, 7], 15, 0x112233);
      controller.restoreDraft(draft);
      ble.artwork = LitchcraftCanvas.blank();
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, draft.withTopology(LitchcraftTopology.v3));
      expect(controller.boardCanvas, controller.canvas);
      expect(ble.artwork, controller.canvas);
      expect(ble.bulkFrames, isNotEmpty);
    },
  );

  test(
    'restored empty draft without a common ancestor never erases board artwork',
    () async {
      controller.restoreDraft(LitchcraftCanvas.blank());
      ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x112233);
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.conflict);
      expect(controller.canvas.hasMarks, isFalse);
      expect(ble.artwork.hasMarks, isTrue);
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'offline divergence offers choice and keep-board refreshes newest physical marks',
    () async {
      await _drive(controller.enter());
      ble.connection(false);
      await Future<void>.delayed(Duration.zero);
      await controller.setBrush(color: 0x123456);
      controller.paintSquares([0]);
      controller.endStroke();
      final detached = controller.canvas;
      ble.artwork = LitchcraftCanvas.blank().paint([63], 15, 0xabcdef);
      ble.revision = 11;
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.conflict);
      expect(controller.canvas, detached);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.replaceBegin),
        isEmpty,
      );
      ble.artwork = ble.artwork.paint([7], 15, 0xabcdef);
      ble.revision = 12;
      await _drive(controller.resolveConflict(useBoard: true));
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(controller.canvas.colors[0], 0);
    },
  );

  test(
    'reconnect adopts changed physical artwork when the app is unchanged',
    () async {
      await _drive(controller.enter());
      ble.connection(false);
      await Future<void>.delayed(Duration.zero);
      ble.artwork = LitchcraftCanvas.blank().paint([56], 15, 0x123456);
      ble.revision = 11;
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(ble.requests.map((r) => r.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
        LitchcraftProtocol.snapshot,
      ]);
    },
  );

  test(
    'draw while first snapshot is pending keeps the new local stroke',
    () async {
      _Request? read;
      ble.onSnapshot = (request) {
        read = request;
      };
      final entering = controller.enter();
      await _until(() => read != null);
      expect(read, isNotNull);
      controller.paintSquares([7]);
      controller.endStroke();
      final drawn = controller.canvas;
      ble.snapshot(read!.id);
      await _drive(entering);
      expect(
        controller.canvas,
        drawn,
        reason:
            'a first mark made during Connecting must survive the asynchronous inspection',
      );
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.boardCanvas, drawn);
      expect(ble.artwork, drawn);
    },
  );

  test(
    'live drag coalesces cells and retains one stroke across bounded packets',
    () async {
      await _drive(controller.enter());
      ble.onMutation = (request) async {
        expect(request.opcode, LitchcraftProtocol.paint);
        final color = ble.brushErase ? 0 : ble.brushColor;
        for (final square in request.payload.skip(3)) {
          final next = ble.artwork.paint([square], ble.brushMask, color);
          if (next == ble.artwork) continue;
          ble.artwork = next;
          ++ble.revision;
          ble.emit(
            LitchcraftProtocol.cell,
            requestId: request.id,
            payload: [square, ble.brushMask, ...LitchcraftProtocol.rgb(color)],
          );
        }
        ble.state(request.id);
      };
      controller.beginStroke();
      controller.paintSquares(List.generate(64, (index) => index));
      controller.paintSquares([0, 63]);
      controller.endStroke();
      await _idle(controller);
      final paint = ble.requests
          .where((r) => r.opcode == LitchcraftProtocol.paint)
          .toList();
      expect(paint, hasLength(8));
      expect(paint.every((r) => r.bytes.length <= 20), isTrue);
      expect(
        paint.expand((r) => r.payload.skip(3)).toSet(),
        List.generate(64, (i) => i).toSet(),
      );
      expect(
        paint.map((r) => r.payload[0] | r.payload[1] << 8).toSet(),
        hasLength(1),
      );
      expect(paint.first.payload[2] & 1, 1);
      expect(paint.last.payload[2] & 2, 2);
      expect(paint.take(7).every((r) => r.payload[2] & 2 == 0), isTrue);
      expect(
        ble.requests.where(
          (request) => request.opcode == LitchcraftProtocol.snapshot,
        ),
        hasLength(1),
        reason:
            'exactly acknowledged app paint must not download the full canvas',
      );
    },
  );

  test(
    'live drag coalesces pointer cadence behind a delayed acknowledgement',
    () async {
      await _drive(controller.enter());
      final firstAck = Completer<void>();
      var paintWrites = 0;
      ble.onMutation = (request) async {
        if (request.opcode == LitchcraftProtocol.paint) {
          ++paintWrites;
          if (paintWrites == 1) await firstAck.future;
        }
        ble.state(request.id);
      };

      controller.beginStroke();
      for (var square = 0; square < 32; ++square) {
        controller.paintSquares(<int>[square]);
        await Future<void>.delayed(Duration.zero);
      }
      controller.endStroke();
      await _until(() => paintWrites == 1);
      firstAck.complete();
      await _idle(controller);

      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        hasLength(5),
        reason:
            'the first cell leaves immediately; the remaining 31 coalesce into four packets',
      );
    },
  );

  test('leaving and deliberately re-entering retains board artwork', () async {
    ble.artwork = LitchcraftCanvas.blank().paint(<int>[0, 63], 15, 0x123456);
    await _enterUsingBoard(controller);
    final before = controller.canvas;
    await _drive(controller.leave());
    await _drive(controller.enter());
    expect(controller.status, LitchcraftStatus.live);
    expect(controller.canvas, before);
    expect(controller.canvas.hasMarks, isTrue);
    expect(ble.litchcraftSubscriptionCount, 1);
    expect(ble.requests.map((request) => request.opcode), <int>[
      LitchcraftProtocol.enter,
      LitchcraftProtocol.snapshot,
      LitchcraftProtocol.exit,
      LitchcraftProtocol.enter,
    ]);
  });

  test(
    'repeated navigation and unchanged reconnects stay live without canvas reload churn',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint(<int>[0, 63], 15, 0x123456);
      await _enterUsingBoard(controller);
      final expected = controller.canvas;

      for (var cycle = 0; cycle < 12; ++cycle) {
        await _drive(controller.leave());
        await _drive(controller.enter());
        expect(controller.status, LitchcraftStatus.live);
        expect(controller.canvas, expected);

        if (cycle % 3 == 2) {
          ble.connection(false);
          await Future<void>.delayed(Duration.zero);
          ble.connection(true);
          await _idle(controller);
          expect(controller.status, LitchcraftStatus.live);
          expect(controller.canvas, expected);
        }
      }

      expect(
        ble.litchcraftSubscriptionCount,
        5,
        reason: 'one notification subscription is retained per BLE generation',
      );
      expect(
        ble.requests.where(
          (request) => request.opcode == LitchcraftProtocol.snapshot,
        ),
        hasLength(5),
        reason:
            'initial admission plus four actual reconnects require snapshots; '
            'sessions=${ble.requests.where((request) => request.opcode == LitchcraftProtocol.snapshot).map((request) => request.session).toList()}',
      );
      expect(ble.bulkFrames, isEmpty);
    },
  );

  test(
    'unchanged reconnect adopts one state reply without streaming the canvas',
    () async {
      ble.inspectSupported = true;
      ble.artwork = LitchcraftCanvas.blank().paint(<int>[0, 63], 15, 0x123456);
      await _enterUsingBoard(controller);
      final expected = controller.canvas;
      ble.requests.clear();

      ble.connection(false);
      await Future<void>.delayed(Duration.zero);
      ble.connection(true);
      await _idle(controller);

      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, expected);
      expect(ble.requests.map((request) => request.opcode), <int>[
        LitchcraftProtocol.inspect,
      ]);
    },
  );

  test(
    'manual repair rebuilds a failed notification link and only inspects unchanged art',
    () async {
      ble.inspectSupported = true;
      ble.artwork = LitchcraftCanvas.blank().paint(<int>[0, 63], 15, 0x123456);
      await _enterUsingBoard(controller);
      final expected = controller.canvas;

      ble.connection(false);
      await Future<void>.delayed(Duration.zero);
      ble.onInspect = (_) => ble.packets.addError(
        StateError('scripted notification registration failure'),
      );
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.error);

      ble.onInspect = null;
      ble.requests.clear();
      await _drive(controller.retryConnection());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, expected);
      expect(ble.requests.map((request) => request.opcode), <int>[
        LitchcraftProtocol.inspect,
      ]);
    },
  );

  test(
    'changed reconnect inspects first then requests one negotiated bulk snapshot',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint(<int>[0], 15, 0x123456);
      await _enterUsingBoard(controller);
      ble.inspectSupported = true;
      ble.requests.clear();

      ble.connection(false);
      await Future<void>.delayed(Duration.zero);
      ble.artwork = ble.artwork.paint(<int>[63], 15, 0xabcdef);
      ++ble.revision;
      ble.connection(true);
      await _idle(controller);

      expect(controller.status, LitchcraftStatus.live);
      expect(ble.requests.map((request) => request.opcode), <int>[
        LitchcraftProtocol.inspect,
        LitchcraftProtocol.snapshot,
      ]);
      expect(ble.requests.last.payload, <int>[1]);
    },
  );

  test(
    'same-link re-entry falls back to a snapshot when the revision is not contiguous',
    () async {
      ble.artwork = LitchcraftCanvas.blank().paint(<int>[0], 15, 0x123456);
      await _enterUsingBoard(controller);
      await _drive(controller.leave());
      ble.revision += 3;

      await _drive(controller.enter());

      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(ble.litchcraftSubscriptionCount, 1);
      expect(
        ble.requests
            .where((request) => request.opcode == LitchcraftProtocol.snapshot)
            .length,
        2,
      );
    },
  );

  test('tool change ends current drag before board brush changes', () async {
    await _drive(controller.enter());
    ble.onMutation = (request) async {
      if (request.opcode == LitchcraftProtocol.paint) {
        final color = ble.brushErase ? 0 : ble.brushColor;
        for (final square in request.payload.skip(3)) {
          final next = ble.artwork.paint([square], ble.brushMask, color);
          if (next == ble.artwork) continue;
          ble.artwork = next;
          ++ble.revision;
          ble.emit(
            LitchcraftProtocol.cell,
            requestId: request.id,
            payload: [square, ble.brushMask, ...LitchcraftProtocol.rgb(color)],
          );
        }
      } else if (request.opcode == LitchcraftProtocol.tool) {
        ble.brushErase = request.payload[0] != 0;
        ble.brushMask = request.payload[1];
        ble.brushColor = LitchcraftProtocol.readRgb(request.payload, 2);
        ++ble.revision;
        ble.emit(
          LitchcraftProtocol.brush,
          requestId: request.id,
          payload: [
            ...LitchcraftProtocol.rgb(ble.brushColor),
            ble.brushErase ? 1 : 0,
            ble.brushMask,
          ],
        );
      } else {
        fail('unexpected mutation ${request.opcode}');
      }
      ble.state(request.id);
    };
    controller.paintSquares([0]);
    await _drive(controller.setBrush(color: 0x123456));
    controller.paintSquares([1]);
    controller.endStroke();
    await _idle(controller);
    final writes = ble.requests
        .where(
          (r) =>
              r.opcode != LitchcraftProtocol.snapshot &&
              r.opcode != LitchcraftProtocol.enter,
        )
        .toList();
    expect(writes.map((r) => r.opcode), [
      LitchcraftProtocol.paint,
      LitchcraftProtocol.tool,
      LitchcraftProtocol.paint,
    ]);
    expect(writes.first.payload[2], 3);
    expect(writes.last.payload[2], 3);
    expect(writes.first.payload.take(2), isNot(writes.last.payload.take(2)));
    expect(
      ble.requests.where(
        (request) => request.opcode == LitchcraftProtocol.snapshot,
      ),
      hasLength(1),
      reason:
          'an exactly acknowledged brush change cannot imply unknown artwork',
    );
  });

  test(
    'leave preempts unsent edits and restores the local draft on re-entry',
    () async {
      await _drive(controller.enter());
      controller.paintSquares([0]);
      final draft = controller.canvas;
      await _drive(controller.leave());
      expect(ble.requests.map((r) => r.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
        LitchcraftProtocol.exit,
      ]);
      expect(controller.status, LitchcraftStatus.offline);
      expect(ble.active, isFalse);
      expect(controller.canvas, draft);
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, draft);
      expect(ble.artwork, draft);
    },
  );

  for (final action in ['fill', 'clear', 'rotate']) {
    test(
      'Exit retains unsent whole-canvas $action intent for re-entry',
      () async {
        await _drive(controller.enter());
        await _drive(controller.fillChecker(dark: true));
        controller.paintSquares([0]);
        controller.endStroke();
        final before = controller.canvas;
        final expected = switch (action) {
          'fill' => before.paint(
            List.generate(64, (i) => i),
            controller.targetMask,
            controller.color,
          ),
          'clear' => LitchcraftCanvas.blank(topology: before.topology),
          _ => before.rotated(1),
        };
        final Future<void> editing = switch (action) {
          'fill' => controller.fillAll(),
          'clear' => controller.clear(),
          _ => controller.rotateArtwork(1).then((_) {}),
        };
        await _drive(controller.leave());
        await _drive(editing);
        expect(controller.canvas, expected);
        await _drive(controller.enter());
        expect(controller.status, LitchcraftStatus.live);
        expect(ble.artwork, expected);
      },
    );
  }

  test(
    'opening a new document retires an unsent old static transform before Exit',
    () async {
      await _drive(controller.enter());
      final oldFill = controller.fillAll();
      final replacement = LitchcraftCanvas.blank().paint([17], 15, 0x123456);
      controller.restoreDocument(staticLitchcraftDocument(replacement));
      await _drive(controller.leave());
      await _drive(oldFill);
      expect(controller.canvas, replacement);
      await _drive(controller.enter());
      expect(controller.canvas, replacement);
      expect(ble.artwork, replacement);
    },
  );

  test(
    'an old Paint completion cannot replace a newly opened static document',
    () async {
      await _drive(controller.enter());
      final releasePaint = Completer<void>();
      var paintStarted = false;
      ble.onMutation = (request) async {
        expect(request.opcode, LitchcraftProtocol.paint);
        paintStarted = true;
        await releasePaint.future;
        ble.artwork = ble.artwork.paint([0], 15, 0xff7430);
        ++ble.revision;
        ble.emit(
          LitchcraftProtocol.cell,
          requestId: request.id,
          payload: [0, 15, ...LitchcraftProtocol.rgb(0xff7430)],
        );
        ble.state(request.id);
      };
      controller.paintSquares([0]);
      controller.endStroke();
      await _until(() => paintStarted);
      final replacement = LitchcraftCanvas.blank().paint([17], 15, 0x123456);
      controller.restoreDocument(staticLitchcraftDocument(replacement));
      releasePaint.complete();
      await _idle(controller);
      expect(controller.canvas, replacement);
      ble.onMutation = null;
      await _drive(controller.leave());
      expect(controller.canvas, replacement);
      await _drive(controller.enter());
      expect(controller.canvas, replacement);
      expect(controller.status, LitchcraftStatus.conflict);
      await _drive(controller.resolveConflict(useBoard: false));
      expect(ble.artwork, replacement);
    },
  );

  test(
    'failed exit retains selection so Play can retry board ownership',
    () async {
      await _drive(controller.enter());
      ble.onMutation = (request) async {
        if (request.opcode == LitchcraftProtocol.exit) {
          throw StateError('scripted exit failure');
        }
        ble.state(request.id);
      };
      await expectLater(controller.leave(), throwsA(isA<StateError>()));
      expect(controller.selected, isTrue);
      expect(ble.active, isTrue);

      ble.onMutation = null;
      await _drive(controller.leave());
      expect(controller.selected, isFalse);
      expect(ble.active, isFalse);
    },
  );

  test(
    'physical exit during admission snapshot never reports a live canvas',
    () async {
      ble.onSnapshot = (request) {
        ble.state(request.id);
        ble.begin(request.id);
        ble.chunks(request.id, ble.artwork);
        final frozenRevision = ble.revision;
        ble.active = false;
        ++ble.revision;
        ble.state(0);
        ble.emit(
          LitchcraftProtocol.snapshotEnd,
          requestId: request.id,
          atRevision: frozenRevision,
        );
      };
      await _drive(controller.enter());
      expect(controller.boardActive, isFalse);
      expect(controller.status, LitchcraftStatus.offline);
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await _idle(controller);
      expect(ble.requests.map((r) => r.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
      ]);
    },
  );

  test(
    'leaving offline then reconnecting on Play confirms the retained board exit',
    () async {
      await _drive(controller.enter());
      ble.connection(false);
      await _drive(controller.leave());
      expect(ble.active, isTrue);
      ble.requests.clear();
      ble.connection(true);
      await _idle(controller);
      expect(ble.active, isFalse);
      expect(controller.boardActive, isFalse);
      expect(ble.requests.map((r) => r.opcode), [LitchcraftProtocol.exit]);
    },
  );

  test(
    'reconnect while canceled admission unwinds still restores the board session',
    () async {
      var snapshots = 0;
      ble.onSnapshot = (request) {
        if (++snapshots == 1) {
          ble.state(request.id);
          ble.begin(request.id);
          ble.connection(false);
          ble.connection(true);
        } else {
          ble.snapshot(request.id);
        }
      };
      await _drive(controller.enter());
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.boardActive, isTrue);
      expect(snapshots, 2);
    },
  );

  test(
    'pending departure never exits a different board session after reboot',
    () async {
      await _drive(controller.enter());
      final oldSession = controller.sessionId;
      ble.connection(false);
      await _drive(controller.leave());
      ++ble.session;
      ble.onMutation = (request) async {
        expect(request.opcode, LitchcraftProtocol.exit);
        expect(request.session, oldSession);
        ble.state(request.id, status: 2);
      };
      ble.connection(true);
      await _idle(controller);
      expect(ble.active, isTrue);
      expect(controller.boardActive, isFalse);
      expect(controller.status, LitchcraftStatus.offline);
    },
  );

  test(
    'physical exit is not automatically reversed on application resume',
    () async {
      await _drive(controller.enter());
      ble.active = false;
      ble.revision = 11;
      ble.state(0);
      expect(controller.status, LitchcraftStatus.offline);
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await Future<void>.delayed(Duration.zero);
      expect(ble.requests.map((r) => r.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
      ]);
    },
  );

  test(
    'physical exit while disconnected preserves local marks without automatic admission',
    () async {
      await _drive(controller.enter());
      ble.connection(false);
      await controller.setBrush(color: 0x123456);
      controller.paintSquares([0, 7]);
      controller.endStroke();
      final draft = controller.canvas;
      // No State notification is available while the radio link is absent.
      ble.artwork = LitchcraftCanvas.blank().paint([56, 63], 8, 0xabcdef);
      ble.active = false;
      ble.revision = 12;
      ble.requests.clear();
      ble.connection(true);
      await _idle(controller);
      expect(ble.requests.map((r) => r.opcode), [LitchcraftProtocol.snapshot]);
      expect(ble.requests.single.session, 0);
      expect(controller.status, LitchcraftStatus.offline);
      expect(controller.boardActive, isFalse);
      expect(controller.canvas, draft);
      expect(await controller.coherentSnapshot(), draft);

      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await _idle(controller);
      expect(ble.requests, hasLength(1));
      expect(controller.status, LitchcraftStatus.offline);

      // A fresh deliberate selection/retry may request admission again.
      await _drive(controller.enter());
      expect(ble.requests.skip(1).map((r) => r.opcode), [
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
      ]);
      expect(controller.boardActive, isTrue);
      expect(controller.status, LitchcraftStatus.conflict);
      expect(controller.canvas, draft);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.replaceBegin),
        isEmpty,
      );
    },
  );

  test(
    'physical exit while disconnected adopts final artwork when no local edits exist',
    () async {
      await _drive(controller.enter());
      ble.connection(false);
      ble.artwork = LitchcraftCanvas.blank().paint([56, 63], 2, 0xabcdef);
      ble.active = false;
      ble.revision = 12;
      ble.requests.clear();
      ble.connection(true);
      await _idle(controller);
      expect(ble.requests.map((r) => r.opcode), [LitchcraftProtocol.snapshot]);
      expect(controller.status, LitchcraftStatus.offline);
      expect(controller.canvas, ble.artwork);
      expect(controller.boardActive, isFalse);
      controller.didChangeAppLifecycleState(AppLifecycleState.resumed);
      await _idle(controller);
      expect(ble.requests, hasLength(1));
    },
  );

  test(
    'inactive current V3 reboot waits for explicit admission and preserves the phone',
    () async {
      await _withCurrentStore((peer, owner) async {
        final saved = staticLitchcraftDocument(
          LitchcraftCanvas.blank().paint([0, 63], 4, 0x123456),
        );
        peer.installSavedStoreDocument(saved);
        await _drive(owner.enter());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        final cached = owner.documentSession.document;
        await peer.retireSharedConnection();
        owner.restoreDocument(cached);
        peer.boardBootNonce++;
        peer.session = 73;
        peer.revision = 1;
        peer.active = false;
        peer.installSavedStoreDocument(
          staticLitchcraftDocument(LitchcraftCanvas.blank()),
        );
        peer.requests.clear();
        peer.operations.clear();
        final inspections = peer.frameStoreInspectCount;
        final clocks = peer.inspectionPackets
            .where(isLitchcraftSourceClockInspection)
            .length;
        peer.connection(true);
        await _drive(owner.settleModeResolution());
        expect(owner.status, LitchcraftStatus.offline);
        expect(owner.boardActive, isFalse);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, cached),
          isTrue,
        );
        expect(
          peer.requests,
          isEmpty,
          reason: 'passive reboot inspection never sends Enter',
        );
        expect(peer.frameStoreInspectCount, inspections);
        expect(
          peer.inspectionPackets.where(isLitchcraftSourceClockInspection),
          hasLength(clocks),
        );
        expect(peer.operations, [LitchcraftV2Protocol.inspect]);

        await _drive(owner.enter());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(owner.status, LitchcraftStatus.live);
        expect(owner.boardActive, isTrue);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, cached),
          isTrue,
        );
        expect(sameLitchcraftDocument(peer.document, cached), isTrue);
        expect(peer.requests.map((request) => request.opcode), [
          LitchcraftProtocol.enter,
        ]);
        expect(
          peer.operations,
          contains(LitchcraftV2Protocol.frameStoreCommit),
        );
      });
    },
  );

  for (final active in [true, false]) {
    test(
      'current restored ${active ? 'active' : 'inactive'} board preserves detached phone edits',
      () async {
        await _withCurrentStore((peer, owner) async {
          peer.installSavedStoreDocument(
            staticLitchcraftDocument(
              LitchcraftCanvas.blank().paint([0], 15, 0x123456),
            ),
          );
          await _drive(owner.enter());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          await peer.retireSharedConnection();
          owner.documentSession.paintSelectedFrame(
            squares: [7],
            mask: 15,
            color: 0xff0000,
          );
          final draft = owner.documentSession.document;
          peer.boardBootNonce++;
          peer.session = 73;
          peer.revision = 1;
          peer.active = active;
          final durable = staticLitchcraftDocument(
            LitchcraftCanvas.blank().paint([63], 15, 0xabcdef),
          );
          peer.installSavedStoreDocument(durable, generation: 4);
          peer.requests.clear();
          peer.operations.clear();
          peer.connection(true);
          await _drive(owner.settleModeResolution());
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          expect(
            sameLitchcraftDocument(owner.documentSession.document, draft),
            isTrue,
          );
          expect(sameLitchcraftDocument(peer.document, durable), isTrue);
          expect(peer.requests, isEmpty);
          expect(
            peer.operations,
            isNot(contains(LitchcraftV2Protocol.frameStoreCommit)),
          );
          if (active) {
            expect(owner.status, LitchcraftStatus.conflict);
            await _drive(owner.resolveConflict(useBoard: false));
            expect(owner.status, LitchcraftStatus.live);
            expect(
              sameLitchcraftDocument(owner.documentSession.document, draft),
              isTrue,
            );
            expect(sameLitchcraftDocument(peer.document, draft), isTrue);
            expect(owner.sessionId, 73);
          } else {
            expect(owner.status, LitchcraftStatus.offline);
            expect(owner.boardActive, isFalse);
            expect(peer.operations, [LitchcraftV2Protocol.inspect]);
          }
        });
      },
    );
  }

  test(
    'disconnect immediately after entry request settles offline without commands',
    () async {
      final entering = controller.enter();
      ble.connection(false);
      await _drive(entering);
      expect(controller.status, LitchcraftStatus.offline);
      expect(controller.pending, isFalse);
      expect(ble.requests, isEmpty);
    },
  );

  test(
    'selected offline canvas can admit the first newly connected board',
    () async {
      ble.connected = false;
      ble.active = false;
      await _drive(controller.enter());
      expect(controller.status, LitchcraftStatus.offline);
      expect(ble.requests, isEmpty);
      ble.connection(true);
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(ble.requests.map((r) => r.opcode), [
        LitchcraftProtocol.snapshot,
        LitchcraftProtocol.enter,
        LitchcraftProtocol.snapshot,
      ]);
    },
  );

  for (final (phase, label) in <(int, String)>[
    (LitchcraftProtocol.colorPickPicking, 'Picking'),
    (LitchcraftProtocol.colorPickPicked, 'Picked'),
  ]) {
    test(
      'current connection probe adopts active Store during $label',
      () async {
        await _withCurrentStore((peer, owner) async {
          peer.session = 42;
          peer.beforeFrameStoreInspect = () async {
            peer.beforeFrameStoreInspect = null;
            peer.colorPick(phase);
          };
          expect(await _drive(owner.adoptActiveBoard()), isTrue);
          await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
          expect(owner.selected, isTrue);
          expect(owner.status, LitchcraftStatus.live);
          expect(
            owner.physicalColorPicking,
            phase == LitchcraftProtocol.colorPickPicking,
          );
          expect(
            owner.physicalColorPicked,
            phase == LitchcraftProtocol.colorPickPicked,
          );
          expect(
            peer.requests,
            isEmpty,
            reason: 'passive adoption never requests Enter',
          );
          expect(_modeInspectCount(peer), 1);
          expect(peer.frameStoreInspectCapabilities, everyElement(7));
          expect(peer.snapshotCount, 0);
        });
      },
    );
  }

  test(
    'current inactive connection probe waits for explicit Craft admission',
    () async {
      await _withCurrentStore((peer, owner) async {
        peer.active = false;
        expect(await _drive(owner.adoptActiveBoard()), isFalse);
        expect(owner.selected, isFalse);
        expect(owner.status, LitchcraftStatus.offline);
        expect(peer.operations, [LitchcraftV2Protocol.inspect]);
        expect(peer.requests, isEmpty);
        await _drive(owner.enter());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(owner.selected, isTrue);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.requests.map((request) => request.opcode), [
          LitchcraftProtocol.enter,
        ]);
      });
    },
  );

  test(
    'current inactive probe hears later physical Craft entry on the same link',
    () async {
      await _withCurrentStore((peer, owner) async {
        peer.active = false;
        expect(await _drive(owner.adoptActiveBoard()), isFalse);
        expect(owner.selected, isFalse);
        expect(_modeInspectCount(peer), 1);
        expect(peer.litchcraftSubscriptionCount, 1);
        peer
          ..active = true
          ..frameStoreActive = true
          ..session = 42
          ..revision = 11;
        peer.advancedState(0);
        await _untilBackgroundRepair(
          () => owner.selected && !owner.boardArtworkUnresolved,
        );
        await _idle(owner);
        expect(owner.boardActive, isTrue);
        expect(owner.status, LitchcraftStatus.live);
        expect(peer.requests, isEmpty);
        expect(peer.snapshotCount, 0);
        expect(
          _modeInspectCount(peer),
          2,
          reason:
              'one initial passive probe and one fresh physical-entry proof',
        );
        expect(peer.litchcraftSubscriptionCount, 1);
      });
    },
  );

  test(
    'rejected admission hears a later physical entry after the board is cleared',
    () async {
      ble
        ..active = false
        ..enterStatus = 1;

      await _drive(controller.enter());
      expect(controller.selected, isTrue);
      expect(controller.boardActive, isFalse);
      expect(controller.status, LitchcraftStatus.gameActive);

      ble
        ..enterStatus = 0
        ..active = true
        ..session = 42
        ..revision = 11;
      ble.state(0);
      await _idle(controller);

      expect(controller.selected, isTrue);
      expect(controller.boardActive, isTrue);
      expect(controller.status, LitchcraftStatus.live);
      expect(
        ble.requests.map((request) => request.opcode),
        contains(LitchcraftProtocol.snapshot),
      );
    },
  );

  test(
    'physical entry pending stays quiet and recovers when its completion event is missed',
    () async {
      ble
        ..active = false
        ..enterStatus = LitchcraftProtocol.statusEntryPending;

      await controller.enter();
      expect(controller.selected, isTrue);
      expect(controller.boardActive, isFalse);
      expect(controller.status, LitchcraftStatus.connecting);
      expect(controller.errorMessage, isNull);

      // Model the board completing its X+O hold without delivering the
      // unsolicited State event. The controller's bounded transition repair
      // must discover the already-owned session without exposing Game active.
      ble
        ..enterStatus = 0
        ..active = true
        ..session = 42
        ..revision = 11;
      await Future<void>.delayed(const Duration(milliseconds: 550));
      await _idle(controller);

      expect(controller.selected, isTrue);
      expect(controller.boardActive, isTrue);
      expect(controller.status, LitchcraftStatus.live);
      expect(
        ble.requests.where(
          (request) => request.opcode == LitchcraftProtocol.enter,
        ),
        hasLength(2),
      );
    },
  );

  test(
    'current active inspection adopts mode while a typed Store body fails',
    () async {
      await _withCurrentStore((peer, owner) async {
        peer.session = 42;
        peer.admitStoreReadsBeforeResponse = true;
        peer.rejectNextFrameStoreSnapshotStatus = 10;
        expect(await _drive(owner.adoptActiveBoard()), isTrue);
        await _untilBackgroundRepair(
          () =>
              owner.status == LitchcraftStatus.error &&
              !owner.authorityResolving,
        );
        expect(owner.selected, isTrue);
        expect(owner.boardActive, isTrue);
        expect(owner.status, LitchcraftStatus.error);
        expect(_modeInspectCount(peer), 1);
        expect(peer.frameStoreInspectCapabilities, everyElement(7));
        expect(peer.frameStoreSnapshotTransactionCount, 1);
        expect(
          peer.requests,
          isEmpty,
          reason: 'active mode needs no Enter or legacy Snapshot',
        );
        expect(owner.errorMessage, contains('10'));
        expect(owner.connectionRecoveryRequired, isFalse);
        expect(peer._connectionSession!.isActivated, isTrue);
        await _drive(owner.retryConnection());
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          sameLitchcraftDocument(owner.documentSession.document, peer.document),
          isTrue,
        );
      });
    },
  );

  test(
    'current replacement connection retires stale adoption and probes its own mode',
    () async {
      await _withCurrentStore((peer, owner) async {
        peer.cachedBoardRotate = null;
        final firstOrientation = Completer<Object?>();
        peer.onReadBoardRotate = () => firstOrientation.future;
        final firstAdoption = owner.adoptActiveBoard();
        await _until(() => peer.settingReads.isNotEmpty);
        await peer.retireSharedConnection();
        peer.cachedBoardRotate = false;
        peer.connection(true);
        final secondAdoption = owner.adoptActiveBoard();
        firstOrientation.complete(false);
        expect(await _drive(secondAdoption), isTrue);
        await _untilBackgroundRepair(() => !owner.boardArtworkUnresolved);
        expect(await firstAdoption, isFalse);
        expect(owner.selected, isTrue);
        expect(owner.status, LitchcraftStatus.live);
        expect(
          _modeInspectCount(peer),
          2,
          reason: 'each distinct connection activation owns one mode probe',
        );
        expect(
          peer.litchcraftSubscriptionCount,
          2,
          reason:
              'the retired SDK subscription cannot serve the new activation',
        );
      });
    },
  );

  test(
    'write failure remains actionable and coherent save refuses stale cache',
    () async {
      await _drive(controller.enter());
      ble.onMutation = (_) async {
        throw StateError('scripted GATT failure');
      };
      await _drive(controller.setBrush(color: 0x123456));
      expect(controller.status, LitchcraftStatus.error);
      expect(controller.errorMessage, contains('scripted GATT failure'));
      await expectLater(controller.coherentSnapshot(), throwsStateError);
    },
  );

  test(
    'same-request old revision snapshot is ignored until current revision arrives',
    () async {
      await _drive(controller.enter());
      final current = LitchcraftCanvas.blank().paint([63], 15, 0xabcdef);
      ble.onSnapshot = (request) {
        ble.state(request.id);
        ble.begin(request.id, atRevision: 9);
        ble.chunks(
          request.id,
          LitchcraftCanvas.blank().paint([0], 15, 0x123456),
          atRevision: 9,
        );
        ble.emit(
          LitchcraftProtocol.snapshotEnd,
          requestId: request.id,
          atRevision: 9,
        );
        ble.artwork = current;
        ble.snapshot(request.id);
      };
      expect(await _drive(controller.coherentSnapshot()), current);
      expect(controller.revision, 10);
    },
  );

  test(
    'same-request unrelated session snapshot cannot hijack a live canvas read',
    () async {
      await _drive(controller.enter());
      final current = LitchcraftCanvas.blank().paint([63], 15, 0xabcdef);
      ble.onSnapshot = (request) {
        ble.state(request.id);
        ble.begin(request.id, sessionId: 40);
        ble.chunks(
          request.id,
          LitchcraftCanvas.blank().paint([0], 15, 0x123456),
          sessionId: 40,
        );
        ble.emit(
          LitchcraftProtocol.snapshotEnd,
          requestId: request.id,
          sessionId: 40,
        );
        ble.artwork = current;
        ble.snapshot(request.id);
      };
      expect(await _drive(controller.coherentSnapshot()), current);
      expect(controller.sessionId, 41);
    },
  );

  test(
    'bulk replacement crosses the platform boundary in four high-MTU writes',
    () async {
      await _drive(controller.enter());
      final previous = controller.canvas;
      final replacement = LitchcraftCanvas(
        colors: List.generate(256, (i) => i * 7919),
      );
      var committed = false;
      ble.onBulk = (frames, request) async {
        expect(
          controller.canvas,
          previous,
          reason: 'staging bytes are not a completed image',
        );
        final staged = <int>[];
        for (final frame in frames) {
          expect(frame[0], LitchcraftProtocol.magic);
          expect(frame[1], LitchcraftProtocol.bulkReplace);
          for (var index = 0; index < frame[11]; ++index) {
            staged.add(LitchcraftProtocol.readRgb(frame, 12 + index * 3));
          }
        }
        expect(staged, replacement.colors);
        committed = true;
        ble.artwork = replacement;
        ++ble.revision;
        ble.state(request);
      };
      final snapshotsBefore = ble.requests
          .where((r) => r.opcode == LitchcraftProtocol.snapshot)
          .length;
      await _drive(controller.replaceCanvas(replacement));
      expect(committed, isTrue);
      expect(controller.canvas, replacement);
      expect(ble.bulkFrames, hasLength(4));
      expect(ble.bulkFrames.every((frame) => frame.length <= 244), isTrue);
      expect(ble.bulkFrames.last[10], 1);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.replaceChunk),
        isEmpty,
      );
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.snapshot),
        hasLength(snapshotsBefore),
        reason: 'accepted bulk commit does not reread 128 notification chunks',
      );
    },
  );

  test(
    'live artwork rotation commits the transformed document to the board',
    () async {
      await _drive(controller.enter());
      final source = LitchcraftCanvas.blank()
          .paint(<int>[0], 1, 0x112233)
          .paint(<int>[0], 2, 0x445566);
      await _drive(controller.replaceCanvas(source));
      ble.bulkFrames.clear();

      await _drive(controller.rotateArtwork(1));

      expect(controller.canvas, source.rotated(1));
      expect(ble.artwork, source.rotated(1));
      expect(ble.bulkFrames, hasLength(4));
    },
  );

  test(
    'saved V3 artwork keeps exact colors but adopts live V2 topology',
    () async {
      ble.hardwareRevision = LitchHardwareRevision.v2;
      ble.artwork = LitchcraftCanvas.blank(topology: LitchcraftTopology.v2);
      await _drive(controller.enter());
      final saved = LitchcraftCanvas(
        colors: List<int>.generate(256, (int index) => index * 7919),
        topology: LitchcraftTopology.v3,
      );
      final snapshotsBefore = ble.requests
          .where((request) => request.opcode == LitchcraftProtocol.snapshot)
          .length;

      await _drive(controller.replaceCanvas(saved));

      expect(saved.topology, LitchcraftTopology.v3);
      expect(controller.canvas.colors, saved.colors);
      expect(controller.canvas.topology, LitchcraftTopology.v2);
      expect(controller.canvas.topology.targets, const <int>[1, 10, 4]);
      expect(ble.artwork.colors, saved.colors);
      expect(ble.artwork.topology, LitchcraftTopology.v2);
      expect(ble.bulkFrames, hasLength(4));
      expect(
        ble.requests.where(
          (request) => request.opcode == LitchcraftProtocol.snapshot,
        ),
        hasLength(snapshotsBefore),
        reason: 'accepted bulk commit retains its no-reread optimization',
      );
      final coherent = await _drive(controller.coherentSnapshot());
      expect(coherent.colors, saved.colors);
      expect(coherent.topology, LitchcraftTopology.v2);
    },
  );

  for (final supportsBulk in [false, true]) {
    for (final topology in <LitchcraftTopology>[
      LitchcraftTopology.v2,
      LitchcraftTopology.v3,
    ]) {
      test(
        'replacement retains $topology minimum-MTU fallback with bulk capability=$supportsBulk',
        () async {
          ble.hardwareRevision = topology == LitchcraftTopology.v2
              ? LitchHardwareRevision.v2
              : LitchHardwareRevision.v3;
          ble.artwork = LitchcraftCanvas.blank(topology: topology);
          ble.bulkReplaceSupported = supportsBulk;
          ble.onBulk = (_, _) async => throw const LitchcraftBulkUnavailable();
          await _drive(controller.enter());
          final replacement = LitchcraftCanvas(
            colors: List.generate(256, (i) => i * 7919),
          );
          ble.onMutation = (request) async {
            if (request.opcode == LitchcraftProtocol.replaceCommit) {
              ble.artwork = replacement.withTopology(ble.artwork.topology);
              ++ble.revision;
            }
            ble.state(request.id);
          };
          await _drive(controller.replaceCanvas(replacement));
          expect(ble.requests.every((r) => r.bytes.length <= 20), isTrue);
          expect(
            ble.requests.where(
              (r) => r.opcode == LitchcraftProtocol.replaceChunk,
            ),
            hasLength(86),
          );
          expect(
            ble.requests.where(
              (r) =>
                  r.opcode == LitchcraftProtocol.replaceChunk &&
                  !r.withResponse,
            ),
            isEmpty,
          );
          expect(controller.canvas, replacement.withTopology(topology));
        },
      );
    }
  }

  test('detached artwork retains its saved topology metadata', () async {
    ble.connected = false;
    final saved = LitchcraftCanvas(
      colors: List<int>.generate(256, (int index) => index * 7919),
      topology: LitchcraftTopology.v3,
    );

    await _drive(controller.replaceCanvas(saved));

    expect(controller.canvas, same(saved));
    expect(controller.canvas.topology, LitchcraftTopology.v3);
  });

  test(
    'interrupted replacement never commits or presents its partial transfer',
    () async {
      await _drive(controller.enter());
      final previous = controller.canvas;
      ble.onBulk = (frames, request) async {
        expect(frames, hasLength(4));
        throw StateError('link interrupted while staging');
      };
      await _drive(
        controller.replaceCanvas(
          LitchcraftCanvas.blank().paint([0, 63], 15, 0xabcdef),
        ),
      );
      expect(controller.canvas, previous);
      expect(controller.status, LitchcraftStatus.error);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.replaceChunk),
        isEmpty,
      );
    },
  );

  test(
    'firmware congestion signal resyncs accepted operation without applying it twice',
    () async {
      await _drive(controller.enter());
      ble.onMutation = (request) async {
        ble.artwork = LitchcraftCanvas.blank().paint([0], 15, 0x123456);
        ble.revision = 11;
        ble.emit(LitchcraftProtocol.resyncRequired);
      };
      controller.paintSquares([0]);
      controller.endStroke();
      await _idle(controller);
      expect(controller.status, LitchcraftStatus.live);
      expect(controller.canvas, ble.artwork);
      expect(
        ble.requests.where((r) => r.opcode == LitchcraftProtocol.paint),
        hasLength(1),
      );
    },
  );

  for (final status in <int>[1, 2]) {
    test(
      'rejected exit status $status retains board ownership until retry succeeds',
      () async {
        await _drive(controller.enter());
        ble.onMutation = (request) async {
          expect(request.opcode, LitchcraftProtocol.exit);
          ble.state(request.id, status: status);
        };
        await expectLater(_drive(controller.leave()), throwsStateError);
        expect(ble.active, isTrue);
        expect(controller.selected, isTrue);
        expect(controller.boardActive, isTrue);
        expect(controller.status, LitchcraftStatus.error);
        expect(controller.errorMessage, isNotEmpty);

        ble.onMutation = null;
        await _drive(controller.leave());
        expect(ble.active, isFalse);
        expect(controller.selected, isFalse);
        expect(controller.boardActive, isFalse);
        expect(
          ble.requests.where(
            (request) => request.opcode == LitchcraftProtocol.exit,
          ),
          hasLength(2),
        );
      },
    );
  }

  test(
    'gap during snapshot retries before claiming a coherent saved revision',
    () async {
      await _drive(controller.enter());
      final latest = LitchcraftCanvas.blank().paint([7, 63], 15, 0x123456);
      var reads = 0;
      ble.onSnapshot = (request) {
        if (++reads == 1) {
          ble.state(request.id);
          ble.begin(request.id);
          ble.chunks(request.id, ble.artwork);
          // The snapshot's revision is 10; one delta was omitted before rev12.
          ble.emit(
            LitchcraftProtocol.cell,
            atRevision: 12,
            payload: [63, 15, 0x12, 0x34, 0x56],
          );
          ble.emit(LitchcraftProtocol.snapshotEnd, requestId: request.id);
        } else {
          ble.revision = 12;
          ble.artwork = latest;
          ble.snapshot(request.id);
        }
      };
      final saved = await _drive(controller.coherentSnapshot());
      expect(
        reads,
        2,
        reason: 'an incomplete intervening revision needs a fresh snapshot',
      );
      expect(saved, latest);
      expect(controller.revision, 12);
    },
  );
}
